// SPDX-License-Identifier: GPL-2.0-only
#include "app_internal.hpp"
#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace pafta;
using namespace pafta::app_internal;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void test_save_and_history(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    SbbBuilding building{};
    building.version = 14;
    building.name = "house";
    building.size_z = 1;
    building.cells.resize(1);
    SbcMap map{};
    map.version = 12;
    map.buildings.push_back({0, "house", 0, 0, 0, 0});
    write_file_bytes(root / "building/house.sbb", rebuild_sbb(building));
    write_file_bytes(root / "city2.sbc", rebuild_sbc(map));
    SblTemplate buildblock{};
    buildblock.version = 1;
    buildblock.layers.resize(1);
    buildblock.layers[0].portal_type = 9;
    write_file_bytes(root / "buildblock/door.sbl", rebuild_sbl_buildblock(buildblock));

    std::string status;
    auto loaded = load_project_folder(root, status, false);
    require(loaded.has_value(), "could not load synthetic map: " + status);
    auto& project = *loaded;
    EditorUi ui{};
    commit_pending_edits(project, ui);
    begin_city_action(project, ui, "move house");
    project.city->data.buildings[0].x = 32;
    project.buildblocks[0].data.layers[0].portal_type = 10;
    project.buildblocks[0].dirty = true;
    ui.city_dirty = true;
    ui.buildblock_dirty = true;
    commit_pending_edits(project, ui);
    require(project.undo_stack.size() == 1, "one edit did not produce one undo step");
    save_map_to(project, ui, root, status);
    require(status.empty(), "save failed: " + status);
    require(!ui.city_dirty && !ui.buildblock_dirty && !project.buildblocks[0].dirty && project.saved_undo_depth == 1,
        "save did not mark the current history and assets as saved");
    auto reopened = load_project_folder(root, status, false);
    require(reopened && reopened->city->data.buildings[0].x == 32,
        "saved placement did not survive reopening");
    require(reopened->buildings.size() == 1, "saved archive lost its building");
    require(reopened->buildblocks.size() == 1 && reopened->buildblocks[0].data.layers[0].portal_type == 10,
        "saved buildblock edit did not survive reopening");
    const auto copy = root / "copy";
    std::filesystem::create_directory(copy);
    save_map_to(project, ui, copy, status);
    require(status.empty(), "Save As failed: " + status);
    auto copied = load_project_folder(copy, status, false);
    require(copied && copied->city->data.buildings[0].x == 32 && copied->buildblocks.size() == 1 &&
        copied->buildblocks[0].data.layers[0].portal_type == 10,
        "Save As lost the city or an unchanged buildblock");

    undo_city(project, ui, status);
    require(project.city->data.buildings[0].x == 0 && ui.city_dirty,
        "undo across a save did not restore the original placement");
    redo_city(project, ui, status);
    require(project.city->data.buildings[0].x == 32 && project.undo_stack.size() == 1,
        "redo across a save did not restore the edited placement");
    undo_city(project, ui, status);
    project.city->data.buildings[0].x = 64;
    commit_pending_edits(project, ui);
    require(project.redo_stack.empty() && !project.saved_undo_depth,
        "editing after undo kept an invalid redo path or saved marker");

    const auto city_before = read_file_bytes(root / "city2.sbc");
    const auto archive_before = read_file_bytes(root / "test.csx");
    const auto blocked = root / "blocked";
    std::filesystem::create_directories(blocked / "city2.sbc");
    status.clear();
    save_map_to(project, ui, blocked, status);
    require(status.starts_with("Map save failed:") && ui.city_dirty,
        "failed save cleared unsaved changes or omitted its error");
    require(read_file_bytes(root / "city2.sbc") == city_before &&
        read_file_bytes(root / "test.csx") == archive_before,
        "failed save changed the original map");
    require(!std::filesystem::exists(blocked / "test.csx"), "failed staging installed part of the map");

    project.save_blocking_load_errors.push_back("broken.sbb");
    status.clear();
    save_map_to(project, ui, root, status);
    require(status.find("broken.sbb") != std::string::npos && ui.city_dirty,
        "incomplete project was saved or lost its diagnostic");
    require(read_file_bytes(root / "city2.sbc") == city_before, "incomplete project overwrote the saved city");
}

void test_archive_snapshot(const std::filesystem::path& root) {
    const auto source = root / "source";
    const auto copy = root / "copy";
    std::filesystem::create_directories(source);
    std::filesystem::create_directories(copy);
    SbcMap map; map.version = 12;
    write_file_bytes(source / "city2.sbc", rebuild_sbc(map));
    CsxEntry texture{};
    texture.kind = CsxEntryKind::Texture; texture.type = 0x585a0004U; texture.name = "retained";
    texture.payload.resize(0x4c); texture.payload[0] = 1;
    CsxEntry unknown{}; unknown.name = "unknown"; unknown.payload = {17, 29};
    CsxArchive archive{0xc5f17e01U, 0, 0, {texture, unknown}};
    write_file_bytes(source / "test.csx", build_csx(archive));
    write_file_bytes(source / "texture/loose.png", {137, 'P', 'N', 'G', 13, 10, 26, 10});
    std::string status;
    auto loaded = load_project_folder(source, status, false);
    require(loaded && loaded->save_blocking_load_errors.empty(), "archive snapshot map failed to load");
    EditorUi ui;
    save_map_to(*loaded, ui, copy, status);
    require(status.empty(), "first Save As failed: " + status);
    const auto expected = read_file_bytes(copy / "test.csx");
    require(parse_csx(expected).entries.size() == 3, "archive, unknown, or loose texture was omitted");
    std::filesystem::remove_all(source);
    ui.city_dirty = true;
    save_map_to(*loaded, ui, copy, status);
    require(status.empty() && !ui.city_dirty, "Save As still depended on the removed source: " + status);
    require(read_file_bytes(copy / "test.csx") == expected, "saving after source removal lost asset bytes");
}

void test_archive_workspace(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    EditorProject project; project.root = root;
    SbcMap map; map.version = 10;
    SbcPortal portal; portal.legacy_blocks.resize(512); portal.legacy_blocks[0] = 0x40000000U;
    portal.textures.resize(4096); portal.itemsets.resize(512); map.portals.push_back(portal);
    project.city = LoadedAsset<SbcMap>{root / "city2.sbc", "city2", map};
    const auto original = project.city->data;
    ArchiveWorkspace workspace;
    import_project_archive(workspace, project);
    require(project.city->data == original && project.generated_city_assets.empty(),
        "copying map assets changed the legacy city");
    require(workspace.dirty, "archive edits are not visible to the app close workflow");
    workspace.archive.entries.push_back({0, CsxEntryKind::Unknown, 0, 0, 0, "entry", {1, 2}});
    std::filesystem::create_directory(root / "blocked.csx");
    bool failed = false;
    try { save_archive_to(workspace, root / "blocked.csx"); }
    catch (const std::runtime_error&) { failed = true; }
    require(failed && workspace.dirty, "failed archive save cleared unsaved edits");
    save_archive_to(workspace, root / "saved.csx");
    require(!workspace.dirty && parse_csx_file(root / "saved.csx").entries[0].payload == std::vector<std::uint8_t>{1, 2},
        "archive save did not preserve the workspace bytes");
}

void test_rename_history(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    EditorProject project; project.root = root;
    SbbBuilding building; building.version = 14; building.size_z = 1;
    project.buildings.push_back({root / "building/old.sbb", "old", building});
    SbcMap map; map.version = 12; map.buildings.push_back({0, "old", 0, 0, 0, 0});
    project.city = LoadedAsset<SbcMap>{root / "city2.sbc", "city2", map};
    EditorUi ui; ui.tab = EditorTab::Buildings;
    std::string status;
    commit_pending_edits(project, ui);
    rename_building(project, ui, "new", status);
    require(project.buildings[0].name == "new" && project.city->data.buildings[0].name == "new" && can_undo(project, ui, false),
        "rename did not change the asset and placement with an undo step");
    rename_building(project, ui, "latest", status);
    undo_edit(project, ui, status, false);
    require(project.buildings[0].name == "new", "undo of second rename lost the intermediate identity");
    undo_edit(project, ui, status, false);
    require(project.buildings[0].name == "old" && project.city->data.buildings[0].name == "old",
        "undo rename did not restore the original identity and placements");
    undo_edit(project, ui, status, true);
    undo_edit(project, ui, status, true);
    require(project.buildings[0].name == "latest" && project.city->data.buildings[0].name == "latest",
        "redo rename did not restore the final identity");
    save_map_to(project, ui, root, status);
    require(status.empty(), "renamed map failed to save: " + status);
    auto reopened = load_project_folder(root, status, false);
    require(reopened && reopened->buildings.size() == 1 && reopened->buildings[0].name == "latest" &&
        reopened->city->data.buildings[0].name == "latest", "saved rename did not survive reopening");
    undo_edit(project, ui, status, false);
    require(project.buildings[0].name == "new" && ui.building_dirty && ui.city_dirty,
        "undo rename after saving did not restore identity or dirty state");
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("pafta-workflow-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
    } cleanup{root};
    ImGui::CreateContext();
    try {
        test_save_and_history(root / "save");
        test_archive_snapshot(root / "snapshot");
        test_archive_workspace(root / "archive");
        test_rename_history(root / "rename");
        ImGui::DestroyContext();
        std::cout << "Save, reopen, failed save, undo, redo, and incomplete-source checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        ImGui::DestroyContext();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
