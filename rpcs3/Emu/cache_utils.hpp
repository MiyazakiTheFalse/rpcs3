#pragma once

#include <string>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>
#include "util/types.hpp"

namespace rpcs3::cache
{
	inline constexpr u32 emu_cache_schema_version = 1;

	struct manifest_record
	{
		std::string artifact_type;
		std::string hash_key;
		std::string metadata;
		std::string compatibility_tuple;
		std::string format_version;
	};

	std::string get_ppu_cache();
	std::string get_shared_cas_root();
	std::string get_platform_cache_id();
	std::string make_compatibility_tuple(std::string_view domain, std::string_view backend_id, std::string_view platform_fields = {});

	std::string put_to_cas(const void* data, std::size_t size, std::string_view extension = {});
	bool get_from_cas(const std::string& hash_key, std::vector<uchar>& out);
	std::string make_manifest_record(std::string_view artifact_type, const std::string& hash_key, std::string_view metadata = {}, std::string_view compatibility_tuple = {}, std::string_view format_version = {});
	bool parse_manifest_record(std::string_view line, manifest_record& out);
	bool is_manifest_record_compatible(const manifest_record& rec, std::string_view expected_artifact_type, std::string_view expected_compatibility_tuple, std::string_view expected_format_version);
	void limit_cache_size();
}
