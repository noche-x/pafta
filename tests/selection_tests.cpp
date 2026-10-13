// SPDX-License-Identifier: GPL-2.0-only
#include "app_internal.hpp"
#include <imgui.h>

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace pafta;
using namespace pafta::app_internal;

constexpr Vector2 viewport{800.0F, 800.0F};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Camera3D top_camera(float x = 0.0F, float z = 0.0F) {
    return {{x, 100.0F, z}, {x, 0.0F, z}, {0.0F, 0.0F, -1.0F}, 96.0F, CAMERA_ORTHOGRAPHIC};
}

Vector2 screen(const Camera3D& camera, Vector3 point) {
    return GetWorldToScreenEx(point, camera, static_cast<int>(viewport.x), static_cast<int>(viewport.y));
}

EditorProject mixed_project() {
    EditorProject project;
    SbcMap map; map.version = 12;
    map.buildings = {{0, "inside", 0, 0, 0, 0}, {1, "outside", 24, 0, 24, 0}};
    map.intersections = {{0, -8, 0, 0}, {1, 8, 0, 0}};
    map.streets = {{0, 0, 1, 0, 1, 1, "crossing"}};
    SbcPortal portal;
    portal.blocks.assign(512U, 0U);
    portal.blocks[0] = 0x00010000U;
    portal.blocks[7U + 7U * 8U] = 0x00010000U;
    map.portals.push_back(std::move(portal));
    project.city = LoadedAsset<SbcMap>{{}, "city", std::move(map)};
    return project;
}

void select_middle(EditorProject& project, EditorUi& ui, bool additive = false) {
    const auto camera = top_camera();
    box_select_city(project, ui, camera, screen(camera, {-4, 0, -4}), screen(camera, {8, 0, 8}), viewport, additive);
}

void test_mixed_box_and_delete() {
    auto project = mixed_project();
    EditorUi ui;
    select_middle(project, ui);
    require(ui.selected_city_buildings == std::vector<std::size_t>{0U}, "box missed the building or selected one outside it");
    require(ui.selected_city_streets == std::vector<std::size_t>{0U},
        "box missed a road whose segment crosses it while both endpoints lie outside");
    require(ui.selected_piece_cells == std::vector<CityGridPoint>{{0, 0, 0}},
        "box selected the whole portal instead of only its voxel inside the rectangle");
    require(ui.selected_portals == std::vector<std::size_t>{0U}, "box did not retain the selected voxel's portal");

    std::string status;
    commit_pending_edits(project, ui);
    delete_box_selection(project, ui);
    const auto& map = project.city->data;
    require(map.buildings.size() == 1U && map.buildings[0].name == "outside", "mixed delete removed the wrong building");
    require(map.streets.empty(), "mixed delete kept the last road");
    require(map.portals.size() == 1U && map.portals[0].blocks[0] == 0U && map.portals[0].blocks[63U] != 0U,
        "mixed delete removed an unselected voxel in the same portal");
    require(ui.selected_city_buildings.empty() && ui.selected_city_streets.empty() && ui.selected_piece_cells.empty(),
        "mixed delete retained stale selected indices");
    undo_city(project, ui, status);
    require(project.city->data.buildings.size() == 2U && project.city->data.streets.size() == 1U &&
        project.city->data.portals[0].blocks[0] != 0U, "undo did not restore the mixed deletion");
}

void test_filters_and_additive() {
    auto project = mixed_project();
    EditorUi ui;
    ui.box_select_buildings = false;
    ui.box_select_pieces = false;
    select_middle(project, ui);
    require(ui.selected_city_buildings.empty() && ui.selected_piece_cells.empty() &&
        ui.selected_city_streets == std::vector<std::size_t>{0U}, "road-only box ignored the object filters");
    ui.box_select_roads = false;
    ui.box_select_pieces = true;
    select_middle(project, ui, true);
    require(ui.selected_city_streets == std::vector<std::size_t>{0U} && ui.selected_piece_cells.size() == 1U,
        "additive box discarded the existing road selection");
    select_middle(project, ui, true);
    require(ui.selected_piece_cells.size() == 1U && ui.selected_portals.size() == 1U,
        "additive box duplicated a selected voxel or portal");
    ui.box_select_buildings = true;
    ui.box_select_pieces = false;
    select_middle(project, ui);
    require(ui.selected_city_buildings == std::vector<std::size_t>{0U} && ui.selected_city_streets.empty() &&
        ui.selected_piece_cells.empty(), "replacement box kept disabled object selections");
    const auto camera = top_camera();
    box_select_city(project, ui, camera, {0, 0}, {2, 2}, viewport, false);
    require(ui.city_selection == CitySelectionKind::None && ui.selected_city_buildings.empty(),
        "empty replacement box retained the old selection");
}

void test_legacy_voxel_selection() {
    EditorProject project;
    SbcMap map; map.version = 10;
    SbcPortal portal;
    portal.fields = {0, -1, 0, 1};
    portal.legacy_blocks.assign(512U, 0U);
    portal.legacy_blocks[0] = 0x40000000U;
    portal.legacy_blocks[7] = 0x80000000U;
    map.portals.push_back(std::move(portal));
    project.city = LoadedAsset<SbcMap>{{}, "legacy", std::move(map)};
    EditorUi ui;
    ui.box_select_buildings = ui.box_select_roads = false;
    const CityGridPoint cell{sbc_portal_cell_base(-1), 0, sbc_portal_cell_base(1)};
    const Vector3 center{(static_cast<float>(cell.x) + 0.5F) * 4.0F, 2.0F,
        (static_cast<float>(cell.z) + 0.5F) * 4.0F};
    const auto camera = top_camera(center.x, center.z);
    const auto p = screen(camera, center);
    box_select_city(project, ui, camera, {p.x - 10, p.y - 10}, {p.x + 10, p.y + 10}, viewport, false);
    require(ui.selected_piece_cells == std::vector<CityGridPoint>{cell},
        "legacy voxel in a negative sector was absent or projected at the wrong location");
    delete_box_selection(project, ui);
    require(project.city->data.portals.size() == 1U && project.city->data.portals[0].legacy_blocks[0] == 0U &&
        project.city->data.portals[0].legacy_blocks[7] != 0U, "legacy delete changed an unselected voxel");
}

void test_mixed_move_and_collision() {
    auto project = mixed_project();
    EditorUi ui;
    select_middle(project, ui);
    std::string status;
    nudge_box_selection(project, ui, 1, 2, 3, status);
    const auto& map = project.city->data;
    require(map.buildings[0].x == 1 && map.buildings[0].y == 2 && map.buildings[0].z == 3 && map.buildings[1].x == 24,
        "mixed move changed the wrong building or omitted an axis");
    require(map.intersections[0].x == -7 && map.intersections[1].x == 9 && map.intersections[0].y == 2,
        "mixed move omitted a road endpoint");
    require(map.portals[0].blocks[0] == 0U && map.portals[0].blocks[1U + 2U * 64U + 3U * 8U] == 0x10000U &&
        map.portals[0].blocks[63U] == 0x10000U, "mixed move lost the selected piece or moved an unselected piece");
    require(ui.selected_piece_cells == std::vector<CityGridPoint>{{1, 2, 3}}, "piece selection did not follow the moved voxel");

    auto blocked = mixed_project();
    blocked.city->data.portals[0].blocks[1] = 0x00010000U;
    const auto before = blocked.city->data;
    EditorUi blocked_ui;
    blocked_ui.selected_city_buildings = {0U};
    blocked_ui.selected_city_streets = {0U};
    blocked_ui.selected_piece_cells = {{0, 0, 0}};
    nudge_box_selection(blocked, blocked_ui, 1, 0, 0, status);
    require(blocked.city->data == before && !blocked_ui.city_dirty,
        "colliding piece move partially moved the buildings or roads");
    require(status.find("already has a piece") != std::string::npos, "rejected move omitted the collision diagnostic");

    auto adjacent = mixed_project();
    adjacent.city->data.portals[0].blocks[1] = 0x01010000U;
    EditorUi adjacent_ui; adjacent_ui.selected_piece_cells = {{0, 0, 0}, {1, 0, 0}};
    nudge_box_selection(adjacent, adjacent_ui, 1, 0, 0, status);
    require(adjacent.city->data.portals[0].blocks[0] == 0U && adjacent.city->data.portals[0].blocks[1] == 0x00010000U &&
        adjacent.city->data.portals[0].blocks[2] == 0x01010000U,
        "adjacent selected voxels blocked each other or lost their rotation words");
}

void test_shared_road_node_and_overflow() {
    auto project = mixed_project();
    project.city->data.intersections.push_back({2, 0, 0, 8});
    project.city->data.streets.push_back({1, 0, 2, 0, 1, 1, "second"});
    EditorUi ui; ui.selected_city_streets = {0U, 1U};
    std::string status;
    nudge_box_selection(project, ui, 1, 0, 0, status);
    require(project.city->data.intersections[0].x == -7 && project.city->data.intersections[1].x == 9 &&
        project.city->data.intersections[2].x == 1, "shared road endpoint moved more than once");

    project.city->data.buildings[0].x = std::numeric_limits<std::int32_t>::max();
    ui.selected_city_buildings = {0U};
    const auto before = project.city->data;
    nudge_box_selection(project, ui, 1, 0, 0, status);
    require(project.city->data == before, "overflow rejection partially moved the road group");
    require(status.find("out of range") != std::string::npos, "overflow rejection omitted its diagnostic");
}

void test_legacy_move_preserves_payload() {
    EditorProject project;
    SbcMap map; map.version = 10;
    SbcPortal portal;
    portal.fields = {0, -1, 0, 0};
    portal.legacy_blocks.assign(512U, 0U);
    portal.legacy_blocks[7] = 0x81000014U;
    portal.textures.assign(4096U, 0U);
    portal.itemsets.assign(512U, 0U);
    portal.itemsets[7] = 0x40000002U;
    for (std::size_t slot = 0; slot < 8U; ++slot) portal.textures[56U + slot] = static_cast<std::uint16_t>(100U + slot);
    map.portals.push_back(std::move(portal));
    project.city = LoadedAsset<SbcMap>{{}, "legacy", std::move(map)};
    EditorUi ui; ui.selected_piece_cells = {{-1, 0, 0}};
    std::string status;
    nudge_box_selection(project, ui, 1, 0, 0, status);
    const auto& portals = project.city->data.portals;
    require(portals.size() == 2U && portals[0].legacy_blocks[7] == 0U && portals[0].itemsets[7] == 0U,
        "legacy move did not clear its source voxel and item set");
    require(portals[1].blocks.empty() && portals[1].legacy_blocks[0] == 0x81000014U && portals[1].itemsets[0] == 0x40000002U,
        "legacy sector crossing lost the word, rotation, item set, or voxel representation");
    for (std::size_t slot = 0; slot < 8U; ++slot) {
        require(portals[1].textures[slot] == 100U + slot && portals[0].textures[56U + slot] == 0U,
            "legacy sector crossing lost a face texture or kept it at the source");
    }
}

void test_additive_active_object() {
    auto project = mixed_project();
    project.city->data.streets.push_back({1, 0, 1, 0, 1, 1, "second"});
    EditorUi ui;
    std::string status;
    select_city_thing(project, ui, {CitySelectionKind::Building, 0U}, false, {}, {}, status);
    select_city_thing(project, ui, {CitySelectionKind::Building, 1U}, true, {}, {}, status);
    select_city_thing(project, ui, {CitySelectionKind::Building, 1U}, true, {}, {}, status);
    require(ui.selected_city_buildings == std::vector<std::size_t>{0U} && ui.selected_city_building == 0U,
        "deselecting the active building left the gizmo on an unselected building");
    select_city_thing(project, ui, {CitySelectionKind::Road, 0U}, false, {}, {}, status);
    select_city_thing(project, ui, {CitySelectionKind::Road, 1U}, true, {}, {}, status);
    select_city_thing(project, ui, {CitySelectionKind::Road, 1U}, true, {}, {}, status);
    require(ui.selected_city_streets == std::vector<std::size_t>{0U} && ui.selected_city_street == 0U,
        "deselecting the active road left property actions on an unselected road");
}

void test_erase_hover_preserves_previous_group() {
    auto project = mixed_project();
    auto& map = project.city->data;
    map.intersections.push_back({2, -8, 0, 4});
    map.intersections.push_back({3, 8, 0, 4});
    map.streets.push_back({1, 2, 3, 0, 1, 1, "hovered"});
    MapScene scene;
    RenderRoad road{}; road.source = map.streets[1]; road.source_index = 1;
    scene.roads.push_back(road);
    CityTileCell tile{}; tile.z = 4; tile.kind = CityTileKind::Road; tile.street_index = 1;
    scene.city_tiles.push_back(tile);
    EditorUi ui;
    ui.city_selection = CitySelectionKind::Building;
    ui.selected_city_buildings = {0U};
    ui.selected_city_streets = {0U};
    ui.selected_piece_cells = {{0, 0, 0}};
    ui.selected_portals = {0U};
    std::string status;
    erase_city_thing_under_cursor(project, ui, scene, {2, 100, 18}, {0, -1, 0}, status);
    require(map.streets.size() == 1U && map.streets[0].name == "crossing",
        "Erase removed a selected road instead of the hovered road");
    require(map.buildings.size() == 2U && map.portals[0].blocks[0] != 0U,
        "Erase deleted buildings or pieces from the previous mixed selection");
    require(ui.selected_city_buildings.empty() && ui.selected_city_streets.empty() && ui.selected_piece_cells.empty() &&
        ui.selected_portals.empty(), "Erase retained stale mixed selection indices");
}

} // namespace

// Moving a junction keeps roads straight: roads across the move carry their far
// junction along, roads along it stretch, and a move that collapses a road is refused.
void test_move_junction() {
    SbcMap map; map.version = 12;
    map.intersections = {{0, 0, 0, 0}, {1, 20, 0, 0}, {2, 20, 0, 20}};
    map.streets = {{0, 0, 1, 0, 2, 2, "east"}, {1, 1, 2, 0, 2, 2, "south"}};
    require(move_city_junction(map, 1, 4, 0), "moving a junction along X was refused");
    require(map.intersections[1].x == 24 && map.intersections[2].x == 24 && map.intersections[0].x == 0,
        "a road across the move did not carry its far junction along");
    require(move_city_junction(map, 2, 0, 4) && map.intersections[2].z == 24 && map.intersections[1].z == 0,
        "a road along the move did not stretch");
    const auto before = map.intersections;
    require(!move_city_junction(map, 1, -23, 0) && map.intersections == before, "a move that collapses a road was applied");
    require(!move_city_junction(map, 1, -30, 0) && map.intersections == before, "a move that turns a road around was applied");
}

// The road edit preview finds the tiles a new road adds, and the ones it lays over a
// road that stays as it is.
void test_road_edit_overlaps() {
    SbcMap before; before.version = 12;
    before.intersections = {{0, 0, 0, 0}, {1, 40, 0, 0}};
    before.streets = {{0, 0, 1, 0, 2, 2, "old"}};
    auto apart = before;
    apart.intersections.push_back({2, 0, 0, 40});
    apart.intersections.push_back({3, 40, 0, 40});
    apart.streets.push_back({1, 2, 3, 0, 2, 2, "new"});
    const auto clear = diff_city_road_tiles(before, apart, {}, false);
    require(!clear.changed.empty() && clear.overlaps.empty(), "a road well apart from the others was reported as overlapping");
    auto beside = before;
    beside.intersections.push_back({2, 0, 0, 2});
    beside.intersections.push_back({3, 40, 0, 2});
    beside.streets.push_back({1, 2, 3, 0, 2, 2, "new"});
    require(!diff_city_road_tiles(before, beside, {}, false).overlaps.empty(), "a road laid over another road was not reported");
    for (const auto end : {std::size_t{0}, std::size_t{1}}) {
        auto joined = before;
        const auto& node = before.intersections[end];
        joined.intersections.push_back({2, node.x, 0, 40});
        joined.streets.push_back({1, static_cast<std::int32_t>(end), 2, 0, 2, 2, "new"});
        const auto t = diff_city_road_tiles(before, joined, {}, false);
        require(t.overlaps.empty(), "a road joining an existing junction was reported as overlapping (" + std::to_string(t.overlaps.size()) + " cells)");
    }
}

void test_ramp_end_curbs() {
    // Exact reported grade: 16 cells, four levels, four cells per level.
    for (const bool along_x : {true, false}) for (const bool downhill : {true, false}) {
        SbcMap map; map.version = 12;
        map.intersections = {{0, 600, downhill ? 5 : 1, 600},
            {1, along_x ? 616 : 600, downhill ? 1 : 5, along_x ? 600 : 616}};
        map.streets = {{0, 0, 1, 0, 3, 3, "ramp"}};
        const auto preview = preview_city_street(map, 0U, false);
        require(preview.run.length == 16 && std::abs(preview.run.climb) == 4 && preview.run.cells_per_level == 4 && preview.run.slope_ok(),
            "reported ramp no longer has the expected valid grade");
        for (const int along : {600, 615}) {
            const auto tile = std::ranges::find_if(preview.cells, [&](const CityTileCell& t) {
                return t.street_index == 0 && t.kind == CityTileKind::Curb &&
                    (along_x ? t.x == along : t.z == along);
            });
            require(tile != preview.cells.end() && std::abs(along_x ? tile->slope_x : tile->slope_z) > 0.99F,
                "first/last curb cell of a full-length ramp became a flat floating cap");
        }
    }
}

void test_preview_matches_full_roads() {
    SbcMap before; before.version = 12;
    before.intersections = {{0, 600, 1, 600}, {1, 640, 1, 600}, {2, 600, 1, 650}, {3, 640, 1, 650}};
    before.streets = {{0, 0, 1, 0, 2, 2, "near"}, {1, 2, 3, 0, 2, 2, "far"}};
    auto after = before;
    after.intersections.push_back({4, 600, 1, 625});
    after.streets.push_back({2, 0, 4, 0, 2, 2, "new"});
    const auto delta = diff_city_road_tiles(before, after, {}, false);
    const auto a = build_map_scene(before, {}, false, false, MapSceneBuild::RoadsOnly);
    const auto b = build_map_scene(after, {}, false, false, MapSceneBuild::RoadsOnly);
    const auto same = [](const CityTileCell& x, const CityTileCell& y) { return x.x == y.x && x.y == y.y && x.z == y.z &&
        x.kind == y.kind && x.block == y.block && x.faces == y.faces && x.base_y == y.base_y && x.slope_x == y.slope_x && x.slope_z == y.slope_z; };
    for (const auto& t : b.city_tiles) {
        if (std::ranges::any_of(a.city_tiles, [&](const auto& old) {return same(t, old);})) continue;
        require(std::ranges::any_of(delta.changed, [&](const auto& changed) {return same(t, changed);}),
            "local preview omitted a changed road/junction tile");
    }
    for (const auto& t : delta.changed) require(std::ranges::any_of(b.city_tiles, [&](const auto& full) {return same(t, full);}),
        "local preview added a tile that the full road scene does not contain");
}

int main() {
    ImGui::CreateContext();
    try {
        test_mixed_box_and_delete();
        test_filters_and_additive();
        test_legacy_voxel_selection();
        test_mixed_move_and_collision();
        test_shared_road_node_and_overflow();
        test_legacy_move_preserves_payload();
        test_additive_active_object();
        test_erase_hover_preserves_previous_group();
        test_move_junction();
        test_road_edit_overlaps();
        test_ramp_end_curbs();
        test_preview_matches_full_roads();
        ImGui::DestroyContext();
        std::cout << "Mixed, filtered, additive, road-crossing, and legacy selection checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        ImGui::DestroyContext();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
