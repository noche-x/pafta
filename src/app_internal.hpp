// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "formats.hpp"
#include "editor_state.hpp"
#include "scene.hpp"

#include <raylib.h>
#include <extras/IconsFontAwesome6.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

struct ImDrawData;
struct ImDrawList;
struct ImVec2;

namespace pafta::app_internal {

template <typename T>
struct LoadedAsset {
    std::filesystem::path path;
    std::string name;
    T data;
    bool dirty{};
    bool newly_created{};
};

// One material range of a GpuMesh's index buffer.
struct GpuMeshPart {
    std::uint32_t first_index{};
    std::uint32_t index_count{};
    std::uint32_t material_id{};
    unsigned int texture{};           // diffuse; 0 = untextured (uses color)
    unsigned int material_texture{};  // metallic/roughness map
    Color color{WHITE};
    bool window_material{};
    // World-space bounds of one part of a chunked shadow volume, for view culling.
    bool has_bounds{};
    Vector3 bounds_min{};
    Vector3 bounds_max{};
};

// A whole render buffer in one VAO: interleaved position, uv, and normal, with
// 32-bit indices, split into material parts. Drawn with glDrawElementsInstanced so
// every placement of a template shares one draw per part (see
// draw_gpu_mesh_instances).
struct GpuMesh {
    unsigned int vao{};
    unsigned int vbo{};
    unsigned int ebo{};
    std::uint32_t vertex_count{};
    std::uint32_t index_count{};
    std::vector<GpuMeshPart> parts;

    GpuMesh() = default;
    GpuMesh(const GpuMesh&) = delete;
    GpuMesh& operator=(const GpuMesh&) = delete;
    GpuMesh(GpuMesh&& other) noexcept { *this = std::move(other); }
    GpuMesh& operator=(GpuMesh&& other) noexcept {
        if (this != &other) {
            unload();
            vao = std::exchange(other.vao, 0U);
            vbo = std::exchange(other.vbo, 0U);
            ebo = std::exchange(other.ebo, 0U);
            vertex_count = std::exchange(other.vertex_count, 0U);
            index_count = std::exchange(other.index_count, 0U);
            parts = std::move(other.parts);
        }
        return *this;
    }
    ~GpuMesh() { unload(); }
    [[nodiscard]] bool empty() const { return index_count == 0U; }
    void unload();  // render_gpu.cpp
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
    GpuMesh mesh;
    std::vector<RenderLine> wire_lines;
    // A stencil shadow volume that works for any light direction, built once with the
    // model: caps for each face and a quad for each edge, stretched in the vertex
    // shader. Placements share it through instancing; the sun and stretch length are
    // uniforms.
    GpuMesh shadow_volume;
    // The highest vertex y, for the sector-based stretch length.
    float shadow_top{};
    std::size_t uploaded_bytes{};

    [[nodiscard]] bool empty() const { return mesh.empty(); }
};

enum class GfxDebugDrawCategory : std::size_t {
    CityCells,
    RoadsCurbs,
    Terrain,
    TrafficLightPoles,
    Pieces,
    WireOverlay,
    ShadowVolumes,
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

// Performance settings the renderer reads (copied from EditorUi every frame).
struct RenderSettings {
    bool near_lod{true};           // rooms and glass within kNearLodDistance
    // Rooms only for the placements in interior_placements, at any distance.
    bool interiors_selected_only{false};
    std::vector<std::size_t> interior_placements;
    float building_distance{0.0F}; // 0 = draw every placement
    bool half_textures{false};     // halve texture resolution at load
    bool street_objects{true};     // traffic lights and street signs
    bool live_reflections{true};   // recapture the sky reflection as the camera moves
    int anisotropy{16};            // texture filtering; the game uses 16 (0x278774)
};
extern RenderSettings g_render_settings;

// Times a phase only when the debug overlay is on, and does nothing otherwise. Lets
// drawing code use one RAII line instead of checking g_gfx_debug_stats everywhere.
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

// One undo step in the building, buildblock, or block-mesh editor: the asset's data
// from before the edit, found by index and checked by name.
struct AssetSnapshot {
    std::size_t index{};
    std::string name;
    std::variant<SbbBuilding, SblTemplate, SblBlockMesh> data;
    // A rename step restores `name` when the live asset is still `renamed_to`.
    std::optional<std::string> renamed_to;
};

// Shadow volume vertices. The shader only reads which way the normal points (a sign
// test against the light) and the kind of vertex, so they fit in 20 bytes instead of
// GpuVertex's 36.
struct ShadowVertex {
    float position[3];
    std::uint16_t normal[3];  // unit normal as half floats
    std::uint8_t kind;
    std::uint8_t pad;
};
static_assert(sizeof(ShadowVertex) == 20);

struct ArchiveWorkspace {
    CsxArchive archive{0xc5f17e01U, 0, 0, {}};
    std::array<char, 1024> input_path{};
    // The map folder test.csx is written to (the game only reads that name).
    std::array<char, 1024> output_path{};
    std::vector<std::size_t> selected;
    // The row Shift+click selects from.
    std::optional<std::size_t> anchor;
    std::array<char, 128> filter{};
    std::string message;
    bool dirty{};
    std::vector<std::optional<std::string>> sbl_labels;
    // Hover previews of texture entries, by entry index. Cleared with the labels
    // whenever the entries change.
    std::unordered_map<std::size_t, LoadedTexture> texture_previews;
    // The archive follows the open map until you change it by hand. map_fingerprint
    // tells when the map's assets changed; a refill runs a few steps per frame.
    bool follows_map{true};
    std::uint64_t map_fingerprint{};
    std::optional<CsxArchive> refill;
    std::size_t refill_step{};
    // Files that could not be added, why, and their automatic fix if there is one.
    struct FailedImport {
        std::filesystem::path path;
        std::string error;
        std::optional<FileRepair> repair;
    };
    std::vector<FailedImport> failed_imports;
};

// A source file that failed to load, kept so it can be fixed later. repair is the
// automatic fix found for it, if any.
struct FailedAsset {
    enum class Kind { Building, Sbl, Other };
    Kind kind{Kind::Other};
    std::string name;
    std::filesystem::path source;
    std::vector<std::uint8_t> bytes;
    std::string message;  // the same text as its load_warnings line
    std::optional<FileRepair> repair;
    // Came from Import into map, not from the map folder, so it doesn't block Save
    // and can be dismissed.
    bool imported{};
};

struct EditorProject {
    std::filesystem::path root;
    // Files that failed to load, with their bytes, for "Try auto-fixing".
    std::vector<FailedAsset> failed_assets;
    // Loose files beat test.csx copies of the same asset (chosen when opening).
    bool loose_files_win{};
    // Loaded texture and unknown archive payloads survive moving or deleting the source.
    std::vector<CsxEntry> archive_entries;
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
    // The loose .sis names that were found. The building stores names and positions;
    // the game loads the item-set files from its data/itemset folder.
    std::vector<std::string> item_set_catalog;
    // Item set contents by lower-case name, and the meshes of the item types they use
    // (data/item/<type>.it3) by lower-case type name.
    std::unordered_map<std::string, std::vector<SisItem>> item_sets;
    std::unordered_map<std::string, It3Mesh> item_meshes;
    std::vector<LoadedTexture> textures;
    std::vector<LoadedTexture> material_textures;
    // Mesh and template inputs are rebuilt after asset edits. CSX texture numbers
    // follow the archive order on disk and have their own cache, filled at load.
    std::vector<NamedBlockMesh> cached_block_meshes;
    std::vector<NamedBuildblockTemplate> cached_buildblock_templates;
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
    bool building_floor_preview_roofs{true};
    bool building_floor_preview_dirty{true};
    bool render_buffers_dirty{true};
    // True while the user is in the middle of a gesture (button held or gizmo drag).
    // Whole-scene rebuilds wait for it to end so dragging stays smooth.
    bool defer_heavy_rebuilds{};
    // The city scene fingerprint of the map right now (set every City frame), to tell
    // when a roads and terrain rebuild is waiting.
    std::uint64_t city_scene_current_fingerprint{};
    // SBB edits change terrain stamps and world storage without changing the SBC.
    std::uint64_t city_building_revision{};
    // For each placement, its index into `buildings`, or nullopt if it wasn't found.
    // resolve_placement_asset_indices() rebuilds it when it looks out of date (the
    // size differs, or the city or asset list changed). The three places that used to
    // run find_building_asset()'s lower-casing name search for every placement every
    // frame can just read this instead.
    std::vector<std::optional<std::size_t>> placement_asset_indices;
    std::size_t placement_asset_indices_building_count{};
    std::uint64_t placement_asset_indices_fingerprint{};
    // Placements whose cells are overwritten by a later overlapping placement get their
    // own model without those cells (refresh_placement_override_models).
    std::unordered_map<std::size_t, GpuBuildingRenderModel> placement_override_models;
    // Near-view meshes for each template (building_near_mesh). far_vao links each to
    // the shell model it was built next to, so editing a template rebuilds it.
    struct NearBuildingModel {
        GpuMesh mesh;
        // Shadow volume of the room triangles (without the glass). The game's
        // RenderQueuedShadowVolumeAdjacencyPass @946d0 draws the room models of
        // near sectors with a fixed 32-unit stretch.
        GpuMesh room_shadow_volume;
        double last_used{};
        unsigned int far_vao{};
    };
    std::unordered_map<std::size_t, NearBuildingModel> building_near_models;
    // Near meshes and room volumes are built on a worker (baking took over 100 ms for a
    // skyscraper). The main thread only uploads the result.
    struct NearBuildingBuffers {
        BuildingRenderBuffer mesh;
        std::vector<ShadowVertex> room_shadow_vertices;
        std::vector<std::uint32_t> room_shadow_indices;
    };
    struct NearBuildingJob {
        unsigned int far_vao{};
        std::future<NearBuildingBuffers> buffers;
    };
    std::unordered_map<std::size_t, NearBuildingJob> building_near_jobs;
    std::uint64_t placement_override_fingerprint{};
    bool placement_assets_dirty{true};
    MapScene city_scene_cache;
    std::uint64_t city_scene_cache_fingerprint{};
    bool city_scene_cache_valid{false};
    // Rebuilding the city scene in the background. build_map_scene (roads, terrain,
    // and footprint stamps) runs on a worker thread so editing never holds up the
    // frame. The last good scene keeps drawing until the new one is ready.
    bool city_scene_build_in_progress{};
    std::uint64_t city_scene_build_generation{};
    std::uint64_t city_scene_build_target_fingerprint{};
    std::optional<std::future<MapScene>> city_scene_build_future;
    // GPU meshes for the city's road and curb tiles and terrain, uploaded once per
    // scene rebuild instead of sending every triangle in immediate mode each frame.
    GpuBuildingRenderModel city_tile_gpu_model;
    GpuBuildingRenderModel terrain_gpu_model;
    bool city_scene_gpu_valid{false};
    // The terrain mesh is uploaded separately from the road and curb tiles. It only
    // needs uploading again when something that affects terrain changes
    // (terrain_gpu_fingerprint), so road and street edits skip the slow terrain upload.
    std::uint64_t terrain_gpu_fingerprint{};
    // city_tile_fingerprint of the road and curb tile model that was uploaded.
    std::optional<std::uint64_t> city_tile_gpu_fingerprint;
    bool terrain_gpu_valid{false};
    bool portal_render_dirty{true};
    bool portal_preview_render_dirty{true};
    bool portal_render_textured{};
    bool portal_preview_render_textured{};
    // Rebuilding the portal (piece) mesh in the background. The CPU buffer is built on
    // a worker so placing a piece never stutters. The old mesh keeps showing until the
    // new buffer is ready and uploaded on the main thread.
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
    // Files that failed to parse during load (for example an .sbb version we don't
    // support). They are collected instead of thrown, so one bad asset never stops the
    // whole map from loading.
    std::vector<std::string> load_warnings;
    // Save and CSX rebuilding must never quietly leave out a missing map or geometry
    // file. Item preview warnings don't stop saving.
    std::vector<std::string> save_blocking_load_errors;
    // 3D thumbnails for the template galleries, drawn when first needed and keyed by
    // building asset index. The RenderTexture2D handles are owned here and freed on
    // shutdown.
    std::vector<RenderTexture2D> building_thumbnails;
    std::vector<bool> building_thumbnail_ready;
    std::vector<RenderTexture2D> city_block_thumbnails;
    std::vector<bool> city_block_thumbnail_ready;
    std::vector<RenderTexture2D> block_thumbnails;
    std::vector<bool> block_thumbnail_ready;
    // Item set thumbnails by lower-case set name.
    std::unordered_map<std::string, RenderTexture2D> item_set_thumbnails;
    // The Pieces tab preview comes from one city-block definition and the current
    // block-mesh catalog. Keep it between frames and rebuild it after edits.
    CityBlockPreviewScene cached_city_block_preview_scene;
    std::optional<std::size_t> cached_city_block_preview_index;
    // The buildblock gallery and its 2D preview share this scene.
    // Only rebuild it when the selected template or its data changes.
    BuildblockPreviewScene cached_buildblock_preview_scene;
    std::optional<std::size_t> cached_buildblock_preview_index;
    // Undo and redo for the city map. Each entry is a full SbcMap snapshot plus the
    // name of the action that led to it. undo_stack.back() is the current state, and
    // moving it onto redo_stack undoes it. action_log is a readable list of everything
    // done this session.
    std::vector<std::pair<std::string, SbcMap>> undo_stack;
    std::vector<std::pair<std::string, SbcMap>> redo_stack;
    std::vector<std::string> action_log;
    // The last committed city state. commit_city_edits compares the live map with it, so
    // only real changes become undo steps; pending_city_label names the step.
    std::optional<SbcMap> city_baseline;
    std::string pending_city_label;
    // The same compare-based history for the asset editors, one for each kind, in
    // variant order (building, buildblock, block mesh). asset_baselines holds the last
    // committed state of each kind's selected asset.
    std::array<std::vector<AssetSnapshot>, 3> asset_undo_stacks;
    std::array<std::vector<AssetSnapshot>, 3> asset_redo_stacks;
    std::array<std::optional<AssetSnapshot>, 3> asset_baselines;
    // The undo_stack depth at the last save or load. Entries above it are the "changes
    // since last save" list. Empty once the saved state has left the history (dropped by
    // the undo limit, or thrown away by editing after undoing past the save).
    std::optional<std::size_t> saved_undo_depth{0};
    // Thumbnails are drawn when their button is visible. The main loop refills this
    // every frame, so opening a new gallery never stalls a frame.
    int thumbnail_budget{};
    // Set on load. It drives the loading screen that makes thumbnails ahead of time
    // (and shows progress), so the galleries are ready before you open them.
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

// City view tools (there are no Roads/Buildings/Pieces sub-modes). Select picks
// anything under the cursor (closest wins) and gives the picked building or piece a
// gizmo. Building places a chosen building template, Road draws chains of roads,
// Piece places and paints pieces, and Erase deletes whatever is clicked.
enum class CityTool {
    Select,
    Building,
    Road,
    Piece,
    Erase,
};

// What the block editor has selected inside the block.
enum class BlockElementKind { None, Patch, Box };
// A Blender-style transform of the selected patch or box: G grabs, S scales, R
// rotates. Gizmo arrows and handles use Grab on part of the element.
enum class BlockTransform { None, Grab, Scale, Rotate };

// What the Select tool has selected, so the gizmo and inspector know what to act
// on and the outline knows what to trace.
enum class CitySelectionKind {
    None,
    Building,
    Road,
    Node,
    Piece,
};

// The active tool in the Buildings (template) editor. Select picks a cell,
// PlaceBlock fills the cell under the cursor with the chosen block, PaintTexture
// writes the chosen material onto the picked face, and ItemSet places or clears a
// set on a cell.
enum class BuildingTool {
    Select,
    PlaceBlock,
    PaintTexture,
    ItemSet,
    Anchor,
};

// Settings for the Place Block tool, like editor.exe's block panel and its building
// tool (BuildingTool_WriteCellFromCurrentSelection @1400672a0). You choose whether
// the placed cell gets an outside shell, an inside shell, or both (neither means
// nothing to place), the buildblock template (cell_type) that shapes its walls, and
// the material for each face on each side. The outside and inside layers are written
// separately, each only when it is turned on. Place writes exactly this into the
// target cell.
struct BlockToolConfig {
    bool exterior{true};
    bool interior{false};
    // Off = normal (outside: the solid shell 0x40000000; inside: the generated room
    // 0xffff). On = a piece (0x80000000 | rotation << 24 | index into
    // building.piece_names). The block is picked from project.blocks and added to the
    // building's piece list when placed.
    bool exterior_piece{false};
    bool interior_piece{false};
    std::size_t exterior_piece_block{};
    std::size_t interior_piece_block{};
    std::uint32_t rotation{};
    // Buildblock template (cell_type) for the room's walls, written with the inside layer.
    std::size_t buildblock_index{};
    // The material (from building.materials) for each texture slot 0..7 of each set.
    std::array<std::size_t, 8> exterior_textures{};
    std::array<std::size_t, 8> interior_textures{};
    // Write into the clicked block instead of the empty cell in front of it.
    bool apply_to_clicked{false};
    // The room gets a floor (placed flag bit 0). editor.exe always writes 1. Without a
    // floor, the room below loses its ceiling too (Sbb_BuildBuildingConnectivityMetadata
    // @29120), so stacked rooms without floors make one tall room.
    bool room_floor{true};
};

// The drag gizmo handles on the selected city building. MoveX/MoveZ slide along the
// ground, MoveY lifts it, and Rotate turns it in 90 degree steps.
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

// Editor actions whose keys can be changed. The order is the order shown in
// Preferences.
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

// A small offscreen 3D view inside an ImGui window (building and block browser,
// buildblock window). Drag the image to orbit.
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
    Zoom,
};

// Map screen options: street and avenue names on or off, and their font, as
// "game:<name>" (a font in the game's font folder) or "file:<path>" (TTF/OTF).
struct MapMenuOptions {
    bool street_labels{true};
    std::string font{"game:rockwell"};
};
struct MapFontChoice {
    std::string key;
    std::string label;
};

struct EditorUi {
    EditorTab tab{EditorTab::City};
    // Shortcuts the user can change, indexed by EditorAction. Loaded from the prefs file.
    std::array<Shortcut, static_cast<std::size_t>(EditorAction::Count)> shortcuts{};
    // The action whose key is being recorded in Preferences, if any.
    std::optional<EditorAction> capturing_shortcut;
    bool show_building_browser{};
    bool show_block_browser{};
    bool show_buildblock_window{};
    // The Buildblock window had focus last frame, so undo goes to buildblocks.
    bool buildblock_window_focused{};
    bool show_changes_window{};
    bool show_map_image_window{};
    // Opens by itself when a map loads with files that failed.
    bool show_load_problems_window{};
    // How many cells above (or below) the surface under the cursor the Road, Piece, and
    // Building tools place. Page Up/Down change it.
    int place_height_offset{};
    // Game coordinates shown over the viewport (camera and cursor).
    bool show_coordinates{true};
    // The editor camera this frame, for the camera marker in the map window.
    Vector3 view_camera_position{};
    Vector3 view_camera_target{};
    MapMenuOptions map_menu;
    int map_export_size{512};
    bool map_export_requested{};
    bool show_shortcuts_window{};
    bool show_preferences_window{};
    bool show_graphics_debug{};
    // Which workspace windows are open. Saved in the preferences, so a window you
    // closed stays closed.
    bool show_library_window{false};  // the Outliner
    bool show_asset_drawer{true};
    bool show_properties_window{true};
    bool show_toolbar_window{true};
    // Set for one frame by Windows > Reset layout: windows go back to their default
    // positions and sizes.
    bool reset_layout{};
    std::size_t browser_building{};
    std::size_t browser_block{};
    PreviewView building_preview;
    PreviewView block_preview;
    PreviewView buildblock_preview;
    // Buildblock face editor: what is selected and what is being dragged.
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
    // A tab switch waiting from a tab-bar click or shortcut. The main loop applies it,
    // so the camera reset happens in one place.
    std::optional<EditorTab> requested_tab;
    // The last camera of each mode (by EditorTab) and the building or block it was on.
    std::array<std::optional<std::pair<Camera3D, std::size_t>>, 5> tab_cameras;
    float ui_font_size{15.0F};
    bool city_library_roads{};
    bool show_building_library{true};
    // One search query for each workspace tab. Keeping them separate stops a Buildings
    // search from making another tab look empty.
    std::array<std::array<char, 128>, 6> asset_search{};
    std::size_t selected_building{};
    std::size_t selected_block{};
    BlockElementKind block_element{BlockElementKind::None};
    std::size_t block_element_index{};
    // The transform in progress. It edits a preview copy of the block; the block only
    // changes when the transform is confirmed, so it is one undo step.
    BlockTransform block_transform{BlockTransform::None};
    int block_transform_axis{-1};          // -1 free, 0 X, 1 Y, 2 Z
    bool block_transform_drag{};           // started by dragging a handle; ends on release
    int block_transform_handle{-1};        // which handle (block_editor.cpp), -1 the whole element
    Vector2 block_transform_mouse{};
    std::optional<SblBlockMesh> block_transform_preview;
    // The handle under the mouse, for the hover colour.
    int block_hover_handle{-1};
    std::size_t selected_block_field{};
    std::size_t selected_block_patch{};
    std::size_t selected_block_point{};
    std::size_t selected_buildblock{};
    std::size_t selected_buildblock_layer{};
    // The active tool in each editor, set by the toolbar on the left of the viewport.
    CityTool city_tool{CityTool::Select};
    CitySelectionKind city_selection{CitySelectionKind::None};
    // Multi-selection of things you can place (buildings and piece portals). The single
    // active selection (selected_city_building / selected_portal) is always the last
    // one clicked; these sets hold the whole group for moving and deleting. Empty means
    // "only the single active selection".
    std::vector<std::size_t> selected_city_buildings;
    std::vector<std::size_t> selected_portals;
    // Box selection: while dragging with the Select tool, everything whose screen point
    // is inside the rectangle gets selected. box_select_active is set on press, and the
    // start and current points are in viewport pixels.
    bool box_select_active{false};
    Vector2 box_select_start{};
    // The building or piece template the Building and Piece tools place.
    std::size_t place_building_template{};
    // Rotation (0-3 quarter turns) for the next building the Building tool places.
    std::int32_t place_building_rotation{};
    // Why the Building tool's preview would overlap something (empty means clear).
    std::string placement_conflict;
    std::string road_preview_info;  // set by draw_road_lay_preview, shown by draw_coordinate_readout
    std::vector<CityTileCell> road_preview_cells;  // the road tiles draw_road_lay_preview drew
    // A road's tiles just after it is laid, drawn until the rebuilt city scene shows it.
    std::vector<CityTileCell> road_ghost;
    BuildingTool building_tool{BuildingTool::Select};
    std::size_t selected_item_set{};
    std::size_t item_set_catalog_add{};
    std::uint32_t item_set_rotation{};
    bool clear_item_set{};
    std::array<char, 64> new_item_set_name{};
    // Place Block tool settings (like editor.exe's block panel).
    BlockToolConfig block_config{};
    // Paint Texture tool: the material it writes and which side (outside or inside
    // texture slots) it paints.
    std::vector<std::size_t> selected_materials;
    std::array<char, 128> new_material_name{};
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
    // The Paint tool writes a wall side code (0 open, 1 wall, 3.. buildblock layer)
    // instead of a texture.
    bool paint_side_code{false};
    std::uint32_t paint_code{1};
    std::size_t building_list_add{};
    // Click and hold to keep placing: the last grid cell a held drag placed in, so
    // dragging across cells places quickly without placing in the same cell every
    // frame. Reset when the mouse is released. {INT32_MIN} means nothing placed yet in
    // this drag.
    std::optional<CityGridPoint> drag_stamp_cell;
    // Building cells the current Add Block drag filled (cleared on release).
    std::vector<std::size_t> stroke_block_cells;
    // The cell and face slot the Paint drag painted last (cleared on release).
    std::optional<std::pair<std::size_t, std::uint32_t>> drag_paint_face;
    std::size_t selected_city_street{};
    std::size_t selected_city_intersection{};
    std::size_t selected_city_building{};
    // The Road tool's start point. The first click only remembers it (snapped to a
    // nearby node or road); nodes are made when the second click lays a segment.
    std::optional<CityGridPoint> pending_road_start;
    // Total width in cells of roads the Road tool lays.
    int new_road_width{6};
    // Hover in Roads mode, worked out every frame for the select preview: the node or
    // road a Shift+Click would grab. When both are under the cursor the node wins, the
    // same as for clicks.
    std::optional<std::size_t> hover_city_intersection;
    std::optional<std::size_t> hover_city_street;
    // The selected piece is one voxel (a world grid cell), not the whole 8x8x8 portal.
    // Pieces are edited and highlighted one block at a time, even though they are
    // stored in groups inside portals.
    std::optional<CityGridPoint> selected_piece_voxel;
    std::optional<CityGridPoint> hover_piece_voxel;
    // The raw district word of the selected piece. v11 maps decode it through the named
    // city_blocks list. v10 (round) has no city_blocks, so the inspector reads this
    // directly to show the old cell's kind and index.
    std::uint32_t selected_piece_word{};
    std::size_t selected_city_block{};
    std::size_t selected_city_block_layer{};
    std::size_t selected_city_block_slot{};
    std::size_t selected_piece_rotation{};
    std::size_t place_piece_mesh{};
    std::size_t place_piece_texture{};
    // Place the game's solid cube city block (no name, shown as "nblock").
    bool place_piece_solid{false};
    std::size_t selected_portal{};
    std::array<int, 3> new_portal_cell{};
    bool portal_preview_textured{true};
    // City preview mode. Both true shows the game's terrain and finished road mesh.
    // Both false only shows the editor-style node and segment overlays.
    bool render_roads{true};
    bool render_terrain{true};
    // Performance settings (Preferences): see RenderSettings.
    bool near_lod{true};
    // Draw rooms only for the selected city buildings, at any distance.
    bool interiors_selected_only{false};
    // Opening a map: loose folders beat test.csx when both have an asset.
    bool loose_files_win{false};
    // Middle drag orbits and Shift+middle drag pans, like Blender.
    bool blender_mouse{true};
    // The folder Export mesh writes to; empty asks the first time.
    std::string mesh_export_folder;
    float render_scale{1.0F};        // 3D view resolution, 0.5..1
    float building_distance{0.0F};
    bool half_textures{false};
    float world_time_hours{11.0F};
    bool render_shadows{true};
    bool render_sky{true};
    float shadow_distance{700.0F};
    bool vsync{false};
    int fps_limit{60};               // 0 = unlimited
    bool background_throttle{true};  // 15 fps while the window is not focused
    bool street_objects{true};
    bool live_reflections{true};
    int anisotropy{16};
    std::size_t selected_building_cell{};
    // Hover in the building view: the index of the cell under the cursor this frame
    // (for the outline), worked out every frame; nullopt when there is none.
    std::optional<std::size_t> building_hover_cell;
    std::optional<std::uint32_t> building_hover_face;
    std::size_t selected_building_field{};
    // Buildings tab view: false shows the whole building, true only building_view_floor.
    bool building_view_floor_only{false};
    // Building editor: leave out the outer faces that point up, so you can see the rooms
    // from above.
    bool building_hide_roofs{false};
    int building_view_floor{0};
    float camera_fly_speed{48.0F};
    // Orbiting (Alt + left drag) or panning (middle drag) in progress, and its pivot.
    CameraDrag camera_drag{CameraDrag::None};
    int camera_drag_button{};
    // A view change from the view gizmo or the numpad, animated around a pivot.
    bool view_snap_active{};
    float view_snap_t{};
    Vector3 view_snap_from{};
    Vector3 view_snap_to{};
    Vector3 view_snap_pivot{};
    float view_snap_distance{};
    // The snap ends in an orthographic view (it looks straight along an axis).
    bool view_snap_ortho{};
    Vector3 camera_pivot{};
    float camera_pivot_distance{};
    // A Building or Road tool click waiting for release. Placement only happens if the
    // cursor is still near place_click_start.
    bool place_click_pending{};
    Vector2 place_click_start{};
    // Animated camera focus (F key). While active, the camera eases from the start pose
    // to the target pose over focus_duration. Flying by hand cancels it.
    bool focus_animating{false};
    bool focus_selection_requested{};
    float focus_t{};
    float focus_duration{0.6F};
    Vector3 focus_start_position{};
    Vector3 focus_start_target{};
    Vector3 focus_end_position{};
    Vector3 focus_end_target{};
    bool city_dirty{};
    // Map format upgrades are always announced, never silent (draw_city_upgrade_dialogs).
    bool confirm_piece_upgrade{};
    std::string city_upgrade_notice;
    bool building_dirty{};
    bool block_dirty{};
    bool buildblock_dirty{};
    // The gizmo handle being dragged on the selected city building (none when not
    // dragging). Set when the mouse is pressed over a handle, cleared on release.
    GizmoAxis gizmo_active{GizmoAxis::None};
    // The building's grid position when the gizmo drag started, so the drag turns the
    // total mouse movement into grid cells instead of adding up errors every frame.
    std::int32_t gizmo_drag_origin_x{};
    std::int32_t gizmo_drag_origin_z{};
    std::int32_t gizmo_drag_origin_y{};
    std::int32_t gizmo_drag_origin_rot{};
};

std::string lower_name(std::string_view value);
const LoadedAsset<SbbBuilding>* find_building_asset(const EditorProject& project, std::string_view name);
// Turns the extra game-surface colour attachments that building shaders use on or
// off. Cheap to call often: it does nothing when the state already matches.
void set_game_render_depth_output(bool enabled);
// Rebuilds project.placement_asset_indices if it looks out of date, then returns it.
// Use this instead of find_building_asset() in loops that run every frame for every
// placement.
const std::vector<std::optional<std::size_t>>& resolve_placement_asset_indices(EditorProject& project);
// Rebuilds placement_override_models when placements or template models change.
void refresh_placement_override_models(EditorProject& project);
// Gives freed heap pages back to the OS (otherwise the allocator keeps the peak
// from loading).
void release_free_memory();
// Sets the anisotropic filtering level on every loaded texture (1 = off).
void apply_texture_anisotropy(EditorProject& project, int level);
// Draws ImGui's frame with one upload per draw list (rlImGui's renderer refills
// raylib's single batch buffer for every command, which makes it wait on the GPU).
void render_imgui_draw_data(ImDrawData* draw_data);
// The label for a key in the user's keyboard layout (printable keys through
// glfwGetKeyName, fixed names for the rest). Defined in app.cpp.
std::vector<std::string> game_world_fit_problems(const EditorProject& project);
std::string key_label(int key);

// ui_windows.cpp: shortcuts, preferences, and the floating tool windows.
const ShortcutInfo& shortcut_info(EditorAction action);
void reset_shortcuts(EditorUi& ui);
bool action_pressed(const EditorUi& ui, EditorAction action);
std::string shortcut_label(const Shortcut& shortcut);
std::string action_shortcut_label(const EditorUi& ui, EditorAction action);
// Moves the selected junction, or both ends of the selected road, by `delta` levels.
void nudge_city_junction_level(EditorProject& project, EditorUi& ui, std::int64_t delta);
std::filesystem::path preferences_path();
void load_preferences(EditorUi& ui);
void save_preferences(const EditorUi& ui);
// Time-of-day slider for the sun; true when it changed.
bool draw_world_time_slider(EditorUi& ui);
#ifdef __EMSCRIPTEN__
void web_restore_file(const std::filesystem::path& path, const char* key);
void web_store_file(const std::filesystem::path& path, const char* key);
#endif
void apply_editor_style(float font_size);
void draw_preferences_contents(EditorUi& ui);
// The Interiors setting (Preferences and View menu); true when it changed.
bool draw_interior_mode(EditorUi& ui);
void draw_shortcuts_table(EditorUi& ui, bool editable);
void draw_building_browser_window(EditorProject& project, EditorUi& ui);
void draw_block_browser_window(EditorProject& project, EditorUi& ui);
void draw_buildblock_window(EditorProject& project, EditorUi& ui, std::string& status);
void draw_changes_window(EditorProject& project, EditorUi& ui, std::string& status);
// The game's map screen image (render_gpu.cpp): drawn from the city, or the map
// folder's map.png for the round map. nullptr until the city is built.
const Texture2D* city_map_image(EditorProject& project);
// Game fonts found next to the map, then the system's TTF/OTF fonts.
std::vector<MapFontChoice> map_font_choices(const EditorProject& project);
// The map screen (DrawMapMenu): the map image with its street labels, drawn
// `pixels` square like the game's viewport scaled to the screen.
const Texture2D* map_menu_texture(EditorProject& project, int pixels, const MapMenuOptions& options);
// The same screen drawn `pixels` square, as PNG bytes. Empty when there is no map
// image yet.
std::vector<std::uint8_t> map_menu_png(EditorProject& project, int pixels, const MapMenuOptions& options);
void draw_map_image_window(EditorProject& project, EditorUi& ui);
void draw_shortcuts_window(EditorUi& ui);
void draw_preferences_window(EditorUi& ui);
void unload_preview_views(EditorUi& ui);
// Game limits found from the sizes of subrosa.exe's arrays. Draws red or yellow
// lines for anything at or over a limit, and returns the number of problems.
int draw_map_limit_warnings(EditorProject& project);
void draw_buildblock_face_editor(EditorProject& project, EditorUi& ui);
// One buildblock face drawn like the game's room wall, in a `side` pixel square at
// `min` (ui_limits_buildblock.cpp). The button form is a clickable preview.
void draw_buildblock_layer(ImDrawList* draw_list, ImVec2 min, float side, const SblLayer& layer);
bool buildblock_layer_button(const char* id, const SblLayer& layer, float size, bool selected);
void draw_buildblock_face_canvas(EditorProject& project, EditorUi& ui);
// ui_workspace.cpp: the Toolbar, Assets, Outliner, and Properties windows, which
// change with the current context. draw_viewport_overlays draws the tool hint and map
// warnings over the scene canvas; call it inside the canvas window.
void draw_toolbar_window(EditorProject* project, EditorUi& ui, Camera3D& camera, std::string& status, bool& open_folder_requested);
void draw_asset_drawer_window(EditorProject* project, EditorUi& ui, std::string& status);
float draw_viewport_overlays(EditorProject* project, EditorUi& ui);
void draw_outliner_window(EditorProject* project, EditorUi& ui);
// A name cut to fit `width` pixels, ending in "..." (ui_windows.cpp).
std::string fit_label(const std::string& name, float width);
// A label on the left, in a shared column, for the next widget (ui_panels.cpp).
void field_label(const char* label);
// A search box above a wrapping grid of thumbnails; returns the index clicked
// (ui_panels.cpp). `image(i)` gives a GL texture id (0 means not ready yet) and
// whether it is a render target (stored upside down).
std::optional<std::size_t> search_grid(const char* id, std::array<char, 128>& search, std::size_t count, std::size_t selected,
    float size, float height, const std::function<const std::string&(std::size_t)>& name,
    const std::function<std::pair<unsigned int, bool>(std::size_t)>& image);
void draw_properties_window(EditorProject* project, EditorUi& ui, std::string& status);
void toggle_in_selection(std::vector<std::size_t>& set, std::size_t index);
void focus_current_selection(EditorProject& project, EditorUi& ui, Camera3D& camera);
void begin_city_action(EditorProject& project, EditorUi& ui, std::string label);
const LoadedAsset<SblTemplate>* buildblock_for_cell(const EditorProject& project, const SbbBuilding& building, const SbbCell& cell);
void draw_gpu_building_model_public(GpuBuildingRenderModel& model, Vector3 origin, std::int32_t rotation);
// The model matrix for a city placement (render origin plus quarter-turn basis).
Matrix city_placement_matrix(const SbcBuilding& placement);
// Instanced draws of `model` at every transform in `transforms`, lit by the game
// surface shader (or the flat mask shader). The caller owns the MRT state.
enum class GpuSurface { World, Building, Terrain, Model, Unlit, Mask, ShadowVolume };
void draw_gpu_mesh_instances(const GpuMesh& mesh, const std::vector<Matrix>& transforms, GpuSurface surface);
// The game's near-view switch distance (_render_buildsectorlist: sector centre
// within sqrt(36864) = 192 units).
inline constexpr float kNearLodDistance = 192.0F;
// A template's near-view mesh (rooms, doorways, glass; no shell window faces),
// built the first time it is needed while `build_budget` > 0 (each build uses one).
const GpuMesh* building_near_mesh(EditorProject& project, std::size_t asset_index, int& build_budget);
// Drops near meshes that haven't been used for a few seconds.
void evict_building_near_models(EditorProject& project);
// Placements drawn with the near view this frame: (asset index, placement transform).
std::vector<std::pair<std::size_t, Matrix>>& near_lod_placements();
// One mesh and the transforms to draw it at, for draw_gpu_mesh_batch.
struct GpuMeshDraw {
    const GpuMesh* mesh{};
    std::span<const Matrix> transforms;
};
// Which mesh parts to draw: all of them, only the opaque ones, or only the window
// parts as see-through glass (blended, both sides, after the opaque pass).
enum class GpuPartFilter { All, Opaque, Glass };
// Draws several meshes with one program bind and one instance upload, changing
// only the textures and uniforms between parts. The result is the same as one
// draw_gpu_mesh_instances call per mesh.
void draw_gpu_mesh_batch(const std::vector<GpuMeshDraw>& draws, GpuSurface surface, GpuPartFilter filter = GpuPartFilter::All);
void draw_mesh_faces(const SblBlockMesh& mesh, Vec3 origin, float scale, Color fill);
void undo_city(EditorProject& project, EditorUi& ui, std::string& status);
void redo_city(EditorProject& project, EditorUi& ui, std::string& status);
// Writes building `index` as a loose .sbb (asks where on the desktop, downloads in
// the browser).
void export_building_sbb(const EditorProject& project, std::size_t index, std::string& status);
void save_map_to(EditorProject& project, EditorUi& ui, const std::filesystem::path& folder, std::string& status);
// Records finished edits as undo steps; call once per frame. Skips frames where a
// drag or widget edit is still going, so each gesture becomes one step.
void commit_pending_edits(EditorProject& project, const EditorUi& ui);
// Undo and redo for what is being edited: the buildblock history while the
// Buildblock window has focus, otherwise the city history in City and Pieces, and
// the building or block-mesh history in their editors.
bool can_undo(const EditorProject& project, const EditorUi& ui, bool redo);
void undo_edit(EditorProject& project, EditorUi& ui, std::string& status, bool redo);
void edit_city_building(EditorProject& project, EditorUi& ui, std::size_t placement_index);
std::vector<const SbbBuilding*> resolve_placement_buildings(const EditorProject& project);
void refresh_changed_building(EditorProject& project, EditorUi& ui);
void save_active_tab(EditorProject& project, EditorUi& ui, std::string& status);
void save_all_dirty(EditorProject& project, EditorUi& ui, std::string& status);
bool has_unsaved_changes(const EditorUi& ui);
bool project_is_auto_generated(const EditorProject& project);
bool has_legacy_district_cells(const SbcMap& map);
void load_project_textures(EditorProject& project);
std::vector<std::filesystem::path> project_archive_paths(const EditorProject& project);
std::vector<std::filesystem::path> files_with_extension(const std::filesystem::path& dir, std::string_view extension);
void configure_texture(Texture2D& texture);
#ifdef __EMSCRIPTEN__
void report_web_load_progress(std::string_view stage, int percent);
void web_download(const std::string& name, const std::vector<std::uint8_t>& bytes);
#endif
// Desktop workers parse with load_textures=false; GPU uploads stay on the GL thread.
// When a map has the same asset loose (building/, block/, buildblock/, texture/)
// and in test.csx, the archive copy wins unless loose_files_win is set.
std::optional<EditorProject> load_project_folder(const std::filesystem::path& folder, std::string& error,
    bool load_textures = true, std::atomic<int>* stage = nullptr, bool loose_files_win = false);
// Render inputs worked out from the project. The block-mesh catalog, buildblock
// templates, texture names, and grid (CSX) texture names are worked out once and
// cached on the project (see EditorProject::cached_* / ensure_render_input_cache);
// these functions return references into that cache. Call
// invalidate_render_input_cache after changing blocks, buildblocks, or textures, so
// the next call rebuilds the caches (the CSX grid names only change on load).
// build_grid_texture_names uses the source archive order retained at load.
std::vector<std::string> build_grid_texture_names(const EditorProject& project);
void ensure_render_input_cache(EditorProject& project);
void invalidate_render_input_cache(EditorProject& project);
const std::vector<NamedBlockMesh>& block_meshes(EditorProject& project);
const std::vector<NamedBuildblockTemplate>& buildblock_templates(EditorProject& project);
const std::vector<std::string>& project_grid_texture_names(EditorProject& project);
void set_status(EditorProject& project);

void service_render_rebuild(EditorProject& project);
void invalidate_portal_render_models(EditorProject& project);
void invalidate_city_block_preview_scene(EditorProject& project);
GpuBuildingRenderModel upload_building_render_model(const BuildingRenderBuffer& buffer, const SbbBuilding& building, const EditorProject& project);
GpuBuildingRenderModel& building_tool_floor_preview_model(EditorProject& project, std::size_t building_index, std::optional<std::uint32_t> floor, bool roofs = true);
RenderTexture2D load_game_render_texture(int width, int height);
void unload_game_render_texture(RenderTexture2D target);
// These targets have colour, packed-depth, and shadow attachments. Keep their
// draw-buffer setup and raylib's texture-mode state in the same place.
void begin_game_texture_mode(RenderTexture2D target);
void end_game_texture_mode();
Texture2D game_render_packed_depth(RenderTexture2D target);
void clear_game_render_packed_depth();
void clear_game_render_shadow_factor();
void resolve_game_render_texture(RenderTexture2D target);
const RenderTexture2D* game_render_display_texture(RenderTexture2D target);
void draw_game_sky(const Camera3D& camera, Vector2 viewport_size, Texture2D depth_texture);
// Recaptures the blurred landscape cubemap that world surfaces sample for
// environment light, when the sun, terrain, or camera moved (render_gpu.cpp).
// follow_camera false: keep the last capture while the camera moves.
void invalidate_environment_cubemap();
void update_environment_cubemap(const GpuMesh* terrain, std::uint64_t terrain_fingerprint,
                               Vector3 camera_position, bool follow_camera = true);
void set_game_surface_camera(Vector3 position);
void set_game_world_time(float hours);
// The loaded texture for a material name (matched by name in project.textures), or
// nullptr when the project has no texture with that name. Used to show real texture
// thumbnails in the material pickers instead of names.
const Texture2D* texture_for_material(const EditorProject& project, std::string_view material_name);
// Finds a loaded project texture by material name (ignoring case); nullptr if there
// is none. The mesh exporter uses it too.
const Texture2D* find_project_texture(const EditorProject& project, std::string_view name);
// Picks a material from a grid of thumbnails you can click (the real texture when
// there is one, a colour tile otherwise). Changes `selected` in place; returns true
// when it changed.
bool material_swatch_picker(const EditorProject& project, const char* id, const std::vector<std::string>& materials, std::size_t& selected);
void draw_mesh_faces(const SblBlockMesh& mesh, float scale, Color fill);
void draw_sbb_scene_for_app(
    const SbbBuilding& building,
    GpuBuildingRenderModel& model,
    const SbbCell* selected,
    std::uint32_t guide_floor);
void draw_building_item_set_guides(const EditorProject& project, const SbbBuilding& building, std::optional<std::uint32_t> floor);
void draw_city_building_mask(GpuBuildingRenderModel& model, const SbcBuilding& placement);
void draw_outline_from_mask(RenderTexture2D& mask, Vector2 viewport_size, Color tint);
void draw_city_road_mask(const EditorProject& project, const MapScene& scene, std::size_t street_index);
// Outline masks from real geometry: a building cell's faces in the edit view's mesh,
// one piece voxel's faces, or a road junction's tiles.
void draw_building_face_highlight(const BuildingRenderBuffer& buffer, std::size_t cell, std::uint32_t slot);
void draw_building_cell_mask(const BuildingRenderBuffer& buffer, const SbbBuilding& building, const SbbCell& cell);
void draw_city_piece_mask(const EditorProject& project, CityGridPoint cell);
void draw_city_intersection_mask(const MapScene& scene, std::size_t intersection);
void draw_city_piece_voxels(
    EditorProject& project,
    bool textured,
    std::optional<std::size_t> selected_portal = std::nullopt,
    bool draw_wires = false,
    bool isolate_selected = false);
GpuBuildingRenderModel upload_city_tile_model(const BuildingRenderBuffer& buffer, const EditorProject& project);
GpuBuildingRenderModel upload_city_terrain_model(const BuildingRenderBuffer& buffer, const EditorProject& project);
void unload_city_render_model(GpuBuildingRenderModel& model);
void draw_city_scene(
    EditorProject& project,
    const MapScene& scene,
    std::optional<std::size_t> selected_street,
    bool render_roads,
    bool render_terrain);
void draw_city_editor_overlay(const EditorProject& project, const MapScene& scene);
void draw_city_traffic_light_poles(const EditorProject& project, const MapScene& scene);
void draw_city_shadow_volumes(EditorProject& project, const Camera3D& camera, float max_distance);
void composite_city_shadows(Vector2 viewport_size);
void draw_city_cursor(CityGridPoint point, float unit_size, CityTool tool);
// Work grids size their lines in screen pixels for this camera and viewport.
void set_grid_view(const Camera3D& camera, float viewport_height);
// Placement grid around the city tool's target. cell_fill tints the target cell
// (pieces, buildings); otherwise the lines through the point are highlighted.
void draw_city_edit_grid(CityGridPoint center, const MapScene& scene, Color accent, bool cell_fill);
// Selection and hover overlay in Roads mode: a different look for the selected node,
// the selected road, and the node or road under the cursor (hover_city_* in ui).
void draw_city_block_preview(EditorProject& project, const SbcCityBlock& city_block, const std::vector<NamedBlockMesh>& meshes, std::size_t city_block_index, std::size_t selected_layer, std::size_t selected_slot);
void draw_buildblock_preview_3d(const BuildblockPreviewScene& scene);

Camera3D default_camera();
float adjust_camera_fly_speed(float current_speed, float wheel_delta);
Ray get_viewport_mouse_ray(const Camera3D& camera, Vector2 viewport_mouse_pos, Vector2 viewport_size);
Vector2 get_viewport_mouse_position(Vector2 viewport_pos, Vector2 viewport_size);
void focus_camera_on_city(Camera3D& camera, const EditorProject& project);
FlyInput read_fly_input();
void update_free_fly_camera(Camera3D& camera, float fly_speed, const FlyInput& input);
// Alt + left drag orbits around the point under the cursor, middle drag pans, and
// the wheel zooms toward the cursor. `scene` gives the terrain for the pivot.
void update_orbit_camera(Camera3D& camera, EditorUi& ui, Vector2 viewport_mouse_pos, Vector2 viewport_size,
                         bool viewport_hovered, const MapScene* scene);
// Blender-style view gizmo and numpad views (viewport.cpp). Call inside the scene
// canvas window; keys_allowed when the 3D view has the mouse.
void draw_view_gizmo(Camera3D& camera, EditorUi& ui, bool keys_allowed);
void snap_view(Camera3D& camera, EditorUi& ui, Vector3 direction);
// Orthographic views (Numpad 5, or any axis view): fovy is then the view's height in
// world units. Both keep the target and the size of what is shown.
void set_camera_orthographic(Camera3D& camera);
void set_camera_perspective(Camera3D& camera);
// Half the view's height in world units at `point`, for screen-sized handles.
float view_half_height(const Camera3D& camera, Vector3 point);
std::optional<CityGridPoint> city_grid_point_under_mouse(const Camera3D& camera, Vector2 viewport_mouse_pos, Vector2 viewport_size);
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

void report_failure(std::string& status, std::string message);

// Project and archive operations the file UI uses.
std::uint32_t read_le_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset);
std::string lower_extension(const std::filesystem::path& path);
std::vector<std::uint8_t> normalize_packed_sbb_bytes(const std::vector<std::uint8_t>& payload);
std::vector<std::uint8_t> normalize_packed_sbl_bytes(const std::vector<std::uint8_t>& payload);
void require_complete_project_source(const EditorProject& project);
// Loads failed asset `index` with its repair; false when it has none. The building
// or block joins the map (marked changed, so Save writes it) and its load error is
// cleared, which unblocks Save once every error is gone.
bool apply_asset_repair(EditorProject& project, EditorUi& ui, std::size_t index, std::string& status);
// Assets → Import into map: adds .sbb buildings, .sbl blocks and buildblocks, and
// .png textures to the open map, replacing assets with the same name. Files that
// fail are listed in Load problems (with a fix when there is one) and don't block
// Save. Returns how many were added.
std::size_t import_files_into_project(EditorProject& project, EditorUi& ui, const std::vector<std::filesystem::path>& paths,
                                      std::string& status);
// A plain-language explanation of a parser error, added after its text.
std::string explain_problem(const std::string& error);
// The list of files that failed to load, why, and a "Try auto-fixing" button for
// each one that can be fixed (ui_archive.cpp).
void draw_load_problems(EditorProject& project, EditorUi& ui, std::string& status);
void draw_load_problems_window(EditorProject& project, EditorUi& ui, std::string& status);
void promote_city(EditorProject& project);
CsxArchive project_archive(const EditorProject& project);
// project_archive in steps, so the archive window can fill in over several frames
// and show progress: begin, then every step from 0 to the count, then finish.
CsxArchive project_archive_begin(const EditorProject& project);
std::size_t project_archive_step_count(const EditorProject& project);
void project_archive_step(const EditorProject& project, CsxArchive& archive, std::size_t step);
void project_archive_finish(CsxArchive& archive);
void import_project_archive(ArchiveWorkspace& workspace, const EditorProject& project);
void save_archive_to(ArchiveWorkspace& workspace, std::filesystem::path path);
bool save_archive_workspace(ArchiveWorkspace& workspace);
void draw_csx_editor(ArchiveWorkspace& workspace, bool& open, EditorProject* project, EditorUi& ui, std::string& status);

std::optional<std::filesystem::path> dropped_folder();
std::vector<std::size_t> occupied_cell_indices(const SbbBuilding& building);
std::vector<std::string> block_names(const EditorProject& project);
std::vector<std::string> city_building_names(const SbcMap& map);
std::vector<std::string> city_street_names(const SbcMap& map);

void nudge_city_building(EditorProject& project, EditorUi& ui, int dx, int dy, int dz, int drot);
void set_city_building_position(EditorProject& project, EditorUi& ui, CityGridPoint point);
void duplicate_city_building(EditorProject& project, EditorUi& ui);
void delete_city_building(EditorProject& project, EditorUi& ui);
void nudge_city_street_width(EditorProject& project, EditorUi& ui, int side, int delta);
void duplicate_city_street(EditorProject& project, EditorUi& ui);
void append_city_road_point(EditorProject& project, EditorUi& ui, CityGridPoint point);
struct RoadLay {
    std::optional<CityGridPoint> next_start;  // where the chain continues; empty ends it
    std::optional<std::size_t> last_street;   // the last segment added
};
RoadLay lay_city_road(SbcMap& map, CityGridPoint start_point, CityGridPoint point, std::int32_t width, bool auto_generated);
void set_city_piece_voxel(EditorProject& project, EditorUi& ui, CityGridPoint point);
void repaint_city_piece(EditorProject& project, EditorUi& ui, const std::array<std::string, 8>& textures, std::string& status);
void delete_city_street(EditorProject& project, EditorUi& ui);
void nudge_city_block_slot(EditorProject& project, EditorUi& ui, int delta);
void edit_building(EditorProject& project, EditorUi& ui, std::size_t asset_index);
void recalculate_building_size(SbbBuilding& building);
// Renames the selected building and every placement of it; reports problems in
// `status`.
void rename_building(EditorProject& project, EditorUi& ui, const std::string& name, std::string& status);
void mark_building_changed(EditorProject& project, EditorUi& ui);
void mark_buildblock_changed(EditorProject& project, EditorUi& ui);
void rebuild_buildings_using_buildblock(EditorProject& project, const std::string& buildblock_name);
void mark_block_changed(EditorProject& project, EditorUi& ui, SblBlockMesh& block);

// Draws building asset `index` into a small thumbnail RenderTexture (cached in
// project.building_thumbnails) and returns it, or nullptr if it can't be made.
const RenderTexture2D* building_thumbnail(EditorProject& project, std::size_t index);
void invalidate_building_thumbnail(EditorProject& project, std::size_t index);
void rebuild_single_building_render(EditorProject& project, std::size_t index);
const RenderTexture2D* city_block_thumbnail(EditorProject& project, std::size_t index);
// Block-mesh thumbnail, drawn when first needed (ui_windows.cpp); nullptr until
// ready.
const RenderTexture2D* block_thumbnail(EditorProject& project, std::size_t index);
// A 3D thumbnail of an item set's items on one cell (render_gpu.cpp); nullptr when
// the set's .sis was not found or the thumbnail isn't drawn yet.
const RenderTexture2D* item_set_thumbnail(EditorProject& project, const std::string& name);
void invalidate_buildblock_preview_scene(EditorProject& project);
const BuildblockPreviewScene& buildblock_preview_scene(EditorProject& project, std::size_t index);
void draw_building_inspector(EditorProject& project, EditorUi& ui, std::string& status);
void draw_block_inspector(EditorProject& project, EditorUi& ui, std::string& status);
// Block editor (block_editor.cpp): the selected patch or box of the selected block.
void duplicate_block_element(EditorProject& project, EditorUi& ui);
void delete_block_element(EditorProject& project, EditorUi& ui);
void nudge_block_element(EditorProject& project, EditorUi& ui, Vec3f delta);
void draw_block_editor_view(EditorProject& project, const EditorUi& ui);
void draw_block_editor_handles(EditorProject& project, const EditorUi& ui, const Camera3D& camera);
void pick_block_element(EditorProject& project, EditorUi& ui, Ray ray);
void add_block_patch(EditorProject& project, EditorUi& ui);
void add_block_box(EditorProject& project, EditorUi& ui);
// The 3D block editor's mouse and keys. Call press on a left click in the view; it
// confirms a transform, grabs a handle, or selects. Call update every frame.
void block_editor_press(EditorProject& project, EditorUi& ui, const Camera3D& camera, Vector2 mouse, Vector2 viewport_size);
void block_editor_update(EditorProject& project, EditorUi& ui, const Camera3D& camera, Vector2 mouse, Vector2 viewport_size, bool hovered);
void start_block_transform(EditorProject& project, EditorUi& ui, BlockTransform mode, Vector2 mouse);
void frame_block_camera(const EditorProject& project, const EditorUi& ui, Camera3D& camera);
void draw_buildblock_inspector(EditorProject& project, EditorUi& ui, std::string& status);
void draw_struct_inspector_header(const char* file_type, const char* struct_name, const std::string& record_name);
void draw_struct_section(const char* name);
std::string hex_u32(std::uint32_t value);

} // namespace pafta::app_internal
