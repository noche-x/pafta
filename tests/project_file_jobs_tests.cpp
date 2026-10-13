// SPDX-License-Identifier: GPL-2.0-only
#include "app_internal.hpp"
#include <imgui.h>

#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace pafta;
using namespace pafta::app_internal;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void finish_jobs(EditorProject& project, EditorUi& ui, std::string& status) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (project_file_jobs_pending(project)) {
        service_project_file_jobs(project, ui, status);
        require(std::chrono::steady_clock::now() < deadline, "file worker did not complete");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void finish_archive_jobs(ArchiveWorkspace& workspace) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (archive_file_jobs_pending(workspace)) {
        service_archive_file_jobs(workspace);
        require(std::chrono::steady_clock::now() < deadline, "archive worker did not complete");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

EditorProject make_project(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    EditorProject project;
    project.root = std::filesystem::weakly_canonical(root);
    SbbBuilding building;
    building.version = 14;
    building.name = "house";
    building.size_z = 1;
    building.cells.resize(1);
    project.buildings.push_back({root / "building/house.sbb", "house", building, true});
    project.building_bytes.push_back(rebuild_sbb(building));
    SbcMap map;
    map.version = 12;
    map.buildings.push_back({0, "house", 4, 0, 0, 0});
    project.city = LoadedAsset<SbcMap>{root / "city2.sbc", "city2", map, true};
    project.city_bytes = rebuild_sbc(map);
    write_file_bytes(root / "asphalt2.png", {1, 2, 3});
    write_file_bytes(root / "asphalt2_material.png", {4, 5, 6});
    return project;
}

void test_edit_during_save(const std::filesystem::path& root) {
    auto project = make_project(root);
    EditorUi ui;
    ui.city_dirty = ui.building_dirty = true;
    std::string status;
    request_map_save(project, ui, root, status);
    require(project_file_jobs_pending(project), "save request did not return a running worker");
    project.city->data.buildings[0].x = 20;
    project.buildings[0].data.name = "changed while saving";
    finish_jobs(project, ui, status);
    require(parse_sbc_file(root / "city2.sbc").buildings[0].x == 4, "save used live city edits instead of its source snapshot");
    auto archive = parse_csx_file(root / "test.csx");
    require(parse_sbb(normalize_packed_sbb_bytes(archive.entries[0].payload)).name == "house",
        "save used a building edited after the request");
    require(project.city->data.buildings[0].x == 20 && project.buildings[0].data.name == "changed while saving" &&
        ui.city_dirty && ui.building_dirty && status.find("Newer edits remain unsaved") != std::string::npos,
        "completed save replaced newer edits or cleared their dirty state");
    request_map_save(project, ui, root, status);
    finish_jobs(project, ui, status);
    require(!ui.city_dirty && !ui.building_dirty && !project.buildings[0].dirty,
        "unchanged save completion did not mark its source state clean");
    require(parse_sbc_file(root / "city2.sbc").buildings[0].x == 20, "second save omitted the later city edit");
}

void test_failed_save(const std::filesystem::path& root) {
    auto project = make_project(root / "source");
    EditorUi ui;
    ui.city_dirty = true;
    const auto output = root / "blocked";
    std::filesystem::create_directories(output / "city2.sbc");
    std::string status;
    const auto original_path = project.city->path;
    request_map_save(project, ui, output, status);
    require(project_file_jobs_pending(project), "failed output was written synchronously");
    finish_jobs(project, ui, status);
    require(status.starts_with("Map save failed:") && ui.city_dirty && project.city->path == original_path,
        "failed worker save cleared unsaved edits or changed their source path");
    require(!std::filesystem::exists(output / "test.csx"), "failed worker save installed a partial archive");
}

void test_export_snapshot_and_failure(const std::filesystem::path& root) {
    auto project = make_project(root);
    EditorUi ui;
    std::string status;
    const auto output = root / "house.sbb";
    queue_project_file_job(project, [data = project.buildings[0].data, output] {
        write_file_bytes(output, rebuild_sbb(data));
    }, "Exported house");
    project.buildings[0].data.name = "new name";
    finish_jobs(project, ui, status);
    require(status == "Exported house" && parse_sbb_file(output).name == "house", "export did not own its source snapshot");
    queue_project_file_job(project, [] { throw std::runtime_error("test write error"); }, "not reached");
    finish_jobs(project, ui, status);
    require(status == "Export failed: test write error", "worker export failure was hidden");
}

void test_archive_save_snapshot_and_failure(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    ArchiveWorkspace workspace;
    const auto folder = root.string();
    require(folder.size() < workspace.output_path.size(), "archive test path is too long");
    std::copy(folder.begin(), folder.end(), workspace.output_path.begin());
    workspace.archive.entries.push_back({0, CsxEntryKind::Unknown, 0, 0, 0, "entry", {1, 2}});
    workspace.dirty = true;
    require(save_archive_workspace(workspace) && archive_file_jobs_pending(workspace), "archive save did not queue a worker");
    workspace.archive.entries[0].payload = {3, 4};
    finish_archive_jobs(workspace);
    require(parse_csx_file(root / "test.csx").entries[0].payload == std::vector<std::uint8_t>{1, 2} &&
        workspace.archive.entries[0].payload == std::vector<std::uint8_t>{3, 4} && workspace.dirty,
        "archive save changed newer edits or wrote live bytes");
    require(save_archive_workspace(workspace), "second archive save did not start");
    finish_archive_jobs(workspace);
    require(!workspace.dirty && parse_csx_file(root / "test.csx").entries[0].payload == std::vector<std::uint8_t>{3, 4},
        "archive save did not mark its unchanged source clean");

    std::filesystem::remove(root / "test.csx");
    std::filesystem::create_directory(root / "test.csx");
    workspace.dirty = true;
    require(save_archive_workspace(workspace), "archive failure was not handled on a worker");
    finish_archive_jobs(workspace);
    require(workspace.dirty && workspace.message.starts_with("Archive operation failed:"), "archive worker failure cleared dirty edits");
}

void test_project_archive_writer_exclusion(const std::filesystem::path& root) {
    auto project = make_project(root);
    EditorUi ui;
    std::string status;
    std::promise<void> release;
    auto gate = release.get_future().share();
    queue_project_file_job(project, [gate] { gate.wait(); }, "Export finished");
    ArchiveWorkspace workspace;
    const auto folder = root.string();
    std::copy(folder.begin(), folder.end(), workspace.output_path.begin());
    bool rejected = false;
    try {
        save_archive_workspace(workspace);
    } catch (const std::runtime_error& error) {
        rejected = std::string{error.what()}.find("still running") != std::string::npos;
    }
    release.set_value();
    finish_jobs(project, ui, status);
    finish_archive_jobs(workspace);
    require(rejected, "archive writer ran concurrently with the project file worker");
    require(save_archive_workspace(workspace), "completed project worker did not release the writer ticket");
    finish_archive_jobs(workspace);
}

void test_control_preference_migration(const std::filesystem::path& root) {
    const auto path = root / "prefs.txt";
    const auto path_text = path.string();
    const char* previous = std::getenv("PAFTA_PREFS_PATH");
    const std::optional<std::string> previous_path = previous ? std::optional<std::string>{previous} : std::nullopt;
    const auto set_path = [](const std::string& value) {
#ifdef _WIN32
        _putenv_s("PAFTA_PREFS_PATH", value.c_str());
#else
        if (value.empty()) unsetenv("PAFTA_PREFS_PATH");
        else setenv("PAFTA_PREFS_PATH", value.c_str(), 1);
#endif
    };
    struct Restore {
        std::optional<std::string> previous;
        decltype(set_path) set;
        ~Restore() { set(previous.value_or("")); }
    } restore{previous_path, set_path};
    set_path(path_text);
    require(preferences_path() == path, "isolated preferences path was ignored");
    const std::string text = "camera.fly_speed.city=90\ncamera.fly_speed=77\ncamera.fly_speed.blocks=nan\n"
        "camera.controls=alex\ncamera.blender_mouse=1\nplacement.height_locked=1\nplacement.height_y=7\n";
    write_file_bytes(path, {text.begin(), text.end()});
    EditorUi ui;
    load_preferences(ui);
    require(ui.camera_fly_speeds == std::array<float, 5>{90, 77, 77, 77, 77}, "legacy fly speed migration lost per-mode overrides");
    require(ui.controls_chosen && !ui.blender_mouse && ui.place_height_locked && ui.place_height_y == 7,
        "new control preset or locked placement preference did not load");
    ui.tab = EditorTab::Buildings;
    camera_fly_speed(ui) = 23;
    save_preferences(ui);
    EditorUi reopened;
    load_preferences(reopened);
    require(reopened.camera_fly_speeds == std::array<float, 5>{90, 23, 77, 77, 77} && !reopened.blender_mouse,
        "per-mode fly speeds and control preset did not survive preference save");
}
} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("pafta-file-jobs-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
    } cleanup{root};
    ImGui::CreateContext();
    try {
        test_edit_during_save(root / "edit");
        test_failed_save(root / "failure");
        test_export_snapshot_and_failure(root / "export");
        test_archive_save_snapshot_and_failure(root / "archive");
        test_project_archive_writer_exclusion(root / "writer");
        test_control_preference_migration(root / "prefs");
        ImGui::DestroyContext();
        std::cout << "Asynchronous save and export checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        ImGui::DestroyContext();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
