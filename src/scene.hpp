// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "formats.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace pafta {

std::optional<std::array<std::uint8_t, 3>> procedural_material_rgb(std::string_view name);


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
    // SBL buildblocks use the parser's SblTemplate type.
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
    // Road-marking overlay layer (0 = none), read from the generated overlay array at
    // the same UV.
    float layer{};
};

struct RenderBufferTriangle {
    std::array<std::uint32_t, 3> indices{};
    std::uint32_t material{};
    bool city_window_surface{};
    // Room surfaces inside a building (add_native_interior_room). The shell around them
    // casts the building's shadow, so they cast none.
    bool interior{};
    // Building meshes: the SbbBuilding::cells index this face belongs to, and the
    // cell.shorts slot its texture comes from, for picking and painting.
    std::uint32_t cell{0xffffffffU};
    std::uint8_t slot{0xffU};
    // Buildings: only in the game's far view. An outside face with a buildblock side
    // code is one window-array face (the reflective shell). Within 192 units the game
    // skips it and draws the room inside instead.
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

struct RenderBufferMask {
    std::uint64_t key{};
    std::vector<Vec3> points;
};

struct TrianglePickGrid {
    bool ready{};
    float min_x{};
    float min_z{};
    std::int32_t nx{};
    std::int32_t nz{};
    std::vector<std::uint32_t> offsets;
    std::vector<std::uint32_t> items;
    std::vector<std::uint32_t> visited;
    std::uint32_t stamp{};
};

struct BuildingRenderBuffer {
    float unit_size{4.0F};
    std::vector<std::string> material_names;
    std::vector<RenderBufferVertex> vertices;
    std::vector<RenderBufferTriangle> triangles;
    std::vector<RenderBufferMaterialRun> material_runs;
    std::vector<RenderLine> wire_lines;
    // Picking and highlights belong to this geometry. Builders and picking compaction
    // clear these caches when the geometry changes.
    mutable std::vector<RenderBufferMask> mask_cache;
    mutable TrianglePickGrid pick_grid;
    // Stencil shadow volume baked by bake_shadow_volume: 9 floats per vertex (position,
    // kind/0/0, face normal) and triangle indices.
    std::vector<float> shadow_vertices;
    std::vector<std::uint32_t> shadow_indices;
    float shadow_top{};
};

// Bakes the buffer's shadow volume, which works for any light direction (see the
// shadow volume shader in render_gpu.cpp). CPU work only, safe on worker threads.
void bake_shadow_volume(BuildingRenderBuffer& buffer);
// Frees the baked volume once it is on the GPU; it is only read when uploading.
inline void release_shadow_volume(BuildingRenderBuffer& buffer) {
    std::vector<float>().swap(buffer.shadow_vertices);
    std::vector<std::uint32_t>().swap(buffer.shadow_indices);
}

// Shrinks an uploaded building buffer to what picking reads: one vertex per
// different position (texture coordinates cleared) and the triangle indices.
void keep_pick_data_only(BuildingRenderBuffer& buffer);

struct CellPick {
    std::size_t cell_index{};
    Vec3 hit_world{};
    float t{};
};

// A ray hit on a building's drawn faces: the face's cell and texture slot.
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
    // Game side codes: sc 3..15 picks buildblock.layers[sc - 3].
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
    SbcIntersection source;
    std::size_t source_index{};
    Vec3 position{};
    float width{};
    std::array<Vec3, 4> editor_quad{};
};

struct RenderRoad {
    SbcStreet source;
    std::size_t source_index{};
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
    // Slope of the top surface, as world Y per cell along each axis. A sloping street
    // has a slope that isn't zero, so the cell's top tilts with it instead of stepping;
    // flat tiles (intersections) keep zero.
    float slope_x{};
    float slope_z{};
    CityTileKind kind{CityTileKind::Road};
    // The SbcMap::streets index this road tile belongs to, or -1 for intersection tiles.
    std::int32_t street_index{-1};
    // The game's world cell: 0x40000000 for a solid cell, or 0x80000000 | catalog id
    // for a block, plus the eight face codes that pick each face's texture and road
    // marking overlay (World_SetTileBlockAndResolveNeighborLinks @f830).
    std::uint32_t block{};
    std::array<std::uint16_t, 8> faces{};
    // The SbcMap::intersections index of the junction that placed this tile, or -1.
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
    // The street name on the sign under the arm (an index into native_street_names).
    std::int32_t sign_name{};
};

// Made-up street names, in subrosa.exe's order (PTR_s_First_Street_002b2c00):
// First..Ninth Street for streets along X, and Abrash..Holland Ave along Z.
inline constexpr std::array<const char*, 17> native_street_names{
    "First Street", "Second Street", "Third Street", "Fourth Street", "Fifth Street",
    "Sixth Street", "Seventh Street", "Eighth Street", "Ninth Street",
    "Abrash Ave", "Bunten Ave", "Carmack Ave", "David Ave", "Eugene Ave",
    "Flinn Ave", "Garriott Ave", "Holland Ave",
};

struct RenderBuildingPlacement {
    SbcBuilding source;
    std::size_t source_index{};
    Vec3 position{};
    float rotation{};
};

struct CityBuildingBasis {
    Vec3 x_axis{1.0F, 0.0F, 0.0F};
    Vec3 y_axis{0.0F, 1.0F, 0.0F};
    Vec3 z_axis{0.0F, 0.0F, 1.0F};
};

// A street-name label on the in-game map (DrawMapMenu @88040), in map units. The
// 512px map image covers 0..256, which is world (x - 896) / 8 and (z - 896) / 8.
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

// The game world is the one area that World_LoadMapAndInitializeTerrain @56980
// makes: 64 x 8 x 64 chunks of 64 cells. World_SetTileBlockAndResolveNeighborLinks
// @f830 quietly drops tiles outside it, so the road is missing there while traffic
// still follows the street graph.
inline bool city_cell_in_game_world(std::int32_t x, std::int32_t y, std::int32_t z) {
    return x >= 0 && x < 4096 && y >= 0 && y < 512 && z >= 0 && z < 4096;
}

// How much of the map the game's world storage can hold: 4096 sectors (8^3 cells)
// and 512 chunks (64^3 cells). _area_createsector @12490 and
// World_AllocateAreaChunkIndex @10020 hand them out first come, first served, in
// load order (streets, junctions, district cells, then buildings). A write that
// finds no free slot is dropped, the same as one outside the world.
struct GameWorldFit {
    static constexpr std::size_t max_sectors = 4096;
    static constexpr std::size_t max_chunks = 512;
    std::size_t sectors_used{};
    std::size_t chunks_used{};
    // Dropped because they are outside the world, or because no slot was left.
    std::size_t road_tiles_outside{};
    std::size_t road_tiles_no_room{};
    std::size_t building_cells_outside{};
    std::size_t building_cells_no_room{};
    std::vector<std::vector<std::size_t>> building_cells;  // per placement, dropped cell indices
};

struct MapScene {
    // Source records are owned copies. source_index links them to the input lists, and
    // picking checks that the copy still matches the current record.
    float unit_size{4.0F};
    std::vector<RenderIntersection> intersections;
    std::vector<RenderRoad> roads;
    std::vector<CityTileCell> city_tiles;
    BuildingRenderBuffer city_tile_buffer;
    // Hash of city_tiles, so callers can skip uploading roads that didn't change.
    std::uint64_t city_tile_fingerprint{};
    std::optional<CityTerrain> terrain;
    std::vector<CityTrafficLightPole> traffic_light_poles;
    std::vector<RenderBuildingPlacement> buildings;
    std::vector<CityMapLabel> map_labels;
    MapBounds bounds{};
    GameWorldFit world_fit;
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

// VERIFY: LoadAndInitBlockTemplates_SBL @ 14004d4c0 hands out 16 + 4 + 4 low
// built-in indices before the SBL blocks from files. The default-mesh loop we can
// see in the current decompile walks the first 16-slot table.
inline constexpr std::size_t native_builtin_block_mesh_count = 24U;

SblBlockMesh make_native_builtin_block_mesh();
std::vector<NamedBlockMesh> build_native_block_mesh_catalog(const std::vector<NamedBlockMesh>& file_block_meshes);
SbbScene build_sbb_scene(const SbbBuilding& building);
SbbScene build_sbb_scene(const SbbBuilding& building, const std::vector<NamedBlockMesh>& block_meshes);
SbbScene build_sbb_scene(
    const SbbBuilding& building,
    const std::vector<NamedBlockMesh>& block_meshes,
    const std::vector<NamedBuildblockTemplate>& buildblock_templates);
// The building mesh as the game draws it (city placement, editor view, export).
// `floor` keeps only that cell level, like the building editor's single-floor view.
// roofs = false leaves out every outer face that points up, so you can see the rooms
// from above.
BuildingRenderBuffer build_native_building_render_buffer(
    const SbbBuilding& building,
    const std::vector<NamedBlockMesh>& block_meshes,
    const std::vector<NamedBuildblockTemplate>& buildblock_templates,
    std::optional<std::uint32_t> floor = std::nullopt,
    bool roofs = true);
// The closest face of a building mesh along the ray (either side), from the faces'
// cell and slot tags; nullopt when the ray misses every tagged face.
std::optional<BuildingFacePick> pick_building_face(const BuildingRenderBuffer& buffer, Vec3 origin, Vec3 direction);
// The game's two building views (_render_buildsectorlist). Far is the shell
// (buildblock faces as one reflective window-array face, no rooms). Near skips those
// faces and shows the rooms, their openings, and glass. Both keeps everything.
enum class BuildingLod { Both, Far, Near };
BuildingRenderBuffer build_city_building_full_render_buffer(
    const SbbBuilding& building,
    const std::vector<NamedBlockMesh>& block_meshes,
    const std::vector<NamedBuildblockTemplate>& buildblock_templates,
    BuildingLod lod = BuildingLod::Both);
// Keeps one view's triangles (and only the vertices they use).
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
    // Kept so callers don't change. v10 pieces use the built-in catalog, so loaded
    // buildblock templates don't affect this buffer.
    const std::vector<NamedBuildblockTemplate>& buildblock_templates = {},
    // Kept so callers don't change. v10 texture-grid values pick the game's concrete
    // or asphalt by zero or not zero, not by this loaded texture list.
    const std::vector<std::string>& texture_names = {},
    // Placed buildings (in map.buildings order); their cells replace v10 district cells.
    const std::vector<const SbbBuilding*>& placement_buildings = {});
// Returns the building cell a world-space ray hits, closest first, or nullopt.
std::optional<CellPick> pick_building_cell(
    const SbbBuilding& building,
    const Vec3& ray_origin,
    const Vec3& ray_dir,
    float unit_size,
    std::optional<std::uint32_t> floor = std::nullopt);
// Where a world-space ray meets the y=0 ground plane, or nullopt when the ray runs
// parallel to it or only meets it behind the start.
std::optional<Vec3> ray_ground_hit(const Vec3& ray_origin, const Vec3& ray_dir);
// Where a world-space ray hits the terrain surface (steps along, then halves the
// gap). Falls back to ray_ground_hit when the terrain has no samples or the ray
// starts under the surface, so callers always get a usable ground point.
std::optional<Vec3> ray_terrain_hit(const CityTerrain& terrain, const Vec3& ray_origin, const Vec3& ray_dir);
// Returns the index into map.buildings of the placed footprint the ray hits first,
// or nullopt. placement_buildings[i] gives each placement's loaded .sbb (nullptr
// when not found, in which case one box the size of the placement cell is used).
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
std::optional<std::size_t> pick_city_intersection(
    const SbcMap& map,
    const MapScene& scene,
    const Vec3& ray_origin,
    const Vec3& ray_dir);
// Combined road and node pick: tests both the node markers and the road surfaces and
// returns whichever the ray reaches first (closest to the camera). So a click on the
// visible road selects the road and a click on a node selects the node, just like
// building picking grabs whatever is under the cursor.
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
// Removes junctions no road uses and renumbers the roads' ends.
void remove_unused_city_intersections(SbcMap& map);
CityBuildingBasis native_city_building_rotation_basis(std::int32_t rotation);
Vec3 native_city_rotate_local(Vec3 local, std::int32_t rotation);
Vec3 native_city_building_render_origin(const SbcBuilding& placement, float unit_size = 4.0F);
// For each placement, the indices of its cells that a later overlapping placement
// overwrites.
std::vector<std::vector<std::size_t>> overwritten_building_cells(
    const SbcMap& map, const std::vector<const SbbBuilding*>& placement_buildings);
Vec3 native_city_building_transform_point(const SbcBuilding& placement, Vec3 local, float unit_size = 4.0F);
MapScene build_map_scene(const SbcMap& map);
// placement_buildings[i] gives the loaded .sbb data for map.buildings[i] (nullptr
// when not found). It is used to stamp building footprints into the terrain
// corner-height grid, like the game does when it places buildings.
// auto_generated: isMapAutoGenerated (the "round" map), which skips the terrain
// stamps, the land-use mask, and the block patches.
// fit_game_world drops what the game's world storage can't hold (GameWorldFit).
// The one-argument version skips that and keeps all the street geometry.
MapScene build_map_scene(const SbcMap& map, const std::vector<const SbbBuilding*>& placement_buildings, bool auto_generated = false,
    bool fit_game_world = true);

// A street's slope as the game lays it. Slope blocks only exist for 4..12 cells per
// level (native_grade_slot_table); outside that range the game leaves holes or
// places the wrong block.
struct StreetRun {
    std::int32_t length{};          // cells between the two junction footprints
    std::int32_t climb{};           // levels from the first junction to the second
    std::int32_t cells_per_level{}; // slope run length
    bool slope_ok() const {
        const auto levels = climb < 0 ? -climb : climb;
        return length > 0 && (levels == 0 ||
            (cells_per_level >= 4 && cells_per_level <= 12 && cells_per_level * levels <= length));
    }
};
struct StreetPreview {
    StreetRun run;
    std::vector<CityTileCell> cells;  // the street's tiles and its two junctions' tiles, as the city build places them
};
// Lays only map.streets[street_index], for the Road tool's preview.
StreetPreview preview_city_street(const SbcMap& map, std::size_t street_index, bool auto_generated);
// building_revision changes when loaded SBB data changes outside the map.
std::uint64_t city_scene_fingerprint(const SbcMap& map, std::uint64_t building_revision = 0);
// A fingerprint of only the inputs that affect terrain (intersections, building
// footprints, piece cells). Lets the GPU code upload the heavy terrain mesh again
// only when the terrain really changed, not after every road or street edit.
std::uint64_t city_terrain_fingerprint(const SbcMap& map, std::uint64_t building_revision = 0);
std::uint32_t terrain_noise_hash(std::int32_t x, std::int32_t y, std::uint32_t mask);
float terrain_noise_sample(float x, float z, float frequency_x, float frequency_z);
// city_map: false on isMapAutoGenerated maps, which skip the shoreline cut.
float native_like_city_terrain_height(float world_x, float world_z, bool city_map = true);
float terrain_height_at(const CityTerrain& terrain, float world_x, float world_z);
BuildblockPreviewScene build_buildblock_preview_scene(const SblTemplate& buildblock);
CityBlockPreviewScene build_city_block_preview_scene(const SbcCityBlock& city_block, const std::vector<NamedBlockMesh>& block_meshes);
NativeRotatedBlockFace rotate_block_face_native(const SblBlockMesh& mesh, const SblBlockFace& face, std::int32_t rotation);
// The game's item-set and buildblock inset border rectangle for side codes 3..15.
// It keeps the old "portal overlay" name because app.cpp and main.cpp use it.
std::vector<PortalRect> build_portal_overlay_rects(const SblLayer& layer, PortalSide side);
std::vector<PortalRect> build_portal_wall_rects(const SblLayer& layer, PortalSide side);

} // namespace pafta
