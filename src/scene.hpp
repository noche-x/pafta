#pragma once

#include "formats.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
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
    // Road-marking overlay layer (0 = none), sampled from the generated overlay
    // array at the same UV.
    float layer{};
};

struct RenderBufferTriangle {
    std::array<std::uint32_t, 3> indices{};
    std::uint32_t material{};
    bool city_window_surface{};
    // Room surfaces inside a building (add_native_interior_room); the shell
    // around them casts the building's shadow, so they cast none.
    bool interior{};
    // Building meshes: the SbbBuilding::cells index this face belongs to, and
    // the cell.shorts slot its texture comes from, for picking and painting.
    std::uint32_t cell{0xffffffffU};
    std::uint8_t slot{0xffU};
    // Buildings: the game's far LOD only. An exterior face with a buildblock
    // side code is one window-array face (the reflective shell); within
    // 192 units the game skips it and draws the interior room instead.
    bool far_only{};
};

struct RenderBufferMaterialRun {
    std::uint32_t material{};
    std::uint32_t first_triangle{};
    std::uint32_t triangle_count{};
    bool city_window_surface{};
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
    // Stencil shadow volume baked by bake_shadow_volume: 9 floats per vertex
    // (position, kind/0/0, face normal) and triangle indices.
    std::vector<float> shadow_vertices;
    std::vector<std::uint32_t> shadow_indices;
    float shadow_top{};
};

// Bakes the buffer's light-independent shadow volume (see render_gpu.cpp's
// shadow volume shader). Pure CPU work, safe on worker threads.
void bake_shadow_volume(BuildingRenderBuffer& buffer);
// Frees the baked volume once it is on the GPU; it is only read at upload.
inline void release_shadow_volume(BuildingRenderBuffer& buffer) {
    std::vector<float>().swap(buffer.shadow_vertices);
    std::vector<std::uint32_t>().swap(buffer.shadow_indices);
}

struct CellPick {
    std::size_t cell_index{};
    Vec3 hit_world{};
    float t{};
};

// A ray hit on a building's rendered faces: the face's cell and texture slot.
struct BuildingFacePick {
    std::size_t cell_index{};
    std::uint32_t slot{};
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
    std::array<Vec3, 4> editor_quad{};
};

struct RenderRoad {
    const SbcStreet* source{};
    Vec3 from{};
    Vec3 to{};
    float width{};
    std::array<Vec3, 4> editor_quad{};
};

enum class CityTileKind {
    Road,
    Curb,
};

struct CityTileCell {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};
    float base_y{};
    // Top-surface grade, in world-Y per +1 cell along each axis. A sloping street
    // carries a non-zero gradient so the cell top is tilted to the grade instead
    // of stepping flat; flat tiles (intersections) keep zero.
    float slope_x{};
    float slope_z{};
    CityTileKind kind{CityTileKind::Road};
    // SbcMap::streets index that owns this road tile, or -1 for intersection tiles.
    std::int32_t street_index{-1};
    // The native world cell: 0x40000000 for a solid cell or 0x80000000 | catalog
    // id for a block, and the eight face codes that pick each face's texture and
    // road-marking overlay (World_SetTileBlockAndResolveNeighborLinks @f830).
    std::uint32_t block{};
    std::array<std::uint16_t, 8> faces{};
    // SbcMap::intersections index of the junction that placed this tile, or -1.
    std::int32_t intersection_index{-1};
};

struct CityTerrain {
    float min_x{};
    float max_x{};
    float min_z{};
    float max_z{};
    std::int32_t sample_count_x{};
    std::int32_t sample_count_z{};
    float sample_spacing{};
    std::vector<float> heights;
    BuildingRenderBuffer mesh;
};

struct CityTrafficLightPole {
    Vec3 base{};
    Vec3 arm_direction{};
    // Street name on the sign under the arm (index into native_street_names).
    std::int32_t sign_name{};
};

// Generated street names, in subrosa.exe order (PTR_s_First_Street_002b2c00):
// First..Ninth Street for streets along X, Abrash..Holland Ave along Z.
inline constexpr std::array<const char*, 17> native_street_names{
    "First Street", "Second Street", "Third Street", "Fourth Street", "Fifth Street",
    "Sixth Street", "Seventh Street", "Eighth Street", "Ninth Street",
    "Abrash Ave", "Bunten Ave", "Carmack Ave", "David Ave", "Eugene Ave",
    "Flinn Ave", "Garriott Ave", "Holland Ave",
};

struct RenderBuildingPlacement {
    const SbcBuilding* source{};
    Vec3 position{};
    float rotation{};
};

struct CityBuildingBasis {
    Vec3 x_axis{1.0F, 0.0F, 0.0F};
    Vec3 y_axis{0.0F, 1.0F, 0.0F};
    Vec3 z_axis{0.0F, 0.0F, 1.0F};
};

// A street-name label on the in-game map (DrawMapMenu @88040), in map units:
// the 512px map image spans 0..256, world (x - 896) / 8 and (z - 896) / 8.
struct CityMapLabel {
    std::int32_t name{};  // index into native_street_names
    float x{};
    float y{};
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
    std::vector<CityTileCell> city_tiles;
    BuildingRenderBuffer city_tile_buffer;
    // Hash of city_tiles, so callers can skip re-uploading unchanged roads.
    std::uint64_t city_tile_fingerprint{};
    std::optional<CityTerrain> terrain;
    std::vector<CityTrafficLightPole> traffic_light_poles;
    std::vector<RenderBuildingPlacement> buildings;
    std::vector<CityMapLabel> map_labels;
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

struct CityBlockPreviewSolidCell {
    std::string name;
    BuildingRenderBuffer buffer;
};

struct CityBlockPreviewScene {
    float unit_size{4.0F};
    std::optional<CityBlockPreviewSolidCell> solid_cell;
    std::vector<CityBlockPreviewPiece> pieces;
};

// VERIFY: LoadAndInitBlockTemplates_SBL @ 14004d4c0 assigns 16 + 4 + 4 low
// built-in indices before file SBL blocks; the visible default-mesh loop in the
// current decomp explicitly walks the first 16-slot table.
inline constexpr std::size_t native_builtin_block_mesh_count = 24U;

SblBlockMesh make_native_builtin_block_mesh();
std::vector<NamedBlockMesh> build_native_block_mesh_catalog(const std::vector<NamedBlockMesh>& file_block_meshes);
SbbScene build_sbb_scene(const SbbBuilding& building);
SbbScene build_sbb_scene(const SbbBuilding& building, const std::vector<NamedBlockMesh>& block_meshes);
SbbScene build_sbb_scene(
    const SbbBuilding& building,
    const std::vector<NamedBlockMesh>& block_meshes,
    const std::vector<NamedBuildblockTemplate>& buildblock_templates);
// The runtime building mesh (city placement, editor view, export). `floor`
// keeps only that cell level, as the building editor's single-floor view;
// roofs = false drops every upward outer face so the rooms show from above.
BuildingRenderBuffer build_native_building_render_buffer(
    const SbbBuilding& building,
    const std::vector<NamedBlockMesh>& block_meshes,
    const std::vector<NamedBuildblockTemplate>& buildblock_templates,
    std::optional<std::uint32_t> floor = std::nullopt,
    bool roofs = true);
// Nearest face of a building mesh along the ray (both sides), from the faces'
// cell/slot tags; nullopt when the ray misses every tagged face.
std::optional<BuildingFacePick> pick_building_face(const BuildingRenderBuffer& buffer, Vec3 origin, Vec3 direction);
// The game's two building LODs (_render_buildsectorlist): Far is the shell
// (buildblock faces as one reflective window-array face, no rooms), Near skips
// those faces and shows the rooms, their openings and glass. Both keeps all.
enum class BuildingLod { Both, Far, Near };
BuildingRenderBuffer build_city_building_full_render_buffer(
    const SbbBuilding& building,
    const std::vector<NamedBlockMesh>& block_meshes,
    const std::vector<NamedBuildblockTemplate>& buildblock_templates,
    BuildingLod lod = BuildingLod::Both);
// Keeps one LOD's triangles (and only the vertices they use).
BuildingRenderBuffer select_building_lod(BuildingRenderBuffer buffer, BuildingLod lod);
BuildingRenderBuffer build_native_building_tool_preview_render_buffer(
    const SbbBuilding& building,
    const std::vector<NamedBlockMesh>& block_meshes,
    const std::vector<NamedBuildblockTemplate>& buildblock_templates,
    std::optional<std::uint32_t> floor = std::nullopt);
BuildingRenderBuffer build_native_portal_render_buffer(
    const SbcMap& map,
    const std::vector<NamedBlockMesh>& block_meshes,
    std::optional<std::size_t> selected_portal = std::nullopt,
    // v10 maps (no city_blocks) resolve district piece words by (word & 0xffff)
    // into the PROCEDURAL buildblock template catalog built in code
    // (build_native_builtin_catalog), not the loose buildblock/*.sbl files. This
    // param is retained only for the inspector's name lookup; the render no longer
    // consumes it.
    const std::vector<NamedBuildblockTemplate>& buildblock_templates = {},
    // v10 per-cell face materials come from the legacy texture grid
    // (portal.textures, 8 shorts per cell), each a texture index into this
    // load-order name list (matching the texture set the game loads). Empty =
    // flat/untextured v10 pieces.
    const std::vector<std::string>& texture_names = {},
    // Placed buildings (map.buildings order); their cells replace v10 district cells.
    const std::vector<const SbbBuilding*>& placement_buildings = {});
// Name of the buildblock a v10 district piece word resolves to, indexed by
// `word & 0xffff` into the game-ordered catalog (sorted, capped 64). Empty if the
// index is out of range (the cell renders nothing). For inspector display.
std::string district_buildblock_name_for_index(
    const std::vector<NamedBuildblockTemplate>& buildblock_templates,
    std::uint32_t catalog_index);
// Returns the building cell hit by a world-space ray, nearest first, or nullopt.
std::optional<CellPick> pick_building_cell(
    const SbbBuilding& building,
    const Vec3& ray_origin,
    const Vec3& ray_dir,
    float unit_size,
    std::optional<std::uint32_t> floor = std::nullopt);
// Intersection of a world-space ray with the y=0 ground plane, or nullopt when
// the ray is parallel to it or only hits behind the origin.
std::optional<Vec3> ray_ground_hit(const Vec3& ray_origin, const Vec3& ray_dir);
// World-space hit of a ray against the terrain heightfield surface (marches then
// bisects). Falls back to ray_ground_hit when the terrain has no samples or the
// ray starts below the surface, so callers always get a usable ground point.
std::optional<Vec3> ray_terrain_hit(const CityTerrain& terrain, const Vec3& ray_origin, const Vec3& ray_dir);
// Returns the index into map.buildings whose placed footprint the ray hits
// nearest, or nullopt. placement_buildings[i] resolves each placement's loaded
// .sbb (nullptr when unresolved -> a single placement cell box is used).
std::optional<std::size_t> pick_city_building(
    const SbcMap& map,
    const std::vector<const SbbBuilding*>& placement_buildings,
    const Vec3& ray_origin,
    const Vec3& ray_dir,
    float unit_size);
struct CityPiecePick {
    std::size_t portal_index{};
    std::size_t block_index{};
    std::uint32_t word{};
};
std::optional<CityPiecePick> pick_city_piece(
    const SbcMap& map,
    const Vec3& ray_origin,
    const Vec3& ray_dir,
    float unit_size);
// Returns the index into map.streets of the road segment whose ground footprint
// (segment within its drawn width) the ray's ground hit lands on, or nullopt.
std::optional<std::size_t> pick_city_road(
    const SbcMap& map,
    const MapScene& scene,
    const Vec3& ray_origin,
    const Vec3& ray_dir);
std::optional<std::size_t> pick_city_intersection(
    const SbcMap& map,
    const MapScene& scene,
    const Vec3& ray_origin,
    const Vec3& ray_dir);
// Unified road/node pick: tests both the node markers and the road surfaces and
// returns whichever the ray reaches first (nearest to the camera), so a click on
// the visible road selects the road and a click on a node selects the node -
// matching how building picking just grabs whatever is under the cursor.
struct CityRoadPick {
    enum class Kind { Node, Road };
    Kind kind{};
    std::size_t index{};
};
std::optional<CityRoadPick> pick_city_road_or_node(
    const SbcMap& map,
    const MapScene& scene,
    const Vec3& ray_origin,
    const Vec3& ray_dir);
SbcStreet canonical_city_street_endpoint_order(const SbcMap& map, SbcStreet street);
CityBuildingBasis native_city_building_rotation_basis(std::int32_t rotation);
Vec3 native_city_rotate_local(Vec3 local, std::int32_t rotation);
Vec3 native_city_building_render_origin(const SbcBuilding& placement, float unit_size = 4.0F);
// Per placement, indices of its cells that a later overlapping placement overwrites.
std::vector<std::vector<std::size_t>> overwritten_building_cells(
    const SbcMap& map, const std::vector<const SbbBuilding*>& placement_buildings);
Vec3 native_city_building_transform_point(const SbcBuilding& placement, Vec3 local, float unit_size = 4.0F);
MapScene build_map_scene(const SbcMap& map);
// placement_buildings[i] resolves map.buildings[i] to its loaded .sbb data
// (nullptr when unresolved); used to stamp building ground footprints into the
// terrain corner-height grid like the native instantiation does.
// auto_generated: isMapAutoGenerated (the "round" map), which skips the terrain
// stamps, land-use mask, and block patches.
MapScene build_map_scene(const SbcMap& map, const std::vector<const SbbBuilding*>& placement_buildings, bool auto_generated = false);
std::uint64_t city_scene_fingerprint(const SbcMap& map);
// Subset fingerprint over only terrain-affecting inputs (intersections, building
// footprints, piece cells). Lets the GPU layer re-upload the heavy terrain mesh
// only when the terrain actually changed, not on every road/street edit.
std::uint64_t city_terrain_fingerprint(const SbcMap& map);
std::uint32_t terrain_noise_hash(std::int32_t x, std::int32_t y, std::uint32_t mask);
float terrain_noise_sample(float x, float z, float frequency_x, float frequency_z);
// city_map: false on isMapAutoGenerated maps, which skip the shoreline cut.
float native_like_city_terrain_height(float world_x, float world_z, bool city_map = true);
float terrain_height_at(const CityTerrain& terrain, float world_x, float world_z);
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
