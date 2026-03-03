#include "stdafx.h"
#include "cache_utils.hpp"
#include "system_utils.hpp"
#include "system_config.h"
#include "IdManager.h"
#include "Emu/Cell/lv2/sys_sync.h"
#include "Emu/Cell/PPUAnalyser.h"
#include "Emu/Cell/PPUThread.h"
#include "Crypto/sha1.h"

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

		if (!fs::write_file(object_path, fs::rewrite, data, size))
		{
			sys_log.error("Failed to write CAS object %s (%s)", object_path, fs::g_tls_error);
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

	std::string make_manifest_record(std::string_view artifact_type, const std::string& hash_key, std::string_view metadata)
	{
		std::string record = fmt::format("%s|%s", artifact_type, hash_key);
		if (!metadata.empty())
		{
			record += "|";
			record += metadata;
		}
		record += "\n";
		return record;
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
