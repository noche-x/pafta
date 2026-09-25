#include "app.hpp"
#include "app_internal.hpp"
#include "mesh_export.hpp"
#include "buildblock_catalog.hpp"
#include <stdexcept>
#include "profiler.hpp"

#include <raylib.h>
#include <rlgl.h>
#include <raymath.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <rlImGui.h>

#ifndef __EMSCRIPTEN__
#include <portable-file-dialogs.h>
#include <nfd.hpp>
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
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

// raylib's bundled GLFW desktop backend exports these (they are defined in
// libraylib.a and raylib calls glfwGetKeyName internally), but raylib does not
// re-export them. Forward-declaring lets us show layout-correct key labels
// without pulling in the GLFW header or touching the build graph.
#ifndef __EMSCRIPTEN__
extern "C" const char* glfwGetKeyName(int key, int scancode);
extern "C" int glfwGetKeyScancode(int key);
#endif

namespace map_editor::app_internal {

// A failure the user must see: status keeps showing it, and the "Operation
// failed" popup opens once for it. Plain status text never opens the popup.
std::string g_failure_message;
void report_failure(std::string& status, std::string message) {
    status = message;
    g_failure_message = std::move(message);
}

#ifdef __EMSCRIPTEN__
// Filled by web/file_upload.js after the browser copies a selected directory
// into Emscripten's virtual filesystem. The editor consumes it at the same
// deferred point as a native folder drop, after the current frame is presented.
std::optional<std::filesystem::path> g_web_uploaded_folder;

// Loading a project is deliberately synchronous because raylib texture uploads
// must run on the browser's GL thread. The caller yields once before loading;
// nested Asyncify sleeps here accumulate suspended C++ parser stacks and can
// fail to resume on large projects.
void report_web_load_progress(std::string_view stage, int percent) {
    const auto message = std::string{"Loading "} + std::string{stage} + "... "
        + std::to_string(std::clamp(percent, 0, 100)) + "%";
    EM_ASM({
        const node = document.getElementById('status');
        if (node) node.textContent = UTF8ToString($0);
    }, message.c_str());
}

// The browser build has no writable disk; saved files go to the user as
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

extern "C" EMSCRIPTEN_KEEPALIVE void map_editor_web_files_ready(const char* root) {
    if (root != nullptr && root[0] != '\0') {
        g_web_uploaded_folder = std::filesystem::path{root};
    }
}
#endif

// Display label for a key, localized to the user's keyboard layout. raylib's
// IsKeyPressed checks physical key POSITION (US-QWERTY scancodes), so the
// movement/edit keys keep the same physical spots on every layout; this only
// changes what we PRINT, e.g. the physical W position shows "Z" on AZERTY.
// glfwGetKeyName returns the layout label for printable keys and null for
// non-printable ones (arrows, Tab, Delete...), which fall back to a fixed name.
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
        // glfwGetKeyName returns UTF-8; only uppercase plain ASCII bytes so a
        // multi-byte sequence (e.g. an accented layout key) is never split into
        // an invalid byte that the font then renders as a box.
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

std::vector<std::filesystem::path> project_archive_paths(const EditorProject& project) {
    auto paths = files_with_extension(project.root, ".csx");
    if (project.city) {
        auto companion = project.city->path;
        companion.replace_extension(".csx");
        const auto match = std::find(paths.begin(), paths.end(), companion);
        if (match != paths.end()) std::rotate(match, std::next(match), paths.end());
    }
    return paths;
}

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

void configure_texture(Texture2D& texture) {
    GenTextureMipmaps(&texture);
    SetTextureWrap(texture, TEXTURE_WRAP_REPEAT);
    SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);
    SetTextureFilter(texture, TEXTURE_FILTER_ANISOTROPIC_16X);
}

// Runs body(i) for i in [0, count) across a bounded set of worker threads (capped
// at hardware_concurrency, min 1), blocking until all complete. body must be
// thread-safe for distinct i. Used to fan out the CPU-bound load-time work (PNG
// decode, CSX parse) without spawning one thread per item.
template <typename Body>
void parallel_for(std::size_t count, Body&& body) {
    if (count == 0) {
        return;
    }
#ifdef __EMSCRIPTEN__
    // This target does not enable pthreads. std::async workers cannot run while
    // the main browser thread waits on their futures, so load assets serially.
    for (std::size_t i = 0; i < count; ++i) {
        body(i);
    }
    return;
#else
    const unsigned hw = std::max(1U, std::thread::hardware_concurrency());
    const std::size_t workers = std::min<std::size_t>(count, hw);
    if (workers <= 1) {
        for (std::size_t i = 0; i < count; ++i) {
            body(i);
        }
        return;
    }
    std::atomic<std::size_t> next{0};
    std::vector<std::future<void>> futures;
    futures.reserve(workers);
    for (std::size_t w = 0; w < workers; ++w) {
        futures.push_back(std::async(std::launch::async, [&next, count, &body] {
            for (std::size_t i = next.fetch_add(1); i < count; i = next.fetch_add(1)) {
                body(i);
            }
        }));
    }
    for (auto& future : futures) {
        future.get();
    }
#endif
}

// editor.exe LoadTexture_PNG_ToBuffer @14003ab40 writes each source row into dest
// row (height-1 - row), i.e. it flips PNGs vertically at load for OpenGL's
// bottom-left texture origin. Block UVs are authored against that flipped image, so
// raylib (which uploads PNG rows top-down) must flip too or every texture is
// upside-down. Only obvious on textures with vertical orientation (text/figures).
//
// CPU-only decode stage (no GL): produces a flipped raylib Image. Safe to run on a
// worker thread; raylib's LoadImage*/ImageFlipVertical touch only local pixel data
// (stbi has no shared state in the default config). The GL upload stays on the main
// thread in upload_decoded_image.
Image decode_flipped_png_from_file(const std::filesystem::path& path) {
    Image image = LoadImage(path.string().c_str());
    if (!IsImageValid(image)) {
        return Image{};
    }
    ImageFlipVertical(&image);
    return image;
}

Image decode_flipped_embedded_png(const std::vector<std::uint8_t>& payload, bool material) {
    if (payload.size() < 0x4cU || read_le_u32(payload, 0U) != 1U) {
        return Image{};
    }
    const auto color_size = static_cast<std::size_t>(read_le_u32(payload, 0x44U));
    const auto material_size = static_cast<std::size_t>(read_le_u32(payload, 0x48U));
    const auto image_offset = 0x4cU + (material ? color_size : 0U);
    const auto image_size = material ? material_size : color_size;
    if (image_size == 0U || image_offset > payload.size() || image_size > payload.size() - image_offset) {
        return Image{};
    }
    const auto* image_bytes = payload.data() + image_offset;
    if (image_size < 2U || image_bytes[0] != 0x89U || image_bytes[1] != 0x50U) {
        return Image{};
    }
    Image image = LoadImageFromMemory(".png", image_bytes, static_cast<int>(image_size));
    if (!IsImageValid(image)) {
        return Image{};
    }
    ImageFlipVertical(&image);
    return image;
}

// GL upload stage (main thread only). Consumes the decoded Image and returns a
// Texture2D (id==0 on failure, which the registration pass skips). Frees the image.
Texture2D upload_decoded_image(Image image) {
    if (!IsImageValid(image)) {
        return Texture2D{};
    }
    Texture2D texture = LoadTextureFromImage(image);
    UnloadImage(image);
    return texture;
}

const Texture2D* texture_for_material(const EditorProject& project, std::string_view material_name) {
    if (material_name.empty()) {
        return nullptr;
    }
    const auto lowered = lower_name(material_name);
    for (const auto& tex : project.textures) {
        if (lower_name(tex.name) == lowered) {
            return &tex.texture;
        }
    }
    return nullptr;
}

void load_project_textures(EditorProject& project) {
    MAPED_PROFILE_FUNCTION();
#ifdef __EMSCRIPTEN__
    report_web_load_progress("textures", 63);
#endif
    std::unordered_map<std::string, std::size_t> loaded_names;
    std::unordered_map<std::string, std::size_t> loaded_material_names;

    // One decode task: a name, whether it overrides (CSX-wins upsert) or only adds,
    // and a closure that produces the flipped Image off the main thread. The decode
    // (PNG/stbi) is CPU-bound and dominated load time; the GL upload below stays
    // serial on the main thread. Order is preserved exactly: texture/*.png, then
    // root *.png, then CSX-embedded textures in archive order — texture indices and
    // the CSX-wins merge depend on this sequence.
    struct DecodeTask {
        std::string name;
        bool upsert;
        bool material;
        std::function<Image()> decode;
    };
    std::vector<DecodeTask> tasks;

    for (const auto& path : files_with_extension(project.root / "texture", ".png")) {
        tasks.push_back({stem_name(path), false, false, [path] { return decode_flipped_png_from_file(path); }});
    }
    for (const auto& path : files_with_extension(project.root / "texturematerial", ".png")) {
        tasks.push_back({stem_name(path), false, true, [path] { return decode_flipped_png_from_file(path); }});
    }
    for (const auto& path : files_with_extension(project.root, ".png")) {
        auto name = stem_name(path);
        const auto material = name.ends_with("_material");
        if (material) {
            name.resize(name.size() - std::string_view{"_material"}.size());
        }
        tasks.push_back({std::move(name), false, material, [path] { return decode_flipped_png_from_file(path); }});
    }
    // CSX archives are parsed in parallel; the texture entries they expose are then
    // appended as decode tasks in archive order (matching the previous serial walk).
    {
        MAPED_PROFILE_BLOCK("parse csx archives (textures)");
        const auto csx_paths = project_archive_paths(project);
        std::vector<CsxArchive> archives(csx_paths.size());
        parallel_for(csx_paths.size(), [&](std::size_t i) { archives[i] = parse_csx_file(csx_paths[i]); });
        for (auto& archive : archives) {
            for (const auto& entry : archive.entries) {
                if (entry.kind != CsxEntryKind::Texture) {
                    continue;
                }
                tasks.push_back({entry.name, true, false, [payload = entry.payload] {
                                     return decode_flipped_embedded_png(payload, false);
                                 }});
                tasks.push_back({entry.name, true, true, [payload = entry.payload] {
                                     return decode_flipped_embedded_png(payload, true);
                                 }});
            }
        }
    }

#ifdef __EMSCRIPTEN__
    report_web_load_progress("decode textures", 68);
#endif

    // Parallel decode stage: each task's PNG is decoded + flipped on a worker. The
    // results are collected in task order so the serial upload below is identical to
    // the old single-threaded order.
    std::vector<Image> decoded(tasks.size());
    {
        MAPED_PROFILE_BLOCK("parallel png decode");
        parallel_for(tasks.size(), [&](std::size_t i) { decoded[i] = tasks[i].decode(); });
    }

    // Serial GL upload + registration on the main thread, in original order.
    MAPED_PROFILE_BLOCK("upload textures");
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        Texture2D texture = upload_decoded_image(decoded[i]);
        auto& target = tasks[i].material ? project.material_textures : project.textures;
        auto& names = tasks[i].material ? loaded_material_names : loaded_names;
        if (tasks[i].upsert) {
            if (texture.id != 0U) {
                const auto key = lower_name(tasks[i].name);
                configure_texture(texture);
                if (const auto existing = names.find(key); existing != names.end()) {
                    target[existing->second] = LoadedTexture{std::move(tasks[i].name), texture};
                } else {
                    names.emplace(key, target.size());
                    target.emplace_back(std::move(tasks[i].name), texture);
                }
            }
        } else {
            if (texture.id == 0U) {
                continue;
            }
            const auto key = lower_name(tasks[i].name);
            if (names.contains(key)) {
                UnloadTexture(texture);
                continue;
            }
            configure_texture(texture);
            names.emplace(key, target.size());
            target.emplace_back(std::move(tasks[i].name), texture);
        }
    }

#ifdef __EMSCRIPTEN__
    report_web_load_progress("upload textures", 73);
#endif
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

// A .csx-packed .sbb begins with the container version sentinel 1 (verified
// against subrosa.exe SbbContainer_ReadWrite @00196d60, which hard-checks the
// leading dword == 1), whereas a loose building/*.sbb begins with its real
// version 12-14 (Sbb_LoadFromFile @00026830 gates `version - 12 < 3`). The packed
// body is otherwise laid out exactly like a loose version-14 file: anchors at
// +0x4c/+0x50/+0x54 and the trailing aux/LOD records are always present. So a
// packed SBB is parsed by rewriting its leading sentinel to 14 and handing it to
// the unchanged loose-file parser. Returns the payload unchanged when the leading
// word is not the container sentinel (already a real loose-version file).
std::vector<std::uint8_t> normalize_packed_sbb_bytes(const std::vector<std::uint8_t>& payload) {
    constexpr std::uint32_t container_sentinel = 1U;
    constexpr std::uint32_t loose_version_14 = 0x0eU;
    if (payload.size() < 4U || read_le_u32(payload, 0U) != container_sentinel) {
        return payload;
    }
    std::vector<std::uint8_t> normalized = payload;
    normalized[0] = static_cast<std::uint8_t>(loose_version_14 & 0xFFU);
    normalized[1] = static_cast<std::uint8_t>((loose_version_14 >> 8U) & 0xFFU);
    normalized[2] = static_cast<std::uint8_t>((loose_version_14 >> 16U) & 0xFFU);
    normalized[3] = static_cast<std::uint8_t>((loose_version_14 >> 24U) & 0xFFU);
    return normalized;
}

// CSX-packed .sbl blocks/buildblocks carry the same container sentinel version 1,
// but their body is the loose version-2 layout (with the trailing light/exterior
// face section). parse_sbl only reads that section when version > 1, so a sentinel-1
// block loses its exterior faces (renders as a single authored patch or nothing).
// Rewrite the leading sentinel 1 -> 2 so the loose parser reads the full mesh.
// Leaves real loose-version files (2) untouched.
std::vector<std::uint8_t> normalize_packed_sbl_bytes(const std::vector<std::uint8_t>& payload) {
    constexpr std::uint32_t container_sentinel = 1U;
    constexpr std::uint32_t loose_version_2 = 2U;
    if (payload.size() < 4U || read_le_u32(payload, 0U) != container_sentinel) {
        return payload;
    }
    if (!parse_sbl(payload).block) return payload;
    std::vector<std::uint8_t> normalized = payload;
    normalized[0] = static_cast<std::uint8_t>(loose_version_2 & 0xFFU);
    normalized[1] = 0U;
    normalized[2] = 0U;
    normalized[3] = 0U;
    return normalized;
}

// Loads SBB buildings and SBL blocks/buildblocks packed inside .csx archives in
// the project root, the same way a distributed map ships its geometry (sbl + sbb
// + png; never the city .sbc). Textures are already pulled from .csx by
// load_project_textures, so this only handles the geometry entries.
//
// Merge policy is CSX-wins: a CSX entry whose name matches an already-loaded
// loose-folder asset overrides that slot in place, keeping the asset vector and
// its parallel *_bytes vector index-aligned (placement resolution and the SBB
// patch/rebuild writers index them together).
void load_project_csx_geometry(EditorProject& project) {
    MAPED_PROFILE_FUNCTION();
#ifdef __EMSCRIPTEN__
    report_web_load_progress("archives", 76);
#endif
    // Upserts data + raw bytes keyed by lowercased stem, preserving index
    // alignment between the asset vector and its parallel bytes vector.
    const auto upsert = [](auto& assets, auto& bytes_list, const std::string& name,
                           const std::filesystem::path& source, auto data,
                           std::vector<std::uint8_t> bytes) {
        const auto key = lower_name(name);
        for (std::size_t i = 0; i < assets.size(); ++i) {
            if (lower_name(assets[i].name) == key) {
                assets[i].path = source;
                assets[i].name = name;
                assets[i].data = std::move(data);
                bytes_list[i] = std::move(bytes);
                return;
            }
        }
        assets.push_back({source, name, std::move(data)});
        bytes_list.push_back(std::move(bytes));
    };

#ifdef __EMSCRIPTEN__
    // A browser has one GL thread and a much tighter linear-memory budget. Do
    // not retain every archive payload plus every parsed SBB/SBL at once; merge
    // each entry immediately in the same archive order as the desktop path.
    for (const auto& path : project_archive_paths(project)) {
        try {
            auto archive = parse_csx_file(path);
            for (const auto& entry : archive.entries) {
                const auto name = std::filesystem::path{entry.name}.stem().string();
                if (name.empty()) {
                    continue;
                }
                try {
                    if (entry.kind == CsxEntryKind::Sbb) {
                        auto bytes = normalize_packed_sbb_bytes(entry.payload);
                        const auto existing = std::ranges::find_if(project.buildings, [&name](const auto& asset) {
                            return lower_name(asset.name) == lower_name(name);
                        });
                        if (existing != project.buildings.end()) {
                            const auto index = static_cast<std::size_t>(std::distance(project.buildings.begin(), existing));
                            if (index < project.building_bytes.size() && project.building_bytes[index] == bytes) {
                                continue;
                            }
                        }
                        auto building = parse_sbb(bytes);
                        upsert(project.buildings, project.building_bytes, name, path,
                               std::move(building), std::move(bytes));
                    } else if (entry.kind == CsxEntryKind::Sbl) {
                        auto bytes = normalize_packed_sbl_bytes(entry.payload);
                        const auto block = std::ranges::find_if(project.blocks, [&name](const auto& asset) {
                            return lower_name(asset.name) == lower_name(name);
                        });
                        if (block != project.blocks.end()) {
                            const auto index = static_cast<std::size_t>(std::distance(project.blocks.begin(), block));
                            if (index < project.block_bytes.size() && project.block_bytes[index] == bytes) {
                                continue;
                            }
                        }
                        const auto buildblock = std::ranges::find_if(project.buildblocks, [&name](const auto& asset) {
                            return lower_name(asset.name) == lower_name(name);
                        });
                        if (buildblock != project.buildblocks.end()) {
                            const auto index = static_cast<std::size_t>(std::distance(project.buildblocks.begin(), buildblock));
                            if (index < project.buildblock_bytes.size() && project.buildblock_bytes[index] == bytes) {
                                continue;
                            }
                        }
                        auto parsed = parse_sbl(bytes);
                        if (parsed.block.has_value()) {
                            upsert(project.blocks, project.block_bytes, name, path,
                                   *parsed.block, std::move(bytes));
                        } else if (!parsed.layers.empty()) {
                            upsert(project.buildblocks, project.buildblock_bytes, name, path,
                                   std::move(parsed), std::move(bytes));
                        }
                    }
                } catch (const std::exception& error) {
                    project.load_warnings.push_back(
                        path.filename().string() + " [" + name + "]: " + error.what());
                }
            }
        } catch (const std::exception& error) {
            project.load_warnings.push_back(path.filename().string() + ": " + error.what());
        }
    }
    return;
#endif

    // One parsed geometry entry, ready to upsert. Parsing (parse_sbb/parse_sbl) is
    // CPU-bound and done off the main thread; the upsert below is serial because the
    // CSX-wins merge is order-dependent (it overrides matching slots in place).
    enum class GeometryKind { None, Sbb, Sbl };
    struct ParsedGeometry {
        GeometryKind kind{GeometryKind::None};
        std::string name;
        std::filesystem::path source;
        std::vector<std::uint8_t> bytes;
        std::optional<SbbBuilding> building;
        std::optional<SblTemplate> sbl;
        std::string warning;  // non-empty => push to load_warnings, skip the upsert
    };

    // Flatten every CSX entry across all archives into a work list (in archive +
    // entry order), parsing the archives themselves in parallel first.
    const auto csx_paths = project_archive_paths(project);
    std::vector<CsxArchive> archives(csx_paths.size());
    std::vector<std::string> archive_warnings(csx_paths.size());
    parallel_for(csx_paths.size(), [&](std::size_t i) {
        try {
            archives[i] = parse_csx_file(csx_paths[i]);
        } catch (const std::exception& error) {
            archive_warnings[i] = csx_paths[i].filename().string() + ": " + error.what();
        }
    });

    struct EntryRef {
        std::size_t archive_index;
        std::size_t entry_index;
    };
    std::vector<EntryRef> entry_refs;
    for (std::size_t a = 0; a < archives.size(); ++a) {
        if (!archive_warnings[a].empty()) {
            project.load_warnings.push_back(std::move(archive_warnings[a]));
            continue;
        }
        for (std::size_t e = 0; e < archives[a].entries.size(); ++e) {
            entry_refs.push_back({a, e});
        }
    }

    // Parse SBB/SBL geometry for every entry in parallel into an order-preserving
    // result buffer.
    std::vector<ParsedGeometry> parsed(entry_refs.size());
    parallel_for(entry_refs.size(), [&](std::size_t i) {
        const auto& ref = entry_refs[i];
        const auto& entry = archives[ref.archive_index].entries[ref.entry_index];
        const auto& path = csx_paths[ref.archive_index];
        const auto name = std::filesystem::path{entry.name}.stem().string();
        if (name.empty()) {
            return;
        }
        ParsedGeometry out;
        out.name = name;
        out.source = path;
        try {
            if (entry.kind == CsxEntryKind::Sbb) {
                out.bytes = normalize_packed_sbb_bytes(entry.payload);
                out.building = parse_sbb(out.bytes);
                out.kind = GeometryKind::Sbb;
            } else if (entry.kind == CsxEntryKind::Sbl) {
                out.bytes = normalize_packed_sbl_bytes(entry.payload);
                out.sbl = parse_sbl(out.bytes);
                out.kind = GeometryKind::Sbl;
            }
        } catch (const std::exception& error) {
            out.kind = GeometryKind::None;
            out.warning = path.filename().string() + " [" + name + "]: " + error.what();
        }
        parsed[i] = std::move(out);
    });

    // Serial merge in original order (CSX-wins upsert is order-dependent).
    for (auto& item : parsed) {
        if (!item.warning.empty()) {
            project.load_warnings.push_back(std::move(item.warning));
            continue;
        }
        if (item.kind == GeometryKind::Sbb) {
            upsert(project.buildings, project.building_bytes, item.name, item.source,
                   std::move(*item.building), std::move(item.bytes));
        } else if (item.kind == GeometryKind::Sbl) {
            if (item.sbl->block.has_value()) {
                upsert(project.blocks, project.block_bytes, item.name, item.source,
                       *item.sbl->block, std::move(item.bytes));
            } else if (!item.sbl->layers.empty()) {
                upsert(project.buildblocks, project.buildblock_bytes, item.name, item.source,
                       std::move(*item.sbl), std::move(item.bytes));
            }
        }
    }

}

std::optional<EditorProject> load_project_folder(const std::filesystem::path& folder, std::string& error,
    bool load_textures = true, std::atomic<int>* stage = nullptr) {
    MAPED_PROFILE_FUNCTION();
    const auto input = std::filesystem::weakly_canonical(folder);
    const bool map_file = std::filesystem::is_regular_file(input) && lower_extension(input) == ".sbc";
    const auto root = map_file ? input.parent_path() : input;
    if (!std::filesystem::is_directory(root)) {
        error = "not a folder: " + root.string();
        return std::nullopt;
    }

    EditorProject project;
    project.root = root;

#ifdef __EMSCRIPTEN__
    report_web_load_progress("map", 5);
#endif

    auto city_path = map_file ? input : root / "city2.sbc";
    if (!std::filesystem::exists(city_path)) {
        const auto maps = files_with_extension(root, ".sbc");
        if (maps.size() == 1) city_path = maps.front();
        else if (maps.size() > 1) project.load_warnings.push_back("Multiple city files: keep one SBC per map folder.");
    }
    if (std::filesystem::exists(city_path)) {
        MAPED_PROFILE_BLOCK("load city2.sbc");
        try {
            project.city_bytes = read_file_bytes(city_path);
            project.city = LoadedAsset<SbcMap>{city_path, city_path.stem().string(), parse_sbc(*project.city_bytes)};
        } catch (const std::exception& error) {
            project.city_bytes.reset();
            project.city.reset();
            project.load_warnings.push_back(city_path.filename().string() + ": " + error.what());
        }
        MAPED_PROFILE_END_BLOCK;
    }
    if (stage) stage->store(1);
#ifdef __EMSCRIPTEN__
    report_web_load_progress("buildings", 20);
#endif

    // Loose assets are read+parsed in parallel into an order-preserving buffer, then
    // appended in directory order (asset/byte vectors stay index-aligned, and other
    // code indexes them positionally). read_file_bytes + parse_sbb/parse_sbl are
    // CPU/IO-bound and were the heaviest serial load steps (loose building SBBs in
    // particular). One result slot per file; failures carry a warning instead.
    MAPED_PROFILE_BLOCK("load loose building sbb");
    {
        const auto paths = files_with_extension(root / "building", ".sbb");
        struct LooseSbb {
            std::filesystem::path path;
            std::optional<SbbBuilding> building;
            std::vector<std::uint8_t> bytes;
            std::string warning;
        };
        std::vector<LooseSbb> results(paths.size());
        parallel_for(paths.size(), [&](std::size_t i) {
            results[i].path = paths[i];
            try {
                results[i].bytes = read_file_bytes(paths[i]);
                results[i].building = parse_sbb(results[i].bytes);
            } catch (const std::exception& error) {
                results[i].building.reset();
                results[i].warning = paths[i].filename().string() + ": " + error.what();
            }
        });
        for (auto& r : results) {
            if (!r.warning.empty()) {
                project.load_warnings.push_back(std::move(r.warning));
                continue;
            }
            project.buildings.push_back({r.path, stem_name(r.path), std::move(*r.building)});
            project.building_bytes.push_back(std::move(r.bytes));
        }
    }
    MAPED_PROFILE_END_BLOCK;
    if (stage) stage->store(2);
#ifdef __EMSCRIPTEN__
    report_web_load_progress("blocks", 38);
#endif

    MAPED_PROFILE_BLOCK("load loose block sbl");
    {
        const auto paths = files_with_extension(root / "block", ".sbl");
        struct LooseSbl {
            std::filesystem::path path;
            std::optional<SblTemplate> parsed;
            std::vector<std::uint8_t> bytes;
            std::string warning;
        };
        std::vector<LooseSbl> results(paths.size());
        parallel_for(paths.size(), [&](std::size_t i) {
            results[i].path = paths[i];
            try {
                results[i].bytes = read_file_bytes(paths[i]);
                results[i].parsed = parse_sbl(results[i].bytes);
            } catch (const std::exception& error) {
                results[i].parsed.reset();
                results[i].warning = paths[i].filename().string() + ": " + error.what();
            }
        });
        for (auto& r : results) {
            if (!r.warning.empty()) {
                project.load_warnings.push_back(std::move(r.warning));
                continue;
            }
            if (r.parsed->block.has_value()) {
                project.blocks.push_back({r.path, stem_name(r.path), *r.parsed->block});
                project.block_bytes.push_back(std::move(r.bytes));
            }
        }
    }
    MAPED_PROFILE_END_BLOCK;
    if (stage) stage->store(3);
#ifdef __EMSCRIPTEN__
    report_web_load_progress("buildblocks", 50);
#endif

    MAPED_PROFILE_BLOCK("load loose buildblock sbl");
    {
        const auto paths = files_with_extension(root / "buildblock", ".sbl");
        struct LooseSbl {
            std::filesystem::path path;
            std::optional<SblTemplate> parsed;
            std::vector<std::uint8_t> bytes;
            std::string warning;
        };
        std::vector<LooseSbl> results(paths.size());
        parallel_for(paths.size(), [&](std::size_t i) {
            results[i].path = paths[i];
            try {
                results[i].bytes = read_file_bytes(paths[i]);
                results[i].parsed = parse_sbl(results[i].bytes);
            } catch (const std::exception& error) {
                results[i].parsed.reset();
                results[i].warning = paths[i].filename().string() + ": " + error.what();
            }
        });
        for (auto& r : results) {
            if (!r.warning.empty()) {
                project.load_warnings.push_back(std::move(r.warning));
                continue;
            }
            if (!r.parsed->layers.empty()) {
                project.buildblocks.push_back({r.path, stem_name(r.path), std::move(*r.parsed)});
                project.buildblock_bytes.push_back(std::move(r.bytes));
            }
        }
    }
    MAPED_PROFILE_END_BLOCK;
    if (stage) stage->store(4);

    if (load_textures) {
        MAPED_PROFILE_BLOCK("load project textures");
        load_project_textures(project);
        MAPED_PROFILE_END_BLOCK;
    }
    MAPED_PROFILE_BLOCK("load project csx geometry");
    load_project_csx_geometry(project);
    MAPED_PROFILE_END_BLOCK;
    if (stage) stage->store(5);

    if (!project.city && project.buildings.empty() && project.blocks.empty() && project.buildblocks.empty()) {
        error = "folder does not look like a map folder: " + root.string();
        return std::nullopt;
    }

    project.render_buffers_dirty = true;
    project.status = "loaded " + root.filename().string()
        + " (" + std::to_string(project.buildings.size()) + " buildings, "
        + std::to_string(project.blocks.size()) + " blocks, "
        + std::to_string(project.textures.size()) + " color maps, "
        + std::to_string(project.material_textures.size()) + " material maps)";
    if (!project.load_warnings.empty()) {
        project.status += " | " + std::to_string(project.load_warnings.size()) + " skipped";
        for (const auto& warning : project.load_warnings) {
            project.render_logs.push_back("skipped " + warning);
        }
    }
    return project;
}

// Builds the block-mesh catalog from the loaded SBL blocks (a native built-in set
// prepended via build_native_block_mesh_catalog). Pure function of project.blocks.
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

// Loaded texture names in load order. v10 district pieces index this list via the
// legacy texture grid to resolve per-face materials.
std::vector<std::string> build_texture_names(const EditorProject& project) {
    std::vector<std::string> names;
    names.reserve(project.textures.size());
    for (const auto& texture : project.textures) {
        names.push_back(texture.name);
    }
    return names;
}

// Fills the derived render-input caches (block meshes, buildblock templates,
// texture names) once. Cheap to call every frame: it returns immediately when the
// cache is already valid. The grid texture names parse CSX archives from disk, so
// they have their own validity flag and are only rebuilt when invalidated.
void ensure_render_input_cache(EditorProject& project) {
    if (!project.render_input_cache_valid) {
        MAPED_PROFILE_BLOCK("rebuild render input cache");
        project.cached_block_meshes = build_block_meshes(project);
        project.cached_buildblock_templates = build_buildblock_templates(project);
        project.cached_texture_names = build_texture_names(project);
        project.render_input_cache_valid = true;
    }
    if (!project.grid_texture_names_valid) {
        MAPED_PROFILE_BLOCK("rebuild grid texture names");
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

const std::vector<std::string>& project_texture_names(EditorProject& project) {
    ensure_render_input_cache(project);
    return project.cached_texture_names;
}

const std::vector<std::string>& project_grid_texture_names(EditorProject& project) {
    ensure_render_input_cache(project);
    return project.cached_grid_texture_names;
}

// The v10 district legacy texture grid stores ABSOLUTE indices into the game's
// runtime texture table, whose order is fixed by World_InitResourcePoolsAndCatalogs
// @0x13e408 + World_LoadMapAndInitializeTerrain @0x56b10:
//   index 0      = "concrete"  (built-in, texture/concrete.png)
//   index 1,2    = reserved built-in slots
//   index 3      = the generated default texture (DAT_64cff7e4 = 3)
//   index 4..N   = the map's CSX 0x585a0004 textures IN ARCHIVE ORDER
//     (DAT_64cff560 starts at 4 and increments per CSX texture entry).
// asphalt2.png / asphalt2_material.png / map.png load into fixed model slots, NOT
// this indexed table, so they must NOT shift the indices. This builds the matching
// name list so a district cell's grid index resolves to the same texture the game
// uses. Built-in indices 0-3 are the game's "concrete" + default grey, which live
// in the game's base data/texture dir (not the map folder), so the editor can't
// load them by name; map them to "asphalt2" — the grey road texture the editor
// does have — so plaza/sidewalk cells read grey like in-game. CSX names follow in
// archive order.
//
// This parses every CSX archive from disk, so it is only run when the project's
// CSX set changes (load). The result is cached in project.cached_grid_texture_names
// by ensure_render_input_cache; the per-frame render path reads the cache.
std::vector<std::string> build_grid_texture_names(const EditorProject& project) {
    MAPED_PROFILE_FUNCTION();
    std::vector<std::string> names = {"asphalt2", "asphalt2", "asphalt2", "asphalt2"};
    for (const auto& path : project_archive_paths(project)) {
        const auto archive = parse_csx_file(path);
        for (const auto& entry : archive.entries) {
            if (entry.kind == CsxEntryKind::Texture) {
                names.push_back(entry.name);
            }
        }
    }
    return names;
}

void write_file_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
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

// Suggested default .gltf output path for the active view, or empty with a reason
// in `reason` when the current tab has nothing to export. No OS dialog (pfd's
// osascript save dialog is unreliable on macOS); the path is chosen in-app.
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

// Exports the active view to glTF at an explicit path (no dialog). Builds the right
// geometry and writes <path>.gltf + .bin + .png. Sets `status` to the result.
void run_mesh_export(EditorProject& project, const EditorUi& ui, const std::filesystem::path& path,
                     const ExportOptions& options, std::string& status) {
#ifdef __EMSCRIPTEN__
    (void)project;
    (void)ui;
    (void)path;
    (void)options;
    status = "Export is available in the desktop editor.";
    return;
#else
    MAPED_PROFILE_FUNCTION();
    if (path.empty()) {
        report_failure(status, "Export failed: empty path.");
        return;
    }

    if (ui.tab == EditorTab::Buildings) {
        if (project.buildings.empty()) {
            report_failure(status, "Export failed: no building loaded.");
            return;
        }
        const auto index = std::min(ui.selected_building, project.buildings.size() - 1U);
        const auto& asset = project.buildings[index];
        const auto blocks = block_meshes(project);
        const auto templates = buildblock_templates(project);
        const auto buffer = build_native_building_render_buffer(asset.data, blocks, templates);
        const auto result = export_building_to_gltf(path, buffer, asset.name, project, &asset.data.materials, options);
        if (result.ok) status.clear(); else report_failure(status, "Export failed: " + result.message);
        return;
    }

    if (ui.tab == EditorTab::Blocks) {
        if (project.blocks.empty()) {
            report_failure(status, "Export failed: no block loaded.");
            return;
        }
        const auto index = std::min(ui.selected_block, project.blocks.size() - 1U);
        const auto& asset = project.blocks[index];
        const auto result = export_block_to_gltf(path, asset.data, asset.name, project);
        if (result.ok) status.clear(); else report_failure(status, "Export failed: " + result.message);
        return;
    }

    if (ui.tab == EditorTab::City) {
        if (!project.city) {
            report_failure(status, "Export failed: no city loaded.");
            return;
        }
        const auto placement_buildings = resolve_placement_buildings(project);
        if (options.include_buildings) {
            for (std::size_t i = 0; i < placement_buildings.size(); ++i) if (placement_buildings[i] == nullptr) {
                report_failure(status, "Export failed: missing or ambiguous building " + project.city->data.buildings[i].name);
                return;
            }
        }
        const auto blocks = block_meshes(project);
        const auto templates = buildblock_templates(project);
        const auto scene = build_map_scene(project.city->data, placement_buildings);
        std::vector<BuildingRenderBuffer> placement_buffers;
        placement_buffers.reserve(scene.buildings.size());
        for (std::size_t i = 0; i < scene.buildings.size(); ++i) {
            const SbbBuilding* sbb = i < placement_buildings.size() ? placement_buildings[i] : nullptr;
            placement_buffers.push_back(
                sbb == nullptr ? BuildingRenderBuffer{}
                               : build_native_building_render_buffer(*sbb, blocks, templates));
        }
        // Piece/portal geometry (districts placed with the Piece tool), the same
        // textured buffer the city view draws via the portal render model.
        const auto piece_buffer = build_native_portal_render_buffer(
            project.city->data, blocks, std::nullopt, templates, project_grid_texture_names(project));
        const auto result = export_city_to_gltf(path, scene, placement_buffers, placement_buildings,
                                     piece_buffer, project, options);
        if (result.ok) status.clear(); else report_failure(status, "Export failed: " + result.message);
        return;
    }

    report_failure(status, "Export failed: this view has no mesh to export.");
#endif
}

// Launches a background rebuild of the city scene (roads + terrain field +
// footprint stamping) for the given fingerprint. The worker owns copies of the
// map and the resolved .sbb data so nothing it touches can be mutated or freed by
// the main thread mid-build. GPU upload stays on the main thread (see the caller).
void start_city_scene_rebuild(EditorProject& project, std::uint64_t fingerprint) {
    MAPED_PROFILE_FUNCTION();
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
        const auto asset = find_building_asset(project, placement.name);
        const bool found = asset != nullptr;
        owned->push_back(found ? asset->data : SbbBuilding{});
        resolved.push_back(found ? 1 : 0);
    }

    project.city_scene_build_in_progress = true;
    project.city_scene_build_target_fingerprint = fingerprint;
    const auto generation = ++project.city_scene_build_generation;
    auto build_scene = [map = std::move(map_copy), owned, resolved = std::move(resolved), generation]() {
            MAPED_PROFILE_THREAD("City scene rebuild");
            MAPED_PROFILE_BLOCK("build async map scene");
            std::vector<const SbbBuilding*> placement_buildings;
            placement_buildings.reserve(owned->size());
            for (std::size_t i = 0; i < owned->size(); ++i) {
                placement_buildings.push_back(resolved[i] != 0 ? &(*owned)[i] : nullptr);
            }
            (void)generation;
            return build_map_scene(map, placement_buildings);
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

void draw_city_building_models_full_detail(
    EditorProject& project,
    const SbcBuilding* selected_building,
    const Camera3D& camera,
    Vector2 viewport_size,
    const SbcBuilding* hovered_building = nullptr,
    int render_quality = 1) {
    if (!project.city) {
        return;
    }

    struct DrawEntry {
        std::size_t placement_index{};
        std::size_t asset_index{};
    };

    // Extract the 6 world-space frustum planes from the camera's view-projection
    // matrix (Gribb-Hartmann), once per frame. A sphere is culled only when it is
    // fully outside one plane, which is exact at every angle — no cone approximation
    // like the previous hand-rolled test that blinked buildings out at grazing views.
    struct Plane {
        float a, b, c, d;
    };
    std::array<Plane, 6> frustum_planes{};
    bool frustum_valid = false;
    if (viewport_size.x > 0.0F && viewport_size.y > 0.0F) {
        const auto aspect = viewport_size.x / viewport_size.y;
        const auto view = GetCameraMatrix(camera);
        const auto proj = MatrixPerspective(
            camera.fovy * 0.017453292519943295,
            static_cast<double>(aspect),
            rlGetCullDistanceNear(),
            rlGetCullDistanceFar());
        const auto m = MatrixMultiply(view, proj);
        // Rows of the view-proj give the planes; m is column-major in raylib's
        // Matrix (m0,m4,m8,m12 = first column).
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

        // Cull only if the bounding sphere is fully on the outside of any plane.
        for (const auto& p : frustum_planes) {
            const auto distance = p.a * center.x + p.b * center.y + p.c * center.z + p.d;
            if (distance < -radius) {
                return false;
            }
        }
        return true;
    };

    const auto& placement_asset_indices = resolve_placement_asset_indices(project);
    std::vector<DrawEntry> entries;
    entries.reserve(project.city->data.buildings.size());
    for (std::size_t placement_index = 0; placement_index < project.city->data.buildings.size(); ++placement_index) {
        const auto& placement = project.city->data.buildings[placement_index];
        if (!placement_asset_indices[placement_index].has_value()) {
            continue;
        }
        const auto asset_index = *placement_asset_indices[placement_index];
        if (!visible_in_viewport(project.buildings[asset_index].data, placement)) {
            continue;
        }
        entries.push_back({placement_index, asset_index});
    }

    std::ranges::stable_sort(entries, [](const DrawEntry& left, const DrawEntry& right) {
        return left.asset_index < right.asset_index;
    });

    for (const auto& entry : entries) {
        const auto& placement = project.city->data.buildings[entry.placement_index];
        const auto& asset = project.buildings[entry.asset_index];
        auto& model = project.building_gpu_models[entry.asset_index];
        // A missing GPU model means this asset is still being prepared. Do not
        // replace the building with its full-volume bounds: those opaque boxes
        // obscure the city and look like real geometry. The building appears as
        // soon as its actual render model is uploaded.
        if (model.chunks.empty()) {
            continue;
        }
        const auto origin = native_city_building_render_origin(placement, 4.0F);
        const float dx = camera.position.x - origin.x;
        const float dy = camera.position.y - origin.y;
        const float dz = camera.position.z - origin.z;
        // Boxes only far away: at 200 units nearly the whole city read as boxes
        // at normal editing zoom.
        const float lod_distance = render_quality == 0 ? 250.0F : render_quality == 1 ? 600.0F : 1500.0F;
        const bool use_lod = selected_building != &placement && hovered_building != &placement &&
            dx * dx + dy * dy + dz * dz > lod_distance * lod_distance;
        // Full models use the game surface shader, which writes the extra depth
        // output; LOD boxes use raylib's one-output shader, which WebGL2 drops
        // while that output is enabled. The call is a no-op when unchanged.
        set_game_render_depth_output(!use_lod);
        draw_city_building_model(
            asset.data,
            model,
            placement,
            selected_building == &placement,
            use_lod,
            hovered_building == &placement);
    }
    set_game_render_depth_output(false);
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

std::vector<std::string> city_intersection_names(const SbcMap& map) {
    std::vector<std::string> names;
    names.reserve(map.intersections.size());
    for (const auto& intersection : map.intersections) {
        names.push_back(
            std::to_string(intersection.index) + " (" +
            std::to_string(intersection.x) + ", " +
            std::to_string(intersection.y) + ", " +
            std::to_string(intersection.z) + ")");
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

// Names the next city undo step. The step itself is recorded by
// commit_city_edits once the edit finishes, and only if the map changed.
constexpr std::size_t kMaxUndo = 100;
void begin_city_action(EditorProject& project, EditorUi& ui, std::string label) {
    if (project.pending_city_label.empty()) {
        project.pending_city_label = std::move(label);
    }
    (void)ui;
}

namespace {

void record_action(EditorProject& project, std::string label) {
    project.action_log.push_back(std::move(label));
    if (project.action_log.size() > 500U) {
        project.action_log.erase(project.action_log.begin());
    }
}

// Pushes the committed map as an undo step when the live map differs from it.
void commit_city_edits(EditorProject& project) {
    if (!project.city) {
        project.city_baseline.reset();
        project.pending_city_label.clear();
        return;
    }
    if (project.city_baseline.has_value() && *project.city_baseline != project.city->data) {
        auto label = project.pending_city_label.empty() ? std::string{"edit"} : project.pending_city_label;
        // Editing after undoing past the save discards the redo path back to it.
        if (project.saved_undo_depth && *project.saved_undo_depth > project.undo_stack.size()) {
            project.saved_undo_depth.reset();
        }
        project.undo_stack.emplace_back(label, std::move(*project.city_baseline));
        if (project.undo_stack.size() > kMaxUndo) {
            project.undo_stack.erase(project.undo_stack.begin());
            if (project.saved_undo_depth) {
                if (*project.saved_undo_depth == 0U) project.saved_undo_depth.reset();
                else --*project.saved_undo_depth;
            }
        }
        project.redo_stack.clear();
        record_action(project, std::move(label));
        project.city_baseline.reset();
    }
    if (!project.city_baseline.has_value()) {
        project.city_baseline = project.city->data;
    }
    project.pending_city_label.clear();
}

// Runs `apply(saved, live)` on the live asset a history entry names. False when
// that asset was removed or renamed since.
template <typename F>
bool with_live_asset(EditorProject& project, AssetSnapshot& snapshot, F&& apply) {
    return std::visit([&](auto& saved) {
        using T = std::decay_t<decltype(saved)>;
        const auto find = [&](auto& assets) -> T* {
            return snapshot.index < assets.size() && assets[snapshot.index].name == snapshot.name
                ? &assets[snapshot.index].data : nullptr;
        };
        T* live = nullptr;
        if constexpr (std::is_same_v<T, SbbBuilding>) live = find(project.buildings);
        else if constexpr (std::is_same_v<T, SblTemplate>) live = find(project.buildblocks);
        else live = find(project.blocks);
        if (live == nullptr) return false;
        apply(saved, *live);
        return true;
    }, snapshot.data);
}

// Selected asset index per history kind (building, buildblock, block mesh).
std::optional<std::size_t> selected_asset_index(const EditorProject& project, const EditorUi& ui, std::size_t kind) {
    const auto clamp = [](std::size_t selected, std::size_t size) -> std::optional<std::size_t> {
        if (size == 0U) return std::nullopt;
        return std::min(selected, size - 1U);
    };
    switch (kind) {
        case 0: return clamp(ui.selected_building, project.buildings.size());
        case 1: return clamp(ui.selected_buildblock, project.buildblocks.size());
        default: return clamp(ui.selected_block, project.blocks.size());
    }
}

AssetSnapshot snapshot_asset(const EditorProject& project, std::size_t kind, std::size_t index) {
    switch (kind) {
        case 0: return {index, project.buildings[index].name, project.buildings[index].data};
        case 1: return {index, project.buildblocks[index].name, project.buildblocks[index].data};
        default: return {index, project.blocks[index].name, project.blocks[index].data};
    }
}

void commit_asset_edits(EditorProject& project, const EditorUi& ui) {
    for (std::size_t kind = 0; kind < project.asset_baselines.size(); ++kind) {
        auto& baseline = project.asset_baselines[kind];
        if (baseline.has_value()) {
            bool changed = false;
            if (!with_live_asset(project, *baseline, [&](const auto& saved, const auto& live) { changed = saved != live; })) {
                baseline.reset();
            } else if (changed) {
                record_action(project, "edit " + baseline->name);
                auto& undo = project.asset_undo_stacks[kind];
                undo.push_back(std::move(*baseline));
                if (undo.size() > kMaxUndo) {
                    undo.erase(undo.begin());
                }
                project.asset_redo_stacks[kind].clear();
                baseline.reset();
            }
        }
        const auto selected = selected_asset_index(project, ui, kind);
        if (!selected.has_value()) {
            baseline.reset();
        } else if (!baseline.has_value() || baseline->index != *selected) {
            baseline = snapshot_asset(project, kind, *selected);
        }
    }
}

void step_asset_history(EditorProject& project, EditorUi& ui, std::string& status, std::size_t kind, bool redo) {
    auto& from = redo ? project.asset_redo_stacks[kind] : project.asset_undo_stacks[kind];
    auto& to = redo ? project.asset_undo_stacks[kind] : project.asset_redo_stacks[kind];
    const std::string verb = redo ? "redo" : "undo";
    if (from.empty()) {
        status = "nothing to " + verb;
        return;
    }
    auto step = std::move(from.back());
    from.pop_back();
    if (!with_live_asset(project, step, [](auto& saved, auto& live) { std::swap(saved, live); })) {
        status = verb + " skipped: " + step.name + " was removed or renamed";
        return;
    }
    switch (kind) {
        case 0:
            ui.selected_building = step.index;
            mark_building_changed(project, ui);
            break;
        case 1:
            ui.selected_buildblock = step.index;
            mark_buildblock_changed(project, ui);
            break;
        default:
            ui.selected_block = step.index;
            mark_block_changed(project, ui, project.blocks[step.index].data);
            break;
    }
    record_action(project, verb + ": " + step.name);
    status = verb + " " + step.name;
    project.asset_baselines[kind] = snapshot_asset(project, kind, step.index);
    to.push_back(std::move(step));
}

// Asset history the current context edits, or nullopt for city history.
std::optional<std::size_t> active_asset_history(const EditorUi& ui) {
    if (ui.buildblock_window_focused || ui.tab == EditorTab::Buildblocks) return 1U;
    if (ui.tab == EditorTab::Buildings) return 0U;
    if (ui.tab == EditorTab::Blocks) return 2U;
    return std::nullopt;
}

} // namespace

void commit_pending_edits(EditorProject& project, const EditorUi& ui) {
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || ImGui::IsAnyItemActive() || ui.gizmo_active != GizmoAxis::None) {
        return;
    }
    commit_city_edits(project);
    commit_asset_edits(project, ui);
}

bool can_undo(const EditorProject& project, const EditorUi& ui, bool redo) {
    if (const auto kind = active_asset_history(ui)) {
        return !(redo ? project.asset_redo_stacks[*kind] : project.asset_undo_stacks[*kind]).empty();
    }
    return !(redo ? project.redo_stack : project.undo_stack).empty();
}

void undo_edit(EditorProject& project, EditorUi& ui, std::string& status, bool redo) {
    if (const auto kind = active_asset_history(ui)) {
        commit_asset_edits(project, ui);
        step_asset_history(project, ui, status, *kind, redo);
    } else if (redo) {
        redo_city(project, ui, status);
    } else {
        undo_city(project, ui, status);
    }
}

// Reverts the last action: the pre-edit snapshot becomes current, and the current
// state is pushed to the redo stack so it can be reapplied.
void undo_city(EditorProject& project, EditorUi& ui, std::string& status) {
    commit_city_edits(project);
    if (!project.city || project.undo_stack.empty()) {
        status = "nothing to undo";
        return;
    }
    auto [label, snapshot] = std::move(project.undo_stack.back());
    project.undo_stack.pop_back();
    project.redo_stack.emplace_back(label, project.city->data);
    project.city->data = std::move(snapshot);
    project.city_baseline = project.city->data;
    record_action(project, "undo: " + label);
    invalidate_city_block_preview_scene(project);
    invalidate_portal_render_models(project);
    ui.city_selection = CitySelectionKind::None;
    ui.selected_city_buildings.clear();
    ui.selected_portals.clear();
    ui.pending_road_start.reset();
    ui.city_dirty = true;
    status = "undo " + label;
}

void redo_city(EditorProject& project, EditorUi& ui, std::string& status) {
    commit_city_edits(project);
    if (!project.city || project.redo_stack.empty()) {
        status = "nothing to redo";
        return;
    }
    auto [label, snapshot] = std::move(project.redo_stack.back());
    project.redo_stack.pop_back();
    project.undo_stack.emplace_back(label, project.city->data);
    project.city->data = std::move(snapshot);
    project.city_baseline = project.city->data;
    record_action(project, "redo: " + label);
    invalidate_city_block_preview_scene(project);
    invalidate_portal_render_models(project);
    ui.city_selection = CitySelectionKind::None;
    ui.selected_city_buildings.clear();
    ui.selected_portals.clear();
    ui.pending_road_start.reset();
    ui.city_dirty = true;
    status = "redo " + label;
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

// Locates a cell by grid coordinate in the dense y->z->x cell array, or nullptr.
SbbCell* building_cell_at(SbbBuilding& building, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    for (auto& cell : building.cells) {
        if (cell.x == x && cell.y == y && cell.z == z) {
            return &cell;
        }
    }
    return nullptr;
}

// Face index (0..5) for the hit point on a cell, from the dominant axis of the
// offset between the hit and the cell centre. Order matches the renderer's
// BuildingFaceSide->slot map: 0 Down(-Y), 1 South(-Z), 2 West(-X), 3 North(+Z),
// 4 East(+X), 5 Up(+Y). Returns both the slot and the neighbour delta so Place
// can fill the empty cell on that side.
struct BuildingFaceHit {
    std::uint32_t slot{};
    std::int32_t dx{};
    std::int32_t dy{};
    std::int32_t dz{};
};

BuildingFaceHit building_face_from_hit(const SbbBuilding& building, const SbbCell& cell, Vec3 hit_world, float unit_size) {
    // Cell centre must match the renderer/picker, which recenter by the building
    // anchor (see building_cell_center_world); without this offset the face and
    // neighbour come out wrong and placement lands in the wrong cell.
    const auto cx = (static_cast<float>(cell.x) - static_cast<float>(building.anchor_x) + 0.5F) * unit_size;
    const auto cy = (static_cast<float>(cell.y) - static_cast<float>(building.anchor_y) + 0.5F) * unit_size;
    const auto cz = (static_cast<float>(cell.z) - static_cast<float>(building.anchor_z) + 0.5F) * unit_size;
    const auto ox = hit_world.x - cx;
    const auto oy = hit_world.y - cy;
    const auto oz = hit_world.z - cz;
    const auto ax = std::abs(ox);
    const auto ay = std::abs(oy);
    const auto az = std::abs(oz);
    if (ay >= ax && ay >= az) {
        return oy < 0.0F ? BuildingFaceHit{0, 0, -1, 0} : BuildingFaceHit{5, 0, 1, 0};
    }
    if (ax >= az) {
        return ox < 0.0F ? BuildingFaceHit{2, -1, 0, 0} : BuildingFaceHit{4, 1, 0, 0};
    }
    return oz < 0.0F ? BuildingFaceHit{1, 0, 0, -1} : BuildingFaceHit{3, 0, 0, 1};
}

void draw_building_paint_face(const SbbBuilding& building, const SbbCell& cell, std::uint32_t slot, float unit_size) {
    const float x0 = (static_cast<float>(cell.x) - static_cast<float>(building.anchor_x)) * unit_size;
    const float y0 = (static_cast<float>(cell.y) - static_cast<float>(building.anchor_y)) * unit_size;
    const float z0 = (static_cast<float>(cell.z) - static_cast<float>(building.anchor_z)) * unit_size;
    const float x1 = x0 + unit_size;
    const float y1 = y0 + unit_size;
    const float z1 = z0 + unit_size;
    constexpr float lift = 0.035F;
    std::array<Vector3, 4> face{};
    switch (slot) {
        case 0: face = {{{x0, y0-lift, z1}, {x1, y0-lift, z1}, {x1, y0-lift, z0}, {x0, y0-lift, z0}}}; break;
        case 1: face = {{{x0, y0, z0-lift}, {x1, y0, z0-lift}, {x1, y1, z0-lift}, {x0, y1, z0-lift}}}; break;
        case 2: face = {{{x0-lift, y0, z1}, {x0-lift, y0, z0}, {x0-lift, y1, z0}, {x0-lift, y1, z1}}}; break;
        case 3: face = {{{x1, y0, z1+lift}, {x0, y0, z1+lift}, {x0, y1, z1+lift}, {x1, y1, z1+lift}}}; break;
        case 4: face = {{{x1+lift, y0, z0}, {x1+lift, y0, z1}, {x1+lift, y1, z1}, {x1+lift, y1, z0}}}; break;
        default: face = {{{x0, y1+lift, z0}, {x1, y1+lift, z0}, {x1, y1+lift, z1}, {x0, y1+lift, z1}}}; break;
    }
    // Drawn over the wall it sits on (depth test off) and from both sides: the
    // flush quad otherwise lost to the wall itself and showed only an edge.
    const Color fill{255, 187, 54, 150};
    const Color edge{255, 220, 112, 255};
    rlDrawRenderBatchActive();
    rlDisableDepthTest();
    DrawTriangle3D(face[0], face[1], face[2], fill);
    DrawTriangle3D(face[0], face[2], face[3], fill);
    DrawTriangle3D(face[2], face[1], face[0], fill);
    DrawTriangle3D(face[3], face[2], face[0], fill);
    for (std::size_t index = 0; index < face.size(); ++index) {
        DrawLine3D(face[index], face[(index + 1U) % face.size()], edge);
    }
    rlDrawRenderBatchActive();
    rlEnableDepthTest();
}

// Native solid-cell tag the building renderer keys off:
// is_solid_building_cell tests exterior_block>>29 == 2 (the 0x40000000 class),
// and interior_block == 0xffff marks a fully-faced solid box (the same words
// native_city_block_solid_cell() uses). A bare index word is NOT solid and would
// render nothing, which is why placement looked broken before.
constexpr std::uint32_t kSolidExteriorBlock = 0x40000000U;
constexpr std::uint32_t kSolidInteriorBlock = 0x0000FFFFU;

// Writes a cell from the Place Block config: exterior and/or interior shell, the
// chosen buildblock template (cell_type), and the per-face textures for each side.
// Faces 0..5 map to texture slots 0..5 (exterior) and 8..13 (interior).
void apply_block_config(SbbCell& cell, const BlockToolConfig& config, const SbbBuilding& building) {
    cell.occupied = true;
    cell.exterior_block() = config.exterior ? kSolidExteriorBlock : 0U;
    cell.interior_block() = config.interior ? kSolidInteriorBlock : 0U;
    cell.cell_type() = static_cast<std::uint32_t>(
        building.block_names.empty() ? 0U : std::min(config.buildblock_index, building.block_names.size() - 1U));
    cell.exterior_portal_face() = 0U;
    cell.interior_portal_face() = 0U;
    cell.placed_flag() = 0U;
    cell.payloads.clear();
    const auto material = [&](std::size_t face_material) -> std::uint16_t {
        if (building.materials.empty()) {
            return 0U;
        }
        return static_cast<std::uint16_t>(std::min(face_material, building.materials.size() - 1U));
    };
    for (std::size_t face = 0; face < 6; ++face) {
        cell.shorts[face] = config.exterior ? material(config.exterior_face_material[face]) : cell.shorts[face];
        cell.shorts[8 + face] = config.interior ? material(config.interior_face_material[face]) : cell.shorts[8 + face];
    }
}

// Place Block: extrude a new cell into the empty neighbour of the picked face,
// writing the staged block config (exterior/interior shells, buildblock, textures).
// Returns the filled cell's index, or nullopt when nothing was placed.
std::optional<std::size_t> place_building_block(EditorProject& project, EditorUi& ui, const CellPick& pick, float unit_size, std::string& status) {
    if (project.buildings.empty()) {
        return std::nullopt;
    }
    if (!ui.block_config.exterior && !ui.block_config.interior) {
        status = "enable Exterior or Interior in the block panel first";
        return std::nullopt;
    }
    ui.selected_building = std::min(ui.selected_building, project.buildings.size() - 1U);
    auto& building = project.buildings[ui.selected_building].data;
    if (pick.cell_index >= building.cells.size()) {
        return std::nullopt;
    }
    const auto source = building.cells[pick.cell_index];
    const auto face = building_face_from_hit(building, source, pick.hit_world, unit_size);
    const std::int64_t nx = static_cast<std::int64_t>(source.x) + face.dx;
    const std::int64_t ny = static_cast<std::int64_t>(source.y) + face.dy;
    const std::int64_t nz = static_cast<std::int64_t>(source.z) + face.dz;
    const auto sx = static_cast<std::uint32_t>(std::max<std::int64_t>(0, -nx));
    const auto sy = static_cast<std::uint32_t>(std::max<std::int64_t>(0, -ny));
    const auto sz = static_cast<std::uint32_t>(std::max<std::int64_t>(0, -nz));
    for (auto& cell : building.cells) { cell.x += sx; cell.y += sy; cell.z += sz; }
    building.anchor_x += sx; building.anchor_y += sy; building.anchor_z += sz;
    auto* target = building_cell_at(building, nx + sx, ny + sy, nz + sz);
    if (target == nullptr) {
        SbbCell cell{};
        cell.index = building.cells.size();
        cell.x = static_cast<std::uint32_t>(nx + sx);
        cell.y = static_cast<std::uint32_t>(ny + sy);
        cell.z = static_cast<std::uint32_t>(nz + sz);
        cell.shorts.fill(0xffffU);
        building.cells.push_back(cell);
        target = &building.cells.back();
    }
    if (target->occupied) {
        status = "neighbour cell already filled";
        return std::nullopt;
    }
    apply_block_config(*target, ui.block_config, building);
    mark_building_changed(project, ui);
    status = "placed block at [" + std::to_string(target->x) + "," + std::to_string(target->y) + "," + std::to_string(target->z) + "]";
    return static_cast<std::size_t>(target - building.cells.data());
}

// Paint Texture: write the chosen material into the picked face's texture slot.
// Exterior slots are 0..5, interior slots 8..13 (per the cell.shorts layout); the
// interior toggle picks the side.
void paint_building_face(EditorProject& project, EditorUi& ui, const CellPick& pick, float unit_size, std::string& status) {
    if (project.buildings.empty()) {
        return;
    }
    ui.selected_building = std::min(ui.selected_building, project.buildings.size() - 1U);
    auto& building = project.buildings[ui.selected_building].data;
    if (pick.cell_index >= building.cells.size()) {
        return;
    }
    auto& cell = building.cells[pick.cell_index];
    const auto face = building_face_from_hit(building, cell, pick.hit_world, unit_size);
    const auto slot = (ui.paint_interior ? 8U : 0U) + face.slot;
    if (slot >= cell.shorts.size()) {
        return;
    }
    const auto material = building.materials.empty()
        ? std::uint16_t{0}
        : static_cast<std::uint16_t>(std::min(ui.paint_material_index, building.materials.size() - 1U));
    cell.shorts[slot] = material;
    mark_building_changed(project, ui);
    const auto name = material < building.materials.size() ? building.materials[material] : std::string{"(none)"};
    status = std::string(ui.paint_interior ? "interior" : "exterior") + " face painted " + name;
}

BoundingBox city_building_bounds(const SbcBuilding& placement, const SbbBuilding& asset, float unit);

// Middle of a placed building's footprint at its render-origin height: where
// the gizmo sits and what rotation turns about. Falls back to the render origin
// when the template is not loaded.
Vector3 city_building_center(const EditorProject& project, const SbcBuilding& placement) {
    const auto o = native_city_building_render_origin(placement, 4.0F);
    const auto* asset = find_building_asset(project, placement.name);
    if (asset == nullptr) return {o.x, o.y, o.z};
    const auto bounds = city_building_bounds(placement, asset->data, 4.0F);
    return {(bounds.min.x + bounds.max.x) * 0.5F, o.y, (bounds.min.z + bounds.max.z) * 0.5F};
}

// Gizmo position relative to the render origin (see city_building_center).
Vector3 city_building_gizmo_offset(const EditorProject& project, const SbcBuilding& placement) {
    const auto o = native_city_building_render_origin(placement, 4.0F);
    const auto c = city_building_center(project, placement);
    return {c.x - o.x, 0.0F, c.z - o.z};
}

// Turns a building in quarter steps about its footprint centre, so it spins in
// place instead of swinging around its anchor corner.
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

void place_city_building_copy(EditorProject& project, EditorUi& ui, CityGridPoint point) {
    if (!project.city || project.city->data.buildings.empty()) {
        return;
    }

    duplicate_city_building(project, ui);
    set_city_building_position(project, ui, point);
}

void delete_city_building(EditorProject& project, EditorUi& ui) {
    if (!project.city || project.city->data.buildings.empty()) {
        return;
    }

    auto& map = project.city->data;
    // Delete the whole multi-selection if there is one (descending order keeps the
    // remaining indices valid), else just the active building.
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

// World bounds of a placed building's full cell volume.
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

// The placement the Building tool would make at the cursor: the chosen template
// (ui.place_building_template) turned by ui.place_building_rotation, with its
// footprint centred on the cursor and its lowest floor on the clicked surface.
// The ghost preview and the click both use this, so what you see is what lands.
SbcBuilding city_building_placement_at(const EditorProject& project, const EditorUi& ui, CityGridPoint point) {
    constexpr float unit = 4.0F;
    const auto& tmpl = project.buildings[std::min(ui.place_building_template, project.buildings.size() - 1U)];
    SbcBuilding placement{};
    placement.name = tmpl.name;
    placement.x = point.x;
    // The record's y is the level of the ANCHOR layer (CitySbc_Load pairs
    // size_z with anchor_y). Floors below the anchor are foundation, so to
    // stand the lowest floor on the clicked surface the anchor goes anchor_y
    // levels above it. Without this, houses with anchor_y 1..2 sank that much.
    placement.y = point.y + static_cast<std::int32_t>(tmpl.data.anchor_y);
    placement.z = point.z;
    placement.rotation = ui.place_building_rotation & 3;
    const auto bounds = city_building_bounds(placement, tmpl.data, unit);
    placement.x -= static_cast<std::int32_t>(std::lround(((bounds.min.x + bounds.max.x) * 0.5F - point.x * unit) / unit));
    placement.z -= static_cast<std::int32_t>(std::lround(((bounds.min.z + bounds.max.z) * 0.5F - point.z * unit) / unit));
    return placement;
}

// What a Building-tool placement would overlap: another building's volume or
// a road's surface. Empty when the spot is clear. Placing is still allowed;
// this only drives the red preview and the warning text.
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
        // Roads are axis-aligned; pad the centreline by half the total width.
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

// Drops a NEW placement of the chosen building template at the cursor.
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
        const std::int64_t dy = static_cast<std::int64_t>(it.y) - point.y;
        const auto dist_sq = dx * dx + dy * dy + dz * dz;
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

// A road segment a grid point lands on, mid-span, ready to be split: the street
// index plus the grid point projected onto the segment (axis-aligned, so the new
// node sits exactly on the road line).
struct StreetSplitHit {
    std::size_t street_index{};
    CityGridPoint point{};
};

// Finds an existing road whose footprint the grid point lands on, away from its
// endpoints, so a click there can split it into a T/4-way junction instead of
// dropping a free-floating node. Endpoints are left to nearest_city_intersection.
std::optional<StreetSplitHit> nearest_street_split(const SbcMap& map, CityGridPoint point) {
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
        // Stay clear of the endpoints; those resolve to the existing nodes.
        const double end_margin = 0.75 / std::sqrt(seg_len_sq);
        if (u <= end_margin || u >= 1.0 - end_margin) {
            continue;
        }
        const double cx = a.x + dx * u;
        const double cz = a.z + dz * u;
        const double dist = std::sqrt((point.x - cx) * (point.x - cx) + (point.z - cz) * (point.z - cz));
        // Half the drawn road width, in cells, plus a cell of slack.
        const double half_width = std::max(street.left_width, street.right_width) * 0.5 + 1.0;
        if (dist > half_width || dist >= best_dist) {
            continue;
        }
        const auto road_y = static_cast<std::int32_t>(std::lround(a.y + (b.y - a.y) * u));
        if (std::abs(road_y - point.y) > 1) continue;
        // Snap the split node onto the exact grid cell nearest the projection so
        // the two halves stay axis-aligned with the original road.
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

// Splits street[street_index] at a new node placed at split_point: the original
// segment becomes from->new, and a fresh segment new->to is appended, both
// carrying the source road's width/district/name. Returns the new node index.
std::size_t split_street_at(EditorProject& project, std::size_t street_index, CityGridPoint split_point) {
    auto& map = project.city->data;
    const auto existing = nearest_city_intersection(map, split_point, 0);
    const auto node_index = existing ? *existing : add_city_intersection(map, split_point);

    auto original = map.streets[street_index];
    const auto to_index = original.to_intersection;
    // Reuse the original for from->new; append new->to with the same profile.
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

// Resolves a click to an intersection index: snaps to a nearby existing node,
// else splits an existing road the click lands on (forming a junction), else
// creates a new node at the clicked surface height.
std::size_t resolve_or_create_intersection(EditorProject& project, CityGridPoint point) {
    auto& map = project.city->data;
    if (const auto existing = nearest_city_intersection(map, point, 1)) {
        return *existing;
    }
    if (const auto split = nearest_street_split(map, point)) {
        return split_street_at(project, split->street_index, split->point);
    }
    return add_city_intersection(map, point);
}

void append_city_road_point(EditorProject& project, EditorUi& ui, CityGridPoint point) {
    if (!project.city) {
        return;
    }

    auto& map = project.city->data;

    // First click only records the start, snapped to a nearby node or road so the
    // preview matches. Nothing is created until the second click lays a segment.
    if (!ui.pending_road_start.has_value()) {
        if (const auto node = nearest_city_intersection(map, point, 1)) {
            const auto& existing = map.intersections[*node];
            point = {existing.x, existing.y, existing.z};
        } else if (const auto split = nearest_street_split(map, point)) {
            point = split->point;
        }
        ui.pending_road_start = point;
        return;
    }

    // Clicking the start point again ends the chain without a 0-length road.
    const auto start_point = *ui.pending_road_start;
    if (start_point.x == point.x && start_point.z == point.z) {
        ui.pending_road_start.reset();
        return;
    }

    // Axis-lock the end relative to the start, then resolve/create that node.
    const auto end_point = snap_to_axis(start_point, point);
    // An end one cell from the start would snap back onto the start node; stop
    // before creating that node so the click leaves nothing behind.
    if (std::abs(end_point.x - start_point.x) + std::abs(end_point.z - start_point.z) <= 1) return;
    const auto start_index = resolve_or_create_intersection(project, start_point);
    const auto end_index = resolve_or_create_intersection(project, end_point);
    const auto node_point = [&](std::size_t index) {
        const auto& node = map.intersections[index];
        return CityGridPoint{node.x, node.y, node.z};
    };
    if (end_index == start_index) return;
    for (const auto& existing : map.streets) {
        if ((existing.from_intersection == static_cast<int>(start_index) && existing.to_intersection == static_cast<int>(end_index)) ||
            (existing.to_intersection == static_cast<int>(start_index) && existing.from_intersection == static_cast<int>(end_index))) {
            ui.pending_road_start = node_point(end_index);
            return;
        }
    }
    // New roads use the Tools width and continue the district of a road already
    // meeting the start node.
    SbcStreet street{};
    street.name = "Street";
    street.left_width = ui.new_road_width / 2;
    street.right_width = ui.new_road_width - street.left_width;
    for (const auto& existing : map.streets) {
        if (existing.from_intersection == static_cast<int>(start_index) || existing.to_intersection == static_cast<int>(start_index)) {
            street.district = existing.district;
            break;
        }
    }
    // Split intersected roads at matching heights, then connect every junction
    // in order. Crossings at another elevation remain bridges.
    const auto finish = map.intersections[end_index];
    const bool along_x = start_point.z == finish.z;
    const int from = along_x ? start_point.x : start_point.z;
    const int to = along_x ? finish.x : finish.z;
    if (from == to) return;
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
        split_street_at(project, i, {along_x ? crossing : fixed, static_cast<int>(std::lround(y)), along_x ? fixed : crossing});
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
        ui.selected_city_street = map.streets.size() - 1;
    }
    // The new end node becomes the next start: chained laying.
    ui.pending_road_start = node_point(end_index);
    ui.city_dirty = true;
}

// Ends the current road-laying chain (Esc, or mode/selection change).
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

void set_city_piece_voxel(EditorProject& project, EditorUi& ui, CityGridPoint point) {
    if (!project.city || project.blocks.empty() || project.city->data.version < 11) return;
    auto& map = project.city->data;
    ui.place_piece_mesh = std::min(ui.place_piece_mesh, project.blocks.size() - 1);
    const auto& mesh = project.blocks[ui.place_piece_mesh];
    std::array<std::string, 8> layer{};
    if (!project.textures.empty()) layer.fill(project.textures[std::min(ui.place_piece_texture, project.textures.size() - 1)].name);
    auto block = std::find_if(map.city_blocks.begin(), map.city_blocks.end(), [&](const auto& candidate) {
        return candidate.name == mesh.name && candidate.layers.size() == 1 && candidate.layers[0] == layer;
    });
    if (block == map.city_blocks.end()) {
        if (map.city_blocks.size() >= 4096) return;
        map.city_blocks.push_back({map.city_blocks.size(), mesh.name, {layer}});
        block = std::prev(map.city_blocks.end());
    }
    ui.selected_city_block = static_cast<std::size_t>(block - map.city_blocks.begin());
    ui.selected_city_block_layer = 0;
    auto& portal = find_or_create_portal(project.city->data, point);
    if (portal.blocks.size() < 0x200) {
        portal.blocks.resize(0x200);
    }
    portal.blocks[local_portal_index(point)] = encode_city_piece_word(ui);
    invalidate_portal_render_models(project);
    ui.city_dirty = true;
}

// Moves the selected piece voxel one step, keeping its word (piece, layer,
// rotation). Refuses to overwrite an occupied cell.
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
    auto& target = find_or_create_portal(map, to);  // may reallocate portals
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

    // Resolve the picked voxel to its world grid cell so selection highlights one
    // block, not the whole 8x8x8 portal.
    const auto& portal = project.city->data.portals[pick.portal_index];
    const auto local_x = static_cast<std::int32_t>(pick.block_index & 7U);
    const auto local_z = static_cast<std::int32_t>((pick.block_index / 8U) & 7U);
    const auto local_y = static_cast<std::int32_t>((pick.block_index / 0x40U) & 7U);
    ui.selected_piece_voxel = CityGridPoint{
        portal.fields[1] * 8 + local_x,
        portal.fields[2] * 8 + local_y,
        portal.fields[3] * 8 + local_z,
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
    invalidate_city_block_preview_scene(project);
    invalidate_portal_render_models(project);
    ui.city_dirty = true;
}

// Convert old district cells into named v12 pieces with texture layers. Built-in
// meshes become ordinary v2 SBL assets, so saved cities do not depend on legacy
// numeric catalog IDs. Stage the whole conversion before changing the project.
void promote_city(EditorProject& project) {
    if (!project.city || project.city->data.version >= 12) return;
    auto converted = project.city->data;
    std::vector<LoadedAsset<SblBlockMesh>> generated;
    const auto catalog = build_native_builtin_catalog();
    const auto& textures = project_grid_texture_names(project);
    for (auto& portal : converted.portals) {
        if (portal.legacy_blocks.empty()) continue;
        if (!portal.blocks.empty()) throw std::runtime_error("City has mixed legacy and current district cells");
        portal.blocks.resize(0x200);
        for (std::size_t i = 0; i < std::min<std::size_t>(512, portal.legacy_blocks.size()); ++i) {
            const auto word = portal.legacy_blocks[i];
            if (!word) continue;
            const auto kind = word & 0xe0000000U;
            std::string mesh_name;
            if (kind == 0x80000000U) {
                const auto id = word & 0xffffU;
                if (id == 0) continue; // Native null catalog entry.
                if (id >= catalog.size() || catalog[id].mesh.faces.empty())
                    throw std::runtime_error("Cannot promote unsupported legacy mesh " + std::to_string(id));
                mesh_name = "converted_legacy_" + std::to_string(id);
                // A previous upgrade (e.g. before an undo) already added this mesh;
                // reuse it instead of adding a numbered duplicate every time.
                const auto loaded = std::find_if(project.blocks.begin(), project.blocks.end(), [&](const auto& asset) {
                    return asset.name.starts_with(mesh_name) && asset.data == catalog[id].mesh;
                });
                if (loaded != project.blocks.end()) {
                    mesh_name = loaded->name;
                } else {
                    if (std::none_of(generated.begin(), generated.end(), [&](const auto& asset) { return asset.name == mesh_name; })) {
                        auto name = mesh_name;
                        for (unsigned suffix = 1; std::filesystem::exists(project.root / "block" / (name + ".sbl")) ||
                            std::any_of(project.blocks.begin(), project.blocks.end(), [&](const auto& asset) { return asset.name == name; }); ++suffix)
                            name = mesh_name + "_" + std::to_string(suffix);
                        // Cache the chosen name by catalog ID for later cells.
                        generated.push_back({project.root / "block" / (name + ".sbl"), mesh_name, catalog[id].mesh});
                    }
                    const auto asset = std::find_if(generated.begin(), generated.end(), [&](const auto& asset) { return asset.name == mesh_name; });
                    mesh_name = asset->path.stem().string();
                }
            } else if (kind != 0x40000000U) {
                throw std::runtime_error("Cannot promote unknown legacy district cell class");
            }
            std::array<std::string, 8> layer{};
            for (std::size_t slot = 0; slot < 8; ++slot) {
                const auto offset = i * 8 + slot;
                auto texture = offset < portal.textures.size() ? portal.textures[offset] : 0;
                if (texture >= textures.size()) texture = 0;
                if (!textures.empty()) layer[slot] = textures[texture];
            }
            auto block = std::find_if(converted.city_blocks.begin(), converted.city_blocks.end(), [&](const auto& candidate) {
                return candidate.name == mesh_name && candidate.layers.size() == 1 && candidate.layers[0] == layer;
            });
            if (block == converted.city_blocks.end()) {
                if (converted.city_blocks.size() >= 4096) throw std::runtime_error("Promoted city exceeds the piece limit");
                converted.city_blocks.push_back({converted.city_blocks.size(), mesh_name, {layer}});
                block = std::prev(converted.city_blocks.end());
            }
            // Catalog geometry already contains its rotation. Preserve the
            // texture layer and avoid rotating that baked mesh a second time.
            portal.blocks[i] = static_cast<std::uint32_t>(block - converted.city_blocks.begin()) | 0x10000U;
        }
        portal.legacy_blocks.clear();
        portal.textures.clear();
        portal.itemsets.resize(512);
    }
    converted.version = 12;
    std::vector<std::vector<std::uint8_t>> payloads;
    for (const auto& asset : generated) payloads.push_back(rebuild_sbl_block(asset.data));
    for (std::size_t i = 0; i < generated.size(); ++i) {
        auto& asset = generated[i];
        asset.name = asset.path.stem().string();
        project.generated_city_assets.emplace_back(asset.path, payloads[i]);
        project.block_bytes.push_back(std::move(payloads[i]));
        project.blocks.push_back(std::move(asset));
    }
    project.city->data = std::move(converted);
    invalidate_render_input_cache(project);
    invalidate_portal_render_models(project);
}

void recalculate_building_size(SbbBuilding& building) {
    building.version = 14;
    // Native grid rule, true of every shipped building: X/Z hold the highest
    // cell index (the grid spans 0..size inclusive) and Y the floor count, so
    // burger's 12x12x2 grid holds 13*13*2 = 338 cells. Measure every grid cell,
    // not just occupied ones, so empty columns and upper floors keep the size.
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
    // Edited geometry invalidates baked LODs.
    building.lod_tail.clear();
    building.lod_marker = 1;
    building.lod_group_count = 0;
}

// A buildblock edit only changes the meshes of buildings that reference it, so
// rebuild those instead of dirtying every building in the project.
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
    project.buildings[ui.selected_building].dirty = true;
    recalculate_building_size(project.buildings[ui.selected_building].data);
    ui.building_dirty = true;
    // Only the edited building's mesh + thumbnail need rebuilding, not every city
    // building. The Buildings-tab floor preview is its own cached model.
    project.building_floor_preview_dirty = true;
    rebuild_single_building_render(project, ui.selected_building);
    invalidate_building_thumbnail(project, ui.selected_building);
}

void mark_buildblock_changed(EditorProject& project, EditorUi& ui) {
    project.buildblocks[ui.selected_buildblock].dirty = true;
    auto& model = project.buildblocks[ui.selected_buildblock].data;
    model.version = 1;
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
    project.render_buffers_dirty = true;
    // Editing block data invalidates the cached block-mesh catalog.
    invalidate_render_input_cache(project);
    invalidate_city_block_preview_scene(project);
    invalidate_portal_render_models(project);
}

CsxArchive project_archive(const EditorProject& project) {
    CsxArchive archive{0xc5f17e01U, 0, 0, {}};
    const auto add = [&](CsxEntry entry) {
        if (entry.name.empty() || entry.name.size() > 51 || entry.name.find_first_of("/\\:") != std::string::npos)
            throw std::runtime_error("Invalid archive asset name: " + entry.name);
        auto existing = std::find_if(archive.entries.begin(), archive.entries.end(), [&](const auto& other) {
            return other.type == entry.type && lower_name(other.name) == lower_name(entry.name);
        });
        if (existing == archive.entries.end()) archive.entries.push_back(std::move(entry));
        else *existing = std::move(entry);
    };
    // Loose textures load first; archive textures override them, as in the viewport.
    std::vector<std::filesystem::path> pngs = files_with_extension(project.root / "texture", ".png");
    for (const auto& path : files_with_extension(project.root, ".png")) pngs.push_back(path);
    for (const auto& path : pngs) {
        const auto stem = path.stem().string();
        if (stem.ends_with("_material")) continue;
        const auto color = read_file_bytes(path);
        auto material_path = project.root / "texturematerial" / path.filename();
        if (!std::filesystem::exists(material_path)) material_path = project.root / (stem + "_material.png");
        const auto material = std::filesystem::exists(material_path) ? read_file_bytes(material_path) : std::vector<std::uint8_t>{};
        CsxEntry entry{}; entry.name = stem; entry.kind = CsxEntryKind::Texture; entry.type = 0x585a0004U;
        entry.payload.resize(0x4c, 0); entry.payload[0] = 1;
        for (unsigned i = 0; i < 4; ++i) {
            entry.payload[0x44+i] = static_cast<std::uint8_t>(color.size() >> (8*i));
            entry.payload[0x48+i] = static_cast<std::uint8_t>(material.size() >> (8*i));
        }
        entry.payload.insert(entry.payload.end(), color.begin(), color.end());
        entry.payload.insert(entry.payload.end(), material.begin(), material.end());
        add(std::move(entry));
    }
    for (const auto& path : project_archive_paths(project))
        for (auto& entry : parse_csx_file(path).entries)
            if (entry.kind == CsxEntryKind::Texture || entry.kind == CsxEntryKind::Unknown) add(std::move(entry));
    const auto geometry = [&](const std::string& name, CsxEntryKind kind, std::vector<std::uint8_t> bytes) {
        CsxEntry entry{}; entry.name = name; entry.kind = kind;
        entry.type = kind == CsxEntryKind::Sbb ? 0x585a0002U : 0x585a0001U;
        bytes[0] = 1; bytes[1] = bytes[2] = bytes[3] = 0;
        entry.payload = std::move(bytes); add(std::move(entry));
    };
    for (const auto& asset : project.buildings) geometry(asset.name, CsxEntryKind::Sbb, rebuild_sbb(asset.data));
    for (std::size_t i = 0; i < project.blocks.size(); ++i) {
        auto bytes = project.block_bytes.at(i);
        if (read_le_u32(bytes, 0) == 1) bytes.resize(bytes.size() + 4, 0);
        geometry(project.blocks[i].name, CsxEntryKind::Sbl, std::move(bytes));
    }
    for (const auto& asset : project.buildblocks) {
        if (std::any_of(project.blocks.begin(), project.blocks.end(), [&](const auto& block) { return lower_name(block.name) == lower_name(asset.name); }))
            throw std::runtime_error("Block and buildblock share an archive name: " + asset.name);
        geometry(asset.name, CsxEntryKind::Sbl, rebuild_sbl_buildblock(asset.data));
    }
    const auto rank = [](CsxEntryKind kind) {
        return kind == CsxEntryKind::Sbb ? 0 : kind == CsxEntryKind::Sbl ? 1 : kind == CsxEntryKind::Texture ? 2 : 3;
    };
    std::stable_sort(archive.entries.begin(), archive.entries.end(), [&](const auto& a, const auto& b) {
        return rank(a.kind) < rank(b.kind);
    });
    return archive;
}

// A map consists of its city layout plus the complete, current asset archive.
// The chosen basename names both files; no edited copies or stale source payloads.
void save_active_tab(EditorProject& project, EditorUi& ui, std::string& status) {
#ifndef __EMSCRIPTEN__
    NFD::Guard guard;
    NFD::UniquePath selected;
    const nfdnfilteritem_t filters[] = {{"Sub Rosa map", "sbc"}};
    const auto directory = project.city ? project.city->path.parent_path() : project.root;
    const auto name = project.city ? project.city->path.filename().string() : "city2.sbc";
    const auto result = NFD::SaveDialog(selected, filters, 1, directory.string().c_str(), name.c_str());
    if (result == NFD_CANCEL) return;
    if (result == NFD_ERROR) { report_failure(status, std::string{"Map save failed: "} + NFD::GetError()); return; }
    auto output = std::filesystem::path{selected.get()};
#else
    // No save dialog in the browser: keep the loaded name and download.
    auto output = project.city ? project.city->path : project.root / "city2.sbc";
#endif
    try {
        if (output.extension() != ".sbc") output += ".sbc";
        promote_city(project);
        const auto archive = project_archive(project);
        auto city = project.city ? project.city->data : SbcMap{};
        city.version = 12;
        for (auto& placement : city.buildings) {
            const auto asset = find_building_asset(project, placement.name);
            if (asset == nullptr) throw std::runtime_error("Missing or ambiguous building: " + placement.name);
            placement.name = asset->name;
        }
        const auto map_bytes = rebuild_sbc(city);
        const auto archive_bytes = build_csx(archive);
        auto archive_path = output; archive_path.replace_extension(".csx");
        // Finish serialization before touching either output file.
#ifndef __EMSCRIPTEN__
        write_file_bytes(archive_path, archive_bytes);
        write_file_bytes(output, map_bytes);
#else
        web_download(archive_path.filename().string(), archive_bytes);
        web_download(output.filename().string(), map_bytes);
#endif
        project.city = LoadedAsset<SbcMap>{output, output.stem().string(), std::move(city)};
        project.city_promotion_failed = false;
        project.city_bytes = map_bytes;
        for (auto& asset : project.buildings) asset.dirty = false;
        for (auto& asset : project.buildblocks) asset.dirty = false;
        ui.city_dirty = ui.building_dirty = ui.block_dirty = ui.buildblock_dirty = false;
        project.saved_undo_depth = project.undo_stack.size();
        // Saving normalizes the map; that is not an edit to undo.
        project.city_baseline = project.city->data;
        status.clear();
    } catch (const std::exception& error) {
        report_failure(status, std::string{"Map save failed: "} + error.what());
    }
}

bool has_unsaved_changes(const EditorUi& ui) {
    return ui.city_dirty || ui.building_dirty || ui.block_dirty || ui.buildblock_dirty;
}

void save_all_dirty(EditorProject& project, EditorUi& ui, std::string& status) {
    save_active_tab(project, ui, status);
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
    // XZ plane handle: a square between the X and Z arms (ImGuizmo style).
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
    const auto arm = std::max(1.0F, distance * std::tan(camera.fovy * DEG2RAD * .5F) * 180.0F / std::max(1.0F, viewport_size.y));
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

// Nearest gizmo handle hit by the cursor in viewport pixels. Screen-space hit
// zones keep the gizmo selectable at every zoom level and match the visible
// arrow shafts/heads instead of only testing tiny 3D endpoint spheres.
// What a click would select in the unified city view. A building or piece the ray
// passes through sits in front of the road/terrain beneath it, so volumes win over
// the ground; among ground hits, the road/node picker already chooses nearest.
struct CityAnyPick {
    CitySelectionKind kind{CitySelectionKind::None};
    std::size_t index{};
};

CityAnyPick pick_city_any(EditorProject& project, const MapScene& scene, const Vec3& ro, const Vec3& rd) {
    if (!project.city) {
        return {};
    }
    const auto& map = project.city->data;
    // Pick rendered triangles, so an empty cell or open doorway does not select
    // the building behind the cursor.
    const auto& asset_indices = resolve_placement_asset_indices(project);
    const Ray ray{{ro.x, ro.y, ro.z}, {rd.x, rd.y, rd.z}};
    // Foundation floors sit under the ground; a ray that reaches the ground (or a
    // road on it) first must not pick the building buried behind it.
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
    // Piece voxel volume. Whichever of the nearest building and piece the ray
    // reaches first wins, so a piece in front of a building is pickable.
    const auto piece = pick_city_piece(map, ro, rd, scene.unit_size);
    float piece_distance = std::numeric_limits<float>::max();
    if (piece.has_value() && piece->portal_index < map.portals.size()) {
        const auto& portal = map.portals[piece->portal_index];
        const Vector3 cell_min{
            static_cast<float>(portal.fields[1] * 8 + static_cast<std::int32_t>(piece->block_index & 7U)) * scene.unit_size,
            static_cast<float>(portal.fields[2] * 8 + static_cast<std::int32_t>((piece->block_index / 0x40U) & 7U)) * scene.unit_size,
            static_cast<float>(portal.fields[3] * 8 + static_cast<std::int32_t>((piece->block_index / 8U) & 7U)) * scene.unit_size};
        const auto cell_hit = GetRayCollisionBox(ray, {cell_min, Vector3AddValue(cell_min, scene.unit_size)});
        if (cell_hit.hit) piece_distance = cell_hit.distance;
    }
    if (building && nearest_building <= piece_distance) {
        return {CitySelectionKind::Building, *building};
    }
    if (piece.has_value()) {
        return {CitySelectionKind::Piece, piece->portal_index};
    }
    // Ground: road or node (already nearest-of-the-two).
    const auto road = pick_city_road_or_node(map, scene, ro, rd);
    if (road.has_value()) {
        return road->kind == CityRoadPick::Kind::Node
            ? CityAnyPick{CitySelectionKind::Node, road->index}
            : CityAnyPick{CitySelectionKind::Road, road->index};
    }
    return {};
}

// Adds/removes an index from a selection set (multi-select toggle).
void toggle_in_selection(std::vector<std::size_t>& set, std::size_t index) {
    const auto it = std::find(set.begin(), set.end(), index);
    if (it == set.end()) {
        set.push_back(index);
    } else {
        set.erase(it);
    }
}

// Applies a Select-tool pick. Buildings and piece portals support multi-select
// (additive = Shift/Ctrl): plain click replaces the set with the one thing,
// additive toggles it in/out. Roads and nodes are single-select. The active
// single index (selected_city_building / selected_portal) tracks the last click.
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

// Rubber-band box select: every building and piece voxel whose screen point falls
// in the rectangle joins the selection. additive keeps the existing selection;
// otherwise the box replaces it. Buildings and piece portals can't mix, so a box
// that catches both prefers buildings (the common case).
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
                (static_cast<float>(portal.fields[1] * 8 + static_cast<std::int32_t>(bi & 7U)) + 0.5F) * unit,
                (static_cast<float>(portal.fields[2] * 8 + static_cast<std::int32_t>((bi / 0x40U) & 7U)) + 0.5F) * unit,
                (static_cast<float>(portal.fields[3] * 8 + static_cast<std::int32_t>((bi / 8U) & 7U)) + 0.5F) * unit,
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
        ui.selected_city_buildings.clear();
    } else if (!additive) {
        ui.city_selection = CitySelectionKind::None;
        ui.selected_city_buildings.clear();
        ui.selected_portals.clear();
    }
}

// Erase tool: delete whatever is under the cursor. Clears a piece voxel at the
// ground cell, and deletes the nearest building or road the ray hits. Bare nodes
// are left alone (deleting them would orphan roads).
// Deletes exactly the one thing the Erase hover highlights (pick_city_any):
// a building, a road, or a single piece voxel. Junctions are left alone; they
// go away with their roads.
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

GizmoAxis pick_building_gizmo(
    const Camera3D& camera,
    Vector2 viewport_mouse_pos,
    Vector2 viewport_size,
    const SbcBuilding& placement, bool allow_rotation = true, Vector3 offset = {}) {
    const auto layout = building_gizmo_layout(placement, camera, viewport_size, offset);
    const auto width = static_cast<int>(std::max(1.0F, viewport_size.x));
    const auto height = static_cast<int>(std::max(1.0F, viewport_size.y));
    // Forward axis, to reject points behind the camera (GetWorldToScreenEx still
    // returns a finite but mirrored point for those, which made the gizmo pick
    // bogus handles and feel like it "ate" the click).
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

    // XZ plane square: point-in-quad on the projected corners.
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
    // ImGuizmo-style transform gizmo: constant screen size, axis arrows with
    // cone heads, a translucent XZ plane square, and a Y rotation ring. The
    // hovered/active handle turns yellow; everything draws over the scene.
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

    // XZ plane handle: filled square (both sides) plus outline, colored by the
    // axis it is perpendicular to.
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

    // Y rotation ring, drawn as a thin torus of segments so it reads at any zoom.
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
    // Center pivot.
    DrawSphere(layout.origin, r * 0.5F, active == GizmoAxis::None ? Color{235, 235, 235, 255} : highlight);
    rlDrawRenderBatchActive();
    rlEnableDepthTest();
}

// Maps the cursor's ground (or vertical) position during a gizmo drag to an
// absolute grid cell, then applies it via the existing edit functions. Uses the
// drag-start origin captured in ui so motion is absolute, not accumulated.
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
    // Gizmo and rotation pivot sit at the footprint centre (fixed for the drag).
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

// Overlay (not road rendering) showing the active road-laying chain: a marker on
// the pending start node and an axis-snapped preview line to the cursor, so you
// see the segment before clicking. Drawn inside the City 3D pass.
// Pick the rendered surface, not a projection onto Y=0. The same query drives
// the preview and the click so placing and dragging agree on all three axes.
// Piece tool preview: the chosen block mesh, turned by the brush rotation,
// drawn see-through in the cell a click will fill (same origin and rotation as
// the city piece renderer, which lifts pieces 0.25 above their cell).
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
    // A pin above the cell: one cell is only a few pixels at city zoom.
    const Vector3 pin_base{(cell.x + 0.5F) * unit, (cell.y + 1.0F) * unit, (cell.z + 0.5F) * unit};
    const Vector3 pin_top = Vector3Add(pin_base, {0.0F, unit * 6.0F, 0.0F});
    DrawLine3D(pin_base, pin_top, edge);
    DrawSphere(pin_top, unit * 0.5F, edge);
}

// Nearest visible surface under the cursor: the hit point and its normal.
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
    mesh_hit(scene.city_tile_buffer, nullptr);
    mesh_hit(project.portal_render_buffer, nullptr);
    if (project.city) {
        const auto& placement_asset_indices = resolve_placement_asset_indices(project);
        for (std::size_t placement_index = 0; placement_index < project.city->data.buildings.size(); ++placement_index) {
            const auto& placement = project.city->data.buildings[placement_index];
            if (!placement_asset_indices[placement_index].has_value()) continue;
            const auto index = *placement_asset_indices[placement_index];
            if (index >= project.building_render_buffers.size()) continue;
            const auto& b = project.buildings[index].data;
            // Broad phase before intersecting the actual mesh triangles.
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

// Nearest grid point to the surface under the cursor: road nodes and building
// anchors sit on grid points.
std::optional<CityGridPoint> city_surface_point_under_mouse(
    EditorProject& project, const Camera3D& camera, Vector2 mouse, Vector2 size) {
    const auto hit = city_surface_hit_under_mouse(project, camera, mouse, size);
    if (!hit) return std::nullopt;
    return CityGridPoint{static_cast<int>(std::lround(hit->point.x / 4)),
        static_cast<int>(std::lround(hit->point.y / 4)), static_cast<int>(std::lround(hit->point.z / 4))};
}

// Piece cell against the surface under the cursor. Piece cells span
// [n*4, n*4+4) on every axis, so step a little off the surface along its normal
// and floor: a side or top face picks the empty neighbour, never the cell itself.
std::optional<CityGridPoint> city_surface_cell_under_mouse(
    EditorProject& project, const Camera3D& camera, Vector2 mouse, Vector2 size) {
    const auto hit = city_surface_hit_under_mouse(project, camera, mouse, size);
    if (!hit) return std::nullopt;
    const auto p = Vector3Add(hit->point, Vector3Scale(hit->normal, 0.5F));
    return CityGridPoint{static_cast<int>(std::floor(p.x / 4)),
        static_cast<int>(std::floor(p.y / 4)), static_cast<int>(std::floor(p.z / 4))};
}

// Anchor handles use screen-space drag distance: no jump when a long arrow is
// grabbed, and the model keeps its original origin throughout the gesture.
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

// Archive workspace follows CSXWebParser's file list, texture/material pairing,
// extraction and build workflow. Payloads stay opaque unless explicitly edited.
void draw_csx_editor(bool& open, EditorProject* project) {
    static CsxArchive archive{0xc5f17e01U, 0, 0, {}};
    static std::array<char, 1024> input_path{};
    static std::array<char, 1024> output_path{};
    static std::vector<std::size_t> selected;
    static std::string message;
    static bool dirty = false;
    if (!open) return;
    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("CSX archive", &open)) { ImGui::End(); return; }
    const auto u32 = [](const auto& bytes, std::size_t at) {
        if (at > bytes.size() || bytes.size() - at < 4) throw std::runtime_error("Truncated texture header");
        return static_cast<std::uint32_t>(bytes[at]) | (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
            (static_cast<std::uint32_t>(bytes[at + 2]) << 16) | (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
    };
    const auto put = [](auto& bytes, std::size_t at, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    const auto texture_parts = [&](const CsxEntry& entry) {
        const auto image_size = u32(entry.payload, 0x44);
        const auto material_size = u32(entry.payload, 0x48);
        if (entry.payload.size() < 0x4c || static_cast<std::uint64_t>(image_size) + material_size > entry.payload.size() - 0x4c)
            throw std::runtime_error("Texture payload is truncated: " + entry.name);
        return std::pair{image_size, material_size};
    };
    const auto checked_name = [](const std::string& name) {
        if (name.empty() || name.size() > 51 || name == "." || name == ".." ||
            name.find_first_of("/\\:") != std::string::npos)
            throw std::runtime_error("Asset name must be 1-51 characters without path separators");
    };
    const auto import = [&](const std::filesystem::path& path) {
        const auto extension = lower_extension(path);
        auto bytes = read_file_bytes(path);
        if (extension == ".csx") {
            auto loaded = parse_csx(bytes);
            if (loaded.magic != 0xc5f17e01U || loaded.entries.size() != loaded.entry_count)
                throw std::runtime_error("Invalid CSX directory");
            for (const auto& entry : loaded.entries) {
                checked_name(entry.name);
                if (entry.payload.size() != entry.size) throw std::runtime_error("Truncated CSX entry: " + entry.name);
                if (entry.kind == CsxEntryKind::Texture) texture_parts(entry);
            }
            archive.entries.insert(archive.entries.end(), loaded.entries.begin(), loaded.entries.end());
        } else {
            CsxEntry entry{};
            entry.name = path.stem().string(); checked_name(entry.name);
            if (extension == ".sbb") {
                entry.kind = CsxEntryKind::Sbb; entry.type = 0x585a0002;
                entry.payload = rebuild_sbb(parse_sbb(bytes));
                entry.payload[0] = 1; // CSX geometry sentinel, not loose-file version.
            } else if (extension == ".sbl") {
                entry.kind = CsxEntryKind::Sbl; entry.type = 0x585a0001;
                const auto model = parse_sbl(bytes);
                if (!model.block) bytes = rebuild_sbl_buildblock(model);
                else if (model.version == 1) {
                    // Loose block v1 has no trailing v2 records. Materialize the
                    // empty section before applying the archive sentinel.
                    bytes.resize(bytes.size() + 4, 0);
                }
                entry.payload = std::move(bytes); entry.payload[0] = 1;
            } else if (extension == ".png") {
                if (bytes.size() < 8 || bytes[0] != 137 || bytes[1] != 'P' || bytes[2] != 'N' || bytes[3] != 'G')
                    throw std::runtime_error("Expected a PNG image");
                entry.kind = CsxEntryKind::Texture; entry.type = 0x585a0004;
                entry.payload.resize(0x4c, 0); put(entry.payload, 0, 1);
                put(entry.payload, 0x44, static_cast<std::uint32_t>(bytes.size()));
                entry.payload.insert(entry.payload.end(), bytes.begin(), bytes.end());
            } else throw std::runtime_error("Choose a CSX, SBB, SBL or PNG file");
            archive.entries.push_back(std::move(entry));
        }
        dirty = true;
        message = "Added " + path.filename().string();
    };
    // File operations are the external boundary: keep failures visible in the
    // workspace and retain the archive so a bad file cannot discard other edits.
    const auto action = [&](auto operation) {
        try { operation(); }
        catch (const std::exception& error) { message = error.what(); }
    };
    ImGui::BeginDisabled(project == nullptr);
    if (ImGui::Button("Use open map assets")) action([&] {
        promote_city(*project);
        archive = project_archive(*project);
        selected.clear(); dirty = true; message.clear();
    });
    ImGui::EndDisabled();
    ImGui::TextWrapped("CSX stores assets. File > Save map writes the city layout and its asset archive together.");
    ImGui::InputText("Input file", input_path.data(), input_path.size());
#ifndef __EMSCRIPTEN__
    if (ImGui::Button("Browse and add...")) action([&] {
        const auto paths = pfd::open_file("Add archive assets", "", {"Assets", "*.csx *.sbb *.sbl *.png"}, pfd::opt::multiselect).result();
        for (const auto& path : paths) import(path);
    });
    ImGui::SameLine();
#endif
    if (ImGui::Button("Add file")) action([&] { import(input_path.data()); });
    ImGui::SameLine();
    ImGui::BeginDisabled(selected.empty());
    if (ImGui::Button("Remove selected")) {
        std::sort(selected.rbegin(), selected.rend());
        for (const auto index : selected) if (index < archive.entries.size()) archive.entries.erase(archive.entries.begin() + index);
        selected.clear(); dirty = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear archive")) ImGui::OpenPopup("Clear archive?");
    if (ImGui::BeginPopupModal("Clear archive?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Discard all files in this archive workspace?");
        if (ImGui::Button("Clear")) { archive.entries.clear(); selected.clear(); dirty = false; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(); if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginTable("##archive_files", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 220))) {
        ImGui::TableSetupColumn("Name"); ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("Bytes"); ImGui::TableSetupColumn("Material");
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < archive.entries.size(); ++i) {
            const auto& entry = archive.entries[i];
            ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); ImGui::TableNextColumn();
            const auto it = std::find(selected.begin(), selected.end(), i);
            if (ImGui::Selectable(entry.name.c_str(), it != selected.end(), ImGuiSelectableFlags_SpanAllColumns)) {
                if (!ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeySuper) selected = {i};
                else if (it == selected.end()) selected.push_back(i); else selected.erase(it);
            }
            ImGui::TableNextColumn();
            if (entry.kind == CsxEntryKind::Sbl) {
                try { ImGui::TextUnformatted(parse_sbl(entry.payload).block ? "Block mesh" : "Buildblock"); }
                catch (const std::exception&) { ImGui::TextUnformatted("Invalid SBL"); }
            } else ImGui::TextUnformatted(entry.kind == CsxEntryKind::Sbb ? "Building" : entry.kind == CsxEntryKind::Texture ? "Texture" : "Unknown");
            ImGui::TableNextColumn(); ImGui::Text("%zu", entry.payload.size()); ImGui::TableNextColumn();
            if (entry.kind == CsxEntryKind::Texture && entry.payload.size() >= 0x4c)
                ImGui::TextUnformatted(u32(entry.payload, 0x48) ? "Attached" : "None");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (selected.size() == 1 && selected[0] < archive.entries.size()) {
        auto& entry = archive.entries[selected[0]];
        std::array<char, 52> name{};
        std::copy_n(entry.name.data(), std::min(entry.name.size(), name.size() - 1), name.data());
        if (ImGui::InputText("Asset name", name.data(), name.size(), ImGuiInputTextFlags_EnterReturnsTrue)) action([&] {
            checked_name(name.data()); entry.name = name.data(); dirty = true;
        });
        if (entry.kind == CsxEntryKind::Texture) {
            if (ImGui::Button("Attach material PNG...")) action([&] {
                std::filesystem::path path{input_path.data()};
#ifndef __EMSCRIPTEN__
                const auto paths = pfd::open_file("Material image", "", {"PNG image", "*.png"}).result();
                if (paths.empty()) return;
                path = paths.front();
#endif
                const auto bytes = read_file_bytes(path);
                if (lower_extension(path) != ".png" || bytes.size() < 8 || bytes[0] != 137 || bytes[1] != 'P' || bytes[2] != 'N' || bytes[3] != 'G')
                    throw std::runtime_error("Material must be a PNG image");
                const auto [image_size, material_size] = texture_parts(entry);
                const auto name = path.stem().string();
                if (name.size() > 63) throw std::runtime_error("Material name is too long");
                entry.payload.resize(0x4c + image_size);
                std::fill(entry.payload.begin() + 4, entry.payload.begin() + 0x44, 0);
                std::copy(name.begin(), name.end(), entry.payload.begin() + 4);
                put(entry.payload, 0x48, static_cast<std::uint32_t>(bytes.size()));
                entry.payload.insert(entry.payload.end(), bytes.begin(), bytes.end()); dirty = true;
            });
            ImGui::SameLine();
            if (ImGui::Button("Remove material")) action([&] {
                const auto [image_size, material_size] = texture_parts(entry);
                entry.payload.resize(0x4c + image_size);
                std::fill(entry.payload.begin() + 4, entry.payload.begin() + 0x44, 0);
                put(entry.payload, 0x48, 0); dirty = true;
            });
        }
    }
    ImGui::InputText("Output path", output_path.data(), output_path.size());
    if (ImGui::Button(dirty ? "Build CSX *" : "Build CSX")) action([&] {
        std::filesystem::path path{output_path.data()};
#ifndef __EMSCRIPTEN__
        if (path.empty()) path = pfd::save_file("Build CSX", "build.csx", {"CSX archive", "*.csx"}).result();
#endif
        if (path.empty()) return;
        if (path.extension() != ".csx") path += ".csx";
        for (std::size_t i = 0; i < archive.entries.size(); ++i) {
            checked_name(archive.entries[i].name);
            for (std::size_t j = 0; j < i; ++j)
                if (archive.entries[j].type == archive.entries[i].type && archive.entries[j].name == archive.entries[i].name)
                    throw std::runtime_error("Rename duplicate asset: " + archive.entries[i].name);
        }
        write_file_bytes(path, build_csx(archive)); dirty = false; message.clear();
    });
    ImGui::SameLine();
    ImGui::BeginDisabled(selected.empty());
    if (ImGui::Button("Extract selected to folder")) action([&] {
        std::filesystem::path folder{output_path.data()};
#ifndef __EMSCRIPTEN__
        if (folder.empty()) folder = pfd::select_folder("Extract selected files").result();
#endif
        if (folder.empty()) return;
        if (!std::filesystem::is_directory(folder)) throw std::runtime_error("Output must be an existing folder");
        for (auto i : selected) {
            const auto& entry = archive.entries[i]; checked_name(entry.name);
            if (entry.kind == CsxEntryKind::Texture) {
                const auto [image_size, material_size] = texture_parts(entry);
                write_file_bytes(folder / (entry.name + ".png"), {entry.payload.begin() + 0x4c, entry.payload.begin() + 0x4c + image_size});
                if (material_size) write_file_bytes(folder / (entry.name + ".material.png"),
                    {entry.payload.begin() + 0x4c + image_size, entry.payload.begin() + 0x4c + image_size + material_size});
            } else {
                auto bytes = entry.payload;
                if (entry.kind == CsxEntryKind::Sbb) bytes = normalize_packed_sbb_bytes(bytes);
                if (entry.kind == CsxEntryKind::Sbl) bytes = normalize_packed_sbl_bytes(bytes);
                write_file_bytes(folder / (entry.name + (entry.kind == CsxEntryKind::Sbb ? ".sbb" : entry.kind == CsxEntryKind::Sbl ? ".sbl" : ".bin")), bytes);
            }
        }
        message = "Extracted selected files to " + folder.string();
    });
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", message.c_str());
    ImGui::End();
}

void draw_road_lay_preview(const EditorProject& project, const EditorUi& ui, std::optional<CityGridPoint> cursor) {
    if (ui.city_tool != CityTool::Road || !project.city) {
        return;
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
        return;
    }

    const auto& start = *ui.pending_road_start;
    const Vector3 start_pos{static_cast<float>(start.x) * unit, ground(start.x, start.z, start.y), static_cast<float>(start.z) * unit};
    DrawSphere(start_pos, unit * 0.8F, Color{120, 230, 130, 235});

    if (cursor.has_value()) {
        const auto end = snap_to_axis(CityGridPoint{start.x, start.y, start.z}, *cursor);
        const Vector3 end_pos{static_cast<float>(end.x) * unit, ground(end.x, end.z, start.y), static_cast<float>(end.z) * unit};
        // A strip as wide as the road the click will lay, so its footprint is
        // readable at city zoom (a 1px line was not).
        const auto along = Vector3Subtract(end_pos, start_pos);
        const auto side = Vector3Normalize(Vector3{-along.z, 0.0F, along.x});
        const auto half = Vector3Scale(side, static_cast<float>(ui.new_road_width) * unit * 0.5F);
        const Vector3 a = Vector3Add(start_pos, half), b = Vector3Subtract(start_pos, half);
        const Vector3 c = Vector3Subtract(end_pos, half), d = Vector3Add(end_pos, half);
        const Color fill{255, 230, 120, 90};
        const Color edge{255, 240, 140, 255};
        DrawTriangle3D(a, b, c, fill); DrawTriangle3D(c, b, a, fill);
        DrawTriangle3D(a, c, d, fill); DrawTriangle3D(d, c, a, fill);
        DrawLine3D(a, d, edge); DrawLine3D(b, c, edge);
        DrawSphere(end_pos, unit * 0.6F, Color{255, 220, 110, 220});
    }
}

struct FocusTarget {
    Vector3 point{};
    float radius{};
};

std::optional<FocusTarget> focus_target_for_selection(const EditorProject& project, const EditorUi& ui) {
    constexpr float unit = 4.0F;
    // Pull-back so the bounding sphere of half-extent r fits a ~55deg vertical FOV
    // with a little margin: r / sin(fov/2) * margin.
    const auto fit_radius = [](float r) { return std::max(unit * 4.0F, r / 0.46F * 1.25F); };

    if (ui.tab == EditorTab::City && project.city) {
        const auto& map = project.city->data;
        if (ui.city_selection == CitySelectionKind::Building && !map.buildings.empty()) {
            // Aim at the placed building's mesh centre and fit its full extent.
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
            // Centre of the selected portal's 8x8x8 voxel volume.
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

// Starts an eased camera glide to frame the given point from the camera's current
// viewing direction (so the move feels like an orbit-in, not a teleport).
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

// Advances the focus glide; applies eased interpolation to the camera each frame
// until done. Smoothstep easing reads as a gentle ease-in/ease-out.
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
    if (const auto target = focus_target_for_selection(project, ui)) {
        start_focus_animation(ui, camera, target->point, target->radius);
        return;
    }
    Camera3D framed = camera;
    focus_camera_on_city(framed, project);
    start_focus_animation(ui, camera, framed.target, 130.0F);
}

bool asset_search_matches(const EditorUi& ui, std::string_view name) {
    const auto& query = ui.asset_search[static_cast<std::size_t>(ui.tab)];
    if (query[0] == '\0') {
        return true;
    }
    return lower_name(name).find(lower_name(query.data())) != std::string::npos;
}

// Handles global commands first, then viewport-only editing keys. The caller
// already excludes active text fields and freecam flight.
void handle_editor_hotkeys(
    EditorProject& project,
    EditorUi& ui,
    Camera3D& camera,
    std::string& status,
    bool viewport_hovered) {
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);

    // Global shortcuts (rebindable in Preferences).
    if (action_pressed(ui, EditorAction::Save)) {
        save_active_tab(project, ui, status);
        return;
    }
    // City and asset edits keep separate histories; the current tab picks one so
    // undo never silently reverts a different asset type.
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
    // Number keys pick tools in both modes: 1-5 in the city, 1-4 in the
    // building editor (Select, Add block, Paint, Anchor).
    if (ui.tab == EditorTab::Buildings) {
        const std::pair<EditorAction, BuildingTool> building_tool_actions[] = {
            {EditorAction::ToolSelect, BuildingTool::Select}, {EditorAction::ToolBuilding, BuildingTool::PlaceBlock},
            {EditorAction::ToolRoad, BuildingTool::PaintTexture}, {EditorAction::ToolPiece, BuildingTool::Anchor},
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

    // Esc cancels an in-progress gizmo drag, restoring the captured position.
    if (IsKeyPressed(KEY_ESCAPE) && ui.gizmo_active != GizmoAxis::None) {
        if (project.city) for (const auto& [index, start] : ui.gizmo_start_buildings)
            if (index < project.city->data.buildings.size()) project.city->data.buildings[index] = start;
        ui.gizmo_start_buildings.clear();
        ui.gizmo_active = GizmoAxis::None;
        return;
    }
    // Esc ends an in-progress road-laying chain.
    if (IsKeyPressed(KEY_ESCAPE) && ui.pending_road_start.has_value()) {
        end_road_chain(ui);
        return;
    }

    // Destructive and transform keys belong to the 3D canvas. Requiring the
    // pointer over the rendered image prevents Delete, arrows, and placement keys
    // from firing while the user works in another panel.
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
        const bool nudge_up = action_pressed(ui, EditorAction::NudgeUp);
        const bool nudge_down = action_pressed(ui, EditorAction::NudgeDown);
        const bool rotate = !brush_tool && ui.city_selection == CitySelectionKind::Building && action_pressed(ui, EditorAction::RotateSelection);
        const bool duplicate = action_pressed(ui, EditorAction::DuplicateSelection);
        const bool erase = action_pressed(ui, EditorAction::DeleteSelection);
        // One snapshot per editing keypress.
        const bool edit_key = nudge_left || nudge_right || nudge_forward || nudge_back || nudge_up || nudge_down ||
            rotate || duplicate || erase ||
            IsKeyPressed(KEY_LEFT_BRACKET) || IsKeyPressed(KEY_RIGHT_BRACKET) ||
            IsKeyPressed(KEY_SEMICOLON) || IsKeyPressed(KEY_APOSTROPHE) ||
            IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_EQUAL);
        if (edit_key && ui.city_selection != CitySelectionKind::None) {
            begin_city_action(project, ui, "edit selection");
        }
        // Edit keys act on the current selection (Select tool), regardless of tool.
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
            if (erase) {
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
        }
    } else if (ui.tab == EditorTab::Buildings && !project.buildings.empty()) {
        ui.selected_building = std::min(ui.selected_building, project.buildings.size() - 1U);
        const auto occupied = occupied_cell_indices(project.buildings[ui.selected_building].data);
        if (!occupied.empty() && action_pressed(ui, EditorAction::DeleteSelection)) {
            auto& building = project.buildings[ui.selected_building].data;
            const auto index = occupied[std::min(ui.selected_building_cell, occupied.size() - 1)];
            auto& cell = building.cells[index];
            cell.words.fill(0); cell.item_set_index = 0; cell.payloads.clear(); cell.occupied = false;
            cell.shorts.fill(0xffffU);
            mark_building_changed(project, ui);
        }
        if (!occupied.empty() && action_pressed(ui, EditorAction::NextCell)) {
            if (shift) {
                ui.selected_building_cell = ui.selected_building_cell == 0U ? occupied.size() - 1U : ui.selected_building_cell - 1U;
            } else {
                ui.selected_building_cell = (ui.selected_building_cell + 1U) % occupied.size();
            }
        }
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

// Frees the thumbnail RenderTextures owned by a project before it is replaced or
// destroyed (RenderTexture2D is a POD, so it has no destructor to do this).
void free_project_thumbnails(EditorProject& project) {
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

// Warms the thumbnail caches a few entries per frame after load so the galleries
// open instantly, then clears the pregenerating flag. Thumbnails need the GL
// context, so this runs on the main thread, paced to avoid a long stall.
void service_pregeneration(EditorProject& project) {
    MAPED_PROFILE_FUNCTION();
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

// Centered loading overlay with progress bars while the project warms up (render
// buffers building/uploading, thumbnails pregenerating). Hidden once everything is
// ready so it never blocks normal editing.
// Roads and terrain only build while the City view is open, so outside it
// that step is not waited on (it would otherwise sit "Queued" forever).
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

// Paints one standalone frame with a centered "Parsing <folder>..." message.
// load_project_folder parses every asset synchronously on the main thread (and
// uploads textures via the GPU), so it cannot move to a worker; presenting this
// frame just before the blocking call gives the user a painted load screen
// instead of a frozen/blank window during the parse.
void present_parsing_frame(const std::filesystem::path& folder) {
    MAPED_PROFILE_FUNCTION();
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

    rlImGuiEnd();
    EndDrawing();
}


} // namespace map_editor::app_internal

namespace map_editor {

int run_editor_app() {
    MAPED_PROFILE_FUNCTION();
    using namespace app_internal;
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1440, 900, "map_editor_cpp");
    SetWindowMinSize(1100, 700);
    SetTargetFPS(60);
    // Esc must not quit the app: it is used to cancel a gizmo drag, and closing
    // is gated by the unsaved-changes prompt below. KEY_NULL also removes Esc
    // from WindowShouldClose().
    SetExitKey(KEY_NULL);
    // Push the far clip plane way out (default is 1000). The city spans thousands
    // of world units at 4 units/cell, so the default plane clips distant terrain
    // and buildings. rlgl reads these on every BeginMode3D, so one call sticks.
    rlSetClipPlanes(0.05, 100000.0);

    EditorUi ui;
    load_preferences(ui);
#ifdef __EMSCRIPTEN__
    // ImGui reads imgui.ini on its first frame; bring back the saved layout.
    web_restore_file("imgui.ini", "map_editor.imgui_ini");
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

    // Close gating: a close request (window X, Alt+F4, Exit menu) sets this; with
    // unsaved changes it opens a save prompt and blocks exit until resolved.
    bool running = true;
    bool close_requested = false;
    bool open_unsaved_prompt = false;

    // Deferred project load. A drag-drop / Open Folder sets pending_load_folder;
    // the loop then presents one "Parsing..." frame before running the blocking
    // synchronous parse, so the load screen actually paints instead of freezing
    // on a blank window.
    std::optional<std::filesystem::path> pending_load_folder;
#ifndef __EMSCRIPTEN__
    std::optional<std::future<std::pair<std::optional<EditorProject>, std::string>>> load_future;
    auto load_stage = std::make_shared<std::atomic<int>>(0);
#endif
    // Optional auto-load: if MAPED_AUTOLOAD names a map folder, queue it at
    // startup. Convenient for profiling/screenshot runs that need a known map
    // open without the manual Open Folder step; no effect when the var is unset.
#ifdef __EMSCRIPTEN__
    // Closing or reloading the tab would drop unsaved edits; let the browser ask.
    EM_ASM({
        window.addEventListener('beforeunload', (event) => {
            if (window.mapEditorUnsaved) { event.preventDefault(); event.returnValue = ''; }
        });
        // Keep the browser from acting on editor keys (Ctrl+S "save page",
        // Ctrl+1 "switch tab", Tab moving focus off the canvas, F3 find...).
        // The editor still receives them. Chrome's reserved Ctrl+N/T/W and
        // clipboard/select-all/reload pass through.
        window.addEventListener('keydown', (event) => {
            const key = event.key.toLowerCase();
            const passthrough = ['c', 'v', 'x', 'a', 'r', 'n', 't', 'w', 'q'];
            if ((event.ctrlKey || event.metaKey) && !passthrough.includes(key)) event.preventDefault();
            else if (key === 'tab' || key === 'f3' || key === "'" || key === '/') event.preventDefault();
        });
    });
#endif
    if (const char* env = std::getenv("MAPED_AUTOLOAD")) {
        pending_load_folder = std::filesystem::path{env};
    }
#ifdef __EMSCRIPTEN__
#ifdef MAP_EDITOR_CPP_WEB_PRELOAD_ROOT
    // Only QA builds define this path. The normal browser build stays empty and
    // asks the user to choose or drop a project folder.
    if (!pending_load_folder) {
        pending_load_folder = std::filesystem::path{MAP_EDITOR_CPP_WEB_PRELOAD_ROOT};
    }
#endif
#endif
    // Repeatable visual QA: capture the loaded editor after 300 frames. The
    // optional tab selects city, buildings, pieces, buildblocks, or blocks.
    std::optional<std::filesystem::path> screenshot_path;
    if (const char* env = std::getenv("MAPED_SCREENSHOT")) {
        screenshot_path = std::filesystem::path{env};
    }
    const std::string screenshot_tab = std::getenv("MAPED_SCREENSHOT_TAB") == nullptr
        ? std::string{}
        : std::getenv("MAPED_SCREENSHOT_TAB");
    const int screenshot_frame_target = std::getenv("MAPED_SCREENSHOT_FRAMES") == nullptr
        ? 300
        : std::max(1, std::atoi(std::getenv("MAPED_SCREENSHOT_FRAMES")));
    int screenshot_ready_frames = 0;

    // Mesh-export options dialog. Opened by File > Export Mesh; the popup shows a
    // context-aware title + what-to-export checkboxes, then runs the native save
    // dialog. Options persist across exports during the session.
    bool open_export_popup = false;
    ExportOptions export_options{};
    while (running) {
        MAPED_PROFILE_BLOCK("editor frame");
        gfx_debug_stats.reset_frame();
        gfx_debug_stats.push_frame_time(GetFrameTime() * 1000.0F);
        gfx_debug_stats.add_phase_ms(GfxDebugTimingPhase::TotalFrame, GetFrameTime() * 1000.0);
        g_gfx_debug_stats = &gfx_debug_stats;

        // The OS close button / Alt+F4 raise WindowShouldClose(); route it through
        // the same gate as the Exit menu instead of quitting immediately.
        if (WindowShouldClose()) {
            close_requested = true;
        }

        if (!io.WantCaptureKeyboard && action_pressed(ui, EditorAction::GraphicsDebug)) {
            ui.show_graphics_debug = !ui.show_graphics_debug;
        }
        // Tab switches requested by the tab bar, shortcuts, or tool windows.
        if (ui.requested_tab.has_value()) {
            const auto next = *ui.requested_tab;
            ui.requested_tab.reset();
            if (next != ui.tab) {
                ui.tab = next;
                ui.gizmo_active = GizmoAxis::None;
                ui.box_select_active = false;
                if (project) {
                    if (next == EditorTab::City) {
                        focus_camera_on_city(camera, *project);
                    } else {
                        camera = default_camera();
                        if (next == EditorTab::Buildings) focus_current_selection(*project, ui, camera);
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
            pending_load_folder = std::move(g_web_uploaded_folder);
            g_web_uploaded_folder.reset();
        }
#endif

        // Recent status/action messages, newest first, for the status bar.
        static std::deque<std::string> status_history;
        static Vector2 viewport_mouse_pos = {0, 0};
        static bool is_viewport_hovered = false;
        const bool show_3d_viewport = true;

        if (is_viewport_hovered && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
            DisableCursor();
        }
        if (IsMouseButtonReleased(MOUSE_BUTTON_RIGHT) && IsCursorHidden()) {
            EnableCursor();
        }
        if (IsCursorHidden()) {
            // Manual fly cancels any in-progress focus glide so they never fight.
            // While flying, the wheel sets fly speed; otherwise it zooms.
            ui.camera_fly_speed = adjust_camera_fly_speed(ui.camera_fly_speed, GetMouseWheelMove());
            ui.focus_animating = false;
            update_free_fly_camera(camera, ui.camera_fly_speed);
        } else {
            const MapScene* pivot_scene = project && ui.tab == EditorTab::City ? &project->city_scene_cache : nullptr;
            update_orbit_camera(camera, ui, viewport_mouse_pos, last_viewport_size, is_viewport_hovered, pivot_scene);
        }
        if (project && ui.focus_selection_requested) {
            ui.focus_selection_requested = false;
            focus_current_selection(*project, ui, camera);
        }
        update_focus_animation(ui, camera);

        if (project) {
            project->defer_heavy_rebuilds = IsMouseButtonDown(MOUSE_BUTTON_LEFT) || ui.gizmo_active != GizmoAxis::None;
            commit_pending_edits(*project, ui);
        }
#ifdef __EMSCRIPTEN__
        {
            // ImGui rewrites imgui.ini in memory; mirror it every few seconds.
            static double last_layout_store = 0.0;
            if (GetTime() - last_layout_store > 5.0) {
                last_layout_store = GetTime();
                web_store_file("imgui.ini", "map_editor.imgui_ini");
            }
        }
        {
            static bool reported_unsaved = false;
            const bool unsaved = has_unsaved_changes(ui);
            if (unsaved != reported_unsaved) {
                reported_unsaved = unsaved;
                EM_ASM({ window.mapEditorUnsaved = !!$0; }, unsaved ? 1 : 0);
            }
        }
#endif
        // Editor hotkeys: only when ImGui is not capturing the keyboard (no active
        // text field) and the freecam is not flying, so they never collide with
        // typing or WASD movement.
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
        if (show_3d_viewport && last_viewport_size.x > 0 && last_viewport_size.y > 0) {
            begin_game_texture_mode(viewport_texture);
            ClearBackground(BLACK);
            clear_game_render_packed_depth();
            clear_game_render_shadow_factor();

            BeginMode3D(camera);
            set_game_world_time(ui.world_time_hours);
            set_game_surface_camera(camera.position);
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
                        // Any building, road or piece change moves the fingerprint, and a
                        // rebuild regenerates the whole city's roads and terrain. Wait for
                        // the gesture to end and the map to hold still briefly, so a drag
                        // or a burst of nudges costs one rebuild instead of one per frame.
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
                                project->city_scene_cache_valid = true;
                                project->city_scene_gpu_valid = false;
                            }
                        }
                        if (project->city_scene_cache_valid && !project->city_scene_gpu_valid) {
                            // Road/curb tiles are cheap: re-upload them with every
                            // scene change. The terrain mesh is heavy, so only
                            // re-upload it when terrain-affecting inputs actually
                            // changed (intersections/buildings/pieces) - a road or
                            // street-width edit reuses the existing terrain GPU mesh.
                            MAPED_PROFILE_BLOCK("upload city scene gpu models");
                            unload_city_render_model(project->city_tile_gpu_model);
                            project->city_tile_gpu_model =
                                upload_city_tile_model(project->city_scene_cache.city_tile_buffer, *project);

                            const auto terrain_fp = city_terrain_fingerprint(project->city->data);
                            if (!project->terrain_gpu_valid || terrain_fp != project->terrain_gpu_fingerprint) {
                                unload_city_render_model(project->terrain_gpu_model);
                                if (project->city_scene_cache.terrain.has_value()) {
                                    project->terrain_gpu_model =
                                        upload_city_terrain_model(project->city_scene_cache.terrain->mesh, *project);
                                }
                                project->terrain_gpu_fingerprint = terrain_fp;
                                project->terrain_gpu_valid = true;
                            }
                            project->city_scene_gpu_valid = true;
                        }
                        const auto& city_scene = project->city_scene_cache;
                        const bool game_preview = ui.render_roads;
                        draw_city_scene(*project, city_scene, selected_street, game_preview, ui.render_terrain);
                        if (!game_preview) {
                            draw_city_editor_overlay(*project, city_scene);
                        }
                        // Unified hover: the Select tool (or Erase) highlights
                        // whatever is under the cursor - building, road, node, or
                        // piece - so a click is predictable. nearest wins.
                        const SbcBuilding* hovered_building = nullptr;
                        ui.hover_city_intersection.reset();
                        ui.hover_city_street.reset();
                        ui.hover_piece_voxel.reset();
                        const bool unified_hover = (ui.city_tool == CityTool::Select || ui.city_tool == CityTool::Erase);
                        if (unified_hover && is_viewport_hovered && !IsCursorHidden() && ui.gizmo_active == GizmoAxis::None) {
                            const auto pick_ray = get_viewport_mouse_ray(camera, viewport_mouse_pos, last_viewport_size);
                            const Vec3 ro{pick_ray.position.x, pick_ray.position.y, pick_ray.position.z};
                            const Vec3 rd{pick_ray.direction.x, pick_ray.direction.y, pick_ray.direction.z};
                            // The pick tests every building triangle along the ray; reuse
                            // last frame's answer while the ray and the map are unchanged.
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
                                // Resolve the hovered piece to its single world voxel.
                                if (const auto p = pick_city_piece(project->city->data, ro, rd, city_scene.unit_size);
                                    p.has_value() && p->portal_index < project->city->data.portals.size()) {
                                    const auto& portal = project->city->data.portals[p->portal_index];
                                    ui.hover_piece_voxel = CityGridPoint{
                                        portal.fields[1] * 8 + static_cast<std::int32_t>(p->block_index & 7U),
                                        portal.fields[2] * 8 + static_cast<std::int32_t>((p->block_index / 0x40U) & 7U),
                                        portal.fields[3] * 8 + static_cast<std::int32_t>((p->block_index / 8U) & 7U),
                                    };
                                }
                            }
                        }
                        outline_selected_building =
                            (ui.city_selection == CitySelectionKind::Building) ? selected_building : nullptr;
                        outline_hovered_building = hovered_building;
                        {
                            GfxDebugScopedTimer timer{gfx_debug_stats, GfxDebugTimingPhase::BuildingDraw};
                            draw_city_building_models_full_detail(
                                *project,
                                outline_selected_building,
                                camera,
                                last_viewport_size,
                                hovered_building,
                                ui.render_quality);
                        }
                        {
                            GfxDebugScopedTimer timer{gfx_debug_stats, GfxDebugTimingPhase::PieceDraw};
                            const auto selected_portal = ui.city_selection == CitySelectionKind::Piece
                                ? std::optional<std::size_t>{ui.selected_portal}
                                : std::nullopt;
                            draw_city_piece_voxels(*project, ui.portal_preview_textured, selected_portal);
                            // Single-voxel selection/hover glow (not the 8x8x8 portal).
                            if (ui.city_selection == CitySelectionKind::Piece && ui.selected_piece_voxel.has_value()) {
                                draw_city_piece_glow(*ui.selected_piece_voxel, Color{120, 210, 255, 255});
                            }
                            if (ui.hover_piece_voxel.has_value()) {
                                draw_city_piece_glow(*ui.hover_piece_voxel, ui.city_tool == CityTool::Erase
                                    ? Color{255, 90, 80, 220} : Color{200, 240, 255, 200});
                            }
                        }
                        GfxDebugScopedTimer timer{gfx_debug_stats, GfxDebugTimingPhase::PoleDraw};
                        draw_city_traffic_light_poles(city_scene);
                        if (ui.render_shadows) {
                            draw_city_shadow_volumes(*project, camera, ui.shadow_distance);
                        }

                        // Road and Building previews snap to the grid point on the
                        // nearest visible surface; the Piece preview casts for its own
                        // cell below, and Select/Erase use the hover pick instead.
                        const auto needs_city_cursor = ui.city_tool == CityTool::Road || ui.city_tool == CityTool::Building;
                        const std::optional<CityGridPoint> city_cursor = needs_city_cursor
                            ? city_surface_point_under_mouse(*project, camera, viewport_mouse_pos, last_viewport_size)
                            : std::nullopt;
                        // Node selection/hover glow (roads get the silhouette outline).
                        draw_city_road_selection(*project, ui, city_scene);

                        // Placement previews: the road node, the building's footprint
                        // (its silhouette is outlined below), and the piece itself, all
                        // computed exactly as the click will place them. Erase relies on
                        // the hover highlight above.
                        if (city_cursor.has_value() && ui.city_tool == CityTool::Road) {
                            draw_city_cursor(*city_cursor, city_scene.unit_size, ui.city_tool);
                            draw_city_edit_grid(*city_cursor, city_scene);
                        }
                        ui.placement_conflict.clear();
                        if (city_cursor.has_value() && ui.city_tool == CityTool::Building && !project->buildings.empty()) {
                            ghost_building = city_building_placement_at(*project, ui, *city_cursor);
                            ui.placement_conflict = city_building_placement_conflict(*project, *ghost_building);
                            const auto& tmpl = project->buildings[std::min(ui.place_building_template, project->buildings.size() - 1U)];
                            DrawBoundingBox(city_building_bounds(*ghost_building, tmpl.data, city_scene.unit_size),
                                ui.placement_conflict.empty() ? Color{235, 190, 78, 230} : Color{230, 70, 60, 240});
                        }
                        if (ui.city_tool == CityTool::Piece) {
                            if (const auto cell = city_surface_cell_under_mouse(*project, camera, viewport_mouse_pos, last_viewport_size)) {
                                const auto* mesh = project->blocks.empty()
                                    ? nullptr
                                    : &project->blocks[std::min(ui.place_piece_mesh, project->blocks.size() - 1U)].data;
                                draw_piece_ghost(mesh, *cell, static_cast<std::int32_t>(ui.selected_piece_rotation & 3U));
                                draw_city_edit_grid(*cell, city_scene);
                            }
                        }
                        draw_road_lay_preview(*project, ui, city_cursor);

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
                            // Cell under the cursor, for the silhouette hover outline.
                            ui.building_hover_cell.reset();
                            ui.building_hover_face.reset();
                            if (is_viewport_hovered && !IsCursorHidden()) {
                                const auto& bld = project->buildings[ui.selected_building].data;
                                const auto pr = get_viewport_mouse_ray(camera, viewport_mouse_pos, last_viewport_size);
                                const Vec3 ro{pr.position.x, pr.position.y, pr.position.z};
                                const Vec3 rd{pr.direction.x, pr.direction.y, pr.direction.z};
                                const auto pick = pick_building_cell(bld, ro, rd, 4.0F, preview_floor);
                                if (pick.has_value()) {
                                    ui.building_hover_cell = pick->cell_index;
                                    ui.building_hover_face = building_face_from_hit(
                                        bld, bld.cells[pick->cell_index], pick->hit_world, 4.0F).slot;
                                }
                            }
                            auto& preview_model = building_tool_floor_preview_model(*project, ui.selected_building, preview_floor);
                            GfxDebugScopedTimer timer{gfx_debug_stats, GfxDebugTimingPhase::BuildingDraw};
                            draw_sbb_scene_for_app(
                                project->buildings[ui.selected_building].data,
                                preview_model,
                                selected,
                                static_cast<std::uint32_t>(std::max(0, ui.building_view_floor)));
                            if (ui.building_tool == BuildingTool::PaintTexture &&
                                ui.building_hover_cell.has_value() && ui.building_hover_face.has_value()) {
                                const auto& building = project->buildings[ui.selected_building].data;
                                draw_building_paint_face(
                                    building,
                                    building.cells[*ui.building_hover_cell],
                                    *ui.building_hover_face,
                                    4.0F);
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
            if (ui.render_shadows && ui.tab == EditorTab::City && project && project->city) {
                composite_city_shadows(last_viewport_size);
            }
            if (ui.render_sky) {
                draw_game_sky(camera, last_viewport_size, game_render_packed_depth(viewport_texture));
            }
            end_game_texture_mode();

            // Generic silhouette-outline: render a shape into the mask, then trace
            // a colored outline from it. Buildings, roads, and building cells all
            // use this so hover/selection feedback looks the same everywhere.
            auto composite_outline = [&](const std::function<void()>& draw_mask, Color tint) {
                GfxDebugScopedTimer timer{gfx_debug_stats, GfxDebugTimingPhase::OutlineDraw};
                begin_game_texture_mode(building_outline_mask_texture);
                ClearBackground(BLANK);
                BeginMode3D(camera);
                draw_mask();
                EndMode3D();
                end_game_texture_mode();

                begin_game_texture_mode(viewport_texture);
                draw_outline_from_mask(building_outline_mask_texture, last_viewport_size, tint);
                end_game_texture_mode();
            };

            const Color select_tint{220, 32, 32, 255};
            // Erase shows what it will delete in red, not the neutral hover blue.
            const bool erase_hover = ui.tab == EditorTab::City && ui.city_tool == CityTool::Erase;
            const Color hover_tint = erase_hover ? Color{255, 80, 70, 255} : Color{120, 200, 255, 255};

            if (ui.tab == EditorTab::City && project->city) {
                // Outline every selected building (the whole multi-selection).
                if (ui.city_selection == CitySelectionKind::Building) {
                    if (ui.selected_city_buildings.size() > 1U) {
                        // One mask pass covering the whole multi-selection, then one
                        // outline pass, instead of a full mask+outline round trip per
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
                // Road outline: selected road (when a road is selected) + hover,
                // same silhouette look as buildings.
                if (ui.city_selection == CitySelectionKind::Road && !project->city->data.streets.empty()) {
                    const auto sel = std::min(ui.selected_city_street, project->city->data.streets.size() - 1U);
                    composite_outline([&] { draw_city_road_mask(*project, project->city_scene_cache, sel); }, select_tint);
                }
                if (ui.hover_city_street.has_value()) {
                    const auto h = *ui.hover_city_street;
                    composite_outline([&] { draw_city_road_mask(*project, project->city_scene_cache, h); }, hover_tint);
                }
            } else if (ui.tab == EditorTab::Buildings && !project->buildings.empty()) {
                auto& building = project->buildings[std::min(ui.selected_building, project->buildings.size() - 1U)].data;
                const auto occupied = occupied_cell_indices(building);
                if (!occupied.empty()) {
                    const auto& sel_cell = building.cells[occupied[std::min(ui.selected_building_cell, occupied.size() - 1U)]];
                    composite_outline([&] { draw_building_cell_mask(building, sel_cell); }, select_tint);
                }
                // Paint highlights the target face instead of the whole block.
                if (ui.building_tool != BuildingTool::PaintTexture &&
                    ui.building_hover_cell.has_value() && *ui.building_hover_cell < building.cells.size() &&
                    (occupied.empty() || building.cells[*ui.building_hover_cell].index != building.cells[occupied[std::min(ui.selected_building_cell, occupied.size() - 1U)]].index)) {
                    const auto& hov = building.cells[*ui.building_hover_cell];
                    composite_outline([&] { draw_building_cell_mask(building, hov); }, hover_tint);
                }
            }
        }

        if (show_3d_viewport && project && is_viewport_hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !IsCursorHidden() &&
            ui.camera_drag == CameraDrag::None) {
            if (ui.tab == EditorTab::City && project->city) {
                // Name the undo step. Select only mutates if it grabs a gizmo
                // (handled by the gizmo-drag start), so it is excluded here.
                switch (ui.city_tool) {
                    case CityTool::Piece: begin_city_action(*project, ui, "place piece"); break;
                    case CityTool::Erase: begin_city_action(*project, ui, "erase"); break;
                    case CityTool::Building:
                    case CityTool::Road:
                    case CityTool::Select: break;
                }
                // Placement preview and clicks use the same surface intersection.
                const auto city_cursor = city_surface_point_under_mouse(*project, camera, viewport_mouse_pos, last_viewport_size);
                const auto pick_ray = get_viewport_mouse_ray(camera, viewport_mouse_pos, last_viewport_size);
                const Vec3 ray_origin{pick_ray.position.x, pick_ray.position.y, pick_ray.position.z};
                const Vec3 ray_dir{pick_ray.direction.x, pick_ray.direction.y, pick_ray.direction.z};

                if (ui.city_tool == CityTool::Piece && project->city->data.version < 11) {
                    try { promote_city(*project); }
                    catch (const std::exception& error) {
                        project->city_promotion_failed = true;
                        report_failure(status, std::string{"Piece placement blocked: "} + error.what());
                    }
                }
                if (ui.city_tool == CityTool::Select) {
                    // First test the selected building's gizmo handles (drag if hit);
                    // otherwise select whatever is under the cursor (nearest wins).
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
                        // Begin a potential box-select. On release we decide: a tiny
                        // drag is a single click (pick under cursor), a real drag is a
                        // rubber-band box. This lets one gesture do both.
                        ui.box_select_active = true;
                        ui.box_select_start = viewport_mouse_pos;
                    }
                } else if (ui.city_tool == CityTool::Building || ui.city_tool == CityTool::Road) {
                    // Placed on release (below), so a misclick can be dragged away.
                    ui.place_click_pending = true;
                    ui.place_click_start = viewport_mouse_pos;
                } else if (ui.city_tool == CityTool::Piece) {
                    // Seeds the drag stroke, whose layer stays at this cell's height.
                    if (const auto cell = city_surface_cell_under_mouse(*project, camera, viewport_mouse_pos, last_viewport_size)) {
                        set_city_piece_voxel(*project, ui, *cell);
                        ui.drag_stamp_cell = *cell;
                    }
                } else if (ui.city_tool == CityTool::Erase) {
                    // Delete the highlighted thing; the drag pass continues only
                    // once the cursor reaches another cell.
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
                    // Place Block / Paint Texture both need the face under the
                    // cursor, so they pick the cell ray-hit directly for hit_world.
                    ui.selected_building = std::min(ui.selected_building, project->buildings.size() - 1U);
                    const auto& building = project->buildings[ui.selected_building].data;
                    const auto pick_ray = get_viewport_mouse_ray(camera, viewport_mouse_pos, last_viewport_size);
                    const Vec3 ro{pick_ray.position.x, pick_ray.position.y, pick_ray.position.z};
                    const Vec3 rd{pick_ray.direction.x, pick_ray.direction.y, pick_ray.direction.z};
                    constexpr float unit_size = 4.0F;
                    const auto pick = pick_building_cell(building, ro, rd, unit_size, pick_floor);
                    if (pick.has_value()) {
                        if (ui.building_tool == BuildingTool::PlaceBlock) {
                            ui.stroke_block_cells.clear();
                            if (const auto placed = place_building_block(*project, ui, *pick, unit_size, status)) {
                                ui.stroke_block_cells.push_back(*placed);
                            }
                            // Seed the drag dedupe so the held-stamp pass below does
                            // not place a second block into the same neighbour cell
                            // on this very frame.
                            ui.drag_stamp_cell = CityGridPoint{
                                static_cast<std::int32_t>(building.cells[pick->cell_index].x),
                                static_cast<std::int32_t>(building.cells[pick->cell_index].y),
                                static_cast<std::int32_t>(building.cells[pick->cell_index].z),
                            };
                        } else {
                            paint_building_face(*project, ui, *pick, unit_size, status);
                            ui.drag_paint_face = std::pair{pick->cell_index,
                                building_face_from_hit(building, building.cells[pick->cell_index], pick->hit_world, unit_size).slot};
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
                                auto* cell = building_cell_at(edited, x, floor, z);
                                if (!cell) {
                                    SbbCell fresh{};
                                    fresh.index = edited.cells.size(); fresh.x = x; fresh.y = floor; fresh.z = z;
                                    fresh.shorts.fill(0xffffU);
                                    edited.cells.push_back(fresh); cell = &edited.cells.back();
                                }
                                apply_block_config(*cell, ui.block_config, edited);
                                ui.stroke_block_cells.assign(1, static_cast<std::size_t>(cell - edited.cells.data()));
                                mark_building_changed(*project, ui);
                            }
                        } else status = "Click a block face to paint.";
                    }
                }
            }
        }

        if (project && ui.tab == EditorTab::Buildings && !project->buildings.empty() && ui.building_tool == BuildingTool::Anchor) {
            update_anchor_gizmo(*project, ui, camera, viewport_mouse_pos, last_viewport_size, is_viewport_hovered && !IsCursorHidden());
        }

        // Gizmo drag: while the mouse is held after grabbing a handle, map the
        // cursor to the building's new grid position; release ends the drag.
        if (ui.gizmo_active != GizmoAxis::None) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && project && project->city) {
                apply_building_gizmo_drag(*project, ui, camera, viewport_mouse_pos, last_viewport_size);
            }
            if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                ui.gizmo_active = GizmoAxis::None;
            }
        }

        // Building/Road placement fires on release, and only if the cursor stayed
        // put; dragging away from a misclick cancels it.
        if (ui.place_click_pending && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            ui.place_click_pending = false;
            if (show_3d_viewport && project && project->city && ui.tab == EditorTab::City && is_viewport_hovered &&
                !IsCursorHidden() && Vector2Distance(viewport_mouse_pos, ui.place_click_start) < 6.0F) {
                const auto city_cursor = city_surface_point_under_mouse(*project, camera, viewport_mouse_pos, last_viewport_size);
                if (ui.city_tool == CityTool::Building) {
                    // Drop the chosen building template at the cursor.
                    if (city_cursor.has_value()) {
                        begin_city_action(*project, ui, "place building");
                        place_city_building_from_template(*project, ui, *city_cursor);
                    }
                } else if (ui.city_tool == CityTool::Road) {
                    // Continue from an existing node when the ray hits one, else lay
                    // a segment to the cursor cell.
                    const auto pick_ray = get_viewport_mouse_ray(camera, viewport_mouse_pos, last_viewport_size);
                    const Vec3 ray_origin{pick_ray.position.x, pick_ray.position.y, pick_ray.position.z};
                    const Vec3 ray_dir{pick_ray.direction.x, pick_ray.direction.y, pick_ray.direction.z};
                    begin_city_action(*project, ui, "lay road");
                    const auto picked_node = pick_city_intersection(project->city->data, project->city_scene_cache, ray_origin, ray_dir);
                    if (picked_node.has_value()) {
                        const auto& node = project->city->data.intersections[*picked_node];
                        append_city_road_point(*project, ui, {node.x, node.y, node.z});
                    } else if (city_cursor.has_value()) {
                        append_city_road_point(*project, ui, *city_cursor);
                    }
                }
            }
        }

        // Box-select release: decide click vs rubber-band box. A tiny drag is a
        // single click (pick under cursor); a real drag selects everything whose
        // screen point falls inside the rectangle.
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

        // Click-and-hold stamping for the placement tools: while the button is held
        // and the cursor crosses into a new grid cell, stamp again. The per-cell
        // dedupe (drag_stamp_cell) keeps a held drag from re-stamping the same cell
        // every frame, so dragging lays a fast run of pieces/blocks.
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
                    // pieces it just placed does not stack new ones on top.
                    cursor = city_surface_cell_under_mouse(*project, camera, viewport_mouse_pos, last_viewport_size);
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
                const auto pick = pick_building_cell(building, ro, rd, unit_size, pick_floor);
                if (pick.has_value()) {
                    const CityGridPoint cell{
                        static_cast<std::int32_t>(building.cells[pick->cell_index].x),
                        static_cast<std::int32_t>(building.cells[pick->cell_index].y),
                        static_cast<std::int32_t>(building.cells[pick->cell_index].z),
                    };
                    // Never build against a block this drag placed: the cursor stays
                    // on it, so that would grow a column toward the camera every frame.
                    const bool placed_this_drag = std::ranges::find(ui.stroke_block_cells, pick->cell_index) != ui.stroke_block_cells.end();
                    if (!placed_this_drag && (!ui.drag_stamp_cell.has_value() || !(*ui.drag_stamp_cell == cell))) {
                        if (const auto placed = place_building_block(*project, ui, *pick, unit_size, status)) {
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
                constexpr float unit_size = 4.0F;
                if (const auto pick = pick_building_cell(building, ro, rd, unit_size, pick_floor)) {
                    const std::pair face{pick->cell_index,
                        building_face_from_hit(building, building.cells[pick->cell_index], pick->hit_world, unit_size).slot};
                    if (ui.drag_paint_face != face) {
                        paint_building_face(*project, ui, *pick, unit_size, status);
                        ui.drag_paint_face = face;
                    }
                }
            }
        }

        BeginDrawing();
        ClearBackground(DARKGRAY);

        const auto imgui_begin = std::chrono::steady_clock::now();
        rlImGuiBegin();

        static bool show_csx_editor = false;
        bool open_folder_requested = open_folder_hotkey_requested;
        bool export_mesh_requested = false;
        bool run_export_after_popup = false;



        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                // Built from the shortcut formatter so macOS shows Cmd.
                if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN " Open map folder...", shortcut_label(Shortcut{KEY_O, true, false, false}).c_str())) open_folder_requested = true;
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK " Save map...", action_shortcut_label(ui, EditorAction::Save).c_str(), false, project.has_value())) save_active_tab(*project, ui, status);
                ImGui::MenuItem(ICON_FA_BOX_ARCHIVE " CSX archive...", nullptr, &show_csx_editor);
#ifndef __EMSCRIPTEN__
                // Mesh export and quitting are desktop-only; the browser tab closes itself.
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
                // City selection actions, mirroring their keys so they are discoverable.
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
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_CROSSHAIRS " Frame selection", action_shortcut_label(ui, EditorAction::FocusSelection).c_str())) focus_current_selection(*project, ui, camera);
                if (ImGui::MenuItem(ICON_FA_EXPAND " Whole city", action_shortcut_label(ui, EditorAction::FrameCity).c_str(), false, project->city.has_value())) focus_camera_on_city(camera, *project);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Windows")) {
                ImGui::MenuItem(ICON_FA_LIST " Outliner", action_shortcut_label(ui, EditorAction::WindowLibrary).c_str(), &ui.show_library_window);
                ImGui::MenuItem(ICON_FA_SLIDERS " Properties", action_shortcut_label(ui, EditorAction::WindowProperties).c_str(), &ui.show_properties_window);
                ImGui::MenuItem(ICON_FA_IMAGES " Asset drawer", action_shortcut_label(ui, EditorAction::WindowTools).c_str(), &ui.show_asset_drawer);
                ImGui::Separator();
                ImGui::MenuItem(ICON_FA_BUILDING " Building browser", action_shortcut_label(ui, EditorAction::WindowBuildingBrowser).c_str(), &ui.show_building_browser, project.has_value());
                ImGui::MenuItem(ICON_FA_CUBES " Block browser", action_shortcut_label(ui, EditorAction::WindowBlockBrowser).c_str(), &ui.show_block_browser, project.has_value());
                ImGui::MenuItem(ICON_FA_TABLE_CELLS " Buildblock", action_shortcut_label(ui, EditorAction::WindowBuildblock).c_str(), &ui.show_buildblock_window, project.has_value());
                ImGui::MenuItem(ICON_FA_CLOCK_ROTATE_LEFT " Changes since save", action_shortcut_label(ui, EditorAction::WindowChanges).c_str(), &ui.show_changes_window, project.has_value());
                ImGui::Separator();
                // Preferences lives under Edit; the graphics debug overlay is a
                // developer tool, still on its key (listed under Shortcuts).
                ImGui::MenuItem(ICON_FA_KEYBOARD " Keyboard shortcuts", action_shortcut_label(ui, EditorAction::WindowShortcuts).c_str(), &ui.show_shortcuts_window);
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_TABLE_COLUMNS " Reset window layout")) {
                    ui.show_asset_drawer = ui.show_library_window = ui.show_properties_window = true;
                    ui.reset_layout = true;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }


        // The scene is the workspace itself. Do not put it in a dock node because
        // a docked ImGui window always exposes a tab, even when it is the only one.
        // Warm the thumbnail caches a few per frame after load, and show a loading
        // overlay with progress until render buffers + thumbnails are all ready.
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

        is_viewport_hovered = false;
        // Dockspace over the whole work area. The scene canvas lives in the
        // central node (no tab bar), so tool windows dock around it and the 3D
        // view always fills whatever space is left. The first run (or Windows >
        // Reset layout) builds Tools + Library on the left and Properties on
        // the right; everything can be dragged out, re-docked, or closed.
        // Status bar along the bottom: the latest message stays readable after
        // its 2.5 s pop-up fades; hovering lists the recent ones. Drawn before
        // the dockspace so the panels sit above it.
        if (ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, ImGui::GetFrameHeight(),
                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar)) {
            if (ImGui::BeginMenuBar()) {
                ImGui::TextDisabled("%s", status_history.empty() ? "Ready" : status_history.front().c_str());
                if (!status_history.empty() && ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    for (const auto& message : status_history) ImGui::TextUnformatted(message.c_str());
                    ImGui::EndTooltip();
                }
                ImGui::EndMenuBar();
            }
        }
        ImGui::End();
        {
            const auto* dock_viewport = ImGui::GetMainViewport();
            // A new id (v2) so layouts saved with the old left-hand Tools/Library
            // column are not restored; the viewport now takes everything but a
            // right-hand column.
            const ImGuiID dock_id = ImGui::DockSpaceOverViewport(ImGui::GetID("MainDockV2"), dock_viewport, ImGuiDockNodeFlags_None);
            const ImGuiDockNode* dock_node = ImGui::DockBuilderGetNode(dock_id);
            if (ui.reset_layout || dock_node == nullptr || !dock_node->IsSplitNode()) {
                ImGui::DockBuilderRemoveNode(dock_id);
                ImGui::DockBuilderAddNode(dock_id, ImGuiDockNodeFlags_DockSpace);
                ImGui::DockBuilderSetNodeSize(dock_id, dock_viewport->WorkSize);
                ImGuiID center = dock_id;
                ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24F, nullptr, &center);
                const ImGuiID right_top = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.38F, nullptr, &right);
                if (auto* center_node = ImGui::DockBuilderGetNode(center)) {
                    center_node->LocalFlags |= ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDockingOverMe;
                }
                ImGui::DockBuilderDockWindow("##SceneCanvas", center);
                ImGui::DockBuilderDockWindow("###Outliner", right_top);
                ImGui::DockBuilderDockWindow("###Properties", right);
                // Small reference windows open as tabs beside Properties.
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
                if ((static_cast<int>(size.x) != static_cast<int>(last_viewport_size.x) ||
                     static_cast<int>(size.y) != static_cast<int>(last_viewport_size.y)) &&
                    size.x > 0 && size.y > 0 && GetTime() - last_viewport_resize >= 0.12) {
                    unload_game_render_texture(viewport_texture);
                    UnloadRenderTexture(building_outline_mask_texture);
                    viewport_texture = load_game_render_texture(static_cast<int>(size.x), static_cast<int>(size.y));
                    building_outline_mask_texture = LoadRenderTexture(static_cast<int>(size.x), static_cast<int>(size.y));
                    last_viewport_size = {static_cast<float>(static_cast<int>(size.x)), static_cast<float>(static_cast<int>(size.y))};
                    last_viewport_resize = GetTime();
                }

                resolve_game_render_texture(viewport_texture);
                const auto* display_texture = game_render_display_texture(viewport_texture);
                // Handles belong above the finished scene, including shadows,
                // atmosphere and outlines. They must never be tone-mapped away.
                if (project) {
                    const auto target = display_texture ? *display_texture : viewport_texture;
                    begin_game_texture_mode(target);
                    BeginMode3D(camera);
                    if (ui.tab == EditorTab::City && project->city && ui.city_selection == CitySelectionKind::Building &&
                        ui.selected_city_building < project->city->data.buildings.size()) {
                        const auto& placement = project->city->data.buildings[ui.selected_city_building];
                        // At the footprint centre, which rotation keeps fixed.
                        const auto offset = city_building_gizmo_offset(*project, placement);
                        const auto hovered = ui.gizmo_active != GizmoAxis::None ? ui.gizmo_active :
                            (is_viewport_hovered ? pick_building_gizmo(camera, viewport_mouse_pos, last_viewport_size, placement, true, offset) : GizmoAxis::None);
                        draw_building_gizmo(placement, hovered, camera, last_viewport_size, true, offset);
                    } else if (ui.tab == EditorTab::Buildings && !project->buildings.empty() &&
                               ui.building_tool == BuildingTool::Anchor) {
                        // Only the Anchor tool can drag it; elsewhere it read as draggable.
                        draw_building_gizmo(SbcBuilding{}, ui.anchor_axis, camera, last_viewport_size, false);
                    }
                    EndMode3D();
                    end_game_texture_mode();
                }
                rlImGuiImageRenderTexture(display_texture == nullptr ? &viewport_texture : display_texture);
                is_viewport_hovered = ImGui::IsItemHovered();
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

                viewport_mouse_pos = get_viewport_mouse_position(Vector2{min_pos.x, min_pos.y}, Vector2{size.x, size.y});

                // Rubber-band selection rectangle (drawn in screen space over the
                // image while a box drag is in progress).
                if (ui.box_select_active && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    const ImVec2 p0{min_pos.x + ui.box_select_start.x, min_pos.y + ui.box_select_start.y};
                    const ImVec2 p1{min_pos.x + viewport_mouse_pos.x, min_pos.y + viewport_mouse_pos.y};
                    auto* dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(p0, p1, IM_COL32(120, 180, 255, 40));
                    dl->AddRect(p0, p1, IM_COL32(150, 200, 255, 220));
                }
                draw_viewport_overlays(project ? &*project : nullptr, ui, camera, status, open_folder_requested);
            }
            ImGui::End();

        }
        draw_outliner_window(project ? &*project : nullptr, ui);
        draw_properties_window(project ? &*project : nullptr, ui, status);

        if (project) {
            draw_building_browser_window(*project, ui);
            draw_block_browser_window(*project, ui);
            draw_buildblock_window(*project, ui, status);
            draw_changes_window(*project, ui, status);
        }
        draw_shortcuts_window(ui);
        draw_preferences_window(ui);
        // Cleared after every window drew, so the floating ones also reset.
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

        if (project && project->city && ui.city_dirty && project->city->data.version < 12 && !project->city_promotion_failed) {
            // An upgrade right after undo (map equals the committed state) is not
            // a new edit; otherwise it folds into the pending edit's undo step.
            const bool at_baseline = project->city_baseline == project->city->data;
            try {
                promote_city(*project);
                if (at_baseline) project->city_baseline = project->city->data;
            }
            catch (const std::exception& error) {
                project->city_promotion_failed = true;
                report_failure(status, std::string{"City upgrade failed; edits retained, saving blocked: "} + error.what());
            }
        }
        draw_csx_editor(show_csx_editor, project ? &*project : nullptr);
        static std::string shown_error;
        if (!g_failure_message.empty() && !ImGui::IsPopupOpen("Operation failed")) {
            shown_error = std::move(g_failure_message);
            g_failure_message.clear();
            ImGui::OpenPopup("Operation failed");
        }
        // Auto-resize with wrapped text collapses to a sliver; give it a width.
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
                if (typeof window.mapEditorBrowserOpenPicker === 'function') {
                    window.mapEditorBrowserOpenPicker();
                }
            });
            status = "Choose a Sub Rosa map folder in the browser picker.";
#else
            auto result = pfd::select_folder("Open map folder").result();
            if (!result.empty()) {
                pending_load_folder = std::filesystem::path{result};
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
        if (open_export_popup) {
            ImGui::OpenPopup("Export Mesh");
            open_export_popup = false;
        }
        if (project && ImGui::BeginPopupModal("Export Mesh", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            // Context-aware title + options for the active view.
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
            ImGui::Separator();
            // Defer the native save dialog until after the popup finishes rendering;
            // NFD blocks the main thread and must not run mid-ImGui-frame.
            if (ImGui::Button("Export", ImVec2(120, 0))) {
                run_export_after_popup = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (run_export_after_popup && project) {
#ifndef __EMSCRIPTEN__
            MAPED_PROFILE_BLOCK("export mesh dialog result");
            std::string reason;
            const auto default_path = default_export_path(*project, ui, reason);
            // Native save dialog (NFD-e: Cocoa NSSavePanel on macOS, not osascript).
            NFD::Guard nfd_guard;
            NFD::UniquePath out_path;
            const nfdnfilteritem_t filters[1] = {{"glTF", "gltf"}};
            const auto default_dir = default_path.parent_path().string();
            const auto default_name = default_path.filename().string();
            const auto result = NFD::SaveDialog(
                out_path, filters, 1, default_dir.c_str(), default_name.c_str());
            if (result == NFD_OKAY) {
                run_mesh_export(*project, ui, std::filesystem::path{out_path.get()}, export_options, status);
            } else if (result == NFD_ERROR) {
                report_failure(status, std::string("Export: dialog error: ") + NFD::GetError());
            }
#else
            status = "Export is available in the desktop editor.";
#endif
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
                if (!has_unsaved_changes(ui)) {
                    running = false;
                    ImGui::CloseCurrentPopup();
                }
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

        if (screenshot_path && project && ++screenshot_ready_frames >= screenshot_frame_target) {
            auto screenshot = LoadImageFromScreen();
            ExportImage(screenshot, screenshot_path->string().c_str());
            UnloadImage(screenshot);
            running = false;
        }

        // Run a deferred load now that the normal frame has been presented. Paint
        // a dedicated "Parsing..." frame first so the screen shows progress, then
        // run the blocking synchronous parse and apply the result.
        if (pending_load_folder
#ifndef __EMSCRIPTEN__
            && !load_future
#endif
        ) {
            MAPED_PROFILE_BLOCK("load pending project");
            const auto folder = *pending_load_folder;
            pending_load_folder.reset();
#ifndef __EMSCRIPTEN__
            load_stage->store(0);
            load_future = std::async(std::launch::async, [folder, load_stage] {
                std::string error;
                auto loaded = load_project_folder(folder, error, false, load_stage.get());
                return std::make_pair(std::move(loaded), std::move(error));
            });
        }
        if (load_future && load_future->wait_for(std::chrono::milliseconds{0}) == std::future_status::ready) {
            auto [loaded, error] = load_future->get();
            load_future.reset();
            if (!loaded && !error.empty()) report_failure(status, std::move(error));
            else status = std::move(error);
            if (loaded) load_project_textures(*loaded);
#else
            present_parsing_frame(folder);
            // Let the browser present the parsing frame before the synchronous
            // virtual-filesystem scan begins.
            emscripten_sleep(0);
            auto loaded = load_project_folder(folder, status, true);
            if (!loaded && !status.empty()) report_failure(status, std::string{status});
#endif
            if (project) {
                free_project_thumbnails(*project);
            }
            project = std::move(loaded);
            if (project) {
#ifdef __EMSCRIPTEN__
                EM_ASM({
                    const node = document.getElementById('status');
                    if (node) node.textContent = "";
                });
#endif
                set_status(*project);
                status = "loaded " + project->root.filename().string();
#ifdef MAP_EDITOR_CPP_WEB_QA_START_BUILDINGS
                ui.tab = EditorTab::Buildings;
                const auto qa_building = std::ranges::find_if(project->buildings, [](const auto& asset) {
                    return lower_name(asset.name) == "burger";
                });
                if (qa_building != project->buildings.end()) {
                    const auto index = static_cast<std::size_t>(std::distance(project->buildings.begin(), qa_building));
                    std::swap(project->buildings[0], project->buildings[index]);
                    std::swap(project->building_bytes[0], project->building_bytes[index]);
                }
#endif
                if (screenshot_tab == "buildings") ui.tab = EditorTab::Buildings;
                if (screenshot_tab == "pieces") { ui.tab = EditorTab::City; ui.city_tool = CityTool::Piece; }
                if (screenshot_tab == "buildblocks") ui.tab = EditorTab::Buildblocks;
                if (screenshot_tab == "blocks") ui.tab = EditorTab::Blocks;
                if (screenshot_tab == "win_buildings") ui.show_building_browser = true;
                if (screenshot_tab == "win_blocks") ui.show_block_browser = true;
                if (screenshot_tab == "win_buildblock") ui.show_buildblock_window = true;
                if (screenshot_tab == "win_changes") ui.show_changes_window = true;
                if (screenshot_tab == "win_shortcuts") ui.show_shortcuts_window = true;
                if (screenshot_path && project->city && !project->city->data.buildings.empty()) {
                    ui.city_tool = CityTool::Select;
                    if (ui.tab == EditorTab::City) {
                        ui.city_selection = CitySelectionKind::Building;
                        ui.selected_city_building = 0U;
                        ui.selected_city_buildings = {0U};
                    }
                }
                if (ui.tab == EditorTab::City) {
                    focus_camera_on_city(camera, *project);
                } else {
                    camera = default_camera();
                    if (ui.tab == EditorTab::Buildings) {
                        focus_current_selection(*project, ui, camera);
                    }
                }
            }
        }
#ifdef __EMSCRIPTEN__
        // Keep the single desktop-style loop usable in a browser. Asyncify
        // yields to the browser event loop without duplicating the application
        // state into a second web-only frame implementation.
        emscripten_sleep(0);
#endif
    }

    if (project) {
        free_project_thumbnails(*project);
    }
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

} // namespace map_editor
