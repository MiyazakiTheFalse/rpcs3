#include "stdafx.h"
#include "cache_utils.hpp"
#include "system_utils.hpp"
#include "system_config.h"
#include "IdManager.h"
#include "Emu/Cell/lv2/sys_sync.h"
#include "Emu/Cell/PPUAnalyser.h"
#include "Emu/Cell/PPUThread.h"
#include "Crypto/sha1.h"

#include <array>

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

	std::string make_compatibility_tuple(std::string_view domain, std::string_view backend_id, std::string_view platform_fields)
	{
		if (platform_fields.empty())
		{
			platform_fields = get_platform_cache_id();
		}

		return fmt::format("schema=%u|domain=%s|backend=%s|platform=%s", emu_cache_schema_version, domain, backend_id, platform_fields);
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
		if (record.empty())
		{
			return false;
		}

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

	std::string put_to_cas(const void* data, std::size_t size, std::string_view extension)
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

		const std::string hash_key = canonical_sha1_hex(data, size);
		const std::string object_path = cas_root + hash_key;
		(void)extension;

		if (fs::stat_t st{}; fs::get_stat(object_path, st) && st.size == size)
		{
			return hash_key;
		}


		if (!write_file_atomic(object_path, data, size))
		{
			if (fs::stat_t existing{}; fs::get_stat(object_path, existing) && existing.size == size)
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

		out.resize(f.size());
		return f.read(out.data(), out.size()) == out.size();
	}

	std::string make_manifest_record(std::string_view artifact_type, const std::string& hash_key, std::string_view metadata, std::string_view compatibility_tuple, std::string_view format_version)
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

		usz prev = p1;
		std::array<std::string*, 3> tails = { &out.metadata, &out.compatibility_tuple, &out.format_version };
		for (std::string* target : tails)
		{
			if (prev == umax)
			{
				break;
			}

			const usz next = line.find('|', prev + 1);
			if (next == umax)
			{
				*target = std::string(line.substr(prev + 1));
				prev = umax;
			}
			else
			{
				*target = std::string(line.substr(prev + 1, next - prev - 1));
				prev = next;
			}
		}

		return true;
	}

	bool is_manifest_record_compatible(const manifest_record& rec, std::string_view expected_artifact_type, std::string_view expected_compatibility_tuple, std::string_view expected_format_version)
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

		return true;
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
