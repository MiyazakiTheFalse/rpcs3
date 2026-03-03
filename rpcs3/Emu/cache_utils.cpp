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

#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
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

		const std::string catalog_root = rpcs3::utils::get_cache_dir() + "catalog/" + title_id + "/runs/";
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

		std::string existing;
		const bool has_existing = fs::read_file(path, existing);

		const std::string entry_snippet = fmt::format("\"hash\": \"%s\"", escaped_hash);
		if (has_existing && existing.find(entry_snippet) != std::string::npos)
		{
			return;
		}

		const std::string new_entry = fmt::format(
			"\n    {\n      \"family\": \"%s\",\n      \"artifact\": \"%s\",\n      \"hash\": \"%s\",\n      \"compatibility\": \"%s\",\n      \"format\": \"%s\"\n    }",
			escaped_family, escaped_artifact, escaped_hash, escaped_compat, escaped_format);

		if (!has_existing || existing.empty())
		{
			const std::string text = fmt::format(
				"{\n  \"title_id\": \"%s\",\n  \"app_version\": \"%s\",\n  \"build_id\": \"%s\",\n  \"emu_cache_schema\": %u,\n  \"settings_fingerprint\": \"%s\",\n  \"gpu_fingerprint\": \"%s\",\n  \"entries\": [%s\n  ]\n}\n",
				escape_json_string(title_id), escape_json_string(app_version), escaped_build, emu_cache_schema_version, escaped_cfg, escaped_gpu, new_entry);
			write_text_file_atomic(path, text);
			return;
		}

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

	void limit_cache_size()
	{
		const std::string cache_location = rpcs3::utils::get_hdd1_dir() + "/caches";

		if (!fs::is_dir(cache_location))
		{
			sys_log.warning("Cache does not exist (%s)", cache_location);
			return;
		}

		const u64 size = fs::get_dir_size(cache_location);

		if (size == umax)
		{
			sys_log.error("Could not calculate cache directory '%s' size (%s)", cache_location, fs::g_tls_error);
			return;
		}

		const u64 max_size = static_cast<u64>(g_cfg.vfs.cache_max_size) * 1024 * 1024;

		if (max_size == 0) // Everything must go, so no need to do checks
		{
			fs::remove_all(cache_location, false);
			sys_log.success("Cleared disk cache");
			return;
		}

		if (size <= max_size)
		{
			sys_log.trace("Cache size below limit: %llu/%llu", size, max_size);
			return;
		}

		sys_log.success("Cleaning disk cache...");
		std::vector<fs::dir_entry> file_list{};
		fs::dir cache_dir(cache_location);
		if (!cache_dir)
		{
			sys_log.error("Could not open cache directory '%s' (%s)", cache_location, fs::g_tls_error);
			return;
		}

		// retrieve items to delete
		for (const auto &item : cache_dir)
		{
			if (item.name != "." && item.name != "..")
				file_list.push_back(item);
		}

		cache_dir.close();

		// sort oldest first
		std::sort(file_list.begin(), file_list.end(), FN(x.mtime < y.mtime));

		// keep removing until cache is empty or enough bytes have been cleared
		// cache is cleared down to 80% of limit to increase interval between clears
		const u64 to_remove = static_cast<u64>(size - max_size * 0.8);
		u64 removed = 0;
		for (const auto &item : file_list)
		{
			const std::string &name = cache_location + "/" + item.name;
			const bool is_dir = fs::is_dir(name);
			const u64 item_size = is_dir ? fs::get_dir_size(name) : item.size;

			if (is_dir && item_size == umax)
			{
				sys_log.error("Failed to calculate '%s' item '%s' size (%s)", cache_location, item.name, fs::g_tls_error);
				break;
			}

			if (is_dir ? !fs::remove_all(name, true, true) : !fs::remove_file(name))
			{
				sys_log.error("Could not remove cache directory '%s' item '%s' (%s)", cache_location, item.name, fs::g_tls_error);
				break;
			}

			removed += item_size;
			if (removed >= to_remove)
				break;
		}

		sys_log.success("Cleaned disk cache, removed %.2f MB", size / 1024.0 / 1024.0);
	}
}
