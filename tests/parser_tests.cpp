#include "formats.hpp"
#include "editor_state.hpp"
#include "scene.hpp"

#include <algorithm>
#include <cstring>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <filesystem>
#include <string>
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

void native_building_rendering_ignores_interior_piece_words() {
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
    assert(buffer.triangles.empty());

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
    assert(first.side_codes.south == source->exterior_portal_face());
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
    assert(edge.side_codes.south == 4U);
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
    assert(scene.portal_edges[0].wall_rects.south.size() == 4);
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

    assert(buffer.triangles.size() == 10U);
    assert(buffer.vertices.size() > 8U);
    assert(buffer.vertices.size() <= 20U);
    for (const auto& triangle : buffer.triangles) {
        for (const auto index : triangle.indices) {
            assert(index < buffer.vertices.size());
        }
    }
}

void native_building_render_buffer_never_emits_down_face() {
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

    assert(buffer.triangles.size() == 10U);
    for (const auto& triangle : buffer.triangles) {
        const auto& a = buffer.vertices[triangle.indices[0]].position;
        const auto& b = buffer.vertices[triangle.indices[1]].position;
        const auto& c = buffer.vertices[triangle.indices[2]].position;
        assert(!(close_to(a.y, 0.0F) && close_to(b.y, 0.0F) && close_to(c.y, 0.0F)));
    }
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

void native_city_building_render_buffer_keeps_plain_buildblock_walls_untextured() {
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
    assert(frame_run->triangle_count == 2U);
    assert(buffer.triangles.size() == 10U);
    assert(std::ranges::none_of(buffer.material_runs, [](const map_editor::RenderBufferMaterialRun& run) {
        return run.material == 0x100U;
    }));
    assert(std::ranges::none_of(buffer.material_runs, [](const map_editor::RenderBufferMaterialRun& run) {
        return run.city_window_surface;
    }));
    assert(!buffer.wire_lines.empty());
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

    assert(reparsed.size_x == cell.x + 1U);
    assert(reparsed.size_y == cell.z + 1U);
    assert(reparsed.size_z == cell.y + 1U);
    const auto match = std::ranges::find_if(reparsed.cells, [&cell](const map_editor::SbbCell& reparsed_cell) {
        return reparsed_cell.x == cell.x && reparsed_cell.y == cell.y && reparsed_cell.z == cell.z;
    });
    assert(match != reparsed.cells.end());
    assert(match->exterior_block() == 0x40000000);
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
    assert(close_to(origin.y, 8.0F));
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
    assert(close_to(transformed.y, 10.0F));
    assert(close_to(transformed.z, 81.0F));

    building.rotation = 2;
    basis = map_editor::native_city_building_rotation_basis(building.rotation);
    assert(close_to(basis.x_axis.x, -1.0F));
    assert(close_to(basis.z_axis.z, -1.0F));

    transformed = map_editor::native_city_building_transform_point(building, {1.0F, 2.0F, 3.0F});
    assert(close_to(transformed.x, 43.0F));
    assert(close_to(transformed.y, 10.0F));
    assert(close_to(transformed.z, 81.0F));

    building.rotation = 3;
    basis = map_editor::native_city_building_rotation_basis(building.rotation);
    assert(close_to(basis.x_axis.x, 0.0F));
    assert(close_to(basis.x_axis.z, -1.0F));
    assert(close_to(basis.z_axis.x, 1.0F));

    transformed = map_editor::native_city_building_transform_point(building, {1.0F, 2.0F, 3.0F});
    assert(close_to(transformed.x, 43.0F));
    assert(close_to(transformed.y, 10.0F));
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

void native_city_render_buffer_tags_buildblock_window_openings_only() {
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
        return triangle.city_window_surface && triangle.material == 0x100U;
    }));
    assert(std::ranges::any_of(buffer.material_runs, [](const map_editor::RenderBufferMaterialRun& run) {
        return run.city_window_surface && run.material == 0x100U;
    }));
    for (const auto& triangle : buffer.triangles) {
        if (!triangle.city_window_surface) {
            assert(triangle.material != 0x100U);
            continue;
        }
        assert(triangle.material == 0x100U);
        const auto& a = buffer.vertices[triangle.indices[0]].position;
        const auto& b = buffer.vertices[triangle.indices[1]].position;
        const auto& c = buffer.vertices[triangle.indices[2]].position;
        const auto north_face = close_to(a.z, 0.0F) && close_to(b.z, 0.0F) && close_to(c.z, 0.0F);
        const auto south_face = close_to(a.z, 4.0F) && close_to(b.z, 4.0F) && close_to(c.z, 4.0F);
        assert(north_face || south_face);
        assert(a.y >= 1.5F && b.y >= 1.5F && c.y >= 1.5F);
        assert(a.y <= 2.5F && b.y <= 2.5F && c.y <= 2.5F);
    }
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

void rebuilds_old_sbc_as_native_v12_without_legacy_portal_arrays() {
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

    const auto rebuilt = map_editor::rebuild_sbc(map);
    const auto reparsed = map_editor::parse_sbc(rebuilt);

    assert(reparsed.version == 12);
    assert(reparsed.city_blocks.empty());
    assert(reparsed.portals.size() == 1);
    assert(reparsed.portals[0].blocks[4] == 0U);
    assert(reparsed.portals[0].itemsets[4] == 7U);
    assert(reparsed.portals[0].legacy_blocks.empty());
    assert(reparsed.portals[0].textures.empty());
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

int main() {
    parses_sbb_fixture();
    parses_sbb_v12_without_anchor_words();
    parses_sbb_cell_words_in_native_file_order();
    parses_sbb_piece_payload_index_from_low_10_bits();
    rejects_unsupported_sbb_version();
    parses_sbb_v14_lod_tail();
    extracts_render_cells();
    resolves_sbb_piece_meshes();
    build_sbb_scene_recenters_cells_with_native_anchor();
    native_building_rendering_ignores_interior_piece_words();
    native_building_tool_preview_generates_floor_faces_from_floor_cell_key();
    native_building_tool_preview_generates_ceiling_from_floor_cell_key();
    native_building_tool_preview_places_side_code_faces_on_source_side();
    native_building_tool_preview_generates_inset_boxes_from_floor_cell_side_code();
    resolves_sbb_portal_edge_units();
    native_portal_edge_side_codes_follow_editor_edge_flags();
    attaches_portal_wall_rects_to_scene_edges();
    builds_native_building_render_buffer_from_piece_meshes();
    native_building_render_buffer_indexes_shared_cell_corners_and_uvs();
    native_building_render_buffer_never_emits_down_face();
    native_building_render_buffer_uses_editor_face_order_and_uvs();
    native_building_render_buffer_uses_cell_material_slots();
    native_city_building_render_buffer_keeps_plain_buildblock_walls_untextured();
    native_building_tool_preview_uses_frame_faces_for_buildblock_opening();
    native_building_tool_preview_leaves_buildblock_opening_empty();
    picks_nearest_occupied_building_cell_with_native_anchor();
    edits_selected_sbb_cell_state();
    edits_selected_sbc_building_state();
    edits_selected_sbl_word_state();
    parses_sbc_fixture();
    parses_sbc_v10_without_city_blocks();
    rejects_unsupported_sbc_version();
    builds_map_scene();
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
    native_city_render_buffer_tags_buildblock_window_openings_only();
    rebuilds_sbc_layout_with_new_records();
    rebuilds_sbc_as_native_v12();
    rebuilds_old_sbc_as_native_v12_without_legacy_portal_arrays();
    patches_sbl_word();
    patches_sbl_block_header();
    patches_sbl_layer_portal_type();
    patches_sbl_layer_geometry();
    patches_sbl_layers_with_native_record_counts();
    rebuilds_csx_directory();
    return 0;
}
