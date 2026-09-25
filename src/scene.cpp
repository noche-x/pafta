#include "scene.hpp"

#include "buildblock_catalog.hpp"
#include "profiler.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace map_editor {

std::uint32_t terrain_noise_hash(std::int32_t x, std::int32_t y, std::uint32_t mask) {
    auto u = ((static_cast<std::uint32_t>(y + 1) & mask) * 0x39U) +
        (static_cast<std::uint32_t>(x + 1) & mask);
    u = u ^ (u * 0x2000U);
    const auto v = u * u * 0x3d73U + 0xc0ae5U;
    return (v * v + 0xd208dd0dU) & 0x7fffffffU;
}

namespace {

// -----------------------------------------------------------------------------
// Portal Rectangle Helpers
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
// Asset and Cell Lookup
// -----------------------------------------------------------------------------

const SblBlockMesh* find_block_mesh(const std::vector<NamedBlockMesh>& block_meshes, std::string_view name) {
    const auto match = std::ranges::find_if(block_meshes, [name](const NamedBlockMesh& entry) {
        return entry.name == name;
    });
    return match == block_meshes.end() ? nullptr : &match->mesh;
}

SblBlockMesh make_native_builtin_block_mesh_impl() {
    SblBlockMesh mesh{};
    // VERIFY: the native decomp copies g_Block_Width/Height/Depth before disk
    // LoadBlock_SBL; the existing TS/importer parity notes treat the built-in
    // primitive as 1x1x1.
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
    // UpdatePieceVoxelData_14005fc90 initializes every city block as
    // 0x40000000 and only switches non-empty names that match loaded block
    // templates to 0x80000000 | blockIndex mesh instances.
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

// The native global buildblock catalog (g_buildblockTemplateCatalog) is what v10
// district piece words index by `word & 0xffff`. The game builds it in
// BuildblockCatalog_LoadAll @0x26e70: scan loose buildblock/*.sbl only (NOT CSX),
// strip the extension, sort names bytewise, cap at 64, then index in that order.
// Reproduce that ordering so a district word's index lands on the same template
// the game would resolve. An out-of-range or empty-`block` template renders nothing,
// exactly as the native instancing loop does for unfilled catalog slots.
std::vector<const NamedBuildblockTemplate*> district_buildblock_catalog_order(
    const std::vector<NamedBuildblockTemplate>& templates) {
    std::vector<const NamedBuildblockTemplate*> ordered;
    ordered.reserve(templates.size());
    for (const auto& entry : templates) {
        ordered.push_back(&entry);
    }
    std::ranges::sort(ordered, [](const NamedBuildblockTemplate* lhs, const NamedBuildblockTemplate* rhs) {
        return lhs->name < rhs->name;
    });
    if (ordered.size() > 64U) {
        ordered.resize(64U);
    }
    return ordered;
}

// The real v10 render catalog is g_buildblockTemplateCatalog, built procedurally
// by Buildblock_InitTemplateMeshCatalog @0x10110 (curbs, railings, beams baked in
// 4 Y rotations), NOT the loose buildblock/*.sbl files. Build it once and reuse.
// A district piece word indexes this by (word & 0xffff). An empty entry (vertex/
// face count 0) renders nothing, exactly as the native instancing loop does.
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
// Building Render Buffer Helpers
// -----------------------------------------------------------------------------

constexpr auto native_preview_floor_y = 1.0F / 16.0F;
constexpr auto native_preview_ceiling_y = 7.0F / 8.0F;
constexpr auto native_preview_inset_depth = 1.0F / 16.0F;

bool is_solid_building_cell(const SbbCell& cell) {
    // BuildBuildingBlockRenderBuffers_1400799d0.c:128: 0x40000000 class is the solid-cell tag.
    return (cell.exterior_block() >> 29U) == 2U;
}

std::optional<SbbCellPayload> exterior_piece_payload(const SbbCell& cell) {
    const auto word = cell.exterior_block();
    // BuildBuildingBlockRenderBuffers_1400799d0.c:212: 0x80000000 class instantiates a block mesh.
    if ((word >> 29U) != 4U) {
        return std::nullopt;
    }
    return SbbCellPayload{PayloadKind::Piece, word, word & 0x3ffU};
}

std::optional<SbbCellPayload> interior_piece_payload(const SbbCell& cell) {
    const auto word = cell.interior_block();
    // BuildBuildingBlockRenderBuffers_1400799d0.c:212: 0x80000000 class instantiates a block mesh.
    if ((word >> 29U) != 4U) {
        return std::nullopt;
    }
    return SbbCellPayload{PayloadKind::Piece, word, word & 0x3ffU};
}

bool cell_has_exterior_surface(const SbbCell* cell) {
    return cell != nullptr && is_solid_building_cell(*cell);
}

PortalSideCodes native_edge_side_codes(const SbbCell& cell, const SbbCell* east) {
    // BuildBuildingBlockRenderBuffers_1400799d0.c:136-141 fills edgeFlags for
    // faces 1..4; lines 177-180 consume edgeFlags[face-1] as layer index+3.
    // Native face -> C++ side mapping from lines 149-152:
    // face 1 z+1 -> South, face 2 x-1 -> West, face 3 z-1 -> North,
    // face 4 x+1 -> East.
    return {
        cell.exterior_portal_face(), // face 3 / North: BuildingCell +0x0c.
        cell.interior_portal_face(), // face 2 / West: BuildingCell +0x10.
        cell.exterior_portal_face(), // face 1 / South: BuildingCell +0x0c.
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

// RenderBuildingCellItemsetBoundaryPreview_140079050 and FUN_1400523b0 index
// DAT_140105000 / DAT_140105170 / DAT_140105148. VERIFY(editor.exe
// DAT_140105000): the supplied raw corner order is non-planar with
// DAT_140105170, so this keeps the planar fallback from the prior parity pass.
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

Vec2f native_oriented_uv(const Vec3& position, Vec2f fallback, std::uint32_t orientation) {
    switch (orientation & 0xf0000U) {
        case 0x10000U:
            return {position.z, position.y};
        case 0x20000U:
            return {1.0F - position.z, position.y};
        case 0x30000U:
            return {position.x, position.z};
        case 0x40000U:
            return {position.x, 1.0F - position.z};
        case 0x50000U:
            return {1.0F - position.x, position.y};
        case 0x60000U:
            return {position.x, position.y};
        default:
            return fallback;
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
            rotated.uvs[index] = native_oriented_uv(rotated.vertices[index], rotated.uvs[index], orientation);
        }
    }

    return rotated;
}

// Tilts a cell's TOP surface to a grade. dy_x / dy_z are the world-Y rise per +1
// cell along each axis; only vertices on the cell top (y == max_y) are displaced,
// so the box keeps a flat bottom and the upper edges of its side walls follow the
// slope. Default-constructed (all zero) leaves geometry axis-aligned.
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
            // Only the cell's top vertices ride the grade; the bottom stays flat
            // so adjacent cells still share a watertight base.
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

std::uint32_t material_for_piece_face(const SbbCell& cell, std::uint32_t slot, bool interior) {
    // RenderBuildingCellFaceGroupWorld/Preview clamp logical face slots to
    // 0..7. Interior faces use the cell's interior texture set for slots 0..7;
    // slots 8..15 address the exterior set after subtracting eight.
    if (interior) {
        return material_for_cell_slot(cell, slot < 8U ? slot + 8U : slot < 16U ? slot - 8U : 0U);
    }
    return material_for_cell_slot(cell, std::min(slot, 7U));
}

std::array<Vec3, 4> native_preview_cube_face_vertices(const SbbCell& cell, std::uint32_t face, float unit_size) {
    std::array<Vec3, 4> vertices{};
    const auto& corners = native_preview_face_corners[face];
    const auto cell_x = static_cast<float>(cell.x);
    const auto cell_y = static_cast<float>(cell.y);
    const auto cell_z = static_cast<float>(cell.z);
    // Side walls (faces 1-4) are inset 1/16 inward from the cell boundary, matching
    // editor.exe SetupDefaultFaceVerticesAndEmit (the side template sits at z=0.0625
    // then RotateBlockFaceVertices_90Degrees rotates it to each direction). Without
    // this inset, adjacent solid cells' shared-boundary walls land coplanar and
    // z-fight/squish; the inset places opposing walls 1/16 and 15/16, separated.
    // The inset is along the face's inward normal; with native_preview_face_corners
    // each side face is axis-planar so we push its constant axis toward the cell center.
    for (std::size_t corner = 0; corner < vertices.size(); ++corner) {
        auto position = native_preview_cube_corners[corners[corner]];
        if (face != 0U && face != 5U) {
            // Pull the boundary axis (the one that is constant across all 4 corners)
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
    // RenderBuildingToolCellPreview_140078060 packs these nibbles before
    // FUN_140051070(1, key): own exterior, east interior, south exterior,
    // own interior.
    return {{
        cell.exterior_portal_face() & 0xfU,
        east == nullptr ? 0U : east->interior_portal_face() & 0xfU,
        south == nullptr ? 0U : south->exterior_portal_face() & 0xfU,
        cell.interior_portal_face() & 0xfU,
    }};
}

// VERIFY: orientation corrected from screenshot; pending another visual confirm.
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
    // Windows are openings: they read as see-through cutouts like the door gap,
    // not solid inset boxes. Draw each window as a thin frame around its rect
    // perimeter, leaving the middle open so you can see through it. A frame is four
    // narrow strips; frame_w is the strip thickness in cell-fraction units.
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
    // VERIFY(editor.exe FUN_140051c60): decompilation proves the first rect
    // array at +0x1138; beams share the same SblPortalRect shape in this port
    // and are emitted as equivalent inset boxes for visible beam parity.
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

std::uint32_t city_buildblock_side_code_for_face(
    const SbbCell& cell,
    const SbbCell* east,
    BuildingFaceSide side) {
    switch (side) {
        case BuildingFaceSide::South:
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

std::vector<PortalRect> city_buildblock_window_rects(const SblLayer& layer, PortalSide side) {
    const auto bounds = portal_rect_for_side(layer, side, layer.bounds[0], layer.bounds[1], layer.bounds[2], layer.bounds[3]);
    if (!bounds.has_value()) {
        return {};
    }

    std::vector<PortalRect> rects;
    for (const auto& window : layer.windows) {
        const auto rect = portal_rect_for_side(layer, side, window.x, window.width, window.y, window.height);
        if (rect.has_value() && rects_intersect(*bounds, *rect)) {
            rects.push_back(*rect);
        }
    }
    return rects;
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
    const std::array<SolidCellFaceCheck, 5>& native_sides,
    const SbbCell* east,
    const SblTemplate* template_file,
    MaterialSelector select_material,
    CellTopSlope top_slope = {}) {
    for (const auto& side : native_sides) {
        if (cell_has_exterior_surface(side.neighbor)) {
            continue;
        }

        std::vector<PortalRect> window_rects;
        const auto portal_side = portal_side_for_building_face(side.side);
        // BuildBuildingBlockRenderBuffers only expands the exterior edge
        // profile when the cell has no interior block (native
        // interiorBlock == 0xffff). An interior piece owns that side's
        // geometry and must not inherit the exterior window mask.
        if (portal_side.has_value() && cell.interior_block() == 0xffffU) {
            const auto side_code = city_buildblock_side_code_for_face(cell, east, side.side);
            if (const auto* layer = buildblock_layer_for_side_code(template_file, side_code)) {
                // FUN_140051c60 emits the window rectangles. The game then builds
                // a per-signature texture-array layer from the wall texture and
                // writes the window mask into its alpha channel. Keep the wall
                // material on these separate quads so the shader sees the same
                // base color instead of an unrelated replacement texture.
                window_rects = city_buildblock_window_rects(*layer, *portal_side);
            }
        }

        const auto wall_material = select_material(side.side);
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
    MaterialSelector select_material) {
    const auto cell = native_city_block_solid_cell();
    const auto half = buffer.unit_size * 0.5F;
    const auto cx = origin.x + half;
    const auto cy = origin.y + half;
    const auto cz = origin.z + half;
    // BuildBuildingBlockRenderBuffers emits native faces 1..5 for solid cells;
    // face 0/down stays absent here, matching the existing building solid path.
    const std::array<SolidCellFaceCheck, 5> exposed_sides{{
        {BuildingFaceSide::South, nullptr},
        {BuildingFaceSide::West, nullptr},
        {BuildingFaceSide::North, nullptr},
        {BuildingFaceSide::East, nullptr},
        {BuildingFaceSide::Up, nullptr},
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
    // BuildBuildingBlockRenderBuffers_1400799d0.c:177-180 consumes side-code
    // sc 3..15 as profile index sc-3 into the buildblock/itemset geometry table.
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
    // BuildBuildingBlockRenderBuffers_1400799d0.c:271-276 recenters every
    // emitted vertex by -g_volCenter*cellSize; SBB anchors carry that center.
    return {
        (static_cast<float>(cell.x) - static_cast<float>(building.anchor_x) + 0.5F) * unit_size,
        (static_cast<float>(cell.y) - static_cast<float>(building.anchor_y) + 0.5F) * unit_size,
        (static_cast<float>(cell.z) - static_cast<float>(building.anchor_z) + 0.5F) * unit_size,
    };
}

// -----------------------------------------------------------------------------
// Picking Helpers
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
    CityTileKind kind{};
    // World Y of the walkable top surface at the cell center. For roads this is
    // the drivable asphalt plane (native level * 4.0 + 4.25); for curbs it is the
    // concrete plate top (road surface + 0.25).
    float top_y{};
    // World-Y gradient of the top surface, per +1 cell along each axis. Drives
    // the tilted top so a sloping street reads as a continuous grade.
    float slope_x{};
    float slope_z{};
    // SbcMap::streets index that laid this tile (-1 for intersection tiles, which
    // belong to no single street). Lets clicks on the real asphalt resolve to a
    // road.
    std::int32_t street_index{-1};
};

using CityTileMap = std::unordered_map<CityTileKey, CityTileValue, CityTileKeyHash>;

bool city_tile_kind_overrides(CityTileKind incoming, CityTileKind current) {
    return incoming == CityTileKind::Road && current == CityTileKind::Curb;
}

void set_city_tile(
    CityTileMap& tiles,
    std::int32_t x,
    std::int32_t y,
    std::int32_t z,
    CityTileKind kind,
    float top_y,
    float slope_x = 0.0F,
    float slope_z = 0.0F,
    std::int32_t street_index = -1) {
    const CityTileKey key{x, y, z};
    const auto [match, inserted] = tiles.emplace(key, CityTileValue{kind, top_y, slope_x, slope_z, street_index});
    if (!inserted) {
        if (city_tile_kind_overrides(kind, match->second.kind)) {
            match->second.kind = kind;
            match->second.top_y = top_y;
            match->second.slope_x = slope_x;
            match->second.slope_z = slope_z;
            match->second.street_index = street_index;
        } else if (match->second.street_index < 0 && street_index >= 0) {
            // Adopt a street owner if this tile didn't have one yet.
            match->second.street_index = street_index;
        }
    }
}

// -----------------------------------------------------------------------------
// Native City Street Graph
// (subrosa.exe World_LoadMapAndInitializeTerrain @56980 intersection loader +
//  MapConnector_AddStreetSegment @38e0)
// -----------------------------------------------------------------------------

// Native runtime intersection record. Axis naming follows the editor's SbcMap:
// editor x = native unk0[1], editor z = native pos.x. The native loader stores
// level = sbc y - 1; the drivable road surface sits at level * 4.0 + 4.25
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

// Post-placement extent expansion (World_LoadMapAndInitializeTerrain @56980:
// every positive extent grows by one tile after the perimeter tiles are placed,
// before lane bounds and the terrain patches are computed).
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

// Corner-stamp height grid (UpdateMapTopHeightAndClearOccupancyBit @3fa40).
// One float per tile-grid corner, max-updated; a tile counts as road interior
// for the terrain patch when all four of its corners are stamped > 0.
struct CityCornerStampMap {
    std::unordered_map<std::uint64_t, float> heights;

    static std::uint64_t key(std::int32_t gx, std::int32_t gz) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(gx)) << 32U) |
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(gz));
    }

    void stamp(std::int32_t gx, std::int32_t gz, float height) {
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

// District/piece cells stamp the corner-height grid too
// (ApplyAllCityDistrictCellsToWorld @20000): every occupied cell of an 8^3
// district volume stamps its tile at the cell's own level * 4. The editor's
// portals are those district volumes: fields[1..3] = position in 8-cell units,
// voxel index decodes as y*64 + z*8 + x.
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
                portal.fields[1] * 8 + local_x,
                portal.fields[3] * 8 + local_z,
                static_cast<float>(portal.fields[2] * 8 + local_y) * 4.0F);
        }
    }
}

// Building instantiation stamps every occupied cell of the building's ANCHOR
// layer at the placement record's y * 4 (InstantiateSbbVolumeIntoWorldAndMetadata
// @23d00: UpdateMapTopHeight fires only when the local layer equals the anchor
// layer). Cell tiles are resolved through the verified city placement transform
// (rotation about the anchor), sampling each cell's rotated center.
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
            if (!cell.occupied || cell.y != building->anchor_y) {
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

// Street tile placement (MapConnector_PlaceStreetSegmentTilesAndSlopes @20580).
// Lane cells are solid asphalt tiles at the current integer height level; curbs
// are block-mesh plates one level above. The slope is a centered integer
// run-length region between the two intersection footprints; the editor renders
// it as a continuous tilted top (equivalent to the game's pre-baked 1/16-step
// tilted block meshes per Buildblock_InitTemplateMeshCatalog @10110).
void place_native_street_tiles(
    CityTileMap& tiles,
    CityCornerStampMap& stamps,
    const NativeCityGraph& graph,
    const NativeCityStreet& street) {
    if (street.a < 0 || street.b < 0) {
        return;
    }
    const auto& from = graph.intersections[static_cast<std::size_t>(street.a)];
    const auto& to = graph.intersections[static_cast<std::size_t>(street.b)];
    const auto along_x = street.orientation == 0;

    const auto start = along_x ? from.gx + from.ext_x_pos : from.gz + from.ext_z_pos;
    const auto seg_len = (along_x ? to.gx - to.ext_x_neg : to.gz - to.ext_z_neg) - start;
    const auto center = along_x ? from.gz : from.gx;
    if (seg_len <= 0) {
        return;
    }

    const auto delta = to.level - from.level;
    const auto abs_delta = std::abs(delta);
    const auto run_len = abs_delta > 0 ? std::max(1, seg_len / abs_delta) : 6;
    const auto half = (abs_delta * run_len) >> 1;
    const auto slope_start = (seg_len >> 1) - half;
    const auto slope_end = (seg_len >> 1) + half;

    const auto from_top = native_road_surface_y(from.level);
    const auto to_top = native_road_surface_y(to.level);
    const auto slope_span = std::max(1, slope_end - slope_start);
    const auto rise_per_cell = slope_end > slope_start
        ? (to_top - from_top) / static_cast<float>(slope_span)
        : 0.0F;
    const auto top_at = [&](std::int32_t along) {
        if (along < slope_start || slope_end <= slope_start) {
            return from_top;
        }
        if (along >= slope_end) {
            return to_top;
        }
        return from_top + (to_top - from_top) *
            ((static_cast<float>(along - slope_start) + 0.5F) / static_cast<float>(slope_span));
    };

    auto place = [&](std::int32_t along_coord, std::int32_t cross_coord, std::int32_t level,
                     CityTileKind kind, float top_y, float grade) {
        const auto x = along_x ? along_coord : cross_coord;
        const auto z = along_x ? cross_coord : along_coord;
        const auto slope_x = along_x ? grade : 0.0F;
        const auto slope_z = along_x ? 0.0F : grade;
        set_city_tile(tiles, x, level, z, kind, top_y, slope_x, slope_z, street.source_index);
    };

    auto height = from.level;
    auto run_pos = 0;
    for (auto along = 0; along < seg_len; ++along) {
        const auto in_slope = slope_start <= along && along < slope_end;
        if (in_slope && delta > 0 && run_pos % run_len == 0) {
            height += 1;
        }

        const auto coord = start + along;
        const auto left_curb = center - street.left_width - 1;
        const auto right_curb = center + street.right_width;
        const auto lane_top = in_slope ? top_at(along) : native_road_surface_y(height);
        const auto grade = in_slope ? rise_per_cell : 0.0F;

        // Curbs: flat plates at height+1 outside the slope region (and at the
        // segment end caps); inside the slope they follow the ramp.
        if (along == 0 || along == seg_len - 1 || !in_slope) {
            const auto curb_top = native_road_surface_y(height) + 0.25F;
            place(coord, left_curb, height + 1, CityTileKind::Curb, curb_top, 0.0F);
            place(coord, right_curb, height + 1, CityTileKind::Curb, curb_top, 0.0F);
        } else {
            place(coord, left_curb, height + 1, CityTileKind::Curb, lane_top + 0.25F, grade);
            place(coord, right_curb, height + 1, CityTileKind::Curb, lane_top + 0.25F, grade);
        }

        for (auto offset = -street.left_width; offset < street.right_width; ++offset) {
            place(coord, center + offset, height, CityTileKind::Road, lane_top, grade);
        }

        // Terrain top stamp spans curb to curb (native ~leftWidth .. rightWidth).
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
}

// Intersection footprint + perimeter (MapConnector_PlaceIntersectionPerimeterTiles
// @20130): solid asphalt fill over the asymmetric extents; missing-neighbor
// sides get curb plate rows one level above, extended one tile past both ends.
void place_native_intersection_tiles(
    CityTileMap& tiles,
    const NativeCityGraph& graph,
    const NativeCityIntersection& intersection) {
    const auto top = native_road_surface_y(intersection.level);
    for (auto x = intersection.gx - intersection.ext_x_neg; x < intersection.gx + intersection.ext_x_pos; ++x) {
        for (auto z = intersection.gz - intersection.ext_z_neg; z < intersection.gz + intersection.ext_z_pos; ++z) {
            set_city_tile(tiles, x, intersection.level, z, CityTileKind::Road, top);
        }
    }

    (void)graph;
    const auto curb_level = intersection.level + 1;
    const auto curb_top = top + 0.25F;
    const auto z_row_begin = intersection.gz - intersection.ext_z_neg - 1;
    const auto z_row_end = intersection.gz + intersection.ext_z_pos;  // inclusive
    const auto x_row_begin = intersection.gx - intersection.ext_x_neg - 1;
    const auto x_row_end = intersection.gx + intersection.ext_x_pos;  // inclusive
    if (intersection.street_south < 0) {
        const auto x = intersection.gx + intersection.ext_x_pos;
        for (auto z = z_row_begin; z <= z_row_end; ++z) {
            set_city_tile(tiles, x, curb_level, z, CityTileKind::Curb, curb_top);
        }
    }
    if (intersection.street_north < 0) {
        // Native quirk: the -x row offset uses ext_x_pos (field_0x2c), not the
        // -x extent.
        const auto x = intersection.gx - intersection.ext_x_pos - 1;
        for (auto z = z_row_begin; z <= z_row_end; ++z) {
            set_city_tile(tiles, x, curb_level, z, CityTileKind::Curb, curb_top);
        }
    }
    if (intersection.street_west < 0) {
        const auto z = intersection.gz + intersection.ext_z_pos;
        for (auto x = x_row_begin; x <= x_row_end; ++x) {
            set_city_tile(tiles, x, curb_level, z, CityTileKind::Curb, curb_top);
        }
    }
    if (intersection.street_east < 0) {
        const auto z = intersection.gz - intersection.ext_z_neg - 1;
        for (auto x = x_row_begin; x <= x_row_end; ++x) {
            set_city_tile(tiles, x, curb_level, z, CityTileKind::Curb, curb_top);
        }
    }
}


} // namespace

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

float native_like_city_terrain_height(float world_x, float world_z) {
    // Native runtime terrain samples use grid = world + 4096 at 1 sample per world
    // unit. The 2049^2 source heightfield is upsampled 8x by the game
    // (Terrain_RebuildLandTilesRoadPatchesAndChunkCaches @d12e0); the bilinear
    // corners are H[col-1..col] in X but H[row..row+1] in Z, so the continuous
    // source coordinate carries a one-sample shift on the X axis only.
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

    // City-map shoreline carving (Terrain_GenerateIslandHeightfield @56560,
    // isMapAutoGenerated == 0): an L-shaped water shelf at 11.75 northwest of the
    // city, blended up with the cubic ramp (36 - 24e) * e^2 + 11.75 over four
    // source samples at the boundary.
    const auto min1 = [](float value) { return std::min(value, 1.0F); };
    const auto shore = [&](float edge) { return (36.0F - edge * 24.0F) * edge * edge + 11.75F; };
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

// 1-world-unit terrain height grid over the city region, indexed by integer
// world coordinates (the native land-sample grid minus its +4096 offset).
struct CityHeightfield {
    std::int32_t min_x{};
    std::int32_t min_z{};
    std::int32_t count_x{};
    std::int32_t count_z{};
    std::vector<float> heights;

    bool contains(std::int32_t x, std::int32_t z) const {
        return x >= min_x && z >= min_z && x < min_x + count_x && z < min_z + count_z;
    }

    std::size_t index(std::int32_t x, std::int32_t z) const {
        return static_cast<std::size_t>(z - min_z) * static_cast<std::size_t>(count_x) +
            static_cast<std::size_t>(x - min_x);
    }

    float get(std::int32_t x, std::int32_t z) const {
        return contains(x, z) ? heights[index(x, z)] : 0.0F;
    }

    void set(std::int32_t x, std::int32_t z, float height) {
        if (contains(x, z)) {
            heights[index(x, z)] = height;
        }
    }
};

// The native build patches a fixed, hardcoded list of intersection indices for
// every non-auto-generated map (Terrain_RebuildLandTilesRoadPatchesAndChunkCaches
// @d12e0). The same list applies regardless of map content.
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

// Port of Terrain_ApplyStreetIntersectionHeightPatch @d3340. Expects the
// post-placement expanded extents. Phase 1 writes a bilinear surface between
// the four corner intersections of the city block; phase 2 re-seeds road
// interior tiles (all four mapTopHeight corner stamps > 0) to the stamped road
// heights; phase 3 propagates seed positions outward for 16 passes with weight
// 1 - dist/16 (up to two direction-filtered candidates per sample); phase 4
// smoothstep-blends every reached sample toward its seed heights.
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

    // Neighbor walk: the south/west corner intersections may be one or two
    // streets away (the binary skips one intermediate intersection when the
    // direct neighbor lacks the required cross street).
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

    // Patch rect in world samples. Native quirk: the far corners subtract the
    // same +axis extents (field_0x2c / field_0x30) used on the near corners.
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

    // Phase 1: bilinear block surface between the four corner intersections.
    for (auto qi = 0; qi <= span_q; ++qi) {
        const auto fq = static_cast<float>(qi) / static_cast<float>(span_q);
        for (auto pi = 0; pi <= span_p; ++pi) {
            const auto fp = static_cast<float>(pi) / static_cast<float>(span_p);
            const auto height = (1.0F - fp) * (height_c * fq + height_a * (1.0F - fq)) +
                (height_d * fq + height_b * (1.0F - fq)) * fp;
            field.set(p0 + pi, q0 + qi, height);
        }
    }

    // Phase 2: seed road-interior tiles from the corner stamps.
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

    // Phase 3: 16-pass seed propagation. Each non-seed sample collects up to two
    // candidates (original seed coordinates) from its 8-neighborhood, keeping
    // candidates from distinct directions (dot > 0 test) and replacing weaker
    // parallel ones.
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
                            // -1 = no parallel entry seen, -2 = a parallel entry
                            // already holds an equal or better weight.
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
    CityCornerStampMap& stamps) {
    CityTileMap tile_map;
    for (const auto& street : graph.streets) {
        place_native_street_tiles(tile_map, stamps, graph, street);
    }
    for (const auto& intersection : graph.intersections) {
        place_native_intersection_tiles(tile_map, graph, intersection);
    }
    // District piece cells are no longer paved here: the v10 portal render path
    // emits each district piece's own catalog mesh (g_buildblockTemplateCatalog[
    // word & 0xffff]), so idx-0 plaza cells render their curb top-plate exactly as
    // the binary does — there is no separate asphalt overlay in the native editor.

    std::vector<CityTileCell> tiles;
    tiles.reserve(tile_map.size());
    for (const auto& [key, value] : tile_map) {
        // base_y is the bottom of the rendered solid: a full cell below the
        // surface for road cells, the 0.25-thick concrete plate for curbs.
        const auto base_y = value.kind == CityTileKind::Curb
            ? value.top_y - 0.25F
            : value.top_y - 4.0F;
        tiles.push_back({key.x, key.y, key.z, base_y, value.slope_x, value.slope_z, value.kind, value.street_index});
    }
    std::ranges::sort(tiles, [](const CityTileCell& left, const CityTileCell& right) {
        if (left.y != right.y) {
            return left.y < right.y;
        }
        if (left.z != right.z) {
            return left.z < right.z;
        }
        if (left.x != right.x) {
            return left.x < right.x;
        }
        return static_cast<int>(left.kind) < static_cast<int>(right.kind);
    });
    return tiles;
}

// Curb plate: the native curb block (Buildblock_InitTemplateMeshCatalog @10110,
// template ids 0..15) is a 1/16-cell-high concrete plate with lips on its open
// sides, placed one level above the road. Rendered as a thin slab whose top and
// bottom both ride the street grade.
void add_city_curb_slab(
    BuildingRenderBuffer& buffer,
    VertexIndexMap& vertex_indices,
    const CityTileCell& tile,
    std::uint32_t material,
    float unit_size) {
    constexpr auto thickness = 0.25F;
    const auto x0 = static_cast<float>(tile.x) * unit_size;
    const auto x1 = x0 + unit_size;
    const auto z0 = static_cast<float>(tile.z) * unit_size;
    const auto z1 = z0 + unit_size;
    const auto cx = x0 + unit_size * 0.5F;
    const auto cz = z0 + unit_size * 0.5F;
    auto base_at = [&](float x, float z) {
        return tile.base_y + tile.slope_x * ((x - cx) / unit_size) +
            tile.slope_z * ((z - cz) / unit_size);
    };
    auto corner = [&](float x, float z, float lift) {
        return Vec3{x, base_at(x, z) + lift, z};
    };
    const std::array<Vec2f, 4> uvs{{{0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F}, {0.0F, 1.0F}}};

    // Top plate.
    add_quad(buffer, vertex_indices,
        {corner(x0, z0, thickness), corner(x1, z0, thickness), corner(x1, z1, thickness), corner(x0, z1, thickness)},
        uvs, material);
    // Side lips.
    add_quad(buffer, vertex_indices,
        {corner(x0, z1, thickness), corner(x1, z1, thickness), corner(x1, z1, 0.0F), corner(x0, z1, 0.0F)},
        uvs, material);
    add_quad(buffer, vertex_indices,
        {corner(x1, z0, thickness), corner(x0, z0, thickness), corner(x0, z0, 0.0F), corner(x1, z0, 0.0F)},
        uvs, material);
    add_quad(buffer, vertex_indices,
        {corner(x0, z0, thickness), corner(x0, z1, thickness), corner(x0, z1, 0.0F), corner(x0, z0, 0.0F)},
        uvs, material);
    add_quad(buffer, vertex_indices,
        {corner(x1, z1, thickness), corner(x1, z0, thickness), corner(x1, z0, 0.0F), corner(x1, z1, 0.0F)},
        uvs, material);
}

BuildingRenderBuffer build_city_tile_render_buffer(const std::vector<CityTileCell>& tiles, float unit_size) {
    BuildingRenderBuffer buffer;
    buffer.unit_size = unit_size;
    if (tiles.empty()) {
        return buffer;
    }

    VertexIndexMap vertex_indices;
    std::vector<SbbCell> cells;
    cells.reserve(tiles.size());
    std::unordered_map<CityTileKey, std::size_t, CityTileKeyHash> tile_indices;
    tile_indices.reserve(tiles.size());
    for (std::size_t index = 0; index < tiles.size(); ++index) {
        const auto& tile = tiles[index];
        auto cell = native_city_block_solid_cell();
        cell.index = index;
        cell.occupied = true;
        cells.push_back(cell);
        tile_indices.emplace(CityTileKey{tile.x, tile.y, tile.z}, index);
    }

    auto road_cell_at = [&](std::int32_t x, std::int32_t y, std::int32_t z) -> const SbbCell* {
        const auto match = tile_indices.find(CityTileKey{x, y, z});
        if (match == tile_indices.end() || tiles[match->second].kind != CityTileKind::Road) {
            return nullptr;
        }
        return &cells[match->second];
    };

    const auto road_material = material_id_for_name(buffer, "city_road_asphalt");
    const auto curb_material = material_id_for_name(buffer, "city_curb_concrete");
    for (std::size_t index = 0; index < tiles.size(); ++index) {
        const auto& tile = tiles[index];
        if (tile.kind == CityTileKind::Curb) {
            add_city_curb_slab(buffer, vertex_indices, tile, curb_material, unit_size);
            continue;
        }

        const auto& cell = cells[index];
        const auto* west = road_cell_at(tile.x - 1, tile.y, tile.z);
        const auto* east = road_cell_at(tile.x + 1, tile.y, tile.z);
        const auto* north = road_cell_at(tile.x, tile.y, tile.z - 1);
        const auto* south = road_cell_at(tile.x, tile.y, tile.z + 1);
        const auto* up = road_cell_at(tile.x, tile.y + 1, tile.z);
        const auto cx = (static_cast<float>(tile.x) + 0.5F) * unit_size;
        const auto cy = tile.base_y + unit_size * 0.5F;
        const auto cz = (static_cast<float>(tile.z) + 0.5F) * unit_size;

        const std::array<SolidCellFaceCheck, 5> native_sides{{
            {BuildingFaceSide::South, south},
            {BuildingFaceSide::West, west},
            {BuildingFaceSide::North, north},
            {BuildingFaceSide::East, east},
            {BuildingFaceSide::Up, up},
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
            nullptr,
            [road_material](BuildingFaceSide) {
                return road_material;
            },
            CellTopSlope{tile.slope_x, tile.slope_z});
    }

    sort_triangles_into_native_material_runs(buffer);
    return buffer;
}

void add_city_road_markings(
    BuildingRenderBuffer& buffer,
    const std::vector<RenderRoad>& roads,
    const std::vector<CityTileCell>& tiles) {
    VertexIndexMap vertex_indices;
    const auto center_material = material_id_for_name(buffer, "city_road_line_yellow");
    const auto edge_material = material_id_for_name(buffer, "city_road_line_white");
    constexpr float line_width = 0.26F;
    constexpr float surface_offset = 0.06F;
    const std::array<Vec2f, 4> uvs{{{0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F}, {0.0F, 1.0F}}};
    std::unordered_map<std::uint64_t, const CityTileCell*> road_tiles;
    road_tiles.reserve(tiles.size());
    const auto tile_key = [](std::int32_t x, std::int32_t z) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32U) |
            static_cast<std::uint32_t>(z);
    };
    for (const auto& tile : tiles) {
        if (tile.kind == CityTileKind::Road) {
            road_tiles[tile_key(tile.x, tile.z)] = &tile;
        }
    }
    for (const auto& road : roads) {
        const auto dx = road.to.x-road.from.x;
        const auto dz = road.to.z-road.from.z;
        const auto length = std::sqrt(dx*dx+dz*dz);
        if (length <= buffer.unit_size) continue;
        const Vec3 direction{dx/length, 0.0F, dz/length};
        const Vec3 perpendicular{-direction.z, 0.0F, direction.x};
        const auto trim = std::min(road.width*0.5F, length*0.22F);
        const auto point = [&](float distance, float offset) {
            const auto t = distance/length;
            Vec3 result{
                road.from.x+direction.x*distance+perpendicular.x*offset,
                road.from.y+(road.to.y-road.from.y)*t,
                road.from.z+direction.z*distance+perpendicular.z*offset,
            };
            const auto cell_x = static_cast<std::int32_t>(std::floor(result.x/buffer.unit_size));
            const auto cell_z = static_cast<std::int32_t>(std::floor(result.z/buffer.unit_size));
            if (const auto tile = road_tiles.find(tile_key(cell_x, cell_z)); tile != road_tiles.end()) {
                const auto* surface = tile->second;
                const auto center_x = (static_cast<float>(surface->x)+0.5F)*buffer.unit_size;
                const auto center_z = (static_cast<float>(surface->z)+0.5F)*buffer.unit_size;
                result.y = surface->base_y+buffer.unit_size+
                    surface->slope_x*((result.x-center_x)/buffer.unit_size)+
                    surface->slope_z*((result.z-center_z)/buffer.unit_size);
            }
            result.y += surface_offset;
            return result;
        };
        const auto stripe = [&](float from, float to, float offset, float width, std::uint32_t material) {
            // Follow the per-cell tilted road surface. A single long quad cuts
            // through the game's slope tiles and loses markings on grades.
            for (auto segment_from = from; segment_from < to; segment_from += buffer.unit_size) {
                const auto segment_to = std::min(segment_from+buffer.unit_size, to);
                add_quad(buffer, vertex_indices, {
                    point(segment_from, offset-width*0.5F), point(segment_from, offset+width*0.5F),
                    point(segment_to, offset+width*0.5F), point(segment_to, offset-width*0.5F),
                }, uvs, material);
            }
        };
        const auto edge = std::max(buffer.unit_size*0.75F, road.width*0.5F-buffer.unit_size*0.22F);
        stripe(trim, length-trim, -edge, line_width, edge_material);
        stripe(trim, length-trim, edge, line_width, edge_material);
        const auto dash = buffer.unit_size*1.45F;
        const auto gap = buffer.unit_size*0.65F;
        for (auto distance = trim; distance < length-trim; distance += dash+gap) {
            stripe(distance, std::min(distance+dash, length-trim), 0.0F, line_width*1.35F, center_material);
        }
    }
    sort_triangles_into_native_material_runs(buffer);
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

    // The game removes the terrain surface under road/curb tiles entirely — the
    // drivable asphalt IS the ground there. Without this, the flat terrain plane
    // (sitting at the same patched level as the road tops) z-fights the road on
    // the lowest, flattest blocks. Build a covered-cell set so terrain quads
    // fully under tiles are skipped.
    // District piece cells are no longer paved (they render their own catalog
    // mesh), so the carve below covers only street/curb tiles. District solid
    // cells render their own cube, so carve terrain under them here too —
    // otherwise the terrain mesh buries the flat asphalt slabs.
    std::unordered_set<std::uint64_t> covered_cells;
    covered_cells.reserve(tile_cells.size());
    for (const auto& tile : tile_cells) {
        covered_cells.insert(CityCornerStampMap::key(tile.x, tile.z));
    }
    // Only SINGLETON solid cells render (build_native_portal_render_buffer culls
    // stacked foundation walls), so only carve terrain under singletons. Carving
    // under the culled stacks would punch holes in the ground with nothing behind
    // them. Match the render's cull: a solid with another solid directly above or
    // below is part of a stack and is neither drawn nor carved.
    std::unordered_set<std::uint64_t> solid_cell_coords;
    const auto solid_key3 = [](std::int32_t x, std::int32_t y, std::int32_t z) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 42) |
               (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y) & 0x1fffffU) << 21) |
               static_cast<std::uint64_t>(static_cast<std::uint32_t>(z) & 0x1fffffU);
    };
    for (const auto& portal : map.portals) {
        const auto& voxels = portal.voxel_blocks();
        const auto block_count = std::min<std::size_t>(voxels.size(), 0x200);
        for (std::size_t voxel_index = 0; voxel_index < block_count; ++voxel_index) {
            if ((voxels[voxel_index] & 0xe0000000U) != 0x40000000U) {
                continue;
            }
            const auto sx = portal.fields[1] * 8 + static_cast<std::int32_t>(voxel_index & 7U);
            const auto sz = portal.fields[3] * 8 + static_cast<std::int32_t>((voxel_index >> 3U) & 7U);
            const auto sy = portal.fields[2] * 8 + static_cast<std::int32_t>((voxel_index >> 6U) & 7U);
            solid_cell_coords.insert(solid_key3(sx, sy, sz));
        }
    }
    for (const auto& portal : map.portals) {
        const auto& voxels = portal.voxel_blocks();
        const auto block_count = std::min<std::size_t>(voxels.size(), 0x200);
        for (std::size_t voxel_index = 0; voxel_index < block_count; ++voxel_index) {
            if ((voxels[voxel_index] & 0xe0000000U) != 0x40000000U) {
                continue;
            }
            const auto gx = portal.fields[1] * 8 + static_cast<std::int32_t>(voxel_index & 7U);
            const auto gz = portal.fields[3] * 8 + static_cast<std::int32_t>((voxel_index >> 3U) & 7U);
            const auto gy = portal.fields[2] * 8 + static_cast<std::int32_t>((voxel_index >> 6U) & 7U);
            if (solid_cell_coords.contains(solid_key3(gx, gy + 1, gz)) ||
                solid_cell_coords.contains(solid_key3(gx, gy - 1, gz))) {
                continue;
            }
            covered_cells.insert(CityCornerStampMap::key(gx, gz));
        }
    }
    auto cell_covered = [&](std::int32_t gx, std::int32_t gz) {
        return covered_cells.contains(CityCornerStampMap::key(gx, gz));
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
    // Cover buildings too, not just intersections — otherwise buildings placed
    // outside the intersection bounding box have no ground under them (holes).
    for (const auto& building : map.buildings) {
        min_x = std::min(min_x, building.x);
        max_x = std::max(max_x, building.x);
        min_z = std::min(min_z, building.z);
        max_z = std::max(max_z, building.z);
    }

    // Generous padding so building footprints + road curbs near the edge stay
    // on solid ground.
    constexpr auto terrain_padding_tiles = 24;
    CityTerrain terrain{
        static_cast<float>((min_x - terrain_padding_tiles)) * unit_size,
        static_cast<float>((max_x + terrain_padding_tiles)) * unit_size,
        static_cast<float>((min_z - terrain_padding_tiles)) * unit_size,
        static_cast<float>((max_z + terrain_padding_tiles)) * unit_size,
        0,
        0,
        // Mesh/query resolution. The native heightfield below is computed at the
        // native 1 world-unit sample spacing; the emitted mesh samples it every
        // 2 units to keep the vertex count tractable.
#ifdef __EMSCRIPTEN__
        // The desktop path keeps a dense one-unit patched field. Building that
        // field synchronously in WebAssembly can stall the editor for minutes
        // when a map spans thousands of units. The browser keeps the complete
        // 0x4000 landscape but samples its editable/query surface at the same
        // four-unit cell scale used by city tools.
        4.0F,
#else
        2.0F,
#endif
        {},
        {},
    };

    // Native 1-unit heightfield: base island field, then the hardcoded
    // intersection block patches.
#ifndef __EMSCRIPTEN__
    CityHeightfield field;
    field.min_x = static_cast<std::int32_t>(terrain.min_x);
    field.min_z = static_cast<std::int32_t>(terrain.min_z);
    field.count_x = static_cast<std::int32_t>(terrain.max_x - terrain.min_x) + 1;
    field.count_z = static_cast<std::int32_t>(terrain.max_z - terrain.min_z) + 1;
    field.heights.resize(static_cast<std::size_t>(field.count_x) * static_cast<std::size_t>(field.count_z));
    for (auto z = 0; z < field.count_z; ++z) {
        for (auto x = 0; x < field.count_x; ++x) {
            field.heights[static_cast<std::size_t>(z) * static_cast<std::size_t>(field.count_x) +
                static_cast<std::size_t>(x)] = native_like_city_terrain_height(
                    static_cast<float>(field.min_x + x), static_cast<float>(field.min_z + z));
        }
    }
    for (const auto patch_index : native_city_terrain_patch_indices) {
        apply_native_city_terrain_patch(field, expanded_graph, stamps, patch_index);
    }
#endif

    const auto terrain_height = [&](float world_x, float world_z) {
#ifdef __EMSCRIPTEN__
        return native_like_city_terrain_height(world_x, world_z);
#else
        if (world_x >= terrain.min_x && world_x <= terrain.max_x &&
            world_z >= terrain.min_z && world_z <= terrain.max_z) {
            const auto sample_x = std::clamp(
                static_cast<std::int32_t>(world_x), field.min_x, field.min_x + field.count_x - 1);
            const auto sample_z = std::clamp(
                static_cast<std::int32_t>(world_z), field.min_z, field.min_z + field.count_z - 1);
            return field.get(sample_x, sample_z);
        }
        return native_like_city_terrain_height(world_x, world_z);
#endif
    };

    terrain.sample_count_x = std::max(2, static_cast<std::int32_t>(std::ceil((terrain.max_x - terrain.min_x) / terrain.sample_spacing)) + 1);
    terrain.sample_count_z = std::max(2, static_cast<std::int32_t>(std::ceil((terrain.max_z - terrain.min_z) / terrain.sample_spacing)) + 1);
    terrain.heights.reserve(static_cast<std::size_t>(terrain.sample_count_x * terrain.sample_count_z));
    terrain.mesh.unit_size = unit_size;
    terrain.mesh.material_names.push_back("city_terrain_grass");

    for (auto z = 0; z < terrain.sample_count_z; ++z) {
        for (auto x = 0; x < terrain.sample_count_x; ++x) {
            const auto world_x = std::min(terrain.max_x, terrain.min_x + static_cast<float>(x) * terrain.sample_spacing);
            const auto world_z = std::min(terrain.max_z, terrain.min_z + static_cast<float>(z) * terrain.sample_spacing);
            terrain.heights.push_back(terrain_height(world_x, world_z));
            // Keep the patched local height samples for picking and object
            // placement. The render mesh is generated below in the native
            // 128-unit landscape chunks.
        }
    }

    // Native landscape dimensions proven in
    // Terrain_RebuildLandTilesRoadPatchesAndChunkCaches @ d12e0:
    // 2049 source samples, expanded 8x to a 0x4000-square world. Rendering
    // splits that field into 128-unit subchunks and uses steps 1..64. Use the
    // city as the stable editor focus (the game uses the camera) so the full
    // island is always present without rebuilding terrain on every camera move.
    constexpr float landscape_min = -4096.0F;
    constexpr float landscape_max = landscape_min + 16384.0F;
    constexpr std::int32_t chunk_size = 128;
    const auto focus_x = (terrain.min_x + terrain.max_x) * 0.5F;
    const auto focus_z = (terrain.min_z + terrain.max_z) * 0.5F;
    auto lod_step = [&](float chunk_x, float chunk_z) {
        const auto dx = chunk_x + 64.0F - focus_x;
        const auto dz = chunk_z + 64.0F - focus_z;
        const auto distance = std::sqrt(dx * dx + dz * dz);
        // The native game streams only camera-visible subchunks. This editor
        // mesh is persistent, so native near-LOD radii would expand to about
        // five million uploaded vertices and starve UI input. Preserve the full
        // 0x4000 world but keep high detail local to the authored city.
        if (distance < 256.0F) return 4;
        if (distance < 512.0F) return 8;
        if (distance < 1024.0F) return 16;
        if (distance < 2048.0F) return 32;
        return 64;
    };
    auto render_height = terrain_height;

    for (auto chunk_z = landscape_min; chunk_z < landscape_max; chunk_z += static_cast<float>(chunk_size)) {
        for (auto chunk_x = landscape_min; chunk_x < landscape_max; chunk_x += static_cast<float>(chunk_size)) {
            const auto step = lod_step(chunk_x, chunk_z);
            const auto side = chunk_size / step + 1;
            const auto first_vertex = static_cast<std::uint32_t>(terrain.mesh.vertices.size());
            for (auto z = 0; z < side; ++z) {
                for (auto x = 0; x < side; ++x) {
                    const auto world_x = chunk_x + static_cast<float>(x * step);
                    const auto world_z = chunk_z + static_cast<float>(z * step);
                    terrain.mesh.vertices.push_back({
                        {world_x, render_height(world_x, world_z) - city_terrain_mesh_y_offset, world_z},
                        world_x/32.0F,
                        1.0F-world_z/32.0F});
                }
            }
            for (auto z = 0; z < side - 1; ++z) {
                for (auto x = 0; x < side - 1; ++x) {
            // Skip terrain quads that sit under a road/curb tile — the tile's top
            // surface is the ground there, and the coincident terrain plane
            // z-fights it. The quad's footprint (2 units) fits inside one 4-unit
            // tile cell; test the four corners and skip only when all are covered
            // so the cut follows the tile footprint without leaving holes.
            const auto quad_min_x = chunk_x + static_cast<float>(x * step);
            const auto quad_min_z = chunk_z + static_cast<float>(z * step);
            const auto quad_max_x = quad_min_x + static_cast<float>(step);
            const auto quad_max_z = quad_min_z + static_cast<float>(step);
            const auto cell_of = [&](float wx, float wz) {
                return cell_covered(
                    static_cast<std::int32_t>(std::floor(wx / unit_size)),
                    static_cast<std::int32_t>(std::floor(wz / unit_size)));
            };
            // Sample just inside each corner so a quad flush against a tile edge
            // resolves to the cell it actually overlaps.
            constexpr auto eps = 0.01F;
            if (cell_of(quad_min_x + eps, quad_min_z + eps) &&
                cell_of(quad_max_x - eps, quad_min_z + eps) &&
                cell_of(quad_min_x + eps, quad_max_z - eps) &&
                cell_of(quad_max_x - eps, quad_max_z - eps)) {
                continue;
            }
            const auto i0 = first_vertex + static_cast<std::uint32_t>(z * side + x);
            const auto i1 = i0 + 1U;
            const auto i3 = first_vertex + static_cast<std::uint32_t>((z + 1) * side + x);
            const auto i2 = i3 + 1U;
            terrain.mesh.triangles.push_back({{i0, i1, i2}, 0U, false});
            terrain.mesh.triangles.push_back({{i0, i2, i3}, 0U, false});
                }
            }
        }
    }

    // One material run so the terrain mesh can be uploaded as a GPU chunk (it is
    // a single grass material) instead of drawn triangle-by-triangle.
    sort_triangles_into_native_material_runs(terrain.mesh);
    return terrain;
}

// Traffic light corner objects. Placement: MapConnector_PlaceIntersectionPerimeterTiles
// @20130 stores corner codes only when more than two streets connect, one per
// present neighbor, in the diagonal corner tile outside the footprint, one level
// above the road. Rendering: _area_interior_buildsectormodel @14820 places the
// type-1 street object at the corner cell center plus the per-type local offset
// (0, 3.25, -1) rotated by the code's bits 10-11 (N=0, E=1, S=2, W=3 quarter
// turns) — i.e. the post stands on the curb plate, shifted one unit toward the
// street its arm crosses; the model's arm extends along local -z.
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

        // Pole ground point: corner cell base (+0.25) plus the model offset
        // (origin y 3.25, pole half-height 3) = 0.5 above the cell level — the
        // top of the curb plate.
        const auto base_y = native_road_surface_y(intersection.level) + 0.25F;
        auto add_pole = [&](std::int32_t tile_x, std::int32_t tile_z, Vec3 arm) {
            const auto center_x = (static_cast<float>(tile_x) + 0.5F) * unit_size;
            const auto center_z = (static_cast<float>(tile_z) + 0.5F) * unit_size;
            poles.push_back({
                {center_x + arm.x, base_y, center_z + arm.z},
                arm,
            });
        };
        if (intersection.street_north >= 0) {
            add_pole(intersection.gx + intersection.ext_x_pos, intersection.gz + intersection.ext_z_pos,
                {0.0F, 0.0F, -1.0F});
        }
        if (intersection.street_south >= 0) {
            add_pole(intersection.gx - intersection.ext_x_pos - 1, intersection.gz - intersection.ext_z_pos - 1,
                {0.0F, 0.0F, 1.0F});
        }
        if (intersection.street_east >= 0) {
            add_pole(intersection.gx - intersection.ext_x_pos - 1, intersection.gz + intersection.ext_z_pos,
                {1.0F, 0.0F, 0.0F});
        }
        if (intersection.street_west >= 0) {
            add_pole(intersection.gx + intersection.ext_x_pos, intersection.gz - intersection.ext_z_pos - 1,
                {-1.0F, 0.0F, 0.0F});
        }
    }
    return poles;
}

} // namespace

// -----------------------------------------------------------------------------
// Building Scene and Render Buffers
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
                const auto side_codes = native_edge_side_codes(cell, east);
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

BuildingRenderBuffer build_native_building_render_buffer(
    const SbbBuilding& building,
    const std::vector<NamedBlockMesh>& block_meshes,
    const std::vector<NamedBuildblockTemplate>& buildblock_templates) {
    MAPED_PROFILE_FUNCTION();
    BuildingRenderBuffer buffer;
    const auto unit_size = buffer.unit_size;
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

    auto cell_origin = [&](const SbbCell& cell) {
        return Vec3{
            (static_cast<float>(cell.x) - static_cast<float>(building.anchor_x)) * unit_size,
            (static_cast<float>(cell.y) - static_cast<float>(building.anchor_y)) * unit_size,
            (static_cast<float>(cell.z) - static_cast<float>(building.anchor_z)) * unit_size,
        };
    };

    for (const auto& cell : building.cells) {
        if (!cell.occupied) {
            continue;
        }

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
                    [&cell](BuildingRenderBuffer&, const SblBlockFace&, const NativeRotatedBlockFace& rotated_face) {
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
                    [&cell](BuildingRenderBuffer&, const SblBlockFace&, const NativeRotatedBlockFace& rotated_face) {
                        return material_for_piece_face(cell, rotated_face.material, true);
                    });
            }
        }

        const auto cx = (static_cast<float>(cell.x) - static_cast<float>(building.anchor_x) + 0.5F) * unit_size;
        const auto cy = (static_cast<float>(cell.y) - static_cast<float>(building.anchor_y) + 0.5F) * unit_size;
        const auto cz = (static_cast<float>(cell.z) - static_cast<float>(building.anchor_z) + 0.5F) * unit_size;

        if (!cell_has_exterior_surface(&cell)) {
            continue;
        }

        const auto* west = find_cell_at(cell_index, cell.x - 1U, cell.y, cell.z);
        const auto* east = find_cell_at(cell_index, cell.x + 1U, cell.y, cell.z);
        const auto* north = find_cell_at(cell_index, cell.x, cell.y, cell.z - 1U);
        const auto* south = find_cell_at(cell_index, cell.x, cell.y, cell.z + 1U);
        const auto* up = find_cell_at(cell_index, cell.x, cell.y + 1U, cell.z);
        const auto* template_file = cell.cell_type() < resolved_buildblock_templates.size()
            ? resolved_buildblock_templates[cell.cell_type()]
            : (buildblock_templates.empty() ? nullptr : &buildblock_templates.front().template_file);

        // BuildBuildingBlockRenderBuffers_1400799d0.c:144-146 iterates native
        // faces 1..5 only. Down/face 0 is intentionally absent here.
        const std::array<SolidCellFaceCheck, 5> native_sides{{
            {BuildingFaceSide::South, south},
            {BuildingFaceSide::West, west},
            {BuildingFaceSide::North, north},
            {BuildingFaceSide::East, east},
            {BuildingFaceSide::Up, up},
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
            template_file,
            [&cell](BuildingFaceSide side) {
                return material_for_cell_side(cell, side);
            });
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
    const std::vector<NamedBuildblockTemplate>& buildblock_templates) {
    // City placement uses the runtime building buffer, not the editor's
    // floor-tool preview. The preview interprets placed_flag bits as optional
    // floor/ceiling guides; using it in the city dropped exposed face 5 on
    // otherwise solid top cells and made roofs or top floors disappear.
    return build_native_building_render_buffer(
        building,
        block_meshes,
        buildblock_templates);
}

BuildingRenderBuffer build_native_portal_render_buffer(
    const SbcMap& map,
    const std::vector<NamedBlockMesh>& block_meshes,
    std::optional<std::size_t> selected_portal,
    [[maybe_unused]] const std::vector<NamedBuildblockTemplate>& buildblock_templates,
    const std::vector<std::string>& texture_names) {
    MAPED_PROFILE_FUNCTION();
    BuildingRenderBuffer buffer;
    VertexIndexMap vertex_indices;

    auto layer_slot_material = [&](const SbcCityBlock& city_block, std::size_t layer_index, std::uint32_t slot) {
        return material_id_for_name(buffer, city_block_layer_slot_name(city_block, layer_index, slot));
    };

    // v10 district SOLID (0x40000000) cells are buried world fill. The game never
    // draws the vertical STACKS of them (foundation walls, up to 5 cells deep at
    // Y=6..10) — they sit below the ground and the terrain hides them. The editor
    // drew every cell as an opaque cube, so those buried stacks showed up as black
    // walls in 3D (and were absent from the 2D preview — the tell). A SINGLE solid
    // cell standing alone in its column is a real flat asphalt slab (the plaza /
    // road-end pieces) and still renders. So: cull a solid that has another solid
    // directly above or below it (i.e. it belongs to a stack); keep singletons.
    // Precompute solid coordinates so the render loop can test vertical neighbors.
    std::unordered_set<std::uint64_t> solid_cell_coords;
    // Any occupied (non-empty) district cell, used to detect a solid buried under a
    // piece (e.g. the Y=11 solid support directly below a Y=12 railing piece). The
    // game culls the buried solid's faces against the piece/sidewalk above it; the
    // editor must skip it too or it z-fights the sidewalk tile.
    std::unordered_set<std::uint64_t> occupied_cell_coords;
    const auto solid_key = [](std::int32_t x, std::int32_t y, std::int32_t z) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 42) |
               (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y) & 0x1fffffU) << 21) |
               static_cast<std::uint64_t>(static_cast<std::uint32_t>(z) & 0x1fffffU);
    };
    for (const auto& portal : map.portals) {
        const auto& portal_voxels = portal.voxel_blocks();
        const auto portal_count = std::min<std::size_t>(portal_voxels.size(), 0x200);
        for (std::size_t vi = 0; vi < portal_count; ++vi) {
            const auto cell_word = portal_voxels[vi];
            if (cell_word == 0U) {
                continue;
            }
            const auto sx = portal.fields[1] * 8 + static_cast<std::int32_t>(vi & 7U);
            const auto sz = portal.fields[3] * 8 + static_cast<std::int32_t>((vi >> 3U) & 7U);
            const auto sy = portal.fields[2] * 8 + static_cast<std::int32_t>((vi >> 6U) & 7U);
            occupied_cell_coords.insert(solid_key(sx, sy, sz));
            if ((cell_word & 0xe0000000U) == 0x40000000U) {
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
            const auto city_block_index = static_cast<std::size_t>(word & 0xfffU);

            auto layer_index = static_cast<std::size_t>((word >> 16U) & 0x3fU);
            layer_index = layer_index == 0U ? 0U : layer_index - 1U;
            const auto rotation = static_cast<std::int32_t>((word >> 24U) & 3U);
            const auto local_x = static_cast<std::int32_t>(voxel_index & 7U);
            const auto local_z = static_cast<std::int32_t>((voxel_index >> 3U) & 7U);
            const auto local_y = static_cast<std::int32_t>((voxel_index >> 6U) & 7U);
            const auto origin = Vec3{
                static_cast<float>(portal.fields[1] * 8 + local_x) * buffer.unit_size,
                static_cast<float>(portal.fields[2] * 8 + local_y) * buffer.unit_size,
                static_cast<float>(portal.fields[3] * 8 + local_z) * buffer.unit_size,
            };

            // v10 maps (e.g. round) have no city_blocks. The native loader applies
            // these legacy district cells in World_LoadMapAndInitializeTerrain's
            // legacy loop, feeding the raw word into World_SetTileBlockAndResolveNeighborLinks:
            //   - class 0x40000000 = a solid world cell (the flat asphalt road-end
            //     slabs); per-face material comes from the cell's 8-short texture grid
            //     (portal.textures, 8 per voxel), short 0 = first loaded texture.
            //   - class 0x80000000 = a buildblock-template instance indexed by
            //     (word & 0xffff) into the global buildblock catalog (loose
            //     buildblock/*.sbl, sorted, capped 64). Index 0 = _null and any
            //     out-of-range index render nothing, exactly as the native instancing
            //     loop does for empty catalog slots.
            if (map.city_blocks.empty()) {
                const auto cell_class = word & 0xe0000000U;
                const auto grid_material = [&](std::size_t slot) {
                    const auto grid_base = voxel_index * 8U + std::min<std::size_t>(slot, 7U);
                    auto tex_idx = grid_base < portal.textures.size()
                        ? static_cast<std::size_t>(portal.textures[grid_base])
                        : 0U;
                    // An unset grid slot stores 0xffff; BuildCellRenderGeometryAndAdjacency
                    // @0xc4c0 replaces such entries with the default texture rather than
                    // leaving them unbound. Procedural pieces (railings/curbs) carry no
                    // per-face texture, so every slot is unset -> fall back to texture 0
                    // instead of an empty name (which renders black).
                    if (tex_idx >= texture_names.size()) {
                        tex_idx = 0U;
                    }
                    const auto name = tex_idx < texture_names.size() ? texture_names[tex_idx] : std::string{};
                    return material_id_for_name(buffer, name);
                };

                if (cell_class == 0x40000000U) {
                    // Cull buried solids; keep singleton slabs (flat plaza/road-end
                    // pieces). A solid is buried when:
                    //  - another solid sits directly above OR below it (a vertical
                    //    foundation-wall stack the game never draws), or
                    //  - ANY district cell sits directly above it (e.g. the Y=11
                    //    solid support under a Y=12 railing piece) — the game hides it
                    //    behind the piece + sidewalk, so drawing it z-fights the tile.
                    const auto cell_x = portal.fields[1] * 8 + local_x;
                    const auto cell_y = portal.fields[2] * 8 + local_y;
                    const auto cell_z = portal.fields[3] * 8 + local_z;
                    if (solid_cell_coords.contains(solid_key(cell_x, cell_y + 1, cell_z)) ||
                        solid_cell_coords.contains(solid_key(cell_x, cell_y - 1, cell_z)) ||
                        occupied_cell_coords.contains(solid_key(cell_x, cell_y + 1, cell_z))) {
                        continue;
                    }
                    add_native_city_block_solid_cell(
                        buffer,
                        vertex_indices,
                        origin,
                        [&](BuildingFaceSide side) {
                            return grid_material(material_slot_for_cell_side(side));
                        });
                    continue;
                }

                if (cell_class == 0x80000000U) {
                    const auto catalog_index = static_cast<std::uint32_t>(word & 0xffffU);
                    const auto* mesh = resolve_district_procedural_mesh(catalog_index);
                    if (mesh == nullptr) {
                        continue;
                    }
                    // BuildCellRenderGeometryAndAdjacency @0xc4c0 copies the catalog
                    // mesh's vertices with translation only — it does NOT rotate the
                    // piece geometry by the cell's (word>>24)&3 rotation (that field
                    // only rotates the per-quad UV/material slot at render time, plus
                    // the four pre-rotated catalog ids the map may reference directly).
                    // So render the catalog mesh as authored (rotation 0); the
                    // material slot still comes from the cell's 8-short texture grid.
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

std::string district_buildblock_name_for_index(
    const std::vector<NamedBuildblockTemplate>& buildblock_templates,
    std::uint32_t catalog_index) {
    const auto ordered = district_buildblock_catalog_order(buildblock_templates);
    if (catalog_index >= ordered.size()) {
        return {};
    }
    return ordered[catalog_index]->name;
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

std::optional<Vec3> ray_terrain_hit(const CityTerrain& terrain, const Vec3& ray_origin, const Vec3& ray_dir) {
    if (terrain.heights.empty() || terrain.sample_count_x <= 0 || terrain.sample_count_z <= 0 ||
        terrain.sample_spacing <= 0.0F) {
        return ray_ground_hit(ray_origin, ray_dir);
    }

    // March the ray in fixed steps and detect the span where it crosses from above
    // the heightfield to below it, then bisect that span. signed_gap(t) > 0 means
    // the ray point is above the terrain surface at that x/z. The terrain is a
    // height grid, so a downward-looking ray crosses it at most once over the
    // relevant span; a coarse march plus a short bisection lands within a few
    // millimetres without needing per-triangle intersection.
    const auto signed_gap = [&](float t) {
        const auto p = add_scaled(ray_origin, ray_dir, t);
        return p.y - terrain_height_at(terrain, p.x, p.z);
    };

    const auto span = (terrain.max_x - terrain.min_x) + (terrain.max_z - terrain.min_z);
    const auto max_t = span > 0.0F ? span * 2.0F + 4096.0F : 8192.0F;
    const auto step = std::max(terrain.sample_spacing * 0.5F, 1.0F);

    float prev_t = 0.0F;
    float prev_gap = signed_gap(prev_t);
    // Ray starting under the surface: nothing meaningful to snap to.
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
                (static_cast<float>(portal.fields[1] * 8 + local_x) + 0.5F) * unit_size,
                (static_cast<float>(portal.fields[2] * 8 + local_y) + 0.5F) * unit_size,
                (static_cast<float>(portal.fields[3] * 8 + local_z) + 0.5F) * unit_size,
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

std::optional<std::size_t> pick_city_road(
    const SbcMap& map,
    const MapScene& scene,
    const Vec3& ray_origin,
    const Vec3& ray_dir) {
    // Roads sit on the terrain surface, often well above y=0. Hit-testing against
    // the y=0 plane lands the ground point at the wrong X/Z (the click misses the
    // visible road), so raycast the actual terrain surface like the cursor does.
    const auto hit = scene.terrain.has_value()
        ? ray_terrain_hit(*scene.terrain, ray_origin, ray_dir)
        : ray_ground_hit(ray_origin, ray_dir);
    if (!hit.has_value()) {
        return std::nullopt;
    }

    // Distance from the surface hit to each road segment; the nearest segment
    // within its drawn half-width wins. scene.roads[i].source back-references the
    // SBC street, resolved to an index into map.streets.
    std::optional<std::size_t> nearest;
    float nearest_dist = std::numeric_limits<float>::max();
    for (const auto& road : scene.roads) {
        const auto ax = road.from.x;
        const auto az = road.from.z;
        const auto bx = road.to.x;
        const auto bz = road.to.z;
        const auto dx = bx - ax;
        const auto dz = bz - az;
        const auto seg_len_sq = dx * dx + dz * dz;
        auto u = seg_len_sq > 0.000001F
            ? ((hit->x - ax) * dx + (hit->z - az) * dz) / seg_len_sq
            : 0.0F;
        u = std::clamp(u, 0.0F, 1.0F);
        const auto cx = ax + dx * u;
        const auto cz = az + dz * u;
        const auto dist = std::sqrt((hit->x - cx) * (hit->x - cx) + (hit->z - cz) * (hit->z - cz));
        const auto half_width = std::max(road.width, scene.unit_size) * 0.5F;
        if (dist > half_width || dist >= nearest_dist) {
            continue;
        }

        std::optional<std::size_t> street_index;
        for (std::size_t i = 0; i < map.streets.size(); ++i) {
            if (&map.streets[i] == road.source) {
                street_index = i;
                break;
            }
        }
        if (!street_index.has_value()) {
            continue;
        }
        nearest = *street_index;
        nearest_dist = dist;
    }

    return nearest;
}

std::optional<std::size_t> pick_city_intersection(
    const SbcMap& map,
    const MapScene& scene,
    const Vec3& ray_origin,
    const Vec3& ray_dir) {
    constexpr float marker_size = 2.0F;
    // Pad the pick target a little beyond the drawn marker so the small node cube
    // is easy to grab, but not so much that it swallows the road running through
    // it (a full-cell box made roads unselectable near their endpoints). The
    // unified picker (pick_city_road_or_node) breaks node/road ties by camera
    // distance, so this only needs to be comfortably clickable, not greedy.
    const auto half = marker_size * 0.85F;

    std::optional<std::size_t> nearest;
    auto nearest_t = std::numeric_limits<float>::max();
    for (const auto& intersection : scene.intersections) {
        std::optional<std::size_t> index;
        for (std::size_t i = 0; i < map.intersections.size(); ++i) {
            if (&map.intersections[i] == intersection.source) {
                index = i;
                break;
            }
        }
        if (!index.has_value()) {
            continue;
        }

        const Vec3 center{
            intersection.position.x,
            static_cast<float>(intersection.source->y) * scene.unit_size,
            intersection.position.z,
        };
        const auto t = ray_aabb_intersection_t(
            ray_origin,
            ray_dir,
            {center.x - half, center.y - half, center.z - half},
            {center.x + half, center.y + half, center.z + half});
        if (!t.has_value() || *t >= nearest_t) {
            continue;
        }

        nearest = *index;
        nearest_t = *t;
    }

    return nearest;
}

std::optional<CityRoadPick> pick_city_road_or_node(
    const SbcMap& map,
    const MapScene& scene,
    const Vec3& ray_origin,
    const Vec3& ray_dir) {
    // Node candidate: distance along the ray to the nearest node marker box.
    constexpr float marker_size = 2.0F;
    const auto node_half = marker_size * 0.85F;
    std::optional<std::size_t> node_index;
    float node_t = std::numeric_limits<float>::max();
    for (const auto& intersection : scene.intersections) {
        std::optional<std::size_t> index;
        for (std::size_t i = 0; i < map.intersections.size(); ++i) {
            if (&map.intersections[i] == intersection.source) {
                index = i;
                break;
            }
        }
        if (!index.has_value()) {
            continue;
        }
        const Vec3 center{
            intersection.position.x,
            static_cast<float>(intersection.source->y) * scene.unit_size,
            intersection.position.z,
        };
        const auto t = ray_aabb_intersection_t(
            ray_origin, ray_dir,
            {center.x - node_half, center.y - node_half, center.z - node_half},
            {center.x + node_half, center.y + node_half, center.z + node_half});
        if (t.has_value() && *t < node_t) {
            node_t = *t;
            node_index = *index;
        }
    }

    // Road candidate: where the ray meets the terrain surface, which road's drawn
    // footprint (segment within its half-width) that point lands in. The road t is
    // the distance from the ray origin to that surface hit, so it is directly
    // comparable to the node t above.
    // Pick against the actual rendered road tiles (the asphalt you see), not the
    // abstract centerline. Each road tile is a unit cube; the nearest one the ray
    // enters resolves to its owning street via tile.street_index. Intersection
    // tiles (street_index < 0) are ignored so they fall through to node picking.
    std::optional<std::size_t> road_index;
    float road_t = std::numeric_limits<float>::max();
    const auto unit = scene.unit_size;
    for (const auto& tile : scene.city_tiles) {
        if (tile.kind != CityTileKind::Road || tile.street_index < 0 ||
            static_cast<std::size_t>(tile.street_index) >= map.streets.size()) {
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

    // Whichever the ray reaches first wins. A small bias keeps a direct node click
    // winning when the node box and the road overlap (the node sits on the road).
    if (node_index.has_value() && (!road_index.has_value() || node_t <= road_t + scene.unit_size * 0.25F)) {
        return CityRoadPick{CityRoadPick::Kind::Node, *node_index};
    }
    if (road_index.has_value()) {
        return CityRoadPick{CityRoadPick::Kind::Road, *road_index};
    }
    return std::nullopt;
}

// -----------------------------------------------------------------------------
// City Scene Builders
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
    // StreetPieceTool_CanvasRender_14006c400 applies only this rotation grid
    // correction before RenderBuildingTemplatePreview. LoadCity_SBC stores an
    // extent-center triplet for bounds/placement bookkeeping, but the rendered
    // template mesh is already recentered by BuildBuildingBlockRenderBuffers.
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
        // All native world cells sit 0.25 above their level (cell at level L
        // spans L*4+0.25 .. L*4+4.25); without the lift, building bottoms are
        // coplanar with their stamped terrain plots and z-fight.
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

std::uint64_t city_scene_fingerprint(const SbcMap& map) {
    // FNV-1a over the fields the city scene is built from: intersections (terrain
    // + tile grid + poles), streets (road tiles), and building placements. Anything
    // that changes the rendered city changes one of these, so a mismatch is the
    // signal to rebuild the cached MapScene.
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::int64_t value) {
        auto bits = static_cast<std::uint64_t>(value);
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= bits & 0xFFU;
            hash *= 1099511628211ULL;
            bits >>= 8;
        }
    };
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
        mix(building.x);
        mix(building.y);
        mix(building.z);
        mix(building.rotation);
    }
    // Piece (portal/district) cells stamp the terrain corner-height grid, so
    // piece edits must rebuild the scene too.
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

std::uint64_t city_terrain_fingerprint(const SbcMap& map) {
    // Subset of city_scene_fingerprint covering only the inputs that change the
    // terrain heightfield mesh: intersection positions (corner heights), building
    // footprints, and piece/portal cells (footprint stamping). Streets/road widths
    // are excluded, so road and street edits skip the expensive terrain re-upload.
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::int64_t value) {
        auto bits = static_cast<std::uint64_t>(value);
        for (int byte = 0; byte < 8; ++byte) {
            hash ^= bits & 0xFFU;
            hash *= 1099511628211ULL;
            bits >>= 8;
        }
    };
    mix(static_cast<std::int64_t>(map.intersections.size()));
    for (const auto& it : map.intersections) {
        mix(it.x);
        mix(it.y);
        mix(it.z);
    }
    mix(static_cast<std::int64_t>(map.buildings.size()));
    for (const auto& building : map.buildings) {
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
    return build_map_scene(map, {});
}

MapScene build_map_scene(const SbcMap& map, const std::vector<const SbbBuilding*>& placement_buildings) {
    MAPED_PROFILE_FUNCTION();
    MapScene scene;
    // Native build order (World_LoadMapAndInitializeTerrain @56980): street
    // graph, then street/intersection tiles, district/piece cells, and building
    // footprints (all of which stamp the corner-height grid), then extent
    // expansion, then the terrain rebuild with its patches.
    auto graph = build_native_city_graph(map);
    CityCornerStampMap corner_stamps;
    scene.city_tiles = build_city_tile_cells(graph, corner_stamps);
    stamp_city_portal_cells(corner_stamps, map);
    stamp_city_building_footprints(corner_stamps, map, placement_buildings);
    scene.traffic_light_poles = build_city_traffic_light_poles(graph, scene.unit_size);
    expand_native_city_extents(graph);
    scene.terrain = build_city_terrain(map, graph, corner_stamps, scene.city_tiles, scene.unit_size);
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
            &source,
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
    scene.city_tile_buffer = build_city_tile_render_buffer(scene.city_tiles, scene.unit_size);
    add_city_road_markings(scene.city_tile_buffer, scene.roads, scene.city_tiles);

    for (std::size_t intersection_index = 0; intersection_index < map.intersections.size(); ++intersection_index) {
        const auto& intersection = map.intersections[intersection_index];
        const auto& native = graph.intersections[intersection_index];
        auto width = 2.0F * scene.unit_size;
        for (const auto& road : scene.roads) {
            if (road.source->from_intersection == static_cast<std::int32_t>(intersection.index) ||
                road.source->to_intersection == static_cast<std::int32_t>(intersection.index)) {
                width = std::max(width, road.width);
            }
        }

        scene.intersections.push_back({
            &intersection,
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

    for (const auto& building : map.buildings) {
        const auto origin = native_city_building_render_origin(building, scene.unit_size);
        scene.buildings.push_back({
            &building,
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
// Buildblock Portal Overlay Builders
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

std::vector<PortalRect> build_portal_window_rects(const SblLayer& layer, PortalSide side) {
    const auto bounds = portal_rect_for_side(layer, side, layer.bounds[0], layer.bounds[1], layer.bounds[2], layer.bounds[3]);
    if (!bounds.has_value()) {
        return {};
    }
    std::vector<PortalRect> window_rects;
    for (const auto& window : layer.windows) {
        const auto cutout = portal_rect_for_side(layer, side, window.x, window.width, window.y, window.height);
        if (cutout.has_value() && rects_intersect(*bounds, *cutout)) {
            window_rects.push_back(*cutout);
        }
    }
    return window_rects;
}

std::vector<PortalRect> build_portal_beam_rects(const SblLayer& layer, PortalSide side) {
    std::vector<PortalRect> beam_rects;
    for (const auto& beam : layer.beams) {
        const auto rect = portal_rect_for_side(layer, side, beam.x, beam.width, beam.y, beam.height);
        if (rect.has_value()) {
            beam_rects.push_back(*rect);
        }
    }
    return beam_rects;
}

} // namespace map_editor
