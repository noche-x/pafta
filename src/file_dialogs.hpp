// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <filesystem>
#include <optional>
#include <vector>

namespace pafta::app_internal {

// Cancelling returns no path. Native dialog failures throw with details.
std::vector<std::filesystem::path> dialog_open_archive_assets();
std::vector<std::filesystem::path> dialog_open_map_assets();
std::vector<std::filesystem::path> dialog_open_files(const char* label, const char* extensions);
std::optional<std::filesystem::path> dialog_open_material_image();
std::optional<std::filesystem::path> dialog_save_file(
    const std::filesystem::path& default_path, const char* label, const char* extensions);
std::optional<std::filesystem::path> dialog_pick_folder(
    const std::filesystem::path& default_directory = {});

}
