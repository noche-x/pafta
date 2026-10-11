#include "mesh_export.hpp"
#include "raylib.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace map_editor::app_internal {
// The CPU export test has no loaded GPU textures. The application supplies this
// lookup in production; this link stub keeps the export writer test headless.
const Texture2D* find_project_texture(const EditorProject&, std::string_view) { return nullptr; }
}

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::uint8_t> bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.good(), "cannot read " + path.string());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void word(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) output.push_back(static_cast<std::uint8_t>(value >> shift));
}

void real(std::vector<std::uint8_t>& output, float value) {
    word(output, std::bit_cast<std::uint32_t>(value));
}

void test_triangle_export(const std::filesystem::path& directory) {
    map_editor::BuildingRenderBuffer buffer;
    buffer.vertices = {
        {{0, 0, 0}, 0, 0},
        {{1, 0, 0}, 1, 0},
        {{0, 1, 0}, 0, 1},
    };
    buffer.triangles.push_back({{0, 1, 2}, 0});
    map_editor::ExportMeshGroup group{};
    group.name = "triangle";
    group.buffer = &buffer;
    const auto path = directory / "triangle.gltf";
    map_editor::ExportResult result;
    auto job = map_editor::build_gltf_export(path, {group}, {}, result);
    require(static_cast<bool>(job), "triangle export did not build: " + result.message);
    require(map_editor::gltf_export_texture_count(*job) == 0, "untextured triangle requested a texture");
    result = map_editor::write_gltf_export(*job);
    require(result.ok && result.stats.triangle_count == 1 && result.stats.vertex_count == 3,
        "triangle export stats are wrong: " + result.message);

    std::vector<std::uint8_t> expected;
    for (const float value : {0.F, 0.F, 0.F, 1.F, 0.F, 0.F, 0.F, 1.F, 0.F}) real(expected, value);
    for (int i = 0; i < 3; ++i) for (const float value : {0.F, 0.F, 1.F}) real(expected, value);
    for (const float value : {0.F, 0.F, 1.F, 0.F, 0.F, 1.F}) real(expected, value);
    for (const auto index : {0U, 1U, 2U}) word(expected, index);
    const auto bin = bytes(directory / "triangle.bin");
    require(bin == expected, "glTF binary differs from frozen triangle layout");
    const auto json = bytes(path);
    const std::string json_text(json.begin(), json.end());
    require(json_text.find("triangle.bin") != std::string::npos, "glTF does not reference its binary");

    require(map_editor::write_gltf_export(*job).ok, "second glTF write failed");
    require(bytes(directory / "triangle.bin") == bin, "second glTF binary write changed bytes");
    require(bytes(path) == json, "second glTF JSON write changed bytes");

    group.transformed = true;
    group.origin = {10, 20, 30};
    group.world_offset = {5, 5, 5};
    group.basis.x_axis = {0, 0, 1};
    group.basis.z_axis = {-1, 0, 0};
    auto transformed = map_editor::build_gltf_export(directory / "transformed.gltf", {group}, {}, result);
    require(static_cast<bool>(transformed) && map_editor::write_gltf_export(*transformed).ok,
        "transformed triangle export failed");
    std::vector<std::uint8_t> expected_positions;
    for (const float value : {5.F, 15.F, 25.F, 5.F, 15.F, 26.F, 5.F, 16.F, 25.F})
        real(expected_positions, value);
    const auto transformed_bin = bytes(directory / "transformed.bin");
    require(transformed_bin.size() >= expected_positions.size() &&
            std::equal(expected_positions.begin(), expected_positions.end(), transformed_bin.begin()),
        "glTF world transform or recentering changed binary positions");

    buffer.triangles[0].city_window_surface = true;
    map_editor::ExportOptions no_panes{};
    no_panes.include_window_panes = false;
    map_editor::ExportResult empty_result;
    require(!map_editor::build_gltf_export(directory / "empty.gltf", {group}, no_panes, empty_result),
        "disabled window pane was exported");
}

void test_export_group_selection() {
    map_editor::SblBlockMesh block{};
    block.faces.push_back({5,
        {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}},
        {{0, 0}, {1, 0}, {1, 1}, {0, 1}}, true, 0});
    const auto block_buffer = map_editor::build_block_mesh_render_buffer(block);
    require(block_buffer.vertices.size() == 4 && block_buffer.triangles.size() == 2,
        "block export did not triangulate its quad");
    require(block_buffer.vertices[1].position.x == 4.F && block_buffer.vertices[2].v == 1.F,
        "block export lost scale or UVs");
    require(block_buffer.material_names == std::vector<std::string>{"material_5"},
        "block export lost its material slot");

    map_editor::MapScene scene{};
    scene.bounds = {0, 10, 0, 20};
    scene.terrain.emplace();
    map_editor::SbcBuilding placement{0, "house", 0, 0, 0, 0};
    scene.buildings.push_back({&placement, {}, 0});
    map_editor::SbbBuilding building{};
    building.materials = {"brick"};
    const auto groups = map_editor::city_export_groups(scene, {&block_buffer}, {&building}, block_buffer);
    require(groups.size() == 4, "city export omitted a selected geometry group");
    require(groups[0].name == "terrain" && groups[1].name == "roads" &&
            groups[2].name == "pieces" && groups[3].name == "building_0",
        "city export group order changed");
    require(groups[2].world_offset.x == 5.F && groups[2].world_offset.y == -0.25F &&
            groups[2].world_offset.z == 10.F, "city piece recentering changed");
    require(groups[3].transformed && groups[3].building_materials == &building.materials,
        "city building transform or material table was lost");

    map_editor::ExportOptions only_pieces{};
    only_pieces.include_buildings = false;
    only_pieces.include_terrain = false;
    only_pieces.include_roads = false;
    const auto filtered = map_editor::city_export_groups(scene, {&block_buffer}, {&building}, block_buffer, only_pieces);
    require(filtered.size() == 1 && filtered[0].name == "pieces", "city export options did not filter groups");

    map_editor::SbcMap map{};
    map.buildings.push_back(placement);
    require(map_editor::estimate_city_export_triangles(map, {}, {}, {}) == 0,
        "unresolved city building contributed export triangles");
    building.size_z = 1;
    map_editor::SbbCell cell{};
    cell.exterior_block() = 0x40000000U;
    cell.occupied = true;
    building.cells.push_back(cell);
    const auto expected_triangles = map_editor::build_native_building_render_buffer(building, {}, {}).triangles.size();
    require(expected_triangles > 0 &&
            map_editor::estimate_city_export_triangles(map, {&building}, {}, {}) == expected_triangles,
        "city export triangle estimate differs from its building mesh");
}

} // namespace

int main() {
    try {
        const auto directory = std::filesystem::path{MAP_EDITOR_CPP_TEST_OUTPUT_ROOT};
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        test_triangle_export(directory);
        test_export_group_selection();
        std::cout << "glTF binary and repeated exports match\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
