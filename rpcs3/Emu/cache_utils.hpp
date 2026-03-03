#pragma once

#include <string>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>
#include "util/types.hpp"

namespace rpcs3::cache
{
	std::string get_ppu_cache();
	std::string get_shared_cas_root();

	std::string put_to_cas(const void* data, std::size_t size, std::string_view extension = {});
	bool get_from_cas(const std::string& hash_key, std::vector<uchar>& out);
	std::string make_manifest_record(std::string_view artifact_type, const std::string& hash_key, std::string_view metadata = {});
	void limit_cache_size();
}
