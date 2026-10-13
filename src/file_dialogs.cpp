// SPDX-License-Identifier: GPL-2.0-only
#include "file_dialogs.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

#define NFD_THROWS_EXCEPTIONS
#include <nfd.hpp>

namespace pafta::app_internal {
namespace {

std::string path_utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {bytes.begin(), bytes.end()};
}

std::filesystem::path path_from_utf8(std::string_view text) {
    return std::filesystem::path{std::u8string{text.begin(), text.end()}};
}

bool dialog_accepted(nfdresult_t result) {
    if (result == NFD_OKAY) return true;
    if (result == NFD_CANCEL) return false;
    throw std::runtime_error(std::string{"Native file dialog failed: "} + NFD::GetError());
}

}

std::vector<std::filesystem::path> dialog_open_archive_assets() {
    NFD::Guard guard;
    NFD::UniquePathSet selected;
    const nfdu8filteritem_t filters[] = {{"Archive assets", "csx,sbb,sbl,png"}};
    if (!dialog_accepted(NFD::OpenDialogMultiple(selected, filters, 1))) return {};

    nfdpathsetsize_t count = 0;
    dialog_accepted(NFD::PathSet::Count(selected, count));
    std::vector<std::filesystem::path> paths;
    paths.reserve(count);
    for (nfdpathsetsize_t index = 0; index < count; ++index) {
        NFD::UniquePathSetPathU8 path;
        dialog_accepted(NFD::PathSet::GetPath(selected, index, path));
        paths.push_back(path_from_utf8(path.get()));
    }
    return paths;
}

std::optional<std::filesystem::path> dialog_open_material_image() {
    NFD::Guard guard;
    NFD::UniquePathU8 selected;
    const nfdu8filteritem_t filters[] = {{"PNG image", "png"}};
    if (!dialog_accepted(NFD::OpenDialog(selected, filters, 1))) return std::nullopt;
    return path_from_utf8(selected.get());
}

std::optional<std::filesystem::path> dialog_save_file(
    const std::filesystem::path& default_path, const char* label, const char* extensions) {
    NFD::Guard guard;
    NFD::UniquePathU8 selected;
    const nfdu8filteritem_t filters[] = {{label, extensions}};
    const auto directory = path_utf8(default_path.parent_path());
    const auto filename = path_utf8(default_path.filename());
    if (!dialog_accepted(NFD::SaveDialog(selected, filters, 1, directory.c_str(), filename.c_str()))) {
        return std::nullopt;
    }
    return path_from_utf8(selected.get());
}

std::optional<std::filesystem::path> dialog_pick_folder(const std::filesystem::path& default_directory) {
    NFD::Guard guard;
    NFD::UniquePathU8 selected;
    const auto directory = path_utf8(default_directory);
    if (!dialog_accepted(NFD::PickFolder(selected, directory.c_str()))) return std::nullopt;
    return path_from_utf8(selected.get());
}

}
