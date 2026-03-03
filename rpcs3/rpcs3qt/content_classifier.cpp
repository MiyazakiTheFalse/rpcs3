#include "content_classifier.h"

#include <QObject>

namespace content_classifier
{
	bucket classify_title_dir(const std::string& dir_path, const std::string& expected_title_id)
	{
		return rpcs3::utils::classify_title_dir(dir_path, expected_title_id);
	}

	bucket classify_pkg(std::string_view category, std::string_view app_ver, std::string_view target_app_ver)
	{
		return rpcs3::utils::classify_content_bucket(category, app_ver, target_app_ver);
	}

	QString bucket_name(bucket value)
	{
		switch (value)
		{
		case bucket::install_data: return QObject::tr("Install data");
		case bucket::patch_update_data: return QObject::tr("Patch/update data");
		case bucket::dlc_addon_data: return QObject::tr("DLC/add-on data");
		case bucket::save_data: return QObject::tr("Save data");
		case bucket::unknown: return QObject::tr("Unknown data");
		}

		return QObject::tr("Unknown data");
	}
}
