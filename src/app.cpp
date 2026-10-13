// SPDX-License-Identifier: GPL-2.0-only
#include "app.hpp"
#include "app_internal.hpp"
#include "mesh_export.hpp"
#include "buildblock_catalog.hpp"
#include <cstdio>
#include <stdexcept>
#include "profiler.hpp"

#include <raylib.h>
#include <rlgl.h>
#include <raymath.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <rlImGui.h>

#ifndef __EMSCRIPTEN__
#include "file_dialogs.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

// raylib's bundled GLFW desktop backend defines these in libraylib.a and calls
// glfwGetKeyName itself, but raylib does not re-export them. Declaring them
// here lets us show key labels for the user's layout without including the
// GLFW header or changing the build.
#ifndef __EMSCRIPTEN__
extern "C" const char* glfwGetKeyName(int key, int scancode);
extern "C" int glfwGetKeyScancode(int key);
#endif

namespace pafta::app_internal {

// An error the user has to see. The status bar keeps showing it, and the
// "Operation failed" popup opens once for it. Plain status text never opens
// the popup.
std::string g_failure_message;
void report_failure(std::string& status, std::string message) {
    status = message;
    g_failure_message = std::move(message);
}

void release_web_project(const std::filesystem::path& root) {
#ifdef __EMSCRIPTEN__
    const auto path = root.string();
    EM_ASM({ window.paftaBrowserReleaseProject(UTF8ToString($0)); }, path.c_str());
#else
    (void)root;
#endif
}

#ifdef __EMSCRIPTEN__
// Set by web/file_upload.js once the browser has copied the chosen folder into
// Emscripten's virtual file system. The editor picks it up at the same point as
// a native folder drop, after the current frame is shown.
std::optional<std::filesystem::path> g_web_uploaded_folder;

// Loading runs on one thread on purpose, because raylib texture uploads have to
// happen on the browser's GL thread. The caller yields once before loading.
// Yielding again in here (nested Asyncify sleeps) piles up paused parser stacks,
// which can fail to resume on big projects.
void report_web_load_progress(std::string_view stage, int percent) {
    const auto message = std::string{"Loading "} + std::string{stage} + "... "
        + std::to_string(std::clamp(percent, 0, 100)) + "%";
    EM_ASM({
        const node = document.getElementById('status');
        if (node) node.textContent = UTF8ToString($0);
    }, message.c_str());
}

// The browser build cannot write to disk, so saved files reach the user as
// downloads.
void web_download(const std::string& name, const std::vector<std::uint8_t>& bytes) {
    EM_ASM({
        const data = HEAPU8.slice($1, $1 + $2);
        const link = document.createElement('a');
        link.href = URL.createObjectURL(new Blob([data], {type: 'application/octet-stream'}));
        link.download = UTF8ToString($0);
        document.body.appendChild(link);
        link.click();
        link.remove();
        setTimeout(() => URL.revokeObjectURL(link.href), 10000);
    }, name.c_str(), bytes.data(), bytes.size());
}

extern "C" EMSCRIPTEN_KEEPALIVE void pafta_web_files_ready(const char* root) {
    if (root != nullptr && root[0] != '\0') {
        if (g_web_uploaded_folder) release_web_project(*g_web_uploaded_folder);
        g_web_uploaded_folder = std::filesystem::path{root};
    }
}
#endif

// The label for a key, in the user's keyboard layout. raylib's IsKeyPressed
// checks where a key physically is (US-QWERTY scancodes), so movement and edit
// keys stay in the same place on every layout. This only changes the text we
// show: for example the key in the W position shows "Z" on AZERTY.
// glfwGetKeyName gives the layout's label for printable keys and null for the
// rest (arrows, Tab, Delete...), which use a fixed name instead.
std::string key_label(int key) {
#ifdef __EMSCRIPTEN__
    switch (key) {
        case KEY_LEFT: return "Left";
        case KEY_RIGHT: return "Right";
        case KEY_UP: return "Up";
        case KEY_DOWN: return "Down";
        case KEY_PAGE_UP: return "PgUp";
        case KEY_PAGE_DOWN: return "PgDn";
        case KEY_DELETE: return "Del";
        case KEY_TAB: return "Tab";
        case KEY_SPACE: return "Space";
        case KEY_LEFT_SHIFT: return "Shift";
        case KEY_LEFT_CONTROL: return "Ctrl";
        default:
            // raylib codes for printable keys are their US-layout ASCII values.
            if (key > 32 && key < 127) return std::string(1, static_cast<char>(key));
            return "Key";
    }
#else
    if (const char* name = glfwGetKeyName(key, glfwGetKeyScancode(key)); name != nullptr && name[0] != '\0') {
        std::string label{name};
        // glfwGetKeyName returns UTF-8. Only plain ASCII bytes are uppercased, so a
        // multi-byte character (such as an accented key) is never split into a broken
        // byte that the font would draw as a box.
        for (auto& ch : label) {
            const auto byte = static_cast<unsigned char>(ch);
            if (byte < 0x80U) {
                ch = static_cast<char>(std::toupper(byte));
            }
        }
        return label;
    }
    switch (key) {
        case KEY_LEFT: return "Left";
        case KEY_RIGHT: return "Right";
        case KEY_UP: return "Up";
        case KEY_DOWN: return "Down";
        case KEY_PAGE_UP: return "PgUp";
        case KEY_PAGE_DOWN: return "PgDn";
        case KEY_DELETE: return "Del";
        case KEY_TAB: return "Tab";
        case KEY_SPACE: return "Space";
        case KEY_LEFT_SHIFT: return "Shift";
        case KEY_LEFT_CONTROL: return "Ctrl";
        default: return "?";
    }
#endif
}

std::string gfx_debug_format_bytes(std::uint64_t bytes) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(2);
    if (bytes >= 1024ULL * 1024ULL) {
        stream << static_cast<double>(bytes) / static_cast<double>(1024ULL * 1024ULL) << " MiB";
    } else if (bytes >= 1024ULL) {
        stream << static_cast<double>(bytes) / static_cast<double>(1024ULL) << " KiB";
    } else {
        stream << bytes << " B";
    }
    return stream.str();
}

void accumulate_gpu_model_upload_stats(GfxDebugStats& stats, const GpuBuildingRenderModel& model) {
    stats.uploads.uploaded_vertices += model.mesh.vertex_count;
    stats.uploads.uploaded_indices += model.mesh.index_count;
    stats.uploads.uploaded_bytes += model.uploaded_bytes;
    stats.uploads.chunks += model.mesh.parts.size();
}

void refresh_gfx_debug_counts(GfxDebugStats& stats, const EditorProject* project) {
    if (project == nullptr) {
        return;
    }

    if (project->city.has_value()) {
        stats.counts.buildings = project->city->data.buildings.size();
    } else {
        stats.counts.buildings = project->buildings.size();
    }

    for (const auto& model : project->building_gpu_models) {
        accumulate_gpu_model_upload_stats(stats, model);
    }
    accumulate_gpu_model_upload_stats(stats, project->portal_gpu_model);
    accumulate_gpu_model_upload_stats(stats, project->portal_preview_gpu_model);
    accumulate_gpu_model_upload_stats(stats, project->building_floor_preview_gpu_model);
}

const char* gfx_draw_category_name(GfxDebugDrawCategory category) {
    switch (category) {
        case GfxDebugDrawCategory::CityCells:
            return "Building/city cells";
        case GfxDebugDrawCategory::RoadsCurbs:
            return "Roads/curbs";
        case GfxDebugDrawCategory::Terrain:
            return "Terrain mesh";
        case GfxDebugDrawCategory::TrafficLightPoles:
            return "Traffic-light poles";
        case GfxDebugDrawCategory::Pieces:
            return "Pieces";
        case GfxDebugDrawCategory::WireOverlay:
            return "Wire/overlay";
        case GfxDebugDrawCategory::ShadowVolumes:
            return "Shadow volumes";
        case GfxDebugDrawCategory::Count:
            break;
    }
    return "Unknown";
}

const char* gfx_timing_phase_name(GfxDebugTimingPhase phase) {
    switch (phase) {
        case GfxDebugTimingPhase::SceneRenderBufferBuild:
            return "render rebuild service";
        case GfxDebugTimingPhase::GpuUpload:
            return "gpu upload";
        case GfxDebugTimingPhase::Draw3D:
            return "3D draw total";
        case GfxDebugTimingPhase::TerrainDraw:
            return "terrain draw";
        case GfxDebugTimingPhase::CityRoadDraw:
            return "city/road draw";
        case GfxDebugTimingPhase::BuildingDraw:
            return "building draw";
        case GfxDebugTimingPhase::PieceDraw:
            return "piece draw";
        case GfxDebugTimingPhase::OutlineDraw:
            return "outline draw";
        case GfxDebugTimingPhase::PoleDraw:
            return "pole draw";
        case GfxDebugTimingPhase::ImGuiUi:
            return "ImGui/UI";
        case GfxDebugTimingPhase::TotalFrame:
            return "total frame";
        case GfxDebugTimingPhase::Count:
            break;
    }
    return "unknown";
}

void draw_graphics_debug_window(bool& open, GfxDebugStats& stats) {
    if (!open) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(460, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Graphics Debug", &open)) {
        ImGui::End();
        return;
    }

    const auto total = stats.total_draw();
    const auto total_frame_ms = stats.phase_ms[static_cast<std::size_t>(GfxDebugTimingPhase::TotalFrame)];
    ImGui::Text("FPS %d | frame %.2f ms", GetFPS(), GetFrameTime() * 1000.0F);

    std::array<float, GfxDebugStats::history_size> ordered_history{};
    const auto count = stats.frame_time_count;
    for (std::size_t i = 0; i < count; ++i) {
        const auto source = (stats.frame_time_index + stats.frame_time_history.size() - count + i) %
            stats.frame_time_history.size();
        ordered_history[i] = stats.frame_time_history[source];
    }
    ImGui::PlotLines("frame ms", ordered_history.data(), static_cast<int>(count), 0, nullptr, 0.0F, 50.0F, ImVec2(-FLT_MIN, 64.0F));

    ImGui::SeparatorText("Draw Stats");
    ImGui::Text("Total: %llu tris | %llu verts | %llu calls",
        static_cast<unsigned long long>(total.triangles),
        static_cast<unsigned long long>(total.vertices),
        static_cast<unsigned long long>(total.draw_calls));
    if (ImGui::BeginTable("draw_breakdown", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Category");
        ImGui::TableSetupColumn("Tris", ImGuiTableColumnFlags_WidthFixed, 82.0F);
        ImGui::TableSetupColumn("Verts", ImGuiTableColumnFlags_WidthFixed, 82.0F);
        ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 68.0F);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < static_cast<std::size_t>(GfxDebugDrawCategory::Count); ++i) {
            const auto category = static_cast<GfxDebugDrawCategory>(i);
            const auto& counter = stats.draw[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(gfx_draw_category_name(category));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(counter.triangles));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(counter.vertices));
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(counter.draw_calls));
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("GPU Upload");
    ImGui::Text("%llu chunks | %llu verts | %llu indices | %s",
        static_cast<unsigned long long>(stats.uploads.chunks),
        static_cast<unsigned long long>(stats.uploads.uploaded_vertices),
        static_cast<unsigned long long>(stats.uploads.uploaded_indices),
        gfx_debug_format_bytes(stats.uploads.uploaded_bytes).c_str());

    ImGui::SeparatorText("Timing");
    if (ImGui::BeginTable("timing_breakdown", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Phase");
        ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 72.0F);
        ImGui::TableSetupColumn("avg", ImGuiTableColumnFlags_WidthFixed, 72.0F);
        ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, 54.0F);
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < static_cast<std::size_t>(GfxDebugTimingPhase::Count); ++i) {
            const auto phase = static_cast<GfxDebugTimingPhase>(i);
            const auto ms = stats.phase_ms[i];
            const auto pct = total_frame_ms > 0.0 ? (ms / total_frame_ms) * 100.0 : 0.0;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(gfx_timing_phase_name(phase));
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", ms);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", stats.phase_avg_ms[i]);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f", pct);
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Counts");
    ImGui::Text("Buildings %llu | City tiles %llu | Poles %llu",
        static_cast<unsigned long long>(stats.counts.buildings),
        static_cast<unsigned long long>(stats.counts.city_tiles),
        static_cast<unsigned long long>(stats.counts.traffic_light_poles));
    ImGui::Text("Terrain samples %llu | vertices %llu",
        static_cast<unsigned long long>(stats.counts.terrain_samples),
        static_cast<unsigned long long>(stats.counts.terrain_vertices));
    ImGui::Text("Total scene triangles %llu", static_cast<unsigned long long>(stats.counts.total_scene_triangles));
    ImGui::End();
}

// Builds the block mesh catalog from the loaded SBL blocks, with the game's
// built-in set added first (build_native_block_mesh_catalog). It only depends on
// project.blocks.
std::vector<NamedBlockMesh> build_block_meshes(const EditorProject& project) {
    std::vector<NamedBlockMesh> meshes;
    meshes.reserve(project.blocks.size());
    for (const auto& block : project.blocks) {
        meshes.push_back({block.name, block.data});
    }
    return build_native_block_mesh_catalog(meshes);
}

std::vector<NamedBuildblockTemplate> build_buildblock_templates(const EditorProject& project) {
    std::vector<NamedBuildblockTemplate> templates;
    templates.reserve(project.buildblocks.size());
    for (const auto& buildblock : project.buildblocks) {
        templates.push_back({buildblock.name, buildblock.data});
    }
    return templates;
}

// Editing an asset clears the mesh and template cache. Grid texture names come
// from the archive order on disk and stay the same while the project is open.
void ensure_render_input_cache(EditorProject& project) {
    if (!project.render_input_cache_valid) {
        PAFTA_PROFILE_BLOCK("rebuild render input cache");
        project.cached_block_meshes = build_block_meshes(project);
        project.cached_buildblock_templates = build_buildblock_templates(project);
        project.render_input_cache_valid = true;
    }
    if (!project.grid_texture_names_valid) {
        PAFTA_PROFILE_BLOCK("rebuild grid texture names");
        project.cached_grid_texture_names = build_grid_texture_names(project);
        project.grid_texture_names_valid = true;
    }
}

void invalidate_render_input_cache(EditorProject& project) {
    project.render_input_cache_valid = false;
}

const std::vector<NamedBlockMesh>& block_meshes(EditorProject& project) {
    ensure_render_input_cache(project);
    return project.cached_block_meshes;
}

const std::vector<NamedBuildblockTemplate>& buildblock_templates(EditorProject& project) {
    ensure_render_input_cache(project);
    return project.cached_buildblock_templates;
}

const std::vector<std::string>& project_grid_texture_names(EditorProject& project) {
    ensure_render_input_cache(project);
    return project.cached_grid_texture_names;
}

// The old v10 district texture grid stores ABSOLUTE numbers into the game's
// texture table. World_InitResourcePoolsAndCatalogs @0x13e408 and
// World_LoadMapAndInitializeTerrain @0x56b10 fix its order:
//   index 0      = "concrete" (built in, texture/concrete.png)
//   index 1,2    = built-in slots kept free
//   index 3      = the generated default texture (DAT_64cff7e4 = 3)
//   index 4..N   = the map's CSX 0x585a0004 textures, in archive order
//     (DAT_64cff560 starts at 4 and goes up by one for each CSX texture).
// asphalt2.png, asphalt2_material.png, and map.png load into fixed model slots,
// not this table, so they must not shift the numbers. This builds the same name
// list, so a district cell's grid number finds the same texture the game uses.
// Numbers 0 to 3 are the game's "concrete" and default grey. They live in the
// game's own data/texture folder, not the map folder, so the editor can't load
// them by name. We map them to "asphalt2", the grey road texture the editor does
// have, so plazas and sidewalks look grey like in the game. CSX names come next,
// in archive order.
//
// The source order is captured at load, so saving never changes this table.
std::vector<std::string> build_grid_texture_names(const EditorProject& project) {
    if (project.grid_texture_names_valid) return project.cached_grid_texture_names;
    std::vector<std::string> names = {"asphalt2", "asphalt2", "asphalt2", "asphalt2"};
    for (const auto& entry : project.archive_entries) {
        if (entry.kind == CsxEntryKind::Texture) names.push_back(entry.name);
    }
    return names;
}

std::vector<std::size_t> occupied_cell_indices(const SbbBuilding& building) {
    std::vector<std::size_t> indices;
    for (const auto& cell : building.cells) {
        if (cell.occupied) {
            indices.push_back(cell.index);
        }
    }
    return indices;
}

std::vector<std::string> block_names(const EditorProject& project) {
    std::vector<std::string> names;
    for (const auto& block : project.blocks) {
        names.push_back(block.name);
    }
    return names;
}

std::vector<std::string> city_building_names(const SbcMap& map) {
    std::vector<std::string> names;
    names.reserve(map.buildings.size());
    for (const auto& building : map.buildings) {
        names.push_back(std::to_string(building.index) + " " + building.name);
    }
    return names;
}

// Finds the loaded .sbb for each city placement (nullptr when the name is not
// found), in the same order as map.buildings. The scene cache rebuild and the
// viewport's building picker both use it, so they agree on footprints.
std::vector<const SbbBuilding*> resolve_placement_buildings(const EditorProject& project) {
    std::vector<const SbbBuilding*> placement_buildings;
    if (!project.city) {
        return placement_buildings;
    }
    placement_buildings.reserve(project.city->data.buildings.size());
    for (const auto& placement : project.city->data.buildings) {
        const auto asset = find_building_asset(project, placement.name);
        placement_buildings.push_back(asset == nullptr ? nullptr : &asset->data);
    }
    return placement_buildings;
}

void edit_building(EditorProject& project, EditorUi& ui, std::size_t asset_index) {
    if (asset_index >= project.buildings.size()) return;
    ui.selected_building = asset_index;
    ui.selected_building_cell = 0;
    ui.tab = EditorTab::Buildings;
    ui.building_tool = BuildingTool::Select;
    ui.gizmo_active = GizmoAxis::None;
    ui.box_select_active = false;
    ui.focus_selection_requested = true;
}

void edit_city_building(EditorProject& project, EditorUi& ui, std::size_t placement_index) {
    if (!project.city || placement_index >= project.city->data.buildings.size()) return;
    const auto& name = project.city->data.buildings[placement_index].name;
    const auto asset = find_building_asset(project, name);
    if (asset != nullptr) edit_building(project, ui, static_cast<std::size_t>(asset - project.buildings.data()));
}

// A suggested .gltf path for the current view. When the current tab has nothing to
// export, it returns empty and puts the reason in `reason`. The caller opens the
// native dialog after the options popup closes.
std::filesystem::path default_export_path(const EditorProject& project, const EditorUi& ui, std::string& reason) {
    auto make_path = [&project](const std::string& name) {
        const auto safe = name.empty() ? std::string("export") : name;
        return project.root / (safe + ".gltf");
    };

    if (ui.tab == EditorTab::Buildings) {
        if (project.buildings.empty()) {
            reason = "no building loaded";
            return {};
        }
        const auto index = std::min(ui.selected_building, project.buildings.size() - 1U);
        return make_path(project.buildings[index].name.empty() ? "building" : project.buildings[index].name);
    }
    if (ui.tab == EditorTab::Blocks) {
        if (project.blocks.empty()) {
            reason = "no block loaded";
            return {};
        }
        const auto index = std::min(ui.selected_block, project.blocks.size() - 1U);
        return make_path(project.blocks[index].name.empty() ? "block" : project.blocks[index].name);
    }
    if (ui.tab == EditorTab::City) {
        if (!project.city) {
            reason = "no city loaded";
            return {};
        }
        return make_path("city");
    }
    reason = "this view (switch to Buildings, Blocks, or City)";
    return {};
}

// A mesh export that is running. Geometry is built on a worker from copies of the
// inputs, texture PNGs are read back on this (the GL) thread a few each frame,
// and the glTF and bin files are written on a worker. A popup shows progress.
struct MeshExportTask {
    struct Built {
        GltfExportPtr job;
        ExportResult error;
    };
    std::string what;
    std::shared_ptr<std::atomic<std::size_t>> steps_done = std::make_shared<std::atomic<std::size_t>>(0U);
    std::size_t steps_total{1};
    std::future<Built> build;
    GltfExportPtr job;
    std::future<ExportResult> write;
};

#ifndef __EMSCRIPTEN__
std::optional<MeshExportTask> start_mesh_export(EditorProject& project, const EditorUi& ui,
                                                const std::filesystem::path& path, const ExportOptions& options,
                                                std::string& status) {
    PAFTA_PROFILE_FUNCTION();
    if (path.empty()) {
        report_failure(status, "Export failed: empty path.");
        return std::nullopt;
    }
    MeshExportTask task;
    auto done = task.steps_done;
    auto blocks = block_meshes(project);
    auto templates = buildblock_templates(project);

    if (ui.tab == EditorTab::Buildings) {
        if (project.buildings.empty()) {
            report_failure(status, "Export failed: no building loaded.");
            return std::nullopt;
        }
        const auto& asset = project.buildings[std::min(ui.selected_building, project.buildings.size() - 1U)];
        task.what = asset.name;
        task.build = std::async(std::launch::async,
            [path, options, done, name = asset.name, building = asset.data, blocks = std::move(blocks), templates = std::move(templates)] {
                MeshExportTask::Built built;
                const auto buffer = build_native_building_render_buffer(building, blocks, templates);
                ExportMeshGroup group;
                group.name = name;
                group.buffer = &buffer;
                group.building_materials = &building.materials;
                built.job = build_gltf_export(path, {group}, options, built.error);
                *done = 1U;
                return built;
            });
        return task;
    }

    if (ui.tab == EditorTab::Blocks) {
        if (project.blocks.empty()) {
            report_failure(status, "Export failed: no block loaded.");
            return std::nullopt;
        }
        const auto& asset = project.blocks[std::min(ui.selected_block, project.blocks.size() - 1U)];
        task.what = asset.name;
        task.build = std::async(std::launch::async, [path, options, done, name = asset.name, mesh = asset.data] {
            MeshExportTask::Built built;
            const auto buffer = build_block_mesh_render_buffer(mesh);
            ExportMeshGroup group;
            group.name = name;
            group.buffer = &buffer;
            built.job = build_gltf_export(path, {group}, options, built.error);
            *done = 1U;
            return built;
        });
        return task;
    }

    if (ui.tab == EditorTab::City) {
        if (!project.city) {
            report_failure(status, "Export failed: no city loaded.");
            return std::nullopt;
        }
        const auto placement_buildings = resolve_placement_buildings(project);
        if (options.include_buildings) {
            for (std::size_t i = 0; i < placement_buildings.size(); ++i) if (placement_buildings[i] == nullptr) {
                report_failure(status, "Export failed: missing or ambiguous building " + project.city->data.buildings[i].name);
                return std::nullopt;
            }
        }
        // One copy of each building asset; placements point into it.
        std::vector<SbbBuilding> assets;
        std::vector<std::size_t> placement_asset(placement_buildings.size(), SIZE_MAX);
        std::unordered_map<const SbbBuilding*, std::size_t> copied;
        for (std::size_t i = 0; i < placement_buildings.size(); ++i) {
            if (placement_buildings[i] == nullptr) continue;
            auto [it, inserted] = copied.emplace(placement_buildings[i], assets.size());
            if (inserted) assets.push_back(*placement_buildings[i]);
            placement_asset[i] = it->second;
        }
        task.what = project.city->path.stem().string();
        // The scene, one mesh for each building asset, and the pieces.
        task.steps_total = assets.size() + 2U;
        task.build = std::async(std::launch::async,
            [path, options, done, map = project.city->data, assets = std::move(assets),
             auto_generated = project_is_auto_generated(project),
             placement_asset = std::move(placement_asset), blocks = std::move(blocks),
             templates = std::move(templates), texture_names = project_grid_texture_names(project)] {
                PAFTA_PROFILE_THREAD("Mesh export");
                MeshExportTask::Built built;
                std::vector<const SbbBuilding*> placements(placement_asset.size(), nullptr);
                for (std::size_t i = 0; i < placements.size(); ++i) {
                    if (placement_asset[i] != SIZE_MAX) placements[i] = &assets[placement_asset[i]];
                }
                const auto scene = build_map_scene(map, placements, auto_generated);
                ++*done;
                std::vector<BuildingRenderBuffer> asset_buffers(assets.size());
                if (options.include_buildings) {
                    for (std::size_t i = 0; i < assets.size(); ++i) {
                        asset_buffers[i] = build_native_building_render_buffer(assets[i], blocks, templates);
                        ++*done;
                    }
                }
                std::vector<const BuildingRenderBuffer*> placement_buffers(placements.size(), nullptr);
                for (std::size_t i = 0; i < placements.size(); ++i) {
                    if (placement_asset[i] != SIZE_MAX) placement_buffers[i] = &asset_buffers[placement_asset[i]];
                }
                // Piece and portal geometry (districts placed with the Piece tool): the same
                // textured buffer the city view draws through the portal render model.
                const auto piece_buffer = options.include_pieces
                    ? build_native_portal_render_buffer(map, blocks, std::nullopt, templates, texture_names, placements)
                    : BuildingRenderBuffer{};
                ++*done;
                built.job = build_gltf_export(path,
                    city_export_groups(scene, placement_buffers, placements, piece_buffer, options), options, built.error);
                return built;
            });
        return task;
    }

    report_failure(status, "Export failed: this view has no mesh to export.");
    return std::nullopt;
}

// Moves an export forward by one frame and draws its progress popup. Clears the
// task when it is done.
void service_mesh_export(std::optional<MeshExportTask>& task, const EditorProject& project, std::string& status) {
    if (!task) return;
    const auto ready = [](auto& future) {
        return future.valid() && future.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready;
    };
    const auto close_progress = [] {
        if (ImGui::IsPopupOpen("Exporting") &&
            ImGui::BeginPopupModal("Exporting", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    };
    std::string stage;
    float fraction = 0.0F;
    try {
        if (task->build.valid()) {
            if (ready(task->build)) {
                auto built = task->build.get();
                if (!built.job) throw std::runtime_error(built.error.message);
                task->job = std::move(built.job);
            } else {
                stage = "Building meshes";
                fraction = static_cast<float>(task->steps_done->load()) / static_cast<float>(std::max<std::size_t>(1U, task->steps_total));
            }
        }
        if (task->job && !task->write.valid()) {
            // Reading textures back needs the GL thread, so spend part of each frame on it.
            const auto start = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds{8} &&
                   write_next_gltf_texture(*task->job, project)) {}
            const auto total = gltf_export_texture_count(*task->job);
            const auto written = gltf_export_textures_done(*task->job);
            if (written >= total) {
                task->write = std::async(std::launch::async, [job = task->job.get()] { return write_gltf_export(*job); });
            } else {
                stage = "Writing textures (" + std::to_string(written) + " of " + std::to_string(total) + ")";
                fraction = static_cast<float>(written) / static_cast<float>(std::max<std::size_t>(1U, total));
            }
        }
        if (task->write.valid()) {
            if (ready(task->write)) {
                const auto result = task->write.get();
                if (!result.ok) throw std::runtime_error(result.message);
                status = result.message;
                task.reset();
                close_progress();
                return;
            }
            stage = "Writing file";
            fraction = 1.0F;
        }
    } catch (const std::exception& error) {
        task.reset();
        close_progress();
        report_failure(status, std::string{"Export failed: "} + error.what());
        return;
    }
    if (!ImGui::IsPopupOpen("Exporting")) ImGui::OpenPopup("Exporting");
    ImGui::SetNextWindowSize(ImVec2(420.0F, 0.0F));
    if (ImGui::BeginPopupModal("Exporting", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::Text("Exporting %s", task->what.c_str());
        ImGui::TextDisabled("%s", stage.c_str());
        ImGui::ProgressBar(fraction, ImVec2(-1.0F, 0.0F));
        ImGui::EndPopup();
    }
}
#endif

// Starts rebuilding the city scene in the background (roads, terrain, and the
// footprint stamps) for the given fingerprint. The worker owns copies of the map
// and the .sbb data, so the main thread can't change or free anything it uses
// halfway through. GPU uploads stay on the main thread (see the caller).
void start_city_scene_rebuild(EditorProject& project, std::uint64_t fingerprint) {
    PAFTA_PROFILE_FUNCTION();
    if (!project.city) {
        return;
    }
    auto map_copy = project.city->data;
    // Copy each building asset once. The worker rebuilds its pointers over its own
    // copies, so they can't dangle. resolved[i] is placement i's index into `owned`
    // plus one, or 0 when it has no .sbb.
    auto owned = std::make_shared<std::vector<SbbBuilding>>();
    std::vector<std::size_t> resolved;
    resolved.reserve(project.city->data.buildings.size());
    std::unordered_map<const LoadedAsset<SbbBuilding>*, std::size_t> copied;
    for (const auto& placement : project.city->data.buildings) {
        const auto* asset = find_building_asset(project, placement.name);
        if (asset == nullptr) {
            resolved.push_back(0U);
            continue;
        }
        auto [it, inserted] = copied.emplace(asset, owned->size() + 1U);
        if (inserted) owned->push_back(asset->data);
        resolved.push_back(it->second);
    }

    // v10 district cells under a building are replaced by it, so the portal buffer
    // has to follow building changes.
    if (project.city->data.city_blocks.empty()) invalidate_portal_render_models(project);
    project.city_scene_build_in_progress = true;
    project.city_scene_build_target_fingerprint = fingerprint;
    const auto generation = ++project.city_scene_build_generation;
    auto build_scene = [map = std::move(map_copy), owned, resolved = std::move(resolved), generation,
                        auto_generated = project_is_auto_generated(project)]() {
            PAFTA_PROFILE_THREAD("City scene rebuild");
            PAFTA_PROFILE_BLOCK("build async map scene");
            std::vector<const SbbBuilding*> placement_buildings;
            placement_buildings.reserve(resolved.size());
            for (std::size_t i = 0; i < resolved.size(); ++i) {
                placement_buildings.push_back(resolved[i] != 0U ? &(*owned)[resolved[i] - 1U] : nullptr);
            }
            (void)generation;
            return build_map_scene(map, placement_buildings, auto_generated);
        };
#ifdef __EMSCRIPTEN__
    project.city_scene_cache = build_scene();
    project.city_scene_cache_fingerprint = fingerprint;
    project.city_scene_cache_valid = true;
    project.city_scene_gpu_valid = false;
    project.city_scene_build_in_progress = false;
#else
    project.city_scene_build_future = std::async(std::launch::async, std::move(build_scene));
#endif
}

std::vector<std::pair<std::size_t, Matrix>>& near_lod_placements() {
    static std::vector<std::pair<std::size_t, Matrix>> placements;
    return placements;
}

void draw_city_building_models_full_detail(EditorProject& project, const Camera3D& camera, Vector2 viewport_size) {
    if (!project.city) {
        return;
    }

    // Get the camera's 6 world-space frustum planes from its view-projection matrix
    // (Gribb-Hartmann), once per frame. A sphere is only culled when it is fully
    // outside one plane. That is exact from every angle, so a box that crosses a
    // plane at a grazing angle is never thrown away by mistake.
    struct Plane {
        float a, b, c, d;
    };
    std::array<Plane, 6> frustum_planes{};
    bool frustum_valid = false;
    // Orthographic views skip culling; their box isn't a perspective frustum.
    if (viewport_size.x > 0.0F && viewport_size.y > 0.0F && camera.projection == CAMERA_PERSPECTIVE) {
        const auto aspect = viewport_size.x / viewport_size.y;
        const auto view = GetCameraMatrix(camera);
        const auto proj = MatrixPerspective(
            camera.fovy * 0.017453292519943295,
            static_cast<double>(aspect),
            rlGetCullDistanceNear(),
            rlGetCullDistanceFar());
        const auto m = MatrixMultiply(view, proj);
        // The planes come from the rows of the view-projection matrix. raylib's Matrix is
        // column-major (m0, m4, m8, m12 are the first column).
        auto make = [](float a, float b, float c, float d) {
            const auto len = std::sqrt(a * a + b * b + c * c);
            const auto inv = len > 1e-8F ? 1.0F / len : 0.0F;
            return Plane{a * inv, b * inv, c * inv, d * inv};
        };
        frustum_planes[0] = make(m.m3 + m.m0, m.m7 + m.m4, m.m11 + m.m8, m.m15 + m.m12);   // left
        frustum_planes[1] = make(m.m3 - m.m0, m.m7 - m.m4, m.m11 - m.m8, m.m15 - m.m12);   // right
        frustum_planes[2] = make(m.m3 + m.m1, m.m7 + m.m5, m.m11 + m.m9, m.m15 + m.m13);   // bottom
        frustum_planes[3] = make(m.m3 - m.m1, m.m7 - m.m5, m.m11 - m.m9, m.m15 - m.m13);   // top
        frustum_planes[4] = make(m.m3 + m.m2, m.m7 + m.m6, m.m11 + m.m10, m.m15 + m.m14);  // near
        frustum_planes[5] = make(m.m3 - m.m2, m.m7 - m.m6, m.m11 - m.m10, m.m15 - m.m14);  // far
        frustum_valid = true;
    }

    // The closest distance from the camera to the bounds of the last placement tested.
    float placement_distance = 0.0F;
    auto visible_in_viewport = [&](const SbbBuilding& building, const SbcBuilding& placement) {
        if (!frustum_valid) {
            return true;
        }

        constexpr auto unit_size = 4.0F;
        const float min_x = -static_cast<float>(building.anchor_x) * unit_size;
        const float max_x = (static_cast<float>(std::max(1U, building.size_x)) - static_cast<float>(building.anchor_x)) * unit_size;
        const float min_y = -static_cast<float>(building.anchor_y) * unit_size;
        const float max_y = (static_cast<float>(std::max(1U, building.size_z)) - static_cast<float>(building.anchor_y)) * unit_size;
        const float min_z = -static_cast<float>(building.anchor_z) * unit_size;
        const float max_z = (static_cast<float>(std::max(1U, building.size_y)) - static_cast<float>(building.anchor_z)) * unit_size;
        const std::array<Vec3, 8> local_corners{{
            {min_x, min_y, min_z},
            {max_x, min_y, min_z},
            {max_x, max_y, min_z},
            {min_x, max_y, min_z},
            {min_x, min_y, max_z},
            {max_x, min_y, max_z},
            {max_x, max_y, max_z},
            {min_x, max_y, max_z},
        }};

        Vector3 center{};
        std::array<Vector3, 8> world_corners{};
        std::size_t corner_index = 0;
        for (const auto& local : local_corners) {
            const auto world = native_city_building_transform_point(placement, local, unit_size);
            world_corners[corner_index++] = Vector3{world.x, world.y, world.z};
            center.x += world.x;
            center.y += world.y;
            center.z += world.z;
        }
        center.x /= static_cast<float>(world_corners.size());
        center.y /= static_cast<float>(world_corners.size());
        center.z /= static_cast<float>(world_corners.size());

        auto radius = 0.0F;
        for (const auto& corner : world_corners) {
            const auto dx = corner.x - center.x;
            const auto dy = corner.y - center.y;
            const auto dz = corner.z - center.z;
            radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz));
        }

        {
            const auto dx = center.x - camera.position.x;
            const auto dy = center.y - camera.position.y;
            const auto dz = center.z - camera.position.z;
            placement_distance = std::max(0.0F, std::sqrt(dx * dx + dy * dy + dz * dz) - radius);
        }
        // How far buildings are drawn (Preferences > Performance); 0 means no limit.
        if (const auto far = g_render_settings.building_distance; far > 0.0F && placement_distance > far) return false;
        // Only cull when the bounding sphere is fully outside one of the planes.
        for (const auto& p : frustum_planes) {
            const auto distance = p.a * center.x + p.b * center.y + p.c * center.z + p.d;
            if (distance < -radius) {
                return false;
            }
        }
        return true;
    };

    // All visible placements of one template are drawn with one instanced draw per
    // material, so the number of draw calls grows with templates, not placements.
    refresh_placement_override_models(project);
    const auto& placement_asset_indices = resolve_placement_asset_indices(project);
    std::vector<std::pair<const GpuMesh*, Matrix>> overridden;
    std::map<const GpuMesh*, std::vector<Matrix>> near_by_asset;
    near_lod_placements().clear();
    int near_builds_left = 1;
    static std::vector<std::vector<Matrix>> transforms_by_asset;
    transforms_by_asset.resize(project.buildings.size());
    for (auto& transforms : transforms_by_asset) transforms.clear();
    for (std::size_t placement_index = 0; placement_index < project.city->data.buildings.size(); ++placement_index) {
        const auto& placement = project.city->data.buildings[placement_index];
        if (!placement_asset_indices[placement_index].has_value()) {
            continue;
        }
        const auto asset_index = *placement_asset_indices[placement_index];
        // No GPU model yet means the asset is still being prepared. It shows up as soon
        // as its render model is uploaded.
        if (asset_index >= project.building_gpu_models.size() || project.building_gpu_models[asset_index].empty() ||
            !visible_in_viewport(project.buildings[asset_index].data, placement)) {
            continue;
        }
        if (const auto found = project.placement_override_models.find(placement_index); found != project.placement_override_models.end()) {
            overridden.push_back({&found->second.mesh, city_placement_matrix(placement)});
            continue;
        }
        // The game's near view (_render_buildsectorlist): within kNearLodDistance, the
        // rooms, open doorways, and glass replace the reflective shell. With
        // "selected buildings only", the selection decides instead of the distance. Near meshes are
        // built when needed, one template per frame, and the shell is shown until then.
        const bool near = g_render_settings.interiors_selected_only
            ? std::ranges::find(g_render_settings.interior_placements, placement_index) != g_render_settings.interior_placements.end()
            : placement_distance < kNearLodDistance;
        if (g_render_settings.near_lod && near) {
            if (const auto* near = building_near_mesh(project, asset_index, near_builds_left)) {
                near_by_asset[near].push_back(city_placement_matrix(placement));
                near_lod_placements().push_back({asset_index, city_placement_matrix(placement)});
                continue;
            }
        }
        transforms_by_asset[asset_index].push_back(city_placement_matrix(placement));
    }
    std::vector<GpuMeshDraw> draws;
    draws.reserve(overridden.size() + transforms_by_asset.size() + near_by_asset.size());
    for (const auto& [mesh, transform] : overridden) draws.push_back({mesh, std::span<const Matrix>(&transform, 1)});
    for (std::size_t asset_index = 0; asset_index < transforms_by_asset.size(); ++asset_index) {
        draws.push_back({&project.building_gpu_models[asset_index].mesh, transforms_by_asset[asset_index]});
    }
    std::vector<GpuMeshDraw> near_draws;
    for (const auto& [mesh, transforms] : near_by_asset) near_draws.push_back({mesh, transforms});
    set_game_render_depth_output(true);
    draw_gpu_mesh_batch(draws, GpuSurface::Building);
    draw_gpu_mesh_batch(near_draws, GpuSurface::Building, GpuPartFilter::Opaque);
    set_game_render_depth_output(false);
    // Glass goes last, blended over whatever is behind it.
    draw_gpu_mesh_batch(near_draws, GpuSurface::Building, GpuPartFilter::Glass);
    evict_building_near_models(project);
}

void draw_city_building_mask_for_placement(EditorProject& project, const SbcBuilding* target) {
    if (!project.city || target == nullptr) {
        return;
    }

    const auto placement_index = static_cast<std::size_t>(target - project.city->data.buildings.data());
    if (placement_index >= project.city->data.buildings.size()) {
        return;
    }

    const auto& placement = project.city->data.buildings[placement_index];
    const auto asset = find_building_asset(project, placement.name);
    if (asset == nullptr) {
        return;
    }

    const auto asset_index = static_cast<std::size_t>(asset - project.buildings.data());
    if (asset_index >= project.building_gpu_models.size()) {
        return;
    }

    draw_city_building_mask(project.building_gpu_models[asset_index], placement);
}

std::vector<std::string> city_street_names(const SbcMap& map) {
    std::vector<std::string> names;
    names.reserve(map.streets.size());
    for (const auto& street : map.streets) {
        names.push_back(std::to_string(street.index) + " " + street.name);
    }
    return names;
}

std::optional<std::filesystem::path> dropped_folder() {
    if (!IsFileDropped()) {
        return std::nullopt;
    }

    auto dropped = LoadDroppedFiles();
    std::optional<std::filesystem::path> folder;
    if (dropped.count > 0) {
        folder = std::filesystem::path{dropped.paths[0]};
    }
    UnloadDroppedFiles(dropped);
    return folder;
}

void set_status(EditorProject& project) {
    project.status =
        std::to_string(project.city.has_value() ? 1 : 0) + " city | " +
        std::to_string(project.buildings.size()) + " buildings | " +
        std::to_string(project.blocks.size()) + " blocks | " +
        std::to_string(project.buildblocks.size()) + " buildblocks";
    if (!project.load_warnings.empty()) {
        project.status += " | " + std::to_string(project.load_warnings.size()) + " skipped";
    }
}

// Finds a cell by grid position in the y->z->x cell array, or returns nullptr.
SbbCell* building_cell_at(SbbBuilding& building, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    for (auto& cell : building.cells) {
        if (cell.x == x && cell.y == y && cell.z == z) {
            return &cell;
        }
    }
    return nullptr;
}

// The solid-cell marker the building renderer looks for:
// is_solid_building_cell checks exterior_block>>29 == 2 (the 0x40000000 kind),
// and interior_block == 0xffff picks the generated room. A plain block number
// has no solid-cell marker and does not get this geometry.
constexpr std::uint32_t kSolidExteriorBlock = 0x40000000U;
constexpr std::uint32_t kSolidInteriorBlock = 0x0000FFFFU;

// Check both mesh references before a placement changes the grid, the cell, or
// the name list. Each packed reference has a 10-bit name index.
std::string block_config_problem(const SbbBuilding& building, const BlockToolConfig& config, const EditorProject& project) {
    std::vector<std::string> new_names;
    for (const auto& [enabled, index] : {
            std::pair{config.exterior && config.exterior_piece, config.exterior_piece_block},
            std::pair{config.interior && config.interior_piece, config.interior_piece_block}}) {
        if (!enabled) continue;
        if (index >= project.blocks.size()) return "the selected piece mesh is no longer available";
        const auto name = lower_name(project.blocks[index].name);
        const auto existing = std::ranges::find_if(building.piece_names, [&](const auto& candidate) {
            return lower_name(candidate) == name;
        });
        if (existing != building.piece_names.end()) {
            if (existing - building.piece_names.begin() > 0x3ff) return "the piece-name index exceeds the 1024-name limit";
        } else if (std::ranges::find(new_names, name) == new_names.end()) {
            new_names.push_back(name);
        }
    }
    if (building.piece_names.size() + new_names.size() > 1024) {
        return "the building's 1024-entry piece-name table is full";
    }
    return {};
}

// Writes the turned-on layers of the Place Block settings into a cell, like
// BuildingTool_WriteCellFromCurrentSelection @1400672a0 does: the exterior word
// and its texture set (slots 0..7), then the interior word, cell type, placed
// flag (bit 0 is the room's floor), and interior set (slots 8..15). Rooms in the
// game's buildings have placed flag 1; without it a room has no floor. The Floor
// setting can leave it off to join a room with the one below.
void apply_block_config(SbbCell& cell, const BlockToolConfig& config, SbbBuilding& building, const EditorProject& project) {
    const auto word = [&](bool piece, std::size_t block, std::uint32_t normal) {
        if (!piece) return normal;
        if (block >= project.blocks.size()) throw std::runtime_error("Selected piece mesh is unavailable");
        const auto key = lower_name(project.blocks[block].name);
        auto it = std::ranges::find_if(building.piece_names, [&](const std::string& n) { return lower_name(n) == key; });
        if (it == building.piece_names.end()) {
            if (building.piece_names.size() > 0x3ffU) throw std::runtime_error("Building piece-name table is full");
            building.piece_names.push_back(project.blocks[block].name);
            it = std::prev(building.piece_names.end());
        }
        const auto index = static_cast<std::uint32_t>(it - building.piece_names.begin());
        return 0x80000000U | (config.rotation & 3U) << 24U | index;
    };
    const auto material = [&](std::size_t index) -> std::uint16_t {
        return building.materials.empty() ? std::uint16_t{0} : static_cast<std::uint16_t>(std::min(index, building.materials.size() - 1U));
    };
    if (config.exterior) {
        cell.exterior_block() = word(config.exterior_piece, config.exterior_piece_block, kSolidExteriorBlock);
        for (std::size_t slot = 0; slot < 8; ++slot) cell.shorts[slot] = material(config.exterior_textures[slot]);
    }
    if (config.interior) {
        cell.interior_block() = word(config.interior_piece, config.interior_piece_block, kSolidInteriorBlock);
        cell.cell_type() = static_cast<std::uint32_t>(
            building.block_names.empty() ? 0U : std::min(config.buildblock_index, building.block_names.size() - 1U));
        cell.placed_flag() = config.room_floor ? 1U : 0U;
        for (std::size_t slot = 0; slot < 8; ++slot) cell.shorts[8 + slot] = material(config.interior_textures[slot]);
    }
    refresh_sbb_cell_payloads(cell);
}

// Place Block: fills the empty cell in front of the face you clicked with the
// current block settings (outside and inside shells, buildblock, textures).
// Returns the new cell's index, or nullopt when nothing was placed.
std::optional<std::size_t> place_building_block(EditorProject& project, EditorUi& ui, const BuildingFacePick& pick, Vec3 ray_dir,
                                                float unit_size, std::string& status) {
    if (project.buildings.empty()) {
        return std::nullopt;
    }
    ui.selected_building = std::min(ui.selected_building, project.buildings.size() - 1U);
    auto& building = project.buildings[ui.selected_building].data;
    if (!ui.block_config.exterior && !ui.block_config.interior) {
        status = "enable Exterior or Interior in the block panel first";
        return std::nullopt;
    }
    if (pick.cell_index >= building.cells.size()) {
        return std::nullopt;
    }
    if (const auto problem = block_config_problem(building, ui.block_config, project); !problem.empty()) {
        status = problem;
        return std::nullopt;
    }
    if (ui.block_config.apply_to_clicked) {
        auto& clicked = building.cells[pick.cell_index];
        apply_block_config(clicked, ui.block_config, building, project);
        mark_building_changed(project, ui);
        status = "changed block at [" + std::to_string(clicked.x) + "," + std::to_string(clicked.y) + "," + std::to_string(clicked.z) + "]";
        return pick.cell_index;
    }
    // The empty cell in front of the clicked face: one step back along the ray from
    // the hit, in cells relative to the anchor (the edit view's space).
    const Vec3 front{pick.hit_world.x - ray_dir.x * 0.05F, pick.hit_world.y - ray_dir.y * 0.05F, pick.hit_world.z - ray_dir.z * 0.05F};
    const std::int64_t nx = static_cast<std::int64_t>(std::floor(front.x / unit_size)) + building.anchor_x;
    const std::int64_t ny = static_cast<std::int64_t>(std::floor(front.y / unit_size)) + building.anchor_y;
    const std::int64_t nz = static_cast<std::int64_t>(std::floor(front.z / unit_size)) + building.anchor_z;
    const auto coordinate = shift_sbb_grid_for_cell(building, nx, ny, nz);
    if (!coordinate) {
        status = "placement would exceed the supported 64-cell building grid";
        return std::nullopt;
    }
    const auto [x, y, z] = *coordinate;
    auto* target = building_cell_at(building, x, y, z);
    if (target == nullptr) {
        SbbCell cell{};
        cell.index = building.cells.size();
        cell.x = x;
        cell.y = y;
        cell.z = z;
        cell.shorts.fill(0xffffU);
        building.cells.push_back(cell);
        target = &building.cells.back();
    }
    if (target->occupied) {
        status = "neighbour cell already filled";
        return std::nullopt;
    }
    apply_block_config(*target, ui.block_config, building, project);
    mark_building_changed(project, ui);
    status = "placed block at [" + std::to_string(target->x) + "," + std::to_string(target->y) + "," + std::to_string(target->z) + "]";
    return static_cast<std::size_t>(target - building.cells.data());
}

void place_building_item_set(EditorProject& project, EditorUi& ui, std::size_t cell_index, std::string& status) {
    auto& building = project.buildings[ui.selected_building].data;
    if (cell_index >= building.cells.size()) return;
    if (!ui.clear_item_set && ui.selected_item_set >= building.item_set_names.size()) {
        status = "add an item-set name in Properties first";
        return;
    }
    auto& cell = building.cells[cell_index];
    const auto occupied = occupied_cell_indices(building);
    const auto selected = std::ranges::find(occupied, cell_index);
    if (selected != occupied.end()) ui.selected_building_cell = static_cast<std::size_t>(selected - occupied.begin());
    std::uint32_t value = 0U;
    if (!ui.clear_item_set) {
        const auto packed = sbb_item_set_payload(ui.selected_item_set, ui.item_set_rotation);
        if (!packed) {
            status = "item-set index or rotation does not fit the game's placement word";
            return;
        }
        value = *packed;
    }
    if (cell.item_set_index == value) return;
    cell.item_set_index = value;
    mark_building_changed(project, ui);
    const auto action = ui.clear_item_set ? std::string{"cleared item set"} : "placed " + building.item_set_names[ui.selected_item_set];
    status = action + " at [" +
        std::to_string(cell.x) + "," + std::to_string(cell.y) + "," + std::to_string(cell.z) + "]";
}

// Paint Texture: writes the chosen material into the texture slot of the face you
// clicked (cell.shorts 0..7 outside, 8..15 inside), as its mesh tag says.
void paint_building_face(EditorProject& project, EditorUi& ui, std::size_t cell_index, std::uint32_t slot, std::string& status) {
    if (project.buildings.empty()) {
        return;
    }
    ui.selected_building = std::min(ui.selected_building, project.buildings.size() - 1U);
    auto& building = project.buildings[ui.selected_building].data;
    if (cell_index >= building.cells.size()) {
        return;
    }
    auto& cell = building.cells[cell_index];
    if (slot >= cell.shorts.size()) {
        status = "this face has no texture slot";
        return;
    }
    const auto material = building.materials.empty()
        ? std::uint16_t{0}
        : static_cast<std::uint16_t>(std::min(ui.paint_material_index, building.materials.size() - 1U));
    cell.shorts[slot] = material;
    mark_building_changed(project, ui);
    const auto name = material < building.materials.size() ? building.materials[material] : std::string{"(none)"};
    status = std::string(slot >= 8U ? "interior" : "exterior") + " face painted " + name;
}

// Paint Wall type: writes ui.paint_code into the side-code word the renderer
// reads for the clicked wall. Its outside face and its room face use the same word
// (Sbb_BuildBuildingConnectivityMetadata and the room key): north is the cell's
// +0x0c, west its +0x10, east the +x cell's +0x10, and south the +z cell's +0x0c.
// Shell slots 1..4 are S/W/N/E and room slots 9..12 are N/E/S/W.
// A layer code also points the cell at the chosen buildblock.
void paint_building_side_code(EditorProject& project, EditorUi& ui, std::size_t cell_index, std::uint32_t slot, std::string& status) {
    if (project.buildings.empty()) return;
    ui.selected_building = std::min(ui.selected_building, project.buildings.size() - 1U);
    auto& building = project.buildings[ui.selected_building].data;
    if (cell_index >= building.cells.size()) return;
    const auto face = slot % 8U;
    if (face < 1U || face > 4U) {
        status = "only walls take a wall type";
        return;
    }
    // 0 N, 1 E, 2 S, 3 W
    static constexpr std::array<int, 4> shell_side{2, 3, 0, 1};
    const int side = slot >= 8U ? static_cast<int>(face) - 1 : shell_side[face - 1U];
    auto& cell = building.cells[cell_index];
    SbbCell* target = &cell;
    std::size_t word = SbbCell::exterior_portal_face_word;
    if (side == 3) word = SbbCell::interior_portal_face_word;
    if (side == 1) {
        target = building_cell_at(building, cell.x + 1, cell.y, cell.z);
        word = SbbCell::interior_portal_face_word;
    }
    if (side == 2) target = building_cell_at(building, cell.x, cell.y, cell.z + 1);
    if (target == nullptr) {
        status = "this wall's type is stored in the neighbouring cell; place a block there first";
        return;
    }
    if (ui.paint_code >= 3U) {
        if (building.block_names.empty()) {
            status = "add a buildblock to this building first";
            return;
        }
        cell.cell_type() = static_cast<std::uint32_t>(std::min(ui.block_config.buildblock_index, building.block_names.size() - 1U));
    }
    target->words[word] = (target->words[word] & ~0xfU) | ui.paint_code;
    mark_building_changed(project, ui);
    status = "wall type set to " + std::to_string(ui.paint_code);
}

BoundingBox city_building_bounds(const SbcBuilding& placement, const SbbBuilding& asset, float unit);

// The middle of a placed building's footprint, at its render-origin height. The
// gizmo sits here and rotation turns around it. Uses the render origin when the
// template is not loaded.
Vector3 city_building_center(const EditorProject& project, const SbcBuilding& placement) {
    const auto o = native_city_building_render_origin(placement, 4.0F);
    const auto* asset = find_building_asset(project, placement.name);
    if (asset == nullptr) return {o.x, o.y, o.z};
    const auto bounds = city_building_bounds(placement, asset->data, 4.0F);
    return {(bounds.min.x + bounds.max.x) * 0.5F, o.y, (bounds.min.z + bounds.max.z) * 0.5F};
}

// Where the gizmo sits relative to the render origin (see city_building_center).
Vector3 city_building_gizmo_offset(const EditorProject& project, const SbcBuilding& placement) {
    const auto o = native_city_building_render_origin(placement, 4.0F);
    const auto c = city_building_center(project, placement);
    return {c.x - o.x, 0.0F, c.z - o.z};
}

// Turns a building in quarter turns around the middle of its footprint, so it spins
// in place instead of swinging around its anchor corner.
void rotate_city_building_in_place(const EditorProject& project, SbcBuilding& building, int drot) {
    const auto before = city_building_center(project, building);
    building.rotation = (building.rotation + drot) & 3;
    const auto after = city_building_center(project, building);
    building.x += static_cast<int>(std::lround((before.x - after.x) / 4.0F));
    building.z += static_cast<int>(std::lround((before.z - after.z) / 4.0F));
}

void nudge_city_building(EditorProject& project, EditorUi& ui, int dx, int dy, int dz, int drot) {
    if (!project.city || project.city->data.buildings.empty()) {
        return;
    }

    ui.selected_city_building = std::min(ui.selected_city_building, project.city->data.buildings.size() - 1U);
    auto& building = project.city->data.buildings[ui.selected_city_building];
    building.x += dx;
    building.y += dy;
    building.z += dz;
    if (drot != 0) rotate_city_building_in_place(project, building, drot);
    ui.city_dirty = true;
}

void reindex_city_buildings(SbcMap& map) {
    for (std::size_t index = 0; index < map.buildings.size(); ++index) {
        map.buildings[index].index = index;
    }
}

void reindex_city_streets(SbcMap& map) {
    for (std::size_t index = 0; index < map.streets.size(); ++index) {
        map.streets[index].index = index;
    }
}

std::size_t add_city_intersection(SbcMap& map, CityGridPoint point) {
    map.intersections.push_back({
        map.intersections.size(),
        point.x,
        point.y,
        point.z,
    });
    return map.intersections.back().index;
}

void set_city_building_position(EditorProject& project, EditorUi& ui, CityGridPoint point) {
    if (!project.city || project.city->data.buildings.empty()) {
        return;
    }

    auto& buildings = project.city->data.buildings;
    ui.selected_city_building = std::min(ui.selected_city_building, buildings.size() - 1U);
    auto& building = buildings[ui.selected_city_building];
    building.x = point.x;
    building.y = point.y;
    building.z = point.z;
    ui.city_dirty = true;
}

void duplicate_city_building(EditorProject& project, EditorUi& ui) {
    if (!project.city || project.city->data.buildings.empty()) {
        return;
    }

    auto& buildings = project.city->data.buildings;
    ui.selected_city_building = std::min(ui.selected_city_building, buildings.size() - 1U);
    auto building = buildings[ui.selected_city_building];
    building.index = buildings.size();
    building.x += 4;
    building.z += 4;
    buildings.push_back(std::move(building));
    ui.selected_city_building = buildings.size() - 1U;
    ui.city_dirty = true;
}

void delete_city_building(EditorProject& project, EditorUi& ui) {
    if (!project.city || project.city->data.buildings.empty()) {
        return;
    }

    auto& map = project.city->data;
    // Delete everything selected if several buildings are selected (going from the
    // highest index down keeps the other indices valid), otherwise just the active one.
    std::vector<std::size_t> targets = ui.selected_city_buildings.size() > 1U
        ? ui.selected_city_buildings
        : std::vector<std::size_t>{std::min(ui.selected_city_building, map.buildings.size() - 1U)};
    std::sort(targets.begin(), targets.end(), std::greater<>());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    for (const auto idx : targets) {
        if (idx < map.buildings.size()) {
            map.buildings.erase(map.buildings.begin() + static_cast<std::ptrdiff_t>(idx));
        }
    }
    reindex_city_buildings(map);
    ui.selected_city_buildings.clear();
    ui.selected_city_building = map.buildings.empty() ? 0U : std::min(ui.selected_city_building, map.buildings.size() - 1U);
    if (map.buildings.empty()) {
        ui.city_selection = CitySelectionKind::None;
    }
    ui.city_dirty = true;
}

// World bounds of all the cells of a placed building.
BoundingBox city_building_bounds(const SbcBuilding& placement, const SbbBuilding& asset, float unit) {
    BoundingBox bounds{{FLT_MAX, FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX, -FLT_MAX}};
    for (int corner = 0; corner < 8; ++corner) {
        const Vec3 local{
            ((corner & 1 ? static_cast<float>(asset.size_x + 1) : 0.0F) - asset.anchor_x) * unit,
            ((corner & 2 ? static_cast<float>(asset.size_z) : 0.0F) - asset.anchor_y) * unit,
            ((corner & 4 ? static_cast<float>(asset.size_y + 1) : 0.0F) - asset.anchor_z) * unit};
        const auto p = native_city_building_transform_point(placement, local, unit);
        bounds.min = Vector3Min(bounds.min, {p.x, p.y, p.z});
        bounds.max = Vector3Max(bounds.max, {p.x, p.y, p.z});
    }
    return bounds;
}

// What the Building tool would place at the cursor: the chosen template
// (ui.place_building_template) turned by ui.place_building_rotation, with its
// footprint centred on the cursor and its lowest floor on the clicked surface.
// The ghost preview and the click both use this, so it lands where you see it.
SbcBuilding city_building_placement_at(const EditorProject& project, const EditorUi& ui, CityGridPoint point) {
    constexpr float unit = 4.0F;
    const auto& tmpl = project.buildings[std::min(ui.place_building_template, project.buildings.size() - 1U)];
    SbcBuilding placement{};
    placement.name = tmpl.name;
    placement.x = point.x;
    // The record's y is the level of the ANCHOR layer (CitySbc_Load pairs size_z with
    // anchor_y). Floors below the anchor are foundation. To stand the lowest floor on
    // the clicked surface, the anchor goes anchor_y levels above it. Without this,
    // houses with anchor_y 1 or 2 sank by that much.
    placement.y = point.y + static_cast<std::int32_t>(tmpl.data.anchor_y);
    placement.z = point.z;
    placement.rotation = ui.place_building_rotation & 3;
    const auto bounds = city_building_bounds(placement, tmpl.data, unit);
    placement.x -= static_cast<std::int32_t>(std::lround(((bounds.min.x + bounds.max.x) * 0.5F - point.x * unit) / unit));
    placement.z -= static_cast<std::int32_t>(std::lround(((bounds.min.z + bounds.max.z) * 0.5F - point.z * unit) / unit));
    return placement;
}

// What a Building-tool placement would overlap: another building or the surface of
// a road. Empty when the spot is clear. You can still place there; this only turns
// the preview red and shows the warning.
std::string city_building_placement_conflict(EditorProject& project, const SbcBuilding& ghost) {
    constexpr float unit = 4.0F;
    const auto* tmpl = find_building_asset(project, ghost.name);
    if (!project.city || tmpl == nullptr) return {};
    auto box = city_building_bounds(ghost, tmpl->data, unit);
    // Touching edges are fine; only a real overlap counts.
    box.min = Vector3AddValue(box.min, 0.5F);
    box.max = Vector3SubtractValue(box.max, 0.5F);
    const auto& map = project.city->data;
    const auto& asset_indices = resolve_placement_asset_indices(project);
    for (std::size_t i = 0; i < map.buildings.size(); ++i) {
        if (i >= asset_indices.size() || !asset_indices[i]) continue;
        if (CheckCollisionBoxes(box, city_building_bounds(map.buildings[i], project.buildings[*asset_indices[i]].data, unit))) {
            return "Overlaps building " + map.buildings[i].name;
        }
    }
    for (const auto& street : map.streets) {
        if (street.from_intersection < 0 || street.to_intersection < 0 ||
            static_cast<std::size_t>(street.from_intersection) >= map.intersections.size() ||
            static_cast<std::size_t>(street.to_intersection) >= map.intersections.size()) continue;
        const auto& a = map.intersections[static_cast<std::size_t>(street.from_intersection)];
        const auto& b = map.intersections[static_cast<std::size_t>(street.to_intersection)];
        // Roads run along an axis; widen the centre line by half the total width.
        const float half = static_cast<float>(street.left_width + street.right_width) * 0.5F * unit;
        const BoundingBox road{
            {std::min(a.x, b.x) * unit - half, std::min(a.y, b.y) * unit - unit, std::min(a.z, b.z) * unit - half},
            {std::max(a.x, b.x) * unit + half, std::max(a.y, b.y) * unit + unit, std::max(a.z, b.z) * unit + half}};
        if (CheckCollisionBoxes(box, road)) {
            return "Overlaps road " + street.name;
        }
    }
    return {};
}

// Places a NEW copy of the chosen building template at the cursor.
void place_city_building_from_template(EditorProject& project, EditorUi& ui, CityGridPoint point) {
    if (!project.city || project.buildings.empty()) {
        return;
    }
    auto& buildings = project.city->data.buildings;
    auto placement = city_building_placement_at(project, ui, point);
    placement.index = buildings.size();
    buildings.push_back(std::move(placement));
    ui.selected_city_building = buildings.size() - 1U;
    ui.city_selection = CitySelectionKind::Building;
    ui.selected_city_buildings = {ui.selected_city_building};
    ui.city_dirty = true;
}


void nudge_city_street_width(EditorProject& project, EditorUi& ui, int side, int delta) {
    if (!project.city || project.city->data.streets.empty()) {
        return;
    }

    ui.selected_city_street = std::min(ui.selected_city_street, project.city->data.streets.size() - 1U);
    auto& street = project.city->data.streets[ui.selected_city_street];
    auto& value = side == 0 ? street.left_width : street.right_width;
    value = std::max(0, value + delta);
    ui.city_dirty = true;
}

void duplicate_city_street(EditorProject& project, EditorUi& ui) {
    if (!project.city || project.city->data.streets.empty()) {
        return;
    }

    auto& streets = project.city->data.streets;
    ui.selected_city_street = std::min(ui.selected_city_street, streets.size() - 1U);
    auto street = streets[ui.selected_city_street];
    street.index = streets.size();
    streets.push_back(std::move(street));
    ui.selected_city_street = streets.size() - 1U;
    ui.city_dirty = true;
}

// The closest existing intersection to a grid point within snap_cells, or nullopt.
// Lets a road click reuse a junction that is already there instead of stacking a
// new node on it (as editor.exe does).
std::optional<std::size_t> nearest_city_intersection(const SbcMap& map, CityGridPoint point, std::int32_t snap_cells) {
    std::optional<std::size_t> best;
    std::int64_t best_dist_sq = static_cast<std::int64_t>(snap_cells) * snap_cells;
    for (std::size_t i = 0; i < map.intersections.size(); ++i) {
        const auto& it = map.intersections[i];
        const std::int64_t dx = it.x - point.x;
        const std::int64_t dz = it.z - point.z;
        const std::int64_t dy = static_cast<std::int64_t>(it.y) - point.y;
        const auto dist_sq = dx * dx + dy * dy + dz * dz;
        if (dist_sq <= best_dist_sq) {
            best_dist_sq = dist_sq;
            best = i;
        }
    }
    return best;
}

// Keeps an end point in line with a start node: it locks whichever of X and Z moved
// less, so the segment runs straight along the main direction. Roads are never
// diagonal.
CityGridPoint snap_to_axis(CityGridPoint start, CityGridPoint target) {
    const auto dx = std::abs(target.x - start.x);
    const auto dz = std::abs(target.z - start.z);
    if (dx >= dz) {
        return CityGridPoint{target.x, target.y, start.z};
    }
    return CityGridPoint{start.x, target.y, target.z};
}

// A road a grid point lands on, away from its ends, ready to be split: the street
// index and the grid point moved onto the road (roads run along an axis, so the
// new node sits exactly on the road's line).
struct StreetSplitHit {
    std::size_t street_index{};
    CityGridPoint point{};
};

// Finds a road whose footprint the grid point lands on, away from its ends, so a
// click there splits it into a T or 4-way junction instead of adding a loose node.
// nearest_city_intersection handles the ends.
std::optional<StreetSplitHit> nearest_street_split(const SbcMap& map, CityGridPoint point, bool auto_generated) {
    std::optional<StreetSplitHit> best;
    double best_dist = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < map.streets.size(); ++i) {
        const auto& street = map.streets[i];
        if (street.from_intersection < 0 || street.to_intersection < 0 ||
            static_cast<std::size_t>(street.from_intersection) >= map.intersections.size() ||
            static_cast<std::size_t>(street.to_intersection) >= map.intersections.size()) {
            continue;
        }
        const auto& a = map.intersections[static_cast<std::size_t>(street.from_intersection)];
        const auto& b = map.intersections[static_cast<std::size_t>(street.to_intersection)];
        const double dx = b.x - a.x;
        const double dz = b.z - a.z;
        const double seg_len_sq = dx * dx + dz * dz;
        if (seg_len_sq < 1.0) {
            continue;
        }
        double u = ((point.x - a.x) * dx + (point.z - a.z) * dz) / seg_len_sq;
        // Stay away from the ends; those snap to the existing nodes.
        const double end_margin = 0.75 / std::sqrt(seg_len_sq);
        if (u <= end_margin || u >= 1.0 - end_margin) {
            continue;
        }
        const double cx = a.x + dx * u;
        const double cz = a.z + dz * u;
        const double dist = std::sqrt((point.x - cx) * (point.x - cx) + (point.z - cz) * (point.z - cz));
        // Half the drawn road width, in cells, plus one spare cell.
        const double half_width = std::max(street.left_width, street.right_width) * 0.5 + 1.0;
        if (dist > half_width || dist >= best_dist) {
            continue;
        }
        auto road_y = static_cast<std::int32_t>(std::lround(a.y + (b.y - a.y) * u));
        // The game puts a street's slope in its middle with flat ends, not as a straight
        // ramp from end to end. So split at the height of the lane the game lays there
        // (lanes sit at level y - 1).
        if (a.y != b.y) {
            const auto gx = static_cast<std::int32_t>(std::lround(cx)), gz = static_cast<std::int32_t>(std::lround(cz));
            for (const auto& tile : preview_city_street(map, i, auto_generated).cells) {
                if (tile.kind == CityTileKind::Road && tile.street_index == static_cast<std::int32_t>(i) && tile.x == gx && tile.z == gz) {
                    road_y = tile.y + 1;
                    break;
                }
            }
        }
        if (std::abs(road_y - point.y) > 1) continue;
        // Snap the split node onto the grid cell closest to that point, so both halves stay
        // in line with the original road.
        best = StreetSplitHit{
            i,
            CityGridPoint{
                static_cast<std::int32_t>(std::lround(cx)),
                road_y,
                static_cast<std::int32_t>(std::lround(cz)),
            },
        };
        best_dist = dist;
    }
    return best;
}

// Splits street[street_index] at a new node at split_point. The original segment
// becomes from->new, and a new segment new->to is added. Both keep the original
// road's width, district, and name. Returns the new node's index.
std::size_t split_street_at(SbcMap& map, std::size_t street_index, CityGridPoint split_point) {
    const auto existing = nearest_city_intersection(map, split_point, 0);
    const auto node_index = existing ? *existing : add_city_intersection(map, split_point);

    auto original = map.streets[street_index];
    const auto to_index = original.to_intersection;
    // Reuse the original for from->new, and add new->to with the same settings.
    map.streets[street_index].to_intersection = static_cast<std::int32_t>(node_index);

    SbcStreet tail = original;
    tail.index = map.streets.size();
    tail.from_intersection = static_cast<std::int32_t>(node_index);
    tail.to_intersection = to_index;
    tail = canonical_city_street_endpoint_order(map, std::move(tail));
    map.streets.push_back(std::move(tail));

    map.streets[street_index] = canonical_city_street_endpoint_order(map, std::move(map.streets[street_index]));
    reindex_city_streets(map);
    return node_index;
}

// Turns a click into an intersection index: snaps to a nearby node if there is one,
// or else splits a road the click lands on (making a junction), or else makes a new
// node at the height of the clicked surface.
std::size_t resolve_or_create_intersection(SbcMap& map, CityGridPoint point, bool auto_generated) {
    if (const auto existing = nearest_city_intersection(map, point, 1)) {
        return *existing;
    }
    if (const auto split = nearest_street_split(map, point, auto_generated)) {
        return split_street_at(map, split->street_index, split->point);
    }
    return add_city_intersection(map, point);
}

void append_city_road_point(EditorProject& project, EditorUi& ui, CityGridPoint point) {
    if (!project.city) {
        return;
    }

    auto& map = project.city->data;
    const bool auto_generated = project_is_auto_generated(project);

    // The first click only remembers the start, snapped to a nearby node or road so the
    // preview matches. Nothing is made until the second click lays a segment.
    if (!ui.pending_road_start.has_value()) {
        if (const auto node = nearest_city_intersection(map, point, 1)) {
            const auto& existing = map.intersections[*node];
            point = {existing.x, existing.y, existing.z};
        } else if (const auto split = nearest_street_split(map, point, auto_generated)) {
            point = split->point;
        }
        ui.pending_road_start = point;
        return;
    }

    const auto streets = map.streets.size(), intersections = map.intersections.size();
    const auto laid = lay_city_road(map, *ui.pending_road_start, point, ui.new_road_width, auto_generated);
    ui.pending_road_start = laid.next_start;
    if (laid.last_street) ui.selected_city_street = *laid.last_street;
    if (map.streets.size() != streets || map.intersections.size() != intersections) ui.city_dirty = true;
}

// Lays the road the Road tool would lay from start_point to a click at point. It
// snaps to junctions, splits roads it starts on, ends on, or crosses, and adds the
// segments. The click and its preview share this so they always agree.
RoadLay lay_city_road(SbcMap& map, CityGridPoint start_point, CityGridPoint point, std::int32_t width, bool auto_generated) {
    RoadLay laid;
    // Clicking the start point again ends the chain without making a zero-length road.
    if (start_point.x == point.x && start_point.z == point.z) return {};

    // Axis-lock the end relative to the start, then resolve/create that node.
    const auto end_point = snap_to_axis(start_point, point);
    // An end one cell from the start would snap back onto the start node; stop
    // before creating that node so the click leaves nothing behind.
    if (std::abs(end_point.x - start_point.x) + std::abs(end_point.z - start_point.z) <= 1) return {start_point, std::nullopt};
    const auto start_index = resolve_or_create_intersection(map, start_point, auto_generated);
    const auto end_index = resolve_or_create_intersection(map, end_point, auto_generated);
    const auto node_point = [&](std::size_t index) {
        const auto& node = map.intersections[index];
        return CityGridPoint{node.x, node.y, node.z};
    };
    if (end_index == start_index) return {start_point, std::nullopt};
    for (const auto& existing : map.streets) {
        if ((existing.from_intersection == static_cast<int>(start_index) && existing.to_intersection == static_cast<int>(end_index)) ||
            (existing.to_intersection == static_cast<int>(start_index) && existing.from_intersection == static_cast<int>(end_index))) {
            return {node_point(end_index), std::nullopt};
        }
    }
    // New roads use the width set in Tools, and carry on the district of a road that
    // already meets the start node.
    SbcStreet street{};
    street.name = "Street";
    street.left_width = width / 2;
    street.right_width = width - street.left_width;
    for (const auto& existing : map.streets) {
        if (existing.from_intersection == static_cast<int>(start_index) || existing.to_intersection == static_cast<int>(start_index)) {
            street.district = existing.district;
            break;
        }
    }
    // Split the roads it crosses where the heights match, then join every junction in
    // order. Where roads cross at different heights, they stay as bridges.
    const auto finish = map.intersections[end_index];
    const bool along_x = start_point.z == finish.z;
    const int from = along_x ? start_point.x : start_point.z;
    const int to = along_x ? finish.x : finish.z;
    if (from == to) return {node_point(end_index), std::nullopt};
    const auto old_streets = map.streets;
    for (std::size_t i = 0; i < old_streets.size(); ++i) {
        const auto& existing = old_streets[i];
        if (existing.from_intersection < 0 || existing.to_intersection < 0 ||
            static_cast<std::size_t>(existing.from_intersection) >= map.intersections.size() ||
            static_cast<std::size_t>(existing.to_intersection) >= map.intersections.size()) continue;
        const auto a = map.intersections[existing.from_intersection];
        const auto b = map.intersections[existing.to_intersection];
        if (along_x ? (a.x != b.x || a.z == b.z) : (a.z != b.z || a.x == b.x)) continue;
        const int crossing = along_x ? a.x : a.z;
        const int fixed = along_x ? start_point.z : start_point.x;
        const int ca = along_x ? a.z : a.x, cb = along_x ? b.z : b.x;
        if (crossing <= std::min(from, to) || crossing >= std::max(from, to) ||
            fixed <= std::min(ca, cb) || fixed >= std::max(ca, cb)) continue;
        const float y = start_point.y + (finish.y - start_point.y) * (static_cast<float>(crossing - from) / (to - from));
        const float other_y = a.y + (b.y - a.y) * (static_cast<float>(fixed - ca) / (cb - ca));
        if (std::abs(y - other_y) > .25F) continue;
        split_street_at(map, i, {along_x ? crossing : fixed, static_cast<int>(std::lround(y)), along_x ? fixed : crossing});
    }
    std::vector<std::size_t> junctions{start_index, end_index};
    for (std::size_t i = 0; i < map.intersections.size(); ++i) {
        const auto& node = map.intersections[i];
        const auto coordinate = along_x ? node.x : node.z;
        if ((along_x ? node.z != start_point.z : node.x != start_point.x) ||
            coordinate <= std::min(from, to) || coordinate >= std::max(from, to)) continue;
        const float y = start_point.y + (finish.y - start_point.y) * (static_cast<float>(coordinate - from) / (to - from));
        if (std::abs(y - node.y) <= .25F) junctions.push_back(i);
    }
    std::sort(junctions.begin(), junctions.end(), [&](auto a, auto b) {
        const auto av = along_x ? map.intersections[a].x : map.intersections[a].z;
        const auto bv = along_x ? map.intersections[b].x : map.intersections[b].z;
        return from < to ? av < bv : av > bv;
    });
    for (std::size_t i = 1; i < junctions.size(); ++i) {
        const auto a = static_cast<int>(junctions[i - 1]), b = static_cast<int>(junctions[i]);
        if (a == b || std::any_of(map.streets.begin(), map.streets.end(), [&](const auto& road) {
            return (road.from_intersection == a && road.to_intersection == b) || (road.from_intersection == b && road.to_intersection == a);
        })) continue;
        auto segment = street;
        segment.index = map.streets.size(); segment.from_intersection = a; segment.to_intersection = b;
        map.streets.push_back(canonical_city_street_endpoint_order(map, std::move(segment)));
        laid.last_street = map.streets.size() - 1;
    }
    // The new end node becomes the next start, so roads can be laid in a chain.
    laid.next_start = node_point(end_index);
    return laid;
}

// Ends the current chain of roads (Esc, or a change of mode or selection).
void end_road_chain(EditorUi& ui) {
    ui.pending_road_start.reset();
}

std::int32_t floor_div8(std::int32_t value) {
    return value >= 0 ? value / 8 : -((7 - value) / 8);
}

std::size_t local_portal_index(CityGridPoint point) {
    const auto local_x = static_cast<std::uint32_t>(point.x) & 7U;
    const auto local_y = static_cast<std::uint32_t>(point.y) & 7U;
    const auto local_z = static_cast<std::uint32_t>(point.z) & 7U;
    return static_cast<std::size_t>(local_x + local_y * 0x40U + local_z * 8U);
}

SbcPortal& find_or_create_portal(SbcMap& map, CityGridPoint point) {
    const auto fields = std::array<std::int32_t, 4>{0, floor_div8(point.x), floor_div8(point.y), floor_div8(point.z)};
    const auto match = std::ranges::find_if(map.portals, [fields](const SbcPortal& portal) {
        return portal.fields == fields;
    });
    if (match != map.portals.end()) {
        return *match;
    }

    SbcPortal portal{};
    portal.index = map.portals.size();
    portal.fields = fields;
    portal.blocks.assign(0x200, 0);
    if (map.version < 0x0b) {
        portal.textures.assign(0x1000, 0);
    }
    if (map.version > 9) {
        portal.itemsets.assign(0x200, 0);
    }
    map.portals.push_back(std::move(portal));
    return map.portals.back();
}

std::uint32_t encode_city_piece_word(const EditorUi& ui) {
    return static_cast<std::uint32_t>(ui.selected_city_block & 0xfffU) |
        (static_cast<std::uint32_t>((ui.selected_city_block_layer + 1U) & 0x3fU) << 16U) |
        (static_cast<std::uint32_t>(ui.selected_piece_rotation & 3U) << 24U);
}

// The city block with this mesh name (empty means the game's solid "nblock" cube)
// and one layer of these textures. Added if no record matches.
std::optional<std::size_t> find_or_add_city_block(SbcMap& map, const std::string& name, const std::array<std::string, 8>& layer) {
    const auto block = std::ranges::find_if(map.city_blocks, [&](const SbcCityBlock& candidate) {
        return candidate.name == name && candidate.layers.size() == 1 && candidate.layers[0] == layer;
    });
    if (block != map.city_blocks.end()) return static_cast<std::size_t>(block - map.city_blocks.begin());
    if (map.city_blocks.size() >= 4096) return std::nullopt;
    map.city_blocks.push_back({map.city_blocks.size(), name, {layer}});
    return map.city_blocks.size() - 1U;
}

void set_city_piece_voxel(EditorProject& project, EditorUi& ui, CityGridPoint point) {
    if (!project.city || project.city->data.version < 11 || (project.blocks.empty() && !ui.place_piece_solid)) return;
    auto& map = project.city->data;
    std::string name;
    if (!ui.place_piece_solid) {
        ui.place_piece_mesh = std::min(ui.place_piece_mesh, project.blocks.size() - 1);
        name = project.blocks[ui.place_piece_mesh].name;
    }
    std::array<std::string, 8> layer{};
    if (!project.textures.empty()) layer.fill(project.textures[std::min(ui.place_piece_texture, project.textures.size() - 1)].name);
    const auto block = find_or_add_city_block(map, name, layer);
    if (!block) return;
    ui.selected_city_block = *block;
    ui.selected_city_block_layer = 0;
    auto& portal = find_or_create_portal(project.city->data, point);
    if (portal.blocks.size() < 0x200) {
        portal.blocks.resize(0x200);
    }
    portal.blocks[local_portal_index(point)] = encode_city_piece_word(ui);
    invalidate_portal_render_models(project);
    ui.city_dirty = true;
}

// Repaints the selected piece: points its voxel at a city block with the same mesh
// and these textures, and keeps the rotation.
void repaint_city_piece(EditorProject& project, EditorUi& ui, const std::array<std::string, 8>& textures, std::string& status) {
    if (!project.city || !ui.selected_piece_voxel || project.city->data.version < 11) return;
    auto& map = project.city->data;
    const auto current = static_cast<std::size_t>(ui.selected_piece_word & 0xfffU);
    if (current >= map.city_blocks.size()) return;
    const auto block = find_or_add_city_block(map, std::string{map.city_blocks[current].name}, textures);
    if (!block) {
        status = "the map already has 4096 city blocks";
        return;
    }
    const auto word = (ui.selected_piece_word & 0x03000000U) | (1U << 16U) | static_cast<std::uint32_t>(*block & 0xfffU);
    auto& portal = find_or_create_portal(map, *ui.selected_piece_voxel);
    if (portal.blocks.size() < 0x200) portal.blocks.resize(0x200);
    portal.blocks[local_portal_index(*ui.selected_piece_voxel)] = word;
    ui.selected_piece_word = word;
    invalidate_portal_render_models(project);
    ui.city_dirty = true;
}

// Moves the selected piece voxel one step and keeps its word (piece, layer,
// rotation). Will not overwrite a cell that is already filled.
void move_city_piece(EditorProject& project, EditorUi& ui, int dx, int dy, int dz, std::string& status) {
    if (!project.city || !ui.selected_piece_voxel || project.city->data.version < 11) return;
    auto& map = project.city->data;
    const auto from = *ui.selected_piece_voxel;
    const CityGridPoint to{from.x + dx, from.y + dy, from.z + dz};
    const auto portal_of = [&map](CityGridPoint point) -> SbcPortal* {
        const std::array<std::int32_t, 4> fields{0, floor_div8(point.x), floor_div8(point.y), floor_div8(point.z)};
        const auto match = std::ranges::find_if(map.portals, [&](const SbcPortal& portal) { return portal.fields == fields; });
        return match == map.portals.end() ? nullptr : &*match;
    };
    const auto* source = portal_of(from);
    if (source == nullptr || source->blocks.size() < 0x200) return;
    const auto word = source->blocks[local_portal_index(from)];
    if (word == 0U) return;
    auto& target = find_or_create_portal(map, to);  // can move the portals list in memory
    if (target.blocks.size() < 0x200) target.blocks.resize(0x200);
    if (target.blocks[local_portal_index(to)] != 0U) {
        status = "that cell already has a piece";
        return;
    }
    target.blocks[local_portal_index(to)] = word;
    portal_of(from)->blocks[local_portal_index(from)] = 0U;
    ui.selected_piece_voxel = to;
    invalidate_portal_render_models(project);
    ui.city_dirty = true;
}

void select_city_piece(EditorProject& project, EditorUi& ui, const CityPiecePick& pick, std::string& status) {
    if (!project.city || pick.portal_index >= project.city->data.portals.size()) {
        return;
    }

    ui.selected_portal = pick.portal_index;
    ui.selected_piece_word = pick.word;
    if (!project.city->data.city_blocks.empty()) {
        ui.selected_city_block = std::min<std::size_t>(pick.word & 0xfffU, project.city->data.city_blocks.size() - 1U);
        const auto& city_block = project.city->data.city_blocks[ui.selected_city_block];
        auto layer = static_cast<std::size_t>((pick.word >> 16U) & 0x3fU);
        layer = layer == 0U ? 0U : layer - 1U;
        ui.selected_city_block_layer = city_block.layers.empty()
            ? 0U
            : std::min(layer, city_block.layers.size() - 1U);
        ui.selected_piece_rotation = static_cast<std::size_t>((pick.word >> 24U) & 3U);
    }

    // Find the picked voxel's world grid cell, so the selection highlights one block
    // and not the whole 8x8x8 portal.
    const auto& portal = project.city->data.portals[pick.portal_index];
    const auto local_x = static_cast<std::int32_t>(pick.block_index & 7U);
    const auto local_z = static_cast<std::int32_t>((pick.block_index / 8U) & 7U);
    const auto local_y = static_cast<std::int32_t>((pick.block_index / 0x40U) & 7U);
    ui.selected_piece_voxel = CityGridPoint{
        sbc_portal_cell_base(portal.fields[1]) + local_x,
        sbc_portal_cell_base(portal.fields[2]) + local_y,
        sbc_portal_cell_base(portal.fields[3]) + local_z,
    };

    status = "selected piece block [" +
        std::to_string(ui.selected_piece_voxel->x) + "," +
        std::to_string(ui.selected_piece_voxel->y) + "," +
        std::to_string(ui.selected_piece_voxel->z) + "]";
}

void delete_city_street(EditorProject& project, EditorUi& ui) {
    if (!project.city || project.city->data.streets.size() <= 1U) {
        return;
    }

    auto& map = project.city->data;
    ui.selected_city_street = std::min(ui.selected_city_street, map.streets.size() - 1U);
    map.streets.erase(map.streets.begin() + static_cast<std::ptrdiff_t>(ui.selected_city_street));
    reindex_city_streets(map);
    remove_unused_city_intersections(map);
    ui.selected_city_intersection = std::min(ui.selected_city_intersection, map.intersections.size() - 1U);
    ui.selected_city_street = std::min(ui.selected_city_street, map.streets.size() - 1U);
    ui.city_dirty = true;
}

void nudge_city_block_slot(EditorProject& project, EditorUi& ui, int delta) {
    if (!project.city || project.city->data.city_blocks.empty() || project.blocks.empty()) {
        return;
    }

    ui.selected_city_block = std::min(ui.selected_city_block, project.city->data.city_blocks.size() - 1U);
    auto& city_block = project.city->data.city_blocks[ui.selected_city_block];
    if (city_block.layers.empty()) {
        return;
    }
    ui.selected_city_block_layer = std::min(ui.selected_city_block_layer, city_block.layers.size() - 1U);
    ui.selected_city_block_slot = std::min<std::size_t>(ui.selected_city_block_slot, 7U);

    const auto names = block_names(project);
    auto& slot = city_block.layers[ui.selected_city_block_layer][ui.selected_city_block_slot];
    auto current = std::ranges::find(names, slot);
    auto index = current == names.end() ? 0 : static_cast<std::size_t>(std::distance(names.begin(), current));
    if (delta < 0) {
        index = index == 0 ? names.size() - 1U : index - 1U;
    } else {
        index = (index + 1U) % names.size();
    }
    slot = names[index];
    invalidate_city_block_preview_scene(project);
    invalidate_portal_render_models(project);
    ui.city_dirty = true;
}

void recalculate_building_size(SbbBuilding& building) {
    building.version = 14;
    // The game's grid rule, true for every building it ships: X and Z hold the highest
    // cell index (the grid runs from 0 to size, both included) and Y holds the number
    // of floors. So burger's 12x12x2 grid has 13*13*2 = 338 cells. Measure every grid
    // cell, not just the filled ones, so empty columns and upper floors keep the size.
    building.size_x = building.size_y = 0;
    building.size_z = 1;
    for (std::size_t i = 0; i < building.cells.size(); ++i) {
        auto& cell = building.cells[i]; cell.index = i;
        building.size_x = std::max(building.size_x, cell.x);
        building.size_y = std::max(building.size_y, cell.z);
        building.size_z = std::max(building.size_z, cell.y + 1);
    }
    building.extent_x = std::max(1U, building.size_x - std::min(building.size_x, building.anchor_x));
    building.extent_y = std::max(1U, building.size_z - std::min(building.size_z, building.anchor_y));
    building.extent_z = std::max(1U, building.size_y - std::min(building.size_y, building.anchor_z));
    // Changed geometry makes the saved LODs out of date.
    building.lod_tail.clear();
    building.lod_marker = 1;
    building.lod_group_count = 0;
}

// A buildblock edit only changes the meshes of buildings that use it, so rebuild
// those instead of marking every building in the project as changed.
void rebuild_buildings_using_buildblock(EditorProject& project, const std::string& buildblock_name) {
    invalidate_render_input_cache(project);
    for (std::size_t i = 0; i < project.buildings.size(); ++i) {
        const auto& names = project.buildings[i].data.block_names;
        if (std::find(names.begin(), names.end(), buildblock_name) == names.end()) continue;
        rebuild_single_building_render(project, i);
        invalidate_building_thumbnail(project, i);
    }
}


void mark_building_changed(EditorProject& project, EditorUi& ui) {
    recalculate_building_size(project.buildings[ui.selected_building].data);
    refresh_changed_building(project, ui);
}

void mark_buildblock_changed(EditorProject& project, EditorUi& ui) {
    project.buildblocks[ui.selected_buildblock].dirty = true;
    auto& model = project.buildblocks[ui.selected_buildblock].data;
    for (std::size_t i = 0; i < model.layers.size(); ++i) {
        auto& layer = model.layers[i]; layer.index = i;
        layer.door_count = static_cast<std::uint32_t>(layer.doors.size());
        layer.window_count = static_cast<std::uint32_t>(layer.windows.size());
        layer.beam_count = static_cast<std::uint32_t>(layer.beams.size());
    }
    ui.buildblock_dirty = true;
    project.building_floor_preview_dirty = true;
    invalidate_buildblock_preview_scene(project);
    invalidate_portal_render_models(project);
    rebuild_buildings_using_buildblock(project, project.buildblocks[ui.selected_buildblock].name);
}

void mark_block_changed(EditorProject& project, EditorUi& ui, SblBlockMesh& block) {
    ui.block_dirty = true;
    refresh_sbl_block_faces(block);
    // The archive is written from block_bytes, so keep them matching the edited block.
    for (std::size_t i = 0; i < project.blocks.size(); ++i) {
        if (&project.blocks[i].data != &block) continue;
        project.blocks[i].dirty = true;
        if (i < project.block_bytes.size()) project.block_bytes[i] = write_sbl_block(block);
        if (i < project.block_thumbnail_ready.size()) project.block_thumbnail_ready[i] = false;
    }
    project.render_buffers_dirty = true;
    // Editing block data makes the cached block mesh catalog out of date.
    invalidate_render_input_cache(project);
    invalidate_city_block_preview_scene(project);
    invalidate_portal_render_models(project);
}

// Saves the map screen (the map image and street names) as a PNG.
void export_map_image(EditorProject& project, const EditorUi& ui, std::string& status) {
    const auto png = map_menu_png(project, ui.map_export_size, ui.map_menu);
    if (png.empty()) {
        report_failure(status, "Map image export failed: the city is still being built");
        return;
    }
    const auto name = project.root.filename().string() + "_map.png";
#ifdef __EMSCRIPTEN__
    web_download(name, png);
    status = "downloaded " + name;
#else
    try {
        const auto output = dialog_save_file(project.root / name, "PNG image", "png");
        if (!output) return;
        write_file_bytes(*output, png);
        status = "exported " + output->filename().string();
    } catch (const std::exception& error) {
        report_failure(status, std::string{"Map image export failed: "} + error.what());
    }
#endif
}

void save_map_to(EditorProject& project, EditorUi& ui, const std::filesystem::path& folder, std::string& status);

std::string city_upgrade_notice(std::uint32_t from_version) {
    return "This map was upgraded from format v" + std::to_string(from_version) +
        " to v12. The next save writes v12.";
}

void draw_city_upgrade_dialogs(EditorProject& project, EditorUi& ui, std::string& status) {
    if (ui.confirm_piece_upgrade && !ImGui::IsPopupOpen("Upgrade map format?")) ImGui::OpenPopup("Upgrade map format?");
    ImGui::SetNextWindowSizeConstraints(ImVec2(460.0F, 0.0F), ImVec2(720.0F, FLT_MAX));
    if (ImGui::BeginPopupModal("Upgrade map format?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const auto from = project.city ? project.city->data.version : 12U;
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 440.0F);
        ImGui::Text("This map uses format v%u. Placing pieces needs v12.", from);
        ImGui::Spacing();
        ImGui::TextUnformatted("Upgrading converts its legacy district cells into pieces. The game draws v12 "
            "pieces differently: their textures resolve by name and they draw after buildings, so those "
            "areas can look different in game. Saving without upgrading keeps the map as it is.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button("Upgrade to v12")) {
            try {
                promote_city(project);
                ui.city_dirty = true;
                project.city_promotion_failed = false;
                status = "map format upgraded from v" + std::to_string(from) + " to v12";
            } catch (const std::exception& error) {
                project.city_promotion_failed = true;
                report_failure(status, std::string{"Map upgrade failed: "} + error.what());
            }
            ui.confirm_piece_upgrade = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ui.city_tool = CityTool::Select;
            ui.confirm_piece_upgrade = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!ui.city_upgrade_notice.empty() && !ImGui::IsPopupOpen("Map format upgraded")) ImGui::OpenPopup("Map format upgraded");
    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0F, 0.0F), ImVec2(720.0F, FLT_MAX));
    if (ImGui::BeginPopupModal("Map format upgraded", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 400.0F);
        ImGui::TextUnformatted(ui.city_upgrade_notice.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::Button("OK")) { ui.city_upgrade_notice.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

// -----------------------------------------------------------------------------
// Building drag gizmo
// -----------------------------------------------------------------------------

// Where the gizmo handles for a selected city building sit, in world space. Three
// move handles sit at the ends of short axis arms, and the rotate handle is a ring
// marker off to the side. Sizes follow the grid so the gizmo stays easy to read.
struct GizmoLayout {
    Vector3 origin{};
    Vector3 tip_x{};
    Vector3 tip_y{};
    Vector3 tip_z{};
    Vector3 tip_rot{};
    // XZ plane handle: a square between the X and Z arms (like ImGuizmo).
    std::array<Vector3, 4> plane_xz{};
    float handle_radius{};
    float arm_length{};
};

GizmoLayout building_gizmo_layout(const SbcBuilding& placement, const Camera3D& camera, Vector2 viewport_size, Vector3 offset = {}) {
    constexpr auto unit_size = 4.0F;
    auto o = native_city_building_render_origin(placement, unit_size);
    o.x += offset.x; o.y += offset.y; o.z += offset.z;
    const Vector3 point{o.x, o.y, o.z};
    const auto distance = Vector3Distance(camera.position, point);
    (void)distance;
    const auto arm = std::max(1.0F, view_half_height(camera, point) * 180.0F / std::max(1.0F, viewport_size.y));
    const Vector3 origin{o.x, o.y + unit_size * 0.5F, o.z};
    const float p0 = arm * 0.3F;
    const float p1 = arm * 0.6F;
    return GizmoLayout{
        origin,
        {origin.x + arm, origin.y, origin.z},
        {origin.x, origin.y + arm, origin.z},
        {origin.x, origin.y, origin.z + arm},
        {origin.x + arm * 0.7F, origin.y, origin.z + arm * 0.7F},
        {Vector3{origin.x + p0, origin.y, origin.z + p0}, Vector3{origin.x + p1, origin.y, origin.z + p0},
         Vector3{origin.x + p1, origin.y, origin.z + p1}, Vector3{origin.x + p0, origin.y, origin.z + p1}},
        arm * .065F,
        arm,
    };
}

// What a click would select in the city view. A building or piece the ray passes
// through is in front of the road or ground below it, so those win over the
// ground. Among ground hits, the road and node picker already picks the closest.
struct CityAnyPick {
    CitySelectionKind kind{CitySelectionKind::None};
    std::size_t index{};
};

CityAnyPick pick_city_any(EditorProject& project, const MapScene& scene, const Vec3& ro, const Vec3& rd) {
    if (!project.city) {
        return {};
    }
    const auto& map = project.city->data;
    // Pick the drawn triangles, so an empty cell or an open doorway does not select the
    // building behind the cursor.
    const auto& asset_indices = resolve_placement_asset_indices(project);
    const Ray ray{{ro.x, ro.y, ro.z}, {rd.x, rd.y, rd.z}};
    // Foundation floors are underground. If the ray reaches the ground (or a road on
    // it) first, it must not pick the building buried behind it.
    float ground_distance = std::numeric_limits<float>::max();
    if (scene.terrain.has_value()) {
        if (const auto ground = ray_terrain_hit(*scene.terrain, ro, rd)) {
            ground_distance = Vector3Distance(ray.position, {ground->x, ground->y, ground->z});
        }
    } else if (std::abs(rd.y) > 0.0001F && -ro.y / rd.y > 0.0F) {
        ground_distance = Vector3Distance(ray.position, Vector3Add(ray.position, Vector3Scale(ray.direction, -ro.y / rd.y)));
    }
    float nearest_building = std::numeric_limits<float>::max();
    std::optional<std::size_t> building;
    for (std::size_t i = 0; i < map.buildings.size(); ++i) {
        if (i >= asset_indices.size() || !asset_indices[i] ||
            *asset_indices[i] >= project.building_render_buffers.size()) continue;
        const auto& buffer = project.building_render_buffers[*asset_indices[i]];
        const auto& placement = map.buildings[i];
        const auto& asset = project.buildings[*asset_indices[i]].data;
        BoundingBox bounds{{FLT_MAX, FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX, -FLT_MAX}};
        for (int corner = 0; corner < 8; ++corner) {
            const Vec3 local{
                ((corner & 1 ? static_cast<float>(asset.size_x + 1) : 0.0F) - asset.anchor_x) * scene.unit_size,
                ((corner & 2 ? static_cast<float>(asset.size_z) : 0.0F) - asset.anchor_y) * scene.unit_size,
                ((corner & 4 ? static_cast<float>(asset.size_y + 1) : 0.0F) - asset.anchor_z) * scene.unit_size};
            const auto p = native_city_building_transform_point(placement, local, scene.unit_size);
            bounds.min = Vector3Min(bounds.min, {p.x, p.y, p.z});
            bounds.max = Vector3Max(bounds.max, {p.x, p.y, p.z});
        }
        if (!GetRayCollisionBox(ray, bounds).hit) continue;
        for (const auto& triangle : buffer.triangles) {
            const auto point = [&](std::size_t index) {
                const auto p = native_city_building_transform_point(placement, buffer.vertices[index].position, scene.unit_size);
                return Vector3{p.x, p.y, p.z};
            };
            const auto hit = GetRayCollisionTriangle(ray, point(triangle.indices[0]),
                point(triangle.indices[1]), point(triangle.indices[2]));
            if (hit.hit && hit.distance >= 0.0F && hit.distance < nearest_building && hit.distance <= ground_distance + 1.0F) {
                nearest_building = hit.distance;
                building = i;
            }
        }
    }
    // Piece voxels. Whichever the ray reaches first, the closest building or the
    // closest piece, wins, so you can pick a piece that is in front of a building.
    const auto piece = pick_city_piece(map, ro, rd, scene.unit_size);
    float piece_distance = std::numeric_limits<float>::max();
    if (piece.has_value() && piece->portal_index < map.portals.size()) {
        const auto& portal = map.portals[piece->portal_index];
        const Vector3 cell_min{
            static_cast<float>(sbc_portal_cell_base(portal.fields[1]) + static_cast<std::int32_t>(piece->block_index & 7U)) * scene.unit_size,
            static_cast<float>(sbc_portal_cell_base(portal.fields[2]) + static_cast<std::int32_t>((piece->block_index / 0x40U) & 7U)) * scene.unit_size,
            static_cast<float>(sbc_portal_cell_base(portal.fields[3]) + static_cast<std::int32_t>((piece->block_index / 8U) & 7U)) * scene.unit_size};
        const auto cell_hit = GetRayCollisionBox(ray, {cell_min, Vector3AddValue(cell_min, scene.unit_size)});
        if (cell_hit.hit) piece_distance = cell_hit.distance;
    }
    if (building && nearest_building <= piece_distance) {
        return {CitySelectionKind::Building, *building};
    }
    if (piece.has_value()) {
        return {CitySelectionKind::Piece, piece->portal_index};
    }
    // Ground: a road or a node (already the closer of the two).
    const auto road = pick_city_road_or_node(map, scene, ro, rd);
    if (road.has_value()) {
        return road->kind == CityRoadPick::Kind::Node
            ? CityAnyPick{CitySelectionKind::Node, road->index}
            : CityAnyPick{CitySelectionKind::Road, road->index};
    }
    return {};
}

// Adds an index to a selection set, or removes it if it is already there.
void toggle_in_selection(std::vector<std::size_t>& set, std::size_t index) {
    const auto it = std::find(set.begin(), set.end(), index);
    if (it == set.end()) {
        set.push_back(index);
    } else {
        set.erase(it);
    }
}

// Applies a pick from the Select tool. Buildings and piece portals can be multi-
// selected (additive = Shift or Ctrl): a plain click selects only that thing, and an
// additive click adds or removes it. Roads and nodes are selected one at a time.
// The single active index (selected_city_building / selected_portal) follows the
// last click.
void select_city_thing(
    EditorProject& project,
    EditorUi& ui,
    const CityAnyPick& picked,
    bool additive,
    const Vec3& ray_origin,
    const Vec3& ray_dir,
    std::string& status) {
    switch (picked.kind) {
        case CitySelectionKind::Building: {
            ui.city_selection = CitySelectionKind::Building;
            ui.city_library_roads = false;
            ui.selected_city_building = picked.index;
            if (additive) {
                toggle_in_selection(ui.selected_city_buildings, picked.index);
            } else {
                ui.selected_city_buildings = {picked.index};
            }
            ui.selected_portals.clear();
            break;
        }
        case CitySelectionKind::Piece: {
            if (const auto p = pick_city_piece(project.city->data, ray_origin, ray_dir, project.city_scene_cache.unit_size)) {
                select_city_piece(project, ui, *p, status);
                ui.city_selection = CitySelectionKind::Piece;
                if (additive) {
                    toggle_in_selection(ui.selected_portals, p->portal_index);
                } else {
                    ui.selected_portals = {p->portal_index};
                }
                ui.selected_city_buildings.clear();
            }
            break;
        }
        case CitySelectionKind::Road:
            ui.city_selection = CitySelectionKind::Road;
            ui.city_library_roads = true;
            ui.selected_city_street = picked.index;
            ui.selected_city_buildings.clear();
            ui.selected_portals.clear();
            break;
        case CitySelectionKind::Node:
            ui.city_selection = CitySelectionKind::Node;
            ui.city_library_roads = true;
            ui.selected_city_intersection = picked.index;
            ui.selected_city_buildings.clear();
            ui.selected_portals.clear();
            break;
        case CitySelectionKind::None:
            if (!additive) {
                ui.city_selection = CitySelectionKind::None;
                ui.selected_city_buildings.clear();
                ui.selected_portals.clear();
            }
            break;
    }
}

// Box select: every building and piece voxel whose screen point is inside the
// rectangle gets selected. With additive, the old selection stays; otherwise the
// box replaces it. Buildings and piece portals can't be selected together, so a box
// that catches both picks the buildings (the usual case).
void box_select_city(
    EditorProject& project,
    EditorUi& ui,
    const Camera3D& camera,
    Vector2 a,
    Vector2 b,
    Vector2 viewport_size,
    bool additive) {
    if (!project.city) {
        return;
    }
    const float min_x = std::min(a.x, b.x);
    const float max_x = std::max(a.x, b.x);
    const float min_y = std::min(a.y, b.y);
    const float max_y = std::max(a.y, b.y);
    const auto width = static_cast<int>(std::max(1.0F, viewport_size.x));
    const auto height = static_cast<int>(std::max(1.0F, viewport_size.y));
    const auto in_box = [&](Vector3 p) {
        const auto s = GetWorldToScreenEx(p, camera, width, height);
        return std::isfinite(s.x) && std::isfinite(s.y) &&
            s.x >= min_x && s.x <= max_x && s.y >= min_y && s.y <= max_y;
    };

    constexpr float unit = 4.0F;
    std::vector<std::size_t> buildings;
    for (std::size_t i = 0; i < project.city->data.buildings.size(); ++i) {
        const auto o = native_city_building_render_origin(project.city->data.buildings[i], unit);
        if (in_box({o.x, o.y + unit, o.z})) {
            buildings.push_back(i);
        }
    }
    if (!buildings.empty()) {
        if (!additive) {
            ui.selected_city_buildings.clear();
        }
        for (const auto idx : buildings) {
            if (std::find(ui.selected_city_buildings.begin(), ui.selected_city_buildings.end(), idx) == ui.selected_city_buildings.end()) {
                ui.selected_city_buildings.push_back(idx);
            }
        }
        ui.selected_city_building = ui.selected_city_buildings.back();
        ui.city_selection = CitySelectionKind::Building;
        ui.selected_portals.clear();
        return;
    }

    // No buildings caught: try piece voxels (portal blocks).
    std::vector<std::size_t> portals;
    for (std::size_t pi = 0; pi < project.city->data.portals.size(); ++pi) {
        const auto& portal = project.city->data.portals[pi];
        bool any = false;
        for (std::size_t bi = 0; bi < portal.blocks.size() && bi < 0x200U; ++bi) {
            if (portal.blocks[bi] == 0U) {
                continue;
            }
            const Vector3 c{
                (static_cast<float>(sbc_portal_cell_base(portal.fields[1]) + static_cast<std::int32_t>(bi & 7U)) + 0.5F) * unit,
                (static_cast<float>(sbc_portal_cell_base(portal.fields[2]) + static_cast<std::int32_t>((bi / 0x40U) & 7U)) + 0.5F) * unit,
                (static_cast<float>(sbc_portal_cell_base(portal.fields[3]) + static_cast<std::int32_t>((bi / 8U) & 7U)) + 0.5F) * unit,
            };
            if (in_box(c)) {
                any = true;
                break;
            }
        }
        if (any) {
            portals.push_back(pi);
        }
    }
    if (!portals.empty()) {
        if (!additive) {
            ui.selected_portals.clear();
        }
        for (const auto idx : portals) {
            if (std::find(ui.selected_portals.begin(), ui.selected_portals.end(), idx) == ui.selected_portals.end()) {
                ui.selected_portals.push_back(idx);
            }
        }
        ui.selected_portal = ui.selected_portals.back();
        ui.city_selection = CitySelectionKind::Piece;
        ui.selected_piece_voxel.reset();
        ui.selected_city_buildings.clear();
    } else if (!additive) {
        ui.city_selection = CitySelectionKind::None;
        ui.selected_city_buildings.clear();
        ui.selected_portals.clear();
    }
}

// Deletes the one thing the Erase hover highlights (pick_city_any): a building, a
// road, or a single piece voxel. Junctions are left alone; they go away with their
// roads.
void erase_city_thing_under_cursor(
    EditorProject& project,
    EditorUi& ui,
    const MapScene& scene,
    const Vec3& ro,
    const Vec3& rd,
    std::string& status) {
    if (!project.city) {
        return;
    }
    const auto picked = pick_city_any(project, scene, ro, rd);
    if (picked.kind == CitySelectionKind::Building) {
        ui.selected_city_building = picked.index;
        ui.selected_city_buildings = {picked.index};
        delete_city_building(project, ui);
        status = "deleted building";
    } else if (picked.kind == CitySelectionKind::Road) {
        ui.selected_city_street = picked.index;
        delete_city_street(project, ui);
        status = "deleted road";
    } else if (picked.kind == CitySelectionKind::Piece) {
        const auto piece = pick_city_piece(project.city->data, ro, rd, scene.unit_size);
        if (!piece.has_value()) return;
        auto& portal = project.city->data.portals[piece->portal_index];
        auto& voxels = portal.blocks.empty() ? portal.legacy_blocks : portal.blocks;
        if (piece->block_index >= voxels.size()) return;
        voxels[piece->block_index] = 0;
        invalidate_portal_render_models(project);
        ui.city_dirty = true;
        status = "deleted piece";
    }
}

// The gizmo handle under the cursor, measured in viewport pixels. Hit areas on
// the screen keep the gizmo easy to grab at every zoom and match the arrows you
// see, instead of only testing tiny 3D spheres at their tips.
GizmoAxis pick_building_gizmo(
    const Camera3D& camera,
    Vector2 viewport_mouse_pos,
    Vector2 viewport_size,
    const SbcBuilding& placement, bool allow_rotation = true, Vector3 offset = {}) {
    const auto layout = building_gizmo_layout(placement, camera, viewport_size, offset);
    const auto width = static_cast<int>(std::max(1.0F, viewport_size.x));
    const auto height = static_cast<int>(std::max(1.0F, viewport_size.y));
    // The forward direction, to ignore points behind the camera. GetWorldToScreenEx
    // still returns a mirrored point for those, which made the gizmo pick wrong handles
    // and seem to swallow the click.
    const Vector3 fwd{
        camera.target.x - camera.position.x,
        camera.target.y - camera.position.y,
        camera.target.z - camera.position.z,
    };
    auto in_front = [&](Vector3 p) {
        return (p.x - camera.position.x) * fwd.x + (p.y - camera.position.y) * fwd.y + (p.z - camera.position.z) * fwd.z > 0.0F;
    };
    auto project = [&](Vector3 point) {
        return GetWorldToScreenEx(point, camera, width, height);
    };
    auto finite = [&](Vector2 point) {
        return std::isfinite(point.x) && std::isfinite(point.y);
    };
    if (!in_front(layout.origin)) {
        return GizmoAxis::None;
    }
    auto distance = [](Vector2 a, Vector2 b) {
        const auto dx = a.x - b.x;
        const auto dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    };
    auto segment_distance = [&](Vector2 point, Vector2 a, Vector2 b) {
        const auto ab_x = b.x - a.x;
        const auto ab_y = b.y - a.y;
        const auto len_sq = ab_x * ab_x + ab_y * ab_y;
        if (len_sq <= std::numeric_limits<float>::epsilon()) {
            return distance(point, a);
        }
        auto t = ((point.x - a.x) * ab_x + (point.y - a.y) * ab_y) / len_sq;
        t = std::clamp(t, 0.0F, 1.0F);
        return distance(point, Vector2{a.x + ab_x * t, a.y + ab_y * t});
    };

    GizmoAxis best = GizmoAxis::None;
    float best_score = std::numeric_limits<float>::max();
    auto consider = [&](GizmoAxis axis, float score, float threshold) {
        if (score <= threshold && score < best_score) {
            best_score = score;
            best = axis;
        }
    };
    auto consider_arrow = [&](GizmoAxis axis, Vector3 tip) {
        constexpr auto shaft_threshold = 6.0F;
        constexpr auto head_threshold = 10.0F;
        if (!in_front(tip)) return;
        const auto origin = project(layout.origin);
        const auto end = project(tip);
        if (Vector2Distance(origin, end) < 12.0F || Vector2Distance(viewport_mouse_pos, origin) < 10.0F) return;
        if (!finite(origin) || !finite(end)) {
            return;
        }
        consider(axis, segment_distance(viewport_mouse_pos, origin, end), shaft_threshold);
        consider(axis, distance(viewport_mouse_pos, end), head_threshold);
    };

    consider_arrow(GizmoAxis::MoveX, layout.tip_x);
    consider_arrow(GizmoAxis::MoveY, layout.tip_y);
    consider_arrow(GizmoAxis::MoveZ, layout.tip_z);

    // XZ plane square: is the point inside the projected corners?
    {
        std::array<Vector2, 4> q{};
        bool ok = true;
        for (std::size_t i = 0; i < 4; ++i) {
            if (!in_front(layout.plane_xz[i])) { ok = false; break; }
            q[i] = project(layout.plane_xz[i]);
            if (!finite(q[i])) { ok = false; break; }
        }
        if (ok) {
            const auto side = [&](Vector2 a, Vector2 b, Vector2 pt) {
                return (b.x - a.x) * (pt.y - a.y) - (b.y - a.y) * (pt.x - a.x);
            };
            const auto s0 = side(q[0], q[1], viewport_mouse_pos);
            const auto s1 = side(q[1], q[2], viewport_mouse_pos);
            const auto s2 = side(q[2], q[3], viewport_mouse_pos);
            const auto s3 = side(q[3], q[0], viewport_mouse_pos);
            const bool inside = (s0 >= 0 && s1 >= 0 && s2 >= 0 && s3 >= 0) || (s0 <= 0 && s1 <= 0 && s2 <= 0 && s3 <= 0);
            if (inside) consider(GizmoAxis::MoveXZ, 0.0F, 1.0F);
        }
    }

    if (!allow_rotation) return best;
    constexpr auto rotate_threshold = 6.0F;
    const auto rot_tip = project(layout.tip_rot);
    if (finite(rot_tip)) {
        consider(GizmoAxis::Rotate, distance(viewport_mouse_pos, rot_tip), 10.0F);
    }
    constexpr auto ring_segments = 24;
    const auto ring_radius = layout.arm_length * 0.7F;
    for (int i = 0; i < ring_segments; ++i) {
        const auto a = static_cast<float>(i) / static_cast<float>(ring_segments) * 2.0F * PI;
        const auto b = static_cast<float>(i + 1) / static_cast<float>(ring_segments) * 2.0F * PI;
        const auto p0 = project(Vector3{
            layout.origin.x + std::cos(a) * ring_radius,
            layout.origin.y,
            layout.origin.z + std::sin(a) * ring_radius,
        });
        const auto p1 = project(Vector3{
            layout.origin.x + std::cos(b) * ring_radius,
            layout.origin.y,
            layout.origin.z + std::sin(b) * ring_radius,
        });
        if (finite(p0) && finite(p1)) {
            consider(GizmoAxis::Rotate, segment_distance(viewport_mouse_pos, p0, p1), rotate_threshold);
        }
    }
    return best;
}

void draw_building_gizmo(const SbcBuilding& placement, GizmoAxis active, const Camera3D& camera, Vector2 viewport_size, bool allow_rotation = true, Vector3 offset = {}) {
    // A transform gizmo like ImGuizmo's: it stays the same size on screen and has axis
    // arrows with cone tips, a see-through XZ plane square, and a Y rotation ring. The
    // handle under the mouse or in use turns yellow. Everything draws over the scene.
    rlDrawRenderBatchActive();
    rlDisableDepthTest();
    const auto layout = building_gizmo_layout(placement, camera, viewport_size, offset);
    const Color highlight{255, 214, 0, 255};
    const auto axis_color = [&](GizmoAxis axis, Color base) {
        return active == axis ? highlight : base;
    };
    const Color red{226, 61, 61, 255};
    const Color green{102, 201, 61, 255};
    const Color blue{61, 122, 226, 255};
    const float r = layout.handle_radius;

    auto draw_arrow = [&](Vector3 from, Vector3 to, Color color) {
        const auto head_len = r * 3.2F;
        const Vector3 dir{to.x - from.x, to.y - from.y, to.z - from.z};
        const auto len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (len <= std::numeric_limits<float>::epsilon()) return;
        const Vector3 head_base{
            to.x - dir.x * (head_len / len),
            to.y - dir.y * (head_len / len),
            to.z - dir.z * (head_len / len),
        };
        DrawCylinderEx(from, head_base, r * 0.22F, r * 0.22F, 10, color);
        DrawCylinderEx(head_base, to, r * 0.9F, 0.0F, 16, color);
    };
    draw_arrow(layout.origin, layout.tip_x, axis_color(GizmoAxis::MoveX, red));
    draw_arrow(layout.origin, layout.tip_y, axis_color(GizmoAxis::MoveY, green));
    draw_arrow(layout.origin, layout.tip_z, axis_color(GizmoAxis::MoveZ, blue));

    // XZ plane handle: a filled square (both sides) and an outline, coloured by the
    // axis it is at a right angle to.
    {
        const auto base = axis_color(GizmoAxis::MoveXZ, green);
        const Color fill{base.r, base.g, base.b, static_cast<unsigned char>(active == GizmoAxis::MoveXZ ? 170 : 90)};
        const auto& q = layout.plane_xz;
        DrawTriangle3D(q[0], q[1], q[2], fill);
        DrawTriangle3D(q[0], q[2], q[3], fill);
        DrawTriangle3D(q[2], q[1], q[0], fill);
        DrawTriangle3D(q[3], q[2], q[0], fill);
        for (std::size_t i = 0; i < 4; ++i) DrawLine3D(q[i], q[(i + 1) & 3], base);
    }

    // The Y rotation ring, drawn as a thin ring of segments so it is visible at any zoom.
    if (allow_rotation) {
        const auto color = axis_color(GizmoAxis::Rotate, Color{102, 201, 61, 255});
        const float ring = layout.arm_length * 0.7F;
        constexpr int segments = 48;
        for (int i = 0; i < segments; ++i) {
            const float a0 = static_cast<float>(i) / segments * 2.0F * PI;
            const float a1 = static_cast<float>(i + 1) / segments * 2.0F * PI;
            const Vector3 p0{layout.origin.x + std::cos(a0) * ring, layout.origin.y, layout.origin.z + std::sin(a0) * ring};
            const Vector3 p1{layout.origin.x + std::cos(a1) * ring, layout.origin.y, layout.origin.z + std::sin(a1) * ring};
            DrawCylinderEx(p0, p1, r * 0.18F, r * 0.18F, 6, color);
        }
        DrawSphere(layout.tip_rot, r * 0.6F, color);
    }
    // The centre point.
    DrawSphere(layout.origin, r * 0.5F, active == GizmoAxis::None ? Color{235, 235, 235, 255} : highlight);
    rlDrawRenderBatchActive();
    rlEnableDepthTest();
}

// During a gizmo drag, turns the cursor's ground (or vertical) position into a grid
// cell and applies it with the normal edit functions. It measures from the
// starting point saved in ui, so the move is absolute and errors don't add up.
void apply_building_gizmo_drag(
    EditorProject& project,
    EditorUi& ui,
    const Camera3D& camera,
    Vector2 viewport_mouse_pos,
    Vector2 viewport_size) {
    if (!project.city || project.city->data.buildings.empty()) {
        return;
    }
    ui.selected_city_building = std::min(ui.selected_city_building, project.city->data.buildings.size() - 1U);
    auto& placement = project.city->data.buildings[ui.selected_city_building];
    const auto before = placement;
    SbcBuilding drag_start = placement;
    drag_start.x = ui.gizmo_drag_origin_x; drag_start.y = ui.gizmo_drag_origin_y;
    drag_start.z = ui.gizmo_drag_origin_z; drag_start.rotation = ui.gizmo_drag_origin_rot;
    constexpr auto unit_size = 4.0F;
    // The gizmo and the rotation centre sit at the middle of the footprint (fixed during the drag).
    const auto gizmo_offset = city_building_gizmo_offset(project, drag_start);
    const auto pivot = city_building_center(project, drag_start);
    const auto ray = get_viewport_mouse_ray(camera, viewport_mouse_pos, viewport_size);
    const Vec3 ro{ray.position.x, ray.position.y, ray.position.z};
    const Vec3 rd{ray.direction.x, ray.direction.y, ray.direction.z};

    switch (ui.gizmo_active) {
        case GizmoAxis::MoveX:
        case GizmoAxis::MoveY:
        case GizmoAxis::MoveZ: {
            SbcBuilding start = placement;
            start.x = ui.gizmo_drag_origin_x; start.y = ui.gizmo_drag_origin_y; start.z = ui.gizmo_drag_origin_z;
            const auto layout = building_gizmo_layout(start, camera, viewport_size, gizmo_offset);
            const auto tip = ui.gizmo_active == GizmoAxis::MoveX ? layout.tip_x :
                ui.gizmo_active == GizmoAxis::MoveY ? layout.tip_y : layout.tip_z;
            const auto a = GetWorldToScreenEx(layout.origin, camera, static_cast<int>(viewport_size.x), static_cast<int>(viewport_size.y));
            const auto b = GetWorldToScreenEx(tip, camera, static_cast<int>(viewport_size.x), static_cast<int>(viewport_size.y));
            const auto axis = Vector2Subtract(b, a);
            const auto length2 = Vector2LengthSqr(axis);
            if (length2 <= 1) return;
            const auto delta = static_cast<int>(std::lround(Vector2DotProduct(Vector2Subtract(viewport_mouse_pos, ui.gizmo_drag_mouse), axis) / length2 * layout.arm_length / unit_size));
            if (ui.gizmo_active == GizmoAxis::MoveX) placement.x = ui.gizmo_drag_origin_x + delta;
            if (ui.gizmo_active == GizmoAxis::MoveY) placement.y = ui.gizmo_drag_origin_y + delta;
            if (ui.gizmo_active == GizmoAxis::MoveZ) placement.z = ui.gizmo_drag_origin_z + delta;
            break;
        }
        case GizmoAxis::MoveXZ: {
            const auto origin = building_gizmo_layout(drag_start, camera, viewport_size, gizmo_offset).origin;
            const auto start_ray = get_viewport_mouse_ray(camera, ui.gizmo_drag_mouse, viewport_size);
            if (std::abs(rd.y) < .0001F || std::abs(start_ray.direction.y) < .0001F) return;
            const auto t = (origin.y - ro.y) / rd.y;
            const auto start_t = (origin.y - start_ray.position.y) / start_ray.direction.y;
            if (t < 0 || start_t < 0) return;
            const auto dx = (ro.x + rd.x * t) - (start_ray.position.x + start_ray.direction.x * start_t);
            const auto dz = (ro.z + rd.z * t) - (start_ray.position.z + start_ray.direction.z * start_t);
            placement.x = ui.gizmo_drag_origin_x + static_cast<int>(std::lround(dx / unit_size));
            placement.z = ui.gizmo_drag_origin_z + static_cast<int>(std::lround(dz / unit_size));
            break;
        }
        case GizmoAxis::Rotate: {
            const auto origin = building_gizmo_layout(drag_start, camera, viewport_size, gizmo_offset).origin;
            const auto start_ray = get_viewport_mouse_ray(camera, ui.gizmo_drag_mouse, viewport_size);
            if (std::abs(rd.y) < .0001F || std::abs(start_ray.direction.y) < .0001F) return;
            const auto t = (origin.y - ro.y) / rd.y;
            const auto start_t = (origin.y - start_ray.position.y) / start_ray.direction.y;
            if (t < 0 || start_t < 0) return;
            const auto angle = std::atan2(ro.z + rd.z * t - origin.z, ro.x + rd.x * t - origin.x);
            const auto start_angle = std::atan2(start_ray.position.z + start_ray.direction.z * start_t - origin.z,
                start_ray.position.x + start_ray.direction.x * start_t - origin.x);
            const auto delta = std::remainder(angle - start_angle, 2.0F * PI);
            placement.rotation = (ui.gizmo_drag_origin_rot + static_cast<int>(std::lround(delta / (PI * .5F))) + 4) & 3;
            const auto moved_center = city_building_center(project, placement);
            placement.x += static_cast<int>(std::lround((pivot.x - moved_center.x) / unit_size));
            placement.z += static_cast<int>(std::lround((pivot.z - moved_center.z) / unit_size));
            break;
        }
        case GizmoAxis::None:
            break;
    }

    const auto dx = placement.x - ui.gizmo_drag_origin_x;
    const auto dy = placement.y - ui.gizmo_drag_origin_y;
    const auto dz = placement.z - ui.gizmo_drag_origin_z;
    const auto rotation_delta = (placement.rotation - ui.gizmo_drag_origin_rot + 4) & 3;
    for (const auto& [index, start] : ui.gizmo_start_buildings) {
        if (index == ui.selected_city_building || index >= project.city->data.buildings.size()) continue;
        auto& other = project.city->data.buildings[index];
        other.x = start.x + dx; other.y = start.y + dy; other.z = start.z + dz;
        other.rotation = (start.rotation + rotation_delta) & 3;
        if (ui.gizmo_active == GizmoAxis::Rotate) {
            const auto start_origin = native_city_building_render_origin(start, unit_size);
            const auto relative = native_city_rotate_local({start_origin.x-pivot.x, 0, start_origin.z-pivot.z}, rotation_delta);
            const auto origin = native_city_building_render_origin(other, unit_size);
            other.x += static_cast<int>(std::lround((pivot.x + relative.x - origin.x) / unit_size));
            other.z += static_cast<int>(std::lround((pivot.z + relative.z - origin.z) / unit_size));
        }
    }
    if (placement.x != before.x || placement.y != before.y || placement.z != before.z || placement.rotation != before.rotation)
        ui.city_dirty = true;
}

// Piece tool preview: the chosen block mesh, turned by the brush rotation, drawn
// see-through in the cell a click will fill. It uses the same origin and rotation
// as the city piece renderer, which raises pieces 0.25 above their cell.
void draw_piece_ghost(const SblBlockMesh* mesh, CityGridPoint cell, std::int32_t rotation) {
    constexpr float unit = 4.0F;
    const Vector3 origin{cell.x * unit, cell.y * unit + 0.25F, cell.z * unit};
    const Color fill{91, 166, 206, 140};
    const Color edge{170, 225, 255, 235};
    if (mesh != nullptr) {
        for (const auto& face : mesh->faces) {
            if (face.vertices.size() < 4) continue;
            const auto rotated = rotate_block_face_native(*mesh, face, rotation);
            std::array<Vector3, 4> v{};
            for (std::size_t i = 0; i < v.size(); ++i) {
                v[i] = Vector3Add(origin, Vector3Scale({rotated.vertices[i].x, rotated.vertices[i].y, rotated.vertices[i].z}, unit));
            }
            DrawTriangle3D(v[0], v[1], v[2], fill);
            DrawTriangle3D(v[2], v[1], v[0], fill);
            DrawTriangle3D(v[0], v[2], v[3], fill);
            DrawTriangle3D(v[3], v[2], v[0], fill);
            for (std::size_t i = 0; i < v.size(); ++i) DrawLine3D(v[i], v[(i + 1) % v.size()], edge);
        }
    }
    DrawCubeWiresV({(cell.x + 0.5F) * unit, (cell.y + 0.5F) * unit, (cell.z + 0.5F) * unit}, {unit, unit, unit}, edge);
    // A pin above the cell, because one cell is only a few pixels at city zoom.
    const Vector3 pin_base{(cell.x + 0.5F) * unit, (cell.y + 1.0F) * unit, (cell.z + 0.5F) * unit};
    const Vector3 pin_top = Vector3Add(pin_base, {0.0F, unit * 6.0F, 0.0F});
    DrawLine3D(pin_base, pin_top, edge);
    DrawSphere(pin_top, unit * 0.5F, edge);
}

constexpr float kPickCell = 16.0F;

// The buffer owns this grid. Finishing or compacting the geometry clears it, so a
// replaced buffer at a reused address can never keep the old mesh's grid.
void refresh_pick_grid(const BuildingRenderBuffer& buffer) {
    auto& grid = buffer.pick_grid;
    if (grid.ready) return;
    grid = {};
    if (buffer.triangles.empty()) {
        grid.ready = true;
        return;
    }
    float min_x = FLT_MAX, min_z = FLT_MAX, max_x = -FLT_MAX, max_z = -FLT_MAX;
    for (const auto& v : buffer.vertices) {
        min_x = std::min(min_x, v.position.x); max_x = std::max(max_x, v.position.x);
        min_z = std::min(min_z, v.position.z); max_z = std::max(max_z, v.position.z);
    }
    grid.min_x = min_x;
    grid.min_z = min_z;
    grid.nx = std::max(1, static_cast<std::int32_t>(std::floor((max_x - min_x) / kPickCell)) + 1);
    grid.nz = std::max(1, static_cast<std::int32_t>(std::floor((max_z - min_z) / kPickCell)) + 1);
    const auto cell_range = [&](const RenderBufferTriangle& tri, std::int32_t& x0, std::int32_t& x1, std::int32_t& z0, std::int32_t& z1) {
        float lo_x = FLT_MAX, hi_x = -FLT_MAX, lo_z = FLT_MAX, hi_z = -FLT_MAX;
        for (const auto index : tri.indices) {
            const auto& p = buffer.vertices[index].position;
            lo_x = std::min(lo_x, p.x); hi_x = std::max(hi_x, p.x);
            lo_z = std::min(lo_z, p.z); hi_z = std::max(hi_z, p.z);
        }
        x0 = std::clamp(static_cast<std::int32_t>(std::floor((lo_x - grid.min_x) / kPickCell)), 0, grid.nx - 1);
        x1 = std::clamp(static_cast<std::int32_t>(std::floor((hi_x - grid.min_x) / kPickCell)), 0, grid.nx - 1);
        z0 = std::clamp(static_cast<std::int32_t>(std::floor((lo_z - grid.min_z) / kPickCell)), 0, grid.nz - 1);
        z1 = std::clamp(static_cast<std::int32_t>(std::floor((hi_z - grid.min_z) / kPickCell)), 0, grid.nz - 1);
    };
    grid.offsets.assign(static_cast<std::size_t>(grid.nx) * static_cast<std::size_t>(grid.nz) + 1U, 0U);
    for (const auto& tri : buffer.triangles) {
        std::int32_t x0, x1, z0, z1;
        cell_range(tri, x0, x1, z0, z1);
        for (auto z = z0; z <= z1; ++z) for (auto x = x0; x <= x1; ++x) ++grid.offsets[static_cast<std::size_t>(z * grid.nx + x) + 1U];
    }
    for (std::size_t i = 1; i < grid.offsets.size(); ++i) grid.offsets[i] += grid.offsets[i - 1];
    grid.items.resize(grid.offsets.back());
    auto fill = grid.offsets;
    for (std::uint32_t t = 0; t < buffer.triangles.size(); ++t) {
        std::int32_t x0, x1, z0, z1;
        cell_range(buffer.triangles[t], x0, x1, z0, z1);
        for (auto z = z0; z <= z1; ++z) for (auto x = x0; x <= x1; ++x) grid.items[fill[static_cast<std::size_t>(z * grid.nx + x)]++] = t;
    }
    grid.visited.assign(buffer.triangles.size(), 0U);
    grid.ready = true;
}

// Walks the ray's cells from front to back (a 2D DDA over XZ) and reports every
// triangle hit. Stops once the closest hit is inside the cell just tested.
template <typename Consider>
void pick_grid_ray(TrianglePickGrid& grid, const BuildingRenderBuffer& buffer, const Ray& ray, float& nearest, Consider consider) {
    if (grid.offsets.empty()) return;
    if (++grid.stamp == 0U) { std::ranges::fill(grid.visited, 0U); grid.stamp = 1U; }
    const float max_x = grid.min_x + static_cast<float>(grid.nx) * kPickCell;
    const float max_z = grid.min_z + static_cast<float>(grid.nz) * kPickCell;
    // Clip the ray to the grid's XZ rectangle.
    float t0 = 0.0F, t1 = FLT_MAX;
    const auto slab = [&](float origin, float direction, float lo, float hi) {
        if (std::abs(direction) < 1e-9F) return origin >= lo && origin <= hi;
        float a = (lo - origin) / direction, b = (hi - origin) / direction;
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        return t0 <= t1;
    };
    if (!slab(ray.position.x, ray.direction.x, grid.min_x, max_x) || !slab(ray.position.z, ray.direction.z, grid.min_z, max_z)) return;
    const float start_x = ray.position.x + ray.direction.x * t0;
    const float start_z = ray.position.z + ray.direction.z * t0;
    auto cx = std::clamp(static_cast<std::int32_t>(std::floor((start_x - grid.min_x) / kPickCell)), 0, grid.nx - 1);
    auto cz = std::clamp(static_cast<std::int32_t>(std::floor((start_z - grid.min_z) / kPickCell)), 0, grid.nz - 1);
    const std::int32_t step_x = ray.direction.x >= 0.0F ? 1 : -1;
    const std::int32_t step_z = ray.direction.z >= 0.0F ? 1 : -1;
    const auto boundary = [&](std::int32_t cell, std::int32_t step, float origin_min) {
        return origin_min + static_cast<float>(cell + (step > 0 ? 1 : 0)) * kPickCell;
    };
    float t_max_x = std::abs(ray.direction.x) < 1e-9F ? FLT_MAX : (boundary(cx, step_x, grid.min_x) - ray.position.x) / ray.direction.x;
    float t_max_z = std::abs(ray.direction.z) < 1e-9F ? FLT_MAX : (boundary(cz, step_z, grid.min_z) - ray.position.z) / ray.direction.z;
    const float t_delta_x = std::abs(ray.direction.x) < 1e-9F ? FLT_MAX : kPickCell / std::abs(ray.direction.x);
    const float t_delta_z = std::abs(ray.direction.z) < 1e-9F ? FLT_MAX : kPickCell / std::abs(ray.direction.z);
    while (cx >= 0 && cx < grid.nx && cz >= 0 && cz < grid.nz) {
        const auto cell = static_cast<std::size_t>(cz * grid.nx + cx);
        for (auto i = grid.offsets[cell]; i < grid.offsets[cell + 1U]; ++i) {
            const auto t = grid.items[i];
            if (grid.visited[t] == grid.stamp) continue;
            grid.visited[t] = grid.stamp;
            const auto& tri = buffer.triangles[t];
            const auto& a = buffer.vertices[tri.indices[0]].position;
            const auto& b = buffer.vertices[tri.indices[1]].position;
            const auto& c = buffer.vertices[tri.indices[2]].position;
            consider(GetRayCollisionTriangle(ray, {a.x, a.y, a.z}, {b.x, b.y, b.z}, {c.x, c.y, c.z}));
        }
        const float cell_exit = std::min(t_max_x, t_max_z);
        if (nearest <= cell_exit || cell_exit > t1) break;
        if (t_max_x < t_max_z) { cx += step_x; t_max_x += t_delta_x; }
        else { cz += step_z; t_max_z += t_delta_z; }
    }
}

// The closest visible surface under the cursor: the hit point and its normal.
// This picks the drawn surface, not a projection onto Y=0. The preview and the
// click use the same query, so placing and dragging agree on all three axes.
struct CitySurfaceHit {
    Vector3 point{};
    Vector3 normal{0.0F, 1.0F, 0.0F};
};

std::optional<CitySurfaceHit> city_surface_hit_under_mouse(
    EditorProject& project, const Camera3D& camera, Vector2 mouse, Vector2 size) {
    const Ray ray = get_viewport_mouse_ray(camera, mouse, size);
    float nearest = std::numeric_limits<float>::max();
    std::optional<CitySurfaceHit> hit;
    const auto consider = [&](const RayCollision& collision) {
        if (collision.hit && collision.distance >= 0 && collision.distance < nearest) {
            nearest = collision.distance;
            hit = CitySurfaceHit{collision.point, collision.normal};
        }
    };
    const auto mesh_hit = [&](const BuildingRenderBuffer& buffer, const SbcBuilding* placement) {
        const auto world = [&](const RenderBufferVertex& vertex) {
            const auto p = placement ? native_city_building_transform_point(*placement, vertex.position, 4.0F) : vertex.position;
            return Vector3{p.x, p.y, p.z};
        };
        for (const auto& triangle : buffer.triangles) {
            consider(GetRayCollisionTriangle(ray, world(buffer.vertices[triangle.indices[0]]),
                world(buffer.vertices[triangle.indices[1]]), world(buffer.vertices[triangle.indices[2]])));
        }
    };
    const auto& scene = project.city_scene_cache;
    if (scene.terrain) {
        const auto terrain = ray_terrain_hit(*scene.terrain,
            {ray.position.x, ray.position.y, ray.position.z}, {ray.direction.x, ray.direction.y, ray.direction.z});
        if (terrain) {
            hit = CitySurfaceHit{Vector3{terrain->x, terrain->y, terrain->z}};
            nearest = Vector3Distance(ray.position, hit->point);
        }
    } else if (std::abs(ray.direction.y) > 0.0001F) {
        const auto t = -ray.position.y / ray.direction.y;
        if (t >= 0) { nearest = t; hit = CitySurfaceHit{Vector3Add(ray.position, Vector3Scale(ray.direction, t))}; }
    }
    // World-space meshes are picked through cached grids (rebuilt when they change).
    refresh_pick_grid(scene.city_tile_buffer);
    refresh_pick_grid(project.portal_render_buffer);
    pick_grid_ray(scene.city_tile_buffer.pick_grid, scene.city_tile_buffer, ray, nearest, consider);
    pick_grid_ray(project.portal_render_buffer.pick_grid, project.portal_render_buffer, ray, nearest, consider);
    if (project.city) {
        const auto& placement_asset_indices = resolve_placement_asset_indices(project);
        for (std::size_t placement_index = 0; placement_index < project.city->data.buildings.size(); ++placement_index) {
            const auto& placement = project.city->data.buildings[placement_index];
            if (!placement_asset_indices[placement_index].has_value()) continue;
            const auto index = *placement_asset_indices[placement_index];
            if (index >= project.building_render_buffers.size()) continue;
            const auto& b = project.buildings[index].data;
            // A quick bounds check before testing the mesh's real triangles.
            BoundingBox bounds{{FLT_MAX, FLT_MAX, FLT_MAX}, {-FLT_MAX, -FLT_MAX, -FLT_MAX}};
            for (int corner = 0; corner < 8; ++corner) {
                const Vec3 local{
                    ((corner & 1 ? static_cast<float>(b.size_x + 1) : 0) - b.anchor_x) * 4,
                    ((corner & 2 ? static_cast<float>(b.size_z) : 0) - b.anchor_y) * 4,
                    ((corner & 4 ? static_cast<float>(b.size_y + 1) : 0) - b.anchor_z) * 4};
                const auto w = native_city_building_transform_point(placement, local, 4);
                bounds.min = Vector3Min(bounds.min, {w.x, w.y, w.z});
                bounds.max = Vector3Max(bounds.max, {w.x, w.y, w.z});
            }
            if (GetRayCollisionBox(ray, bounds).hit) mesh_hit(project.building_render_buffers[index], &placement);
        }
    }
    return hit;
}

// The closest grid point to the surface under the cursor. Road nodes and building
// anchors sit on grid points.
std::optional<CityGridPoint> city_surface_point_under_mouse(
    EditorProject& project, const Camera3D& camera, Vector2 mouse, Vector2 size) {
    const auto hit = city_surface_hit_under_mouse(project, camera, mouse, size);
    if (!hit) return std::nullopt;
    return CityGridPoint{static_cast<int>(std::lround(hit->point.x / 4)),
        static_cast<int>(std::lround(hit->point.y / 4)), static_cast<int>(std::lround(hit->point.z / 4))};
}

// The piece cell at the surface under the cursor. Piece cells cover [n*4, n*4+4)
// on every axis, so step a little off the surface along its normal and round down:
// a side or top face picks the empty cell next to it, never the cell itself.
std::optional<CityGridPoint> city_surface_cell_under_mouse(
    EditorProject& project, const Camera3D& camera, Vector2 mouse, Vector2 size) {
    const auto hit = city_surface_hit_under_mouse(project, camera, mouse, size);
    if (!hit) return std::nullopt;
    const auto p = Vector3Add(hit->point, Vector3Scale(hit->normal, 0.5F));
    return CityGridPoint{static_cast<int>(std::floor(p.x / 4)),
        static_cast<int>(std::floor(p.y / 4)), static_cast<int>(std::floor(p.z / 4))};
}

// Game coordinates in the viewport's top-right corner (the toolbar uses the top
// middle): the camera, and the surface under the mouse (city) or the block under the
// mouse (building editor). These are game world units (4 per cell); a cell's level
// counts from its floor at level*4 + 0.25.
void draw_coordinate_readout(EditorProject& project, const EditorUi& ui, const Camera3D& camera,
                             ImVec2 image_min, ImVec2 image_max, Vector2 mouse, Vector2 size, bool hovered, float overlay_bottom) {
    const auto cell_of = [](float value) { return static_cast<int>(std::floor(value / 4.0F)); };
    const auto level_of = [](float y) { return static_cast<int>(std::floor((y - 0.25F) / 4.0F)); };
    struct Line { std::string text; ImU32 color; };
    std::vector<Line> lines;
    char text[128];
    const auto add_point = [&](const char* label, Vector3 p, ImU32 color) {
        lines.push_back({label, color});
        std::snprintf(text, sizeof(text), "X %.2f  Y %.2f  Z %.2f", p.x, p.y, p.z);
        lines.push_back({text, IM_COL32(230, 235, 242, 255)});
        std::snprintf(text, sizeof(text), "cell %d, %d, %d", cell_of(p.x), level_of(p.y), cell_of(p.z));
        lines.push_back({text, IM_COL32(150, 160, 175, 255)});
    };
    // Outside the city, the view is the edited asset's own space, not the map's.
    add_point(ui.tab == EditorTab::City ? "Camera" : "Camera (building)", camera.position, IM_COL32(255, 215, 90, 255));
    if (hovered && ui.tab == EditorTab::City && project.city) {
        if (const auto hit = city_surface_hit_under_mouse(project, camera, mouse, size)) {
            add_point("Cursor", hit->point, IM_COL32(120, 200, 255, 255));
        }
        // The road the next click lays: '+' when the game can build its slope, '!' when it can't.
        if (ui.city_tool == CityTool::Road && !ui.road_preview_info.empty()) {
            const auto color = ui.road_preview_info[0] == '+' ? IM_COL32(150, 255, 165, 255) : IM_COL32(255, 115, 100, 255);
            std::string_view rest{ui.road_preview_info};
            rest.remove_prefix(1);
            for (std::size_t at = 0; at <= rest.size();) {
                const auto end = std::min(rest.find('\n', at), rest.size());
                lines.push_back({std::string{rest.substr(at, end - at)}, color});
                at = end + 1;
            }
        }
    } else if (ui.tab == EditorTab::Buildings && ui.building_hover_cell.has_value() && !project.buildings.empty()) {
        const auto& building = project.buildings[std::min(ui.selected_building, project.buildings.size() - 1U)].data;
        if (*ui.building_hover_cell < building.cells.size()) {
            const auto& cell = building.cells[*ui.building_hover_cell];
            lines.push_back({"Block", IM_COL32(120, 200, 255, 255)});
            std::snprintf(text, sizeof(text), "cell %u, %u, %u", cell.x, cell.y, cell.z);
            lines.push_back({text, IM_COL32(230, 235, 242, 255)});
            std::snprintf(text, sizeof(text), "from anchor %d, %d, %d",
                static_cast<int>(cell.x) - static_cast<int>(building.anchor_x),
                static_cast<int>(cell.y) - static_cast<int>(building.anchor_y),
                static_cast<int>(cell.z) - static_cast<int>(building.anchor_z));
            lines.push_back({text, IM_COL32(150, 160, 175, 255)});
        }
    }
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 padding{8.0F, 6.0F};
    const auto line_height = ImGui::GetTextLineHeight();
    float width = 0.0F;
    for (const auto& line : lines) width = std::max(width, ImGui::CalcTextSize(line.text.c_str()).x);
    const ImVec2 box{width + padding.x * 2.0F, line_height * static_cast<float>(lines.size()) + padding.y * 2.0F};
    // Top left, out of the way of the view gizmo (top right) and the tool hint
    // (bottom middle).
    (void)image_max;
    const ImVec2 origin{image_min.x + 10.0F, std::max(image_min.y, overlay_bottom) + 10.0F};
    draw->AddRectFilled(origin, {origin.x + box.x, origin.y + box.y}, IM_COL32(16, 19, 24, 185), 6.0F);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        draw->AddText({origin.x + padding.x, origin.y + padding.y + line_height * static_cast<float>(i)}, lines[i].color, lines[i].text.c_str());
    }
}

// A placement cursor raised by the tool's height offset.
std::optional<CityGridPoint> raised(std::optional<CityGridPoint> point, int dy) {
    if (point) point->y += dy;
    return point;
}

// Moves road junctions up or down one cell level at a time.
void nudge_city_junction_level(EditorProject& project, EditorUi& ui, std::int64_t delta) {
    if (!project.city || project.city->data.intersections.empty()) return;
    auto& map = project.city->data;
    std::vector<std::size_t> nodes;
    if (ui.city_selection == CitySelectionKind::Node) {
        nodes.push_back(std::min(ui.selected_city_intersection, map.intersections.size() - 1U));
    } else if (!map.streets.empty()) {
        const auto& street = map.streets[std::min(ui.selected_city_street, map.streets.size() - 1U)];
        for (const auto endpoint : {street.from_intersection, street.to_intersection}) {
            if (endpoint >= 0 && static_cast<std::size_t>(endpoint) < map.intersections.size() &&
                std::ranges::find(nodes, static_cast<std::size_t>(endpoint)) == nodes.end()) {
                nodes.push_back(static_cast<std::size_t>(endpoint));
            }
        }
    }
    if (delta == 0) return;
    for (const auto node : nodes) {
        const auto y = static_cast<std::int64_t>(map.intersections[node].y);
        if (delta < std::numeric_limits<std::int32_t>::min() - y ||
            delta > std::numeric_limits<std::int32_t>::max() - y) {
            std::string status;
            report_failure(status, "Junction level exceeds the map's signed 32-bit field");
            return;
        }
    }
    for (const auto node : nodes) {
        map.intersections[node].y = static_cast<std::int32_t>(map.intersections[node].y + delta);
    }
    if (!nodes.empty()) ui.city_dirty = true;
}

// Anchor handles use how far the mouse moved on screen, so a long arrow doesn't
// jump when grabbed, and the model keeps its original origin during the drag.
void update_anchor_gizmo(EditorProject& project, EditorUi& ui, const Camera3D& camera,
    Vector2 mouse, Vector2 size, bool hovered) {
    auto& building = project.buildings[ui.selected_building].data;
    SbcBuilding marker{};
    const auto layout = building_gizmo_layout(marker, camera, size);
    if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        ui.anchor_axis = pick_building_gizmo(camera, mouse, size, marker, false);
        if (ui.anchor_axis == GizmoAxis::Rotate || ui.anchor_axis == GizmoAxis::MoveXZ) ui.anchor_axis = GizmoAxis::None;
        ui.anchor_drag_start = {building.anchor_x, building.anchor_y, building.anchor_z};
        ui.anchor_drag_mouse = mouse;
    }
    if (ui.anchor_axis == GizmoAxis::None) return;
    if (IsKeyPressed(KEY_ESCAPE)) {
        building.anchor_x = ui.anchor_drag_start[0]; building.anchor_y = ui.anchor_drag_start[1]; building.anchor_z = ui.anchor_drag_start[2];
        mark_building_changed(project, ui);
        ui.anchor_axis = GizmoAxis::None;
        return;
    }
    const auto tip = ui.anchor_axis == GizmoAxis::MoveX ? layout.tip_x :
        ui.anchor_axis == GizmoAxis::MoveY ? layout.tip_y : layout.tip_z;
    const auto a = GetWorldToScreenEx(layout.origin, camera, static_cast<int>(size.x), static_cast<int>(size.y));
    const auto b = GetWorldToScreenEx(tip, camera, static_cast<int>(size.x), static_cast<int>(size.y));
    const auto axis = Vector2Subtract(b, a);
    const auto length2 = Vector2LengthSqr(axis);
    if (length2 > 1 && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        const auto delta = static_cast<int>(std::lround(Vector2DotProduct(Vector2Subtract(mouse, ui.anchor_drag_mouse), axis) / length2 * layout.arm_length / 4));
        const auto index = ui.anchor_axis == GizmoAxis::MoveX ? 0U : ui.anchor_axis == GizmoAxis::MoveY ? 1U : 2U;
        auto& value = index == 0 ? building.anchor_x : index == 1 ? building.anchor_y : building.anchor_z;
        const auto next = static_cast<std::uint32_t>(std::max<std::int64_t>(0, static_cast<std::int64_t>(ui.anchor_drag_start[index]) + delta));
        if (next != value) { value = next; mark_building_changed(project, ui); }
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) ui.anchor_axis = GizmoAxis::None;
}

// Road tile tops, tilted like the drawn road (draw_city_road_mask). Junction tiles
// get a fainter fill and an outline.
void draw_road_tiles(const std::vector<CityTileCell>& tiles, float unit, Color fill, Color junction_fill, Color edge) {
    for (const auto& tile : tiles) {
        if (tile.kind != CityTileKind::Road) continue;
        const bool junction = tile.intersection_index >= 0;
        const float x0 = static_cast<float>(tile.x) * unit, z0 = static_cast<float>(tile.z) * unit;
        const float cx = x0 + unit * 0.5F, cz = z0 + unit * 0.5F;
        const auto corner = [&](float px, float pz) {
            return Vector3{px, tile.base_y + unit + tile.slope_x * ((px - cx) / unit) + tile.slope_z * ((pz - cz) / unit) + unit * 0.05F, pz};
        };
        const auto p0 = corner(x0, z0), p1 = corner(x0 + unit, z0), p2 = corner(x0 + unit, z0 + unit), p3 = corner(x0, z0 + unit);
        // Counter-clockwise seen from above (raylib culls the other side).
        DrawTriangle3D(p0, p2, p1, junction ? junction_fill : fill); DrawTriangle3D(p0, p3, p2, junction ? junction_fill : fill);
        if (junction) { DrawLine3D(p0, p1, edge); DrawLine3D(p1, p2, edge); DrawLine3D(p2, p3, edge); DrawLine3D(p3, p0, edge); }
    }
}

// Draws the road the next click will lay and returns its description for the
// coordinate readout (this runs in the 3D pass, outside the ImGui frame).
std::string draw_road_lay_preview(const EditorProject& project, EditorUi& ui, std::optional<CityGridPoint> cursor) {
    ui.road_preview_cells.clear();
    if (ui.city_tool != CityTool::Road || !project.city) {
        return {};
    }
    const auto& map = project.city->data;
    const auto unit = project.city_scene_cache_valid ? project.city_scene_cache.unit_size : 4.0F;
    const auto ground = [&](std::int32_t gx, std::int32_t gz, std::int32_t fallback_level) {
        const auto base = project.city_scene_cache_valid && project.city_scene_cache.terrain.has_value()
            ? terrain_height_at(*project.city_scene_cache.terrain, static_cast<float>(gx) * unit, static_cast<float>(gz) * unit)
            : static_cast<float>(fallback_level) * unit;
        return base + unit * 0.6F;
    };

    if (ui.selected_city_intersection < map.intersections.size()) {
        const auto& selected = map.intersections[ui.selected_city_intersection];
        const Vector3 selected_pos{
            static_cast<float>(selected.x) * unit,
            ground(selected.x, selected.z, selected.y),
            static_cast<float>(selected.z) * unit,
        };
        DrawCubeWiresV(selected_pos, {unit * 0.9F, unit * 0.9F, unit * 0.9F}, Color{255, 255, 255, 235});
    }

    if (!ui.pending_road_start.has_value()) {
        return {};
    }

    const auto& start = *ui.pending_road_start;
    const Vector3 start_pos{static_cast<float>(start.x) * unit, ground(start.x, start.z, start.y), static_cast<float>(start.z) * unit};
    DrawSphere(start_pos, unit * 0.8F, Color{120, 230, 130, 235});
    if (!cursor.has_value()) return {};

    // Lay the road on a copy of the map with the Road tool's own code, then draw the
    // tiles the city build places for every road it adds or changes (a road it starts
    // on, ends on, or crosses is split there).
    auto map_copy = map;
    const bool auto_generated = project_is_auto_generated(project);
    lay_city_road(map_copy, start, *cursor, ui.new_road_width, auto_generated);
    const auto ends = [](const SbcMap& m, const SbcStreet& street) {
        if (street.from_intersection < 0 || street.to_intersection < 0 ||
            static_cast<std::size_t>(std::max(street.from_intersection, street.to_intersection)) >= m.intersections.size()) {
            return std::array<std::int32_t, 8>{};
        }
        const auto& a = m.intersections[static_cast<std::size_t>(street.from_intersection)];
        const auto& b = m.intersections[static_cast<std::size_t>(street.to_intersection)];
        return std::array<std::int32_t, 8>{a.x, a.y, a.z, b.x, b.y, b.z, street.left_width, street.right_width};
    };
    std::set<std::array<std::int32_t, 8>> unchanged;
    for (const auto& street : map.streets) unchanged.insert(ends(map, street));
    std::vector<StreetPreview> previews;
    for (std::size_t i = 0; i < map_copy.streets.size(); ++i) {
        if (!unchanged.contains(ends(map_copy, map_copy.streets[i]))) previews.push_back(preview_city_street(map_copy, i, auto_generated));
    }
    if (previews.empty()) return {};

    // The first road the game can't build decides the message; otherwise the longest.
    const auto in_world = [](const StreetPreview& p) {
        return std::ranges::all_of(p.cells, [](const CityTileCell& c) { return city_cell_in_game_world(c.x, c.y, c.z); });
    };
    const auto bad = std::ranges::find_if(previews, [&](const StreetPreview& p) { return !p.run.slope_ok() || !in_world(p); });
    const auto& shown = bad != previews.end() ? *bad
        : *std::ranges::max_element(previews, {}, [](const StreetPreview& p) { return p.run.length; });
    const bool ok = bad == previews.end();
    const Color fill = ok ? Color{120, 230, 130, 110} : Color{240, 80, 70, 130};
    const Color edge = ok ? Color{150, 255, 160, 255} : Color{255, 110, 100, 255};
    // Curbs are left out so the road surface is easy to see.
    for (const auto& preview : previews) {
        ui.road_preview_cells.insert(ui.road_preview_cells.end(), preview.cells.begin(), preview.cells.end());
    }
    draw_road_tiles(ui.road_preview_cells, unit, fill, Color{fill.r, fill.g, fill.b, 55}, edge);

    const auto& run = shown.run;
    const auto climb = std::abs(run.climb);
    std::string info = "Road " + std::to_string(std::max(0, run.length)) + " cells";
    if (climb == 0) info += ", flat";
    else info += ", " + std::string(run.climb > 0 ? "climbs " : "drops ") + std::to_string(climb) + (climb == 1 ? " level" : " levels") +
        ", " + std::to_string(run.cells_per_level) + " cells per level";
    if (previews.size() > 1) info += " (" + std::to_string(previews.size()) + " road pieces change)";
    if (!in_world(shown)) info += "\nOutside the game's map area: the game will not draw this road.";
    if (!run.slope_ok()) {
        if (run.length <= 0) info += "\nThe junctions overlap: no room for a road.";
        else if (run.cells_per_level * climb > run.length) info += "\nThe slope does not fit: the road ends in a step. Lengthen it.";
        else if (run.cells_per_level < 4) info += "\nToo steep: the game has no slope block for this. Needs " + std::to_string(4 * climb) + "+ cells.";
        else info += "\nToo shallow: the game has no slope block for this. Keep it under " + std::to_string(13 * climb) + " cells or climb more.";
    }
    return (ok ? "+" : "!") + info;
}

struct FocusTarget {
    Vector3 point{};
    float radius{};
};

std::optional<FocusTarget> focus_target_for_selection(const EditorProject& project, const EditorUi& ui) {
    constexpr float unit = 4.0F;
    // Move back far enough that a bounding sphere of half-size r fits a vertical field
    // of view of about 55 degrees, with a little room: r / sin(fov/2) * margin.
    const auto fit_radius = [](float r) { return std::max(unit * 4.0F, r / 0.46F * 1.25F); };

    if (ui.tab == EditorTab::City && project.city) {
        const auto& map = project.city->data;
        if (ui.city_selection == CitySelectionKind::Building && !map.buildings.empty()) {
            // Aim at the middle of the placed building's mesh and fit all of it.
            const auto& b = map.buildings[std::min(ui.selected_city_building, map.buildings.size() - 1U)];
            const auto placements = resolve_placement_buildings(project);
            const auto idx = std::min(ui.selected_city_building, map.buildings.size() - 1U);
            const auto* sbb = idx < placements.size() ? placements[idx] : nullptr;
            const auto o = native_city_building_render_origin(b, unit);
            float ex = unit, ey = unit, ez = unit;
            if (sbb != nullptr) {
                ex = static_cast<float>(std::max(1U, sbb->size_x)) * unit;
                ey = static_cast<float>(std::max(1U, sbb->size_z)) * unit;
                ez = static_cast<float>(std::max(1U, sbb->size_y)) * unit;
            }
            auto center = Vector3{o.x, o.y + ey * 0.5F, o.z};
            if (sbb != nullptr) {
                const Vec3 local_center{
                    (static_cast<float>(sbb->size_x) * 0.5F - static_cast<float>(sbb->anchor_x)) * unit,
                    (static_cast<float>(sbb->size_z) * 0.5F - static_cast<float>(sbb->anchor_y)) * unit,
                    (static_cast<float>(sbb->size_y) * 0.5F - static_cast<float>(sbb->anchor_z)) * unit,
                };
                const auto transformed = native_city_building_transform_point(b, local_center, unit);
                center = {transformed.x, transformed.y, transformed.z};
            }
            const auto r = 0.5F * std::sqrt(ex * ex + ey * ey + ez * ez);
            return FocusTarget{center, fit_radius(r)};
        }
        if (ui.city_selection == CitySelectionKind::Piece && ui.selected_portal < map.portals.size()) {
            // The middle of the selected portal's 8x8x8 voxel volume.
            const auto& p = map.portals[ui.selected_portal];
            constexpr float span = 8.0F;
            const Vector3 c{
                (static_cast<float>(p.fields[1]) * span + span * 0.5F) * unit,
                (static_cast<float>(p.fields[2]) * span + span * 0.5F) * unit,
                (static_cast<float>(p.fields[3]) * span + span * 0.5F) * unit,
            };
            return FocusTarget{c, fit_radius(span * unit * 0.5F)};
        }
        if (ui.city_selection == CitySelectionKind::Road && !map.streets.empty()) {
            const auto& s = map.streets[std::min(ui.selected_city_street, map.streets.size() - 1U)];
            if (s.from_intersection >= 0 && s.to_intersection >= 0 &&
                static_cast<std::size_t>(s.from_intersection) < map.intersections.size() &&
                static_cast<std::size_t>(s.to_intersection) < map.intersections.size()) {
                const auto& a = map.intersections[static_cast<std::size_t>(s.from_intersection)];
                const auto& b = map.intersections[static_cast<std::size_t>(s.to_intersection)];
                const Vector3 mid{(a.x + b.x) * 0.5F * unit, (a.y + b.y) * 0.5F * unit, (a.z + b.z) * 0.5F * unit};
                const auto len = std::sqrt(
                    static_cast<float>((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z))) * unit;
                return FocusTarget{mid, fit_radius(std::max(unit * 4.0F, len * 0.5F))};
            }
        }
        if (ui.city_selection == CitySelectionKind::Node && !map.intersections.empty()) {
            const auto& n = map.intersections[std::min(ui.selected_city_intersection, map.intersections.size() - 1U)];
            return FocusTarget{{static_cast<float>(n.x) * unit, static_cast<float>(n.y) * unit, static_cast<float>(n.z) * unit}, fit_radius(unit * 2.0F)};
        }
    } else if (ui.tab == EditorTab::Buildings && !project.buildings.empty()) {
        // Frame the whole building, centred on its mesh.
        const auto& building = project.buildings[std::min(ui.selected_building, project.buildings.size() - 1U)].data;
        const Vector3 center{
            (static_cast<float>(building.size_x) * 0.5F - static_cast<float>(building.anchor_x)) * unit,
            (static_cast<float>(building.size_z) * 0.5F - static_cast<float>(building.anchor_y)) * unit,
            (static_cast<float>(building.size_y) * 0.5F - static_cast<float>(building.anchor_z)) * unit,
        };
        const auto r = 0.5F * std::sqrt(
            std::pow(static_cast<float>(std::max(1U, building.size_x)) * unit, 2.0F) +
            std::pow(static_cast<float>(std::max(1U, building.size_y)) * unit, 2.0F) +
            std::pow(static_cast<float>(std::max(1U, building.size_z)) * unit, 2.0F));
        return FocusTarget{center, fit_radius(r)};
    }
    return std::nullopt;
}

// Starts a smooth camera move to frame the given point from the direction the
// camera already looks, so it feels like moving in, not jumping.
void start_focus_animation(EditorUi& ui, const Camera3D& camera, Vector3 point, float radius) {
    const Vector3 dir{
        camera.position.x - camera.target.x,
        camera.position.y - camera.target.y,
        camera.position.z - camera.target.z,
    };
    const auto len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    const Vector3 unit_dir = len > 0.0001F
        ? Vector3{dir.x / len, dir.y / len, dir.z / len}
        : Vector3{0.6F, 0.55F, 0.6F};
    ui.focus_start_position = camera.position;
    ui.focus_start_target = camera.target;
    ui.focus_end_target = point;
    ui.focus_end_position = {
        point.x + unit_dir.x * radius,
        point.y + unit_dir.y * radius,
        point.z + unit_dir.z * radius,
    };
    ui.focus_t = 0.0F;
    ui.focus_animating = true;
}

// Moves the focus glide forward, easing the camera each frame until it is done.
// Smoothstep easing starts and stops gently.
void update_focus_animation(EditorUi& ui, Camera3D& camera) {
    if (!ui.focus_animating) {
        return;
    }
    ui.focus_t += GetFrameTime() / std::max(ui.focus_duration, 0.01F);
    const auto raw = std::clamp(ui.focus_t, 0.0F, 1.0F);
    const auto e = raw * raw * (3.0F - 2.0F * raw); // smoothstep
    const auto lerp = [e](Vector3 a, Vector3 b) {
        return Vector3{a.x + (b.x - a.x) * e, a.y + (b.y - a.y) * e, a.z + (b.z - a.z) * e};
    };
    camera.position = lerp(ui.focus_start_position, ui.focus_end_position);
    camera.target = lerp(ui.focus_start_target, ui.focus_end_target);
    if (raw >= 1.0F) {
        ui.focus_animating = false;
    }
}

void focus_current_selection(EditorProject& project, EditorUi& ui, Camera3D& camera) {
    set_camera_perspective(camera);
    if (const auto target = focus_target_for_selection(project, ui)) {
        start_focus_animation(ui, camera, target->point, target->radius);
        return;
    }
    Camera3D framed = camera;
    focus_camera_on_city(framed, project);
    start_focus_animation(ui, camera, framed.target, 130.0F);
}

// Handles global commands first, then keys that only work in the viewport. The
// caller already skips this while a text field is active or the free camera flies.
// Removes the selected piece voxel from its portal.
void delete_selected_city_piece(EditorProject& project, EditorUi& ui) {
    if (!project.city || !ui.selected_piece_voxel) return;
    const auto cell = *ui.selected_piece_voxel;
    const std::array<std::int32_t, 4> fields{0, floor_div8(cell.x), floor_div8(cell.y), floor_div8(cell.z)};
    const auto portal = std::ranges::find_if(project.city->data.portals, [&](const SbcPortal& p) { return p.fields == fields; });
    if (portal != project.city->data.portals.end()) {
        auto& voxels = portal->blocks.empty() ? portal->legacy_blocks : portal->blocks;
        if (local_portal_index(cell) < voxels.size()) voxels[local_portal_index(cell)] = 0U;
        invalidate_portal_render_models(project);
        ui.city_dirty = true;
    }
    ui.city_selection = CitySelectionKind::None;
    ui.selected_piece_voxel.reset();
}

void handle_editor_hotkeys(
    EditorProject& project,
    EditorUi& ui,
    Camera3D& camera,
    std::string& status,
    bool viewport_hovered) {
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    // Global shortcuts (can be changed in Preferences).
    if (action_pressed(ui, EditorAction::Save)) {
        save_active_tab(project, ui, status);
        return;
    }
    // City edits and asset edits have separate histories. The current tab picks one, so
    // undo never quietly undoes a different kind of asset.
    if (action_pressed(ui, EditorAction::Undo)) {
        undo_edit(project, ui, status, shift);
        return;
    }
    if (action_pressed(ui, EditorAction::Redo)) {
        undo_edit(project, ui, status, true);
        return;
    }
    if (action_pressed(ui, EditorAction::FocusSelection)) {
        focus_current_selection(project, ui, camera);
    }
    if (action_pressed(ui, EditorAction::FrameCity)) {
        focus_camera_on_city(camera, project);
    }
    const std::pair<EditorAction, EditorTab> tab_actions[] = {
        {EditorAction::TabCity, EditorTab::City},
        {EditorAction::TabBuildings, EditorTab::Buildings},
    };
    for (const auto& [action, tab] : tab_actions) {
        if (action_pressed(ui, action)) ui.requested_tab = tab;
    }
    if (action_pressed(ui, EditorAction::TabEditor)) ui.show_preferences_window = !ui.show_preferences_window;
    const std::pair<EditorAction, bool*> window_actions[] = {
        {EditorAction::WindowBuildingBrowser, &ui.show_building_browser},
        {EditorAction::WindowBlockBrowser, &ui.show_block_browser},
        {EditorAction::WindowBuildblock, &ui.show_buildblock_window},
        {EditorAction::WindowChanges, &ui.show_changes_window},
        {EditorAction::WindowShortcuts, &ui.show_shortcuts_window},
        {EditorAction::WindowPreferences, &ui.show_preferences_window},
        {EditorAction::WindowTools, &ui.show_asset_drawer},
        {EditorAction::WindowLibrary, &ui.show_library_window},
        {EditorAction::WindowProperties, &ui.show_properties_window},
    };
    for (const auto& [action, flag] : window_actions) {
        if (action_pressed(ui, action)) *flag = !*flag;
    }
    if (action_pressed(ui, EditorAction::ToggleShadows)) { ui.render_shadows = !ui.render_shadows; save_preferences(ui); }
    if (action_pressed(ui, EditorAction::ToggleSky)) { ui.render_sky = !ui.render_sky; save_preferences(ui); }
    // Number keys pick tools in both modes: 1-5 in the city and building editors.
    if (ui.tab == EditorTab::Buildings) {
        const std::pair<EditorAction, BuildingTool> building_tool_actions[] = {
            {EditorAction::ToolSelect, BuildingTool::Select}, {EditorAction::ToolBuilding, BuildingTool::PlaceBlock},
            {EditorAction::ToolRoad, BuildingTool::PaintTexture}, {EditorAction::ToolPiece, BuildingTool::Anchor},
            {EditorAction::ToolErase, BuildingTool::ItemSet},
        };
        for (const auto& [action, tool] : building_tool_actions) {
            if (action_pressed(ui, action)) ui.building_tool = tool;
        }
    }
    if (ui.tab == EditorTab::City) {
        const std::pair<EditorAction, CityTool> tool_actions[] = {
            {EditorAction::ToolSelect, CityTool::Select}, {EditorAction::ToolBuilding, CityTool::Building},
            {EditorAction::ToolRoad, CityTool::Road}, {EditorAction::ToolPiece, CityTool::Piece},
            {EditorAction::ToolErase, CityTool::Erase},
        };
        for (const auto& [action, tool] : tool_actions) {
            if (action_pressed(ui, action)) {
                ui.city_tool = tool;
                if (tool != CityTool::Road) ui.pending_road_start.reset();
            }
        }
    }

    // Esc cancels a gizmo drag and puts the object back where it was.
    if (IsKeyPressed(KEY_ESCAPE) && ui.gizmo_active != GizmoAxis::None) {
        if (project.city) for (const auto& [index, start] : ui.gizmo_start_buildings)
            if (index < project.city->data.buildings.size()) project.city->data.buildings[index] = start;
        ui.gizmo_start_buildings.clear();
        ui.gizmo_active = GizmoAxis::None;
        return;
    }
    // Esc ends a chain of roads being laid.
    if (IsKeyPressed(KEY_ESCAPE) && ui.pending_road_start.has_value()) {
        end_road_chain(ui);
        return;
    }

    // Keys that delete or move things belong to the 3D view. Requiring the mouse to be
    // over the 3D image stops Delete, the arrows, and placement keys from firing while
    // the user works in another panel.
    if (!viewport_hovered) {
        return;
    }

    if (ui.tab == EditorTab::City && project.city) {
        // With a placement tool active, R turns its brush (the ghost shows it)
        // instead of the selection. Shift+R turns the other way.
        const bool brush_tool = ui.city_tool == CityTool::Piece || ui.city_tool == CityTool::Building;
        if (brush_tool && action_pressed(ui, EditorAction::RotateSelection)) {
            const std::size_t step = shift ? 3U : 1U;
            if (ui.city_tool == CityTool::Piece) {
                ui.selected_piece_rotation = (ui.selected_piece_rotation + step) & 3U;
            } else {
                ui.place_building_rotation = (ui.place_building_rotation + static_cast<std::int32_t>(step)) & 3;
            }
        }
        const bool nudge_left = action_pressed(ui, EditorAction::NudgeLeft);
        const bool nudge_right = action_pressed(ui, EditorAction::NudgeRight);
        const bool nudge_forward = action_pressed(ui, EditorAction::NudgeForward);
        const bool nudge_back = action_pressed(ui, EditorAction::NudgeBack);
        bool nudge_up = action_pressed(ui, EditorAction::NudgeUp);
        bool nudge_down = action_pressed(ui, EditorAction::NudgeDown);
        // With a placement tool active, Page Up/Down set the placement height.
        const bool placement_tool = ui.city_tool == CityTool::Road || ui.city_tool == CityTool::Piece ||
            ui.city_tool == CityTool::Building;
        if (placement_tool && (nudge_up || nudge_down)) {
            ui.place_height_offset = std::clamp(ui.place_height_offset + (nudge_up ? 1 : -1), -32, 32);
            nudge_up = nudge_down = false;
        }
        const bool rotate = !brush_tool && ui.city_selection == CitySelectionKind::Building && action_pressed(ui, EditorAction::RotateSelection);
        const bool duplicate = action_pressed(ui, EditorAction::DuplicateSelection);
        const bool erase = action_pressed(ui, EditorAction::DeleteSelection);
        // One undo snapshot per editing key press.
        const bool edit_key = nudge_left || nudge_right || nudge_forward || nudge_back || nudge_up || nudge_down ||
            rotate || duplicate || erase ||
            IsKeyPressed(KEY_LEFT_BRACKET) || IsKeyPressed(KEY_RIGHT_BRACKET) ||
            IsKeyPressed(KEY_SEMICOLON) || IsKeyPressed(KEY_APOSTROPHE) ||
            IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_EQUAL);
        if (edit_key && ui.city_selection != CitySelectionKind::None) {
            begin_city_action(project, ui, "edit selection");
        }
        // Edit keys act on the current selection (Select tool), whatever the tool.
        if (ui.city_selection == CitySelectionKind::Building && !project.city->data.buildings.empty()) {
            if (nudge_left) nudge_city_building(project, ui, -1, 0, 0, 0);
            if (nudge_right) nudge_city_building(project, ui, 1, 0, 0, 0);
            if (nudge_forward) nudge_city_building(project, ui, 0, 0, -1, 0);
            if (nudge_back) nudge_city_building(project, ui, 0, 0, 1, 0);
            if (nudge_up) nudge_city_building(project, ui, 0, 1, 0, 0);
            if (nudge_down) nudge_city_building(project, ui, 0, -1, 0, 0);
            if (rotate) nudge_city_building(project, ui, 0, 0, 0, shift ? 3 : 1);
            if (duplicate) duplicate_city_building(project, ui);
            if (erase) delete_city_building(project, ui);
        } else if ((ui.city_selection == CitySelectionKind::Road || ui.city_selection == CitySelectionKind::Node) &&
                   !project.city->data.streets.empty()) {
            if (nudge_up) nudge_city_junction_level(project, ui, 1);
            if (nudge_down) nudge_city_junction_level(project, ui, -1);
            if (IsKeyPressed(KEY_LEFT_BRACKET)) nudge_city_street_width(project, ui, 0, -1);
            if (IsKeyPressed(KEY_RIGHT_BRACKET)) nudge_city_street_width(project, ui, 0, 1);
            if (IsKeyPressed(KEY_SEMICOLON)) nudge_city_street_width(project, ui, 1, -1);
            if (IsKeyPressed(KEY_APOSTROPHE)) nudge_city_street_width(project, ui, 1, 1);
            if (duplicate) duplicate_city_street(project, ui);
            if (erase) delete_city_street(project, ui);
        } else if (ui.city_selection == CitySelectionKind::Piece && ui.selected_piece_voxel) {
            // Pieces move with the same keys as buildings; Delete removes one.
            if (nudge_left) move_city_piece(project, ui, -1, 0, 0, status);
            if (nudge_right) move_city_piece(project, ui, 1, 0, 0, status);
            if (nudge_forward) move_city_piece(project, ui, 0, 0, -1, status);
            if (nudge_back) move_city_piece(project, ui, 0, 0, 1, status);
            if (nudge_up) move_city_piece(project, ui, 0, 1, 0, status);
            if (nudge_down) move_city_piece(project, ui, 0, -1, 0, status);
            if (erase) delete_selected_city_piece(project, ui);
        }
    } else if (ui.tab == EditorTab::Buildings && !project.buildings.empty()) {
        ui.selected_building = std::min(ui.selected_building, project.buildings.size() - 1U);
        const auto occupied = occupied_cell_indices(project.buildings[ui.selected_building].data);
        if (!occupied.empty() && action_pressed(ui, EditorAction::DeleteSelection)) {
            auto& building = project.buildings[ui.selected_building].data;
            const auto index = occupied[std::min(ui.selected_building_cell, occupied.size() - 1)];
            auto& cell = building.cells[index];
            if (ui.building_tool == BuildingTool::ItemSet) {
                if (cell.item_set_index != 0U) {
                    cell.item_set_index = 0U;
                    mark_building_changed(project, ui);
                }
            } else {
                cell.words.fill(0); cell.item_set_index = 0; cell.payloads.clear(); cell.occupied = false;
                cell.shorts.fill(0xffffU);
                mark_building_changed(project, ui);
            }
        }
        if (!occupied.empty() && action_pressed(ui, EditorAction::NextCell)) {
            if (shift) {
                ui.selected_building_cell = ui.selected_building_cell == 0U ? occupied.size() - 1U : ui.selected_building_cell - 1U;
            } else {
                ui.selected_building_cell = (ui.selected_building_cell + 1U) % occupied.size();
            }
        }
    } else if (ui.tab == EditorTab::Blocks && !project.blocks.empty() && ui.block_element != BlockElementKind::None) {
        // The selected patch or box: the arrows move it 1/8 of a cell (Shift: a whole cell).
        const float step = shift ? 1.0F : 0.125F;
        Vec3f delta{};
        if (action_pressed(ui, EditorAction::NudgeLeft)) delta.x -= step;
        if (action_pressed(ui, EditorAction::NudgeRight)) delta.x += step;
        if (action_pressed(ui, EditorAction::NudgeForward)) delta.z -= step;
        if (action_pressed(ui, EditorAction::NudgeBack)) delta.z += step;
        if (action_pressed(ui, EditorAction::NudgeUp)) delta.y += step;
        if (action_pressed(ui, EditorAction::NudgeDown)) delta.y -= step;
        if (delta.x != 0.0F || delta.y != 0.0F || delta.z != 0.0F) nudge_block_element(project, ui, delta);
        if (action_pressed(ui, EditorAction::DuplicateSelection)) duplicate_block_element(project, ui);
        if (action_pressed(ui, EditorAction::DeleteSelection)) delete_block_element(project, ui);
    } else if (ui.tab == EditorTab::Buildblocks && !project.buildblocks.empty()) {
        ui.selected_buildblock = std::min(ui.selected_buildblock, project.buildblocks.size() - 1U);
        const auto& buildblock = project.buildblocks[ui.selected_buildblock].data;
        if (!buildblock.layers.empty() && action_pressed(ui, EditorAction::NextCell)) {
            const auto count = buildblock.layers.size();
            ui.selected_buildblock_layer = shift
                ? (ui.selected_buildblock_layer == 0U ? count - 1U : ui.selected_buildblock_layer - 1U)
                : (ui.selected_buildblock_layer + 1U) % count;
        }
    }
}

// The right-click menu of the 3D view. A quick right-click (a right drag flies)
// first selects what is under the cursor, like a left click, then offers what can
// be done with it. Call inside the scene canvas window.
void draw_viewport_context_menu(EditorProject& project, EditorUi& ui, Camera3D& camera, Vector2 mouse, Vector2 viewport_size,
                                bool open_requested, std::string& status) {
    if (open_requested) {
        const auto ray = get_viewport_mouse_ray(camera, mouse, viewport_size);
        const Vec3 ro{ray.position.x, ray.position.y, ray.position.z};
        const Vec3 rd{ray.direction.x, ray.direction.y, ray.direction.z};
        if (ui.tab == EditorTab::City && project.city) {
            const auto picked = pick_city_any(project, project.city_scene_cache, ro, rd);
            const bool in_group = picked.kind == CitySelectionKind::Building && ui.city_selection == CitySelectionKind::Building &&
                (picked.index == ui.selected_city_building || std::ranges::find(ui.selected_city_buildings, picked.index) != ui.selected_city_buildings.end());
            if (picked.kind != CitySelectionKind::None && !in_group) select_city_thing(project, ui, picked, false, ro, rd, status);
        } else if (ui.tab == EditorTab::Buildings && !project.buildings.empty()) {
            std::string ignored;
            select_building_cell_from_viewport(project, ui, camera, mouse, viewport_size, std::nullopt, ignored);
        } else if (ui.tab == EditorTab::Blocks && !project.blocks.empty()) {
            const auto before = std::pair{ui.block_element, ui.block_element_index};
            pick_block_element(project, ui, ray);
            // Right-clicking empty space keeps the selection, as in Blender.
            if (ui.block_element == BlockElementKind::None) std::tie(ui.block_element, ui.block_element_index) = before;
        }
        ImGui::OpenPopup("##viewport_menu");
    }
    if (!ImGui::BeginPopup("##viewport_menu")) return;
    const auto shortcut = [&](EditorAction action) { return action_shortcut_label(ui, action); };
    if (ui.tab == EditorTab::City && project.city) {
        auto& map = project.city->data;
        switch (ui.city_selection) {
            case CitySelectionKind::Building:
                if (ui.selected_city_building < map.buildings.size()) {
                    const auto count = std::max<std::size_t>(1U, ui.selected_city_buildings.size());
                    ImGui::TextDisabled("%s", count > 1U ? (std::to_string(count) + " buildings").c_str() : map.buildings[ui.selected_city_building].name.c_str());
                    if (ImGui::MenuItem(ICON_FA_PEN " Edit building")) edit_city_building(project, ui, ui.selected_city_building);
                    if (ImGui::MenuItem(ICON_FA_CROSSHAIRS " Frame", shortcut(EditorAction::FocusSelection).c_str())) focus_current_selection(project, ui, camera);
                    if (ImGui::MenuItem(ICON_FA_ROTATE " Turn 90 degrees", shortcut(EditorAction::RotateSelection).c_str())) {
                        begin_city_action(project, ui, "rotate building");
                        nudge_city_building(project, ui, 0, 0, 0, 1);
                    }
                    if (ImGui::MenuItem(ICON_FA_COPY " Duplicate", shortcut(EditorAction::DuplicateSelection).c_str())) {
                        begin_city_action(project, ui, "duplicate building");
                        duplicate_city_building(project, ui);
                    }
                    if (ImGui::MenuItem(ICON_FA_TRASH " Delete", shortcut(EditorAction::DeleteSelection).c_str())) {
                        begin_city_action(project, ui, "delete building");
                        delete_city_building(project, ui);
                    }
                    ImGui::Separator();
                    bool selected_only = ui.near_lod && ui.interiors_selected_only;
                    if (ImGui::MenuItem(ICON_FA_DOOR_OPEN " Show interiors of selected only", nullptr, &selected_only)) {
                        ui.near_lod = true;
                        ui.interiors_selected_only = selected_only;
                        save_preferences(ui);
                    }
                }
                break;
            case CitySelectionKind::Road:
                ImGui::TextDisabled("Road");
                if (ImGui::MenuItem(ICON_FA_CROSSHAIRS " Frame", shortcut(EditorAction::FocusSelection).c_str())) focus_current_selection(project, ui, camera);
                if (ImGui::MenuItem(ICON_FA_ARROW_UP " Raise")) { begin_city_action(project, ui, "raise road"); nudge_city_junction_level(project, ui, 1); }
                if (ImGui::MenuItem(ICON_FA_ARROW_DOWN " Lower")) { begin_city_action(project, ui, "lower road"); nudge_city_junction_level(project, ui, -1); }
                if (ImGui::MenuItem(ICON_FA_COPY " Duplicate", shortcut(EditorAction::DuplicateSelection).c_str())) {
                    begin_city_action(project, ui, "duplicate road");
                    duplicate_city_street(project, ui);
                }
                if (ImGui::MenuItem(ICON_FA_TRASH " Delete", shortcut(EditorAction::DeleteSelection).c_str())) {
                    begin_city_action(project, ui, "delete road");
                    delete_city_street(project, ui);
                }
                break;
            case CitySelectionKind::Node:
                if (ui.selected_city_intersection < map.intersections.size()) {
                    const auto& node = map.intersections[ui.selected_city_intersection];
                    ImGui::TextDisabled("Junction");
                    if (ImGui::MenuItem(ICON_FA_ROAD " Start a road here")) {
                        ui.city_tool = CityTool::Road;
                        ui.pending_road_start = CityGridPoint{node.x, node.y, node.z};
                    }
                    if (ImGui::MenuItem(ICON_FA_ARROW_UP " Raise")) { begin_city_action(project, ui, "move junction"); nudge_city_junction_level(project, ui, 1); }
                    if (ImGui::MenuItem(ICON_FA_ARROW_DOWN " Lower")) { begin_city_action(project, ui, "move junction"); nudge_city_junction_level(project, ui, -1); }
                }
                break;
            case CitySelectionKind::Piece:
                ImGui::TextDisabled("Piece");
                if (ImGui::MenuItem(ICON_FA_CROSSHAIRS " Frame", shortcut(EditorAction::FocusSelection).c_str())) focus_current_selection(project, ui, camera);
                if (ImGui::MenuItem(ICON_FA_TRASH " Delete", shortcut(EditorAction::DeleteSelection).c_str())) {
                    begin_city_action(project, ui, "delete piece");
                    delete_selected_city_piece(project, ui);
                }
                break;
            case CitySelectionKind::None:
                break;
        }
        if (ui.city_selection != CitySelectionKind::None) ImGui::Separator();
        if (ImGui::BeginMenu(ICON_FA_TOOLBOX " Tool")) {
            constexpr std::pair<CityTool, const char*> tools[] = {{CityTool::Select, "Select"}, {CityTool::Building, "Place building"},
                {CityTool::Road, "Lay road"}, {CityTool::Piece, "Stamp piece"}, {CityTool::Erase, "Erase"}};
            for (const auto& [tool, name] : tools) {
                if (ImGui::MenuItem(name, nullptr, ui.city_tool == tool)) {
                    ui.city_tool = tool;
                    if (tool != CityTool::Road) ui.pending_road_start.reset();
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(ICON_FA_EXPAND " Show the whole city", shortcut(EditorAction::FrameCity).c_str())) focus_camera_on_city(camera, project);
    } else if (ui.tab == EditorTab::Buildings && !project.buildings.empty()) {
        auto& building = project.buildings[std::min(ui.selected_building, project.buildings.size() - 1U)].data;
        const auto occupied = occupied_cell_indices(building);
        if (!occupied.empty()) {
            auto& cell = building.cells[occupied[std::min(ui.selected_building_cell, occupied.size() - 1U)]];
            ImGui::TextDisabled("Block %u, %u, %u", cell.x, cell.y, cell.z);
            if (cell.interior_block() == 0xffffU) {
                bool floor = (cell.placed_flag() & 1U) != 0U;
                if (ImGui::MenuItem(ICON_FA_LAYER_GROUP " Room floor", nullptr, &floor)) {
                    cell.placed_flag() = floor ? (cell.placed_flag() | 1U) : (cell.placed_flag() & ~1U);
                    refresh_sbb_cell_payloads(cell);
                    mark_building_changed(project, ui);
                }
                if (const auto* buildblock = buildblock_for_cell(project, building, cell)) {
                    if (ImGui::MenuItem(ICON_FA_TABLE_CELLS " Edit its buildblock")) {
                        ui.selected_buildblock = static_cast<std::size_t>(buildblock - project.buildblocks.data());
                        ui.selected_buildblock_layer = 0;
                        ui.show_buildblock_window = true;
                    }
                }
            }
            if (ImGui::MenuItem(ICON_FA_TRASH " Delete block", shortcut(EditorAction::DeleteSelection).c_str())) {
                cell.words.fill(0); cell.item_set_index = 0; cell.payloads.clear(); cell.occupied = false;
                cell.shorts.fill(0xffffU);
                mark_building_changed(project, ui);
            }
            ImGui::Separator();
        }
        if (ImGui::BeginMenu(ICON_FA_TOOLBOX " Tool")) {
            constexpr std::pair<BuildingTool, const char*> tools[] = {{BuildingTool::Select, "Select block"}, {BuildingTool::PlaceBlock, "Add block"},
                {BuildingTool::PaintTexture, "Paint faces"}, {BuildingTool::Anchor, "Move anchor"}, {BuildingTool::ItemSet, "Place item set"}};
            for (const auto& [tool, name] : tools) {
                if (ImGui::MenuItem(name, nullptr, ui.building_tool == tool)) ui.building_tool = tool;
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(ICON_FA_HOUSE " Hide roofs", nullptr, &ui.building_hide_roofs)) project.building_floor_preview_dirty = true;
        if (ImGui::MenuItem(ICON_FA_LAYER_GROUP " One floor at a time", nullptr, &ui.building_view_floor_only)) project.building_floor_preview_dirty = true;
        if (ImGui::MenuItem(ICON_FA_CROSSHAIRS " Frame", shortcut(EditorAction::FocusSelection).c_str())) focus_current_selection(project, ui, camera);
    } else if (ui.tab == EditorTab::Blocks && !project.blocks.empty()) {
        if (ui.block_element != BlockElementKind::None) {
            ImGui::TextDisabled("%s %zu", ui.block_element == BlockElementKind::Patch ? "Patch" : "Box", ui.block_element_index + 1U);
            const auto mouse_now = mouse;
            if (ImGui::MenuItem(ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT " Move", "G")) start_block_transform(project, ui, BlockTransform::Grab, mouse_now);
            if (ImGui::MenuItem(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER " Scale", "S")) start_block_transform(project, ui, BlockTransform::Scale, mouse_now);
            if (ImGui::MenuItem(ICON_FA_ROTATE " Rotate", "R")) start_block_transform(project, ui, BlockTransform::Rotate, mouse_now);
            if (ImGui::MenuItem(ICON_FA_COPY " Duplicate", shortcut(EditorAction::DuplicateSelection).c_str())) duplicate_block_element(project, ui);
            if (ImGui::MenuItem(ICON_FA_TRASH " Delete", shortcut(EditorAction::DeleteSelection).c_str())) delete_block_element(project, ui);
            ImGui::Separator();
        }
        if (ImGui::MenuItem(ICON_FA_PLUS " Add patch")) add_block_patch(project, ui);
        if (ImGui::MenuItem(ICON_FA_PLUS " Add box")) add_block_box(project, ui);
        if (ImGui::MenuItem(ICON_FA_EXPAND " Frame the block")) frame_block_camera(project, ui, camera);
    }
    ImGui::Separator();
    if (ImGui::BeginMenu(ICON_FA_EYE " View")) {
        if (ImGui::MenuItem("Front", "Numpad 1")) snap_view(camera, ui, {0.0F, 0.0F, 1.0F});
        if (ImGui::MenuItem("Back", "Ctrl+Numpad 1")) snap_view(camera, ui, {0.0F, 0.0F, -1.0F});
        if (ImGui::MenuItem("Right", "Numpad 3")) snap_view(camera, ui, {1.0F, 0.0F, 0.0F});
        if (ImGui::MenuItem("Left", "Ctrl+Numpad 3")) snap_view(camera, ui, {-1.0F, 0.0F, 0.0F});
        if (ImGui::MenuItem("Top", "Numpad 7")) snap_view(camera, ui, {0.0F, 1.0F, 0.0F});
        if (ImGui::MenuItem("Bottom", "Ctrl+Numpad 7")) snap_view(camera, ui, {0.0F, -1.0F, 0.0F});
        ImGui::Separator();
        bool ortho = camera.projection == CAMERA_ORTHOGRAPHIC;
        if (ImGui::MenuItem("Orthographic", "Numpad 5", &ortho)) {
            ui.view_snap_active = false;
            if (ortho) set_camera_orthographic(camera); else set_camera_perspective(camera);
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT " Undo", shortcut(EditorAction::Undo).c_str(), false, can_undo(project, ui, false))) undo_edit(project, ui, status, false);
    if (ImGui::MenuItem(ICON_FA_ROTATE_RIGHT " Redo", shortcut(EditorAction::Redo).c_str(), false, can_undo(project, ui, true))) undo_edit(project, ui, status, true);
    ImGui::EndPopup();
}

// Frees a project's thumbnail RenderTextures before the project is replaced or
// destroyed. RenderTexture2D is plain data with no destructor, so this has to be done
// by hand.
void free_project_thumbnails(EditorProject& project) {
    for (auto& [name, rt] : project.item_set_thumbnails) {
        if (rt.id != 0U) UnloadRenderTexture(rt);
    }
    project.item_set_thumbnails.clear();
    for (auto& rt : project.building_thumbnails) {
        if (rt.id != 0U) {
            UnloadRenderTexture(rt);
        }
    }
    for (auto& rt : project.city_block_thumbnails) {
        if (rt.id != 0U) {
            UnloadRenderTexture(rt);
        }
    }
    project.building_thumbnails.clear();
    project.building_thumbnail_ready.clear();
    project.city_block_thumbnails.clear();
    project.city_block_thumbnail_ready.clear();
    for (auto& rt : project.block_thumbnails) {
        if (rt.id != 0U) {
            UnloadRenderTexture(rt);
        }
    }
    project.block_thumbnails.clear();
    project.block_thumbnail_ready.clear();
}

// Fills the thumbnail caches a few at a time each frame after loading, so the
// galleries open straight away, then clears the pregenerating flag. Thumbnails need
// the GL context, so this runs on the main thread, spread out to avoid a long pause.
void service_pregeneration(EditorProject& project) {
    PAFTA_PROFILE_FUNCTION();
    if (!project.pregenerating) {
        return;
    }
    constexpr std::size_t per_frame = 3;
    for (std::size_t n = 0; n < per_frame && project.pregen_building_cursor < project.buildings.size(); ++n) {
        building_thumbnail(project, project.pregen_building_cursor++);
    }
    const std::size_t blocks = project.city ? project.city->data.city_blocks.size() : 0U;
    for (std::size_t n = 0; n < per_frame && project.pregen_block_cursor < blocks; ++n) {
        city_block_thumbnail(project, project.pregen_block_cursor++);
    }
    if (project.pregen_building_cursor >= project.buildings.size() && project.pregen_block_cursor >= blocks) {
        project.pregenerating = false;
    }
}

// A loading screen in the middle with progress bars while the project gets ready
// (render buffers building and uploading, thumbnails being made). It hides once
// everything is ready, so it never gets in the way of normal editing. Roads and
// terrain are only built while the City view is open, so outside it that step is
// not waited on (or it would stay "Queued" forever).
void draw_loading_overlay(EditorProject& project, bool city_view) {
    const bool buffers_ready =
        !project.render_build_in_progress &&
        project.pending_gpu_upload_indices.empty() &&
        project.pending_gpu_upload_index >= project.building_render_buffers.size();
    const bool scene_pending = city_view && project.city && !project.city_scene_cache_valid;
    if (buffers_ready && !project.pregenerating && !scene_pending) {
        return;
    }

    const auto vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5F, vp->WorkPos.y + vp->WorkSize.y * 0.5F),
                            ImGuiCond_Always, ImVec2(0.5F, 0.5F));

    if (ImGui::Begin("Loading", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::Text("Preparing %s", project.root.filename().string().c_str());
        ImGui::Dummy(ImVec2(360.0F, 4.0F));

        const float buf_total = std::max<float>(1.0F, static_cast<float>(project.building_render_buffers.size()));
        const float buf_done = std::min<float>(static_cast<float>(project.pending_gpu_upload_index), buf_total);
        ImGui::TextUnformatted("Building meshes");
        ImGui::ProgressBar(buffers_ready ? 1.0F : buf_done / buf_total, ImVec2(360.0F, 0.0F));
        if (scene_pending) {
            ImGui::TextUnformatted(project.city_scene_build_in_progress
                ? "Building roads and terrain on a worker..." : "Queued: roads and terrain");
        }

    }
    ImGui::End();
}

// Delayed and background work that is running, for the status bar. fraction < 0
// means the progress can't be measured (a single worker step).
struct BackgroundJob {
    std::string label;
    float fraction{-1.0F};
};

std::vector<BackgroundJob> background_jobs(const EditorProject& project, bool city_view) {
    std::vector<BackgroundJob> jobs;
    const auto ratio = [](std::size_t done, std::size_t total) {
        return total == 0U ? 0.0F : static_cast<float>(done) / static_cast<float>(total);
    };
    if (project.render_build_in_progress) {
        BackgroundJob job{"Building meshes"};
        if (const auto& progress = project.render_build_progress) {
            std::scoped_lock lock(progress->mutex);
            job.label += " " + std::to_string(progress->completed) + "/" + std::to_string(progress->total);
            if (!progress->current.empty()) job.label += " (" + progress->current + ")";
            job.fraction = ratio(progress->completed, progress->total);
        }
        jobs.push_back(std::move(job));
    }
    if (!project.pending_gpu_upload_indices.empty()) {
        const auto total = project.pending_gpu_upload_index + project.pending_gpu_upload_indices.size();
        jobs.push_back({"Uploading meshes " + std::to_string(project.pending_gpu_upload_index) + "/" + std::to_string(total),
            ratio(project.pending_gpu_upload_index, total)});
    }
    if (project.city) {
        if (project.city_scene_build_in_progress) {
            jobs.push_back({"Rebuilding roads and terrain"});
        } else if (city_view && project.city_scene_cache_valid &&
                   project.city_scene_cache_fingerprint != project.city_scene_current_fingerprint) {
            jobs.push_back({project.defer_heavy_rebuilds ? "Roads and terrain: rebuild after this edit"
                                                         : "Roads and terrain: rebuild queued"});
        } else if (city_view && project.city_scene_cache_valid && !project.city_scene_gpu_valid) {
            jobs.push_back({"Uploading roads and terrain"});
        } else if (city_view && !project.city_scene_cache_valid) {
            jobs.push_back({"Roads and terrain: queued"});
        }
        if (project.portal_rebuild_in_progress) {
            jobs.push_back({"Rebuilding pieces"});
        } else if (city_view && project.portal_render_dirty) {
            jobs.push_back({project.defer_heavy_rebuilds ? "Pieces: rebuild after this edit" : "Pieces: rebuild queued"});
        }
    }
    if (project.pregenerating) {
        const std::size_t blocks = project.city ? project.city->data.city_blocks.size() : 0U;
        const auto total = project.buildings.size() + blocks;
        const auto done = project.pregen_building_cursor + project.pregen_block_cursor;
        jobs.push_back({"Thumbnails " + std::to_string(done) + "/" + std::to_string(total), ratio(done, total)});
    }
    return jobs;
}

#ifdef __EMSCRIPTEN__
// In the browser, loading runs on the GL thread. Show this frame before its single
// Asyncify yield, so parsing starts after the browser has drawn the message.
void present_parsing_frame(const std::filesystem::path& folder) {
    PAFTA_PROFILE_FUNCTION();
    BeginDrawing();
    ClearBackground(DARKGRAY);
    rlImGuiBegin();

    const auto vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5F, vp->WorkPos.y + vp->WorkSize.y * 0.5F),
                            ImGuiCond_Always, ImVec2(0.5F, 0.5F));

    if (ImGui::Begin("Parsing", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::Text("Parsing %s...", folder.filename().string().c_str());
        ImGui::Dummy(ImVec2(360.0F, 4.0F));
        ImGui::TextDisabled("reading buildings, blocks, and archives");
    }
    ImGui::End();

    ImGui::Render();
    render_imgui_draw_data(ImGui::GetDrawData());
    EndDrawing();
}

#endif


} // namespace pafta::app_internal

namespace pafta {

int run_editor_app() {
    PAFTA_PROFILE_FUNCTION();
    using namespace app_internal;
    // No window MSAA. 3D is drawn into offscreen targets and ImGui smooths its own
    // shapes, so multisampling the window only cost memory.
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(1440, 900, "Pafta v" PAFTA_VERSION);
    SetWindowMinSize(1100, 700);
    // PAFTA_BENCH=N: no frame limit. Once the scene has settled, time N frames, print
    // the averages, and exit. Use it with PAFTA_AUTOLOAD.
    const int bench_frames = std::getenv("PAFTA_BENCH") == nullptr ? 0 : std::max(1, std::atoi(std::getenv("PAFTA_BENCH")));
    SetTargetFPS(bench_frames > 0 ? 0 : 60);
    // Esc must not quit the app. It cancels gizmo drags, and closing has to go through
    // the unsaved-changes prompt below. KEY_NULL also stops Esc from triggering
    // WindowShouldClose().
    SetExitKey(KEY_NULL);
    // Push the far clip plane a long way out (the default is 1000). The city is
    // thousands of world units across at 4 units per cell, so the default cuts off far
    // terrain and buildings. rlgl reads these on every BeginMode3D, so one call is enough.
    rlSetClipPlanes(0.05, 100000.0);

    EditorUi ui;
    ArchiveWorkspace archive_workspace;
    load_preferences(ui);
#ifdef __EMSCRIPTEN__
    // ImGui reads imgui.ini on its first frame, which restores the saved layout.
    web_restore_file("imgui.ini", "pafta.imgui_ini");
#endif
    rlImGuiBeginInitImGui();
    apply_editor_style(ui.ui_font_size);
    rlImGuiEndInitImGui();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    Camera3D camera = default_camera();
    std::optional<EditorProject> project;
    std::string status = "drop a map folder or click File -> Open Folder";
    std::string last_notification_status = status;
    std::string notification;
    double notification_until = 0.0;
    std::size_t notified_action_count = 0;
    std::string notified_action;

    RenderTexture2D viewport_texture = load_game_render_texture(1280, 800);
    RenderTexture2D building_outline_mask_texture = LoadRenderTexture(1280, 800);
    Vector2 last_viewport_size = { 1280, 800 };

    GfxDebugStats gfx_debug_stats;

    // Guarding close: a close request (the window X, Alt+F4, or the Exit menu) sets
    // this. With unsaved changes it opens a save prompt and waits for an answer before
    // quitting.
    bool running = true;
    bool close_requested = false;
    bool open_archive_close_prompt = false;
    bool open_unsaved_prompt = false;
    bool unsaved_prompt_for_open = false;
    bool open_project_approved = false;
    std::optional<EditorProject> project_to_open;

    // Opening or dropping a map queues it to replace the current one. On desktop,
    // parsing runs on a worker; in the browser, a progress frame is shown first. The
    // current project stays here until the new one is fully loaded and unsaved edits
    // are dealt with.
    std::optional<std::filesystem::path> pending_load_folder;
#ifndef __EMSCRIPTEN__
    std::optional<std::future<std::pair<std::optional<EditorProject>, std::string>>> load_future;
    auto load_stage = std::make_shared<std::atomic<int>>(0);
#endif
    // Optional autoload: if PAFTA_AUTOLOAD names a map folder, it is queued at start.
    // Useful for profiling and screenshot runs that need a known map open without
    // clicking Open Folder. Does nothing when the variable is not set.
#ifdef __EMSCRIPTEN__
    EM_ASM({
        // The editor owns the keyboard: no browser shortcut works while it is open
        // (Ctrl+S save page, Ctrl+W close, Ctrl+R/F5 reload, Ctrl+1 switch tab, Tab focus,
        // F3 or ' or / find...). The editor still gets every key. Plain typing is left
        // alone (text reaches the editor through the keypress that a blocked keydown would
        // cancel), and Ctrl+C/V/X still work, because the browser's clipboard events are how
        // paste reaches the page.
        window.addEventListener('keydown', (event) => {
            const key = event.key.toLowerCase();
            const modified = event.ctrlKey || event.metaKey || event.altKey;
            const clipboard = (event.ctrlKey || event.metaKey) && ['c', 'v', 'x'].includes(key);
            const browser_key = key === 'tab' || /^f[0-9]+$/.test(key) || key === "'" || key === '/';
            if ((modified && !clipboard) || browser_key) event.preventDefault();
        }, true);
        // Outside fullscreen, Chrome and Firefox never pass Ctrl+W/T/N (Cmd+W/T/N/Q on
        // macOS) to a page; Keyboard Lock catches them in fullscreen. Otherwise, always ask
        // before the tab closes or reloads.
        document.addEventListener('fullscreenchange', () => {
            if (document.fullscreenElement && navigator.keyboard && navigator.keyboard.lock) {
                navigator.keyboard.lock().catch(error => console.warn('Keyboard lock failed:', error));
            }
        });
        window.addEventListener('beforeunload', (event) => { event.preventDefault(); event.returnValue = ""; });
    });
#endif
    if (const char* env = std::getenv("PAFTA_AUTOLOAD")) {
        pending_load_folder = std::filesystem::path{env};
    }
#ifdef __EMSCRIPTEN__
#ifdef PAFTA_WEB_PRELOAD_ROOT
    // Only test builds define this path. The normal browser build leaves it empty and
    // asks the user to choose or drop a project folder.
    if (!pending_load_folder) {
        pending_load_folder = std::filesystem::path{PAFTA_WEB_PRELOAD_ROOT};
    }
#endif
#endif
    // Repeatable visual testing: capture the loaded editor after 300 frames. The
    // optional tab picks city, buildings, pieces, buildblocks, or blocks.
    std::optional<std::filesystem::path> screenshot_path;
    if (const char* env = std::getenv("PAFTA_SCREENSHOT")) {
        screenshot_path = std::filesystem::path{env};
    }
    const std::string screenshot_tab = std::getenv("PAFTA_SCREENSHOT_TAB") == nullptr
        ? std::string{}
        : std::getenv("PAFTA_SCREENSHOT_TAB");
    const int screenshot_frame_target = std::getenv("PAFTA_SCREENSHOT_FRAMES") == nullptr
        ? 300
        : std::max(1, std::atoi(std::getenv("PAFTA_SCREENSHOT_FRAMES")));
    int screenshot_ready_frames = 0;

    // Mesh export options. File > Export Mesh opens this popup, which shows a title for
    // the current view and checkboxes for what to export, then opens the native save
    // dialog. The options are remembered for the rest of the session.
    bool open_export_popup = false;
    ExportOptions export_options{};
#ifndef __EMSCRIPTEN__
    std::optional<MeshExportTask> mesh_export_task;
#endif
    // How many more frames to redraw the 3D view after the last change (see redraw_3d).
    int redraw_frames = 60;
    while (running) {
        PAFTA_PROFILE_BLOCK("editor frame");
        gfx_debug_stats.reset_frame();
        gfx_debug_stats.push_frame_time(GetFrameTime() * 1000.0F);
        gfx_debug_stats.add_phase_ms(GfxDebugTimingPhase::TotalFrame, GetFrameTime() * 1000.0);
        g_gfx_debug_stats = &gfx_debug_stats;
        g_render_settings.building_distance = ui.building_distance;
        g_render_settings.half_textures = ui.half_textures;
        g_render_settings.near_lod = ui.near_lod;
        // Selected-only interiors: the placements whose rooms are drawn at any distance.
        g_render_settings.interior_placements.clear();
        if (ui.interiors_selected_only && ui.city_selection == CitySelectionKind::Building) {
            g_render_settings.interior_placements = ui.selected_city_buildings;
            g_render_settings.interior_placements.push_back(ui.selected_city_building);
        }
        g_render_settings.interiors_selected_only = ui.interiors_selected_only;
        g_render_settings.street_objects = ui.street_objects;
        g_render_settings.live_reflections = ui.live_reflections;
        if (project && g_render_settings.anisotropy != ui.anisotropy) apply_texture_anisotropy(*project, ui.anisotropy);
        g_render_settings.anisotropy = ui.anisotropy;
#ifndef __EMSCRIPTEN__
        // Frame pacing: V-Sync, the frame limit, and a low rate when behind other windows.
        if (bench_frames == 0) {
            static bool vsync_on = false;
            if (ui.vsync != vsync_on) {
                vsync_on = ui.vsync;
                if (vsync_on) SetWindowState(FLAG_VSYNC_HINT); else ClearWindowState(FLAG_VSYNC_HINT);
            }
            const int target = ui.background_throttle && !IsWindowFocused() ? 15 : ui.fps_limit;
            static int applied_target = -1;
            if (target != applied_target) {
                applied_target = target;
                SetTargetFPS(target);
            }
        }
#endif

#ifdef __EMSCRIPTEN__
        // raylib's browser resize handler resizes the canvas but not GLFW's window, and
        // Emscripten's GLFW scales mouse input by window size / CSS size, so clicks drift
        // after any resize. Keep GLFW's size matched.
        static Vector2 glfw_size{1440.0F, 900.0F};
        if (glfw_size.x != static_cast<float>(GetScreenWidth()) || glfw_size.y != static_cast<float>(GetScreenHeight())) {
            glfw_size = {static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())};
            SetWindowSize(GetScreenWidth(), GetScreenHeight());
        }
#endif

        // NewFrame works out the current mouse and capture state from the last layout,
        // before camera movement and editing use it.
        rlImGuiBegin();

        // The OS close button and Alt+F4 trigger WindowShouldClose(). Send that through the
        // same check as the Exit menu instead of quitting right away.
        if (WindowShouldClose()) {
            close_requested = true;
        }

        if (!io.WantCaptureKeyboard && action_pressed(ui, EditorAction::GraphicsDebug)) {
            ui.show_graphics_debug = !ui.show_graphics_debug;
        }
        // Tab switches asked for by the tab bar, shortcuts, or tool windows.
        if (ui.requested_tab.has_value()) {
            const auto next = *ui.requested_tab;
            ui.requested_tab.reset();
            if (next != ui.tab) {
                // Each mode keeps its own camera, so switching back returns to where
                // you were. Building and Block mode only reuse it for the same asset.
                const auto asset_of = [&](EditorTab tab) {
                    return tab == EditorTab::Buildings ? ui.selected_building : tab == EditorTab::Blocks ? ui.selected_block : std::size_t{0};
                };
                const auto old_slot = static_cast<std::size_t>(ui.tab);
                ui.tab_cameras[old_slot] = std::pair{camera, asset_of(ui.tab)};
                ui.tab = next;
                ui.gizmo_active = GizmoAxis::None;
                ui.box_select_active = false;
                ui.view_snap_active = false;
                ui.focus_animating = false;
                const auto& saved = ui.tab_cameras[static_cast<std::size_t>(next)];
                if (project && saved && saved->second == asset_of(next)) {
                    camera = saved->first;
                } else if (project) {
                    if (next == EditorTab::City) {
                        focus_camera_on_city(camera, *project);
                    } else {
                        camera = default_camera();
                        if (next == EditorTab::Buildings) focus_current_selection(*project, ui, camera);
                        if (next == EditorTab::Blocks) frame_block_camera(*project, ui, camera);
                    }
                }
            }
        }
        if (project) {
            project->thumbnail_budget = 2;
        }

        if (auto folder = dropped_folder()) {
            pending_load_folder = std::move(*folder);
        }
#ifdef __EMSCRIPTEN__
        if (g_web_uploaded_folder.has_value()) {
            if (pending_load_folder) release_web_project(*pending_load_folder);
            pending_load_folder = std::move(g_web_uploaded_folder);
            g_web_uploaded_folder.reset();
        }
#endif

        // Recent status and action messages, newest first, for the status bar.
        static std::deque<std::string> status_history;
        static Vector2 viewport_mouse_pos = {0, 0};
        static Vector2 viewport_origin = {0, 0};
        static bool is_viewport_hovered = false;
        const auto* scene_window = ImGui::FindWindowByName("##SceneCanvas");
        is_viewport_hovered = scene_window != nullptr && ImGui::GetCurrentContext()->HoveredWindow == scene_window;
        viewport_mouse_pos = get_viewport_mouse_position(viewport_origin, last_viewport_size);
        const bool show_3d_viewport = true;

        // Right button: held, it flies the camera; a quick click without moving
        // opens the context menu instead (drawn in the scene canvas below).
        static Vector2 right_click_at{};
        static double right_click_time = 0.0;
        static float right_click_moved = 0.0F;
        bool viewport_menu_requested = false;
        if (is_viewport_hovered && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) && ui.block_transform == BlockTransform::None) {
            right_click_at = viewport_mouse_pos;
            right_click_time = GetTime();
            right_click_moved = 0.0F;
            set_camera_perspective(camera);
            DisableCursor();
        }
        if (IsCursorHidden()) {
            right_click_moved += Vector2Length(GetMouseDelta());
            const auto fly = read_fly_input();
            if (fly.forward || fly.back || fly.left || fly.right || fly.up || fly.down) right_click_moved += 100.0F;
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_RIGHT) && IsCursorHidden()) {
            EnableCursor();
            viewport_menu_requested = right_click_moved < 6.0F && GetTime() - right_click_time < 0.4;
        }
        if (IsCursorHidden()) {
            ui.view_snap_active = false;
            // Flying by hand cancels any focus glide so the two never fight. While flying,
            // the wheel sets the flying speed; otherwise it zooms.
            ui.camera_fly_speed = adjust_camera_fly_speed(ui.camera_fly_speed, GetMouseWheelMove());
            ui.focus_animating = false;
            update_free_fly_camera(camera, ui.camera_fly_speed, read_fly_input());
        } else {
            const MapScene* pivot_scene = project && ui.tab == EditorTab::City ? &project->city_scene_cache : nullptr;
            update_orbit_camera(camera, ui, viewport_mouse_pos, last_viewport_size, is_viewport_hovered, pivot_scene);
        }
        if (project && ui.focus_selection_requested) {
            ui.focus_selection_requested = false;
            focus_current_selection(*project, ui, camera);
        }
        update_focus_animation(ui, camera);
        ui.view_camera_position = camera.position;
        ui.view_camera_target = camera.target;
        // Visual testing: PAFTA_CAMERA="px,py,pz,tx,ty,tz" fixes the camera in place.
        if (const char* pose = std::getenv("PAFTA_CAMERA"); pose && (screenshot_path || bench_frames > 0)) {
            float v[6]{};
            if (std::sscanf(pose, "%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
                camera.position = {v[0], v[1], v[2]};
                camera.target = {v[3], v[4], v[5]};
            }
        }
        // Visual testing: PAFTA_FLY="w,space,..." holds those fly keys (w a s d space ctrl
        // shift z) for the last PAFTA_FLY_FRAMES (60) screenshot frames and prints how far
        // the camera flew, starting from PAFTA_CAMERA if that is set.
        if (const char* keys = std::getenv("PAFTA_FLY"); keys && screenshot_path && project) {
            const int fly_frames = std::getenv("PAFTA_FLY_FRAMES") ? std::atoi(std::getenv("PAFTA_FLY_FRAMES")) : 60;
            static Vector3 flown{};
            static double fly_seconds = 0.0;
            if (screenshot_ready_frames >= screenshot_frame_target - fly_frames) {
                const std::string k = std::string(",") + keys + ",";
                const auto has = [&](const char* key) { return k.find(std::string(",") + key + ",") != std::string::npos; };
                const FlyInput input{has("w"), has("s"), has("a"), has("d"), has("space"), has("ctrl"), has("shift"), has("z")};
                camera.position = Vector3Add(camera.position, flown);
                camera.target = Vector3Add(camera.target, flown);
                const auto before = camera.position;
                update_free_fly_camera(camera, ui.camera_fly_speed, input);
                flown = Vector3Add(flown, Vector3Subtract(camera.position, before));
                fly_seconds += GetFrameTime();
                const auto f = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
                std::printf("fly: moved %.2f,%.2f,%.2f |%.2f| in %.3fs at speed %.1f forward %.3f,%.3f,%.3f\n",
                    flown.x, flown.y, flown.z, Vector3Length(flown), fly_seconds, ui.camera_fly_speed, f.x, f.y, f.z);
            }
        }

        if (project) {
            project->defer_heavy_rebuilds = IsMouseButtonDown(MOUSE_BUTTON_LEFT) || ui.gizmo_active != GizmoAxis::None;
commit_pending_edits(*project, ui);
        }
#ifdef __EMSCRIPTEN__
        {
            // ImGui rewrites imgui.ini in memory; copy it to disk every few seconds.
            static double last_layout_store = 0.0;
            if (GetTime() - last_layout_store > 5.0) {
                last_layout_store = GetTime();
                web_store_file("imgui.ini", "pafta.imgui_ini");
            }
        }
#endif
        // Editor hotkeys only run when ImGui is not using the keyboard (no active text field)
        // and the free camera is not flying, so they never clash with typing or WASD
        // movement.
        if (project && !io.WantCaptureKeyboard && !IsCursorHidden()) {
            handle_editor_hotkeys(*project, ui, camera, status, is_viewport_hovered);
        }
        const bool open_folder_hotkey_requested =
            !io.WantCaptureKeyboard && !IsCursorHidden() &&
            (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)
#ifdef __APPLE__
             || IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER)
#endif
            ) && IsKeyPressed(KEY_O);

        const SbcBuilding* outline_selected_building = nullptr;
        const SbcBuilding* outline_hovered_building = nullptr;
        std::optional<SbcBuilding> ghost_building;
        // Size in pixels of the 3D render target (the panel size times the render scale).
        const Vector2 render_size{static_cast<float>(viewport_texture.texture.width), static_cast<float>(viewport_texture.texture.height)};
        // When idle, a view that hasn't changed keeps its last frame instead of being
        // redrawn. Any input, background work, or map change since the last scene build
        // keeps it redrawing, and so does a short wait after them.
        {
            bool input = GetMouseDelta().x != 0.0F || GetMouseDelta().y != 0.0F || GetMouseWheelMove() != 0.0F ||
                IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) ||
                IsWindowResized();
            for (int key = KEY_SPACE; key <= KEY_KB_MENU && !input; ++key) input = IsKeyDown(key);
            bool working = project && (project->render_build_in_progress || !project->pending_gpu_upload_indices.empty() ||
                project->city_scene_build_in_progress || !project->city_scene_gpu_valid ||
                project->city_scene_current_fingerprint != project->city_scene_cache_fingerprint ||
                !project->building_near_jobs.empty() || project->portal_rebuild_future.has_value() || !ui.road_ghost.empty());
#ifndef __EMSCRIPTEN__
            working = working || load_future.has_value() || mesh_export_task.has_value();
#endif
            if (input || working || bench_frames > 0 || screenshot_path.has_value()) redraw_frames = 60;
#ifndef __EMSCRIPTEN__
            // Settled and nothing running: sleep until the next input event instead of
            // redrawing the interface at the frame limit.
            static bool waiting = false;
            if ((redraw_frames == 0) != waiting) {
                waiting = redraw_frames == 0;
                if (waiting) {
                    EnableEventWaiting();
                    release_free_memory();  // the periodic release does not run while asleep
                } else {
                    DisableEventWaiting();
                }
            }
#endif
        }
        const bool redraw_3d = redraw_frames > 0;
        if (redraw_frames > 0) --redraw_frames;
        if (redraw_3d && show_3d_viewport && last_viewport_size.x > 0 && last_viewport_size.y > 0) {
            set_game_world_time(ui.world_time_hours);
            update_environment_cubemap(project && project->terrain_gpu_valid ? &project->terrain_gpu_model.mesh : nullptr,
                project && project->terrain_gpu_valid ? project->terrain_gpu_fingerprint : 0U,
                camera.position, g_render_settings.live_reflections);
            begin_game_texture_mode(viewport_texture);
            ClearBackground(BLACK);
            clear_game_render_packed_depth();
            clear_game_render_shadow_factor();

            BeginMode3D(camera);
            set_game_world_time(ui.world_time_hours);
            set_game_surface_camera(camera.position);
            set_grid_view(camera, render_size.y);
            {
                GfxDebugScopedTimer draw_3d_timer{gfx_debug_stats, GfxDebugTimingPhase::Draw3D};
                if (project) {
                    {
                        GfxDebugScopedTimer timer{gfx_debug_stats, GfxDebugTimingPhase::SceneRenderBufferBuild};
service_render_rebuild(*project);
                    }
                    if (ui.tab == EditorTab::City && project->city) {
                        const auto* selected_building = project->city->data.buildings.empty()
                            ? nullptr
                            : &project->city->data.buildings[std::min(ui.selected_city_building, project->city->data.buildings.size() - 1U)];
                        std::optional<std::size_t> selected_street;
                        if (!project->city->data.streets.empty()) {
                            selected_street = std::min(ui.selected_city_street, project->city->data.streets.size() - 1U);
                        }
                        const auto scene_fingerprint = city_scene_fingerprint(project->city->data, project->city_building_revision);
                        project->city_scene_current_fingerprint = scene_fingerprint;
                        // Any change to a building, road, or piece changes the fingerprint, and a
                        // rebuild remakes the whole city's roads and terrain. Wait for the
                        // gesture to end and the map to stay still for a moment, so a drag or a
                        // quick series of nudges costs one rebuild instead of one per frame.
                        static std::uint64_t pending_fingerprint = 0;
                        static double pending_since = 0.0;
                        if (scene_fingerprint != pending_fingerprint) {
                            pending_fingerprint = scene_fingerprint;
                            pending_since = GetTime();
                        }
                        const bool scene_settled = !project->defer_heavy_rebuilds && GetTime() - pending_since > 0.2;
                        if ((!project->city_scene_cache_valid ||
                             (project->city_scene_cache_fingerprint != scene_fingerprint && scene_settled)) &&
                                   !project->city_scene_build_in_progress) {
                            // The map changed: rebuild the scene (roads, terrain, and
                            // footprint stamps) on a worker thread. The last good scene
                            // keeps drawing until the new one is ready.
start_city_scene_rebuild(*project, scene_fingerprint);
                        }

                        // Use a finished background rebuild if it still matches the
                        // current map. Otherwise throw it away as out of date and let
                        // the next frame start a fresh build.
                        if (project->city_scene_build_future.has_value() &&
                            project->city_scene_build_future->wait_for(std::chrono::milliseconds{0}) == std::future_status::ready) {
                            auto built = project->city_scene_build_future->get();
                            project->city_scene_build_future.reset();
                            project->city_scene_build_in_progress = false;
                            if (project->city_scene_build_target_fingerprint == scene_fingerprint) {
                                project->city_scene_cache = std::move(built);
                                ui.road_ghost.clear();
                                project->city_scene_cache_fingerprint = scene_fingerprint;
                                project->city_scene_cache_valid = true;
                                project->city_scene_gpu_valid = false;
                            }
                        }
                        if (project->city_scene_cache_valid && !project->city_scene_gpu_valid) {
                            // Upload again only what changed: the road and curb tiles
                            // when their cells differ (a building edit leaves them alone),
                            // and the heavy terrain mesh when its inputs
                            // (intersections, buildings, pieces) changed.
                            PAFTA_PROFILE_BLOCK("upload city scene gpu models");
                            if (project->city_tile_gpu_fingerprint != project->city_scene_cache.city_tile_fingerprint) {
                                unload_city_render_model(project->city_tile_gpu_model);
                                project->city_tile_gpu_model =
                                    upload_city_tile_model(project->city_scene_cache.city_tile_buffer, *project);
                                release_shadow_volume(project->city_scene_cache.city_tile_buffer);
                                release_free_memory();
                                project->city_tile_gpu_fingerprint = project->city_scene_cache.city_tile_fingerprint;
                            }

                            const auto terrain_fp = city_terrain_fingerprint(project->city->data, project->city_building_revision);
                            if (!project->terrain_gpu_valid || terrain_fp != project->terrain_gpu_fingerprint) {
                                unload_city_render_model(project->terrain_gpu_model);
                                if (project->city_scene_cache.terrain.has_value()) {
                                    project->terrain_gpu_model =
                                        upload_city_terrain_model(project->city_scene_cache.terrain->mesh, *project);
                                }
                                project->terrain_gpu_fingerprint = terrain_fp;
                                project->terrain_gpu_valid = true;
                                invalidate_environment_cubemap();
                            }
                            project->city_scene_gpu_valid = true;
                        }
                        const auto& city_scene = project->city_scene_cache;
                        const bool game_preview = ui.render_roads;
                        draw_city_scene(*project, city_scene, selected_street, game_preview, ui.render_terrain);
                        if (!game_preview) {
                            draw_city_editor_overlay(*project, city_scene);
                        }
                        // One hover for everything: the Select tool (or Erase)
                        // highlights whatever is under the cursor (building, road,
                        // node, or piece), so you know what a click will do. The
                        // closest one wins.
                        const SbcBuilding* hovered_building = nullptr;
                        ui.hover_city_intersection.reset();
                        ui.hover_city_street.reset();
                        ui.hover_piece_voxel.reset();
                        const bool unified_hover = (ui.city_tool == CityTool::Select || ui.city_tool == CityTool::Erase);
                        if (unified_hover && is_viewport_hovered && !IsCursorHidden() && ui.gizmo_active == GizmoAxis::None) {
                            const auto pick_ray = get_viewport_mouse_ray(camera, viewport_mouse_pos, last_viewport_size);
                            const Vec3 ro{pick_ray.position.x, pick_ray.position.y, pick_ray.position.z};
                            const Vec3 rd{pick_ray.direction.x, pick_ray.direction.y, pick_ray.direction.z};
                            // The pick tests every building triangle along the ray, so
                            // reuse last frame's answer while the ray and map are the same.
                            static Ray last_ray{};
                            static std::uint64_t last_map = 0;
                            static CityAnyPick last_pick{};
                            const auto map_key = scene_fingerprint;
                            if (!(Vector3Equals(pick_ray.position, last_ray.position) && Vector3Equals(pick_ray.direction, last_ray.direction) &&
                                  map_key == last_map)) {
                                last_pick = pick_city_any(*project, city_scene, ro, rd);
                                last_ray = pick_ray; last_map = map_key;
                            }
                            const auto hov = last_pick;
                            if (hov.kind == CitySelectionKind::Building && hov.index < project->city->data.buildings.size()) {
                                hovered_building = &project->city->data.buildings[hov.index];
                            } else if (hov.kind == CitySelectionKind::Road) {
                                ui.hover_city_street = hov.index;
                            } else if (hov.kind == CitySelectionKind::Node) {
                                ui.hover_city_intersection = hov.index;
                            } else if (hov.kind == CitySelectionKind::Piece) {
                                // Find the single world voxel of the piece under the mouse.
                                if (const auto p = pick_city_piece(project->city->data, ro, rd, city_scene.unit_size);
                                    p.has_value() && p->portal_index < project->city->data.portals.size()) {
                                    const auto& portal = project->city->data.portals[p->portal_index];
                                    ui.hover_piece_voxel = CityGridPoint{
                                        sbc_portal_cell_base(portal.fields[1]) + static_cast<std::int32_t>(p->block_index & 7U),
                                        sbc_portal_cell_base(portal.fields[2]) + static_cast<std::int32_t>((p->block_index / 0x40U) & 7U),
                                        sbc_portal_cell_base(portal.fields[3]) + static_cast<std::int32_t>((p->block_index / 8U) & 7U),
                                    };
                                }
                            }
                        }
                        outline_selected_building =
                            (ui.city_selection == CitySelectionKind::Building) ? selected_building : nullptr;
                        outline_hovered_building = hovered_building;
                        {
                            GfxDebugScopedTimer timer{gfx_debug_stats, GfxDebugTimingPhase::BuildingDraw};
                            draw_city_building_models_full_detail(*project, camera, last_viewport_size);
                        }
                        {
                            GfxDebugScopedTimer timer{gfx_debug_stats, GfxDebugTimingPhase::PieceDraw};
                            const auto selected_portal = ui.city_selection == CitySelectionKind::Piece
                                ? std::optional<std::size_t>{ui.selected_portal}
                                : std::nullopt;
                            draw_city_piece_voxels(*project, ui.portal_preview_textured, selected_portal);
                        }
                        GfxDebugScopedTimer timer{gfx_debug_stats, GfxDebugTimingPhase::PoleDraw};
                        if (g_render_settings.street_objects) draw_city_traffic_light_poles(*project, city_scene);
                        if (ui.render_shadows) {
                            draw_city_shadow_volumes(*project, camera, ui.building_distance > 0.0F
                                ? std::min(ui.shadow_distance, ui.building_distance) : ui.shadow_distance);
                        }

                        // Road and Building previews snap to the grid point on the
                        // closest visible surface. The Piece preview finds its own cell
                        // below, and Select and Erase use the hover pick instead.
                        const auto needs_city_cursor = ui.city_tool == CityTool::Road || ui.city_tool == CityTool::Building;
                        const std::optional<CityGridPoint> city_cursor = needs_city_cursor
                            ? raised(city_surface_point_under_mouse(*project, camera, viewport_mouse_pos, last_viewport_size),
                                ui.place_height_offset)
                            : std::nullopt;
                        // Glow for selected or hovered nodes (roads get the outline instead).

                        // Placement previews: the road node, the building's footprint
                        // (its outline is drawn below), and the piece itself, all
                        // worked out exactly as the click will place them. Erase uses
                        // the hover highlight above.
                        if (city_cursor.has_value() && ui.city_tool == CityTool::Road) {
                            draw_city_cursor(*city_cursor, city_scene.unit_size, ui.city_tool);
                            draw_city_edit_grid(*city_cursor, city_scene, Color{82, 196, 130, 255}, false);
                        }
                        ui.placement_conflict.clear();
                        if (city_cursor.has_value() && ui.city_tool == CityTool::Building && !project->buildings.empty()) {
                            ghost_building = city_building_placement_at(*project, ui, *city_cursor);
                            ui.placement_conflict = city_building_placement_conflict(*project, *ghost_building);
                            const auto& tmpl = project->buildings[std::min(ui.place_building_template, project->buildings.size() - 1U)];
                            DrawBoundingBox(city_building_bounds(*ghost_building, tmpl.data, city_scene.unit_size),
                                ui.placement_conflict.empty() ? Color{235, 190, 78, 230} : Color{230, 70, 60, 240});
                            draw_city_edit_grid(*city_cursor, city_scene, Color{235, 190, 78, 255}, false);
                        }
                        if (ui.city_tool == CityTool::Piece) {
                            if (const auto cell = raised(city_surface_cell_under_mouse(*project, camera, viewport_mouse_pos, last_viewport_size),
                                    ui.place_height_offset)) {
                                const auto* mesh = project->blocks.empty() || ui.place_piece_solid
                                    ? nullptr
                                    : &project->blocks[std::min(ui.place_piece_mesh, project->blocks.size() - 1U)].data;
                                draw_piece_ghost(mesh, *cell, static_cast<std::int32_t>(ui.selected_piece_rotation & 3U));
                                draw_city_edit_grid(*cell, city_scene, Color{91, 166, 206, 255}, true);
                            }
                        }
                        ui.road_preview_info = draw_road_lay_preview(*project, ui, city_cursor);
                        draw_road_tiles(ui.road_ghost, city_scene.unit_size, Color{220, 220, 225, 120}, Color{220, 220, 225, 60}, Color{235, 235, 240, 200});

                    } else if (ui.tab == EditorTab::Buildings && !project->buildings.empty()) {
                        ui.selected_building = std::min(ui.selected_building, project->buildings.size() - 1U);
                        const auto occupied = occupied_cell_indices(project->buildings[ui.selected_building].data);
                        const auto* selected = occupied.empty()
                            ? nullptr
                            : &project->buildings[ui.selected_building].data.cells[occupied[std::min(ui.selected_building_cell, occupied.size() - 1U)]];
                        if (ui.selected_building < project->buildings.size()) {
                            // The whole building (nullopt) or one chosen floor.
                            const std::optional<std::uint32_t> preview_floor = ui.building_view_floor_only
                                ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(std::max(0, ui.building_view_floor)))
                                : std::nullopt;
                            // The cell under the cursor, for the hover outline.
                            ui.building_hover_cell.reset();
                            ui.building_hover_face.reset();
                            if (is_viewport_hovered && !IsCursorHidden()) {
                                // The face that is really under the cursor, from the edit view's mesh.
                                const auto pr = get_viewport_mouse_ray(camera, viewport_mouse_pos, last_viewport_size);
                                const Vec3 ro{pr.position.x, pr.position.y, pr.position.z};
                                const Vec3 rd{pr.direction.x, pr.direction.y, pr.direction.z};
                                if (const auto pick = pick_building_face(project->building_floor_preview_render_buffer, ro, rd)) {
                                    ui.building_hover_cell = pick->cell_index;
                                    ui.building_hover_face = pick->slot;
                                }
                            }
                            auto& preview_model = building_tool_floor_preview_model(*project, ui.selected_building, preview_floor,
                                !ui.building_hide_roofs);
                            GfxDebugScopedTimer timer{gfx_debug_stats, GfxDebugTimingPhase::BuildingDraw};
                            draw_sbb_scene_for_app(
                                project->buildings[ui.selected_building].data,
                                preview_model,
                                selected,
                                static_cast<std::uint32_t>(std::max(0, ui.building_view_floor)));
                            draw_building_item_set_guides(*project, project->buildings[ui.selected_building].data, preview_floor);
                            if (ui.building_tool == BuildingTool::PaintTexture &&
                                ui.building_hover_cell.has_value() && ui.building_hover_face.has_value()) {
                                draw_building_face_highlight(project->building_floor_preview_render_buffer,
                                    *ui.building_hover_cell, *ui.building_hover_face);
                            }
                        }
                    } else if (ui.tab == EditorTab::Pieces && project->city && !project->city->data.city_blocks.empty()) {
                        if (!project->city->data.portals.empty()) {
                            ui.selected_portal = std::min(ui.selected_portal, project->city->data.portals.size() - 1U);
                            draw_city_piece_voxels(*project, ui.portal_preview_textured, ui.selected_portal, true, true);
                        } else {
                            ui.selected_city_block = std::min(ui.selected_city_block, project->city->data.city_blocks.size() - 1U);
                            const auto& city_block = project->city->data.city_blocks[ui.selected_city_block];
                            if (!city_block.layers.empty()) {
                                ui.selected_city_block_layer = std::min(ui.selected_city_block_layer, city_block.layers.size() - 1U);
                                ui.selected_city_block_slot = std::min<std::size_t>(ui.selected_city_block_slot, 7U);
                            }
                            draw_city_block_preview(*project, city_block, block_meshes(*project), ui.selected_city_block, ui.selected_city_block_layer, ui.selected_city_block_slot);
                        }
                    } else if (ui.tab == EditorTab::Buildblocks && !project->buildblocks.empty()) {
                        ui.selected_buildblock = std::min(ui.selected_buildblock, project->buildblocks.size() - 1U);
                        draw_buildblock_preview_3d(buildblock_preview_scene(*project, ui.selected_buildblock));
                    } else if (ui.tab == EditorTab::Blocks && !project->blocks.empty()) {
                        draw_block_editor_view(*project, ui);
                    } else {
                        DrawGrid(64, 4.0F);
                    }
                } else {
                    DrawGrid(64, 4.0F);
                }
            }
            EndMode3D();
            if (ui.render_shadows && ui.tab == EditorTab::City && project && project->city) {
                composite_city_shadows(render_size);
            }
            if (ui.render_sky) {
                draw_game_sky(camera, render_size, game_render_packed_depth(viewport_texture));
            }
            end_game_texture_mode();

            // General outline: draw a shape into the mask, then trace a
            // coloured outline around it. Buildings, roads, and building cells
            // all use this, so hover and selection look the same everywhere.
            auto composite_outline = [&](const std::function<void()>& draw_mask, Color tint) {
                GfxDebugScopedTimer timer{gfx_debug_stats, GfxDebugTimingPhase::OutlineDraw};
                begin_game_texture_mode(building_outline_mask_texture);
                ClearBackground(BLANK);
                BeginMode3D(camera);
                draw_mask();
                EndMode3D();
                end_game_texture_mode();

                begin_game_texture_mode(viewport_texture);
                draw_outline_from_mask(building_outline_mask_texture, render_size, tint);
                end_game_texture_mode();
            };

            const Color select_tint{220, 32, 32, 255};
            // Erase shows what it will delete in red, not the normal hover blue.
            const bool erase_hover = ui.tab == EditorTab::City && ui.city_tool == CityTool::Erase;
            const Color hover_tint = erase_hover ? Color{255, 80, 70, 255} : Color{120, 200, 255, 255};

            if (project && ui.tab == EditorTab::City && project->city) {
                // Outline every selected building (the whole multi-selection).
                if (ui.city_selection == CitySelectionKind::Building) {
                    if (ui.selected_city_buildings.size() > 1U) {
                        // One mask pass for the whole multi-selection, then one
                        // outline pass, instead of a full mask and outline for every
                        // selected building.
                        composite_outline([&] {
                            for (const auto idx : ui.selected_city_buildings) {
                                if (idx < project->city->data.buildings.size()) {
                                    draw_city_building_mask_for_placement(*project, &project->city->data.buildings[idx]);
                                }
                            }
                        }, select_tint);
                    } else if (outline_selected_building != nullptr) {
                        composite_outline([&] { draw_city_building_mask_for_placement(*project, outline_selected_building); }, select_tint);
                    }
                }
                if (outline_hovered_building != nullptr && outline_hovered_building != outline_selected_building) {
                    composite_outline([&] { draw_city_building_mask_for_placement(*project, outline_hovered_building); }, hover_tint);
                }
                if (ghost_building.has_value()) {
                    const auto asset = std::min(ui.place_building_template, project->buildings.size() - 1U);
                    if (asset < project->building_gpu_models.size()) {
                        composite_outline([&] { draw_city_building_mask(project->building_gpu_models[asset], *ghost_building); },
                            ui.placement_conflict.empty() ? Color{235, 190, 78, 255} : Color{230, 70, 60, 255});
                    }
                }
                // Road outline: the selected road (if a road is selected) and the
                // hover, with the same outline look as buildings.
                if (ui.city_selection == CitySelectionKind::Road && !project->city->data.streets.empty()) {
                    const auto sel = std::min(ui.selected_city_street, project->city->data.streets.size() - 1U);
                    composite_outline([&] { draw_city_road_mask(*project, project->city_scene_cache, sel); }, select_tint);
                }
                if (ui.hover_city_street.has_value()) {
                    const auto h = *ui.hover_city_street;
                    composite_outline([&] { draw_city_road_mask(*project, project->city_scene_cache, h); }, hover_tint);
                }
                // Junctions and single piece voxels: the same outline, traced
                // from their real tiles and faces.
                const auto& map_intersections = project->city->data.intersections;
                if (ui.city_selection == CitySelectionKind::Node && !map_intersections.empty()) {
                    const auto sel = std::min(ui.selected_city_intersection, map_intersections.size() - 1U);
                    composite_outline([&] { draw_city_intersection_mask(project->city_scene_cache, sel); }, select_tint);
                }
                if (ui.hover_city_intersection.has_value() &&
                    !(ui.city_selection == CitySelectionKind::Node && *ui.hover_city_intersection == ui.selected_city_intersection)) {
                    const auto h = *ui.hover_city_intersection;
                    composite_outline([&] { draw_city_intersection_mask(project->city_scene_cache, h); }, hover_tint);
                }
                if (ui.city_selection == CitySelectionKind::Piece && ui.selected_piece_voxel.has_value()) {
                    const auto cell = *ui.selected_piece_voxel;
                    composite_outline([&] { draw_city_piece_mask(*project, cell); }, select_tint);
                }
                if (ui.hover_piece_voxel.has_value() &&
                    !(ui.city_selection == CitySelectionKind::Piece && ui.selected_piece_voxel == ui.hover_piece_voxel)) {
                    const auto cell = *ui.hover_piece_voxel;
                    composite_outline([&] { draw_city_piece_mask(*project, cell); }, hover_tint);
                }
            } else if (project && ui.tab == EditorTab::Buildings && !project->buildings.empty()) {
                auto& building = project->buildings[std::min(ui.selected_building, project->buildings.size() - 1U)].data;
                const auto occupied = occupied_cell_indices(building);
                if (!occupied.empty()) {
                    const auto& sel_cell = building.cells[occupied[std::min(ui.selected_building_cell, occupied.size() - 1U)]];
                    composite_outline([&] { draw_building_cell_mask(project->building_floor_preview_render_buffer, building, sel_cell); }, select_tint);
                }
                // Paint highlights the face it will paint instead of the whole block.
                if (ui.building_tool != BuildingTool::PaintTexture &&
                    ui.building_hover_cell.has_value() && *ui.building_hover_cell < building.cells.size() &&
                    (occupied.empty() || building.cells[*ui.building_hover_cell].index != building.cells[occupied[std::min(ui.selected_building_cell, occupied.size() - 1U)]].index)) {
                    const auto& hov = building.cells[*ui.building_hover_cell];
                    composite_outline([&] { draw_building_cell_mask(project->building_floor_preview_render_buffer, building, hov); }, hover_tint);
                }
            }
        }

        if (project) {
            block_editor_update(*project, ui, camera, viewport_mouse_pos, last_viewport_size, is_viewport_hovered && !IsCursorHidden());
        }
        if (show_3d_viewport && project && is_viewport_hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !IsCursorHidden() &&
            ui.camera_drag == CameraDrag::None) {
            if (ui.tab == EditorTab::Blocks && !project->blocks.empty()) {
                block_editor_press(*project, ui, camera, viewport_mouse_pos, last_viewport_size);
            }
            if (ui.tab == EditorTab::City && project->city) {
                // Name the undo step. Select only changes things when it grabs a
                // gizmo (handled where the gizmo drag starts), so it is left out here.
                switch (ui.city_tool) {
                    case CityTool::Piece: begin_city_action(*project, ui, "place piece"); break;
                    case CityTool::Erase: begin_city_action(*project, ui, "erase"); break;
                    case CityTool::Building:
                    case CityTool::Road:
                    case CityTool::Select: break;
                }
                // The placement preview and clicks use the same surface hit.
                const auto city_cursor = city_surface_point_under_mouse(*project, camera, viewport_mouse_pos, last_viewport_size);
                const auto pick_ray = get_viewport_mouse_ray(camera, viewport_mouse_pos, last_viewport_size);
                const Vec3 ray_origin{pick_ray.position.x, pick_ray.position.y, pick_ray.position.z};
                const Vec3 ray_dir{pick_ray.direction.x, pick_ray.direction.y, pick_ray.direction.z};

                if (ui.city_tool == CityTool::Piece && project->city->data.version < 11 && !ui.confirm_piece_upgrade &&
                    !project->city_promotion_failed) {
                    // Converting old district cells changes what the game draws,
                    // so that upgrade waits for the user (draw_city_upgrade_dialogs).
                    if (has_legacy_district_cells(project->city->data)) {
                        ui.confirm_piece_upgrade = true;
                    } else {
                        const auto from = project->city->data.version;
                        try {
                            promote_city(*project);
                            ui.city_dirty = true;
                            project->city_promotion_failed = false;
                            ui.city_upgrade_notice = city_upgrade_notice(from);
                        }
                        catch (const std::exception& error) {
                            project->city_promotion_failed = true;
                            report_failure(status, std::string{"Piece placement blocked: "} + error.what());
                        }
                    }
                }
                if (ui.city_tool == CityTool::Select) {
                    // First check the selected building's gizmo handles (drag if one is
                    // hit); otherwise select whatever is under the cursor (closest wins).
                    GizmoAxis gizmo_hit = GizmoAxis::None;
                    if (ui.city_selection == CitySelectionKind::Building && !project->city->data.buildings.empty()) {
                        const auto sel = std::min(ui.selected_city_building, project->city->data.buildings.size() - 1U);
                        gizmo_hit = pick_building_gizmo(camera, viewport_mouse_pos, last_viewport_size, project->city->data.buildings[sel], true,
                            city_building_gizmo_offset(*project, project->city->data.buildings[sel]));
                    }
                    const auto clicked = pick_city_any(*project, project->city_scene_cache, ray_origin, ray_dir);
                    const bool double_click = clicked.kind == CitySelectionKind::Building &&
                        ui.last_clicked_building == clicked.index && GetTime() - ui.last_building_click_time < 0.3 &&
                        Vector2Distance(viewport_mouse_pos, ui.last_building_click_mouse) < 6;
                    ui.last_clicked_building = clicked.kind == CitySelectionKind::Building ? clicked.index : static_cast<std::size_t>(-1);
                    ui.last_building_click_time = GetTime();
                    ui.last_building_click_mouse = viewport_mouse_pos;
                    if (double_click) {
                        edit_city_building(*project, ui, clicked.index);
                        ui.last_clicked_building = static_cast<std::size_t>(-1);
                    } else if (gizmo_hit != GizmoAxis::None) {
                        begin_city_action(*project, ui, "move building");
                        const auto& placement = project->city->data.buildings[std::min(ui.selected_city_building, project->city->data.buildings.size() - 1U)];
                        ui.gizmo_active = gizmo_hit;
                        ui.gizmo_drag_mouse = viewport_mouse_pos;
                        ui.gizmo_drag_origin_x = placement.x;
                        ui.gizmo_drag_origin_y = placement.y;
                        ui.gizmo_drag_origin_z = placement.z;
                        ui.gizmo_drag_origin_rot = placement.rotation;
                        ui.gizmo_start_buildings.clear();
                        for (const auto index : ui.selected_city_buildings)
                            if (index < project->city->data.buildings.size())
                                ui.gizmo_start_buildings.emplace_back(index, project->city->data.buildings[index]);
                        if (std::none_of(ui.gizmo_start_buildings.begin(), ui.gizmo_start_buildings.end(), [&](const auto& saved) { return saved.first == ui.selected_city_building; }))
                            ui.gizmo_start_buildings.emplace_back(ui.selected_city_building, placement);
                    } else {
                        // Maybe start a box select. On release we decide: a tiny drag
                        // counts as a single click (pick what is under the cursor), and a
                        // real drag is a selection box. One gesture can do either.
                        ui.box_select_active = true;
                        ui.box_select_start = viewport_mouse_pos;
                    }
                } else if (ui.city_tool == CityTool::Building || ui.city_tool == CityTool::Road) {
                    // Placed on release (below), so you can drag away from a misclick.
                    ui.place_click_pending = true;
                    ui.place_click_start = viewport_mouse_pos;
                } else if (ui.city_tool == CityTool::Piece) {
                    // Starts the drag stroke, which stays on this cell's layer.
                    if (const auto cell = raised(city_surface_cell_under_mouse(*project, camera, viewport_mouse_pos, last_viewport_size),
                            ui.place_height_offset)) {
                        set_city_piece_voxel(*project, ui, *cell);
                        ui.drag_stamp_cell = *cell;
                    }
                } else if (ui.city_tool == CityTool::Erase) {
                    // Delete the highlighted thing. The drag pass only continues once
                    // the cursor reaches another cell.
                    erase_city_thing_under_cursor(*project, ui, project->city_scene_cache, ray_origin, ray_dir, status);
                    ui.drag_stamp_cell = city_cursor;
                }
            } else if (ui.tab == EditorTab::Pieces && project->city && !project->city->data.city_blocks.empty()) {
                const auto piece_cursor = city_block_slot_under_mouse(camera, viewport_mouse_pos, last_viewport_size);
                if (piece_cursor.has_value()) {
                    ui.selected_city_block_layer = std::min(ui.selected_city_block_layer, piece_cursor->first);
                    ui.selected_city_block_slot = piece_cursor->second;
                    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
                        nudge_city_block_slot(*project, ui, 1);
                    }
                }
            } else if (ui.tab == EditorTab::Buildings && !project->buildings.empty()) {
                const std::optional<std::uint32_t> pick_floor = ui.building_view_floor_only
                    ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(std::max(0, ui.building_view_floor)))
                    : std::nullopt;
                if (ui.building_tool == BuildingTool::Select) {
                    select_building_cell_from_viewport(*project, ui, camera, viewport_mouse_pos, last_viewport_size, pick_floor, status);
                } else if (ui.building_tool != BuildingTool::Anchor) {
                    // These tools use the face hit to find the cell to edit.
                    ui.selected_building = std::min(ui.selected_building, project->buildings.size() - 1U);
                    const auto& building = project->buildings[ui.selected_building].data;
                    const auto pick_ray = get_viewport_mouse_ray(camera, viewport_mouse_pos, last_viewport_size);
                    const Vec3 ro{pick_ray.position.x, pick_ray.position.y, pick_ray.position.z};
                    const Vec3 rd{pick_ray.direction.x, pick_ray.direction.y, pick_ray.direction.z};
                    constexpr float unit_size = 4.0F;
                    const auto pick = pick_building_face(project->building_floor_preview_render_buffer, ro, rd);
                    if (pick.has_value()) {
                        if (ui.building_tool == BuildingTool::PlaceBlock) {
                            ui.stroke_block_cells.clear();
                            if (const auto placed = place_building_block(*project, ui, *pick, rd, unit_size, status)) {
                                ui.stroke_block_cells.push_back(*placed);
                            }
                            // Mark this cell for the drag, so the hold-to-stamp pass below
                            // doesn't place a second block in the same neighbouring cell
                            // in this same frame.
                            ui.drag_stamp_cell = CityGridPoint{
                                static_cast<std::int32_t>(building.cells[pick->cell_index].x),
                                static_cast<std::int32_t>(building.cells[pick->cell_index].y),
                                static_cast<std::int32_t>(building.cells[pick->cell_index].z),
                            };
                        } else if (ui.building_tool == BuildingTool::ItemSet) {
                            place_building_item_set(*project, ui, pick->cell_index, status);
                        } else {
                            if (ui.paint_side_code) paint_building_side_code(*project, ui, pick->cell_index, pick->slot, status);
                            else paint_building_face(*project, ui, pick->cell_index, pick->slot, status);
                            ui.drag_paint_face = std::pair{pick->cell_index, pick->slot};
                        }
                    } else {
                        if (ui.building_tool == BuildingTool::PlaceBlock && std::abs(rd.y) > 0.0001F) {
                            auto& edited = project->buildings[ui.selected_building].data;
                            const auto floor = pick_floor.value_or(edited.anchor_y);
                            const float plane_y = (static_cast<float>(floor) - edited.anchor_y) * unit_size;
                            const float t = (plane_y - ro.y) / rd.y;
                            const auto x = static_cast<int>(std::floor((ro.x + rd.x * t) / unit_size)) + static_cast<int>(edited.anchor_x);
                            const auto z = static_cast<int>(std::floor((ro.z + rd.z * t) / unit_size)) + static_cast<int>(edited.anchor_z);
                            if (t >= 0 && x >= 0 && z >= 0 && (ui.block_config.exterior || ui.block_config.interior)) {
                                const auto problem = block_config_problem(edited, ui.block_config, *project);
                                if (x >= static_cast<int>(supported_sbb_grid_side) ||
                                    z >= static_cast<int>(supported_sbb_grid_side) || floor >= supported_sbb_grid_side) {
                                    status = "placement would exceed the supported 64-cell building grid";
                                } else if (!problem.empty()) {
                                    status = problem;
                                } else {
                                    auto* cell = building_cell_at(edited, x, floor, z);
                                    if (!cell) {
                                        SbbCell fresh{};
                                        fresh.index = edited.cells.size(); fresh.x = x; fresh.y = floor; fresh.z = z;
                                        fresh.shorts.fill(0xffffU);
                                        edited.cells.push_back(fresh); cell = &edited.cells.back();
                                    }
                                    apply_block_config(*cell, ui.block_config, edited, *project);
                                    ui.stroke_block_cells.assign(1, static_cast<std::size_t>(cell - edited.cells.data()));
                                    mark_building_changed(*project, ui);
                                }
                            }
                        } else if (ui.building_tool == BuildingTool::ItemSet) {
                            status = "Click a block to place or clear its item set.";
                        } else status = "Click a block face to paint.";
                    }
                }
            }
        }

        if (project && ui.tab == EditorTab::Buildings && !project->buildings.empty() && ui.building_tool == BuildingTool::Anchor) {
            update_anchor_gizmo(*project, ui, camera, viewport_mouse_pos, last_viewport_size, is_viewport_hovered && !IsCursorHidden());
        }

        // Gizmo drag: while the mouse is held after grabbing a handle, turn the
        // cursor into the building's new grid position. Letting go ends the drag.
        if (ui.gizmo_active != GizmoAxis::None) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && project && project->city) {
                apply_building_gizmo_drag(*project, ui, camera, viewport_mouse_pos, last_viewport_size);
            }
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                ui.gizmo_active = GizmoAxis::None;
            }
        }

        // Building and Road placement happen on release, and only if the cursor
        // stayed still. Dragging away from a misclick cancels it.
        if (ui.place_click_pending && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            ui.place_click_pending = false;
            if (show_3d_viewport && project && project->city && ui.tab == EditorTab::City && is_viewport_hovered &&
                !IsCursorHidden() && Vector2Distance(viewport_mouse_pos, ui.place_click_start) < 6.0F) {
                const auto city_cursor = raised(city_surface_point_under_mouse(*project, camera, viewport_mouse_pos, last_viewport_size),
                    ui.place_height_offset);
                if (ui.city_tool == CityTool::Building) {
                    // Place the chosen building template at the cursor.
                    if (city_cursor.has_value()) {
                        begin_city_action(*project, ui, "place building");
                        place_city_building_from_template(*project, ui, *city_cursor);
                    }
                } else if (ui.city_tool == CityTool::Road) {
                    // Carry on from an existing node when the ray hits one, otherwise
                    // lay a segment to the cursor's cell.
                    const auto pick_ray = get_viewport_mouse_ray(camera, viewport_mouse_pos, last_viewport_size);
                    const Vec3 ray_origin{pick_ray.position.x, pick_ray.position.y, pick_ray.position.z};
                    const Vec3 ray_dir{pick_ray.direction.x, pick_ray.direction.y, pick_ray.direction.z};
                    begin_city_action(*project, ui, "lay road");
                    const auto picked_node = pick_city_intersection(project->city->data, project->city_scene_cache, ray_origin, ray_dir);
                    const auto streets_before = project->city->data.streets.size();
                    if (picked_node.has_value()) {
                        const auto& node = project->city->data.intersections[*picked_node];
                        append_city_road_point(*project, ui, {node.x, node.y, node.z});
                    } else if (city_cursor.has_value()) {
                        append_city_road_point(*project, ui, *city_cursor);
                    }
                    // Show the new road until the rebuilt scene draws it.
                    if (project->city->data.streets.size() != streets_before) {
                        ui.road_ghost.insert(ui.road_ghost.end(), ui.road_preview_cells.begin(), ui.road_preview_cells.end());
                    }
                }
            }
        }

        // Box select on release: was it a click or a box? A tiny drag is a single
        // click (pick what is under the cursor); a real drag selects everything whose
        // screen point is inside the rectangle.
        if (ui.box_select_active && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && project && project->city) {
            const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)
#ifdef __APPLE__
                || IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER)
#endif
                ;
            const auto dx = viewport_mouse_pos.x - ui.box_select_start.x;
            const auto dy = viewport_mouse_pos.y - ui.box_select_start.y;
            const bool is_drag = (dx * dx + dy * dy) > 36.0F; // > 6px = a box
            if (is_drag) {
                box_select_city(*project, ui, camera, ui.box_select_start, viewport_mouse_pos, last_viewport_size, shift || ctrl);
            } else {
                const auto pick_ray = get_viewport_mouse_ray(camera, viewport_mouse_pos, last_viewport_size);
                const Vec3 ro{pick_ray.position.x, pick_ray.position.y, pick_ray.position.z};
                const Vec3 rd{pick_ray.direction.x, pick_ray.direction.y, pick_ray.direction.z};
                const auto picked = pick_city_any(*project, project->city_scene_cache, ro, rd);
                select_city_thing(*project, ui, picked, shift || ctrl, ro, rd, status);
            }
            ui.box_select_active = false;
        }

        // Click and hold to keep placing with the placement tools: while the button
        // is held and the cursor moves into a new grid cell, place again. drag_stamp_cell
        // stops a held drag from placing in the same cell every frame, so dragging
        // quickly lays a row of pieces or blocks.
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            ui.drag_stamp_cell.reset();
            ui.stroke_block_cells.clear();
            ui.drag_paint_face.reset();
        }
        if (show_3d_viewport && project && is_viewport_hovered && !IsCursorHidden() &&
            IsMouseButtonDown(MOUSE_BUTTON_LEFT) && ui.gizmo_active == GizmoAxis::None && ui.camera_drag == CameraDrag::None) {
            if (ui.tab == EditorTab::City && project->city &&
                (ui.city_tool == CityTool::Piece || ui.city_tool == CityTool::Erase)) {
                const auto ray = get_viewport_mouse_ray(camera, viewport_mouse_pos, last_viewport_size);
                std::optional<CityGridPoint> cursor;
                if (ui.city_tool == CityTool::Erase) {
                    cursor = city_surface_point_under_mouse(*project, camera, viewport_mouse_pos, last_viewport_size);
                } else if (ui.drag_stamp_cell.has_value()) {
                    // A stroke stays on its first cell's layer, so dragging over the
                    // pieces it just placed doesn't stack new ones on top.
                    cursor = raised(city_surface_cell_under_mouse(*project, camera, viewport_mouse_pos, last_viewport_size),
                        ui.place_height_offset);
                    if (cursor.has_value() && cursor->y != ui.drag_stamp_cell->y) cursor.reset();
                }
                if (cursor.has_value() && (!ui.drag_stamp_cell.has_value() || !(*ui.drag_stamp_cell == *cursor))) {
                    if (ui.city_tool == CityTool::Erase) {
                        const Vec3 ro{ray.position.x, ray.position.y, ray.position.z};
                        const Vec3 rd{ray.direction.x, ray.direction.y, ray.direction.z};
                        erase_city_thing_under_cursor(*project, ui, project->city_scene_cache, ro, rd, status);
                    } else {
                        set_city_piece_voxel(*project, ui, *cursor);
                    }
                    ui.drag_stamp_cell = *cursor;
                }
            } else if (ui.tab == EditorTab::Buildings && !project->buildings.empty() &&
                       ui.building_tool == BuildingTool::PlaceBlock) {
                const std::optional<std::uint32_t> pick_floor = ui.building_view_floor_only
                    ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(std::max(0, ui.building_view_floor)))
                    : std::nullopt;
                ui.selected_building = std::min(ui.selected_building, project->buildings.size() - 1U);
                const auto& building = project->buildings[ui.selected_building].data;
                const auto pick_ray = get_viewport_mouse_ray(camera, viewport_mouse_pos, last_viewport_size);
                const Vec3 ro{pick_ray.position.x, pick_ray.position.y, pick_ray.position.z};
                const Vec3 rd{pick_ray.direction.x, pick_ray.direction.y, pick_ray.direction.z};
                constexpr float unit_size = 4.0F;
                (void)pick_floor;
                const auto pick = pick_building_face(project->building_floor_preview_render_buffer, ro, rd);
                if (pick.has_value()) {
                    const CityGridPoint cell{
                        static_cast<std::int32_t>(building.cells[pick->cell_index].x),
                        static_cast<std::int32_t>(building.cells[pick->cell_index].y),
                        static_cast<std::int32_t>(building.cells[pick->cell_index].z),
                    };
                    // Never build onto a block this drag placed. The cursor stays on it,
                    // so that would grow a column toward the camera every frame.
                    const bool placed_this_drag = std::ranges::find(ui.stroke_block_cells, pick->cell_index) != ui.stroke_block_cells.end();
                    if (!placed_this_drag && (!ui.drag_stamp_cell.has_value() || !(*ui.drag_stamp_cell == cell))) {
                        if (const auto placed = place_building_block(*project, ui, *pick, rd, unit_size, status)) {
                            ui.stroke_block_cells.push_back(*placed);
                        }
                        ui.drag_stamp_cell = cell;
                    }
                }
            } else if (ui.tab == EditorTab::Buildings && !project->buildings.empty() &&
                       ui.building_tool == BuildingTool::PaintTexture) {
                // Drag to paint every face the cursor passes over, once each.
                const std::optional<std::uint32_t> pick_floor = ui.building_view_floor_only
                    ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(std::max(0, ui.building_view_floor)))
                    : std::nullopt;
                ui.selected_building = std::min(ui.selected_building, project->buildings.size() - 1U);
                const auto& building = project->buildings[ui.selected_building].data;
                const auto pick_ray = get_viewport_mouse_ray(camera, viewport_mouse_pos, last_viewport_size);
                const Vec3 ro{pick_ray.position.x, pick_ray.position.y, pick_ray.position.z};
                const Vec3 rd{pick_ray.direction.x, pick_ray.direction.y, pick_ray.direction.z};
                (void)pick_floor;
                (void)building;
                if (const auto pick = pick_building_face(project->building_floor_preview_render_buffer, ro, rd)) {
                    const std::pair face{pick->cell_index, pick->slot};
                    if (ui.drag_paint_face != face) {
                        if (ui.paint_side_code) paint_building_side_code(*project, ui, pick->cell_index, pick->slot, status);
                        else paint_building_face(*project, ui, pick->cell_index, pick->slot, status);
                        ui.drag_paint_face = face;
                    }
                }
            }
        }

        BeginDrawing();
        ClearBackground(DARKGRAY);

        const auto imgui_begin = std::chrono::steady_clock::now();

        static bool show_csx_editor = false;
        bool open_folder_requested = open_folder_hotkey_requested;
        bool export_mesh_requested = false;
        bool run_export_after_popup = false;



        if (ImGui::BeginMainMenuBar()) {
            ImGui::TextDisabled("Pafta v" PAFTA_VERSION);
            if (ImGui::BeginMenu("File")) {
                // Made with the shortcut formatter, so macOS shows Cmd.
                if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN " Open map folder...", shortcut_label(Shortcut{KEY_O, true, false, false}).c_str())) open_folder_requested = true;
                // Which copy wins when a map has an asset both loose and in test.csx.
                if (ImGui::BeginMenu(ICON_FA_LAYER_GROUP " When loading, prefer")) {
                    bool changed = false;
                    if (ImGui::MenuItem("test.csx archive first", nullptr, !ui.loose_files_win)) { ui.loose_files_win = false; changed = true; }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Archive copies replace loose files with the same name (what the game does).");
                    if (ImGui::MenuItem("Loose folders first", nullptr, ui.loose_files_win)) { ui.loose_files_win = true; changed = true; }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Files in building/, block/, buildblock/, and texture/ replace\narchive copies with the same name.");
                    if (changed) save_preferences(ui);
                    if (project) {
                        ImGui::Separator();
                        const bool differs = project->loose_files_win != ui.loose_files_win;
                        if (ImGui::MenuItem(ICON_FA_ROTATE " Reopen this map", nullptr, false, differs)) pending_load_folder = project->root;
                        if (!differs && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            ImGui::SetTooltip("The open map already uses this choice.");
                        }
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK " Save map...", action_shortcut_label(ui, EditorAction::Save).c_str(), false, project.has_value())) save_active_tab(*project, ui, status);
                ImGui::MenuItem(ICON_FA_BOX_ARCHIVE " CSX archive...", nullptr, &show_csx_editor);
                {
                    // The building being edited, or else the selected city building's template.
                    std::optional<std::size_t> building;
                    if (project && !project->buildings.empty()) {
                        if (ui.tab == EditorTab::Buildings) {
                            building = std::min(ui.selected_building, project->buildings.size() - 1U);
                        } else if (project->city && ui.city_selection == CitySelectionKind::Building &&
                                   ui.selected_city_building < project->city->data.buildings.size()) {
                            if (const auto* asset = find_building_asset(*project, project->city->data.buildings[ui.selected_city_building].name)) {
                                building = static_cast<std::size_t>(asset - project->buildings.data());
                            }
                        }
                    }
                    const auto label = std::string{ICON_FA_BUILDING " Export building"} +
                        (building ? " " + project->buildings[*building].name : std::string{}) + " (.sbb)...";
                    if (ImGui::MenuItem(label.c_str(), nullptr, false, building.has_value())) export_building_sbb(*project, *building, status);
                    if (!building && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        ImGui::SetTooltip("Edit a building, or select one in the city, to export it");
                    }
                }
#ifndef __EMSCRIPTEN__
                // Mesh export and quitting are desktop only; in the browser you close the tab.
                if (ImGui::MenuItem(ICON_FA_FILE_EXPORT " Export mesh...", nullptr, false, project != std::nullopt)) {
                    export_mesh_requested = true;
                }
                ImGui::Separator();
#ifdef __APPLE__
                constexpr const char* kQuitLabel = "Cmd+Q";
#else
                constexpr const char* kQuitLabel = "Alt+F4";
#endif
                if (ImGui::MenuItem(ICON_FA_RIGHT_FROM_BRACKET " Exit", kQuitLabel)) {
                    close_requested = true;
                }
#endif
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Assets", project.has_value())) {
                const auto unique_name = [&](const char* folder, const char* stem, const char* extension, const auto& assets) {
                    std::string name = stem;
                    for (unsigned suffix = 1; std::filesystem::exists(project->root / folder / (name + extension)) ||
                        std::any_of(assets.begin(), assets.end(), [&](const auto& asset) { return asset.name == name; }); ++suffix)
                        name = std::string{stem} + "_" + std::to_string(suffix);
                    return name;
                };
#ifndef __EMSCRIPTEN__
                if (ImGui::MenuItem(ICON_FA_FILE_IMPORT " Import into map...")) {
                    try {
                        const auto paths = dialog_open_map_assets();
                        if (!paths.empty()) import_files_into_project(*project, ui, paths, status);
                    } catch (const std::exception& error) {
                        report_failure(status, std::string{"Import failed: "} + error.what());
                    }
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add .sbb buildings, .sbl blocks, and .png textures to this map.\nAssets with the same name are replaced.");
                ImGui::Separator();
#endif
                if (ImGui::MenuItem(ICON_FA_MAP " New city", nullptr, false, !project->city)) {
                    SbcMap map{}; map.version = 12;
                    project->city = LoadedAsset<SbcMap>{project->root / "city2.sbc", "city2", map, true, true};
                    project->city_baseline.reset();
                    project->city_bytes = rebuild_sbc(map);
                    ui.tab = EditorTab::City; ui.city_dirty = true;
                    camera = default_camera();
                }
                if (ImGui::MenuItem(ICON_FA_BUILDING " New building")) {
                    SbbBuilding building{}; building.version = 14; building.size_z = 1;
                    building.name = unique_name("building", "building", ".sbb", project->buildings);
                    building.materials = {"default"};
                    project->building_bytes.push_back(rebuild_sbb(building));
                    project->buildings.push_back({project->root / "building" / (building.name + ".sbb"), building.name, building, true, true});
                    ++project->city_building_revision;
                    ui.selected_building = project->buildings.size() - 1;
                    ui.tab = EditorTab::Buildings; ui.building_tool = BuildingTool::PlaceBlock;
                    ui.building_dirty = true;
                    project->building_floor_preview_dirty = true;
                    project->placement_assets_dirty = true;
                    invalidate_render_input_cache(*project);
                    // Only the new (empty) building needs a mesh; nothing else changed.
                    rebuild_single_building_render(*project, ui.selected_building);
                    camera = default_camera();
                }
                if (ImGui::MenuItem(ICON_FA_TABLE_CELLS " New buildblock")) {
                    SblTemplate model{}; model.version = 1;
                    model.layers.resize(16);
                    for (std::size_t i = 0; i < model.layers.size(); ++i) {
                        model.layers[i].index = i; model.layers[i].bounds = {0, 1, 0, 1};
                    }
                    const auto name = unique_name("buildblock", "buildblock", ".sbl", project->buildblocks);
                    project->buildblock_bytes.push_back(rebuild_sbl_buildblock(model));
                    project->buildblocks.push_back({project->root / "buildblock" / (name + ".sbl"), name, model, true, true});
                    ui.selected_buildblock = project->buildblocks.size() - 1;
                    ui.selected_buildblock_layer = 0; ui.show_buildblock_window = true;
                    mark_buildblock_changed(*project, ui);
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit", project.has_value())) {
                if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT " Undo", action_shortcut_label(ui, EditorAction::Undo).c_str(), false, can_undo(*project, ui, false))) undo_edit(*project, ui, status, false);
                if (ImGui::MenuItem(ICON_FA_ROTATE_RIGHT " Redo", action_shortcut_label(ui, EditorAction::Redo).c_str(), false, can_undo(*project, ui, true))) undo_edit(*project, ui, status, true);
                // City selection actions, matching their keys so people can find them.
                const bool city_edit = ui.tab == EditorTab::City && project->city;
                const bool building_selected = city_edit && ui.city_selection == CitySelectionKind::Building && !project->city->data.buildings.empty();
                const bool road_selected = city_edit && (ui.city_selection == CitySelectionKind::Road || ui.city_selection == CitySelectionKind::Node) &&
                    !project->city->data.streets.empty();
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_COPY " Duplicate", action_shortcut_label(ui, EditorAction::DuplicateSelection).c_str(), false, building_selected || road_selected)) {
                    begin_city_action(*project, ui, building_selected ? "duplicate building" : "duplicate road");
                    if (building_selected) duplicate_city_building(*project, ui); else duplicate_city_street(*project, ui);
                }
                if (ImGui::MenuItem(ICON_FA_TRASH " Delete", action_shortcut_label(ui, EditorAction::DeleteSelection).c_str(), false, building_selected || road_selected)) {
                    begin_city_action(*project, ui, building_selected ? "delete building" : "delete road");
                    if (building_selected) delete_city_building(*project, ui); else delete_city_street(*project, ui);
                }
                if (ImGui::MenuItem(ICON_FA_ARROWS_ROTATE " Rotate", action_shortcut_label(ui, EditorAction::RotateSelection).c_str(), false, building_selected)) {
                    begin_city_action(*project, ui, "rotate building");
                    nudge_city_building(*project, ui, 0, 0, 0, 1);
                }
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_OBJECT_GROUP " Select all buildings", nullptr, false, city_edit && !project->city->data.buildings.empty())) {
                    ui.city_tool = CityTool::Select;
                    ui.city_selection = CitySelectionKind::Building;
                    ui.selected_city_building = 0;
                    ui.selected_city_buildings.resize(project->city->data.buildings.size());
                    std::iota(ui.selected_city_buildings.begin(), ui.selected_city_buildings.end(), std::size_t{0});
                }
                if (ImGui::MenuItem(ICON_FA_XMARK " Deselect", nullptr, false, city_edit && ui.city_selection != CitySelectionKind::None)) {
                    ui.city_selection = CitySelectionKind::None;
                    ui.selected_city_buildings.clear();
                }
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_GEAR " Preferences...", action_shortcut_label(ui, EditorAction::WindowPreferences).c_str())) ui.show_preferences_window = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View", project.has_value())) {
                if (ImGui::MenuItem(ICON_FA_CITY " City mode", action_shortcut_label(ui, EditorAction::TabCity).c_str(), ui.tab == EditorTab::City)) ui.requested_tab = EditorTab::City;
                if (ImGui::MenuItem(ICON_FA_BUILDING " Building mode", action_shortcut_label(ui, EditorAction::TabBuildings).c_str(), ui.tab == EditorTab::Buildings, !project->buildings.empty())) ui.requested_tab = EditorTab::Buildings;
                if (ImGui::MenuItem(ICON_FA_CUBES " Block mode", nullptr, ui.tab == EditorTab::Blocks)) ui.requested_tab = EditorTab::Blocks;
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_CROSSHAIRS " Frame selection", action_shortcut_label(ui, EditorAction::FocusSelection).c_str())) focus_current_selection(*project, ui, camera);
                if (ImGui::MenuItem(ICON_FA_EXPAND " Whole city", action_shortcut_label(ui, EditorAction::FrameCity).c_str(), false, project->city.has_value())) focus_camera_on_city(camera, *project);
                if (ImGui::MenuItem(ICON_FA_LOCATION_CROSSHAIRS " Coordinates", nullptr, &ui.show_coordinates)) save_preferences(ui);
                if (draw_interior_mode(ui)) save_preferences(ui);
                ImGui::Separator();
                if (draw_world_time_slider(ui)) save_preferences(ui);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Windows")) {
                bool windows_changed = false;
                windows_changed |= ImGui::MenuItem(ICON_FA_TOOLBOX " Toolbar", nullptr, &ui.show_toolbar_window);
                windows_changed |= ImGui::MenuItem(ICON_FA_LIST " Outliner", action_shortcut_label(ui, EditorAction::WindowLibrary).c_str(), &ui.show_library_window);
                windows_changed |= ImGui::MenuItem(ICON_FA_SLIDERS " Properties", action_shortcut_label(ui, EditorAction::WindowProperties).c_str(), &ui.show_properties_window);
                windows_changed |= ImGui::MenuItem(ICON_FA_IMAGES " Assets", action_shortcut_label(ui, EditorAction::WindowTools).c_str(), &ui.show_asset_drawer);
                if (windows_changed) save_preferences(ui);
                ImGui::Separator();
                ImGui::MenuItem(ICON_FA_BUILDING " Building browser", action_shortcut_label(ui, EditorAction::WindowBuildingBrowser).c_str(), &ui.show_building_browser, project.has_value());
                ImGui::MenuItem(ICON_FA_CUBES " Block browser", action_shortcut_label(ui, EditorAction::WindowBlockBrowser).c_str(), &ui.show_block_browser, project.has_value());
                ImGui::MenuItem(ICON_FA_TABLE_CELLS " Buildblock", action_shortcut_label(ui, EditorAction::WindowBuildblock).c_str(), &ui.show_buildblock_window, project.has_value());
                ImGui::MenuItem(ICON_FA_CLOCK_ROTATE_LEFT " Changes since save", action_shortcut_label(ui, EditorAction::WindowChanges).c_str(), &ui.show_changes_window, project.has_value());
                ImGui::MenuItem(ICON_FA_MAP " Map image", nullptr, &ui.show_map_image_window, project.has_value());
                ImGui::MenuItem(ICON_FA_TRIANGLE_EXCLAMATION " Load problems", nullptr, &ui.show_load_problems_window, project.has_value());
                ImGui::Separator();
                // Preferences is under Edit. The graphics debug overlay is a developer
                // tool, so it stays on its key only (listed under Shortcuts).
                ImGui::MenuItem(ICON_FA_KEYBOARD " Keyboard shortcuts", action_shortcut_label(ui, EditorAction::WindowShortcuts).c_str(), &ui.show_shortcuts_window);
                ImGui::Separator();
                ImGui::TextDisabled("Drag a window's title or tab to move or dock it.");
                if (ImGui::MenuItem(ICON_FA_TABLE_COLUMNS " Reset window layout")) {
                    ui.show_asset_drawer = ui.show_properties_window = ui.show_toolbar_window = true;
                    ui.show_library_window = false;
                    ui.reset_layout = true;
                    save_preferences(ui);
                }
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }


        // Fill the thumbnail caches a few at a time each frame after loading, and show
        // a loading screen with progress until the render buffers and thumbnails are ready.
        if (project) {
            service_pregeneration(*project);
            draw_loading_overlay(*project, ui.tab == EditorTab::City);
        }
#ifndef __EMSCRIPTEN__
        if (load_future) {
            static constexpr const char* stages[] = {
                "Reading city map", "Reading buildings", "Reading blocks",
                "Reading buildblocks", "Reading archives", "Preparing textures"};
            const int stage = std::clamp(load_stage->load(), 0, 5);
            const auto* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x * 0.5F,
                vp->WorkPos.y + vp->WorkSize.y * 0.5F}, ImGuiCond_Always, {0.5F, 0.5F});
            if (ImGui::Begin("Loading map", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
                ImGui::TextUnformatted(stages[stage]);
                ImGui::ProgressBar(static_cast<float>(stage) / 6.0F, {320.0F, 0.0F});
            }
            ImGui::End();
        }
#endif

        // Status bar along the bottom. The latest message stays readable after its
        // 2.5 s pop-up fades, and hovering lists the recent ones. It is drawn before the
        // dockspace so the panels sit above it.
        if (ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, ImGui::GetFrameHeight(),
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar)) {
            if (ImGui::BeginMenuBar()) {
                ImGui::TextDisabled("%s", status_history.empty() ? "Ready" : status_history.front().c_str());
                if (!status_history.empty() && ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    for (const auto& message : status_history) ImGui::TextUnformatted(message.c_str());
                    ImGui::EndTooltip();
                }
                // Background work, on the right: the first job shown inline, and all
                // of them with their progress on hover.
                auto jobs = project ? background_jobs(*project, ui.tab == EditorTab::City) : std::vector<BackgroundJob>{};
#ifndef __EMSCRIPTEN__
                if (load_future) {
                    static constexpr const char* load_stages[] = {"Reading city map", "Reading buildings", "Reading blocks",
                        "Reading buildblocks", "Reading archives", "Preparing textures"};
                    const int stage = std::clamp(load_stage->load(), 0, 5);
                    jobs.insert(jobs.begin(), {std::string("Loading: ") + load_stages[stage], static_cast<float>(stage) / 6.0F});
                }
                if (mesh_export_task) {
                    jobs.push_back({"Exporting " + mesh_export_task->what,
                        static_cast<float>(mesh_export_task->steps_done->load()) /
                            static_cast<float>(std::max<std::size_t>(1U, mesh_export_task->steps_total))});
                }
#endif
                if (!jobs.empty()) {
                    static constexpr const char* spinner[] = {"|", "/", "-", "\\"};
                    const auto frame = static_cast<std::size_t>(GetTime() * 8.0) % 4U;
                    std::string line = std::string(spinner[frame]) + " " + jobs.front().label;
                    if (jobs.size() > 1U) line += "  (+" + std::to_string(jobs.size() - 1U) + " more)";
                    constexpr float bar_width = 90.0F;
                    const bool bar = jobs.front().fraction >= 0.0F;
                    const float width = ImGui::CalcTextSize(line.c_str()).x + (bar ? bar_width + ImGui::GetStyle().ItemSpacing.x : 0.0F);
                    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 24.0F, ImGui::GetWindowWidth() - width - 12.0F));
                    ImGui::BeginGroup();
                    ImGui::TextUnformatted(line.c_str());
                    if (bar) {
                        ImGui::SameLine();
                        ImGui::ProgressBar(jobs.front().fraction, ImVec2(bar_width, ImGui::GetTextLineHeight()), "");
                    }
                    ImGui::EndGroup();
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        for (const auto& job : jobs) {
                            ImGui::TextUnformatted(job.label.c_str());
                            if (job.fraction >= 0.0F) ImGui::ProgressBar(job.fraction, ImVec2(240.0F, 0.0F));
                        }
                        ImGui::EndTooltip();
                    }
                }
                ImGui::EndMenuBar();
            }
        }
        ImGui::End();
        {
            const auto* dock_viewport = ImGui::GetMainViewport();
            // A dockspace over the whole work area. The scene canvas sits in the central
            // node with no tab bar, so tool windows dock around it and the 3D view fills
            // whatever space is left. The default layout (first run, or Windows > Reset
            // layout) puts Properties and the Outliner on the right and Assets under the
            // view. After that the saved layout is kept as you arranged it; windows can be
            // dragged out, docked anywhere around the view, or closed.
            const ImGuiID dock_id = ImGui::DockSpaceOverViewport(ImGui::GetID("MainDockV3"), dock_viewport, ImGuiDockNodeFlags_None);
            static bool layout_checked = false;
            const ImGuiDockNode* dock_node = ImGui::DockBuilderGetNode(dock_id);
            const bool first_run = !layout_checked && (dock_node == nullptr || !dock_node->IsSplitNode());
            layout_checked = true;
            if (ui.reset_layout || first_run) {
                ImGui::DockBuilderRemoveNode(dock_id);
                ImGui::DockBuilderAddNode(dock_id, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dock_id, dock_viewport->WorkSize);
                ImGuiID center = dock_id;
                ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24F, nullptr, &center);
                const ImGuiID right_top = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.38F, nullptr, &right);
                const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.22F, nullptr, &center);
                if (auto* center_node = ImGui::DockBuilderGetNode(center)) {
                    center_node->LocalFlags |= ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDockingOverMe;
                }
                ImGui::DockBuilderDockWindow("##SceneCanvas", center);
                ImGui::DockBuilderDockWindow("###Outliner", right_top);
                ImGui::DockBuilderDockWindow("###Properties", right);
                ImGui::DockBuilderDockWindow("###Assets", bottom);
                // Small reference windows open as tabs next to Properties.
                ImGui::DockBuilderDockWindow("###Changes", right);
                ImGui::DockBuilderDockWindow("###Shortcuts", right);
                ImGui::DockBuilderDockWindow("###Preferences", right);
                ImGui::DockBuilderFinish(dock_id);
            }
        }
        if (show_3d_viewport) {
            constexpr ImGuiWindowFlags scene_flags = ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_NoScrollWithMouse |
                ImGuiWindowFlags_NoBringToFrontOnFocus |
                ImGuiWindowFlags_NoNavFocus;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
            ImGui::Begin("##SceneCanvas", nullptr, scene_flags);
            ImGui::PopStyleVar();
            {
                ImVec2 size = ImGui::GetContentRegionAvail();
                static double last_viewport_resize = 0.0;
                static float last_render_scale = 1.0F;
                // The 3D view is drawn at render_scale of the panel and stretched to
                // fill it. Mouse picking keeps using panel coordinates.
                const auto render_scale = std::clamp(ui.render_scale, 0.5F, 1.0F);
                if ((static_cast<int>(size.x) != static_cast<int>(last_viewport_size.x) ||
                     static_cast<int>(size.y) != static_cast<int>(last_viewport_size.y) ||
                     render_scale != last_render_scale) &&
                    size.x > 0 && size.y > 0 && GetTime() - last_viewport_resize >= 0.12) {
                    const int width = std::max(1, static_cast<int>(size.x * render_scale));
                    const int height = std::max(1, static_cast<int>(size.y * render_scale));
                    unload_game_render_texture(viewport_texture);
                    UnloadRenderTexture(building_outline_mask_texture);
                    viewport_texture = load_game_render_texture(width, height);
                    building_outline_mask_texture = LoadRenderTexture(width, height);
                    last_viewport_size = {static_cast<float>(static_cast<int>(size.x)), static_cast<float>(static_cast<int>(size.y))};
                    last_render_scale = render_scale;
                    last_viewport_resize = GetTime();
                    redraw_frames = 60;
                }

                if (redraw_3d) resolve_game_render_texture(viewport_texture);
                const auto* display_texture = game_render_display_texture(viewport_texture);
                // Handles go on top of the finished scene, including shadows,
                // atmosphere, and outlines. Tone mapping must never hide them.
                if (project && redraw_3d) {
                    const auto target = display_texture ? *display_texture : viewport_texture;
                    begin_game_texture_mode(target);
                    BeginMode3D(camera);
                    if (ui.tab == EditorTab::City && project->city && ui.city_selection == CitySelectionKind::Building &&
                        ui.selected_city_building < project->city->data.buildings.size()) {
                        const auto& placement = project->city->data.buildings[ui.selected_city_building];
                        // At the middle of the footprint, which rotation keeps still.
                        const auto offset = city_building_gizmo_offset(*project, placement);
                        const auto hovered = ui.gizmo_active != GizmoAxis::None ? ui.gizmo_active :
                            (is_viewport_hovered ? pick_building_gizmo(camera, viewport_mouse_pos, last_viewport_size, placement, true, offset) : GizmoAxis::None);
                        draw_building_gizmo(placement, hovered, camera, last_viewport_size, true, offset);
                    } else if (ui.tab == EditorTab::Blocks) {
                        draw_block_editor_handles(*project, ui, camera);
                    } else if (ui.tab == EditorTab::Buildings && !project->buildings.empty() &&
                               ui.building_tool == BuildingTool::Anchor) {
                        // Only the Anchor tool can drag it; in other tools it looked draggable but wasn't.
                        draw_building_gizmo(SbcBuilding{}, ui.anchor_axis, camera, last_viewport_size, false);
                    }
                    EndMode3D();
                    end_game_texture_mode();
                }
                {
                    const auto* shown = display_texture == nullptr ? &viewport_texture : display_texture;
                    // Stretched to fill the panel (render scale), and flipped like all render textures.
                    rlImGuiImageRect(&shown->texture, static_cast<int>(last_viewport_size.x), static_cast<int>(last_viewport_size.y),
                        Rectangle{0.0F, 0.0F, static_cast<float>(shown->texture.width), -static_cast<float>(shown->texture.height)});
                }
                is_viewport_hovered = ImGui::IsItemHovered();
                const ImVec2 image_min = ImGui::GetItemRectMin();
                const ImVec2 image_max = ImGui::GetItemRectMax();
                if (project && project->city && ui.tab == EditorTab::City && ImGui::BeginDragDropTarget()) {
                    if (const auto* payload = ImGui::AcceptDragDropPayload("MAP_BUILDING")) {
                        if (payload->DataSize == sizeof(std::size_t)) {
                            const auto index = *static_cast<const std::size_t*>(payload->Data);
                            const auto image_min = ImGui::GetItemRectMin();
                            const auto mouse = ImGui::GetMousePos();
                            const auto point = city_surface_point_under_mouse(*project, camera,
                                {mouse.x - image_min.x, mouse.y - image_min.y}, {size.x, size.y});
                            if (index < project->buildings.size() && point) {
                                begin_city_action(*project, ui, "drop building");
                                ui.place_building_template = index;
                                place_city_building_from_template(*project, ui, *point);
                                ui.city_tool = CityTool::Select;
                            }
                        }
                    }
                    ImGui::EndDragDropTarget();
                }

                ImVec2 min_pos = ImGui::GetWindowPos();
                min_pos.x += ImGui::GetWindowContentRegionMin().x;
                min_pos.y += ImGui::GetWindowContentRegionMin().y;

                viewport_origin = {min_pos.x, min_pos.y};
                viewport_mouse_pos = get_viewport_mouse_position(viewport_origin, last_viewport_size);

                // The box selection rectangle (drawn in screen space over the
                // image while a box drag is going on).
                if (ui.box_select_active && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    const ImVec2 p0{min_pos.x + ui.box_select_start.x, min_pos.y + ui.box_select_start.y};
                    const ImVec2 p1{min_pos.x + viewport_mouse_pos.x, min_pos.y + viewport_mouse_pos.y};
                    auto* dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(p0, p1, IM_COL32(120, 180, 255, 40));
                    dl->AddRect(p0, p1, IM_COL32(150, 200, 255, 220));
                }
                const auto overlay_bottom = draw_viewport_overlays(project ? &*project : nullptr, ui);
                draw_view_gizmo(camera, ui, is_viewport_hovered && !IsCursorHidden());
                if (project) {
                    draw_viewport_context_menu(*project, ui, camera, right_click_at, last_viewport_size, viewport_menu_requested, status);
                }
                if (project && ui.show_coordinates) {
                    draw_coordinate_readout(*project, ui, camera, image_min, image_max,
                        viewport_mouse_pos, last_viewport_size, is_viewport_hovered, overlay_bottom);
                }
            }
            ImGui::End();

        }
        draw_toolbar_window(project ? &*project : nullptr, ui, camera, status, open_folder_requested);
        draw_asset_drawer_window(project ? &*project : nullptr, ui, status);
        draw_outliner_window(project ? &*project : nullptr, ui);
        draw_properties_window(project ? &*project : nullptr, ui, status);

        if (project) {
            draw_building_browser_window(*project, ui);
            draw_block_browser_window(*project, ui);
            draw_buildblock_window(*project, ui, status);
            draw_changes_window(*project, ui, status);
            draw_map_image_window(*project, ui);
            if (ui.map_export_requested) {
                ui.map_export_requested = false;
                export_map_image(*project, ui, status);
            }
        }
        draw_shortcuts_window(ui);
        draw_preferences_window(ui);
        // Cleared after every window has drawn, so floating windows reset too.
        ui.reset_layout = false;

        if (project && !project->action_log.empty() &&
            (project->action_log.size() != notified_action_count ||
             project->action_log.back() != notified_action)) {
            notified_action_count = project->action_log.size();
            notified_action = project->action_log.back();
            notification = notified_action;
            notification_until = GetTime() + 2.5;
            status_history.push_front(notification);
        } else if (status != last_notification_status && !status.empty()) {
            notification = status;
            notification_until = GetTime() + 2.5;
            status_history.push_front(notification);
        }
        if (status_history.size() > 30U) status_history.resize(30U);
        last_notification_status = status;
        if (!notification.empty() && GetTime() < notification_until) {
            const auto* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x - 18.0F,
                vp->WorkPos.y + 42.0F}, ImGuiCond_Always, {1.0F, 0.0F});
            ImGui::SetNextWindowBgAlpha(0.88F);
            if (ImGui::Begin("##Action notification", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings)) {
                ImGui::TextUnformatted(notification.c_str());
            }
            ImGui::End();
        }

        if (project && project->city && ui.city_dirty && project->city->data.version < 12 && !project->city_promotion_failed &&
            !has_legacy_district_cells(project->city->data)) {
            // An upgrade right after an undo (the map equals the committed state) is not
            // a new edit; otherwise it joins the undo step of the edit in progress.
            const bool at_baseline = project->city_baseline == project->city->data;
            const auto from = project->city->data.version;
            try {
                promote_city(*project);
                if (at_baseline) project->city_baseline = project->city->data;
                ui.city_upgrade_notice = city_upgrade_notice(from);
            }
            catch (const std::exception& error) {
                project->city_promotion_failed = true;
                report_failure(status, std::string{"City upgrade failed; edits retained, saving blocked: "} + error.what());
            }
        }
        if (project) draw_city_upgrade_dialogs(*project, ui, status);
        draw_csx_editor(archive_workspace, show_csx_editor, project ? &*project : nullptr, ui, status);
        if (project) draw_load_problems_window(*project, ui, status);
        static std::string shown_error;
        if (!g_failure_message.empty() && !ImGui::IsPopupOpen("Operation failed")) {
            shown_error = std::move(g_failure_message);
            g_failure_message.clear();
            ImGui::OpenPopup("Operation failed");
        }
        // Auto-resize with wrapped text shrinks to a sliver, so give it a width.
        ImGui::SetNextWindowSizeConstraints(ImVec2(420.0F, 0.0F), ImVec2(720.0F, FLT_MAX));
        if (ImGui::BeginPopupModal("Operation failed", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 400.0F);
            ImGui::TextUnformatted(shown_error.c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::Button("OK")) { status.clear(); shown_error.clear(); ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }

        const auto imgui_end = std::chrono::steady_clock::now();
        gfx_debug_stats.add_phase_ms(
            GfxDebugTimingPhase::ImGuiUi,
            std::chrono::duration<double, std::milli>(imgui_end - imgui_begin).count());
        refresh_gfx_debug_counts(gfx_debug_stats, project.has_value() ? &*project : nullptr);
        gfx_debug_stats.counts.total_scene_triangles = gfx_debug_stats.total_draw().triangles;
        draw_graphics_debug_window(ui.show_graphics_debug, gfx_debug_stats);

        if (open_folder_requested) {
#ifdef __EMSCRIPTEN__
            EM_ASM({
                window.paftaBrowserOpenPicker();
            });
            status = "Choose a Sub Rosa map folder in the browser picker.";
#else
            try {
                if (const auto folder = dialog_pick_folder()) pending_load_folder = *folder;
            } catch (const std::exception& error) {
                report_failure(status, std::string{"Open map failed: "} + error.what());
            }
#endif
        }

        if (export_mesh_requested && project) {
            std::string reason;
            const auto default_path = default_export_path(*project, ui, reason);
            if (default_path.empty()) {
                report_failure(status, "Export failed: " + reason + ".");
            } else {
                open_export_popup = true;
            }
        }
#ifndef __EMSCRIPTEN__
        if (project) service_mesh_export(mesh_export_task, *project, status);
#endif
        // Where the export goes: the remembered folder plus the default name for this
        // view. The field can be edited by hand or set with Browse.
        static std::array<char, 1024> export_path_text{};
#ifndef __EMSCRIPTEN__
        static bool export_browse_requested = false;
#endif
        if (open_export_popup && project) {
            std::string reason;
            auto path = default_export_path(*project, ui, reason);
            if (!ui.mesh_export_folder.empty()) path = std::filesystem::path{ui.mesh_export_folder} / path.filename();
            const auto text = path.string();
            export_path_text.fill('\0');
            std::copy_n(text.data(), std::min(text.size(), export_path_text.size() - 1U), export_path_text.data());
        }
        if (open_export_popup) {
            ImGui::OpenPopup("Export Mesh");
            open_export_popup = false;
        }
        if (project && ImGui::BeginPopupModal("Export Mesh", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            // A title and options that fit the current view.
            const char* scope =
                ui.tab == EditorTab::City ? "City" :
                ui.tab == EditorTab::Buildings ? "Building" :
                ui.tab == EditorTab::Blocks ? "Block" : "Mesh";
            ImGui::Text("Export %s Mesh as glTF", scope);
            ImGui::Separator();
            ImGui::Spacing();

            if (ui.tab == EditorTab::City) {
                ImGui::TextUnformatted("Include:");
                ImGui::Checkbox("Buildings", &export_options.include_buildings);
                ImGui::Checkbox("Pieces", &export_options.include_pieces);
                ImGui::Checkbox("Terrain", &export_options.include_terrain);
                ImGui::Checkbox("Roads / curbs", &export_options.include_roads);
                ImGui::Checkbox("Window panes", &export_options.include_window_panes);
            } else if (ui.tab == EditorTab::Buildings) {
                ImGui::Checkbox("Window panes", &export_options.include_window_panes);
            } else {
                ImGui::TextDisabled("No options for this view.");
            }

            ImGui::Spacing();
            ImGui::SeparatorText("Save to");
            ImGui::SetNextItemWidth(420.0F);
            ImGui::InputText("##export_path", export_path_text.data(), export_path_text.size());
#ifndef __EMSCRIPTEN__
            ImGui::SameLine();
            // The native dialog runs after the popup has finished drawing.
            if (ImGui::Button("Browse...")) export_browse_requested = true;
#endif
            ImGui::TextDisabled("A .gltf file; its textures are written next to it.");
            ImGui::Separator();
            ImGui::BeginDisabled(export_path_text[0] == '\0');
            if (ImGui::Button("Export", ImVec2(120, 0))) {
                run_export_after_popup = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

#ifndef __EMSCRIPTEN__
        if (export_browse_requested) {
            export_browse_requested = false;
            try {
                if (const auto output = dialog_save_file(export_path_text.data(), "glTF", "gltf")) {
                    const auto text = output->string();
                    export_path_text.fill('\0');
                    std::copy_n(text.data(), std::min(text.size(), export_path_text.size() - 1U), export_path_text.data());
                }
            } catch (const std::exception& error) {
                report_failure(status, std::string{"Choosing the export file failed: "} + error.what());
            }
        }
#endif
        if (run_export_after_popup && project) {
#ifndef __EMSCRIPTEN__
            PAFTA_PROFILE_BLOCK("export mesh");
            std::filesystem::path output{export_path_text.data()};
            if (lower_extension(output) != ".gltf") output += ".gltf";
            try {
                if (!output.parent_path().empty() && !std::filesystem::is_directory(output.parent_path())) {
                    throw std::runtime_error("the folder " + output.parent_path().string() + " does not exist");
                }
                ui.mesh_export_folder = output.parent_path().string();
                save_preferences(ui);
                mesh_export_task = start_mesh_export(*project, ui, output, export_options, status);
            } catch (const std::exception& error) {
                report_failure(status, std::string{"Mesh export failed: "} + error.what());
            }
#else
            status = "Export is available in the desktop editor.";
#endif
        }

        // Deal with a waiting close request. No unsaved changes: quit now. Unsaved
        // changes: show the save prompt (once) and keep running until the user answers.
        if (close_requested && archive_workspace.dirty) {
            open_archive_close_prompt = true;
            close_requested = false;
        }
        if (open_archive_close_prompt) {
            ImGui::OpenPopup("Unsaved archive");
            open_archive_close_prompt = false;
        }
        if (ImGui::BeginPopupModal("Unsaved archive", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Save the archive workspace before exiting?");
            if (ImGui::Button("Save archive")) {
                try {
                    if (save_archive_workspace(archive_workspace)) {
                        close_requested = true;
                        ImGui::CloseCurrentPopup();
                    }
                } catch (const std::exception& error) {
                    report_failure(status, std::string{"Archive save failed: "} + error.what());
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard archive")) {
                archive_workspace.dirty = false;
                close_requested = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (close_requested) {
            close_requested = false;
            if (project_to_open) release_web_project(project_to_open->root);
            project_to_open.reset();
            open_project_approved = false;
            unsaved_prompt_for_open = false;
            if (project && has_unsaved_changes(ui)) {
                open_unsaved_prompt = true;
            } else {
                running = false;
            }
        }
        // Keep the current project until its replacement has loaded successfully
        // and the user has dealt with any edits, including ones made while loading.
        if (project_to_open && !open_project_approved && !ImGui::IsPopupOpen("Unsaved Changes")) {
            if (project && has_unsaved_changes(ui)) {
                unsaved_prompt_for_open = true;
                open_unsaved_prompt = true;
            } else {
                open_project_approved = true;
            }
        }
        if (open_unsaved_prompt) {
            ImGui::OpenPopup("Unsaved Changes");
            open_unsaved_prompt = false;
        }
        if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(unsaved_prompt_for_open
                ? "You have unsaved changes. Save before opening another map?"
                : "You have unsaved changes. Save before exiting?");
            ImGui::Spacing();
            if (ImGui::Button(unsaved_prompt_for_open ? "Save & Open" : "Save & Exit", ImVec2(120, 0))) {
                if (project) {
                    save_all_dirty(*project, ui, status);
                }
                if (!has_unsaved_changes(ui)) {
                    if (unsaved_prompt_for_open) open_project_approved = true;
                    else running = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button(unsaved_prompt_for_open ? "Discard & Open" : "Discard & Exit", ImVec2(120, 0))) {
                if (unsaved_prompt_for_open) open_project_approved = true;
                else running = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                if (unsaved_prompt_for_open && project_to_open) {
                    release_web_project(project_to_open->root);
                    project_to_open.reset();
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::Render();
        render_imgui_draw_data(ImGui::GetDrawData());
        EndDrawing();
        g_gfx_debug_stats = nullptr;
        // Background builds (scene, near detail, pieces) free big buffers at any
        // time, so hand that memory back to the OS every so often.
        if (static double last_release = 0.0; GetTime() - last_release > 10.0) {
            last_release = GetTime();
            release_free_memory();
        }

        if (bench_frames > 0 && project && project->city_scene_gpu_valid && !project->render_build_in_progress &&
            project->pending_gpu_upload_indices.empty()) {
            static int bench_warmup = 60;
            static int bench_count = 0;
            static double bench_ms = 0.0;
            static GfxDebugDrawCounter bench_draw{};
            static std::array<std::uint64_t, static_cast<std::size_t>(GfxDebugDrawCategory::Count)> bench_category_tris{};
            static std::array<double, static_cast<std::size_t>(GfxDebugTimingPhase::Count)> bench_phase_ms{};
            if (bench_warmup > 0) {
                --bench_warmup;
            } else {
                bench_ms += GetFrameTime() * 1000.0;
                for (std::size_t i = 0; i < bench_phase_ms.size(); ++i) bench_phase_ms[i] += gfx_debug_stats.phase_ms[i];
                const auto draw = gfx_debug_stats.total_draw();
                bench_draw.draw_calls += draw.draw_calls;
                bench_draw.triangles += draw.triangles;
                for (std::size_t i = 0; i < bench_category_tris.size(); ++i) bench_category_tris[i] += gfx_debug_stats.draw[i].triangles;
                if (++bench_count == bench_frames) {
                    std::printf("bench: %d frames, %.3f ms/frame, %.0f draws/frame, %.0f tris/frame\n", bench_count,
                        bench_ms / bench_count, static_cast<double>(bench_draw.draw_calls) / bench_count,
                        static_cast<double>(bench_draw.triangles) / bench_count);
                    for (std::size_t i = 0; i < bench_category_tris.size(); ++i) {
                        std::printf("  %s: %.0f tris\n", gfx_draw_category_name(static_cast<GfxDebugDrawCategory>(i)),
                            static_cast<double>(bench_category_tris[i]) / bench_count);
                    }
                    // Only the measured frames (the running averages include loading).
                    for (std::size_t i = 0; i < bench_phase_ms.size(); ++i) {
                        std::printf("  %s: %.3f ms\n", gfx_timing_phase_name(static_cast<GfxDebugTimingPhase>(i)), bench_phase_ms[i] / bench_count);
                    }
                    running = false;
                }
            }
        }
        // Save round-trip test: PAFTA_SAVE=<folder> saves the loaded map there and
        // quits. PAFTA_SAVE_NUDGE=1 first moves building 0 up and back down.
        if (const char* save_to = std::getenv("PAFTA_SAVE"); save_to && project && project->city) {
            if (std::getenv("PAFTA_SAVE_NUDGE") && !project->city->data.buildings.empty()) {
                ui.city_selection = CitySelectionKind::Building;
                ui.selected_city_building = 0;
                begin_city_action(*project, ui, "edit selection");
                nudge_city_building(*project, ui, 0, 1, 0, 0);
                begin_city_action(*project, ui, "edit selection");
                nudge_city_building(*project, ui, 0, -1, 0, 0);
            }
            save_map_to(*project, ui, save_to, status);
            std::printf("save: %s\n", status.empty() ? "ok" : status.c_str());
            running = false;
        }
        if (screenshot_path && project && ++screenshot_ready_frames >= screenshot_frame_target) {
            auto screenshot = LoadImageFromScreen();
            ExportImage(screenshot, screenshot_path->string().c_str());
            UnloadImage(screenshot);
            running = false;
        }

        // Parse the new map without changing the current project. On desktop,
        // workers do the CPU work; textures are uploaded here on the GL thread.
        if (pending_load_folder && !project_to_open
#ifndef __EMSCRIPTEN__
            && !load_future
#endif
        ) {
            PAFTA_PROFILE_BLOCK("load pending project");
            const auto folder = *pending_load_folder;
            pending_load_folder.reset();
            try {
#ifndef __EMSCRIPTEN__
                load_stage->store(0);
                load_future = std::async(std::launch::async, [folder, load_stage, loose_first = ui.loose_files_win] {
                    std::string error;
                    auto loaded = load_project_folder(folder, error, false, load_stage.get(), loose_first);
                    return std::make_pair(std::move(loaded), std::move(error));
                });
#else
                present_parsing_frame(folder);
                emscripten_sleep(0);
                std::string error;
                auto loaded = load_project_folder(folder, error, true, nullptr, ui.loose_files_win);
                if (loaded) {
                    project_to_open = std::move(loaded);
                    status = std::move(error);
                } else {
                    release_web_project(folder);
                    report_failure(status, std::move(error));
                }
#endif
            } catch (const std::exception& error) {
                release_web_project(folder);
                report_failure(status, "Open map failed: " + folder.string() + ": " + error.what());
            }
        }
#ifndef __EMSCRIPTEN__
        if (load_future && load_future->wait_for(std::chrono::milliseconds{0}) == std::future_status::ready) {
            try {
                auto [loaded, error] = load_future->get();
                load_future.reset();
                if (loaded) {
                    load_project_textures(*loaded);
                    project_to_open = std::move(loaded);
                    status = std::move(error);
                } else {
                    report_failure(status, std::move(error));
                }
            } catch (const std::exception& error) {
                load_future.reset();
                report_failure(status, std::string{"Open map failed: "} + error.what());
            }
        }
#endif
        // An export's geometry belongs to the current map, and its texture readbacks
        // must finish on that same map before it is replaced.
        if (project_to_open && open_project_approved
#ifndef __EMSCRIPTEN__
            && !mesh_export_task
#endif
        ) {
            if (project) {
                free_project_thumbnails(*project);
                release_web_project(project->root);
            }
            unload_preview_views(ui);
            project = std::move(project_to_open);
            project_to_open.reset();
            open_project_approved = false;
            ui.city_dirty = ui.building_dirty = ui.block_dirty = ui.buildblock_dirty = false;
            ui.city_selection = CitySelectionKind::None;
            ui.selected_city_buildings.clear();
            ui.selected_portals.clear();
            ui.selected_materials.clear();
            ui.selected_piece_voxel.reset();
            ui.pending_road_start.reset();
            ui.gizmo_active = GizmoAxis::None;
            ui.box_select_active = false;
            ui.place_click_pending = false;
            if (project) {
#ifdef __EMSCRIPTEN__
                EM_ASM({
                    const node = document.getElementById('status');
                    if (node) node.textContent = "";
                });
#endif
                set_status(*project);
                status = "loaded " + project->root.filename().string();
                // Files that failed to load are listed, with fixes where there are some.
                ui.show_load_problems_window = !project->load_warnings.empty();
                // A newly opened map always starts in the City view.
                ui.tab = EditorTab::City;
                ui.city_tool = CityTool::Select;
                ui.city_selection = CitySelectionKind::None;
                ui.selected_city_buildings.clear();
                if (screenshot_tab == "buildings" || screenshot_tab == "buildings_open") ui.tab = EditorTab::Buildings;
                if (screenshot_tab == "buildings_open") ui.building_hide_roofs = true;
                if (screenshot_tab == "buildings_walls") {
                    ui.tab = EditorTab::Buildings;
                    ui.building_tool = BuildingTool::PaintTexture;
                    ui.paint_side_code = true;
                }
                if (const char* name = std::getenv("PAFTA_SCREENSHOT_BUILDING")) {
                    if (const auto* asset = find_building_asset(*project, name)) {
                        ui.selected_building = static_cast<std::size_t>(asset - project->buildings.data());
                    }
                }
                if (screenshot_tab == "buildings_items") {
                    ui.tab = EditorTab::Buildings;
                    ui.building_tool = BuildingTool::ItemSet;
                }
                if (screenshot_tab == "buildings_place") {
                    ui.tab = EditorTab::Buildings;
                    ui.building_tool = BuildingTool::PlaceBlock;
                    ui.block_config.interior = true;
                }
                if (screenshot_tab == "pieces") { ui.tab = EditorTab::City; ui.city_tool = CityTool::Piece; }
                if (screenshot_tab == "buildblocks") ui.tab = EditorTab::Buildblocks;
                if (screenshot_tab == "blocks") {
                    ui.tab = EditorTab::Blocks;
                    // Show a selection: the first patch, or else the first box.
                    if (!project->blocks.empty()) {
                        const auto& block = project->blocks[std::min(ui.selected_block, project->blocks.size() - 1U)].data;
                        if (!block.authored_patches.empty()) ui.block_element = BlockElementKind::Patch;
                        else if (!block.lights.empty()) ui.block_element = BlockElementKind::Box;
                        ui.block_element_index = 0;
                    }
                }
                if (screenshot_tab == "win_buildings") ui.show_building_browser = true;
                if (screenshot_tab == "win_blocks") ui.show_block_browser = true;
                if (screenshot_tab == "win_buildblock") ui.show_buildblock_window = true;
                if (screenshot_tab == "win_changes") ui.show_changes_window = true;
                if (screenshot_tab == "win_preferences") ui.show_preferences_window = true;
                if (screenshot_tab == "win_map") ui.show_map_image_window = true;
                if (screenshot_tab == "win_csx") show_csx_editor = true;
                if (screenshot_tab == "autofix") {
                    for (std::size_t i = project->failed_assets.size(); i-- > 0;) apply_asset_repair(*project, ui, i, status);
                    std::printf("autofix: %zu warnings left, %zu buildings\n", project->load_warnings.size(), project->buildings.size());
                }
                if (screenshot_tab == "win_shortcuts") ui.show_shortcuts_window = true;
                if (screenshot_path && project->city && !project->city->data.buildings.empty() && screenshot_tab != "pieces") {
                    ui.city_tool = CityTool::Select;
                    if (ui.tab == EditorTab::City) {
                        ui.city_selection = CitySelectionKind::Building;
                        ui.selected_city_building = 0U;
                        ui.selected_city_buildings = {0U};
                    }
                }
                ui.tab_cameras = {};  // a new map starts every mode framed
                if (ui.tab == EditorTab::City) {
                    focus_camera_on_city(camera, *project);
                } else {
                    camera = default_camera();
                    if (ui.tab == EditorTab::Buildings) {
                        focus_current_selection(*project, ui, camera);
                    }
                    if (ui.tab == EditorTab::Blocks) frame_block_camera(*project, ui, camera);
                }

            }
        }
#ifdef __EMSCRIPTEN__
        // Keep the one desktop-style loop working in a browser. Asyncify yields to the
        // browser's event loop, so we don't need a second, web-only copy of the frame
        // code and its state.
        emscripten_sleep(0);
#endif
    }

    if (project) {
        free_project_thumbnails(*project);
    }
    if (project_to_open) release_web_project(project_to_open->root);
    if (project) release_web_project(project->root);
    if (pending_load_folder) release_web_project(*pending_load_folder);
    project_to_open.reset();
    project.reset();
    archive_workspace.texture_previews.clear();  // GL textures, freed before the window closes
    unload_preview_views(ui);
    unload_game_render_texture(viewport_texture);
    UnloadRenderTexture(building_outline_mask_texture);
    if (IsCursorHidden()) {
        EnableCursor();
    }
    rlImGuiShutdown();
    CloseWindow();
    return 0;
}

} // namespace pafta
