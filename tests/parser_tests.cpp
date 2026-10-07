#include "formats.hpp"
#include "editor_state.hpp"
#include "buildblock_catalog.hpp"
#include "scene.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <filesystem>
#include <set>
#include <string>
#include <stdexcept>
#include <vector>

namespace {

std::filesystem::path fixture_root() {
    return std::filesystem::path{MAP_EDITOR_CPP_FIXTURE_ROOT};
}

bool close_to(float left, float right) {
    return std::fabs(left - right) < 0.0001F;
}

bool has_buffer_vertex(
    const map_editor::BuildingRenderBuffer& buffer,
    float x,
    float y,
    float z,
    float u,
    float v) {
    return std::ranges::any_of(buffer.vertices, [&](const map_editor::RenderBufferVertex& vertex) {
        return close_to(vertex.position.x, x) &&
            close_to(vertex.position.y, y) &&
            close_to(vertex.position.z, z) &&
            close_to(vertex.u, u) &&
            close_to(vertex.v, v);
    });
}

bool has_buffer_position(const map_editor::BuildingRenderBuffer& buffer, float x, float y, float z) {
    return std::ranges::any_of(buffer.vertices, [&](const map_editor::RenderBufferVertex& vertex) {
        return close_to(vertex.position.x, x) &&
            close_to(vertex.position.y, y) &&
            close_to(vertex.position.z, z);
    });
}

bool all_buffer_positions_on_x(const map_editor::BuildingRenderBuffer& buffer, float x) {
    return !buffer.vertices.empty() && std::ranges::all_of(buffer.vertices, [&](const map_editor::RenderBufferVertex& vertex) {
        return close_to(vertex.position.x, x);
    });
}

bool all_buffer_positions_on_y(const map_editor::BuildingRenderBuffer& buffer, float y) {
    return !buffer.vertices.empty() && std::ranges::all_of(buffer.vertices, [&](const map_editor::RenderBufferVertex& vertex) {
        return close_to(vertex.position.y, y);
    });
}

bool all_buffer_positions_on_z(const map_editor::BuildingRenderBuffer& buffer, float z) {
    return !buffer.vertices.empty() && std::ranges::all_of(buffer.vertices, [&](const map_editor::RenderBufferVertex& vertex) {
        return close_to(vertex.position.z, z);
    });
}

bool point_in_triangle_2d(float px, float py, map_editor::Vec3 a, map_editor::Vec3 b, map_editor::Vec3 c) {
    const auto area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 0.0001F) {
        return false;
    }
    const auto s = ((a.y - c.y) * (px - c.x) + (c.x - a.x) * (py - c.y)) / area;
    const auto t = ((c.y - b.y) * (px - c.x) + (b.x - c.x) * (py - c.y)) / area;
    const auto u = 1.0F - s - t;
    return s >= -0.0001F && t >= -0.0001F && u >= -0.0001F;
}

bool buffer_has_triangle_covering_z_point(
    const map_editor::BuildingRenderBuffer& buffer,
    float z,
    float x,
    float y) {
    return std::ranges::any_of(buffer.triangles, [&](const map_editor::RenderBufferTriangle& triangle) {
        const auto& a = buffer.vertices[triangle.indices[0]].position;
        const auto& b = buffer.vertices[triangle.indices[1]].position;
        const auto& c = buffer.vertices[triangle.indices[2]].position;
        return close_to(a.z, z) && close_to(b.z, z) && close_to(c.z, z) &&
            point_in_triangle_2d(x, y, a, b, c);
    });
}

bool has_city_tile(
    const map_editor::MapScene& scene,
    std::int32_t x,
    std::int32_t y,
    std::int32_t z,
    map_editor::CityTileKind kind) {
    return std::ranges::any_of(scene.city_tiles, [&](const map_editor::CityTileCell& cell) {
        return cell.x == x && cell.y == y && cell.z == z && cell.kind == kind;
    });
}

const map_editor::CityTileCell* find_city_tile(
    const map_editor::MapScene& scene,
    std::int32_t x,
    std::int32_t z,
    map_editor::CityTileKind kind) {
    const auto match = std::ranges::find_if(scene.city_tiles, [&](const map_editor::CityTileCell& cell) {
        return cell.x == x && cell.z == z && cell.kind == kind;
    });
    return match == scene.city_tiles.end() ? nullptr : &*match;
}

std::size_t count_city_tiles_at_x(
    const map_editor::MapScene& scene,
    std::int32_t x,
    map_editor::CityTileKind kind) {
    return static_cast<std::size_t>(std::ranges::count_if(scene.city_tiles, [&](const map_editor::CityTileCell& cell) {
        return cell.x == x && cell.kind == kind;
    }));
}

void append_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void append_f32(std::vector<std::uint8_t>& bytes, float value) {
    std::uint32_t word{};
    static_assert(sizeof(word) == sizeof(value));
    std::memcpy(&word, &value, sizeof(value));
    append_u32(bytes, word);
}

void parses_sbb_fixture() {
    const auto building = map_editor::parse_sbb_file(
        fixture_root() / "server/data/test2/building/test.sbb");

    assert(building.version == 13);
    assert(building.name.empty());
    assert(building.size_x == 12);
    assert(building.size_y == 12);
    assert(building.size_z == 5);
    assert(building.materials.size() >= 3);
    assert(building.materials[0] == "redbricks");
    assert(building.materials[1] == "woodpanels0");
    assert(building.materials[2] == "blackroof");
    assert(building.piece_names.size() == 4);
    assert(building.block_names.size() == 2);
    assert(building.item_set_names.empty());
    assert(!building.cells.empty());
}

void parses_sbb_v12_without_anchor_words() {
    std::vector<std::uint8_t> bytes;
    append_u32(bytes, 12);
    bytes.resize(bytes.size() + 0x40);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    bytes.insert(bytes.end(), {'b', 'r', 'i', 'c', 'k'});
    bytes.resize(0x54 + 0x40);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    for (std::size_t index = 0; index < 4; ++index) {
        append_u32(bytes, 0x40000000U + static_cast<std::uint32_t>(index));
        append_u32(bytes, 0xffffU);
        append_u32(bytes, 0U);
        append_u32(bytes, 0U);
        append_u32(bytes, 0U);
        append_u32(bytes, 0U);
        bytes.resize(bytes.size() + 16 * 2);
        append_u32(bytes, 0U);
    }

    const auto building = map_editor::parse_sbb(bytes);

    assert(building.version == 12);
    assert(building.anchor_x == 4);
    assert(building.anchor_y == 4);
    assert(building.anchor_z == 4);
    assert(building.materials.size() == 1);
    assert(building.materials[0] == "brick");
    assert(building.cells.size() == 4);
    assert(building.cells[0].exterior_block() == 0x40000000U);
}

void parses_sbb_cell_words_in_native_file_order() {
    std::vector<std::uint8_t> bytes;
    append_u32(bytes, 12);
    bytes.resize(bytes.size() + 0x40);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 1);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 0x40000000U);
    append_u32(bytes, 0xffffU);
    append_u32(bytes, 7U);
    append_u32(bytes, 8U);
    append_u32(bytes, 9U);
    append_u32(bytes, 10U);
    for (std::size_t index = 0; index < 16; ++index) {
        const auto value = static_cast<std::uint16_t>(index == 2 ? 12U : index == 10 ? 34U : 0U);
        bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    }
    append_u32(bytes, 11U);

    const auto building = map_editor::parse_sbb(bytes);

    assert(building.cells.size() == 1);
    const auto& cell = building.cells[0];
    assert(cell.exterior_block() == 0x40000000U);
    assert(cell.interior_block() == 0xffffU);
    assert(cell.cell_type() == 7U);
    assert(cell.exterior_portal_face() == 8U);
    assert(cell.interior_portal_face() == 9U);
    assert(cell.placed_flag() == 10U);
    assert(cell.shorts[2] == 12U);
    assert(cell.shorts[10] == 34U);
    assert(cell.item_set_index == 11U);
    assert(std::ranges::any_of(cell.payloads, [](const map_editor::SbbCellPayload& payload) {
        return payload.kind == map_editor::PayloadKind::Block && payload.word == 0x40000000U;
    }));
}

void parses_sbb_piece_payload_index_from_low_10_bits() {
    std::vector<std::uint8_t> bytes;
    append_u32(bytes, 12);
    bytes.resize(bytes.size() + 0x40);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 1);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 0x80000405U);
    append_u32(bytes, 0xffffU);
    append_u32(bytes, 0U);
    append_u32(bytes, 0U);
    append_u32(bytes, 0U);
    append_u32(bytes, 0U);
    bytes.resize(bytes.size() + 16 * 2);
    append_u32(bytes, 0U);

    const auto building = map_editor::parse_sbb(bytes);

    assert(building.cells.size() == 1);
    assert(building.cells[0].payloads.size() == 1);
    assert(building.cells[0].payloads[0].kind == map_editor::PayloadKind::Piece);
    assert(building.cells[0].payloads[0].index == 5U);
}

void rejects_unsupported_sbb_version() {
    std::vector<std::uint8_t> bytes;
    append_u32(bytes, 11);
    bytes.resize(0x50);

    bool threw = false;
    try {
        (void)map_editor::parse_sbb(bytes);
    } catch (const std::runtime_error&) {
        threw = true;
    }

    assert(threw);
}

void parses_sbb_v14_lod_tail() {
    std::vector<std::uint8_t> bytes;
    append_u32(bytes, 14);
    bytes.resize(bytes.size() + 0x40);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 1);
    append_u32(bytes, 4);
    append_u32(bytes, 0);
    append_u32(bytes, 4);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    bytes.resize(bytes.size() + 0x3c);
    append_u32(bytes, 1);
    append_u32(bytes, 2);
    append_u32(bytes, 3);

    const auto building = map_editor::parse_sbb(bytes);

    assert(building.version == 14);
    assert(building.cells.size() == 1);
    assert(building.lod_marker == 1);
    assert(building.lod_group_count == 2);
    assert(building.lod_tail.size() == 12);
}

void extracts_render_cells() {
    const auto building = map_editor::parse_sbb_file(
        fixture_root() / "server/data/test2/building/test.sbb");
    const auto scene = map_editor::build_sbb_scene(building);

    assert(scene.unit_size == 4.0F);
    assert(scene.width > 0.0F);
    assert(scene.height > 0.0F);
    assert(scene.depth > 0.0F);
    assert(!scene.cells.empty());

    bool has_block_payload = false;
    bool has_piece_payload = false;
    for (const auto& cell : building.cells) {
        for (const auto& payload : cell.payloads) {
            has_block_payload = has_block_payload || payload.kind == map_editor::PayloadKind::Block;
            has_piece_payload = has_piece_payload || payload.kind == map_editor::PayloadKind::Piece;
        }
    }

    assert(has_block_payload);
    assert(has_piece_payload);
}

void writes_sbl_blocks_losslessly() {
    // Every shipped block survives write_sbl_block: same structure, and a
    // version-2 file comes back byte for byte.
    std::size_t checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(fixture_root() / "server/data/test2/block")) {
        if (entry.path().extension() != ".sbl") continue;
        const auto bytes = map_editor::read_file_bytes(entry.path());
        const auto parsed = map_editor::parse_sbl(bytes);
        if (!parsed.block) continue;
        const auto written = map_editor::write_sbl_block(*parsed.block);
        const auto reparsed = map_editor::parse_sbl(written);
        assert(reparsed.block.has_value());
        assert(reparsed.block->width == parsed.block->width && reparsed.block->exterior_flags == parsed.block->exterior_flags);
        assert(reparsed.block->authored_patches == parsed.block->authored_patches);
        assert(reparsed.block->lights == parsed.block->lights);
        assert(reparsed.block->faces == parsed.block->faces);
        if (parsed.version == 2) assert(written == bytes);
        ++checked;
    }
    assert(checked > 10);
}

void resolves_sbb_piece_meshes() {
    const auto block = map_editor::parse_sbl_file(
        fixture_root() / "server/data/test2/block/astairwellfloor.sbl");
    assert(block.block.has_value());

    map_editor::SbbBuilding building{};
    building.extent_x = 1;
    building.extent_y = 1;
    building.extent_z = 1;
    building.piece_names = {"astairwellfloor"};
    building.cells.push_back({
        0,
        0,
        0,
        0,
        {0x80000000U, 0U, 0U, 0U, 0U, 0U},
        0,
        true,
        {map_editor::SbbCellPayload{map_editor::PayloadKind::Piece, 0x80000000U, 0U}},
    });

    const auto scene = map_editor::build_sbb_scene(
        building,
        {map_editor::NamedBlockMesh{"astairwellfloor", *block.block}});

    assert(!scene.pieces.empty());
    assert(scene.pieces[0].mesh.faces.size() == block.block->faces.size());
    assert(scene.cells.empty());
}

void build_sbb_scene_recenters_cells_with_native_anchor() {
    map_editor::SbbBuilding building{};
    building.anchor_x = 2;
    building.anchor_y = 3;
    building.anchor_z = 4;
    building.extent_x = 1;
    building.extent_y = 1;
    building.extent_z = 1;
    building.cells.push_back({
        0,
        2,
        3,
        4,
        {0x40000000U, 0xffffU, 0U, 0U, 0U, 0U},
        0,
        true,
        {map_editor::SbbCellPayload{map_editor::PayloadKind::Block, 0x40000000U, 0U}},
    });

    const auto scene = map_editor::build_sbb_scene(building);

    assert(scene.cells.size() == 1U);
    assert(close_to(scene.cells[0].center.x, 2.0F));
    assert(close_to(scene.cells[0].center.y, 2.0F));
    assert(close_to(scene.cells[0].center.z, 2.0F));
}

void native_building_rendering_includes_interior_piece_words() {
    map_editor::SblBlockMesh mesh{};
    mesh.width = 1;
    mesh.height = 1;
    mesh.depth = 1;
    mesh.faces.push_back({
        1U,
        {
            {0.0F, 0.0F, 0.0F},
            {1.0F, 0.0F, 0.0F},
            {1.0F, 1.0F, 0.0F},
            {0.0F, 1.0F, 0.0F},
        },
        {
            {0.0F, 0.0F},
            {1.0F, 0.0F},
            {1.0F, 1.0F},
            {0.0F, 1.0F},
        },
        false,
        0U,
    });

    map_editor::SbbBuilding building{};
    building.extent_x = 1;
    building.extent_y = 1;
    building.extent_z = 1;
    building.piece_names = {"piece"};
    building.cells.push_back({
        0,
        0,
        0,
        0,
        {0U, 0x80000000U, 0U, 0U, 0U, 0U},
        0,
        true,
        {map_editor::SbbCellPayload{map_editor::PayloadKind::Piece, 0x80000000U, 0U}},
    });

    const auto scene = map_editor::build_sbb_scene(
        building,
        {map_editor::NamedBlockMesh{"piece", mesh}});
    const auto buffer = map_editor::build_native_building_render_buffer(
        building,
        {map_editor::NamedBlockMesh{"piece", mesh}},
        {});

    assert(scene.pieces.empty());
    assert(buffer.triangles.size() == 2U);
    assert(buffer.material_runs.size() == 1U);

    const auto preview_buffer = map_editor::build_native_building_tool_preview_render_buffer(
        building,
        {map_editor::NamedBlockMesh{"piece", mesh}},
        {},
        0U);
    assert(preview_buffer.triangles.size() == 2U);
    assert(preview_buffer.material_runs.size() == 1U);
}

void native_building_tool_preview_generates_floor_faces_from_floor_cell_key() {
    map_editor::SbbBuilding building{};
    building.extent_x = 1;
    building.extent_y = 2;
    building.extent_z = 1;
    map_editor::SbbCell floor0_cell{
        0,
        0,
        0,
        0,
        {0x40000000U, 0xffffU, 0U, 0U, 0U, 1U},
        0,
        true,
        {map_editor::SbbCellPayload{map_editor::PayloadKind::Block, 0x40000000U, 0U}},
    };
    floor0_cell.shorts[8] = 18U;
    building.cells.push_back(floor0_cell);

    map_editor::SbbCell floor1_cell{
        1,
        0,
        1,
        0,
        {0x40000000U, 0xffffU, 0U, 0U, 0U, 1U},
        0,
        true,
        {map_editor::SbbCellPayload{map_editor::PayloadKind::Block, 0x40000000U, 0U}},
    };
    floor1_cell.shorts[8] = 28U;
    building.cells.push_back(floor1_cell);

    const auto floor0 = map_editor::build_native_building_tool_preview_render_buffer(building, {}, {}, 0U);
    const auto floor1 = map_editor::build_native_building_tool_preview_render_buffer(building, {}, {}, 1U);

    assert(floor0.triangles.size() == 2U);
    assert(floor1.triangles.size() == 2U);
    assert(has_buffer_vertex(floor0, 0.0F, 0.25F, 0.0F, 0.0F, 0.0F));
    assert(has_buffer_vertex(floor0, 4.0F, 0.25F, 0.0F, 0.0F, 1.0F));
    assert(has_buffer_vertex(floor0, 4.0F, 0.25F, 4.0F, 1.0F, 1.0F));
    assert(has_buffer_vertex(floor0, 0.0F, 0.25F, 4.0F, 1.0F, 0.0F));
    assert(has_buffer_position(floor1, 0.0F, 4.25F, 0.0F));
    assert(std::ranges::any_of(floor1.material_runs, [](const map_editor::RenderBufferMaterialRun& run) {
        return run.material == 28U && run.triangle_count == 2U;
    }));
}

void native_building_tool_preview_generates_ceiling_from_floor_cell_key() {
    map_editor::SbbBuilding building{};
    building.extent_x = 1;
    building.extent_y = 1;
    building.extent_z = 1;
    map_editor::SbbCell cell{
        0,
        0,
        0,
        0,
        {0x40000000U, 0xffffU, 0U, 0U, 0U, 3U},
        0,
        true,
        {map_editor::SbbCellPayload{map_editor::PayloadKind::Block, 0x40000000U, 0U}},
    };
    cell.shorts[8] = 18U;
    cell.shorts[13] = 33U;
    building.cells.push_back(cell);

    const auto buffer = map_editor::build_native_building_tool_preview_render_buffer(building, {}, {}, 0U);

    assert(buffer.triangles.size() == 4U);
    assert(has_buffer_position(buffer, 0.0F, 0.25F, 0.0F));
    assert(has_buffer_position(buffer, 0.0F, 3.5F, 0.0F));
    assert(std::ranges::any_of(buffer.material_runs, [](const map_editor::RenderBufferMaterialRun& run) {
        return run.material == 33U && run.triangle_count == 2U;
    }));
}

void native_building_tool_preview_emits_only_exposed_solid_top_faces() {
    auto make_cell = [](std::size_t index, std::uint32_t y) {
        return map_editor::SbbCell{
            index,
            0,
            y,
            0,
            {0x40000000U, 0U, 0U, 0U, 0U, 0U},
            0,
            true,
            {},
        };
    };

    map_editor::SbbBuilding exposed{};
    exposed.cells.push_back(make_cell(0U, 0U));
    const auto exposed_buffer = map_editor::build_native_building_tool_preview_render_buffer(exposed, {}, {}, 0U);
    assert(exposed_buffer.triangles.size() == 10U);
    assert(has_buffer_position(exposed_buffer, 0.0F, 4.0F, 0.0F));

    map_editor::SbbBuilding covered{};
    covered.cells.push_back(make_cell(0U, 0U));
    covered.cells.push_back(make_cell(1U, 1U));
    const auto covered_buffer = map_editor::build_native_building_tool_preview_render_buffer(covered, {}, {}, 0U);
    assert(covered_buffer.triangles.size() == 8U);
}

map_editor::BuildingRenderBuffer native_preview_buffer_for_default_side(std::uint32_t side) {
    auto make_cell = [](std::uint32_t x, std::uint32_t z, std::uint32_t exterior_face, std::uint32_t interior_face, bool occupied) {
        return map_editor::SbbCell{
            0,
            x,
            0,
            z,
            {0x40000000U, 0xffffU, 0U, exterior_face, interior_face, 0U},
            0,
            occupied,
            {},
        };
    };

    map_editor::SbbBuilding building{};
    building.extent_x = 2;
    building.extent_y = 1;
    building.extent_z = 2;

    switch (side) {
        case 0U:
            building.cells.push_back(make_cell(0U, 0U, 1U, 0U, true));
            break;
        case 1U:
            building.cells.push_back(make_cell(0U, 0U, 0U, 0U, true));
            building.cells.push_back(make_cell(1U, 0U, 0U, 1U, false));
            break;
        case 2U:
            building.cells.push_back(make_cell(0U, 0U, 0U, 0U, true));
            building.cells.push_back(make_cell(0U, 1U, 1U, 0U, false));
            break;
        case 3U:
        default:
            building.cells.push_back(make_cell(0U, 0U, 0U, 1U, true));
            break;
    }

    return map_editor::build_native_building_tool_preview_render_buffer(building, {}, {}, 0U);
}

void native_building_tool_preview_places_side_code_faces_on_source_side() {
    const auto north = native_preview_buffer_for_default_side(0U);
    const auto east = native_preview_buffer_for_default_side(1U);
    const auto south = native_preview_buffer_for_default_side(2U);
    const auto west = native_preview_buffer_for_default_side(3U);

    assert(north.triangles.size() == 2U);
    assert(east.triangles.size() == 2U);
    assert(south.triangles.size() == 2U);
    assert(west.triangles.size() == 2U);

    // Side walls are inset 1/16 of a cell inward from the boundary (editor.exe
    // SetupDefaultFaceVerticesAndEmit places the side template at 0.0625), so for
    // a unit_size=4 cell the inset is 0.25. This separates adjacent cells' walls
    // (opposing walls land at 0.25 and 3.75) instead of z-fighting on a shared plane.
    assert(all_buffer_positions_on_z(north, 0.25F));
    assert(all_buffer_positions_on_x(east, 3.75F));
    assert(all_buffer_positions_on_z(south, 3.75F));
    assert(all_buffer_positions_on_x(west, 0.25F));
}

void native_building_tool_preview_generates_inset_boxes_from_floor_cell_side_code() {
    map_editor::SblLayer layer{};
    layer.index = 0;
    layer.portal_type = 0;
    layer.bounds = {0.25F, 0.5F, 0.25F, 0.5F};
    layer.windows.push_back({0U, 0.35F, 0.2F, 0.35F, 0.25F});
    layer.beams.push_back({0U, 0.1F, 0.8F, 0.7F, 0.1F});

    map_editor::SblTemplate buildblock{};
    buildblock.version = 1;
    buildblock.layers.push_back(layer);

    map_editor::SbbBuilding building{};
    building.extent_x = 1;
    building.extent_y = 1;
    building.extent_z = 1;
    building.block_names = {"portal"};
    map_editor::SbbCell cell{
        0,
        0,
        0,
        0,
        {0x40000000U, 0xffffU, 0U, 3U, 0U, 0U},
        0,
        true,
        {map_editor::SbbCellPayload{map_editor::PayloadKind::Block, 0x40000000U, 0U}},
    };
    cell.shorts[9] = 19U;
    cell.shorts[14] = 46U;
    building.cells.push_back(cell);

    const auto buffer = map_editor::build_native_building_tool_preview_render_buffer(
        building,
        {},
        {map_editor::NamedBuildblockTemplate{"portal", buildblock}},
        0U);

    assert(buffer.triangles.size() >= 24U);
    assert(has_buffer_position(buffer, 0.0F, 0.0F, 0.25F));
    assert(std::ranges::any_of(buffer.material_runs, [](const map_editor::RenderBufferMaterialRun& run) {
        return run.material == 46U;
    }));
}

void resolves_sbb_portal_edge_units() {
    const auto building = map_editor::parse_sbb_file(
        fixture_root() / "server/data/test2/building/test.sbb");
    const auto test_buildblock = map_editor::parse_sbl_file(
        fixture_root() / "server/data/test2/buildblock/test.sbl");
    const auto parking_buildblock = map_editor::parse_sbl_file(
        fixture_root() / "server/data/test2/buildblock/parking.sbl");

    std::size_t expected_edges = 0;
    for (const auto& cell : building.cells) {
        if ((cell.exterior_block() >> 29U) == 2U && cell.interior_block() == 0xffffU && cell.cell_type() < building.block_names.size()) {
            ++expected_edges;
        }
    }

    const auto scene = map_editor::build_sbb_scene(
        building,
        {},
        {
            map_editor::NamedBuildblockTemplate{"test", test_buildblock},
            map_editor::NamedBuildblockTemplate{"parking", parking_buildblock},
        });

    assert(expected_edges > 0);
    assert(scene.portal_edges.size() == expected_edges);

    const auto& first = scene.portal_edges.front();
    const auto* source = first.source;
    assert(source != nullptr);
    assert(first.template_file.layers.size() == test_buildblock.layers.size());
    assert(first.side_codes.north == source->exterior_portal_face());
    assert(first.side_codes.west == source->interior_portal_face());

    const auto east = std::ranges::find_if(building.cells, [source](const map_editor::SbbCell& cell) {
        return cell.x == source->x + 1U && cell.y == source->y && cell.z == source->z;
    });
    // Sbb_BuildBuildingConnectivityMetadata @29120: south is the (z+1) cell's w3.
    const auto south = std::ranges::find_if(building.cells, [source](const map_editor::SbbCell& cell) {
        return cell.x == source->x && cell.y == source->y && cell.z == source->z + 1U;
    });
    assert(first.side_codes.south == (south == building.cells.end() ? 0U : south->exterior_portal_face()));
    assert(first.side_codes.east == (east == building.cells.end() ? 0U : east->interior_portal_face()));
}

void native_portal_edge_side_codes_follow_editor_edge_flags() {
    map_editor::SblTemplate buildblock{};
    buildblock.version = 1;
    buildblock.layers.resize(4);

    map_editor::SbbBuilding building{};
    building.extent_x = 2;
    building.extent_y = 2;
    building.extent_z = 1;
    building.block_names = {"portal"};
    building.cells.push_back({
        0,
        0,
        0,
        0,
        {0x40000000U, 0xffffU, 0U, 4U, 5U, 0U},
        0,
        true,
        {},
    });
    building.cells.push_back({
        1,
        0,
        0,
        1,
        {0x40000000U, 0xffffU, 0U, 9U, 0U, 0U},
        0,
        true,
        {},
    });
    building.cells.push_back({
        2,
        1,
        0,
        0,
        {0x40000000U, 6U, 0U, 0U, 6U, 0U},
        0,
        true,
        {},
    });

    const auto scene = map_editor::build_sbb_scene(
        building,
        {},
        {map_editor::NamedBuildblockTemplate{"portal", buildblock}});

    assert(scene.portal_edges.size() >= 1);
    const auto& edge = scene.portal_edges.front();
    // South is the z+1 cell's w3 (@29120).
    assert(edge.side_codes.south == 9U);
    assert(edge.side_codes.west == 5U);
    assert(edge.side_codes.north == 4U);
    assert(edge.side_codes.east == 6U);
}

void attaches_portal_wall_rects_to_scene_edges() {
    map_editor::SblLayer layer{};
    layer.index = 1;
    layer.portal_type = 0;
    layer.bounds = {0.25F, 0.5F, 0.25F, 0.5F};

    map_editor::SblTemplate buildblock{};
    buildblock.version = 1;
    buildblock.layers.resize(2);
    buildblock.layers[1] = layer;

    map_editor::SbbBuilding building{};
    building.extent_x = 1;
    building.extent_y = 1;
    building.extent_z = 1;
    building.block_names = {"portal"};
    building.cells.push_back({
        0,
        0,
        0,
        0,
        {0x40000000U, 0xffffU, 0U, 4U, 0U, 0U},
        0,
        true,
        {},
    });

    const auto scene = map_editor::build_sbb_scene(
        building,
        {},
        {map_editor::NamedBuildblockTemplate{"portal", buildblock}});

    assert(scene.portal_edges.size() == 1);
    assert(scene.portal_edges[0].wall_rects.north.size() == 4);
    // No z+1 cell supplies a south code (@29120).
    assert(scene.portal_edges[0].wall_rects.south.empty());
    assert(scene.portal_edges[0].wall_rects.west.empty());
    assert(close_to(scene.portal_edges[0].wall_rects.north[0].height, 0.25F));
}

void builds_native_building_render_buffer_from_piece_meshes() {
    const auto building = map_editor::parse_sbb_file(
        fixture_root() / "server/data/test2/building/test.sbb");
    const auto block = map_editor::parse_sbl_file(
        fixture_root() / "server/data/test2/block/astairwellfloor.sbl");
    assert(block.block.has_value());

    const auto buffer = map_editor::build_native_building_render_buffer(
        building,
        {map_editor::NamedBlockMesh{"astairwellfloor", *block.block}},
        {});

    assert(!buffer.vertices.empty());
    assert(!buffer.triangles.empty());
    assert(buffer.triangles.size() % 2U == 0U);
    for (const auto& triangle : buffer.triangles) {
        for (const auto index : triangle.indices) {
            assert(index < buffer.vertices.size());
        }
    }
    assert(!buffer.material_runs.empty());
    assert(buffer.material_runs.front().first_triangle == 0U);
    for (std::size_t index = 1; index < buffer.material_runs.size(); ++index) {
        const auto& previous = buffer.material_runs[index - 1];
        const auto& current = buffer.material_runs[index];
        assert(previous.material < current.material ||
            (previous.material == current.material && !previous.city_window_surface && current.city_window_surface));
        assert(buffer.material_runs[index - 1].first_triangle + buffer.material_runs[index - 1].triangle_count ==
               buffer.material_runs[index].first_triangle);
    }
    assert(buffer.material_runs.back().first_triangle + buffer.material_runs.back().triangle_count == buffer.triangles.size());
}

void native_building_render_buffer_indexes_shared_cell_corners_and_uvs() {
    map_editor::SbbBuilding building{};
    building.extent_x = 1;
    building.extent_y = 1;
    building.extent_z = 1;
    building.cells.push_back({
        0,
        0,
        0,
        0,
        {0x40000000U, 0xffffU, 0U, 0U, 0U, 0U},
        0,
        true,
        {},
    });

    const auto buffer = map_editor::build_native_building_render_buffer(building, {}, {});

    // Six shell faces: nothing solid touches the lone cell (the bottom
    // included). Its room adds the derived ceiling.
    assert(std::ranges::count_if(buffer.triangles, [](const map_editor::RenderBufferTriangle& t) { return !t.interior; }) == 12);
    assert(buffer.vertices.size() > 8U);
    assert(buffer.vertices.size() <= 24U);
    for (const auto& triangle : buffer.triangles) {
        for (const auto index : triangle.indices) {
            assert(index < buffer.vertices.size());
        }
    }
}

void native_building_render_buffer_emits_down_face_only_over_open_cells() {
    // World_SetTileBlockAndResolveNeighborLinks @f830: a solid cell keeps its
    // bottom face unless a solid cell sits below, so overhangs show their
    // underside and a cell resting on another does not.
    map_editor::SbbBuilding building{};
    building.cells.push_back({0, 0, 0, 0, {0x40000000U, 0xffffU, 0U, 0U, 0U, 0U}, 0, true, {}});
    building.cells.push_back({1, 1, 1, 0, {0x40000000U, 0xffffU, 0U, 0U, 0U, 0U}, 0, true, {}});
    building.cells.push_back({2, 0, 1, 0, {0x40000000U, 0xffffU, 0U, 0U, 0U, 0U}, 0, true, {}});
    const auto buffer = map_editor::build_native_building_render_buffer(building, {}, {});
    const auto bottom_at = [&](float y, float x0, float x1) {
        return std::ranges::count_if(buffer.triangles, [&](const map_editor::RenderBufferTriangle& triangle) {
            const auto& a = buffer.vertices[triangle.indices[0]].position;
            const auto& b = buffer.vertices[triangle.indices[1]].position;
            const auto& c = buffer.vertices[triangle.indices[2]].position;
            const auto cx = (a.x + b.x + c.x) / 3.0F;
            return close_to(a.y, y) && close_to(b.y, y) && close_to(c.y, y) && cx > x0 && cx < x1;
        });
    };
    assert(bottom_at(0.0F, 0.0F, 4.0F) == 2);  // ground cell
    assert(bottom_at(4.0F, 4.0F, 8.0F) == 2);  // overhang at x 1, nothing below
    assert(bottom_at(4.0F, 0.0F, 4.0F) == 0);  // resting on the ground cell
}

void native_building_render_buffer_uses_editor_face_order_and_uvs() {
    map_editor::SbbBuilding building{};
    building.extent_x = 1;
    building.extent_y = 1;
    building.extent_z = 1;
    building.cells.push_back({
        0,
        0,
        0,
        0,
        {0x40000000U, 0xffffU, 0U, 0U, 0U, 0U},
        0,
        true,
        {},
    });

    const auto buffer = map_editor::build_native_building_render_buffer(building, {}, {});

    assert(has_buffer_vertex(buffer, 0.0F, 4.0F, 4.0F, 0.0F, 1.0F));
    assert(has_buffer_vertex(buffer, 4.0F, 4.0F, 4.0F, 1.0F, 1.0F));
    assert(has_buffer_vertex(buffer, 4.0F, 0.0F, 4.0F, 1.0F, 0.0F));
    assert(has_buffer_vertex(buffer, 0.0F, 0.0F, 4.0F, 0.0F, 0.0F));
    assert(has_buffer_vertex(buffer, 0.0F, 4.0F, 0.0F, 0.0F, 1.0F));
    assert(has_buffer_vertex(buffer, 4.0F, 4.0F, 0.0F, 1.0F, 1.0F));
}

void native_building_render_buffer_uses_cell_material_slots() {
    map_editor::SbbBuilding building{};
    building.extent_x = 1;
    building.extent_y = 1;
    building.extent_z = 1;
    map_editor::SbbCell cell{
        0,
        0,
        0,
        0,
        {0x40000000U, 0xffffU, 0U, 0U, 0U, 0U},
        0,
        true,
        {},
    };
    cell.shorts[2] = 12U;
    building.cells.push_back(cell);

    const auto buffer = map_editor::build_native_building_render_buffer(building, {}, {});
    const auto material_run = std::ranges::find_if(buffer.material_runs, [](const map_editor::RenderBufferMaterialRun& run) {
        return run.material == 12U;
    });

    assert(material_run != buffer.material_runs.end());
    assert(material_run->triangle_count == 2U);
}

void native_building_render_buffer_renders_interior_piece_with_interior_materials() {
    map_editor::SblBlockMesh mesh{};
    mesh.width = 1U;
    mesh.height = 1U;
    mesh.depth = 1U;
    mesh.faces.push_back({
        0U,
        {
            {0.0F, 1.0F, 0.0F},
            {1.0F, 1.0F, 0.0F},
            {1.0F, 0.0F, 0.0F},
            {0.0F, 0.0F, 0.0F},
        },
        {
            {0.0F, 1.0F},
            {1.0F, 1.0F},
            {1.0F, 0.0F},
            {0.0F, 0.0F},
        },
        false,
        0U,
    });

    map_editor::SbbBuilding building{};
    building.extent_x = 1U;
    building.extent_y = 1U;
    building.extent_z = 1U;
    building.piece_names = {"interior"};
    auto cell = map_editor::SbbCell{
        0U,
        0U,
        0U,
        0U,
        {0x40000000U, 0x80000000U, 0U, 0U, 0U, 0U},
        0U,
        true,
        {map_editor::SbbCellPayload{map_editor::PayloadKind::Piece, 0x80000000U, 0U}},
    };
    cell.shorts[8] = 91U;
    building.cells.push_back(cell);

    const auto buffer = map_editor::build_native_building_render_buffer(
        building,
        {map_editor::NamedBlockMesh{"interior", mesh}},
        {});
    const auto interior_run = std::ranges::find_if(buffer.material_runs, [](const map_editor::RenderBufferMaterialRun& run) {
        return run.material == 91U;
    });

    assert(interior_run != buffer.material_runs.end());
    assert(interior_run->triangle_count == 2U);
}

// A 0xffff interior cell gets the _block_createbuildblock room: the floor at
// 1/16 pulled in from walled sides, and each walled side 1/16 inside the edge,
// all two-sided, in the interior texture set.
void native_building_render_buffer_builds_interior_room() {
    map_editor::SbbBuilding building{};
    auto cell = map_editor::SbbCell{0, 0, 0, 0, {0x40000000U, 0xffffU, 0U, 1U, 0U, 1U}, 0, true, {}};
    cell.shorts[8] = 21U;  // interior floor
    cell.shorts[9] = 22U;  // interior north wall: _block_addbeam's slot 1 on side 0
    building.cells.push_back(cell);
    const auto buffer = map_editor::build_native_building_render_buffer(building, {}, {});
    const auto count = [&](std::uint32_t material, auto predicate) {
        return std::ranges::count_if(buffer.triangles, [&](const map_editor::RenderBufferTriangle& tri) {
            return tri.material == material && predicate(tri);
        });
    };
    const auto all_y = [&](float y) {
        return [&buffer, y](const map_editor::RenderBufferTriangle& tri) {
            return std::ranges::all_of(tri.indices, [&](std::uint32_t i) { return std::abs(buffer.vertices[i].position.y - y) < 1e-4F; });
        };
    };
    const auto all_z = [&](float z) {
        return [&buffer, z](const map_editor::RenderBufferTriangle& tri) {
            return std::ranges::all_of(tri.indices, [&](std::uint32_t i) { return std::abs(buffer.vertices[i].position.z - z) < 1e-4F; });
        };
    };
    // Cell spans 0..4 (anchor 0); floor at 4/16, wall at z = 4/16.
    assert(count(21U, all_y(0.25F)) == 4);
    // The floor is two-sided; the wall faces only into the room.
    assert(count(22U, all_z(0.25F)) == 2);
    // Visible faces wind so their geometric normal points away from the viewer:
    // the kept north wall (z = 0.25) is seen from the room, so its normal is -z.
    for (const auto& tri : buffer.triangles) {
        if (tri.material != 22U) continue;
        const auto& p0 = buffer.vertices[tri.indices[0]].position;
        const auto& p1 = buffer.vertices[tri.indices[1]].position;
        const auto& p2 = buffer.vertices[tri.indices[2]].position;
        const auto normal_z = (p1.x - p0.x) * (p2.y - p0.y) - (p1.y - p0.y) * (p2.x - p0.x);
        assert(normal_z < 0.0F);
    }
    // The floor stops at the north wall but runs to the open sides.
    for (const auto& tri : buffer.triangles) {
        if (tri.material != 21U) continue;
        for (const auto i : tri.indices) {
            const auto& p = buffer.vertices[i].position;
            assert(p.z >= 0.25F - 1e-4F && p.x >= -1e-4F && p.x <= 4.0F + 1e-4F);
        }
    }
}

void native_building_rooms_follow_the_game_room_key() {
    const auto room = [](std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t w3, std::uint32_t w4, std::uint32_t placed) {
        return map_editor::SbbCell{0, x, y, z, {0x40000000U, 0xffffU, 0U, w3, w4, placed}, 0, true, {}};
    };
    const auto interior_quads_at_y = [](const map_editor::BuildingRenderBuffer& buffer, float y) {
        return std::ranges::count_if(buffer.triangles, [&](const map_editor::RenderBufferTriangle& t) {
            return t.interior && std::ranges::all_of(t.indices, [&](std::uint32_t i) { return close_to(buffer.vertices[i].position.y, y); });
        });
    };
    // Ceiling (Sbb_BuildBuildingConnectivityMetadata): at 7/8 under a room that
    // has its own floor or on the top layer, none under a room without one.
    {
        map_editor::SbbBuilding building{};
        building.cells = {room(0, 0, 0, 0, 0, 1), room(0, 1, 0, 0, 0, 1)};
        building.cells[1].index = 1;
        const auto buffer = map_editor::build_native_building_render_buffer(building, {}, {});
        assert(interior_quads_at_y(buffer, 3.5F) == 4);  // lower room: upper has a floor
        assert(interior_quads_at_y(buffer, 7.5F) == 4);  // upper room: top layer
        building.cells[1].placed_flag() = 0U;
        const auto open = map_editor::build_native_building_render_buffer(building, {}, {});
        assert(interior_quads_at_y(open, 3.5F) == 0);
    }
    // South walls run the game's wall frame backwards: a type-0 opening at
    // x 0.1..0.3 sits at x 0.1..0.3 (0.4..1.2 units), not mirrored.
    {
        map_editor::SblLayer layer{};
        layer.bounds = {0.1F, 0.2F, 0.2F, 0.5F};
        map_editor::SblTemplate bb{};
        bb.version = 1;
        bb.layers.resize(2);
        bb.layers[1] = layer;
        map_editor::SbbBuilding building{};
        building.block_names = {"bb"};
        building.cells = {room(0, 0, 0, 0, 0, 0), map_editor::SbbCell{1, 0, 0, 1, {0U, 0U, 0U, 4U, 0U, 0U}, 0, false, {}}};
        const auto buffer = map_editor::build_native_building_render_buffer(building, {}, {map_editor::NamedBuildblockTemplate{"bb", bb}});
        float lo = 9.0F, hi = -9.0F;
        for (const auto& t : buffer.triangles) {
            if (!t.interior || t.slot != 14U) continue;  // jambs
            for (const auto i : t.indices) {
                lo = std::min(lo, buffer.vertices[i].position.x);
                hi = std::max(hi, buffer.vertices[i].position.x);
            }
        }
        assert(close_to(lo, 0.4F) && close_to(hi, 1.2F));
    }
    // Corner fill (code 2): the west neighbour's north wall ends at this room's
    // open NW corner, so the room fills the 1/16 notch there.
    {
        map_editor::SbbBuilding building{};
        building.cells = {room(0, 0, 0, 1, 0, 0), room(1, 0, 0, 0, 0, 0)};
        building.cells[1].index = 1;
        const auto buffer = map_editor::build_native_building_render_buffer(building, {}, {});
        assert(std::ranges::any_of(buffer.triangles, [&](const map_editor::RenderBufferTriangle& t) {
            return t.interior && t.cell == 1U && std::ranges::all_of(t.indices, [&](std::uint32_t i) {
                const auto& p = buffer.vertices[i].position;
                return close_to(p.z, 0.25F) && p.x >= 4.0F - 1e-4F && p.x <= 4.25F + 1e-4F;
            });
        }));
    }
}

void building_face_pick_returns_the_painted_slot_of_each_face() {
    // Paint writes cell.shorts[pick.slot]; every tagged triangle must be drawn
    // with that slot's material, and each side must pick its own slot.
    map_editor::SbbBuilding building{};
    auto cell = map_editor::SbbCell{0, 0, 0, 0, {0x40000000U, 0xffffU, 0U, 1U, 0U, 1U}, 0, true, {}};
    for (std::uint32_t slot = 0; slot < 16U; ++slot) cell.shorts[slot] = static_cast<std::uint16_t>(100U + slot);
    building.cells.push_back(cell);
    const auto buffer = map_editor::build_native_building_render_buffer(building, {}, {});
    std::size_t tagged = 0;
    for (const auto& tri : buffer.triangles) {
        if (tri.slot == 0xffU) continue;
        ++tagged;
        assert(tri.cell == 0U);
        assert(tri.material == 100U + tri.slot);
    }
    assert(tagged > 0U);
    const std::array<std::pair<map_editor::Vec3, map_editor::Vec3>, 5> rays{{
        {{-10.0F, 2.0F, 2.0F}, {1.0F, 0.0F, 0.0F}},
        {{14.0F, 2.0F, 2.0F}, {-1.0F, 0.0F, 0.0F}},
        {{2.0F, 2.0F, -10.0F}, {0.0F, 0.0F, 1.0F}},
        {{2.0F, 2.0F, 14.0F}, {0.0F, 0.0F, -1.0F}},
        {{2.0F, 14.0F, 2.0F}, {0.0F, -1.0F, 0.0F}},
    }};
    std::set<std::uint32_t> slots;
    for (const auto& [origin, direction] : rays) {
        const auto pick = map_editor::pick_building_face(buffer, origin, direction);
        assert(pick.has_value());
        assert(pick->cell_index == 0U);
        assert(pick->slot < 8U);
        slots.insert(pick->slot);
    }
    assert(slots.size() == rays.size());
}

void native_building_buildblock_bounds_are_the_opening() {
    map_editor::SblLayer layer{};
    layer.index = 1;
    layer.portal_type = 0;
    layer.bounds = {0.25F, 0.5F, 0.25F, 0.5F};

    map_editor::SblTemplate buildblock{};
    buildblock.version = 1;
    buildblock.layers.resize(2);
    buildblock.layers[1] = layer;

    map_editor::SbbBuilding building{};
    building.extent_x = 1;
    building.extent_y = 1;
    building.extent_z = 1;
    building.block_names = {"portal"};
    auto cell = map_editor::SbbCell{
        0,
        0,
        0,
        0,
        {0x40000000U, 0xffffU, 0U, 4U, 0U, 0U},
        0,
        true,
        {},
    };
    cell.shorts[2] = 12U;
    building.cells.push_back(cell);

    const auto buffer = map_editor::build_native_building_render_buffer(
        building,
        {},
        {map_editor::NamedBuildblockTemplate{"portal", buildblock}});

    const auto frame_run = std::ranges::find_if(buffer.material_runs, [](const map_editor::RenderBufferMaterialRun& run) {
        return run.material == 12U;
    });
    assert(frame_run != buffer.material_runs.end());
    assert(std::ranges::none_of(buffer.material_runs, [](const map_editor::RenderBufferMaterialRun& run) {
        return run.material == 0x100U;
    }));
    assert(!buffer.wire_lines.empty());
    // Bounds x 0.25..0.75, y 0.25..0.75 of the north (z = 0) wall; cell 0..4.
    // _generatematerialwindowtexture: from outside the bounds is a reflective
    // opening even though the layer has no window records.
    const auto centroid = [&](const map_editor::RenderBufferTriangle& tri) {
        map_editor::Vec3 c{};
        for (const auto i : tri.indices) {
            const auto& p = buffer.vertices[i].position;
            c = {c.x + p.x / 3.0F, c.y + p.y / 3.0F, c.z + p.z / 3.0F};
        }
        return c;
    };
    const auto in_opening = [](const map_editor::Vec3& c) { return c.x > 1.0F && c.x < 3.0F && c.y > 1.0F && c.y < 3.0F; };
    assert(std::ranges::any_of(buffer.triangles, [&](const map_editor::RenderBufferTriangle& tri) {
        return tri.city_window_surface && !tri.interior && std::abs(centroid(tri).z) < 1e-4F && in_opening(centroid(tri));
    }));
    // BuildBlock_AddBeams: the room wall frames the bounds and leaves it open;
    // the jambs (interior slot 6) run 1/16 deep from the cell edge.
    assert(std::ranges::none_of(buffer.triangles, [&](const map_editor::RenderBufferTriangle& tri) {
        const auto c = centroid(tri);
        return tri.interior && !tri.city_window_surface && std::abs(c.z - 0.25F) < 1e-4F && in_opening(c);
    }));
    assert(std::ranges::any_of(buffer.triangles, [&](const map_editor::RenderBufferTriangle& tri) {
        const auto c = centroid(tri);
        return tri.interior && tri.slot == 14U && c.z > 0.0F && c.z < 0.25F;
    }));
}

void native_building_tool_preview_uses_frame_faces_for_buildblock_opening() {
    map_editor::SblLayer layer{};
    layer.index = 1;
    layer.portal_type = 0;
    layer.bounds = {0.25F, 0.5F, 0.25F, 0.5F};
    layer.windows.push_back({0U, 0.375F, 0.25F, 0.4F, 0.2F});
    layer.beams.push_back({0U, 0.1F, 0.8F, 0.7F, 0.08F});

    map_editor::SblTemplate buildblock{};
    buildblock.version = 1;
    buildblock.layers.resize(2);
    buildblock.layers[1] = layer;

    map_editor::SbbBuilding building{};
    building.extent_x = 1;
    building.extent_y = 1;
    building.extent_z = 1;
    building.block_names = {"portal"};
    auto cell = map_editor::SbbCell{
        0,
        0,
        0,
        0,
        {0x40000000U, 0xffffU, 0U, 4U, 0U, 0U},
        0,
        true,
        {map_editor::SbbCellPayload{map_editor::PayloadKind::Block, 0x40000000U, 0U}},
    };
    cell.shorts[14] = 46U;
    building.cells.push_back(cell);

    const auto buffer = map_editor::build_native_building_tool_preview_render_buffer(
        building,
        {},
        {map_editor::NamedBuildblockTemplate{"portal", buildblock}},
        0U);

    assert(buffer.triangles.size() > 10U);
    assert(has_buffer_position(buffer, 0.0F, 0.0F, 0.25F));
    assert(std::ranges::any_of(buffer.material_runs, [](const map_editor::RenderBufferMaterialRun& run) {
        return run.material == 46U;
    }));
}

void city_building_render_buffer_uses_full_building_preview_geometry() {
    map_editor::SblBlockMesh interior_mesh{};
    interior_mesh.width = 1;
    interior_mesh.height = 1;
    interior_mesh.depth = 1;
    interior_mesh.faces.push_back({
        0U,
        {
            {0.0F, 1.0F, 0.0F},
            {1.0F, 1.0F, 0.0F},
            {1.0F, 0.0F, 0.0F},
            {0.0F, 0.0F, 0.0F},
        },
        {
            {0.0F, 1.0F},
            {1.0F, 1.0F},
            {1.0F, 0.0F},
            {0.0F, 0.0F},
        },
        false,
        0x60000U,
    });

    map_editor::SblLayer layer{};
    layer.index = 1;
    layer.portal_type = 0;
    layer.bounds = {0.25F, 0.5F, 0.25F, 0.5F};
    layer.windows.push_back({0U, 0.375F, 0.25F, 0.4F, 0.2F});
    layer.beams.push_back({0U, 0.1F, 0.8F, 0.7F, 0.08F});

    map_editor::SblTemplate buildblock{};
    buildblock.version = 1;
    buildblock.layers.resize(2);
    buildblock.layers[1] = layer;

    map_editor::SbbBuilding building{};
    building.extent_x = 1;
    building.extent_y = 1;
    building.extent_z = 1;
    building.block_names = {"portal"};
    building.piece_names = {"interior"};
    auto cell = map_editor::SbbCell{
        0,
        0,
        0,
        0,
        {0x40000000U, 0x80000000U, 0U, 4U, 0U, 0U},
        0,
        true,
        {
            map_editor::SbbCellPayload{map_editor::PayloadKind::Block, 0x40000000U, 0U},
            map_editor::SbbCellPayload{map_editor::PayloadKind::Piece, 0x80000000U, 0U},
        },
    };
    cell.shorts[14] = 46U;
    cell.shorts[8] = 91U;
    cell.shorts[3] = 12U;
    building.cells.push_back(cell);

    const auto buffer = map_editor::build_city_building_full_render_buffer(
        building,
        {map_editor::NamedBlockMesh{"interior", interior_mesh}},
        {map_editor::NamedBuildblockTemplate{"portal", buildblock}});

    assert(std::ranges::any_of(buffer.material_runs, [](const map_editor::RenderBufferMaterialRun& run) {
        return run.material == 91U;
    }));
}

void native_building_tool_preview_leaves_buildblock_opening_empty() {
    map_editor::SblLayer layer{};
    layer.index = 1;
    layer.portal_type = 0;
    layer.bounds = {0.25F, 0.5F, 0.25F, 0.5F};

    map_editor::SblTemplate buildblock{};
    buildblock.version = 1;
    buildblock.layers.resize(2);
    buildblock.layers[1] = layer;

    map_editor::SbbBuilding building{};
    building.extent_x = 1;
    building.extent_y = 1;
    building.extent_z = 1;
    building.block_names = {"portal"};
    auto cell = map_editor::SbbCell{
        0,
        0,
        0,
        0,
        {0x40000000U, 0xffffU, 0U, 4U, 0U, 0U},
        0,
        true,
        {map_editor::SbbCellPayload{map_editor::PayloadKind::Block, 0x40000000U, 0U}},
    };
    cell.shorts[14] = 46U;
    building.cells.push_back(cell);

    const auto buffer = map_editor::build_native_building_tool_preview_render_buffer(
        building,
        {},
        {map_editor::NamedBuildblockTemplate{"portal", buildblock}},
        0U);

    assert(!buffer_has_triangle_covering_z_point(buffer, 0.25F, 2.0F, 2.0F));
}

void picks_nearest_occupied_building_cell_with_native_anchor() {
    map_editor::SbbBuilding building{};
    building.anchor_x = 2;
    building.anchor_y = 3;
    building.anchor_z = 4;
    building.extent_x = 3;
    building.extent_y = 1;
    building.extent_z = 1;
    building.cells.push_back({
        17,
        1,
        3,
        4,
        {0x40000000U, 0xffffU, 0U, 0U, 0U, 0U},
        0,
        false,
        {},
    });
    building.cells.push_back({
        18,
        2,
        3,
        4,
        {0x40000000U, 0xffffU, 0U, 0U, 0U, 0U},
        0,
        true,
        {},
    });
    building.cells.push_back({
        19,
        3,
        3,
        4,
        {0x40000000U, 0xffffU, 0U, 0U, 0U, 0U},
        0,
        true,
        {},
    });

    const auto pick = map_editor::pick_building_cell(building, {-10.0F, 2.0F, 2.0F}, {1.0F, 0.0F, 0.0F}, 4.0F);

    assert(pick.has_value());
    assert(pick->cell_index == 1U);
    assert(close_to(pick->t, 10.0F));
    assert(close_to(pick->hit_world.x, 0.0F));
    assert(close_to(pick->hit_world.y, 2.0F));
    assert(close_to(pick->hit_world.z, 2.0F));
}

void building_cell_picker_respects_visible_floor_filter() {
    map_editor::SbbBuilding building{};
    building.anchor_x = 0;
    building.anchor_y = 0;
    building.anchor_z = 0;
    building.extent_x = 1;
    building.extent_y = 2;
    building.extent_z = 1;
    building.cells.push_back({
        10,
        0,
        0,
        0,
        {0x40000000U, 0xffffU, 0U, 0U, 0U, 0U},
        0,
        true,
        {},
    });
    building.cells.push_back({
        11,
        0,
        1,
        0,
        {0x40000000U, 0xffffU, 0U, 0U, 0U, 0U},
        0,
        true,
        {},
    });

    const auto pick = map_editor::pick_building_cell(
        building,
        {2.0F, 12.0F, 2.0F},
        {0.0F, -1.0F, 0.0F},
        4.0F,
        0U);

    assert(pick.has_value());
    assert(pick->cell_index == 0U);
}

void city_building_picker_ignores_empty_cells_inside_bounds() {
    map_editor::SbbBuilding building{};
    building.anchor_x = 0;
    building.anchor_y = 0;
    building.anchor_z = 0;
    building.extent_x = 3;
    building.extent_y = 1;
    building.extent_z = 1;
    building.cells.push_back({
        20,
        0,
        0,
        0,
        {0x40000000U, 0xffffU, 0U, 0U, 0U, 0U},
        0,
        true,
        {},
    });
    building.cells.push_back({
        21,
        1,
        0,
        0,
        {0x40000000U, 0xffffU, 0U, 0U, 0U, 0U},
        0,
        false,
        {},
    });
    building.cells.push_back({
        22,
        2,
        0,
        0,
        {0x40000000U, 0xffffU, 0U, 0U, 0U, 0U},
        0,
        true,
        {},
    });

    map_editor::SbcMap map{};
    map.buildings.push_back({0, "sparse", 0, 0, 0, 0});
    const std::vector<const map_editor::SbbBuilding*> placement_buildings{&building};

    const auto pick = map_editor::pick_city_building(
        map,
        placement_buildings,
        {6.0F, 8.0F, 2.0F},
        {0.0F, -1.0F, 0.0F},
        4.0F);

    assert(!pick.has_value());
}

void city_piece_picker_selects_occupied_portal_voxel() {
    map_editor::SbcMap map{};
    map_editor::SbcPortal portal{};
    portal.index = 0;
    portal.fields = {0, 2, 1, 3};
    portal.blocks.assign(0x200, 0U);
    portal.blocks[4U + 2U * 0x40U + 5U * 8U] = 0x01020007U;
    map.portals.push_back(portal);

    const auto pick = map_editor::pick_city_piece(
        map,
        {82.0F, 64.0F, 118.0F},
        {0.0F, -1.0F, 0.0F},
        4.0F);

    assert(pick.has_value());
    assert(pick->portal_index == 0U);
    assert(pick->block_index == 4U + 2U * 0x40U + 5U * 8U);
    assert(pick->word == 0x01020007U);
}

void edits_selected_sbb_cell_state() {
    auto building = map_editor::parse_sbb_file(
        fixture_root() / "server/data/test2/building/test.sbb");
    map_editor::SbbEditorState state{building};

    const auto initial = state.selected_index();
    state.select_next_occupied();
    assert(state.selected_index() != initial);

    state.set_active_field(map_editor::SbbCellField::ExteriorPortalFace);
    state.adjust_active_field(5);
    const auto edited = state.selected_cell();
    assert(edited != nullptr);
    assert(edited->exterior_portal_face() == building.cells[state.selected_index()].exterior_portal_face() + 5U);
    assert(state.dirty());
}

void edits_selected_sbc_building_state() {
    auto map = map_editor::parse_sbc_file(
        fixture_root() / "server/data/test2/city2.sbc");
    map_editor::SbcEditorState state{map};

    assert(state.selected_index() == 0);
    state.select_next_building();
    assert(state.selected_index() == 1);

    state.set_active_field(map_editor::SbcBuildingField::X);
    state.adjust_active_field(12);
    const auto edited = state.selected_building();
    assert(edited != nullptr);
    assert(edited->x == map.buildings[1].x + 12);
    assert(state.dirty());
}

void edits_selected_sbl_word_state() {
    auto bytes = map_editor::read_file_bytes(fixture_root() / "server/data/test2/buildblock/test.sbl");
    map_editor::SblEditorState state{bytes};

    assert(state.selected_word_index() == 0);
    state.select_next_word();
    assert(state.selected_word_index() == 1);
    const auto before = state.selected_word_value();

    state.adjust_selected_word(9);
    assert(state.selected_word_value() == before + 9U);
    assert(state.dirty());
}

void parses_sbc_fixture() {
    const auto map = map_editor::parse_sbc_file(
        fixture_root() / "server/data/test2/city2.sbc");

    assert(map.version == 12);
    assert(map.intersections.size() == 72);
    assert(map.streets.size() == 126);
    assert(map.buildings.size() == 178);
    assert(map.streets[0].from_intersection == 0);
    assert(map.streets[0].to_intersection == 1);
    assert(map.streets[0].name == "Street");
    assert(map.buildings[0].name == "bank");
    assert(map.buildings[0].x == 324);
    assert(map.buildings[0].z == 403);
    assert(map.buildings[0].rotation == 3);
    assert(!map.city_blocks.empty());
    assert(!map.city_blocks[0].layers.empty());
    assert(map.city_blocks[0].layers[0].size() == 8);
    assert(!map.portals.empty());
    assert(map.portals[0].blocks.size() == 512);
    assert(map.portals[0].blocks[164] == 65537U);
}

void parses_sbc_v10_without_city_blocks() {
    std::vector<std::uint8_t> bytes;
    append_u32(bytes, 10); // version
    append_u32(bytes, 0);  // layer name count
    append_u32(bytes, 0);  // intersections
    append_u32(bytes, 0);  // streets
    append_u32(bytes, 0);  // buildings
    append_u32(bytes, 1);  // portals; v10 has no city-block section
    append_u32(bytes, 0);
    append_u32(bytes, 2);
    append_u32(bytes, 3);
    append_u32(bytes, 4);
    for (std::size_t index = 0; index < 0x200; ++index) {
        append_u32(bytes, index == 17 ? 0x80000005U : 0U);
    }
    for (std::size_t index = 0; index < 0x1000; ++index) {
        const auto value = static_cast<std::uint16_t>(index == 7 ? 123U : 0U);
        bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
        bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    }
    for (std::size_t index = 0; index < 0x200; ++index) {
        append_u32(bytes, index == 17 ? 9U : 0U);
    }

    auto map = map_editor::parse_sbc(bytes);

    assert(map.version == 10);
    assert(map.city_blocks.empty());
    assert(map.portals.size() == 1);
    assert(map.portals[0].fields[1] == 2);
    assert(map.portals[0].fields[2] == 3);
    assert(map.portals[0].fields[3] == 4);
    assert(map.portals[0].blocks.empty());
    assert(map.portals[0].legacy_blocks[17] == 0x80000005U);
    assert(map.portals[0].textures[7] == 123U);
    assert(map.portals[0].itemsets[17] == 9U);

    map.portals[0].legacy_blocks[17] = 0x40000007U;
    map.portals[0].textures[7] = 321U;
    map.portals[0].itemsets[17] = 11U;

    const auto patched = map_editor::patch_sbc_portals(bytes, map);
    const auto reparsed = map_editor::parse_sbc(patched);

    assert(reparsed.portals[0].blocks.empty());
    assert(reparsed.portals[0].legacy_blocks[17] == 0x40000007U);
    assert(reparsed.portals[0].textures[7] == 321U);
    assert(reparsed.portals[0].itemsets[17] == 11U);
}

void rejects_unsupported_sbc_version() {
    std::vector<std::uint8_t> bytes;
    append_u32(bytes, 13);

    bool threw = false;
    try {
        (void)map_editor::parse_sbc(bytes);
    } catch (const std::runtime_error&) {
        threw = true;
    }

    assert(threw);
}

void builds_map_scene() {
    const auto map = map_editor::parse_sbc_file(
        fixture_root() / "server/data/test2/city2.sbc");
    const auto scene = map_editor::build_map_scene(map);

    assert(scene.unit_size == 4.0F);
    assert(scene.intersections.size() == map.intersections.size());
    assert(scene.roads.size() == map.streets.size());
    assert(scene.buildings.size() == map.buildings.size());
    assert(scene.bounds.max_x > scene.bounds.min_x);
    assert(scene.bounds.max_z > scene.bounds.min_z);
    assert(scene.roads[0].width == static_cast<float>(
        map.streets[0].left_width + map.streets[0].right_width + 2) * scene.unit_size);
}

void builds_grid_aligned_city_street_tile_strip() {
    map_editor::SbcMap map{};
    map.intersections.push_back({0U, 0, 0, 0});
    map.intersections.push_back({1U, 4, 0, 0});
    map.streets.push_back({0U, 0, 1, 0, 1, 2, "East"});

    const auto scene = map_editor::build_map_scene(map);

    // Native semantics: cell level = sbc y - 1 (subrosa.exe loads unk0[2] as
    // record y - 1). Lanes span [-leftWidth, rightWidth); curb plates sit one
    // level above the lanes, one tile outside each edge.
    for (std::int32_t x = 0; x <= 3; ++x) {
        assert(has_city_tile(scene, x, -1, -1, map_editor::CityTileKind::Road));
        assert(has_city_tile(scene, x, -1, 0, map_editor::CityTileKind::Road));
        assert(has_city_tile(scene, x, -1, 1, map_editor::CityTileKind::Road));
    }
    assert(has_city_tile(scene, 2, 0, -2, map_editor::CityTileKind::Curb));
    assert(has_city_tile(scene, 2, 0, 2, map_editor::CityTileKind::Curb));
    assert(count_city_tiles_at_x(scene, 2, map_editor::CityTileKind::Road) == 3U);
    assert(scene.city_tile_buffer.triangles.size() > 0U);
    assert(std::ranges::any_of(scene.city_tile_buffer.material_names, [](const std::string& name) {
        return name == "city_road_asphalt";
    }));
    assert(std::ranges::any_of(scene.city_tile_buffer.material_names, [](const std::string& name) {
        return name == "city_curb_concrete";
    }));
}

void removes_junctions_no_road_uses() {
    map_editor::SbcMap map{};
    for (std::int32_t i = 0; i < 4; ++i) map.intersections.push_back({static_cast<std::size_t>(i), 100 + i * 20, 5, 100});
    map.streets.push_back({0U, 0, 3, 0, 1, 1, "Kept"});
    map_editor::remove_unused_city_intersections(map);
    assert(map.intersections.size() == 2U);
    assert(map.intersections[1].index == 1U && map.intersections[1].x == 160);
    assert(map.streets[0].from_intersection == 0 && map.streets[0].to_intersection == 1);
}

void drops_city_tiles_outside_the_game_world() {
    map_editor::SbcMap map{};
    // Climbs from level 500 to 520 over 300 cells: the top crosses the
    // 512-level world ceiling, where the game drops the tiles.
    map.intersections.push_back({0U, 100, 500, 100});
    map.intersections.push_back({1U, 400, 520, 100});
    map.streets.push_back({0U, 0, 1, 0, 1, 2, "East"});
    const auto full = map_editor::build_map_scene(map);
    const auto fit = map_editor::build_map_scene(map, {});
    assert(fit.world_fit.road_tiles_outside > 0U && fit.world_fit.road_tiles_no_room == 0U);
    assert(fit.city_tiles.size() + fit.world_fit.road_tiles_outside == full.city_tiles.size());
    assert(std::ranges::all_of(fit.city_tiles, [](const auto& t) { return map_editor::city_cell_in_game_world(t.x, t.y, t.z); }));
    assert(fit.world_fit.sectors_used > 0U && fit.world_fit.sectors_used <= map_editor::GameWorldFit::max_sectors);
}

void builds_city_street_tile_strips_for_all_cardinal_endpoint_orders() {
    const auto has_segment = [](std::int32_t from_x, std::int32_t from_z, std::int32_t to_x, std::int32_t to_z,
                                 std::int32_t probe_x, std::int32_t probe_z) {
        map_editor::SbcMap map{};
        map.intersections.push_back({0U, from_x, 0, from_z});
        map.intersections.push_back({1U, to_x, 0, to_z});
        map.streets.push_back({0U, 0, 1, 0, 1, 1, "Road"});

        const auto scene = map_editor::build_map_scene(map);
        return find_city_tile(scene, probe_x, probe_z, map_editor::CityTileKind::Road) != nullptr;
    };

    assert(has_segment(0, 0, 4, 0, 2, 0));
    assert(has_segment(4, 0, 0, 0, 2, 0));
    assert(has_segment(0, 0, 0, 4, 0, 2));
    assert(has_segment(0, 4, 0, 0, 0, 2));
}

void canonicalizes_city_street_endpoint_order_for_native_axes() {
    map_editor::SbcMap map{};
    map.intersections.push_back({0U, 8, 0, 0});
    map.intersections.push_back({1U, 0, 0, 0});
    map.intersections.push_back({2U, 4, 0, 8});
    map.intersections.push_back({3U, 4, 0, 0});

    const auto west = map_editor::canonical_city_street_endpoint_order(map, {0U, 0, 1, 0, 1, 1, "West"});
    const auto north = map_editor::canonical_city_street_endpoint_order(map, {1U, 2, 3, 0, 1, 1, "North"});

    assert(west.from_intersection == 1);
    assert(west.to_intersection == 0);
    assert(north.from_intersection == 3);
    assert(north.to_intersection == 2);
}

void fills_intersection_footprint_and_closes_missing_sides_with_curbs() {
    map_editor::SbcMap map{};
    map.intersections.push_back({0U, 0, 0, 0});
    map.intersections.push_back({1U, 4, 0, 0});
    map.intersections.push_back({2U, 0, 0, 6});
    map.streets.push_back({0U, 0, 1, 0, 1, 1, "East"});
    map.streets.push_back({1U, 0, 2, 0, 1, 1, "South"});

    const auto scene = map_editor::build_map_scene(map);

    // Footprint fill over the native extents (x: -ext_x_neg .. ext_x_pos - 1,
    // z: -ext_z_neg .. ext_z_pos - 1) at level y - 1 = -1.
    assert(has_city_tile(scene, -1, -1, -1, map_editor::CityTileKind::Road));
    assert(has_city_tile(scene, 0, -1, 0, map_editor::CityTileKind::Road));
    // Missing -x neighbor: curb row one level up at x = -ext_x_pos - 1.
    assert(has_city_tile(scene, -2, 0, -2, map_editor::CityTileKind::Curb));
    assert(has_city_tile(scene, -2, 0, 1, map_editor::CityTileKind::Curb));
    // Missing -z neighbor: curb row one level up at z = -ext_z_neg - 1.
    assert(has_city_tile(scene, 1, 0, -2, map_editor::CityTileKind::Curb));
}

void terrain_noise_hash_matches_native_all_ones_mask() {
    assert(map_editor::terrain_noise_hash(0, 0, 0xffffffffU) == 0x63faaa2eU);
    assert(map_editor::terrain_noise_hash(640, 640, 0xffffffffU) == 0x432be22eU);
    assert(map_editor::terrain_noise_hash(2048, 2048, 0xffffffffU) == 0x676c2a2eU);
    assert(map_editor::terrain_noise_hash(640, 640, 0xffffffffU) !=
        map_editor::terrain_noise_hash(0, 0, 0xffffffffU));
}

void terrain_noise_sample_uses_native_two_to_negative_thirty_normalizer() {
    constexpr auto sx = 640.0F;
    constexpr auto sz = 640.0F;
    constexpr auto fx = 1.0F / 64.0F;
    constexpr auto fz = 1.0F / 128.0F;
    const auto expected = static_cast<float>(map_editor::terrain_noise_hash(10, 5, 0xffffffffU)) * 0x1p-30F;

    assert(close_to(map_editor::terrain_noise_sample(sx, sz, fx, fz), expected));
}

void native_like_city_terrain_height_matches_native_core_and_far_island() {
    // World origin sits on the city-map shoreline shelf carved to 11.75; the
    // city core (around world 1024..2592) stays on the flat 23.875 base.
    assert(close_to(map_editor::native_like_city_terrain_height(0.0F, 0.0F), 11.75F));
    assert(close_to(map_editor::native_like_city_terrain_height(1024.0F, 1024.0F), 23.875F));
    assert(close_to(map_editor::native_like_city_terrain_height(1536.0F, 1536.0F), 23.875F));

    const auto far_height = map_editor::native_like_city_terrain_height(4096.0F, 4096.0F);
    assert(far_height > 60.0F);
    assert(far_height < 260.0F);
    assert(!close_to(far_height, 23.875F));
}

float native_surface_y(std::int32_t sbc_y) {
    // Native level = sbc y - 1; drivable surface = level * 4 + 4.25.
    return static_cast<float>(sbc_y - 1) * 4.0F + 4.25F;
}

// City block whose north-east corner is intersection index 2 — one of the 28
// hardcoded indices Terrain_RebuildLandTilesRoadPatchesAndChunkCaches patches.
map_editor::SbcMap native_patch_block_map(std::array<std::int32_t, 4> sbc_levels) {
    map_editor::SbcMap map{};
    map.intersections.push_back({0U, 632, sbc_levels[1], 600});  // B: +x of A
    map.intersections.push_back({1U, 600, sbc_levels[2], 632});  // C: +z of A
    map.intersections.push_back({2U, 600, sbc_levels[0], 600});  // A: patched index 2
    map.intersections.push_back({3U, 632, sbc_levels[3], 632});  // D: diagonal
    map.streets.push_back({0U, 2, 0, 0, 1, 1, "AB"});
    map.streets.push_back({1U, 2, 1, 0, 1, 1, "AC"});
    map.streets.push_back({2U, 1, 3, 0, 1, 1, "CD"});
    map.streets.push_back({3U, 0, 3, 0, 1, 1, "BD"});
    return map;
}

void city_terrain_patches_hardcoded_intersection_blocks() {
    const auto scene = map_editor::build_map_scene(native_patch_block_map({5, 5, 5, 9}));

    assert(scene.terrain.has_value());
    assert(scene.terrain->mesh.triangles.size() > 0U);
    assert(close_to(scene.terrain->mesh.vertices.front().position.x, -4096.0F));
    assert(close_to(scene.terrain->mesh.vertices.front().position.z, -4096.0F));
    assert(close_to(scene.terrain->mesh.vertices.back().position.x, 12288.0F));
    assert(close_to(scene.terrain->mesh.vertices.back().position.z, 12288.0F));
    assert(scene.terrain->mesh.triangles.size() < 300000U);

    // Patch rect uses the +1-expanded extents: world [2408, 2520] on both axes.
    // Corners pin to the intersection surfaces; the center is their bilinear mix.
    const auto corner = map_editor::terrain_height_at(*scene.terrain, 2408.0F, 2408.0F);
    assert(close_to(corner, native_surface_y(5)));
    const auto center = map_editor::terrain_height_at(*scene.terrain, 2464.0F, 2464.0F);
    const auto expected_center =
        (3.0F * native_surface_y(5) + native_surface_y(9)) * 0.25F;
    assert(close_to(center, expected_center));

    // Outside the patched block the city core stays on the flat island base.
    const auto outside = map_editor::terrain_height_at(*scene.terrain, 2300.0F, 2400.0F);
    assert(close_to(outside, 23.875F));
}

void city_terrain_flat_block_patch_is_level() {
    const auto scene = map_editor::build_map_scene(native_patch_block_map({5, 5, 5, 5}));

    assert(scene.terrain.has_value());
    assert(close_to(map_editor::terrain_height_at(*scene.terrain, 2410.0F, 2410.0F), native_surface_y(5)));
    assert(close_to(map_editor::terrain_height_at(*scene.terrain, 2464.0F, 2464.0F), native_surface_y(5)));
    assert(close_to(map_editor::terrain_height_at(*scene.terrain, 2518.0F, 2518.0F), native_surface_y(5)));
}

void city_terrain_inside_patch_conforms_to_piece_cells() {
    // District/piece cells stamp the corner-height grid; inside a patched block
    // the terrain re-seeds to the stamped cell levels instead of the bilinear.
    auto map = native_patch_block_map({5, 5, 5, 5});
    map_editor::SbcPortal portal;
    portal.index = 0;
    portal.fields = {0, 76, 1, 76};  // tiles 608..615, cell level 8 -> 32.0
    portal.blocks.assign(512, 0U);
    for (std::size_t voxel = 0; voxel < 64; ++voxel) {  // bottom layer occupied
        portal.blocks[voxel] = 1U;
    }
    map.portals.push_back(portal);

    const auto scene = map_editor::build_map_scene(map);
    assert(scene.terrain.has_value());
    // World 2440 sits over a seeded tile: terrain matches the stamped level.
    assert(close_to(map_editor::terrain_height_at(*scene.terrain, 2440.0F, 2440.0F), 32.0F));
    // Far corner of the block stays on the bilinear surface.
    assert(close_to(map_editor::terrain_height_at(*scene.terrain, 2518.0F, 2518.0F), native_surface_y(5)));

    // The piece payload participates in the scene fingerprint.
    const auto without = map_editor::city_scene_fingerprint(native_patch_block_map({5, 5, 5, 5}));
    assert(map_editor::city_scene_fingerprint(map) != without);
}

void city_district_cell_replaces_the_street_tile_in_its_cell() {
    // Later world writes replace street cells (ApplyAllCityDistrictCellsToWorld,
    // InstantiateSbbVolumeIntoWorldAndMetadata): a piece in a road tile's cell
    // removes that tile.
    auto map = native_patch_block_map({5, 5, 5, 5});
    const auto before = map_editor::build_map_scene(map);
    assert(!before.city_tiles.empty());
    const auto tile = before.city_tiles.front();
    map_editor::SbcPortal portal;
    portal.index = 0;
    portal.fields = {0, tile.x / 8, tile.y / 8, tile.z / 8};
    portal.blocks.assign(512, 0U);
    portal.blocks[static_cast<std::size_t>((tile.y % 8) * 64 + (tile.z % 8) * 8 + (tile.x % 8))] = 1U;
    map.portals.push_back(portal);
    const auto after = map_editor::build_map_scene(map);
    const auto same = [&](const map_editor::CityTileCell& other) {
        return other.x == tile.x && other.y == tile.y && other.z == tile.z;
    };
    assert(std::ranges::any_of(before.city_tiles, same));
    assert(std::ranges::none_of(after.city_tiles, same));
    assert(after.city_tiles.size() == before.city_tiles.size() - 1U);
}

void city_scene_fingerprint_sees_every_voxel() {
    map_editor::SbcMap map;
    map_editor::SbcPortal portal;
    portal.fields = {0, 1, 2, 3};
    portal.blocks.assign(512, 0U);
    map.portals.push_back(portal);
    const auto base = map_editor::city_scene_fingerprint(map);
    for (const std::size_t word : {0U, 1U, 2U, 3U, 255U, 510U, 511U}) {
        auto changed = map;
        changed.portals[0].blocks[word] = 0x40000000U;
        assert(map_editor::city_scene_fingerprint(changed) != base);
    }
    assert(map_editor::city_scene_fingerprint(map) == base);
}

void fly_camera_moves_along_the_view() {
    using map_editor::FlyInput;
    using map_editor::fly_camera_step;
    const auto near = [](map_editor::Vec3 v, float x, float y, float z) {
        return std::abs(v.x - x) < 1e-4F && std::abs(v.y - y) < 1e-4F && std::abs(v.z - z) < 1e-4F;
    };
    const auto len = [](map_editor::Vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); };
    const map_editor::Vec3 level{0.0F, 0.0F, -1.0F};
    const map_editor::Vec3 up45{0.0F, 1.0F, -1.0F};
    // W follows the view, including its pitch; S is the reverse.
    assert(near(fly_camera_step(level, {.forward = true}, 10.0F, 1.0F), 0.0F, 0.0F, -10.0F));
    const auto climb = fly_camera_step(up45, {.forward = true}, 10.0F, 1.0F);
    assert(climb.y > 7.0F && climb.z < -7.0F && std::abs(len(climb) - 10.0F) < 1e-3F);
    assert(near(fly_camera_step(up45, {.back = true}, 10.0F, 1.0F), -climb.x, -climb.y, -climb.z));
    // D strafes right and stays level even while looking up; A is the reverse.
    assert(near(fly_camera_step(up45, {.right = true}, 10.0F, 1.0F), 10.0F, 0.0F, 0.0F));
    assert(near(fly_camera_step(level, {.left = true}, 10.0F, 1.0F), -10.0F, 0.0F, 0.0F));
    // Space / Ctrl go straight up / down whatever the view; together they cancel.
    assert(near(fly_camera_step(up45, {.up = true}, 10.0F, 1.0F), 0.0F, 10.0F, 0.0F));
    assert(near(fly_camera_step(level, {.down = true}, 10.0F, 1.0F), 0.0F, -10.0F, 0.0F));
    assert(near(fly_camera_step(level, {.up = true, .down = true}, 10.0F, 1.0F), 0.0F, 0.0F, 0.0F));
    assert(near(fly_camera_step(level, {.forward = true, .back = true}, 10.0F, 1.0F), 0.0F, 0.0F, 0.0F));
    // Diagonals are not faster.
    assert(std::abs(len(fly_camera_step(level, {.forward = true, .right = true, .up = true}, 10.0F, 1.0F)) - 10.0F) < 1e-3F);
    // Shift 1/4, Z 4x, Shift+Z 1/16; scales with frame time.
    assert(std::abs(len(fly_camera_step(level, {.forward = true, .slow = true}, 16.0F, 1.0F)) - 4.0F) < 1e-3F);
    assert(std::abs(len(fly_camera_step(level, {.forward = true, .fast = true}, 16.0F, 1.0F)) - 64.0F) < 1e-3F);
    assert(std::abs(len(fly_camera_step(level, {.forward = true, .slow = true, .fast = true}, 16.0F, 1.0F)) - 1.0F) < 1e-3F);
    assert(std::abs(len(fly_camera_step(level, {.forward = true}, 16.0F, 0.5F)) - 8.0F) < 1e-3F);
    // Looking straight down: W descends, strafing still has a sane direction.
    const map_editor::Vec3 down{0.0F, -1.0F, 0.0F};
    assert(near(fly_camera_step(down, {.forward = true}, 10.0F, 1.0F), 0.0F, -10.0F, 0.0F));
    const auto strafe = fly_camera_step(down, {.right = true}, 10.0F, 1.0F);
    assert(std::isfinite(strafe.x) && std::abs(len(strafe) - 10.0F) < 1e-3F && strafe.y == 0.0F);
    // No keys, no motion.
    assert(near(fly_camera_step(level, {}, 10.0F, 1.0F), 0.0F, 0.0F, 0.0F));
}

void overlapping_city_placements_let_the_later_one_win() {
    // InstantiateSbbVolumeIntoWorldAndMetadata deletes the world cell under each
    // building cell with an exterior block word, so the later placement wins.
    map_editor::SbbBuilding building;
    for (std::uint32_t cx = 0; cx < 3; ++cx) {
        map_editor::SbbCell cell;
        cell.x = cx;
        cell.occupied = true;
        cell.words[0] = 0x40000000U;
        building.cells.push_back(cell);
    }
    map_editor::SbcMap map;
    map.buildings.push_back({0U, "a", 100, 8, 100, 0});
    map.buildings.push_back({1U, "a", 102, 8, 100, 0});
    const auto overwritten = map_editor::overwritten_building_cells(map, {&building, &building});
    assert(overwritten.size() == 2U);
    assert(overwritten[1].empty());
    assert(overwritten[0].size() == 1U);
    assert(overwritten[0][0] == 2U);
}

void city_terrain_inside_patch_conforms_to_building_footprints() {
    // Building instantiation stamps its anchor-layer footprint at record y*4;
    // inside a patched block the terrain conforms to the building plot.
    auto map = native_patch_block_map({5, 5, 5, 5});
    map.buildings.push_back({0U, "plot", 610, 8, 610, 0});

    map_editor::SbbBuilding building;
    building.name = "plot";
    building.anchor_x = 0;
    building.anchor_y = 0;
    building.anchor_z = 0;
    for (std::uint32_t cx = 0; cx < 4; ++cx) {
        for (std::uint32_t cz = 0; cz < 4; ++cz) {
            map_editor::SbbCell cell;
            cell.x = cx;
            cell.y = 0;
            cell.z = cz;
            cell.occupied = true;
            cell.exterior_block() = 0x40000000U;
            building.cells.push_back(cell);
        }
    }

    const auto scene = map_editor::build_map_scene(map, {&building});
    assert(scene.terrain.has_value());
    // Tiles 610..613 stamped at 8*4=32: terrain over the plot follows it.
    assert(close_to(map_editor::terrain_height_at(*scene.terrain, 2446.0F, 2446.0F), 32.0F));
    // Without the resolved building, the block stays bilinear.
    const auto plain = map_editor::build_map_scene(map);
    assert(close_to(map_editor::terrain_height_at(*plain.terrain, 2446.0F, 2446.0F), native_surface_y(5)));
}

void city_road_tile_heights_follow_native_slope_region() {
    // Native streets are flat at each end and ramp only inside the centered
    // integer run-length slope region (MapConnector_PlaceStreetSegmentTilesAndSlopes
    // @20580); the surface sits at level*4+4.25 with level = sbc y - 1.
    map_editor::SbcMap map{};
    map.intersections.push_back({0U, 600, 1, 600});   // level 0, surface 4.25
    map.intersections.push_back({1U, 660, 9, 600});   // level 8, surface 36.25
    map.streets.push_back({0U, 0, 1, 0, 1, 1, "Slope"});

    const auto scene = map_editor::build_map_scene(map);

    const auto* start = find_city_tile(scene, 600, 600, map_editor::CityTileKind::Road);
    const auto* middle = find_city_tile(scene, 630, 600, map_editor::CityTileKind::Road);
    const auto* end = find_city_tile(scene, 659, 600, map_editor::CityTileKind::Road);
    assert(start != nullptr);
    assert(middle != nullptr);
    assert(end != nullptr);

    // Solid box bottoms: surface - 4.
    assert(close_to(start->base_y, 0.25F));
    assert(close_to(end->base_y, 32.25F));
    assert(middle->base_y > start->base_y && middle->base_y < end->base_y);

    // segLen=60, |delta|=8 -> runLen=7, slope region [2, 58): grade spans the
    // 32-unit climb over 56 cells. End cells outside the region stay flat.
    const auto expected_rise_per_cell = 32.0F / 56.0F;
    assert(close_to(middle->slope_x, expected_rise_per_cell));
    assert(close_to(middle->slope_z, 0.0F));
    assert(close_to(start->slope_x, 0.0F));

    // Native cell payload: flat lanes are solid cells whose top face (slot 5)
    // carries the lane marking code; sloped lanes are aa14 grade blocks with the
    // code in slot 0 too (the grade quad's material slot).
    assert(start->block == 0x40000000U);
    assert(start->faces[5] == 0x4101U);
    assert((middle->block & 0x80000000U) != 0U);
    assert(middle->faces[0] == 0x4101U && middle->faces[5] == 0x4101U);
    const auto grades = map_editor::native_grade_slot_table();
    assert(std::ranges::find(grades, static_cast<std::int32_t>(middle->block & 0xffffU)) != grades.end());
    const auto* left_lane = find_city_tile(scene, 600, 599, map_editor::CityTileKind::Road);
    assert(left_lane != nullptr && left_lane->faces[5] == 0x4201U);

    // Curb plates ride one level above the lanes.
    assert(has_city_tile(scene, 600, 1, 598, map_editor::CityTileKind::Curb));
    assert(has_city_tile(scene, 600, 1, 601, map_editor::CityTileKind::Curb));

    // The grade reaches the geometry: the road buffer spans the full climb.
    auto min_top = std::numeric_limits<float>::max();
    auto max_top = std::numeric_limits<float>::lowest();
    for (const auto& vertex : scene.city_tile_buffer.vertices) {
        min_top = std::min(min_top, vertex.position.y);
        max_top = std::max(max_top, vertex.position.y);
    }
    assert(max_top - min_top > 32.0F * 0.9F);
}

void city_road_scene_positions_use_native_game_surface_height() {
    map_editor::SbcMap map{};
    map.intersections.push_back({0U, 600, 7, 600});
    map.intersections.push_back({1U, 620, 9, 600});
    map.streets.push_back({0U, 0, 1, 0, 1, 1, "Raised"});

    const auto scene = map_editor::build_map_scene(map);

    assert(scene.intersections.size() == 2U);
    assert(scene.roads.size() == 1U);
    assert(close_to(scene.intersections.front().position.y, 28.25F));
    assert(close_to(scene.roads.front().from.y, 28.25F));
    assert(close_to(scene.roads.front().to.y, 36.25F));
}

void city_editor_guides_use_native_expanded_extents() {
    map_editor::SbcMap map{};
    map.intersections.push_back({0U, 10, 5, 10});
    map.intersections.push_back({1U, 20, 5, 10});
    map.intersections.push_back({2U, 10, 5, 20});
    map.intersections.push_back({3U, 0, 5, 10});
    map.intersections.push_back({4U, 10, 5, 0});
    map.streets.push_back({0U, 0, 1, 0, 1, 2, "E"});
    map.streets.push_back({1U, 0, 2, 0, 1, 2, "S"});
    map.streets.push_back({2U, 3, 0, 0, 1, 2, "W"});
    map.streets.push_back({3U, 4, 0, 0, 1, 2, "N"});

    const auto scene = map_editor::build_map_scene(map);
    const auto& node = scene.intersections[0].editor_quad;
    assert(close_to(node[0].x, 28.0F) && close_to(node[0].y, 20.0F) && close_to(node[0].z, 52.0F));
    assert(close_to(node[1].x, 48.0F) && close_to(node[1].z, 52.0F));
    assert(close_to(node[2].x, 48.0F) && close_to(node[2].z, 32.0F));
    assert(close_to(node[3].x, 28.0F) && close_to(node[3].z, 32.0F));

    const auto& east = scene.roads[0].editor_quad;
    assert(close_to(east[0].x, 48.0F) && close_to(east[0].y, 20.0F) && close_to(east[0].z, 32.0F));
    assert(close_to(east[1].x, 48.0F) && close_to(east[1].z, 52.0F));
    assert(close_to(east[2].x, 80.0F) && close_to(east[2].z, 52.0F));
    assert(close_to(east[3].x, 80.0F) && close_to(east[3].z, 32.0F));
}

void picks_city_intersection_marker_from_ray() {
    map_editor::SbcMap map{};
    map.intersections.push_back({0U, 4, 3, 8});
    const auto scene = map_editor::build_map_scene(map);
    const auto marker_y = static_cast<float>(map.intersections.front().y) * scene.unit_size;

    const auto picked = map_editor::pick_city_intersection(
        map,
        scene,
        {scene.intersections.front().position.x, marker_y + 10.0F, scene.intersections.front().position.z},
        {0.0F, -1.0F, 0.0F});

    assert(picked.has_value());
    assert(*picked == 0U);
}

void traffic_light_poles_stand_at_native_corners_of_3way_plus_intersections() {
    // Native corner objects only appear when more than two streets connect
    // (MapConnector_PlaceIntersectionPerimeterTiles @20130).
    map_editor::SbcMap two_way{};
    two_way.intersections.push_back({0U, 600, 5, 600});
    two_way.intersections.push_back({1U, 632, 5, 600});
    two_way.streets.push_back({0U, 0, 1, 0, 1, 1, "East"});
    assert(map_editor::build_map_scene(two_way).traffic_light_poles.empty());

    map_editor::SbcMap cross{};
    cross.intersections.push_back({0U, 600, 5, 600});  // 4-way center
    cross.intersections.push_back({1U, 632, 5, 600});
    cross.intersections.push_back({2U, 600, 5, 632});
    cross.intersections.push_back({3U, 568, 5, 600});
    cross.intersections.push_back({4U, 600, 5, 568});
    cross.streets.push_back({0U, 0, 1, 0, 1, 1, "E"});
    cross.streets.push_back({1U, 0, 2, 0, 1, 1, "S"});
    cross.streets.push_back({2U, 3, 0, 0, 1, 1, "W"});
    cross.streets.push_back({3U, 4, 0, 0, 1, 1, "N"});

    const auto scene = map_editor::build_map_scene(cross);
    assert(scene.traffic_light_poles.size() == 4U);
    // One pole per present neighbor: post at the corner cell center shifted one
    // unit toward the street its arm crosses (the native type-1 local offset
    // (0, 3.25, -1) rotated per corner), arm axis-aligned over the lanes.
    assert(std::ranges::any_of(scene.traffic_light_poles, [](const map_editor::CityTrafficLightPole& pole) {
        return close_to(pole.base.x, 2406.0F) && close_to(pole.base.z, 2405.0F) &&
            close_to(pole.arm_direction.x, 0.0F) && pole.arm_direction.z < -0.5F;
    }));
    assert(std::ranges::any_of(scene.traffic_light_poles, [](const map_editor::CityTrafficLightPole& pole) {
        return close_to(pole.base.x, 2394.0F) && close_to(pole.base.z, 2395.0F) &&
            close_to(pole.arm_direction.x, 0.0F) && pole.arm_direction.z > 0.5F;
    }));
    assert(std::ranges::any_of(scene.traffic_light_poles, [](const map_editor::CityTrafficLightPole& pole) {
        return close_to(pole.base.x, 2395.0F) && close_to(pole.base.z, 2406.0F) &&
            pole.arm_direction.x > 0.5F && close_to(pole.arm_direction.z, 0.0F);
    }));
    assert(std::ranges::any_of(scene.traffic_light_poles, [](const map_editor::CityTrafficLightPole& pole) {
        return close_to(pole.base.x, 2405.0F) && close_to(pole.base.z, 2394.0F) &&
            pole.arm_direction.x < -0.5F && close_to(pole.arm_direction.z, 0.0F);
    }));
    // Signs name the crossing street: streets along Z at gx 600 are the 7th
    // avenue ((600-256)/56 = 6, Garriott Ave); streets along X at gz 600 are past
    // Ninth Street and keep index 0 (First Street).
    for (const auto& pole : scene.traffic_light_poles) {
        const auto expected = std::abs(pole.arm_direction.z) > 0.5F ? 15 : 0;
        assert(pole.sign_name == expected);
        (void)expected;
    }
    assert(std::string_view{map_editor::native_street_names[15]} == "Garriott Ave");
}

void parses_buildblock_sbl_fixture() {
    const auto template_file = map_editor::parse_sbl_file(
        fixture_root() / "server/data/test2/buildblock/test.sbl");

    assert(template_file.version == 1);
    assert(!template_file.block.has_value());
    assert(template_file.layers.size() == 16);
    assert(template_file.layers[1].door_count == 0);
    assert(template_file.layers[1].window_count == 2);
    assert(template_file.layers[1].beam_count == 3);
}

void builds_buildblock_preview_scene() {
    const auto template_file = map_editor::parse_sbl_file(
        fixture_root() / "server/data/test2/buildblock/test.sbl");

    const auto scene = map_editor::build_buildblock_preview_scene(template_file);

    assert(scene.rects.size() >= 1U + template_file.layers[1].windows.size() + template_file.layers[1].beams.size());
    const auto window = std::ranges::find_if(scene.rects, [](const map_editor::BuildblockPreviewRect& rect) {
        return rect.layer_index == 1 && rect.kind == map_editor::BuildblockPreviewRectKind::Window;
    });
    assert(window != scene.rects.end());
    assert(window->size.x > 0.0F);
    assert(window->size.y > 0.0F);
}

void city_block_preview_treats_unmatched_record_name_as_solid_not_layer_meshes() {
    const auto block = map_editor::parse_sbl_file(
        fixture_root() / "server/data/test2/block/astairwellfloor.sbl");
    assert(block.block.has_value());

    map_editor::SbcCityBlock city_block{};
    city_block.name = "piece_test";
    city_block.layers.push_back({
        "astairwellfloor",
        "",
        "",
        "",
        "",
        "",
        "",
        "astairwellfloor",
    });

    const auto scene = map_editor::build_city_block_preview_scene(
        city_block,
        {map_editor::NamedBlockMesh{"astairwellfloor", *block.block}});

    assert(scene.pieces.empty());
    assert(scene.solid_cell.has_value());
    assert(scene.solid_cell->name == "piece_test");
    assert(scene.solid_cell->buffer.triangles.size() == 10U);
}

void city_block_preview_prefers_native_record_name_mesh() {
    const auto block = map_editor::parse_sbl_file(
        fixture_root() / "server/data/test2/block/astairwellfloor.sbl");
    assert(block.block.has_value());

    map_editor::SbcCityBlock city_block{};
    city_block.name = "astairwellfloor";
    city_block.layers.push_back({
        "",
        "",
        "",
        "",
        "",
        "",
        "",
        "",
    });

    const auto scene = map_editor::build_city_block_preview_scene(
        city_block,
        {map_editor::NamedBlockMesh{"astairwellfloor", *block.block}});

    assert(scene.pieces.size() == 1);
    assert(scene.pieces[0].name == "astairwellfloor");
    assert(scene.pieces[0].layer_index == 0);
    assert(scene.pieces[0].slot_index == 0);
}

void city_block_preview_renders_empty_name_as_nblock_solid_cell() {
    map_editor::SbcCityBlock city_block{};
    city_block.index = 0;

    const auto scene = map_editor::build_city_block_preview_scene(city_block, {});

    assert(scene.pieces.empty());
    assert(scene.solid_cell.has_value());
    assert(scene.solid_cell->name == "nblock");
    assert(scene.solid_cell->buffer.triangles.size() == 10U);
    assert(has_buffer_vertex(scene.solid_cell->buffer, 0.0F, 4.0F, 4.0F, 0.0F, 1.0F));
}

void city_block_preview_renders_unmatched_name_as_solid_cell() {
    map_editor::SbcCityBlock city_block{};
    city_block.index = 3;
    city_block.name = "missing_template";

    const auto scene = map_editor::build_city_block_preview_scene(city_block, {});

    assert(scene.pieces.empty());
    assert(scene.solid_cell.has_value());
    assert(scene.solid_cell->name == "missing_template");
    assert(scene.solid_cell->buffer.triangles.size() == 10U);
}

void builds_portal_wall_rects_from_sbl_layer() {
    const auto template_file = map_editor::parse_sbl_file(
        fixture_root() / "server/data/test2/buildblock/test.sbl");

    const auto rects = map_editor::build_portal_wall_rects(template_file.layers[1], map_editor::PortalSide::North);

    assert(rects.size() == 4);
    assert(close_to(rects[0].x, 0.0F));
    assert(close_to(rects[0].y, 0.0F));
    assert(close_to(rects[0].width, 1.0F));
    assert(close_to(rects[0].height, 0.0625F));
    assert(close_to(rects[1].y, 0.859375F));
    assert(close_to(rects[2].width, 0.0625F));
    assert(close_to(rects[3].x, 0.9375F));
}

void builds_native_portal_overlay_rect_from_sbl_layer() {
    map_editor::SblLayer layer{};
    layer.bounds = {0.25F, 0.5F, 0.125F, 0.75F};
    layer.windows.push_back({0U, 0.375F, 0.25F, 0.4F, 0.2F});
    layer.beams.push_back({0U, 0.1F, 0.8F, 0.7F, 0.08F});

    const auto rects = map_editor::build_portal_overlay_rects(layer, map_editor::PortalSide::North);

    assert(rects.size() == 1);
    assert(close_to(rects[0].x, 0.25F));
    assert(close_to(rects[0].width, 0.5F));
    assert(close_to(rects[0].y, 0.125F));
    assert(close_to(rects[0].height, 0.75F));
}

void parses_block_sbl_fixture() {
    const auto template_file = map_editor::parse_sbl_file(
        fixture_root() / "server/data/test2/block/astairwellfloor.sbl");

    assert(template_file.version == 2);
    assert(template_file.block.has_value());
    assert(template_file.block->width > 0);
    assert(template_file.block->height > 0);
    assert(template_file.block->depth > 0);
    assert(template_file.block->exterior_flags.size() == 6);
    assert(template_file.block->face_count > 0);
    assert(template_file.layers.empty());
}

void unsupported_sbl_version_is_not_block_mesh() {
    std::vector<std::uint8_t> bytes;
    append_u32(bytes, 3);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    for (std::size_t index = 0; index < 7; ++index) {
        append_u32(bytes, 0);
    }

    const auto template_file = map_editor::parse_sbl(bytes);

    assert(template_file.version == 3);
    assert(!template_file.block.has_value());
}

void parses_sbl_block_face_vertices() {
    const auto template_file = map_editor::parse_sbl_file(
        fixture_root() / "server/data/test2/block/garagedoor.sbl");

    assert(template_file.block.has_value());
    assert(template_file.block->width == 2);
    assert(template_file.block->height == 1);
    assert(template_file.block->depth == 1);
    assert(template_file.block->faces.size() >= 2);
    const auto& vertices = template_file.block->faces[0].vertices;
    assert(vertices.size() == 4);
    assert(close_to(vertices[0].x, 0.0F));
    assert(close_to(vertices[0].y, 0.0625F));
    assert(close_to(vertices[0].z, 0.0F));
    assert(close_to(vertices[1].x, 1.0F));
    assert(close_to(vertices[2].z, 1.0F));
}

void native_sbl_default_faces_keep_material_slots_and_oriented_uvs() {
    std::vector<std::uint8_t> bytes;
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 1);
    append_u32(bytes, 0);
    append_u32(bytes, 0);

    const auto template_file = map_editor::parse_sbl(bytes);

    assert(template_file.block.has_value());
    assert(template_file.block->faces.size() == 1U);
    const auto& face = template_file.block->faces.front();
    assert(face.material == 1U);
    assert(face.orientation == 0x60000U);
    assert(close_to(face.uvs[0].u, face.vertices[0].x));
    assert(close_to(face.uvs[0].v, face.vertices[0].y));
}

void parses_authored_sbl_patch_vertices() {
    const auto template_file = map_editor::parse_sbl_file(
        fixture_root() / "server/data/test2/block/garagerampcircular.sbl");

    assert(template_file.block.has_value());
    const auto authored = std::ranges::find_if(template_file.block->faces, [](const map_editor::SblBlockFace& face) {
        return face.authored;
    });
    assert(authored != template_file.block->faces.end());
    assert(authored->vertices.size() == 4);
    assert(!template_file.block->authored_patches.empty());
    assert(template_file.block->authored_patches[0].control_points.size() == 16);
    assert(template_file.block->authored_patches[0].offset == 44);
}

void applies_native_sbl_authored_uv_orientation_flags() {
    std::vector<std::uint8_t> bytes;
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    for (std::size_t index = 0; index < 6; ++index) {
        append_u32(bytes, 0);
    }
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 0x10002);
    append_u32(bytes, 0);
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            append_f32(bytes, static_cast<float>(column) / 3.0F);
            append_f32(bytes, 0.75F);
            append_f32(bytes, 0.25F);
            append_f32(bytes, 0.0F);
            append_f32(bytes, 0.0F);
        }
    }

    const auto template_file = map_editor::parse_sbl(bytes);
    assert(template_file.block.has_value());
    const auto authored = std::ranges::find_if(template_file.block->faces, [](const map_editor::SblBlockFace& face) {
        return face.authored;
    });

    assert(authored != template_file.block->faces.end());
    assert(authored->material == 2);
    assert(close_to(authored->uvs[0].u, 0.25F));
    assert(close_to(authored->uvs[0].v, 0.75F));
}

void applies_native_sbl_authored_auto_orientation_flag() {
    std::vector<std::uint8_t> bytes;
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    for (std::size_t index = 0; index < 6; ++index) {
        append_u32(bytes, 0);
    }
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 2);
    append_u32(bytes, 1);
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            append_f32(bytes, static_cast<float>(column) / 3.0F);
            append_f32(bytes, static_cast<float>(row) / 3.0F);
            append_f32(bytes, 0.25F);
            append_f32(bytes, 0.9F);
            append_f32(bytes, 0.8F);
        }
    }

    const auto template_file = map_editor::parse_sbl(bytes);
    assert(template_file.block.has_value());
    const auto authored = std::ranges::find_if(template_file.block->faces, [](const map_editor::SblBlockFace& face) {
        return face.authored;
    });

    assert(authored != template_file.block->faces.end());
    assert(authored->material == 2);
    assert(authored->orientation == 0x50000U);
    assert(close_to(authored->uvs[0].u, 1.0F - authored->vertices[0].x));
    assert(close_to(authored->uvs[0].v, authored->vertices[0].y));
}

void native_building_render_buffer_rotates_piece_material_slots_and_uvs() {
    map_editor::SblBlockMesh mesh{};
    mesh.width = 1;
    mesh.height = 1;
    mesh.depth = 1;
    mesh.faces.push_back({
        1U,
        {
            {0.0F, 1.0F, 0.0625F},
            {1.0F, 1.0F, 0.0625F},
            {1.0F, 0.0F, 0.0625F},
            {0.0F, 0.0F, 0.0625F},
        },
        {
            {0.0F, 1.0F},
            {1.0F, 1.0F},
            {1.0F, 0.0F},
            {0.0F, 0.0F},
        },
        false,
        0x60000U,
    });

    map_editor::SbbBuilding building{};
    building.extent_x = 1;
    building.extent_y = 1;
    building.extent_z = 1;
    building.piece_names = {"piece"};
    map_editor::SbbCell cell{
        0,
        0,
        0,
        0,
        {0x80000000U | (1U << 24U), 0U, 0U, 0U, 0U, 0U},
        0,
        true,
        {map_editor::SbbCellPayload{map_editor::PayloadKind::Piece, 0x80000000U | (1U << 24U), 0U}},
    };
    cell.shorts[2] = 77U;
    building.cells.push_back(cell);

    const auto buffer = map_editor::build_native_building_render_buffer(
        building,
        {map_editor::NamedBlockMesh{"piece", mesh}},
        {});
    const auto run = std::ranges::find_if(buffer.material_runs, [](const map_editor::RenderBufferMaterialRun& material_run) {
        return material_run.material == 77U;
    });

    assert(run != buffer.material_runs.end());
    assert(run->triangle_count == 2U);
    assert(std::ranges::any_of(buffer.vertices, [](const map_editor::RenderBufferVertex& vertex) {
        return close_to(vertex.u, vertex.position.z / 4.0F);
    }));
}

void native_building_render_buffer_uses_generated_builtin_block_for_unresolved_piece_refs() {
    map_editor::SblBlockMesh fallback_mesh{};
    fallback_mesh.width = 1;
    fallback_mesh.height = 1;
    fallback_mesh.depth = 1;
    fallback_mesh.faces.push_back({
        1U,
        {
            {0.0F, 1.0F, 0.0F},
            {1.0F, 1.0F, 0.0F},
            {1.0F, 0.0F, 0.0F},
            {0.0F, 0.0F, 0.0F},
        },
        {
            {0.0F, 1.0F},
            {1.0F, 1.0F},
            {1.0F, 0.0F},
            {0.0F, 0.0F},
        },
        false,
        0x60000U,
    });

    map_editor::SbbBuilding building{};
    building.extent_x = 1;
    building.extent_y = 1;
    building.extent_z = 1;
    building.piece_names = {"missing"};
    map_editor::SbbCell cell{
        0,
        0,
        0,
        0,
        {0x80000000U, 0U, 0U, 0U, 0U, 0U},
        0,
        true,
        {map_editor::SbbCellPayload{map_editor::PayloadKind::Piece, 0x80000000U, 0U}},
    };
    cell.shorts[0] = 44U;
    cell.shorts[1] = 33U;
    building.cells.push_back(cell);

    const auto buffer = map_editor::build_native_building_render_buffer(
        building,
        {map_editor::NamedBlockMesh{"fallback", fallback_mesh}},
        {});

    assert(buffer.triangles.size() == 2U);
    assert(buffer.material_runs.size() == 1U);
    assert(buffer.material_runs[0].material == 44U);
    assert(all_buffer_positions_on_y(buffer, 0.25F));
}

void native_block_mesh_catalog_seeds_builtin_blocks_before_file_blocks() {
    map_editor::SblBlockMesh file_mesh{};
    file_mesh.width = 2U;
    file_mesh.height = 3U;
    file_mesh.depth = 4U;

    const auto catalog = map_editor::build_native_block_mesh_catalog({map_editor::NamedBlockMesh{"file_block", file_mesh}});

    assert(catalog.size() == map_editor::native_builtin_block_mesh_count + 1U);
    for (std::size_t index = 0; index < map_editor::native_builtin_block_mesh_count; ++index) {
        assert(catalog[index].name.empty());
        assert(catalog[index].mesh.width == 1U);
        assert(catalog[index].mesh.height == 1U);
        assert(catalog[index].mesh.depth == 1U);
        assert(catalog[index].mesh.faces.size() == 1U);
        assert(close_to(catalog[index].mesh.faces[0].vertices[0].y, 0.0625F));
    }
    assert(catalog[map_editor::native_builtin_block_mesh_count].name == "file_block");
    assert(catalog[map_editor::native_builtin_block_mesh_count].mesh.width == 2U);
}

void native_block_rotation_uses_positive_rotated_bounds() {
    map_editor::SblBlockMesh mesh{};
    mesh.width = 2;
    mesh.height = 1;
    mesh.depth = 1;
    map_editor::SblBlockFace face{
        1U,
        {
            {0.0F, 0.0F, 0.0F},
            {2.0F, 0.0F, 0.0F},
            {2.0F, 1.0F, 0.0F},
            {0.0F, 1.0F, 0.0F},
        },
        {},
        false,
        0x60000U,
    };

    const auto rotated = map_editor::rotate_block_face_native(mesh, face, 1);

    assert(close_to(rotated.vertices[0].x, 1.0F));
    assert(close_to(rotated.vertices[0].z, 0.0F));
    assert(close_to(rotated.vertices[1].x, 1.0F));
    assert(close_to(rotated.vertices[1].z, 2.0F));
    assert(rotated.material == 2U);

    const auto rotated_twice = map_editor::rotate_block_face_native(mesh, face, 2);
    assert(close_to(rotated_twice.vertices[0].x, 2.0F));
    assert(close_to(rotated_twice.vertices[0].z, 1.0F));
    assert(close_to(rotated_twice.vertices[1].x, 0.0F));
    assert(close_to(rotated_twice.vertices[1].z, 1.0F));
    assert(rotated_twice.material == 3U);

    const auto rotated_thrice = map_editor::rotate_block_face_native(mesh, face, 3);
    assert(close_to(rotated_thrice.vertices[0].x, 0.0F));
    assert(close_to(rotated_thrice.vertices[0].z, 2.0F));
    assert(close_to(rotated_thrice.vertices[1].x, 0.0F));
    assert(close_to(rotated_thrice.vertices[1].z, 0.0F));
    assert(rotated_thrice.material == 4U);
}

void native_block_mesh_rotation_only_cycles_side_material_slots() {
    map_editor::SblBlockMesh mesh{};
    mesh.width = 1;
    mesh.height = 1;
    mesh.depth = 1;
    map_editor::SblBlockFace face{
        9U,
        {
            {0.0F, 0.0F, 0.0F},
            {1.0F, 0.0F, 0.0F},
            {1.0F, 1.0F, 0.0F},
            {0.0F, 1.0F, 0.0F},
        },
        {},
        false,
        0x60000U,
    };

    const auto rotated_overlay = map_editor::rotate_block_face_native(mesh, face, 1);
    assert(rotated_overlay.material == 9U);

    face.material = 1U;
    const auto rotated_side = map_editor::rotate_block_face_native(mesh, face, 1);
    assert(rotated_side.material == 2U);
}

void tessellates_authored_sbl_patches() {
    const auto template_file = map_editor::parse_sbl_file(
        fixture_root() / "server/data/test2/block/garagerampcircular.sbl");

    assert(template_file.block.has_value());
    assert(template_file.block->faces.size() > 9);
}

void emits_sbl_v2_light_record_faces() {
    std::vector<std::uint8_t> bytes;
    append_u32(bytes, 2);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    append_u32(bytes, 1);
    for (int index = 0; index < 6; ++index) {
        append_u32(bytes, 0);
    }
    append_u32(bytes, 0);
    append_u32(bytes, 1);

    const std::array<std::array<float, 3>, 8> points{{
        {{0.0F, 0.0F, 0.0F}},
        {{1.0F, 0.0F, 0.0F}},
        {{1.0F, 1.0F, 0.0F}},
        {{0.0F, 1.0F, 0.0F}},
        {{0.0F, 0.0F, 1.0F}},
        {{1.0F, 0.0F, 1.0F}},
        {{1.0F, 1.0F, 1.0F}},
        {{0.0F, 1.0F, 1.0F}},
    }};
    for (const auto& point : points) {
        append_f32(bytes, point[0]);
        append_f32(bytes, point[1]);
        append_f32(bytes, point[2]);
    }
    for (std::uint32_t index = 0; index < 6; ++index) {
        append_u32(bytes, 10U + index);
    }
    append_u32(bytes, 0x3f);

    const auto template_file = map_editor::parse_sbl(bytes);
    assert(template_file.block.has_value());
    assert(template_file.block->light_count == 1);
    assert(template_file.block->lights.size() == 1);
    assert(template_file.block->lights[0].scalars[0] == 10);
    assert(template_file.block->lights[0].scalars[5] == 15);
    assert(template_file.block->lights[0].final_value == 0x3f);
    assert(template_file.block->faces.size() == 6);
    assert(close_to(template_file.block->faces[0].vertices[0].x, 0.0F));
    assert(close_to(template_file.block->faces[0].vertices[0].y, 1.0F));
    // Native LoadAndInitBlockTemplates_SBL derives each light-face UV
    // orientation from that face's triangle normal. For the cube fixture the
    // first face winds toward -Z and the last face toward +Z.
    assert(template_file.block->faces[0].orientation == 0x50000U);
    assert(template_file.block->faces[5].orientation == 0x60000U);
}

void patches_sbl_authored_patch_control_point() {
    const auto bytes = map_editor::read_file_bytes(fixture_root() / "server/data/test2/block/garagerampcircular.sbl");
    auto template_file = map_editor::parse_sbl(bytes);
    assert(template_file.block.has_value());
    assert(!template_file.block->authored_patches.empty());
    auto& patch = template_file.block->authored_patches[0];
    const auto original_x = patch.control_points[0].position.x;
    const auto original_u = patch.control_points[0].u;

    patch.control_points[0].position.x = original_x + 0.25F;
    patch.control_points[0].u = original_u + 0.125F;

    const auto patched = map_editor::patch_sbl_block_patches(bytes, *template_file.block);
    const auto reparsed = map_editor::parse_sbl(patched);

    assert(reparsed.block.has_value());
    assert(close_to(reparsed.block->authored_patches[0].control_points[0].position.x, original_x + 0.25F));
    assert(close_to(reparsed.block->authored_patches[0].control_points[0].u, original_u + 0.125F));
}

void refreshes_sbl_authored_patch_render_faces() {
    auto template_file = map_editor::parse_sbl_file(
        fixture_root() / "server/data/test2/block/garagerampcircular.sbl");
    assert(template_file.block.has_value());
    assert(!template_file.block->authored_patches.empty());

    const auto original_authored = std::ranges::find_if(template_file.block->faces, [](const map_editor::SblBlockFace& face) {
        return face.authored;
    });
    assert(original_authored != template_file.block->faces.end());
    const auto original_x = original_authored->vertices[0].x;

    template_file.block->authored_patches[0].control_points[0].position.x += 0.5F;
    map_editor::refresh_sbl_block_faces(*template_file.block);

    const auto refreshed_authored = std::ranges::find_if(template_file.block->faces, [](const map_editor::SblBlockFace& face) {
        return face.authored;
    });
    assert(refreshed_authored != template_file.block->faces.end());
    assert(close_to(refreshed_authored->vertices[0].x, original_x + 0.5F));
}

void parses_csx_fixture() {
    const auto archive = map_editor::parse_csx_file(
        fixture_root() / "server/data/test2/test.csx");

    assert(archive.entry_count > 100);
    assert(archive.entries.size() == archive.entry_count);
    assert(archive.entries[0].name == "apartment01");
    assert(archive.entries[0].type == 0x585a0002);
    // 0x585a0002 is a building .sbb (verified: parse_sbb succeeds on its bytes once
    // the container sentinel version is normalized 1 -> 14).
    assert(archive.entries[0].kind == map_editor::CsxEntryKind::Sbb);

    bool has_sbl = false;
    bool has_sbb = false;
    bool has_texture = false;
    for (const auto& entry : archive.entries) {
        has_sbl = has_sbl || entry.kind == map_editor::CsxEntryKind::Sbl;
        has_sbb = has_sbb || entry.kind == map_editor::CsxEntryKind::Sbb;
        has_texture = has_texture || entry.kind == map_editor::CsxEntryKind::Texture;
    }
    assert(has_sbl);
    assert(has_sbb);
    assert(has_texture);
}

void patches_sbb_cell_words() {
    const auto bytes = map_editor::read_file_bytes(fixture_root() / "server/data/test2/building/test.sbb");
    const auto building = map_editor::parse_sbb(bytes);
    auto cell = building.cells[0];
    cell.exterior_block() = 0x40000000;
    cell.interior_block() = 0xffff;
    cell.cell_type() = 7;
    cell.exterior_portal_face() = 8;
    cell.interior_portal_face() = 9;
    cell.placed_flag() = 10;
    cell.shorts[0] = 123;
    cell.shorts[15] = 456;
    cell.item_set_index = 11;

    const auto patched = map_editor::patch_sbb_cell(bytes, building, cell);
    const auto reparsed = map_editor::parse_sbb(patched);

    assert(reparsed.cells[0].exterior_block() == 0x40000000);
    assert(reparsed.cells[0].interior_block() == 0xffff);
    assert(reparsed.cells[0].cell_type() == 7);
    assert(reparsed.cells[0].exterior_portal_face() == 8);
    assert(reparsed.cells[0].interior_portal_face() == 9);
    assert(reparsed.cells[0].placed_flag() == 10);
    assert(reparsed.cells[0].shorts[0] == 123);
    assert(reparsed.cells[0].shorts[15] == 456);
    assert(reparsed.cells[0].item_set_index == 11);
}

void patches_sbb_cells() {
    const auto bytes = map_editor::read_file_bytes(fixture_root() / "server/data/test2/building/test.sbb");
    auto building = map_editor::parse_sbb(bytes);
    assert(building.cells.size() > 1);

    building.cells[0].exterior_portal_face() = 12;
    building.cells[0].item_set_index = 2;
    building.cells[1].interior_portal_face() = 34;
    building.cells[1].placed_flag() = 56;

    const auto patched = map_editor::patch_sbb_cells(bytes, building);
    const auto reparsed = map_editor::parse_sbb(patched);

    assert(reparsed.cells[0].exterior_portal_face() == 12);
    assert(reparsed.cells[0].item_set_index == 2);
    assert(reparsed.cells[1].interior_portal_face() == 34);
    assert(reparsed.cells[1].placed_flag() == 56);
}

void rebuilds_sbb_as_native_v14_with_lod_tail() {
    const auto bytes = map_editor::read_file_bytes(fixture_root() / "server/data/test2/building/test.sbb");
    auto building = map_editor::parse_sbb(bytes);
    building.name = "rebuilt_cpp";
    building.lod_tail = {1, 0, 0, 0, 0, 0, 0, 0};

    const auto rebuilt = map_editor::rebuild_sbb(building);
    const auto reparsed = map_editor::parse_sbb(rebuilt);

    assert(reparsed.version == 14);
    assert(reparsed.name == "rebuilt_cpp");
    assert(reparsed.materials == building.materials);
    assert(reparsed.piece_names == building.piece_names);
    assert(reparsed.block_names == building.block_names);
    assert(reparsed.item_set_names == building.item_set_names);
    assert(reparsed.cells.size() == building.cells.size());
    assert(reparsed.lod_marker == 1);
    assert(reparsed.lod_group_count == 0);
    assert(reparsed.lod_tail == building.lod_tail);
}

void rebuilds_sbb_grows_native_dimensions_for_occupied_cells() {
    auto building = map_editor::parse_sbb_file(fixture_root() / "server/data/test2/building/test.sbb");
    auto cell = building.cells[0];
    cell.index = building.cells.size();
    cell.x = building.size_x + 2U;
    cell.y = building.size_z + 1U;
    cell.z = building.size_y + 3U;
    cell.exterior_block() = 0x40000000;
    cell.interior_block() = 0;
    building.cells.push_back(cell);

    const auto rebuilt = map_editor::rebuild_sbb(building);
    const auto reparsed = map_editor::parse_sbb(rebuilt);

    // X/Z sizes are the highest index (grid spans 0..size); Y is the floor count.
    assert(reparsed.size_x == cell.x);
    assert(reparsed.size_y == cell.z);
    assert(reparsed.size_z == cell.y + 1U);
    const auto match = std::ranges::find_if(reparsed.cells, [&cell](const map_editor::SbbCell& reparsed_cell) {
        return reparsed_cell.x == cell.x && reparsed_cell.y == cell.y && reparsed_cell.z == cell.z;
    });
    assert(match != reparsed.cells.end());
    assert(match->exterior_block() == 0x40000000);
}

void rebuilds_sbb_keeps_native_dimensions() {
    // burger has blocks on its last X/Z index; saving it unchanged must not
    // grow the grid (it used to become 13x13).
    const auto building = map_editor::parse_sbb_file(fixture_root() / "server/data/test2/building/burger.sbb");
    const auto reparsed = map_editor::parse_sbb(map_editor::rebuild_sbb(building));
    if (reparsed.size_x != building.size_x || reparsed.size_y != building.size_y ||
        reparsed.size_z != building.size_z || reparsed.cells.size() != building.cells.size()) {
        std::fprintf(stderr, "burger rebuilt as %ux%ux%u (%zu cells), expected %ux%ux%u (%zu cells)\n",
            reparsed.size_x, reparsed.size_y, reparsed.size_z, reparsed.cells.size(),
            building.size_x, building.size_y, building.size_z, building.cells.size());
        std::abort();
    }
}

void patches_sbc_building_record() {
    const auto bytes = map_editor::read_file_bytes(fixture_root() / "server/data/test2/city2.sbc");
    const auto map = map_editor::parse_sbc(bytes);
    auto building = map.buildings[0];
    building.name = "bank_cpp";
    building.x += 12;
    building.y += 1;
    building.z += 24;
    building.rotation = 2;

    const auto patched = map_editor::patch_sbc_building(bytes, building, map.version);
    const auto reparsed = map_editor::parse_sbc(patched);

    assert(reparsed.buildings[0].name == "bank_cpp");
    assert(reparsed.buildings[0].x == map.buildings[0].x + 12);
    assert(reparsed.buildings[0].y == map.buildings[0].y + 1);
    assert(reparsed.buildings[0].z == map.buildings[0].z + 24);
    assert(reparsed.buildings[0].rotation == 2);
}

void patches_sbc_building_records() {
    const auto bytes = map_editor::read_file_bytes(fixture_root() / "server/data/test2/city2.sbc");
    auto map = map_editor::parse_sbc(bytes);

    map.buildings[0].x += 3;
    map.buildings[0].rotation = 1;
    map.buildings[1].z += 7;
    map.buildings[1].rotation = 2;

    const auto patched = map_editor::patch_sbc_buildings(bytes, map);
    const auto reparsed = map_editor::parse_sbc(patched);

    assert(reparsed.buildings[0].x == map.buildings[0].x);
    assert(reparsed.buildings[0].rotation == 1);
    assert(reparsed.buildings[1].z == map.buildings[1].z);
    assert(reparsed.buildings[1].rotation == 2);
}

void patches_sbc_street_records() {
    const auto bytes = map_editor::read_file_bytes(fixture_root() / "server/data/test2/city2.sbc");
    auto map = map_editor::parse_sbc(bytes);
    assert(map.streets.size() > 1);

    map.streets[0].from_intersection = 2;
    map.streets[0].to_intersection = 3;
    map.streets[0].left_width += 1;
    map.streets[0].right_width += 2;
    map.streets[0].name = "RoadCpp";
    map.streets[1].district += 4;

    const auto patched = map_editor::patch_sbc_streets(bytes, map);
    const auto reparsed = map_editor::parse_sbc(patched);

    assert(reparsed.streets[0].from_intersection == 2);
    assert(reparsed.streets[0].to_intersection == 3);
    assert(reparsed.streets[0].left_width == map.streets[0].left_width);
    assert(reparsed.streets[0].right_width == map.streets[0].right_width);
    assert(reparsed.streets[0].name == "RoadCpp");
    assert(reparsed.streets[1].district == map.streets[1].district);
}

void patches_sbc_city_block_layers() {
    const auto bytes = map_editor::read_file_bytes(fixture_root() / "server/data/test2/city2.sbc");
    auto map = map_editor::parse_sbc(bytes);
    assert(!map.city_blocks.empty());
    assert(!map.city_blocks[0].layers.empty());

    map.city_blocks[0].name = "piece_cpp";
    map.city_blocks[0].layers[0][0] = "asphalt";
    map.city_blocks[0].layers[0][7] = "sidewalk";

    const auto patched = map_editor::patch_sbc_city_blocks(bytes, map);
    const auto reparsed = map_editor::parse_sbc(patched);

    assert(reparsed.city_blocks[0].name == "piece_cpp");
    assert(reparsed.city_blocks[0].layers[0][0] == "asphalt");
    assert(reparsed.city_blocks[0].layers[0][7] == "sidewalk");
}

void patches_sbc_portal_blocks() {
    const auto bytes = map_editor::read_file_bytes(fixture_root() / "server/data/test2/city2.sbc");
    auto map = map_editor::parse_sbc(bytes);
    assert(!map.portals.empty());
    assert(map.portals[0].blocks.size() == 512);

    map.portals[0].blocks[164] = 0x02010003U;
    map.portals[0].blocks[165] = 0;

    const auto patched = map_editor::patch_sbc_portals(bytes, map);
    const auto reparsed = map_editor::parse_sbc(patched);

    assert(reparsed.portals[0].blocks[164] == 0x02010003U);
    assert(reparsed.portals[0].blocks[165] == 0U);

    auto appended_map = map_editor::parse_sbc(bytes);
    auto portal = appended_map.portals[0];
    portal.index = appended_map.portals.size();
    portal.fields = {0, 77, 0, 88};
    portal.blocks.assign(0x200, 0);
    portal.blocks[0] = 0x01010001U;
    portal.itemsets.assign(0x200, 0);
    appended_map.portals.push_back(std::move(portal));

    const auto appended = map_editor::patch_sbc_portals(bytes, appended_map);
    const auto reparsed_appended = map_editor::parse_sbc(appended);

    assert(reparsed_appended.portals.size() == appended_map.portals.size());
    assert(reparsed_appended.portals.back().fields[1] == 77);
    assert(reparsed_appended.portals.back().fields[3] == 88);
    assert(reparsed_appended.portals.back().blocks[0] == 0x01010001U);
}

void native_portal_render_buffer_uses_city_block_name_mesh_and_layer_materials() {
    map_editor::SblBlockMesh mesh{};
    mesh.width = 1;
    mesh.height = 1;
    mesh.depth = 1;
    mesh.faces.push_back({
        1U,
        {
            {0.0F, 0.0F, 1.0F},
            {1.0F, 0.0F, 1.0F},
            {1.0F, 1.0F, 1.0F},
            {0.0F, 1.0F, 1.0F},
        },
        {
            {0.0F, 0.0F},
            {1.0F, 0.0F},
            {1.0F, 1.0F},
            {0.0F, 1.0F},
        },
        false,
        0x60000U,
    });

    map_editor::SbcMap map{};
    map.city_blocks.push_back({
        0,
        "portalmesh",
        {std::array<std::string, 8>{"down", "south", "west", "north", "east", "up", "extra0", "extra1"}},
    });
    map_editor::SbcPortal portal{};
    portal.fields = {0, 2, 3, 4};
    portal.blocks.assign(0x200, 0U);
    portal.blocks[0] = 0x00010000U;
    map.portals.push_back(std::move(portal));

    const auto buffer = map_editor::build_native_portal_render_buffer(
        map,
        {map_editor::NamedBlockMesh{"portalmesh", mesh}});

    assert(buffer.triangles.size() == 2U);
    assert(buffer.material_names.size() == 1U);
    assert(buffer.material_names[0] == "south");
    assert(has_buffer_vertex(buffer, 64.0F, 96.0F, 132.0F, 0.0F, 0.0F));
}

void native_portal_render_buffer_renders_unnamed_city_block_as_solid_cell_faces() {
    map_editor::SbcMap map{};
    map.city_blocks.push_back({
        0,
        "",
        {std::array<std::string, 8>{"down", "south", "west", "north", "east", "up", "extra0", "extra1"}},
    });
    map_editor::SbcPortal portal{};
    portal.fields = {0, 0, 0, 0};
    portal.blocks.assign(0x200, 0U);
    portal.blocks[0] = 0x01010000U;
    map.portals.push_back(std::move(portal));

    const auto buffer = map_editor::build_native_portal_render_buffer(map, {});

    assert(buffer.triangles.size() == 10U);
    assert(buffer.material_names.size() == 5U);
    assert(std::ranges::find(buffer.material_names, "south") != buffer.material_names.end());
    assert(std::ranges::find(buffer.material_names, "west") != buffer.material_names.end());
    assert(std::ranges::find(buffer.material_names, "north") != buffer.material_names.end());
    assert(std::ranges::find(buffer.material_names, "east") != buffer.material_names.end());
    assert(std::ranges::find(buffer.material_names, "up") != buffer.material_names.end());
    assert(has_buffer_vertex(buffer, 0.0F, 4.0F, 4.0F, 0.0F, 1.0F));
    for (const auto& triangle : buffer.triangles) {
        const auto& a = buffer.vertices[triangle.indices[0]].position;
        const auto& b = buffer.vertices[triangle.indices[1]].position;
        const auto& c = buffer.vertices[triangle.indices[2]].position;
        assert(!(close_to(a.y, 0.0F) && close_to(b.y, 0.0F) && close_to(c.y, 0.0F)));
    }
}

void native_portal_render_buffer_drops_v10_cells_under_buildings() {
    // World_LoadMapAndInitializeTerrain places v10 district cells first; a
    // building cell with an exterior block word then replaces the world cell.
    map_editor::SbbBuilding building{};
    building.cells.push_back({0, 0, 0, 0, {0x40000000U, 0U, 0U, 0U, 0U, 0U}, 0, true, {}});
    map_editor::SbcMap map{};
    map.buildings.push_back({0, "b", 20, 0, 20, 0});
    const auto world = map_editor::native_city_building_transform_point(map.buildings[0], {2.0F, 0.0F, 2.0F}, 4.0F);
    const auto cx = static_cast<std::int32_t>(std::floor(world.x / 4.0F));
    const auto cz = static_cast<std::int32_t>(std::floor(world.z / 4.0F));
    map_editor::SbcPortal portal{};
    portal.fields = {0, cx / 8, 0, cz / 8};
    portal.legacy_blocks.assign(0x200, 0U);
    portal.legacy_blocks[static_cast<std::size_t>((cx % 8) | ((cz % 8) << 3))] = 0x40000000U;
    map.portals.push_back(std::move(portal));

    assert(!map_editor::build_native_portal_render_buffer(map, {}).triangles.empty());
    const std::vector<const map_editor::SbbBuilding*> placements{&building};
    assert(map_editor::build_native_portal_render_buffer(map, {}, std::nullopt, {}, {}, placements).triangles.empty());
}

void native_portal_render_buffer_renders_unmatched_named_city_block_as_solid_cell_faces() {
    map_editor::SbcMap map{};
    map.city_blocks.push_back({
        0,
        "missing_template",
        {std::array<std::string, 8>{"down", "south", "west", "north", "east", "up", "extra0", "extra1"}},
    });
    map_editor::SbcPortal portal{};
    portal.fields = {0, 0, 0, 0};
    portal.blocks.assign(0x200, 0U);
    portal.blocks[0] = 0x01010000U;
    map.portals.push_back(std::move(portal));

    const auto buffer = map_editor::build_native_portal_render_buffer(map, {});

    assert(buffer.triangles.size() == 10U);
    assert(buffer.material_names.size() == 5U);
    assert(std::ranges::find(buffer.material_names, "south") != buffer.material_names.end());
    assert(std::ranges::find(buffer.material_names, "west") != buffer.material_names.end());
    assert(std::ranges::find(buffer.material_names, "north") != buffer.material_names.end());
    assert(std::ranges::find(buffer.material_names, "east") != buffer.material_names.end());
    assert(std::ranges::find(buffer.material_names, "up") != buffer.material_names.end());
}

void native_portal_render_buffer_clamps_face_material_slots() {
    map_editor::SblBlockMesh mesh{};
    mesh.width = 1;
    mesh.height = 1;
    mesh.depth = 1;
    mesh.faces.push_back({
        12U,
        {
            {0.0F, 0.0F, 1.0F},
            {1.0F, 0.0F, 1.0F},
            {1.0F, 1.0F, 1.0F},
            {0.0F, 1.0F, 1.0F},
        },
        {
            {0.0F, 0.0F},
            {1.0F, 0.0F},
            {1.0F, 1.0F},
            {0.0F, 1.0F},
        },
        false,
        0x60000U,
    });

    map_editor::SbcMap map{};
    map.city_blocks.push_back({
        0,
        "portalmesh",
        {std::array<std::string, 8>{"down", "south", "west", "north", "east", "up", "extra0", "extra1"}},
    });
    map_editor::SbcPortal portal{};
    portal.fields = {0, 0, 0, 0};
    portal.blocks.assign(0x200, 0U);
    portal.blocks[0] = 0x00010000U;
    map.portals.push_back(std::move(portal));

    const auto buffer = map_editor::build_native_portal_render_buffer(
        map,
        {map_editor::NamedBlockMesh{"portalmesh", mesh}});

    assert(buffer.triangles.size() == 2U);
    assert(buffer.material_names.size() == 1U);
    assert(buffer.material_names[0] == "extra1");
}

void native_city_building_render_origin_applies_editor_rotation_offsets() {
    map_editor::SbcBuilding building{};
    building.x = 10;
    building.y = 2;
    building.z = 20;

    building.rotation = 0;
    auto origin = map_editor::native_city_building_render_origin(building);
    assert(close_to(origin.x, 40.0F));
    // y*4 plus the native 0.25 cell lift (world cells span L*4+0.25..L*4+4.25).
    assert(close_to(origin.y, 8.25F));
    assert(close_to(origin.z, 80.0F));

    building.rotation = 1;
    origin = map_editor::native_city_building_render_origin(building);
    assert(close_to(origin.x, 44.0F));
    assert(close_to(origin.z, 80.0F));

    building.rotation = 2;
    origin = map_editor::native_city_building_render_origin(building);
    assert(close_to(origin.x, 44.0F));
    assert(close_to(origin.z, 84.0F));

    building.rotation = 3;
    origin = map_editor::native_city_building_render_origin(building);
    assert(close_to(origin.x, 40.0F));
    assert(close_to(origin.z, 84.0F));
}

void native_city_building_basis_uses_inverted_rotation_with_editor_offsets() {
    map_editor::SbcBuilding building{};
    building.x = 10;
    building.y = 2;
    building.z = 20;

    building.rotation = 1;
    auto basis = map_editor::native_city_building_rotation_basis(building.rotation);
    assert(close_to(basis.x_axis.x, 0.0F));
    assert(close_to(basis.x_axis.y, 0.0F));
    assert(close_to(basis.x_axis.z, 1.0F));
    assert(close_to(basis.z_axis.x, -1.0F));
    assert(close_to(basis.z_axis.y, 0.0F));
    assert(close_to(basis.z_axis.z, 0.0F));

    auto transformed = map_editor::native_city_building_transform_point(building, {1.0F, 2.0F, 3.0F});
    assert(close_to(transformed.x, 41.0F));
    assert(close_to(transformed.y, 10.25F));
    assert(close_to(transformed.z, 81.0F));

    building.rotation = 2;
    basis = map_editor::native_city_building_rotation_basis(building.rotation);
    assert(close_to(basis.x_axis.x, -1.0F));
    assert(close_to(basis.z_axis.z, -1.0F));

    transformed = map_editor::native_city_building_transform_point(building, {1.0F, 2.0F, 3.0F});
    assert(close_to(transformed.x, 43.0F));
    assert(close_to(transformed.y, 10.25F));
    assert(close_to(transformed.z, 81.0F));

    building.rotation = 3;
    basis = map_editor::native_city_building_rotation_basis(building.rotation);
    assert(close_to(basis.x_axis.x, 0.0F));
    assert(close_to(basis.x_axis.z, -1.0F));
    assert(close_to(basis.z_axis.x, 1.0F));

    transformed = map_editor::native_city_building_transform_point(building, {1.0F, 2.0F, 3.0F});
    assert(close_to(transformed.x, 43.0F));
    assert(close_to(transformed.y, 10.25F));
    assert(close_to(transformed.z, 83.0F));
}

void native_city_render_buffer_does_not_tag_plain_upper_walls_as_windows() {
    map_editor::SbbBuilding building{};
    building.extent_x = 1;
    building.extent_y = 2;
    building.extent_z = 1;
    building.size_x = 1;
    building.size_y = 1;
    building.size_z = 2;
    building.cells.push_back({0, 0, 0, 0, {0x40000000U, 0xffffU, 0U, 0U, 0U, 0U}, 0, true, {}});
    building.cells.push_back({1, 0, 1, 0, {0x40000000U, 0xffffU, 0U, 0U, 0U, 0U}, 0, true, {}});

    const auto buffer = map_editor::build_native_building_render_buffer(building, {}, {});
    assert(std::ranges::none_of(buffer.triangles, [](const map_editor::RenderBufferTriangle& triangle) {
        return triangle.city_window_surface;
    }));
    assert(std::ranges::none_of(buffer.material_runs, [](const map_editor::RenderBufferMaterialRun& run) {
        return run.city_window_surface;
    }));
}

void native_city_render_buffer_tags_buildblock_window_openings_with_wall_material() {
    map_editor::SblLayer layer{};
    layer.index = 1;
    layer.portal_type = 0;
    layer.bounds = {0.25F, 0.5F, 0.25F, 0.5F};
    layer.windows.push_back({0U, 0.375F, 0.25F, 0.4F, 0.2F});

    map_editor::SblTemplate buildblock{};
    buildblock.version = 1;
    buildblock.layers.resize(2);
    buildblock.layers[1] = layer;

    map_editor::SbbBuilding building{};
    building.extent_x = 1;
    building.extent_y = 1;
    building.extent_z = 1;
    building.block_names = {"portal"};
    building.cells.push_back({
        0,
        0,
        0,
        0,
        {0x40000000U, 0xffffU, 0U, 4U, 0U, 0U},
        0,
        true,
        {},
    });

    const auto buffer = map_editor::build_native_building_render_buffer(
        building,
        {},
        {map_editor::NamedBuildblockTemplate{"portal", buildblock}});

    assert(std::ranges::any_of(buffer.triangles, [](const map_editor::RenderBufferTriangle& triangle) {
        return triangle.city_window_surface && triangle.material == 0U;
    }));
    assert(std::ranges::any_of(buffer.material_runs, [](const map_editor::RenderBufferMaterialRun& run) {
        return run.city_window_surface && run.material == 0U;
    }));
    for (const auto& triangle : buffer.triangles) {
        if (!triangle.city_window_surface) {
            continue;
        }
        assert(triangle.material == 0U);
        const auto& a = buffer.vertices[triangle.indices[0]].position;
        const auto& b = buffer.vertices[triangle.indices[1]].position;
        const auto& c = buffer.vertices[triangle.indices[2]].position;
        const auto north_face = close_to(a.z, 0.0F) && close_to(b.z, 0.0F) && close_to(c.z, 0.0F);
        const auto south_face = close_to(a.z, 4.0F) && close_to(b.z, 4.0F) && close_to(c.z, 4.0F);
        if (north_face || south_face) {
            // Exterior pane (and its back face): the whole bounds (y 0.25..0.75
            // of the cell) is the reflective area (_generatematerialwindowtexture).
            assert(a.y >= 1.0F && b.y >= 1.0F && c.y >= 1.0F);
            assert(a.y <= 3.0F && b.y <= 3.0F && c.y <= 3.0F);
        } else {
            // Room glass: the window record (y 0.4..0.6), 1/64 in from the edge.
            assert(close_to(a.z, 4.0F / 64.0F) && close_to(b.z, 4.0F / 64.0F) && close_to(c.z, 4.0F / 64.0F));
            assert(a.y >= 1.6F - 1e-3F && b.y >= 1.6F - 1e-3F && c.y >= 1.6F - 1e-3F);
            assert(a.y <= 2.4F + 1e-3F && b.y <= 2.4F + 1e-3F && c.y <= 2.4F + 1e-3F);
        }
    }
}

void native_city_render_buffer_does_not_apply_exterior_windows_to_interior_cells() {
    map_editor::SblLayer layer{};
    layer.index = 1;
    layer.bounds = {0.25F, 0.5F, 0.25F, 0.5F};
    layer.windows.push_back({0U, 0.375F, 0.25F, 0.4F, 0.2F});

    map_editor::SblTemplate buildblock{};
    buildblock.version = 1;
    buildblock.layers.resize(2);
    buildblock.layers[1] = layer;

    map_editor::SbbBuilding building{};
    building.block_names = {"portal"};
    building.cells.push_back({
        0,
        0,
        0,
        0,
        {0x40000000U, 0x80000000U, 0U, 4U, 0U, 0U},
        0,
        true,
        {},
    });

    const auto buffer = map_editor::build_native_building_render_buffer(
        building,
        {},
        {map_editor::NamedBuildblockTemplate{"portal", buildblock}});

    assert(std::ranges::none_of(buffer.triangles, [](const map_editor::RenderBufferTriangle& triangle) {
        return triangle.city_window_surface;
    }));
}

void rebuilds_sbc_layout_with_new_records() {
    const auto bytes = map_editor::read_file_bytes(fixture_root() / "server/data/test2/city2.sbc");
    auto map = map_editor::parse_sbc(bytes);
    const auto original_intersection_count = map.intersections.size();
    const auto original_building_count = map.buildings.size();
    const auto original_street_count = map.streets.size();
    assert(!map.intersections.empty());
    assert(!map.buildings.empty());
    assert(!map.streets.empty());
    assert(!map.city_blocks.empty());

    auto intersection = map.intersections[0];
    intersection.index = map.intersections.size();
    intersection.x += 9;
    intersection.z += 11;
    map.intersections.push_back(intersection);

    auto building = map.buildings[0];
    building.index = map.buildings.size();
    building.name = "new_cpp";
    building.x += 16;
    map.buildings.push_back(building);

    auto street = map.streets[0];
    street.index = map.streets.size();
    street.name = "NewRoad";
    street.to_intersection = static_cast<std::int32_t>(intersection.index);
    street.district += 1;
    map.streets.push_back(street);

    const auto patched = map_editor::rebuild_sbc_layout(bytes, map);
    const auto reparsed = map_editor::parse_sbc(patched);

    assert(reparsed.intersections.size() == original_intersection_count + 1U);
    assert(reparsed.buildings.size() == original_building_count + 1U);
    assert(reparsed.streets.size() == original_street_count + 1U);
    assert(reparsed.intersections.back().x == intersection.x);
    assert(reparsed.intersections.back().z == intersection.z);
    assert(reparsed.buildings.back().name == "new_cpp");
    assert(reparsed.streets.back().name == "NewRoad");
    assert(reparsed.streets.back().to_intersection == static_cast<std::int32_t>(reparsed.intersections.back().index));
    assert(reparsed.city_blocks.size() == map.city_blocks.size());
    assert(reparsed.city_blocks[0].name == map.city_blocks[0].name);
}

void rebuilds_sbc_as_native_v12() {
    auto map = map_editor::parse_sbc_file(fixture_root() / "server/data/test2/city2.sbc");
    assert(!map.city_blocks.empty());
    assert(!map.portals.empty());
    map.streets[0].name = "NativeRoad";
    map.city_blocks[0].name = "native_piece";
    map.city_blocks[0].layers[0][0] = "native_slot";
    map.portals[0].blocks[3] = 0x02010005U;
    map.portals[0].itemsets.resize(0x200);
    map.portals[0].itemsets[3] = 0x40000009U;

    const auto rebuilt = map_editor::rebuild_sbc(map);
    const auto reparsed = map_editor::parse_sbc(rebuilt);

    assert(reparsed.version == 12);
    assert(reparsed.intersections.size() == map.intersections.size());
    assert(reparsed.streets[0].name == "NativeRoad");
    assert(reparsed.buildings.size() == map.buildings.size());
    assert(reparsed.city_blocks[0].name == "native_piece");
    assert(reparsed.city_blocks[0].layers[0][0] == "native_slot");
    assert(reparsed.portals[0].blocks[3] == 0x02010005U);
    assert(reparsed.portals[0].itemsets[3] == 0x40000009U);
    assert(reparsed.portal_connections.size() == map.portal_connections.size());
}

void rejects_lossy_legacy_sbc_upgrade() {
    std::vector<std::uint8_t> bytes;
    append_u32(bytes, 10);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 0);
    append_u32(bytes, 1);
    append_u32(bytes, 0);
    append_u32(bytes, 1);
    append_u32(bytes, 2);
    append_u32(bytes, 3);
    for (std::size_t index = 0; index < 0x200; ++index) {
        append_u32(bytes, index == 4 ? 0x80000001U : 0U);
    }
    for (std::size_t index = 0; index < 0x1000; ++index) {
        bytes.push_back(0);
        bytes.push_back(0);
    }
    for (std::size_t index = 0; index < 0x200; ++index) {
        append_u32(bytes, index == 4 ? 7U : 0U);
    }
    append_u32(bytes, 0);

    auto map = map_editor::parse_sbc(bytes);
    assert(map.version == 10);
    assert(map.portals[0].blocks.empty());
    assert(map.portals[0].legacy_blocks[4] == 0x80000001U);

    bool rejected = false;
    try { (void)map_editor::rebuild_sbc(map); }
    catch (const std::runtime_error&) { rejected = true; }
    assert(rejected);
    assert(map.portals[0].legacy_blocks[4] == 0x80000001U);
    assert(map.portals[0].itemsets[4] == 7U);

}

void patches_sbl_word() {
    const auto bytes = map_editor::read_file_bytes(fixture_root() / "server/data/test2/buildblock/test.sbl");
    const auto patched = map_editor::patch_sbl_word(bytes, 1, 42);
    const auto reparsed = map_editor::parse_sbl(patched);

    assert(reparsed.layers[0].portal_type == 42);
}

void patches_sbl_block_header() {
    const auto bytes = map_editor::read_file_bytes(fixture_root() / "server/data/test2/block/astairwellfloor.sbl");
    auto template_file = map_editor::parse_sbl(bytes);
    assert(template_file.block.has_value());
    auto block = *template_file.block;
    block.width += 1;
    block.height += 1;
    block.depth += 1;
    block.exterior_flags[0] = block.exterior_flags[0] == 0 ? 1 : 0;
    block.exterior_flags[5] = block.exterior_flags[5] == 0 ? 1 : 0;

    const auto patched = map_editor::patch_sbl_block_header(bytes, block);
    const auto reparsed = map_editor::parse_sbl(patched);

    assert(reparsed.block.has_value());
    assert(reparsed.block->width == block.width);
    assert(reparsed.block->height == block.height);
    assert(reparsed.block->depth == block.depth);
    assert(reparsed.block->exterior_flags[0] == block.exterior_flags[0]);
    assert(reparsed.block->exterior_flags[5] == block.exterior_flags[5]);
}

void patches_sbl_layer_portal_type() {
    const auto bytes = map_editor::read_file_bytes(fixture_root() / "server/data/test2/buildblock/test.sbl");
    const auto template_file = map_editor::parse_sbl(bytes);
    assert(template_file.layers.size() > 1);

    const auto patched = map_editor::patch_sbl_layer_portal_type(bytes, template_file, 1, 99);
    const auto reparsed = map_editor::parse_sbl(patched);

    assert(reparsed.layers[1].portal_type == 99);
}

void patches_sbl_layer_geometry() {
    const auto bytes = map_editor::read_file_bytes(fixture_root() / "server/data/test2/buildblock/test.sbl");
    auto template_file = map_editor::parse_sbl(bytes);
    assert(template_file.layers.size() > 1);
    assert(!template_file.layers[1].windows.empty());
    assert(!template_file.layers[1].beams.empty());

    template_file.layers[1].bounds[0] = 0.125F;
    template_file.layers[1].bounds[1] = 0.75F;
    template_file.layers[1].windows[0].x = 0.25F;
    template_file.layers[1].windows[0].width = 0.5F;
    template_file.layers[1].beams[0].height = 0.125F;

    const auto patched = map_editor::patch_sbl_layers(bytes, template_file);
    const auto reparsed = map_editor::parse_sbl(patched);

    assert(close_to(reparsed.layers[1].bounds[0], 0.125F));
    assert(close_to(reparsed.layers[1].bounds[1], 0.75F));
    assert(close_to(reparsed.layers[1].windows[0].x, 0.25F));
    assert(close_to(reparsed.layers[1].windows[0].width, 0.5F));
    assert(close_to(reparsed.layers[1].beams[0].height, 0.125F));
}

void patches_sbl_layers_with_native_record_counts() {
    const auto bytes = map_editor::read_file_bytes(fixture_root() / "server/data/test2/buildblock/test.sbl");
    auto template_file = map_editor::parse_sbl(bytes);
    assert(template_file.layers.size() > 1);
    assert(template_file.layers[1].windows.size() == 2);

    template_file.layers[1].window_count = 99;
    template_file.layers[1].beam_count = 99;

    const auto patched = map_editor::patch_sbl_layers(bytes, template_file);
    const auto reparsed = map_editor::parse_sbl(patched);

    assert(reparsed.layers[1].window_count == template_file.layers[1].windows.size());
    assert(reparsed.layers[1].beam_count == template_file.layers[1].beams.size());
}

void rebuilds_buildblock_after_adding_and_removing_openings() {
    map_editor::SblTemplate model{};
    model.version = 1;
    model.layers.resize(2);
    model.layers[0].bounds = {0, 1, 0, 1};
    model.layers[0].doors.push_back({0, .25F, .5F});
    model.layers[0].windows.push_back({0, .125F, .25F, .5F, .25F});
    model.layers[1].portal_type = 1;
    model.layers[1].bounds = {.25F, .5F, .125F, .75F};
    model.layers[1].beams.push_back({0, 0, 1, .5F, .125F});
    auto parsed = map_editor::parse_sbl(map_editor::rebuild_sbl_buildblock(model));
    assert(!parsed.block);
    assert(parsed.layers.size() == 2);
    assert(parsed.layers[0].door_count == 1);
    assert(parsed.layers[0].windows[0].width == .25F);
    assert(parsed.layers[1].bounds == model.layers[1].bounds);
    model.layers[0].doors.clear();
    model.layers[0].windows.clear();
    parsed = map_editor::parse_sbl(map_editor::rebuild_sbl_buildblock(model));
    assert(parsed.layers[0].doors.empty());
    assert(parsed.layers[1].portal_type == 1);
    assert(parsed.layers[1].beams[0].height == .125F);
}

void serializes_generated_quad_as_latest_sbl() {
    map_editor::SblBlockMesh mesh{};
    mesh.width = mesh.height = mesh.depth = 1;
    mesh.faces.push_back({2, {{0,0,0}, {1,0,0}, {1,1,0}, {0,1,0}},
        {{0,0}, {1,0}, {1,1}, {0,1}}, true, 0});
    const auto parsed = map_editor::parse_sbl(map_editor::rebuild_sbl_block(mesh));
    assert(parsed.version == 2 && parsed.block);
    assert(parsed.block->faces.size() == 1);
    const auto& face = parsed.block->faces[0];
    assert(face.material == 2);
    for (std::size_t i = 0; i < 4; ++i) {
        assert(face.vertices[i].x == mesh.faces[0].vertices[i].x);
        assert(face.vertices[i].y == mesh.faces[0].vertices[i].y);
        assert(face.vertices[i].z == mesh.faces[0].vertices[i].z);
        assert(face.uvs[i].u == mesh.faces[0].uvs[i].u);
        assert(face.uvs[i].v == mesh.faces[0].uvs[i].v);
    }
}

void preserves_sbc_header_records_on_upgrade() {
    map_editor::SbcMap map{};
    map.version = 12;
    map.header_records.resize(1);
    map.header_records[0][0] = 0x41;
    map.header_records[0][63] = 0x7f;
    const auto parsed = map_editor::parse_sbc(map_editor::rebuild_sbc(map));
    assert(parsed.header_records == map.header_records);
}

void rebuilds_csx_directory() {
    auto archive = map_editor::parse_csx_file(fixture_root() / "server/data/test2/test.csx");
    archive.entries[0].name = "renamed_cpp";
    archive.entries[0].payload = std::vector<std::uint8_t>{1, 2, 3, 4};

    const auto rebuilt = map_editor::build_csx(archive);
    const auto reparsed = map_editor::parse_csx(rebuilt);

    assert(reparsed.entries.size() == archive.entries.size());
    assert(reparsed.entries[0].name == "renamed_cpp");
    assert(reparsed.entries[0].size == 4);
    assert((reparsed.entries[0].payload == std::vector<std::uint8_t>{1, 2, 3, 4}));
}

}

void city_scene_fingerprint_changes_only_with_scene_inputs() {
    map_editor::SbcMap map{};
    map.intersections.push_back({0, 0, 0, 0});
    map.intersections.push_back({1, 4, 1, 0});
    map.streets.push_back({0, 0, 1, 0, 2, 2, "main"});
    map.buildings.push_back({0, "base01", 8, 2, 8, 0});

    const auto base = map_editor::city_scene_fingerprint(map);
    assert(map_editor::city_scene_fingerprint(map) == base);

    auto moved = map;
    moved.buildings[0].x += 1;
    assert(map_editor::city_scene_fingerprint(moved) != base);

    auto raised = map;
    raised.intersections[1].y += 1;
    assert(map_editor::city_scene_fingerprint(raised) != base);

    auto renamed = map;
    renamed.buildings[0].name = "tower07";
    assert(map_editor::city_scene_fingerprint(renamed) == base);
}

int main() {
    native_portal_render_buffer_drops_v10_cells_under_buildings();
    rebuilds_buildblock_after_adding_and_removing_openings();
    serializes_generated_quad_as_latest_sbl();
    preserves_sbc_header_records_on_upgrade();
    city_scene_fingerprint_changes_only_with_scene_inputs();
    parses_sbb_fixture();
    rebuilds_sbb_keeps_native_dimensions();
    parses_sbb_v12_without_anchor_words();
    parses_sbb_cell_words_in_native_file_order();
    parses_sbb_piece_payload_index_from_low_10_bits();
    rejects_unsupported_sbb_version();
    parses_sbb_v14_lod_tail();
    extracts_render_cells();
    resolves_sbb_piece_meshes();
    writes_sbl_blocks_losslessly();
    build_sbb_scene_recenters_cells_with_native_anchor();
    native_building_rendering_includes_interior_piece_words();
    native_building_tool_preview_generates_floor_faces_from_floor_cell_key();
    native_building_tool_preview_generates_ceiling_from_floor_cell_key();
    native_building_tool_preview_emits_only_exposed_solid_top_faces();
    native_building_tool_preview_places_side_code_faces_on_source_side();
    native_building_tool_preview_generates_inset_boxes_from_floor_cell_side_code();
    resolves_sbb_portal_edge_units();
    native_portal_edge_side_codes_follow_editor_edge_flags();
    attaches_portal_wall_rects_to_scene_edges();
    builds_native_building_render_buffer_from_piece_meshes();
    native_building_render_buffer_indexes_shared_cell_corners_and_uvs();
    native_building_render_buffer_emits_down_face_only_over_open_cells();
    native_building_render_buffer_uses_editor_face_order_and_uvs();
    native_building_render_buffer_uses_cell_material_slots();
    native_building_render_buffer_renders_interior_piece_with_interior_materials();
    native_building_buildblock_bounds_are_the_opening();
    native_building_tool_preview_uses_frame_faces_for_buildblock_opening();
    city_building_render_buffer_uses_full_building_preview_geometry();
    native_building_tool_preview_leaves_buildblock_opening_empty();
    picks_nearest_occupied_building_cell_with_native_anchor();
    building_cell_picker_respects_visible_floor_filter();
    city_building_picker_ignores_empty_cells_inside_bounds();
    city_piece_picker_selects_occupied_portal_voxel();
    edits_selected_sbb_cell_state();
    edits_selected_sbc_building_state();
    edits_selected_sbl_word_state();
    parses_sbc_fixture();
    parses_sbc_v10_without_city_blocks();
    rejects_unsupported_sbc_version();
    builds_map_scene();
    builds_grid_aligned_city_street_tile_strip();
    drops_city_tiles_outside_the_game_world();
    removes_junctions_no_road_uses();
    builds_city_street_tile_strips_for_all_cardinal_endpoint_orders();
    canonicalizes_city_street_endpoint_order_for_native_axes();
    fills_intersection_footprint_and_closes_missing_sides_with_curbs();
    terrain_noise_hash_matches_native_all_ones_mask();
    terrain_noise_sample_uses_native_two_to_negative_thirty_normalizer();
    native_like_city_terrain_height_matches_native_core_and_far_island();
    native_building_render_buffer_builds_interior_room();
    building_face_pick_returns_the_painted_slot_of_each_face();
    native_building_rooms_follow_the_game_room_key();
    city_district_cell_replaces_the_street_tile_in_its_cell();
    city_terrain_patches_hardcoded_intersection_blocks();
    city_terrain_flat_block_patch_is_level();
    city_terrain_inside_patch_conforms_to_piece_cells();
    city_terrain_inside_patch_conforms_to_building_footprints();
    overlapping_city_placements_let_the_later_one_win();
    fly_camera_moves_along_the_view();
    city_scene_fingerprint_sees_every_voxel();
    city_road_tile_heights_follow_native_slope_region();
    city_road_scene_positions_use_native_game_surface_height();
    city_editor_guides_use_native_expanded_extents();
    picks_city_intersection_marker_from_ray();
    traffic_light_poles_stand_at_native_corners_of_3way_plus_intersections();
    parses_buildblock_sbl_fixture();
    builds_buildblock_preview_scene();
    city_block_preview_treats_unmatched_record_name_as_solid_not_layer_meshes();
    city_block_preview_prefers_native_record_name_mesh();
    city_block_preview_renders_empty_name_as_nblock_solid_cell();
    city_block_preview_renders_unmatched_name_as_solid_cell();
    builds_portal_wall_rects_from_sbl_layer();
    builds_native_portal_overlay_rect_from_sbl_layer();
    parses_block_sbl_fixture();
    unsupported_sbl_version_is_not_block_mesh();
    parses_sbl_block_face_vertices();
    native_sbl_default_faces_keep_material_slots_and_oriented_uvs();
    parses_authored_sbl_patch_vertices();
    applies_native_sbl_authored_uv_orientation_flags();
    applies_native_sbl_authored_auto_orientation_flag();
    native_building_render_buffer_rotates_piece_material_slots_and_uvs();
    native_building_render_buffer_uses_generated_builtin_block_for_unresolved_piece_refs();
    native_block_mesh_catalog_seeds_builtin_blocks_before_file_blocks();
    native_block_rotation_uses_positive_rotated_bounds();
    native_block_mesh_rotation_only_cycles_side_material_slots();
    tessellates_authored_sbl_patches();
    emits_sbl_v2_light_record_faces();
    patches_sbl_authored_patch_control_point();
    refreshes_sbl_authored_patch_render_faces();
    parses_csx_fixture();
    patches_sbb_cell_words();
    patches_sbb_cells();
    rebuilds_sbb_as_native_v14_with_lod_tail();
    rebuilds_sbb_grows_native_dimensions_for_occupied_cells();
    patches_sbc_building_record();
    patches_sbc_building_records();
    patches_sbc_street_records();
    patches_sbc_city_block_layers();
    patches_sbc_portal_blocks();
    native_portal_render_buffer_uses_city_block_name_mesh_and_layer_materials();
    native_portal_render_buffer_renders_unnamed_city_block_as_solid_cell_faces();
    native_portal_render_buffer_renders_unmatched_named_city_block_as_solid_cell_faces();
    native_portal_render_buffer_clamps_face_material_slots();
    native_city_building_render_origin_applies_editor_rotation_offsets();
    native_city_building_basis_uses_inverted_rotation_with_editor_offsets();
    native_city_render_buffer_does_not_tag_plain_upper_walls_as_windows();
    native_city_render_buffer_tags_buildblock_window_openings_with_wall_material();
    native_city_render_buffer_does_not_apply_exterior_windows_to_interior_cells();
    rebuilds_sbc_layout_with_new_records();
    rebuilds_sbc_as_native_v12();
    rejects_lossy_legacy_sbc_upgrade();
    patches_sbl_word();
    patches_sbl_block_header();
    patches_sbl_layer_portal_type();
    patches_sbl_layer_geometry();
    patches_sbl_layers_with_native_record_counts();
    rebuilds_csx_directory();
    return 0;
}
