#pragma once

#include <string>
#include <vector>

namespace map_editor::app_internal {

std::vector<std::string> dialog_open_archive_assets();
std::vector<std::string> dialog_open_material_image();
std::string dialog_save_csx();
std::string dialog_extract_folder();
std::string dialog_open_map_folder();

}
