#pragma once

#include "formats.hpp"
#include "scene.hpp"

#include <raylib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace map_editor::app_internal {

template <typename T>
struct LoadedAsset {
    std::filesystem::path path;
    std::string name;
    T data;
};

struct GpuBuildingMeshChunk {
    Mesh mesh{};
    Material material{};
    Texture2D source_texture{};
    std::uint32_t material_id{};
    bool textured{};
    bool window_material{};

    GpuBuildingMeshChunk() = default;
    GpuBuildingMeshChunk(const GpuBuildingMeshChunk&) = delete;
    GpuBuildingMeshChunk& operator=(const GpuBuildingMeshChunk&) = delete;

    GpuBuildingMeshChunk(GpuBuildingMeshChunk&& other) noexcept
        : mesh(other.mesh),
          material(other.material),
          source_texture(other.source_texture),
          material_id(other.material_id),
          textured(other.textured),
          window_material(other.window_material) {
        other.mesh = {};
        other.material = {};
        other.source_texture = {};
        other.material_id = 0;
        other.textured = false;
        other.window_material = false;
    }

    GpuBuildingMeshChunk& operator=(GpuBuildingMeshChunk&& other) noexcept {
        if (this != &other) {
            unload();
            mesh = other.mesh;
            material = other.material;
            source_texture = other.source_texture;
            material_id = other.material_id;
            textured = other.textured;
            window_material = other.window_material;
            other.mesh = {};
            other.material = {};
            other.source_texture = {};
            other.material_id = 0;
            other.textured = false;
            other.window_material = false;
        }
        return *this;
    }

    ~GpuBuildingMeshChunk() {
        unload();
    }

    void unload() {
        if (mesh.vertices != nullptr || mesh.vboId != nullptr || mesh.vaoId != 0U) {
            UnloadMesh(mesh);
            mesh = {};
        }
        if (material.maps != nullptr) {
            MemFree(material.maps);
            material = {};
        }
    }
};

struct LoadedTexture {
    std::string name;
    Texture2D texture{};

    LoadedTexture() = default;
    LoadedTexture(std::string texture_name, Texture2D loaded_texture)
        : name(std::move(texture_name)),
          texture(loaded_texture) {}
    LoadedTexture(const LoadedTexture&) = delete;
    LoadedTexture& operator=(const LoadedTexture&) = delete;

    LoadedTexture(LoadedTexture&& other) noexcept
        : name(std::move(other.name)),
          texture(other.texture) {
        other.texture = {};
    }

    LoadedTexture& operator=(LoadedTexture&& other) noexcept {
        if (this != &other) {
            unload();
            name = std::move(other.name);
            texture = other.texture;
            other.texture = {};
        }
        return *this;
    }

    ~LoadedTexture() {
        unload();
    }

    void unload() {
        if (texture.id != 0U) {
            UnloadTexture(texture);
            texture = {};
        }
    }
};

struct GpuBuildingRenderModel {
    std::vector<GpuBuildingMeshChunk> chunks;
    std::vector<RenderLine> wire_lines;
    std::size_t uploaded_vertices{};
    std::size_t uploaded_indices{};
    std::size_t uploaded_bytes{};
};

enum class GfxDebugDrawCategory : std::size_t {
    CityCells,
    RoadsCurbs,
    Terrain,
    TrafficLightPoles,
    Pieces,
    WireOverlay,
    Count,
};

enum class GfxDebugTimingPhase : std::size_t {
    SceneRenderBufferBuild,
    GpuUpload,
    Draw3D,
    TerrainDraw,
    CityRoadDraw,
    BuildingDraw,
    PieceDraw,
    OutlineDraw,
    PoleDraw,
    ImGuiUi,
    TotalFrame,
    Count,
};

struct GfxDebugDrawCounter {
    std::uint64_t triangles{};
    std::uint64_t vertices{};
    std::uint64_t draw_calls{};
};

struct GfxDebugUploadStats {
    std::uint64_t uploaded_vertices{};
    std::uint64_t uploaded_indices{};
    std::uint64_t uploaded_bytes{};
    std::uint64_t chunks{};
};

struct GfxDebugCounts {
    std::uint64_t buildings{};
    std::uint64_t city_tiles{};
    std::uint64_t terrain_samples{};
    std::uint64_t terrain_vertices{};
    std::uint64_t traffic_light_poles{};
    std::uint64_t total_scene_triangles{};
};

struct GfxDebugStats {
    static constexpr std::size_t history_size = 120;
    std::array<float, history_size> frame_time_history{};
    std::size_t frame_time_index{};
    std::size_t frame_time_count{};
    std::array<GfxDebugDrawCounter, static_cast<std::size_t>(GfxDebugDrawCategory::Count)> draw{};
    std::array<double, static_cast<std::size_t>(GfxDebugTimingPhase::Count)> phase_ms{};
    std::array<double, static_cast<std::size_t>(GfxDebugTimingPhase::Count)> phase_avg_ms{};
    GfxDebugUploadStats uploads{};
    GfxDebugCounts counts{};

    void reset_frame() {
        draw = {};
        phase_ms = {};
        uploads = {};
        counts = {};
    }

    void push_frame_time(float ms) {
        frame_time_history[frame_time_index] = ms;
        frame_time_index = (frame_time_index + 1U) % frame_time_history.size();
        frame_time_count = std::min(frame_time_count + 1U, frame_time_history.size());
    }

    void add_draw(GfxDebugDrawCategory category, std::uint64_t triangles, std::uint64_t vertices, std::uint64_t draw_calls) {
        auto& counter = draw[static_cast<std::size_t>(category)];
        counter.triangles += triangles;
        counter.vertices += vertices;
        counter.draw_calls += draw_calls;
    }

    void add_phase_ms(GfxDebugTimingPhase phase, double ms) {
        auto& current = phase_ms[static_cast<std::size_t>(phase)];
        current += ms;
        auto& average = phase_avg_ms[static_cast<std::size_t>(phase)];
        average = average == 0.0 ? current : average * 0.92 + current * 0.08;
    }

    [[nodiscard]] GfxDebugDrawCounter total_draw() const {
        GfxDebugDrawCounter total{};
        for (const auto& counter : draw) {
            total.triangles += counter.triangles;
            total.vertices += counter.vertices;
            total.draw_calls += counter.draw_calls;
        }
        return total;
    }
};

struct GfxDebugScopedTimer {
    GfxDebugStats& stats;
    GfxDebugTimingPhase phase;
    std::chrono::steady_clock::time_point begin{std::chrono::steady_clock::now()};

    GfxDebugScopedTimer(GfxDebugStats& target, GfxDebugTimingPhase target_phase)
        : stats(target),
          phase(target_phase) {}

    ~GfxDebugScopedTimer() {
        const auto end = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration<double, std::milli>(end - begin).count();
        stats.add_phase_ms(phase, ms);
    }
};

extern GfxDebugStats* g_gfx_debug_stats;

// Times a phase only when the debug overlay is active; no-op otherwise. Lets draw
// code use one RAII line instead of branching on g_gfx_debug_stats everywhere.
struct GfxDebugMaybeTimer {
    GfxDebugTimingPhase phase;
    std::chrono::steady_clock::time_point begin{std::chrono::steady_clock::now()};

    explicit GfxDebugMaybeTimer(GfxDebugTimingPhase target_phase) : phase(target_phase) {}

    ~GfxDebugMaybeTimer() {
        if (g_gfx_debug_stats != nullptr) {
            const auto end = std::chrono::steady_clock::now();
            const auto ms = std::chrono::duration<double, std::milli>(end - begin).count();
            g_gfx_debug_stats->add_phase_ms(phase, ms);
        }
    }
};

struct RenderBuildResult {
    std::uint64_t generation{};
    std::vector<BuildingRenderBuffer> buffers;
};

struct RenderBuildProgress {
    std::mutex mutex;
    std::vector<std::string> logs;
    struct ReadyBuildingBuffers {
        std::size_t index{};
        BuildingRenderBuffer city_buffer;
    };
    std::vector<ReadyBuildingBuffers> ready_buffers;
    std::uint64_t generation{};
    std::size_t completed{};
    std::size_t total{};
    std::string current;
};

struct EditorProject {
    std::filesystem::path root;
    std::optional<std::vector<std::uint8_t>> city_bytes;
    std::optional<LoadedAsset<SbcMap>> city;
    std::vector<std::vector<std::uint8_t>> building_bytes;
    std::vector<std::vector<std::uint8_t>> block_bytes;
    std::vector<std::vector<std::uint8_t>> buildblock_bytes;
    std::vector<LoadedAsset<SbbBuilding>> buildings;
    std::vector<LoadedAsset<SblBlockMesh>> blocks;
    std::vector<LoadedAsset<SblTemplate>> buildblocks;
    std::vector<LoadedTexture> textures;
    std::vector<BuildingRenderBuffer> building_render_buffers;
    std::vector<GpuBuildingRenderModel> building_gpu_models;
    BuildingRenderBuffer portal_render_buffer;
    GpuBuildingRenderModel portal_gpu_model;
    BuildingRenderBuffer portal_preview_render_buffer;
    GpuBuildingRenderModel portal_preview_gpu_model;
    BuildingRenderBuffer building_floor_preview_render_buffer;
    GpuBuildingRenderModel building_floor_preview_gpu_model;
    std::optional<std::size_t> portal_preview_index;
    std::optional<std::size_t> building_floor_preview_building;
    std::optional<std::uint32_t> building_floor_preview_floor{};
    bool building_floor_preview_dirty{true};
    bool render_buffers_dirty{true};
    MapScene city_scene_cache;
    std::uint64_t city_scene_cache_fingerprint{};
    bool city_scene_cache_valid{false};
    // Background city-scene rebuild. build_map_scene (roads + terrain field +
    // footprint stamping) runs on a worker thread so editing never blocks the
    // frame; the last-good scene keeps rendering until the new one is ready.
    bool city_scene_build_in_progress{};
    std::uint64_t city_scene_build_generation{};
    std::uint64_t city_scene_build_target_fingerprint{};
    std::optional<std::future<MapScene>> city_scene_build_future;
    // GPU meshes for the city road/curb tiles and terrain, uploaded once per scene
    // rebuild instead of re-submitting every triangle in immediate mode each frame.
    GpuBuildingRenderModel city_tile_gpu_model;
    GpuBuildingRenderModel terrain_gpu_model;
    bool city_scene_gpu_valid{false};
    // Terrain mesh is uploaded separately from the road/curb tiles: it only needs
    // re-upload when terrain-affecting inputs change (terrain_gpu_fingerprint),
    // so road/street edits skip the heavy terrain GPU upload.
    std::uint64_t terrain_gpu_fingerprint{};
    bool terrain_gpu_valid{false};
    bool portal_render_dirty{true};
    bool portal_preview_render_dirty{true};
    bool portal_render_textured{};
    bool portal_preview_render_textured{};
    // Async portal (piece) mesh rebuild: the CPU buffer build runs on a worker so
    // placing a piece never hitches the frame; the old mesh keeps showing until
    // the new buffer is ready and uploaded on the main thread.
    bool portal_rebuild_in_progress{};
    std::optional<std::future<BuildingRenderBuffer>> portal_rebuild_future;
    bool render_build_in_progress{};
    std::uint64_t render_build_generation{};
    std::optional<std::future<RenderBuildResult>> render_build_future;
    std::shared_ptr<RenderBuildProgress> render_build_progress;
    std::deque<std::size_t> pending_gpu_upload_indices;
    std::size_t pending_gpu_upload_index{};
    std::vector<std::string> render_logs;
    std::string status;
    // Per-file parse failures hit during load (e.g. an unsupported .sbb version).
    // Collected instead of thrown so one bad asset never aborts the whole map.
    std::vector<std::string> load_warnings;
    // Lazily-rendered 3D thumbnails for the template galleries, keyed by building
    // asset index. RenderTexture2D handles are owned here and freed on shutdown.
    std::vector<RenderTexture2D> building_thumbnails;
    std::vector<bool> building_thumbnail_ready;
    std::vector<RenderTexture2D> city_block_thumbnails;
    std::vector<bool> city_block_thumbnail_ready;
    // Undo/redo of the city map. Each entry is a full SbcMap snapshot plus the
    // label of the action that produced the state AFTER it. undo_stack.back() is
    // the current state; popping it (onto redo_stack) reverts. The action_log is a
    // human-readable backlog of everything done this session.
    std::vector<std::pair<std::string, SbcMap>> undo_stack;
    std::vector<std::pair<std::string, SbcMap>> redo_stack;
    std::vector<std::string> action_log;
    // Set on load: drives the loading screen that pregenerates thumbnails (and
    // shows progress) so the galleries are warm before you open them.
    bool pregenerating{true};
    std::size_t pregen_building_cursor{};
    std::size_t pregen_block_cursor{};
};

enum class EditorTab {
    City,
    Buildings,
    Pieces,
    Buildblocks,
    Blocks,
};

// Unified city-view tools (no Roads/Buildings/Pieces sub-mode). Select picks
// anything under the cursor (nearest wins) and gives the picked building/piece a
// gizmo; Building places a chosen building template; Road draws road chains; Piece
// places/draws pieces; Erase deletes whatever is clicked.
enum class CityTool {
    Select,
    Building,
    Road,
    Piece,
    Erase,
};

// What the Select tool currently has selected, so the gizmo/inspector know what to
// act on and the outline knows what to trace.
enum class CitySelectionKind {
    None,
    Building,
    Road,
    Node,
    Piece,
};

// Active tool in the Buildings (template) editor. Select picks a cell, PlaceBlock
// occupies the cell under the cursor with the chosen block, PaintTexture writes
// the chosen material onto the picked face.
enum class BuildingTool {
    Select,
    PlaceBlock,
    PaintTexture,
};

// Staged configuration for the Place Block tool, mirroring editor.exe's block
// panel: choose whether the placed cell has an exterior shell, an interior shell,
// or both (neither = nothing to place), pick the buildblock template (cell_type)
// that shapes its walls, and pick the material for each of the six faces on each
// side. Place writes exactly this into the target cell.
struct BlockToolConfig {
    bool exterior{true};
    bool interior{false};
    // Buildblock template index into building.block_names (cell_type). 0 = the
    // plain solid box.
    std::size_t buildblock_index{};
    // Material index (into building.materials) per face. Faces 0..5 are
    // Down, South, West, North, East, Up - the renderer's slot order.
    std::array<std::size_t, 6> exterior_face_material{};
    std::array<std::size_t, 6> interior_face_material{};
};

// Drag-gizmo handle on the selected city building. MoveX/MoveZ slide along the
// ground axes, MoveY lifts vertically, Rotate spins 90deg steps.
enum class GizmoAxis {
    None,
    MoveX,
    MoveY,
    MoveZ,
    Rotate,
};

struct CityGridPoint {
    std::int32_t x{};
    std::int32_t y{};
    std::int32_t z{};

    friend bool operator==(const CityGridPoint& a, const CityGridPoint& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
};

struct EditorUi {
    EditorTab tab{EditorTab::City};
    std::size_t selected_building{};
    std::size_t selected_block{};
    std::size_t selected_block_field{};
    std::size_t selected_block_patch{};
    std::size_t selected_block_point{};
    std::size_t selected_buildblock{};
    std::size_t selected_buildblock_layer{};
    std::size_t selected_buildblock_door{};
    std::size_t selected_buildblock_window{};
    std::size_t selected_buildblock_beam{};
    // Active tools per editor, set by the viewport-left toolbar.
    CityTool city_tool{CityTool::Select};
    CitySelectionKind city_selection{CitySelectionKind::None};
    // Multi-selection of placeable things (buildings + piece portals). The active
    // single selection (selected_city_building / selected_portal) is always the
    // last-clicked member; these sets carry the whole group for move/delete. Empty
    // means "just the single active selection".
    std::vector<std::size_t> selected_city_buildings;
    std::vector<std::size_t> selected_portals;
    // Box (rubber-band) selection: while dragging with the Select tool, everything
    // whose screen point falls in the rectangle gets selected. box_select_active is
    // set on press, the start/current points are viewport-space pixels.
    bool box_select_active{false};
    Vector2 box_select_start{};
    // Building/piece template to place with the Building/Piece tools.
    std::size_t place_building_template{};
    BuildingTool building_tool{BuildingTool::Select};
    // Place Block tool configuration (editor.exe-style block panel).
    BlockToolConfig block_config{};
    // Paint Texture tool: the material it writes and which side (exterior/interior
    // texture slots) it paints.
    std::size_t paint_material_index{};
    bool paint_interior{false};
    // Click-and-hold stamping: the last grid cell a held-drag stamped, so dragging
    // across cells places fast without re-stamping the same cell every frame. Reset
    // on mouse release. {INT32_MIN} means "nothing stamped yet this drag".
    std::optional<CityGridPoint> drag_stamp_cell;
    std::size_t selected_city_street{};
    std::size_t selected_city_intersection{};
    std::size_t selected_city_building{};
    std::optional<std::size_t> pending_road_intersection;
    // Roads-mode hover, recomputed each frame for the select preview: the node or
    // road a Shift+Click would grab. Node wins over road when both are under the
    // cursor, matching the click priority.
    std::optional<std::size_t> hover_city_intersection;
    std::optional<std::size_t> hover_city_street;
    // Selected piece is a single voxel (world grid cell), not the whole 8x8x8
    // portal: pieces are edited and highlighted one block at a time even though
    // they are stored grouped in portals internally.
    std::optional<CityGridPoint> selected_piece_voxel;
    std::optional<CityGridPoint> hover_piece_voxel;
    // Raw district word of the selected piece. v11 maps decode it through the named
    // city_blocks table; v10 (round) has no city_blocks, so the inspector reads this
    // directly to show the legacy cell's class/index.
    std::uint32_t selected_piece_word{};
    std::size_t selected_city_block{};
    std::size_t selected_city_block_layer{};
    std::size_t selected_city_block_slot{};
    std::size_t selected_piece_rotation{};
    std::size_t selected_portal{};
    bool portal_preview_textured{true};
    // City ground rendering toggles. On = full game roads/terrain meshes.
    // render_roads off => flat rectangle/square blockout for roads+intersections.
    // render_terrain off => terrain hidden entirely.
    bool render_roads{true};
    bool render_terrain{true};
    std::size_t selected_building_cell{};
    // Building-view hover: the cell index under the cursor this frame (for the
    // silhouette outline), recomputed each frame; nullopt when none.
    std::optional<std::size_t> building_hover_cell;
    std::size_t selected_building_field{};
    std::size_t selected_buildblock_field{};
    // Buildings tab view: false => whole building, true => only building_view_floor.
    bool building_view_floor_only{false};
    int building_view_floor{0};
    float camera_fly_speed{48.0F};
    // Animated camera focus (F key): when active, the camera eases from the start
    // pose to the target pose over focus_duration. Cancelled by manual fly input.
    bool focus_animating{false};
    float focus_t{};
    float focus_duration{0.6F};
    Vector3 focus_start_position{};
    Vector3 focus_start_target{};
    Vector3 focus_end_position{};
    Vector3 focus_end_target{};
    bool city_dirty{};
    bool building_dirty{};
    bool block_dirty{};
    bool buildblock_dirty{};
    // Active drag-gizmo handle for the selected city building (none when not
    // dragging). Set on mouse-press over a handle, cleared on release.
    GizmoAxis gizmo_active{GizmoAxis::None};
    // Building grid position captured when a gizmo drag started, so the drag maps
    // absolute mouse motion to grid cells instead of accumulating per-frame error.
    std::int32_t gizmo_drag_origin_x{};
    std::int32_t gizmo_drag_origin_z{};
    std::int32_t gizmo_drag_origin_y{};
    std::int32_t gizmo_drag_origin_rot{};
};

std::string lower_name(std::string_view value);
// Display label for a key, localized to the user's keyboard layout (printable
// keys via glfwGetKeyName, fixed names for the rest). Defined in app.cpp.
std::string key_label(int key);
std::vector<const SbbBuilding*> resolve_placement_buildings(const EditorProject& project);
std::optional<EditorProject> load_project_folder(const std::filesystem::path& folder, std::string& error);
std::vector<NamedBlockMesh> block_meshes(const EditorProject& project);
std::vector<NamedBuildblockTemplate> buildblock_templates(const EditorProject& project);
std::vector<std::string> project_texture_names(const EditorProject& project);
std::vector<std::string> project_grid_texture_names(const EditorProject& project);
void write_file_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes);
void set_status(EditorProject& project);

void service_render_rebuild(EditorProject& project);
void invalidate_portal_render_models(EditorProject& project);
GpuBuildingRenderModel upload_building_render_model(const BuildingRenderBuffer& buffer, const SbbBuilding& building, const EditorProject& project);
GpuBuildingRenderModel& building_tool_floor_preview_model(EditorProject& project, std::size_t building_index, std::optional<std::uint32_t> floor);
Texture2D load_city_window_gradient_texture();
// Loaded texture for a material name (matches project.textures by name), or
// nullptr when the project has no texture by that name. Used to show real texture
// thumbnails in the material pickers instead of name strings.
const Texture2D* texture_for_material(const EditorProject& project, std::string_view material_name);
// Looks up a loaded project texture by material name (case-insensitive); nullptr if
// absent. Shared with the mesh exporter.
const Texture2D* find_project_texture(const EditorProject& project, std::string_view name);
// Picks a material by clickable thumbnail grid (real texture when available, a
// color tile otherwise). Edits selected in place; returns true on change.
bool material_swatch_picker(const EditorProject& project, const char* id, const std::vector<std::string>& materials, std::size_t& selected);
void draw_mesh_faces(const SblBlockMesh& mesh, float scale, Color fill);
void draw_sbb_scene_for_app(const SbbBuilding& building, GpuBuildingRenderModel& model, const SbbCell* selected);
void draw_city_building_model(
    const SbbBuilding& template_file,
    GpuBuildingRenderModel& model,
    const SbcBuilding& placement,
    bool selected,
    bool use_lod,
    const Texture2D* city_window_texture = nullptr,
    bool hovered = false);
void draw_city_building_mask(GpuBuildingRenderModel& model, const SbcBuilding& placement);
void draw_outline_from_mask(RenderTexture2D& mask, Vector2 viewport_size, Color tint);
void draw_city_road_mask(const EditorProject& project, const MapScene& scene, std::size_t street_index);
void draw_building_cell_mask(const SbbBuilding& building, const SbbCell& cell);
void draw_city_piece_voxels(
    EditorProject& project,
    bool textured,
    std::optional<std::size_t> selected_portal = std::nullopt,
    bool draw_wires = false,
    bool isolate_selected = false);
void draw_city_piece_glow(CityGridPoint cell, Color color);
GpuBuildingRenderModel upload_city_tile_model(const BuildingRenderBuffer& buffer, const EditorProject& project);
GpuBuildingRenderModel upload_city_terrain_model(const BuildingRenderBuffer& buffer, const EditorProject& project);
void unload_city_render_model(GpuBuildingRenderModel& model);
void draw_city_scene(
    EditorProject& project,
    const MapScene& scene,
    const SbcStreet* selected_street,
    bool render_roads,
    bool render_terrain);
void draw_city_editor_overlay(const EditorProject& project, const MapScene& scene);
void draw_city_traffic_light_poles(const MapScene& scene);
void draw_city_cursor(CityGridPoint point, float unit_size, CityTool tool);
// Terrain-following grid drawn around the cursor while laying roads/pieces, so
// the click cell and its neighbours are legible against the ground.
void draw_city_edit_grid(CityGridPoint center, const MapScene& scene);
// Roads-mode selection/hover overlay: distinct styling for the selected node, the
// selected road, and the node/road under the cursor (hover_city_* in ui).
void draw_city_road_selection(const EditorProject& project, const EditorUi& ui, const MapScene& scene);
void draw_city_block_preview(const SbcCityBlock& city_block, const std::vector<NamedBlockMesh>& meshes, std::size_t selected_layer, std::size_t selected_slot);
void draw_buildblock_preview_3d(const BuildblockPreviewScene& scene);

Camera3D default_camera();
float adjust_camera_fly_speed(float current_speed, float wheel_delta);
Ray get_viewport_mouse_ray(const Camera3D& camera, Vector2 viewport_mouse_pos, Vector2 viewport_size);
Vector2 get_viewport_mouse_position(Vector2 viewport_pos, Vector2 viewport_size);
void focus_camera_on_city(Camera3D& camera, const EditorProject& project);
void update_free_fly_camera(Camera3D& camera, float fly_speed);
std::optional<CityGridPoint> city_grid_point_under_mouse(const Camera3D& camera, Vector2 viewport_mouse_pos, Vector2 viewport_size);
std::optional<CityGridPoint> city_grid_point_under_mouse_terrain(
    const Camera3D& camera,
    Vector2 viewport_mouse_pos,
    Vector2 viewport_size,
    const MapScene& scene);
std::optional<std::pair<std::size_t, std::size_t>> city_block_slot_under_mouse(
    const Camera3D& camera,
    Vector2 viewport_mouse_pos,
    Vector2 viewport_size);
bool select_building_cell_from_viewport(
    EditorProject& project,
    EditorUi& ui,
    const Camera3D& camera,
    Vector2 viewport_mouse_pos,
    Vector2 viewport_size,
    std::optional<std::uint32_t> floor,
    std::string& status);

std::optional<std::filesystem::path> dropped_folder();
std::vector<std::string> building_names(const EditorProject& project);
std::vector<std::size_t> occupied_cell_indices(const SbbBuilding& building);
std::vector<std::string> block_names(const EditorProject& project);
std::vector<std::string> city_building_names(const SbcMap& map);
std::vector<std::string> city_street_names(const SbcMap& map);
std::vector<std::string> city_block_names(const SbcMap& map);
std::vector<std::string> portal_names(const SbcMap& map);

std::uint32_t& building_cell_field(SbbCell& cell, std::size_t field_index);
void nudge_building_cell(EditorProject& project, EditorUi& ui, int delta);
void save_building(EditorProject& project, EditorUi& ui, std::string& status);
void nudge_buildblock_geometry(EditorProject& project, EditorUi& ui, int steps);
std::string block_field_value(const SblBlockMesh& block, const EditorUi& ui);
void nudge_block(EditorProject& project, EditorUi& ui, int delta);
void save_block(EditorProject& project, EditorUi& ui, std::string& status);
void save_buildblock(EditorProject& project, EditorUi& ui, std::string& status);
void nudge_city_building(EditorProject& project, EditorUi& ui, int dx, int dy, int dz, int drot);
void set_city_building_position(EditorProject& project, EditorUi& ui, CityGridPoint point);
void duplicate_city_building(EditorProject& project, EditorUi& ui);
void place_city_building_copy(EditorProject& project, EditorUi& ui, CityGridPoint point);
void delete_city_building(EditorProject& project, EditorUi& ui);
void nudge_city_street_endpoint(EditorProject& project, EditorUi& ui, int endpoint, int delta);
void nudge_city_street_width(EditorProject& project, EditorUi& ui, int side, int delta);
void nudge_city_street_district(EditorProject& project, EditorUi& ui, int delta);
void duplicate_city_street(EditorProject& project, EditorUi& ui);
void append_city_road_point(EditorProject& project, EditorUi& ui, CityGridPoint point);
void set_city_piece_voxel(EditorProject& project, EditorUi& ui, CityGridPoint point);
void clear_city_piece_voxel(EditorProject& project, CityGridPoint point, EditorUi& ui);
void delete_city_street(EditorProject& project, EditorUi& ui);
void nudge_city_block_slot(EditorProject& project, EditorUi& ui, int delta);
void clear_city_block_slot(EditorProject& project, EditorUi& ui);
void save_city(EditorProject& project, EditorUi& ui, std::string& status);
void mark_building_changed(EditorProject& project, EditorUi& ui);
void mark_buildblock_changed(EditorProject& project, EditorUi& ui);
void mark_block_changed(EditorProject& project, EditorUi& ui, SblBlockMesh& block);

void draw_portal_details(const SbcPortal& portal);
void draw_building_asset_panel(EditorProject& project, EditorUi& ui);
void draw_buildblock_gallery(EditorProject& project, EditorUi& ui);
// Renders building asset `index` to a small thumbnail RenderTexture (cached in
// project.building_thumbnails) and returns it, or nullptr if it can't be made.
const RenderTexture2D* building_thumbnail(EditorProject& project, std::size_t index);
void invalidate_building_thumbnail(EditorProject& project, std::size_t index);
void rebuild_single_building_render(EditorProject& project, std::size_t index);
const RenderTexture2D* city_block_thumbnail(EditorProject& project, std::size_t index);
// Template galleries: clickable 3D thumbnails of building / piece templates that
// set the active placement template for the Building / Piece tools.
void draw_city_building_template_gallery(EditorProject& project, EditorUi& ui);
void draw_city_piece_template_gallery(EditorProject& project, EditorUi& ui);
void draw_buildblock_preview_panel(EditorProject& project, EditorUi& ui);
void draw_building_inspector(EditorProject& project, EditorUi& ui, std::string& status);
// Viewport-left tool overlay (mode combobox + tool buttons). Anchored at the
// screen-space top-left of the render image (origin_x, origin_y). Returns true
// when the overlay is hovered, so the caller can suppress 3D picking for the click.
bool draw_viewport_toolbar(EditorProject& project, EditorUi& ui, float origin_x, float origin_y);
// Context-sensitive controls hint, drawn as key-cap shapes anchored to the
// bottom-right of the render image (origin + size). Shows only the bindings for
// the active tab/tool so it stays small and never needs scrolling.
void draw_controls_overlay(const EditorUi& ui, float origin_x, float origin_y, float width, float height);
void draw_block_inspector(EditorProject& project, EditorUi& ui, std::string& status);
void draw_buildblock_inspector(EditorProject& project, EditorUi& ui, std::string& status);
void apply_editor_imgui_style();
void load_default_imgui_layout();

} // namespace map_editor::app_internal
