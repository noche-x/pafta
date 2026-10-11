#include "file_dialogs.hpp"

// Keep this Windows-header-based library out of the raylib translation units.
#include <portable-file-dialogs.h>

namespace map_editor::app_internal {

std::vector<std::string> dialog_open_archive_assets() {
    return pfd::open_file("Add archive assets", "", {"Assets", "*.csx *.sbb *.sbl *.png"},
                          pfd::opt::multiselect).result();
}

std::vector<std::string> dialog_open_material_image() {
    return pfd::open_file("Material image", "", {"PNG image", "*.png"}).result();
}

std::string dialog_save_csx() {
    return pfd::save_file("Build CSX", "build.csx", {"CSX archive", "*.csx"}).result();
}

std::string dialog_extract_folder() {
    return pfd::select_folder("Extract selected files").result();
}

std::string dialog_open_map_folder() {
    return pfd::select_folder("Open map folder").result();
}

}
