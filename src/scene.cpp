// SPDX-License-Identifier: GPL-2.0-only
#include "scene.hpp"

#include "buildblock_catalog.hpp"
#include "profiler.hpp"

#include <algorithm>
#include <thread>
#include <atomic>
#include <future>
#include <numeric>
#include <bit>
#include <cmath>
#include <tuple>
#include <limits>
#include <span>
#include <stdexcept>
#include <optional>
#include <string>
#include <string_view>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pafta {

std::optional<std::array<std::uint8_t, 3>> procedural_material_rgb(std::string_view name) {
    if (name == "city_road_asphalt") return std::array<std::uint8_t, 3>{43, 47, 50};
    if (name == "city_curb_concrete") return std::array<std::uint8_t, 3>{150, 154, 151};
    if (name == "city_terrain_grass") return std::array<std::uint8_t, 3>{64, 84, 38};
    if (name == "__window") return std::array<std::uint8_t, 3>{128, 128, 128};
    return std::nullopt;
}


std::uint32_t terrain_noise_hash(std::int32_t x, std::int32_t y, std::uint32_t mask) {
    auto u = ((static_cast<std::uint32_t>(y + 1) & mask) * 0x39U) +
        (static_cast<std::uint32_t>(x + 1) & mask);
    u = u ^ (u * 0x2000U);
    const auto v = u * u * 0x3d73U + 0xc0ae5U;
    return (v * v + 0xd208dd0dU) & 0x7fffffffU;
}

namespace {

// Geometry keeps its source cell after material sorting. The renderer can hide
// deleted cells by updating indices while a replacement mesh builds on a worker.
struct RenderCellTag {
    BuildingRenderBuffer& buffer;
    std::size_t first_triangle;
    std::uint32_t cell;
    ~RenderCellTag() {
        for (auto t = first_triangle; t < buffer.triangles.size(); ++t) buffer.triangles[t].cell = cell;
    }
};

// -----------------------------------------------------------------------------
// Portal rectangle helpers
// -----------------------------------------------------------------------------

float clamp01(float value) {
    return std::clamp(value, 0.0F, 1.0F);
}

float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

std::optional<PortalRect> portal_rect(float x, float width, float y, float height) {
    const auto min_x = clamp01(x);
    const auto min_y = clamp01(y);
    const auto max_x = clamp01(x + width);
    const auto max_y = clamp01(y + height);
    if (max_x - min_x <= 0.0001F || max_y - min_y <= 0.0001F) {
        return std::nullopt;
    }

    return PortalRect{
        min_x,
        min_y,
        max_x - min_x,
        max_y - min_y,
        max_x,
        max_y,
    };
}

bool mirrors_native_portal_x(const SblLayer& layer, PortalSide side) {
    if (layer.portal_type != 1U) {
        return true;
    }
    return side == PortalSide::South || side == PortalSide::West;
}

std::optional<PortalRect> portal_rect_for_side(
    const SblLayer& layer,
    PortalSide side,
    float x,
    float width,
    float y,
    float height) {
    if (mirrors_native_portal_x(layer, side)) {
        return portal_rect(1.0F - (x + width), width, y, height);
    }
    return portal_rect(x, width, y, height);
}

// Where subrosa.exe puts a layer rectangle on a building wall, as u along the wall
// (N/S walls: +x, E/W walls: +z) in fractions of a cell.
// Outside (the far view), the rectangle is drawn into the wall texture without
// mirroring (_generatematerialwindowtexture), and the face UVs follow the face's
// tangent (DAT_002b35a0: south +x, west +z, north -x, east -z). So it ends up
// mirrored on north and east walls only.
std::optional<PortalRect> building_exterior_rect(PortalSide side, float x, float width, float y, float height) {
    if (side == PortalSide::North || side == PortalSide::East) return portal_rect(1.0F - (x + width), width, y, height);
    return portal_rect(x, width, y, height);
}
// Inside (the room: BuildBlock_AddBeams / _block_addwindows), x is mirrored to
// 1 - (x + width), except for portal type 1 on the south and west sides. This
// happens in a wall frame that _block_addwindows turns (x,z) -> (1 - z, x) for each
// side, so it runs backwards on south and west walls. The result: mirrored on north
// and east walls, and on every wall for portal type 1. So type 0 lines up with the
// outside rectangle, as in the game.
std::optional<PortalRect> building_room_rect(const SblLayer& layer, PortalSide side, float x, float width, float y, float height) {
    if (layer.portal_type == 1U) return portal_rect(1.0F - (x + width), width, y, height);
    return building_exterior_rect(side, x, width, y, height);
}

PortalRect unit_portal_rect() {
    return {0.0F, 0.0F, 1.0F, 1.0F, 1.0F, 1.0F};
}

std::optional<PortalRect> intersect_rects(const PortalRect& left, const PortalRect& right) {
    return portal_rect(
        std::max(left.x, right.x),
        std::min(left.max_x, right.max_x) - std::max(left.x, right.x),
        std::max(left.y, right.y),
        std::min(left.max_y, right.max_y) - std::max(left.y, right.y));
}

std::vector<PortalRect> subtract_portal_rect(const PortalRect& base, const PortalRect& cutout) {
    const auto overlap = intersect_rects(base, cutout);
    if (!overlap.has_value()) {
        return {base};
    }

    std::vector<PortalRect> rects;
    const std::array candidates{
        portal_rect(base.x, base.width, base.y, overlap->y - base.y),
        portal_rect(base.x, base.width, overlap->max_y, base.max_y - overlap->max_y),
        portal_rect(base.x, overlap->x - base.x, overlap->y, overlap->height),
        portal_rect(overlap->max_x, base.max_x - overlap->max_x, overlap->y, overlap->height),
    };
    for (const auto& rect : candidates) {
        if (rect.has_value()) {
            rects.push_back(*rect);
        }
    }
    return rects;
}

bool rects_intersect(const PortalRect& left, const PortalRect& right) {
    return left.x < right.max_x && left.max_x > right.x && left.y < right.max_y && left.max_y > right.y;
}

PayloadKind visible_payload_kind(const SbbCell& cell) {
    const auto piece = std::ranges::find_if(cell.payloads, [](const SbbCellPayload& payload) {
        return payload.kind == PayloadKind::Piece;
    });
    if (piece != cell.payloads.end()) {
        return PayloadKind::Piece;
    }

    const auto block = std::ranges::find_if(cell.payloads, [](const SbbCellPayload& payload) {
        return payload.kind == PayloadKind::Block;
    });
    if (block != cell.payloads.end()) {
        return PayloadKind::Block;
    }

    return PayloadKind::Other;
}

// -----------------------------------------------------------------------------
// Asset and cell lookup
// -----------------------------------------------------------------------------

const SblBlockMesh* find_block_mesh(const std::vector<NamedBlockMesh>& block_meshes, std::string_view name) {
    const auto match = std::ranges::find_if(block_meshes, [name](const NamedBlockMesh& entry) {
        return entry.name == name;
    });
    return match == block_meshes.end() ? nullptr : &match->mesh;
}

SblBlockMesh make_native_builtin_block_mesh_impl() {
    SblBlockMesh mesh{};
    // VERIFY: the decompiled game copies g_Block_Width/Height/Depth before loading the
    // file in LoadBlock_SBL. The older TS and importer notes treat the built-in shape
    // as 1x1x1.
    mesh.width = 1U;
    mesh.height = 1U;
    mesh.depth = 1U;
    mesh.exterior_flags[0] = 1U;
    refresh_sbl_block_faces(mesh);
    return mesh;
}

const SblBlockMesh& native_builtin_block_mesh_singleton() {
    static const auto mesh = make_native_builtin_block_mesh_impl();
    return mesh;
}

bool begins_with_native_builtin_block_meshes(const std::vector<NamedBlockMesh>& block_meshes) {
    if (block_meshes.size() < native_builtin_block_mesh_count) {
        return false;
    }
    return std::ranges::all_of(
        block_meshes.begin(),
        block_meshes.begin() + static_cast<std::ptrdiff_t>(native_builtin_block_mesh_count),
        [](const NamedBlockMesh& entry) {
            return entry.name.empty();
        });
}

const SblBlockMesh* resolve_building_block_mesh(
    const std::vector<NamedBlockMesh>& block_meshes,
    const std::vector<std::string>& local_names,
    std::uint32_t local_index) {
    if (local_index < local_names.size()) {
        if (const auto* mesh = find_block_mesh(block_meshes, local_names[local_index])) {
            return mesh;
        }
    }
    if (begins_with_native_builtin_block_meshes(block_meshes) && local_index < block_meshes.size()) {
        return &block_meshes[local_index].mesh;
    }
    return &native_builtin_block_mesh_singleton();
}

enum class CityBlockRenderClass {
    SolidCell,
    Mesh,
};

struct CityBlockRenderResolution {
    CityBlockRenderClass render_class{CityBlockRenderClass::SolidCell};
    const SblBlockMesh* mesh{};
};

CityBlockRenderResolution resolve_city_block_render(
    const SbcCityBlock& city_block,
    const std::vector<NamedBlockMesh>& block_meshes) {
    // UpdatePieceVoxelData_14005fc90 starts every city block as 0x40000000, and only
    // turns blocks whose name matches a loaded block template into
    // 0x80000000 | blockIndex meshes.
    if (!city_block.name.empty()) {
        if (const auto* mesh = find_block_mesh(block_meshes, city_block.name)) {
            return {CityBlockRenderClass::Mesh, mesh};
        }
    }
    return {CityBlockRenderClass::SolidCell, nullptr};
}

std::string_view city_block_layer_slot_name(
    const SbcCityBlock& city_block,
    std::size_t layer_index,
    std::uint32_t slot) {
    if (city_block.layers.empty()) {
        return {};
    }
    const auto& layer = city_block.layers[std::min(layer_index, city_block.layers.size() - 1U)];
    return slot < layer.size() ? std::string_view{layer[slot]} : std::string_view{};
}

const SblTemplate* find_buildblock_template(const std::vector<NamedBuildblockTemplate>& templates, std::string_view name) {
    const auto match = std::ranges::find_if(templates, [name](const NamedBuildblockTemplate& entry) {
        return entry.name == name;
    });
    return match == templates.end() ? nullptr : &match->template_file;
}

const SblTemplate* resolve_building_buildblock_template(
    const std::vector<NamedBuildblockTemplate>& templates,
    const std::vector<std::string>& local_names,
    std::uint32_t local_index) {
    if (local_index < local_names.size()) {
        if (const auto* template_file = find_buildblock_template(templates, local_names[local_index])) {
            return template_file;
        }
    }
    return templates.empty() ? nullptr : &templates.front().template_file;
}

// Buildblock_InitTemplateMeshCatalog @0x10110 provides the built-in catalog that
// the low 16 bits of v10 district words point into. Loaded buildblock files are a
// separate catalog. Empty built-in slots have no geometry in the game.
const SblBlockMesh* resolve_district_procedural_mesh(std::uint32_t catalog_index) {
    static const std::vector<BuildblockCatalogEntry> catalog = build_native_builtin_catalog();
    if (catalog_index >= catalog.size()) {
        return nullptr;
    }
    const auto& mesh = catalog[catalog_index].mesh;
    if (mesh.faces.empty()) {
        return nullptr;
    }
    return &mesh;
}

const SbbCell* find_cell_at(const std::vector<SbbCell>& cells, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    const auto match = std::ranges::find_if(cells, [x, y, z](const SbbCell& cell) {
        return cell.x == x && cell.y == y && cell.z == z;
    });
    return match == cells.end() ? nullptr : &*match;
}

struct CellKey {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t z{};

    friend bool operator==(const CellKey&, const CellKey&) = default;
};

struct CellKeyHash {
    std::size_t operator()(const CellKey& key) const noexcept {
        auto value = static_cast<std::uint64_t>(key.x);
        value ^= static_cast<std::uint64_t>(key.y) + 0x9e3779b97f4a7c15ULL + (value << 6U) + (value >> 2U);
        value ^= static_cast<std::uint64_t>(key.z) + 0x9e3779b97f4a7c15ULL + (value << 6U) + (value >> 2U);
        return static_cast<std::size_t>(value);
    }
};

using CellIndexMap = std::unordered_map<CellKey, const SbbCell*, CellKeyHash>;

CellIndexMap build_cell_index(const std::vector<SbbCell>& cells) {
    CellIndexMap index;
    index.reserve(cells.size());
    for (const auto& cell : cells) {
        index.emplace(CellKey{cell.x, cell.y, cell.z}, &cell);
    }
    return index;
}

const SbbCell* find_cell_at(const CellIndexMap& index, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    const auto match = index.find(CellKey{x, y, z});
    return match == index.end() ? nullptr : match->second;
}

// -----------------------------------------------------------------------------
// Building render buffer helpers
// -----------------------------------------------------------------------------

constexpr auto native_preview_floor_y = 1.0F / 16.0F;
constexpr auto native_preview_ceiling_y = 7.0F / 8.0F;
constexpr auto native_preview_inset_depth = 1.0F / 16.0F;

bool is_solid_building_cell(const SbbCell& cell) {
    // BuildBuildingBlockRenderBuffers_1400799d0.c:128: the 0x40000000 kind marks a solid cell.
    return (cell.exterior_block() >> 29U) == 2U;
}

std::optional<SbbCellPayload> exterior_piece_payload(const SbbCell& cell) {
    const auto word = cell.exterior_block();
    // BuildBuildingBlockRenderBuffers_1400799d0.c:212: the 0x80000000 kind places a block mesh.
    if ((word >> 29U) != 4U) {
        return std::nullopt;
    }
    return SbbCellPayload{PayloadKind::Piece, word, word & 0x3ffU};
}

std::optional<SbbCellPayload> interior_piece_payload(const SbbCell& cell) {
    const auto word = cell.interior_block();
    // BuildBuildingBlockRenderBuffers_1400799d0.c:212: the 0x80000000 kind places a block mesh.
    if ((word >> 29U) != 4U) {
        return std::nullopt;
    }
    return SbbCellPayload{PayloadKind::Piece, word, word & 0x3ffU};
}

bool cell_has_exterior_surface(const SbbCell* cell) {
    return cell != nullptr && is_solid_building_cell(*cell);
}

PortalSideCodes native_edge_side_codes(const SbbCell& cell, const SbbCell* east, const SbbCell* south) {
    // BuildBuildingBlockRenderBuffers_1400799d0.c:136-141 fills edgeFlags for faces
    // 1..4, and lines 177-180 use edgeFlags[face-1] as layer index + 3.
    // Game face to C++ side, from lines 149-152:
    // face 1 z+1 -> South, face 2 x-1 -> West, face 3 z-1 -> North,
    // face 4 x+1 -> East.
    return {
        cell.exterior_portal_face(), // face 3 / North: BuildingCell +0x0c.
        cell.interior_portal_face(), // face 2 / West: BuildingCell +0x10.
        // Face 1 (South) uses the south neighbour's +0x0c, not this cell's
        // (Sbb_BuildBuildingConnectivityMetadata @29120 reads the record one z row on).
        south == nullptr ? 0U : south->exterior_portal_face(),
        east == nullptr ? 0U : east->interior_portal_face(), // face 4 / East: east-neighbor +0x10.
    };
}

enum class BuildingFaceSide {
    West,
    East,
    North,
    South,
    Up,
    Down,
};

// RenderBuildingCellItemsetBoundaryPreview_140079050 and FUN_1400523b0 use
// DAT_140105000 / DAT_140105170 / DAT_140105148. The corner order recorded there
// doesn't match the face table, so this preview uses a plain cube layout instead.
// How those tables relate to each other is still unknown.
constexpr std::array<Vec3, 8> native_preview_cube_corners{{
    {0.0F, 0.0F, 0.0F},
    {1.0F, 0.0F, 0.0F},
    {1.0F, 0.0F, 1.0F},
    {0.0F, 0.0F, 1.0F},
    {0.0F, 1.0F, 0.0F},
    {1.0F, 1.0F, 0.0F},
    {1.0F, 1.0F, 1.0F},
    {0.0F, 1.0F, 1.0F},
}};

constexpr std::array<std::array<std::uint8_t, 4>, 6> native_preview_face_corners{{
    {{3U, 2U, 1U, 0U}},
    {{7U, 6U, 2U, 3U}},
    {{4U, 7U, 3U, 0U}},
    {{5U, 4U, 0U, 1U}},
    {{6U, 5U, 1U, 2U}},
    {{4U, 5U, 6U, 7U}},
}};

constexpr std::array<Vec2f, 4> native_preview_face_uvs{{
    {0.0F, 0.0F},
    {0.0F, 1.0F},
    {1.0F, 1.0F},
    {1.0F, 0.0F},
}};

struct VertexKey {
    std::int64_t x{};
    std::int64_t y{};
    std::int64_t z{};
    std::int64_t u{};
    std::int64_t v{};

    friend bool operator==(const VertexKey&, const VertexKey&) = default;
};

struct VertexKeyHash {
    std::size_t operator()(const VertexKey& key) const noexcept {
        auto value = static_cast<std::uint64_t>(key.x);
        value ^= static_cast<std::uint64_t>(key.y) + 0x9e3779b97f4a7c15ULL + (value << 6U) + (value >> 2U);
        value ^= static_cast<std::uint64_t>(key.z) + 0x9e3779b97f4a7c15ULL + (value << 6U) + (value >> 2U);
        value ^= static_cast<std::uint64_t>(key.u) + 0x9e3779b97f4a7c15ULL + (value << 6U) + (value >> 2U);
        value ^= static_cast<std::uint64_t>(key.v) + 0x9e3779b97f4a7c15ULL + (value << 6U) + (value >> 2U);
        return static_cast<std::size_t>(value);
    }
};

using VertexIndexMap = std::unordered_map<VertexKey, std::uint32_t, VertexKeyHash>;

std::int64_t quantize_vertex_coord(float value) {
    if (std::abs(value) < 0.00001F) {
        value = 0.0F;
    }
    return static_cast<std::int64_t>(std::llround(value * 1000000.0F));
}

VertexKey vertex_key(Vec3 vertex, float u, float v) {
    return {
        quantize_vertex_coord(vertex.x),
        quantize_vertex_coord(vertex.y),
        quantize_vertex_coord(vertex.z),
        quantize_vertex_coord(u),
        quantize_vertex_coord(v),
    };
}

std::uint32_t add_indexed_vertex(BuildingRenderBuffer& buffer, VertexIndexMap& vertex_indices, Vec3 vertex, Vec2f uv) {
    const auto key = vertex_key(vertex, uv.u, uv.v);
    if (const auto existing = vertex_indices.find(key); existing != vertex_indices.end()) {
        return existing->second;
    }

    const auto index = static_cast<std::uint32_t>(buffer.vertices.size());
    buffer.vertices.push_back({vertex, uv.u, uv.v});
    vertex_indices.emplace(key, index);
    return index;
}

std::uint32_t rotate_native_material_slot(std::uint32_t material, std::int32_t rotation) {
    auto rotate_range = [rotation](std::uint32_t value, std::uint32_t first) {
        return ((value - first + static_cast<std::uint32_t>(rotation & 3)) & 3U) + first;
    };
    if (material >= 1U && material <= 4U) {
        return rotate_range(material, 1U);
    }
    if (material >= 9U && material <= 12U) {
        return rotate_range(material, 9U);
    }
    return material;
}

std::uint32_t rotate_native_block_mesh_material_slot(std::uint32_t material, std::int32_t rotation) {
    auto rotate_range = [rotation](std::uint32_t value, std::uint32_t first) {
        return ((value - first + static_cast<std::uint32_t>(rotation & 3)) & 3U) + first;
    };
    return material >= 1U && material <= 4U ? rotate_range(material, 1U) : material;
}

std::uint32_t rotate_native_orientation_once(std::uint32_t orientation) {
    switch (orientation & 0xf0000U) {
        case 0x60000U:
            return 0x10000U;
        case 0x10000U:
            return 0x50000U;
        case 0x50000U:
            return 0x20000U;
        case 0x20000U:
            return 0x60000U;
        default:
            return orientation & 0xf0000U;
    }
}

NativeRotatedBlockFace rotate_block_face_native_impl(const SblBlockMesh& mesh, const SblBlockFace& face, std::int32_t rotation) {
    NativeRotatedBlockFace rotated;
    auto width = static_cast<float>(mesh.width);
    auto depth = static_cast<float>(mesh.depth);
    auto orientation = face.orientation & 0xf0000U;
    rotated.material = face.material & 0xffffU;

    for (std::size_t index = 0; index < rotated.vertices.size(); ++index) {
        rotated.vertices[index] = {face.vertices[index].x, face.vertices[index].y, face.vertices[index].z};
        rotated.uvs[index] = index < face.uvs.size() ? face.uvs[index] : Vec2f{};
    }

    for (std::int32_t step = 0; step < (rotation & 3); ++step) {
        for (auto& vertex : rotated.vertices) {
            const auto next_x = depth - vertex.z;
            vertex.z = vertex.x;
            vertex.x = next_x;
        }
        std::swap(width, depth);

        if (orientation == 0x30000U || orientation == 0x40000U) {
            rotated.vertices = {rotated.vertices[3], rotated.vertices[0], rotated.vertices[1], rotated.vertices[2]};
            rotated.uvs = {rotated.uvs[3], rotated.uvs[0], rotated.uvs[1], rotated.uvs[2]};
        }

        if (orientation == 0U ||
            orientation == 0x60000U ||
            orientation == 0x10000U ||
            orientation == 0x50000U ||
            orientation == 0x20000U) {
            rotated.material = rotate_native_block_mesh_material_slot(rotated.material, 1);
            orientation = rotate_native_orientation_once(orientation);
        }
    }

    if (orientation != 0U) {
        for (std::size_t index = 0; index < rotated.uvs.size(); ++index) {
            const auto& vertex = rotated.vertices[index];
            rotated.uvs[index] = native_oriented_uv({vertex.x, vertex.y, vertex.z}, rotated.uvs[index], orientation);
        }
    }

    return rotated;
}

// Tilts the TOP of a cell to a slope. dy_x and dy_z are how much world Y rises per
// cell along each axis. Only the vertices on the cell's top (y == max_y) move, so
// the box keeps a flat bottom and the top edges of its side walls follow the slope.
// The default (all zero) leaves the geometry level.
struct CellTopSlope {
    float dy_x{};
    float dy_z{};
};

std::array<Vec3, 4> face_rect_vertices(
    BuildingFaceSide side,
    float cx,
    float cy,
    float cz,
    float u,
    float width,
    float v,
    float height,
    float unit_size,
    CellTopSlope top_slope = {}) {
    const auto half = unit_size * 0.5F;
    const auto min_x = cx - half;
    const auto max_x = cx + half;
    const auto min_y = cy - half;
    const auto max_y = cy + half;
    const auto min_z = cz - half;
    const auto max_z = cz + half;
    const auto x0 = min_x + u * unit_size;
    const auto x1 = min_x + (u + width) * unit_size;
    const auto y0 = min_y + v * unit_size;
    const auto y1 = min_y + (v + height) * unit_size;
    const auto z0 = min_z + u * unit_size;
    const auto z1 = min_z + (u + width) * unit_size;

    std::array<Vec3, 4> vertices{};
    switch (side) {
        case BuildingFaceSide::West:
            vertices = {Vec3{min_x, y1, z0}, Vec3{min_x, y1, z1}, Vec3{min_x, y0, z1}, Vec3{min_x, y0, z0}};
            break;
        case BuildingFaceSide::East:
            vertices = {Vec3{max_x, y1, z1}, Vec3{max_x, y1, z0}, Vec3{max_x, y0, z0}, Vec3{max_x, y0, z1}};
            break;
        case BuildingFaceSide::Down:
            vertices = {Vec3{x0, min_y, z1}, Vec3{x1, min_y, z1}, Vec3{x1, min_y, z0}, Vec3{x0, min_y, z0}};
            break;
        case BuildingFaceSide::Up:
            vertices = {Vec3{x0, max_y, z0}, Vec3{x1, max_y, z0}, Vec3{x1, max_y, z1}, Vec3{x0, max_y, z1}};
            break;
        case BuildingFaceSide::North:
            vertices = {Vec3{x1, y1, min_z}, Vec3{x0, y1, min_z}, Vec3{x0, y0, min_z}, Vec3{x1, y0, min_z}};
            break;
        case BuildingFaceSide::South:
        default:
            vertices = {Vec3{x0, y1, max_z}, Vec3{x1, y1, max_z}, Vec3{x1, y0, max_z}, Vec3{x0, y0, max_z}};
            break;
    }

    if (top_slope.dy_x != 0.0F || top_slope.dy_z != 0.0F) {
        for (auto& vertex : vertices) {
            // Only the cell's top vertices follow the slope. The bottom stays flat
            // so neighbouring cells still share a closed base.
            if (vertex.y >= max_y) {
                vertex.y += top_slope.dy_x * ((vertex.x - cx) / unit_size) +
                    top_slope.dy_z * ((vertex.z - cz) / unit_size);
            }
        }
    }
    return vertices;
}

void add_quad(
    BuildingRenderBuffer& buffer,
    VertexIndexMap& vertex_indices,
    const std::array<Vec3, 4>& vertices,
    const std::array<Vec2f, 4>& uvs,
    std::uint32_t material,
    bool city_window_surface = false) {
    const std::array indices{
        add_indexed_vertex(buffer, vertex_indices, vertices[0], uvs[0]),
        add_indexed_vertex(buffer, vertex_indices, vertices[1], uvs[1]),
        add_indexed_vertex(buffer, vertex_indices, vertices[2], uvs[2]),
        add_indexed_vertex(buffer, vertex_indices, vertices[3], uvs[3]),
    };
    buffer.triangles.push_back({{indices[0], indices[1], indices[2]}, material, city_window_surface});
    buffer.triangles.push_back({{indices[0], indices[2], indices[3]}, material, city_window_surface});
    buffer.wire_lines.push_back({vertices[0], vertices[1]});
    buffer.wire_lines.push_back({vertices[1], vertices[2]});
    buffer.wire_lines.push_back({vertices[2], vertices[3]});
    buffer.wire_lines.push_back({vertices[3], vertices[0]});
}

std::uint32_t material_id_for_name(BuildingRenderBuffer& buffer, std::string_view name) {
    const auto match = std::ranges::find(buffer.material_names, name);
    if (match != buffer.material_names.end()) {
        return static_cast<std::uint32_t>(std::distance(buffer.material_names.begin(), match));
    }
    buffer.material_names.emplace_back(name);
    return static_cast<std::uint32_t>(buffer.material_names.size() - 1U);
}

template <typename BlockMaterialSelector>
void add_rotated_block_mesh_to_buffer(
    BuildingRenderBuffer& buffer,
    VertexIndexMap& vertex_indices,
    const SblBlockMesh& mesh,
    Vec3 origin,
    std::int32_t rotation,
    BlockMaterialSelector select_material) {
    for (const auto& face : mesh.faces) {
        if (face.vertices.size() < 4) {
            continue;
        }
        const auto rotated_face = rotate_block_face_native_impl(mesh, face, rotation);
        std::array<Vec3, 4> vertices{};
        for (std::size_t index = 0; index < vertices.size(); ++index) {
            vertices[index] = {
                origin.x + rotated_face.vertices[index].x * buffer.unit_size,
                origin.y + rotated_face.vertices[index].y * buffer.unit_size,
                origin.z + rotated_face.vertices[index].z * buffer.unit_size,
            };
        }
        add_quad(buffer, vertex_indices, vertices, rotated_face.uvs, select_material(buffer, face, rotated_face));
    }
}

std::size_t material_slot_for_cell_side(BuildingFaceSide side) {
    switch (side) {
        case BuildingFaceSide::Down:
            return 0U;
        case BuildingFaceSide::South:
            return 1U;
        case BuildingFaceSide::West:
            return 2U;
        case BuildingFaceSide::North:
            return 3U;
        case BuildingFaceSide::East:
            return 4U;
        case BuildingFaceSide::Up:
        default:
            return 5U;
    }
}

std::uint32_t material_for_cell_slot(const SbbCell& cell, std::uint32_t slot) {
    const auto material = slot < cell.shorts.size() ? cell.shorts[slot] : 0U;
    return material == 0xffffU ? 0U : material;
}

std::uint32_t material_for_cell_side(const SbbCell& cell, BuildingFaceSide side) {
    const auto slot = material_slot_for_cell_side(side);
    return material_for_cell_slot(cell, static_cast<std::uint32_t>(slot));
}

std::uint32_t piece_face_slot(std::uint32_t slot, bool interior) {
    // RenderBuildingCellFaceGroupWorld/Preview clamp face slots to 0..7. Inside faces
    // use the cell's inside texture set for slots 0..7, and slots 8..15 use the
    // outside set after subtracting eight.
    if (interior) {
        if (slot < 8U) return slot + 8U;
        if (slot < 16U) return slot - 8U;
        return 0U;
    }
    return std::min(slot, 7U);
}

std::uint32_t material_for_piece_face(const SbbCell& cell, std::uint32_t slot, bool interior) {
    return material_for_cell_slot(cell, piece_face_slot(slot, interior));
}

std::array<Vec3, 4> native_preview_cube_face_vertices(const SbbCell& cell, std::uint32_t face, float unit_size) {
    std::array<Vec3, 4> vertices{};
    const auto& corners = native_preview_face_corners[face];
    const auto cell_x = static_cast<float>(cell.x);
    const auto cell_y = static_cast<float>(cell.y);
    const auto cell_z = static_cast<float>(cell.z);
    // Side walls (faces 1-4) sit 1/16 in from the cell's edge, like editor.exe
    // SetupDefaultFaceVerticesAndEmit (the side template sits at z=0.0625, and
    // RotateBlockFaceVertices_90Degrees turns it to face each direction). Without this,
    // the walls two solid cells share land in the same plane and flicker. With it,
    // opposite walls sit at 1/16 and 15/16, apart from each other.
    // The wall moves along the face's inward normal. With native_preview_face_corners
    // each side face is flat on one axis, so we move that axis toward the cell's centre.
    for (std::size_t corner = 0; corner < vertices.size(); ++corner) {
        auto position = native_preview_cube_corners[corners[corner]];
        if (face != 0U && face != 5U) {
            // Pull the edge axis (the one that is the same at all 4 corners)
            // inward by native_preview_floor_y.
            const auto& c0 = native_preview_cube_corners[corners[0]];
            const auto& c1 = native_preview_cube_corners[corners[1]];
            const auto& c2 = native_preview_cube_corners[corners[2]];
            if (c0.x == c1.x && c1.x == c2.x) {
                position.x += (c0.x == 0.0F ? native_preview_floor_y : -native_preview_floor_y);
            } else if (c0.z == c1.z && c1.z == c2.z) {
                position.z += (c0.z == 0.0F ? native_preview_floor_y : -native_preview_floor_y);
            }
        }
        vertices[corner] = {
            (position.x + cell_x) * unit_size,
            (position.y + cell_y) * unit_size,
            (position.z + cell_z) * unit_size,
        };
    }
    return vertices;
}

std::array<Vec3, 4> native_tool_default_face_vertices(const SbbCell& cell, std::uint32_t face, float unit_size) {
    if (face != 0U && face != 5U) {
        return native_preview_cube_face_vertices(cell, face, unit_size);
    }

    const auto y = face == 0U ? native_preview_floor_y : native_preview_ceiling_y;
    const std::array local_vertices{
        Vec3{0.0F, y, 0.0F},
        Vec3{1.0F, y, 0.0F},
        Vec3{1.0F, y, 1.0F},
        Vec3{0.0F, y, 1.0F},
    };

    std::array<Vec3, 4> vertices{};
    for (std::size_t index = 0; index < vertices.size(); ++index) {
        vertices[index] = {
            (static_cast<float>(cell.x) + local_vertices[index].x) * unit_size,
            (static_cast<float>(cell.y) + local_vertices[index].y) * unit_size,
            (static_cast<float>(cell.z) + local_vertices[index].z) * unit_size,
        };
    }
    return vertices;
}

std::uint32_t material_for_tool_generated_slot(const SbbCell& cell, std::uint32_t material) {
    // RenderBuildingCellFaceGroupPreview_140078840, generated-mesh branch:
    // param_2 == 1 reads material ids 0..7 from the second cell texture set,
    // and ids 8..15 from the first set after subtracting 8.
    const auto slot = material < 8U ? 8U + material : ((material - 8U) & 7U);
    return material_for_cell_slot(cell, slot);
}

void add_native_tool_default_face(
    BuildingRenderBuffer& buffer,
    VertexIndexMap& vertex_indices,
    const SbbCell& cell,
    std::uint32_t face) {
    add_quad(
        buffer,
        vertex_indices,
        native_tool_default_face_vertices(cell, face, buffer.unit_size),
        native_preview_face_uvs,
        material_for_tool_generated_slot(cell, face));
}

std::array<std::uint32_t, 4> native_tool_preview_side_codes(
    const SbbCell& cell,
    const SbbCell* east,
    const SbbCell* south) {
    // RenderBuildingToolCellPreview_140078060 packs these nibbles before calling
    // FUN_140051070(1, key): own exterior, east interior, south exterior,
    // own interior.
    return {{
        cell.exterior_portal_face() & 0xfU,
        east == nullptr ? 0U : east->interior_portal_face() & 0xfU,
        south == nullptr ? 0U : south->exterior_portal_face() & 0xfU,
        cell.interior_portal_face() & 0xfU,
    }};
}

// VERIFY: orientation was fixed from a screenshot; needs one more visual check.
constexpr std::array<std::uint32_t, 4> native_tool_preview_side_faces{{3U, 4U, 1U, 2U}};

Vec3 rotate_native_tool_side(Vec3 vertex, std::uint32_t side) {
    for (std::uint32_t step = 0; step < (side & 3U); ++step) {
        vertex = {1.0F - vertex.z, vertex.y, vertex.x};
    }
    return vertex;
}

std::array<Vec3, 4> native_tool_box_face_vertices(
    const SbbCell& cell,
    std::uint32_t side,
    float x,
    float y,
    float z,
    float width,
    float height,
    float depth,
    std::uint32_t face,
    float unit_size) {
    std::array<Vec3, 4> vertices{};
    const auto& corners = native_preview_face_corners[face];
    for (std::size_t corner = 0; corner < vertices.size(); ++corner) {
        const auto& position = native_preview_cube_corners[corners[corner]];
        auto local = Vec3{
            x + position.x * width,
            y + position.y * height,
            z + position.z * depth,
        };
        local = rotate_native_tool_side(local, side);
        vertices[corner] = {
            (static_cast<float>(cell.x) + local.x) * unit_size,
            (static_cast<float>(cell.y) + local.y) * unit_size,
            (static_cast<float>(cell.z) + local.z) * unit_size,
        };
    }
    return vertices;
}

using NativeToolBoxMaterialSlots = std::array<std::uint32_t, 6>;

void add_native_tool_box(
    BuildingRenderBuffer& buffer,
    VertexIndexMap& vertex_indices,
    const SbbCell& cell,
    std::uint32_t side,
    float x,
    float y,
    float z,
    float width,
    float height,
    float depth,
    std::uint32_t face_mask,
    const NativeToolBoxMaterialSlots& material_slots) {
    if (width <= 0.0F || height <= 0.0F || depth <= 0.0F) {
        return;
    }

    for (std::uint32_t face = 0; face < native_preview_face_corners.size(); ++face) {
        if ((face_mask & (1U << face)) == 0U) {
            continue;
        }
        const auto material = rotate_native_material_slot(material_slots[face], static_cast<std::int32_t>(side));
        add_quad(
            buffer,
            vertex_indices,
            native_tool_box_face_vertices(cell, side, x, y, z, width, height, depth, face, buffer.unit_size),
            native_preview_face_uvs,
            material_for_tool_generated_slot(cell, material));
    }
}

float native_tool_inset_x(const SblLayer& layer, std::uint32_t side, float x, float width) {
    // FUN_140051c60 mirrors x unless portal_type == 1 and side > 1.
    if (layer.portal_type == 1U && side > 1U) {
        return x;
    }
    return 1.0F - (x + width);
}

void add_native_tool_inset_detail(
    BuildingRenderBuffer& buffer,
    VertexIndexMap& vertex_indices,
    const SbbCell& cell,
    std::uint32_t side,
    const SblLayer& layer) {
    NativeToolBoxMaterialSlots material_slots{{0U, 1U, 2U, 0xbU, 4U, 5U}};
    const auto kind_one = layer.portal_type == 1U;
    const auto x = native_tool_inset_x(layer, side, layer.bounds[0], layer.bounds[1]);
    const auto width = layer.bounds[1];
    const auto y = layer.bounds[2];
    const auto height = layer.bounds[3];

    if (x > 0.0F) {
        material_slots[4] = 6U;
        add_native_tool_box(
            buffer,
            vertex_indices,
            cell,
            side,
            0.0F,
            0.0F,
            0.0F,
            x,
            1.0F,
            native_preview_inset_depth,
            kind_one ? 0x12U : 0x1aU,
            material_slots);
    }
    if (x + width < 1.0F) {
        material_slots[2] = 6U;
        add_native_tool_box(
            buffer,
            vertex_indices,
            cell,
            side,
            x + width,
            0.0F,
            0.0F,
            1.0F - (x + width),
            1.0F,
            native_preview_inset_depth,
            kind_one ? 0x6U : 0xeU,
            material_slots);
    }
    if (y > 0.0F) {
        material_slots[5] = 6U;
        add_native_tool_box(
            buffer,
            vertex_indices,
            cell,
            side,
            0.0F,
            0.0F,
            0.0F,
            1.0F,
            y,
            native_preview_inset_depth,
            kind_one ? 0x22U : 0x2aU,
            material_slots);
    }
    if (y + height < 1.0F) {
        material_slots[0] = 6U;
        add_native_tool_box(
            buffer,
            vertex_indices,
            cell,
            side,
            0.0F,
            y + height,
            0.0F,
            1.0F,
            1.0F - (y + height),
            native_preview_inset_depth,
            kind_one ? 0x3U : 0xbU,
            material_slots);
    }

    auto add_rect_box = [&](const SblPortalRect& rect) {
        add_native_tool_box(
            buffer,
            vertex_indices,
            cell,
            side,
            native_tool_inset_x(layer, side, rect.x, rect.width),
            rect.y,
            0.0F,
            rect.width,
            rect.height,
            native_preview_inset_depth,
            0x3fU,
            material_slots);
    };
    // Windows are openings. They should look see-through like the door gap, not like
    // solid boxes set into the wall. Draw each window as a thin frame around its
    // rectangle, leaving the middle open so you can see through. A frame is four narrow
    // strips; frame_w is how thick a strip is, in fractions of a cell.
    constexpr float frame_w = 1.0F / 16.0F;
    auto add_frame_strip = [&](float fx, float fy, float fwidth, float fheight) {
        if (fwidth <= 0.0F || fheight <= 0.0F) {
            return;
        }
        add_native_tool_box(
            buffer,
            vertex_indices,
            cell,
            side,
            native_tool_inset_x(layer, side, fx, fwidth),
            fy,
            0.0F,
            fwidth,
            fheight,
            native_preview_inset_depth,
            0x3fU,
            material_slots);
    };
    for (const auto& window : layer.windows) {
        const float wx = window.x;
        const float wy = window.y;
        const float ww = window.width;
        const float wh = window.height;
        const float fw = std::min(frame_w, ww * 0.5F);
        const float fh = std::min(frame_w, wh * 0.5F);
        add_frame_strip(wx, wy, ww, fh);                       // bottom
        add_frame_strip(wx, wy + wh - fh, ww, fh);             // top
        add_frame_strip(wx, wy + fh, fw, wh - 2.0F * fh);      // left
        add_frame_strip(wx + ww - fw, wy + fh, fw, wh - 2.0F * fh); // right
    }
    // VERIFY(editor.exe FUN_140051c60): the decompile proves the first rectangle array
    // is at +0x1138. In this port, beams use the same SblPortalRect shape and are drawn
    // as the same kind of inset boxes, so beams show up.
    for (const auto& beam : layer.beams) {
        add_rect_box(beam);
    }
}

void add_native_tool_floor_cell_mesh(
    BuildingRenderBuffer& buffer,
    VertexIndexMap& vertex_indices,
    const SbbBuilding& building,
    const std::vector<NamedBuildblockTemplate>& buildblock_templates,
    const SbbCell& cell,
    const SbbCell* east,
    const SbbCell* south) {
    const auto side_codes = native_tool_preview_side_codes(cell, east, south);
    const auto* template_file = resolve_building_buildblock_template(
        buildblock_templates,
        building.block_names,
        cell.cell_type());

    if ((cell.placed_flag() & 1U) != 0U) {
        add_native_tool_default_face(buffer, vertex_indices, cell, 0U);
    }
    if ((cell.placed_flag() & 2U) != 0U) {
        add_native_tool_default_face(buffer, vertex_indices, cell, 5U);
    }

    for (std::uint32_t side = 0; side < side_codes.size(); ++side) {
        const auto side_code = side_codes[side];
        if (side_code == 1U) {
            add_native_tool_default_face(buffer, vertex_indices, cell, native_tool_preview_side_faces[side]);
        } else if (side_code >= 3U && side_code <= 15U && template_file != nullptr) {
            const auto layer_index = static_cast<std::size_t>(side_code - 3U);
            if (layer_index < template_file->layers.size()) {
                add_native_tool_inset_detail(buffer, vertex_indices, cell, side, template_file->layers[layer_index]);
            }
        }
    }
}

std::array<Vec2f, 4> quad_uvs(float u, float width, float v, float height) {
    return {{
        {u, v + height},
        {u + width, v + height},
        {u + width, v},
        {u, v},
    }};
}

std::optional<PortalSide> portal_side_for_building_face(BuildingFaceSide side) {
    switch (side) {
        case BuildingFaceSide::South:
            return PortalSide::South;
        case BuildingFaceSide::West:
            return PortalSide::West;
        case BuildingFaceSide::North:
            return PortalSide::North;
        case BuildingFaceSide::East:
            return PortalSide::East;
        case BuildingFaceSide::Up:
        case BuildingFaceSide::Down:
        default:
            return std::nullopt;
    }
}

// Sbb_BuildBuildingConnectivityMetadata @29120: face 1 (+Z) reads the +0x0c of the
// cell at z+1, face 2 (-X) this cell's +0x10, face 3 (-Z) this cell's +0x0c, and
// face 4 (+X) the +0x10 of the cell at x+1. Each side code belongs to the wall
// between two cells, and both cells share it.
std::uint32_t city_buildblock_side_code_for_face(
    const SbbCell& cell,
    const SbbCell* east,
    const SbbCell* south,
    BuildingFaceSide side) {
    switch (side) {
        case BuildingFaceSide::South:
            return south == nullptr ? 0U : south->exterior_portal_face();
        case BuildingFaceSide::North:
            return cell.exterior_portal_face();
        case BuildingFaceSide::West:
            return cell.interior_portal_face();
        case BuildingFaceSide::East:
            return east == nullptr ? 0U : east->interior_portal_face();
        case BuildingFaceSide::Up:
        case BuildingFaceSide::Down:
        default:
            return 0U;
    }
}

const SblLayer* buildblock_layer_for_side_code(const SblTemplate* template_file, std::uint32_t side_code) {
    if (template_file == nullptr || side_code < 3U || side_code > 15U) {
        return nullptr;
    }
    const auto layer_index = static_cast<std::size_t>(side_code - 3U);
    return layer_index < template_file->layers.size() ? &template_file->layers[layer_index] : nullptr;
}

std::vector<PortalRect> subtract_city_window_rects(const std::vector<PortalRect>& windows) {
    std::vector<PortalRect> walls{unit_portal_rect()};
    for (const auto& window : windows) {
        std::vector<PortalRect> next;
        for (const auto& wall : walls) {
            auto fragments = subtract_portal_rect(wall, window);
            next.insert(next.end(), fragments.begin(), fragments.end());
        }
        walls = std::move(next);
    }
    return walls;
}

void add_city_building_face_rect(
    BuildingRenderBuffer& buffer,
    VertexIndexMap& vertex_indices,
    BuildingFaceSide side,
    float cx,
    float cy,
    float cz,
    const PortalRect& rect,
    std::uint32_t material,
    bool city_window_surface,
    CellTopSlope top_slope = {}) {
    add_quad(
        buffer,
        vertex_indices,
        face_rect_vertices(side, cx, cy, cz, rect.x, rect.width, rect.y, rect.height, buffer.unit_size, top_slope),
        quad_uvs(rect.x, rect.width, rect.y, rect.height),
        material,
        city_window_surface);
}

struct SolidCellFaceCheck {
    BuildingFaceSide side;
    const SbbCell* neighbor{};
};

template <typename MaterialSelector>
void add_native_solid_cell_faces(
    BuildingRenderBuffer& buffer,
    VertexIndexMap& vertex_indices,
    const SbbCell& cell,
    float cx,
    float cy,
    float cz,
    std::span<const SolidCellFaceCheck> native_sides,
    const SbbCell* east,
    const SbbCell* south,
    const SblTemplate* template_file,
    MaterialSelector select_material,
    CellTopSlope top_slope = {}) {
    for (const auto& side : native_sides) {
        if (cell_has_exterior_surface(side.neighbor)) {
            continue;
        }

        std::vector<PortalRect> window_rects;
        // The game gives every face with a buildblock side code the window-array
        // texture, and near sectors skip those faces (_render_buildsectorlist). So
        // up close, an inside piece on that side (like a garage door) stands open.
        bool buildblock_coded = false;
        const auto portal_side = portal_side_for_building_face(side.side);
        if (portal_side.has_value()) {
            const auto side_code = city_buildblock_side_code_for_face(cell, east, south, side.side);
            const auto* layer = buildblock_layer_for_side_code(template_file, side_code);
            buildblock_coded = layer != nullptr;
            // BuildBuildingBlockRenderBuffers only builds the outside edge shape
            // when the cell has no interior block (the game's interiorBlock ==
            // 0xffff). An interior piece owns that side's geometry and must not
            // pick up the outside window mask.
            if (layer != nullptr && cell.interior_block() == 0xffffU) {
                // _generatematerialwindowtexture @9a1a1 makes one texture-array
                // layer for each wall texture, buildblock, and layer: the wall
                // texture, with alpha 1 over the layer's BOUNDS rectangle. Window
                // and door records are not read. array_world.frag uses that alpha as
                // shine, so from outside the whole bounds is a reflective opening,
                // door layers (bounds with no windows) included. Keep the wall
                // material on these quads so the shader sees the same base colour.
                if (const auto bounds = building_exterior_rect(
                        *portal_side, layer->bounds[0], layer->bounds[1], layer->bounds[2], layer->bounds[3])) {
                    window_rects.push_back(*bounds);
                }
            }
        }

        const auto wall_material = select_material(side.side);
        const auto first_triangle = buffer.triangles.size();
        const auto wall_rects = window_rects.empty()
            ? std::vector<PortalRect>{unit_portal_rect()}
            : subtract_city_window_rects(window_rects);
        for (const auto& rect : wall_rects) {
            add_city_building_face_rect(
                buffer,
                vertex_indices,
                side.side,
                cx,
                cy,
                cz,
                rect,
                wall_material,
                false,
                top_slope);
        }
        for (const auto& rect : window_rects) {
            add_city_building_face_rect(
                buffer,
                vertex_indices,
                side.side,
                cx,
                cy,
                cz,
                rect,
                wall_material,
                true,
                top_slope);
        }
        if (buildblock_coded) {
            for (auto t = first_triangle; t < buffer.triangles.size(); ++t) buffer.triangles[t].far_only = true;
        }
    }
}

SbbCell native_city_block_solid_cell() {
    return {
        0,
        0,
        0,
        0,
        {0x40000000U, 0xffffU, 0U, 0U, 0U, 0U},
        0,
        true,
        {},
    };
}

template <typename MaterialSelector>
void add_native_city_block_solid_cell(
    BuildingRenderBuffer& buffer,
    VertexIndexMap& vertex_indices,
    Vec3 origin,
    MaterialSelector select_material,
    std::array<bool, 5> covered = {}) {
    const auto cell = native_city_block_solid_cell();
    const auto half = buffer.unit_size * 0.5F;
    const auto cx = origin.x + half;
    const auto cy = origin.y + half;
    const auto cz = origin.z + half;
    // BuildBuildingBlockRenderBuffers makes game faces 1..5 for solid cells. Face 0
    // (down) is left out here, like in the existing solid building path. A side
    // covered by a solid neighbour is not made (the visible-face mask).
    const auto neighbor = [&](std::size_t i) { return covered[i] ? &cell : nullptr; };
    const std::array<SolidCellFaceCheck, 5> exposed_sides{{
        {BuildingFaceSide::South, neighbor(0)},
        {BuildingFaceSide::West, neighbor(1)},
        {BuildingFaceSide::North, neighbor(2)},
        {BuildingFaceSide::East, neighbor(3)},
        {BuildingFaceSide::Up, neighbor(4)},
    }};

    add_native_solid_cell_faces(
        buffer,
        vertex_indices,
        cell,
        cx,
        cy,
        cz,
        exposed_sides,
        nullptr,
        nullptr,
        nullptr,
        select_material);
}

BuildingRenderBuffer build_city_block_solid_preview_buffer() {
    BuildingRenderBuffer buffer;
    VertexIndexMap vertex_indices;
    add_native_city_block_solid_cell(
        buffer,
        vertex_indices,
        {},
        [](BuildingFaceSide side) {
            return static_cast<std::uint32_t>(material_slot_for_cell_side(side));
        });
    return buffer;
}

void sort_triangles_into_native_material_runs(BuildingRenderBuffer& buffer) {
    // Picking grids store triangle numbers, which sorting changes.
    buffer.mask_cache.clear();
    buffer.cell_mask_index.clear();
    buffer.terrain_chunks.clear();
    buffer.pick_grid = {};
    std::stable_sort(buffer.triangles.begin(), buffer.triangles.end(), [](const RenderBufferTriangle& left, const RenderBufferTriangle& right) {
        if (left.material != right.material) {
            return left.material < right.material;
        }
        return !left.city_window_surface && right.city_window_surface;
    });

    buffer.material_runs.clear();
    for (std::size_t index = 0; index < buffer.triangles.size();) {
        const auto material = buffer.triangles[index].material;
        const auto city_window_surface = buffer.triangles[index].city_window_surface;
        const auto first = index;
        while (index < buffer.triangles.size() &&
               buffer.triangles[index].material == material &&
               buffer.triangles[index].city_window_surface == city_window_surface) {
            ++index;
        }
        buffer.material_runs.push_back({
            material,
            static_cast<std::uint32_t>(first),
            static_cast<std::uint32_t>(index - first),
            city_window_surface,
        });
    }
}

std::vector<PortalRect> wall_rects_for_side_code(const SblTemplate& template_file, std::uint32_t side_code, PortalSide side) {
    // BuildBuildingBlockRenderBuffers_1400799d0.c:177-180 uses side codes 3..15 as
    // shape index sc-3 into the buildblock/item-set geometry table.
    if (side_code < 3U || side_code > 15U) {
        return {};
    }
    const auto layer_index = static_cast<std::size_t>(side_code - 3U);
    if (layer_index >= template_file.layers.size()) {
        return {};
    }
    return build_portal_wall_rects(template_file.layers[layer_index], side);
}

Vec3 building_cell_center_world(const SbbBuilding& building, const SbbCell& cell, float unit_size) {
    // BuildBuildingBlockRenderBuffers_1400799d0.c:271-276 moves every vertex by
    // -g_volCenter*cellSize to centre it. SBB anchors hold that centre.
    return {
        (static_cast<float>(cell.x) - static_cast<float>(building.anchor_x) + 0.5F) * unit_size,
        (static_cast<float>(cell.y) - static_cast<float>(building.anchor_y) + 0.5F) * unit_size,
        (static_cast<float>(cell.z) - static_cast<float>(building.anchor_z) + 0.5F) * unit_size,
    };
}

// -----------------------------------------------------------------------------
// Picking helpers
// -----------------------------------------------------------------------------

Vec3 add_scaled(Vec3 origin, Vec3 direction, float t) {
    return {
        origin.x + direction.x * t,
        origin.y + direction.y * t,
        origin.z + direction.z * t,
    };
}

std::optional<float> ray_aabb_intersection_t(Vec3 origin, Vec3 direction, Vec3 min, Vec3 max) {
    if (std::abs(direction.x) <= 0.000001F &&
        std::abs(direction.y) <= 0.000001F &&
        std::abs(direction.z) <= 0.000001F) {
        return std::nullopt;
    }

    auto t_min = 0.0F;
    auto t_max = std::numeric_limits<float>::max();
    auto intersect_slab = [&](float slab_min, float slab_max, float ray_origin, float ray_dir) {
        if (std::abs(ray_dir) <= 0.000001F) {
            return ray_origin >= slab_min && ray_origin <= slab_max;
        }
        auto near_t = (slab_min - ray_origin) / ray_dir;
        auto far_t = (slab_max - ray_origin) / ray_dir;
        if (near_t > far_t) {
            std::swap(near_t, far_t);
        }
        t_min = std::max(t_min, near_t);
        t_max = std::min(t_max, far_t);
        return t_min <= t_max;
    };

    if (!intersect_slab(min.x, max.x, origin.x, direction.x) ||
        !intersect_slab(min.y, max.y, origin.y, direction.y) ||
        !intersect_slab(min.z, max.z, origin.z, direction.z)) {
        return std::nullopt;
    }
    return t_min;
}

PortalWallRects build_portal_wall_rects_for_edge(const SblTemplate& template_file, const PortalSideCodes& side_codes) {
    return {
        wall_rects_for_side_code(template_file, side_codes.north, PortalSide::North),
        wall_rects_for_side_code(template_file, side_codes.west, PortalSide::West),
        wall_rects_for_side_code(template_file, side_codes.south, PortalSide::South),
        wall_rects_for_side_code(template_file, side_codes.east, PortalSide::East),
    };
}

struct CityTileKey {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};

    friend bool operator==(const CityTileKey&, const CityTileKey&) = default;
};

struct CityTileKeyHash {
    std::size_t operator()(const CityTileKey& key) const noexcept {
        auto value = static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.x));
        value ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.y)) + 0x9e3779b97f4a7c15ULL + (value << 6U) + (value >> 2U);
        value ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.z)) + 0x9e3779b97f4a7c15ULL + (value << 6U) + (value >> 2U);
        return static_cast<std::size_t>(value);
    }
};

struct CityTileValue {
    std::uint32_t block{};
    std::array<std::uint16_t, 8> faces{};
    CityTileKind kind{};
    float base_y{};
    float slope_x{};
    float slope_z{};
    std::int32_t street_index{-1};
    std::int32_t intersection_index{-1};
};

using CityTileMap = std::unordered_map<CityTileKey, CityTileValue, CityTileKeyHash>;

constexpr std::uint32_t kNativeSolidBlock = 0x40000000U;
constexpr std::uint32_t kNativeMeshBlock = 0x80000000U;
using NativeFaceArray = std::array<std::uint16_t, 8>;
constexpr NativeFaceArray kFacesZero{};
constexpr NativeFaceArray kFacesOne{1, 1, 1, 1, 1, 1, 1, 1};

// World_SetTileBlockAndResolveNeighborLinks @f830 overwrites the cell, so the last
// placement wins. The street owner is only editor data for picking: an
// intersection fill keeps the street that paved the cell first.
void set_city_tile(
    CityTileMap& tiles,
    std::int32_t x,
    std::int32_t y,
    std::int32_t z,
    std::uint32_t block,
    const NativeFaceArray& faces,
    CityTileKind kind,
    std::int32_t street_index = -1,
    float slope_x = 0.0F,
    float slope_z = 0.0F,
    float top_center = std::numeric_limits<float>::quiet_NaN()) {
    auto& tile = tiles[CityTileKey{x, y, z}];
    const auto owner = street_index >= 0 ? street_index : tile.street_index;
    // Cell y runs from y*4+0.25 to y*4+4.25. Sloped tiles report the slope at the
    // cell's centre, for the road outline and picking.
    const auto base_y = std::isnan(top_center) ? static_cast<float>(y) * 4.0F + 0.25F : top_center - 4.0F;
    tile = {block, faces, kind, base_y, slope_x, slope_z, owner};
}

// -----------------------------------------------------------------------------
// The game's city street graph
// (subrosa.exe World_LoadMapAndInitializeTerrain @56980 intersection loader and
// MapConnector_AddStreetSegment @38e0)
// -----------------------------------------------------------------------------

// The game's runtime intersection record. Axis names follow the editor's SbcMap:
// editor x = game unk0[1], editor z = game pos.x. The game's loader stores
// level = sbc y - 1, and the road surface you drive on is at level * 4.0 + 4.25
// (DAT_00278688 / DAT_00278698).
struct NativeCityIntersection {
    std::int32_t gx{};
    std::int32_t gz{};
    std::int32_t level{};
    std::int32_t ext_x_pos{};  // native field_0x2c: max left width of z-streets
    std::int32_t ext_x_neg{};  // native field_0x34: max right width of z-streets
    std::int32_t ext_z_pos{};  // native field_0x30: max right width of x-streets
    std::int32_t ext_z_neg{};  // native field_0x38: max left width of x-streets
    std::int32_t street_south{-1};  // street leaving toward +x (native streetSouth)
    std::int32_t street_north{-1};  // street arriving from -x
    std::int32_t street_west{-1};   // street leaving toward +z
    std::int32_t street_east{-1};   // street arriving from -z (native field_0x28)
};

struct NativeCityStreet {
    std::int32_t a{-1};
    std::int32_t b{-1};
    std::int32_t orientation{};  // 0 = runs along x, 1 = runs along z (native unk1[0])
    std::int32_t left_width{};   // native unk1[1] (startOffset)
    std::int32_t right_width{};  // native unk1[2] (endOffset)
    std::int32_t source_index{-1}; // index into SbcMap::streets this came from
    // The made-up name (MapConnector_AddStreetSegment @38e0): from the start
    // intersection, (gz - 256) / 32 picks First..Ninth Street along X, and
    // (gx - 256) / 56 picks Abrash..Holland Ave along Z. Positions outside those
    // ranges keep the zeroed field, which reads as First Street.
    std::int32_t name_index{};
};

struct NativeCityGraph {
    std::vector<NativeCityIntersection> intersections;
    std::vector<NativeCityStreet> streets;
};

float native_road_surface_y(std::int32_t level) {
    return static_cast<float>(level) * 4.0F + 4.25F;
}

NativeCityGraph build_native_city_graph(const SbcMap& map) {
    NativeCityGraph graph;
    graph.intersections.reserve(map.intersections.size());
    for (const auto& intersection : map.intersections) {
        NativeCityIntersection native;
        native.gx = intersection.x;
        native.gz = intersection.z;
        native.level = intersection.y - 1;
        graph.intersections.push_back(native);
    }

    for (std::size_t street_src = 0; street_src < map.streets.size(); ++street_src) {
        const auto ordered_street = canonical_city_street_endpoint_order(map, map.streets[street_src]);
        if (ordered_street.from_intersection < 0 || ordered_street.to_intersection < 0) {
            continue;
        }
        auto a = static_cast<std::size_t>(ordered_street.from_intersection);
        auto b = static_cast<std::size_t>(ordered_street.to_intersection);
        if (a >= graph.intersections.size() || b >= graph.intersections.size() || a == b) {
            continue;
        }

        const auto dx = graph.intersections[b].gx - graph.intersections[a].gx;
        const auto dz = graph.intersections[b].gz - graph.intersections[a].gz;
        const auto orientation = std::abs(dz) < std::abs(dx) ? 0 : 1;

        NativeCityStreet native;
        native.a = static_cast<std::int32_t>(a);
        native.b = static_cast<std::int32_t>(b);
        native.orientation = orientation;
        native.source_index = static_cast<std::int32_t>(street_src);
        native.left_width = std::max(0, ordered_street.left_width);
        native.right_width = std::max(0, ordered_street.right_width);
        if (orientation == 0) {
            const auto row = (graph.intersections[a].gz - 256) / 32;
            native.name_index = row >= 0 && row <= 8 ? row : 0;
        } else {
            const auto column = (graph.intersections[a].gx - 256) / 56;
            native.name_index = column >= 0 && column <= 7 ? 9 + column : 0;
        }
        const auto street_index = static_cast<std::int32_t>(graph.streets.size());
        auto& from = graph.intersections[a];
        auto& to = graph.intersections[b];
        if (orientation == 0) {
            from.street_south = street_index;
            to.street_north = street_index;
            from.ext_z_pos = std::max(from.ext_z_pos, native.right_width);
            from.ext_z_neg = std::max(from.ext_z_neg, native.left_width);
            to.ext_z_pos = std::max(to.ext_z_pos, native.right_width);
            to.ext_z_neg = std::max(to.ext_z_neg, native.left_width);
        } else {
            from.street_west = street_index;
            to.street_east = street_index;
            from.ext_x_pos = std::max(from.ext_x_pos, native.left_width);
            from.ext_x_neg = std::max(from.ext_x_neg, native.right_width);
            to.ext_x_pos = std::max(to.ext_x_pos, native.left_width);
            to.ext_x_neg = std::max(to.ext_x_neg, native.right_width);
        }
        graph.streets.push_back(native);
    }
    return graph;
}

// Growing the extents after placement (World_LoadMapAndInitializeTerrain @56980:
// every positive extent grows by one tile after the edge tiles are placed, before
// lane bounds and terrain patches are worked out).
void expand_native_city_extents(NativeCityGraph& graph) {
    for (auto& intersection : graph.intersections) {
        if (intersection.ext_x_pos > 0) {
            intersection.ext_x_pos += 1;
        }
        if (intersection.ext_x_neg > 0) {
            intersection.ext_x_neg += 1;
        }
        if (intersection.ext_z_pos > 0) {
            intersection.ext_z_pos += 1;
        }
        if (intersection.ext_z_neg > 0) {
            intersection.ext_z_neg += 1;
        }
    }
}

// The corner-stamp height grid (UpdateMapTopHeightAndClearOccupancyBit @3fa40).
// One float per tile-grid corner, which only ever goes up. A tile counts as inside a
// road for the terrain patch when all four of its corners are stamped above 0.
struct CityCornerStampMap {
    std::unordered_map<std::uint64_t, float> heights;
    // Land-use mask (g_landUseMaskBits): cells whose bit a stamp cleared. The
    // landscape shader throws away terrain there (landscape.frag, land_usemask).
    // Only cells inside the intersections' bounding box are cleared.
    std::unordered_set<std::uint64_t> masked;
    std::int32_t mask_min_x{}, mask_max_x{-1}, mask_min_z{}, mask_max_z{-1};
    // isMapAutoGenerated: UpdateMapTopHeightAndClearOccupancyBit does nothing.
    bool disabled{};

    static std::uint64_t key(std::int32_t gx, std::int32_t gz) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(gx)) << 32U) |
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(gz));
    }

    // UpdateMapTopHeightAndClearOccupancyBit @3fa40.
    void stamp(std::int32_t gx, std::int32_t gz, float height) {
        if (disabled || static_cast<std::uint32_t>(gx) >= 0xfffU || static_cast<std::uint32_t>(gz) >= 0xfffU) return;
        if (gx >= mask_min_x && gx <= mask_max_x && gz >= mask_min_z && gz <= mask_max_z) masked.insert(key(gx, gz));
        const std::array<std::pair<std::int32_t, std::int32_t>, 4> corners{{
            {gx, gz}, {gx + 1, gz}, {gx, gz + 1}, {gx + 1, gz + 1},
        }};
        for (const auto& [cx, cz] : corners) {
            auto& slot = heights[key(cx, cz)];
            slot = std::max(slot, height);
        }
    }

    float at(std::int32_t gx, std::int32_t gz) const {
        const auto match = heights.find(key(gx, gz));
        return match == heights.end() ? 0.0F : match->second;
    }
};

// District and piece cells stamp the corner-height grid too
// (ApplyAllCityDistrictCellsToWorld @20000): every filled cell of an 8^3 district
// volume stamps its tile at the cell's own level * 4. The editor's portals are
// those district volumes: fields[1..3] are the position in 8-cell units, and the
// voxel index is y*64 + z*8 + x.
void stamp_city_portal_cells(CityCornerStampMap& stamps, const SbcMap& map) {
    for (const auto& portal : map.portals) {
        const auto& voxels = portal.voxel_blocks();
        const auto block_count = std::min<std::size_t>(voxels.size(), 0x200);
        for (std::size_t voxel_index = 0; voxel_index < block_count; ++voxel_index) {
            if (voxels[voxel_index] == 0U) {
                continue;
            }
            const auto local_x = static_cast<std::int32_t>(voxel_index & 7U);
            const auto local_z = static_cast<std::int32_t>((voxel_index >> 3U) & 7U);
            const auto local_y = static_cast<std::int32_t>((voxel_index >> 6U) & 7U);
            stamps.stamp(
                sbc_portal_cell_base(portal.fields[1]) + local_x,
                sbc_portal_cell_base(portal.fields[3]) + local_z,
                static_cast<float>(sbc_portal_cell_base(portal.fields[2]) + local_y) * 4.0F);
        }
    }
}

// Placing a building stamps every filled cell of the building's ANCHOR layer at the
// placement record's y * 4 (InstantiateSbbVolumeIntoWorldAndMetadata @23d00 only
// calls UpdateMapTopHeight when the local layer is the anchor layer). Each cell's
// tile is found with the checked city placement transform (rotation around the
// anchor), using the rotated centre of the cell.
void stamp_city_building_footprints(
    CityCornerStampMap& stamps,
    const SbcMap& map,
    const std::vector<const SbbBuilding*>& placement_buildings) {
    for (std::size_t index = 0; index < map.buildings.size() && index < placement_buildings.size(); ++index) {
        const auto* building = placement_buildings[index];
        if (building == nullptr) {
            continue;
        }
        const auto& placement = map.buildings[index];
        const auto height = static_cast<float>(placement.y) * 4.0F;
        for (const auto& cell : building->cells) {
            // The anchor layer's placed cells (the ones with an exterior block word).
            if (cell.exterior_block() == 0U || cell.y != building->anchor_y) {
                continue;
            }
            const Vec3 local{
                (static_cast<float>(cell.x) - static_cast<float>(building->anchor_x) + 0.5F) * 4.0F,
                0.0F,
                (static_cast<float>(cell.z) - static_cast<float>(building->anchor_z) + 0.5F) * 4.0F,
            };
            const auto world = native_city_building_transform_point(placement, local, 4.0F);
            stamps.stamp(
                static_cast<std::int32_t>(std::floor(world.x / 4.0F)),
                static_cast<std::int32_t>(std::floor(world.z / 4.0F)),
                height);
        }
    }
}

// Later writes to the world replace street cells. Streets are placed first. Then
// district cells (ApplyAllCityDistrictCellsToWorld @20000) overwrite their cell,
// and InstantiateSbbVolumeIntoWorldAndMetadata @23ac0 deletes the block in every
// filled cell of the building before writing the building's own.
void remove_replaced_city_tiles(
    std::vector<CityTileCell>& tiles,
    const SbcMap& map,
    const std::vector<const SbbBuilding*>& placement_buildings) {
    std::unordered_set<CityTileKey, CityTileKeyHash> replaced;
    for (const auto& portal : map.portals) {
        const auto& voxels = portal.voxel_blocks();
        const auto count = std::min<std::size_t>(voxels.size(), 0x200);
        for (std::size_t i = 0; i < count; ++i) {
            if (voxels[i] == 0U) continue;
            replaced.insert({sbc_portal_cell_base(portal.fields[1]) + static_cast<std::int32_t>(i & 7U),
                sbc_portal_cell_base(portal.fields[2]) + static_cast<std::int32_t>((i >> 6U) & 7U),
                sbc_portal_cell_base(portal.fields[3]) + static_cast<std::int32_t>((i >> 3U) & 7U)});
        }
    }
    for (std::size_t index = 0; index < map.buildings.size() && index < placement_buildings.size(); ++index) {
        const auto* building = placement_buildings[index];
        if (building == nullptr) continue;
        const auto& placement = map.buildings[index];
        for (const auto& cell : building->cells) {
            // Only cells with an exterior block word are deleted and rewritten.
            if (cell.exterior_block() == 0U) continue;
            const Vec3 local{
                (static_cast<float>(cell.x) - static_cast<float>(building->anchor_x) + 0.5F) * 4.0F,
                0.0F,
                (static_cast<float>(cell.z) - static_cast<float>(building->anchor_z) + 0.5F) * 4.0F,
            };
            const auto world = native_city_building_transform_point(placement, local, 4.0F);
            replaced.insert({static_cast<std::int32_t>(std::floor(world.x / 4.0F)),
                placement.y + static_cast<std::int32_t>(cell.y) - static_cast<std::int32_t>(building->anchor_y),
                static_cast<std::int32_t>(std::floor(world.z / 4.0F))});
        }
    }
    if (replaced.empty()) return;
    std::erase_if(tiles, [&](const CityTileCell& tile) { return replaced.contains({tile.x, tile.y, tile.z}); });
}

// Replays how the game fills its world storage (GameWorldFit), in load order, and
// removes from `tiles` the road tiles that don't fit.
// ponytail: a tile only keeps its last writer, so each street's tiles are replayed
// in order along the street rather than in the game's exact write order. The only
// difference is which cells survive on the one street that fills the storage up.
GameWorldFit fit_to_game_world(
    std::vector<CityTileCell>& tiles,
    const NativeCityGraph& graph,
    const SbcMap& map,
    const std::vector<const SbbBuilding*>& placement_buildings) {
    GameWorldFit fit;
    std::unordered_set<CityTileKey, CityTileKeyHash> sectors, chunks;
    enum class Placed { Ok, Outside, NoRoom };
    const auto place = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        if (!city_cell_in_game_world(x, y, z)) return Placed::Outside;
        if (sectors.contains({x >> 3, y >> 3, z >> 3})) return Placed::Ok;
        if (!chunks.contains({x >> 6, y >> 6, z >> 6})) {
            if (chunks.size() >= GameWorldFit::max_chunks) return Placed::NoRoom;
            chunks.insert({x >> 6, y >> 6, z >> 6});
        }
        if (sectors.size() >= GameWorldFit::max_sectors) return Placed::NoRoom;
        sectors.insert({x >> 3, y >> 3, z >> 3});
        return Placed::Ok;
    };

    // Street tiles street by street, then junction tiles (intersection index -1 last).
    std::vector<std::size_t> order(tiles.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    const auto rank = [&](const CityTileCell& t) {
        const bool street = t.intersection_index < 0 && t.street_index >= 0 &&
            static_cast<std::size_t>(t.street_index) < graph.streets.size();
        const auto along = !street ? 0 : graph.streets[static_cast<std::size_t>(t.street_index)].orientation == 0 ? t.x : t.z;
        return std::tuple{street ? 0 : 1, street ? t.street_index : t.intersection_index, along};
    };
    std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) { return rank(tiles[a]) < rank(tiles[b]); });
    std::vector<bool> dropped(tiles.size());
    for (const auto i : order) {
        const auto placed = place(tiles[i].x, tiles[i].y, tiles[i].z);
        dropped[i] = placed != Placed::Ok;
        if (placed == Placed::Outside) ++fit.road_tiles_outside;
        if (placed == Placed::NoRoom) ++fit.road_tiles_no_room;
    }

    for (const auto& portal : map.portals) {
        const auto& voxels = portal.voxel_blocks();
        for (std::size_t i = 0; i < std::min<std::size_t>(voxels.size(), 0x200); ++i) {
            if (voxels[i] == 0U) continue;
            place(sbc_portal_cell_base(portal.fields[1]) + static_cast<std::int32_t>(i & 7U),
                sbc_portal_cell_base(portal.fields[2]) + static_cast<std::int32_t>((i >> 6U) & 7U),
                sbc_portal_cell_base(portal.fields[3]) + static_cast<std::int32_t>((i >> 3U) & 7U));
        }
    }

    fit.building_cells.resize(map.buildings.size());
    for (std::size_t index = 0; index < map.buildings.size() && index < placement_buildings.size(); ++index) {
        const auto* building = placement_buildings[index];
        if (building == nullptr) continue;
        const auto& placement = map.buildings[index];
        for (std::size_t c = 0; c < building->cells.size(); ++c) {
            const auto& cell = building->cells[c];
            if (!cell.occupied) continue;
            const Vec3 local{
                (static_cast<float>(cell.x) - static_cast<float>(building->anchor_x) + 0.5F) * 4.0F,
                0.0F,
                (static_cast<float>(cell.z) - static_cast<float>(building->anchor_z) + 0.5F) * 4.0F,
            };
            const auto world = native_city_building_transform_point(placement, local, 4.0F);
            const auto placed = place(static_cast<std::int32_t>(std::floor(world.x / 4.0F)),
                placement.y + static_cast<std::int32_t>(cell.y) - static_cast<std::int32_t>(building->anchor_y),
                static_cast<std::int32_t>(std::floor(world.z / 4.0F)));
            if (placed == Placed::Ok) continue;
            fit.building_cells[index].push_back(c);
            ++(placed == Placed::Outside ? fit.building_cells_outside : fit.building_cells_no_room);
        }
    }

    std::size_t i = 0;
    std::erase_if(tiles, [&](const CityTileCell&) { return dropped[i++]; });
    fit.sectors_used = sectors.size();
    fit.chunks_used = chunks.size();
    return fit;
}

} // namespace

// For each placement, the cells a later placement overwrites. The game deletes the
// world cell under every building cell with an exterior block word before writing
// its own (InstantiateSbbVolumeIntoWorldAndMetadata @23ac0), so where placements
// overlap, the later one wins.
std::vector<std::vector<std::size_t>> overwritten_building_cells(
    const SbcMap& map,
    const std::vector<const SbbBuilding*>& placement_buildings) {
    std::vector<std::vector<std::size_t>> overwritten(map.buildings.size());
    std::unordered_set<CityTileKey, CityTileKeyHash> claimed;
    for (std::size_t index = std::min(map.buildings.size(), placement_buildings.size()); index-- > 0;) {
        const auto* building = placement_buildings[index];
        if (building == nullptr) continue;
        const auto& placement = map.buildings[index];
        for (std::size_t c = 0; c < building->cells.size(); ++c) {
            const auto& cell = building->cells[c];
            if (!cell.occupied || cell.exterior_block() == 0U) continue;
            const Vec3 local{
                (static_cast<float>(cell.x) - static_cast<float>(building->anchor_x) + 0.5F) * 4.0F,
                0.0F,
                (static_cast<float>(cell.z) - static_cast<float>(building->anchor_z) + 0.5F) * 4.0F,
            };
            const auto world = native_city_building_transform_point(placement, local, 4.0F);
            const CityTileKey key{static_cast<std::int32_t>(std::floor(world.x / 4.0F)),
                placement.y + static_cast<std::int32_t>(cell.y) - static_cast<std::int32_t>(building->anchor_y),
                static_cast<std::int32_t>(std::floor(world.z / 4.0F))};
            if (!claimed.insert(key).second) overwritten[index].push_back(c);
        }
    }
    return overwritten;
}

std::vector<PlacementOverrideRenderBuffer> build_placement_override_render_buffers(
    const SbcMap& map, const std::vector<const SbbBuilding*>& placement_buildings,
    const std::vector<NamedBlockMesh>& blocks, const std::vector<NamedBuildblockTemplate>& templates,
    const std::vector<std::vector<std::size_t>>& dropped_cells) {
    PAFTA_PROFILE_FUNCTION();
    auto overwritten = overwritten_building_cells(map, placement_buildings);
    std::vector<PlacementOverrideRenderBuffer> built;
    for (std::size_t i = 0; i < overwritten.size(); ++i) {
        const auto later_count = overwritten[i].size();
        const auto dropped_count = i < dropped_cells.size() ? dropped_cells[i].size() : 0U;
        if (later_count == 0U && dropped_count == 0U) continue;
        const auto* source = placement_buildings.at(i);
        if (source == nullptr) throw std::runtime_error("Placement override has hidden cells but no building asset: " + map.buildings[i].name);
        auto building = *source;
        for (const auto cell : overwritten[i]) building.cells.at(cell).occupied = false;
        if (i < dropped_cells.size()) {
            for (const auto cell : dropped_cells[i]) building.cells.at(cell).occupied = false;
        }
        auto buffer = build_city_building_full_render_buffer(building, blocks, templates, BuildingLod::Far);
        bake_shadow_volume(buffer);
        built.push_back({i, std::move(buffer), later_count, dropped_count});
    }
    return built;
}

namespace {

// A street's length between the junction footprints, its change in level, and how
// many cells it takes per level of slope, worked out the same way as
// MapConnector_PlaceStreetSegmentTilesAndSlopes.
StreetRun native_street_run(const NativeCityGraph& graph, const NativeCityStreet& street, bool auto_generated) {
    const auto& from = graph.intersections[static_cast<std::size_t>(street.a)];
    const auto& to = graph.intersections[static_cast<std::size_t>(street.b)];
    const auto along_x = street.orientation == 0;
    const auto start = along_x ? from.gx + from.ext_x_pos : from.gz + from.ext_z_pos;
    StreetRun run;
    run.length = (along_x ? to.gx - to.ext_x_neg : to.gz - to.ext_z_neg) - start;
    run.climb = to.level - from.level;
    const auto abs_delta = std::abs(run.climb);
    // ponytail: the game divides by zero when a street is shorter than its climb.
    // We set the run length to 1 instead of copying the crash.
    run.cells_per_level = abs_delta > 0 ? std::max(1, run.length / abs_delta) : 6;
    // isMapAutoGenerated (the "round" map) uses a short fixed run instead: 6 cells
    // per level, or 10 when the street is at least 10x longer than its climb. So the
    // street runs flat, slopes in the middle, then runs flat again.
    if (auto_generated) run.cells_per_level = abs_delta > 0 && abs_delta <= run.length / 10 ? 10 : 6;
    return run;
}

// Lays a street's tiles (MapConnector_PlaceStreetSegmentTilesAndSlopes @20580).
// Lanes are solid cells at the current level, with the lane marking code in face
// slot 5 (the top). Inside the sloped stretch in the middle, they become slope
// blocks from the aa14 table, with the code also in slot 0 (the slope quad's
// material). Curbs are the curb block one level up, and inside the slope they are
// slope side blocks from the ba14 table at the lane level.
// Branch 0 (orientation 0) walks along +X with lanes spread in Z and uses table
// edges 0 (up) and 2 (down). Branch 1 walks along +Z with lanes in X and uses
// edges 1 and 3.
void place_native_street_tiles(
    CityTileMap& tiles,
    CityCornerStampMap& stamps,
    const NativeCityGraph& graph,
    const NativeCityStreet& street,
    bool auto_generated) {
    static const auto grade_slots = native_grade_slot_table();
    if (street.a < 0 || street.b < 0) {
        return;
    }
    const auto& from = graph.intersections[static_cast<std::size_t>(street.a)];
    const auto& to = graph.intersections[static_cast<std::size_t>(street.b)];
    const auto along_x = street.orientation == 0;

    const auto start = along_x ? from.gx + from.ext_x_pos : from.gz + from.ext_z_pos;
    const auto center = along_x ? from.gz : from.gx;
    const auto run = native_street_run(graph, street, auto_generated);
    const auto seg_len = run.length;
    const auto delta = run.climb;
    const auto abs_delta = std::abs(delta);
    const auto run_len = run.cells_per_level;
    const auto half = (abs_delta * run_len) >> 1;
    const auto slope_start = (seg_len >> 1) - half;
    const auto slope_end = (seg_len >> 1) + half;

    // Uphill reads edge 0/1 at n*64+runPos. Downhill reads the table 31/47 ints past
    // its start at n*65-runPos (edge 2/3, with the steps reversed). Reads past the end
    // of the table give id 0, like the zeroed globals the game would read.
    const auto grade_block = [&](bool side, std::int32_t run_pos) {
        const auto base = (side ? 1024 : 0) + (along_x ? 0 : 16) + (delta > 0 ? 0 : 31);
        const auto index = base + (delta > 0 ? run_len * 64 + run_pos : run_len * 65 - run_pos);
        const auto id = index >= 0 && index < static_cast<std::int32_t>(grade_slots.size())
            ? grade_slots[static_cast<std::size_t>(index)]
            : 0;
        return kNativeMeshBlock | static_cast<std::uint32_t>(id);
    };
    const auto place = [&](std::int32_t along_coord, std::int32_t cross_coord, std::int32_t level, std::uint32_t block,
                           const NativeFaceArray& faces, CityTileKind kind, float grade = 0.0F,
                           float top_center = std::numeric_limits<float>::quiet_NaN()) {
        const auto x = along_x ? along_coord : cross_coord;
        const auto z = along_x ? cross_coord : along_coord;
        set_city_tile(tiles, x, level, z, block, faces, kind, street.source_index,
            along_x ? grade : 0.0F, along_x ? 0.0F : grade, top_center);
    };

    const std::uint16_t center_code = along_x ? 0x4101 : 0x4601;
    const std::uint16_t left_code = along_x ? 0x4201 : 0x4701;
    const std::uint16_t neg_code = along_x ? 0x4501 : 0x4a01;
    const std::uint16_t pos_code = along_x ? 0x4401 : 0x4901;
    const auto curb = kNativeMeshBlock | native_curb_block_id;
    const auto left_curb = center - street.left_width - 1;
    const auto right_curb = center + street.right_width;

    auto height = from.level;
    auto run_pos = 0;
    auto last_faces = kFacesOne;
    for (auto along = 0; along < seg_len; ++along) {
        const auto in_slope = slope_start <= along && along < slope_end;
        if (in_slope && delta > 0 && run_pos % run_len == 0) {
            height += 1;
        }
        const auto coord = start + along;
        // The slope of this cell's surface, for the outline and picking.
        const auto step = 4.0F / static_cast<float>(run_len);
        const auto grade = in_slope ? (delta > 0 ? step : -step) : 0.0F;
        const auto k = delta > 0 ? run_pos : run_len - 1 - run_pos;
        const auto top_center = static_cast<float>(height) * 4.0F + 0.25F + (static_cast<float>(k) + 0.5F) * step;

        if (!in_slope) {
            place(coord, left_curb, height + 1, curb, kFacesZero, CityTileKind::Curb);
            place(coord, right_curb, height + 1, curb, kFacesZero, CityTileKind::Curb);
        } else {
            const auto side = grade_block(true, run_pos);
            place(coord, left_curb, height, side, kFacesZero, CityTileKind::Curb, grade, top_center + 0.25F);
            place(coord, right_curb, height, side, kFacesZero, CityTileKind::Curb, grade, top_center + 0.25F);
        }

        auto faces = kFacesOne;
        for (auto offset = -street.left_width; offset < street.right_width; ++offset) {
            std::uint16_t code = 1;
            if (offset == -1) {
                code = left_code;
            } else if (offset == 0) {
                code = center_code;
            } else if (offset < -1) {
                code = street.left_width - 1 < -offset ? 1 : neg_code;
            } else if (offset >= 1 && offset < street.right_width - 1) {
                code = pos_code;
            }
            faces[5] = code;
            if (!in_slope) {
                place(coord, center + offset, height, kNativeSolidBlock, faces, CityTileKind::Road);
            } else {
                faces[0] = code;
                place(coord, center + offset, height, grade_block(false, run_pos), faces, CityTileKind::Road, grade, top_center);
            }
        }
        last_faces = faces;

        // The terrain-top stamp covers curb to curb (the game's ~leftWidth .. rightWidth).
        for (auto offset = -street.left_width - 1; offset <= street.right_width; ++offset) {
            const auto sx = along_x ? coord : center + offset;
            const auto sz = along_x ? center + offset : coord;
            stamps.stamp(sx, sz, static_cast<float>(height) * 4.0F);
        }

        if (in_slope) {
            if (delta < 0 && run_pos % run_len == run_len - 1) {
                height -= 1;
            }
            run_pos = (run_pos + 1) % run_len;
        }
    }

    // Stop lines: the lane rows touching each end become solid cells with the end-cap
    // code, when that end has a cross street (extents measured before the +1 growth).
    // The rest of the face array is whatever the last lane wrote.
    last_faces[5] = along_x ? 0x4801 : 0x4301;
    const auto from_cap = along_x
        ? from.ext_x_pos > 0 && (from.ext_z_pos > 0 || from.ext_z_neg > 0)
        : from.ext_z_pos > 0 && (from.ext_x_pos > 0 || from.ext_x_neg > 0);
    const auto to_cap = along_x
        ? to.ext_x_neg > 0 && (to.ext_z_pos > 0 || to.ext_z_neg > 0)
        : to.ext_z_neg > 0 && (to.ext_x_pos > 0 || to.ext_x_neg > 0);
    for (auto cross = center - street.left_width; cross < center + street.right_width; ++cross) {
        if (from_cap && slope_start > 0) {
            place(start, cross, from.level, kNativeSolidBlock, last_faces, CityTileKind::Road);
        }
        if (to_cap && slope_end < seg_len) {
            place(along_x ? to.gx - to.ext_x_neg - 1 : to.gz - to.ext_z_neg - 1, cross, to.level,
                kNativeSolidBlock, last_faces, CityTileKind::Road);
        }
    }
}

// An intersection's footprint and edges (MapConnector_PlaceIntersectionPerimeterTiles
// @20130): solid asphalt over its uneven extents. Sides with no street get curb rows
// one level up, reaching one tile past both ends.
void place_native_intersection_tiles(CityTileMap& tiles, const NativeCityIntersection& intersection, std::int32_t index) {
    // Every tile this junction writes is tagged with it, for its outline.
    const auto tag = [&](std::int32_t x, std::int32_t y, std::int32_t z) { tiles[CityTileKey{x, y, z}].intersection_index = index; };
    for (auto z = intersection.gz - intersection.ext_z_neg; z < intersection.gz + intersection.ext_z_pos; ++z) {
        for (auto x = intersection.gx - intersection.ext_x_neg; x < intersection.gx + intersection.ext_x_pos; ++x) {
            set_city_tile(tiles, x, intersection.level, z, kNativeSolidBlock, kFacesOne, CityTileKind::Road);
            tag(x, intersection.level, z);
        }
    }

    const auto curb = kNativeMeshBlock | native_curb_block_id;
    const auto curb_level = intersection.level + 1;
    const auto z_row_begin = intersection.gz - intersection.ext_z_neg - 1;
    const auto z_row_end = intersection.gz + intersection.ext_z_pos;  // inclusive
    const auto x_row_begin = intersection.gx - intersection.ext_x_neg - 1;
    const auto x_row_end = intersection.gx + intersection.ext_x_pos;  // inclusive
    const auto place_curb = [&](std::int32_t x, std::int32_t z) {
        // A ramp can reach the junction with no flat cell left. Its side mesh
        // already joins the curb; an extra flat corner plate floats over it.
        for (const auto y : {curb_level, curb_level - 1}) {
            const auto found = tiles.find({x, y, z});
            if (found != tiles.end() && found->second.kind == CityTileKind::Curb &&
                (found->second.slope_x != 0.0F || found->second.slope_z != 0.0F)) return;
        }
        set_city_tile(tiles, x, curb_level, z, curb, kFacesZero, CityTileKind::Curb);
        tag(x, curb_level, z);
    };
    if (intersection.street_south < 0) {
        const auto x = intersection.gx + intersection.ext_x_pos;
        for (auto z = z_row_begin; z <= z_row_end; ++z) {
            place_curb(x, z);
        }
    }
    if (intersection.street_north < 0) {
        // A quirk of the game: the -x row offset uses ext_x_pos (field_0x2c), not the
        // -x extent.
        const auto x = intersection.gx - intersection.ext_x_pos - 1;
        for (auto z = z_row_begin; z <= z_row_end; ++z) {
            place_curb(x, z);
        }
    }
    if (intersection.street_west < 0) {
        const auto z = intersection.gz + intersection.ext_z_pos;
        for (auto x = x_row_begin; x <= x_row_end; ++x) {
            place_curb(x, z);
        }
    }
    if (intersection.street_east < 0) {
        const auto z = intersection.gz - intersection.ext_z_neg - 1;
        for (auto x = x_row_begin; x <= x_row_end; ++x) {
            place_curb(x, z);
        }
    }
}


} // namespace

// Removes junctions that no road uses. The game still lays a junction's tiles
// (MapConnector_PlaceIntersectionPerimeterTiles), so a leftover one shows as a slab.
void remove_unused_city_intersections(SbcMap& map) {
    std::vector<bool> used(map.intersections.size());
    for (const auto& street : map.streets) {
        for (const auto end : {street.from_intersection, street.to_intersection}) {
            if (end >= 0 && static_cast<std::size_t>(end) < used.size()) used[static_cast<std::size_t>(end)] = true;
        }
    }
    std::vector<std::int32_t> remap(map.intersections.size(), -1);
    std::size_t kept = 0;
    for (std::size_t i = 0; i < map.intersections.size(); ++i) {
        if (!used[i]) continue;
        remap[i] = static_cast<std::int32_t>(kept);
        map.intersections[kept] = map.intersections[i];
        map.intersections[kept].index = kept;
        ++kept;
    }
    map.intersections.resize(kept);
    for (auto& street : map.streets) {
        for (auto* end : {&street.from_intersection, &street.to_intersection}) {
            if (*end >= 0 && static_cast<std::size_t>(*end) < remap.size()) *end = remap[static_cast<std::size_t>(*end)];
        }
    }
}

SbcStreet canonical_city_street_endpoint_order(const SbcMap& map, SbcStreet street) {
    if (street.from_intersection < 0 || street.to_intersection < 0) {
        return street;
    }

    const auto a = static_cast<std::size_t>(street.from_intersection);
    const auto b = static_cast<std::size_t>(street.to_intersection);
    if (a >= map.intersections.size() || b >= map.intersections.size() || a == b) {
        return street;
    }

    const auto& from = map.intersections[a];
    const auto& to = map.intersections[b];
    const auto dx = to.x - from.x;
    const auto dz = to.z - from.z;
    const auto along_x = std::abs(dz) < std::abs(dx);
    if ((along_x && dx < 0) || (!along_x && dz < 0)) {
        std::swap(street.from_intersection, street.to_intersection);
    }
    return street;
}

float terrain_noise_sample(float x, float z, float frequency_x, float frequency_z) {
    const auto sx = x * frequency_x;
    const auto sz = z * frequency_z;
    const auto x0 = static_cast<std::int32_t>(std::floor(sx));
    const auto z0 = static_cast<std::int32_t>(std::floor(sz));
    const auto tx = sx - static_cast<float>(x0);
    const auto tz = sz - static_cast<float>(z0);
    const auto ux = tx * tx * (3.0F - 2.0F * tx);
    const auto uz = tz * tz * (3.0F - 2.0F * tz);
    const auto normalized_hash = [](std::int32_t hx, std::int32_t hz) {
        return static_cast<float>(terrain_noise_hash(hx, hz, 0xffffffffU)) * 0x1p-30F;
    };
    const auto n00 = normalized_hash(x0, z0);
    const auto n10 = normalized_hash(x0 + 1, z0);
    const auto n01 = normalized_hash(x0, z0 + 1);
    const auto n11 = normalized_hash(x0 + 1, z0 + 1);
    return lerp(lerp(n00, n10, ux), lerp(n01, n11, ux), uz);
}

float native_like_city_terrain_height(float world_x, float world_z, bool city_map) {
    // The game's runtime terrain samples use grid = world + 4096, one sample per world
    // unit. The game scales the 2049^2 source heightfield up 8x
    // (Terrain_RebuildLandTilesRoadPatchesAndChunkCaches @d12e0). The bilinear corners
    // are H[col-1..col] in X but H[row..row+1] in Z, so the smooth source coordinate is
    // shifted by one sample on the X axis only.
    const auto gx = (world_x + 4088.0F) * 0.125F;
    const auto gz = (world_z + 4096.0F) * 0.125F;
    const auto dx1 = gx - 640.0F;
    const auto dz1 = gz - 640.0F;
    const auto r1 = std::clamp(std::sqrt(dx1 * dx1 + dz1 * dz1) * 0.00390625F - 0.75F, 0.0F, 3.0F);
    const auto r1sq = r1 * r1;

    const auto noise1 = terrain_noise_sample(gx, gz, 1.0F / 64.0F, 1.0F / 128.0F);
    const auto noise2 = terrain_noise_sample(gx, gz, 1.0F / 128.0F, 1.0F / 128.0F);
    const auto noise3 = terrain_noise_sample(gx, gz, 1.0F / 64.0F, 1.0F / 64.0F);

    const auto dx2 = gx - 704.0F;
    const auto dz2 = gz - 704.0F;
    const auto r2 = std::clamp((std::sqrt(dx2 * dx2 + dz2 * dz2) - 320.0F) * 0.015625F, 0.0F, 1.0F);

    const auto base_blend =
        noise3 * 8.0F + 23.8125F + (noise1 + r1sq) * 16.0F + (noise2 * 32.0F + r1sq * 8.0F) * r1sq;
    auto height = base_blend * r2 + (1.0F - r2) * 23.875F;

    // Cutting the shoreline on city maps (Terrain_GenerateIslandHeightfield @56560,
    // isMapAutoGenerated == 0): an L-shaped shallow water shelf at 11.75 north-west of
    // the city, blended up with the cubic ramp (36 - 24e) * e^2 + 11.75 over four
    // source samples at the edge.
    const auto min1 = [](float value) { return std::min(value, 1.0F); };
    const auto shore = [&](float edge) { return (36.0F - edge * 24.0F) * edge * edge + 11.75F; };
    if (!city_map) return height;
    if (gz <= 637.0F && gx < 768.0F) {
        const auto edge = clamp01(std::max(
            1.0F - min1((638.0F - gz) * 0.25F),
            1.0F - min1((768.0F - gx) * 0.25F)));
        height = shore(edge);
    }
    if (gx < 638.0F && gz < 768.0F) {
        const auto column_term = gz <= 637.0F ? 0.0F : 1.0F - min1((638.0F - gx) * 0.25F);
        const auto edge = clamp01(std::max(column_term, 1.0F - min1((768.0F - gz) * 0.25F)));
        height = shore(edge);
    }
    return height;
}

namespace {

constexpr auto city_terrain_mesh_y_offset = 0.05F;

// Runs row(0..rows-1) across the CPU cores (in order in the web build).
template <typename F>
void parallel_rows(std::int32_t rows, F&& row) {
#ifdef __EMSCRIPTEN__
    for (std::int32_t r = 0; r < rows; ++r) row(r);
#else
    if (rows <= 0) return;
    std::atomic<std::int32_t> next{0};
    const auto count = std::min(static_cast<unsigned>(rows), std::max(1U, std::thread::hardware_concurrency()));
    // Futures also wait while the stack unwinds, so row's captured state stays alive if
    // a worker fails. get() passes that failure on to the scene builder.
    std::vector<std::future<void>> workers;
    workers.reserve(count);
    for (unsigned worker = 0; worker < count; ++worker) {
        workers.push_back(std::async(std::launch::async, [&] {
            for (auto r = next.fetch_add(1); r < rows; r = next.fetch_add(1)) row(r);
        }));
    }
    for (auto& worker : workers) worker.get();
#endif
}

// A terrain height grid at one world unit per sample over the city, indexed by whole
// world coordinates (the game's land-sample grid without its +4096 offset). Only
// patched samples are stored. Everything else is the island height, worked out when
// asked (a full grid for a long map would be tens of millions of samples).
struct CityHeightfield {
    static constexpr std::int32_t patch_side = 64;
    using Patch = std::array<float, patch_side * patch_side>;
    std::int32_t min_x{};
    std::int32_t min_z{};
    std::int32_t count_x{};
    std::int32_t count_z{};
    bool city_map{true};
    // Patches write contiguous rectangles. Store 64x64 sample pages instead of
    // allocating one hash node per sample (millions on a normal city map).
    std::unordered_map<std::size_t, Patch> patched;

    bool contains(std::int32_t x, std::int32_t z) const {
        return x >= min_x && z >= min_z && x < min_x + count_x && z < min_z + count_z;
    }

    std::size_t patch_index(std::int32_t x, std::int32_t z) const {
        const auto columns = (count_x + patch_side - 1) / patch_side;
        return static_cast<std::size_t>((z - min_z) / patch_side) * static_cast<std::size_t>(columns) +
            static_cast<std::size_t>((x - min_x) / patch_side);
    }

    std::size_t sample_index(std::int32_t x, std::int32_t z) const {
        return static_cast<std::size_t>((z - min_z) % patch_side) * patch_side +
            static_cast<std::size_t>((x - min_x) % patch_side);
    }

    float get(std::int32_t x, std::int32_t z) const {
        if (!contains(x, z)) return 0.0F;
        const auto found = patched.find(patch_index(x, z));
        if (found != patched.end()) {
            const auto height = found->second[sample_index(x, z)];
            if (!std::isnan(height)) return height;
        }
        return native_like_city_terrain_height(static_cast<float>(x), static_cast<float>(z), city_map);
    }

    void set(std::int32_t x, std::int32_t z, float height) {
        if (contains(x, z)) {
            auto [entry, inserted] = patched.try_emplace(patch_index(x, z));
            if (inserted) entry->second.fill(std::numeric_limits<float>::quiet_NaN());
            entry->second[sample_index(x, z)] = height;
        }
    }
};

// The game patches a fixed list of intersection numbers on every map that is not
// auto-generated (Terrain_RebuildLandTilesRoadPatchesAndChunkCaches @d12e0). The
// list is the same whatever the map contains.
constexpr std::array<std::int32_t, 28> native_city_terrain_patch_indices{
    2, 40, 32, 46, 45, 47, 28, 10, 33, 42, 35, 13, 59, 31,
    52, 48, 50, 19, 22, 58, 62, 61, 41, 36, 43, 44, 38, 39,
};

struct CityPatchCandidate {
    std::int32_t px{};
    std::int32_t qz{};
    float weight{};
};

struct CityPatchCell {
    std::int32_t count{};
    std::array<CityPatchCandidate, 2> entries{};
};

// Port of Terrain_ApplyStreetIntersectionHeightPatch @d3340. Expects the extents
// after the +1 growth. Phase 1 writes a bilinear surface between the four corner
// intersections of the block. Phase 2 resets road tiles (all four mapTopHeight
// corner stamps > 0) to the stamped road heights. Phase 3 spreads those seed
// positions outward for 16 passes with weight 1 - dist/16 (up to two candidates per
// sample, filtered by direction). Phase 4 smoothstep-blends every sample reached
// toward its seed heights.
void apply_native_city_terrain_patch(
    CityHeightfield& field,
    const NativeCityGraph& graph,
    const CityCornerStampMap& stamps,
    std::int32_t intersection_index) {
    const auto& intersections = graph.intersections;
    const auto& streets = graph.streets;
    if (intersection_index < 0 ||
        static_cast<std::size_t>(intersection_index) >= intersections.size()) {
        return;
    }
    const auto& a = intersections[static_cast<std::size_t>(intersection_index)];

    // Walking to neighbours: the south and west corner intersections may be one or two
    // streets away (the game skips one intersection in between when the direct
    // neighbour doesn't have the cross street it needs).
    if (a.street_south < 0 || a.street_west < 0) {
        return;
    }
    auto b = streets[static_cast<std::size_t>(a.street_south)].b;
    if (intersections[static_cast<std::size_t>(b)].street_west < 0) {
        const auto south = intersections[static_cast<std::size_t>(b)].street_south;
        if (south < 0) {
            return;
        }
        b = streets[static_cast<std::size_t>(south)].b;
        if (intersections[static_cast<std::size_t>(b)].street_west < 0) {
            return;
        }
    }
    auto c = streets[static_cast<std::size_t>(a.street_west)].b;
    if (intersections[static_cast<std::size_t>(c)].street_south < 0) {
        const auto west = intersections[static_cast<std::size_t>(c)].street_west;
        if (west < 0) {
            return;
        }
        c = streets[static_cast<std::size_t>(west)].b;
        if (intersections[static_cast<std::size_t>(c)].street_south < 0) {
            return;
        }
    }
    auto d = streets[static_cast<std::size_t>(intersections[static_cast<std::size_t>(c)].street_south)].b;
    if (intersections[static_cast<std::size_t>(d)].street_east < 0) {
        const auto south = intersections[static_cast<std::size_t>(d)].street_south;
        if (south < 0) {
            return;
        }
        d = streets[static_cast<std::size_t>(south)].b;
        if (intersections[static_cast<std::size_t>(d)].street_east < 0) {
            return;
        }
    }

    const auto& corner_b = intersections[static_cast<std::size_t>(b)];
    const auto& corner_c = intersections[static_cast<std::size_t>(c)];
    const auto& corner_d = intersections[static_cast<std::size_t>(d)];

    // The patch rectangle in world samples. A quirk of the game: the far corners
    // subtract the same +axis extents (field_0x2c / field_0x30) used on the near corners.
    const auto tile_p0 = a.gx + a.ext_x_pos;
    const auto tile_q0 = a.gz + a.ext_z_pos;
    const auto p0 = tile_p0 * 4;
    const auto p1 = (corner_b.gx - corner_b.ext_x_pos) * 4;
    const auto q0 = tile_q0 * 4;
    const auto q1 = (corner_c.gz - corner_c.ext_z_pos) * 4;
    const auto span_p = p1 - p0;
    const auto span_q = q1 - q0;
    if (span_p <= 0 || span_q <= 0) {
        return;
    }

    const auto height_a = native_road_surface_y(a.level);
    const auto height_b = native_road_surface_y(corner_b.level);
    const auto height_c = native_road_surface_y(corner_c.level);
    const auto height_d = native_road_surface_y(corner_d.level);

    // Phase 1: a bilinear block surface between the four corner intersections.
    for (auto qi = 0; qi <= span_q; ++qi) {
        const auto fq = static_cast<float>(qi) / static_cast<float>(span_q);
        for (auto pi = 0; pi <= span_p; ++pi) {
            const auto fp = static_cast<float>(pi) / static_cast<float>(span_p);
            const auto height = (1.0F - fp) * (height_c * fq + height_a * (1.0F - fq)) +
                (height_d * fq + height_b * (1.0F - fq)) * fp;
            field.set(p0 + pi, q0 + qi, height);
        }
    }

    // Phase 2: seed road tiles from the corner stamps.
    const auto cells_p = span_p >> 2;
    const auto cells_q = span_q >> 2;
    const auto grid_w = span_p + 1;
    const auto grid_h = span_q + 1;
    std::vector<CityPatchCell> cells(static_cast<std::size_t>(grid_w) * static_cast<std::size_t>(grid_h));
    auto cell_at = [&](std::int32_t p, std::int32_t q) -> CityPatchCell& {
        return cells[static_cast<std::size_t>(q) * static_cast<std::size_t>(grid_w) +
            static_cast<std::size_t>(p)];
    };

    for (auto qc = 0; qc < cells_q; ++qc) {
        for (auto pc = 0; pc < cells_p; ++pc) {
            const auto m00 = stamps.at(tile_p0 + pc, tile_q0 + qc);
            const auto m10 = stamps.at(tile_p0 + pc + 1, tile_q0 + qc);
            const auto m01 = stamps.at(tile_p0 + pc, tile_q0 + qc + 1);
            const auto m11 = stamps.at(tile_p0 + pc + 1, tile_q0 + qc + 1);
            if (m00 <= 0.0F || m10 <= 0.0F || m01 <= 0.0F || m11 <= 0.0F) {
                continue;
            }
            for (auto qs = 0; qs < 5; ++qs) {
                const auto fq = static_cast<float>(qs) * 0.25F;
                for (auto ps = 0; ps < 5; ++ps) {
                    const auto fp = static_cast<float>(ps) * 0.25F;
                    const auto p = pc * 4 + ps;
                    const auto q = qc * 4 + qs;
                    if (p >= grid_w || q >= grid_h) {
                        continue;
                    }
                    auto& cell = cell_at(p, q);
                    if (cell.count != 0) {
                        continue;
                    }
                    cell.entries[0] = {p, q, 1.0F};
                    cell.count = 1;
                    const auto height = (1.0F - fp) * (m00 * (1.0F - fq) + m01 * fq) +
                        (m10 * (1.0F - fq) + m11 * fq) * fp;
                    field.set(p0 + p, q0 + q, height);
                }
            }
        }
    }

    // Phase 3: spread the seeds over 16 passes. Each sample that is not a seed picks up
    // to two candidates (holding the original seed's coordinates) from its 8
    // neighbours. It keeps candidates from different directions (dot > 0 test) and
    // replaces weaker ones that point the same way.
    for (auto pass = 0; pass < 16; ++pass) {
        for (auto q = 1; q < span_q; ++q) {
            for (auto p = 1; p < span_p; ++p) {
                auto& cell = cell_at(p, q);
                if (cell.count != 0 && cell.entries[0].weight >= 1.0F) {
                    continue;
                }
                for (auto dq = -1; dq <= 1; ++dq) {
                    for (auto dp = -1; dp <= 1; ++dp) {
                        if (dp == 0 && dq == 0) {
                            continue;
                        }
                        const auto np = p + dp;
                        const auto nq = q + dq;
                        if (np < 0 || nq < 0 || np >= grid_w || nq >= grid_h) {
                            continue;
                        }
                        const auto& neighbor = cell_at(np, nq);
                        for (auto entry = 0; entry < neighbor.count; ++entry) {
                            const auto source = neighbor.entries[static_cast<std::size_t>(entry)];
                            const auto diff_p = static_cast<float>(source.px - p);
                            const auto diff_q = static_cast<float>(source.qz - q);
                            const auto distance = std::sqrt(diff_p * diff_p + diff_q * diff_q);
                            const auto weight = 1.0F - distance * 0.0625F;
                            if (weight <= 0.0F) {
                                continue;
                            }
                            const auto dir_p = distance == 0.0F ? 0.0F : diff_p / distance;
                            const auto dir_q = distance == 0.0F ? 0.0F : diff_q / distance;
                            if (cell.count == 0) {
                                cell.entries[0] = {source.px, source.qz, weight};
                                cell.count = 1;
                                continue;
                            }
                            // -1 = no candidate pointing the same way seen yet, -2 = one
                            // already has an equal or better weight.
                            auto best = -1;
                            for (auto existing = 0; existing < cell.count; ++existing) {
                                const auto& held = cell.entries[static_cast<std::size_t>(existing)];
                                const auto held_p = static_cast<float>(held.px - p);
                                const auto held_q = static_cast<float>(held.qz - q);
                                const auto held_distance = std::sqrt(held_p * held_p + held_q * held_q);
                                const auto held_dir_p = held_distance == 0.0F ? 0.0F : held_p / held_distance;
                                const auto held_dir_q = held_distance == 0.0F ? 0.0F : held_q / held_distance;
                                if (held_dir_p * dir_p + held_dir_q * dir_q > 0.0F) {
                                    best = -2;
                                    if (held.weight < weight) {
                                        best = existing;
                                    }
                                }
                            }
                            if (best == -1) {
                                if (cell.count < 2) {
                                    cell.entries[static_cast<std::size_t>(cell.count)] =
                                        {source.px, source.qz, weight};
                                    cell.count += 1;
                                }
                            } else if (best >= 0) {
                                cell.entries[static_cast<std::size_t>(best)] =
                                    {source.px, source.qz, weight};
                            }
                        }
                    }
                }
            }
        }
    }

    // Phase 4: smoothstep merge toward the seed heights.
    for (auto q = 1; q < span_q; ++q) {
        for (auto p = 1; p < span_p; ++p) {
            const auto& cell = cell_at(p, q);
            if (cell.count <= 0) {
                continue;
            }
            const auto current = field.get(p0 + p, q0 + q);
            auto target = 0.0F;
            if (cell.count < 2) {
                target = field.get(p0 + cell.entries[0].px, q0 + cell.entries[0].qz);
            } else {
                auto weight_sum = cell.entries[0].weight + cell.entries[1].weight;
                weight_sum = std::min(weight_sum, 1.0F);
                const auto ratio = clamp01(cell.entries[0].weight / weight_sum);
                const auto blend = (3.0F - 2.0F * ratio) * ratio * ratio;
                const auto height0 = field.get(p0 + cell.entries[0].px, q0 + cell.entries[0].qz);
                const auto height1 = field.get(p0 + cell.entries[1].px, q0 + cell.entries[1].qz);
                target = (1.0F - blend) * height1 + blend * height0;
            }
            auto max_weight = 0.0F;
            for (auto entry = 0; entry < cell.count; ++entry) {
                max_weight = std::max(max_weight, cell.entries[static_cast<std::size_t>(entry)].weight);
            }
            max_weight = clamp01(max_weight);
            const auto blend = (3.0F - 2.0F * max_weight) * max_weight * max_weight;
            field.set(p0 + p, q0 + q, (1.0F - blend) * current + target * blend);
        }
    }
}

std::vector<CityTileCell> build_city_tile_cells(
    const NativeCityGraph& graph,
    CityCornerStampMap& stamps,
    bool auto_generated) {
    CityTileMap tile_map;
    for (const auto& street : graph.streets) {
        place_native_street_tiles(tile_map, stamps, graph, street, auto_generated);
    }
    for (std::size_t index = 0; index < graph.intersections.size(); ++index) {
        place_native_intersection_tiles(tile_map, graph.intersections[index], static_cast<std::int32_t>(index));
    }

    std::vector<CityTileCell> tiles;
    tiles.reserve(tile_map.size());
    for (const auto& [key, value] : tile_map) {
        tiles.push_back({key.x, key.y, key.z, value.base_y, value.slope_x, value.slope_z, value.kind, value.street_index,
            value.block, value.faces, value.intersection_index});
    }
    std::ranges::sort(tiles, [](const CityTileCell& left, const CityTileCell& right) {
        return std::tie(left.y, left.z, left.x) < std::tie(right.y, right.z, right.x);
    });
    return tiles;
}

// Face tables of BuildCellRenderGeometryAndAdjacency @c555, read from subrosa.exe:
// cube corners DAT_002b34f0, face corners DAT_002b3660, and each face's tangent
// DAT_002b35a0 and bitangent DAT_002b35f0, which project the UVs.
// Faces: 0 -Y, 1 +Z, 2 -X, 3 -Z, 4 +X, 5 +Y.
constexpr std::array<Vec3, 8> kNativeCubeCorner{{
    {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}, {0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1},
}};
constexpr std::array<std::array<std::uint8_t, 4>, 6> kNativeFaceCorners{{
    {3, 2, 1, 0}, {7, 6, 2, 3}, {4, 7, 3, 0}, {5, 4, 0, 1}, {6, 5, 1, 2}, {4, 5, 6, 7},
}};
constexpr std::array<Vec3, 6> kNativeFaceTangent{{
    {-1, 0, 0}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}, {0, 0, -1}, {1, 0, 0},
}};
constexpr std::array<Vec3, 6> kNativeFaceBitangent{{
    {0, 0, -1}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 0, 1},
}};
constexpr std::array<std::array<std::int32_t, 3>, 6> kNativeFaceNeighbor{{
    {0, -1, 0}, {0, 0, 1}, {-1, 0, 0}, {0, 0, -1}, {1, 0, 0}, {0, 1, 0},
}};

// A face code names a texture and a road-marking overlay layer. With bit 14 set,
// the low byte is the texture and bits 8..12 are the overlay layer. A negative code
// means the default texture (index 3) with overlay code & 0x3ff.
struct NativeFaceTexture {
    std::uint32_t texture{};
    std::uint32_t overlay{};
};

NativeFaceTexture decode_native_face(std::uint16_t code) {
    if ((code & 0x4000U) != 0U) {
        return {code & 0xffU, (code >> 8U) & 0x1fU};
    }
    if ((code & 0x8000U) != 0U) {
        return {3U, code & 0x3ffU};
    }
    return {code, 0U};
}

// Road cells only use texture 0 (concrete, for curbs) and 1 (asphalt2, for lanes).
std::uint32_t city_tile_material(BuildingRenderBuffer& buffer, std::uint32_t texture) {
    return material_id_for_name(buffer, texture == 0U ? "city_curb_concrete" : "city_road_asphalt");
}

void add_city_tile_quad(BuildingRenderBuffer& buffer, const std::array<Vec3, 4>& p, const std::array<Vec2f, 4>& uv,
                        std::uint16_t code) {
    const auto texture = decode_native_face(code);
    const auto material = city_tile_material(buffer, texture.texture);
    const auto first = static_cast<std::uint32_t>(buffer.vertices.size());
    for (std::size_t i = 0; i < 4; ++i) {
        buffer.vertices.push_back({p[i], uv[i].u, uv[i].v, static_cast<float>(texture.overlay)});
    }
    buffer.triangles.push_back({{first, first + 1U, first + 2U}, material, false});
    buffer.triangles.push_back({{first, first + 2U, first + 3U}, material, false});
}

} // namespace

// A hash map with open addressing in flat arrays. The shadow bake inserts millions
// of small keys, and node-based maps spend most of their time in malloc.
template <typename K, typename V, typename Hash>
class FlatMap {
public:
    void reserve(std::size_t count) {
        std::size_t capacity = 16;
        while (capacity < count * 2U) capacity *= 2U;
        if (capacity > slots_.size()) rehash(capacity);
    }
    // The value for key, and whether it was just added.
    std::pair<V*, bool> emplace(const K& key, const V& value) {
        if ((size_ + 1U) * 2U > slots_.size()) rehash(std::max<std::size_t>(16, slots_.size() * 2U));
        auto i = slot_of(key);
        std::optional<std::size_t> reuse;
        for (; slots_[i].state != kEmpty; i = (i + 1U) & (slots_.size() - 1U)) {
            if (slots_[i].state == kFull && slots_[i].key == key) return {&slots_[i].value, false};
            if (slots_[i].state == kErased && !reuse) reuse = i;
        }
        if (reuse) i = *reuse;
        slots_[i] = {key, value, kFull};
        ++size_;
        return {&slots_[i].value, true};
    }
    V* find(const K& key) {
        if (slots_.empty()) return nullptr;
        for (auto i = slot_of(key); slots_[i].state != kEmpty; i = (i + 1U) & (slots_.size() - 1U)) {
            if (slots_[i].state == kFull && slots_[i].key == key) return &slots_[i].value;
        }
        return nullptr;
    }
    const V* find(const K& key) const { return const_cast<FlatMap*>(this)->find(key); }
    const V& at(const K& key) const { return *find(key); }
    // Erases the entry whose value find() returned.
    void erase(V* value) {
        auto& slot = *reinterpret_cast<Slot*>(reinterpret_cast<char*>(value) - offsetof(Slot, value));
        slot.state = kErased;
        --size_;
    }

private:
    enum : std::uint8_t { kEmpty, kFull, kErased };
    // Mixes the hash so linear probing over a power-of-two table spreads keys well.
    std::size_t slot_of(const K& key) const {
        auto x = static_cast<std::uint64_t>(Hash{}(key));
        x ^= x >> 33U; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33U;
        return static_cast<std::size_t>(x) & (slots_.size() - 1U);
    }
    struct Slot {
        K key{};
        V value{};
        std::uint8_t state{kEmpty};
    };
    void rehash(std::size_t capacity) {
        auto old = std::move(slots_);
        slots_.assign(capacity, Slot{});
        size_ = 0;
        for (auto& slot : old) if (slot.state == kFull) emplace(slot.key, slot.value);
    }
    std::vector<Slot> slots_;
    std::size_t size_{};
};

void build_terrain_draw_chunks(BuildingRenderBuffer& buffer) {
    constexpr float chunk_size = 128.0F;
    buffer.terrain_chunks.clear();
    std::vector<std::size_t> bucket_for_triangle(buffer.triangles.size());
    std::size_t covered = 0;
    for (const auto& run : buffer.material_runs) {
        std::unordered_map<std::uint64_t, std::size_t> buckets;
        for (std::size_t i = run.first_triangle; i < static_cast<std::size_t>(run.first_triangle) + run.triangle_count; ++i) {
            const auto& triangle = buffer.triangles.at(i);
            const auto& a = buffer.vertices.at(triangle.indices[0]).position;
            const auto& b = buffer.vertices.at(triangle.indices[1]).position;
            const auto& c = buffer.vertices.at(triangle.indices[2]).position;
            const auto x = static_cast<std::int32_t>(std::floor((a.x + b.x + c.x) / (3.0F * chunk_size)));
            const auto z = static_cast<std::int32_t>(std::floor((a.z + b.z + c.z) / (3.0F * chunk_size)));
            const auto key = static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32U | static_cast<std::uint32_t>(z);
            const auto [it, inserted] = buckets.emplace(key, buffer.terrain_chunks.size());
            if (inserted) buffer.terrain_chunks.push_back({{run.material, 0, 0, run.city_window_surface}, a, a});
            auto& chunk = buffer.terrain_chunks[it->second];
            for (const auto p : {a, b, c}) {
                chunk.bounds_min = {std::min(chunk.bounds_min.x, p.x), std::min(chunk.bounds_min.y, p.y), std::min(chunk.bounds_min.z, p.z)};
                chunk.bounds_max = {std::max(chunk.bounds_max.x, p.x), std::max(chunk.bounds_max.y, p.y), std::max(chunk.bounds_max.z, p.z)};
            }
            ++chunk.run.triangle_count;
            bucket_for_triangle[i] = it->second;
            ++covered;
        }
    }
    if (covered != buffer.triangles.size()) throw std::runtime_error("Terrain material runs do not cover its triangles");
    std::vector<std::size_t> next;
    std::uint32_t first = 0;
    for (auto& chunk : buffer.terrain_chunks) {
        chunk.run.first_triangle = first;
        next.push_back(first);
        first += chunk.run.triangle_count;
    }
    std::vector<RenderBufferTriangle> ordered(buffer.triangles.size());
    for (std::size_t i = 0; i < buffer.triangles.size(); ++i) ordered[next[bucket_for_triangle[i]]++] = buffer.triangles[i];
    buffer.triangles = std::move(ordered);
}

bool terrain_bounds_visible(Vec3 lo, Vec3 hi, Vec3 camera, float distance, const ViewFrustumPlanes& planes) {
    if (distance > 0.0F) {
        const auto dx = std::max({lo.x - camera.x, 0.0F, camera.x - hi.x});
        const auto dy = std::max({lo.y - camera.y, 0.0F, camera.y - hi.y});
        const auto dz = std::max({lo.z - camera.z, 0.0F, camera.z - hi.z});
        if (dx * dx + dy * dy + dz * dz > distance * distance) return false;
    }
    for (const auto& p : planes) {
        const Vec3 corner{p[0] >= 0.0F ? hi.x : lo.x, p[1] >= 0.0F ? hi.y : lo.y, p[2] >= 0.0F ? hi.z : lo.z};
        if (p[0] * corner.x + p[1] * corner.y + p[2] * corner.z + p[3] < 0.0F) return false;
    }
    return true;
}

void keep_pick_data_only(BuildingRenderBuffer& buffer) {
    buffer.mask_cache.clear();
    buffer.cell_mask_index.clear();
    buffer.pick_grid = {};
    struct PositionHash {
        std::size_t operator()(const Vec3& p) const {
            return std::hash<float>{}(p.x) * 73856093U ^ std::hash<float>{}(p.y) * 19349663U ^ std::hash<float>{}(p.z) * 83492791U;
        }
    };
    struct PositionEqual {
        bool operator()(const Vec3& a, const Vec3& b) const { return a.x == b.x && a.y == b.y && a.z == b.z; }
    };
    std::unordered_map<Vec3, std::uint32_t, PositionHash, PositionEqual> index_of;
    index_of.reserve(buffer.vertices.size());
    std::vector<RenderBufferVertex> vertices;
    std::vector<std::uint32_t> remap(buffer.vertices.size());
    for (std::size_t i = 0; i < buffer.vertices.size(); ++i) {
        const auto [found, added] = index_of.emplace(buffer.vertices[i].position, static_cast<std::uint32_t>(vertices.size()));
        if (added) vertices.push_back({buffer.vertices[i].position});
        remap[i] = found->second;
    }
    for (auto& triangle : buffer.triangles) {
        for (auto& index : triangle.indices) {
            if (index >= remap.size()) throw std::runtime_error("Picking buffer triangle index exceeds its vertices");
            index = remap[index];
        }
    }
    vertices.shrink_to_fit();
    buffer.vertices = std::move(vertices);
    buffer.triangles.shrink_to_fit();
    std::vector<RenderBufferMaterialRun>().swap(buffer.material_runs);
    std::vector<TerrainDrawChunk>().swap(buffer.terrain_chunks);
    std::vector<RenderLine>().swap(buffer.wire_lines);
    release_shadow_volume(buffer);
}

void bake_shadow_volume(BuildingRenderBuffer& buffer) {
    // Build the volume once from the face adjacency. RenderQueuedShadowVolumeAdjacencyPass
    // @11c950 stretches every triangle of a sector, window panes included. The game's
    // walls are whole cell quads (windows are in the texture's alpha), but ours are cut
    // around window rectangles, which leaves T-junctions. A long edge next to several
    // short ones never pairs up, both sides make side quads in the same place, and depth
    // rounding leaks light through as thin lines. So split every edge at the vertices
    // that lie on it, then pair each edge a->b with b->a.
    using Key = std::array<std::int64_t, 3>;
    const auto point_key = [](Vec3 point) {
        return Key{std::llround(point.x * 1024.0), std::llround(point.y * 1024.0), std::llround(point.z * 1024.0)};
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const {
            return static_cast<std::size_t>((key[0] * 73856093) ^ (key[1] * 19349663) ^ (key[2] * 83492791));
        }
    };
    struct IdentityHash {
        std::size_t operator()(std::uint64_t value) const { return static_cast<std::size_t>(value); }
    };
    // Edge a->b between point ids, with a direction.
    const auto edge_key = [](std::uint32_t a, std::uint32_t b) { return static_cast<std::uint64_t>(a) << 32U | b; };

    // Distinct points (on a 1/1024-unit grid) get ids, and edges and faces use the ids.
    FlatMap<Key, std::uint32_t, KeyHash> point_id;
    std::vector<Key> point_keys;
    std::vector<Vec3> point_positions;
    const auto id_of = [&](const Key& key, Vec3 position) {
        const auto [id, added] = point_id.emplace(key, static_cast<std::uint32_t>(point_keys.size()));
        if (added) {
            point_keys.push_back(key);
            point_positions.push_back(position);
        }
        return *id;
    };
    point_id.reserve(buffer.vertices.size());
    std::vector<std::uint32_t> vertex_point(buffer.vertices.size());
    for (std::size_t i = 0; i < buffer.vertices.size(); ++i) {
        vertex_point[i] = id_of(point_key(buffer.vertices[i].position), buffer.vertices[i].position);
    }
    // The buffer's own points, sorted into 1-unit grid cells.
    const auto cell_of = [](const Key& key) { return Key{key[0] >> 10, key[1] >> 10, key[2] >> 10}; };
    std::unordered_map<Key, std::vector<std::uint32_t>, KeyHash> grid;
    for (std::uint32_t id = 0; id < point_keys.size(); ++id) grid[cell_of(point_keys[id])].push_back(id);

    // Points strictly inside segment a-b, ordered from a.
    std::vector<std::pair<std::int64_t, std::uint32_t>> on_edge;
    const auto split_points = [&](std::uint32_t a_id, std::uint32_t b_id) {
        on_edge.clear();
        const auto& a = point_keys[a_id];
        const auto& b = point_keys[b_id];
        const Key d{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const auto length_sq = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (length_sq == 0) return;
        const auto lo = cell_of({std::min(a[0], b[0]), std::min(a[1], b[1]), std::min(a[2], b[2])});
        const auto hi = cell_of({std::max(a[0], b[0]), std::max(a[1], b[1]), std::max(a[2], b[2])});
        for (auto x = lo[0]; x <= hi[0]; ++x)
        for (auto y = lo[1]; y <= hi[1]; ++y)
        for (auto z = lo[2]; z <= hi[2]; ++z) {
            const auto bucket = grid.find({x, y, z});
            if (bucket == grid.end()) continue;
            for (const auto id : bucket->second) {
                const auto& p = point_keys[id];
                const Key v{p[0] - a[0], p[1] - a[1], p[2] - a[2]};
                const auto t = v[0] * d[0] + v[1] * d[1] + v[2] * d[2];
                if (t <= 0 || t >= length_sq) continue;
                // Squared distance from the line, within 2/1024 units.
                const auto cx = static_cast<double>(v[1] * d[2] - v[2] * d[1]);
                const auto cy = static_cast<double>(v[2] * d[0] - v[0] * d[2]);
                const auto cz = static_cast<double>(v[0] * d[1] - v[1] * d[0]);
                if (cx * cx + cy * cy + cz * cz > 4.0 * static_cast<double>(length_sq)) continue;
                on_edge.emplace_back(t, id);
            }
        }
        std::ranges::sort(on_edge);
    };

    // Split faces remember the render triangle they came from. Caps use that triangle
    // as drawn, so a cap's depth matches its surface exactly, and every piece shares
    // its normal, so the pieces agree on which way they face.
    struct Face {
        std::array<std::uint32_t, 3> points{};
        std::array<std::int32_t, 3> adjacent{{-1, -1, -1}};
        std::uint32_t parent{};
    };
    const auto usable = [&](const RenderBufferTriangle& triangle) {
        return !triangle.interior && triangle.indices[0] < buffer.vertices.size() &&
            triangle.indices[1] < buffer.vertices.size() && triangle.indices[2] < buffer.vertices.size();
    };
    std::vector<std::array<std::uint32_t, 3>> parents;
    parents.reserve(buffer.triangles.size());
    for (const auto& triangle : buffer.triangles) {
        if (usable(triangle)) {
            parents.push_back({vertex_point[triangle.indices[0]], vertex_point[triangle.indices[1]], vertex_point[triangle.indices[2]]});
        }
    }
    // An edge whose reverse is another triangle's edge pairs as it is. Only the rest
    // can meet a T-junction, so only they are searched for split points.
    FlatMap<std::uint64_t, std::uint8_t, IdentityHash> directed_edges;
    directed_edges.reserve(parents.size() * 3U);
    for (const auto& ids : parents) {
        for (std::size_t edge = 0; edge < 3; ++edge) directed_edges.emplace(edge_key(ids[edge], ids[(edge + 1U) % 3U]), 1U);
    }

    std::vector<Face> faces;
    faces.reserve(parents.size());
    FlatMap<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>, IdentityHash> open_edges;
    open_edges.reserve(parents.size() * 2U);
    const auto add_face = [&](const std::array<std::uint32_t, 3>& ids, std::uint32_t parent) {
        const auto face_index = static_cast<std::uint32_t>(faces.size());
        faces.push_back({ids, {{-1, -1, -1}}, parent});
        for (std::uint32_t edge = 0; edge < 3; ++edge) {
            const auto from = ids[edge];
            const auto to = ids[(edge + 1U) % 3U];
            if (auto* owner = open_edges.find(edge_key(to, from)); owner != nullptr) {
                faces[face_index].adjacent[edge] = static_cast<std::int32_t>(owner->first);
                faces[owner->first].adjacent[owner->second] = static_cast<std::int32_t>(face_index);
                open_edges.erase(owner);
            } else {
                open_edges.emplace(edge_key(from, to), {face_index, edge});
            }
        }
    };
    std::vector<std::uint32_t> ring;
    for (std::uint32_t t = 0; t < parents.size(); ++t) {
        const auto ids = parents[t];
        ring.clear();
        for (std::size_t edge = 0; edge < 3; ++edge) {
            ring.push_back(ids[edge]);
            const auto next = ids[(edge + 1U) % 3U];
            if (directed_edges.find(edge_key(next, ids[edge])) != nullptr) continue;
            split_points(ids[edge], next);
            for (const auto& [distance, id] : on_edge) ring.push_back(id);
        }
        if (ring.size() == 3) {
            add_face(ids, t);
            continue;
        }
        // Fan the split polygon out from its centre, so no fan triangle is flat
        // (split points are on a line with their edge's corners).
        Vec3 centroid{};
        for (const auto id : ids) {
            const auto& p = point_positions[id];
            centroid = {centroid.x + p.x / 3.0F, centroid.y + p.y / 3.0F, centroid.z + p.z / 3.0F};
        }
        const auto center = id_of(point_key(centroid), centroid);
        for (std::size_t i = 0; i < ring.size(); ++i) add_face({center, ring[i], ring[(i + 1U) % ring.size()]}, t);
    }

    // RenderQueuedShadowVolumeAdjacencyPass picks the outline edges every frame in a
    // geometry shader: it stretches the faces turned away from the light and adds a side
    // where the neighbour faces the light or there is no neighbour. We have no geometry
    // shader, so we bake every possible piece: each face gets a front and a back cap,
    // and each edge a quad whose two ends carry their own face's normal (the kind is in
    // uv.x). The shadow vertex shader keeps the caps of faces turned away and stretches
    // an edge end when its face is lit, so only the outline quads end up with any area.
    // An open edge's far end always stretches. Faces are wound clockwise (the game's
    // order), so cross(p1 - p0, p2 - p0) points inward, and a face is turned away when
    // that normal meets the light.
    enum : std::uint8_t { kEdge = 0, kOpenEdge = 1, kFrontCap = 2, kBackCap = 3 };
    // The vertex layout matches the GPU mesh: position, uv (kind, 0, 0), normal.
    auto& vertices = buffer.shadow_vertices;
    vertices.clear();
    vertices.reserve(parents.size() * 4U * 9U);
    std::uint32_t vertex_count = 0;
    std::vector<Vec3> normals(parents.size());
    // Identical vertices (corners shared by caps in one plane) are stored once, keyed
    // by point id, kind, and the bits of the normal.
    using VertexKey = std::array<std::uint32_t, 5>;
    struct VertexHash {
        std::size_t operator()(const VertexKey& key) const {
            std::uint64_t hash = 1469598103934665603ULL;
            for (const auto value : key) hash = (hash ^ value) * 1099511628211ULL;
            return static_cast<std::size_t>(hash);
        }
    };
    FlatMap<VertexKey, std::uint32_t, VertexHash> vertex_index;
    vertex_index.reserve(parents.size() * 4U);
    const auto emit = [&](std::uint32_t point, Vec3 normal, std::uint8_t kind) {
        const auto [found, added] = vertex_index.emplace(VertexKey{point, kind, std::bit_cast<std::uint32_t>(normal.x),
            std::bit_cast<std::uint32_t>(normal.y), std::bit_cast<std::uint32_t>(normal.z)}, vertex_count);
        if (added) {
            const auto& p = point_positions[point];
            vertices.insert(vertices.end(), {p.x, p.y, p.z, static_cast<float>(kind), 0.0F, 0.0F, normal.x, normal.y, normal.z});
            ++vertex_count;
        }
        return *found;
    };
    auto& indices = buffer.shadow_indices;
    indices.clear();
    indices.reserve(parents.size() * 6U + faces.size() * 9U);
    buffer.shadow_top = 0.0F;
    for (std::size_t t = 0; t < parents.size(); ++t) {
        const auto& p0 = point_positions[parents[t][0]];
        const auto& p1 = point_positions[parents[t][1]];
        const auto& p2 = point_positions[parents[t][2]];
        const Vec3 e1{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
        const Vec3 e2{p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
        // Unit length, rounded. The shader only reads the sign against the light,
        // and rounding lets faces in the same plane share their cap corners.
        const Vec3 n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
        const auto length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        const auto unit = [&](float v) { return length > 0.0F ? std::round(v / length * 65536.0F) / 65536.0F : 0.0F; };
        normals[t] = {unit(n.x), unit(n.y), unit(n.z)};
        buffer.shadow_top = std::max({buffer.shadow_top, p0.y, p1.y, p2.y});
        for (const auto i : {0, 1, 2}) indices.push_back(emit(parents[t][i], normals[t], kFrontCap));
        for (const auto i : {0, 2, 1}) indices.push_back(emit(parents[t][i], normals[t], kBackCap));
    }
    for (std::size_t f = 0; f < faces.size(); ++f) {
        const auto normal = normals[faces[f].parent];
        for (std::size_t edge = 0; edge < 3; ++edge) {
            const auto other = faces[f].adjacent[edge];
            if (other >= 0 && static_cast<std::size_t>(other) < f) continue;
            const auto from = faces[f].points[edge];
            const auto to = faces[f].points[(edge + 1U) % 3U];
            const auto other_normal = other >= 0 ? normals[faces[static_cast<std::size_t>(other)].parent] : Vec3{};
            // Two faces in the same plane always face the light the same way, so
            // the edge between them is never part of an outline: skip its quad.
            if (other >= 0) {
                const Vec3 cross{normal.y * other_normal.z - normal.z * other_normal.y,
                    normal.z * other_normal.x - normal.x * other_normal.z, normal.x * other_normal.y - normal.y * other_normal.x};
                const auto dot = normal.x * other_normal.x + normal.y * other_normal.y + normal.z * other_normal.z;
                const auto lengths = (normal.x * normal.x + normal.y * normal.y + normal.z * normal.z) *
                    (other_normal.x * other_normal.x + other_normal.y * other_normal.y + other_normal.z * other_normal.z);
                if (dot > 0.0F && cross.x * cross.x + cross.y * cross.y + cross.z * cross.z <= lengths * 1e-8F) continue;
            }
            const auto far_kind = other >= 0 ? kEdge : kOpenEdge;
            // (to, from, to'), (to', from, from') when this face is the dark one.
            const auto a = emit(to, normal, kEdge);
            const auto b = emit(from, normal, kEdge);
            const auto c = emit(to, other_normal, far_kind);
            const auto d = emit(from, other_normal, far_kind);
            indices.insert(indices.end(), {a, b, c, c, b, d});
        }
    }
}

namespace {

// Road tiles, built the way BuildCellRenderGeometryAndAdjacency builds them: solid
// cells make each face that no solid neighbour covers, block cells place their
// catalog mesh, and every face takes its texture and overlay from the cell's face
// array. Cell y runs from y*4+0.25 to y*4+4.25.
BuildingRenderBuffer build_city_tile_render_buffer(const std::vector<CityTileCell>& tiles, float unit_size) {
    static const auto catalog = build_native_builtin_catalog();
    BuildingRenderBuffer buffer;
    buffer.unit_size = unit_size;
    std::unordered_set<CityTileKey, CityTileKeyHash> solids;
    for (const auto& tile : tiles) {
        if ((tile.block & 0xe0000000U) == kNativeSolidBlock) {
            solids.insert({tile.x, tile.y, tile.z});
        }
    }
    for (std::size_t tile_index = 0; tile_index < tiles.size(); ++tile_index) {
        const auto& tile = tiles[tile_index];
        const RenderCellTag tag{buffer, buffer.triangles.size(), static_cast<std::uint32_t>(tile_index)};
        const Vec3 origin{
            static_cast<float>(tile.x) * unit_size,
            static_cast<float>(tile.y) * unit_size + 0.25F,
            static_cast<float>(tile.z) * unit_size,
        };
        if ((tile.block & 0xe0000000U) == kNativeSolidBlock) {
            // UVs project the position inside the sector (8-cell sectors) at 0.25 per
            // world unit, so the texture repeats once per cell.
            const Vec3 local{
                static_cast<float>(tile.x & 7),
                static_cast<float>(tile.y & 7),
                static_cast<float>(tile.z & 7),
            };
            for (std::size_t face = 0; face < 6; ++face) {
                const auto& n = kNativeFaceNeighbor[face];
                if ((face == 0 && tile.y == 0) || solids.contains({tile.x + n[0], tile.y + n[1], tile.z + n[2]})) {
                    continue;
                }
                std::array<Vec3, 4> p{};
                std::array<Vec2f, 4> uv{};
                for (std::size_t i = 0; i < 4; ++i) {
                    const auto& c = kNativeCubeCorner[kNativeFaceCorners[face][i]];
                    const Vec3 sector{(local.x + c.x) * unit_size, (local.y + c.y) * unit_size, (local.z + c.z) * unit_size};
                    const auto& t = kNativeFaceTangent[face];
                    const auto& b = kNativeFaceBitangent[face];
                    uv[i] = {
                        (t.x * sector.x + t.y * sector.y + t.z * sector.z) * 0.25F,
                        (b.x * sector.x + b.y * sector.y + b.z * sector.z) * 0.25F,
                    };
                    p[i] = {origin.x + c.x * unit_size, origin.y + c.y * unit_size, origin.z + c.z * unit_size};
                }
                add_city_tile_quad(buffer, p, uv, tile.faces[face]);
            }
            continue;
        }
        const auto id = tile.block & 0xffffU;
        if (id >= catalog.size()) {
            continue;
        }
        for (const auto& face : catalog[id].mesh.faces) {
            if (face.vertices.size() < 4 || face.uvs.size() < 4) {
                continue;
            }
            std::array<Vec3, 4> p{};
            std::array<Vec2f, 4> uv{};
            for (std::size_t i = 0; i < 4; ++i) {
                const auto& v = face.vertices[i];
                p[i] = {origin.x + v.x * unit_size, origin.y + v.y * unit_size, origin.z + v.z * unit_size};
                uv[i] = face.uvs[i];
            }
            add_city_tile_quad(buffer, p, uv, tile.faces[face.material & 7U]);
        }
    }
    sort_triangles_into_native_material_runs(buffer);
    return buffer;
}

// One landscape chunk's grid at `step` units per sample into `mesh`. Quads under a
// road, piece or building tile are left out: the tile's top is the ground there, and
// terrain at the same height flickers against it. A quad is only left out when all
// four of its corners are covered, so the cut follows the tile outline without holes.
template <typename Height, typename Covered>
void append_terrain_chunk(BuildingRenderBuffer& mesh, float chunk_x, float chunk_z, std::int32_t step, float unit_size,
    const Height& height, const Covered& covered) {
    constexpr std::int32_t chunk_size = 128;
    const auto side = chunk_size / step + 1;
    const auto first_vertex = static_cast<std::uint32_t>(mesh.vertices.size());
    for (auto z = 0; z < side; ++z) {
        for (auto x = 0; x < side; ++x) {
            const auto world_x = chunk_x + static_cast<float>(x * step);
            const auto world_z = chunk_z + static_cast<float>(z * step);
            mesh.vertices.push_back({{world_x, height(world_x, world_z) - city_terrain_mesh_y_offset, world_z}, world_x / 32.0F, 1.0F - world_z / 32.0F});
        }
    }
    const auto cell_of = [&](float wx, float wz) {
        return covered(static_cast<std::int32_t>(std::floor(wx / unit_size)), static_cast<std::int32_t>(std::floor(wz / unit_size)));
    };
    for (auto z = 0; z < side - 1; ++z) {
        for (auto x = 0; x < side - 1; ++x) {
            const auto quad_min_x = chunk_x + static_cast<float>(x * step);
            const auto quad_min_z = chunk_z + static_cast<float>(z * step);
            const auto quad_max_x = quad_min_x + static_cast<float>(step);
            const auto quad_max_z = quad_min_z + static_cast<float>(step);
            // Sample just inside each corner, so a quad that touches a tile's edge finds
            // the cell it really overlaps.
            constexpr auto eps = 0.01F;
            if (cell_of(quad_min_x + eps, quad_min_z + eps) && cell_of(quad_max_x - eps, quad_min_z + eps) &&
                cell_of(quad_min_x + eps, quad_max_z - eps) && cell_of(quad_max_x - eps, quad_max_z - eps)) {
                continue;
            }
            const auto i0 = first_vertex + static_cast<std::uint32_t>(z * side + x);
            const auto i1 = i0 + 1U;
            const auto i3 = first_vertex + static_cast<std::uint32_t>((z + 1) * side + x);
            const auto i2 = i3 + 1U;
            mesh.triangles.push_back({{i0, i1, i2}, 0U, false});
            mesh.triangles.push_back({{i0, i2, i3}, 0U, false});
        }
    }
}

std::optional<CityTerrain> build_city_terrain(
    const SbcMap& map,
    const NativeCityGraph& expanded_graph,
    const CityCornerStampMap& stamps,
    const std::vector<CityTileCell>& tile_cells,
    float unit_size) {
    if (map.intersections.empty()) {
        return std::nullopt;
    }

    // The ground is thrown away where the land-use mask bit is clear: every cell that a
    // street tile, district cell, or building's anchor layer stamped (see
    // CityCornerStampMap::stamp). Buildings and pieces have no ground under them.
    (void)tile_cells;
    auto cell_covered = [&](std::int32_t gx, std::int32_t gz) {
        return stamps.masked.contains(CityCornerStampMap::key(gx, gz));
    };

    auto min_x = std::numeric_limits<std::int32_t>::max();
    auto max_x = std::numeric_limits<std::int32_t>::lowest();
    auto min_z = std::numeric_limits<std::int32_t>::max();
    auto max_z = std::numeric_limits<std::int32_t>::lowest();
    for (const auto& intersection : map.intersections) {
        min_x = std::min(min_x, intersection.x);
        max_x = std::max(max_x, intersection.x);
        min_z = std::min(min_z, intersection.z);
        max_z = std::max(max_z, intersection.z);
    }
    // Cover the buildings too, not just the intersections. Otherwise buildings placed
    // outside the intersections' bounding box have holes under them.
    for (const auto& building : map.buildings) {
        min_x = std::min(min_x, building.x);
        max_x = std::max(max_x, building.x);
        min_z = std::min(min_z, building.z);
        max_z = std::max(max_z, building.z);
    }

    // Plenty of padding, so building footprints and road curbs near the edge still
    // stand on solid ground.
    constexpr auto terrain_padding_tiles = 24;
    CityTerrain terrain{
        static_cast<float>((min_x - terrain_padding_tiles)) * unit_size,
        static_cast<float>((max_x + terrain_padding_tiles)) * unit_size,
        static_cast<float>((min_z - terrain_padding_tiles)) * unit_size,
        static_cast<float>((max_z + terrain_padding_tiles)) * unit_size,
        0,
        0,
        // Mesh and query resolution. The heightfield below is worked out at the game's
        // spacing of one sample per world unit; the mesh samples it every 2 units to
        // keep the number of vertices manageable.
#ifdef __EMSCRIPTEN__
        // The desktop path keeps a full one-unit patched heightfield. Building that
        // field on one thread in WebAssembly can freeze the editor for minutes when
        // a map is thousands of units across. So the browser keeps the whole 0x4000
        // landscape, but samples the surface you edit and query at the same four-unit
        // cell size the city tools use.
        4.0F,
#else
        2.0F,
#endif
        {},
        {},
        {},
    };

    // The game's one-unit heightfield: the base island, then the fixed intersection
    // block patches (which only write to the heightfield, never read it).
    CityHeightfield field;
    field.min_x = static_cast<std::int32_t>(terrain.min_x);
    field.min_z = static_cast<std::int32_t>(terrain.min_z);
    field.count_x = static_cast<std::int32_t>(terrain.max_x - terrain.min_x) + 1;
    field.count_z = static_cast<std::int32_t>(terrain.max_z - terrain.min_z) + 1;
    field.city_map = !stamps.disabled;
    // Terrain_RebuildLandTilesRoadPatchesAndChunkCaches only patches city maps.
    for (const auto patch_index : native_city_terrain_patch_indices) {
        if (stamps.disabled) break;
        apply_native_city_terrain_patch(field, expanded_graph, stamps, patch_index);
    }

    const auto terrain_height = [&](float world_x, float world_z) {
        if (world_x >= terrain.min_x && world_x <= terrain.max_x &&
            world_z >= terrain.min_z && world_z <= terrain.max_z) {
            const auto sample_x = std::clamp(
                static_cast<std::int32_t>(world_x), field.min_x, field.min_x + field.count_x - 1);
            const auto sample_z = std::clamp(
                static_cast<std::int32_t>(world_z), field.min_z, field.min_z + field.count_z - 1);
            return field.get(sample_x, sample_z);
        }
        return native_like_city_terrain_height(world_x, world_z, !stamps.disabled);
    };

    terrain.sample_count_x = std::max(2, static_cast<std::int32_t>(std::ceil((terrain.max_x - terrain.min_x) / terrain.sample_spacing)) + 1);
    terrain.sample_count_z = std::max(2, static_cast<std::int32_t>(std::ceil((terrain.max_z - terrain.min_z) / terrain.sample_spacing)) + 1);
    terrain.heights.resize(static_cast<std::size_t>(terrain.sample_count_x) * static_cast<std::size_t>(terrain.sample_count_z));
    terrain.mesh.unit_size = unit_size;
    terrain.mesh.material_names.push_back("city_terrain_grass");

    // Keep the patched local heights for picking and placing objects. The render mesh
    // is made below, in the game's 128-unit landscape chunks.
    parallel_rows(terrain.sample_count_z, [&](std::int32_t z) {
        for (auto x = 0; x < terrain.sample_count_x; ++x) {
            const auto world_x = std::min(terrain.max_x, terrain.min_x + static_cast<float>(x) * terrain.sample_spacing);
            const auto world_z = std::min(terrain.max_z, terrain.min_z + static_cast<float>(z) * terrain.sample_spacing);
            terrain.heights[static_cast<std::size_t>(z) * static_cast<std::size_t>(terrain.sample_count_x) + static_cast<std::size_t>(x)] =
                terrain_height(world_x, world_z);
        }
    });

    // Landscape size, proven in Terrain_RebuildLandTilesRoadPatchesAndChunkCaches @ d12e0:
    // 2049 source samples scaled up 8x to a world 0x4000 across. Rendering splits that
    // into 128-unit sub-chunks with steps from 1 to 64. Centre on the city, which does
    // not move (the game centres on the camera), so the whole island is always there
    // without rebuilding the terrain every time the camera moves.
    constexpr float landscape_min = -4096.0F;
    constexpr float landscape_max = landscape_min + 16384.0F;
    constexpr std::int32_t chunk_size = 128;
    auto lod_step = [&](float chunk_x, float chunk_z) { return terrain_fixed_step(terrain, chunk_x, chunk_z); };
    auto render_height = terrain_height;

    // Chunk rows build in parallel into their own buffers, then join in row order.
    const auto chunk_rows = static_cast<std::int32_t>((landscape_max - landscape_min) / static_cast<float>(chunk_size));
    std::vector<BuildingRenderBuffer> rows(static_cast<std::size_t>(chunk_rows));
    parallel_rows(chunk_rows, [&](std::int32_t row) {
        auto& mesh = rows[static_cast<std::size_t>(row)];
        const auto chunk_z = landscape_min + static_cast<float>(row * chunk_size);
        for (auto chunk_x = landscape_min; chunk_x < landscape_max; chunk_x += static_cast<float>(chunk_size)) {
            append_terrain_chunk(mesh, chunk_x, chunk_z, lod_step(chunk_x, chunk_z), unit_size, render_height, cell_covered);
        }
    });
    std::size_t vertex_total = 0, triangle_total = 0;
    for (const auto& row : rows) { vertex_total += row.vertices.size(); triangle_total += row.triangles.size(); }
    terrain.mesh.vertices.reserve(vertex_total);
    terrain.mesh.triangles.reserve(triangle_total);
    for (auto& row : rows) {
        const auto offset = static_cast<std::uint32_t>(terrain.mesh.vertices.size());
        terrain.mesh.vertices.insert(terrain.mesh.vertices.end(), row.vertices.begin(), row.vertices.end());
        for (auto triangle : row.triangles) {
            for (auto& index : triangle.indices) index += offset;
            terrain.mesh.triangles.push_back(triangle);
        }
        row = {};
    }

    sort_triangles_into_native_material_runs(terrain.mesh);
    build_terrain_draw_chunks(terrain.mesh);
    terrain.covered_cells.insert(stamps.masked.begin(), stamps.masked.end());
    terrain.city_map = !stamps.disabled;
    return terrain;
}

// Traffic light corner objects. Placement: MapConnector_PlaceIntersectionPerimeterTiles
// @20130 only stores corner codes when more than two streets meet. It stores one
// for each neighbour that exists, in the diagonal corner tile just outside the
// footprint, one level above the road. Drawing: _area_interior_buildsectormodel
// @14820 places the type-1 street object at the centre of the corner cell plus the
// type's own offset (0, 3.25, -1), turned by bits 10-11 of the code (N=0, E=1, S=2,
// W=3 quarter turns). So the post stands on the curb plate, moved one unit toward
// the street its arm reaches over. The model's arm points along local -z.
std::vector<CityTrafficLightPole> build_city_traffic_light_poles(
    const NativeCityGraph& graph,
    float unit_size) {
    std::vector<CityTrafficLightPole> poles;
    for (const auto& intersection : graph.intersections) {
        const auto neighbor_count =
            static_cast<int>(intersection.street_south >= 0) +
            static_cast<int>(intersection.street_north >= 0) +
            static_cast<int>(intersection.street_west >= 0) +
            static_cast<int>(intersection.street_east >= 0);
        if (neighbor_count <= 2) {
            continue;
        }

        // Where the pole meets the ground: the bottom of the corner cell (+0.25)
        // plus the model offset (origin y 3.25, pole half-height 3), which is 0.5
        // above the cell level, the top of the curb plate.
        const auto base_y = native_road_surface_y(intersection.level) + 0.25F;
        // The sign names the street the arm reaches over (RenderModels @212e0): the
        // neighbour one step clockwise in (south, west, north, east) from the pole's
        // direction, or else the one counter-clockwise, or else street 0.
        const std::array<std::int32_t, 4> neighbours{
            intersection.street_south, intersection.street_west, intersection.street_north, intersection.street_east};
        auto add_pole = [&](std::int32_t tile_x, std::int32_t tile_z, Vec3 arm, std::size_t direction) {
            const auto center_x = (static_cast<float>(tile_x) + 0.5F) * unit_size;
            const auto center_z = (static_cast<float>(tile_z) + 0.5F) * unit_size;
            auto street = neighbours[(direction + 1U) & 3U];
            if (street < 0) street = neighbours[(direction + 3U) & 3U];
            if (street < 0) street = 0;
            const auto name = static_cast<std::size_t>(street) < graph.streets.size()
                ? graph.streets[static_cast<std::size_t>(street)].name_index
                : 0;
            poles.push_back({
                {center_x + arm.x, base_y, center_z + arm.z},
                arm,
                name,
            });
        };
        if (intersection.street_north >= 0) {
            add_pole(intersection.gx + intersection.ext_x_pos, intersection.gz + intersection.ext_z_pos,
                {0.0F, 0.0F, -1.0F}, 2U);
        }
        if (intersection.street_south >= 0) {
            add_pole(intersection.gx - intersection.ext_x_pos - 1, intersection.gz - intersection.ext_z_pos - 1,
                {0.0F, 0.0F, 1.0F}, 0U);
        }
        if (intersection.street_east >= 0) {
            add_pole(intersection.gx - intersection.ext_x_pos - 1, intersection.gz + intersection.ext_z_pos,
                {1.0F, 0.0F, 0.0F}, 3U);
        }
        if (intersection.street_west >= 0) {
            add_pole(intersection.gx + intersection.ext_x_pos, intersection.gz - intersection.ext_z_pos - 1,
                {-1.0F, 0.0F, 0.0F}, 1U);
        }
    }
    return poles;
}

} // namespace

// -----------------------------------------------------------------------------
// Building scene and render buffers
// -----------------------------------------------------------------------------

float terrain_height_at(const CityTerrain& terrain, float world_x, float world_z) {
    if (terrain.heights.empty() || terrain.sample_count_x <= 0 || terrain.sample_count_z <= 0 || terrain.sample_spacing <= 0.0F) {
        return 0.0F;
    }

    const auto sx = std::clamp((world_x - terrain.min_x) / terrain.sample_spacing, 0.0F, static_cast<float>(terrain.sample_count_x - 1));
    const auto sz = std::clamp((world_z - terrain.min_z) / terrain.sample_spacing, 0.0F, static_cast<float>(terrain.sample_count_z - 1));
    const auto x0 = static_cast<std::int32_t>(std::floor(sx));
    const auto z0 = static_cast<std::int32_t>(std::floor(sz));
    const auto x1 = std::min(x0 + 1, terrain.sample_count_x - 1);
    const auto z1 = std::min(z0 + 1, terrain.sample_count_z - 1);
    const auto tx = sx - static_cast<float>(x0);
    const auto tz = sz - static_cast<float>(z0);
    auto height_at = [&](std::int32_t x, std::int32_t z) {
        return terrain.heights[static_cast<std::size_t>(z * terrain.sample_count_x + x)];
    };
    const auto z0_height = height_at(x0, z0) + (height_at(x1, z0) - height_at(x0, z0)) * tx;
    const auto z1_height = height_at(x0, z1) + (height_at(x1, z1) - height_at(x0, z1)) * tx;
    return z0_height + (z1_height - z0_height) * tz;
}

SblBlockMesh make_native_builtin_block_mesh() {
    return make_native_builtin_block_mesh_impl();
}

std::vector<NamedBlockMesh> build_native_block_mesh_catalog(const std::vector<NamedBlockMesh>& file_block_meshes) {
    std::vector<NamedBlockMesh> catalog;
    catalog.reserve(native_builtin_block_mesh_count + file_block_meshes.size());

    const auto built_in_mesh = make_native_builtin_block_mesh();
    for (std::size_t index = 0; index < native_builtin_block_mesh_count; ++index) {
        catalog.push_back({"", built_in_mesh});
    }
    catalog.insert(catalog.end(), file_block_meshes.begin(), file_block_meshes.end());
    return catalog;
}

SbbScene build_sbb_scene(const SbbBuilding& building) {
    return build_sbb_scene(building, {}, {});
}

SbbScene build_sbb_scene(const SbbBuilding& building, const std::vector<NamedBlockMesh>& block_meshes) {
    return build_sbb_scene(building, block_meshes, {});
}

SbbScene build_sbb_scene(
    const SbbBuilding& building,
    const std::vector<NamedBlockMesh>& block_meshes,
    const std::vector<NamedBuildblockTemplate>& buildblock_templates) {
    SbbScene scene;
    scene.width = static_cast<float>(std::max(1U, building.extent_x)) * scene.unit_size;
    scene.height = static_cast<float>(std::max(1U, building.extent_y)) * scene.unit_size;
    scene.depth = static_cast<float>(std::max(1U, building.extent_z)) * scene.unit_size;

    for (const auto& cell : building.cells) {
        if (!cell.occupied) {
            continue;
        }

        const auto piece_payload = exterior_piece_payload(cell);
        if (piece_payload.has_value()) {
            if (const auto* mesh = resolve_building_block_mesh(block_meshes, building.piece_names, piece_payload->index)) {
                scene.pieces.push_back({
                    .origin = {
                        (static_cast<float>(cell.x) - static_cast<float>(building.anchor_x)) * scene.unit_size,
                        (static_cast<float>(cell.y) - static_cast<float>(building.anchor_y)) * scene.unit_size,
                        (static_cast<float>(cell.z) - static_cast<float>(building.anchor_z)) * scene.unit_size,
                    },
                    .mesh = *mesh,
                    .rotation = static_cast<std::int32_t>((piece_payload->word >> 24U) & 3U),
                });
                continue;
            }
        }

        if (is_solid_building_cell(cell) && cell.interior_block() == 0xffffU && cell.cell_type() < building.block_names.size()) {
            if (const auto* template_file = find_buildblock_template(buildblock_templates, building.block_names[cell.cell_type()])) {
                const auto* east = find_cell_at(building.cells, cell.x + 1U, cell.y, cell.z);
                const auto* south = find_cell_at(building.cells, cell.x, cell.y, cell.z + 1U);
                const auto side_codes = native_edge_side_codes(cell, east, south);
                scene.portal_edges.push_back({
                    &cell,
                    {
                        (static_cast<float>(cell.x) - static_cast<float>(building.anchor_x)) * scene.unit_size,
                        (static_cast<float>(cell.y) - static_cast<float>(building.anchor_y)) * scene.unit_size,
                        (static_cast<float>(cell.z) - static_cast<float>(building.anchor_z)) * scene.unit_size,
                    },
                    cell.exterior_block(),
                    side_codes,
                    build_portal_wall_rects_for_edge(*template_file, side_codes),
                    *template_file,
                });
            }
        }

        const auto center = building_cell_center_world(building, cell, scene.unit_size);
        scene.cells.push_back({
            center,
            {scene.unit_size, scene.unit_size, scene.unit_size},
            visible_payload_kind(cell),
        });
    }

    return scene;
}

// The room inside a cell whose interior block is 0xffff. The game builds it with
// _block_createbuildblock(0, key) @1a990, from the key that
// InstantiateSbbVolumeIntoWorldAndMetadata @23ac0 packs for each cell: side codes
// for north (the cell's +0x0c word), east (the +x cell's +0x10), south (the +z
// cell's +0x0c), and west (the cell's +0x10); bit 16 for the floor and bit 17 for
// the ceiling, from the placed flag; and the cell type, which picks the buildblock.
// Side code 1 is a plain wall (_block_addbasicsurface @19140), and 3..15 is a wall
// with that buildblock layer's windows. Walls stand 1/16 inside the cell's edge.
// The floor (_block_addbasicsurface2 @194b0) lies at 1/16, pulled in 1/16 from each
// side that has a wall, and the ceiling is at 7/8. Surfaces use the inside texture
// set (slots 8..15). Floors are two-sided: the game's rooms only set the floor bit,
// so a room's ceiling is the floor above, seen from below. Walls only face inward.
// ponytail: code 2 (the curved corner pieces) and the door-flag bits are not made
// yet; add them when a building needs them.
void add_native_interior_room(
    BuildingRenderBuffer& buffer,
    VertexIndexMap& vertex_indices,
    const SbbCell& cell,
    const std::array<std::uint32_t, 4>& codes,
    bool ceiling,
    const SblTemplate* template_file,
    float cx,
    float cy,
    float cz) {
    const auto unit = buffer.unit_size;
    const Vec3 base{cx - unit * 0.5F, cy - unit * 0.5F, cz - unit * 0.5F};
    const auto at = [&](float x, float y, float z) { return Vec3{base.x + x * unit, base.y + y * unit, base.z + z * unit}; };
    // Floors are two-sided (the floor above is this room's ceiling). Walls only face
    // into the room: from outside they sit 1/16 behind the outside wall and would
    // flicker against it from far away. A face you can see has its normal pointing away
    // from you, so an inward wall's normal points away from the cell's centre.
    // Material slots are numbered the way the room builder numbers them
    // (_block_addbeam @19940): 0..7 point into the cell's inside set, and 8..15 into
    // its outside set. Wall faces turn with the side, so a room's N/E/S/W walls use
    // inside slots 1/2/3/4, and their outer faces use outside slots 3/4/1/2 (the
    // shell's N/E/S/W slots).
    const auto shorts_index = [](std::uint32_t game_slot) { return game_slot < 8U ? game_slot + 8U : game_slot - 8U; };
    const auto wall_slot = [](BuildingFaceSide side) -> std::uint32_t {
        switch (side) {
            case BuildingFaceSide::North: return 1U;
            case BuildingFaceSide::East: return 2U;
            case BuildingFaceSide::South: return 3U;
            case BuildingFaceSide::West: return 4U;
            default: return static_cast<std::uint32_t>(material_slot_for_cell_side(side));
        }
    };
    const auto add_surface = [&](const std::array<Vec3, 4>& p, const std::array<Vec2f, 4>& uv, BuildingFaceSide side, bool both,
                                 std::optional<std::uint32_t> slot_override = std::nullopt, bool glass = false) {
        const auto game_slot = slot_override.value_or(wall_slot(side));
        const auto material = material_for_cell_slot(cell, shorts_index(game_slot));
        const auto first = buffer.triangles.size();
        const std::array<Vec3, 4> reversed{p[3], p[2], p[1], p[0]};
        const std::array<Vec2f, 4> reversed_uv{uv[3], uv[2], uv[1], uv[0]};
        const auto keep = [&] {
            if (both) return true;
            const Vec3 e1{p[1].x - p[0].x, p[1].y - p[0].y, p[1].z - p[0].z};
            const Vec3 e2{p[2].x - p[0].x, p[2].y - p[0].y, p[2].z - p[0].z};
            const Vec3 normal{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
            const auto centre = at(0.5F, 0.5F, 0.5F);
            const Vec3 out{(p[0].x + p[2].x) * 0.5F - centre.x, (p[0].y + p[2].y) * 0.5F - centre.y, (p[0].z + p[2].z) * 0.5F - centre.z};
            return normal.x * out.x + normal.y * out.y + normal.z * out.z > 0.0F;
        }();
        if (both || keep) add_quad(buffer, vertex_indices, p, uv, material, glass);
        if (both || !keep) add_quad(buffer, vertex_indices, reversed, reversed_uv, material, glass);
        const auto slot = static_cast<std::uint8_t>(shorts_index(game_slot));
        for (auto index = first; index < buffer.triangles.size(); ++index) {
            buffer.triangles[index].interior = true;
            buffer.triangles[index].cell = static_cast<std::uint32_t>(cell.index);
            buffer.triangles[index].slot = slot;
        }
    };
    constexpr float inset = 1.0F / 16.0F;
    const auto walled = [&](std::size_t side) { return codes[side] != 0U && codes[side] != 2U; };
    const auto flat = [&](float y, BuildingFaceSide side) {
        const auto x0 = walled(3) ? inset : 0.0F;
        const auto x1 = walled(1) ? 1.0F - inset : 1.0F;
        const auto z0 = walled(0) ? inset : 0.0F;
        const auto z1 = walled(2) ? 1.0F - inset : 1.0F;
        add_surface({at(x0, y, z0), at(x1, y, z0), at(x1, y, z1), at(x0, y, z1)},
            {Vec2f{x0, z0}, Vec2f{x1, z0}, Vec2f{x1, z1}, Vec2f{x0, z1}}, side, true);
    };
    if ((cell.placed_flag() & 1U) != 0U) flat(inset, BuildingFaceSide::Down);
    if (ceiling) flat(7.0F / 8.0F, BuildingFaceSide::Up);

    // Side walls, built the way _block_createbuildblock @1a990 builds them. Code 1 is a
    // plain wall (_block_addbasicsurface). Codes 3..15 go through BuildBlock_AddBeams
    // @1a1c0:
    // - The layer's BOUNDS rectangle is the opening. Up to four boxes 1/16 deep frame
    //   it (left, right, bottom, top). Their faces toward the opening (the jambs, sill,
    //   and lintel) use inside slot 6.
    // - Then every beam record is a box. Type 0 is flush with the wall (1/16 deep).
    //   Types 1 and 2 stick out 1/16 each way (3/16 and 5/16 deep). Other types are
    //   thin dividers (1/64 deep at 1/128, slot 7).
    // - _block_addwindows @1a590 then adds a glass pane for each window record, 1/64 in
    //   (1/256 for portal type 1), but only when the layer's portal type is 0 or the
    //   side is north or east.
    // So door layers (portal type 1, with bounds and no windows) are open doorways.
    // A pane is one quad, facing out of the cell. The game draws it without culling and
    // shades its two sides differently (see the glass pass).
    // Rooms only exist in the near view, where the shell's buildblock faces are not
    // drawn, so the frame's outer face never meets the shell.
    // u runs along the wall and v up it; d is how far in from the cell's edge.
    const std::array<BuildingFaceSide, 4> faces{
        BuildingFaceSide::North, BuildingFaceSide::East, BuildingFaceSide::South, BuildingFaceSide::West};
    for (std::size_t side = 0; side < 4; ++side) {
        const auto wall = [&](float u, float v, float d) {
            switch (side) {
                case 0: return at(u, v, d);
                case 1: return at(1.0F - d, v, u);
                case 2: return at(u, v, 1.0F - d);
                default: return at(d, v, u);
            }
        };
        // A quad flat against the wall at depth d, over [u0,u1] x [v0,v1].
        const auto facing = [&](float u0, float u1, float v0, float v1, float d) {
            return std::array<Vec3, 4>{wall(u0, v1, d), wall(u1, v1, d), wall(u1, v0, d), wall(u0, v0, d)};
        };
        // A box over [u0,u1] x [v0,v1] x [d0,d1]. Faces toward the room or the edge use
        // the wall slot, and the four sides use `edge_slot`.
        const auto box = [&](float u0, float u1, float v0, float v1, float d0, float d1, std::uint32_t edge_slot) {
            add_surface(facing(u0, u1, v0, v1, d1), quad_uvs(u0, u1 - u0, v0, v1 - v0), faces[side], true);
            add_surface(facing(u0, u1, v0, v1, d0), quad_uvs(u0, u1 - u0, v0, v1 - v0), faces[side], true);
            const auto along = quad_uvs(u0, u1 - u0, d0, d1 - d0);
            const auto up = quad_uvs(v0, v1 - v0, d0, d1 - d0);
            add_surface({wall(u0, v0, d0), wall(u1, v0, d0), wall(u1, v0, d1), wall(u0, v0, d1)}, along, faces[side], true, edge_slot);
            add_surface({wall(u0, v1, d0), wall(u1, v1, d0), wall(u1, v1, d1), wall(u0, v1, d1)}, along, faces[side], true, edge_slot);
            add_surface({wall(u0, v0, d0), wall(u0, v1, d0), wall(u0, v1, d1), wall(u0, v0, d1)}, up, faces[side], true, edge_slot);
            add_surface({wall(u1, v0, d0), wall(u1, v1, d0), wall(u1, v1, d1), wall(u1, v0, d1)}, up, faces[side], true, edge_slot);
        };
        if (codes[side] == 1U) {
            add_surface(facing(0.0F, 1.0F, 0.0F, 1.0F, inset), quad_uvs(0.0F, 1.0F, 0.0F, 1.0F), faces[side], false);
            continue;
        }
        if (codes[side] == 2U) {
            // Corner fill: two full-height strips, 1/16 wide, at the side's starting
            // corner (NW, NE, SE, SW for N, E, S, W; the game's wall frame runs
            // backwards on S and W). They close the gap where a neighbour's inset
            // wall ends. The second strip uses the previous side's wall slot.
            const bool reversed = side >= 2U;
            const auto u0 = reversed ? 1.0F - inset : 0.0F;
            const auto u1 = reversed ? 1.0F : inset;
            const auto edge = reversed ? 1.0F - inset : inset;
            add_surface(facing(u0, u1, 0.0F, 1.0F, inset), quad_uvs(u0, inset, 0.0F, 1.0F), faces[side], true);
            add_surface({wall(edge, 1.0F, 0.0F), wall(edge, 1.0F, inset), wall(edge, 0.0F, inset), wall(edge, 0.0F, 0.0F)},
                quad_uvs(0.0F, inset, 0.0F, 1.0F), faces[side], true, wall_slot(faces[(side + 3U) % 4U]));
            continue;
        }
        const auto* layer = buildblock_layer_for_side_code(template_file, codes[side]);
        const auto portal_side = portal_side_for_building_face(faces[side]);
        if (layer == nullptr || !portal_side.has_value()) continue;
        const auto rect_of = [&](float x, float width, float y, float height) {
            return building_room_rect(*layer, *portal_side, x, width, y, height);
        };
        const auto opening = rect_of(layer->bounds[0], layer->bounds[1], layer->bounds[2], layer->bounds[3]);
        // Frame: the wall without the opening, facing the room at depth `inset`.
        const auto frame = opening ? subtract_city_window_rects({*opening}) : std::vector<PortalRect>{unit_portal_rect()};
        for (const auto& rect : frame) {
            add_surface(facing(rect.x, rect.max_x, rect.y, rect.max_y, inset),
                quad_uvs(rect.x, rect.width, rect.y, rect.height), faces[side], false);
            // The outer face on the cell's edge, for portal type 0 only. It shows in
            // the near view, where the shell's window face is not drawn.
            if (layer->portal_type == 0U) {
                add_surface(facing(rect.x, rect.max_x, rect.y, rect.max_y, 0.0F),
                    quad_uvs(rect.x, rect.width, rect.y, rect.height), faces[side], true,
                    8U + static_cast<std::uint32_t>(material_slot_for_cell_side(faces[side])));
            }
        }
        if (opening) {
            constexpr std::uint32_t kFrameSlot = 6U;
            const auto& o = *opening;
            const auto along = quad_uvs(o.x, o.width, 0.0F, inset);
            const auto up = quad_uvs(o.y, o.height, 0.0F, inset);
            if (o.y > 0.0F) add_surface({wall(o.x, o.y, 0.0F), wall(o.max_x, o.y, 0.0F), wall(o.max_x, o.y, inset), wall(o.x, o.y, inset)}, along, faces[side], true, kFrameSlot);
            if (o.max_y < 1.0F) add_surface({wall(o.x, o.max_y, 0.0F), wall(o.max_x, o.max_y, 0.0F), wall(o.max_x, o.max_y, inset), wall(o.x, o.max_y, inset)}, along, faces[side], true, kFrameSlot);
            if (o.x > 0.0F) add_surface({wall(o.x, o.y, 0.0F), wall(o.x, o.max_y, 0.0F), wall(o.x, o.max_y, inset), wall(o.x, o.y, inset)}, up, faces[side], true, kFrameSlot);
            if (o.max_x < 1.0F) add_surface({wall(o.max_x, o.y, 0.0F), wall(o.max_x, o.max_y, 0.0F), wall(o.max_x, o.max_y, inset), wall(o.max_x, o.y, inset)}, up, faces[side], true, kFrameSlot);
        }
        for (const auto& beam : layer->beams) {
            const auto rect = rect_of(beam.x, beam.width, beam.y, beam.height);
            if (!rect) continue;
            float d0 = 0.0F, d1 = inset;
            std::uint32_t slot = 6U;
            if (beam.type == 1U) { d0 = -inset; d1 = -inset + 0.1875F; }
            else if (beam.type == 2U) { d0 = -inset; d1 = -inset + 0.3125F; }
            else if (beam.type != 0U) { d0 = 1.0F / 128.0F; d1 = d0 + 1.0F / 64.0F; slot = 7U; }
            box(rect->x, rect->max_x, rect->y, rect->max_y, d0, d1, slot);
        }
        if (layer->portal_type == 0U || side < 2U) {
            const auto depth = layer->portal_type == 1U ? 1.0F / 256.0F : 1.0F / 64.0F;
            for (const auto& window : layer->windows) {
                if (const auto rect = rect_of(window.x, window.width, window.y, window.height)) {
                    add_surface(facing(rect->x, rect->max_x, rect->y, rect->max_y, depth),
                        quad_uvs(rect->x, rect->width, rect->y, rect->height), faces[side], false, std::nullopt, true);
                }
            }
        }
    }
}

BuildingRenderBuffer build_native_building_render_buffer(
    const SbbBuilding& building,
    const std::vector<NamedBlockMesh>& block_meshes,
    const std::vector<NamedBuildblockTemplate>& buildblock_templates,
    std::optional<std::uint32_t> floor,
    bool roofs) {
    PAFTA_PROFILE_FUNCTION();
    BuildingRenderBuffer buffer;
    const auto unit_size = buffer.unit_size;
    VertexIndexMap vertex_indices;
    vertex_indices.reserve(building.cells.size() * 4U);
    buffer.vertices.reserve(building.cells.size() * 4U);
    buffer.triangles.reserve(building.cells.size() * 6U);
    const auto cell_lookup = build_cell_index(building.cells);

    const auto piece_resolution_count = begins_with_native_builtin_block_meshes(block_meshes)
        ? std::max(building.piece_names.size(), block_meshes.size())
        : building.piece_names.size();
    std::vector<const SblBlockMesh*> resolved_piece_meshes(piece_resolution_count);
    for (std::size_t index = 0; index < resolved_piece_meshes.size(); ++index) {
        resolved_piece_meshes[index] = resolve_building_block_mesh(
            block_meshes,
            building.piece_names,
            static_cast<std::uint32_t>(index));
    }
    std::vector<const SblTemplate*> resolved_buildblock_templates(building.block_names.size());
    for (std::size_t index = 0; index < resolved_buildblock_templates.size(); ++index) {
        resolved_buildblock_templates[index] = resolve_building_buildblock_template(
            buildblock_templates,
            building.block_names,
            static_cast<std::uint32_t>(index));
    }

    const auto resolved_piece_mesh = [&](std::uint32_t index) -> const SblBlockMesh* {
        return index < resolved_piece_meshes.size()
            ? resolved_piece_meshes[index]
            : &native_builtin_block_mesh_singleton();
    };

    // InstantiateSbbVolumeIntoWorldAndMetadata only places rooms in cells that no
    // interior piece's footprint already covers (a 2-wide garage door owns its second
    // cell). Pieces reach +x, +z, and +y from their own cell.
    std::unordered_set<std::uint64_t> piece_footprint;
    const auto footprint_key = [](std::uint64_t x, std::uint64_t y, std::uint64_t z) { return x << 42U | y << 21U | z; };
    for (const auto& cell : building.cells) {
        const auto payload = interior_piece_payload(cell);
        const auto* mesh = payload ? resolved_piece_mesh(payload->index) : nullptr;
        if (mesh == nullptr) continue;
        const bool turned = ((payload->word >> 24U) & 1U) != 0U;
        const auto size_x = turned ? mesh->depth : mesh->width;
        const auto size_z = turned ? mesh->width : mesh->depth;
        for (std::uint32_t dy = 0; dy < mesh->height; ++dy)
            for (std::uint32_t dz = 0; dz < size_z; ++dz)
                for (std::uint32_t dx = 0; dx < size_x; ++dx)
                    if ((dx | dy | dz) != 0U) piece_footprint.insert(footprint_key(cell.x + dx, cell.y + dy, cell.z + dz));
    }

    auto cell_origin = [&](const SbbCell& cell) {
        return Vec3{
            (static_cast<float>(cell.x) - static_cast<float>(building.anchor_x)) * unit_size,
            (static_cast<float>(cell.y) - static_cast<float>(building.anchor_y)) * unit_size,
            (static_cast<float>(cell.z) - static_cast<float>(building.anchor_z)) * unit_size,
        };
    };

    for (const auto& cell : building.cells) {
        // Showing one floor hides the others, so the floor above no longer
        // covers this floor's top faces.
        if (!cell.occupied || (floor.has_value() && cell.y != *floor)) {
            continue;
        }
        // Tag every face this cell makes with the cell and its texture slot. A
        // selector call names the slot of the triangles made after it.
        const auto cell_index = static_cast<std::uint32_t>(&cell - building.cells.data());
        std::size_t tagged = buffer.triangles.size();
        std::uint8_t pending_slot = 0xffU;
        const auto tag = [&](std::uint32_t next_slot) {
            for (; tagged < buffer.triangles.size(); ++tagged) {
                buffer.triangles[tagged].cell = cell_index;
                buffer.triangles[tagged].slot = pending_slot;
            }
            pending_slot = static_cast<std::uint8_t>(next_slot);
        };

        const auto piece_payload = exterior_piece_payload(cell);
        if (piece_payload.has_value()) {
            if (const auto* mesh = resolved_piece_mesh(piece_payload->index)) {
                const auto origin = cell_origin(cell);
                const auto rotation = static_cast<std::int32_t>((piece_payload->word >> 24U) & 3U);
                add_rotated_block_mesh_to_buffer(
                    buffer,
                    vertex_indices,
                    *mesh,
                    origin,
                    rotation,
                    [&](BuildingRenderBuffer&, const SblBlockFace&, const NativeRotatedBlockFace& rotated_face) {
                        tag(piece_face_slot(rotated_face.material, false));
                        return material_for_piece_face(cell, rotated_face.material, false);
                    });
            }
        }

        const auto interior_payload = interior_piece_payload(cell);
        if (interior_payload.has_value()) {
            if (const auto* mesh = resolved_piece_mesh(interior_payload->index)) {
                const auto origin = cell_origin(cell);
                const auto rotation = static_cast<std::int32_t>((interior_payload->word >> 24U) & 3U);
                add_rotated_block_mesh_to_buffer(
                    buffer,
                    vertex_indices,
                    *mesh,
                    origin,
                    rotation,
                    [&](BuildingRenderBuffer&, const SblBlockFace&, const NativeRotatedBlockFace& rotated_face) {
                        tag(piece_face_slot(rotated_face.material, true));
                        return material_for_piece_face(cell, rotated_face.material, true);
                    });
            }
        }

        const auto cx = (static_cast<float>(cell.x) - static_cast<float>(building.anchor_x) + 0.5F) * unit_size;
        const auto cy = (static_cast<float>(cell.y) - static_cast<float>(building.anchor_y) + 0.5F) * unit_size;
        const auto cz = (static_cast<float>(cell.z) - static_cast<float>(building.anchor_z) + 0.5F) * unit_size;

        const auto* west = find_cell_at(cell_lookup, cell.x - 1U, cell.y, cell.z);
        const auto* east = find_cell_at(cell_lookup, cell.x + 1U, cell.y, cell.z);
        const auto* north = find_cell_at(cell_lookup, cell.x, cell.y, cell.z - 1U);
        const auto* south = find_cell_at(cell_lookup, cell.x, cell.y, cell.z + 1U);
        const auto* up = floor.has_value() ? nullptr : find_cell_at(cell_lookup, cell.x, cell.y + 1U, cell.z);
        const auto* template_file = cell.cell_type() < resolved_buildblock_templates.size()
            ? resolved_buildblock_templates[cell.cell_type()]
            : (buildblock_templates.empty() ? nullptr : &buildblock_templates.front().template_file);

        if (cell.interior_block() == 0xffffU && !piece_footprint.contains(footprint_key(cell.x, cell.y, cell.z))) {
            tag(0xffU);
            // InstantiateSbbVolumeIntoWorldAndMetadata @23ac0 packs the room key:
            // side codes N (own +0x0c), E ((x+1) +0x10), S ((z+1) +0x0c), and
            // W (own +0x10). It turns an open side into code 2 (a corner fill)
            // where a neighbour's wall ends at that side's starting corner.
            // Sbb_BuildBuildingConnectivityMetadata @29120 works out the ceiling:
            // the top layer, or when the cell above has no exterior or interior
            // block, or has its own floor.
            const auto code_at = [&](std::int64_t dx, std::int64_t dz, bool north_word) -> std::uint32_t {
                if (static_cast<std::int64_t>(cell.x) + dx < 0 || static_cast<std::int64_t>(cell.z) + dz < 0) return 0U;
                const auto* other = find_cell_at(cell_lookup, static_cast<std::uint32_t>(cell.x + dx), cell.y,
                    static_cast<std::uint32_t>(cell.z + dz));
                if (other == nullptr) return 0U;
                return north_word ? other->exterior_portal_face() : other->interior_portal_face();
            };
            const auto wall_code = [](std::uint32_t code) { return code == 1U || (code >= 3U && code <= 15U); };
            std::array<std::uint32_t, 4> codes{
                cell.exterior_portal_face(),
                code_at(1, 0, false),
                code_at(0, 1, true),
                cell.interior_portal_face(),
            };
            const auto [n, e, s, w] = codes;
            if (n == 0U) {
                if (w == 0U && (wall_code(code_at(-1, 0, true)) || wall_code(code_at(0, -1, false)))) codes[0] = 2U;
                if (e == 0U && (wall_code(code_at(1, 0, true)) || wall_code(code_at(1, -1, false)))) codes[1] = 2U;
            }
            if (s == 0U) {
                if (e == 0U && (wall_code(code_at(1, 1, true)) || wall_code(code_at(1, 1, false)))) codes[2] = 2U;
                if (w == 0U && (wall_code(code_at(-1, 1, true)) || wall_code(code_at(0, 1, false)))) codes[3] = 2U;
            }
            const auto* above = floor.has_value() ? nullptr : find_cell_at(cell_lookup, cell.x, cell.y + 1U, cell.z);
            const bool ceiling = above == nullptr || !above->occupied || above->exterior_block() == 0U ||
                above->interior_block() == 0U || above->placed_flag() != 0U;
            add_native_interior_room(buffer, vertex_indices, cell, codes, ceiling, template_file, cx, cy, cz);
            // The room tags its own slots; here the cell is its position.
            for (; tagged < buffer.triangles.size(); ++tagged) buffer.triangles[tagged].cell = cell_index;
        }

        if (!cell_has_exterior_surface(&cell)) {
            tag(0xffU);
            continue;
        }
        // Without roofs, every upward face counts as covered.
        static const auto roof_cover = native_city_block_solid_cell();
        if (!roofs) up = &roof_cover;

        // World_SetTileBlockAndResolveNeighborLinks @f830 gives a solid cell all six
        // faces and only removes those against a solid neighbour (and the bottom at world
        // y 0), so you can see the underside of overhangs.
        const auto* down = floor.has_value() || cell.y == 0U ? nullptr : find_cell_at(cell_lookup, cell.x, cell.y - 1U, cell.z);
        const std::array<SolidCellFaceCheck, 6> native_sides{{
            {BuildingFaceSide::South, south},
            {BuildingFaceSide::West, west},
            {BuildingFaceSide::North, north},
            {BuildingFaceSide::East, east},
            {BuildingFaceSide::Up, up},
            {BuildingFaceSide::Down, down},
        }};

        add_native_solid_cell_faces(
            buffer,
            vertex_indices,
            cell,
            cx,
            cy,
            cz,
            native_sides,
            east,
            south,
            template_file,
            [&](BuildingFaceSide side) {
                tag(static_cast<std::uint32_t>(material_slot_for_cell_side(side)));
                return material_for_cell_side(cell, side);
            });
        tag(0xffU);
    }

    sort_triangles_into_native_material_runs(buffer);
    return buffer;
}

BuildingRenderBuffer build_native_building_tool_preview_render_buffer(
    const SbbBuilding& building,
    const std::vector<NamedBlockMesh>& block_meshes,
    const std::vector<NamedBuildblockTemplate>& buildblock_templates,
    std::optional<std::uint32_t> floor) {
    BuildingRenderBuffer buffer;
    VertexIndexMap vertex_indices;
    vertex_indices.reserve(building.cells.size() * 4U);
    buffer.vertices.reserve(building.cells.size() * 4U);
    buffer.triangles.reserve(building.cells.size() * 6U);
    const auto cell_index = build_cell_index(building.cells);

    const auto piece_resolution_count = begins_with_native_builtin_block_meshes(block_meshes)
        ? std::max(building.piece_names.size(), block_meshes.size())
        : building.piece_names.size();
    std::vector<const SblBlockMesh*> resolved_piece_meshes(piece_resolution_count);
    for (std::size_t index = 0; index < resolved_piece_meshes.size(); ++index) {
        resolved_piece_meshes[index] = resolve_building_block_mesh(
            block_meshes,
            building.piece_names,
            static_cast<std::uint32_t>(index));
    }

    auto cell_origin = [&](const SbbCell& cell) {
        return Vec3{
            static_cast<float>(cell.x) * buffer.unit_size,
            static_cast<float>(cell.y) * buffer.unit_size,
            static_cast<float>(cell.z) * buffer.unit_size,
        };
    };

    auto add_piece = [&](const SbbCell& cell, const SbbCellPayload& payload, bool interior) {
        const auto* mesh = payload.index < resolved_piece_meshes.size()
            ? resolved_piece_meshes[payload.index]
            : &native_builtin_block_mesh_singleton();
        if (mesh == nullptr) {
            return;
        }
        const auto rotation = static_cast<std::int32_t>((payload.word >> 24U) & 3U);
        add_rotated_block_mesh_to_buffer(
            buffer,
            vertex_indices,
            *mesh,
            cell_origin(cell),
            rotation,
            [&cell, interior](BuildingRenderBuffer&, const SblBlockFace&, const NativeRotatedBlockFace& rotated_face) {
                return material_for_piece_face(cell, rotated_face.material, interior);
            });
    };

    auto is_solid = [](const SbbCell* cell) {
        return cell != nullptr && (cell->exterior_block() & 0xe0000000U) == 0x40000000U;
    };

    for (const auto& cell : building.cells) {
        if (!cell.occupied || (floor.has_value() && cell.y != *floor)) {
            continue;
        }

        if (const auto payload = interior_piece_payload(cell)) {
            add_piece(cell, *payload, true);
        }
        if (const auto payload = exterior_piece_payload(cell)) {
            add_piece(cell, *payload, false);
        }

        if ((cell.exterior_block() & 0xe0000000U) != 0x40000000U) {
            continue;
        }

        const auto* west = find_cell_at(cell_index, cell.x - 1U, cell.y, cell.z);
        const auto* east = find_cell_at(cell_index, cell.x + 1U, cell.y, cell.z);
        const auto* north = find_cell_at(cell_index, cell.x, cell.y, cell.z - 1U);
        const auto* south = find_cell_at(cell_index, cell.x, cell.y, cell.z + 1U);
        const auto* up = find_cell_at(cell_index, cell.x, cell.y + 1U, cell.z);

        if (cell.interior_block() == 0xffffU) {
            add_native_tool_floor_cell_mesh(
                buffer,
                vertex_indices,
                building,
                buildblock_templates,
                cell,
                east,
                south);
            continue;
        }

        std::uint32_t boundary_mask = 0U;
        if ((cell.interior_block() == 0U || (south != nullptr ? south->exterior_portal_face() : 0U) < 2U) && !is_solid(south)) {
            boundary_mask |= 1U << 1U;
        }
        if ((cell.interior_block() == 0U || cell.interior_portal_face() < 2U) && !is_solid(west)) {
            boundary_mask |= 1U << 2U;
        }
        if ((cell.interior_block() == 0U || cell.exterior_portal_face() < 2U) && !is_solid(north)) {
            boundary_mask |= 1U << 3U;
        }
        if ((cell.interior_block() == 0U || (east != nullptr ? east->interior_portal_face() : 0U) < 2U) && !is_solid(east)) {
            boundary_mask |= 1U << 4U;
        }
        if (!is_solid(up)) {
            boundary_mask |= 1U << 5U;
        }

        for (std::uint32_t face = 1U; face < native_preview_face_corners.size(); ++face) {
            if ((boundary_mask & (1U << face)) == 0U) {
                continue;
            }
            add_quad(
                buffer,
                vertex_indices,
                native_preview_cube_face_vertices(cell, face, buffer.unit_size),
                native_preview_face_uvs,
                material_for_cell_slot(cell, face));
        }
    }

    sort_triangles_into_native_material_runs(buffer);
    return buffer;
}

BuildingRenderBuffer build_city_building_full_render_buffer(
    const SbbBuilding& building,
    const std::vector<NamedBlockMesh>& block_meshes,
    const std::vector<NamedBuildblockTemplate>& buildblock_templates,
    BuildingLod lod) {
    // City placement uses the game's building buffer, not the editor's floor-tool
    // preview. The preview treats placed_flag bits as optional floor and ceiling
    // guides; using it in the city dropped the visible top face (face 5) on solid top
    // cells, which made roofs or top floors disappear.
    return select_building_lod(build_native_building_render_buffer(building, block_meshes, buildblock_templates), lod);
}

BuildingRenderBuffer select_building_lod(BuildingRenderBuffer buffer, BuildingLod lod) {
    if (lod == BuildingLod::Both) return buffer;
    const bool near = lod == BuildingLod::Near;
    std::erase_if(buffer.triangles, [near](const RenderBufferTriangle& tri) { return near ? tri.far_only : tri.interior; });
    // Keep only the vertices the remaining triangles use.
    std::vector<std::uint32_t> remap(buffer.vertices.size(), 0xffffffffU);
    std::vector<RenderBufferVertex> vertices;
    for (auto& tri : buffer.triangles) {
        for (auto& index : tri.indices) {
            if (remap[index] == 0xffffffffU) {
                remap[index] = static_cast<std::uint32_t>(vertices.size());
                vertices.push_back(buffer.vertices[index]);
            }
            index = remap[index];
        }
    }
    vertices.shrink_to_fit();
    buffer.vertices = std::move(vertices);
    buffer.triangles.shrink_to_fit();
    sort_triangles_into_native_material_runs(buffer);
    return buffer;
}

BuildingRenderBuffer build_native_portal_render_buffer(
    const SbcMap& map,
    const std::vector<NamedBlockMesh>& block_meshes,
    std::optional<std::size_t> selected_portal,
    [[maybe_unused]] const std::vector<NamedBuildblockTemplate>& buildblock_templates,
    [[maybe_unused]] const std::vector<std::string>& texture_names,
    const std::vector<const SbbBuilding*>& placement_buildings) {
    PAFTA_PROFILE_FUNCTION();
    BuildingRenderBuffer buffer;
    VertexIndexMap vertex_indices;

    auto layer_slot_material = [&](const SbcCityBlock& city_block, std::size_t layer_index, std::uint32_t slot) {
        return material_id_for_name(buffer, city_block_layer_slot_name(city_block, layer_index, slot));
    };

    // Solid district cells, for the neighbour face test below.
    std::unordered_set<std::uint64_t> solid_cell_coords;
    const auto solid_key = [](std::int32_t x, std::int32_t y, std::int32_t z) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 42) |
               (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y) & 0x1fffffU) << 21) |
               static_cast<std::uint64_t>(static_cast<std::uint32_t>(z) & 0x1fffffU);
    };
    // World_LoadMapAndInitializeTerrain places v10 district cells before the
    // buildings, and InstantiateSbbVolumeIntoWorldAndMetadata then writes every
    // building cell that has an exterior block word over the world cell. Those
    // district cells are gone, and the building's solid cells count as neighbours in
    // the face test. (Newer city blocks are placed after the buildings.)
    std::unordered_set<CityTileKey, CityTileKeyHash> building_cells;
    for (std::size_t index = 0; map.city_blocks.empty() && index < map.buildings.size() && index < placement_buildings.size(); ++index) {
        const auto* building = placement_buildings[index];
        if (building == nullptr) continue;
        const auto& placement = map.buildings[index];
        for (const auto& cell : building->cells) {
            if (cell.exterior_block() == 0U) continue;
            const Vec3 local{
                (static_cast<float>(cell.x) - static_cast<float>(building->anchor_x) + 0.5F) * 4.0F,
                0.0F,
                (static_cast<float>(cell.z) - static_cast<float>(building->anchor_z) + 0.5F) * 4.0F,
            };
            const auto world = native_city_building_transform_point(placement, local, 4.0F);
            const CityTileKey key{static_cast<std::int32_t>(std::floor(world.x / 4.0F)),
                placement.y + static_cast<std::int32_t>(cell.y) - static_cast<std::int32_t>(building->anchor_y),
                static_cast<std::int32_t>(std::floor(world.z / 4.0F))};
            building_cells.insert(key);
            if ((cell.exterior_block() & 0xe0000000U) == 0x40000000U) solid_cell_coords.insert(solid_key(key.x, key.y, key.z));
        }
    }
    const auto replaced_by_building = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        return building_cells.contains({x, y, z});
    };
    for (const auto& portal : map.portals) {
        const auto& portal_voxels = portal.voxel_blocks();
        const auto portal_count = std::min<std::size_t>(portal_voxels.size(), 0x200);
        for (std::size_t vi = 0; vi < portal_count; ++vi) {
            const auto cell_word = portal_voxels[vi];
            if (cell_word == 0U) {
                continue;
            }
            const auto sx = sbc_portal_cell_base(portal.fields[1]) + static_cast<std::int32_t>(vi & 7U);
            const auto sz = sbc_portal_cell_base(portal.fields[3]) + static_cast<std::int32_t>((vi >> 3U) & 7U);
            const auto sy = sbc_portal_cell_base(portal.fields[2]) + static_cast<std::int32_t>((vi >> 6U) & 7U);
            if ((cell_word & 0xe0000000U) == 0x40000000U && !replaced_by_building(sx, sy, sz)) {
                solid_cell_coords.insert(solid_key(sx, sy, sz));
            }
        }
    }

    for (std::size_t portal_index = 0; portal_index < map.portals.size(); ++portal_index) {
        if (selected_portal.has_value() && portal_index != *selected_portal) {
            continue;
        }
        const auto& portal = map.portals[portal_index];
        const auto& voxels = portal.voxel_blocks();
        const auto block_count = std::min<std::size_t>(voxels.size(), 0x200);
        for (std::size_t voxel_index = 0; voxel_index < block_count; ++voxel_index) {
            const auto word = voxels[voxel_index];
            if (word == 0U) {
                continue;
            }
            const RenderCellTag tag{buffer, buffer.triangles.size(),
                static_cast<std::uint32_t>(portal_index * 512U + voxel_index)};
            const auto city_block_index = static_cast<std::size_t>(word & 0xfffU);

            auto layer_index = static_cast<std::size_t>((word >> 16U) & 0x3fU);
            layer_index = layer_index == 0U ? 0U : layer_index - 1U;
            const auto rotation = static_cast<std::int32_t>((word >> 24U) & 3U);
            const auto local_x = static_cast<std::int32_t>(voxel_index & 7U);
            const auto local_z = static_cast<std::int32_t>((voxel_index >> 3U) & 7U);
            const auto local_y = static_cast<std::int32_t>((voxel_index >> 6U) & 7U);
            const auto origin = Vec3{
                static_cast<float>(sbc_portal_cell_base(portal.fields[1]) + local_x) * buffer.unit_size,
                static_cast<float>(sbc_portal_cell_base(portal.fields[2]) + local_y) * buffer.unit_size,
                static_cast<float>(sbc_portal_cell_base(portal.fields[3]) + local_z) * buffer.unit_size,
            };

            // World_LoadMapAndInitializeTerrain's old loop passes these words to
            // World_SetTileBlockAndResolveNeighborLinks: the 0x40000000 kind is a
            // solid cell, and 0x80000000 picks from the built-in catalog by its low
            // 16 bits. The per-face texture grid is only read as zero or not zero,
            // picking concrete or asphalt, not a loaded texture by number.
            if (map.city_blocks.empty()) {
                if (replaced_by_building(sbc_portal_cell_base(portal.fields[1]) + local_x, sbc_portal_cell_base(portal.fields[2]) + local_y,
                        sbc_portal_cell_base(portal.fields[3]) + local_z)) {
                    continue;
                }
                const auto cell_class = word & 0xe0000000U;
                const auto grid_material = [&](std::size_t slot) {
                    const auto grid_base = voxel_index * 8U + std::min<std::size_t>(slot, 7U);
                    const auto tex_idx = grid_base < portal.textures.size()
                        ? static_cast<std::size_t>(portal.textures[grid_base])
                        : 0U;
                    // World_LoadMapAndInitializeTerrain's old district loop gives
                    // World_SetTileBlockAndResolveNeighborLinks each face as (grid != 0),
                    // so these cells only ever use texture 0 ("concrete") or texture 1,
                    // the slot asphalt2.png loads into. The stored number itself is never
                    // used.
                    return material_id_for_name(buffer, tex_idx == 0U ? "city_curb_concrete" : "city_road_asphalt");
                };

                if (cell_class == 0x40000000U) {
                    // World_SetTileBlockAndResolveNeighborLinks places every district cell
                    // as solid, and BuildCellRenderGeometryAndAdjacency then only skips
                    // the faces shared with a solid neighbour.
                    const auto cell_x = sbc_portal_cell_base(portal.fields[1]) + local_x;
                    const auto cell_y = sbc_portal_cell_base(portal.fields[2]) + local_y;
                    const auto cell_z = sbc_portal_cell_base(portal.fields[3]) + local_z;
                    const auto solid = [&](std::int32_t dx, std::int32_t dy, std::int32_t dz) {
                        return solid_cell_coords.contains(solid_key(cell_x + dx, cell_y + dy, cell_z + dz));
                    };
                    add_native_city_block_solid_cell(
                        buffer,
                        vertex_indices,
                        origin,
                        [&](BuildingFaceSide side) {
                            return grid_material(material_slot_for_cell_side(side));
                        },
                        {solid(0, 0, 1), solid(-1, 0, 0), solid(0, 0, -1), solid(1, 0, 0), solid(0, 1, 0)});
                    continue;
                }

                if (cell_class == 0x80000000U) {
                    const auto catalog_index = static_cast<std::uint32_t>(word & 0xffffU);
                    const auto* mesh = resolve_district_procedural_mesh(catalog_index);
                    if (mesh == nullptr) {
                        continue;
                    }
                    // BuildCellRenderGeometryAndAdjacency @0xc4c0 copies the catalog mesh's
                    // vertices and only moves them. It does NOT rotate the piece by the
                    // cell's (word>>24)&3 rotation. That field only turns the UV and
                    // material slot of each quad when drawing, plus the four pre-rotated
                    // catalog ids the map can point to directly. So draw the catalog mesh as
                    // it is (rotation 0); the material slot still comes from the cell's
                    // 8-short texture grid.
                    add_rotated_block_mesh_to_buffer(
                        buffer,
                        vertex_indices,
                        *mesh,
                        origin,
                        0,
                        [&](BuildingRenderBuffer&, const SblBlockFace&, const NativeRotatedBlockFace& rotated_face) {
                            return grid_material(rotated_face.material);
                        });
                }
                continue;
            }

            if (city_block_index >= map.city_blocks.size()) {
                continue;
            }
            const auto& city_block = map.city_blocks[city_block_index];
            const auto resolution = resolve_city_block_render(city_block, block_meshes);
            if (resolution.render_class == CityBlockRenderClass::Mesh && resolution.mesh != nullptr) {
                add_rotated_block_mesh_to_buffer(
                    buffer,
                    vertex_indices,
                    *resolution.mesh,
                    origin,
                    rotation,
                    [&](BuildingRenderBuffer&, const SblBlockFace&, const NativeRotatedBlockFace& rotated_face) {
                        const auto material_slot = std::min<std::uint32_t>(rotated_face.material, 7U);
                        return layer_slot_material(city_block, layer_index, material_slot);
                });
                continue;
            }
            add_native_city_block_solid_cell(
                buffer,
                vertex_indices,
                origin,
                [&](BuildingFaceSide side) {
                    return layer_slot_material(
                        city_block,
                        layer_index,
                        static_cast<std::uint32_t>(material_slot_for_cell_side(side)));
                });
        }
    }

    sort_triangles_into_native_material_runs(buffer);
    return buffer;
}

std::optional<CellPick> pick_building_cell(
    const SbbBuilding& building,
    const Vec3& ray_origin,
    const Vec3& ray_dir,
    float unit_size,
    std::optional<std::uint32_t> floor) {
    if (unit_size <= 0.0F) {
        return std::nullopt;
    }

    const auto half = unit_size * 0.5F;
    std::optional<CellPick> nearest;
    for (std::size_t cell_index = 0; cell_index < building.cells.size(); ++cell_index) {
        const auto& cell = building.cells[cell_index];
        if (!cell.occupied || (floor.has_value() && cell.y != *floor)) {
            continue;
        }

        const auto center = building_cell_center_world(building, cell, unit_size);
        const auto min = Vec3{center.x - half, center.y - half, center.z - half};
        const auto max = Vec3{center.x + half, center.y + half, center.z + half};
        const auto t = ray_aabb_intersection_t(ray_origin, ray_dir, min, max);
        if (!t.has_value()) {
            continue;
        }
        if (nearest.has_value() && *t >= nearest->t) {
            continue;
        }

        nearest = CellPick{
            cell_index,
            add_scaled(ray_origin, ray_dir, *t),
            *t,
        };
    }

    return nearest;
}

std::optional<Vec3> ray_ground_hit(const Vec3& ray_origin, const Vec3& ray_dir) {
    if (std::abs(ray_dir.y) < 0.0001F) {
        return std::nullopt;
    }
    const auto t = -ray_origin.y / ray_dir.y;
    if (t < 0.0F) {
        return std::nullopt;
    }
    return add_scaled(ray_origin, ray_dir, t);
}

std::int32_t terrain_fixed_step(const CityTerrain& terrain, float chunk_x, float chunk_z) {
    // The game only streams the sub-chunks the camera can see, at camera-distance
    // detail. This mesh stays loaded, so the game's near-detail distances would add up
    // to about five million uploaded vertices. Keep the whole 0x4000 world, with high
    // detail only near the city; update_terrain_detail adds the camera's detail on top.
    const auto dx = chunk_x + 64.0F - (terrain.min_x + terrain.max_x) * 0.5F;
    const auto dz = chunk_z + 64.0F - (terrain.min_z + terrain.max_z) * 0.5F;
    const auto distance = std::sqrt(dx * dx + dz * dz);
    if (distance < 256.0F) return 4;
    if (distance < 512.0F) return 8;
    if (distance < 1024.0F) return 16;
    if (distance < 2048.0F) return 32;
    return 64;
}

BuildingRenderBuffer build_terrain_chunk_mesh(const CityTerrain& terrain, float chunk_x, float chunk_z, std::int32_t step, float unit_size) {
    BuildingRenderBuffer mesh;
    mesh.unit_size = unit_size;
    mesh.material_names.push_back("city_terrain_grass");
    const auto height = [&](float x, float z) {
        return x >= terrain.min_x && x <= terrain.max_x && z >= terrain.min_z && z <= terrain.max_z
            ? terrain_height_at(terrain, x, z) : native_like_city_terrain_height(x, z, terrain.city_map);
    };
    const auto covered = [&](std::int32_t gx, std::int32_t gz) {
        return terrain.covered_cells.contains(CityCornerStampMap::key(gx, gz));
    };
    append_terrain_chunk(mesh, chunk_x, chunk_z, std::max(1, step), unit_size, height, covered);
    sort_triangles_into_native_material_runs(mesh);
    build_terrain_draw_chunks(mesh);
    return mesh;
}

std::optional<Vec3> ray_terrain_hit(const CityTerrain& terrain, const Vec3& ray_origin, const Vec3& ray_dir) {
    if (terrain.heights.empty() || terrain.sample_count_x <= 0 || terrain.sample_count_z <= 0 ||
        terrain.sample_spacing <= 0.0F) {
        return ray_ground_hit(ray_origin, ray_dir);
    }

    // Move along the ray in fixed steps to find the stretch where it goes from above
    // the heightfield to below it, then halve that stretch until it is small.
    // signed_gap(t) > 0 means the ray is above the ground at that x/z. The terrain is a
    // height grid, so a ray looking down crosses it at most once in the stretch that
    // matters. A rough march plus a short bisection lands within a few millimetres
    // without testing every triangle.
    const auto signed_gap = [&](float t) {
        const auto p = add_scaled(ray_origin, ray_dir, t);
        return p.y - terrain_height_at(terrain, p.x, p.z);
    };

    const auto span = (terrain.max_x - terrain.min_x) + (terrain.max_z - terrain.min_z);
    const auto max_t = span > 0.0F ? span * 2.0F + 4096.0F : 8192.0F;
    const auto step = std::max(terrain.sample_spacing * 0.5F, 1.0F);

    float prev_t = 0.0F;
    float prev_gap = signed_gap(prev_t);
    // The ray starts under the ground, so there is nothing sensible to snap to.
    if (prev_gap < 0.0F) {
        return ray_ground_hit(ray_origin, ray_dir);
    }
    for (float t = step; t <= max_t; t += step) {
        const auto gap = signed_gap(t);
        if (gap <= 0.0F) {
            float lo = prev_t;
            float hi = t;
            for (int i = 0; i < 24; ++i) {
                const auto mid = (lo + hi) * 0.5F;
                if (signed_gap(mid) > 0.0F) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            return add_scaled(ray_origin, ray_dir, (lo + hi) * 0.5F);
        }
        prev_t = t;
        prev_gap = gap;
    }
    (void)prev_gap;
    return ray_ground_hit(ray_origin, ray_dir);
}

std::optional<std::size_t> pick_city_building(
    const SbcMap& map,
    const std::vector<const SbbBuilding*>& placement_buildings,
    const Vec3& ray_origin,
    const Vec3& ray_dir,
    float unit_size) {
    if (unit_size <= 0.0F) {
        return std::nullopt;
    }

    const auto half = unit_size * 0.5F;
    std::optional<std::size_t> nearest;
    float nearest_t = std::numeric_limits<float>::max();
    for (std::size_t index = 0; index < map.buildings.size(); ++index) {
        const auto& placement = map.buildings[index];
        const auto* building = index < placement_buildings.size() ? placement_buildings[index] : nullptr;
        auto test_cell_box = [&](Vec3 center) {
            const auto box_min = Vec3{center.x - half, center.y - half, center.z - half};
            const auto box_max = Vec3{center.x + half, center.y + half, center.z + half};
            const auto t = ray_aabb_intersection_t(ray_origin, ray_dir, box_min, box_max);
            if (!t.has_value() || *t >= nearest_t) {
                return;
            }
            nearest = index;
            nearest_t = *t;
        };

        bool tested_loaded_cell = false;
        if (building != nullptr) {
            for (const auto& cell : building->cells) {
                if (!cell.occupied) {
                    continue;
                }
                const Vec3 local{
                    (static_cast<float>(cell.x) - static_cast<float>(building->anchor_x) + 0.5F) * unit_size,
                    (static_cast<float>(cell.y) - static_cast<float>(building->anchor_y) + 0.5F) * unit_size,
                    (static_cast<float>(cell.z) - static_cast<float>(building->anchor_z) + 0.5F) * unit_size,
                };
                test_cell_box(native_city_building_transform_point(placement, local, unit_size));
                tested_loaded_cell = true;
            }
        }
        if (!tested_loaded_cell) {
            const auto origin = native_city_building_render_origin(placement, unit_size);
            test_cell_box({origin.x, origin.y + half, origin.z});
        }
    }

    return nearest;
}

std::optional<CityPiecePick> pick_city_piece(
    const SbcMap& map,
    const Vec3& ray_origin,
    const Vec3& ray_dir,
    float unit_size) {
    if (unit_size <= 0.0F) {
        return std::nullopt;
    }

    const auto half = unit_size * 0.5F;
    std::optional<CityPiecePick> nearest;
    float nearest_t = std::numeric_limits<float>::max();
    for (std::size_t portal_index = 0; portal_index < map.portals.size(); ++portal_index) {
        const auto& portal = map.portals[portal_index];
        const auto& voxels = portal.voxel_blocks();
        for (std::size_t block_index = 0; block_index < voxels.size() && block_index < 0x200U; ++block_index) {
            const auto word = voxels[block_index];
            if (word == 0U) {
                continue;
            }

            const auto local_x = static_cast<std::int32_t>(block_index & 7U);
            const auto local_z = static_cast<std::int32_t>((block_index / 8U) & 7U);
            const auto local_y = static_cast<std::int32_t>((block_index / 0x40U) & 7U);
            const Vec3 center{
                (static_cast<float>(sbc_portal_cell_base(portal.fields[1]) + local_x) + 0.5F) * unit_size,
                (static_cast<float>(sbc_portal_cell_base(portal.fields[2]) + local_y) + 0.5F) * unit_size,
                (static_cast<float>(sbc_portal_cell_base(portal.fields[3]) + local_z) + 0.5F) * unit_size,
            };
            const auto box_min = Vec3{center.x - half, center.y - half, center.z - half};
            const auto box_max = Vec3{center.x + half, center.y + half, center.z + half};
            const auto t = ray_aabb_intersection_t(ray_origin, ray_dir, box_min, box_max);
            if (!t.has_value() || *t >= nearest_t) {
                continue;
            }

            nearest = CityPiecePick{portal_index, block_index, word};
            nearest_t = *t;
        }
    }
    return nearest;
}

namespace {

// The junction whose drawn crossing tiles the ray reaches first, and how far along the
// ray. A junction is picked by the road square you see, not a marker inside it.
std::optional<std::pair<std::size_t, float>> pick_junction_tile(
    const SbcMap& map, const MapScene& scene, const Vec3& ray_origin, const Vec3& ray_dir) {
    // A cached scene can keep drawing after an edit, but must not select a junction
    // that moved.
    std::vector<bool> current(map.intersections.size(), false);
    for (const auto& intersection : scene.intersections) {
        if (intersection.source_index < map.intersections.size() && intersection.source == map.intersections[intersection.source_index]) {
            current[intersection.source_index] = true;
        }
    }
    std::optional<std::pair<std::size_t, float>> nearest;
    const auto unit = scene.unit_size;
    for (const auto& tile : scene.city_tiles) {
        if (tile.intersection_index < 0 || tile.street_index >= 0 ||
            static_cast<std::size_t>(tile.intersection_index) >= current.size() || !current[static_cast<std::size_t>(tile.intersection_index)]) {
            continue;
        }
        const Vec3 box_min{static_cast<float>(tile.x) * unit, tile.base_y, static_cast<float>(tile.z) * unit};
        const Vec3 box_max{box_min.x + unit, tile.base_y + unit, box_min.z + unit};
        const auto t = ray_aabb_intersection_t(ray_origin, ray_dir, box_min, box_max);
        if (t.has_value() && (!nearest || *t < nearest->second)) nearest = std::pair{static_cast<std::size_t>(tile.intersection_index), *t};
    }
    // A road's end has no crossing tiles (the road runs over its junction), so each
    // junction also has a pad as wide as its widest road, at the road's surface.
    std::vector<std::int32_t> half_width(map.intersections.size(), 0);
    for (const auto& street : map.streets) {
        for (const auto end : {street.from_intersection, street.to_intersection}) {
            if (end >= 0 && static_cast<std::size_t>(end) < half_width.size()) {
                half_width[static_cast<std::size_t>(end)] = std::max({half_width[static_cast<std::size_t>(end)], street.left_width, street.right_width});
            }
        }
    }
    for (std::size_t i = 0; i < map.intersections.size(); ++i) {
        if (!current[i] || half_width[i] == 0) continue;
        const auto& node = map.intersections[i];
        const auto w = static_cast<float>(half_width[i]) * unit;
        const Vec3 box_min{static_cast<float>(node.x) * unit - w, static_cast<float>(node.y - 1) * unit, static_cast<float>(node.z) * unit - w};
        const Vec3 box_max{static_cast<float>(node.x) * unit + w, static_cast<float>(node.y) * unit + 0.5F, static_cast<float>(node.z) * unit + w};
        const auto t = ray_aabb_intersection_t(ray_origin, ray_dir, box_min, box_max);
        if (t.has_value() && (!nearest || *t < nearest->second)) nearest = std::pair{i, *t};
    }
    return nearest;
}

} // namespace

std::optional<std::size_t> pick_city_intersection(
    const SbcMap& map,
    const MapScene& scene,
    const Vec3& ray_origin,
    const Vec3& ray_dir) {
    const auto hit = pick_junction_tile(map, scene, ray_origin, ray_dir);
    return hit ? std::optional<std::size_t>{hit->first} : std::nullopt;
}

std::optional<CityRoadPick> pick_city_road_or_node(
    const SbcMap& map,
    const MapScene& scene,
    const Vec3& ray_origin,
    const Vec3& ray_dir) {
    const auto node = pick_junction_tile(map, scene, ray_origin, ray_dir);

    // Check each street once before going through its tiles. A cached scene can keep
    // drawing after an edit, but must not select a record that moved or changed.
    std::vector<bool> current_roads(map.streets.size(), false);
    for (const auto& road : scene.roads) {
        if (road.source_index < map.streets.size() && road.source == map.streets[road.source_index]) {
            current_roads[road.source_index] = true;
        }
    }
    // The closest drawn road tile gives the street it belongs to.
    std::optional<std::size_t> road_index;
    float road_t = std::numeric_limits<float>::max();
    const auto unit = scene.unit_size;
    for (const auto& tile : scene.city_tiles) {
        if (tile.kind != CityTileKind::Road || tile.street_index < 0 ||
            static_cast<std::size_t>(tile.street_index) >= current_roads.size() ||
            !current_roads[static_cast<std::size_t>(tile.street_index)]) {
            continue;
        }
        const Vec3 box_min{static_cast<float>(tile.x) * unit, tile.base_y, static_cast<float>(tile.z) * unit};
        const Vec3 box_max{box_min.x + unit, tile.base_y + unit, box_min.z + unit};
        const auto t = ray_aabb_intersection_t(ray_origin, ray_dir, box_min, box_max);
        if (t.has_value() && *t < road_t) {
            road_t = *t;
            road_index = static_cast<std::size_t>(tile.street_index);
        }
    }

    // Whichever tile the ray reaches first wins.
    if (node.has_value() && (!road_index.has_value() || node->second <= road_t)) {
        return CityRoadPick{CityRoadPick::Kind::Node, node->first};
    }
    if (road_index.has_value()) {
        return CityRoadPick{CityRoadPick::Kind::Road, *road_index};
    }
    return std::nullopt;
}

// -----------------------------------------------------------------------------
// City scene builders
// -----------------------------------------------------------------------------

CityBuildingBasis native_city_building_rotation_basis(std::int32_t rotation) {
    const auto effective_rotation = (4 - (rotation & 3)) & 3;
    switch (effective_rotation) {
        case 1:
            return {
                {0.0F, 0.0F, -1.0F},
                {0.0F, 1.0F, 0.0F},
                {1.0F, 0.0F, 0.0F},
            };
        case 2:
            return {
                {-1.0F, 0.0F, 0.0F},
                {0.0F, 1.0F, 0.0F},
                {0.0F, 0.0F, -1.0F},
            };
        case 3:
            return {
                {0.0F, 0.0F, 1.0F},
                {0.0F, 1.0F, 0.0F},
                {-1.0F, 0.0F, 0.0F},
            };
        default:
            return {};
    }
}

Vec3 native_city_rotate_local(Vec3 local, std::int32_t rotation) {
    const auto basis = native_city_building_rotation_basis(rotation);
    return {
        local.x * basis.x_axis.x + local.y * basis.y_axis.x + local.z * basis.z_axis.x,
        local.x * basis.x_axis.y + local.y * basis.y_axis.y + local.z * basis.z_axis.y,
        local.x * basis.x_axis.z + local.y * basis.y_axis.z + local.z * basis.z_axis.z,
    };
}

namespace {

Vec3 native_city_editor_offset_cell(std::int32_t rotation) {
    auto x = 0;
    auto z = 0;
    // StreetPieceTool_CanvasRender_14006c400 only applies this rotation fix before
    // RenderBuildingTemplatePreview. LoadCity_SBC stores a centre point of the extents
    // for bounds and placement, but the drawn template mesh is already centred by
    // BuildBuildingBlockRenderBuffers.
    switch (rotation & 3) {
        case 1:
            x = 1;
            break;
        case 2:
            x = 1;
            z = 1;
            break;
        case 3:
            z = 1;
            break;
        default:
            break;
    }
    return {static_cast<float>(x), 0.0F, static_cast<float>(z)};
}

} // namespace

Vec3 native_city_building_render_origin(const SbcBuilding& placement, float unit_size) {
    const auto offset = native_city_editor_offset_cell(placement.rotation);
    return {
        (static_cast<float>(placement.x) + offset.x) * unit_size,
        // Every world cell in the game sits 0.25 above its level (a cell at level L
        // runs from L*4+0.25 to L*4+4.25). Without that lift, building bottoms sit in
        // the same plane as their stamped ground and flicker against it.
        static_cast<float>(placement.y) * unit_size + 0.25F,
        (static_cast<float>(placement.z) + offset.z) * unit_size,
    };
}

Vec3 native_city_building_transform_point(const SbcBuilding& placement, Vec3 local, float unit_size) {
    const auto origin = native_city_building_render_origin(placement, unit_size);
    const auto rotated = native_city_rotate_local(local, placement.rotation);
    return {
        origin.x + rotated.x,
        origin.y + rotated.y,
        origin.z + rotated.z,
    };
}

std::optional<BuildingFacePick> pick_building_face(const BuildingRenderBuffer& buffer, Vec3 origin, Vec3 direction) {
    std::optional<BuildingFacePick> best;
    for (const auto& tri : buffer.triangles) {
        if (tri.cell == 0xffffffffU) continue;
        const auto& a = buffer.vertices[tri.indices[0]].position;
        const auto& b = buffer.vertices[tri.indices[1]].position;
        const auto& c = buffer.vertices[tri.indices[2]].position;
        // Moller-Trumbore, either side.
        const Vec3 e1{b.x - a.x, b.y - a.y, b.z - a.z};
        const Vec3 e2{c.x - a.x, c.y - a.y, c.z - a.z};
        const Vec3 p{direction.y * e2.z - direction.z * e2.y, direction.z * e2.x - direction.x * e2.z, direction.x * e2.y - direction.y * e2.x};
        const auto det = e1.x * p.x + e1.y * p.y + e1.z * p.z;
        if (std::abs(det) < 1e-9F) continue;
        const auto inv = 1.0F / det;
        const Vec3 s{origin.x - a.x, origin.y - a.y, origin.z - a.z};
        const auto u = (s.x * p.x + s.y * p.y + s.z * p.z) * inv;
        if (u < 0.0F || u > 1.0F) continue;
        const Vec3 q{s.y * e1.z - s.z * e1.y, s.z * e1.x - s.x * e1.z, s.x * e1.y - s.y * e1.x};
        const auto v = (direction.x * q.x + direction.y * q.y + direction.z * q.z) * inv;
        if (v < 0.0F || u + v > 1.0F) continue;
        const auto t = (e2.x * q.x + e2.y * q.y + e2.z * q.z) * inv;
        if (t <= 0.0F || (best && t >= best->t)) continue;
        best = BuildingFacePick{tri.cell, tri.slot, {origin.x + direction.x * t, origin.y + direction.y * t, origin.z + direction.z * t}, t};
    }
    return best;
}

std::uint64_t city_scene_fingerprint(const SbcMap& map, std::uint64_t building_revision) {
    // A hash of everything the city scene is built from: intersections (terrain, tile
    // grid, poles), streets (road tiles), and building placements. The revision covers
    // edited SBB assets that terrain stamps and world storage use.
    std::uint64_t hash = 1469598103934665603ULL;
    // An FNV variant that works a word at a time. This runs every frame over every
    // piece voxel and only needs to notice changes, so it doesn't have to be byte-wise
    // FNV-1a.
    const auto mix = [&hash](std::int64_t value) {
        hash ^= static_cast<std::uint64_t>(value);
        hash *= 1099511628211ULL;
    };
    mix(static_cast<std::int64_t>(building_revision));
    mix(static_cast<std::int64_t>(map.intersections.size()));
    for (const auto& it : map.intersections) {
        mix(it.x);
        mix(it.y);
        mix(it.z);
    }
    mix(static_cast<std::int64_t>(map.streets.size()));
    for (const auto& street : map.streets) {
        mix(street.from_intersection);
        mix(street.to_intersection);
        mix(street.left_width);
        mix(street.right_width);
    }
    mix(static_cast<std::int64_t>(map.buildings.size()));
    for (const auto& building : map.buildings) {
        mix(static_cast<std::int64_t>(building.name.size()));
        for (const unsigned char c : building.name) mix(c);
        mix(building.x);
        mix(building.y);
        mix(building.z);
        mix(building.rotation);
    }
    // Piece (portal or district) cells stamp the terrain's corner-height grid, so
    // piece edits have to rebuild the scene too.
    mix(static_cast<std::int64_t>(map.portals.size()));
    for (const auto& portal : map.portals) {
        mix(portal.fields[1]);
        mix(portal.fields[2]);
        mix(portal.fields[3]);
        // Four separate lanes, so the multiplies don't wait on each other;
        // every word still goes into one lane.
        const auto& words = portal.voxel_blocks();
        std::array<std::uint64_t, 4> lanes{hash, hash + 1U, hash + 2U, hash + 3U};
        std::size_t i = 0;
        for (; i + 4U <= words.size(); i += 4U) {
            for (std::size_t lane = 0; lane < 4U; ++lane) {
                lanes[lane] = (lanes[lane] ^ words[i + lane]) * 1099511628211ULL;
            }
        }
        for (; i < words.size(); ++i) lanes[0] = (lanes[0] ^ words[i]) * 1099511628211ULL;
        for (const auto lane : lanes) mix(static_cast<std::int64_t>(lane));
    }
    return hash;
}

std::uint64_t city_terrain_fingerprint(const SbcMap& map, std::uint64_t building_revision) {
    // Part of city_scene_fingerprint, covering only what changes the terrain mesh:
    // street geometry and intersection positions (corner heights and road masks),
    // building footprints, and piece cells. A street's name does not affect it.
    std::uint64_t hash = 1469598103934665603ULL;
    // An FNV variant that works a word at a time. This runs every frame over every
    // piece voxel and only needs to notice changes, so it doesn't have to be byte-wise
    // FNV-1a.
    const auto mix = [&hash](std::int64_t value) {
        hash ^= static_cast<std::uint64_t>(value);
        hash *= 1099511628211ULL;
    };
    mix(static_cast<std::int64_t>(building_revision));
    mix(static_cast<std::int64_t>(map.intersections.size()));
    for (const auto& it : map.intersections) {
        mix(it.x);
        mix(it.y);
        mix(it.z);
    }
    mix(static_cast<std::int64_t>(map.streets.size()));
    for (const auto& street : map.streets) {
        mix(street.from_intersection);
        mix(street.to_intersection);
        mix(street.left_width);
        mix(street.right_width);
    }
    mix(static_cast<std::int64_t>(map.buildings.size()));
    for (const auto& building : map.buildings) {
        mix(static_cast<std::int64_t>(building.name.size()));
        for (const unsigned char c : building.name) mix(c);
        mix(building.x);
        mix(building.y);
        mix(building.z);
        mix(building.rotation);
    }
    mix(static_cast<std::int64_t>(map.portals.size()));
    for (const auto& portal : map.portals) {
        mix(portal.fields[1]);
        mix(portal.fields[2]);
        mix(portal.fields[3]);
        for (const auto word : portal.voxel_blocks()) {
            mix(static_cast<std::int64_t>(word));
        }
    }
    return hash;
}

MapScene build_map_scene(const SbcMap& map) {
    return build_map_scene(map, {}, false, false);
}

StreetPreview preview_city_street(const SbcMap& map, std::size_t street_index, bool auto_generated) {
    StreetPreview preview;
    const auto graph = build_native_city_graph(map);
    const auto street = std::ranges::find(graph.streets, static_cast<std::int32_t>(street_index), &NativeCityStreet::source_index);
    if (street == graph.streets.end() || street->a < 0 || street->b < 0) return preview;
    preview.run = native_street_run(graph, *street, auto_generated);
    CityTileMap tile_map;
    CityCornerStampMap stamps;
    stamps.disabled = true;
    place_native_street_tiles(tile_map, stamps, graph, *street, auto_generated);
    for (const auto index : {street->a, street->b}) {
        place_native_intersection_tiles(tile_map, graph.intersections[static_cast<std::size_t>(index)], index);
    }
    for (const auto& [key, value] : tile_map) {
        preview.cells.push_back({key.x, key.y, key.z, value.base_y, value.slope_x, value.slope_z, value.kind, value.street_index,
            value.block, value.faces, value.intersection_index});
    }
    return preview;
}

std::array<std::int32_t, 8> city_street_signature(const SbcMap& map, const SbcStreet& street) {
    if (street.from_intersection < 0 || street.to_intersection < 0 ||
        static_cast<std::size_t>(std::max(street.from_intersection, street.to_intersection)) >= map.intersections.size()) {
        return {};
    }
    const auto& a = map.intersections[static_cast<std::size_t>(street.from_intersection)];
    const auto& b = map.intersections[static_cast<std::size_t>(street.to_intersection)];
    return {a.x, a.y, a.z, b.x, b.y, b.z, street.left_width, street.right_width};
}

RoadEditTiles diff_city_road_tiles(const SbcMap& before, const SbcMap& after,
    const std::vector<const SbbBuilding*>& placement_buildings, bool auto_generated) {
    const auto old_graph = build_native_city_graph(before);
    const auto new_graph = build_native_city_graph(after);
    using NodeKey = std::array<std::int32_t, 3>;
    using NodeShape = std::array<std::int32_t, 8>;
    const auto node_shapes = [](const NativeCityGraph& graph) {
        std::map<NodeKey, NodeShape> shapes;
        for (const auto& n : graph.intersections) shapes[{n.gx, n.level, n.gz}] = {
            n.ext_x_pos, n.ext_x_neg, n.ext_z_pos, n.ext_z_neg,
            n.street_north >= 0, n.street_south >= 0, n.street_east >= 0, n.street_west >= 0};
        return shapes;
    };
    const auto old_nodes = node_shapes(old_graph), new_nodes = node_shapes(new_graph);
    std::set<NodeKey> changed_nodes;
    for (const auto& [key, shape] : old_nodes) {
        const auto n = new_nodes.find(key);
        if (n == new_nodes.end() || n->second != shape) changed_nodes.insert(key);
    }
    for (const auto& [key, shape] : new_nodes) {
        const auto n = old_nodes.find(key);
        if (n == old_nodes.end() || n->second != shape) changed_nodes.insert(key);
    }
    std::set<std::array<std::int32_t, 8>> old_ids, new_ids;
    for (const auto& road : before.streets) old_ids.insert(city_street_signature(before, road));
    for (const auto& road : after.streets) new_ids.insert(city_street_signature(after, road));
    using Area = std::array<std::int32_t, 4>;
    const auto street_area = [](const NativeCityGraph& graph, const NativeCityStreet& street) -> Area {
        const auto& a = graph.intersections[street.a];
        const auto& b = graph.intersections[street.b];
        return {std::min(a.gx - a.ext_x_neg - 1, b.gx - b.ext_x_neg - 1),
            std::min(a.gz - a.ext_z_neg - 1, b.gz - b.ext_z_neg - 1),
            std::max(a.gx + a.ext_x_pos + 1, b.gx + b.ext_x_pos + 1),
            std::max(a.gz + a.ext_z_pos + 1, b.gz + b.ext_z_pos + 1)};
    };
    const auto affected = [&](const SbcMap& map, const NativeCityGraph& graph, const NativeCityStreet& street, const auto& other_ids) {
        const auto& a = graph.intersections[street.a];
        const auto& b = graph.intersections[street.b];
        return !other_ids.contains(city_street_signature(map, map.streets[street.source_index])) ||
            changed_nodes.contains({a.gx, a.level, a.gz}) || changed_nodes.contains({b.gx, b.level, b.gz});
    };
    std::vector<Area> areas;
    const auto add_areas = [&](const SbcMap& map, const NativeCityGraph& graph, const auto& other_ids) {
        for (const auto& street : graph.streets) if (affected(map, graph, street, other_ids)) areas.push_back(street_area(graph, street));
        for (const auto& n : graph.intersections) if (changed_nodes.contains({n.gx, n.level, n.gz})) {
            areas.push_back({n.gx - n.ext_x_neg - 1, n.gz - n.ext_z_neg - 1, n.gx + n.ext_x_pos + 1, n.gz + n.ext_z_pos + 1});
        }
    };
    add_areas(before, old_graph, new_ids);
    add_areas(after, new_graph, old_ids);
    const auto nearby = [&](Area a) {
        return std::ranges::any_of(areas, [&](Area b) { return a[0] <= b[2] && b[0] <= a[2] && a[1] <= b[3] && b[1] <= a[3]; });
    };
    const auto tiles_of = [&](const SbcMap& map, const NativeCityGraph& graph, const auto& other_ids) {
        CityCornerStampMap stamps;
        stamps.disabled = true;
        CityTileMap tiles;
        std::set<std::int32_t> nodes;
        for (const auto& street : graph.streets) {
            // Unchanged crossing streets still matter for overlap warnings. Include
            // their local context using the same area set on both sides of the diff.
            if (!affected(map, graph, street, other_ids) && !nearby(street_area(graph, street))) continue;
            place_native_street_tiles(tiles, stamps, graph, street, auto_generated);
            nodes.insert(street.a); nodes.insert(street.b);
        }
        // Junctions also overwrite the ends of unchanged roads. Include their
        // local tiles without rebuilding the rest of the network.
        for (std::size_t i = 0; i < graph.intersections.size(); ++i) {
            const auto& n = graph.intersections[i];
            if (changed_nodes.contains({n.gx, n.level, n.gz})) nodes.insert(static_cast<std::int32_t>(i));
        }
        for (const auto i : nodes) place_native_intersection_tiles(tiles, graph.intersections[i], i);
        std::unordered_map<CityTileKey, CityTileCell, CityTileKeyHash> out;
        for (const auto& [key, value] : tiles) out.emplace(key, CityTileCell{key.x, key.y, key.z,
            value.base_y, value.slope_x, value.slope_z, value.kind, value.street_index, value.block, value.faces, value.intersection_index});
        return out;
    };
    const auto old_tiles = tiles_of(before, old_graph, new_ids);
    const auto new_tiles = tiles_of(after, new_graph, old_ids);
    // Roads the edit leaves alone, by their index after the edit.
    std::map<std::array<std::int32_t, 8>, std::int32_t> kept;
    for (std::size_t i = 0; i < after.streets.size(); ++i) kept.emplace(city_street_signature(after, after.streets[i]), static_cast<std::int32_t>(i));
    std::set<std::array<std::int32_t, 8>> old_signatures;
    for (const auto& street : before.streets) old_signatures.insert(city_street_signature(before, street));
    std::erase_if(kept, [&](const auto& entry) { return !old_signatures.contains(entry.first); });

    const auto same = [](const CityTileCell& a, const CityTileCell& b) {
        return a.kind == b.kind && a.block == b.block && a.faces == b.faces && a.base_y == b.base_y &&
            a.slope_x == b.slope_x && a.slope_z == b.slope_z;
    };
    RoadEditTiles diff;
    std::unordered_set<CityTileKey, CityTileKeyHash> shown;
    for (const auto& [key, tile] : new_tiles) {
        const auto old = old_tiles.find(key);
        if (old != old_tiles.end() && same(old->second, tile)) continue;
        diff.changed.push_back(tile);
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                for (int dy = -1; dy <= 1; ++dy) {
                    const CityTileKey near{key.x + dx, key.y + dy, key.z + dz};
                    if (new_tiles.contains(near) && shown.insert(near).second) diff.shown.push_back(new_tiles.at(near));
                }
            }
        }
        // Junction tiles take over the ends of the roads that meet there; that is how
        // the game joins roads, not an overlap.
        if (old == old_tiles.end() || old->second.street_index < 0 || tile.intersection_index >= 0 ||
            static_cast<std::size_t>(old->second.street_index) >= before.streets.size()) continue;
        // The tile belonged to a road that stays as it is, and now another road owns it.
        const auto owner = kept.find(city_street_signature(before, before.streets[static_cast<std::size_t>(old->second.street_index)]));
        if (owner != kept.end() && owner->second != tile.street_index) diff.overlaps.push_back(tile);
    }
    for (const auto& [key, tile] : old_tiles) {
        if (!new_tiles.contains(key)) diff.removed.push_back(tile);
    }
    for (const auto& [key, tile] : new_tiles) {
        const bool reshaped_road = tile.street_index >= 0 && static_cast<std::size_t>(tile.street_index) < after.streets.size() &&
            !old_ids.contains(city_street_signature(after, after.streets[static_cast<std::size_t>(tile.street_index)]));
        const bool reshaped_node = tile.intersection_index >= 0 && static_cast<std::size_t>(tile.intersection_index) < new_graph.intersections.size() &&
            changed_nodes.contains({new_graph.intersections[tile.intersection_index].gx, new_graph.intersections[tile.intersection_index].level,
                new_graph.intersections[tile.intersection_index].gz});
        if ((reshaped_road || reshaped_node) && shown.insert(key).second) diff.shown.push_back(tile);
    }
    std::unordered_map<CityTileKey, const SbcPortal*, CityTileKeyHash> portals;
    for (const auto& portal : before.portals) portals[{portal.fields[1], portal.fields[2], portal.fields[3]}] = &portal;
    const auto sector = [](std::int32_t n) { return static_cast<std::int32_t>((static_cast<std::int64_t>(n) - (n < 0 ? 7 : 0)) / 8); };
    for (const auto& tile : diff.changed) {
        bool replaced = false;
        if (const auto p = portals.find({sector(tile.x), sector(tile.y), sector(tile.z)}); p != portals.end()) {
            const auto& words = p->second->voxel_blocks();
            const auto i = static_cast<std::size_t>((tile.y - sector(tile.y) * 8) * 64 +
                (tile.z - sector(tile.z) * 8) * 8 + tile.x - sector(tile.x) * 8);
            replaced = i < words.size() && words[i] != 0;
        }
        for (std::size_t bi = 0; !replaced && bi < before.buildings.size() && bi < placement_buildings.size(); ++bi) {
            const auto* building = placement_buildings[bi];
            if (building == nullptr) continue;
            const auto& placement = before.buildings[bi];
            const auto origin = native_city_building_render_origin(placement, 4.0F);
            const auto local = native_city_rotate_local({tile.x * 4.0F + 2.0F - origin.x, 0.0F,
                tile.z * 4.0F + 2.0F - origin.z}, -placement.rotation);
            const auto x = static_cast<std::int32_t>(std::floor(local.x / 4.0F)) + static_cast<std::int32_t>(building->anchor_x);
            const auto z = static_cast<std::int32_t>(std::floor(local.z / 4.0F)) + static_cast<std::int32_t>(building->anchor_z);
            const auto y = tile.y - placement.y + static_cast<std::int32_t>(building->anchor_y);
            if (x < 0 || z < 0 || y < 0 || x > static_cast<std::int32_t>(building->size_x) ||
                z > static_cast<std::int32_t>(building->size_y) || y >= static_cast<std::int32_t>(building->size_z)) continue;
            const auto index = static_cast<std::size_t>(x) + (building->size_x + 1U) *
                (static_cast<std::size_t>(z) + (building->size_y + 1U) * static_cast<std::size_t>(y));
            const auto matches = [&](const SbbCell& cell) { return cell.x == static_cast<std::uint32_t>(x) &&
                cell.y == static_cast<std::uint32_t>(y) && cell.z == static_cast<std::uint32_t>(z); };
            if (index < building->cells.size() && matches(building->cells[index])) replaced = building->cells[index].exterior_block() != 0;
            else {
                // Parser output is dense; authored/test buildings can be sparse.
                const auto cell = std::ranges::find_if(building->cells, matches);
                replaced = cell != building->cells.end() && cell->exterior_block() != 0;
            }
        }
        if (replaced) diff.under_buildings.push_back(tile);
    }
    return diff;
}

BuildingRenderBuffer build_city_road_tile_buffer(const std::vector<CityTileCell>& tiles, float unit_size) {
    return build_city_tile_render_buffer(tiles, unit_size);
}

MapScene build_map_scene(const SbcMap& map, const std::vector<const SbbBuilding*>& placement_buildings, bool auto_generated,
    bool fit_game_world, MapSceneBuild detail) {
    PAFTA_PROFILE_FUNCTION();
    MapScene scene;
    // The game's build order (World_LoadMapAndInitializeTerrain @56980): the street
    // graph, then street and intersection tiles, district and piece cells, and building
    // footprints (all of which stamp the corner-height grid), then growing the extents,
    // then rebuilding the terrain with its patches.
    auto graph = build_native_city_graph(map);
    CityCornerStampMap corner_stamps;
    corner_stamps.disabled = auto_generated;
    // The mask bounds use the raw intersection extents (0x35fd1444..0x35fd1458),
    // collected before any tile is placed.
    if (!map.intersections.empty()) {
        corner_stamps.mask_min_x = corner_stamps.mask_min_z = std::numeric_limits<std::int32_t>::max();
        corner_stamps.mask_max_x = corner_stamps.mask_max_z = std::numeric_limits<std::int32_t>::lowest();
        for (const auto& intersection : map.intersections) {
            corner_stamps.mask_min_x = std::min(corner_stamps.mask_min_x, intersection.x);
            corner_stamps.mask_max_x = std::max(corner_stamps.mask_max_x, intersection.x);
            corner_stamps.mask_min_z = std::min(corner_stamps.mask_min_z, intersection.z);
            corner_stamps.mask_max_z = std::max(corner_stamps.mask_max_z, intersection.z);
        }
    }
    scene.city_tiles = build_city_tile_cells(graph, corner_stamps, auto_generated);
    if (fit_game_world) scene.world_fit = fit_to_game_world(scene.city_tiles, graph, map, placement_buildings);
    remove_replaced_city_tiles(scene.city_tiles, map, placement_buildings);
    // The road mesh and its shadow volume only need the tiles, so build them while the
    // terrain builds.
    auto tile_buffer = std::async(
#ifdef __EMSCRIPTEN__
        std::launch::deferred,
#else
        std::launch::async,
#endif
        [&tiles = std::as_const(scene.city_tiles), unit = scene.unit_size, detail] {
            auto buffer = build_city_tile_render_buffer(tiles, unit);
            if (detail == MapSceneBuild::Complete) bake_shadow_volume(buffer);
            return buffer;
        });
    stamp_city_portal_cells(corner_stamps, map);
    stamp_city_building_footprints(corner_stamps, map, placement_buildings);
    scene.traffic_light_poles = build_city_traffic_light_poles(graph, scene.unit_size);
    // Map labels (DrawMapMenu @88040): a street along X is named on its first segment
    // (the one with no street arriving at its start from -X). A street along Z is named
    // on the segment starting at gz 320..323, moved 16 map units down on odd avenue
    // columns. Labels sit at the middle of the segment, at (world - 896) / 8.
    for (const auto& street : graph.streets) {
        const auto& a = graph.intersections[static_cast<std::size_t>(street.a)];
        const auto& b = graph.intersections[static_cast<std::size_t>(street.b)];
        const bool labelled = street.orientation == 0
            ? a.street_north < 0
            : static_cast<std::uint32_t>(a.gz - 320) < 4U;
        if (!labelled) continue;
        auto y = (static_cast<float>((a.gz + b.gz) * 2) - 896.0F) * 0.125F;
        if (street.orientation == 1) y += static_cast<float>(((a.gx - 256) / 56) & 1) * 16.0F;
        scene.map_labels.push_back({street.name_index, (static_cast<float>((a.gx + b.gx) * 2) - 896.0F) * 0.125F, y});
    }
    expand_native_city_extents(graph);
    if (detail == MapSceneBuild::Complete) scene.terrain = build_city_terrain(map, graph, corner_stamps, scene.city_tiles, scene.unit_size);
    for (const auto& street : graph.streets) {
        if (street.a < 0 || street.b < 0 || street.source_index < 0) {
            continue;
        }
        const auto from_index = static_cast<std::size_t>(street.a);
        const auto to_index = static_cast<std::size_t>(street.b);
        const auto source_index = static_cast<std::size_t>(street.source_index);
        if (from_index >= graph.intersections.size() || to_index >= graph.intersections.size() ||
            source_index >= map.streets.size()) {
            continue;
        }

        const auto& source = map.streets[source_index];
        const auto& source_from = map.intersections[static_cast<std::size_t>(source.from_intersection)];
        const auto& source_to = map.intersections[static_cast<std::size_t>(source.to_intersection)];
        const auto& from = graph.intersections[from_index];
        const auto& to = graph.intersections[to_index];
        Vec3 guide_from{
            static_cast<float>(from.gx) * scene.unit_size,
            static_cast<float>(from.level + 1) * scene.unit_size,
            static_cast<float>(from.gz) * scene.unit_size,
        };
        Vec3 guide_to{
            static_cast<float>(to.gx) * scene.unit_size,
            static_cast<float>(to.level + 1) * scene.unit_size,
            static_cast<float>(to.gz) * scene.unit_size,
        };
        if (street.orientation == 0) {
            guide_from.x += static_cast<float>(from.ext_x_pos) * scene.unit_size;
            guide_to.x -= static_cast<float>(to.ext_x_neg) * scene.unit_size;
        } else {
            guide_from.z += static_cast<float>(from.ext_z_pos) * scene.unit_size;
            guide_to.z -= static_cast<float>(to.ext_z_neg) * scene.unit_size;
        }

        const auto dx = guide_to.x - guide_from.x;
        const auto dz = guide_to.z - guide_from.z;
        const auto length = std::sqrt(dx * dx + dz * dz);
        std::array<Vec3, 4> editor_quad{};
        if (length > 0.0F) {
            const Vec3 perpendicular{-dz / length, 0.0F, dx / length};
            const auto left = static_cast<float>(street.left_width + 1) * scene.unit_size;
            const auto right = static_cast<float>(street.right_width + 1) * scene.unit_size;
            editor_quad = {{
                {guide_from.x - perpendicular.x * left, guide_from.y, guide_from.z - perpendicular.z * left},
                {guide_from.x + perpendicular.x * right, guide_from.y, guide_from.z + perpendicular.z * right},
                {guide_to.x + perpendicular.x * right, guide_to.y, guide_to.z + perpendicular.z * right},
                {guide_to.x - perpendicular.x * left, guide_to.y, guide_to.z - perpendicular.z * left},
            }};
        }

        scene.roads.push_back({
            source,
            source_index,
            {
                static_cast<float>(source_from.x) * scene.unit_size,
                native_road_surface_y(source_from.y - 1),
                static_cast<float>(source_from.z) * scene.unit_size,
            },
            {
                static_cast<float>(source_to.x) * scene.unit_size,
                native_road_surface_y(source_to.y - 1),
                static_cast<float>(source_to.z) * scene.unit_size,
            },
            static_cast<float>(street.left_width + street.right_width + 2) * scene.unit_size,
            editor_quad,
        });
    }
    scene.city_tile_buffer = tile_buffer.get();
    scene.city_tile_fingerprint = 1469598103934665603ULL;
    for (const auto& tile : scene.city_tiles) {
        for (const auto value : {static_cast<std::uint64_t>(static_cast<std::uint32_t>(tile.x)),
                 static_cast<std::uint64_t>(static_cast<std::uint32_t>(tile.y)),
                 static_cast<std::uint64_t>(static_cast<std::uint32_t>(tile.z)),
                 static_cast<std::uint64_t>(tile.block),
                 static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(tile.base_y)),
                 static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(tile.slope_x)),
                 static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(tile.slope_z))}) {
            scene.city_tile_fingerprint = (scene.city_tile_fingerprint ^ value) * 1099511628211ULL;
        }
        for (const auto face : tile.faces) scene.city_tile_fingerprint = (scene.city_tile_fingerprint ^ face) * 1099511628211ULL;
    }

    for (std::size_t intersection_index = 0; intersection_index < map.intersections.size(); ++intersection_index) {
        const auto& intersection = map.intersections[intersection_index];
        const auto& native = graph.intersections[intersection_index];
        auto width = 2.0F * scene.unit_size;
        for (const auto& road : scene.roads) {
            if (road.source.from_intersection == static_cast<std::int32_t>(intersection_index) ||
                road.source.to_intersection == static_cast<std::int32_t>(intersection_index)) {
                width = std::max(width, road.width);
            }
        }

        scene.intersections.push_back({
            intersection,
            intersection_index,
            {
                static_cast<float>(intersection.x) * scene.unit_size,
                native_road_surface_y(intersection.y - 1),
                static_cast<float>(intersection.z) * scene.unit_size,
            },
            width,
            {{
                {
                    static_cast<float>(native.gx - native.ext_x_neg) * scene.unit_size,
                    static_cast<float>(native.level + 1) * scene.unit_size,
                    static_cast<float>(native.gz + native.ext_z_pos) * scene.unit_size,
                },
                {
                    static_cast<float>(native.gx + native.ext_x_pos) * scene.unit_size,
                    static_cast<float>(native.level + 1) * scene.unit_size,
                    static_cast<float>(native.gz + native.ext_z_pos) * scene.unit_size,
                },
                {
                    static_cast<float>(native.gx + native.ext_x_pos) * scene.unit_size,
                    static_cast<float>(native.level + 1) * scene.unit_size,
                    static_cast<float>(native.gz - native.ext_z_neg) * scene.unit_size,
                },
                {
                    static_cast<float>(native.gx - native.ext_x_neg) * scene.unit_size,
                    static_cast<float>(native.level + 1) * scene.unit_size,
                    static_cast<float>(native.gz - native.ext_z_neg) * scene.unit_size,
                },
            }},
        });
    }

    for (std::size_t building_index = 0; building_index < map.buildings.size(); ++building_index) {
        const auto& building = map.buildings[building_index];
        const auto origin = native_city_building_render_origin(building, scene.unit_size);
        scene.buildings.push_back({
            building,
            building_index,
            origin,
            static_cast<float>(building.rotation),
        });
    }

    auto min_x = std::numeric_limits<float>::max();
    auto max_x = std::numeric_limits<float>::lowest();
    auto min_z = std::numeric_limits<float>::max();
    auto max_z = std::numeric_limits<float>::lowest();
    auto include = [&](Vec3 position) {
        min_x = std::min(min_x, position.x);
        max_x = std::max(max_x, position.x);
        min_z = std::min(min_z, position.z);
        max_z = std::max(max_z, position.z);
    };

    for (const auto& intersection : scene.intersections) {
        include(intersection.position);
    }
    for (const auto& building : scene.buildings) {
        include(building.position);
    }
    for (const auto& tile : scene.city_tiles) {
        include({
            static_cast<float>(tile.x) * scene.unit_size,
            static_cast<float>(tile.y) * scene.unit_size,
            static_cast<float>(tile.z) * scene.unit_size,
        });
    }

    if (scene.intersections.empty() && scene.buildings.empty() && scene.city_tiles.empty()) {
        min_x = 0.0F;
        max_x = 0.0F;
        min_z = 0.0F;
        max_z = 0.0F;
    }
    scene.bounds = {min_x, max_x, min_z, max_z};

    return scene;
}

BuildblockPreviewScene build_buildblock_preview_scene(const SblTemplate& buildblock) {
    BuildblockPreviewScene scene;
    constexpr auto scale = 4.0F;
    constexpr auto thickness = 0.08F;

    auto add_rect = [&](std::size_t layer_index, BuildblockPreviewRectKind kind, float x, float width, float y, float height) {
        if (width <= 0.0F || height <= 0.0F) {
            return;
        }
        scene.rects.push_back({
            layer_index,
            kind,
            {
                static_cast<float>(layer_index) * scene.layer_spacing + (x + width * 0.5F) * scale,
                (y + height * 0.5F) * scale,
                kind == BuildblockPreviewRectKind::Bounds ? 0.0F : 0.08F,
            },
            {width * scale, height * scale, thickness},
        });
    };

    for (const auto& layer : buildblock.layers) {
        add_rect(layer.index, BuildblockPreviewRectKind::Bounds, layer.bounds[0], layer.bounds[1], layer.bounds[2], layer.bounds[3]);
        for (const auto& window : layer.windows) {
            add_rect(layer.index, BuildblockPreviewRectKind::Window, window.x, window.width, window.y, window.height);
        }
        for (const auto& beam : layer.beams) {
            add_rect(layer.index, BuildblockPreviewRectKind::Beam, beam.x, beam.width, beam.y, beam.height);
        }
        for (const auto& door : layer.doors) {
            scene.doors.push_back({
                layer.index,
                door.type,
                {
                    static_cast<float>(layer.index) * scene.layer_spacing + door.x * scale,
                    door.y * scale,
                    0.18F,
                },
            });
        }
    }

    return scene;
}

CityBlockPreviewScene build_city_block_preview_scene(const SbcCityBlock& city_block, const std::vector<NamedBlockMesh>& block_meshes) {
    CityBlockPreviewScene scene;

    const auto resolution = resolve_city_block_render(city_block, block_meshes);
    if (resolution.render_class == CityBlockRenderClass::Mesh && resolution.mesh != nullptr) {
        scene.pieces.push_back({
            0,
            0,
            std::string{city_block_display_name(city_block)},
            {},
            *resolution.mesh,
        });
        return scene;
    }

    scene.solid_cell = CityBlockPreviewSolidCell{
        std::string{city_block_display_name(city_block)},
        build_city_block_solid_preview_buffer(),
    };
    return scene;
}

NativeRotatedBlockFace rotate_block_face_native(const SblBlockMesh& mesh, const SblBlockFace& face, std::int32_t rotation) {
    return rotate_block_face_native_impl(mesh, face, rotation);
}

// -----------------------------------------------------------------------------
// Buildblock portal overlay builders
// -----------------------------------------------------------------------------

std::vector<PortalRect> build_portal_overlay_rects(const SblLayer& layer, PortalSide side) {
    const auto bounds = portal_rect_for_side(layer, side, layer.bounds[0], layer.bounds[1], layer.bounds[2], layer.bounds[3]);
    if (!bounds.has_value()) {
        return {};
    }
    return {*bounds};
}

std::vector<PortalRect> build_portal_wall_rects(const SblLayer& layer, PortalSide side) {
    const auto bounds = portal_rect_for_side(layer, side, layer.bounds[0], layer.bounds[1], layer.bounds[2], layer.bounds[3]);
    if (!bounds.has_value()) {
        return {};
    }

    auto walls = subtract_portal_rect(unit_portal_rect(), *bounds);
    for (const auto& window : layer.windows) {
        const auto cutout = portal_rect_for_side(layer, side, window.x, window.width, window.y, window.height);
        if (!cutout.has_value() || !rects_intersect(*bounds, *cutout)) {
            continue;
        }

        std::vector<PortalRect> next;
        for (const auto& wall : walls) {
            auto fragments = subtract_portal_rect(wall, *cutout);
            next.insert(next.end(), fragments.begin(), fragments.end());
        }
        walls = std::move(next);
    }

    return walls;
}

} // namespace pafta
