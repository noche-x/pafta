#include "scene.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace map_editor {
namespace {

// -----------------------------------------------------------------------------
// Portal Rectangle Helpers
// -----------------------------------------------------------------------------

float clamp01(float value) {
    return std::clamp(value, 0.0F, 1.0F);
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
            rotated.material = rotate_native_material_slot(rotated.material, 1);
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

std::array<Vec3, 4> face_rect_vertices(
    BuildingFaceSide side,
    float cx,
    float cy,
    float cz,
    float u,
    float width,
    float v,
    float height,
    float unit_size) {
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

    switch (side) {
        case BuildingFaceSide::West:
            return {Vec3{min_x, y1, z0}, Vec3{min_x, y1, z1}, Vec3{min_x, y0, z1}, Vec3{min_x, y0, z0}};
        case BuildingFaceSide::East:
            return {Vec3{max_x, y1, z1}, Vec3{max_x, y1, z0}, Vec3{max_x, y0, z0}, Vec3{max_x, y0, z1}};
        case BuildingFaceSide::Down:
            return {Vec3{x0, min_y, z1}, Vec3{x1, min_y, z1}, Vec3{x1, min_y, z0}, Vec3{x0, min_y, z0}};
        case BuildingFaceSide::Up:
            return {Vec3{x0, max_y, z0}, Vec3{x1, max_y, z0}, Vec3{x1, max_y, z1}, Vec3{x0, max_y, z1}};
        case BuildingFaceSide::North:
            return {Vec3{x1, y1, min_z}, Vec3{x0, y1, min_z}, Vec3{x0, y0, min_z}, Vec3{x1, y0, min_z}};
        case BuildingFaceSide::South:
        default:
            return {Vec3{x0, y1, max_z}, Vec3{x1, y1, max_z}, Vec3{x1, y0, max_z}, Vec3{x0, y0, max_z}};
    }
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
    for (const auto& window : layer.windows) {
        add_rect_box(window);
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
    bool city_window_surface) {
    add_quad(
        buffer,
        vertex_indices,
        face_rect_vertices(side, cx, cy, cz, rect.x, rect.width, rect.y, rect.height, buffer.unit_size),
        quad_uvs(rect.x, rect.width, rect.y, rect.height),
        material,
        city_window_surface);
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

} // namespace

// -----------------------------------------------------------------------------
// Building Scene and Render Buffers
// -----------------------------------------------------------------------------

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
    BuildingRenderBuffer buffer;
    const auto unit_size = buffer.unit_size;
    VertexIndexMap vertex_indices;
    const auto cell_index = build_cell_index(building.cells);

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
            if (const auto* mesh = resolve_building_block_mesh(block_meshes, building.piece_names, piece_payload->index)) {
                const auto origin = cell_origin(cell);
                const auto rotation = static_cast<std::int32_t>((piece_payload->word >> 24U) & 3U);
                add_rotated_block_mesh_to_buffer(
                    buffer,
                    vertex_indices,
                    *mesh,
                    origin,
                    rotation,
                    [&cell](BuildingRenderBuffer&, const SblBlockFace&, const NativeRotatedBlockFace& rotated_face) {
                        return material_for_cell_slot(cell, rotated_face.material);
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
        const auto* template_file = resolve_building_buildblock_template(
            buildblock_templates,
            building.block_names,
            cell.cell_type());

        struct NativeSideCheck {
            BuildingFaceSide side;
            const SbbCell* neighbor;
        };
        // BuildBuildingBlockRenderBuffers_1400799d0.c:144-146 iterates native
        // faces 1..5 only. Down/face 0 is intentionally absent here.
        const std::array native_sides{
            NativeSideCheck{BuildingFaceSide::South, south},
            NativeSideCheck{BuildingFaceSide::West, west},
            NativeSideCheck{BuildingFaceSide::North, north},
            NativeSideCheck{BuildingFaceSide::East, east},
            NativeSideCheck{BuildingFaceSide::Up, up},
        };

        for (const auto& side : native_sides) {
            if (cell_has_exterior_surface(side.neighbor)) {
                continue;
            }

            std::vector<PortalRect> window_rects;
            const auto portal_side = portal_side_for_building_face(side.side);
            if (portal_side.has_value()) {
                const auto side_code = city_buildblock_side_code_for_face(cell, east, side.side);
                if (const auto* layer = buildblock_layer_for_side_code(template_file, side_code)) {
                    // FUN_140051c60 emits layer window rects as buildblock opening
                    // geometry; BuildBuildingBlockRenderBuffers tags itemset/opening
                    // quads with slot 0x100. Only these rects get the city window texture.
                    window_rects = city_buildblock_window_rects(*layer, *portal_side);
                }
            }

            const auto wall_material = material_for_cell_side(cell, side.side);
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
                    false);
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
                    0x100U,
                    true);
            }
        }
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
    const auto cell_index = build_cell_index(building.cells);

    auto cell_origin = [&](const SbbCell& cell) {
        return Vec3{
            static_cast<float>(cell.x) * buffer.unit_size,
            static_cast<float>(cell.y) * buffer.unit_size,
            static_cast<float>(cell.z) * buffer.unit_size,
        };
    };

    auto add_piece = [&](const SbbCell& cell, const SbbCellPayload& payload, std::uint32_t texture_base) {
        const auto* mesh = resolve_building_block_mesh(block_meshes, building.piece_names, payload.index);
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
            [&cell, texture_base](BuildingRenderBuffer&, const SblBlockFace&, const NativeRotatedBlockFace& rotated_face) {
                const auto material_slot = std::min<std::uint32_t>(rotated_face.material, 7U);
                return material_for_cell_slot(cell, texture_base + material_slot);
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
            add_piece(cell, *payload, 8U);
        }
        if (const auto payload = exterior_piece_payload(cell)) {
            add_piece(cell, *payload, 0U);
        }

        if ((cell.exterior_block() & 0xe0000000U) != 0x40000000U) {
            continue;
        }

        const auto* west = find_cell_at(cell_index, cell.x - 1U, cell.y, cell.z);
        const auto* east = find_cell_at(cell_index, cell.x + 1U, cell.y, cell.z);
        const auto* north = find_cell_at(cell_index, cell.x, cell.y, cell.z - 1U);
        const auto* south = find_cell_at(cell_index, cell.x, cell.y, cell.z + 1U);

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

BuildingRenderBuffer build_native_portal_render_buffer(
    const SbcMap& map,
    const std::vector<NamedBlockMesh>& block_meshes,
    std::optional<std::size_t> selected_portal) {
    BuildingRenderBuffer buffer;
    VertexIndexMap vertex_indices;

    auto layer_slot_material = [&](const SbcCityBlock& city_block, std::size_t layer_index, std::uint32_t slot) {
        if (city_block.layers.empty()) {
            return material_id_for_name(buffer, {});
        }
        const auto& layer = city_block.layers[std::min(layer_index, city_block.layers.size() - 1U)];
        return material_id_for_name(buffer, slot < layer.size() ? layer[slot] : std::string_view{});
    };

    for (std::size_t portal_index = 0; portal_index < map.portals.size(); ++portal_index) {
        if (selected_portal.has_value() && portal_index != *selected_portal) {
            continue;
        }
        const auto& portal = map.portals[portal_index];
        const auto block_count = std::min<std::size_t>(portal.blocks.size(), 0x200);
        for (std::size_t voxel_index = 0; voxel_index < block_count; ++voxel_index) {
            const auto word = portal.blocks[voxel_index];
            if (word == 0U) {
                continue;
            }
            const auto city_block_index = static_cast<std::size_t>(word & 0xfffU);
            if (city_block_index >= map.city_blocks.size()) {
                continue;
            }

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

            const auto& city_block = map.city_blocks[city_block_index];
            if (!city_block.name.empty()) {
                if (const auto* mesh = find_block_mesh(block_meshes, city_block.name)) {
                    add_rotated_block_mesh_to_buffer(
                        buffer,
                        vertex_indices,
                        *mesh,
                        origin,
                        rotation,
                        [&](BuildingRenderBuffer&, const SblBlockFace&, const NativeRotatedBlockFace& rotated_face) {
                            const auto material_slot = std::min<std::uint32_t>(rotated_face.material, 7U);
                            return layer_slot_material(city_block, layer_index, material_slot);
                    });
                    continue;
                }
            }
        }
    }

    sort_triangles_into_native_material_runs(buffer);
    return buffer;
}

std::optional<CellPick> pick_building_cell(
    const SbbBuilding& building,
    const Vec3& ray_origin,
    const Vec3& ray_dir,
    float unit_size) {
    if (unit_size <= 0.0F) {
        return std::nullopt;
    }

    const auto half = unit_size * 0.5F;
    std::optional<CellPick> nearest;
    for (std::size_t cell_index = 0; cell_index < building.cells.size(); ++cell_index) {
        const auto& cell = building.cells[cell_index];
        if (!cell.occupied) {
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
        static_cast<float>(placement.y) * unit_size,
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

MapScene build_map_scene(const SbcMap& map) {
    MapScene scene;

    for (const auto& street : map.streets) {
        if (street.from_intersection < 0 || street.to_intersection < 0) {
            continue;
        }
        const auto from_index = static_cast<std::size_t>(street.from_intersection);
        const auto to_index = static_cast<std::size_t>(street.to_intersection);
        if (from_index >= map.intersections.size() || to_index >= map.intersections.size()) {
            continue;
        }

        const auto& from = map.intersections[from_index];
        const auto& to = map.intersections[to_index];
        scene.roads.push_back({
            &street,
            {
                static_cast<float>(from.x) * scene.unit_size,
                static_cast<float>(from.y) * scene.unit_size,
                static_cast<float>(from.z) * scene.unit_size,
            },
            {
                static_cast<float>(to.x) * scene.unit_size,
                static_cast<float>(to.y) * scene.unit_size,
                static_cast<float>(to.z) * scene.unit_size,
            },
            static_cast<float>(std::max(2, street.left_width + street.right_width + 2)) * scene.unit_size,
        });
    }

    for (const auto& intersection : map.intersections) {
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
                static_cast<float>(intersection.y) * scene.unit_size,
                static_cast<float>(intersection.z) * scene.unit_size,
            },
            width,
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

    if (scene.intersections.empty() && scene.buildings.empty()) {
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
    constexpr auto slots_per_row = 4U;

    if (!city_block.name.empty()) {
        if (const auto* mesh = find_block_mesh(block_meshes, city_block.name)) {
            scene.pieces.push_back({
                0,
                0,
                city_block.name,
                {},
                *mesh,
            });
            return scene;
        }
    }

    for (std::size_t layer = 0; layer < city_block.layers.size(); ++layer) {
        for (std::size_t slot = 0; slot < city_block.layers[layer].size(); ++slot) {
            const auto& name = city_block.layers[layer][slot];
            if (name.empty()) {
                continue;
            }
            const auto* mesh = find_block_mesh(block_meshes, name);
            if (mesh == nullptr) {
                continue;
            }
            scene.pieces.push_back({
                layer,
                slot,
                name,
                {
                    static_cast<float>(slot % slots_per_row) * scene.unit_size * 2.0F,
                    static_cast<float>(layer) * scene.unit_size * 1.5F,
                    static_cast<float>(slot / slots_per_row) * scene.unit_size * 2.0F,
                },
                *mesh,
            });
        }
    }

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
