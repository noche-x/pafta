#include "app.hpp"
#include "app_internal.hpp"

#include <raylib.h>
#include <rlgl.h>
#include <imgui.h>
#include <rlImGui.h>

#include <portable-file-dialogs.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// raylib's bundled GLFW desktop backend exports these (they are defined in
// libraylib.a and raylib calls glfwGetKeyName internally), but raylib does not
// re-export them. Forward-declaring lets us show layout-correct key labels
// without pulling in the GLFW header or touching the build graph.
extern "C" const char* glfwGetKeyName(int key, int scancode);
extern "C" int glfwGetKeyScancode(int key);

namespace map_editor::app_internal {

// Display label for a key, localized to the user's keyboard layout. raylib's
// IsKeyPressed checks physical key POSITION (US-QWERTY scancodes), so the
// movement/edit keys keep the same physical spots on every layout; this only
// changes what we PRINT, e.g. the physical W position shows "Z" on AZERTY.
// glfwGetKeyName returns the layout label for printable keys and null for
// non-printable ones (arrows, Tab, Delete...), which fall back to a fixed name.
std::string key_label(int key) {
    if (const char* name = glfwGetKeyName(key, glfwGetKeyScancode(key)); name != nullptr && name[0] != '\0') {
        std::string label{name};
        for (auto& ch : label) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
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
    stats.uploads.uploaded_vertices += model.uploaded_vertices;
    stats.uploads.uploaded_indices += model.uploaded_indices;
    stats.uploads.uploaded_bytes += model.uploaded_bytes;
    stats.uploads.chunks += model.chunks.size();
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

std::string lower_extension(const std::filesystem::path& path) {
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return extension;
}

std::string stem_name(const std::filesystem::path& path) {
    return path.stem().string();
}

std::vector<std::filesystem::path> files_with_extension(const std::filesystem::path& dir, std::string_view extension);

std::string lower_name(std::string_view value) {
    std::string lowered{value};
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered;
}

std::string city_block_label(const SbcCityBlock& city_block) {
    return std::string{city_block_display_name(city_block)};
}

std::uint32_t read_le_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + 4U > bytes.size()) {
        return 0U;
    }
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

void configure_texture(Texture2D texture) {
    SetTextureWrap(texture, TEXTURE_WRAP_REPEAT);
    SetTextureFilter(texture, TEXTURE_FILTER_BILINEAR);
}

void add_texture_if_valid(EditorProject& project, std::string name, Texture2D texture, std::unordered_map<std::string, std::size_t>& loaded_names) {
    if (texture.id == 0U) {
        return;
    }

    const auto key = lower_name(name);
    if (loaded_names.contains(key)) {
        UnloadTexture(texture);
        return;
    }

    configure_texture(texture);
    loaded_names.emplace(key, project.textures.size());
    project.textures.emplace_back(std::move(name), texture);
}

std::optional<Texture2D> load_embedded_png_texture(const std::vector<std::uint8_t>& payload) {
    if (payload.size() < 0x4cU || read_le_u32(payload, 0U) != 1U) {
        return std::nullopt;
    }

    const auto image_size = static_cast<std::size_t>(read_le_u32(payload, 0x44U));
    if (image_size == 0U || image_size > payload.size() - 0x4cU) {
        return std::nullopt;
    }
    const auto* image_bytes = payload.data() + 0x4cU;
    if (image_size < 2U || image_bytes[0] != 0x89U || image_bytes[1] != 0x50U) {
        return std::nullopt;
    }

    Image image = LoadImageFromMemory(".png", image_bytes, static_cast<int>(image_size));
    if (!IsImageValid(image)) {
        return std::nullopt;
    }

    Texture2D texture = LoadTextureFromImage(image);
    UnloadImage(image);
    if (texture.id == 0U) {
        return std::nullopt;
    }
    return texture;
}

void load_project_textures(EditorProject& project) {
    std::unordered_map<std::string, std::size_t> loaded_names;

    for (const auto& path : files_with_extension(project.root / "texture", ".png")) {
        add_texture_if_valid(project, stem_name(path), LoadTexture(path.string().c_str()), loaded_names);
    }
    for (const auto& path : files_with_extension(project.root, ".png")) {
        add_texture_if_valid(project, stem_name(path), LoadTexture(path.string().c_str()), loaded_names);
    }
    for (const auto& path : files_with_extension(project.root, ".csx")) {
        const auto archive = parse_csx_file(path);
        for (const auto& entry : archive.entries) {
            if (entry.kind != CsxEntryKind::Texture) {
                continue;
            }
            if (loaded_names.contains(lower_name(entry.name))) {
                continue;
            }
            if (auto texture = load_embedded_png_texture(entry.payload)) {
                add_texture_if_valid(project, entry.name, *texture, loaded_names);
            }
        }
    }
}

std::vector<std::filesystem::path> files_with_extension(const std::filesystem::path& dir, std::string_view extension) {
    std::vector<std::filesystem::path> files;
    if (!std::filesystem::is_directory(dir)) {
        return files;
    }

    for (const auto& entry : std::filesystem::directory_iterator{dir}) {
        if (entry.is_regular_file() && lower_extension(entry.path()) == extension) {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    return files;
}

std::optional<EditorProject> load_project_folder(const std::filesystem::path& folder, std::string& error) {
    const auto root = std::filesystem::weakly_canonical(folder);
    if (!std::filesystem::is_directory(root)) {
        error = "not a folder: " + root.string();
        return std::nullopt;
    }

    EditorProject project;
    project.root = root;

    const auto city_path = root / "city2.sbc";
    if (std::filesystem::exists(city_path)) {
        project.city_bytes = read_file_bytes(city_path);
        project.city = LoadedAsset<SbcMap>{city_path, "city2", parse_sbc(*project.city_bytes)};
    }

    for (const auto& path : files_with_extension(root / "building", ".sbb")) {
        auto bytes = read_file_bytes(path);
        project.buildings.push_back({path, stem_name(path), parse_sbb(bytes)});
        project.building_bytes.push_back(std::move(bytes));
    }

    for (const auto& path : files_with_extension(root / "block", ".sbl")) {
        auto bytes = read_file_bytes(path);
        auto parsed = parse_sbl(bytes);
        if (parsed.block.has_value()) {
            project.blocks.push_back({path, stem_name(path), *parsed.block});
            project.block_bytes.push_back(std::move(bytes));
        }
    }

    for (const auto& path : files_with_extension(root / "buildblock", ".sbl")) {
        auto bytes = read_file_bytes(path);
        auto parsed = parse_sbl(bytes);
        if (!parsed.layers.empty()) {
            project.buildblocks.push_back({path, stem_name(path), std::move(parsed)});
            project.buildblock_bytes.push_back(std::move(bytes));
        }
    }

    load_project_textures(project);

    if (!project.city && project.buildings.empty() && project.blocks.empty() && project.buildblocks.empty()) {
        error = "folder does not look like a map folder: " + root.string();
        return std::nullopt;
    }

    project.render_buffers_dirty = true;
    project.status = "loaded " + root.filename().string() + " (" + std::to_string(project.textures.size()) + " textures)";
    return project;
}

std::vector<NamedBlockMesh> block_meshes(const EditorProject& project) {
    std::vector<NamedBlockMesh> meshes;
    meshes.reserve(project.blocks.size());
    for (const auto& block : project.blocks) {
        meshes.push_back({block.name, block.data});
    }
    return build_native_block_mesh_catalog(meshes);
}

std::vector<NamedBuildblockTemplate> buildblock_templates(const EditorProject& project) {
    std::vector<NamedBuildblockTemplate> templates;
    templates.reserve(project.buildblocks.size());
    for (const auto& buildblock : project.buildblocks) {
        templates.push_back({buildblock.name, buildblock.data});
    }
    return templates;
}

void write_file_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file) {
        throw std::runtime_error("could not open output file: " + path.string());
    }
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        throw std::runtime_error("could not write output file: " + path.string());
    }
}

std::vector<std::string> building_names(const EditorProject& project) {
    std::vector<std::string> names;
    for (const auto& building : project.buildings) {
        names.push_back(building.name);
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

// Resolves each city placement to its loaded .sbb (nullptr when the name does
// not resolve), parallel to map.buildings. Shared by the scene-cache rebuild and
// the viewport building picker so both see the same footprint geometry.
std::vector<const SbbBuilding*> resolve_placement_buildings(const EditorProject& project) {
    std::vector<const SbbBuilding*> placement_buildings;
    if (!project.city) {
        return placement_buildings;
    }
    placement_buildings.reserve(project.city->data.buildings.size());
    for (const auto& placement : project.city->data.buildings) {
        const auto asset = std::ranges::find_if(
            project.buildings,
            [&placement](const LoadedAsset<SbbBuilding>& candidate) {
                return candidate.name == placement.name || candidate.data.name == placement.name;
            });
        placement_buildings.push_back(asset == project.buildings.end() ? nullptr : &asset->data);
    }
    return placement_buildings;
}

// Launches a background rebuild of the city scene (roads + terrain field +
// footprint stamping) for the given fingerprint. The worker owns copies of the
// map and the resolved .sbb data so nothing it touches can be mutated or freed by
// the main thread mid-build. GPU upload stays on the main thread (see the caller).
void start_city_scene_rebuild(EditorProject& project, std::uint64_t fingerprint) {
    if (!project.city) {
        return;
    }
    auto map_copy = project.city->data;
    // Copy each resolved building by value; the worker rebuilds the pointer view
    // over its own owned copies so the pointers cannot dangle. resolved[i] marks
    // whether placement i resolved to a real .sbb (else the slot is a nullptr).
    auto owned = std::make_shared<std::vector<SbbBuilding>>();
    std::vector<char> resolved;
    owned->reserve(project.city->data.buildings.size());
    resolved.reserve(project.city->data.buildings.size());
    for (const auto& placement : project.city->data.buildings) {
        const auto asset = std::ranges::find_if(
            project.buildings,
            [&placement](const LoadedAsset<SbbBuilding>& candidate) {
                return candidate.name == placement.name || candidate.data.name == placement.name;
            });
        const bool found = asset != project.buildings.end();
        owned->push_back(found ? asset->data : SbbBuilding{});
        resolved.push_back(found ? 1 : 0);
    }

    project.city_scene_build_in_progress = true;
    project.city_scene_build_target_fingerprint = fingerprint;
    const auto generation = ++project.city_scene_build_generation;
    project.city_scene_build_future = std::async(
        std::launch::async,
        [map = std::move(map_copy), owned, resolved = std::move(resolved), generation]() {
            std::vector<const SbbBuilding*> placement_buildings;
            placement_buildings.reserve(owned->size());
            for (std::size_t i = 0; i < owned->size(); ++i) {
                placement_buildings.push_back(resolved[i] != 0 ? &(*owned)[i] : nullptr);
            }
            (void)generation;
            return build_map_scene(map, placement_buildings);
        });
}

void draw_city_building_models_full_detail(
    EditorProject& project,
    const SbcBuilding* selected_building,
    const Texture2D* city_window_texture,
    const SbcBuilding* hovered_building = nullptr) {
    if (!project.city) {
        return;
    }

    // PERF: City view intentionally skips the distant box LOD; revisit culling/batching if full detail regresses.
    for (const auto& placement : project.city->data.buildings) {
        const auto asset = std::ranges::find_if(project.buildings, [&placement](const LoadedAsset<SbbBuilding>& candidate) {
            return candidate.name == placement.name || candidate.data.name == placement.name;
        });
        if (asset == project.buildings.end()) {
            continue;
        }

        const auto asset_index = static_cast<std::size_t>(std::distance(project.buildings.begin(), asset));
        if (asset_index >= project.building_gpu_models.size()) {
            continue;
        }

        draw_city_building_model(
            asset->data,
            project.building_gpu_models[asset_index],
            placement,
            selected_building == &placement,
            false,
            city_window_texture,
            hovered_building == &placement);
    }
}

std::vector<std::string> city_street_names(const SbcMap& map) {
    std::vector<std::string> names;
    names.reserve(map.streets.size());
    for (const auto& street : map.streets) {
        names.push_back(std::to_string(street.index) + " " + street.name);
    }
    return names;
}

std::vector<std::string> city_block_names(const SbcMap& map) {
    std::vector<std::string> names;
    names.reserve(map.city_blocks.size());
    for (const auto& city_block : map.city_blocks) {
        names.push_back(std::to_string(city_block.index) + " " + city_block_label(city_block));
    }
    return names;
}

std::vector<std::string> portal_names(const SbcMap& map) {
    std::vector<std::string> names;
    names.reserve(map.portals.size());
    for (const auto& portal : map.portals) {
        const auto placed = std::ranges::count_if(portal.blocks, [](std::uint32_t word) {
            return word != 0U;
        });
        names.push_back(
            std::to_string(portal.index) + " [" +
            std::to_string(portal.fields[1]) + "," +
            std::to_string(portal.fields[2]) + "," +
            std::to_string(portal.fields[3]) + "] voxels " +
            std::to_string(placed));
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
}

std::filesystem::path edited_city_path(const EditorProject& project) {
    if (!project.city.has_value()) {
        return project.root / "city2.edited.sbc";
    }
    auto output = project.city->path;
    output.replace_filename(output.stem().string() + ".edited" + output.extension().string());
    return output;
}

std::filesystem::path edited_path(const std::filesystem::path& path) {
    auto output = path;
    output.replace_filename(output.stem().string() + ".edited" + output.extension().string());
    return output;
}

std::uint32_t& building_cell_field(SbbCell& cell, std::size_t field_index) {
    switch (field_index) {
        case 0:
            return cell.exterior_block();
        case 1:
            return cell.interior_block();
        case 2:
            return cell.cell_type();
        case 3:
            return cell.exterior_portal_face();
        case 4:
            return cell.interior_portal_face();
        case 5:
            return cell.placed_flag();
        case 6:
            return cell.item_set_index;
    }
    return cell.exterior_block();
}

void nudge_building_cell(EditorProject& project, EditorUi& ui, int delta) {
    if (project.buildings.empty()) {
        return;
    }
    ui.selected_building = std::min(ui.selected_building, project.buildings.size() - 1U);
    auto& building = project.buildings[ui.selected_building].data;
    const auto occupied = occupied_cell_indices(building);
    if (occupied.empty()) {
        return;
    }
    ui.selected_building_cell = std::min(ui.selected_building_cell, occupied.size() - 1U);
    auto& value = building_cell_field(building.cells[occupied[ui.selected_building_cell]], ui.selected_building_field);
    if (delta < 0 && value < static_cast<std::uint32_t>(-delta)) {
        value = 0;
    } else {
        value = static_cast<std::uint32_t>(static_cast<std::int64_t>(value) + delta);
    }
    ui.building_dirty = true;
    project.render_buffers_dirty = true;
    project.building_floor_preview_dirty = true;
}

void save_building(EditorProject& project, EditorUi& ui, std::string& status) {
    if (project.buildings.empty() || ui.selected_building >= project.buildings.size() || ui.selected_building >= project.building_bytes.size()) {
        return;
    }
    const auto& asset = project.buildings[ui.selected_building];
    const auto output = edited_path(asset.path);
    write_file_bytes(output, rebuild_sbb(asset.data));
    status = "saved " + output.filename().string();
    ui.building_dirty = false;
}

float adjusted_unit(float value, int steps) {
    return std::clamp(value + static_cast<float>(steps) * 0.0625F, 0.0F, 1.0F);
}

void nudge_buildblock_geometry(EditorProject& project, EditorUi& ui, int steps) {
    if (project.buildblocks.empty()) {
        return;
    }
    ui.selected_buildblock = std::min(ui.selected_buildblock, project.buildblocks.size() - 1U);
    auto& buildblock = project.buildblocks[ui.selected_buildblock].data;
    if (buildblock.layers.empty()) {
        return;
    }
    ui.selected_buildblock_layer = std::min(ui.selected_buildblock_layer, buildblock.layers.size() - 1U);
    auto& layer = buildblock.layers[ui.selected_buildblock_layer];
    if (!layer.doors.empty()) {
        ui.selected_buildblock_door = std::min(ui.selected_buildblock_door, layer.doors.size() - 1U);
    }
    if (!layer.windows.empty()) {
        ui.selected_buildblock_window = std::min(ui.selected_buildblock_window, layer.windows.size() - 1U);
    }
    if (!layer.beams.empty()) {
        ui.selected_buildblock_beam = std::min(ui.selected_buildblock_beam, layer.beams.size() - 1U);
    }

    switch (ui.selected_buildblock_field) {
        case 0:
            layer.bounds[0] = adjusted_unit(layer.bounds[0], steps);
            break;
        case 1:
            layer.bounds[1] = adjusted_unit(layer.bounds[1], steps);
            break;
        case 2:
            layer.bounds[2] = adjusted_unit(layer.bounds[2], steps);
            break;
        case 3:
            layer.bounds[3] = adjusted_unit(layer.bounds[3], steps);
            break;
        case 4:
            if (!layer.doors.empty()) {
                layer.doors[ui.selected_buildblock_door].x = adjusted_unit(layer.doors[ui.selected_buildblock_door].x, steps);
            }
            break;
        case 5:
            if (!layer.doors.empty()) {
                layer.doors[ui.selected_buildblock_door].y = adjusted_unit(layer.doors[ui.selected_buildblock_door].y, steps);
            }
            break;
        case 6:
            if (!layer.doors.empty()) {
                auto& type = layer.doors[ui.selected_buildblock_door].type;
                type = steps < 0 && type < static_cast<std::uint32_t>(-steps) ? 0U : static_cast<std::uint32_t>(static_cast<std::int64_t>(type) + steps);
            }
            break;
        case 7:
            if (!layer.windows.empty()) {
                layer.windows[ui.selected_buildblock_window].x = adjusted_unit(layer.windows[ui.selected_buildblock_window].x, steps);
            }
            break;
        case 8:
            if (!layer.windows.empty()) {
                layer.windows[ui.selected_buildblock_window].width = adjusted_unit(layer.windows[ui.selected_buildblock_window].width, steps);
            }
            break;
        case 9:
            if (!layer.windows.empty()) {
                layer.windows[ui.selected_buildblock_window].y = adjusted_unit(layer.windows[ui.selected_buildblock_window].y, steps);
            }
            break;
        case 10:
            if (!layer.windows.empty()) {
                layer.windows[ui.selected_buildblock_window].height = adjusted_unit(layer.windows[ui.selected_buildblock_window].height, steps);
            }
            break;
        case 11:
            if (!layer.windows.empty()) {
                auto& type = layer.windows[ui.selected_buildblock_window].type;
                type = steps < 0 && type < static_cast<std::uint32_t>(-steps) ? 0U : static_cast<std::uint32_t>(static_cast<std::int64_t>(type) + steps);
            }
            break;
        case 12:
            if (!layer.beams.empty()) {
                layer.beams[ui.selected_buildblock_beam].x = adjusted_unit(layer.beams[ui.selected_buildblock_beam].x, steps);
            }
            break;
        case 13:
            if (!layer.beams.empty()) {
                layer.beams[ui.selected_buildblock_beam].width = adjusted_unit(layer.beams[ui.selected_buildblock_beam].width, steps);
            }
            break;
        case 14:
            if (!layer.beams.empty()) {
                layer.beams[ui.selected_buildblock_beam].y = adjusted_unit(layer.beams[ui.selected_buildblock_beam].y, steps);
            }
            break;
        case 15:
            if (!layer.beams.empty()) {
                layer.beams[ui.selected_buildblock_beam].height = adjusted_unit(layer.beams[ui.selected_buildblock_beam].height, steps);
            }
            break;
        case 16:
            if (!layer.beams.empty()) {
                auto& type = layer.beams[ui.selected_buildblock_beam].type;
                type = steps < 0 && type < static_cast<std::uint32_t>(-steps) ? 0U : static_cast<std::uint32_t>(static_cast<std::int64_t>(type) + steps);
            }
            break;
        default:
            break;
    }
    ui.buildblock_dirty = true;
    project.render_buffers_dirty = true;
    project.building_floor_preview_dirty = true;
    invalidate_portal_render_models(project);
}

std::string block_field_value(const SblBlockMesh& block, const EditorUi& ui) {
    const auto field = ui.selected_block_field;
    const auto patch_index = block.authored_patches.empty() ? 0U : std::min(ui.selected_block_patch, block.authored_patches.size() - 1U);
    const auto point_index = block.authored_patches.empty() || block.authored_patches[patch_index].control_points.empty()
        ? 0U
        : std::min(ui.selected_block_point, block.authored_patches[patch_index].control_points.size() - 1U);
    switch (field) {
        case 0:
            return "width=" + std::to_string(block.width);
        case 1:
            return "height=" + std::to_string(block.height);
        case 2:
            return "depth=" + std::to_string(block.depth);
        case 3:
        case 4:
        case 5:
        case 6:
        case 7:
        case 8: {
            const auto flag = std::min<std::size_t>(field - 3U, block.exterior_flags.size() - 1U);
            return "flag" + std::to_string(flag) + "=" + std::to_string(block.exterior_flags[flag]);
        }
        case 9:
            return block.authored_patches.empty() ? "patch x=(none)" : "patch" + std::to_string(patch_index) + " point" + std::to_string(point_index) + " x=" + std::to_string(block.authored_patches[patch_index].control_points[point_index].position.x);
        case 10:
            return block.authored_patches.empty() ? "patch y=(none)" : "patch" + std::to_string(patch_index) + " point" + std::to_string(point_index) + " y=" + std::to_string(block.authored_patches[patch_index].control_points[point_index].position.y);
        case 11:
            return block.authored_patches.empty() ? "patch z=(none)" : "patch" + std::to_string(patch_index) + " point" + std::to_string(point_index) + " z=" + std::to_string(block.authored_patches[patch_index].control_points[point_index].position.z);
        case 12:
            return block.authored_patches.empty() ? "patch u=(none)" : "patch" + std::to_string(patch_index) + " point" + std::to_string(point_index) + " u=" + std::to_string(block.authored_patches[patch_index].control_points[point_index].u);
        case 13:
            return block.authored_patches.empty() ? "patch v=(none)" : "patch" + std::to_string(patch_index) + " point" + std::to_string(point_index) + " v=" + std::to_string(block.authored_patches[patch_index].control_points[point_index].v);
    }
    return "width=" + std::to_string(block.width);
}

void nudge_block_header(EditorProject& project, EditorUi& ui, int delta) {
    if (project.blocks.empty()) {
        return;
    }
    ui.selected_block = std::min(ui.selected_block, project.blocks.size() - 1U);
    auto& block = project.blocks[ui.selected_block].data;
    auto adjust_dimension = [delta](std::uint32_t& value) {
        const auto next = static_cast<int>(value) + delta;
        value = static_cast<std::uint32_t>(std::clamp(next, 1, 0x40));
    };

    switch (ui.selected_block_field) {
        case 0:
            adjust_dimension(block.width);
            break;
        case 1:
            adjust_dimension(block.height);
            break;
        case 2:
            adjust_dimension(block.depth);
            break;
        default: {
            const auto flag = std::min<std::size_t>(ui.selected_block_field - 3U, block.exterior_flags.size() - 1U);
            block.exterior_flags[flag] = block.exterior_flags[flag] == 0 ? 1U : 0U;
            break;
        }
    }
    ui.block_dirty = true;
    refresh_sbl_block_faces(block);
    project.render_buffers_dirty = true;
    invalidate_portal_render_models(project);
}

void nudge_block_patch(EditorProject& project, EditorUi& ui, int steps) {
    if (project.blocks.empty()) {
        return;
    }
    ui.selected_block = std::min(ui.selected_block, project.blocks.size() - 1U);
    auto& block = project.blocks[ui.selected_block].data;
    if (block.authored_patches.empty()) {
        return;
    }
    ui.selected_block_patch = std::min(ui.selected_block_patch, block.authored_patches.size() - 1U);
    auto& patch = block.authored_patches[ui.selected_block_patch];
    if (patch.control_points.empty()) {
        return;
    }
    ui.selected_block_point = std::min(ui.selected_block_point, patch.control_points.size() - 1U);
    auto& point = patch.control_points[ui.selected_block_point];
    const auto delta = static_cast<float>(steps) * 0.0625F;
    switch (ui.selected_block_field) {
        case 9:
            point.position.x += delta;
            break;
        case 10:
            point.position.y += delta;
            break;
        case 11:
            point.position.z += delta;
            break;
        case 12:
            point.u += delta;
            break;
        case 13:
            point.v += delta;
            break;
        default:
            break;
    }
    ui.block_dirty = true;
    refresh_sbl_block_faces(block);
    project.render_buffers_dirty = true;
    invalidate_portal_render_models(project);
}

void nudge_block(EditorProject& project, EditorUi& ui, int delta) {
    if (ui.selected_block_field < 9U) {
        nudge_block_header(project, ui, delta);
    } else {
        nudge_block_patch(project, ui, delta);
    }
}

void save_block(EditorProject& project, EditorUi& ui, std::string& status) {
    if (project.blocks.empty() || ui.selected_block >= project.blocks.size() || ui.selected_block >= project.block_bytes.size()) {
        return;
    }
    const auto& asset = project.blocks[ui.selected_block];
    const auto output = edited_path(asset.path);
    auto patched = patch_sbl_block_header(project.block_bytes[ui.selected_block], asset.data);
    patched = patch_sbl_block_patches(patched, asset.data);
    write_file_bytes(output, patched);
    status = "saved " + output.filename().string();
    ui.block_dirty = false;
}

void save_buildblock(EditorProject& project, EditorUi& ui, std::string& status) {
    if (project.buildblocks.empty() || ui.selected_buildblock >= project.buildblocks.size() || ui.selected_buildblock >= project.buildblock_bytes.size()) {
        return;
    }
    const auto& asset = project.buildblocks[ui.selected_buildblock];
    auto patched = patch_sbl_layers(project.buildblock_bytes[ui.selected_buildblock], asset.data);
    const auto output = edited_path(asset.path);
    write_file_bytes(output, patched);
    status = "saved " + output.filename().string();
    ui.buildblock_dirty = false;
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
    building.rotation = (building.rotation + drot) & 3;
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

void place_city_building_copy(EditorProject& project, EditorUi& ui, CityGridPoint point) {
    if (!project.city || project.city->data.buildings.empty()) {
        return;
    }

    duplicate_city_building(project, ui);
    set_city_building_position(project, ui, point);
}

void delete_city_building(EditorProject& project, EditorUi& ui) {
    if (!project.city || project.city->data.buildings.size() <= 1U) {
        return;
    }

    auto& map = project.city->data;
    ui.selected_city_building = std::min(ui.selected_city_building, map.buildings.size() - 1U);
    map.buildings.erase(map.buildings.begin() + static_cast<std::ptrdiff_t>(ui.selected_city_building));
    reindex_city_buildings(map);
    ui.selected_city_building = std::min(ui.selected_city_building, map.buildings.size() - 1U);
    ui.city_dirty = true;
}

void nudge_city_street_endpoint(EditorProject& project, EditorUi& ui, int endpoint, int delta) {
    if (!project.city || project.city->data.streets.empty() || project.city->data.intersections.empty()) {
        return;
    }

    ui.selected_city_street = std::min(ui.selected_city_street, project.city->data.streets.size() - 1U);
    auto& street = project.city->data.streets[ui.selected_city_street];
    auto& value = endpoint == 0 ? street.from_intersection : street.to_intersection;
    const auto max_index = static_cast<int>(project.city->data.intersections.size() - 1U);
    value = std::clamp(value + delta, 0, max_index);
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

void nudge_city_street_district(EditorProject& project, EditorUi& ui, int delta) {
    if (!project.city || project.city->data.streets.empty()) {
        return;
    }

    ui.selected_city_street = std::min(ui.selected_city_street, project.city->data.streets.size() - 1U);
    auto& street = project.city->data.streets[ui.selected_city_street];
    street.district = std::max(0, street.district + delta);
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

// Nearest existing intersection to a grid point within snap_cells, or nullopt.
// Lets a road click reuse an existing junction instead of stacking a new node on
// top of it (editor.exe behavior).
std::optional<std::size_t> nearest_city_intersection(const SbcMap& map, CityGridPoint point, std::int32_t snap_cells) {
    std::optional<std::size_t> best;
    std::int64_t best_dist_sq = static_cast<std::int64_t>(snap_cells) * snap_cells;
    for (std::size_t i = 0; i < map.intersections.size(); ++i) {
        const auto& it = map.intersections[i];
        const std::int64_t dx = it.x - point.x;
        const std::int64_t dz = it.z - point.z;
        const auto dist_sq = dx * dx + dz * dz;
        if (dist_sq <= best_dist_sq) {
            best_dist_sq = dist_sq;
            best = i;
        }
    }
    return best;
}

// Constrains an end point to be axis-aligned with a start node: locks whichever
// of X/Z moved less, so the segment runs straight along the dominant axis. Roads
// are never diagonal.
CityGridPoint snap_to_axis(CityGridPoint start, CityGridPoint target) {
    const auto dx = std::abs(target.x - start.x);
    const auto dz = std::abs(target.z - start.z);
    if (dx >= dz) {
        return CityGridPoint{target.x, target.y, start.z};
    }
    return CityGridPoint{start.x, target.y, target.z};
}

// Resolves a click to an intersection index: snaps to a nearby existing node, or
// creates a new one at the given point. New nodes pick up terrain height so the
// road can run up or down with the land.
std::size_t resolve_or_create_intersection(EditorProject& project, CityGridPoint point) {
    auto& map = project.city->data;
    if (const auto existing = nearest_city_intersection(map, point, 1)) {
        return *existing;
    }
    CityGridPoint placed = point;
    if (project.city_scene_cache_valid && project.city_scene_cache.terrain.has_value()) {
        const auto unit = project.city_scene_cache.unit_size;
        const auto h = terrain_height_at(*project.city_scene_cache.terrain,
            static_cast<float>(point.x) * unit, static_cast<float>(point.z) * unit);
        placed.y = static_cast<std::int32_t>(std::lround(h / unit));
    }
    return add_city_intersection(map, placed);
}

void append_city_road_point(EditorProject& project, EditorUi& ui, CityGridPoint point) {
    if (!project.city) {
        return;
    }

    auto& map = project.city->data;

    // First click of a chain: pick the start node (existing or new). No segment
    // yet; the next click closes a segment to here.
    if (!ui.pending_road_intersection.has_value()) {
        ui.pending_road_intersection = resolve_or_create_intersection(project, point);
        ui.city_dirty = true;
        return;
    }

    // Clicking the active start node again ends the chain without a 0-length road.
    const auto& start_node = map.intersections[*ui.pending_road_intersection];
    if (start_node.x == point.x && start_node.z == point.z) {
        ui.pending_road_intersection.reset();
        return;
    }

    // Axis-lock the end relative to the start, then resolve/create that node.
    const CityGridPoint start_point{start_node.x, start_node.y, start_node.z};
    const auto end_point = snap_to_axis(start_point, point);
    const auto end_index = resolve_or_create_intersection(project, end_point);

    SbcStreet street{};
    if (!map.streets.empty()) {
        street = map.streets[std::min(ui.selected_city_street, map.streets.size() - 1U)];
    } else {
        street.name = "Street";
        street.left_width = 3;
        street.right_width = 3;
    }
    street.index = map.streets.size();
    street.from_intersection = static_cast<std::int32_t>(*ui.pending_road_intersection);
    street.to_intersection = static_cast<std::int32_t>(end_index);
    map.streets.push_back(std::move(street));
    ui.selected_city_street = map.streets.size() - 1U;
    // The new end node becomes the next start: chained laying.
    ui.pending_road_intersection = end_index;
    ui.city_dirty = true;
}

// Ends the current road-laying chain (Esc, or mode/selection change).
void end_road_chain(EditorUi& ui) {
    ui.pending_road_intersection.reset();
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

void set_city_piece_voxel(EditorProject& project, EditorUi& ui, CityGridPoint point) {
    if (!project.city || project.city->data.city_blocks.empty()) {
        return;
    }

    ui.selected_city_block = std::min(ui.selected_city_block, project.city->data.city_blocks.size() - 1U);
    auto& city_block = project.city->data.city_blocks[ui.selected_city_block];
    if (!city_block.layers.empty()) {
        ui.selected_city_block_layer = std::min(ui.selected_city_block_layer, city_block.layers.size() - 1U);
    } else {
        ui.selected_city_block_layer = 0;
    }

    auto& portal = find_or_create_portal(project.city->data, point);
    if (portal.blocks.size() < 0x200) {
        portal.blocks.resize(0x200);
    }
    portal.blocks[local_portal_index(point)] = encode_city_piece_word(ui);
    invalidate_portal_render_models(project);
    ui.city_dirty = true;
}

void clear_city_piece_voxel(EditorProject& project, CityGridPoint point, EditorUi& ui) {
    if (!project.city) {
        return;
    }
    const auto fields = std::array<std::int32_t, 4>{0, floor_div8(point.x), floor_div8(point.y), floor_div8(point.z)};
    const auto match = std::ranges::find_if(project.city->data.portals, [fields](const SbcPortal& portal) {
        return portal.fields == fields;
    });
    if (match == project.city->data.portals.end() || match->blocks.size() < 0x200) {
        return;
    }
    match->blocks[local_portal_index(point)] = 0;
    invalidate_portal_render_models(project);
    ui.city_dirty = true;
}

void delete_city_street(EditorProject& project, EditorUi& ui) {
    if (!project.city || project.city->data.streets.size() <= 1U) {
        return;
    }

    auto& map = project.city->data;
    ui.selected_city_street = std::min(ui.selected_city_street, map.streets.size() - 1U);
    map.streets.erase(map.streets.begin() + static_cast<std::ptrdiff_t>(ui.selected_city_street));
    reindex_city_streets(map);
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
    invalidate_portal_render_models(project);
    ui.city_dirty = true;
}

void clear_city_block_slot(EditorProject& project, EditorUi& ui) {
    if (!project.city || project.city->data.city_blocks.empty()) {
        return;
    }

    ui.selected_city_block = std::min(ui.selected_city_block, project.city->data.city_blocks.size() - 1U);
    auto& city_block = project.city->data.city_blocks[ui.selected_city_block];
    if (city_block.layers.empty()) {
        return;
    }
    ui.selected_city_block_layer = std::min(ui.selected_city_block_layer, city_block.layers.size() - 1U);
    ui.selected_city_block_slot = std::min<std::size_t>(ui.selected_city_block_slot, 7U);
    city_block.layers[ui.selected_city_block_layer][ui.selected_city_block_slot].clear();
    invalidate_portal_render_models(project);
    ui.city_dirty = true;
}

void save_city(EditorProject& project, EditorUi& ui, std::string& status) {
    if (!project.city || !project.city_bytes) {
        return;
    }
    const auto output = edited_city_path(project);
    write_file_bytes(output, rebuild_sbc(project.city->data));
    status = "saved " + output.filename().string();
    ui.city_dirty = false;
}

void mark_building_changed(EditorProject& project, EditorUi& ui) {
    ui.building_dirty = true;
    project.render_buffers_dirty = true;
    project.building_floor_preview_dirty = true;
}

void mark_buildblock_changed(EditorProject& project, EditorUi& ui) {
    ui.buildblock_dirty = true;
    project.render_buffers_dirty = true;
    project.building_floor_preview_dirty = true;
    invalidate_portal_render_models(project);
}

void mark_block_changed(EditorProject& project, EditorUi& ui, SblBlockMesh& block) {
    ui.block_dirty = true;
    refresh_sbl_block_faces(block);
    project.render_buffers_dirty = true;
    invalidate_portal_render_models(project);
}

// Saves whichever tab's edited copy is active, mirroring the Inspector Save
// buttons. Used by the Ctrl+S hotkey.
void save_active_tab(EditorProject& project, EditorUi& ui, std::string& status) {
    switch (ui.tab) {
        case EditorTab::City:
            save_city(project, ui, status);
            break;
        case EditorTab::Pieces:
            save_city(project, ui, status);
            break;
        case EditorTab::Buildings:
            save_building(project, ui, status);
            break;
        case EditorTab::Blocks:
            save_block(project, ui, status);
            break;
        case EditorTab::Buildblocks:
            save_buildblock(project, ui, status);
            break;
    }
}

bool has_unsaved_changes(const EditorUi& ui) {
    return ui.city_dirty || ui.building_dirty || ui.block_dirty || ui.buildblock_dirty;
}

// Saves every category that has pending edits, using the same per-asset save path
// as the Inspector buttons (each writes the currently-selected asset of its kind).
void save_all_dirty(EditorProject& project, EditorUi& ui, std::string& status) {
    if (ui.city_dirty) {
        save_city(project, ui, status);
    }
    if (ui.building_dirty) {
        save_building(project, ui, status);
    }
    if (ui.block_dirty) {
        save_block(project, ui, status);
    }
    if (ui.buildblock_dirty) {
        save_buildblock(project, ui, status);
    }
}

// -----------------------------------------------------------------------------
// Building drag gizmo
// -----------------------------------------------------------------------------

// Geometry of the gizmo handles for a selected city building, in world space.
// Three move handles sit at the tip of short axis arms; the rotate handle is an
// offset ring marker. Sizes scale with the unit grid so the gizmo stays readable.
struct GizmoLayout {
    Vector3 origin{};
    Vector3 tip_x{};
    Vector3 tip_y{};
    Vector3 tip_z{};
    Vector3 tip_rot{};
    float handle_radius{};
    float arm_length{};
};

GizmoLayout building_gizmo_layout(const SbcBuilding& placement) {
    constexpr auto unit_size = 4.0F;
    const auto o = native_city_building_render_origin(placement, unit_size);
    const auto arm = unit_size * 2.5F;
    const Vector3 origin{o.x, o.y + unit_size * 0.5F, o.z};
    return GizmoLayout{
        origin,
        {origin.x + arm, origin.y, origin.z},
        {origin.x, origin.y + arm, origin.z},
        {origin.x, origin.y, origin.z + arm},
        {origin.x + arm * 0.7F, origin.y, origin.z + arm * 0.7F},
        unit_size * 0.45F,
        arm,
    };
}

// Nearest gizmo handle hit by the cursor ray, or None. A small sphere test at
// each handle tip is enough; the arms are only a visual guide.
GizmoAxis pick_building_gizmo(const Camera3D& camera, Vector2 virtual_mouse_pos, const SbcBuilding& placement) {
    const auto layout = building_gizmo_layout(placement);
    const auto ray = GetMouseRay(virtual_mouse_pos, camera);
    const std::array<std::pair<GizmoAxis, Vector3>, 4> handles{{
        {GizmoAxis::MoveX, layout.tip_x},
        {GizmoAxis::MoveY, layout.tip_y},
        {GizmoAxis::MoveZ, layout.tip_z},
        {GizmoAxis::Rotate, layout.tip_rot},
    }};
    GizmoAxis best = GizmoAxis::None;
    float best_t = std::numeric_limits<float>::max();
    for (const auto& [axis, tip] : handles) {
        const auto hit = GetRayCollisionSphere(ray, tip, layout.handle_radius);
        if (hit.hit && hit.distance < best_t) {
            best_t = hit.distance;
            best = axis;
        }
    }
    return best;
}

void draw_building_gizmo(const SbcBuilding& placement, GizmoAxis active) {
    const auto layout = building_gizmo_layout(placement);
    const auto axis_color = [active](GizmoAxis axis, Color base) {
        return active == axis ? Color{255, 255, 255, 255} : base;
    };
    const Color red{235, 92, 88, 255};
    const Color green{120, 220, 110, 255};
    const Color blue{96, 156, 235, 255};
    const Color yellow{245, 210, 90, 255};

    DrawLine3D(layout.origin, layout.tip_x, axis_color(GizmoAxis::MoveX, red));
    DrawLine3D(layout.origin, layout.tip_y, axis_color(GizmoAxis::MoveY, green));
    DrawLine3D(layout.origin, layout.tip_z, axis_color(GizmoAxis::MoveZ, blue));
    DrawSphere(layout.tip_x, layout.handle_radius, axis_color(GizmoAxis::MoveX, red));
    DrawSphere(layout.tip_y, layout.handle_radius, axis_color(GizmoAxis::MoveY, green));
    DrawSphere(layout.tip_z, layout.handle_radius, axis_color(GizmoAxis::MoveZ, blue));
    // Rotate handle: a torus-like marker (sphere stand-in for portability).
    DrawSphere(layout.tip_rot, layout.handle_radius * 0.9F, axis_color(GizmoAxis::Rotate, yellow));
    DrawCircle3D(layout.origin, layout.arm_length * 0.7F, {1.0F, 0.0F, 0.0F}, 90.0F,
        axis_color(GizmoAxis::Rotate, yellow));
}

// Maps the cursor's ground (or vertical) position during a gizmo drag to an
// absolute grid cell, then applies it via the existing edit functions. Uses the
// drag-start origin captured in ui so motion is absolute, not accumulated.
void apply_building_gizmo_drag(EditorProject& project, EditorUi& ui, const Camera3D& camera, Vector2 virtual_mouse_pos) {
    if (!project.city || project.city->data.buildings.empty()) {
        return;
    }
    ui.selected_city_building = std::min(ui.selected_city_building, project.city->data.buildings.size() - 1U);
    auto& placement = project.city->data.buildings[ui.selected_city_building];
    constexpr auto unit_size = 4.0F;
    const auto ray = GetMouseRay(virtual_mouse_pos, camera);
    const Vec3 ro{ray.position.x, ray.position.y, ray.position.z};
    const Vec3 rd{ray.direction.x, ray.direction.y, ray.direction.z};

    switch (ui.gizmo_active) {
        case GizmoAxis::MoveX:
        case GizmoAxis::MoveZ: {
            const auto hit = ray_ground_hit(ro, rd);
            if (!hit.has_value()) {
                return;
            }
            if (ui.gizmo_active == GizmoAxis::MoveX) {
                placement.x = static_cast<std::int32_t>(std::lround(hit->x / unit_size));
            } else {
                placement.z = static_cast<std::int32_t>(std::lround(hit->z / unit_size));
            }
            ui.city_dirty = true;
            break;
        }
        case GizmoAxis::MoveY: {
            // Project the ray onto the vertical line through the building origin
            // and read the height; vertical drag needs the camera-facing plane.
            const auto origin = native_city_building_render_origin(placement, unit_size);
            if (std::abs(rd.z) < 0.0001F && std::abs(rd.x) < 0.0001F) {
                return;
            }
            // Intersect with a plane that contains the Y axis and faces the camera
            // (use the X=origin.x plane unless the ray is parallel to it).
            float t;
            if (std::abs(rd.x) > 0.0001F) {
                t = (origin.x - ro.x) / rd.x;
            } else {
                t = (origin.z - ro.z) / rd.z;
            }
            if (t < 0.0F) {
                return;
            }
            const auto world_y = ro.y + rd.y * t;
            placement.y = static_cast<std::int32_t>(std::lround(world_y / unit_size));
            ui.city_dirty = true;
            break;
        }
        case GizmoAxis::Rotate: {
            // Rotate snaps to 90deg steps based on the cursor angle around origin.
            const auto hit = ray_ground_hit(ro, rd);
            if (!hit.has_value()) {
                return;
            }
            const auto origin = native_city_building_render_origin(placement, unit_size);
            const auto angle = std::atan2(hit->z - origin.z, hit->x - origin.x);
            // Snap the building's facing to the cursor's quadrant around origin.
            const auto step = static_cast<int>(std::lround(angle / (3.14159265F * 0.5F))) & 3;
            placement.rotation = step;
            ui.city_dirty = true;
            break;
        }
        case GizmoAxis::None:
            break;
    }
}

// Overlay (not road rendering) showing the active road-laying chain: a marker on
// the pending start node and an axis-snapped preview line to the cursor, so you
// see the segment before clicking. Drawn inside the City 3D pass.
void draw_road_lay_preview(const EditorProject& project, const EditorUi& ui, std::optional<CityGridPoint> cursor) {
    if (ui.city_mode != CityEditMode::Roads || !ui.pending_road_intersection.has_value() || !project.city) {
        return;
    }
    const auto& map = project.city->data;
    if (*ui.pending_road_intersection >= map.intersections.size()) {
        return;
    }
    const auto unit = project.city_scene_cache_valid ? project.city_scene_cache.unit_size : 4.0F;
    const auto& start = map.intersections[*ui.pending_road_intersection];
    const auto ground = [&](std::int32_t gx, std::int32_t gz, std::int32_t fallback_level) {
        const auto base = project.city_scene_cache_valid && project.city_scene_cache.terrain.has_value()
            ? terrain_height_at(*project.city_scene_cache.terrain, static_cast<float>(gx) * unit, static_cast<float>(gz) * unit)
            : static_cast<float>(fallback_level) * unit;
        return base + unit * 0.6F;
    };
    const Vector3 start_pos{static_cast<float>(start.x) * unit, ground(start.x, start.z, start.y), static_cast<float>(start.z) * unit};
    DrawSphere(start_pos, unit * 0.4F, Color{120, 230, 130, 235});

    if (cursor.has_value()) {
        const auto end = snap_to_axis(CityGridPoint{start.x, start.y, start.z}, *cursor);
        const Vector3 end_pos{static_cast<float>(end.x) * unit, ground(end.x, end.z, start.y), static_cast<float>(end.z) * unit};
        DrawLine3D(start_pos, end_pos, Color{255, 240, 140, 255});
        DrawSphere(end_pos, unit * 0.3F, Color{255, 220, 110, 220});
    }
}

// Collapsible reference of the controls active for the current tab/mode. Lives
// at the top of the Inspector so the keyboard map is discoverable without leaving
// the editing surface. Collapsed by default; a short fixed list, no scrolling.
void draw_controls_help(const EditorUi& ui) {
    if (!ImGui::CollapsingHeader("Controls")) {
        return;
    }
    const auto k = [](int key) { return key_label(key); };
    ImGui::TextDisabled(
        "Camera: hold Right-Mouse + %s%s%s%s, Space/Shift up/down, Ctrl 2x, wheel speed",
        k(KEY_W).c_str(), k(KEY_A).c_str(), k(KEY_S).c_str(), k(KEY_D).c_str());
    ImGui::TextDisabled("Global: %s+S save  |  %s focus  |  %s+O open",
        k(KEY_LEFT_CONTROL).c_str(), k(KEY_F).c_str(), k(KEY_LEFT_CONTROL).c_str());
    ImGui::Separator();
    if (ui.tab == EditorTab::City) {
        if (ui.city_mode == CityEditMode::Buildings) {
            ImGui::TextUnformatted("Click select / drag gizmo  |  Shift+Click move  |  Ctrl+Shift+Click copy");
            ImGui::Text("Arrows move X/Z  |  PgUp/PgDn move Y  |  %s rotate (Shift+%s back)",
                k(KEY_R).c_str(), k(KEY_R).c_str());
            ImGui::Text("%s+D duplicate  |  Del delete", k(KEY_LEFT_CONTROL).c_str());
        } else if (ui.city_mode == CityEditMode::Roads) {
            ImGui::TextUnformatted("Click lays road chain (axis-locked, snaps to nodes)  |  Esc ends");
            ImGui::TextUnformatted("Shift+Click selects a road to edit");
            ImGui::Text("Arrows from/to nodes  |  %s %s left width  |  %s %s right width",
                k(KEY_LEFT_BRACKET).c_str(), k(KEY_RIGHT_BRACKET).c_str(),
                k(KEY_SEMICOLON).c_str(), k(KEY_APOSTROPHE).c_str());
            ImGui::Text("%s %s district  |  %s+D duplicate  |  Del delete",
                k(KEY_MINUS).c_str(), k(KEY_EQUAL).c_str(), k(KEY_LEFT_CONTROL).c_str());
        } else {
            ImGui::Text("Shift+Click place piece  |  Ctrl+Click clear  |  %s rotate", k(KEY_R).c_str());
        }
    } else if (ui.tab == EditorTab::Buildings) {
        ImGui::TextUnformatted("Click select cell  |  Tab/Shift+Tab next/prev cell");
        ImGui::Text("Left/Right field -1/+1  |  %s +16  |  Up/Down change field", k(KEY_EQUAL).c_str());
    } else if (ui.tab == EditorTab::Blocks) {
        ImGui::TextUnformatted("Left/Right value -1/+1  |  Up/Down -4/+4  |  Tab cycle field");
    } else if (ui.tab == EditorTab::Buildblocks) {
        ImGui::TextUnformatted("Left/Right -1/16 +1/16  |  Up/Down -4/16 +4/16  |  Tab cycle layer");
    } else {
        ImGui::TextUnformatted("Click slot select  |  Shift+Click cycle mesh");
    }
}

// Translates keyboard input into the same edit operations the Inspector buttons
// drive. Runs only when ImGui does not want the keyboard and the freecam is not
// flying, so it never collides with text fields or WASD movement.
void handle_editor_hotkeys(EditorProject& project, EditorUi& ui, Camera3D& camera, std::string& status) {
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

    // Global shortcuts.
    if (ctrl && IsKeyPressed(KEY_S)) {
        save_active_tab(project, ui, status);
        return;
    }
    if (IsKeyPressed(KEY_F)) {
        focus_camera_on_city(camera, project);
    }
    // Esc cancels an in-progress gizmo drag, restoring the captured position.
    if (IsKeyPressed(KEY_ESCAPE) && ui.gizmo_active != GizmoAxis::None) {
        if (project.city && !project.city->data.buildings.empty()) {
            auto& placement = project.city->data.buildings[std::min(ui.selected_city_building, project.city->data.buildings.size() - 1U)];
            placement.x = ui.gizmo_drag_origin_x;
            placement.y = ui.gizmo_drag_origin_y;
            placement.z = ui.gizmo_drag_origin_z;
            placement.rotation = ui.gizmo_drag_origin_rot;
        }
        ui.gizmo_active = GizmoAxis::None;
        return;
    }
    // Esc ends an in-progress road-laying chain.
    if (IsKeyPressed(KEY_ESCAPE) && ui.pending_road_intersection.has_value()) {
        end_road_chain(ui);
        return;
    }

    if (ui.tab == EditorTab::City && project.city) {
        if (ui.city_mode == CityEditMode::Buildings && !project.city->data.buildings.empty()) {
            if (IsKeyPressed(KEY_LEFT)) nudge_city_building(project, ui, -1, 0, 0, 0);
            if (IsKeyPressed(KEY_RIGHT)) nudge_city_building(project, ui, 1, 0, 0, 0);
            if (IsKeyPressed(KEY_UP)) nudge_city_building(project, ui, 0, 0, -1, 0);
            if (IsKeyPressed(KEY_DOWN)) nudge_city_building(project, ui, 0, 0, 1, 0);
            if (IsKeyPressed(KEY_PAGE_UP)) nudge_city_building(project, ui, 0, 1, 0, 0);
            if (IsKeyPressed(KEY_PAGE_DOWN)) nudge_city_building(project, ui, 0, -1, 0, 0);
            if (IsKeyPressed(KEY_R)) nudge_city_building(project, ui, 0, 0, 0, shift ? 3 : 1);
            if (ctrl && IsKeyPressed(KEY_D)) duplicate_city_building(project, ui);
            if (IsKeyPressed(KEY_DELETE)) delete_city_building(project, ui);
        } else if (ui.city_mode == CityEditMode::Roads && !project.city->data.streets.empty()) {
            if (IsKeyPressed(KEY_LEFT)) nudge_city_street_endpoint(project, ui, 0, -1);
            if (IsKeyPressed(KEY_RIGHT)) nudge_city_street_endpoint(project, ui, 0, 1);
            if (IsKeyPressed(KEY_UP)) nudge_city_street_endpoint(project, ui, 1, 1);
            if (IsKeyPressed(KEY_DOWN)) nudge_city_street_endpoint(project, ui, 1, -1);
            if (IsKeyPressed(KEY_LEFT_BRACKET)) nudge_city_street_width(project, ui, 0, -1);
            if (IsKeyPressed(KEY_RIGHT_BRACKET)) nudge_city_street_width(project, ui, 0, 1);
            if (IsKeyPressed(KEY_SEMICOLON)) nudge_city_street_width(project, ui, 1, -1);
            if (IsKeyPressed(KEY_APOSTROPHE)) nudge_city_street_width(project, ui, 1, 1);
            if (IsKeyPressed(KEY_MINUS)) nudge_city_street_district(project, ui, -1);
            if (IsKeyPressed(KEY_EQUAL)) nudge_city_street_district(project, ui, 1);
            if (ctrl && IsKeyPressed(KEY_D)) duplicate_city_street(project, ui);
            if (IsKeyPressed(KEY_DELETE)) delete_city_street(project, ui);
        } else if (ui.city_mode == CityEditMode::Pieces) {
            if (IsKeyPressed(KEY_R)) ui.selected_piece_rotation = (ui.selected_piece_rotation + 1U) & 3U;
        }
    } else if (ui.tab == EditorTab::Buildings && !project.buildings.empty()) {
        ui.selected_building = std::min(ui.selected_building, project.buildings.size() - 1U);
        const auto occupied = occupied_cell_indices(project.buildings[ui.selected_building].data);
        if (IsKeyPressed(KEY_LEFT)) nudge_building_cell(project, ui, -1);
        if (IsKeyPressed(KEY_RIGHT)) nudge_building_cell(project, ui, 1);
        if (IsKeyPressed(KEY_EQUAL)) nudge_building_cell(project, ui, 16);
        if (IsKeyPressed(KEY_UP)) {
            ui.selected_building_field = ui.selected_building_field == 0U ? 6U : ui.selected_building_field - 1U;
        }
        if (IsKeyPressed(KEY_DOWN)) {
            ui.selected_building_field = (ui.selected_building_field + 1U) % 7U;
        }
        if (!occupied.empty() && IsKeyPressed(KEY_TAB)) {
            if (shift) {
                ui.selected_building_cell = ui.selected_building_cell == 0U ? occupied.size() - 1U : ui.selected_building_cell - 1U;
            } else {
                ui.selected_building_cell = (ui.selected_building_cell + 1U) % occupied.size();
            }
        }
    } else if (ui.tab == EditorTab::Blocks && !project.blocks.empty()) {
        if (IsKeyPressed(KEY_LEFT)) nudge_block(project, ui, -1);
        if (IsKeyPressed(KEY_RIGHT)) nudge_block(project, ui, 1);
        if (IsKeyPressed(KEY_UP)) nudge_block(project, ui, 4);
        if (IsKeyPressed(KEY_DOWN)) nudge_block(project, ui, -4);
        if (IsKeyPressed(KEY_TAB)) {
            // 14 fields: 0..8 header/flags, 9..13 patch control-point components.
            ui.selected_block_field = shift
                ? (ui.selected_block_field == 0U ? 13U : ui.selected_block_field - 1U)
                : (ui.selected_block_field + 1U) % 14U;
        }
    } else if (ui.tab == EditorTab::Buildblocks && !project.buildblocks.empty()) {
        if (IsKeyPressed(KEY_LEFT)) nudge_buildblock_geometry(project, ui, -1);
        if (IsKeyPressed(KEY_RIGHT)) nudge_buildblock_geometry(project, ui, 1);
        if (IsKeyPressed(KEY_UP)) nudge_buildblock_geometry(project, ui, 4);
        if (IsKeyPressed(KEY_DOWN)) nudge_buildblock_geometry(project, ui, -4);
        ui.selected_buildblock = std::min(ui.selected_buildblock, project.buildblocks.size() - 1U);
        const auto& buildblock = project.buildblocks[ui.selected_buildblock].data;
        if (!buildblock.layers.empty() && IsKeyPressed(KEY_TAB)) {
            const auto count = buildblock.layers.size();
            ui.selected_buildblock_layer = shift
                ? (ui.selected_buildblock_layer == 0U ? count - 1U : ui.selected_buildblock_layer - 1U)
                : (ui.selected_buildblock_layer + 1U) % count;
        }
    }
}


} // namespace map_editor::app_internal

namespace map_editor {

int run_editor_app() {
    using namespace app_internal;
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1440, 900, "map_editor_cpp");
    SetTargetFPS(60);
    // Esc must not quit the app: it is used to cancel a gizmo drag, and closing
    // is gated by the unsaved-changes prompt below. KEY_NULL also removes Esc
    // from WindowShouldClose().
    SetExitKey(KEY_NULL);
    // Push the far clip plane way out (default is 1000). The city spans thousands
    // of world units at 4 units/cell, so the default plane clips distant terrain
    // and buildings. rlgl reads these on every BeginMode3D, so one call sticks.
    rlSetClipPlanes(0.05, 100000.0);

    rlImGuiSetup(true);
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    apply_editor_imgui_style();
    if (io.IniFilename == nullptr || !std::filesystem::exists(io.IniFilename)) {
        load_default_imgui_layout();
    }

    Camera3D camera = default_camera();
    std::optional<EditorProject> project;
    EditorUi ui;
    std::string status = "drop a map folder or click File -> Open Folder";

    RenderTexture2D viewport_texture = LoadRenderTexture(1280, 800);
    Vector2 last_viewport_size = { 1280, 800 };
    Texture2D city_window_texture = load_city_window_gradient_texture();

    bool show_demo_window = false;
    bool show_graphics_debug = false;
    GfxDebugStats gfx_debug_stats;

    // Close gating: a close request (window X, Alt+F4, Exit menu) sets this; with
    // unsaved changes it opens a save prompt and blocks exit until resolved.
    bool running = true;
    bool close_requested = false;
    bool open_unsaved_prompt = false;

    while (running) {
        gfx_debug_stats.reset_frame();
        gfx_debug_stats.push_frame_time(GetFrameTime() * 1000.0F);
        gfx_debug_stats.add_phase_ms(GfxDebugTimingPhase::TotalFrame, GetFrameTime() * 1000.0);
        g_gfx_debug_stats = &gfx_debug_stats;

        // The OS close button / Alt+F4 raise WindowShouldClose(); route it through
        // the same gate as the Exit menu instead of quitting immediately.
        if (WindowShouldClose()) {
            close_requested = true;
        }

        if (IsKeyPressed(KEY_F3)) {
            show_graphics_debug = !show_graphics_debug;
        }

        if (auto folder = dropped_folder()) {
            project = load_project_folder(*folder, status);
            if (project) {
                set_status(*project);
                if (ui.tab == EditorTab::City) {
                    focus_camera_on_city(camera, *project);
                } else {
                    camera = default_camera();
                }
            }
        }

        static Vector2 virtual_mouse_pos = {0, 0};
        static Vector2 viewport_mouse_pos = {0, 0};
        static bool is_viewport_hovered = false;
        const bool show_3d_viewport = !(project && ui.tab == EditorTab::Buildblocks);
        if (!show_3d_viewport) {
            is_viewport_hovered = false;
        }

        if (is_viewport_hovered && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            DisableCursor();
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_RIGHT) && IsCursorHidden()) {
            EnableCursor();
        }
        if (is_viewport_hovered || IsCursorHidden()) {
            ui.camera_fly_speed = adjust_camera_fly_speed(ui.camera_fly_speed, GetMouseWheelMove());
        }
        if (IsCursorHidden()) {
            update_free_fly_camera(camera, ui.camera_fly_speed);
        }

        // Editor hotkeys: only when ImGui is not capturing the keyboard (no active
        // text field) and the freecam is not flying, so they never collide with
        // typing or WASD movement.
        if (project && !io.WantCaptureKeyboard && !IsCursorHidden()) {
            handle_editor_hotkeys(*project, ui, camera, status);
        }

        if (show_3d_viewport && last_viewport_size.x > 0 && last_viewport_size.y > 0) {
            BeginTextureMode(viewport_texture);
            ClearBackground(Color{24, 27, 30, 255});

            BeginMode3D(camera);
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
                        const auto* selected_street = project->city->data.streets.empty()
                            ? nullptr
                            : &project->city->data.streets[std::min(ui.selected_city_street, project->city->data.streets.size() - 1U)];
                        const auto scene_fingerprint = city_scene_fingerprint(project->city->data);
                        if (!project->city_scene_cache_valid) {
                            // First scene for this project: build synchronously so
                            // there is always something to show on load. Only edits
                            // (below) go to the background worker.
                            const auto placement_buildings = resolve_placement_buildings(*project);
                            project->city_scene_cache = build_map_scene(project->city->data, placement_buildings);
                            project->city_scene_cache_fingerprint = scene_fingerprint;
                            project->city_scene_cache_valid = true;
                            project->city_scene_gpu_valid = false;
                        } else if (project->city_scene_cache_fingerprint != scene_fingerprint &&
                                   !project->city_scene_build_in_progress) {
                            // The map changed: rebuild the scene (roads + terrain
                            // field + footprint stamping) on a worker thread. The
                            // last-good scene keeps rendering until it is ready.
                            start_city_scene_rebuild(*project, scene_fingerprint);
                        }

                        // Adopt a finished background rebuild if it still matches
                        // the current map; otherwise discard it as stale and let the
                        // next frame kick a fresh build.
                        if (project->city_scene_build_future.has_value() &&
                            project->city_scene_build_future->wait_for(std::chrono::milliseconds{0}) == std::future_status::ready) {
                            auto built = project->city_scene_build_future->get();
                            project->city_scene_build_future.reset();
                            project->city_scene_build_in_progress = false;
                            if (project->city_scene_build_target_fingerprint == scene_fingerprint) {
                                project->city_scene_cache = std::move(built);
                                project->city_scene_cache_fingerprint = scene_fingerprint;
                                project->city_scene_gpu_valid = false;
                            }
                        }
                        const auto game_preview = ui.city_view_mode == CityViewMode::GamePreview;
                        if (game_preview && !project->city_scene_gpu_valid) {
                            unload_city_render_model(project->city_tile_gpu_model);
                            unload_city_render_model(project->terrain_gpu_model);
                            project->city_tile_gpu_model =
                                upload_city_tile_model(project->city_scene_cache.city_tile_buffer, *project);
                            if (project->city_scene_cache.terrain.has_value()) {
                                project->terrain_gpu_model =
                                    upload_city_terrain_model(project->city_scene_cache.terrain->mesh, *project);
                            }
                            project->city_scene_gpu_valid = true;
                        }
                        const auto& city_scene = project->city_scene_cache;
                        if (game_preview) {
                            draw_city_scene(*project, city_scene, selected_street, ui.render_roads, ui.render_terrain);
                        } else {
                            draw_city_editor_overlay(*project, city_scene);
                        }
                        // Hover preview: outline the building a click would select.
                        const SbcBuilding* hovered_building = nullptr;
                        if (ui.city_mode == CityEditMode::Buildings && is_viewport_hovered &&
                            !IsCursorHidden() && ui.gizmo_active == GizmoAxis::None) {
                            const auto pick_ray = GetMouseRay(virtual_mouse_pos, camera);
                            const Vec3 ro{pick_ray.position.x, pick_ray.position.y, pick_ray.position.z};
                            const Vec3 rd{pick_ray.direction.x, pick_ray.direction.y, pick_ray.direction.z};
                            const auto placements = resolve_placement_buildings(*project);
                            const auto picked = pick_city_building(
                                project->city->data, placements, ro, rd, project->city_scene_cache.unit_size);
                            if (picked.has_value() && *picked < project->city->data.buildings.size()) {
                                hovered_building = &project->city->data.buildings[*picked];
                            }
                        }
                        {
                            GfxDebugScopedTimer timer{gfx_debug_stats, GfxDebugTimingPhase::BuildingDraw};
                            draw_city_building_models_full_detail(*project, selected_building, &city_window_texture, hovered_building);
                        }
                        {
                            GfxDebugScopedTimer timer{gfx_debug_stats, GfxDebugTimingPhase::BuildingDraw};
                            draw_city_piece_voxels(*project, ui.portal_preview_textured);
                        }
                        if (game_preview) {
                            GfxDebugScopedTimer timer{gfx_debug_stats, GfxDebugTimingPhase::PoleDraw};
                            draw_city_traffic_light_poles(city_scene);
                        }

                        const auto city_cursor = city_grid_point_under_mouse(camera, virtual_mouse_pos);
                        if (city_cursor.has_value()) {
                            draw_city_cursor(*city_cursor, 4.0F, ui.city_mode);
                        }
                        draw_road_lay_preview(*project, ui, city_cursor);

                        // Drag gizmo on the selected building. Hover-highlights the
                        // handle under the cursor unless a drag is already active.
                        if (ui.city_mode == CityEditMode::Buildings && !project->city->data.buildings.empty()) {
                            const auto sel = std::min(ui.selected_city_building, project->city->data.buildings.size() - 1U);
                            const auto& placement = project->city->data.buildings[sel];
                            const auto hovered = ui.gizmo_active != GizmoAxis::None
                                ? ui.gizmo_active
                                : (is_viewport_hovered && !IsCursorHidden()
                                       ? pick_building_gizmo(camera, virtual_mouse_pos, placement)
                                       : GizmoAxis::None);
                            draw_building_gizmo(placement, hovered);
                        }
                    } else if (ui.tab == EditorTab::Buildings && !project->buildings.empty()) {
                        ui.selected_building = std::min(ui.selected_building, project->buildings.size() - 1U);
                        const auto occupied = occupied_cell_indices(project->buildings[ui.selected_building].data);
                        const auto* selected = occupied.empty()
                            ? nullptr
                            : &project->buildings[ui.selected_building].data.cells[occupied[std::min(ui.selected_building_cell, occupied.size() - 1U)]];
                        if (ui.selected_building < project->buildings.size()) {
                            // Whole building (nullopt) or a single chosen floor.
                            const std::optional<std::uint32_t> preview_floor = ui.building_view_floor_only
                                ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(std::max(0, ui.building_view_floor)))
                                : std::nullopt;
                            auto& preview_model = building_tool_floor_preview_model(*project, ui.selected_building, preview_floor);
                            GfxDebugScopedTimer timer{gfx_debug_stats, GfxDebugTimingPhase::BuildingDraw};
                            draw_sbb_scene_for_app(
                                project->buildings[ui.selected_building].data,
                                preview_model,
                                selected);
                        }
                    } else if (ui.tab == EditorTab::Pieces && project->city && !project->city->data.city_blocks.empty()) {
                        ui.selected_city_block = std::min(ui.selected_city_block, project->city->data.city_blocks.size() - 1U);
                        const auto& city_block = project->city->data.city_blocks[ui.selected_city_block];
                        if (!city_block.layers.empty()) {
                            ui.selected_city_block_layer = std::min(ui.selected_city_block_layer, city_block.layers.size() - 1U);
                            ui.selected_city_block_slot = std::min<std::size_t>(ui.selected_city_block_slot, 7U);
                        }
                        draw_city_block_preview(city_block, block_meshes(*project), ui.selected_city_block_layer, ui.selected_city_block_slot);
                    } else if (ui.tab == EditorTab::Blocks && !project->blocks.empty()) {
                        ui.selected_block = std::min(ui.selected_block, project->blocks.size() - 1U);
                        draw_mesh_faces(project->blocks[ui.selected_block].data, 4.0F, {98, 148, 204, 220});
                    } else {
                        DrawGrid(64, 4.0F);
                    }
                } else {
                    DrawGrid(64, 4.0F);
                }
            }
            EndMode3D();
            EndTextureMode();
        }

        if (show_3d_viewport && project && is_viewport_hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !IsCursorHidden()) {
            if (ui.tab == EditorTab::City && project->city) {
                const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
                const auto city_cursor = city_grid_point_under_mouse(camera, virtual_mouse_pos);
                const auto pick_ray = GetMouseRay(virtual_mouse_pos, camera);
                const Vec3 ray_origin{pick_ray.position.x, pick_ray.position.y, pick_ray.position.z};
                const Vec3 ray_dir{pick_ray.direction.x, pick_ray.direction.y, pick_ray.direction.z};
                if (ui.city_mode == CityEditMode::Buildings) {
                    // A plain click first tests the gizmo handles of the selected
                    // building; hitting one starts a drag instead of reselecting.
                    GizmoAxis gizmo_hit = GizmoAxis::None;
                    if (!shift && !ctrl && !project->city->data.buildings.empty()) {
                        const auto sel = std::min(ui.selected_city_building, project->city->data.buildings.size() - 1U);
                        gizmo_hit = pick_building_gizmo(camera, virtual_mouse_pos, project->city->data.buildings[sel]);
                    }
                    if (gizmo_hit != GizmoAxis::None) {
                        const auto& placement = project->city->data.buildings[std::min(ui.selected_city_building, project->city->data.buildings.size() - 1U)];
                        ui.gizmo_active = gizmo_hit;
                        ui.gizmo_drag_origin_x = placement.x;
                        ui.gizmo_drag_origin_y = placement.y;
                        ui.gizmo_drag_origin_z = placement.z;
                        ui.gizmo_drag_origin_rot = placement.rotation;
                    } else if (shift && ctrl) {
                        if (city_cursor.has_value()) place_city_building_copy(*project, ui, *city_cursor);
                    } else if (shift) {
                        if (city_cursor.has_value()) set_city_building_position(*project, ui, *city_cursor);
                    } else {
                        const auto placements = resolve_placement_buildings(*project);
                        const auto picked = pick_city_building(
                            project->city->data, placements, ray_origin, ray_dir, project->city_scene_cache.unit_size);
                        if (picked.has_value()) {
                            ui.selected_city_building = *picked;
                        }
                    }
                } else if (ui.city_mode == CityEditMode::Roads) {
                    // Plain click lays the road chain (snap to a node or create one,
                    // axis-aligned, chained). Shift+click selects a road to edit.
                    if (shift) {
                        const auto picked = pick_city_road(
                            project->city->data, project->city_scene_cache, ray_origin, ray_dir);
                        if (picked.has_value()) {
                            ui.selected_city_street = *picked;
                        }
                    } else {
                        if (city_cursor.has_value()) append_city_road_point(*project, ui, *city_cursor);
                    }
                } else if (ui.city_mode == CityEditMode::Pieces) {
                    // Plain click selects/moves the grid cursor; Shift stamps a
                    // voxel; Ctrl clears one.
                    if (city_cursor.has_value()) {
                        if (ctrl) {
                            clear_city_piece_voxel(*project, *city_cursor, ui);
                        } else if (shift) {
                            set_city_piece_voxel(*project, ui, *city_cursor);
                        }
                    }
                }
            } else if (ui.tab == EditorTab::Pieces && project->city && !project->city->data.city_blocks.empty()) {
                const auto piece_cursor = city_block_slot_under_mouse(camera, virtual_mouse_pos);
                if (piece_cursor.has_value()) {
                    ui.selected_city_block_layer = std::min(ui.selected_city_block_layer, piece_cursor->first);
                    ui.selected_city_block_slot = piece_cursor->second;
                    if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
                        nudge_city_block_slot(*project, ui, 1);
                    }
                }
            } else if (ui.tab == EditorTab::Buildings && !project->buildings.empty()) {
                select_building_cell_from_viewport(*project, ui, camera, viewport_mouse_pos, last_viewport_size, status);
            }
        }

        // Gizmo drag: while the mouse is held after grabbing a handle, map the
        // cursor to the building's new grid position; release ends the drag.
        if (ui.gizmo_active != GizmoAxis::None) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && project && project->city) {
                apply_building_gizmo_drag(*project, ui, camera, virtual_mouse_pos);
            }
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                ui.gizmo_active = GizmoAxis::None;
            }
        }

        BeginDrawing();
        ClearBackground(DARKGRAY);

        const auto imgui_begin = std::chrono::steady_clock::now();
        rlImGuiBegin();

        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

        bool open_folder_requested = false;
        bool reset_layout_requested = false;

        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open Folder...", "Ctrl+O")) {
                    open_folder_requested = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Alt+F4")) {
                    close_requested = true;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Window")) {
                if (ImGui::MenuItem("Reset Dock Layout")) {
                    reset_layout_requested = true;
                }
                ImGui::MenuItem("Show ImGui Demo Window", nullptr, &show_demo_window);
                ImGui::MenuItem("Graphics Debug", "F3", &show_graphics_debug);
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        if (show_demo_window) {
            ImGui::ShowDemoWindow(&show_demo_window);
        }
        if (reset_layout_requested) {
            load_default_imgui_layout();
        }

        is_viewport_hovered = false;
        if (show_3d_viewport) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("Viewport 3D");
            {
                is_viewport_hovered = ImGui::IsWindowHovered();
                ImVec2 size = ImGui::GetContentRegionAvail();

                if ((size.x != last_viewport_size.x || size.y != last_viewport_size.y) && size.x > 0 && size.y > 0) {
                    UnloadRenderTexture(viewport_texture);
                    viewport_texture = LoadRenderTexture(static_cast<int>(size.x), static_cast<int>(size.y));
                    last_viewport_size = { size.x, size.y };
                }

                rlImGuiImageRenderTexture(&viewport_texture);

                ImVec2 min_pos = ImGui::GetWindowPos();
                min_pos.x += ImGui::GetWindowContentRegionMin().x;
                min_pos.y += ImGui::GetWindowContentRegionMin().y;

                virtual_mouse_pos = get_virtual_mouse_position(Vector2{min_pos.x, min_pos.y}, Vector2{size.x, size.y});
                viewport_mouse_pos = get_viewport_mouse_position(Vector2{min_pos.x, min_pos.y}, Vector2{size.x, size.y});
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }

        ImGui::Begin("Asset Browser");
        if (!project) {
            ImGui::Text("No project loaded.");
            if (ImGui::Button("Open Folder")) {
                open_folder_requested = true;
            }
        } else {
            ImGui::Text("Project: %s", project->root.filename().string().c_str());
            ImGui::Separator();

            const char* tab_names[] = { "City##asset_tab_city", "Buildings##asset_tab_buildings", "Pieces##asset_tab_pieces", "Buildblocks##asset_tab_buildblocks", "Blocks##asset_tab_blocks" };
            for (int t = 0; t < 5; ++t) {
                bool is_active = (static_cast<int>(ui.tab) == t);
                if (is_active) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
                }
                if (ImGui::Button(tab_names[t], ImVec2(ImGui::GetContentRegionAvail().x / (5.0f - t) - 4, 30))) {
                    ui.tab = static_cast<EditorTab>(t);
                    if (ui.tab == EditorTab::City) {
                        focus_camera_on_city(camera, *project);
                    } else {
                        camera = default_camera();
                    }
                }
                if (is_active) {
                    ImGui::PopStyleColor();
                }
                if (t < 4) ImGui::SameLine();
            }

            ImGui::Separator();
            ImGui::Spacing();

            if (ui.tab == EditorTab::City) {
                if (project->city) {
                    ImGui::Text("city2.sbc");
                    
                    int mode_idx = static_cast<int>(ui.city_mode);
                    ImGui::RadioButton("Roads", &mode_idx, 0); ImGui::SameLine();
                    ImGui::RadioButton("Buildings", &mode_idx, 1); ImGui::SameLine();
                    ImGui::RadioButton("Pieces", &mode_idx, 2);
                    const auto new_mode = static_cast<CityEditMode>(mode_idx);
                    if (new_mode != ui.city_mode) {
                        end_road_chain(ui);  // don't carry a pending start across modes
                    }
                    ui.city_mode = new_mode;

                    if (ui.city_mode == CityEditMode::Buildings) {
                        ImGui::TextDisabled("Click: select / drag gizmo  |  Shift+Click: move  |  Ctrl+Shift+Click: copy");
                    } else if (ui.city_mode == CityEditMode::Roads) {
                        if (ui.pending_road_intersection.has_value()) {
                            ImGui::TextDisabled("Click: extend (axis-locked)  |  click start node or Esc: end  |  Shift+Click: select");
                        } else {
                            ImGui::TextDisabled("Click: start road (snaps to node)  |  Shift+Click: select road");
                        }
                    } else {
                        ImGui::TextDisabled("Shift+Click: place  |  Ctrl+Click: clear  |  %s: rotate", key_label(KEY_R).c_str());
                    }

                    ImGui::Separator();
                    int view_idx = ui.city_view_mode == CityViewMode::GamePreview ? 0 : 1;
                    ImGui::TextUnformatted("View:");
                    ImGui::RadioButton("Game preview", &view_idx, 0); ImGui::SameLine();
                    ImGui::RadioButton("Editor view", &view_idx, 1);
                    ui.city_view_mode = view_idx == 0 ? CityViewMode::GamePreview : CityViewMode::Editor;

                    ImGui::TextUnformatted("Game preview layers:");
                    ImGui::Checkbox("Roads##render_roads", &ui.render_roads); ImGui::SameLine();
                    ImGui::Checkbox("Terrain##render_terrain", &ui.render_terrain);
                    ImGui::Separator();

                    if (ui.city_mode == CityEditMode::Roads && !project->city->data.streets.empty()) {
                        auto names = city_street_names(project->city->data);
                        ImGui::Text("Roads (%zu):", names.size());
                        if (ImGui::BeginListBox("##roads", ImVec2(-FLT_MIN, -FLT_MIN))) {
                            for (std::size_t i = 0; i < names.size(); ++i) {
                                ImGui::PushID(static_cast<int>(i));
                                const bool selected = (ui.selected_city_street == i);
                                if (ImGui::Selectable(names[i].c_str(), selected)) {
                                    ui.selected_city_street = i;
                                }
                                ImGui::PopID();
                            }
                            ImGui::EndListBox();
                        }
                    } else if (ui.city_mode == CityEditMode::Buildings && !project->city->data.buildings.empty()) {
                        auto names = city_building_names(project->city->data);
                        ImGui::Text("Placed Buildings (%zu):", names.size());
                        if (ImGui::BeginListBox("##placed_bldgs", ImVec2(-FLT_MIN, -FLT_MIN))) {
                            for (std::size_t i = 0; i < names.size(); ++i) {
                                ImGui::PushID(static_cast<int>(i));
                                const bool selected = (ui.selected_city_building == i);
                                if (ImGui::Selectable(names[i].c_str(), selected)) {
                                    ui.selected_city_building = i;
                                }
                                ImGui::PopID();
                            }
                            ImGui::EndListBox();
                        }
                    } else if (ui.city_mode == CityEditMode::Pieces && !project->city->data.city_blocks.empty()) {
                        auto names = city_block_names(project->city->data);
                        ImGui::Text("Piece Blocks (%zu):", names.size());
                        const auto list_height = project->city->data.portals.empty()
                            ? -FLT_MIN
                            : std::max(120.0F, ImGui::GetContentRegionAvail().y * 0.42F);
                        if (ImGui::BeginListBox("##piece_blocks", ImVec2(-FLT_MIN, list_height))) {
                            for (std::size_t i = 0; i < names.size(); ++i) {
                                ImGui::PushID(static_cast<int>(i));
                                const bool selected = (ui.selected_city_block == i);
                                if (ImGui::Selectable(names[i].c_str(), selected)) {
                                    ui.selected_city_block = i;
                                }
                                ImGui::PopID();
                            }
                            ImGui::EndListBox();
                        }
                        if (!project->city->data.portals.empty()) {
                            ui.selected_portal = std::min(ui.selected_portal, project->city->data.portals.size() - 1U);
                            auto portal_items = portal_names(project->city->data);
                            ImGui::SeparatorText("Portals");
                            if (ImGui::BeginListBox("##portal_preview_portals", ImVec2(-FLT_MIN, -FLT_MIN))) {
                                for (std::size_t i = 0; i < portal_items.size(); ++i) {
                                    ImGui::PushID(static_cast<int>(i));
                                    const bool selected = (ui.selected_portal == i);
                                    if (ImGui::Selectable(portal_items[i].c_str(), selected)) {
                                        ui.selected_portal = i;
                                    }
                                    ImGui::PopID();
                                }
                                ImGui::EndListBox();
                            }
                        }
                    }
                } else {
                    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "city2.sbc missing");
                }
            } else if (ui.tab == EditorTab::Buildings) {
                draw_building_asset_panel(*project, ui);
            } else if (ui.tab == EditorTab::Pieces) {
                if (project->city && !project->city->data.city_blocks.empty()) {
                    auto names = city_block_names(project->city->data);
                    ImGui::Text("City Block Templates:");
                    if (ImGui::BeginListBox("##city_pieces", ImVec2(-FLT_MIN, -FLT_MIN))) {
                        for (std::size_t i = 0; i < names.size(); ++i) {
                            ImGui::PushID(static_cast<int>(i));
                            const bool selected = (ui.selected_city_block == i);
                            if (ImGui::Selectable(names[i].c_str(), selected)) {
                                ui.selected_city_block = i;
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndListBox();
                    }
                }
            } else if (ui.tab == EditorTab::Blocks) {
                auto names = block_names(*project);
                ImGui::Text("Blocks (*.sbl):");
                if (ImGui::BeginListBox("##blocks", ImVec2(-FLT_MIN, -FLT_MIN))) {
                    for (std::size_t i = 0; i < names.size(); ++i) {
                        ImGui::PushID(static_cast<int>(i));
                        const bool selected = (ui.selected_block == i);
                        if (ImGui::Selectable(names[i].c_str(), selected)) {
                            ui.selected_block = i;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndListBox();
                }
            } else if (ui.tab == EditorTab::Buildblocks) {
                draw_buildblock_gallery(*project, ui);
            }
        }
        ImGui::End();

        ImGui::Begin("Inspector");
        if (project) {
            draw_controls_help(ui);
            if (ui.tab == EditorTab::City && project->city) {
                if (ui.city_mode == CityEditMode::Roads && !project->city->data.streets.empty()) {
                    ui.selected_city_street = std::min(ui.selected_city_street, project->city->data.streets.size() - 1U);
                    auto& street = project->city->data.streets[ui.selected_city_street];
                    ImGui::Text("Road Inspector");
                    ImGui::Separator();
                    ImGui::Text("Name: %s", street.name.c_str());
                    ImGui::Text("From Node: %d", street.from_intersection);
                    ImGui::Text("To Node: %d", street.to_intersection);
                    ImGui::Text("Left Width: %d | Right Width: %d", street.left_width, street.right_width);
                    ImGui::Text("District: %d", street.district);

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Text("Actions:");
                    
                    if (ImGui::Button("F-")) nudge_city_street_endpoint(*project, ui, 0, -1);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decrement from node");
                    ImGui::SameLine();
                    if (ImGui::Button("F+")) nudge_city_street_endpoint(*project, ui, 0, 1);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Increment from node");
                    ImGui::SameLine();
                    if (ImGui::Button("T-")) nudge_city_street_endpoint(*project, ui, 1, -1);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decrement to node");
                    ImGui::SameLine();
                    if (ImGui::Button("T+")) nudge_city_street_endpoint(*project, ui, 1, 1);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Increment to node");

                    if (ImGui::Button("L-")) nudge_city_street_width(*project, ui, 0, -1);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decrement left width");
                    ImGui::SameLine();
                    if (ImGui::Button("L+")) nudge_city_street_width(*project, ui, 0, 1);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Increment left width");
                    ImGui::SameLine();
                    if (ImGui::Button("R-")) nudge_city_street_width(*project, ui, 1, -1);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decrement right width");
                    ImGui::SameLine();
                    if (ImGui::Button("R+")) nudge_city_street_width(*project, ui, 1, 1);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Increment right width");

                    if (ImGui::Button("D-")) nudge_city_street_district(*project, ui, -1);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decrement district index");
                    ImGui::SameLine();
                    if (ImGui::Button("D+")) nudge_city_street_district(*project, ui, 1);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Increment district index");
                    ImGui::SameLine();
                    if (ImGui::Button("Duplicate")) duplicate_city_street(*project, ui);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Duplicate current street (Ctrl+D)");
                    ImGui::SameLine();
                    if (ImGui::Button("Delete")) delete_city_street(*project, ui);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete current street (Del)");

                    ImGui::Spacing();
                    if (ImGui::Button(ui.city_dirty ? "Save City *" : "Save City", ImVec2(-FLT_MIN, 35))) {
                        save_city(*project, ui, status);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save city changes to city2.edited.sbc");
                } else if (ui.city_mode == CityEditMode::Buildings && !project->city->data.buildings.empty()) {
                    ui.selected_city_building = std::min(ui.selected_city_building, project->city->data.buildings.size() - 1U);
                    auto& building = project->city->data.buildings[ui.selected_city_building];
                    ImGui::Text("Building Placement");
                    ImGui::Separator();
                    ImGui::Text("Name: %s", building.name.c_str());
                    ImGui::Text("Position: [%d, %d, %d]", building.x, building.y, building.z);
                    ImGui::Text("Rotation: %d", building.rotation);

                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::Text("Actions:");
                    if (ImGui::Button("X-")) nudge_city_building(*project, ui, -1, 0, 0, 0);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nudge X left");
                    ImGui::SameLine();
                    if (ImGui::Button("X+")) nudge_city_building(*project, ui, 1, 0, 0, 0);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nudge X right");
                    ImGui::SameLine();
                    if (ImGui::Button("Z-")) nudge_city_building(*project, ui, 0, 0, -1, 0);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nudge Z back");
                    ImGui::SameLine();
                    if (ImGui::Button("Z+")) nudge_city_building(*project, ui, 0, 0, 1, 0);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nudge Z forward");

                    if (ImGui::Button("Y-")) nudge_city_building(*project, ui, 0, -1, 0, 0);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nudge Y down");
                    ImGui::SameLine();
                    if (ImGui::Button("Y+")) nudge_city_building(*project, ui, 0, 1, 0, 0);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nudge Y up");
                    ImGui::SameLine();
                    if (ImGui::Button("Rotate")) nudge_city_building(*project, ui, 0, 0, 0, 1);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotate building 90 degrees (%s, Shift+%s back)", key_label(KEY_R).c_str(), key_label(KEY_R).c_str());

                    if (ImGui::Button("Duplicate")) duplicate_city_building(*project, ui);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Duplicate current building (Ctrl+%s)", key_label(KEY_D).c_str());
                    ImGui::SameLine();
                    if (ImGui::Button("Delete")) delete_city_building(*project, ui);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete building placement (Del)");
                    ImGui::SameLine();
                    if (ImGui::Button(ui.city_dirty ? "Save City *" : "Save City")) {
                        save_city(*project, ui, status);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save city changes to city2.edited.sbc");
                } else if (ui.city_mode == CityEditMode::Pieces && !project->city->data.city_blocks.empty()) {
                    ui.selected_city_block = std::min(ui.selected_city_block, project->city->data.city_blocks.size() - 1U);
                    auto& city_block = project->city->data.city_blocks[ui.selected_city_block];
                    const auto block_label = city_block_label(city_block);
                    ImGui::Text("City Piece Placement");
                    ImGui::Separator();
                    ImGui::Text("Block Name: %s", block_label.c_str());
                    ImGui::Text("Layer: %zu | Rotation: %zu", ui.selected_city_block_layer, ui.selected_piece_rotation);
                    if (!project->city->data.portals.empty()) {
                        ui.selected_portal = std::min(ui.selected_portal, project->city->data.portals.size() - 1U);
                        const auto& portal = project->city->data.portals[ui.selected_portal];
                        ImGui::Text("Portal Preview: %zu [%d, %d, %d]", ui.selected_portal, portal.fields[1], portal.fields[2], portal.fields[3]);
                        int portal_view = ui.portal_preview_textured ? 1 : 0;
                        ImGui::RadioButton("Flat##portal_preview_view", &portal_view, 0);
                        ImGui::SameLine();
                        ImGui::RadioButton("Textured##portal_preview_view", &portal_view, 1);
                        ui.portal_preview_textured = portal_view == 1;
                        if (ImGui::CollapsingHeader("Portal Details", ImGuiTreeNodeFlags_DefaultOpen)) {
                            draw_portal_details(portal);
                        }
                    }

                    ImGui::Spacing();
                    ImGui::Separator();
                    if (ImGui::Button("Rotate Block")) {
                        ui.selected_piece_rotation = (ui.selected_piece_rotation + 1U) & 3U;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rotate voxel before placing");
                    ImGui::TextWrapped("Click in the 3D viewport to place piece. Ctrl+click to clear.");
                    
                    ImGui::Spacing();
                    if (ImGui::Button(ui.city_dirty ? "Save City *" : "Save City", ImVec2(-FLT_MIN, 35))) {
                        save_city(*project, ui, status);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save city changes to city2.edited.sbc");
                }
            } else if (ui.tab == EditorTab::Buildings) {
                draw_building_inspector(*project, ui, status);
            } else if (ui.tab == EditorTab::Pieces && project->city && !project->city->data.city_blocks.empty()) {
                ui.selected_city_block = std::min(ui.selected_city_block, project->city->data.city_blocks.size() - 1U);
                auto& city_block = project->city->data.city_blocks[ui.selected_city_block];
                const auto block_label = city_block_label(city_block);
                
                ImGui::Text("City Block: %s", block_label.c_str());
                ImGui::Separator();
                
                if (!city_block.layers.empty()) {
                    ui.selected_city_block_layer = std::min(ui.selected_city_block_layer, city_block.layers.size() - 1U);
                    ui.selected_city_block_slot = std::min<std::size_t>(ui.selected_city_block_slot, 7U);
                    const auto& slot_name = city_block.layers[ui.selected_city_block_layer][ui.selected_city_block_slot];
                    
                    ImGui::Text("Layer: %zu | Slot: %zu", ui.selected_city_block_layer, ui.selected_city_block_slot);
                    ImGui::Text("Voxel Mesh: %s", slot_name.empty() ? "(empty)" : slot_name.c_str());
                    
                    if (ImGui::Button("Layer-") && ui.selected_city_block_layer > 0) {
                        --ui.selected_city_block_layer;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decrement layer");
                    ImGui::SameLine();
                    if (ImGui::Button("Layer+") && ui.selected_city_block_layer + 1U < city_block.layers.size()) {
                        ++ui.selected_city_block_layer;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Increment layer");
                    
                    if (ImGui::Button("Slot-")) {
                        ui.selected_city_block_slot = ui.selected_city_block_slot == 0 ? 7U : ui.selected_city_block_slot - 1U;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select previous piece slot");
                    ImGui::SameLine();
                    if (ImGui::Button("Slot+")) {
                        ui.selected_city_block_slot = (ui.selected_city_block_slot + 1U) % 8U;
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select next piece slot");
                    
                    if (ImGui::Button("Clear Slot")) clear_city_block_slot(*project, ui);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Empty current voxel slot");
                    ImGui::SameLine();
                    if (ImGui::Button("Next Mesh")) nudge_city_block_slot(*project, ui, 1);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cycle block mesh in slot");
                    
                    ImGui::Spacing();
                    if (ImGui::Button(ui.city_dirty ? "Save City *" : "Save City", ImVec2(-FLT_MIN, 35))) {
                        save_city(*project, ui, status);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save voxel changes to city2.edited.sbc");
                }
            } else if (ui.tab == EditorTab::Blocks) {
                draw_block_inspector(*project, ui, status);
            } else if (ui.tab == EditorTab::Buildblocks) {
                draw_buildblock_inspector(*project, ui, status);
            }
        } else {
            ImGui::Text("Please load a project.");
        }
        ImGui::End();

        if (project &&
            ui.tab == EditorTab::City &&
            project->city &&
            ui.city_mode == CityEditMode::Pieces &&
            !project->city->data.portals.empty()) {
            ImGui::Begin("2D Preview");
            static RenderTexture2D preview_texture = LoadRenderTexture(300, 300);
            ImVec2 p_size = ImGui::GetContentRegionAvail();
            if (p_size.x > 0 && p_size.y > 0) {
                static ImVec2 last_p_size = {300, 300};
                if (p_size.x != last_p_size.x || p_size.y != last_p_size.y) {
                    UnloadRenderTexture(preview_texture);
                    preview_texture = LoadRenderTexture(static_cast<int>(p_size.x), static_cast<int>(p_size.y));
                    last_p_size = p_size;
                }

                BeginTextureMode(preview_texture);
                ClearBackground(Color{24, 27, 30, 255});

                if (project->city && !project->city->data.portals.empty()) {
                    ui.selected_portal = std::min(ui.selected_portal, project->city->data.portals.size() - 1U);
                    const auto& portal = project->city->data.portals[ui.selected_portal];
                    const auto target = Vector3{
                        static_cast<float>(portal.fields[1] * 8 + 4) * 4.0F,
                        static_cast<float>(portal.fields[2] * 8 + 4) * 4.0F,
                        static_cast<float>(portal.fields[3] * 8 + 4) * 4.0F,
                    };
                    const Camera3D preview_camera{
                        {target.x + 34.0F, target.y + 28.0F, target.z + 34.0F},
                        target,
                        {0.0F, 1.0F, 0.0F},
                        45.0F,
                        CAMERA_PERSPECTIVE,
                    };
                    BeginMode3D(preview_camera);
                    DrawGrid(16, 4.0F);
                    draw_city_piece_voxels(*project, ui.portal_preview_textured, ui.selected_portal);
                    EndMode3D();
                    DrawText(
                        ui.portal_preview_textured ? "portal preview: textured" : "portal preview: flat",
                        12,
                        12,
                        14,
                        RAYWHITE);
                }

                EndTextureMode();
                rlImGuiImageRenderTexture(&preview_texture);
            }
            ImGui::End();
        }

        if (project && ui.tab == EditorTab::Buildblocks) {
            ImGui::Begin("Buildblock 2D Preview");
            draw_buildblock_preview_panel(*project, ui);
            ImGui::End();
        }

        ImGui::Begin("Console / Status");
        ImGui::Text("Status: %s", status.c_str());
        if (project) {
            ImGui::Text("Render: %s", project->status.c_str());
            ImGui::Separator();
            ImGui::BeginChild("render-log", ImVec2(0, 120), true);
            for (const auto& line : project->render_logs) {
                ImGui::TextUnformatted(line.c_str());
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                ImGui::SetScrollHereY(1.0F);
            }
            ImGui::EndChild();
        }
        ImGui::Text("Camera Pos: [%.2f, %.2f, %.2f]", camera.position.x, camera.position.y, camera.position.z);
        ImGui::Text("Fly Speed: %.1f", ui.camera_fly_speed);
        ImGui::End();

        const auto imgui_end = std::chrono::steady_clock::now();
        gfx_debug_stats.add_phase_ms(
            GfxDebugTimingPhase::ImGuiUi,
            std::chrono::duration<double, std::milli>(imgui_end - imgui_begin).count());
        refresh_gfx_debug_counts(gfx_debug_stats, project.has_value() ? &*project : nullptr);
        gfx_debug_stats.counts.total_scene_triangles = gfx_debug_stats.total_draw().triangles;
        draw_graphics_debug_window(show_graphics_debug, gfx_debug_stats);

        if (open_folder_requested) {
            auto result = pfd::select_folder("Open SRC map folder").result();
            if (!result.empty()) {
                project = load_project_folder(result, status);
                if (project) {
                    set_status(*project);
                    if (ui.tab == EditorTab::City) {
                        focus_camera_on_city(camera, *project);
                    } else {
                        camera = default_camera();
                    }
                }
            }
        }

        // Resolve a pending close request. Clean -> exit now. Dirty -> raise the
        // save prompt (once) and keep running until the user resolves it.
        if (close_requested) {
            close_requested = false;
            if (project && has_unsaved_changes(ui)) {
                open_unsaved_prompt = true;
            } else {
                running = false;
            }
        }
        if (open_unsaved_prompt) {
            ImGui::OpenPopup("Unsaved Changes");
            open_unsaved_prompt = false;
        }
        if (ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("You have unsaved changes. Save before exiting?");
            ImGui::Spacing();
            if (ImGui::Button("Save & Exit", ImVec2(120, 0))) {
                if (project) {
                    save_all_dirty(*project, ui, status);
                }
                running = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Discard & Exit", ImVec2(120, 0))) {
                running = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        rlImGuiEnd();
        EndDrawing();
        g_gfx_debug_stats = nullptr;
    }

    UnloadRenderTexture(viewport_texture);
    if (city_window_texture.id != 0U) {
        UnloadTexture(city_window_texture);
    }
    if (IsCursorHidden()) {
        EnableCursor();
    }
    rlImGuiShutdown();
    CloseWindow();
    return 0;
}

} // namespace map_editor
