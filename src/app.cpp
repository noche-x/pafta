#include "app.hpp"
#include "app_internal.hpp"
#include "mesh_export.hpp"
#include "profiler.hpp"

#include <raylib.h>
#include <rlgl.h>
#include <raymath.h>
#include <imgui.h>
#include <rlImGui.h>
#include <extras/FA6FreeSolidFontData.h>
#include <extras/IconsFontAwesome6.h>

#include <portable-file-dialogs.h>
#include <nfd.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
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
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
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
}

// Merges the bundled Font Awesome 6 Solid glyphs into the current atlas font so
// ICON_FA_* codepoints render. Mirrors rlImGui's own SetupFontAwesome (merge mode,
// atlas does not own the static data) so it composes with whatever base font is
// active.
void merge_font_awesome(ImGuiIO& io, float size) {
    static const ImWchar icon_ranges[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
    ImFontConfig config;
    config.MergeMode = true;
    config.PixelSnapH = true;
    config.FontDataOwnedByAtlas = false;
    config.OversampleH = 2;
    config.OversampleV = 1;
    io.Fonts->AddFontFromMemoryCompressedTTF(
        const_cast<std::uint8_t*>(fa_solid_900_compressed_data),
        static_cast<int>(fa_solid_900_compressed_size),
        size,
        &config,
        icon_ranges);
}

// Rebuilds the ImGui font atlas so it covers Latin-1 + Latin Extended (for
// non-ASCII layout key names) AND the Font Awesome icon glyphs used by the
// toolbar. A system TTF is used for the base font when available; otherwise the
// built-in ASCII default stays (key_label sanitizes to ASCII as a backstop). FA
// is merged on top in either case, since rlImGuiSetup's atlas is cleared here.
void load_editor_fonts(ImGuiIO& io) {
    static const ImWchar latin_ranges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin-1 Supplement
        0x0100, 0x017F, // Latin Extended-A
        0x0180, 0x024F, // Latin Extended-B
        0,
    };
    constexpr float base_size = 15.0F;
    const char* candidates[] = {
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
    };

    io.Fonts->Clear();
    ImFont* base = nullptr;
    for (const char* path : candidates) {
        if (std::filesystem::exists(path)) {
            base = io.Fonts->AddFontFromFileTTF(path, base_size, nullptr, latin_ranges);
            if (base != nullptr) {
                break;
            }
        }
    }
    if (base == nullptr) {
        io.Fonts->AddFontDefault();
    }
    merge_font_awesome(io, base_size);
    io.Fonts->Build();
    // No explicit texture reload: this rlImGui builds/updates the font atlas
    // texture lazily each frame, so the rebuilt io.Fonts is picked up on the next
    // rlImGuiBegin before anything draws.
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

void upsert_texture_if_valid(EditorProject& project, std::string name, Texture2D texture, std::unordered_map<std::string, std::size_t>& loaded_names) {
    if (texture.id == 0U) {
        return;
    }

    const auto key = lower_name(name);
    configure_texture(texture);
    if (const auto existing = loaded_names.find(key); existing != loaded_names.end()) {
        project.textures[existing->second] = LoadedTexture{std::move(name), texture};
        return;
    }

    loaded_names.emplace(key, project.textures.size());
    project.textures.emplace_back(std::move(name), texture);
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

Image decode_flipped_embedded_png(const std::vector<std::uint8_t>& payload) {
    if (payload.size() < 0x4cU || read_le_u32(payload, 0U) != 1U) {
        return Image{};
    }
    const auto image_size = static_cast<std::size_t>(read_le_u32(payload, 0x44U));
    if (image_size == 0U || image_size > payload.size() - 0x4cU) {
        return Image{};
    }
    const auto* image_bytes = payload.data() + 0x4cU;
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
// Texture2D (id==0 on failure, which add_texture_if_valid skips). Frees the image.
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
    std::unordered_map<std::string, std::size_t> loaded_names;

    // One decode task: a name, whether it overrides (CSX-wins upsert) or only adds,
    // and a closure that produces the flipped Image off the main thread. The decode
    // (PNG/stbi) is CPU-bound and dominated load time; the GL upload below stays
    // serial on the main thread. Order is preserved exactly: texture/*.png, then
    // root *.png, then CSX-embedded textures in archive order — texture indices and
    // the CSX-wins merge depend on this sequence.
    struct DecodeTask {
        std::string name;
        bool upsert;
        std::function<Image()> decode;
    };
    std::vector<DecodeTask> tasks;

    for (const auto& path : files_with_extension(project.root / "texture", ".png")) {
        tasks.push_back({stem_name(path), false, [path] { return decode_flipped_png_from_file(path); }});
    }
    for (const auto& path : files_with_extension(project.root, ".png")) {
        tasks.push_back({stem_name(path), false, [path] { return decode_flipped_png_from_file(path); }});
    }
    // CSX archives are parsed in parallel; the texture entries they expose are then
    // appended as decode tasks in archive order (matching the previous serial walk).
    {
        MAPED_PROFILE_BLOCK("parse csx archives (textures)");
        const auto csx_paths = files_with_extension(project.root, ".csx");
        std::vector<CsxArchive> archives(csx_paths.size());
        parallel_for(csx_paths.size(), [&](std::size_t i) { archives[i] = parse_csx_file(csx_paths[i]); });
        for (auto& archive : archives) {
            for (const auto& entry : archive.entries) {
                if (entry.kind != CsxEntryKind::Texture) {
                    continue;
                }
                tasks.push_back({entry.name, true, [payload = entry.payload] {
                                     return decode_flipped_embedded_png(payload);
                                 }});
            }
        }
    }

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
        if (tasks[i].upsert) {
            if (texture.id != 0U) {
                upsert_texture_if_valid(project, std::move(tasks[i].name), texture, loaded_names);
            }
        } else {
            add_texture_if_valid(project, std::move(tasks[i].name), texture, loaded_names);
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
    const auto csx_paths = files_with_extension(project.root, ".csx");
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

std::optional<EditorProject> load_project_folder(const std::filesystem::path& folder, std::string& error) {
    MAPED_PROFILE_FUNCTION();
    const auto root = std::filesystem::weakly_canonical(folder);
    if (!std::filesystem::is_directory(root)) {
        error = "not a folder: " + root.string();
        return std::nullopt;
    }

    EditorProject project;
    project.root = root;

    const auto city_path = root / "city2.sbc";
    if (std::filesystem::exists(city_path)) {
        MAPED_PROFILE_BLOCK("load city2.sbc");
        try {
            project.city_bytes = read_file_bytes(city_path);
            project.city = LoadedAsset<SbcMap>{city_path, "city2", parse_sbc(*project.city_bytes)};
        } catch (const std::exception& error) {
            project.city_bytes.reset();
            project.city.reset();
            project.load_warnings.push_back(city_path.filename().string() + ": " + error.what());
        }
        MAPED_PROFILE_END_BLOCK;
    }

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

    MAPED_PROFILE_BLOCK("load project textures");
    load_project_textures(project);
    MAPED_PROFILE_END_BLOCK;
    MAPED_PROFILE_BLOCK("load project csx geometry");
    load_project_csx_geometry(project);
    MAPED_PROFILE_END_BLOCK;

    if (!project.city && project.buildings.empty() && project.blocks.empty() && project.buildblocks.empty()) {
        error = "folder does not look like a map folder: " + root.string();
        return std::nullopt;
    }

    project.render_buffers_dirty = true;
    project.status = "loaded " + root.filename().string()
        + " (" + std::to_string(project.buildings.size()) + " buildings, "
        + std::to_string(project.blocks.size()) + " blocks, "
        + std::to_string(project.textures.size()) + " textures)";
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
    for (const auto& path : files_with_extension(project.root, ".csx")) {
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
    MAPED_PROFILE_FUNCTION();
    if (path.empty()) {
        status = "Export: empty path.";
        return;
    }

    if (ui.tab == EditorTab::Buildings) {
        if (project.buildings.empty()) {
            status = "Export: no building loaded.";
            return;
        }
        const auto index = std::min(ui.selected_building, project.buildings.size() - 1U);
        const auto& asset = project.buildings[index];
        const auto blocks = block_meshes(project);
        const auto templates = buildblock_templates(project);
        const auto buffer = build_native_building_render_buffer(asset.data, blocks, templates);
        status = export_building_to_gltf(path, buffer, asset.name, project, &asset.data.materials, options).message;
        return;
    }

    if (ui.tab == EditorTab::Blocks) {
        if (project.blocks.empty()) {
            status = "Export: no block loaded.";
            return;
        }
        const auto index = std::min(ui.selected_block, project.blocks.size() - 1U);
        const auto& asset = project.blocks[index];
        status = export_block_to_gltf(path, asset.data, asset.name, project).message;
        return;
    }

    if (ui.tab == EditorTab::City) {
        if (!project.city) {
            status = "Export: no city loaded.";
            return;
        }
        const auto placement_buildings = resolve_placement_buildings(project);
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
        status = export_city_to_gltf(path, scene, placement_buffers, placement_buildings,
                                     piece_buffer, project, options).message;
        return;
    }

    status = "Export: this view has no mesh to export.";
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
            MAPED_PROFILE_THREAD("City scene rebuild");
            MAPED_PROFILE_BLOCK("build async map scene");
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
    const Camera3D& camera,
    Vector2 viewport_size,
    const Texture2D* city_window_texture,
    const SbcBuilding* hovered_building = nullptr) {
    if (!project.city) {
        return;
    }

    struct DrawEntry {
        std::size_t placement_index{};
        std::size_t asset_index{};
    };

    auto dot = [](Vector3 a, Vector3 b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    };
    auto cross = [](Vector3 a, Vector3 b) {
        return Vector3{
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x,
        };
    };
    auto normalize = [](Vector3 value, Vector3 fallback) {
        const auto len = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
        return len <= std::numeric_limits<float>::epsilon()
            ? fallback
            : Vector3{value.x / len, value.y / len, value.z / len};
    };
    auto camera_forward = [&]() {
        const Vector3 value{
            camera.target.x - camera.position.x,
            camera.target.y - camera.position.y,
            camera.target.z - camera.position.z,
        };
        return normalize(value, Vector3{0.0F, 0.0F, -1.0F});
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
        const float max_y = (static_cast<float>(std::max(1U, building.size_y)) - static_cast<float>(building.anchor_y)) * unit_size;
        const float min_z = -static_cast<float>(building.anchor_z) * unit_size;
        const float max_z = (static_cast<float>(std::max(1U, building.size_z)) - static_cast<float>(building.anchor_z)) * unit_size;
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

    std::vector<DrawEntry> entries;
    entries.reserve(project.city->data.buildings.size());
    for (std::size_t placement_index = 0; placement_index < project.city->data.buildings.size(); ++placement_index) {
        const auto& placement = project.city->data.buildings[placement_index];
        const auto asset = std::ranges::find_if(project.buildings, [&placement](const LoadedAsset<SbbBuilding>& candidate) {
            return candidate.name == placement.name || candidate.data.name == placement.name;
        });
        if (asset == project.buildings.end()) {
            continue;
        }

        const auto asset_index = static_cast<std::size_t>(std::distance(project.buildings.begin(), asset));
        if (asset_index >= project.building_gpu_models.size() || !visible_in_viewport(asset->data, placement)) {
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
        draw_city_building_model(
            asset.data,
            project.building_gpu_models[entry.asset_index],
            placement,
            selected_building == &placement,
            false,
            city_window_texture,
            hovered_building == &placement);
    }
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
    const auto asset = std::ranges::find_if(project.buildings, [&placement](const LoadedAsset<SbbBuilding>& candidate) {
        return candidate.name == placement.name || candidate.data.name == placement.name;
    });
    if (asset == project.buildings.end()) {
        return;
    }

    const auto asset_index = static_cast<std::size_t>(std::distance(project.buildings.begin(), asset));
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

// Snapshots the current city map before an edit so it can be undone, and records
// the action in the backlog. Call this immediately BEFORE a user-driven map
// mutation. Clears the redo stack (a new action invalidates redo). Capped so the
// history does not grow without bound.
constexpr std::size_t kMaxUndo = 100;
void begin_city_action(EditorProject& project, EditorUi& ui, std::string label) {
    if (!project.city) {
        return;
    }
    project.undo_stack.emplace_back(label, project.city->data);
    if (project.undo_stack.size() > kMaxUndo) {
        project.undo_stack.erase(project.undo_stack.begin());
    }
    project.redo_stack.clear();
    project.action_log.push_back(std::move(label));
    if (project.action_log.size() > 500U) {
        project.action_log.erase(project.action_log.begin());
    }
    (void)ui;
}

// Reverts the last action: the pre-edit snapshot becomes current, and the current
// state is pushed to the redo stack so it can be reapplied.
void undo_city(EditorProject& project, EditorUi& ui, std::string& status) {
    if (!project.city || project.undo_stack.empty()) {
        status = "nothing to undo";
        return;
    }
    auto [label, snapshot] = std::move(project.undo_stack.back());
    project.undo_stack.pop_back();
    project.redo_stack.emplace_back(label, project.city->data);
    project.city->data = std::move(snapshot);
    project.action_log.push_back("undo: " + label);
    invalidate_portal_render_models(project);
    ui.city_selection = CitySelectionKind::None;
    ui.selected_city_buildings.clear();
    ui.selected_portals.clear();
    ui.city_dirty = true;
    status = "undo " + label;
}

void redo_city(EditorProject& project, EditorUi& ui, std::string& status) {
    if (!project.city || project.redo_stack.empty()) {
        status = "nothing to redo";
        return;
    }
    auto [label, snapshot] = std::move(project.redo_stack.back());
    project.redo_stack.pop_back();
    project.undo_stack.emplace_back(label, project.city->data);
    project.city->data = std::move(snapshot);
    project.action_log.push_back("redo: " + label);
    invalidate_portal_render_models(project);
    ui.city_selection = CitySelectionKind::None;
    ui.selected_city_buildings.clear();
    ui.selected_portals.clear();
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
void place_building_block(EditorProject& project, EditorUi& ui, const CellPick& pick, float unit_size, std::string& status) {
    if (project.buildings.empty()) {
        return;
    }
    if (!ui.block_config.exterior && !ui.block_config.interior) {
        status = "enable Exterior or Interior in the block panel first";
        return;
    }
    ui.selected_building = std::min(ui.selected_building, project.buildings.size() - 1U);
    auto& building = project.buildings[ui.selected_building].data;
    if (pick.cell_index >= building.cells.size()) {
        return;
    }
    const auto source = building.cells[pick.cell_index];
    const auto face = building_face_from_hit(building, source, pick.hit_world, unit_size);
    const std::int64_t nx = static_cast<std::int64_t>(source.x) + face.dx;
    const std::int64_t ny = static_cast<std::int64_t>(source.y) + face.dy;
    const std::int64_t nz = static_cast<std::int64_t>(source.z) + face.dz;
    if (nx < 0 || ny < 0 || nz < 0) {
        status = "can't place outside the building grid";
        return;
    }
    auto* target = building_cell_at(building, static_cast<std::uint32_t>(nx), static_cast<std::uint32_t>(ny), static_cast<std::uint32_t>(nz));
    if (target == nullptr) {
        status = "no cell slot on that side (building grid edge)";
        return;
    }
    if (target->occupied) {
        status = "neighbour cell already filled";
        return;
    }
    apply_block_config(*target, ui.block_config, building);
    mark_building_changed(project, ui);
    status = "placed block at [" + std::to_string(target->x) + "," + std::to_string(target->y) + "," + std::to_string(target->z) + "]";
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

// Drops a NEW placement of the chosen building template (ui.place_building_template,
// an index into the loaded building assets) at the cursor cell.
void place_city_building_from_template(EditorProject& project, EditorUi& ui, CityGridPoint point) {
    if (!project.city || project.buildings.empty()) {
        return;
    }
    const auto idx = std::min(ui.place_building_template, project.buildings.size() - 1U);
    const auto& tmpl = project.buildings[idx];
    auto& buildings = project.city->data.buildings;
    SbcBuilding placement{};
    placement.index = buildings.size();
    placement.name = tmpl.data.name.empty() ? tmpl.name : tmpl.data.name;
    placement.x = point.x;
    placement.y = point.y;
    placement.z = point.z;
    placement.rotation = 0;
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
        // Snap the split node onto the exact grid cell nearest the projection so
        // the two halves stay axis-aligned with the original road.
        best = StreetSplitHit{
            i,
            CityGridPoint{
                static_cast<std::int32_t>(std::lround(cx)),
                point.y,
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
    CityGridPoint placed = split_point;
    if (project.city_scene_cache_valid && project.city_scene_cache.terrain.has_value()) {
        const auto unit = project.city_scene_cache.unit_size;
        const auto h = terrain_height_at(*project.city_scene_cache.terrain,
            static_cast<float>(placed.x) * unit, static_cast<float>(placed.z) * unit);
        placed.y = static_cast<std::int32_t>(std::lround(h / unit));
    }
    const auto node_index = add_city_intersection(map, placed);

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
// creates a new node. New nodes pick up terrain height so the road can run up or
// down with the land.
std::size_t resolve_or_create_intersection(EditorProject& project, CityGridPoint point) {
    auto& map = project.city->data;
    if (const auto existing = nearest_city_intersection(map, point, 1)) {
        return *existing;
    }
    if (const auto split = nearest_street_split(map, point)) {
        return split_street_at(project, split->street_index, split->point);
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
    street = canonical_city_street_endpoint_order(map, std::move(street));
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
    // Only the edited building's mesh + thumbnail need rebuilding, not every city
    // building. The Buildings-tab floor preview is its own cached model.
    project.building_floor_preview_dirty = true;
    rebuild_single_building_render(project, ui.selected_building);
    invalidate_building_thumbnail(project, ui.selected_building);
}

void mark_buildblock_changed(EditorProject& project, EditorUi& ui) {
    ui.buildblock_dirty = true;
    project.render_buffers_dirty = true;
    project.building_floor_preview_dirty = true;
    // Editing buildblock data invalidates the cached buildblock-template catalog.
    invalidate_render_input_cache(project);
    invalidate_portal_render_models(project);
}

void mark_block_changed(EditorProject& project, EditorUi& ui, SblBlockMesh& block) {
    ui.block_dirty = true;
    refresh_sbl_block_faces(block);
    project.render_buffers_dirty = true;
    // Editing block data invalidates the cached block-mesh catalog.
    invalidate_render_input_cache(project);
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
    const auto arm = unit_size * 4.5F;
    const Vector3 origin{o.x, o.y + unit_size * 0.5F, o.z};
    return GizmoLayout{
        origin,
        {origin.x + arm, origin.y, origin.z},
        {origin.x, origin.y + arm, origin.z},
        {origin.x, origin.y, origin.z + arm},
        {origin.x + arm * 0.7F, origin.y, origin.z + arm * 0.7F},
        unit_size * 0.35F,
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

CityAnyPick pick_city_any(const EditorProject& project, const MapScene& scene, const Vec3& ro, const Vec3& rd) {
    if (!project.city) {
        return {};
    }
    const auto& map = project.city->data;
    // Building volume (nearest building footprint the ray hits).
    const auto placements = resolve_placement_buildings(project);
    const auto building = pick_city_building(map, placements, ro, rd, scene.unit_size);
    if (building.has_value()) {
        return {CitySelectionKind::Building, *building};
    }
    // Piece voxel volume.
    const auto piece = pick_city_piece(map, ro, rd, scene.unit_size);
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
            ui.selected_city_street = picked.index;
            ui.selected_city_buildings.clear();
            ui.selected_portals.clear();
            break;
        case CitySelectionKind::Node:
            ui.city_selection = CitySelectionKind::Node;
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
void erase_city_thing_under_cursor(
    EditorProject& project,
    EditorUi& ui,
    const MapScene& scene,
    const Vec3& ro,
    const Vec3& rd,
    std::optional<CityGridPoint> cursor,
    std::string& status) {
    if (!project.city) {
        return;
    }
    if (cursor.has_value()) {
        clear_city_piece_voxel(project, *cursor, ui);
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
    }
}

GizmoAxis pick_building_gizmo(
    const Camera3D& camera,
    Vector2 viewport_mouse_pos,
    Vector2 viewport_size,
    const SbcBuilding& placement) {
    const auto layout = building_gizmo_layout(placement);
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
        constexpr auto shaft_threshold = 18.0F;
        constexpr auto head_threshold = 26.0F;
        const auto origin = project(layout.origin);
        const auto end = project(tip);
        if (!finite(origin) || !finite(end)) {
            return;
        }
        consider(axis, segment_distance(viewport_mouse_pos, origin, end), shaft_threshold);
        consider(axis, distance(viewport_mouse_pos, end), head_threshold);
    };

    consider_arrow(GizmoAxis::MoveX, layout.tip_x);
    consider_arrow(GizmoAxis::MoveY, layout.tip_y);
    consider_arrow(GizmoAxis::MoveZ, layout.tip_z);

    constexpr auto rotate_threshold = 16.0F;
    const auto rot_tip = project(layout.tip_rot);
    if (finite(rot_tip)) {
        consider(GizmoAxis::Rotate, distance(viewport_mouse_pos, rot_tip), 22.0F);
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

void draw_building_gizmo(const SbcBuilding& placement, GizmoAxis active) {
    const auto layout = building_gizmo_layout(placement);
    const auto axis_color = [active](GizmoAxis axis, Color base) {
        return active == axis ? Color{255, 255, 255, 255} : base;
    };
    const Color red{235, 92, 88, 255};
    const Color green{120, 220, 110, 255};
    const Color blue{96, 156, 235, 255};
    const Color yellow{245, 210, 90, 255};

    auto draw_arrow = [](Vector3 from, Vector3 to, float radius, Color color) {
        const auto head_len = radius * 2.6F;
        const Vector3 dir{to.x - from.x, to.y - from.y, to.z - from.z};
        const auto len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (len <= std::numeric_limits<float>::epsilon()) {
            return;
        }
        const Vector3 head_base{
            to.x - dir.x * (head_len / len),
            to.y - dir.y * (head_len / len),
            to.z - dir.z * (head_len / len),
        };
        DrawCylinderEx(from, head_base, radius * 0.28F, radius * 0.28F, 12, color);
        DrawCylinderEx(head_base, to, radius * 0.8F, 0.0F, 16, color);
    };

    draw_arrow(layout.origin, layout.tip_x, layout.handle_radius, axis_color(GizmoAxis::MoveX, red));
    draw_arrow(layout.origin, layout.tip_y, layout.handle_radius, axis_color(GizmoAxis::MoveY, green));
    draw_arrow(layout.origin, layout.tip_z, layout.handle_radius, axis_color(GizmoAxis::MoveZ, blue));
    // Rotate handle: a torus-like marker (sphere stand-in for portability).
    DrawSphere(layout.tip_rot, layout.handle_radius * 0.9F, axis_color(GizmoAxis::Rotate, yellow));
    DrawCircle3D(layout.origin, layout.arm_length * 0.7F, {1.0F, 0.0F, 0.0F}, 90.0F,
        axis_color(GizmoAxis::Rotate, yellow));
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
    // Capture the active building's pose so the same delta can be applied to the
    // rest of a multi-selection (group move/rotate).
    const auto before_x = placement.x;
    const auto before_y = placement.y;
    const auto before_z = placement.z;
    const auto before_rot = placement.rotation;
    constexpr auto unit_size = 4.0F;
    const auto ray = get_viewport_mouse_ray(camera, viewport_mouse_pos, viewport_size);
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

    // Group move/rotate: apply the active building's delta to the rest of the
    // multi-selection. (Rotate sets an absolute facing, so the group shares it.)
    const auto dx = placement.x - before_x;
    const auto dy = placement.y - before_y;
    const auto dz = placement.z - before_z;
    const auto new_rot = placement.rotation;
    if (ui.selected_city_buildings.size() > 1U) {
        for (const auto idx : ui.selected_city_buildings) {
            if (idx == ui.selected_city_building || idx >= project.city->data.buildings.size()) {
                continue;
            }
            auto& other = project.city->data.buildings[idx];
            if (ui.gizmo_active == GizmoAxis::Rotate) {
                other.rotation = new_rot;
            } else {
                other.x += dx;
                other.y += dy;
                other.z += dz;
            }
        }
    }
    (void)before_rot;
}

// Overlay (not road rendering) showing the active road-laying chain: a marker on
// the pending start node and an axis-snapped preview line to the cursor, so you
// see the segment before clicking. Drawn inside the City 3D pass.
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

    if (!ui.pending_road_intersection.has_value() || *ui.pending_road_intersection >= map.intersections.size()) {
        return;
    }

    const auto& start = map.intersections[*ui.pending_road_intersection];
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
        if (ui.city_tool == CityTool::Select) {
            ImGui::TextUnformatted("Click selects whatever is under the cursor (building / road / node / piece)");
            ImGui::Text("Selected building/piece shows a gizmo  |  %s rotate  |  Del delete", k(KEY_R).c_str());
        } else if (ui.city_tool == CityTool::Building) {
            ImGui::TextUnformatted("Click drops the chosen building template on the ground");
            ImGui::Text("Pick a template in the Inspector  |  %s rotate", k(KEY_R).c_str());
        } else if (ui.city_tool == CityTool::Road) {
            ImGui::TextUnformatted("Click lays a road chain (axis-locked, snaps to nodes, splits roads)  |  Esc ends");
            ImGui::Text("%s %s left width  |  %s %s right width  |  %s %s district",
                k(KEY_LEFT_BRACKET).c_str(), k(KEY_RIGHT_BRACKET).c_str(),
                k(KEY_SEMICOLON).c_str(), k(KEY_APOSTROPHE).c_str(),
                k(KEY_MINUS).c_str(), k(KEY_EQUAL).c_str());
        } else if (ui.city_tool == CityTool::Piece) {
            ImGui::Text("Click places the chosen piece (hold-drag to draw)  |  %s rotate", k(KEY_R).c_str());
        } else { // Erase
            ImGui::TextUnformatted("Click deletes whatever is under the cursor (hold-drag to erase pieces)");
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
// World point the camera should frame for the current selection, plus a framing
// radius (how far back to sit). Returns nullopt when nothing is selected, so the
// caller can fall back to the whole-city framing.
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
                ey = static_cast<float>(std::max(1U, sbb->size_y)) * unit;
                ez = static_cast<float>(std::max(1U, sbb->size_z)) * unit;
            }
            const Vector3 center{o.x, o.y + ey * 0.5F, o.z};
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
        const Vector3 center{0.0F, static_cast<float>(std::max(1U, building.size_y)) * unit * 0.5F, 0.0F};
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

bool active_tab_dirty(const EditorUi& ui) {
    switch (ui.tab) {
        case EditorTab::City:
        case EditorTab::Pieces:
            return ui.city_dirty;
        case EditorTab::Buildings:
            return ui.building_dirty;
        case EditorTab::Buildblocks:
            return ui.buildblock_dirty;
        case EditorTab::Blocks:
            return ui.block_dirty;
    }
    return false;
}

const char* active_tool_label(const EditorUi& ui) {
    if (ui.tab == EditorTab::City) {
        switch (ui.city_tool) {
            case CityTool::Select: return "Select";
            case CityTool::Building: return "Place building";
            case CityTool::Road: return "Draw road";
            case CityTool::Piece: return "Place piece";
            case CityTool::Erase: return "Erase";
        }
    }
    if (ui.tab == EditorTab::Buildings) {
        switch (ui.building_tool) {
            case BuildingTool::Select: return "Select cell";
            case BuildingTool::PlaceBlock: return "Place block";
            case BuildingTool::PaintTexture: return "Paint texture";
        }
    }
    return "Inspect";
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
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

    // Global shortcuts.
    if (ctrl && IsKeyPressed(KEY_S)) {
        save_active_tab(project, ui, status);
        return;
    }
    // The current undo stack owns city changes only. Do not silently undo the city
    // while the user is editing a different asset type.
    const bool city_history_active = ui.tab == EditorTab::City || ui.tab == EditorTab::Pieces;
    if (city_history_active && ctrl && IsKeyPressed(KEY_Z)) {
        if (shift) {
            redo_city(project, ui, status);
        } else {
            undo_city(project, ui, status);
        }
        return;
    }
    if (city_history_active && ctrl && IsKeyPressed(KEY_Y)) {
        redo_city(project, ui, status);
        return;
    }
    if (IsKeyPressed(KEY_F)) {
        focus_current_selection(project, ui, camera);
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

    // Destructive and transform keys belong to the 3D canvas. Requiring the
    // pointer over the rendered image prevents Delete, arrows, and placement keys
    // from firing while the user works in another panel.
    if (!viewport_hovered) {
        return;
    }

    if (ui.tab == EditorTab::City && project.city) {
        // Piece rotation applies whenever the Piece tool is active.
        if (ui.city_tool == CityTool::Piece && IsKeyPressed(KEY_R)) {
            ui.selected_piece_rotation = (ui.selected_piece_rotation + 1U) & 3U;
        }
        // One snapshot per editing keypress (arrows / PgUp/Dn / R / Ctrl+D / Del).
        const bool edit_key = IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_UP) ||
            IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_PAGE_UP) || IsKeyPressed(KEY_PAGE_DOWN) ||
            IsKeyPressed(KEY_DELETE) || (ctrl && IsKeyPressed(KEY_D)) ||
            IsKeyPressed(KEY_LEFT_BRACKET) || IsKeyPressed(KEY_RIGHT_BRACKET) ||
            IsKeyPressed(KEY_SEMICOLON) || IsKeyPressed(KEY_APOSTROPHE) ||
            IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_EQUAL) ||
            (ui.city_selection == CitySelectionKind::Building && IsKeyPressed(KEY_R));
        if (edit_key && ui.city_selection != CitySelectionKind::None) {
            begin_city_action(project, ui, "edit selection");
        }
        // Edit keys act on the current selection (Select tool), regardless of tool.
        if (ui.city_selection == CitySelectionKind::Building && !project.city->data.buildings.empty()) {
            if (IsKeyPressed(KEY_LEFT)) nudge_city_building(project, ui, -1, 0, 0, 0);
            if (IsKeyPressed(KEY_RIGHT)) nudge_city_building(project, ui, 1, 0, 0, 0);
            if (IsKeyPressed(KEY_UP)) nudge_city_building(project, ui, 0, 0, -1, 0);
            if (IsKeyPressed(KEY_DOWN)) nudge_city_building(project, ui, 0, 0, 1, 0);
            if (IsKeyPressed(KEY_PAGE_UP)) nudge_city_building(project, ui, 0, 1, 0, 0);
            if (IsKeyPressed(KEY_PAGE_DOWN)) nudge_city_building(project, ui, 0, -1, 0, 0);
            if (IsKeyPressed(KEY_R)) nudge_city_building(project, ui, 0, 0, 0, shift ? 3 : 1);
            if (ctrl && IsKeyPressed(KEY_D)) duplicate_city_building(project, ui);
            if (IsKeyPressed(KEY_DELETE)) delete_city_building(project, ui);
        } else if ((ui.city_selection == CitySelectionKind::Road || ui.city_selection == CitySelectionKind::Node) &&
                   !project.city->data.streets.empty()) {
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
void draw_loading_overlay(EditorProject& project) {
    const bool buffers_ready =
        !project.render_build_in_progress &&
        project.pending_gpu_upload_indices.empty() &&
        project.pending_gpu_upload_index >= project.building_render_buffers.size();
    if (buffers_ready && !project.pregenerating) {
        return;
    }

    const auto vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5F, vp->WorkPos.y + vp->WorkSize.y * 0.5F),
                            ImGuiCond_Always, ImVec2(0.5F, 0.5F));
    ImGui::SetNextWindowBgAlpha(0.92F);
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

        const float thumb_total = std::max<float>(1.0F,
            static_cast<float>(project.buildings.size() + (project.city ? project.city->data.city_blocks.size() : 0U)));
        const float thumb_done = static_cast<float>(project.pregen_building_cursor + project.pregen_block_cursor);
        ImGui::TextUnformatted("Thumbnails");
        ImGui::ProgressBar(project.pregenerating ? thumb_done / thumb_total : 1.0F, ImVec2(360.0F, 0.0F));
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
    ImGui::SetNextWindowBgAlpha(0.92F);
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
    // Build the atlas with Latin-1 + Latin Extended (layout key names) and Font
    // Awesome icons (toolbar). rlImGuiSetup already merged FA; this rebuild adds
    // the wider Latin coverage and re-merges FA so both survive.
    load_editor_fonts(io);
    apply_editor_imgui_style();
    bool reset_layout_requested = io.IniFilename == nullptr || !std::filesystem::exists(io.IniFilename);

    Camera3D camera = default_camera();
    std::optional<EditorProject> project;
    EditorUi ui;
    std::string status = "drop a map folder or click File -> Open Folder";

    RenderTexture2D viewport_texture = LoadRenderTexture(1280, 800);
    RenderTexture2D building_outline_mask_texture = LoadRenderTexture(1280, 800);
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

    // Deferred project load. A drag-drop / Open Folder sets pending_load_folder;
    // the loop then presents one "Parsing..." frame before running the blocking
    // synchronous parse, so the load screen actually paints instead of freezing
    // on a blank window.
    std::optional<std::filesystem::path> pending_load_folder;
    // Optional auto-load: if MAPED_AUTOLOAD names a map folder, queue it at
    // startup. Convenient for profiling/screenshot runs that need a known map
    // open without the manual Open Folder step; no effect when the var is unset.
    if (const char* env = std::getenv("MAPED_AUTOLOAD")) {
        pending_load_folder = std::filesystem::path{env};
    }

    // Mesh-export options dialog. Opened by File > Export Mesh; the popup shows a
    // context-aware title + what-to-export checkboxes, then runs the native save
    // dialog. Options persist across exports during the session.
    bool open_export_popup = false;
    ExportOptions export_options{};
    bool layout_checked = false;

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

        if (IsKeyPressed(KEY_F3)) {
            show_graphics_debug = !show_graphics_debug;
        }

        if (auto folder = dropped_folder()) {
            pending_load_folder = std::move(*folder);
        }

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
            // Manual fly cancels any in-progress focus glide so they never fight.
            ui.focus_animating = false;
            update_free_fly_camera(camera, ui.camera_fly_speed);
        }
        update_focus_animation(ui, camera);

        // Editor hotkeys: only when ImGui is not capturing the keyboard (no active
        // text field) and the freecam is not flying, so they never collide with
        // typing or WASD movement.
        if (project && !io.WantCaptureKeyboard && !IsCursorHidden()) {
            handle_editor_hotkeys(*project, ui, camera, status, is_viewport_hovered);
        }

        const SbcBuilding* outline_selected_building = nullptr;
        const SbcBuilding* outline_hovered_building = nullptr;
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
                            MAPED_PROFILE_BLOCK("build initial map scene");
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
                        if (!project->city_scene_gpu_valid) {
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
                        draw_city_scene(*project, city_scene, selected_street, ui.render_roads, ui.render_terrain);
                        draw_city_editor_overlay(*project, city_scene);
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
                            const auto hov = pick_city_any(*project, city_scene, ro, rd);
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
                                &city_window_texture,
                                hovered_building);
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
                                draw_city_piece_glow(*ui.hover_piece_voxel, Color{200, 240, 255, 200});
                            }
                        }
                        GfxDebugScopedTimer timer{gfx_debug_stats, GfxDebugTimingPhase::PoleDraw};
                        draw_city_traffic_light_poles(city_scene);

                        // Road/Piece/Erase tools snap to the terrain surface and
                        // show the edit grid; the others use the flat grid cell.
                        const bool ground_tool = ui.city_tool == CityTool::Road ||
                            ui.city_tool == CityTool::Piece || ui.city_tool == CityTool::Erase;
                        const auto city_cursor = ground_tool
                            ? city_grid_point_under_mouse_terrain(camera, viewport_mouse_pos, last_viewport_size, city_scene)
                            : city_grid_point_under_mouse(camera, viewport_mouse_pos, last_viewport_size);
                        if ((ui.city_tool == CityTool::Road || ui.city_tool == CityTool::Piece) && city_cursor.has_value()) {
                            draw_city_edit_grid(*city_cursor, city_scene);
                        }
                        // Node selection/hover glow (roads get the silhouette outline).
                        draw_city_road_selection(*project, ui, city_scene);

                        // Show the placement cursor for the ground tools only.
                        if (city_cursor.has_value() &&
                            (ui.city_tool == CityTool::Road || ui.city_tool == CityTool::Piece ||
                             ui.city_tool == CityTool::Building || ui.city_tool == CityTool::Erase)) {
                            draw_city_cursor(*city_cursor, city_scene.unit_size, ui.city_tool);
                        }
                        draw_road_lay_preview(*project, ui, city_cursor);

                        // Gizmo on the selected building (Select tool). Hover-
                        // highlights the handle under the cursor unless dragging.
                        if (ui.city_tool == CityTool::Select && ui.city_selection == CitySelectionKind::Building &&
                            !project->city->data.buildings.empty()) {
                            const auto sel = std::min(ui.selected_city_building, project->city->data.buildings.size() - 1U);
                            const auto& placement = project->city->data.buildings[sel];
                            const auto hovered = ui.gizmo_active != GizmoAxis::None
                                ? ui.gizmo_active
                                : (is_viewport_hovered && !IsCursorHidden()
                                       ? pick_building_gizmo(camera, viewport_mouse_pos, last_viewport_size, placement)
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
                            // Cell under the cursor, for the silhouette hover outline.
                            ui.building_hover_cell.reset();
                            if (is_viewport_hovered && !IsCursorHidden()) {
                                const auto& bld = project->buildings[ui.selected_building].data;
                                const auto pr = get_viewport_mouse_ray(camera, viewport_mouse_pos, last_viewport_size);
                                const Vec3 ro{pr.position.x, pr.position.y, pr.position.z};
                                const Vec3 rd{pr.direction.x, pr.direction.y, pr.direction.z};
                                const auto pick = pick_building_cell(bld, ro, rd, 4.0F, preview_floor);
                                if (pick.has_value()) {
                                    ui.building_hover_cell = pick->cell_index;
                                }
                            }
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

            // Generic silhouette-outline: render a shape into the mask, then trace
            // a colored outline from it. Buildings, roads, and building cells all
            // use this so hover/selection feedback looks the same everywhere.
            auto composite_outline = [&](const std::function<void()>& draw_mask, Color tint) {
                GfxDebugScopedTimer timer{gfx_debug_stats, GfxDebugTimingPhase::OutlineDraw};
                BeginTextureMode(building_outline_mask_texture);
                ClearBackground(BLANK);
                BeginMode3D(camera);
                draw_mask();
                EndMode3D();
                EndTextureMode();

                BeginTextureMode(viewport_texture);
                draw_outline_from_mask(building_outline_mask_texture, last_viewport_size, tint);
                EndTextureMode();
            };

            const Color select_tint{220, 32, 32, 255};
            const Color hover_tint{120, 200, 255, 255};

            if (ui.tab == EditorTab::City && project->city) {
                // Outline every selected building (the whole multi-selection).
                if (ui.city_selection == CitySelectionKind::Building) {
                    if (ui.selected_city_buildings.size() > 1U) {
                        for (const auto idx : ui.selected_city_buildings) {
                            if (idx < project->city->data.buildings.size()) {
                                const auto* b = &project->city->data.buildings[idx];
                                composite_outline([&] { draw_city_building_mask_for_placement(*project, b); }, select_tint);
                            }
                        }
                    } else if (outline_selected_building != nullptr) {
                        composite_outline([&] { draw_city_building_mask_for_placement(*project, outline_selected_building); }, select_tint);
                    }
                }
                if (outline_hovered_building != nullptr && outline_hovered_building != outline_selected_building) {
                    composite_outline([&] { draw_city_building_mask_for_placement(*project, outline_hovered_building); }, hover_tint);
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
                if (ui.building_hover_cell.has_value() && *ui.building_hover_cell < building.cells.size() &&
                    (occupied.empty() || building.cells[*ui.building_hover_cell].index != building.cells[occupied[std::min(ui.selected_building_cell, occupied.size() - 1U)]].index)) {
                    const auto& hov = building.cells[*ui.building_hover_cell];
                    composite_outline([&] { draw_building_cell_mask(building, hov); }, hover_tint);
                }
            }
        }

        if (show_3d_viewport && project && is_viewport_hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !IsCursorHidden()) {
            if (ui.tab == EditorTab::City && project->city) {
                const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
                // Snapshot for undo before any mutating tool acts. Select only
                // mutates if it grabs a gizmo (handled by the gizmo-drag start),
                // so it is excluded here.
                switch (ui.city_tool) {
                    case CityTool::Building: begin_city_action(*project, ui, "place building"); break;
                    case CityTool::Road: begin_city_action(*project, ui, "lay road"); break;
                    case CityTool::Piece: begin_city_action(*project, ui, "place piece"); break;
                    case CityTool::Erase: begin_city_action(*project, ui, "erase"); break;
                    case CityTool::Select: break;
                }
                // Building/piece placement keeps the flat grid cell; road/piece/erase
                // snap to the terrain surface.
                const bool ground_tool = ui.city_tool == CityTool::Road ||
                    ui.city_tool == CityTool::Piece || ui.city_tool == CityTool::Erase;
                const auto city_cursor = ground_tool
                    ? city_grid_point_under_mouse_terrain(camera, viewport_mouse_pos, last_viewport_size, project->city_scene_cache)
                    : city_grid_point_under_mouse(camera, viewport_mouse_pos, last_viewport_size);
                const auto pick_ray = get_viewport_mouse_ray(camera, viewport_mouse_pos, last_viewport_size);
                const Vec3 ray_origin{pick_ray.position.x, pick_ray.position.y, pick_ray.position.z};
                const Vec3 ray_dir{pick_ray.direction.x, pick_ray.direction.y, pick_ray.direction.z};

                if (ui.city_tool == CityTool::Select) {
                    // First test the selected building's gizmo handles (drag if hit);
                    // otherwise select whatever is under the cursor (nearest wins).
                    GizmoAxis gizmo_hit = GizmoAxis::None;
                    if (ui.city_selection == CitySelectionKind::Building && !project->city->data.buildings.empty()) {
                        const auto sel = std::min(ui.selected_city_building, project->city->data.buildings.size() - 1U);
                        gizmo_hit = pick_building_gizmo(camera, viewport_mouse_pos, last_viewport_size, project->city->data.buildings[sel]);
                    }
                    if (gizmo_hit != GizmoAxis::None) {
                        begin_city_action(*project, ui, "move building");
                        const auto& placement = project->city->data.buildings[std::min(ui.selected_city_building, project->city->data.buildings.size() - 1U)];
                        ui.gizmo_active = gizmo_hit;
                        ui.gizmo_drag_origin_x = placement.x;
                        ui.gizmo_drag_origin_y = placement.y;
                        ui.gizmo_drag_origin_z = placement.z;
                        ui.gizmo_drag_origin_rot = placement.rotation;
                    } else {
                        // Begin a potential box-select. On release we decide: a tiny
                        // drag is a single click (pick under cursor), a real drag is a
                        // rubber-band box. This lets one gesture do both.
                        ui.box_select_active = true;
                        ui.box_select_start = viewport_mouse_pos;
                    }
                } else if (ui.city_tool == CityTool::Building) {
                    // Drop the chosen building template at the cursor.
                    if (city_cursor.has_value()) {
                        place_city_building_from_template(*project, ui, *city_cursor);
                    }
                } else if (ui.city_tool == CityTool::Road) {
                    // Continue from an existing node when the ray hits one, else lay
                    // a segment to the cursor cell.
                    const auto picked_node = pick_city_intersection(project->city->data, project->city_scene_cache, ray_origin, ray_dir);
                    if (picked_node.has_value()) {
                        const auto& node = project->city->data.intersections[*picked_node];
                        append_city_road_point(*project, ui, {node.x, node.y, node.z});
                    } else if (city_cursor.has_value()) {
                        append_city_road_point(*project, ui, *city_cursor);
                    }
                } else if (ui.city_tool == CityTool::Piece) {
                    if (city_cursor.has_value()) {
                        set_city_piece_voxel(*project, ui, *city_cursor);
                    }
                } else if (ui.city_tool == CityTool::Erase) {
                    // Delete whatever is under the cursor: piece voxel first (ground
                    // cell), else the nearest building/road/node.
                    erase_city_thing_under_cursor(*project, ui, project->city_scene_cache, ray_origin, ray_dir, city_cursor, status);
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
                } else {
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
                            place_building_block(*project, ui, *pick, unit_size, status);
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
                        }
                    } else {
                        status = ui.building_tool == BuildingTool::PlaceBlock
                            ? "click a block face to place against"
                            : "click a block face to paint";
                    }
                }
            }
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

        // Box-select release: decide click vs rubber-band box. A tiny drag is a
        // single click (pick under cursor); a real drag selects everything whose
        // screen point falls inside the rectangle.
        if (ui.box_select_active && IsMouseButtonReleased(MOUSE_BUTTON_LEFT) && project && project->city) {
            const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
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
        }
        if (show_3d_viewport && project && is_viewport_hovered && !IsCursorHidden() &&
            IsMouseButtonDown(MOUSE_BUTTON_LEFT) && ui.gizmo_active == GizmoAxis::None) {
            if (ui.tab == EditorTab::City && project->city &&
                (ui.city_tool == CityTool::Piece || ui.city_tool == CityTool::Erase)) {
                const auto cursor = city_grid_point_under_mouse_terrain(
                    camera, viewport_mouse_pos, last_viewport_size, project->city_scene_cache);
                if (cursor.has_value() && (!ui.drag_stamp_cell.has_value() || !(*ui.drag_stamp_cell == *cursor))) {
                    if (ui.city_tool == CityTool::Erase) {
                        clear_city_piece_voxel(*project, *cursor, ui);
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
                    if (!ui.drag_stamp_cell.has_value() || !(*ui.drag_stamp_cell == cell)) {
                        place_building_block(*project, ui, *pick, unit_size, status);
                        ui.drag_stamp_cell = cell;
                    }
                }
            }
        }

        BeginDrawing();
        ClearBackground(DARKGRAY);

        const auto imgui_begin = std::chrono::steady_clock::now();
        rlImGuiBegin();

        draw_editor_dockspace(reset_layout_requested);
        reset_layout_requested = false;

        // Warm the thumbnail caches a few per frame after load, and show a loading
        // overlay with progress until render buffers + thumbnails are all ready.
        if (project) {
            service_pregeneration(*project);
            draw_loading_overlay(*project);
        }

        bool open_folder_requested = false;
        bool export_mesh_requested = false;
        bool run_export_after_popup = false;

        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open Folder...", "Ctrl+O")) {
                    open_folder_requested = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Export Mesh...", nullptr, false, project != std::nullopt)) {
                    export_mesh_requested = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit", "Alt+F4")) {
                    close_requested = true;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View")) {
                if (ImGui::MenuItem("Reset Dock Layout")) {
                    reset_layout_requested = true;
                }
                ImGui::MenuItem("Graphics Debug", "F3", &show_graphics_debug);
                if (ImGui::BeginMenu("Developer")) {
                    ImGui::MenuItem("ImGui Demo Window", nullptr, &show_demo_window);
                    ImGui::EndMenu();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (project) {
                ImGui::TextDisabled("%s", project->root.filename().string().c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton(active_tab_dirty(ui) ? "Save *" : "Save")) {
                    save_active_tab(*project, ui, status);
                }
                ImGui::SameLine();
                const bool city_history_active = ui.tab == EditorTab::City || ui.tab == EditorTab::Pieces;
                ImGui::BeginDisabled(!city_history_active || project->undo_stack.empty());
                if (ImGui::SmallButton("Undo")) {
                    undo_city(*project, ui, status);
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(!city_history_active || project->redo_stack.empty());
                if (ImGui::SmallButton("Redo")) {
                    redo_city(*project, ui, status);
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::SmallButton("Focus")) {
                    focus_current_selection(*project, ui, camera);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Export")) {
                    export_mesh_requested = true;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", active_tool_label(ui));
                if (has_unsaved_changes(ui)) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.94F, 0.79F, 0.35F, 1.0F), "Unsaved");
                }
            } else if (ImGui::SmallButton("Open map folder...")) {
                open_folder_requested = true;
            }
            ImGui::EndMainMenuBar();
        }

        if (show_demo_window) {
            ImGui::ShowDemoWindow(&show_demo_window);
        }
        is_viewport_hovered = false;
        if (show_3d_viewport) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
            ImGui::Begin("Viewport 3D");
            {
                ImVec2 size = ImGui::GetContentRegionAvail();

                if ((size.x != last_viewport_size.x || size.y != last_viewport_size.y) && size.x > 0 && size.y > 0) {
                    UnloadRenderTexture(viewport_texture);
                    UnloadRenderTexture(building_outline_mask_texture);
                    viewport_texture = LoadRenderTexture(static_cast<int>(size.x), static_cast<int>(size.y));
                    building_outline_mask_texture = LoadRenderTexture(static_cast<int>(size.x), static_cast<int>(size.y));
                    last_viewport_size = { size.x, size.y };
                }

                rlImGuiImageRenderTexture(&viewport_texture);
                is_viewport_hovered = ImGui::IsItemHovered();

                ImVec2 min_pos = ImGui::GetWindowPos();
                min_pos.x += ImGui::GetWindowContentRegionMin().x;
                min_pos.y += ImGui::GetWindowContentRegionMin().y;

                viewport_mouse_pos = get_viewport_mouse_position(Vector2{min_pos.x, min_pos.y}, Vector2{size.x, size.y});

                // Blender-style tool overlay on the viewport's left edge. It draws
                // over the render image; hovering it suppresses 3D picking so a
                // toolbar click never also clicks into the scene.
                if (project) {
                    if (draw_viewport_toolbar(*project, ui, min_pos.x, min_pos.y)) {
                        is_viewport_hovered = false;
                    }
                    draw_controls_overlay(ui, min_pos.x, min_pos.y, size.x, size.y);
                }
                // Rubber-band selection rectangle (drawn in screen space over the
                // image while a box drag is in progress).
                if (ui.box_select_active && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                    const ImVec2 p0{min_pos.x + ui.box_select_start.x, min_pos.y + ui.box_select_start.y};
                    const ImVec2 p1{min_pos.x + viewport_mouse_pos.x, min_pos.y + viewport_mouse_pos.y};
                    auto* dl = ImGui::GetWindowDrawList();
                    dl->AddRectFilled(p0, p1, IM_COL32(120, 180, 255, 40));
                    dl->AddRect(p0, p1, IM_COL32(150, 200, 255, 220));
                }
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }

        ImGui::Begin("Asset Browser");
        if (!layout_checked) {
            layout_checked = true;
            reset_layout_requested = ImGui::GetWindowWidth() < 220.0F;
        }
        if (!project) {
            ImGui::Text("No project loaded.");
            if (ImGui::Button("Open Folder")) {
                open_folder_requested = true;
            }
        } else {
            ImGui::Text("Project: %s", project->root.filename().string().c_str());
            ImGui::Separator();

            EditorTab next_tab = ui.tab;
            if (ImGui::BeginTabBar("##workspace_tabs", ImGuiTabBarFlags_FittingPolicyResizeDown)) {
                const auto tab = [&](const char* label, EditorTab value) {
                    if (ImGui::BeginTabItem(label)) {
                        next_tab = value;
                        ImGui::EndTabItem();
                    }
                };
                tab("City", EditorTab::City);
                tab("Buildings", EditorTab::Buildings);
                tab("Pieces", EditorTab::Pieces);
                tab("Buildblocks", EditorTab::Buildblocks);
                tab("Blocks", EditorTab::Blocks);
                ImGui::EndTabBar();
            }
            if (next_tab != ui.tab) {
                ui.tab = next_tab;
                if (ui.tab == EditorTab::City) {
                    focus_camera_on_city(camera, *project);
                } else {
                    camera = default_camera();
                }
            }

            auto& search = ui.asset_search[static_cast<std::size_t>(ui.tab)];
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##asset_search", "Search this workspace...", search.data(), search.size());
            ImGui::Spacing();

            if (ui.tab == EditorTab::City) {
                if (project->city) {
                    ImGui::Text("city2.sbc");
                    ImGui::TextDisabled("Active tool: %s", active_tool_label(ui));
                    ImGui::Separator();
                    ImGui::TextUnformatted("Preview:");
                    int preview_mode = ui.render_roads && ui.render_terrain ? 1 : 0;
                    if (ImGui::RadioButton("Editor", &preview_mode, 0)) {
                        ui.render_roads = false;
                        ui.render_terrain = false;
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton("In game", &preview_mode, 1)) {
                        ui.render_roads = true;
                        ui.render_terrain = true;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Show the game terrain and finished road surfaces");
                    }
                    ImGui::Separator();

                    // The Building/Piece tools pick a template to place; show the
                    // template gallery for the active tool. Other tools list the
                    // map's existing things to select.
                    if (ui.city_tool == CityTool::Building && !project->buildings.empty()) {
                        draw_city_building_template_gallery(*project, ui);
                    } else if (ui.city_tool == CityTool::Piece && !project->city->data.city_blocks.empty()) {
                        draw_city_piece_template_gallery(*project, ui);
                        if (!project->city->data.portals.empty()) {
                            ui.selected_portal = std::min(ui.selected_portal, project->city->data.portals.size() - 1U);
                            auto portal_items = portal_names(project->city->data);
                            ImGui::SeparatorText("Portals");
                            if (ImGui::BeginListBox("##portal_preview_portals", ImVec2(-FLT_MIN, 140.0F))) {
                                for (std::size_t i = 0; i < portal_items.size(); ++i) {
                                    ImGui::PushID(static_cast<int>(i));
                                    if (ImGui::Selectable(portal_items[i].c_str(), ui.selected_portal == i)) {
                                        ui.selected_portal = i;
                                    }
                                    ImGui::PopID();
                                }
                                ImGui::EndListBox();
                            }
                        }
                    } else {
                        // Select/Road/Erase: list placed buildings and roads to pick.
                        if (!project->city->data.buildings.empty()) {
                            const auto names = city_building_names(project->city->data);
                            std::vector<std::size_t> visible;
                            visible.reserve(names.size());
                            for (std::size_t i = 0; i < names.size(); ++i) {
                                if (asset_search_matches(ui, names[i])) {
                                    visible.push_back(i);
                                }
                            }
                            ImGui::Text("Placed Buildings (%zu):", visible.size());
                            if (ImGui::BeginListBox("##placed_bldgs", ImVec2(-FLT_MIN, 150.0F))) {
                                ImGuiListClipper clipper;
                                clipper.Begin(static_cast<int>(visible.size()));
                                while (clipper.Step()) {
                                    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                                        const auto i = visible[static_cast<std::size_t>(row)];
                                        ImGui::PushID(static_cast<int>(i));
                                        const bool in_group = std::find(ui.selected_city_buildings.begin(), ui.selected_city_buildings.end(), i) != ui.selected_city_buildings.end();
                                        const bool active = ui.city_selection == CitySelectionKind::Building &&
                                            (ui.selected_city_building == i || in_group);
                                        if (ImGui::Selectable(names[i].c_str(), active)) {
                                            ui.selected_city_building = i;
                                            ui.city_selection = CitySelectionKind::Building;
                                            // Ctrl/Shift-click in the list toggles group membership.
                                            if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift) {
                                                toggle_in_selection(ui.selected_city_buildings, i);
                                            } else {
                                                ui.selected_city_buildings = {i};
                                            }
                                        }
                                        ImGui::PopID();
                                    }
                                }
                                ImGui::EndListBox();
                            }
                        }
                        if (!project->city->data.streets.empty()) {
                            const auto names = city_street_names(project->city->data);
                            std::vector<std::size_t> visible;
                            visible.reserve(names.size());
                            for (std::size_t i = 0; i < names.size(); ++i) {
                                if (asset_search_matches(ui, names[i])) {
                                    visible.push_back(i);
                                }
                            }
                            ImGui::Text("Roads (%zu):", visible.size());
                            if (ImGui::BeginListBox("##roads", ImVec2(-FLT_MIN, -FLT_MIN))) {
                                ImGuiListClipper clipper;
                                clipper.Begin(static_cast<int>(visible.size()));
                                while (clipper.Step()) {
                                    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                                        const auto i = visible[static_cast<std::size_t>(row)];
                                        ImGui::PushID(static_cast<int>(i));
                                        if (ImGui::Selectable(names[i].c_str(), ui.city_selection == CitySelectionKind::Road && ui.selected_city_street == i)) {
                                            ui.selected_city_street = i;
                                            ui.city_selection = CitySelectionKind::Road;
                                        }
                                        ImGui::PopID();
                                    }
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
                    const auto names = city_block_names(project->city->data);
                    std::vector<std::size_t> visible;
                    visible.reserve(names.size());
                    for (std::size_t i = 0; i < names.size(); ++i) {
                        if (asset_search_matches(ui, names[i])) {
                            visible.push_back(i);
                        }
                    }
                    ImGui::Text("City Block Templates (%zu):", visible.size());
                    if (ImGui::BeginListBox("##city_pieces", ImVec2(-FLT_MIN, -FLT_MIN))) {
                        ImGuiListClipper clipper;
                        clipper.Begin(static_cast<int>(visible.size()));
                        while (clipper.Step()) {
                            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                                const auto i = visible[static_cast<std::size_t>(row)];
                                ImGui::PushID(static_cast<int>(i));
                                const bool selected = (ui.selected_city_block == i);
                                if (ImGui::Selectable(names[i].c_str(), selected)) {
                                    ui.selected_city_block = i;
                                }
                                ImGui::PopID();
                            }
                        }
                        ImGui::EndListBox();
                    }
                }
            } else if (ui.tab == EditorTab::Blocks) {
                const auto names = block_names(*project);
                std::vector<std::size_t> visible;
                visible.reserve(names.size());
                for (std::size_t i = 0; i < names.size(); ++i) {
                    if (asset_search_matches(ui, names[i])) {
                        visible.push_back(i);
                    }
                }
                ImGui::Text("Blocks (*.sbl) (%zu):", visible.size());
                if (ImGui::BeginListBox("##blocks", ImVec2(-FLT_MIN, -FLT_MIN))) {
                    ImGuiListClipper clipper;
                    clipper.Begin(static_cast<int>(visible.size()));
                    while (clipper.Step()) {
                        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                            const auto i = visible[static_cast<std::size_t>(row)];
                            ImGui::PushID(static_cast<int>(i));
                            const bool selected = (ui.selected_block == i);
                            if (ImGui::Selectable(names[i].c_str(), selected)) {
                                ui.selected_block = i;
                            }
                            ImGui::PopID();
                        }
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
            if (ui.tab == EditorTab::City && ui.city_selection == CitySelectionKind::None) {
                ImGui::SeparatorText("Selection");
                ImGui::TextUnformatted("Nothing selected");
                ImGui::TextWrapped("Use Select in the viewport, then click a building, road, node, or piece.");
            }
            if (ui.tab == EditorTab::City && project->city) {
                if ((ui.city_selection == CitySelectionKind::Road || ui.city_selection == CitySelectionKind::Node) &&
                    (!project->city->data.intersections.empty() || !project->city->data.streets.empty())) {
                    if (!project->city->data.intersections.empty()) {
                        ui.selected_city_intersection =
                            std::min(ui.selected_city_intersection, project->city->data.intersections.size() - 1U);
                        const auto& node = project->city->data.intersections[ui.selected_city_intersection];
                        ImGui::Text("Node Inspector");
                        ImGui::Separator();
                        ImGui::Text("Node: %zu", ui.selected_city_intersection);
                        ImGui::Text("Grid: %d, %d, %d", node.x, node.y, node.z);
                        if (ui.pending_road_intersection.has_value()) {
                            ImGui::Text("Road Start: %zu", *ui.pending_road_intersection);
                        }
                    }
                    if (!project->city->data.streets.empty()) {
                        ImGui::Spacing();
                        ImGui::Separator();
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
                    }
                } else if (ui.city_selection == CitySelectionKind::Building && !project->city->data.buildings.empty()) {
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
                } else if (ui.city_selection == CitySelectionKind::Piece && project->city->data.city_blocks.empty() &&
                           ui.selected_piece_voxel.has_value()) {
                    // v10 maps (round) have no named city_blocks: the picked cell is a
                    // raw legacy district word. Decode and show its class/index inline
                    // since there's no city_block table to drive the normal inspector.
                    const auto word = ui.selected_piece_word;
                    const auto cell_class = word & 0xe0000000U;
                    ImGui::Text("Legacy District Cell (v10)");
                    ImGui::Separator();
                    ImGui::Text("Cell: [%d, %d, %d]", ui.selected_piece_voxel->x,
                        ui.selected_piece_voxel->y, ui.selected_piece_voxel->z);
                    ImGui::Text("Word: 0x%08X", word);
                    if (cell_class == 0x40000000U) {
                        ImGui::Text("Type: Solid cell (road/asphalt slab)");
                    } else if (cell_class == 0x80000000U) {
                        const auto catalog_index = static_cast<std::uint32_t>(word & 0xffffU);
                        const auto name = district_buildblock_name_for_index(
                            buildblock_templates(*project), catalog_index);
                        ImGui::Text("Type: Buildblock piece");
                        ImGui::Text("Catalog Index: %u", catalog_index);
                        ImGui::Text("Buildblock: %s", name.empty() ? "(empty / unloaded slot)" : name.c_str());
                        ImGui::Text("Rotation: %u", (word >> 24U) & 3U);
                    } else {
                        ImGui::Text("Type: 0x%08X", cell_class);
                    }
                    if (!project->city->data.portals.empty()) {
                        ui.selected_portal = std::min(ui.selected_portal, project->city->data.portals.size() - 1U);
                        const auto& portal = project->city->data.portals[ui.selected_portal];
                        ImGui::Spacing();
                        if (ImGui::CollapsingHeader("Portal Details", ImGuiTreeNodeFlags_DefaultOpen)) {
                            draw_portal_details(portal);
                        }
                    }
                } else if (ui.city_selection == CitySelectionKind::Piece && !project->city->data.city_blocks.empty()) {
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
            } else if (ui.tab == EditorTab::Pieces && project->city && project->city->data.city_blocks.empty()) {
                // v10 maps (round) store district cells as raw legacy words, not the
                // named city_blocks table this tab edits. Editing isn't supported;
                // point the user at the City tab where legacy cells can be inspected.
                ImGui::TextWrapped("This map (SBC v%u) has no named city blocks.",
                    project->city->data.version);
                ImGui::Spacing();
                ImGui::TextWrapped(
                    "Its district cells are legacy words rendered directly from the buildblock "
                    "catalog and solid road slabs. Select a cell on the City tab to inspect it.");
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
            (ui.city_tool == CityTool::Piece || ui.city_selection == CitySelectionKind::Piece) &&
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
                    draw_city_piece_voxels(*project, ui.portal_preview_textured, ui.selected_portal, true, true);
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

        // History backlog: the list of actions done, with undo/redo controls.
        ImGui::Begin("History");
        if (project && project->city) {
            const bool can_undo = !project->undo_stack.empty();
            const bool can_redo = !project->redo_stack.empty();
            ImGui::BeginDisabled(!can_undo);
            if (ImGui::Button("Undo")) undo_city(*project, ui, status);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!can_redo);
            if (ImGui::Button("Redo")) redo_city(*project, ui, status);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextDisabled("%s+Z / %s+Y", key_label(KEY_LEFT_CONTROL).c_str(), key_label(KEY_LEFT_CONTROL).c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear log")) project->action_log.clear();
            ImGui::Separator();
            ImGui::BeginChild("action-log", ImVec2(0, 0), true);
            for (auto it = project->action_log.rbegin(); it != project->action_log.rend(); ++it) {
                ImGui::TextUnformatted(it->c_str());
            }
            ImGui::EndChild();
        } else {
            ImGui::TextDisabled("No city loaded.");
        }
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
                pending_load_folder = std::filesystem::path{result};
            }
        }

        if (export_mesh_requested && project) {
            std::string reason;
            const auto default_path = default_export_path(*project, ui, reason);
            if (default_path.empty()) {
                status = "Export: " + reason + ".";
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
                status = std::string("Export: dialog error: ") + NFD::GetError();
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

        // Run a deferred load now that the normal frame has been presented. Paint
        // a dedicated "Parsing..." frame first so the screen shows progress, then
        // run the blocking synchronous parse and apply the result.
        if (pending_load_folder) {
            MAPED_PROFILE_BLOCK("load pending project");
            const auto folder = *pending_load_folder;
            pending_load_folder.reset();
            present_parsing_frame(folder);
            if (project) {
                free_project_thumbnails(*project);
            }
            project = load_project_folder(folder, status);
            if (project) {
                set_status(*project);
                status = "loaded " + project->root.filename().string();
                if (ui.tab == EditorTab::City) {
                    focus_camera_on_city(camera, *project);
                } else {
                    camera = default_camera();
                }
            }
        }
    }

    if (project) {
        free_project_thumbnails(*project);
    }
    UnloadRenderTexture(viewport_texture);
    UnloadRenderTexture(building_outline_mask_texture);
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
