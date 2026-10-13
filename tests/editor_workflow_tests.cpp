// SPDX-License-Identifier: GPL-2.0-only
#include "app_internal.hpp"
#include <imgui.h>
#include <imgui_internal.h>

#include <chrono>
#include <algorithm>
#include <vector>
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
    // The game loads the road textures from the map folder, so Save As must bring them.
    write_file_bytes(root / "asphalt2.png", {1, 2, 3});
    write_file_bytes(root / "asphalt2_material.png", {4, 5, 6});

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
    require(read_file_bytes(copy / "asphalt2.png") == std::vector<std::uint8_t>{1, 2, 3} &&
        read_file_bytes(copy / "asphalt2_material.png") == std::vector<std::uint8_t>{4, 5, 6},
        "Save As did not copy the road textures the game loads from the map folder");

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
    write_file_bytes(source / "asphalt2.png", {1});
    write_file_bytes(source / "asphalt2_material.png", {2});
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

void test_block_face_paint(const std::filesystem::path& root) {
    EditorProject project; project.root = root;
    EditorUi ui; ui.tab = EditorTab::Blocks;
    std::string status;
    create_new_block(project, ui, status);
    add_block_patch(project, ui);
    auto& patch = project.blocks[0].data.authored_patches[0];
    patch.normal_flag_a = 0x50002U;
    mark_block_changed(project, ui, project.blocks[0].data);
    commit_pending_edits(project, ui);
    const auto before = project.blocks[0].data;
    paint_block_element(project, ui, 7U);
    require(patch.normal_flag_a == 0x50007U, "patch paint lost projection bits or selected the wrong slot");
    require(parse_sbl(project.block_bytes[0]).block->authored_patches[0].normal_flag_a == 0x50007U,
        "painted slot did not reach the exported SBL bytes");
    commit_pending_edits(project, ui);
    undo_edit(project, ui, status, false);
    require(project.blocks[0].data == before, "undo did not restore the patch's texture slot");
    undo_edit(project, ui, status, true);
    require(project.blocks[0].data.authored_patches[0].normal_flag_a == 0x50007U,
        "redo did not restore the painted patch slot");

    add_block_box(project, ui);
    paint_block_element(project, ui, 3U);
    require(project.blocks[0].data.lights[0].scalars[0] == 3U, "box paint selected the wrong slot");
    const auto painted = project.blocks[0].data;
    paint_block_element(project, ui, 8U);
    require(project.blocks[0].data == painted, "unsupported texture slot changed the block");
    ui.block_element = BlockElementKind::None;
    paint_block_element(project, ui, 5U);
    require(project.blocks[0].data == painted, "painting without a surface changed the block");
}

void test_block_switch(const std::filesystem::path& root) {
    EditorProject project; project.root = root;
    EditorUi ui; ui.tab = EditorTab::Blocks;
    std::string status;
    create_new_block(project, ui, status);
    const auto first = project.blocks[0].data;
    create_new_block(project, ui, status);
    ui.block_transform = BlockTransform::Grab;
    ui.block_transform_preview = first;
    select_block_asset(ui, 1U);
    require(!ui.block_transform_preview && ui.block_transform == BlockTransform::None && ui.block_selection_block == 1U,
        "changing blocks retained another block's transform");
    add_block_patch(project, ui);
    add_block_box(project, ui);
    require(project.blocks[0].data == first && project.blocks[1].data.authored_patches.size() == 1U && project.blocks[1].data.lights.size() == 1U,
        "new surfaces were added to the wrong block");
    SbcMap map; map.version = 12;
    SbcCityBlock preset; preset.name = project.blocks[1].name; map.city_blocks.push_back(preset);
    project.city = LoadedAsset<SbcMap>{{}, "city2", map};
    select_block_asset(ui, 0U);
    ui.city_tool = CityTool::Select; ui.city_selection = CitySelectionKind::Piece;
    ui.selected_piece_voxel = CityGridPoint{600, 1, 600}; ui.selected_piece_word = 0x80010000U;
    select_city_piece_mesh(project, ui);
    require(ui.selected_block == 1U, "opening a selected city piece defaulted to nblock instead of its mesh");
    // Adding during a transform cancels that preview before drawing/selecting the new surface.
    ui.block_transform = BlockTransform::Grab;
    ui.block_transform_preview = first;
    add_block_patch(project, ui);
    require(!ui.block_transform_preview && project.blocks[1].data.authored_patches.size() == 2U,
        "an old transform hid the new patch");
}

void test_graphics_debug_sampling() {
    require(gfx_debug_format_count(0) == "0" && gfx_debug_format_count(999) == "999" &&
        gfx_debug_format_count(1000) == "1,000" && gfx_debug_format_count(1234567890ULL) == "1,234,567,890" &&
        gfx_debug_format_count(18446744073709551615ULL) == "18,446,744,073,709,551,615", "graphics counters do not group digits correctly");
    GfxDebugStats stats;
    require(!stats.push_frame_time(10.0F, 0.0) && !stats.push_frame_time(30.0F, 0.25) && stats.frame_time_count == 0,
        "frame graph advanced before the half-second interval");
    require(stats.push_frame_time(20.0F, 0.5) && stats.frame_time_count == 1 && stats.frame_time_history[0] == 20.0F &&
        stats.frame_peak_history[0] == 30.0F, "frame graph lost the interval average or peak");
    require(!stats.push_frame_time(100.0F, 0.75) && stats.push_frame_time(10.0F, 1.0) && stats.frame_time_history[1] == 55.0F,
        "frame graph retained frames from a previous interval");
    for (int i = 1; i <= 130; ++i) stats.push_frame_time(static_cast<float>(i), 1.0 + i * 0.5);
    const auto last = (stats.frame_time_index + stats.history_size - 1U) % stats.history_size;
    require(stats.frame_time_count == 120 && stats.frame_time_history[last] == 130.0F, "frame graph did not retain its latest minute");
    Texture2D texture{1, 4, 4, 3, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    require(gpu_texture_bytes(texture) == 84U, "GPU texture estimate omitted mip levels");
    texture.id = 0;
    require(gpu_texture_bytes(texture) == 0U, "an unloaded texture counted as GPU memory");
    GpuMemoryStats memory; memory.buffer_bytes = 10; memory.texture_bytes = 20; memory.target_bytes = 30;
    require(memory.total_bytes() == 60, "GPU memory estimate did not include every allocation category");
}

void test_terrain_range_merging() {
    GpuMeshPart a; a.first_index = 0; a.index_count = 6; a.material_id = 2; a.texture = 9; a.material_texture = 10;
    auto b = a; b.first_index = 6;
    require(append_gpu_draw_range(a, b) && a.index_count == 12U, "adjacent terrain chunks still require separate draws");
    auto gap = b; gap.first_index = 18;
    require(!append_gpu_draw_range(a, gap) && a.index_count == 12U, "terrain range merging included a culled gap");
    auto other = b; other.first_index = 12; other.texture = 12;
    require(!append_gpu_draw_range(a, other), "terrain range merging changed a chunk's texture");
    other = b; other.first_index = 12; other.material_id = 3;
    require(!append_gpu_draw_range(a, other), "terrain range merging changed a chunk's material");
    other = b; other.first_index = 12; other.color.r = 0;
    require(!append_gpu_draw_range(a, other), "terrain range merging changed a chunk's tint");
}

void test_terrain_draw_culling() {
    BuildingRenderBuffer buffer;
    buffer.material_names = {"grass", "other"};
    for (const float x : {10.0F, 530.0F, -250.0F, 260.0F}) {
        const auto first = static_cast<std::uint32_t>(buffer.vertices.size());
        buffer.vertices.insert(buffer.vertices.end(), {{{x, 0, 0}}, {{x + 8, 0, 0}}, {{x, 0, 8}}});
        RenderBufferTriangle triangle; triangle.indices = {first, first + 1U, first + 2U};
        triangle.material = first == 9 ? 1U : 0U;
        buffer.triangles.push_back(triangle);
    }
    buffer.material_runs = {{0, 0, 3, false}, {1, 3, 1, false}};
    const auto original = buffer.triangles;
    build_terrain_draw_chunks(buffer);
    require(buffer.terrain_chunks.size() == 4 && buffer.triangles.size() == original.size(), "terrain chunking lost triangles or mixed distant regions");
    std::size_t visible = 0, covered = 0;
    for (const auto& chunk : buffer.terrain_chunks) {
        require(chunk.run.first_triangle == covered, "terrain draw ranges are not contiguous");
        for (std::size_t i = chunk.run.first_triangle; i < covered + chunk.run.triangle_count; ++i) {
            const auto& t = buffer.triangles[i];
            require(t.material == chunk.run.material, "terrain chunk crossed a material boundary");
            for (const auto index : t.indices) {
                const auto p = buffer.vertices[index].position;
                require(p.x >= chunk.bounds_min.x && p.x <= chunk.bounds_max.x && p.y >= chunk.bounds_min.y && p.y <= chunk.bounds_max.y &&
                    p.z >= chunk.bounds_min.z && p.z <= chunk.bounds_max.z, "terrain bounds omit part of a triangle");
            }
        }
        if (terrain_bounds_visible(chunk.bounds_min, chunk.bounds_max, {0, 0, 0}, 128.0F, {})) visible += chunk.run.triangle_count;
        covered += chunk.run.triangle_count;
    }
    require(covered == original.size() && visible == 1U, "distance culling still submits distant terrain triangles");
    ViewFrustumPlanes planes{}; planes[0] = {1, 0, 0, -100};
    require(!terrain_bounds_visible({0, 0, 0}, {8, 1, 8}, {}, 0, planes) &&
        terrain_bounds_visible({90, 0, 0}, {110, 1, 8}, {}, 0, planes), "frustum culling dropped a boundary chunk or retained an offscreen one");
    require(terrain_bounds_visible({-10, -10, -10}, {1000, 1000, 1000}, {}, 1.0F, {}),
        "distance culling used the chunk center and dropped geometry containing the camera");
    require(terrain_bounds_visible({530, 0, 0}, {538, 0, 8}, {}, 0, {}), "unlimited terrain distance still clips by distance");
    build_terrain_draw_chunks(buffer);
    require(buffer.terrain_chunks.size() == 4 && buffer.triangles.size() == original.size(), "rebuilding terrain chunks duplicated geometry");
}

void test_shared_building_sources() {
    EditorProject project;
    SbbBuilding building; building.name = "house"; building.size_z = 1; building.cells.resize(1);
    project.buildings.push_back({{}, "house", building});
    SbcMap map; map.version = 12;
    map.buildings = {{0, "house", 600, 1, 600, 0}, {1, "house", 620, 1, 600, 0}, {2, "missing", 640, 1, 600, 0}};
    project.city = LoadedAsset<SbcMap>{{}, "city2", map};
    const auto first = snapshot_city_buildings(project);
    require(first.owned->size() == 1U && first.resolved == std::vector<std::size_t>{1, 1, 0},
        "repeated placements copied building geometry or lost missing assets");
    project.city->data.buildings[0].x += 1;
    require(snapshot_city_buildings(project).owned == first.owned, "placement edits recopied immutable building cells");
    project.buildings[0].data.anchor_x = 3U;
    ++project.city_building_revision;
    const auto next = snapshot_city_buildings(project);
    require(next.owned != first.owned && next.owned->at(0).anchor_x == 3U && first.owned->at(0).anchor_x == 0U,
        "a building edit modified the snapshot owned by an earlier worker");
}

void test_collapsed_block_sections(const std::filesystem::path& root) {
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {800.0F, 600.0F}; io.DeltaTime = 1.0F / 60.0F;
    unsigned char* pixels = nullptr; int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    EditorProject project; project.root = root;
    EditorUi ui; ui.tab = EditorTab::Blocks; ui.block_textured = false;
    std::string status;
    create_new_block(project, ui, status);
    add_block_box(project, ui);
    add_block_patch(project, ui);
    constexpr const char* sections[] = {"View", "Box", "Surfaces", "Sides", "Bounds (cells)", "Corners (cells)", "Curve"};
    for (const auto kind : {BlockElementKind::Box, BlockElementKind::Patch}) {
        ui.block_element = kind; ui.block_element_index = 0;
        for (unsigned state = 0; state < (1U << std::size(sections)); ++state) {
            ImGui::NewFrame();
            ImGui::SetNextWindowSize({700, 900});
            ImGui::Begin("Properties###Properties", nullptr, ImGuiWindowFlags_NoSavedSettings);
            for (std::size_t i = 0; i < std::size(sections); ++i)
                ImGui::GetStateStorage()->SetInt(ImGui::GetID(sections[i]), (state & (1U << i)) != 0U);
            ImGui::End();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.8F, 0.8F, 0.8F, 1.0F});
            const auto colors = ImGui::GetCurrentContext()->ColorStack.Size;
            draw_properties_window(&project, ui, status);
            require(ImGui::GetCurrentContext()->ColorStack.Size == colors, "Block Properties consumed its caller's style color");
            ImGui::PopStyleColor();
            ImGui::Render(); // actual Properties, 256 mixed section states; no native window or GPU
        }
    }
    ui.tab = EditorTab::City; ui.city_tool = CityTool::Piece;
    project.city = LoadedAsset<SbcMap>{{}, "city2", SbcMap{}};
    ImGui::NewFrame();
    ImGui::Begin("Properties###Properties");
    ImGui::GetStateStorage()->SetInt(ImGui::GetID("Placing piece"), 0);
    ImGui::End();
    draw_properties_window(&project, ui, status);
    ImGui::Render();
}

void test_fly_delta_time() {
    const Camera3D initial{{0, 0, 0}, {0, 0, -1}, {0, 1, 0}, 45, CAMERA_PERSPECTIVE};
    auto slow = initial, fast = initial;
    for (int i = 0; i < 30; ++i) update_free_fly_camera(slow, 120.0F, {.forward = true}, 1.0F / 30.0F);
    for (int i = 0; i < 120; ++i) update_free_fly_camera(fast, 120.0F, {.forward = true}, 1.0F / 120.0F);
    require(std::abs(slow.position.z + 120.0F) < 0.01F && std::abs(slow.position.z - fast.position.z) < 0.01F,
        "fly distance depends on update/frame count rather than elapsed seconds");
}

void test_prepared_road() {
    EditorProject project;
    SbcMap map; map.version = 12;
    SbcPortal piece; piece.fields = {0, 75, 0, 75}; piece.blocks.assign(512U, 0x40000000U);
    map.portals.push_back(piece);
    project.city = LoadedAsset<SbcMap>{{}, "city2", map};
    EditorUi ui; ui.pending_road_start = CityGridPoint{600, 1, 600}; ui.new_road_width = 6;
    const CityGridPoint end{618, 5, 600};
    const auto expected = prepare_city_road_plan(project, ui, end);
    require(expected.portals.empty(), "preparing a road copied unrelated map voxel payloads");
    project.road_edit_preview.after_hash = road_edit_hash(expected);
    project.road_edit_preview.display_tiles.push_back(CityTileCell{});
    require(commit_prepared_city_road(project, ui, end), "prepared road was refused");
    require(project.city->data.streets == expected.streets && project.city->data.intersections == expected.intersections &&
        project.city->data.portals == map.portals && ui.city_dirty, "committing a road differs from its preview or changed unrelated pieces");
    require(project.pending_roads.tiles.size() == 1U && project.pending_roads.fingerprint == road_edit_hash(project.city->data) &&
        project.road_edit_preview.display_tiles.empty(), "unrelated piece data prevented transfer of the prepared road display");
    const auto before = project.city->data.streets;
    prepare_city_road_plan(project, ui, {638, 5, 600});
    project.city->data.intersections.back().y += 1;
    require(!commit_prepared_city_road(project, ui, {638, 5, 600}) && project.city->data.streets == before,
        "a stale road plan overwrote a later edit");
}

void benchmark_road(const std::filesystem::path& path) {
    const auto original = parse_sbc(read_file_bytes(path));
    std::vector<double> preparation, diff, mesh, commit, snapshot, history;
    std::size_t tile_count = 0;
    std::string status;
    auto loaded = load_project_folder(path.parent_path(), status, false);
    require(loaded.has_value(), "could not load benchmark sources: " + status);
    auto& project = *loaded;
    const auto buildings = resolve_placement_buildings(project);
    const auto elapsed = [](auto start) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); };
    for (int i = 0; i < 100; ++i) {
        project.city->data = original;
        project.city_baseline = original;
        project.undo_stack.clear();
        EditorUi ui; ui.pending_road_start = CityGridPoint{900, 1, 900}; ui.new_road_width = 6;
        const CityGridPoint end{916, 5, 900};
        auto start = std::chrono::steady_clock::now();
        const auto& planned = prepare_city_road_plan(project, ui, end);
        preparation.push_back(elapsed(start));
        if (i == 0) {
            const auto ramp = preview_city_street(planned, *project.road_placement_plan.last_street, false);
            require(ramp.run.length == 16 && ramp.run.climb == 4 && ramp.run.cells_per_level == 4, "benchmark does not match the reported ramp grade");
        }
        start = std::chrono::steady_clock::now();
        const auto delta = diff_city_road_tiles(project.city->data, planned, buildings, false);
        diff.push_back(elapsed(start)); tile_count = delta.shown.size();
        start = std::chrono::steady_clock::now();
        const auto buffer = build_city_road_tile_buffer(delta.shown, 4.0F);
        mesh.push_back(elapsed(start));
        start = std::chrono::steady_clock::now();
        require(commit_prepared_city_road(project, ui, end), "benchmark road commit failed");
        commit.push_back(elapsed(start));
        start = std::chrono::steady_clock::now();
        const auto sources = snapshot_city_buildings(project);
        snapshot.push_back(elapsed(start));
        start = std::chrono::steady_clock::now();
        commit_pending_edits(project, ui);
        history.push_back(elapsed(start));
    }
    const auto report = [](const char* name, std::vector<double> times) {
        std::sort(times.begin(), times.end());
        std::cout << name << " median=" << times[times.size()/2U] << " ms p95=" << times[times.size()*95U/100U] << " ms max=" << times.back() << " ms\n";
    };
    std::cout << "Road-only CPU benchmark: " << original.streets.size() << " existing roads, " << original.portals.size() << " portals, " << tile_count << " local preview tiles; 100 samples.\n";
    report("prepare", preparation); report("tile diff", diff); report("mesh CPU", mesh); report("commit", commit);
    report("worker SBB snapshot", snapshot); report("history", history);
}

void benchmark_terrain(const std::filesystem::path& path) {
    std::string status;
    auto project = load_project_folder(path.parent_path(), status, false);
    require(project.has_value(), "could not load terrain benchmark: " + status);
    const auto start = std::chrono::steady_clock::now();
    const auto scene = build_map_scene(project->city->data, resolve_placement_buildings(*project), project_is_auto_generated(*project));
    require(scene.terrain.has_value(), "benchmark map has no terrain");
    const auto& mesh = scene.terrain->mesh;
    std::cout << "Terrain: " << gfx_debug_format_count(mesh.triangles.size()) << " triangles, " << mesh.terrain_chunks.size() << " draw chunks. Scene built in "
        << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() << " ms.\n";
    const Vec3 camera{1900, 72, 1100};
    for (const float distance : {0.0F, 128.0F, 512.0F, 1024.0F}) {
        std::uint64_t triangles = 0, chunks = 0, draws = 0;
        std::optional<GpuMeshPart> range;
        const auto cull_start = std::chrono::steady_clock::now();
        for (const auto& chunk : mesh.terrain_chunks) {
            if (!terrain_bounds_visible(chunk.bounds_min, chunk.bounds_max, camera, distance, {})) { range.reset(); continue; }
            triangles += chunk.run.triangle_count; ++chunks;
            GpuMeshPart part; part.first_index = chunk.run.first_triangle * 3U; part.index_count = chunk.run.triangle_count * 3U;
            part.material_id = chunk.run.material;
            if (!range || !append_gpu_draw_range(*range, part)) { range = part; ++draws; }
        }
        std::cout << "distance=" << distance << " (0=unlimited), distance-only visible triangles=" << gfx_debug_format_count(triangles)
            << ", chunks=" << chunks << ", merged draws=" << draws << ", cull CPU=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cull_start).count() << " ms\n";
    }
}

void test_city_scene_adoption() {
    EditorProject project;
    const auto scene = [](float height) {
        MapScene built;
        built.terrain.emplace();
        built.terrain->heights.push_back(height);
        return built;
    };
    project.city_scene_current_fingerprint = 1U;
    require(adopt_city_scene_result(project, scene(10.0F), 1U, true), "initial city scene was not accepted");
    project.city_scene_current_fingerprint = 2U;
    require(adopt_city_scene_result(project, MapScene{}, 2U, false), "edited road scene was not accepted");
    require(!project.city_scene_cache_complete && project.city_scene_terrain_fingerprint == 1U &&
        project.city_scene_cache.terrain->heights[0] == 10.0F, "fast roads lost the previous terrain or skipped full rebuilding");
    require(!adopt_city_scene_result(project, scene(99.0F), 1U, true) && project.city_scene_cache_fingerprint == 2U,
        "a stale full scene replaced a newer edit");

    // Undo can return to a terrain revision that is already uploaded. Its fast road
    // geometry still needs a complete result to restore the road shadow buffers.
    project.city_scene_current_fingerprint = 1U;
    require(adopt_city_scene_result(project, MapScene{}, 1U, false) && !project.city_scene_cache_complete,
        "rapid undo incorrectly marked a road-only scene complete");
    require(!adopt_city_scene_result(project, scene(99.0F), 2U, true), "the undone edit was accepted after undo");
    require(adopt_city_scene_result(project, scene(10.0F), 1U, true) && project.city_scene_cache_complete,
        "the complete undo result was not accepted");
    require(!adopt_city_scene_result(project, MapScene{}, 1U, false) && project.city_scene_cache_complete &&
        project.city_scene_cache.terrain->heights[0] == 10.0F, "a late fast scene downgraded the complete result");
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && (std::string{argv[1]} == "--road-bench" || std::string{argv[1]} == "--terrain-bench")) {
        ImGui::CreateContext(); // history reads ImGui's active-item state, even headlessly
        try { if (std::string{argv[1]} == "--road-bench") benchmark_road(argv[2]); else benchmark_terrain(argv[2]); ImGui::DestroyContext(); return 0; }
        catch (const std::exception& error) { ImGui::DestroyContext(); std::cerr << error.what() << '\n'; return 1; }
    }
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
        test_block_face_paint(root / "paint");
        test_block_switch(root / "switch");
        test_prepared_road();
        test_fly_delta_time();
        test_shared_building_sources();
        test_graphics_debug_sampling();
        test_terrain_draw_culling();
        test_terrain_range_merging();
        test_collapsed_block_sections(root / "ui");
        test_city_scene_adoption();
        ImGui::DestroyContext();
        std::cout << "Save, reopen, failed save, undo, redo, and incomplete-source checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        ImGui::DestroyContext();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
