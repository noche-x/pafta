#pragma once

#include "formats.hpp"
#include "scene.hpp"

#include <raylib.h>
#include <extras/IconsFontAwesome6.h>

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
#include <variant>
#include <vector>

namespace map_editor::app_internal {

template <typename T>
struct LoadedAsset {
    std::filesystem::path path;
    std::string name;
    T data;
    bool dirty{};
    bool newly_created{};
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
    struct ShadowFace {
        std::array<Vec3, 3> vertices{};
        std::array<std::int32_t, 3> adjacent{{-1, -1, -1}};
    };
    std::vector<GpuBuildingMeshChunk> chunks;
    std::vector<RenderLine> wire_lines;
    // Opaque triangle topology used by the stencil shadow pass. Built once
    // with the GPU model instead of rediscovering adjacency every frame.
    std::vector<ShadowFace> shadow_faces;
    // CPU fallback for platforms without the game's geometry-shader path.
    // Rebuild only when the sun direction changes, not once per frame and
    // once per placed instance.
    mutable std::array<std::vector<Vec3>, 4> shadow_volume_vertices;
    mutable std::array<bool, 4> shadow_volume_valid{};
    mutable Vec3 shadow_volume_light{};
    mutable Vec3 shadow_volume_extrusion{};
    mutable bool shadow_volume_light_valid{};
    // GPU copy of shadow_volume_vertices, uploaded once per rotation whenever
    // the CPU vertex list above is (re)built, and drawn instead of pushing
    // every vertex through rlVertex3f each frame. Positions only, unindexed.
    // Unloaded in unload_city_render_model(); the destructor here mirrors
    // GpuBuildingMeshChunk's pattern (UnloadMesh is safe on a zeroed Mesh).
    mutable std::array<Mesh, 4> shadow_volume_meshes{};
    mutable std::array<bool, 4> shadow_volume_mesh_uploaded{};
    std::size_t uploaded_vertices{};
    std::size_t uploaded_indices{};
    std::size_t uploaded_bytes{};

    GpuBuildingRenderModel() = default;
    GpuBuildingRenderModel(const GpuBuildingRenderModel&) = delete;
    GpuBuildingRenderModel& operator=(const GpuBuildingRenderModel&) = delete;
    GpuBuildingRenderModel(GpuBuildingRenderModel&&) noexcept = default;

    GpuBuildingRenderModel& operator=(GpuBuildingRenderModel&& other) noexcept {
        if (this != &other) {
            unload_shadow_volume_meshes();
            chunks = std::move(other.chunks);
            wire_lines = std::move(other.wire_lines);
            shadow_faces = std::move(other.shadow_faces);
            shadow_volume_vertices = std::move(other.shadow_volume_vertices);
            shadow_volume_valid = other.shadow_volume_valid;
            shadow_volume_light = other.shadow_volume_light;
            shadow_volume_extrusion = other.shadow_volume_extrusion;
            shadow_volume_light_valid = other.shadow_volume_light_valid;
            shadow_volume_meshes = other.shadow_volume_meshes;
            shadow_volume_mesh_uploaded = other.shadow_volume_mesh_uploaded;
            other.shadow_volume_meshes.fill(Mesh{});
            other.shadow_volume_mesh_uploaded.fill(false);
            uploaded_vertices = other.uploaded_vertices;
            uploaded_indices = other.uploaded_indices;
            uploaded_bytes = other.uploaded_bytes;
        }
        return *this;
    }

    ~GpuBuildingRenderModel() {
        unload_shadow_volume_meshes();
    }

    // UnloadMesh is safe on a default-constructed (zeroed) Mesh: raylib's
    // implementation checks vaoId/vboId before freeing.
    void unload_shadow_volume_meshes() {
        for (auto& mesh : shadow_volume_meshes) {
            if (mesh.vaoId != 0U || mesh.vboId != nullptr) {
                UnloadMesh(mesh);
            }
            mesh = {};
        }
        shadow_volume_mesh_uploaded.fill(false);
    }
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

// One building / buildblock / block-mesh editor undo step: the asset's data
// before the edit, located by index and guarded by name.
struct AssetSnapshot {
    std::size_t index{};
    std::string name;
    std::variant<SbbBuilding, SblTemplate, SblBlockMesh> data;
};

struct EditorProject {
    std::filesystem::path root;
    std::vector<std::pair<std::filesystem::path, std::vector<std::uint8_t>>> generated_city_assets;
    bool city_promotion_failed{};
    std::optional<std::vector<std::uint8_t>> city_bytes;
    std::optional<LoadedAsset<SbcMap>> city;
    std::vector<std::vector<std::uint8_t>> building_bytes;
    std::vector<std::vector<std::uint8_t>> block_bytes;
    std::vector<std::vector<std::uint8_t>> buildblock_bytes;
    std::vector<LoadedAsset<SbbBuilding>> buildings;
    std::vector<LoadedAsset<SblBlockMesh>> blocks;
    std::vector<LoadedAsset<SblTemplate>> buildblocks;
    std::vector<LoadedTexture> textures;
    std::vector<LoadedTexture> material_textures;
    // Derived render inputs, computed once from the loaded assets and reused every
    // frame instead of rebuilt per draw. project_grid_texture_names in particular
    // used to re-parse every CSX archive from disk on each call (it runs from the
    // per-frame portal render path). These are filled by ensure_render_input_cache
    // and invalidated (render_input_cache_valid=false) whenever blocks/buildblocks/
    // textures or the CSX texture set change. The CSX names are independent of the
    // in-memory assets, so they are cached separately and only invalidated on load.
    std::vector<NamedBlockMesh> cached_block_meshes;
    std::vector<NamedBuildblockTemplate> cached_buildblock_templates;
    std::vector<std::string> cached_texture_names;
    std::vector<std::string> cached_grid_texture_names;
    bool render_input_cache_valid{false};
    bool grid_texture_names_valid{false};
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
    // True while the user is mid-gesture (button held / gizmo drag): whole-scene
    // rebuilds wait for it to end so dragging stays smooth.
    bool defer_heavy_rebuilds{};
    // Per-placement resolved index into `buildings`, nullopt if unresolved.
    // Rebuilt lazily by resolve_placement_asset_indices() whenever it looks
    // stale (size mismatch or the city/asset list changed), so the three
    // per-frame call sites that used to re-run find_building_asset()'s
    // string-lowering search per placement can just index this instead.
    std::vector<std::optional<std::size_t>> placement_asset_indices;
    std::size_t placement_asset_indices_building_count{};
    std::uint64_t placement_asset_indices_fingerprint{};
    bool placement_assets_dirty{true};
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
    std::vector<RenderTexture2D> block_thumbnails;
    std::vector<bool> block_thumbnail_ready;
    // The Pieces-tab preview is derived from one city-block definition and the
    // current block-mesh catalog. Keep it across frames and rebuild on edits.
    CityBlockPreviewScene cached_city_block_preview_scene;
    std::optional<std::size_t> cached_city_block_preview_index;
    // The buildblock gallery and its 2D preview share this derived scene.
    // Rebuild it only when the selected template or its data changes.
    BuildblockPreviewScene cached_buildblock_preview_scene;
    std::optional<std::size_t> cached_buildblock_preview_index;
    // Undo/redo of the city map. Each entry is a full SbcMap snapshot plus the
    // label of the action that produced the state AFTER it. undo_stack.back() is
    // the current state; popping it (onto redo_stack) reverts. The action_log is a
    // human-readable backlog of everything done this session.
    std::vector<std::pair<std::string, SbcMap>> undo_stack;
    std::vector<std::pair<std::string, SbcMap>> redo_stack;
    std::vector<std::string> action_log;
    // Last committed city state. commit_city_edits diffs the live map against it,
    // so only real changes become undo steps; pending_city_label names the step.
    std::optional<SbcMap> city_baseline;
    std::string pending_city_label;
    // Same diff-based history for the asset editors, one per kind in variant
    // order (building, buildblock, block mesh). asset_baselines holds the last
    // committed state of each kind's selected asset.
    std::array<std::vector<AssetSnapshot>, 3> asset_undo_stacks;
    std::array<std::vector<AssetSnapshot>, 3> asset_redo_stacks;
    std::array<std::optional<AssetSnapshot>, 3> asset_baselines;
    // undo_stack depth at the last save/load. Entries above it are the
    // "changes since last save" list. Empty once the saved state has left the
    // history (dropped by the undo cap, or discarded by editing after undoing
    // past the save).
    std::optional<std::size_t> saved_undo_depth{0};
    // Thumbnails render lazily while their button is visible; the main loop
    // refills this each frame so a fresh gallery never stalls a frame.
    int thumbnail_budget{};
    // Set on load: drives the loading screen that pregenerates thumbnails (and
    // shows progress) so the galleries are warm before you open them.
    bool pregenerating{false};
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
    Anchor,
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
    MoveXZ,
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

// Rebindable editor actions. Order is the display order in Preferences.
enum class EditorAction {
    Save,
    Undo,
    Redo,
    FocusSelection,
    FrameCity,
    ToolSelect,
    ToolBuilding,
    ToolRoad,
    ToolPiece,
    ToolErase,
    RotateSelection,
    DuplicateSelection,
    DeleteSelection,
    NudgeLeft,
    NudgeRight,
    NudgeForward,
    NudgeBack,
    NudgeUp,
    NudgeDown,
    NextCell,
    TabEditor,
    TabCity,
    TabBuildings,
    TabPieces,
    WindowBuildingBrowser,
    WindowBlockBrowser,
    WindowBuildblock,
    WindowChanges,
    WindowShortcuts,
    WindowPreferences,
    ToggleShadows,
    ToggleSky,
    GraphicsDebug,
    WindowTools,
    WindowLibrary,
    WindowProperties,
    Count,
};

struct Shortcut {
    int key{};
    bool ctrl{};
    bool shift{};
    bool alt{};
    bool operator==(const Shortcut&) const = default;
};

struct ShortcutInfo {
    const char* name;
    const char* description;
    Shortcut default_binding;
};

// Small offscreen 3D view embedded in an ImGui window (building/block browser,
// buildblock window). Orbit by dragging the image.
struct PreviewView {
    RenderTexture2D target{};
    float yaw{0.8F};
    float pitch{0.55F};
    float zoom{1.0F};
};

enum class CameraDrag {
    None,
    Orbit,
    Pan,
};

struct EditorUi {
    EditorTab tab{EditorTab::City};
    // Rebindable shortcuts, indexed by EditorAction. Loaded from the prefs file.
    std::array<Shortcut, static_cast<std::size_t>(EditorAction::Count)> shortcuts{};
    // Action whose binding is being captured in Preferences, if any.
    std::optional<EditorAction> capturing_shortcut;
    bool show_building_browser{};
    bool show_block_browser{};
    bool show_buildblock_window{};
    // The Buildblock window had focus last frame; undo then targets buildblocks.
    bool buildblock_window_focused{};
    bool show_changes_window{};
    bool show_shortcuts_window{};
    bool show_preferences_window{};
    bool show_graphics_debug{};
    bool show_library_window{true};  // the Outliner
    bool show_asset_drawer{true};
    bool asset_drawer_open{true};
    bool show_properties_window{true};
    // Set for one frame by Windows > Reset layout: windows re-take their
    // default positions and sizes.
    bool reset_layout{};
    std::size_t browser_building{};
    std::size_t browser_block{};
    PreviewView building_preview;
    PreviewView block_preview;
    PreviewView buildblock_preview;
    // Buildblock face editor: which item is selected and what is being dragged.
    // kind: 0 face bounds, 1 window, 2 beam, 3 door.
    int bb_selected_kind{0};
    int bb_selected_index{0};
    int bb_drag_kind{-1};
    int bb_drag_index{-1};
    int bb_drag_corner{-1}; // -1 move, 0..3 corners (x0y0, x1y0, x1y1, x0y1)
    float bb_drag_off_x{};
    float bb_drag_off_y{};
    bool bb_drag_changed{};
    int bb_snap_steps{16};
    // Pending tab switch from a tab-bar click or shortcut; the main loop applies
    // it so the camera reset happens in one place.
    std::optional<EditorTab> requested_tab;
    float ui_font_size{15.0F};
    bool city_library_roads{};
    bool show_building_library{true};
    // One persistent search query per workspace tab. Keeping the queries separate
    // prevents a Buildings search from making another tab appear empty.
    std::array<std::array<char, 128>, 6> asset_search{};
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
    // Rotation (0-3 quarter turns) for the next building the Building tool places.
    std::int32_t place_building_rotation{};
    // Why the Building-tool preview would overlap something (empty = clear).
    std::string placement_conflict;
    BuildingTool building_tool{BuildingTool::Select};
    // Place Block tool configuration (editor.exe-style block panel).
    BlockToolConfig block_config{};
    // Paint Texture tool: the material it writes and which side (exterior/interior
    // texture slots) it paints.
    std::vector<std::size_t> selected_materials;
    std::array<char, 64> new_material_name{};
    std::size_t material_selection_building{static_cast<std::size_t>(-1)};
    GizmoAxis anchor_axis{GizmoAxis::None};
    Vector2 anchor_drag_mouse{};
    Vector2 gizmo_drag_mouse{};
    std::vector<std::pair<std::size_t, SbcBuilding>> gizmo_start_buildings;
    std::size_t last_clicked_building{static_cast<std::size_t>(-1)};
    double last_building_click_time{-1};
    Vector2 last_building_click_mouse{};
    std::array<std::uint32_t, 3> anchor_drag_start{};
    std::size_t paint_material_index{};
    bool paint_interior{false};
    // Click-and-hold stamping: the last grid cell a held-drag stamped, so dragging
    // across cells places fast without re-stamping the same cell every frame. Reset
    // on mouse release. {INT32_MIN} means "nothing stamped yet this drag".
    std::optional<CityGridPoint> drag_stamp_cell;
    // Building cells the current Add-block drag filled (cleared on release).
    std::vector<std::size_t> stroke_block_cells;
    // Cell and face slot the Paint drag last painted (cleared on release).
    std::optional<std::pair<std::size_t, std::uint32_t>> drag_paint_face;
    std::size_t selected_city_street{};
    std::size_t selected_city_intersection{};
    std::size_t selected_city_building{};
    // Road tool start point. The first click only records it (snapped to a nearby
    // node or road); nodes are created when the second click lays a segment.
    std::optional<CityGridPoint> pending_road_start;
    // Total width in cells for roads the Road tool lays.
    int new_road_width{6};
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
    std::size_t place_piece_mesh{};
    std::size_t place_piece_texture{};
    std::size_t selected_portal{};
    std::array<int, 3> new_portal_cell{};
    bool portal_preview_textured{true};
    // City preview mode. Both true shows the game terrain and finished road mesh;
    // both false shows only the native editor-style node and segment overlays.
    bool render_roads{true};
    bool render_terrain{true};
    float world_time_hours{12.0F};
    int render_quality{1}; // 0 low, 1 balanced, 2 high
    bool render_shadows{true};
    bool render_sky{true};
    float shadow_distance{700.0F};
    std::size_t selected_building_cell{};
    // Building-view hover: the cell index under the cursor this frame (for the
    // silhouette outline), recomputed each frame; nullopt when none.
    std::optional<std::size_t> building_hover_cell;
    std::optional<std::uint32_t> building_hover_face;
    std::size_t selected_building_field{};
    std::size_t selected_buildblock_field{};
    // Buildings tab view: false => whole building, true => only building_view_floor.
    bool building_view_floor_only{false};
    int building_view_floor{0};
    float camera_fly_speed{48.0F};
    // Orbit (Alt+left drag) / pan (middle drag) in progress, and its pivot.
    CameraDrag camera_drag{CameraDrag::None};
    Vector3 camera_pivot{};
    float camera_pivot_distance{};
    // Building/Road tool click waiting for release; placement fires only if the
    // cursor is still near place_click_start.
    bool place_click_pending{};
    Vector2 place_click_start{};
    // Animated camera focus (F key): when active, the camera eases from the start
    // pose to the target pose over focus_duration. Cancelled by manual fly input.
    bool focus_animating{false};
    bool focus_selection_requested{};
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
const LoadedAsset<SbbBuilding>* find_building_asset(const EditorProject& project, std::string_view name);
// Toggles the extra game-surface color attachments used by building shaders.
// Cheap to call repeatedly: a no-op when the requested state already matches.
void set_game_render_depth_output(bool enabled);
// Rebuilds project.placement_asset_indices if it looks stale, then returns it.
// Use this instead of find_building_asset() in per-frame, per-placement loops.
const std::vector<std::optional<std::size_t>>& resolve_placement_asset_indices(EditorProject& project);
// Display label for a key, localized to the user's keyboard layout (printable
// keys via glfwGetKeyName, fixed names for the rest). Defined in app.cpp.
std::string key_label(int key);

// ui_windows.cpp: shortcuts, preferences, and the floating tool windows.
const ShortcutInfo& shortcut_info(EditorAction action);
void reset_shortcuts(EditorUi& ui);
bool action_pressed(const EditorUi& ui, EditorAction action);
std::string shortcut_label(const Shortcut& shortcut);
std::string action_shortcut_label(const EditorUi& ui, EditorAction action);
std::filesystem::path preferences_path();
void load_preferences(EditorUi& ui);
void save_preferences(const EditorUi& ui);
#ifdef __EMSCRIPTEN__
void web_restore_file(const std::filesystem::path& path, const char* key);
void web_store_file(const std::filesystem::path& path, const char* key);
#endif
void apply_editor_style(float font_size);
void draw_preferences_contents(EditorUi& ui);
void draw_shortcuts_table(EditorUi& ui, bool editable);
void draw_building_browser_window(EditorProject& project, EditorUi& ui);
void draw_block_browser_window(EditorProject& project, EditorUi& ui);
void draw_buildblock_window(EditorProject& project, EditorUi& ui, std::string& status);
void draw_changes_window(EditorProject& project, EditorUi& ui, std::string& status);
void draw_shortcuts_window(EditorUi& ui);
void draw_preferences_window(EditorUi& ui);
void unload_preview_views(EditorUi& ui);
// Game limits recovered from subrosa.exe array capacities. Draws red/yellow
// lines for anything at or over a limit; returns the number of problems.
int draw_map_limit_warnings(EditorProject& project);
void draw_buildblock_face_editor(EditorProject& project, EditorUi& ui);
void draw_buildblock_face_canvas(EditorProject& project, EditorUi& ui);
// ui_workspace.cpp: the context-aware Tools / Library / Properties windows.
// Toolbar (top) and asset drawer (bottom) drawn over the scene canvas; call
// inside the canvas window after the scene image.
void draw_viewport_overlays(EditorProject* project, EditorUi& ui, Camera3D& camera, std::string& status, bool& open_folder_requested);
void draw_outliner_window(EditorProject* project, EditorUi& ui);
// Name cut to fit `width` pixels with "..." (ui_windows.cpp).
std::string fit_label(const std::string& name, float width);
// Left-hand label at a shared column for the next widget (ui_panels.cpp).
void field_label(const char* label);
void draw_properties_window(EditorProject* project, EditorUi& ui, std::string& status);
bool asset_search_matches(const EditorUi& ui, std::string_view name);
void toggle_in_selection(std::vector<std::size_t>& set, std::size_t index);
void focus_current_selection(EditorProject& project, EditorUi& ui, Camera3D& camera);
void begin_city_action(EditorProject& project, EditorUi& ui, std::string label);
const LoadedAsset<SblTemplate>* buildblock_for_cell(const EditorProject& project, const SbbBuilding& building, const SbbCell& cell);
void draw_gpu_building_model_public(GpuBuildingRenderModel& model, Vector3 origin, std::int32_t rotation);
void draw_mesh_faces(const SblBlockMesh& mesh, Vec3 origin, float scale, Color fill);
void undo_city(EditorProject& project, EditorUi& ui, std::string& status);
void redo_city(EditorProject& project, EditorUi& ui, std::string& status);
// Records finished edits as undo steps; call once per frame. Skips frames with a
// drag or widget edit in progress so each gesture becomes one step.
void commit_pending_edits(EditorProject& project, const EditorUi& ui);
// Undo/redo for what is being edited: buildblock history while the Buildblock
// window has focus, else city history in City/Pieces and the building or
// block-mesh history in their editors.
bool can_undo(const EditorProject& project, const EditorUi& ui, bool redo);
void undo_edit(EditorProject& project, EditorUi& ui, std::string& status, bool redo);
void edit_city_building(EditorProject& project, EditorUi& ui, std::size_t placement_index);
std::vector<const SbbBuilding*> resolve_placement_buildings(const EditorProject& project);
std::optional<EditorProject> load_project_folder(const std::filesystem::path& folder, std::string& error);
// Derived render inputs. The block-mesh catalog, buildblock templates, texture
// names, and grid (CSX) texture names are computed once and cached on the project
// (see EditorProject::cached_* / ensure_render_input_cache); these accessors return
// references into that cache. Call invalidate_render_input_cache after mutating
// blocks/buildblocks/textures so the next access rebuilds the in-memory caches (the
// CSX grid names only change on load). build_grid_texture_names is the disk-parsing
// builder behind the grid-name cache, exposed for the load path.
std::vector<std::string> build_grid_texture_names(const EditorProject& project);
void ensure_render_input_cache(EditorProject& project);
void invalidate_render_input_cache(EditorProject& project);
const std::vector<NamedBlockMesh>& block_meshes(EditorProject& project);
const std::vector<NamedBuildblockTemplate>& buildblock_templates(EditorProject& project);
const std::vector<std::string>& project_texture_names(EditorProject& project);
const std::vector<std::string>& project_grid_texture_names(EditorProject& project);
void write_file_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes);
void set_status(EditorProject& project);

void service_render_rebuild(EditorProject& project);
void invalidate_portal_render_models(EditorProject& project);
void invalidate_city_block_preview_scene(EditorProject& project);
GpuBuildingRenderModel upload_building_render_model(const BuildingRenderBuffer& buffer, const SbbBuilding& building, const EditorProject& project);
GpuBuildingRenderModel& building_tool_floor_preview_model(EditorProject& project, std::size_t building_index, std::optional<std::uint32_t> floor);
RenderTexture2D load_game_render_texture(int width, int height);
void unload_game_render_texture(RenderTexture2D target);
// Raylib's WebGL3 backend in this bundled version does not bind the target FBO
// from BeginTextureMode. Keep texture-mode setup in one wrapper so web and
// desktop render to the same attachment.
void begin_game_texture_mode(RenderTexture2D target);
void end_game_texture_mode();
Texture2D game_render_packed_depth(RenderTexture2D target);
void clear_game_render_packed_depth();
void clear_game_render_shadow_factor();
void resolve_game_render_texture(RenderTexture2D target);
const RenderTexture2D* game_render_display_texture(RenderTexture2D target);
void draw_game_sky(const Camera3D& camera, Vector2 viewport_size, Texture2D depth_texture);
void set_game_surface_camera(Vector3 position);
void set_game_surface_terrain(bool enabled);
void set_game_world_time(float hours);
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
void draw_sbb_scene_for_app(
    const SbbBuilding& building,
    GpuBuildingRenderModel& model,
    const SbbCell* selected,
    std::uint32_t guide_floor);
void draw_city_building_model(
    const SbbBuilding& template_file,
    GpuBuildingRenderModel& model,
    const SbcBuilding& placement,
    bool selected,
    bool use_lod,
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
void draw_city_road_markings(const MapScene& scene);
void draw_city_editor_overlay(const EditorProject& project, const MapScene& scene);
void draw_city_traffic_light_poles(const MapScene& scene);
void draw_city_shadow_volumes(EditorProject& project, const Camera3D& camera, float max_distance);
void composite_city_shadows(Vector2 viewport_size);
void draw_city_cursor(CityGridPoint point, float unit_size, CityTool tool);
// Terrain-following grid drawn around the cursor while laying roads/pieces, so
// the click cell and its neighbours are legible against the ground.
void draw_city_edit_grid(CityGridPoint center, const MapScene& scene);
// Roads-mode selection/hover overlay: distinct styling for the selected node, the
// selected road, and the node/road under the cursor (hover_city_* in ui).
void draw_city_road_selection(const EditorProject& project, const EditorUi& ui, const MapScene& scene);
void draw_city_block_preview(EditorProject& project, const SbcCityBlock& city_block, const std::vector<NamedBlockMesh>& meshes, std::size_t city_block_index, std::size_t selected_layer, std::size_t selected_slot);
void draw_buildblock_preview_3d(const BuildblockPreviewScene& scene);

Camera3D default_camera();
float adjust_camera_fly_speed(float current_speed, float wheel_delta);
Ray get_viewport_mouse_ray(const Camera3D& camera, Vector2 viewport_mouse_pos, Vector2 viewport_size);
Vector2 get_viewport_mouse_position(Vector2 viewport_pos, Vector2 viewport_size);
void focus_camera_on_city(Camera3D& camera, const EditorProject& project);
void update_free_fly_camera(Camera3D& camera, float fly_speed);
// Alt+left drag orbits around the point under the cursor, middle drag pans, and
// the wheel zooms toward the cursor. `scene` supplies terrain for the pivot.
void update_orbit_camera(Camera3D& camera, EditorUi& ui, Vector2 viewport_mouse_pos, Vector2 viewport_size,
                         bool viewport_hovered, const MapScene* scene);
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
void delete_city_street(EditorProject& project, EditorUi& ui);
void nudge_city_block_slot(EditorProject& project, EditorUi& ui, int delta);
void clear_city_block_slot(EditorProject& project, EditorUi& ui);
void edit_building(EditorProject& project, EditorUi& ui, std::size_t asset_index);
void recalculate_building_size(SbbBuilding& building);
void mark_building_changed(EditorProject& project, EditorUi& ui);
void mark_buildblock_changed(EditorProject& project, EditorUi& ui);
void rebuild_buildings_using_buildblock(EditorProject& project, const std::string& buildblock_name);
void mark_block_changed(EditorProject& project, EditorUi& ui, SblBlockMesh& block);

void draw_portal_details(const SbcPortal& portal);
// Renders building asset `index` to a small thumbnail RenderTexture (cached in
// project.building_thumbnails) and returns it, or nullptr if it can't be made.
const RenderTexture2D* building_thumbnail(EditorProject& project, std::size_t index);
void invalidate_building_thumbnail(EditorProject& project, std::size_t index);
void rebuild_single_building_render(EditorProject& project, std::size_t index);
const RenderTexture2D* city_block_thumbnail(EditorProject& project, std::size_t index);
// Lazily rendered block-mesh thumbnail (ui_windows.cpp); nullptr until ready.
const RenderTexture2D* block_thumbnail(EditorProject& project, std::size_t index);
// Template galleries: clickable 3D thumbnails of building / piece templates that
// set the active placement template for the Building / Piece tools.
void draw_buildblock_preview_panel(EditorProject& project, EditorUi& ui);
void invalidate_buildblock_preview_scene(EditorProject& project);
const BuildblockPreviewScene& buildblock_preview_scene(EditorProject& project, std::size_t index);
void draw_building_inspector(EditorProject& project, EditorUi& ui, std::string& status);
// Viewport-left tool overlay (mode combobox + tool buttons). Anchored at the
// screen-space top-left of the render image (origin_x, origin_y). Returns true
// when the overlay is hovered, so the caller can suppress 3D picking for the click.
// Context-sensitive controls hint, drawn as key-cap shapes anchored to the
// bottom-right of the render image (origin + size). Shows only the bindings for
// the active tab/tool so it stays small and never needs scrolling.
void draw_block_inspector(EditorProject& project, EditorUi& ui, std::string& status);
void draw_buildblock_inspector(EditorProject& project, EditorUi& ui, std::string& status);
void draw_struct_inspector_header(const char* file_type, const char* struct_name, const std::string& record_name);
void draw_struct_section(const char* name);
std::string hex_u32(std::uint32_t value);

} // namespace map_editor::app_internal
