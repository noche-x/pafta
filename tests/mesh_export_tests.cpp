// SPDX-License-Identifier: GPL-2.0-only
#include "mesh_export.hpp"
#include "raylib.h"
#include "json.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pafta::app_internal {
// The CPU export test has no GPU textures loaded. The real app provides this lookup;
// this stub lets the export writer test run without a window.
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
    pafta::BuildingRenderBuffer buffer;
    buffer.vertices = {
        {{0, 0, 0}, 0, 0},
        {{1, 0, 0}, 1, 0},
        {{0, 1, 0}, 0, 1},
    };
    buffer.triangles.push_back({{0, 1, 2}, 0});
    pafta::ExportMeshGroup group{};
    group.name = "triangle";
    group.buffer = &buffer;
    const auto path = directory / "triangle.gltf";
    pafta::ExportResult result;
    auto job = pafta::build_gltf_export(path, {group}, {}, result);
    require(static_cast<bool>(job), "triangle export did not build: " + result.message);
    require(pafta::gltf_export_texture_count(*job) == 0, "untextured triangle requested a texture");
    result = pafta::write_gltf_export(*job);
    require(result.ok && result.stats.triangle_count == 1 && result.stats.vertex_count == 3,
        "triangle export stats are wrong: " + result.message);

    std::vector<std::uint8_t> expected;
    for (const float value : {0.F, 0.F, 0.F, 1.F, 0.F, 0.F, 0.F, 1.F, 0.F}) real(expected, value);
    for (int i = 0; i < 3; ++i) for (const float value : {0.F, 0.F, 1.F}) real(expected, value);
    for (const float value : {0.F, 0.F, 1.F, 0.F, 0.F, 1.F}) real(expected, value);
    for (const auto index : {0U, 1U, 2U}) word(expected, index);
    const auto bin = bytes(directory / "triangle.bin");
    require(bin == expected, "glTF binary differs from frozen triangle layout");
    buffer.material_names = {"city_terrain_grass"};
    auto grass = pafta::build_gltf_export(directory / "grass.gltf", {group}, {}, result);
    require(grass && pafta::write_gltf_export(*grass).ok, "grass export failed");
    const auto grass_bytes = bytes(directory / "grass.gltf");
    const auto grass_json = nlohmann::json::parse(std::string{grass_bytes.begin(), grass_bytes.end()});
    const auto& color = grass_json["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"];
    const auto rgb = *pafta::procedural_material_rgb("city_terrain_grass");
    require(rgb == std::array<std::uint8_t, 3>{64, 84, 38}, "viewport grass color changed");
    for (std::size_t channel = 0; channel < rgb.size(); ++channel) {
        require(std::abs(color[channel].get<double>() - rgb[channel] / 255.0) < 1e-9,
            "export grass differs from the viewport material");
    }
    buffer.material_names.clear();
    const auto json = bytes(path);
    const std::string json_text(json.begin(), json.end());
    require(json_text.find("triangle.bin") != std::string::npos, "glTF does not reference its binary");

    require(pafta::write_gltf_export(*job).ok, "second glTF write failed");
    require(bytes(directory / "triangle.bin") == bin, "second glTF binary write changed bytes");
    require(bytes(path) == json, "second glTF JSON write changed bytes");

    group.transformed = true;
    group.origin = {10, 20, 30};
    group.world_offset = {5, 5, 5};
    group.basis.x_axis = {0, 0, 1};
    group.basis.z_axis = {-1, 0, 0};
    auto transformed = pafta::build_gltf_export(directory / "transformed.gltf", {group}, {}, result);
    require(static_cast<bool>(transformed) && pafta::write_gltf_export(*transformed).ok,
        "transformed triangle export failed");
    std::vector<std::uint8_t> expected_positions;
    for (const float value : {5.F, 15.F, 25.F, 5.F, 15.F, 26.F, 5.F, 16.F, 25.F})
        real(expected_positions, value);
    const auto transformed_bin = bytes(directory / "transformed.bin");
    require(transformed_bin.size() >= expected_positions.size() &&
            std::equal(expected_positions.begin(), expected_positions.end(), transformed_bin.begin()),
        "glTF world transform or recentering changed binary positions");

    buffer.triangles[0].city_window_surface = true;
    pafta::ExportOptions no_panes{};
    no_panes.include_window_panes = false;
    pafta::ExportResult empty_result;
    require(!pafta::build_gltf_export(directory / "empty.gltf", {group}, no_panes, empty_result),
        "disabled window pane was exported");
}

void test_invalid_triangle(const std::filesystem::path& directory) {
    pafta::BuildingRenderBuffer buffer;
    buffer.vertices.resize(1);
    pafta::RenderBufferTriangle triangle{};
    triangle.indices = {0, 1, 0};
    buffer.triangles.push_back(triangle);
    pafta::ExportMeshGroup group{};
    group.name = "invalid building";
    group.buffer = &buffer;
    pafta::ExportResult result;
    try {
        pafta::build_gltf_export(directory / "invalid.gltf", {group}, {}, result);
    } catch (const std::runtime_error& error) {
        require(std::string{error.what()}.find(group.name) != std::string::npos,
            "invalid export omitted the group name");
        return;
    }
    throw std::runtime_error("invalid triangle was exported");
}

void test_material_queue(const std::filesystem::path& directory) {
    pafta::BuildingRenderBuffer buffer;
    buffer.vertices = {{{0,0,0},0,0}, {{1,0,0},1,0}, {{0,1,0},0,1}};
    buffer.triangles.push_back({{0,1,2},0});
    buffer.material_names = {"brick"};
    pafta::ExportMeshGroup group{};
    group.buffer = &buffer;
    pafta::ExportResult result;
    const auto job = pafta::build_gltf_export(directory / "shared.gltf", {group, group}, {}, result);
    require(job && pafta::gltf_export_texture_count(*job) == 1,
        "groups sharing one material queued duplicate texture readbacks");
}

void test_export_group_selection() {
    pafta::SblBlockMesh block{};
    block.faces.push_back({5,
        {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}},
        {{0, 0}, {1, 0}, {1, 1}, {0, 1}}, true, 0});
    const auto block_buffer = pafta::build_block_mesh_render_buffer(block);
    require(block_buffer.vertices.size() == 4 && block_buffer.triangles.size() == 2,
        "block export did not triangulate its quad");
    require(block_buffer.vertices[1].position.x == 4.F && block_buffer.vertices[2].v == 1.F,
        "block export lost scale or UVs");
    require(block_buffer.material_names == std::vector<std::string>{"material_5"},
        "block export lost its material slot");

    pafta::MapScene scene{};
    scene.bounds = {0, 10, 0, 20};
    scene.terrain.emplace();
    pafta::SbcBuilding placement{0, "house", 0, 0, 0, 0};
    scene.buildings.push_back({placement, 0, {}, 0});
    pafta::SbbBuilding building{};
    building.materials = {"brick"};
    const auto groups = pafta::city_export_groups(scene, {&block_buffer}, {&building}, block_buffer);
    require(groups.size() == 4, "city export omitted a selected geometry group");
    require(groups[0].name == "terrain" && groups[1].name == "roads" &&
            groups[2].name == "pieces" && groups[3].name == "building_0",
        "city export group order changed");
    require(groups[2].world_offset.x == 5.F && groups[2].world_offset.y == -0.25F &&
            groups[2].world_offset.z == 10.F, "city piece recentering changed");
    require(groups[3].transformed && groups[3].building_materials == &building.materials,
        "city building transform or material table was lost");

    pafta::ExportOptions only_pieces{};
    only_pieces.include_buildings = false;
    only_pieces.include_terrain = false;
    only_pieces.include_roads = false;
    const auto filtered = pafta::city_export_groups(scene, {&block_buffer}, {&building}, block_buffer, only_pieces);
    require(filtered.size() == 1 && filtered[0].name == "pieces", "city export options did not filter groups");

    pafta::SbcMap map{};
    map.buildings.push_back(placement);
    require(pafta::estimate_city_export_triangles(map, {}, {}, {}) == 0,
        "unresolved city building contributed export triangles");
    building.size_z = 1;
    pafta::SbbCell cell{};
    cell.exterior_block() = 0x40000000U;
    cell.occupied = true;
    building.cells.push_back(cell);
    const auto expected_triangles = pafta::build_native_building_render_buffer(building, {}, {}).triangles.size();
    require(expected_triangles > 0 &&
            pafta::estimate_city_export_triangles(map, {&building}, {}, {}) == expected_triangles,
        "city export triangle estimate differs from its building mesh");
}

} // namespace

int main() {
    try {
        const auto directory = std::filesystem::path{PAFTA_TEST_OUTPUT_ROOT};
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        test_triangle_export(directory);
        test_invalid_triangle(directory);
        test_material_queue(directory);
        test_export_group_selection();
        std::cout << "glTF binary and repeated exports match\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
