#include "stdafx.h"
#include "cache_utils.hpp"
#include "system_utils.hpp"
#include "system_config.h"
#include "IdManager.h"
#include "Emu/System.h"
#include "rpcs3_version.h"
#include "Emu/Cell/lv2/sys_sync.h"
#include "Emu/Cell/PPUAnalyser.h"
#include "Emu/Cell/PPUThread.h"
#include "Crypto/sha1.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <ctime>
#include <cstring>
#include <memory>
#include <mutex>
#include <regex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <system_error>
#include <zstd.h>

#if __has_include(<lz4.h>)
#include <lz4.h>
#define RPCS3_CAS_HAS_LZ4 1
#else
#define RPCS3_CAS_HAS_LZ4 0
#endif

LOG_CHANNEL(sys_log, "SYS");

namespace rpcs3::cache
{
	std::string get_ppu_cache()
	{
		const auto _main = g_fxo->try_get<main_ppu_module<lv2_obj>>();

		if (!_main || _main->cache.empty())
		{
			ppu_log.error("PPU Cache location not initialized.");
			return {};
		}

		return _main->cache;
	}



	std::string get_shared_cas_root()
	{
		std::string path = rpcs3::utils::get_cache_dir() + "cas/";
		if (!fs::is_dir(path) && !fs::create_path(path))
		{
			sys_log.error("Failed to initialize shared CAS root %s (%s)", path, fs::g_tls_error);
			return {};
		}

		return path;
	}

	std::string get_platform_cache_id()
	{
		const auto os = utils::get_OS_version();
		return fmt::format("%s-%s-%u.%u.%u-cpu%X.%X", os.type, os.arch, os.version_major, os.version_minor, os.version_patch, utils::get_cpu_family(), utils::get_cpu_model());
	}

	std::string make_platform_fields(std::initializer_list<compatibility_field> fields)
	{
		std::string result;
		for (const auto& [key, value] : fields)
		{
			if (key.empty() || value.empty())
			{
				continue;
			}

			if (!result.empty())
			{
				result += ';';
			}

			fmt::append(result, "%s=%s", key, value);
		}

		return result;
	}

	std::string make_rsx_platform_fields(std::string_view renderer_backend, std::initializer_list<compatibility_field> platform_fields)
	{
		std::string fields = make_platform_fields(platform_fields);
		if (!fields.empty())
		{
			fields += ';';
		}

		fmt::append(fields, "os=%s", get_platform_cache_id());

		if (!renderer_backend.empty())
		{
			fmt::append(fields, ";renderer=%s", renderer_backend);
		}

		return fields;
	}

	static u32 get_schema_version_for_domain(std::string_view domain)
	{
		if (domain == "ppu")
		{
			return ppu_cache_schema_version;
		}

		if (domain == "spu")
		{
			return spu_cache_schema_version;
		}

		if (domain == "rsx")
		{
			return rsx_cache_schema_version;
		}

		return emu_cache_schema_version;
	}

	std::string make_compatibility_tuple(std::string_view domain, std::string_view backend_id, std::string_view platform_fields)
	{
		if (platform_fields.empty())
		{
			platform_fields = get_platform_cache_id();
		}

		return fmt::format("schema=%u|domain=%s|backend=%s|platform=%s", get_schema_version_for_domain(domain), domain, backend_id, platform_fields);
	}

	static std::string canonical_sha1_hex(const void* data, std::size_t size)
	{
		sha1_context ctx;
		u8 digest[20]{};
		sha1_starts(&ctx);
		sha1_update(&ctx, reinterpret_cast<const u8*>(data), size);
		sha1_finish(&ctx, digest);
		std::string out;
		out.reserve(40);
		for (const u8 b : digest)
		{
			fmt::append(out, "%02x", b);
		}
		return out;
	}

	static std::string make_atomic_tmp_path(const std::string& target)
	{
		static atomic_t<u64> s_tmp_counter{0};
		return fmt::format("%s.tmp.%llu", target, s_tmp_counter++);
	}

	namespace
	{
		std::mutex s_manifest_append_map_mutex;
		std::unordered_map<std::string, std::shared_ptr<std::mutex>> s_manifest_append_mutexes;

		std::shared_ptr<std::mutex> get_manifest_append_mutex(const std::string& path)
		{
			std::lock_guard lock(s_manifest_append_map_mutex);
			auto& path_mutex = s_manifest_append_mutexes[path];
			if (!path_mutex)
			{
				path_mutex = std::make_shared<std::mutex>();
			}

			return path_mutex;
		}
	}

	namespace
	{
		std::mutex s_current_run_mutex;
		std::unordered_map<std::string, std::string> s_current_run_id_by_title;
	}

	bool write_file_atomic(const std::string& path, const void* data, std::size_t size)
	{
		if (!data || !size)
		{
			return false;
		}

		const std::string tmp_path = make_atomic_tmp_path(path);
		fs::remove_file(tmp_path);
		fs::file out(tmp_path, fs::write + fs::create + fs::trunc);
		if (!out)
		{
			sys_log.error("Failed to open temp cache file %s (%s)", tmp_path, fs::g_tls_error);
			return false;
		}

		if (out.write(data, size) != size)
		{
			sys_log.error("Failed to write temp cache file %s (%s)", tmp_path, fs::g_tls_error);
			out.close();
			fs::remove_file(tmp_path);
			return false;
		}

		out.sync();
		out.close();

		if (fs::stat_t st{}; !fs::get_stat(tmp_path, st) || st.size != size)
		{
			sys_log.error("Temp cache file %s size verification failed (%s)", tmp_path, fs::g_tls_error);
			fs::remove_file(tmp_path);
			return false;
		}

		if (!fs::rename(tmp_path, path, true))
		{
			sys_log.error("Failed to atomically replace cache file %s -> %s (%s)", tmp_path, path, fs::g_tls_error);
			fs::remove_file(tmp_path);
			return false;
		}

		return true;
	}

	bool write_text_file_atomic(const std::string& path, std::string_view text)
	{
		if (text.empty())
		{
			return false;
		}

		return write_file_atomic(path, text.data(), text.size());
	}

	bool append_manifest_record_atomic(const std::string& path, std::string_view record, bool use_journal)
	{
		// Invariant: all manifest writers (PPU/SPU/RSX) must use this API and must not append to manifest files directly.
		if (record.empty())
		{
			return false;
		}

		const std::shared_ptr<std::mutex> path_mutex = get_manifest_append_mutex(path);
		const std::lock_guard append_guard(*path_mutex);

		const std::string journal_path = path + ".pending";
		std::string pending;
		if (use_journal)
		{
			if (!fs::read_file(journal_path, pending) && fs::is_file(journal_path))
			{
				return false;
			}

			if (!write_text_file_atomic(journal_path, std::string(record)))
			{
				return false;
			}
		}

		std::string snapshot;
		if (!fs::read_file(path, snapshot) && fs::is_file(path))
		{
			return false;
		}
		snapshot += pending;
		snapshot += record;
		if (!write_text_file_atomic(path, snapshot))
		{
			return false;
		}

		if (use_journal)
		{
			fs::remove_file(journal_path);
		}

		return true;
	}

	namespace
	{
		inline constexpr u32 s_cas_blob_magic = 0x52434331; // RCC1
		inline constexpr u8 s_cas_blob_version = 1;
		inline constexpr u8 s_cas_flag_checksum32 = 0x1;

		struct cas_blob_header
		{
			u32 magic{};
			u8 version{};
			u8 codec{};
			u8 flags{};
			u8 reserved{};
			u64 uncompressed_size{};
			u32 checksum32{};
		};

		static_assert(sizeof(cas_blob_header) == 20);

		u32 get_checksum32(const void* data, usz size)
		{
			sha1_context ctx;
			u8 digest[20]{};
			sha1_starts(&ctx);
			sha1_update(&ctx, reinterpret_cast<const u8*>(data), size);
			sha1_finish(&ctx, digest);

			u32 out{};
			std::memcpy(&out, digest, sizeof(out));
			return out;
		}

		cas_cache_tier infer_tier(std::string_view extension)
		{
			if (extension == "obj" || extension == "spu")
			{
				return cas_cache_tier::hot;
			}

			return cas_cache_tier::warm;
		}

		cas_codec infer_codec(cas_cache_tier tier)
		{
			switch (tier)
			{
			case cas_cache_tier::hot:
				return cas_codec::lz4;
			case cas_cache_tier::warm:
			case cas_cache_tier::cold:
				return cas_codec::zstd;
			default:
				return cas_codec::zstd;
			}
		}

		struct cas_storage_policy
		{
			std::string_view manifest_name;
			cas_cache_tier default_tier;
		};

		cas_storage_policy get_policy_for_artifact(cas_artifact_type artifact)
		{
			switch (artifact)
			{
			case cas_artifact_type::spu_function_blob:
				return {"spu", cas_cache_tier::hot};
			case cas_artifact_type::ppu_object_blob:
				return {"ppu_obj", cas_cache_tier::hot};
			case cas_artifact_type::rsx_raw_fp_blob:
				return {"fp", cas_cache_tier::warm};
			case cas_artifact_type::rsx_raw_vp_blob:
				return {"vp", cas_cache_tier::warm};
			case cas_artifact_type::rsx_pipeline_blob:
				return {"rsx_pipeline", cas_cache_tier::hot};
			default:
				return {std::string_view{}, cas_cache_tier::auto_select};
			}
		}

		struct cas_metrics
		{
			atomic_t<u64> encode_lz4_count{};
			atomic_t<u64> encode_zstd_count{};
			atomic_t<u64> encode_none_count{};
			atomic_t<u64> decode_lz4_count{};
			atomic_t<u64> decode_zstd_count{};
			atomic_t<u64> decode_none_count{};
			atomic_t<u64> encode_input_bytes{};
			atomic_t<u64> encode_output_bytes{};
			atomic_t<u64> decode_output_bytes{};
			atomic_t<u64> decode_failures{};
		};

		cas_metrics& get_cas_metrics()
		{
			static cas_metrics s_metrics{};
			return s_metrics;
		}

		void record_encode_metrics(cas_codec codec, usz src_size, usz compressed_size)
		{
			auto& m = get_cas_metrics();
			m.encode_input_bytes += src_size;
			m.encode_output_bytes += compressed_size;
			switch (codec)
			{
			case cas_codec::none: m.encode_none_count++; break;
			case cas_codec::lz4: m.encode_lz4_count++; break;
			case cas_codec::zstd: m.encode_zstd_count++; break;
			default: break;
			}
		}

		void record_decode_metrics(cas_codec codec, usz decoded_size, bool ok)
		{
			auto& m = get_cas_metrics();
			if (!ok)
			{
				m.decode_failures++;
				return;
			}
			m.decode_output_bytes += decoded_size;
			switch (codec)
			{
			case cas_codec::none: m.decode_none_count++; break;
			case cas_codec::lz4: m.decode_lz4_count++; break;
			case cas_codec::zstd: m.decode_zstd_count++; break;
			default: break;
			}
		}

		void log_cas_stats_if_needed()
		{
			static atomic_t<u64> s_event_count{};
			const u64 event_count = ++s_event_count;
			if ((event_count % 256) != 0)
			{
				return;
			}

			const auto& m = get_cas_metrics();
			sys_log.notice("CAS stats: enc[n=%llu lz4=%llu zstd=%llu none=%llu in=%llu out=%llu] dec[n=%llu lz4=%llu zstd=%llu none=%llu out=%llu fail=%llu]",
				m.encode_lz4_count + m.encode_zstd_count + m.encode_none_count,
				m.encode_lz4_count,
				m.encode_zstd_count,
				m.encode_none_count,
				m.encode_input_bytes,
				m.encode_output_bytes,
				m.decode_lz4_count + m.decode_zstd_count + m.decode_none_count,
				m.decode_lz4_count,
				m.decode_zstd_count,
				m.decode_none_count,
				m.decode_output_bytes,
				m.decode_failures);
		}

		bool compress_blob(cas_codec codec, const uchar* src, usz size, std::vector<uchar>& compressed, cas_codec& resolved_codec)
		{
			resolved_codec = codec;
			switch (codec)
			{
			case cas_codec::none:
				compressed.assign(src, src + size);
				return true;
			case cas_codec::lz4:
			{
#if RPCS3_CAS_HAS_LZ4
				const int bound = LZ4_compressBound(static_cast<int>(size));
				if (bound <= 0)
				{
					return false;
				}

				compressed.resize(bound);
				const int out_size = LZ4_compress_default(reinterpret_cast<const char*>(src), reinterpret_cast<char*>(compressed.data()), static_cast<int>(size), bound);
				if (out_size <= 0)
				{
					return false;
				}
				compressed.resize(static_cast<usz>(out_size));
				return true;
#else
				static atomic_t<bool> s_warned = false;
				if (!s_warned.exchange(true))
				{
					sys_log.notice("CAS requested LZ4 codec but LZ4 headers are unavailable in this build; falling back to Zstd.");
				}
				resolved_codec = cas_codec::zstd;
				[[fallthrough]];
#endif
			}
			case cas_codec::zstd:
			{
				compressed.resize(ZSTD_compressBound(size));
				const usz out_size = ZSTD_compress(compressed.data(), compressed.size(), src, size, 3);
				if (ZSTD_isError(out_size))
				{
					return false;
				}
				compressed.resize(out_size);
				return true;
			}
			default:
				return false;
			}
		}

		bool decompress_blob(cas_codec codec, const uchar* src, usz src_size, uchar* dst, usz dst_size)
		{
			switch (codec)
			{
			case cas_codec::none:
				if (src_size != dst_size)
				{
					return false;
				}
				std::memcpy(dst, src, src_size);
				return true;
			case cas_codec::lz4:
#if RPCS3_CAS_HAS_LZ4
				return LZ4_decompress_safe(reinterpret_cast<const char*>(src), reinterpret_cast<char*>(dst), static_cast<int>(src_size), static_cast<int>(dst_size)) == static_cast<int>(dst_size);
#else
				return false;
#endif
			case cas_codec::zstd:
			{
				const usz out_size = ZSTD_decompress(dst, dst_size, src, src_size);
				return !ZSTD_isError(out_size) && out_size == dst_size;
			}
			default:
				return false;
			}
		}
	}

	std::string_view to_manifest_artifact_name(cas_artifact_type artifact)
	{
		return get_policy_for_artifact(artifact).manifest_name;
	}

	cas_cache_tier get_default_tier_for_artifact(cas_artifact_type artifact)
	{
		return get_policy_for_artifact(artifact).default_tier;
	}

	std::string put_to_cas(const void* data, std::size_t size, cas_artifact_type artifact, cas_cache_tier tier)
	{
		if (tier == cas_cache_tier::auto_select)
		{
			tier = get_default_tier_for_artifact(artifact);
		}

		return put_to_cas(data, size, to_manifest_artifact_name(artifact), tier, cas_codec::auto_select);
	}

	std::string put_to_cas(const void* data, std::size_t size, std::string_view extension, cas_cache_tier tier, cas_codec codec)
	{
		if (!data || !size)
		{
			return {};
		}

		const std::string cas_root = get_shared_cas_root();
		if (cas_root.empty())
		{
			return {};
		}

		if (tier == cas_cache_tier::auto_select)
		{
			tier = infer_tier(extension);
		}

		if (codec == cas_codec::auto_select)
		{
			codec = infer_codec(tier);
		}

		const std::string hash_key = canonical_sha1_hex(data, size);
		const std::string object_path = cas_root + hash_key;

		if (fs::is_file(object_path))
		{
			std::vector<uchar> existing;
			if (get_from_cas(hash_key, existing) && existing.size() == size && canonical_sha1_hex(existing.data(), existing.size()) == hash_key)
			{
				return hash_key;
			}
		}

		std::vector<uchar> compressed;
		cas_codec resolved_codec{};
		if (!compress_blob(codec, reinterpret_cast<const uchar*>(data), size, compressed, resolved_codec))
		{
			sys_log.error("Failed to compress CAS object %s (codec=%u)", object_path, static_cast<u8>(codec));
			return {};
		}

		record_encode_metrics(resolved_codec, size, compressed.size());
		log_cas_stats_if_needed();

		cas_blob_header hdr{};
		hdr.magic = s_cas_blob_magic;
		hdr.version = s_cas_blob_version;
		hdr.codec = static_cast<u8>(resolved_codec);
		hdr.flags = s_cas_flag_checksum32;
		hdr.uncompressed_size = size;
		hdr.checksum32 = get_checksum32(data, size);

		std::vector<uchar> object(sizeof(hdr) + compressed.size());
		std::memcpy(object.data(), &hdr, sizeof(hdr));
		if (!compressed.empty())
		{
			std::memcpy(object.data() + sizeof(hdr), compressed.data(), compressed.size());
		}

		if (!write_file_atomic(object_path, object.data(), object.size()))
		{
			std::vector<uchar> existing;
			if (get_from_cas(hash_key, existing) && existing.size() == size && canonical_sha1_hex(existing.data(), existing.size()) == hash_key)
			{
				return hash_key;
			}

			sys_log.error("Failed to write CAS object %s (%s)", object_path, fs::g_tls_error);
			return {};
		}

		std::vector<uchar> verify;
		if (!get_from_cas(hash_key, verify) || verify.size() != size || canonical_sha1_hex(verify.data(), verify.size()) != hash_key)
		{
			sys_log.error("CAS object verification failed for %s", object_path);
			return {};
		}

		return hash_key;
	}

	bool get_from_cas(const std::string& hash_key, std::vector<uchar>& out)
	{
		const std::string cas_root = get_shared_cas_root();
		if (cas_root.empty())
		{
			return false;
		}

		const std::string path = cas_root + hash_key;
		fs::file f(path);
		if (!f)
		{
			return false;
		}

		std::vector<uchar> in;
		in.resize(f.size());
		if (f.read(in.data(), in.size()) != in.size())
		{
			return false;
		}

		if (in.size() < sizeof(cas_blob_header))
		{
			out = std::move(in);
			return true;
		}

		cas_blob_header hdr{};
		std::memcpy(&hdr, in.data(), sizeof(hdr));
		if (hdr.magic != s_cas_blob_magic)
		{
			out = std::move(in);
			return true;
		}

		if (hdr.version != s_cas_blob_version || hdr.uncompressed_size > umax)
		{
			return false;
		}

		const usz payload_size = in.size() - sizeof(hdr);
		const uchar* payload = in.data() + sizeof(hdr);
		out.resize(static_cast<usz>(hdr.uncompressed_size));

		const auto codec = static_cast<cas_codec>(hdr.codec);
		if (!decompress_blob(codec, payload, payload_size, out.data(), out.size()))
		{
			record_decode_metrics(codec, 0, false);
			log_cas_stats_if_needed();
			return false;
		}

		if ((hdr.flags & s_cas_flag_checksum32) && get_checksum32(out.data(), out.size()) != hdr.checksum32)
		{
			record_decode_metrics(codec, 0, false);
			log_cas_stats_if_needed();
			return false;
		}

		record_decode_metrics(codec, out.size(), true);
		log_cas_stats_if_needed();
		return true;
	}

	std::string make_manifest_record(cas_artifact_type artifact, const std::string& hash_key, std::string_view metadata, std::string_view compatibility_tuple, std::string_view format_version, cas_cache_tier tier)
	{
		if (tier == cas_cache_tier::auto_select)
		{
			tier = get_default_tier_for_artifact(artifact);
		}

		return make_manifest_record(to_manifest_artifact_name(artifact), hash_key, metadata, compatibility_tuple, format_version, cas_codec::auto_select, tier);
	}

	std::string make_manifest_record(std::string_view artifact_type, const std::string& hash_key, std::string_view metadata, std::string_view compatibility_tuple, std::string_view format_version, cas_codec codec, cas_cache_tier tier)
	{
		std::string record = fmt::format("%s|%s", artifact_type, hash_key);
		if (!metadata.empty())
		{
			record += "|";
			record += metadata;
		}
		if (!compatibility_tuple.empty())
		{
			record += "|";
			record += compatibility_tuple;
		}
		if (!format_version.empty())
		{
			record += "|";
			record += format_version;
		}

		if (codec != cas_codec::auto_select)
		{
			cas_codec manifest_codec = codec;
#if !RPCS3_CAS_HAS_LZ4
			if (manifest_codec == cas_codec::lz4)
			{
				manifest_codec = cas_codec::zstd;
			}
#endif
			record += "|codec=";
			record += std::to_string(static_cast<u8>(manifest_codec));
		}

		if (tier != cas_cache_tier::auto_select)
		{
			record += "|tier=";
			record += std::to_string(static_cast<u8>(tier));
		}

		record += "\n";
		return record;
	}

	bool parse_manifest_record(std::string_view line, manifest_record& out)
	{
		if (!line.empty() && line.back() == '\n')
		{
			line.remove_suffix(1);
		}

		const usz p0 = line.find('|');
		if (p0 == umax)
		{
			return false;
		}

		const usz p1 = line.find('|', p0 + 1);

		out = {};
		out.artifact_type = std::string(line.substr(0, p0));
		if (p1 == umax)
		{
			out.hash_key = std::string(line.substr(p0 + 1));
			return !out.hash_key.empty();
		}

		out.hash_key = std::string(line.substr(p0 + 1, p1 - p0 - 1));

		std::vector<std::string_view> tails;
		tails.reserve(8);
		for (usz prev = p1; prev != umax;)
		{
			const usz next = line.find('|', prev + 1);
			if (next == umax)
			{
				tails.emplace_back(line.substr(prev + 1));
				break;
			}

			tails.emplace_back(line.substr(prev + 1, next - prev - 1));
			prev = next;
		}

		if (!tails.empty()) out.metadata = std::string(tails[0]);
		if (tails.size() > 1) out.compatibility_tuple = std::string(tails[1]);
		if (tails.size() > 2) out.format_version = std::string(tails[2]);

		for (usz i = 3; i < tails.size(); ++i)
		{
			const auto part = tails[i];
			if (const usz eq = part.find('='); eq != umax)
			{
				const auto key = part.substr(0, eq);
				const auto value = part.substr(eq + 1);
				if (key == "codec")
				{
					out.codec = std::string(value);
				}
				else if (key == "tier")
				{
					out.tier = std::string(value);
				}
			}
		}

		return true;
	}

	bool is_manifest_record_compatible(const manifest_record& rec, cas_artifact_type expected_artifact, std::string_view expected_compatibility_tuple, std::string_view expected_format_version, cas_cache_tier expected_tier)
	{
		return is_manifest_record_compatible(rec, to_manifest_artifact_name(expected_artifact), expected_compatibility_tuple, expected_format_version, cas_codec::auto_select, expected_tier);
	}

	bool is_manifest_record_compatible(const manifest_record& rec, std::string_view expected_artifact_type, std::string_view expected_compatibility_tuple, std::string_view expected_format_version, cas_codec expected_codec, cas_cache_tier expected_tier)
	{
		if (rec.artifact_type != expected_artifact_type)
		{
			return false;
		}

		if (!expected_compatibility_tuple.empty() && rec.compatibility_tuple != expected_compatibility_tuple)
		{
			return false;
		}

		if (!expected_format_version.empty() && rec.format_version != expected_format_version)
		{
			return false;
		}

		if (expected_codec != cas_codec::auto_select)
		{
			if (rec.codec.empty() || rec.codec != std::to_string(static_cast<u8>(expected_codec)))
			{
				return false;
			}
		}
		else if (!rec.codec.empty() && rec.codec != std::to_string(static_cast<u8>(cas_codec::none)) && rec.codec != std::to_string(static_cast<u8>(cas_codec::lz4)) && rec.codec != std::to_string(static_cast<u8>(cas_codec::zstd)))
		{
			return false;
		}

		if (expected_tier != cas_cache_tier::auto_select)
		{
			if (rec.tier.empty() || rec.tier != std::to_string(static_cast<u8>(expected_tier)))
			{
				return false;
			}
		}
		else if (!rec.tier.empty() && rec.tier != std::to_string(static_cast<u8>(cas_cache_tier::hot)) && rec.tier != std::to_string(static_cast<u8>(cas_cache_tier::warm)) && rec.tier != std::to_string(static_cast<u8>(cas_cache_tier::cold)))
		{
			return false;
		}

		return true;
	}


	static std::string escape_json_string(std::string_view in)
	{
		std::string out;
		out.reserve(in.size() + 8);

		for (const char ch : in)
		{
			switch (ch)
			{
			case '\\': out += "\\\\"; break;
			case '"': out += "\\\""; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default: out += ch; break;
			}
		}

		return out;
	}

	static std::string make_catalog_runs_root(std::string_view title_id)
	{
		return rpcs3::utils::get_cache_dir() + "catalog/" + std::string(title_id) + "/runs/";
	}

	static std::string make_run_json_path(std::string_view title_id, std::string_view run_id)
	{
		return make_catalog_runs_root(title_id) + std::string(run_id) + ".json";
	}

	static u64 get_unix_epoch_seconds_now()
	{
		return static_cast<u64>(std::time(nullptr));
	}

	static bool upsert_json_key_value(std::string& text, std::string_view key, std::string_view raw_value, bool only_if_missing = false)
	{
		const std::regex key_re(fmt::format("(\"%s\"\\s*:\\s*)(?:\"[^\"]*\"|[0-9]+|true|false|null)", key));
		std::smatch match{};
		if (std::regex_search(text, match, key_re) && match.size() > 1)
		{
			if (only_if_missing)
			{
				return false;
			}

			text.replace(match.position(0), match.length(0), match[1].str() + std::string(raw_value));
			return true;
		}

		const usz brace_pos = text.find('{');
		if (brace_pos == umax)
		{
			return false;
		}

		const std::string insertion = fmt::format("\n  \"%s\": %s,", key, raw_value);
		text.insert(brace_pos + 1, insertion);
		return true;
	}

	void record_catalog_reference(std::string_view family, std::string_view artifact_type, std::string_view hash_key, std::string_view compatibility_tuple, std::string_view format_version, std::string_view settings_fingerprint, std::string_view gpu_fingerprint)
	{
		if (hash_key.empty() || family.empty())
		{
			return;
		}

		auto sanitize_token = [](std::string value)
		{
			for (char& c : value)
			{
				if (!std::isalnum(static_cast<uchar>(c)) && c != '-' && c != '_' && c != '.')
				{
					c = '_';
				}
			}
			return value;
		};

		const std::string title_id = sanitize_token(Emu.GetTitleID());
		if (title_id.empty())
		{
			return;
		}

		std::string app_version = sanitize_token(Emu.GetAppVersion());
		if (app_version.empty())
		{
			app_version = "unknown";
		}

		const auto [build_revision, build_hash] = rpcs3::get_commit_and_hash();
		const std::string build_id = sanitize_token(fmt::format("%s-%s", build_revision, build_hash));
		const std::string settings_hash = canonical_sha1_hex(settings_fingerprint.data(), settings_fingerprint.size());
		const std::string gpu_hash = canonical_sha1_hex(gpu_fingerprint.data(), gpu_fingerprint.size());

		const std::string run_id = fmt::format("%s-app%s-build%s-emu%u-cfg%s%s",
			title_id,
			app_version,
			build_id,
			emu_cache_schema_version,
			settings_hash.substr(0, 12),
			gpu_fingerprint.empty() ? "" : ("-gpu" + gpu_hash.substr(0, 12)));

		{
			const std::lock_guard lock(s_current_run_mutex);
			s_current_run_id_by_title[title_id] = run_id;
		}

		const std::string catalog_root = make_catalog_runs_root(title_id);
		if (!fs::is_dir(catalog_root) && !fs::create_path(catalog_root))
		{
			sys_log.error("Failed to initialize cache catalog directory %s (%s)", catalog_root, fs::g_tls_error);
			return;
		}

		const std::string path = catalog_root + run_id + ".json";
		const std::string escaped_hash = escape_json_string(hash_key);
		const std::string escaped_family = escape_json_string(family);
		const std::string escaped_artifact = escape_json_string(artifact_type);
		const std::string escaped_compat = escape_json_string(compatibility_tuple);
		const std::string escaped_format = escape_json_string(format_version);
		const std::string escaped_cfg = escape_json_string(settings_fingerprint);
		const std::string escaped_gpu = escape_json_string(gpu_fingerprint);
		const std::string escaped_build = escape_json_string(build_id);
		const u64 now = get_unix_epoch_seconds_now();

		std::string existing;
		const bool has_existing = fs::read_file(path, existing);
		if (has_existing)
		{
			upsert_json_key_value(existing, "created_at", std::to_string(now), true);
			upsert_json_key_value(existing, "pinned", "false", true);
			upsert_json_key_value(existing, "run_reason", "null", true);
			upsert_json_key_value(existing, "label", "null", true);
		}

		const std::string entry_snippet = fmt::format("\"hash\": \"%s\"", escaped_hash);
		if (has_existing && existing.find(entry_snippet) != std::string::npos)
		{
			upsert_json_key_value(existing, "last_accessed_at", std::to_string(now));
			write_text_file_atomic(path, existing);
			return;
		}

		const std::string new_entry = fmt::format(
			"\n    {\n      \"family\": \"%s\",\n      \"artifact\": \"%s\",\n      \"hash\": \"%s\",\n      \"compatibility\": \"%s\",\n      \"format\": \"%s\"\n    }",
			escaped_family, escaped_artifact, escaped_hash, escaped_compat, escaped_format);

		if (!has_existing || existing.empty())
		{
			const std::string text = fmt::format(
				"{\n  \"title_id\": \"%s\",\n  \"app_version\": \"%s\",\n  \"build_id\": \"%s\",\n  \"emu_cache_schema\": %u,\n  \"created_at\": %llu,\n  \"last_accessed_at\": %llu,\n  \"pinned\": false,\n  \"run_reason\": null,\n  \"label\": null,\n  \"settings_fingerprint\": \"%s\",\n  \"gpu_fingerprint\": \"%s\",\n  \"entries\": [%s\n  ]\n}\n",
				escape_json_string(title_id), escape_json_string(app_version), escaped_build, emu_cache_schema_version, now, now, escaped_cfg, escaped_gpu, new_entry);
			write_text_file_atomic(path, text);
			return;
		}

		upsert_json_key_value(existing, "last_accessed_at", std::to_string(now));

		const usz entries_end = existing.rfind("]");
		if (entries_end == umax)
		{
			return;
		}

		const bool has_entries = existing.find("\"hash\":") != umax;
		std::string updated = existing.substr(0, entries_end);
		if (has_entries)
		{
			updated += ",";
		}
		updated += new_entry;
		updated += existing.substr(entries_end);
		write_text_file_atomic(path, updated);
	}

	struct gc_run_info
	{
		std::string path;
		std::string title_id;
		std::string name;
		u64 timestamp = 0;
		bool pinned = false;
		bool retained = false;
		std::vector<std::string> hashes;
	};

	namespace
	{
		inline constexpr usz s_default_recent_runs_per_title = 5;

		static std::optional<u64> parse_u64_str(std::string_view raw)
		{
			u64 value = 0;
			const char* begin = raw.data();
			const char* end = raw.data() + raw.size();
			const auto [ptr, ec] = std::from_chars(begin, end, value);
			if (ec != std::errc{} || ptr != end)
			{
				return std::nullopt;
			}

			return value;
		}

		struct run_json_metadata
		{
			std::string title_id;
			bool pinned = false;
			u64 created_at = 0;
			std::optional<u64> last_accessed_at;
			std::string run_reason;
			std::string label;
			std::vector<std::string> hashes;
		};

		std::string extract_json_string(std::string_view text, std::string_view key)
		{
			const std::regex re(fmt::format("\"%s\"\\s*:\\s*\"([^\"]*)\"", key));
			const std::string input(text);
			std::smatch match{};
			if (std::regex_search(input, match, re) && match.size() > 1)
			{
				return match[1].str();
			}

			return {};
		}

		bool extract_json_bool(std::string_view text, std::string_view key)
		{
			const std::regex re(fmt::format("\"%s\"\\s*:\\s*(true|false)", key));
			const std::string input(text);
			std::smatch match{};
			if (std::regex_search(input, match, re) && match.size() > 1)
			{
				return match[1].str() == "true";
			}

			return false;
		}

		static std::optional<u64> parse_iso8601_timestamp(std::string_view value)
		{
			const std::regex re(R"(^\s*(\d{4})-(\d{2})-(\d{2})[Tt ](\d{2}):(\d{2}):(\d{2})(?:\.\d+)?(?:([Zz])|([+-])(\d{2}):?(\d{2}))?\s*$)");
			const std::string input(value);
			std::smatch match{};
			if (!std::regex_match(input, match, re))
			{
				return std::nullopt;
			}

			const auto year = parse_u64_str(match[1].str());
			const auto month = parse_u64_str(match[2].str());
			const auto day = parse_u64_str(match[3].str());
			const auto hour = parse_u64_str(match[4].str());
			const auto minute = parse_u64_str(match[5].str());
			const auto second = parse_u64_str(match[6].str());
			if (!year || !month || !day || !hour || !minute || !second)
			{
				return std::nullopt;
			}

			std::tm tm{};
			tm.tm_year = static_cast<int>(*year) - 1900;
			tm.tm_mon = static_cast<int>(*month) - 1;
			tm.tm_mday = static_cast<int>(*day);
			tm.tm_hour = static_cast<int>(*hour);
			tm.tm_min = static_cast<int>(*minute);
			tm.tm_sec = static_cast<int>(*second);

			std::time_t utc_seconds = 0;
#ifdef _WIN32
			utc_seconds = _mkgmtime(&tm);
#else
			utc_seconds = timegm(&tm);
#endif

			if (utc_seconds < 0)
			{
				return std::nullopt;
			}

			s64 offset = 0;
			if (match[8].matched && match[9].matched && match[10].matched)
			{
				const auto offset_h = parse_u64_str(match[9].str());
				const auto offset_m = parse_u64_str(match[10].str());
				if (!offset_h || !offset_m)
				{
					return std::nullopt;
				}

				offset = static_cast<s64>(*offset_h) * 3600 + static_cast<s64>(*offset_m) * 60;
				if (match[8].str() == "-")
				{
					offset = -offset;
				}
			}

			return static_cast<u64>(static_cast<s64>(utc_seconds) - offset);
		}

		static std::optional<u64> extract_json_timestamp(std::string_view text, std::string_view key)
		{
			const std::string input(text);
			std::smatch match{};

			const std::regex number_re(fmt::format("\"%s\"\\s*:\\s*([0-9]+)", key));
			if (std::regex_search(input, match, number_re) && match.size() > 1)
			{
				if (const auto value = parse_u64_str(match[1].str()))
				{
					return *value;
				}
			}

			const std::regex string_re(fmt::format("\"%s\"\\s*:\\s*\"([^\"]+)\"", key));
			if (std::regex_search(input, match, string_re) && match.size() > 1)
			{
				const std::string raw = match[1].str();
				if (!raw.empty() && std::all_of(raw.begin(), raw.end(), [](char ch) { return std::isdigit(static_cast<uchar>(ch)) != 0; }))
				{
					if (const auto value = parse_u64_str(raw))
					{
						return *value;
					}
				}

				return parse_iso8601_timestamp(raw);
			}

			return std::nullopt;
		}

		static run_json_metadata parse_run_json_metadata(std::string_view content, u64 file_mtime)
		{
			run_json_metadata metadata{};
			metadata.title_id = extract_json_string(content, "title_id");
			metadata.pinned = extract_json_bool(content, "pinned");
			metadata.created_at = extract_json_timestamp(content, "created_at").value_or(file_mtime);
			metadata.last_accessed_at = extract_json_timestamp(content, "last_accessed_at");
			metadata.run_reason = extract_json_string(content, "run_reason");
			metadata.label = extract_json_string(content, "label");

			const std::regex hash_re("\"hash\"\\s*:\\s*\"([0-9a-fA-F]{40})\"");
			const std::string input(content);
			for (std::sregex_iterator it(input.begin(), input.end(), hash_re), end_it; it != end_it; ++it)
			{
				metadata.hashes.emplace_back((*it)[1].str());
			}

			return metadata;
		}



		std::vector<gc_run_info> scan_catalog_runs()
		{
			std::vector<gc_run_info> runs;
			const std::string catalog_root = rpcs3::utils::get_cache_dir() + "catalog/";
			if (!fs::is_dir(catalog_root))
			{
				return runs;
			}

			for (const auto& title_dir : fs::dir(catalog_root))
			{
				if (!title_dir.is_directory || title_dir.name == "." || title_dir.name == "..")
				{
					continue;
				}

				const std::string runs_dir = catalog_root + title_dir.name + "/runs/";
				if (!fs::is_dir(runs_dir))
				{
					continue;
				}

				for (const auto& run_file : fs::dir(runs_dir))
				{
					if (run_file.is_directory || run_file.name == "." || run_file.name == ".." || !run_file.name.ends_with(".json"))
					{
						continue;
					}

					std::string content;
					const std::string full_path = runs_dir + run_file.name;
					if (!fs::read_file(full_path, content))
					{
						continue;
					}

					gc_run_info run{};
					run.path = full_path;
					run.name = run_file.name;

					const run_json_metadata metadata = parse_run_json_metadata(content, run_file.mtime);
					run.timestamp = metadata.last_accessed_at.value_or(metadata.created_at);
					run.title_id = metadata.title_id;
					if (run.title_id.empty())
					{
						run.title_id = title_dir.name;
					}
					run.pinned = metadata.pinned;
					run.hashes = metadata.hashes;
					runs.emplace_back(std::move(run));
				}
			}

			return runs;
		}

		struct cas_blob_info
		{
			std::string key;
			std::string path;
			u64 size = 0;
		};

		std::vector<cas_blob_info> scan_cas_blobs()
		{
			std::vector<cas_blob_info> blobs;
			const std::string cas_root = get_shared_cas_root();
			if (cas_root.empty() || !fs::is_dir(cas_root))
			{
				return blobs;
			}

			auto scan_dir = [&blobs](const std::string& base_path)
			{
				for (const auto& entry : fs::dir(base_path))
				{
					if (entry.name == "." || entry.name == ".." || entry.is_directory)
					{
						continue;
					}

					blobs.push_back({entry.name, base_path + entry.name, entry.size});
				}
			};

			for (const auto& entry : fs::dir(cas_root))
			{
				if (entry.name == "." || entry.name == "..")
				{
					continue;
				}

				if (entry.is_directory)
				{
					scan_dir(cas_root + entry.name + "/");
				}
				else
				{
					blobs.push_back({entry.name, cas_root + entry.name, entry.size});
				}
			}

			return blobs;
		}
	}


	std::vector<run_info> list_runs(std::string_view title_id)
	{
		std::vector<run_info> out;
		if (title_id.empty())
		{
			return out;
		}

		const std::string runs_root = make_catalog_runs_root(title_id);
		if (!fs::is_dir(runs_root))
		{
			return out;
		}

		for (const auto& run_file : fs::dir(runs_root))
		{
			if (run_file.is_directory || run_file.name == "." || run_file.name == ".." || !run_file.name.ends_with(".json"))
			{
				continue;
			}

			std::string content;
			const std::string full_path = runs_root + run_file.name;
			if (!fs::read_file(full_path, content))
			{
				continue;
			}

			const run_json_metadata metadata = parse_run_json_metadata(content, run_file.mtime);
			run_info info{};
			info.title_id = metadata.title_id.empty() ? std::string(title_id) : std::move(metadata.title_id);
			info.run_id = run_file.name.substr(0, run_file.name.size() - 5);
			info.created_at = metadata.created_at;
			info.last_accessed_at = metadata.last_accessed_at.value_or(metadata.created_at);
			info.pinned = metadata.pinned;
			info.run_reason = std::move(metadata.run_reason);
			info.label = std::move(metadata.label);
			out.emplace_back(std::move(info));
		}

		std::sort(out.begin(), out.end(), [](const run_info& lhs, const run_info& rhs)
		{
			if (lhs.last_accessed_at != rhs.last_accessed_at)
			{
				return lhs.last_accessed_at > rhs.last_accessed_at;
			}

			return lhs.run_id > rhs.run_id;
		});

		return out;
	}

	bool set_run_pinned(std::string_view title_id, std::string_view run_id, bool pinned)
	{
		if (title_id.empty() || run_id.empty())
		{
			return false;
		}

		std::string run_json;
		const std::string run_path = make_run_json_path(title_id, run_id);
		if (!fs::read_file(run_path, run_json))
		{
			return false;
		}

		if (!upsert_json_key_value(run_json, "pinned", pinned ? "true" : "false"))
		{
			return false;
		}

		return write_text_file_atomic(run_path, run_json);
	}

	bool set_current_run_pinned(bool pinned)
	{
		const std::string title_id = Emu.GetTitleID();
		if (title_id.empty())
		{
			return false;
		}

		std::string run_id;
		{
			const std::lock_guard lock(s_current_run_mutex);
			if (const auto it = s_current_run_id_by_title.find(title_id); it != s_current_run_id_by_title.end())
			{
				run_id = it->second;
			}
		}

		if (!run_id.empty())
		{
			return set_run_pinned(title_id, run_id, pinned);
		}

		auto runs = list_runs(title_id);
		if (runs.empty())
		{
			return false;
		}

		return set_run_pinned(title_id, runs.front().run_id, pinned);
	}

	void run_policy_gc(bool hard_emergency_mode)
	{
		auto runs = scan_catalog_runs();
		std::unordered_map<std::string, std::vector<usz>> runs_by_title;
		for (usz i = 0; i < runs.size(); ++i)
		{
			runs_by_title[runs[i].title_id].push_back(i);
		}

		u64 retained_pinned = 0;
		u64 retained_recent = 0;
		for (auto& title_runs : runs_by_title)
		{
			auto& indices = title_runs.second;
			std::sort(indices.begin(), indices.end(), [&](usz a, usz b)
			{
				if (runs[a].timestamp != runs[b].timestamp)
				{
					return runs[a].timestamp > runs[b].timestamp;
				}
				return runs[a].name > runs[b].name;
			});

			usz kept_recent = 0;
			for (const usz idx : indices)
			{
				if (runs[idx].pinned)
				{
					runs[idx].retained = true;
					retained_pinned++;
					continue;
				}

				if (kept_recent < s_default_recent_runs_per_title)
				{
					runs[idx].retained = true;
					retained_recent++;
					kept_recent++;
				}
			}
		}

		auto build_reachable = [&runs]()
		{
			std::unordered_set<std::string> reachable_hashes;
			for (const auto& run : runs)
			{
				if (!run.retained)
				{
					continue;
				}

				for (const auto& hash : run.hashes)
				{
					reachable_hashes.emplace(hash);
				}
			}
			return reachable_hashes;
		};

		auto blobs = scan_cas_blobs();
		u64 blobs_removed = 0;
		u64 bytes_reclaimed = 0;

		auto delete_orphans = [&](const std::unordered_set<std::string>& reachable_hashes)
		{
			for (auto& blob : blobs)
			{
				if (blob.path.empty() || reachable_hashes.contains(blob.key))
				{
					continue;
				}

				if (fs::remove_file(blob.path))
				{
					blobs_removed++;
					bytes_reclaimed += blob.size;
					blob.path.clear();
				}
			}
		};

		auto reachable_hashes = build_reachable();
		delete_orphans(reachable_hashes);

		const std::string cache_location = rpcs3::utils::get_hdd1_dir() + "/caches";
		const u64 max_size = static_cast<u64>(g_cfg.vfs.cache_max_size) * 1024 * 1024;
		if (max_size == 0)
		{
			if (fs::is_dir(cache_location))
			{
				fs::remove_all(cache_location, false);
			}
		}
		else if (fs::is_dir(cache_location))
		{
			u64 cur_size = fs::get_dir_size(cache_location);
			if (cur_size != umax && cur_size > max_size)
			{
				std::vector<usz> evictable;
				for (usz i = 0; i < runs.size(); ++i)
				{
					if (runs[i].retained && (hard_emergency_mode || !runs[i].pinned))
					{
						evictable.push_back(i);
					}
				}

				std::sort(evictable.begin(), evictable.end(), [&](usz a, usz b)
				{
					if (runs[a].timestamp != runs[b].timestamp)
					{
						return runs[a].timestamp < runs[b].timestamp;
					}
					return runs[a].name < runs[b].name;
				});

				for (const usz idx : evictable)
				{
					if (cur_size <= max_size)
					{
						break;
					}

					runs[idx].retained = false;
					fs::remove_file(runs[idx].path);

					reachable_hashes = build_reachable();
					delete_orphans(reachable_hashes);

					cur_size = fs::get_dir_size(cache_location);
					if (cur_size == umax)
					{
						break;
					}
				}
			}
		}

		u64 retained_total = 0;
		for (const auto& run : runs)
		{
			retained_total += run.retained ? 1 : 0;
		}

		sys_log.notice("Cache GC summary: runs_scanned=%u runs_retained=%u retained_pinned=%u retained_recent=%u reachable_hashes=%u blobs_removed=%u bytes_reclaimed=%u",
			static_cast<u32>(runs.size()),
			static_cast<u32>(retained_total),
			static_cast<u32>(retained_pinned),
			static_cast<u32>(retained_recent),
			static_cast<u32>(reachable_hashes.size()),
			static_cast<u32>(blobs_removed),
			bytes_reclaimed);
	}

	void limit_cache_size()
	{
		run_policy_gc(false);
	}

}
