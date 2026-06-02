#pragma once

#include "formats.hpp"

#include <optional>
#include <vector>

namespace map_editor {

/**
 * Native building domain hierarchy, as used by editor.exe:
 * - Block mesh: SblBlockMesh. Native cell mesh-instance words use 0x80000000
 *   as the present bit, low bits as blockId-1 / piece id, rotation in bits
 *   24+, and interior 0xffff as empty.
 * - Buildblock: SblTemplate, the solid/building-cell template surface. Each
 *   layer is a portal_type record with bounds, doors, windows, and beams.
 * - Portal face: SbbCell exteriorPortalFace (+0x0c) / interiorPortalFace
 *   (+0x10), storing the native side-code.
 * - Edge: wall generated from side-code sc 3..15 by selecting
 *   buildblock.layers[sc - 3] and emitting wall/inset rects.
 * - Piece: parallel cell payload kind (word & 0x3ff), a standalone block mesh
 *   instanced separately from solid/buildblock cells.
 */

struct Vec3 {
    float x{};
    float y{};
    float z{};
};

struct RenderCell {
    Vec3 center{};
    Vec3 size{};
    PayloadKind payload_kind{PayloadKind::Other};
};

struct NamedBlockMesh {
    std::string name;
    SblBlockMesh mesh;
};

struct NamedBuildblockTemplate {
    std::string name;
    // SblTemplate is the native buildblock file model; keep the old type name
    // for parser/API compatibility while using Buildblock at scene boundaries.
    SblTemplate template_file;
};

struct RenderPiece {
    Vec3 origin{};
    SblBlockMesh mesh;
    std::int32_t rotation{};
};

struct RenderBufferVertex {
    Vec3 position{};
    float u{};
    float v{};
};

struct RenderBufferTriangle {
    std::array<std::uint32_t, 3> indices{};
    std::uint32_t material{};
};

struct RenderBufferMaterialRun {
    std::uint32_t material{};
    std::uint32_t first_triangle{};
    std::uint32_t triangle_count{};
};

struct RenderLine {
    Vec3 from{};
    Vec3 to{};
};

struct BuildingRenderBuffer {
    float unit_size{4.0F};
    std::vector<std::string> material_names;
    std::vector<RenderBufferVertex> vertices;
    std::vector<RenderBufferTriangle> triangles;
    std::vector<RenderBufferMaterialRun> material_runs;
    std::vector<RenderLine> wire_lines;
};

struct CellPick {
    std::size_t cell_index{};
    Vec3 hit_world{};
    float t{};
};

struct NativeRotatedBlockFace {
    std::array<Vec3, 4> vertices{};
    std::array<Vec2f, 4> uvs{};
    std::uint32_t material{};
};

struct PortalSideCodes {
    std::uint32_t north{};
    std::uint32_t west{};
    std::uint32_t south{};
    std::uint32_t east{};
};

enum class PortalSide {
    South,
    West,
    North,
    East,
};

struct PortalRect {
    float x{};
    float y{};
    float width{};
    float height{};
    float max_x{};
    float max_y{};
};

struct PortalWallRects {
    std::vector<PortalRect> north;
    std::vector<PortalRect> west;
    std::vector<PortalRect> south;
    std::vector<PortalRect> east;
};

struct RenderPortalEdge {
    const SbbCell* source{};
    Vec3 origin{};
    std::uint32_t block_word{};
    // Native side-codes: sc 3..15 selects buildblock.layers[sc - 3].
    PortalSideCodes side_codes{};
    PortalWallRects wall_rects{};
    SblTemplate template_file;
};

struct SbbScene {
    float unit_size{4.0F};
    float width{};
    float height{};
    float depth{};
    std::vector<RenderCell> cells;
    std::vector<RenderPiece> pieces;
    std::vector<RenderPortalEdge> portal_edges;
};

struct RenderIntersection {
    const SbcIntersection* source{};
    Vec3 position{};
    float width{};
};

struct RenderRoad {
    const SbcStreet* source{};
    Vec3 from{};
    Vec3 to{};
    float width{};
};

struct RenderBuildingPlacement {
    const SbcBuilding* source{};
    Vec3 position{};
    float rotation{};
};

struct MapBounds {
    float min_x{};
    float max_x{};
    float min_z{};
    float max_z{};
};

struct MapScene {
    float unit_size{4.0F};
    std::vector<RenderIntersection> intersections;
    std::vector<RenderRoad> roads;
    std::vector<RenderBuildingPlacement> buildings;
    MapBounds bounds{};
};

enum class BuildblockPreviewRectKind {
    Bounds,
    Window,
    Beam,
};

struct BuildblockPreviewRect {
    std::size_t layer_index{};
    BuildblockPreviewRectKind kind{BuildblockPreviewRectKind::Bounds};
    Vec3 center{};
    Vec3 size{};
};

struct BuildblockPreviewDoor {
    std::size_t layer_index{};
    std::uint32_t type{};
    Vec3 position{};
};

struct BuildblockPreviewScene {
    float layer_spacing{5.0F};
    std::vector<BuildblockPreviewRect> rects;
    std::vector<BuildblockPreviewDoor> doors;
};

struct CityBlockPreviewPiece {
    std::size_t layer_index{};
    std::size_t slot_index{};
    std::string name;
    Vec3 origin{};
    SblBlockMesh mesh;
};

struct CityBlockPreviewScene {
    float unit_size{4.0F};
    std::vector<CityBlockPreviewPiece> pieces;
};

SbbScene build_sbb_scene(const SbbBuilding& building);
SbbScene build_sbb_scene(const SbbBuilding& building, const std::vector<NamedBlockMesh>& block_meshes);
SbbScene build_sbb_scene(
    const SbbBuilding& building,
    const std::vector<NamedBlockMesh>& block_meshes,
    const std::vector<NamedBuildblockTemplate>& buildblock_templates);
BuildingRenderBuffer build_native_building_render_buffer(
    const SbbBuilding& building,
    const std::vector<NamedBlockMesh>& block_meshes,
    const std::vector<NamedBuildblockTemplate>& buildblock_templates);
BuildingRenderBuffer build_native_building_tool_preview_render_buffer(
    const SbbBuilding& building,
    const std::vector<NamedBlockMesh>& block_meshes,
    const std::vector<NamedBuildblockTemplate>& buildblock_templates,
    std::optional<std::uint32_t> floor = std::nullopt);
BuildingRenderBuffer build_native_portal_render_buffer(
    const SbcMap& map,
    const std::vector<NamedBlockMesh>& block_meshes,
    std::optional<std::size_t> selected_portal = std::nullopt);
// Returns the building cell hit by a world-space ray, nearest first, or nullopt.
std::optional<CellPick> pick_building_cell(
    const SbbBuilding& building,
    const Vec3& ray_origin,
    const Vec3& ray_dir,
    float unit_size);
Vec3 native_city_building_render_origin(const SbcBuilding& placement, float unit_size = 4.0F);
MapScene build_map_scene(const SbcMap& map);
BuildblockPreviewScene build_buildblock_preview_scene(const SblTemplate& buildblock);
CityBlockPreviewScene build_city_block_preview_scene(const SbcCityBlock& city_block, const std::vector<NamedBlockMesh>& block_meshes);
NativeRotatedBlockFace rotate_block_face_native(const SblBlockMesh& mesh, const SblBlockFace& face, std::int32_t rotation);
// Native itemset/buildblock inset border rect for side-code sc 3..15. Kept
// under the historical "portal overlay" API name because app.cpp/main.cpp use it.
std::vector<PortalRect> build_portal_overlay_rects(const SblLayer& layer, PortalSide side);
std::vector<PortalRect> build_portal_wall_rects(const SblLayer& layer, PortalSide side);
std::vector<PortalRect> build_portal_window_rects(const SblLayer& layer, PortalSide side);
std::vector<PortalRect> build_portal_beam_rects(const SblLayer& layer, PortalSide side);

} // namespace map_editor
