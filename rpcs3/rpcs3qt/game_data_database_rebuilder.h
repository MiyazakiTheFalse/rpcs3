#pragma once

#include <QWidget>
#include <optional>

#include "Emu/system_utils.hpp"

class game_data_database_rebuilder
{
public:
	static std::optional<rpcs3::utils::game_data_database_result> run(QWidget* parent, const std::map<std::string, std::string>& games_yml_entries);
};
