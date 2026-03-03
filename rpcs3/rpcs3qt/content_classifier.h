#pragma once

#include "Emu/system_utils.hpp"

#include <QString>

namespace content_classifier
{
	using bucket = rpcs3::utils::content_bucket;

	bucket classify_title_dir(const std::string& dir_path, const std::string& expected_title_id);
	bucket classify_pkg(std::string_view category, std::string_view app_ver, std::string_view target_app_ver);
	QString bucket_name(bucket value);
}
