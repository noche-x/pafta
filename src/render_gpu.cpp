// SPDX-License-Identifier: GPL-2.0-only
#include "app_internal.hpp"
#include "profiler.hpp"

#include <rlgl.h>
#include <imgui.h>

// rlImGui's texture upload (rlImGui.cpp). Its draw loop is replaced below.
void ImGui_ImplRaylib_UpdateTexture(ImTextureData* tex);
#include <raymath.h>
#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#include <emscripten/html5.h>
#else
#include <external/glad.h>
#endif

#include <algorithm>
#include <cfloat>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <cstdio>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef __EMSCRIPTEN__
#define PAFTA_GLSL_VERSION "#version 300 es\nprecision highp float;\nprecision highp int;\n"
#else
#define PAFTA_GLSL_VERSION "#version 330 core\n"
#endif

namespace pafta::app_internal {

GfxDebugStats* g_gfx_debug_stats = nullptr;
RenderSettings g_render_settings;
// Turns on the room-glass branch; the shader works out the pane's alpha itself.
constexpr float kRoomGlassAlpha = 1.0F;
// _resetgame starts the clock at 0x244140, which is 11:00.
float g_world_time_hours = 11.0F;

Vector3 game_light_direction() {
    // Game_RunFixedStep: angle = pi + t*2pi/5616000 in World mode (type 4), and
    // pi + t*2pi/5184000 in the others. DrawGameInfo shows t/216000 as the hour, so
    // World's sun runs a 26-hour day against a 24-hour clock. The client reads the
    // type as 4 bits (Net_ProcessServerSnapshotStream), so modes built on World, like
    // Ultimate (type 4 + 16), get the 26 hours too.
    // ponytail: World only; Round, Race, and other types would need 24 here.
    // Math_Vec3RotateAroundAxis @7b4d0 turns by -angle (v.cos + (v x a).sin), the
    // opposite way to raylib's rotation.
    constexpr float kSunCycleHours = 26.0F;
    const float angle = PI + g_world_time_hours*(2.0F*PI/kSunCycleHours);
    return Vector3RotateByAxisAngle(
        Vector3Normalize(Vector3{0.0F, 1.0F, 0.5F}),
        Vector3{0.0F, 0.4472135901F, -0.8944271803F},
        -angle);
}

std::string format_ms(double ms) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(2);
    stream << ms << "ms";
    return stream.str();
}

std::string format_bytes(std::size_t bytes) {
    std::ostringstream stream;
    stream.setf(std::ios::fixed);
    stream.precision(2);
    if (bytes >= 1024U * 1024U) {
        stream << static_cast<double>(bytes) / static_cast<double>(1024U * 1024U) << " MiB";
    } else if (bytes >= 1024U) {
        stream << static_cast<double>(bytes) / static_cast<double>(1024U) << " KiB";
    } else {
        stream << bytes << " B";
    }
    return stream.str();
}

struct GameRenderResources {
    Texture2D packed_depth{};
    Texture2D shadow_factor{};
    RenderTexture2D display{};
    Texture2D direct_light{};
};

std::unordered_map<unsigned int, GameRenderResources>& game_render_resources() {
    static std::unordered_map<unsigned int, GameRenderResources> resources;
    return resources;
}

namespace {
// Whether the bound game target has the packed-depth and direct-light attachments,
// and how many draw buffers are on. We track this ourselves instead of asking GL,
// because glGet* makes some drivers wait for the GPU (Apple's GL-on-Metal
// especially).
bool g_target_has_mrt = false;
int g_draw_buffer_count = 1;
}  // namespace

void set_game_render_depth_output(bool enabled) {
    // The game surface shader writes the lit colour, the packed straight-line
    // distance the atmosphere pass uses, and the direct light the shadow pass
    // removes. WebGL2 needs a fragment output for every active draw buffer, so all
    // three are only on while that shader draws. raylib's helper shaders (grids,
    // lines, ImGui) only write colour 0.
    const int wanted = enabled && g_target_has_mrt ? 3 : 1;
    if (wanted == g_draw_buffer_count) return;
    rlDrawRenderBatchActive();
    static constexpr GLenum outputs[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    glDrawBuffers(wanted, outputs);
    g_draw_buffer_count = wanted;
}

#ifdef __EMSCRIPTEN__
// WebGL2 can only draw to half floats with EXT_color_buffer_float (every current
// desktop browser has it). With it, the web build matches desktop: HDR scene colour,
// then the Reinhard and sRGB step. Without it, colours stay 8-bit with no tone
// mapping.
bool web_hdr_targets() {
    static const bool supported =
        emscripten_webgl_enable_extension(emscripten_webgl_get_current_context(), "EXT_color_buffer_float");
    return supported;
}
#endif

RenderTexture2D load_game_render_texture(int width, int height) {
    auto target = LoadRenderTexture(width, height);
    if (target.id == 0U) throw std::runtime_error("Could not allocate Pafta viewport framebuffer");

#ifdef __EMSCRIPTEN__
    const bool hdr = web_hdr_targets();
#else
    constexpr bool hdr = true;
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, target.id);
    if (hdr) {
        rlUnloadTexture(target.texture.id);
        target.texture = {rlLoadTexture(nullptr, width, height, PIXELFORMAT_UNCOMPRESSED_R16G16B16A16, 1),
            width, height, 1, PIXELFORMAT_UNCOMPRESSED_R16G16B16A16};
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture.id, 0);
    }

    // One packed depth and stencil attachment is used for depth tests and shadow volumes.
    if (target.depth.id != 0U) glDeleteRenderbuffers(1, &target.depth.id);
    glGenRenderbuffers(1, &target.depth.id);
    glBindRenderbuffer(GL_RENDERBUFFER, target.depth.id);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, target.depth.id);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, target.depth.id);

    Texture2D packed_depth{rlLoadTexture(nullptr, width, height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1),
        width, height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    Texture2D shadow_factor{rlLoadTexture(nullptr, width, height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1),
        width, height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    Texture2D direct_light{rlLoadTexture(nullptr, width, height, target.texture.format, 1),
        width, height, 1, target.texture.format};
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, packed_depth.id, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, direct_light.id, 0);
    const GLenum draw_buffers[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
#ifdef __EMSCRIPTEN__
    glDrawBuffers(1, draw_buffers);
#else
    glDrawBuffers(2, draw_buffers);
#endif
    const auto scene_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    auto display = LoadRenderTexture(width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, display.id);
    const auto display_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (target.depth.id == 0U || packed_depth.id == 0U || shadow_factor.id == 0U || direct_light.id == 0U ||
        display.id == 0U || scene_status != GL_FRAMEBUFFER_COMPLETE || display_status != GL_FRAMEBUFFER_COMPLETE) {
        UnloadTexture(packed_depth);
        UnloadTexture(shadow_factor);
        UnloadTexture(direct_light);
        UnloadRenderTexture(display);
        UnloadRenderTexture(target);
        throw std::runtime_error("Pafta viewport render target is incomplete (" +
            std::to_string(width) + "x" + std::to_string(height) + ")");
    }
    game_render_resources()[target.id] = {packed_depth, shadow_factor, display, direct_light};
    return target;
}

void begin_game_texture_mode(RenderTexture2D target) {
    g_draw_buffer_count = 1;
    g_target_has_mrt = game_render_resources().contains(target.id);
    BeginTextureMode(target);
    rlDrawRenderBatchActive();
    // The sky and shadow passes read these textures. Put their output attachments back
    // before drawing geometry; they have to be detached to be read.
    if (const auto it = game_render_resources().find(target.id); it != game_render_resources().end()) {
        glFramebufferTexture2D(
            GL_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT1,
            GL_TEXTURE_2D,
            it->second.packed_depth.id,
            0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, it->second.direct_light.id, 0);
        const GLenum color_only[] = {GL_COLOR_ATTACHMENT0};
        glDrawBuffers(1, color_only);
    }
    // Changing tabs can interrupt the city shadow pass. Never carry its write masks or
    // stencil test into the next viewport frame.
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xff);
    glDisable(GL_STENCIL_TEST);
}

void end_game_texture_mode() {
    g_draw_buffer_count = 1;
    g_target_has_mrt = false;
    rlDrawRenderBatchActive();
    const auto target = rlGetActiveFramebuffer();
    if (game_render_resources().contains(target)) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, 0, 0);
        const GLenum color_only[] = {GL_COLOR_ATTACHMENT0};
        glDrawBuffers(1, color_only);
    }
    EndTextureMode();
}

void unload_game_render_texture(RenderTexture2D target) {
    if (target.id == 0U) return;
    auto& resources = game_render_resources();
    if (const auto it = resources.find(target.id); it != resources.end()) {
        UnloadTexture(it->second.packed_depth);
        UnloadTexture(it->second.shadow_factor);
        UnloadTexture(it->second.direct_light);
        UnloadRenderTexture(it->second.display);
        resources.erase(it);
    }
    UnloadRenderTexture(target);
}

Texture2D game_render_packed_depth(RenderTexture2D target) {
    const auto& resources = game_render_resources();
    const auto it = resources.find(target.id);
    return it == resources.end() ? Texture2D{} : it->second.packed_depth;
}

void clear_game_render_packed_depth() {
    if (game_render_resources().empty()) return;
    constexpr GLfloat clear_depth[] = {1.0F, 1.0F, 1.0F, 1.0F};
    rlDrawRenderBatchActive();
    const GLenum outputs[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
    glDrawBuffers(3, outputs);
    glClearBufferfv(GL_COLOR, 1, clear_depth);
    constexpr GLfloat no_light[] = {0, 0, 0, 0};
    glClearBufferfv(GL_COLOR, 2, no_light);
    const GLenum color_only[] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, color_only);
    g_draw_buffer_count = 1;
}

void clear_game_render_shadow_factor() {
    if (game_render_resources().empty()) return;
    GLint framebuffer = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    const auto resource = game_render_resources().find(static_cast<unsigned int>(framebuffer));
    if (resource == game_render_resources().end()) return;
    GLint scene_color = 0;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &scene_color);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D,
        resource->second.shadow_factor.id,
        0);
    const GLenum shadow_only[] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, shadow_only);
    constexpr GLfloat fully_lit[] = {1.0F, 1.0F, 1.0F, 1.0F};
    glClearBufferfv(GL_COLOR, 0, fully_lit);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, static_cast<GLuint>(scene_color), 0);
    const GLenum color_only[] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, color_only);
}

struct GameTonemapShader {
    Shader shader{};
    int resolution_loc{-1};
    int apply_tonemap_loc{-1};
    int shadow_factor_loc{-1};
    int packed_depth_loc{-1};
    int direct_light_loc{-1};
};

GameTonemapShader& game_tonemap_shader_cache() {
    static GameTonemapShader cache = [] {
        GameTonemapShader value;
        value.shader = LoadShaderFromMemory(nullptr, PAFTA_GLSL_VERSION R"glsl(
in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform sampler2D shadowFactor;
uniform sampler2D directLight;
uniform sampler2D packedDepth;
uniform vec2 resolution;
uniform int applyTonemap;
out vec4 finalColor;
vec3 convertToSrgb(vec3 linearColor) {
    bvec3 cutoff=lessThan(linearColor,vec3(0.0031308));
    vec3 higher=vec3(1.055)*pow(linearColor,vec3(1.0/2.4))-vec3(0.055);
    vec3 lower=linearColor*vec3(12.92);
    return mix(higher,lower,cutoff);
}
vec3 sceneColor(vec2 uv) {
    // The game's world.frag only shadows direct diffuse and specular light. Keep the
    // sky, ambient light, and reflections instead of darkening the whole image.
    return max(texture(texture0, uv).rgb - texture(directLight, uv).rgb * (1.0-texture(shadowFactor, uv).r), vec3(0.0));
}
void main() {
    // hdr_composite.frag: Reinhard, then sRGB, with no anti-aliasing pass.
    vec3 color=sceneColor(fragTexCoord);
    if (applyTonemap==1) {
        color=color/(color+vec3(1.0));
        color=convertToSrgb(color);
    }
    finalColor=vec4(color,1.0);
}
)glsl");
        value.resolution_loc = GetShaderLocation(value.shader, "resolution");
        value.apply_tonemap_loc = GetShaderLocation(value.shader, "applyTonemap");
        value.shadow_factor_loc = GetShaderLocation(value.shader, "shadowFactor");
        value.packed_depth_loc = GetShaderLocation(value.shader, "packedDepth");
        value.direct_light_loc = GetShaderLocation(value.shader, "directLight");
        return value;
    }();
    return cache;
}

void resolve_game_render_texture(RenderTexture2D target) {
    const auto it = game_render_resources().find(target.id);
    if (it == game_render_resources().end()) return;
    if (it->second.display.id == 0U) return;
    begin_game_texture_mode(it->second.display);
    ClearBackground(BLACK);
    auto& tonemap = game_tonemap_shader_cache();
    auto& shader = tonemap.shader;
    const Vector2 resolution{
        static_cast<float>(target.texture.width),
        static_cast<float>(target.texture.height),
    };
    SetShaderValue(shader, tonemap.resolution_loc, &resolution.x, SHADER_UNIFORM_VEC2);
    const int apply_tonemap =
#ifdef __EMSCRIPTEN__
        web_hdr_targets() ? 1 : 0;
#else
        1;
#endif
    SetShaderValue(shader, tonemap.apply_tonemap_loc, &apply_tonemap, SHADER_UNIFORM_INT);
    BeginShaderMode(shader);
    SetShaderValueTexture(shader, tonemap.shadow_factor_loc, it->second.shadow_factor);
    SetShaderValueTexture(shader, tonemap.direct_light_loc, it->second.direct_light);
    SetShaderValueTexture(shader, tonemap.packed_depth_loc, it->second.packed_depth);
    DrawTextureRec(target.texture, {0.0F, 0.0F, static_cast<float>(target.texture.width), -static_cast<float>(target.texture.height)}, {0.0F, 0.0F}, WHITE);
    EndShaderMode();
    end_game_texture_mode();
}

const RenderTexture2D* game_render_display_texture(RenderTexture2D target) {
    const auto it = game_render_resources().find(target.id);
    return it == game_render_resources().end() ? nullptr : &it->second.display;
}

void append_render_log(EditorProject& project, std::string message) {
    // Progress, not errors: stdout keeps it out of the browser's error console.
    std::cout << "[render] " << message << '\n';
    project.render_logs.push_back(std::move(message));
    if (project.render_logs.size() > 80U) {
        project.render_logs.erase(project.render_logs.begin(), project.render_logs.begin() + static_cast<std::ptrdiff_t>(project.render_logs.size() - 80U));
    }
}

void push_render_progress_log(const std::shared_ptr<RenderBuildProgress>& progress, std::string message) {
    std::scoped_lock lock(progress->mutex);
    progress->logs.push_back(std::move(message));
}

std::string render_build_progress_status(const std::shared_ptr<RenderBuildProgress>& progress) {
    if (progress == nullptr) {
        return "building render buffers...";
    }

    std::scoped_lock lock(progress->mutex);
    std::string status = "building render buffers " + std::to_string(progress->completed) + "/" + std::to_string(progress->total);
    if (!progress->current.empty()) {
        status += ": " + progress->current;
    }
    return status;
}

void drain_render_build_progress(EditorProject& project) {
    if (project.render_build_progress == nullptr) {
        return;
    }

    std::vector<std::string> logs;
    std::vector<RenderBuildProgress::ReadyBuildingBuffers> ready_buffers;
    {
        std::scoped_lock lock(project.render_build_progress->mutex);
        logs = std::move(project.render_build_progress->logs);
        project.render_build_progress->logs.clear();
        ready_buffers = std::move(project.render_build_progress->ready_buffers);
        project.render_build_progress->ready_buffers.clear();
    }

    for (auto& log : logs) {
        append_render_log(project, std::move(log));
    }
    if (project.render_build_progress->generation != project.render_build_generation || project.render_buffers_dirty) {
        return;
    }
    for (auto& ready : ready_buffers) {
        if (ready.index >= project.building_render_buffers.size() ||
            ready.index >= project.building_gpu_models.size()) {
            continue;
        }
        project.building_render_buffers[ready.index] = std::move(ready.city_buffer);
        project.pending_gpu_upload_indices.push_back(ready.index);
    }
}

void start_async_render_buffer_rebuild(EditorProject& project) {
    PAFTA_PROFILE_FUNCTION();
    auto buildings = project.buildings;
    auto blocks = block_meshes(project);
    auto templates = buildblock_templates(project);
    const auto generation = ++project.render_build_generation;
    auto progress = std::make_shared<RenderBuildProgress>();
    progress->generation = generation;
    progress->total = buildings.size();
    project.render_build_progress = progress;
    project.render_buffers_dirty = false;
    project.render_build_in_progress = true;
    project.building_render_buffers.clear();
    project.building_render_buffers.resize(buildings.size());
    // Keep the current models drawing until each new one uploads (the upload
    // move-assigns over them). Clearing them here made the whole city go blank.
    project.building_gpu_models.resize(buildings.size());
    project.building_floor_preview_dirty = true;
    project.pending_gpu_upload_indices.clear();
    project.pending_gpu_upload_index = 0;
    append_render_log(project, "render build queued: " + std::to_string(buildings.size()) + " buildings");

    project.render_build_future = std::async(std::launch::async, [
        buildings = std::move(buildings),
        blocks = std::move(blocks),
        templates = std::move(templates),
        generation,
        progress]() {
        PAFTA_PROFILE_THREAD("Building render rebuild");
        PAFTA_PROFILE_BLOCK("build async building render buffers");
        using Clock = std::chrono::steady_clock;
        RenderBuildResult result;
        result.generation = generation;
        const auto total_begin = Clock::now();
        std::atomic<std::size_t> next_building{0};
        const auto worker_count = std::min<std::size_t>(
            buildings.size(),
            std::min(8U, std::max(1U, std::thread::hardware_concurrency())));
        std::vector<std::future<void>> workers;
        workers.reserve(worker_count);
        for (std::size_t worker = 0; worker < worker_count; ++worker) {
            workers.push_back(std::async(std::launch::async, [&] {
                for (auto building_index = next_building.fetch_add(1);
                     building_index < buildings.size();
                     building_index = next_building.fetch_add(1)) {
                    const auto& building = buildings[building_index];
                    {
                        std::scoped_lock lock(progress->mutex);
                        progress->current = building.name;
                    }
                    const auto begin = Clock::now();
                    auto city_buffer = build_city_building_full_render_buffer(building.data, blocks, templates, BuildingLod::Far);
                    bake_shadow_volume(city_buffer);
                    const auto end = Clock::now();
                    const auto ms = std::chrono::duration<double, std::milli>(end - begin).count();
                    const auto expanded_vertex_count = city_buffer.triangles.size() * 3U;
                    {
                        std::scoped_lock lock(progress->mutex);
                        ++progress->completed;
                        progress->logs.push_back(
                            "cpu buffer " + building.name +
                            ": " + std::to_string(city_buffer.vertices.size()) + " city verts, " +
                            std::to_string(city_buffer.triangles.size()) + " city tris, " +
                            std::to_string(expanded_vertex_count > city_buffer.vertices.size() ? expanded_vertex_count - city_buffer.vertices.size() : 0U) + " verts avoided, " +
                            format_ms(ms));
                        progress->ready_buffers.push_back({
                            building_index,
                            std::move(city_buffer),
                        });
                    }
                }
            }));
        }
        for (auto& worker : workers) worker.get();

        {
            std::scoped_lock lock(progress->mutex);
            progress->current.clear();
        }
        const auto total_end = Clock::now();
        const auto total_ms = std::chrono::duration<double, std::milli>(total_end - total_begin).count();
        push_render_progress_log(progress, "cpu render build total: " + format_ms(total_ms));
        return result;
    });
}

#ifdef __EMSCRIPTEN__
void start_incremental_render_buffer_rebuild(EditorProject& project) {
    const auto generation = ++project.render_build_generation;
    auto progress = std::make_shared<RenderBuildProgress>();
    progress->generation = generation;
    progress->total = project.buildings.size();
    // build_next_browser_render_buffer then builds one building per frame.
    project.render_build_progress = std::move(progress);
    project.render_buffers_dirty = false;
    project.render_build_in_progress = !project.buildings.empty();
    project.building_render_buffers.clear();
    project.building_render_buffers.resize(project.buildings.size());
    // Keep the current models drawing until each new one uploads.
    project.building_gpu_models.resize(project.buildings.size());
    project.building_floor_preview_dirty = true;
    project.pending_gpu_upload_indices.clear();
    project.pending_gpu_upload_index = 0;
}

void build_next_browser_render_buffer(EditorProject& project) {
    if (!project.render_build_in_progress || project.render_build_progress == nullptr) {
        return;
    }
    const auto index = project.render_build_progress->completed;
    if (index >= project.buildings.size()) {
        project.render_build_in_progress = false;
        project.render_build_progress.reset();
        return;
    }
    project.render_build_progress->current = project.buildings[index].name;
    project.building_render_buffers[index] = build_city_building_full_render_buffer(
        project.buildings[index].data,
        block_meshes(project),
        buildblock_templates(project),
        BuildingLod::Far);
    project.pending_gpu_upload_indices.push_back(index);
    ++project.render_build_progress->completed;
    if (project.render_build_progress->completed >= project.buildings.size()) {
        project.render_build_in_progress = false;
        project.render_build_progress.reset();
    }
}
#endif

void accept_render_build_result(EditorProject& project, RenderBuildResult result) {
    drain_render_build_progress(project);
    project.render_build_in_progress = false;
    if (result.generation != project.render_build_generation || project.render_buffers_dirty) {
        append_render_log(project, "discarded stale render build");
        project.render_build_progress.reset();
        return;
    }

    project.render_build_progress.reset();
    append_render_log(project, "cpu render build accepted: " + std::to_string(project.building_render_buffers.size()) + " buildings");
}

void upload_next_pending_building(EditorProject& project) {
    PAFTA_PROFILE_FUNCTION();
    if (project.pending_gpu_upload_indices.empty()) {
        return;
    }

    using Clock = std::chrono::steady_clock;
    const auto index = project.pending_gpu_upload_indices.front();
    project.pending_gpu_upload_indices.pop_front();
    if (index >= project.building_render_buffers.size() ||
        index >= project.building_gpu_models.size()) {
        return;
    }
    const auto begin = Clock::now();
    if (index >= project.buildings.size()) {
        return;
    }
    project.building_gpu_models[index] = upload_building_render_model(project.building_render_buffers[index], project.buildings[index].data, project);
    keep_pick_data_only(project.building_render_buffers[index]);
    // Loading uses far more memory than the editor keeps, so hand the freed pages back.
    if (project.pending_gpu_upload_indices.empty()) release_free_memory();
    const auto end = Clock::now();
    const auto ms = std::chrono::duration<double, std::milli>(end - begin).count();
    const auto name = index < project.buildings.size() ? project.buildings[index].name : std::to_string(index);
    const auto& model = project.building_gpu_models[index];
    append_render_log(
        project,
        "gpu upload " + name +
        ": " + std::to_string(model.mesh.parts.size()) + " parts, " +
        std::to_string(model.mesh.vertex_count) + " verts, " +
        std::to_string(model.mesh.index_count) + " indices, " +
        format_bytes(model.uploaded_bytes) + ", " +
        format_ms(ms));
    ++project.pending_gpu_upload_index;
}

void invalidate_portal_render_models(EditorProject& project) {
    project.portal_render_dirty = true;
    project.portal_preview_render_dirty = true;
}

void invalidate_city_block_preview_scene(EditorProject& project) {
    project.cached_city_block_preview_index.reset();
}

void service_render_rebuild(EditorProject& project) {
    PAFTA_PROFILE_FUNCTION();
    if (project.render_buffers_dirty && !project.render_build_in_progress) {
#ifdef __EMSCRIPTEN__
        start_incremental_render_buffer_rebuild(project);
#else
        start_async_render_buffer_rebuild(project);
#endif
    }

#ifdef __EMSCRIPTEN__
    // No worker threads here: build and upload buildings until about 12 ms of this
    // frame is used, so loading speed depends on the work, not on frame pacing.
    const auto budget_end = std::chrono::steady_clock::now() + std::chrono::milliseconds{12};
    do {
        build_next_browser_render_buffer(project);
        upload_next_pending_building(project);
    } while (project.render_build_in_progress && std::chrono::steady_clock::now() < budget_end);
#else
    drain_render_build_progress(project);

    if (project.render_build_future.has_value()) {
        const auto ready = project.render_build_future->wait_for(std::chrono::milliseconds{0});
        if (ready == std::future_status::ready) {
            auto result = project.render_build_future->get();
            project.render_build_future.reset();
            accept_render_build_result(project, std::move(result));
        }
    }
#endif

    if (!project.pending_gpu_upload_indices.empty()) {
        // Upload for up to about 8 ms of the frame instead of one building per frame,
        // so loading speed depends on the work, not the frame rate.
        std::optional<GfxDebugScopedTimer> timer;
        if (g_gfx_debug_stats != nullptr) timer.emplace(*g_gfx_debug_stats, GfxDebugTimingPhase::GpuUpload);
        const auto budget_end = std::chrono::steady_clock::now() + std::chrono::milliseconds{8};
        do {
            upload_next_pending_building(project);
        } while (!project.pending_gpu_upload_indices.empty() && std::chrono::steady_clock::now() < budget_end);
    }

    if (!project.render_build_in_progress &&
        project.pending_gpu_upload_indices.empty() &&
        project.pending_gpu_upload_index >= project.building_render_buffers.size() &&
        !project.building_render_buffers.empty()) {
        project.status = "render ready: " + std::to_string(project.building_render_buffers.size()) + " buildings";
    } else if (project.render_build_in_progress) {
        project.status = render_build_progress_status(project.render_build_progress);
    } else if (!project.pending_gpu_upload_indices.empty()) {
        project.status = "uploading meshes " + std::to_string(project.pending_gpu_upload_index) + "/" + std::to_string(project.building_render_buffers.size());
    }
}

Vector3 to_raylib(Vec3 value) {
    return {value.x, value.y, value.z};
}

// The selection highlight: a pulsing, layered wireframe box that looks like a glow
// without a bloom pass (which would need a shader over the whole scene and fragile
// GL code on each platform). Used for the selected building, road, and piece, so
// selection looks the same everywhere.

Vector3 scaled(Vec3f value, Vec3 origin, float scale) {
    return {
        origin.x + value.x * scale,
        origin.y + value.y * scale,
        origin.z + value.z * scale,
    };
}

Vector3 city_transform(Vector3 origin, Vector3 local, std::int32_t rotation) {
    const auto rotated = native_city_rotate_local({local.x, local.y, local.z}, rotation);
    return {origin.x + rotated.x, origin.y + rotated.y, origin.z + rotated.z};
}

void draw_mesh_faces(const SblBlockMesh& mesh, Vec3 origin, float scale, Color fill) {
    for (const auto& face : mesh.faces) {
        if (face.vertices.size() < 4) {
            continue;
        }
        const auto color = face.authored ? Color{220, 152, 70, 220} : fill;
        const auto a = scaled(face.vertices[0], origin, scale);
        const auto b = scaled(face.vertices[1], origin, scale);
        const auto c = scaled(face.vertices[2], origin, scale);
        const auto d = scaled(face.vertices[3], origin, scale);
        DrawTriangle3D(a, b, c, color);
        DrawTriangle3D(c, b, a, color);
        DrawTriangle3D(a, c, d, color);
        DrawTriangle3D(d, c, a, color);
        if (g_gfx_debug_stats != nullptr) {
            g_gfx_debug_stats->add_draw(GfxDebugDrawCategory::Pieces, 4U, 12U, 4U);
        }
        DrawLine3D(a, b, Color{20, 23, 26, 255});
        DrawLine3D(b, c, Color{20, 23, 26, 255});
        DrawLine3D(c, d, Color{20, 23, 26, 255});
        DrawLine3D(d, a, Color{20, 23, 26, 255});
        if (g_gfx_debug_stats != nullptr) {
            g_gfx_debug_stats->add_draw(GfxDebugDrawCategory::WireOverlay, 0U, 8U, 4U);
        }
    }
}

void draw_mesh_faces(const SblBlockMesh& mesh, float scale, Color fill) {
    draw_mesh_faces(mesh, {}, scale, fill);
}

void draw_render_buffer_faces(const BuildingRenderBuffer& buffer, Color fill) {
    for (const auto& triangle : buffer.triangles) {
        if (triangle.indices[0] >= buffer.vertices.size() ||
            triangle.indices[1] >= buffer.vertices.size() ||
            triangle.indices[2] >= buffer.vertices.size()) {
            continue;
        }
        const auto a = to_raylib(buffer.vertices[triangle.indices[0]].position);
        const auto b = to_raylib(buffer.vertices[triangle.indices[1]].position);
        const auto c = to_raylib(buffer.vertices[triangle.indices[2]].position);
        DrawTriangle3D(a, b, c, fill);
        DrawTriangle3D(c, b, a, fill);
    }
    for (const auto& line : buffer.wire_lines) {
        DrawLine3D(to_raylib(line.from), to_raylib(line.to), Color{20, 23, 26, 255});
    }
}

Color native_material_color(std::uint32_t material, bool selected, bool window_material = false) {
    if (selected) {
        return {235, 194, 82, 210};
    }
    if (window_material) {
        return {170, 190, 201, 220};
    }
    if (material == 0x100U) {
        return {0, 0, 0, 191};
    }
    switch (material % 6U) {
        case 1:
            return {217, 213, 203, 255};
        case 2:
            return {98, 148, 204, 210};
        case 3:
            return {145, 190, 215, 120};
        case 4:
            return {126, 151, 174, 170};
        case 5:
            return {220, 152, 70, 220};
        case 0:
        default:
            return {197, 139, 97, 255};
    }
}

const Texture2D* find_project_texture(const EditorProject& project, std::string_view name) {
    const auto key = lower_name(name);
    const auto match = std::ranges::find_if(project.textures, [&key](const LoadedTexture& texture) {
        return lower_name(texture.name) == key;
    });
    return match == project.textures.end() ? nullptr : &match->texture;
}

const Texture2D* find_project_material_texture(const EditorProject& project, std::string_view name) {
    const auto key = lower_name(name);
    const auto match = std::ranges::find_if(project.material_textures, [&key](const LoadedTexture& texture) {
        return lower_name(texture.name) == key;
    });
    return match == project.material_textures.end() ? nullptr : &match->texture;
}

const Texture2D& generated_game_landscape_color_texture() {
    static Texture2D texture = [] {
        // client/src/hooks/landscapeGenerateTexture.hpp recreates the game's generated
        // 0x400-square green landscape colour texture as RGBA (0x40, 0x54, 0x26, 0xff).
        // bc_grass01b is a building texture, and it caused the bright green patches in
        // the distant landscape.
        Image image = GenImageColor(0x400, 0x400, Color{0x40, 0x54, 0x26, 0xff});
        auto loaded = LoadTextureFromImage(image);
        UnloadImage(image);
        GenTextureMipmaps(&loaded);
        SetTextureWrap(loaded, TEXTURE_WRAP_REPEAT);
        SetTextureFilter(loaded, TEXTURE_FILTER_TRILINEAR);
        SetTextureFilter(loaded, TEXTURE_FILTER_ANISOTROPIC_16X);
        return loaded;
    }();
    return texture;
}

// The game's texture slot 0 ("concrete"), used by curbs, sidewalks, and old
// district pieces with face texture 0. No file holds it:
// Texture_GenerateProceduralNoise(0) @99730 fills 128x128 RGBA with hashed value
// noise n() (cosine-blended, about [-1,1]) added up at four frequencies:
// R = G = 168 + 2*sum, B = 167 + 1.75*sum, alpha 0x20. _settexture gives it a flat
// material map (0x60, 0xc0, 0).
// ponytail: our hash is not the game's, so the speckles don't match pixel for
// pixel; the colour, range, and frequencies do.
const Texture2D& game_slot0_texture(bool material) {
    static const std::array<Texture2D, 2> textures = [] {
        constexpr int size = 128;
        const auto hash = [](int x, int y) {
            auto h = static_cast<std::uint32_t>(x + y * 57);
            h = (h << 13U) ^ h;
            h = h * (h * h * 15731U + 789221U) + 1376312589U;
            return 1.0F - static_cast<float>(h & 0x7fffffffU) / 1073741824.0F;
        };
        const auto noise = [&](float x, float y) {
            const int ix = static_cast<int>(std::floor(x));
            const int iy = static_cast<int>(std::floor(y));
            const auto fx = (1.0F - std::cos((x - ix) * PI)) * 0.5F;
            const auto fy = (1.0F - std::cos((y - iy) * PI)) * 0.5F;
            const auto a = hash(ix & 127, iy & 127) + (hash((ix + 1) & 127, iy & 127) - hash(ix & 127, iy & 127)) * fx;
            const auto b = hash(ix & 127, (iy + 1) & 127) + (hash((ix + 1) & 127, (iy + 1) & 127) - hash(ix & 127, (iy + 1) & 127)) * fx;
            return a + (b - a) * fy;
        };
        Image color = GenImageColor(size, size, BLANK);
        auto* pixels = static_cast<Color*>(color.data);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const auto fx = static_cast<float>(x);
                const auto fy = static_cast<float>(y);
                const auto n1 = noise(fx * 0.5F, fy * 0.25F);
                const auto n2 = noise(fx * 0.25F, fy * 0.125F);
                const auto n3 = noise(fx, fy * 0.5F);
                const auto n4 = noise(fx * 2.0F + 1235.0F, fy + 123.0F);
                const auto grey = static_cast<unsigned char>(std::clamp(168.0F + 2.0F * (n1 + n2 + n3 + n4), 0.0F, 255.0F));
                const auto blue = static_cast<unsigned char>(std::clamp(167.0F + 1.75F * (n1 + n2 + n3 + n4), 0.0F, 255.0F));
                // Alpha stays opaque here: world.frag always writes alpha 1.
                pixels[y * size + x] = Color{grey, grey, blue, 0xff};
            }
        }
        auto color_texture = LoadTextureFromImage(color);
        UnloadImage(color);
        Image material = GenImageColor(4, 4, Color{0x60, 0xc0, 0x00, 0xff});
        auto material_texture = LoadTextureFromImage(material);
        UnloadImage(material);
        for (auto* t : {&color_texture, &material_texture}) {
            GenTextureMipmaps(t);
            SetTextureWrap(*t, TEXTURE_WRAP_REPEAT);
            SetTextureFilter(*t, TEXTURE_FILTER_TRILINEAR);
        }
        return std::array<Texture2D, 2>{color_texture, material_texture};
    }();
    return textures[material ? 1 : 0];
}

std::string_view game_surface_texture_name(std::string_view name) {
    if (name == "city_road_asphalt") return "asphalt2";
    if (name == "city_curb_concrete") return "concrete";
    if (name == "city_terrain_grass") return "bc_grass01b";
    return name;
}

const Texture2D* texture_for_material(const EditorProject& project, const SbbBuilding& building, std::uint32_t material_id) {
    if (material_id == 0x100U) {
        return nullptr;
    }
    const auto local_slot = static_cast<std::size_t>(material_id & 0xffU);
    if (local_slot < building.materials.size()) {
        return find_project_texture(project, building.materials[local_slot]);
    }
    return nullptr;
}

const Texture2D* material_texture_for_material(const EditorProject& project, const SbbBuilding& building, std::uint32_t material_id) {
    if (material_id == 0x100U) {
        return nullptr;
    }
    const auto local_slot = static_cast<std::size_t>(material_id & 0xffU);
    return local_slot < building.materials.size()
        ? find_project_material_texture(project, building.materials[local_slot])
        : nullptr;
}

const Texture2D* texture_for_buffer_material(const EditorProject& project, const BuildingRenderBuffer& buffer, std::uint32_t material_id) {
    if (material_id >= buffer.material_names.size()) {
        return nullptr;
    }
    if (buffer.material_names[material_id] == "city_terrain_grass") {
        if (const auto* detail = find_project_texture(project, "bc_grass01b")) return detail;
        return &generated_game_landscape_color_texture();
    }
    if (buffer.material_names[material_id] == "city_curb_concrete") return &game_slot0_texture(false);
    return find_project_texture(project, game_surface_texture_name(buffer.material_names[material_id]));
}

const Texture2D* material_texture_for_buffer_material(const EditorProject& project, const BuildingRenderBuffer& buffer, std::uint32_t material_id) {
    if (material_id >= buffer.material_names.size()) {
        return nullptr;
    }
    if (buffer.material_names[material_id] == "city_curb_concrete") return &game_slot0_texture(true);
    return find_project_material_texture(project, game_surface_texture_name(buffer.material_names[material_id]));
}

Texture2D default_game_material_texture();

void GpuMesh::unload() {
    if (ebo != 0U) glDeleteBuffers(1, &ebo);
    if (vbo != 0U) glDeleteBuffers(1, &vbo);
    if (vao != 0U) glDeleteVertexArrays(1, &vao);
    vao = vbo = ebo = 0U;
    vertex_count = index_count = 0U;
    parts.clear();
}

namespace {

// Vertex attribute locations every GpuMesh program shares. 3..6 are the columns
// of each instance's model matrix, read from the instance stream.
constexpr GLuint kAttribPosition = 0;
constexpr GLuint kAttribTexCoord = 1;
constexpr GLuint kAttribNormal = 2;
constexpr GLuint kAttribInstance = 3;

struct GpuVertex {
    float position[3];
    float uv[3];  // u, v, road-marking overlay layer
    float normal[3];
};

// Uploads interleaved vertices and 32-bit indices into a new VAO. Its per-instance
// attributes (divisor 1) are pointed at the instance stream for each draw.
GpuMesh upload_gpu_vertices(const std::vector<GpuVertex>& vertices, const std::vector<std::uint32_t>& indices) {
    GpuMesh mesh;
    if (vertices.empty() || indices.empty()) return mesh;
    rlDrawRenderBatchActive();
    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);
    glGenBuffers(1, &mesh.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(GpuVertex)), vertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(kAttribPosition);
    glVertexAttribPointer(kAttribPosition, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), reinterpret_cast<void*>(offsetof(GpuVertex, position)));
    glEnableVertexAttribArray(kAttribTexCoord);
    glVertexAttribPointer(kAttribTexCoord, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), reinterpret_cast<void*>(offsetof(GpuVertex, uv)));
    glEnableVertexAttribArray(kAttribNormal);
    glVertexAttribPointer(kAttribNormal, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), reinterpret_cast<void*>(offsetof(GpuVertex, normal)));
    for (GLuint column = 0; column < 4; ++column) {
        glEnableVertexAttribArray(kAttribInstance + column);
        glVertexAttribDivisor(kAttribInstance + column, 1);
    }
    glGenBuffers(1, &mesh.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)), indices.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    mesh.vertex_count = static_cast<std::uint32_t>(vertices.size());
    mesh.index_count = static_cast<std::uint32_t>(indices.size());
    return mesh;
}

// An IEEE half float from a float in [-1, 1]. Values too small for a half become
// a signed zero.
std::uint16_t half_from_unit_float(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const auto sign = static_cast<std::uint16_t>((bits >> 16U) & 0x8000U);
    const auto exponent = static_cast<std::int32_t>((bits >> 23U) & 0xffU) - 127 + 15;
    if (exponent <= 0) return sign;
    if (exponent >= 31) return static_cast<std::uint16_t>(sign | 0x7c00U);
    auto mantissa = (bits & 0x7fffffU) + 0x1000U;  // round to nearest
    auto half = static_cast<std::uint32_t>(exponent) << 10U;
    if ((mantissa & 0x800000U) != 0U) { mantissa = 0; half += 1U << 10U; }
    return static_cast<std::uint16_t>(sign | std::min<std::uint32_t>(half + (mantissa >> 13U), 0x7c00U));
}

std::vector<ShadowVertex> pack_shadow_vertices(const std::vector<float>& baked) {
    const auto count = baked.size() / 9U;
    std::vector<ShadowVertex> vertices(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto* v = &baked[i * 9U];  // position, kind, 0, 0, normal
        auto& out = vertices[i];
        std::copy(v, v + 3, out.position);
        const auto length = std::sqrt(v[6] * v[6] + v[7] * v[7] + v[8] * v[8]);
        const auto scale = length > 0.0F ? 1.0F / length : 0.0F;
        for (std::size_t c = 0; c < 3; ++c) out.normal[c] = half_from_unit_float(v[6 + c] * scale);
        out.kind = static_cast<std::uint8_t>(v[3]);
        out.pad = 0;
    }
    return vertices;
}

GpuMesh upload_shadow_volume(const std::vector<ShadowVertex>& vertices, const std::vector<std::uint32_t>& indices) {
    GpuMesh mesh;
    const auto count = vertices.size();
    if (count == 0U || indices.empty()) return mesh;
    rlDrawRenderBatchActive();
    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);
    glGenBuffers(1, &mesh.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(ShadowVertex)), vertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(kAttribPosition);
    glVertexAttribPointer(kAttribPosition, 3, GL_FLOAT, GL_FALSE, sizeof(ShadowVertex), reinterpret_cast<void*>(offsetof(ShadowVertex, position)));
    // The kind arrives as vertexTexCoord.x, and the normal as vertexNormal.
    glEnableVertexAttribArray(kAttribTexCoord);
    glVertexAttribPointer(kAttribTexCoord, 1, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(ShadowVertex), reinterpret_cast<void*>(offsetof(ShadowVertex, kind)));
    glEnableVertexAttribArray(kAttribNormal);
    glVertexAttribPointer(kAttribNormal, 3, GL_HALF_FLOAT, GL_FALSE, sizeof(ShadowVertex), reinterpret_cast<void*>(offsetof(ShadowVertex, normal)));
    for (GLuint column = 0; column < 4; ++column) {
        glEnableVertexAttribArray(kAttribInstance + column);
        glVertexAttribDivisor(kAttribInstance + column, 1);
    }
    glGenBuffers(1, &mesh.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)), indices.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    mesh.vertex_count = static_cast<std::uint32_t>(count);
    mesh.index_count = static_cast<std::uint32_t>(indices.size());
    return mesh;
}

} // namespace

// One VAO for the whole buffer, one part per material run. Normals are smoothed
// over shared vertices; flat-shaded surfaces work theirs out in the shader instead.
// flat_faces makes three separate vertices per triangle with the face normal, for
// surfaces lit by the vertex normal that need to keep hard edges.
GpuMesh upload_gpu_mesh(const BuildingRenderBuffer& buffer, const std::function<void(GpuMeshPart&)>& describe_part, bool flat_faces = false) {
    std::vector<GpuVertex> vertices(flat_faces ? 0U : buffer.vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const auto& v = buffer.vertices[i];
        vertices[i] = {{v.position.x, v.position.y, v.position.z}, {v.u, v.v, v.layer}, {0.0F, 0.0F, 0.0F}};
    }
    std::vector<std::uint32_t> indices;
    indices.reserve(buffer.triangles.size() * 3U);
    std::vector<GpuMeshPart> parts;
    for (const auto& run : buffer.material_runs) {
        const auto first = static_cast<std::size_t>(run.first_triangle);
        if (first > buffer.triangles.size() || run.triangle_count > buffer.triangles.size() - first) {
            throw std::runtime_error("GPU mesh material run exceeds its triangle buffer");
        }
        const auto end = first + run.triangle_count;
        GpuMeshPart part;
        part.first_index = static_cast<std::uint32_t>(indices.size());
        part.material_id = run.material;
        part.window_material = run.city_window_surface;
        for (std::size_t t = run.first_triangle; t < end; ++t) {
            const auto& tri = buffer.triangles[t];
            if (tri.indices[0] >= buffer.vertices.size() || tri.indices[1] >= buffer.vertices.size() || tri.indices[2] >= buffer.vertices.size()) {
                throw std::runtime_error("GPU mesh triangle " + std::to_string(t) + " exceeds its vertex buffer");
            }
            const auto& a = buffer.vertices[tri.indices[0]].position;
            const auto& b = buffer.vertices[tri.indices[1]].position;
            const auto& c = buffer.vertices[tri.indices[2]].position;
            const auto n = Vector3CrossProduct({b.x - a.x, b.y - a.y, b.z - a.z}, {c.x - a.x, c.y - a.y, c.z - a.z});
            if (flat_faces) {
                for (const auto index : tri.indices) {
                    const auto& v = buffer.vertices[index];
                    indices.push_back(static_cast<std::uint32_t>(vertices.size()));
                    vertices.push_back({{v.position.x, v.position.y, v.position.z}, {v.u, v.v, v.layer}, {n.x, n.y, n.z}});
                }
                continue;
            }
            for (const auto index : tri.indices) {
                auto& out = vertices[index].normal;
                out[0] += n.x;
                out[1] += n.y;
                out[2] += n.z;
                indices.push_back(index);
            }
        }
        part.index_count = static_cast<std::uint32_t>(indices.size()) - part.first_index;
        if (part.index_count == 0U) continue;
        describe_part(part);
        parts.push_back(part);
    }
    for (auto& v : vertices) {
        const auto n = Vector3{v.normal[0], v.normal[1], v.normal[2]};
        const auto unit = Vector3LengthSqr(n) > 1e-12F ? Vector3Normalize(n) : Vector3{0.0F, 1.0F, 0.0F};
        v.normal[0] = unit.x;
        v.normal[1] = unit.y;
        v.normal[2] = unit.z;
    }
    auto mesh = upload_gpu_vertices(vertices, indices);
    mesh.parts = std::move(parts);
    return mesh;
}

// Used by building, portal, and city models: the part's textures come from the
// resolver, and its fallback colour from the game's material palette.
void describe_textured_part(GpuMeshPart& part, const Texture2D* texture, const Texture2D* material_texture) {
    part.material_texture = material_texture != nullptr && material_texture->id != 0U
        ? material_texture->id
        : default_game_material_texture().id;
    if (texture != nullptr && texture->id != 0U) {
        part.texture = texture->id;
        part.color = WHITE;
    } else {
        part.color = native_material_color(part.material_id, false, part.window_material);
    }
}

GpuBuildingRenderModel upload_city_buffer_model(
    const BuildingRenderBuffer& buffer,
    const EditorProject& project,
    bool flat_faces,
    const std::function<Color(std::uint32_t)>& color_for_material) {
    GpuBuildingRenderModel model;
    // No wire_lines: city surfaces never draw wires.
    model.mesh = upload_gpu_mesh(buffer, [&](GpuMeshPart& part) {
        describe_textured_part(part,
            texture_for_buffer_material(project, buffer, part.material_id),
            material_texture_for_buffer_material(project, buffer, part.material_id));
        // City surfaces tint their texture with the material colour (the terrain
        // uses it as the generated landscape colour map).
        part.color = color_for_material(part.material_id);
    }, flat_faces);
    model.uploaded_bytes = model.mesh.vertex_count * sizeof(GpuVertex) + model.mesh.index_count * sizeof(std::uint32_t);
    return model;
}

// chunked: for large world-space volumes (roads, pieces). Their triangles are
// grouped into 256-unit ground chunks, one part each with its own bounds, so draws
// can skip the chunks that are out of view.
void build_shadow_volume(GpuBuildingRenderModel& model, const BuildingRenderBuffer& buffer, bool chunked = false) {
    // Workers bake the volume together with the buffer (bake_shadow_volume). Small
    // buffers built on this thread are baked here.
    const BuildingRenderBuffer* baked = &buffer;
    BuildingRenderBuffer local;
    if (buffer.shadow_indices.empty() && !buffer.triangles.empty()) {
        local.vertices = buffer.vertices;
        local.triangles = buffer.triangles;
        bake_shadow_volume(local);
        baked = &local;
    }
    model.shadow_top = baked->shadow_top;
    if (!chunked) {
        model.shadow_volume = upload_shadow_volume(pack_shadow_vertices(baked->shadow_vertices), baked->shadow_indices);
        if (!model.shadow_volume.empty()) model.shadow_volume.parts.push_back({0U, model.shadow_volume.index_count});
        return;
    }
    const auto& positions = baked->shadow_vertices;  // 9 floats per vertex, position first
    const auto& source = baked->shadow_indices;
    const auto position = [&](std::uint32_t index) {
        return Vector3{positions[index * 9U], positions[index * 9U + 1U], positions[index * 9U + 2U]};
    };
    struct Triangle { std::uint64_t chunk; std::uint32_t first; };
    std::vector<Triangle> triangles;
    triangles.reserve(source.size() / 3U);
    for (std::uint32_t t = 0; t + 2U < source.size(); t += 3U) {
        const auto a = position(source[t]), b = position(source[t + 1U]), c = position(source[t + 2U]);
        const auto cx = static_cast<std::int32_t>(std::floor((a.x + b.x + c.x) / (3.0F * 256.0F)));
        const auto cz = static_cast<std::int32_t>(std::floor((a.z + b.z + c.z) / (3.0F * 256.0F)));
        triangles.push_back({static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32U | static_cast<std::uint32_t>(cz), t});
    }
    std::ranges::stable_sort(triangles, {}, &Triangle::chunk);
    std::vector<std::uint32_t> indices;
    indices.reserve(source.size());
    std::vector<GpuMeshPart> parts;
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        if (i == 0 || triangles[i].chunk != triangles[i - 1].chunk) {
            GpuMeshPart part{static_cast<std::uint32_t>(indices.size()), 0U};
            part.has_bounds = true;
            part.bounds_min = {FLT_MAX, FLT_MAX, FLT_MAX};
            part.bounds_max = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
            parts.push_back(part);
        }
        auto& part = parts.back();
        for (std::uint32_t k = 0; k < 3U; ++k) {
            const auto index = source[triangles[i].first + k];
            indices.push_back(index);
            part.bounds_min = Vector3Min(part.bounds_min, position(index));
            part.bounds_max = Vector3Max(part.bounds_max, position(index));
        }
        part.index_count += 3U;
    }
    model.shadow_volume = upload_shadow_volume(pack_shadow_vertices(positions), indices);
    if (!model.shadow_volume.empty()) model.shadow_volume.parts = std::move(parts);
}

namespace {
// The view frustum and stretch direction, for culling chunked shadow volume parts.
struct ShadowCull {
    std::array<std::array<float, 4>, 6> planes{};
    Vector3 extrusion{};
};
std::optional<ShadowCull> g_shadow_cull;

// Whether the box, stretched along the shadow direction, can reach the view.
bool shadow_part_visible(const ShadowCull& cull, const GpuMeshPart& part) {
    const auto lo = Vector3Min(part.bounds_min, Vector3Add(part.bounds_min, cull.extrusion));
    const auto hi = Vector3Max(part.bounds_max, Vector3Add(part.bounds_max, cull.extrusion));
    for (const auto& p : cull.planes) {
        // The box corner furthest along the plane's normal.
        const Vector3 corner{p[0] >= 0.0F ? hi.x : lo.x, p[1] >= 0.0F ? hi.y : lo.y, p[2] >= 0.0F ? hi.z : lo.z};
        if (p[0] * corner.x + p[1] * corner.y + p[2] * corner.z + p[3] < 0.0F) return false;
    }
    return true;
}
} // namespace

void render_imgui_draw_data(ImDrawData* draw_data) {
    if (draw_data->Textures != nullptr) {
        for (ImTextureData* texture : *draw_data->Textures) {
            if (texture->Status != ImTextureStatus_OK) ::ImGui_ImplRaylib_UpdateTexture(texture);
        }
    }
    const int fb_width = static_cast<int>(draw_data->DisplaySize.x * draw_data->FramebufferScale.x);
    const int fb_height = static_cast<int>(draw_data->DisplaySize.y * draw_data->FramebufferScale.y);
    if (fb_width <= 0 || fb_height <= 0) return;
    rlDrawRenderBatchActive();

    static Shader shader{};
    static int projection_loc = -1;
    static GLuint vao = 0, vbo = 0, ibo = 0;
    if (vao == 0) {
        shader = LoadShaderFromMemory(PAFTA_GLSL_VERSION R"glsl(
layout(location=0) in vec2 position;
layout(location=1) in vec2 uv;
layout(location=2) in vec4 color;
uniform mat4 projection;
out vec2 frag_uv;
out vec4 frag_color;
void main() {
    frag_uv = uv;
    frag_color = color;
    gl_Position = projection * vec4(position, 0.0, 1.0);
}
)glsl", PAFTA_GLSL_VERSION R"glsl(
in vec2 frag_uv;
in vec4 frag_color;
uniform sampler2D texture0;
out vec4 out_color;
void main() { out_color = frag_color * texture(texture0, frag_uv); }
)glsl");
        projection_loc = GetShaderLocation(shader, "projection");
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ibo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), reinterpret_cast<void*>(offsetof(ImDrawVert, pos)));
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), reinterpret_cast<void*>(offsetof(ImDrawVert, uv)));
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert), reinterpret_cast<void*>(offsetof(ImDrawVert, col)));
        glBindVertexArray(0);
    }

    const GLboolean blend = glIsEnabled(GL_BLEND), cull = glIsEnabled(GL_CULL_FACE), depth = glIsEnabled(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    glViewport(0, 0, fb_width, fb_height);
    const float l = draw_data->DisplayPos.x, r = l + draw_data->DisplaySize.x;
    const float t = draw_data->DisplayPos.y, b = t + draw_data->DisplaySize.y;
    const float projection[16] = {
        2.0F / (r - l), 0.0F, 0.0F, 0.0F,
        0.0F, 2.0F / (t - b), 0.0F, 0.0F,
        0.0F, 0.0F, -1.0F, 0.0F,
        (r + l) / (l - r), (t + b) / (b - t), 0.0F, 1.0F,
    };
    glUseProgram(shader.id);
    glUniformMatrix4fv(projection_loc, 1, GL_FALSE, projection);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    const auto scale = draw_data->FramebufferScale;
    for (const ImDrawList* list : draw_data->CmdLists) {
        // New storage for each list: the GPU may still be reading the old contents,
        // and writing over them would make the driver wait.
        glBufferData(GL_ARRAY_BUFFER, list->VtxBuffer.Size * static_cast<GLsizeiptr>(sizeof(ImDrawVert)), list->VtxBuffer.Data, GL_STREAM_DRAW);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, list->IdxBuffer.Size * static_cast<GLsizeiptr>(sizeof(ImDrawIdx)), list->IdxBuffer.Data, GL_STREAM_DRAW);
        for (const auto& cmd : list->CmdBuffer) {
            const float x0 = (cmd.ClipRect.x - l) * scale.x, y0 = (cmd.ClipRect.y - t) * scale.y;
            const float x1 = (cmd.ClipRect.z - l) * scale.x, y1 = (cmd.ClipRect.w - t) * scale.y;
            if (x1 <= x0 || y1 <= y0 || cmd.ElemCount == 0) continue;
            glScissor(static_cast<GLint>(x0), static_cast<GLint>(static_cast<float>(fb_height) - y1),
                static_cast<GLsizei>(x1 - x0), static_cast<GLsizei>(y1 - y0));
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(cmd.GetTexID()));
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cmd.ElemCount), sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                reinterpret_cast<void*>(static_cast<std::size_t>(cmd.IdxOffset) * sizeof(ImDrawIdx)));
        }
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
    if (!blend) glDisable(GL_BLEND);
    if (cull) glEnable(GL_CULL_FACE);
    if (depth) glEnable(GL_DEPTH_TEST);
}

void apply_texture_anisotropy(EditorProject& project, int level) {
    rlDrawRenderBatchActive();
    for (auto* textures : {&project.textures, &project.material_textures}) {
        for (const auto& loaded : *textures) rlTextureParameters(loaded.texture.id, RL_TEXTURE_FILTER_ANISOTROPIC, level);
    }
}

void release_free_memory() {
#if defined(__APPLE__)
    malloc_zone_pressure_relief(nullptr, 0);
#elif defined(__GLIBC__)
    malloc_trim(0);
#endif
}

GpuBuildingRenderModel upload_building_render_model(const BuildingRenderBuffer& buffer, const SbbBuilding& building, const EditorProject& project) {
    GpuBuildingRenderModel model;
    // No wire_lines: building models are never drawn with wires.
    build_shadow_volume(model, buffer);
    model.mesh = upload_gpu_mesh(buffer, [&](GpuMeshPart& part) {
        describe_textured_part(part,
            texture_for_material(project, building, part.material_id),
            material_texture_for_material(project, building, part.material_id));
    });
    model.uploaded_bytes = model.mesh.vertex_count * sizeof(GpuVertex) + model.mesh.index_count * sizeof(std::uint32_t);
    return model;
}

Vector3 transform_buffer_vertex(Vec3 value, Vector3 origin, std::int32_t rotation) {
    return city_transform(origin, {value.x, value.y, value.z}, rotation);
}

Matrix building_transform_matrix(Vector3 origin, std::int32_t rotation) {
    const auto basis = native_city_building_rotation_basis(rotation);
    return Matrix{
        basis.x_axis.x, basis.y_axis.x, basis.z_axis.x, origin.x,
        basis.x_axis.y, basis.y_axis.y, basis.z_axis.y, origin.y,
        basis.x_axis.z, basis.y_axis.z, basis.z_axis.z, origin.z,
        0.0F, 0.0F, 0.0F, 1.0F,
    };
}

Matrix city_placement_matrix(const SbcBuilding& placement) {
    const auto origin = native_city_building_render_origin(placement, 4.0F);
    return building_transform_matrix({origin.x, origin.y, origin.z}, placement.rotation);
}

// The vertex stage every GpuMesh program shares: each instance's model matrix in
// attributes 3..6, and one view-projection uniform per draw.
#define PAFTA_INSTANCED_VS PAFTA_GLSL_VERSION R"glsl(
layout(location=0) in vec3 vertexPosition;
layout(location=1) in vec3 vertexTexCoord;
layout(location=2) in vec3 vertexNormal;
layout(location=3) in mat4 instanceTransform;
uniform mat4 viewProjection;
// Shadow volume caps reuse the render triangles; invariant keeps their depth
// exactly the same as the surface they lie on.
invariant gl_Position;
out vec3 fragTexCoord;
out vec3 fragNormal;
out vec3 fragPosition;
void main() {
    vec4 world = instanceTransform * vec4(vertexPosition, 1.0);
    fragTexCoord = vertexTexCoord;
    // Placements only turn in quarter turns, so the top-left 3x3 is orthonormal.
    fragNormal = mat3(instanceTransform) * vertexNormal;
    fragPosition = world.xyz;
    gl_Position = viewProjection * world;
}
)glsl"

// A flat mask shader, used by the screen-space outline pass and the stencil shadow
// volumes. It draws into an offscreen alpha mask (or with colour writes off).
struct MaskShader {
    Shader shader{};
    int view_projection_loc{-1};
    bool loaded{false};
};

MaskShader& mask_shader() {
    static MaskShader cache;
    if (!cache.loaded) {
        const char* fs =
            PAFTA_GLSL_VERSION
            "out vec4 finalColor;\n"
            "void main() { finalColor = vec4(1.0); }\n";
        cache.shader = LoadShaderFromMemory(PAFTA_INSTANCED_VS, fs);
        cache.view_projection_loc = GetShaderLocation(cache.shader, "viewProjection");
        cache.loaded = true;
    }
    return cache;
}

// Stencil shadow volumes (see build_shadow_volume). uv.x is the kind of vertex:
// 0 edge end, 1 far end of an open edge, 2 front cap, 3 back cap. The normal is the
// face's own (pointing inward, as faces are wound clockwise). A face is stretched
// when it is turned away from the light, as RenderQueuedShadowVolumeAdjacencyPass
// does.
struct ShadowVolumeShader {
    Shader shader{};
    int view_projection_loc{-1};
    int light_direction_loc{-1};
    int extrusion_loc{-1};
    bool loaded{false};
};

ShadowVolumeShader& shadow_volume_shader() {
    static ShadowVolumeShader cache;
    if (!cache.loaded) {
        const char* vs = PAFTA_GLSL_VERSION R"glsl(
layout(location=0) in vec3 vertexPosition;
layout(location=1) in vec3 vertexTexCoord;
layout(location=2) in vec3 vertexNormal;
layout(location=3) in mat4 instanceTransform;
uniform mat4 viewProjection;
uniform vec3 lightDirection;
uniform vec3 extrusion;
invariant gl_Position;
void main() {
    int kind = int(vertexTexCoord.x + 0.5);
    bool lit = dot(mat3(instanceTransform) * vertexNormal, lightDirection) < 0.0;
    // Caps of lit faces shrink to a point that gets clipped.
    if (kind >= 2 && lit) {
        gl_Position = vec4(0.0);
        return;
    }
    vec4 world = instanceTransform * vec4(vertexPosition, 1.0);
    if (kind == 1 || kind == 3 || (kind == 0 && lit)) world.xyz += extrusion;
    gl_Position = viewProjection * world;
}
)glsl";
        const char* fs =
            PAFTA_GLSL_VERSION
            "out vec4 finalColor;\n"
            "void main() { finalColor = vec4(1.0); }\n";
        cache.shader = LoadShaderFromMemory(vs, fs);
        cache.view_projection_loc = GetShaderLocation(cache.shader, "viewProjection");
        cache.light_direction_loc = GetShaderLocation(cache.shader, "lightDirection");
        cache.extrusion_loc = GetShaderLocation(cache.shader, "extrusion");
        cache.loaded = true;
    }
    return cache;
}

// Sets the sun direction and the stretch vector for the ShadowVolume draws that follow.
void set_shadow_volume_light(Vector3 light, Vector3 extrusion) {
    const auto& shader = shadow_volume_shader();
    glUseProgram(shader.shader.id);
    glUniform3f(shader.light_direction_loc, light.x, light.y, light.z);
    glUniform3f(shader.extrusion_loc, extrusion.x, extrusion.y, extrusion.z);
}

// Sub Rosa's world shader reads the colour and material maps, lights them with its
// world light, and uses the colour's alpha as window shine for sky reflections. The
// editor does the same, and uses the game's atmosphere for its background.
struct LitShader {
    Shader shader{};
    int view_projection_loc{-1};
    int color_loc{-1};
    int view_position_loc{-1};
    int window_surface_loc{-1};
    int glass_alpha_loc{-1};
    int model_surface_loc{-1};
    int unlit_surface_loc{-1};
    int building_surface_loc{-1};
    int terrain_surface_loc{-1};
    int light_direction_loc{-1};
    int light_ambient_loc{-1};
    int light_diffuse_loc{-1};
    int cubemap_pass_loc{-1};
    bool loaded{false};
};

struct GameLightColors {
    Vector3 ambient{};
    Vector3 diffuse{};
};

// Port of subrosa.exe WorldShading @0xa61f0. The optical depth through a sphere of
// radius 512 seen from radius 496 (constants @0x278aec/@0x278ae8/@0x278958), times
// 0.029296875 (@0x278174). The fixed axis samples have an extra factor of
// sqrt(0.09765625) (@0x2781e8). The wavelengths @0x2b23d0 are (0.5, 0.625, 1.0).
GameLightColors game_light_colors(Vector3 light) {
    const auto depth = [](float dy) {
        return std::sqrt(262144.0F - (246016.0F - dy*dy*246016.0F)) - dy*496.0F;
    };
    constexpr float wavelength[3] = {0.5F, 0.625F, 1.0F};
    const float light_depth = depth(light.y)*0.029296875F;
    const float horizon_depth = depth(0.0F)*0.3125F*0.029296875F;
    const float zenith_depth = depth(1.0F)*0.3125F*0.029296875F;
    const float rise = 1.0F - light.y;
    const float height = 1.0F - rise*rise*rise*rise;
    GameLightColors colors;
    float* ambient = &colors.ambient.x;
    float* diffuse = &colors.diffuse.x;
    for (int i = 0; i < 3; ++i) {
        ambient[i] = (4.0F*std::exp(-wavelength[i]*horizon_depth)*0.5F +
                      std::exp(-wavelength[i]*zenith_depth)*0.75F)*height*0.75F;
        diffuse[i] = std::exp(-wavelength[i]*light_depth)*1.125F;
    }
    return colors;
}


LitShader& lit_shader() {
    static LitShader cache;
    if (!cache.loaded) {
        const char* vs = PAFTA_INSTANCED_VS;
        const char* fs = PAFTA_GLSL_VERSION R"glsl(
in vec3 fragTexCoord;
in vec3 fragNormal;
in vec3 fragPosition;
uniform sampler2D texture0;
uniform sampler2D texture1;
// Road-marking overlay arrays (tex_atlas_color / tex_atlas_material). Layer 0 is
// empty, so every surface can sample them and only marked road faces change.
uniform highp sampler2DArray overlayColor;
uniform highp sampler2DArray overlayMaterial;
uniform vec4 colDiffuse;
uniform vec3 viewPosition;
uniform int windowSurface;
// > 0: this window part is room glass for the near view (the game's glass shader).
uniform float glassAlpha;
// GenerateMapTextureFromVisibleSectors draws sectors with the image shader:
// only the base texture, with no lighting.
uniform int unlitSurface;
// model.frag surfaces (street objects): the texture's alpha is the shine. 1 draws
// the texel unlit, below 0.5 is a non-metal with roughness 0.5, else 0.5/0.125.
uniform int modelSurface;
uniform int buildingSurface;
uniform int terrainSurface;
uniform vec3 lightDirection;
// subrosa.exe WorldShading @0xa61f0 works these out from the sun's height every
// frame. The world shader only reads lightdiffuse; the landscape shader reads both.
uniform vec3 lightAmbient;
uniform vec3 lightDiffuse;
// The blurred landscape cubemap (RenderLandscapeCubemapAndBlurCapture @93e60),
// sampled with true world directions. cubemapPass is set while capturing it; the
// landscape then uses the cubemap colour map, grey over the city block.
uniform samplerCube environment;
uniform int cubemapPass;
layout(location=0) out vec4 finalColor;
layout(location=1) out vec4 packedDepth;
layout(location=2) out vec4 directLight;
const float PI = 3.1415926;
vec3 envDiffuse(vec3 n) {
    return textureLod(environment, n, 8.0).rgb;
}
vec3 toLinear(vec3 color) {
    bvec3 cutoff = lessThan(color, vec3(0.04045));
    return mix(pow((color + vec3(0.055))/vec3(1.055), vec3(2.4)), color/vec3(12.92), cutoff);
}
vec3 fresnelFactor(vec3 f0, float product) {
    return mix(f0, vec3(1.0), pow(1.01-product, 5.0));
}
float distributionGgx(float roughness, float ndh) {
    float m = roughness*roughness;
    float m2 = m*m;
    float d = (ndh*m2-ndh)*ndh+1.0;
    return m2/(PI*d*d);
}
float geometrySchlick(float roughness, float ndv, float ndl) {
    float k = roughness*roughness*0.5;
    return 0.25/((ndv*(1.0-k)+k)*(ndl*(1.0-k)+k));
}
void main() {
    // The game uses mipmapped linear minification. Add a small bias that grows with
    // distance, so tiny far-away wall and window texels settle on one mip level instead
    // of flickering between bright and dark while the camera moves. Close surfaces keep
    // their full-resolution texture.
    float textureDistance = length(viewPosition-fragPosition);
    float mipBias = smoothstep(160.0, 900.0, textureDistance)*0.65;
    // landscape.vert: tex_detail at (x/32, 1-z/32) in world units.
    vec2 surfaceUv = terrainSurface == 1 ? vec2(fragPosition.x/32.0, 1.0-fragPosition.z/32.0) : fragTexCoord.xy;
    vec4 textureSample = texture(texture0, surfaceUv, mipBias);
    vec4 sampled = textureSample * colDiffuse;
    if (unlitSurface == 1) {
        finalColor = vec4(sampled.rgb, 1.0);
        directLight = vec4(0.0);
        packedDepth = vec4(1.0);
        return;
    }
    // world.frag: the overlay blends by its alpha over the sRGB colour and over the
    // linear material (the overlay's own material is not made linear).
    vec4 overlay = texture(overlayColor, fragTexCoord, mipBias);
    sampled.rgb = mix(sampled.rgb, overlay.rgb, overlay.a);
    vec3 albedo = toLinear(sampled.rgb);
    // The game's world.frag also turns the material texture from sRGB to linear
    // before reading its metallic and roughness channels.
    vec3 material = toLinear(texture(texture1, fragTexCoord.xy, mipBias).rgb);
    material = mix(material, texture(overlayMaterial, fragTexCoord, mipBias).rgb, overlay.a);
    float metallic = clamp(material.r, 0.0, 1.0);
    float roughness = clamp(material.g, 0.0, 1.0);
    if (modelSurface == 1) {
        metallic = textureSample.a < 0.5 ? 0.0 : 0.5;
        roughness = textureSample.a < 0.5 ? 0.5 : 0.125;
    }
    // The derivative normal already faces the viewer. Vertex normals point inward
    // (clockwise winding), so a back-facing triangle that wasn't culled flips them.
    vec3 n = buildingSurface == 1
        ? normalize(cross(dFdx(fragPosition), dFdy(fragPosition)))
        : normalize(gl_FrontFacing ? fragNormal : -fragNormal);
    vec3 v = normalize(viewPosition-fragPosition);
    vec3 color;
    vec3 direct;
    if (terrainSurface == 1) {
        // landscape.frag: wrapped diffuse with rho=3, ambient light based on the sun's
        // height, and a view shine driven by the detail texture. texture0 is tex_detail,
        // and colDiffuse stands in for the generated tex_color landscape map.
        // tex_color is a flat (0x40,0x54,0x26); the cubemap copy is grey (0x54) over
        // texels 0x100..0x1bf x 0x240..0x2ff, world x and z 0..3072.
        vec3 landColor = colDiffuse.rgb;
        if (cubemapPass == 1 && fragPosition.x >= 0.0 && fragPosition.x < 3072.0 &&
            fragPosition.z >= 0.0 && fragPosition.z < 3072.0) landColor = vec3(84.0/255.0);
        vec3 base = toLinear(textureSample.rgb)*toLinear(landColor);
        float ndl0 = dot(n, lightDirection);
        float scale = 0.25+max(ndl0, 0.0)*0.75;
        scale = (4.0/3.0)*(1.0-1.0/(1.0+3.0*scale));
        vec3 ambientPart = base*lightAmbient*(0.375+ndl0*0.125);
        direct = base*lightDiffuse*scale*0.625;
        float shine = 0.0625+(1.0-textureSample.r)*0.5;
        float rim = 1.0-abs(dot(v, n));
        rim *= rim;
        rim *= rim;
        shine = (shine+(1.0-shine)*rim)*0.25;
        vec3 rv = normalize(reflect(lightDirection, n));
        direct += pow(max(dot(rv, v), 0.0), 2.0)*shine*lightDiffuse;
        color = ambientPart+direct;
    } else {
        vec3 h = normalize(v+lightDirection);
        float ndl = max(0.001, dot(n, lightDirection));
        float ndv = max(0.001, dot(n, v));
        float ndh = max(0.001, dot(n, h));
        float hdv = max(0.001, dot(h, v));
        vec3 f = fresnelFactor(mix(vec3(0.02), albedo, metallic), hdv);
        vec3 specular = f * geometrySchlick(roughness+0.0625, ndv, ndl) * distributionGgx(roughness+0.0625, ndh) * ndl;
        float wrap = 1.0-pow(1.0-ndl, 1.0+roughness*3.0);
        vec3 diffuse = (vec3(1.0)-f)*wrap*(1.0-roughness*0.375);
        specular *= lightDiffuse;
        diffuse *= lightDiffuse;
        direct = diffuse*1.5*mix(albedo, vec3(0.0), metallic)+specular;
        // Diffuse environment light: one cubemap texel per face (lod 8).
        diffuse += envDiffuse(n)*0.75;
        diffuse *= 1.5;
        // Specular environment light: lod roughness^2*8.
        vec3 r = reflect(-v, n);
        vec3 reflected = textureLod(environment, r, roughness*roughness*8.0).rgb;
        specular += reflected*0.75*fresnelFactor(mix(vec3(0.02), albedo, metallic), ndv)*(1.0-roughness*0.5);
        color = diffuse*mix(albedo, vec3(0.0), metallic)+specular;
        if (windowSurface == 1) {
            // array_world.frag window shine: a sharp cubemap reflection that fades
            // toward half the ambient blue at grazing angles.
            float scale = 1.0-abs(dot(-v, n));
            scale = 0.5+pow(scale, 2.5)*0.5;
            vec3 reflection = texture(environment, r).rgb*scale+vec3((1.0-scale)*0.5*lightAmbient.b);
            color = mix(color, reflection, textureSample.a);
            direct *= 1.0-textureSample.a;
        }
    }
    if (modelSurface == 1 && textureSample.a >= 1.0) {
        color = albedo;
        direct = vec3(0.0);
    }
    float alpha = windowSurface == 1 || modelSurface == 1 ? 1.0 : sampled.a;
    if (windowSurface == 1 && glassAlpha > 0.0) {
        // subrosa.exe room-glass shader (vertex @0x2f9820, fragment @0x2f9a10), with
        // premultiplied alpha, drawn with glBlendFunc(ONE, ONE_MINUS_SRC_ALPHA). The pane
        // faces out of its cell, so the front face is the outside.
        float scale = clamp((textureDistance-32.0)*0.125*0.125*0.75, 0.0, 1.0);
        float scale2 = min(0.5+pow(1.0-abs(dot(v, n)), 3.0)*0.5, 1.0);
        scale *= 1.0-scale2;
        color = gl_FrontFacing ? textureLod(environment, reflect(-v, n), 0.0).rgb : vec3(0.0625*0.5);
        color = color*scale2+vec3(scale*lightAmbient.b*0.5);
        alpha = scale2+scale;
        if (!gl_FrontFacing) alpha *= 0.25;
        direct = vec3(0.0);
    }
    finalColor = vec4(color, alpha);
    directLight = vec4(direct, alpha);
    float fragmentDepth = length(viewPosition-fragPosition);
    vec3 encodedDepth = fract(vec3(fragmentDepth*0.0000152587890625, fragmentDepth*0.00390625, fragmentDepth));
    packedDepth = vec4(floor(encodedDepth*256.0)/255.0, 1.0);
}
)glsl";
        cache.shader = LoadShaderFromMemory(vs, fs);
        cache.view_projection_loc = GetShaderLocation(cache.shader, "viewProjection");
        cache.color_loc = GetShaderLocation(cache.shader, "colDiffuse");
        const int diffuse_unit = 0;
        const int material_unit = 1;
        SetShaderValue(cache.shader, GetShaderLocation(cache.shader, "texture0"), &diffuse_unit, SHADER_UNIFORM_INT);
        SetShaderValue(cache.shader, GetShaderLocation(cache.shader, "texture1"), &material_unit, SHADER_UNIFORM_INT);
        const int overlay_color_unit = 2;
        const int overlay_material_unit = 3;
        SetShaderValue(cache.shader, GetShaderLocation(cache.shader, "overlayColor"), &overlay_color_unit, SHADER_UNIFORM_INT);
        SetShaderValue(cache.shader, GetShaderLocation(cache.shader, "overlayMaterial"), &overlay_material_unit, SHADER_UNIFORM_INT);
        cache.view_position_loc = GetShaderLocation(cache.shader, "viewPosition");
        cache.window_surface_loc = GetShaderLocation(cache.shader, "windowSurface");
        cache.glass_alpha_loc = GetShaderLocation(cache.shader, "glassAlpha");
        cache.model_surface_loc = GetShaderLocation(cache.shader, "modelSurface");
        cache.unlit_surface_loc = GetShaderLocation(cache.shader, "unlitSurface");
        cache.building_surface_loc = GetShaderLocation(cache.shader, "buildingSurface");
        cache.terrain_surface_loc = GetShaderLocation(cache.shader, "terrainSurface");
        cache.light_direction_loc = GetShaderLocation(cache.shader, "lightDirection");
        cache.light_ambient_loc = GetShaderLocation(cache.shader, "lightAmbient");
        cache.light_diffuse_loc = GetShaderLocation(cache.shader, "lightDiffuse");
        cache.cubemap_pass_loc = GetShaderLocation(cache.shader, "cubemapPass");
        const int environment_unit = 4;
        SetShaderValue(cache.shader, GetShaderLocation(cache.shader, "environment"), &environment_unit, SHADER_UNIFORM_INT);
        cache.loaded = true;
    }
    return cache;
}

Texture2D default_game_material_texture() {
    static Texture2D texture = [] {
        // world.frag decodes the material map from sRGB. 188 gives a linear roughness of
        // about 0.5. Zero red keeps fallback surfaces non-metal, instead of making every
        // missing material a bit metallic.
        Image image = GenImageColor(1, 1, Color{0, 188, 0, 255});
        auto loaded = LoadTextureFromImage(image);
        UnloadImage(image);
        SetTextureWrap(loaded, TEXTURE_WRAP_REPEAT);
        SetTextureFilter(loaded, TEXTURE_FILTER_BILINEAR);
        return loaded;
    }();
    return texture;
}

// Light colours at `scale` (the cubemap capture draws the landscape at 0.5).
void set_game_surface_light(float scale) {
    auto& shader = lit_shader();
    const auto light = Vector3Normalize(game_light_direction());
    const auto colors = game_light_colors(light);
    const auto ambient = Vector3Scale(colors.ambient, scale);
    const auto diffuse = Vector3Scale(colors.diffuse, scale);
    SetShaderValue(shader.shader, shader.light_direction_loc, &light.x, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader.shader, shader.light_ambient_loc, &ambient.x, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader.shader, shader.light_diffuse_loc, &diffuse.x, SHADER_UNIFORM_VEC3);
}

void set_game_surface_camera(Vector3 position) {
    auto& shader = lit_shader();
    if (shader.shader.id != 0U && shader.view_position_loc >= 0) {
        SetShaderValue(shader.shader, shader.view_position_loc, &position.x, SHADER_UNIFORM_VEC3);
        set_game_surface_light(1.0F);
    }
}

void set_game_world_time(float hours) {
    g_world_time_hours = std::clamp(hours, 0.0F, 24.0F);
}

namespace {

// Model matrices for each instance, for every GpuMesh draw. Each push re-sends the
// whole buffer (glBufferData with data), so the driver gives us new storage instead
// of waiting for earlier draws. A glBufferSubData into storage still in use made
// every draw wait on Apple's GL-on-Metal.
GLuint instance_stream() {
    static GLuint vbo = 0U;
    if (vbo == 0U) glGenBuffers(1, &vbo);
    return vbo;
}

GfxDebugDrawCategory category_for(GpuSurface surface) {
    switch (surface) {
        case GpuSurface::Terrain: return GfxDebugDrawCategory::Terrain;
        case GpuSurface::Mask: return GfxDebugDrawCategory::WireOverlay;
        case GpuSurface::ShadowVolume: return GfxDebugDrawCategory::ShadowVolumes;
        default: return GfxDebugDrawCategory::CityCells;
    }
}

} // namespace

namespace {
struct EnvironmentCubemap {
    GLuint texture{};
    GLuint framebuffer{};
    GLuint depth{};
    // WebGL: blurred levels are drawn here and copied into the cube, because a cube
    // level outside [base, max] makes the framebuffer incomplete.
    GLuint scratch{};
    Vector3 camera{};
    Vector3 light{};
    std::uint64_t terrain_fingerprint{};
    bool has_terrain{};
    bool valid{};
    bool capturing{};
};

EnvironmentCubemap& environment_cubemap() {
    static EnvironmentCubemap cubemap;
    return cubemap;
}

} // namespace

// The road-marking overlay arrays the world shader samples on units 4/5
// (texture 1's slots itemModels[0x356/0x357]). subrosa.exe makes them one layer at
// a time @0x98ff0 (the function Ghidra calls Texture_GenerateCubemapFace):
// 128x128, 11 layers, with x/y the texel column/row.
//   colour: layers 1/2 have a yellow (255,255,32) line on rows 5..10 / 117..122,
//           and 6/7 the same on columns. 4/5 have a white dash on rows 5..10 /
//           117..122 over columns 32..95, and 9/10 the same on columns 5..10 /
//           117..122 over rows 32..95. 3 has white rows 16..31 and 96..111, and 8
//           has white columns 11..26 and 91..106. They are only opaque where
//           marked; layer 0 is empty.
//   material: (128, 130 + 2*cos(noise), 0) on marked layers, 0 on layer 0. The
//           cosine part is not ported, only its average.
struct OverlayArrays {
    GLuint color{};
    GLuint material{};
};

const OverlayArrays& road_overlay_arrays() {
    static const OverlayArrays arrays = [] {
        constexpr int size = 128;
        constexpr int layers = 11;
        const auto in = [](int value, int from, int to) { return value >= from && value <= to; };
        std::vector<std::uint8_t> color(static_cast<std::size_t>(size * size * layers) * 4U, 0U);
        std::vector<std::uint8_t> material(color.size(), 0U);
        for (int layer = 1; layer < layers; ++layer) {
            const bool yellow = layer == 1 || layer == 2 || layer == 6 || layer == 7;
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    bool marked = false;
                    switch (layer) {
                        case 1: marked = in(y, 5, 10); break;
                        case 2: marked = in(y, 117, 122); break;
                        case 6: marked = in(x, 5, 10); break;
                        case 7: marked = in(x, 117, 122); break;
                        case 4: marked = in(y, 5, 10) && in(x, 32, 95); break;
                        case 5: marked = in(y, 117, 122) && in(x, 32, 95); break;
                        case 9: marked = in(x, 5, 10) && in(y, 32, 95); break;
                        case 10: marked = in(x, 117, 122) && in(y, 32, 95); break;
                        case 3: marked = in(y, 16, 31) || in(y, 96, 111); break;
                        case 8: marked = in(x, 11, 26) || in(x, 91, 106); break;
                        default: break;
                    }
                    const auto texel = (static_cast<std::size_t>(layer * size + y) * size + static_cast<std::size_t>(x)) * 4U;
                    color[texel + 0] = 255;
                    color[texel + 1] = 255;
                    color[texel + 2] = yellow ? 32 : 255;
                    color[texel + 3] = marked ? 255 : 0;
                    material[texel + 0] = 128;
                    material[texel + 1] = 130;
                }
            }
        }
        OverlayArrays result;
        rlDrawRenderBatchActive();
        for (auto [id, pixels] : {std::pair{&result.color, &color}, std::pair{&result.material, &material}}) {
            glGenTextures(1, id);
            glBindTexture(GL_TEXTURE_2D_ARRAY, *id);
            glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, size, size, layers, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels->data());
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
        }
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        return result;
    }();
    return arrays;
}

void draw_gpu_mesh_instances(const GpuMesh& mesh, const std::vector<Matrix>& transforms, GpuSurface surface) {
    draw_gpu_mesh_batch({{&mesh, transforms}}, surface);
}

void draw_gpu_mesh_batch(const std::vector<GpuMeshDraw>& draws, GpuSurface surface, GpuPartFilter filter) {
    std::size_t total_instances = 0;
    for (const auto& draw : draws) {
        if (draw.mesh != nullptr && !draw.mesh->empty()) total_instances += draw.transforms.size();
    }
    if (total_instances == 0U) return;
    rlDrawRenderBatchActive();
    const auto view_projection = MatrixToFloatV(MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection()));
    const bool lit = surface != GpuSurface::Mask && surface != GpuSurface::ShadowVolume;
    // Made the first time it is used, which flushes raylib's batch and unbinds the
    // program, so do it before binding ours.
    if (lit) road_overlay_arrays();
    if (surface == GpuSurface::ShadowVolume) {
        const auto& shader = shadow_volume_shader();
        glUseProgram(shader.shader.id);
        glUniformMatrix4fv(shader.view_projection_loc, 1, GL_FALSE, view_projection.v);
    } else if (lit) {
        const auto& shader = lit_shader();
        glUseProgram(shader.shader.id);
        glUniformMatrix4fv(shader.view_projection_loc, 1, GL_FALSE, view_projection.v);
        glUniform1i(shader.building_surface_loc, surface == GpuSurface::Building ? 1 : 0);
        glUniform1i(shader.terrain_surface_loc, surface == GpuSurface::Terrain ? 1 : 0);
        glUniform1i(shader.model_surface_loc, surface == GpuSurface::Model ? 1 : 0);
        glUniform1i(shader.unlit_surface_loc, surface == GpuSurface::Unlit ? 1 : 0);
    } else {
        const auto& shader = mask_shader();
        glUseProgram(shader.shader.id);
        glUniformMatrix4fv(shader.view_projection_loc, 1, GL_FALSE, view_projection.v);
    }
    // Every mesh's instances go into one buffer; each mesh reads its own slice.
    thread_local std::vector<float16> columns;
    columns.clear();
    columns.reserve(total_instances);
    for (const auto& draw : draws) {
        if (draw.mesh == nullptr || draw.mesh->empty()) continue;
        for (const auto& transform : draw.transforms) columns.push_back(MatrixToFloatV(transform));
    }
    glBindBuffer(GL_ARRAY_BUFFER, instance_stream());
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(columns.size() * sizeof(float16)), columns.data(), GL_STREAM_DRAW);
    // World geometry is wound clockwise, as in the game, which draws it with
    // glFrontFace(GL_CW) and back faces culled. With raylib's counter-clockwise front
    // face, that means culling GL_FRONT, and the lit shader flips the normals of the
    // (always back-facing) triangles that are left. Masks and shadow volumes stay
    // two-sided.
    const bool glass = filter == GpuPartFilter::Glass;
    if (lit && !glass) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);
    } else {
        glDisable(GL_CULL_FACE);
    }
    if (lit) glUniform1f(lit_shader().glass_alpha_loc, glass ? kRoomGlassAlpha : 0.0F);
    const bool blend_was_on = glIsEnabled(GL_BLEND) == GL_TRUE;
    if (glass) {
        // Room glass (_block_addwindows) is see-through: blend it over the room
        // behind it and keep the depth unchanged for geometry drawn later.
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        // The shadow pass subtracts direct light per pixel after this pass, so
        // the pane must scale the direct light behind it by (1 - alpha), like the
        // colour. The packed depth stays that of the surface behind.
        if (g_target_has_mrt) {
            static constexpr GLenum glass_outputs[] = {GL_COLOR_ATTACHMENT0, GL_NONE, GL_COLOR_ATTACHMENT2};
            glDrawBuffers(3, glass_outputs);
        }
    }
    if (lit) {
        // Unbound while capturing, so the cube is never read while being written.
        const auto& environment = environment_cubemap();
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_CUBE_MAP, environment.capturing ? 0U : environment.texture);
        const auto& overlay = road_overlay_arrays();
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D_ARRAY, overlay.color);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D_ARRAY, overlay.material);
    }
    const auto default_texture = rlGetTextureIdDefault();
    // Each part only rebinds what is different from the part before.
    GLuint bound_texture = ~0U;
    GLuint bound_material = ~0U;
    Color bound_color{0, 0, 0, 0};
    int bound_window = -1;
    bool color_set = false;
    std::size_t first_instance = 0;
    std::uint64_t triangles = 0, indices = 0, calls = 0;
    for (const auto& draw : draws) {
        if (draw.mesh == nullptr || draw.mesh->empty() || draw.transforms.empty()) continue;
        const auto& mesh = *draw.mesh;
        glBindVertexArray(mesh.vao);
        for (GLuint column = 0; column < 4; ++column) {
            glVertexAttribPointer(kAttribInstance + column, 4, GL_FLOAT, GL_FALSE, sizeof(float16),
                reinterpret_cast<void*>(first_instance * sizeof(float16) + column * 4U * sizeof(float)));
        }
        const auto instances = static_cast<GLsizei>(draw.transforms.size());
        for (const auto& part : mesh.parts) {
            if ((filter == GpuPartFilter::Opaque && part.window_material) || (glass && !part.window_material)) continue;
            if (part.has_bounds && g_shadow_cull && !shadow_part_visible(*g_shadow_cull, part)) continue;
            if (lit) {
                const auto& shader = lit_shader();
                const auto material = part.material_texture != 0U ? part.material_texture : default_texture;
                const auto texture = part.texture != 0U ? part.texture : default_texture;
                if (material != bound_material) {
                    glActiveTexture(GL_TEXTURE1);
                    glBindTexture(GL_TEXTURE_2D, material);
                    bound_material = material;
                }
                if (texture != bound_texture) {
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, texture);
                    bound_texture = texture;
                }
                if (!color_set || part.color.r != bound_color.r || part.color.g != bound_color.g ||
                    part.color.b != bound_color.b || part.color.a != bound_color.a) {
                    glUniform4f(shader.color_loc, part.color.r / 255.0F, part.color.g / 255.0F, part.color.b / 255.0F, part.color.a / 255.0F);
                    bound_color = part.color;
                    color_set = true;
                }
                const int window = part.window_material ? 1 : 0;
                if (window != bound_window) {
                    glUniform1i(shader.window_surface_loc, window);
                    bound_window = window;
                }
            }
            glDrawElementsInstanced(GL_TRIANGLES, static_cast<GLsizei>(part.index_count), GL_UNSIGNED_INT,
                reinterpret_cast<void*>(static_cast<std::size_t>(part.first_index) * sizeof(std::uint32_t)), instances);
            triangles += static_cast<std::uint64_t>(part.index_count / 3U) * draw.transforms.size();
            indices += static_cast<std::uint64_t>(part.index_count) * draw.transforms.size();
            ++calls;
        }
        first_instance += draw.transforms.size();
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    if (lit) {
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    }
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (glass) {
        glDepthMask(GL_TRUE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        if (g_target_has_mrt) {
            static constexpr GLenum outputs[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
            glDrawBuffers(g_draw_buffer_count, outputs);
        }
        if (!blend_was_on) glDisable(GL_BLEND);
    }
    if (lit) glUniform1f(lit_shader().glass_alpha_loc, 0.0F);
    glCullFace(GL_BACK);
    glEnable(GL_CULL_FACE);
    if (g_gfx_debug_stats != nullptr) g_gfx_debug_stats->add_draw(category_for(surface), triangles, indices, calls);
}

struct SkyShader {
    Shader shader{};
    int light_direction_loc{-1};
    int world_position_loc{-1};
    int depth_texture_loc{-1};
    int inverse_resolution_loc{-1};
    int mvp_loc{-1};
    unsigned int vao{0};
    unsigned int vbo{0};
    bool loaded{false};
};

SkyShader& sky_shader() {
    static SkyShader cache;
    if (!cache.loaded) {
        const char* vs = PAFTA_GLSL_VERSION R"glsl(
uniform mat4 modelviewprojectionmatrix;
uniform vec3 lightdirection;
layout(location=0) in vec3 position_in;
layout(location=1) in vec3 viewvec_in;
out vec3 lightvec;
out vec3 viewvec;
void main() {
    lightvec=lightdirection;
    viewvec=viewvec_in;
    gl_Position=modelviewprojectionmatrix*vec4(position_in,1.0);
}
)glsl";
        const char* fs = PAFTA_GLSL_VERSION R"glsl(
out vec4 finalColor;
in vec3 lightvec;
in vec3 viewvec;
uniform vec3 worldPosition;
uniform sampler2D depthTexture;
uniform vec2 inverseResolution;
const vec4 ATMOSPHERE = vec4(0.0502275936, 0.0001, 6.4, 637.1);
const vec3 WAVELENGTH = vec3(0.625, 0.75, 1.0);
const int SAMPLE_COUNT = 4;
float rayDistanceSphere(vec3 position, vec3 direction, float radius) {
    float scale = dot(position, direction);
    float b2 = dot(position, position)-scale*scale;
    return sqrt(max(0.0, radius*radius-b2))-scale;
}
float rayDistanceSphere2(vec3 position, vec3 direction, float radius) {
    float scale = -dot(position, direction);
    if (scale < 0.0) return 0.0;
    float b2 = dot(position, position)-scale*scale;
    if (b2 > radius*radius) return 0.0;
    return scale-sqrt(max(0.0, radius*radius-b2));
}
vec4 rayleighScattering(vec3 position, vec3 viewNormal, vec3 lightNormal) {
    float inverseSamples = 1.0/float(SAMPLE_COUNT);
    float viewLength = length(position);
    float phase = dot(viewNormal, lightNormal);
    phase = 3.0/(16.0*3.14159265359)*(1.0+phase*phase);
    vec3 outScatter = WAVELENGTH*ATMOSPHERE.x;
    vec3 inScatter = phase*WAVELENGTH*ATMOSPHERE.x;
    vec3 color = vec3(0.0);
    if (viewLength > 4.0) {
        float sun = max(dot(viewNormal, lightNormal)-0.9998, 0.0)*262144.0;
        color = vec3(min(sun, 16.0));
    }
    float dist = 1.0-inverseSamples*0.5;
    for (int count = 0; count < SAMPLE_COUNT; ++count) {
        vec3 rayPosition = worldPosition+viewNormal*viewLength*dist;
        float rayLength = rayDistanceSphere(rayPosition, lightNormal, ATMOSPHERE.w+ATMOSPHERE.z);
        float height = 1.0-(length(rayPosition)-ATMOSPHERE.w)/ATMOSPHERE.z;
        height = clamp(height, 0.0, 1.0);
        height *= height;
        float density = height*8.0;
        vec3 rayleigh = inScatter*density*8.0*exp(-rayLength*density*0.25*ATMOSPHERE.x*WAVELENGTH);
        vec3 extinction = outScatter*density*0.5;
        color *= exp(-extinction*viewLength*inverseSamples);
        color += (1.0-exp(-extinction*viewLength*inverseSamples))*(rayleigh/max(extinction, vec3(1e-8)));
        dist -= inverseSamples;
    }
    return vec4(color, 1.0);
}
void main() {
    vec3 viewNormal = normalize(viewvec);
    vec3 lightNormal = normalize(lightvec);
    vec2 depthTexCoord = (gl_FragCoord.xy+vec2(0.125))*inverseResolution;
    vec3 encodedDepth = texture(depthTexture, depthTexCoord).rgb;
    float distance = encodedDepth.r*65536.0+encodedDepth.g*256.0+encodedDepth.b;
    bool skyPixel = distance > 65536.0;
    if (skyPixel) {
        distance = rayDistanceSphere2(worldPosition, viewNormal, ATMOSPHERE.w);
        if (distance <= 0.0) distance = rayDistanceSphere(worldPosition, viewNormal, ATMOSPHERE.w+ATMOSPHERE.z);
    } else {
        distance *= ATMOSPHERE.y;
    }
    finalColor = rayleighScattering(viewNormal*distance, viewNormal, lightNormal);
}
)glsl";
        cache.shader = LoadShaderFromMemory(vs, fs);
        cache.light_direction_loc = GetShaderLocation(cache.shader, "lightdirection");
        cache.world_position_loc = GetShaderLocation(cache.shader, "worldPosition");
        cache.depth_texture_loc = GetShaderLocation(cache.shader, "depthTexture");
        cache.inverse_resolution_loc = GetShaderLocation(cache.shader, "inverseResolution");
        cache.mvp_loc = GetShaderLocation(cache.shader, "modelviewprojectionmatrix");
        glGenVertexArrays(1, &cache.vao);
        glGenBuffers(1, &cache.vbo);
        glBindVertexArray(cache.vao);
        glBindBuffer(GL_ARRAY_BUFFER, cache.vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(float)*24, nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float)*6, nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(float)*6, reinterpret_cast<void*>(sizeof(float)*3));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        cache.loaded = true;
    }
    return cache;
}

// Draws the atmosphere over a full-screen quad whose corners look along `rays`
// (bottom-left, bottom-right, top-right, top-left). depth_texture holds the packed
// scene distance; a white texel means open sky.
void draw_sky_rays(const std::array<Vector3, 4>& rays, Vector3 camera_position, Vector2 inverse_resolution,
                   GLuint depth_texture) {
    auto& sky = sky_shader();
    const Vector3 light_direction = game_light_direction();
    const Vector3 world_position{
        camera_position.x*0.0001F,
        camera_position.y*0.0001F+637.1F,
        camera_position.z*0.0001F,
    };
    const float vertices[] = {
        -1.0F, -1.0F, 0.0F, rays[0].x, rays[0].y, rays[0].z,
         1.0F, -1.0F, 0.0F, rays[1].x, rays[1].y, rays[1].z,
         1.0F,  1.0F, 0.0F, rays[2].x, rays[2].y, rays[2].z,
        -1.0F,  1.0F, 0.0F, rays[3].x, rays[3].y, rays[3].z,
    };
    constexpr float identity[] = {
        1.0F, 0.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F, 0.0F,
        0.0F, 0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 0.0F, 1.0F,
    };
    glUseProgram(sky.shader.id);
    glUniform3fv(sky.light_direction_loc, 1, &light_direction.x);
    glUniform3fv(sky.world_position_loc, 1, &world_position.x);
    glUniform2fv(sky.inverse_resolution_loc, 1, &inverse_resolution.x);
    glUniformMatrix4fv(sky.mvp_loc, 1, GL_FALSE, identity);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, depth_texture);
    glUniform1i(sky.depth_texture_loc, 0);
    glBindVertexArray(sky.vao);
    glBindBuffer(GL_ARRAY_BUFFER, sky.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(rlGetShaderIdDefault());
}

void draw_game_sky(const Camera3D& camera, Vector2 viewport_size, Texture2D depth_texture) {
    if (viewport_size.x <= 0.0F || viewport_size.y <= 0.0F) {
        return;
    }
    if (depth_texture.id == 0U) {
        return;
    }
    const auto forward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    const auto right = Vector3Normalize(Vector3CrossProduct(forward, camera.up));
    const auto up = Vector3Normalize(Vector3CrossProduct(right, forward));
    const float aspect = viewport_size.x / viewport_size.y;
    const float vertical_fov = std::tan(camera.fovy * DEG2RAD * 0.5F);
    const float horizontal_fov = vertical_fov*aspect;
    const auto ray = [&](float horizontal, float vertical) {
        return Vector3Add(forward, Vector3Add(Vector3Scale(right, horizontal), Vector3Scale(up, vertical)));
    };
    const std::array<Vector3, 4> rays{
        ray(-horizontal_fov, -vertical_fov),
        ray( horizontal_fov, -vertical_fov),
        ray( horizontal_fov,  vertical_fov),
        ray(-horizontal_fov,  vertical_fov),
    };
    const unsigned int framebuffer = rlGetActiveFramebuffer();
    rlDrawRenderBatchActive();
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
    const GLenum color_only[] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, color_only);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    // RenderSky @122690: GL_ONE, GL_SRC_ALPHA over the lit scene.
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_SRC_ALPHA);
    draw_sky_rays(rays, camera.position, {1.0F/viewport_size.x, 1.0F/viewport_size.y}, depth_texture.id);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, depth_texture.id, 0);
    const GLenum color_only_after_sky[] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, color_only_after_sky);
    rlEnableFramebuffer(framebuffer);
}

namespace {

// RenderLandscapeCubemapAndBlurCapture @93e60: a 512 cube around the camera.
// Each face is cleared to 0.125 grey and filled with the atmosphere
// (_renderframeatmospherecubemap @122ca0, treating every texel as open sky), and
// then the landscape is drawn at half light with the grey-city colour map.
// _blurcubemap @1b6a0 then fills levels 1..9 (256 down to 1), each one a blur of
// the level above, with the original cubemap blur shader.
constexpr int kEnvironmentSize = 512;
constexpr int kEnvironmentLevels = 10;

// The direction through NDC (x, y) of a cube face (faces +X -X +Y -Y +Z -Z), using
// GL's cube-map layout, so the capture is read back with true directions.
Vector3 cube_face_direction(int face, float x, float y) {
    switch (face) {
        case 0: return {1.0F, -y, -x};
        case 1: return {-1.0F, -y, x};
        case 2: return {x, 1.0F, y};
        case 3: return {x, -1.0F, -y};
        case 4: return {x, -y, 1.0F};
        default: return {-x, -y, -1.0F};
    }
}

struct CubemapBlurShader {
    Shader shader{};
    int lod_loc{-1};
    GLuint vao{};
    GLuint vbo{};
};

CubemapBlurShader& cubemap_blur_shader() {
    static CubemapBlurShader cache = [] {
        CubemapBlurShader value;
        // The original cubemap_blur shaders, without the game's y/z axis flip (this
        // cube is captured with GL cube-map directions).
        value.shader = LoadShaderFromMemory(PAFTA_GLSL_VERSION R"glsl(
layout(location=0) in vec3 position_in;
layout(location=1) in vec3 viewvec_in;
layout(location=2) in vec3 upvec_in;
out vec3 viewvec;
out vec3 upvec;
void main() {
    viewvec = viewvec_in;
    upvec = upvec_in;
    gl_Position = vec4(position_in, 1.0);
}
)glsl", PAFTA_GLSL_VERSION R"glsl(
uniform samplerCube tex_color;
uniform float texlod;
in vec3 viewvec;
in vec3 upvec;
out vec4 fragcolor;
#define PI 3.1415926
void main() {
    float maxangle = PI*0.03125;
    vec3 frontnormal = normalize(viewvec);
    vec3 rightnormal = normalize(cross(frontnormal, upvec));
    vec3 upnormal = normalize(cross(rightnormal, frontnormal));
    vec3 accum = vec3(0.0);
    float total = 0.0;
    const int numofsamples = 2;
    for (int county = -numofsamples; county <= numofsamples; county++) {
        float siny = sin(float(county)*maxangle/float(numofsamples));
        for (int countx = -numofsamples; countx <= numofsamples; countx++) {
            float sinx = sin(float(countx)*maxangle/float(numofsamples));
            vec3 sampledir = frontnormal+upnormal*siny+rightnormal*sinx;
            float opacity = cos(float(county)*PI*0.375/float(numofsamples))*cos(float(countx)*PI*0.375/float(numofsamples));
            accum += textureLod(tex_color, sampledir, texlod).rgb*opacity;
            total += opacity;
        }
    }
    fragcolor = vec4(accum/total, 1.0);
}
)glsl");
        value.lod_loc = GetShaderLocation(value.shader, "texlod");
        glGenVertexArrays(1, &value.vao);
        glGenBuffers(1, &value.vbo);
        glBindVertexArray(value.vao);
        glBindBuffer(GL_ARRAY_BUFFER, value.vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 36, nullptr, GL_DYNAMIC_DRAW);
        for (GLuint attribute = 0; attribute < 3; ++attribute) {
            glEnableVertexAttribArray(attribute);
            glVertexAttribPointer(attribute, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 9,
                reinterpret_cast<void*>(attribute * 3U * sizeof(float)));
        }
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        return value;
    }();
    return cache;
}

void create_environment_cubemap(EnvironmentCubemap& cubemap) {
    glGenTextures(1, &cubemap.texture);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cubemap.texture);
#ifdef __EMSCRIPTEN__
    // Half floats need EXT_color_buffer_float (web_hdr_targets); otherwise 8-bit.
    const GLenum internal_format = web_hdr_targets() ? GL_RGBA16F : GL_RGBA8;
    const GLenum type = web_hdr_targets() ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE;
#else
    constexpr GLenum internal_format = GL_RGBA16F;
    constexpr GLenum type = GL_HALF_FLOAT;
#endif
    for (int level = 0; level < kEnvironmentLevels; ++level) {
        const int size = std::max(1, kEnvironmentSize >> level);
        for (int face = 0; face < 6; ++face) {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(face), level, internal_format,
                size, size, 0, GL_RGBA, type, nullptr);
        }
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, kEnvironmentLevels - 1);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    glGenFramebuffers(1, &cubemap.framebuffer);
    glGenRenderbuffers(1, &cubemap.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, cubemap.depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kEnvironmentSize, kEnvironmentSize);
#ifdef __EMSCRIPTEN__
    glGenRenderbuffers(1, &cubemap.scratch);
    glBindRenderbuffer(GL_RENDERBUFFER, cubemap.scratch);
    glRenderbufferStorage(GL_RENDERBUFFER, internal_format, kEnvironmentSize / 2, kEnvironmentSize / 2);
#endif
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
}

} // namespace

void invalidate_environment_cubemap() {
    environment_cubemap().valid = false;
}

void update_environment_cubemap(const GpuMesh* terrain, std::uint64_t terrain_fingerprint,
                               Vector3 camera_position, bool follow_camera) {
    auto& cubemap = environment_cubemap();
    const auto light = Vector3Normalize(game_light_direction());
    const bool has_terrain = terrain != nullptr;
    // Capturing six faces and their blurred levels is slow. Reuse them until the
    // scene inputs change, or until live reflections have moved 16 units.
    if (cubemap.valid && (!follow_camera || Vector3Distance(cubemap.camera, camera_position) < 16.0F) &&
        Vector3LengthSqr(Vector3Subtract(cubemap.light, light)) < 1e-10F &&
        cubemap.has_terrain == has_terrain && cubemap.terrain_fingerprint == terrain_fingerprint) {
        return;
    }
    rlDrawRenderBatchActive();
    if (cubemap.texture == 0U) create_environment_cubemap(cubemap);
    cubemap.camera = camera_position;
    cubemap.light = light;
    cubemap.has_terrain = has_terrain;
    cubemap.terrain_fingerprint = terrain_fingerprint;
    cubemap.valid = true;
    cubemap.capturing = true;

    GLint previous_framebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
    GLint previous_viewport[4]{};
    glGetIntegerv(GL_VIEWPORT, previous_viewport);
    const auto previous_modelview = rlGetMatrixModelview();
    const auto previous_projection = rlGetMatrixProjection();

    glBindFramebuffer(GL_FRAMEBUFFER, cubemap.framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, cubemap.depth);
    const GLenum color_only[] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, color_only);
    glViewport(0, 0, kEnvironmentSize, kEnvironmentSize);
    const auto projection = MatrixPerspective(PI * 0.5, 1.0, 1.0, 20000.0);
    auto& lit = lit_shader();
    for (int face = 0; face < 6; ++face) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(face),
            cubemap.texture, 0);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glClearColor(0.125F, 0.125F, 0.125F, 0.0F);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        // Atmosphere: no blending, every texel open sky (white packed depth).
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
        glDepthMask(GL_FALSE);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_BLEND);
        draw_sky_rays({
            cube_face_direction(face, -1.0F, -1.0F), cube_face_direction(face, 1.0F, -1.0F),
            cube_face_direction(face, 1.0F, 1.0F), cube_face_direction(face, -1.0F, 1.0F),
        }, camera_position, {1.0F, 1.0F}, rlGetTextureIdDefault());
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (terrain != nullptr && !terrain->empty()) {
            // View rows: the face's right, the face's up, minus forward.
            const auto forward = cube_face_direction(face, 0.0F, 0.0F);
            const auto right = Vector3Subtract(cube_face_direction(face, 1.0F, 0.0F), forward);
            const auto up = Vector3Subtract(cube_face_direction(face, 0.0F, 1.0F), forward);
            const Matrix view{
                right.x, right.y, right.z, -Vector3DotProduct(right, camera_position),
                up.x, up.y, up.z, -Vector3DotProduct(up, camera_position),
                -forward.x, -forward.y, -forward.z, Vector3DotProduct(forward, camera_position),
                0.0F, 0.0F, 0.0F, 1.0F,
            };
            rlSetMatrixModelview(view);
            rlSetMatrixProjection(projection);
            SetShaderValue(lit.shader, lit.view_position_loc, &camera_position.x, SHADER_UNIFORM_VEC3);
            const int on = 1;
            SetShaderValue(lit.shader, lit.cubemap_pass_loc, &on, SHADER_UNIFORM_INT);
            set_game_surface_light(0.5F);
            draw_gpu_mesh_instances(*terrain, {MatrixIdentity()}, GpuSurface::Terrain);
            const int off = 0;
            SetShaderValue(lit.shader, lit.cubemap_pass_loc, &off, SHADER_UNIFORM_INT);
            set_game_surface_light(1.0F);
        }
    }

    auto& blur = cubemap_blur_shader();
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
    glUseProgram(blur.shader.id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, cubemap.texture);
    glBindVertexArray(blur.vao);
    glBindBuffer(GL_ARRAY_BUFFER, blur.vbo);
    for (int level = 1; level < kEnvironmentLevels; ++level) {
        // Only read the level above while writing this one.
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL, level - 1);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, level - 1);
        glUniform1f(blur.lod_loc, static_cast<float>(level - 1));
        const int size = std::max(1, kEnvironmentSize >> level);
        glViewport(0, 0, size, size);
        for (int face = 0; face < 6; ++face) {
#ifdef __EMSCRIPTEN__
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, cubemap.scratch);
#else
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(face), cubemap.texture, level);
#endif
            const auto up = Vector3Subtract(cube_face_direction(face, 0.0F, 1.0F), cube_face_direction(face, 0.0F, 0.0F));
            float vertices[36]{};
            const std::array<std::array<float, 2>, 4> corners{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
            for (std::size_t i = 0; i < 4; ++i) {
                const auto dir = cube_face_direction(face, corners[i][0], corners[i][1]);
                const float v[9]{corners[i][0], corners[i][1], 0.0F, dir.x, dir.y, dir.z, up.x, up.y, up.z};
                std::copy(std::begin(v), std::end(v), vertices + i * 9U);
            }
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
            glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
#ifdef __EMSCRIPTEN__
            glCopyTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(face), level, 0, 0, 0, 0, size, size);
#endif
        }
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, kEnvironmentLevels - 1);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    glUseProgram(rlGetShaderIdDefault());

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous_framebuffer));
    glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
    rlSetMatrixModelview(previous_modelview);
    rlSetMatrixProjection(previous_projection);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_CULL_FACE);
    cubemap.capturing = false;
}

struct MaskOutlineShader {
    Shader shader{};
    int texel_size_loc{-1};
    int color_loc{-1};
    bool loaded{false};
};

MaskOutlineShader& mask_outline_shader() {
    static MaskOutlineShader cache;
    if (!cache.loaded) {
        const char* fs =
            PAFTA_GLSL_VERSION
            "in vec2 fragTexCoord;\n"
            "in vec4 fragColor;\n"
            "out vec4 finalColor;\n"
            "uniform sampler2D texture0;\n"
            "uniform vec2 texelSize;\n"
            "uniform vec4 outlineColor;\n"
            "void main() {\n"
            "    float center = texture(texture0, fragTexCoord).a;\n"
            "    float neighbor = 0.0;\n"
            "    for (int y = -2; y <= 2; ++y) {\n"
            "        for (int x = -2; x <= 2; ++x) {\n"
            "            if (x == 0 && y == 0) { continue; }\n"
            "            vec2 offset = vec2(float(x), float(y)) * texelSize;\n"
            "            neighbor = max(neighbor, texture(texture0, fragTexCoord + offset).a);\n"
            "        }\n"
            "    }\n"
            "    float outside = step(0.01, neighbor) * (1.0 - step(0.01, center));\n"
            "    finalColor = vec4(outlineColor.rgb, outlineColor.a * outside) * fragColor;\n"
            "}\n";
        cache.shader = LoadShaderFromMemory(nullptr, fs);
        cache.texel_size_loc = GetShaderLocation(cache.shader, "texelSize");
        cache.color_loc = GetShaderLocation(cache.shader, "outlineColor");
        cache.loaded = true;
    }
    return cache;
}

void draw_city_building_mask(GpuBuildingRenderModel& model, const SbcBuilding& placement) {
    draw_gpu_mesh_instances(model.mesh, {city_placement_matrix(placement)}, GpuSurface::Mask);
}

// Draws the footprint of one road into the active mask as solid geometry, so
// draw_outline_from_mask traces an outline around it (the same look city buildings
// get). Drawn at the road's terrain heights.
void draw_city_road_mask(const EditorProject& project, const MapScene& scene, std::size_t street_index) {
    if (!project.city || street_index >= project.city->data.streets.size()) {
        return;
    }
    // Draw the street's real road tiles (the asphalt you clicked) into the mask, so the
    // outline follows the real shape, not a simplified strip.
    const auto unit = scene.unit_size > 0.0F ? scene.unit_size : 4.0F;
    rlDisableBackfaceCulling();
    for (const auto& tile : scene.city_tiles) {
        if (tile.kind != CityTileKind::Road || tile.street_index < 0 ||
            static_cast<std::size_t>(tile.street_index) != street_index) {
            continue;
        }
        const auto top = tile.base_y + unit;
        const float x0 = static_cast<float>(tile.x) * unit;
        const float x1 = x0 + unit;
        const float z0 = static_cast<float>(tile.z) * unit;
        const float z1 = z0 + unit;
        const float cx = x0 + unit * 0.5F;
        const float cz = z0 + unit * 0.5F;
        const float lift = unit * 0.04F;
        // Follow the road's slope so the outline covers the whole visible surface.
        const auto corner = [&](float px, float pz) {
            const float dy = tile.slope_x * ((px - cx) / unit) + tile.slope_z * ((pz - cz) / unit);
            return Vector3{px, top + dy + lift, pz};
        };
        const Vector3 p0 = corner(x0, z0);
        const Vector3 p1 = corner(x1, z0);
        const Vector3 p2 = corner(x1, z1);
        const Vector3 p3 = corner(x0, z1);
        DrawTriangle3D(p0, p1, p2, WHITE);
        DrawTriangle3D(p0, p2, p3, WHITE);
        DrawTriangle3D(p2, p1, p0, WHITE); // back face too, so it shows from any angle
        DrawTriangle3D(p3, p2, p0, WHITE);
    }
    rlEnableBackfaceCulling();
}

namespace {

// The triangles of `buffer` whose centre passes `inside`, flattened to three
// vertices each. Cached per highlight key until the buffer changes, so a hover
// outline doesn't search a city-sized mesh every frame.
const std::vector<Vec3>& mask_triangles(std::uint64_t key, const BuildingRenderBuffer& buffer,
                                      const std::function<bool(Vec3)>& inside) {
    auto& entries = buffer.mask_cache;
    for (const auto& entry : entries) {
        if (entry.key == key) return entry.points;
    }
    if (entries.size() == 8U) entries.erase(entries.begin());
    auto& entry = entries.emplace_back(RenderBufferMask{key, {}});
    for (const auto& tri : buffer.triangles) {
        const auto& a = buffer.vertices[tri.indices[0]].position;
        const auto& b = buffer.vertices[tri.indices[1]].position;
        const auto& c = buffer.vertices[tri.indices[2]].position;
        if (!inside({(a.x + b.x + c.x) / 3.0F, (a.y + b.y + c.y) / 3.0F, (a.z + b.z + c.z) / 3.0F})) continue;
        entry.points.insert(entry.points.end(), {a, b, c});
    }
    return entry.points;
}

// Draws the cached mask triangles, both windings, raised by `offset`. The batch is
// flushed before culling is turned back on, because DrawTriangle3D draws later.
bool draw_mask_triangles(const std::vector<Vec3>& points, Vector3 offset) {
    if (points.empty()) return false;
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();
    for (std::size_t i = 0; i + 2 < points.size(); i += 3) {
        const auto a = Vector3Add(to_raylib(points[i]), offset);
        const auto b = Vector3Add(to_raylib(points[i + 1]), offset);
        const auto c = Vector3Add(to_raylib(points[i + 2]), offset);
        DrawTriangle3D(a, b, c, WHITE);
        DrawTriangle3D(c, b, a, WHITE);
    }
    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();
    return true;
}

bool inside_cell_box(Vec3 p, float x0, float y0, float z0) {
    constexpr float unit = 4.0F;
    constexpr float eps = 0.01F;
    return p.x >= x0 - eps && p.x <= x0 + unit + eps && p.y >= y0 - eps && p.y <= y0 + unit + eps &&
        p.z >= z0 - eps && p.z <= z0 + unit + eps;
}

std::uint64_t mask_key(std::uint64_t kind, std::int64_t a, std::int64_t b = 0, std::int64_t c = 0) {
    return kind << 60U ^ (static_cast<std::uint64_t>(a) & 0xfffffU) << 40U ^ (static_cast<std::uint64_t>(b) & 0xfffffU) << 20U ^
        (static_cast<std::uint64_t>(c) & 0xfffffU);
}

} // namespace

// One building cell's drawn faces (the edit view's mesh, in anchored coordinates)
// into the active mask, so the outline follows the block's real shape. An empty
// cell uses its cube instead.
void draw_building_cell_mask(const BuildingRenderBuffer& buffer, const SbbBuilding& building, const SbbCell& cell) {
    constexpr auto unit_size = 4.0F;
    const auto x0 = (static_cast<float>(cell.x) - static_cast<float>(building.anchor_x)) * unit_size;
    const auto y0 = (static_cast<float>(cell.y) - static_cast<float>(building.anchor_y)) * unit_size;
    const auto z0 = (static_cast<float>(cell.z) - static_cast<float>(building.anchor_z)) * unit_size;
    const auto& points = mask_triangles(mask_key(1, cell.x, cell.y, cell.z), buffer,
        [&](Vec3 p) { return inside_cell_box(p, x0, y0, z0); });
    if (!draw_mask_triangles(points, {})) {
        DrawCubeV({x0 + unit_size * 0.5F, y0 + unit_size * 0.5F, z0 + unit_size * 0.5F}, {unit_size, unit_size, unit_size}, WHITE);
    }
}

// Tints the drawn triangles of one cell face (the paint or hover target), both
// windings, over the lit mesh.
void draw_building_face_highlight(const BuildingRenderBuffer& buffer, std::size_t cell, std::uint32_t slot) {
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();
    rlDisableDepthMask();
    constexpr Color tint{255, 200, 60, 110};
    for (const auto& tri : buffer.triangles) {
        if (tri.cell != cell || tri.slot != slot) continue;
        const auto v = [&](int i) { const auto& p = buffer.vertices[tri.indices[i]].position; return Vector3{p.x, p.y, p.z}; };
        DrawTriangle3D(v(0), v(1), v(2), tint);
        DrawTriangle3D(v(2), v(1), v(0), tint);
    }
    rlDrawRenderBatchActive();
    rlEnableDepthMask();
    rlEnableBackfaceCulling();
}

// One piece voxel's drawn faces into the active mask. Piece geometry is in world
// space and is drawn 0.25 above its cell (draw_city_piece_voxels).
void draw_city_piece_mask(const EditorProject& project, CityGridPoint cell) {
    constexpr auto unit_size = 4.0F;
    const auto x0 = static_cast<float>(cell.x) * unit_size;
    const auto y0 = static_cast<float>(cell.y) * unit_size;
    const auto z0 = static_cast<float>(cell.z) * unit_size;
    const auto& points = mask_triangles(mask_key(2, cell.x, cell.y, cell.z), project.portal_render_buffer,
        [&](Vec3 p) { return inside_cell_box(p, x0, y0, z0); });
    if (!draw_mask_triangles(points, {0.0F, 0.25F, 0.0F})) {
        DrawCubeV({x0 + unit_size * 0.5F, y0 + 0.25F + unit_size * 0.5F, z0 + unit_size * 0.5F}, {unit_size, unit_size, unit_size}, WHITE);
    }
}

// A road junction's road and curb tiles (the ones it placed) into the active mask.
void draw_city_intersection_mask(const MapScene& scene, std::size_t intersection) {
    std::unordered_set<std::uint64_t> cells;
    const auto cell_key = [](std::int32_t x, std::int32_t y, std::int32_t z) {
        return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 42U) ^
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y) & 0x1fffffU) << 21U) ^
            static_cast<std::uint64_t>(static_cast<std::uint32_t>(z) & 0x1fffffU);
    };
    const auto& points = mask_triangles(mask_key(3, static_cast<std::int64_t>(intersection)), scene.city_tile_buffer, [&](Vec3 p) {
        if (cells.empty()) {
            for (const auto& tile : scene.city_tiles) {
                if (tile.intersection_index == static_cast<std::int32_t>(intersection)) cells.insert(cell_key(tile.x, tile.y, tile.z));
            }
        }
        // Tile cells run from y*4+0.25 to y*4+4.25.
        return cells.contains(cell_key(static_cast<std::int32_t>(std::floor(p.x / 4.0F)),
            static_cast<std::int32_t>(std::floor((p.y - 0.25F) / 4.0F)), static_cast<std::int32_t>(std::floor(p.z / 4.0F))));
    });
    draw_mask_triangles(points, {});
}

void draw_outline_from_mask(RenderTexture2D& mask, Vector2 viewport_size, Color tint) {
    auto& outline = mask_outline_shader();
    if (outline.shader.id == 0 || mask.texture.id == 0U || viewport_size.x <= 0.0F || viewport_size.y <= 0.0F) {
        return;
    }

    const float texel_size[2]{
        1.0F / static_cast<float>(std::max(1, mask.texture.width)),
        1.0F / static_cast<float>(std::max(1, mask.texture.height)),
    };
    const float color[4]{
        static_cast<float>(tint.r) / 255.0F,
        static_cast<float>(tint.g) / 255.0F,
        static_cast<float>(tint.b) / 255.0F,
        static_cast<float>(tint.a) / 255.0F,
    };
    SetShaderValue(outline.shader, outline.texel_size_loc, texel_size, SHADER_UNIFORM_VEC2);
    SetShaderValue(outline.shader, outline.color_loc, color, SHADER_UNIFORM_VEC4);

    BeginBlendMode(BLEND_ALPHA);
    BeginShaderMode(outline.shader);
    DrawTexturePro(
        mask.texture,
        Rectangle{0.0F, 0.0F, static_cast<float>(mask.texture.width), -static_cast<float>(mask.texture.height)},
        Rectangle{0.0F, 0.0F, viewport_size.x, viewport_size.y},
        Vector2{0.0F, 0.0F},
        0.0F,
        WHITE);
    EndShaderMode();
    EndBlendMode();
}

void draw_gpu_building_model(
    GpuBuildingRenderModel& model,
    Vector3 origin,
    std::int32_t rotation,
    bool draw_wires,
    GpuSurface surface = GpuSurface::Building,
    bool room_glass = false
) {
    const std::vector<Matrix> transform{building_transform_matrix(origin, rotation)};
    set_game_render_depth_output(true);
    // Near-view meshes: their window parts are room glass, drawn see-through last.
    draw_gpu_mesh_batch({{&model.mesh, transform}}, surface, room_glass ? GpuPartFilter::Opaque : GpuPartFilter::All);
    set_game_render_depth_output(false);
    if (room_glass) draw_gpu_mesh_batch({{&model.mesh, transform}}, surface, GpuPartFilter::Glass);

    if (!draw_wires) {
        return;
    }
    const auto wire = Color{40, 45, 50, 255};
    for (const auto& line : model.wire_lines) {
        DrawLine3D(transform_buffer_vertex(line.from, origin, rotation), transform_buffer_vertex(line.to, origin, rotation), wire);
    }
    if (g_gfx_debug_stats != nullptr && !model.wire_lines.empty()) {
        g_gfx_debug_stats->add_draw(
            GfxDebugDrawCategory::WireOverlay,
            0U,
            static_cast<std::uint64_t>(model.wire_lines.size()) * 2U,
            static_cast<std::uint64_t>(model.wire_lines.size()));
    }
}

namespace {

// The view the work grids size their lines for (set_grid_view).
struct GridView {
    Vector3 position{};
    float pixels_per_radian{800.0F};
};
GridView g_grid_view;

struct GridLineStyle {
    Color color;
    float pixels;  // on-screen width
};

// A flat work grid. Every line is a quad lying in the plane, the same width on
// screen at any distance, split per cell so its width and fade follow the view.
// It is drawn twice: normally where nothing covers it, and faintly behind geometry,
// so it still shows through walls without drowning them out.
struct WorkGrid {
    float y{};
    float unit{4.0F};
    std::int32_t x0{}, x1{}, z0{}, z1{};  // cell range, lines at x0..x1 and z0..z1
    Vector2 fade_center{};
    float fade_radius{};  // 0 = no fade
    std::function<GridLineStyle(bool along_x, std::int32_t line)> style;
};

void emit_grid_quad(Vector3 a, Vector3 b, Vector3 side, Color ca, Color cb) {
    rlColor4ub(ca.r, ca.g, ca.b, ca.a); rlVertex3f(a.x - side.x, a.y, a.z - side.z);
    rlColor4ub(cb.r, cb.g, cb.b, cb.a); rlVertex3f(b.x - side.x, b.y, b.z - side.z);
    rlColor4ub(cb.r, cb.g, cb.b, cb.a); rlVertex3f(b.x + side.x, b.y, b.z + side.z);
    rlColor4ub(ca.r, ca.g, ca.b, ca.a); rlVertex3f(a.x - side.x, a.y, a.z - side.z);
    rlColor4ub(cb.r, cb.g, cb.b, cb.a); rlVertex3f(b.x + side.x, b.y, b.z + side.z);
    rlColor4ub(ca.r, ca.g, ca.b, ca.a); rlVertex3f(a.x + side.x, a.y, a.z + side.z);
}

float grid_fade(const WorkGrid& grid, float x, float z) {
    if (grid.fade_radius <= 0.0F) return 1.0F;
    const auto d = std::hypot(x - grid.fade_center.x, z - grid.fade_center.y) / grid.fade_radius;
    return std::clamp((1.0F - d) / 0.4F, 0.0F, 1.0F);
}

void emit_work_grid(const WorkGrid& grid, float alpha_scale) {
    rlBegin(RL_TRIANGLES);
    const auto half_width = [&](Vector3 p, float pixels) {
        const auto distance = std::max(0.5F, Vector3Distance(p, g_grid_view.position));
        return 0.5F * pixels * distance / g_grid_view.pixels_per_radian;
    };
    const auto faded = [&](Color c, float x, float z) {
        return Color{c.r, c.g, c.b, static_cast<unsigned char>(static_cast<float>(c.a) * alpha_scale * grid_fade(grid, x, z))};
    };
    for (int pass = 0; pass < 2; ++pass) {
        const bool along_x = pass == 0;  // lines of constant z run along x
        const auto first = along_x ? grid.z0 : grid.x0;
        const auto last = along_x ? grid.z1 : grid.x1;
        for (auto line = first; line <= last; ++line) {
            const auto style = grid.style(along_x, line);
            if (style.color.a == 0) continue;
            const auto fixed = static_cast<float>(line) * grid.unit;
            const auto begin = along_x ? grid.x0 : grid.z0;
            const auto end = along_x ? grid.x1 : grid.z1;
            for (auto step = begin; step < end; ++step) {
                const auto s0 = static_cast<float>(step) * grid.unit;
                const auto s1 = s0 + grid.unit;
                const Vector3 a = along_x ? Vector3{s0, grid.y, fixed} : Vector3{fixed, grid.y, s0};
                const Vector3 b = along_x ? Vector3{s1, grid.y, fixed} : Vector3{fixed, grid.y, s1};
                const auto w = half_width(Vector3Lerp(a, b, 0.5F), style.pixels);
                const Vector3 side = along_x ? Vector3{0.0F, 0.0F, w} : Vector3{w, 0.0F, 0.0F};
                const auto ca = faded(style.color, a.x, a.z);
                const auto cb = faded(style.color, b.x, b.z);
                if (ca.a == 0 && cb.a == 0) continue;
                emit_grid_quad(a, b, side, ca, cb);
            }
        }
    }
    rlEnd();
}

// Draws the grid over the scene without writing depth: at full strength in front of
// geometry, and at a fifth behind it.
void draw_work_grid(const WorkGrid& grid, const std::function<void(float)>& extras = {}) {
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();
    rlDisableDepthMask();
    rlEnableDepthTest();
    emit_work_grid(grid, 1.0F);
    if (extras) extras(1.0F);
    rlDrawRenderBatchActive();
    glDepthFunc(GL_GREATER);
    emit_work_grid(grid, 0.2F);
    if (extras) extras(0.2F);
    rlDrawRenderBatchActive();
    glDepthFunc(GL_LEQUAL);
    rlEnableDepthMask();
    rlEnableBackfaceCulling();
}

std::int32_t floor_mod(std::int32_t value, std::int32_t m) {
    return ((value % m) + m) % m;
}

constexpr Color kGridMinor{205, 218, 232, 46};
constexpr Color kGridMajor{120, 180, 255, 120};
constexpr Color kGridAxisX{238, 88, 82, 215};
constexpr Color kGridAxisZ{82, 150, 245, 215};

} // namespace

void set_grid_view(const Camera3D& camera, float viewport_height) {
    g_grid_view.position = camera.position;
    const auto half_fov = std::tan(camera.fovy * DEG2RAD * 0.5F);
    g_grid_view.pixels_per_radian = viewport_height * 0.5F / std::max(half_fov, 1e-3F);
}

void draw_sbb_scene_for_app(
    const SbbBuilding& building,
    GpuBuildingRenderModel& model,
    const SbbCell* selected,
    std::uint32_t guide_floor) {
    constexpr auto unit_size = 4.0F;
    constexpr std::uint32_t grid_cells = 64;
    const auto floor = std::min(guide_floor, building.size_z > 0U ? building.size_z - 1U : 0U);
    const auto grid_y = (static_cast<float>(floor) - static_cast<float>(building.anchor_y)) * unit_size + 0.02F;

    // The building mesh is relative to the anchor, like the picker. It is the game's
    // near view (rooms, doorways, glass), as if you were standing next to it.
    draw_gpu_building_model(model, {}, 0, false, GpuSurface::Building, true);

    // editor.exe's fixed 64-cell work grid (65 lines per axis) at the guide floor.
    // Every fourth line groups blocks. The anchor's lines are the X (red) and Z (blue)
    // axes, and the saved footprint is outlined in amber.
    WorkGrid grid;
    grid.unit = unit_size;
    grid.y = grid_y;
    grid.x0 = -static_cast<std::int32_t>(building.anchor_x);
    grid.x1 = static_cast<std::int32_t>(grid_cells) - static_cast<std::int32_t>(building.anchor_x);
    grid.z0 = -static_cast<std::int32_t>(building.anchor_z);
    grid.z1 = static_cast<std::int32_t>(grid_cells) - static_cast<std::int32_t>(building.anchor_z);
    // Fade the far rows so the distant lines don't blur into a solid sheet.
    grid.fade_center = {(static_cast<float>(building.size_x) * 0.5F - static_cast<float>(building.anchor_x)) * unit_size,
                        (static_cast<float>(building.size_y) * 0.5F - static_cast<float>(building.anchor_z)) * unit_size};
    grid.fade_radius = (std::max(building.size_x, building.size_y) * 0.5F + 20.0F) * unit_size;
    grid.style = [](bool along_x, std::int32_t line) {
        if (line == 0) return GridLineStyle{along_x ? kGridAxisX : kGridAxisZ, 2.4F};
        if (floor_mod(line, 4) == 0) return GridLineStyle{kGridMajor, 1.6F};
        return GridLineStyle{kGridMinor, 1.0F};
    };
    const auto size_x = static_cast<std::int32_t>(building.size_x);
    const auto size_z = static_cast<std::int32_t>(building.size_y);
    draw_work_grid(grid, [&](float alpha_scale) {
        WorkGrid footprint;
        footprint.unit = unit_size;
        footprint.y = grid_y + 0.01F;
        footprint.x0 = grid.x0; footprint.x1 = grid.x0 + size_x;
        footprint.z0 = grid.z0; footprint.z1 = grid.z0 + size_z;
        footprint.style = [&](bool along_x, std::int32_t line) {
            const bool edge = along_x ? (line == footprint.z0 || line == footprint.z1) : (line == footprint.x0 || line == footprint.x1);
            return GridLineStyle{edge ? Color{240, 170, 60, 235} : Color{0, 0, 0, 0}, 2.6F};
        };
        emit_work_grid(footprint, alpha_scale);
    });

    // The selected cell is outlined from its real faces (draw_building_cell_mask).
    (void)selected;
}

// Item sets placed the way the game places them (setup_item_sets / entry_pose):
// each item stands on the centre of the cell's floor, turned with the set in quarter
// turns around Y. Sets whose items have no data/item mesh keep a gold diamond marker.
// Draws the items of one set, untextured and shaded by facing, around `origin` (the
// centre of a cell's floor) turned by `quarter_turns`. False when none of its item
// meshes are loaded.
bool draw_item_set_items(const EditorProject& project, const std::vector<SisItem>& items, Vector3 origin, std::uint32_t quarter_turns) {
    const float angle = static_cast<float>(quarter_turns & 3U) * PI * 0.5F;
    const float c = std::cos(angle), s = std::sin(angle);
    const auto turn = [&](Vec3f v) { return Vec3f{v.x * c - v.z * s, v.y, v.x * s + v.z * c}; };
    bool drawn = false;
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();
    rlBegin(RL_TRIANGLES);
    for (const auto& item : items) {
        const auto mesh = project.item_meshes.find(lower_name(item.type));
        if (mesh == project.item_meshes.end()) continue;
        drawn = true;
        const auto p = turn(item.pos);
        const std::array<Vec3f, 3> r{turn(item.rot[0]), turn(item.rot[1]), turn(item.rot[2])};
        const auto world = [&](std::uint32_t i) {
            const auto l = i < mesh->second.vertices.size() ? mesh->second.vertices[i] : Vec3f{};
            return Vector3{origin.x + p.x + r[0].x * l.x + r[1].x * l.y + r[2].x * l.z,
                origin.y + p.y + r[0].y * l.x + r[1].y * l.y + r[2].y * l.z,
                origin.z + p.z + r[0].z * l.x + r[1].z * l.y + r[2].z * l.z};
        };
        for (const auto& face : mesh->second.faces) {
            if (face.size() < 3) continue;
            const auto a = world(face[0]), b = world(face[1]), d = world(face[2]);
            // No texture: shade by how much the face points up.
            const auto n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(d, a)));
            const auto light = static_cast<unsigned char>(120.0F + 100.0F * std::fabs(n.y) + 30.0F * std::fabs(n.x));
            rlColor4ub(light, light, static_cast<unsigned char>(light * 0.9F), 255);
            for (std::size_t k = 1; k + 1 < face.size(); ++k) {
                const auto v1 = world(face[k]), v2 = world(face[k + 1]);
                rlVertex3f(a.x, a.y, a.z); rlVertex3f(v1.x, v1.y, v1.z); rlVertex3f(v2.x, v2.y, v2.z);
            }
        }
    }
    rlEnd();
    // rlgl draws its batch later, so flush it while culling is still off. Otherwise
    // the items draw with culling on, and .it3 faces wind the other way, so only
    // their back faces showed (they looked inside out).
    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();
    return drawn;
}

void draw_building_item_set_guides(const EditorProject& project, const SbbBuilding& building, std::optional<std::uint32_t> floor) {
    constexpr float unit = 4.0F;
    constexpr Color color{250, 196, 75, 255};
    for (const auto& cell : building.cells) {
        if (cell.item_set_index == 0U || (floor && cell.y != *floor)) continue;
        const float x = (static_cast<float>(cell.x) - building.anchor_x) * unit + unit * 0.5F;
        const float y = (static_cast<float>(cell.y) - building.anchor_y) * unit;
        const float z = (static_cast<float>(cell.z) - building.anchor_z) * unit + unit * 0.5F;
        const auto name_index = sbb_item_set_name_index(cell.item_set_index);
        const auto set = name_index < building.item_set_names.size()
            ? project.item_sets.find(lower_name(building.item_set_names[name_index])) : project.item_sets.end();
        if (set != project.item_sets.end() &&
            draw_item_set_items(project, set->second, {x, y, z}, sbb_item_set_rotation(cell.item_set_index))) continue;
        const float top = y + unit + 0.08F;
        const Vector3 north{x, top, z - 1.3F};
        const Vector3 east{x + 1.3F, top, z};
        const Vector3 south{x, top, z + 1.3F};
        const Vector3 west{x - 1.3F, top, z};
        DrawLine3D(north, east, color);
        DrawLine3D(east, south, color);
        DrawLine3D(south, west, color);
        DrawLine3D(west, north, color);
    }
}

const RenderTexture2D* item_set_thumbnail(EditorProject& project, const std::string& name) {
    const auto key = lower_name(name);
    const auto set = project.item_sets.find(key);
    if (set == project.item_sets.end()) return nullptr;
    if (const auto found = project.item_set_thumbnails.find(key); found != project.item_set_thumbnails.end()) return &found->second;
    if (project.thumbnail_budget <= 0) return nullptr;
    --project.thumbnail_budget;
    // Frame the items themselves, over a patch of floor under them.
    Vector3 lo{FLT_MAX, FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const auto& item : set->second) {
        const auto mesh = project.item_meshes.find(lower_name(item.type));
        if (mesh == project.item_meshes.end()) continue;
        for (const auto& l : mesh->second.vertices) {
            const Vector3 v{item.pos.x + item.rot[0].x * l.x + item.rot[1].x * l.y + item.rot[2].x * l.z,
                item.pos.y + item.rot[0].y * l.x + item.rot[1].y * l.y + item.rot[2].y * l.z,
                item.pos.z + item.rot[0].z * l.x + item.rot[1].z * l.y + item.rot[2].z * l.z};
            lo = Vector3Min(lo, v);
            hi = Vector3Max(hi, v);
        }
    }
    if (lo.x > hi.x) lo = hi = {};
    const auto center = Vector3Scale(Vector3Add(lo, hi), 0.5F);
    const float radius = std::max(0.5F, Vector3Distance(lo, hi) * 0.5F);
    const float dist = radius * 1.9F;
    Camera3D cam{};
    cam.position = {center.x + dist * 0.7F, center.y + dist * 0.6F, center.z + dist * 0.7F};
    cam.target = center;
    cam.up = {0.0F, 1.0F, 0.0F};
    cam.fovy = 45.0F;
    cam.projection = CAMERA_PERSPECTIVE;
    auto& target = project.item_set_thumbnails[key];
    target = LoadRenderTexture(128, 128);
    begin_game_texture_mode(target);
    ClearBackground(Color{32, 36, 41, 255});
    BeginMode3D(cam);
    rlEnableDepthTest();
    DrawPlane({center.x, std::min(0.0F, lo.y) - 0.01F, center.z}, {radius * 3.0F, radius * 3.0F}, Color{70, 74, 80, 255});
    if (!draw_item_set_items(project, set->second, {}, 0U)) {
        DrawCube({0.0F, 0.6F, 0.0F}, 1.2F, 1.2F, 1.2F, Color{250, 196, 75, 255});
    }
    EndMode3D();
    end_game_texture_mode();
    return &target;
}

void invalidate_building_thumbnail(EditorProject& project, std::size_t index) {
    if (index < project.building_thumbnail_ready.size()) {
        project.building_thumbnail_ready[index] = false;
    }
}

// Rebuilds and uploads just one building's city mesh, instead of marking the whole
// project as changed (which reloads every building). Used when one building
// template is edited in the Buildings tab.
void rebuild_single_building_render(EditorProject& project, std::size_t index) {
    if (index >= project.buildings.size()) {
        return;
    }
    // Loading owns older snapshots. Rebuild the current assets after it finishes, so a
    // queued upload can't replace this edit with old geometry.
    if (project.render_build_in_progress || !project.pending_gpu_upload_indices.empty()) {
        project.render_buffers_dirty = true;
        return;
    }
    if (project.building_render_buffers.size() != project.buildings.size()) {
        // A new building uploads here, outside the queue. Count it as uploaded so the
        // loading screen (uploaded vs. buffer count) doesn't wait forever.
        if (project.pending_gpu_upload_index >= project.building_render_buffers.size()) {
            project.pending_gpu_upload_index = project.buildings.size();
        }
        project.building_render_buffers.resize(project.buildings.size());
    }
    if (project.building_gpu_models.size() != project.buildings.size()) {
        project.building_gpu_models.resize(project.buildings.size());
    }
    const auto blocks = block_meshes(project);
    const auto templates = buildblock_templates(project);
    project.building_render_buffers[index] =
        build_city_building_full_render_buffer(project.buildings[index].data, blocks, templates, BuildingLod::Far);
    project.building_gpu_models[index] =
        upload_building_render_model(project.building_render_buffers[index], project.buildings[index].data, project);
    keep_pick_data_only(project.building_render_buffers[index]);
}

const RenderTexture2D* building_thumbnail(EditorProject& project, std::size_t index) {
    if (index >= project.buildings.size()) {
        return nullptr;
    }
    if (project.building_thumbnails.size() != project.buildings.size()) {
        project.building_thumbnails.resize(project.buildings.size());
        project.building_thumbnail_ready.assign(project.buildings.size(), false);
    }
    if (project.building_thumbnail_ready[index]) {
        return &project.building_thumbnails[index];
    }
    if (project.thumbnail_budget <= 0) {
        return nullptr;
    }
    --project.thumbnail_budget;

    constexpr int kSize = 192; // draw bigger than shown, so it stays sharp when shrunk.
    constexpr float unit_size = 4.0F;
    auto& building = project.buildings[index].data;

    // Reuse the city GPU model if it is already uploaded. Only build a fresh copy on
    // the CPU for buildings whose render model isn't ready yet.
    GpuBuildingRenderModel local_model;
    GpuBuildingRenderModel* model_ptr = nullptr;
    if (index < project.building_gpu_models.size() && !project.building_gpu_models[index].empty()) {
        model_ptr = &project.building_gpu_models[index];
    } else {
        const auto& blocks = block_meshes(project);
        const auto& templates = buildblock_templates(project);
        const auto buffer = build_native_building_render_buffer(building, blocks, templates);
        local_model = upload_building_render_model(buffer, building, project);
        model_ptr = &local_model;
    }
    auto& model = *model_ptr;

    auto& target = project.building_thumbnails[index];
    if (target.id == 0U) {
        target = LoadRenderTexture(kSize, kSize);
    }

    // Frame the whole building from a three-quarter view.
    const float sx = static_cast<float>(std::max(1U, building.size_x)) * unit_size;
    const float sy = static_cast<float>(std::max(1U, building.size_z)) * unit_size;
    const float sz = static_cast<float>(std::max(1U, building.size_y)) * unit_size;
    const float r = 0.5F * std::sqrt(sx * sx + sy * sy + sz * sz);
    const Vector3 center{
        (static_cast<float>(building.size_x) * 0.5F - static_cast<float>(building.anchor_x)) * unit_size,
        (static_cast<float>(building.size_z) * 0.5F - static_cast<float>(building.anchor_y)) * unit_size,
        (static_cast<float>(building.size_y) * 0.5F - static_cast<float>(building.anchor_z)) * unit_size,
    };
    const float dist = std::max(unit_size * 2.0F, r * 2.4F);
    Camera3D cam{};
    cam.position = {center.x + dist * 0.7F, center.y + dist * 0.6F, center.z + dist * 0.7F};
    cam.target = center;
    cam.up = {0.0F, 1.0F, 0.0F};
    cam.fovy = 45.0F;
    cam.projection = CAMERA_PERSPECTIVE;

    begin_game_texture_mode(target);
    ClearBackground(Color{32, 36, 41, 255});
    BeginMode3D(cam);
    set_game_surface_camera(cam.position);
    rlEnableDepthTest();
    // Building meshes are relative to the anchor, like the picker and the city.
    draw_gpu_building_model(model, {}, 0, false);
    // A faint ground plane gives the building a sense of size and direction.
    DrawGrid(static_cast<int>(std::max(building.size_x, building.size_y)) + 2, unit_size);
    EndMode3D();
    end_game_texture_mode();
    GenTextureMipmaps(&target.texture);
    SetTextureFilter(target.texture, TEXTURE_FILTER_BILINEAR);

    project.building_thumbnail_ready[index] = true;
    return &target;
}

const RenderTexture2D* city_block_thumbnail(EditorProject& project, std::size_t index) {
    if (!project.city || index >= project.city->data.city_blocks.size()) {
        return nullptr;
    }
    if (project.city_block_thumbnails.size() != project.city->data.city_blocks.size()) {
        project.city_block_thumbnails.resize(project.city->data.city_blocks.size());
        project.city_block_thumbnail_ready.assign(project.city->data.city_blocks.size(), false);
    }
    if (project.city_block_thumbnail_ready[index]) {
        return &project.city_block_thumbnails[index];
    }
    if (project.thumbnail_budget <= 0) {
        return nullptr;
    }
    --project.thumbnail_budget;

    constexpr int kSize = 192;
    const auto meshes = block_meshes(project);
    const auto scene = build_city_block_preview_scene(project.city->data.city_blocks[index], meshes);

    auto& target = project.city_block_thumbnails[index];
    if (target.id == 0U) {
        target = LoadRenderTexture(kSize, kSize);
    }

    const float unit = scene.unit_size;
    const float r = unit * 6.0F;
    const Vector3 center{unit, unit, unit};
    const float dist = r * 2.2F;
    Camera3D cam{};
    cam.position = {center.x + dist * 0.7F, center.y + dist * 0.6F, center.z + dist * 0.7F};
    cam.target = center;
    cam.up = {0.0F, 1.0F, 0.0F};
    cam.fovy = 45.0F;
    cam.projection = CAMERA_PERSPECTIVE;

    begin_game_texture_mode(target);
    ClearBackground(Color{32, 36, 41, 255});
    BeginMode3D(cam);
    set_game_surface_camera(cam.position);
    rlEnableDepthTest();
    if (scene.solid_cell.has_value()) {
        draw_render_buffer_faces(scene.solid_cell->buffer, {120, 160, 210, 255});
    }
    for (const auto& piece : scene.pieces) {
        draw_mesh_faces(piece.mesh, piece.origin, scene.unit_size, {120, 160, 210, 255});
    }
    EndMode3D();
    end_game_texture_mode();
    GenTextureMipmaps(&target.texture);
    SetTextureFilter(target.texture, TEXTURE_FILTER_BILINEAR);

    project.city_block_thumbnail_ready[index] = true;
    return &target;
}

const LoadedAsset<SbbBuilding>* find_building_asset(const EditorProject& project, std::string_view name) {
    // Archive file names are identities. A shared display name must never take a
    // placement away from an exact asset-name match.
    const auto key = lower_name(name);
    const auto exact = std::ranges::find_if(project.buildings, [&](const auto& asset) {
        return lower_name(asset.name) == key;
    });
    if (exact != project.buildings.end()) return &*exact;
    const LoadedAsset<SbbBuilding>* alias = nullptr;
    for (const auto& asset : project.buildings) if (lower_name(asset.data.name) == key) {
        if (alias != nullptr) return nullptr; // An old display name that matches more than one asset.
        alias = &asset;
    }
    return alias;
}

const std::vector<std::optional<std::size_t>>& resolve_placement_asset_indices(EditorProject& project) {
    const auto building_count = project.city ? project.city->data.buildings.size() : 0U;
    // Placements find their building by name, so the key is the placement names. Any
    // edit, undo, or template swap that renames or reorders placements changes it.
    std::uint64_t fingerprint = 1469598103934665603ULL;
    if (project.city) {
        for (const auto& placement : project.city->data.buildings) {
            fingerprint = (fingerprint ^ std::hash<std::string>{}(placement.name)) * 1099511628211ULL;
        }
    }
    if (!project.placement_assets_dirty &&
        project.placement_asset_indices.size() == building_count &&
        project.placement_asset_indices_building_count == project.buildings.size() &&
        project.placement_asset_indices_fingerprint == fingerprint) {
        return project.placement_asset_indices;
    }

    project.placement_asset_indices.clear();
    project.placement_asset_indices.reserve(building_count);
    if (project.city) {
        for (const auto& placement : project.city->data.buildings) {
            const auto* asset = find_building_asset(project, placement.name);
            project.placement_asset_indices.push_back(
                asset != nullptr ? std::optional<std::size_t>(static_cast<std::size_t>(asset - project.buildings.data())) : std::nullopt);
        }
    }
    project.placement_asset_indices_building_count = project.buildings.size();
    project.placement_asset_indices_fingerprint = fingerprint;
    project.placement_assets_dirty = false;
    return project.placement_asset_indices;
}

const GpuMesh* building_near_mesh(EditorProject& project, std::size_t asset_index, int& build_budget) {
    if (asset_index >= project.buildings.size() || asset_index >= project.building_gpu_models.size()) return nullptr;
    const auto far_vao = project.building_gpu_models[asset_index].mesh.vao;
    if (auto found = project.building_near_models.find(asset_index); found != project.building_near_models.end()) {
        if (found->second.far_vao == far_vao) {
            found->second.last_used = GetTime();
            return &found->second.mesh;
        }
        project.building_near_models.erase(found);
    }
    if (auto job = project.building_near_jobs.find(asset_index); job != project.building_near_jobs.end()) {
        // Deferred work (the web build) runs in get() below.
        if (job->second.buffers.wait_for(std::chrono::seconds{0}) == std::future_status::timeout) return nullptr;
        auto built = job->second.buffers.get();
        const bool stale = job->second.far_vao != far_vao;
        project.building_near_jobs.erase(job);
        // An edit replaced the shell while this was building; start again below.
        if (!stale) {
            const auto& building = project.buildings[asset_index].data;
            EditorProject::NearBuildingModel model;
            model.mesh = upload_gpu_mesh(built.mesh, [&](GpuMeshPart& part) {
                describe_textured_part(part,
                    texture_for_material(project, building, part.material_id),
                    material_texture_for_material(project, building, part.material_id));
            });
            model.room_shadow_volume = upload_shadow_volume(built.room_shadow_vertices, built.room_shadow_indices);
            if (!model.room_shadow_volume.empty()) model.room_shadow_volume.parts.push_back({0U, model.room_shadow_volume.index_count});
            model.last_used = GetTime();
            model.far_vao = far_vao;
            return &project.building_near_models.insert_or_assign(asset_index, std::move(model)).first->second.mesh;
        }
    }
    if (build_budget <= 0) return nullptr;
    --build_budget;
    project.building_near_jobs[asset_index] = {far_vao, std::async(
#ifdef __EMSCRIPTEN__
        std::launch::deferred,  // no pthreads in the browser build: std::thread aborts
#else
        std::launch::async,
#endif
        [building = project.buildings[asset_index].data, blocks = block_meshes(project), templates = buildblock_templates(project)] {
            PAFTA_PROFILE_THREAD("Near building build");
            auto buffer = build_city_building_full_render_buffer(building, blocks, templates, BuildingLod::Near);
            BuildingRenderBuffer rooms;
            rooms.vertices = buffer.vertices;
            for (auto triangle : buffer.triangles) {
                if (!triangle.interior || triangle.city_window_surface) continue;
                triangle.interior = false;
                rooms.triangles.push_back(triangle);
            }
            EditorProject::NearBuildingBuffers built;
            built.mesh = std::move(buffer);
            if (!rooms.triangles.empty()) {
                bake_shadow_volume(rooms);
                built.room_shadow_vertices = pack_shadow_vertices(rooms.shadow_vertices);
                built.room_shadow_indices = std::move(rooms.shadow_indices);
            }
            return built;
        })};
    return nullptr;
}

void evict_building_near_models(EditorProject& project) {
    const auto now = GetTime();
    std::erase_if(project.building_near_models, [now](const auto& entry) { return now - entry.second.last_used > 5.0; });
}

void refresh_placement_override_models(EditorProject& project) {
    if (!project.city) {
        project.placement_override_models.clear();
        return;
    }
    // Wait for the template models to finish loading, because overrides copy them.
    if (project.render_build_in_progress || !project.pending_gpu_upload_indices.empty()) return;
    const auto& asset_indices = resolve_placement_asset_indices(project);
    std::uint64_t fingerprint = 1469598103934665603ULL;
    const auto mix = [&](std::uint64_t v) { fingerprint = (fingerprint ^ v) * 1099511628211ULL; };
    for (const auto& placement : project.city->data.buildings) {
        mix(std::hash<std::string>{}(placement.name));
        mix(static_cast<std::uint32_t>(placement.x));
        mix(static_cast<std::uint32_t>(placement.y));
        mix(static_cast<std::uint32_t>(placement.z));
        mix(static_cast<std::uint32_t>(placement.rotation));
    }
    // A template model that was uploaded again gets a new vao.
    for (const auto& model : project.building_gpu_models) mix(model.mesh.vao);
    // Cells the game's world storage can't hold (GameWorldFit) are hidden too.
    const auto* dropped = project.city_scene_cache_valid ? &project.city_scene_cache.world_fit.building_cells : nullptr;
    // The scene key changes even when different dropped cells have the same count;
    // counts alone can't tell this geometry apart.
    if (dropped != nullptr) mix(project.city_scene_cache_fingerprint);
    if (fingerprint == project.placement_override_fingerprint) return;
    project.placement_override_fingerprint = fingerprint;
    project.placement_override_models.clear();

    std::vector<const SbbBuilding*> buildings(asset_indices.size());
    for (std::size_t i = 0; i < asset_indices.size(); ++i) {
        if (asset_indices[i].has_value()) buildings[i] = &project.buildings[*asset_indices[i]].data;
    }
    auto overwritten = overwritten_building_cells(project.city->data, buildings);
    std::vector<std::size_t> later_placements(overwritten.size());
    for (std::size_t i = 0; i < overwritten.size(); ++i) {
        later_placements[i] = overwritten[i].size();
        if (dropped != nullptr && i < dropped->size()) overwritten[i].insert(overwritten[i].end(), (*dropped)[i].begin(), (*dropped)[i].end());
    }
    for (std::size_t i = 0; i < overwritten.size(); ++i) {
        if (overwritten[i].empty()) continue;
        auto building = *buildings[i];
        for (const auto c : overwritten[i]) building.cells[c].occupied = false;
        const auto buffer = build_city_building_full_render_buffer(building, block_meshes(project), buildblock_templates(project), BuildingLod::Far);
        project.placement_override_models.emplace(i, upload_building_render_model(buffer, building, project));
        const auto& p = project.city->data.buildings[i];
        append_render_log(project, "placement " + std::to_string(i) + " " + p.name + " at " + std::to_string(p.x) + "," +
            std::to_string(p.y) + "," + std::to_string(p.z) + ": " + std::to_string(later_placements[i]) +
            " cells overwritten by later placements, " + std::to_string(overwritten[i].size() - later_placements[i]) +
            " past the game's world storage");
    }
}

void draw_street_object_shadow_volumes(const MapScene& scene, const Camera3D& camera, float max_distance, Vector3 light);

void draw_city_shadow_volumes(EditorProject& project, const Camera3D& camera, float max_distance) {
    if (!project.city) return;
    refresh_placement_override_models(project);
    const auto& placement_asset_indices = resolve_placement_asset_indices(project);

    rlDrawRenderBatchActive();
    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xff);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    glStencilFunc(GL_ALWAYS, 0, 0xff);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);
    glEnable(GL_DEPTH_TEST);
    glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_KEEP);
    glStencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_KEEP);
    // A face turned away from the sun keeps its front cap on its own surface, where
    // equal depths pass or fail at random, which shows as streaks on walls the sun
    // grazes. Push the volumes a little deeper (scaled by slope) so that cap always fails
    // and counts the same as the back cap. The face's own lighting already darkens it.
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0F, 4.0F);

    const auto light = Vector3Normalize(game_light_direction());
    const auto sector_extrusion = [&](std::size_t sector) {
        return Vector3Scale(light, -static_cast<float>(sector + 1U) * 32.0F / std::max(light.y, 0.125F));
    };
    // View frustum planes (the 3D pass's matrices are current). A volume is skipped when
    // its building, stretched along the shadow direction, is outside a plane.
    std::array<std::array<float, 4>, 6> planes{};
    {
        const auto m = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
        const std::array<std::array<float, 4>, 6> raw{{
            {m.m3 + m.m0, m.m7 + m.m4, m.m11 + m.m8, m.m15 + m.m12}, {m.m3 - m.m0, m.m7 - m.m4, m.m11 - m.m8, m.m15 - m.m12},
            {m.m3 + m.m1, m.m7 + m.m5, m.m11 + m.m9, m.m15 + m.m13}, {m.m3 - m.m1, m.m7 - m.m5, m.m11 - m.m9, m.m15 - m.m13},
            {m.m3 + m.m2, m.m7 + m.m6, m.m11 + m.m10, m.m15 + m.m14}, {m.m3 - m.m2, m.m7 - m.m6, m.m11 - m.m10, m.m15 - m.m14},
        }};
        for (std::size_t i = 0; i < 6; ++i) {
            const auto length = std::sqrt(raw[i][0] * raw[i][0] + raw[i][1] * raw[i][1] + raw[i][2] * raw[i][2]);
            for (std::size_t k = 0; k < 4; ++k) planes[i][k] = length > 1e-8F ? raw[i][k] / length : 0.0F;
        }
    }
    const auto swept_visible = [&](Vector3 center, float radius, Vector3 extrusion) {
        const auto end = Vector3Add(center, extrusion);
        for (const auto& p : planes) {
            const auto a = p[0] * center.x + p[1] * center.y + p[2] * center.z + p[3];
            const auto b = p[0] * end.x + p[1] * end.y + p[2] * end.z + p[3];
            if (std::max(a, b) < -radius) return false;
        }
        return true;
    };
    // Group placements by (asset, top sector). Each group is one instanced draw of the
    // asset's volume with that sector's stretch length.
    std::unordered_map<std::size_t, std::vector<Matrix>> groups;
    for (std::size_t placement_index = 0; placement_index < project.city->data.buildings.size(); ++placement_index) {
        const auto& placement = project.city->data.buildings[placement_index];
        if (!placement_asset_indices[placement_index].has_value()) continue;
        const auto asset_index = *placement_asset_indices[placement_index];
        if (asset_index >= project.building_gpu_models.size()) continue;
        const auto origin = native_city_building_render_origin(placement, 4.0F);
        const auto camera_delta = Vector3Subtract({origin.x, origin.y, origin.z}, camera.position);
        if (max_distance > 0.0F && Vector3LengthSqr(camera_delta) > max_distance * max_distance) continue;
        // Sectors stretch (sector y + 1) * 32 / max(light.y, 0.125), which is far
        // enough to reach the ground from the sector's height. A placement uses its
        // top sector.
        const auto& building = project.buildings[asset_index].data;
        const auto top_cell = placement.y + static_cast<std::int32_t>(building.size_z) - static_cast<std::int32_t>(building.anchor_y);
        const auto top_sector = static_cast<std::size_t>(std::max(0, top_cell) / 8);
        {
            // The bounding sphere of the building's cells, stretched along the shadow.
            const auto half = Vector3Scale({static_cast<float>(building.size_x + 1U), static_cast<float>(building.size_z),
                static_cast<float>(building.size_y + 1U)}, 2.0F);
            const auto local_center = Vec3{half.x - static_cast<float>(building.anchor_x) * 4.0F,
                half.y - static_cast<float>(building.anchor_y) * 4.0F, half.z - static_cast<float>(building.anchor_z) * 4.0F};
            const auto center = native_city_building_transform_point(placement, local_center, 4.0F);
            if (!swept_visible({center.x, center.y, center.z}, Vector3Length(half),
                    sector_extrusion(std::min<std::size_t>(top_sector, 63U)))) continue;
        }
        if (const auto found = project.placement_override_models.find(placement_index); found != project.placement_override_models.end()) {
            set_shadow_volume_light(light, sector_extrusion(std::min<std::size_t>(top_sector, 63U)));
            draw_gpu_mesh_instances(found->second.shadow_volume, {city_placement_matrix(placement)}, GpuSurface::ShadowVolume);
            continue;
        }
        groups[asset_index * 64U + std::min<std::size_t>(top_sector, 63U)].push_back(city_placement_matrix(placement));
    }
    for (const auto& [key, transforms] : groups) {
        set_shadow_volume_light(light, sector_extrusion(key % 64U));
        draw_gpu_mesh_instances(project.building_gpu_models[key / 64U].shadow_volume, transforms, GpuSurface::ShadowVolume);
    }
    // Rooms in near sectors cast shadows too, stretched a fixed 32 units (0x2787d4),
    // not by sector height (RenderQueuedShadowVolumeAdjacencyPass @946d0).
    std::unordered_map<std::size_t, std::vector<Matrix>> rooms_by_asset;
    for (const auto& [asset_index, transform] : near_lod_placements()) rooms_by_asset[asset_index].push_back(transform);
    set_shadow_volume_light(light, Vector3Scale(light, -32.0F));
    for (const auto& [asset_index, transforms] : rooms_by_asset) {
        if (const auto found = project.building_near_models.find(asset_index); found != project.building_near_models.end()) {
            draw_gpu_mesh_instances(found->second.room_shadow_volume, transforms, GpuSurface::ShadowVolume);
        }
    }
    // Road and curb tiles are world sectors too: one volume at the highest sector.
    // Both world volumes only draw the chunks whose shadow can reach the view.
    const auto road_extrusion = sector_extrusion(static_cast<std::size_t>(project.city_tile_gpu_model.shadow_top / 32.0F));
    set_shadow_volume_light(light, road_extrusion);
    g_shadow_cull = ShadowCull{planes, road_extrusion};
    draw_gpu_mesh_instances(project.city_tile_gpu_model.shadow_volume, {MatrixIdentity()}, GpuSurface::ShadowVolume);
    // The game's queued district meshes stretch 32 units (0x2787d4).
    // Vertices are already in world space, so they don't need moving.
    set_shadow_volume_light(light, Vector3Scale(light, -32.0F));
    g_shadow_cull = ShadowCull{planes, Vector3Scale(light, -32.0F)};
    draw_gpu_mesh_instances(project.portal_gpu_model.shadow_volume, {MatrixIdentity()}, GpuSurface::ShadowVolume);
    g_shadow_cull.reset();
    if (g_render_settings.street_objects) draw_street_object_shadow_volumes(project.city_scene_cache, camera, max_distance, light);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glDisable(GL_STENCIL_TEST);
}

void composite_city_shadows(Vector2 viewport_size) {
    rlDrawRenderBatchActive();
    const auto framebuffer = rlGetActiveFramebuffer();
    const auto resource = game_render_resources().find(framebuffer);
    if (resource == game_render_resources().end() || resource->second.shadow_factor.id == 0U) {
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0xff);
        return;
    }

    // The game's shaders read a screen-space shadow factor texture. Keep the finished
    // scene colour in attachment 0 and write the stencil result into a separate factor
    // attachment, instead of painting black over the frame. The final pass reads this
    // factor after all scene drawing.
    GLint scene_color = 0;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &scene_color);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_2D,
        resource->second.shadow_factor.id,
        0);
    const GLenum shadow_only[] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, shadow_only);
    glDisable(GL_STENCIL_TEST);
    constexpr GLfloat fully_lit[] = {1.0F, 1.0F, 1.0F, 1.0F};
    glClearBufferfv(GL_COLOR, 0, fully_lit);

    glEnable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glStencilMask(0x00);
    glStencilFunc(GL_NOTEQUAL, 0, 0xff);
    DrawRectangle(
        0,
        0,
        static_cast<int>(viewport_size.x),
        static_cast<int>(viewport_size.y),
        Color{0, 0, 0, 255});
    rlDrawRenderBatchActive();

    const GLenum color_only[] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, color_only);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, static_cast<GLuint>(scene_color), 0);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0xff);
    glEnable(GL_DEPTH_TEST);
}

GpuBuildingRenderModel upload_portal_render_model(const BuildingRenderBuffer& buffer, const EditorProject& project, bool textured) {
    GpuBuildingRenderModel model;
    model.wire_lines = buffer.wire_lines;
    build_shadow_volume(model, buffer, true);
    model.mesh = upload_gpu_mesh(buffer, [&](GpuMeshPart& part) {
        describe_textured_part(part,
            textured ? texture_for_buffer_material(project, buffer, part.material_id) : nullptr,
            material_texture_for_buffer_material(project, buffer, part.material_id));
    });
    model.uploaded_bytes = model.mesh.vertex_count * sizeof(GpuVertex) + model.mesh.index_count * sizeof(std::uint32_t);
    return model;
}

GpuBuildingRenderModel& portal_render_model(EditorProject& project, bool textured, std::optional<std::size_t> selected_portal) {
    PAFTA_PROFILE_FUNCTION();
    // References into the project's cached render inputs (built once at load, not every
    // frame). Only the background-rebuild branch below copies them, and only when a
    // rebuild is actually queued, so the normal path allocates nothing. v10 district
    // pieces point into the game's absolute texture table (built-ins, then CSX in
    // archive order), not the editor's load order; see project_grid_texture_names.
    const auto& blocks = block_meshes(project);
    const auto& bb_templates = buildblock_templates(project);
    const auto& tex_names = project_grid_texture_names(project);
    if (selected_portal.has_value()) {
        if (project.portal_preview_render_dirty ||
            project.portal_preview_render_textured != textured ||
            project.portal_preview_index != selected_portal) {
            PAFTA_PROFILE_BLOCK("portal_render_model rebuild selected portal preview");
            project.portal_preview_render_buffer = build_native_portal_render_buffer(project.city->data, blocks, selected_portal, bb_templates, tex_names, resolve_placement_buildings(project));
            project.portal_preview_gpu_model = upload_portal_render_model(project.portal_preview_render_buffer, project, textured);
            release_shadow_volume(project.portal_preview_render_buffer);
            PAFTA_PROFILE_END_BLOCK;
            project.portal_preview_index = selected_portal;
            project.portal_preview_render_textured = textured;
            project.portal_preview_render_dirty = false;
        }
        return project.portal_preview_gpu_model;
    }

    // Start a worker to rebuild the (possibly large) buffer of all portals when the
    // pieces change. The old mesh keeps drawing until the new one is ready, so placing a
    // piece never holds up the frame. Rebuilding every piece is slow, so wait for a paint
    // stroke to finish instead of rebuilding for every cell it stamps.
    if ((project.portal_render_dirty || project.portal_render_textured != textured) &&
        !project.portal_rebuild_in_progress && !project.defer_heavy_rebuilds) {
        PAFTA_PROFILE_BLOCK("portal_render_model queue async rebuild");
#ifdef __EMSCRIPTEN__
        // The browser build has no pthreads (std::async aborts there), so
        // rebuild right here.
        project.portal_render_dirty = false;
        project.portal_render_textured = textured;
        project.portal_render_buffer = build_native_portal_render_buffer(project.city->data, blocks, std::nullopt, bb_templates, tex_names, resolve_placement_buildings(project));
        project.portal_gpu_model = upload_portal_render_model(project.portal_render_buffer, project, textured);
        release_shadow_volume(project.portal_render_buffer);
#else
        const auto map = project.city->data;
        const auto blocks_copy = blocks;
        const auto bb_copy = bb_templates;
        const auto tex_copy = tex_names;
        // The worker owns copies of the placed buildings.
        std::vector<std::optional<SbbBuilding>> buildings_copy;
        for (const auto* building : resolve_placement_buildings(project)) {
            buildings_copy.push_back(building == nullptr ? std::nullopt : std::optional<SbbBuilding>{*building});
        }
        project.portal_rebuild_in_progress = true;
        project.portal_render_dirty = false;
        project.portal_render_textured = textured;
        project.portal_rebuild_future = std::async(std::launch::async, [map, blocks_copy, bb_copy, tex_copy,
                                                                        buildings_copy = std::move(buildings_copy)]() {
            PAFTA_PROFILE_THREAD("Portal render rebuild");
            PAFTA_PROFILE_BLOCK("build async portal render buffer");
            std::vector<const SbbBuilding*> buildings;
            for (const auto& building : buildings_copy) buildings.push_back(building ? &*building : nullptr);
            auto buffer = build_native_portal_render_buffer(map, blocks_copy, std::nullopt, bb_copy, tex_copy, buildings);
            bake_shadow_volume(buffer);
            return buffer;
        });
#endif
        PAFTA_PROFILE_END_BLOCK;
    }
    if (project.portal_rebuild_future.has_value() &&
        project.portal_rebuild_future->wait_for(std::chrono::milliseconds{0}) == std::future_status::ready) {
        PAFTA_PROFILE_BLOCK("portal_render_model adopt async rebuild");
        auto buffer = project.portal_rebuild_future->get();
        project.portal_rebuild_future.reset();
        project.portal_rebuild_in_progress = false;
        // An edit or texture-mode change while the worker ran needs a new build.
        // Keep the last accepted mesh until that one is ready.
        if (!project.portal_render_dirty && project.portal_render_textured == textured) {
            project.portal_render_buffer = std::move(buffer);
            project.portal_gpu_model = upload_portal_render_model(project.portal_render_buffer, project, textured);
            release_shadow_volume(project.portal_render_buffer);
        }
        PAFTA_PROFILE_END_BLOCK;
    }
    return project.portal_gpu_model;
}

GpuBuildingRenderModel& building_tool_floor_preview_model(
    EditorProject& project,
    std::size_t building_index,
    std::optional<std::uint32_t> floor,
    bool roofs
) {
    if (project.building_floor_preview_dirty ||
        project.building_floor_preview_building != building_index ||
        project.building_floor_preview_floor != floor ||
        project.building_floor_preview_roofs != roofs) {
        const auto& blocks = block_meshes(project);
        const auto& templates = buildblock_templates(project);
        project.building_floor_preview_render_buffer = select_building_lod(build_native_building_render_buffer(
            project.buildings[building_index].data,
            blocks,
            templates,
            floor,
            roofs), BuildingLod::Near);
        project.building_floor_preview_gpu_model = upload_building_render_model(
            project.building_floor_preview_render_buffer,
            project.buildings[building_index].data,
            project);
        release_shadow_volume(project.building_floor_preview_render_buffer);
        project.building_floor_preview_building = building_index;
        project.building_floor_preview_floor = floor;
        project.building_floor_preview_roofs = roofs;
        project.building_floor_preview_dirty = false;
    }
    return project.building_floor_preview_gpu_model;
}

void draw_city_piece_voxels(
    EditorProject& project,
    bool textured,
    std::optional<std::size_t> selected_portal,
    bool draw_wires,
    bool isolate_selected) {
    if (!project.city) {
        return;
    }
    auto& model = portal_render_model(project, textured, isolate_selected ? selected_portal : std::nullopt);
    // The game's world cells sit 0.25 above their level. Without that lift, the piece's
    // ground cells sit in the same plane as their stamped ground and flicker against it.
    draw_gpu_building_model(model, {0.0F, 0.25F, 0.0F}, 0, draw_wires);

    // The caller outlines the selected or hovered voxel (draw_city_piece_mask).
}

// A glow box over a single piece voxel (one world grid cell), so pieces highlight
// one at a time instead of as the whole 8x8x8 portal they are stored in.

// Fallback colours without textures for the road textures (asphalt2 / concrete).
Color city_tile_material_color(const BuildingRenderBuffer& buffer, std::uint32_t material) {
    const auto name = material < buffer.material_names.size() ? std::string_view{buffer.material_names[material]} : "city_road_asphalt";
    const auto rgb = procedural_material_rgb(name).value_or(*procedural_material_rgb("city_road_asphalt"));
    return {rgb[0], rgb[1], rgb[2], 255};
}

Color terrain_material_color(const BuildingRenderBuffer&, std::uint32_t) {
    const auto rgb = *procedural_material_rgb("city_terrain_grass");
    return {rgb[0], rgb[1], rgb[2], 255};
}

// Road tiles are lit by the vertex normals of each face, and the terrain by smoothed
// normals with the landscape shading. Both are in world space and drawn once.
void draw_uploaded_city_model(GpuBuildingRenderModel& model, bool terrain = false) {
    set_game_render_depth_output(true);
    draw_gpu_mesh_instances(model.mesh, {MatrixIdentity()}, terrain ? GpuSurface::Terrain : GpuSurface::World);
    set_game_render_depth_output(false);
}

void unload_city_render_model(GpuBuildingRenderModel& model) {
    model = GpuBuildingRenderModel{};
}

GpuBuildingRenderModel upload_city_tile_model(const BuildingRenderBuffer& buffer, const EditorProject& project) {
    // The game draws road textures without tint. The colour only stands in for a texture
    // the map folder doesn't have.
    auto model = upload_city_buffer_model(buffer, project, true, [&buffer](std::uint32_t material) {
        return city_tile_material_color(buffer, material);
    });
    build_shadow_volume(model, buffer, true);
    for (auto& part : model.mesh.parts) {
        if (part.texture != 0U) part.color = WHITE;
    }
    return model;
}

// The landscape detail map (_landscape_generatetexture @d4360, tex_detail):
// 1024x1024 grey noise. Texel (x, y) comes from n = y*57 + x, n ^= n << 13,
// v = ((n*n*15731 + 789221)^2 - 0x2df722f3) & 0x7fffffff (the game squares where
// the usual hash multiplies by n), grey = int(16 - v * 2^-26 + 192).
// Its alpha (a cosine pattern) isn't used by the landscape shader.
Texture2D landscape_detail_texture() {
    static const Texture2D texture = [] {
        constexpr int size = 1024;
        Image image = GenImageColor(size, size, WHITE);
        auto* pixels = static_cast<Color*>(image.data);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                auto n = static_cast<std::uint32_t>(y * 57 + x);
                n ^= n << 13U;
                auto h = n * n * 15731U + 789221U;
                h = (h * h - 0x2df722f3U) & 0x7fffffffU;
                const auto grey = static_cast<unsigned char>(
                    static_cast<int>(16.0F - static_cast<float>(h) * 1.4901161e-08F + 192.0F));
                pixels[y * size + x] = Color{grey, grey, grey, 255};
            }
        }
        auto loaded = LoadTextureFromImage(image);
        UnloadImage(image);
        GenTextureMipmaps(&loaded);
        SetTextureWrap(loaded, TEXTURE_WRAP_REPEAT);
        SetTextureFilter(loaded, TEXTURE_FILTER_TRILINEAR);
        return loaded;
    }();
    return texture;
}

GpuBuildingRenderModel upload_city_terrain_model(const BuildingRenderBuffer& buffer, const EditorProject& project) {
    auto model = upload_city_buffer_model(buffer, project, false, [&buffer](std::uint32_t material) {
        return terrain_material_color(buffer, material);
    });
    // Landscape: the generated detail map times the flat generated colour map.
    for (auto& part : model.mesh.parts) part.texture = landscape_detail_texture().id;
    return model;
}

void draw_city_scene(
    EditorProject& project,
    const MapScene& scene,
    std::optional<std::size_t> selected_street,
    bool render_roads,
    bool render_terrain) {
    if (g_gfx_debug_stats != nullptr) {
        if (scene.terrain.has_value()) {
            g_gfx_debug_stats->counts.terrain_vertices = scene.terrain->mesh.vertices.size();
            g_gfx_debug_stats->counts.terrain_samples = scene.terrain->mesh.vertices.size();
        }
        g_gfx_debug_stats->counts.city_tiles = scene.city_tiles.size();
        g_gfx_debug_stats->counts.traffic_light_poles = scene.traffic_light_poles.size();
    }

    if (render_terrain) {
        GfxDebugMaybeTimer timer{GfxDebugTimingPhase::TerrainDraw};
        draw_uploaded_city_model(project.terrain_gpu_model, true);
    }

    {
        GfxDebugMaybeTimer timer{GfxDebugTimingPhase::CityRoadDraw};
        if (render_roads) {
            draw_uploaded_city_model(project.city_tile_gpu_model);
        }
    }

    for (const auto& road : scene.roads) {
        const auto selected = selected_street == road.source_index && project.city &&
            road.source_index < project.city->data.streets.size() && road.source == project.city->data.streets[road.source_index];
        if (selected) {
            const auto lift = scene.unit_size * 1.1F;
            const auto pulse = 0.5F + 0.5F * static_cast<float>(std::sin(GetTime() * 4.0));
            const auto glow = Color{255, 215, 90, static_cast<unsigned char>(170 + 70 * pulse)};
            const auto line = Color{255, 235, 140, 255};

            const auto height_at = [&](float t) {
                return road.from.y + (road.to.y - road.from.y) * t + lift;
            };
            const auto seg_len = std::sqrt(
                (road.to.x - road.from.x) * (road.to.x - road.from.x) +
                (road.to.z - road.from.z) * (road.to.z - road.from.z));
            const auto steps = std::clamp(static_cast<int>(seg_len / scene.unit_size), 1, 256);
            auto prev = Vector3{road.from.x, height_at(0.0F), road.from.z};
            for (int i = 1; i <= steps; ++i) {
                const auto t = static_cast<float>(i) / static_cast<float>(steps);
                const auto x = road.from.x + (road.to.x - road.from.x) * t;
                const auto z = road.from.z + (road.to.z - road.from.z) * t;
                const auto cur = Vector3{x, height_at(t), z};
                DrawLine3D(prev, cur, line);
                prev = cur;
            }
            DrawSphere({road.from.x, height_at(0.0F), road.from.z},
                scene.unit_size * (0.32F + 0.08F * pulse), glow);
            DrawSphere({road.to.x, height_at(1.0F), road.to.z},
                scene.unit_size * (0.32F + 0.08F * pulse), glow);
            if (g_gfx_debug_stats != nullptr) {
                g_gfx_debug_stats->add_draw(GfxDebugDrawCategory::WireOverlay, 0U,
                    static_cast<std::uint64_t>(steps) * 2U + 4U, static_cast<std::uint64_t>(steps) + 2U);
            }
        }
    }
}

void draw_city_editor_overlay(const EditorProject&, const MapScene& scene) {
    GfxDebugMaybeTimer timer{GfxDebugTimingPhase::CityRoadDraw};
    constexpr float marker_size = 2.0F;
    const auto node_area_color = Color{76, 139, 214, 85};
    const auto segment_color = Color{255, 225, 112, 100};
    const auto marker_fill = Color{76, 139, 214, 170};
    const auto marker_wire = Color{218, 236, 255, 235};

    const auto draw_quad = [](const std::array<Vec3, 4>& quad, Color color) {
        DrawTriangle3D(to_raylib(quad[0]), to_raylib(quad[1]), to_raylib(quad[2]), color);
        DrawTriangle3D(to_raylib(quad[0]), to_raylib(quad[2]), to_raylib(quad[3]), color);
    };

    for (const auto& intersection : scene.intersections) {
        draw_quad(intersection.editor_quad, node_area_color);
    }
    for (const auto& road : scene.roads) {
        draw_quad(road.editor_quad, segment_color);
    }

    for (const auto& intersection : scene.intersections) {
        auto position = to_raylib(intersection.position);
        position.y = static_cast<float>(intersection.source.y) * scene.unit_size;
        const Vector3 size{marker_size, marker_size, marker_size};
        DrawCubeV(position, size, marker_fill);
        DrawCubeWiresV(position, size, marker_wire);
    }

    if (g_gfx_debug_stats != nullptr) {
        const auto marker_count = static_cast<std::uint64_t>(scene.intersections.size());
        const auto quad_count = marker_count + static_cast<std::uint64_t>(scene.roads.size());
        g_gfx_debug_stats->add_draw(
            GfxDebugDrawCategory::WireOverlay,
            marker_count * 12U + quad_count * 2U,
            marker_count * 60U + quad_count * 6U,
            marker_count * 2U + quad_count * 2U);
    }
}

// Street objects (World_InitResourcePoolsAndCatalogs @13dcd0). Each type is a set
// of boxes centred on their position (BuildBlock_GenerateCubeVerticesFaces @3fb90),
// which GeneratedModel_BuildFromStaticTableSet675f @5b840 turns straight into the
// render model. Every face samples texel (0,0), except the -X face of the last two
// boxes of types 1 and 4, which use the whole texture. Type 1 is the traffic light
// (post, arm, two signal heads; the arm points along local -Z), and type 4 is the
// street sign, drawn with the light's transform. Box centres are relative to the
// post's centre, 3 above where the pole meets the ground.
struct StreetObjectBox {
    Vector3 center;
    Vector3 size;
    bool textured;  // -X face carries the full texture
};
constexpr std::array<StreetObjectBox, 4> kTrafficLightBoxes{{
    {{0.0F, 0.0F, 0.0F}, {0.375F, 6.0F, 0.375F}, false},
    {{0.0F, 3.0F, -5.0F}, {0.25F, 0.25F, 10.0F}, false},
    {{0.0F, 3.0F, -6.0F}, {0.3125F, 1.0F, 0.5F}, true},
    {{0.0F, 3.0F, -10.0F}, {0.3125F, 1.0F, 0.5F}, true},
}};
constexpr std::array<StreetObjectBox, 1> kStreetSignBoxes{{
    {{0.0F, 2.625F, -8.0F}, {0.0625F, 0.5F, 2.0F}, true},
}};

template <std::size_t N>
BuildingRenderBuffer build_street_object_buffer(const std::array<StreetObjectBox, N>& boxes) {
    // Cube corners DAT_002b34f0 and face corners DAT_002b3660 (face 2 is -X,
    // corners 4,7,3,0), centred on the box.
    static constexpr std::array<Vector3, 8> corners{{
        {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}, {0, 1, 0}, {1, 1, 0}, {1, 1, 1}, {0, 1, 1},
    }};
    static constexpr std::array<std::array<std::uint32_t, 4>, 6> faces{{
        {3, 2, 1, 0}, {7, 6, 2, 3}, {4, 7, 3, 0}, {5, 4, 0, 1}, {6, 5, 1, 2}, {4, 5, 6, 7},
    }};
    static constexpr std::array<std::array<float, 2>, 4> face_uv{{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
    BuildingRenderBuffer buffer;
    for (const auto& box : boxes) {
        for (std::size_t face = 0; face < faces.size(); ++face) {
            const auto base = static_cast<std::uint32_t>(buffer.vertices.size());
            for (std::size_t i = 0; i < 4; ++i) {
                const auto& c = corners[faces[face][i]];
                const bool mapped = box.textured && face == 2;
                buffer.vertices.push_back({{
                    box.center.x + (c.x - 0.5F) * box.size.x,
                    box.center.y + (c.y - 0.5F) * box.size.y + 3.0F,
                    box.center.z + (c.z - 0.5F) * box.size.z,
                }, mapped ? face_uv[i][0] : 0.0F, mapped ? face_uv[i][1] : 0.0F});
            }
            buffer.triangles.push_back({{base, base + 1U, base + 2U}, 0U, false});
            buffer.triangles.push_back({{base, base + 2U, base + 3U}, 0U, false});
        }
    }
    buffer.material_runs.push_back({0U, 0U, static_cast<std::uint32_t>(buffer.triangles.size()), false});
    return buffer;
}

// A street object's render mesh plus its shadow-volume faces, already turned for
// the four pole directions (arm toward -Z, +X, +Z, -X), which placements then share
// by moving them. Models cast 64-unit volumes
// (RenderQueuedModelWithAdjacencyGeometryPass @124fe0, DAT_00278814).
struct StreetObjectModel {
    GpuMesh mesh;
    std::array<GpuBuildingRenderModel, 4> shadow;
};

float street_object_yaw(Vec3 arm_direction) {
    // The arm points along -Z. raylib's +Y turn is right-handed, so X is negated to
    // point it along arm_direction (the same handedness flip as buildings).
    return std::atan2(-arm_direction.x, -arm_direction.z);
}

std::size_t street_object_direction(Vec3 arm_direction) {
    if (arm_direction.x > 0.5F) return 1U;
    if (arm_direction.z > 0.5F) return 2U;
    if (arm_direction.x < -0.5F) return 3U;
    return 0U;
}

template <std::size_t N>
StreetObjectModel build_street_object_model(const std::array<StreetObjectBox, N>& boxes) {
    auto buffer = build_street_object_buffer(boxes);
    StreetObjectModel model;
    model.mesh = upload_gpu_mesh(buffer, [](GpuMeshPart& part) {
        part.material_texture = default_game_material_texture().id;
    }, true);
    constexpr std::array<Vec3, 4> arms{{{0, 0, -1}, {1, 0, 0}, {0, 0, 1}, {-1, 0, 0}}};
    for (std::size_t direction = 0; direction < arms.size(); ++direction) {
        auto rotated = buffer;
        const auto rotation = MatrixRotateY(street_object_yaw(arms[direction]));
        for (auto& vertex : rotated.vertices) {
            const auto p = Vector3Transform({vertex.position.x, vertex.position.y, vertex.position.z}, rotation);
            vertex.position = {p.x, p.y, p.z};
        }
        build_shadow_volume(model.shadow[direction], rotated);
    }
    return model;
}

StreetObjectModel& traffic_light_model() {
    static StreetObjectModel model = build_street_object_model(kTrafficLightBoxes);
    return model;
}

StreetObjectModel& street_sign_model() {
    static StreetObjectModel model = build_street_object_model(kStreetSignBoxes);
    return model;
}

void draw_street_object_shadow_volumes(const MapScene& scene, const Camera3D& camera, float max_distance, Vector3 light) {
    std::array<std::vector<Matrix>, 4> by_direction;
    for (const auto& pole : scene.traffic_light_poles) {
        if (max_distance > 0.0F &&
            Vector3DistanceSqr({pole.base.x, pole.base.y, pole.base.z}, camera.position) > max_distance * max_distance) {
            continue;
        }
        by_direction[street_object_direction(pole.arm_direction)].push_back(MatrixTranslate(pole.base.x, pole.base.y, pole.base.z));
    }
    set_shadow_volume_light(light, Vector3Scale(light, -64.0F));
    for (std::size_t direction = 0; direction < by_direction.size(); ++direction) {
        if (by_direction[direction].empty()) continue;
        for (auto* model : {&traffic_light_model(), &street_sign_model()}) {
            draw_gpu_mesh_instances(model->shadow[direction].shadow_volume, by_direction[direction], GpuSurface::ShadowVolume);
        }
    }
}

// The red signal (_generatetexture @9a8cc, texture DAT_43ebf074): 128x128 dark grey
// (32,32,32) with alpha 32 (a lit surface), and the lamp at columns 40..87, rows
// 84..107 in red (255,32,32) with alpha 255 (drawn without lighting). Rows go
// bottom-up in v, so the lamp is in the top half of the head. Yellow and green move
// the lamp to rows 52..75 and 20..43. The editor shows the state the map loads with.
Texture2D traffic_light_signal_texture() {
    static const Texture2D texture = [] {
        Image image = GenImageColor(128, 128, Color{32, 32, 32, 32});
        auto* pixels = static_cast<Color*>(image.data);
        for (int y = 84; y <= 107; ++y) {
            for (int x = 40; x <= 87; ++x) pixels[y * 128 + x] = Color{255, 32, 32, 255};
        }
        auto loaded = LoadTextureFromImage(image);
        UnloadImage(image);
        GenTextureMipmaps(&loaded);
        SetTextureFilter(loaded, TEXTURE_FILTER_TRILINEAR);
        return loaded;
    }();
    return texture;
}

// Street-name label (RenderStreetLabelTexture @9da10): 256x64, cleared to
// (0, 0.25, 0, 0), with the name in white in font 4 ("overpass", a 16x16 atlas of
// 64px cells with widths in a CSV) at size 52, centred on (128, 32). Alpha stays 0
// so the sign is lit. The font is in the game's install (<game>/font), two folders
// above a map folder; without it the label uses raylib's font.
struct GameFont {
    Image atlas{};
    std::array<float, 256> width{};  // advance per unit size: (base - 0.5) / cell height
    int cell{64};
};

// raw_widths: fonts with flag 1 (fixedsys) use the CSV widths as they are; the
// others subtract half a pixel (Font_LoadAtlasMetricsAndCursorTexture).
std::optional<GameFont> load_game_font(const std::filesystem::path& map_root, const char* name, bool raw_widths = false) {
    for (const auto& dir : {map_root.parent_path().parent_path() / "font", map_root.parent_path() / "font"}) {
        std::ifstream csv(dir / (std::string(name) + ".csv"));
        if (!csv) continue;
        GameFont font;
        std::array<int, 256> base{};
        float cell_height = 64.0F;
        std::string line;
        while (std::getline(csv, line)) {
            int index = 0;
            int value = 0;
            if (std::sscanf(line.c_str(), "Char %d Base Width,%d", &index, &value) == 2 && index >= 0 && index < 256) {
                base[static_cast<std::size_t>(index)] = value;
            } else if (std::sscanf(line.c_str(), "Cell Height,%d", &value) == 1) {
                cell_height = static_cast<float>(value);
                font.cell = value;
            }
        }
        for (std::size_t c = 0; c < 256; ++c) font.width[c] = (static_cast<float>(base[c]) - (raw_widths ? 0.0F : 0.5F)) / cell_height;
        font.atlas = LoadImage((dir / (std::string(name) + ".png")).string().c_str());
        if (font.atlas.data == nullptr) continue;
        ImageFormat(&font.atlas, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        return font;
    }
    return std::nullopt;
}

Texture2D render_street_label(const char* text, const GameFont* font) {
    constexpr int width = 256;
    constexpr int height = 64;
    constexpr float size = 52.0F;
    // How much of each pixel the glyphs cover, top-down, laid over the green below.
    Image glyphs = GenImageColor(width, height, BLANK);
    if (font != nullptr) {
        float total = 0.0F;
        for (const char* c = text; *c != '\0'; ++c) total += size * font->width[static_cast<unsigned char>(*c)];
        float x = width * 0.5F - total * 0.5F;
        const float y = height * 0.5F - size * 0.5F;
        const auto* atlas = static_cast<const Color*>(font->atlas.data);
        auto* out = static_cast<Color*>(glyphs.data);
        for (const char* c = text; *c != '\0'; ++c) {
            const auto code = static_cast<unsigned char>(*c);
            const float glyph_width = size * font->width[code];
            const int cell_x = (code & 15) * font->cell;
            const int cell_y = (code >> 4) * font->cell;
            for (int py = static_cast<int>(y); py < static_cast<int>(y + size) && py < height; ++py) {
                for (int px = static_cast<int>(x); px < static_cast<int>(x + glyph_width) && px < width; ++px) {
                    if (px < 0 || py < 0) continue;
                    const int sx = cell_x + static_cast<int>((static_cast<float>(px) + 0.5F - x) / size * static_cast<float>(font->cell));
                    const int sy = cell_y + static_cast<int>((static_cast<float>(py) + 0.5F - y) / size * static_cast<float>(font->cell));
                    if (sx < 0 || sy < 0 || sx >= font->atlas.width || sy >= font->atlas.height) continue;
                    const auto texel = atlas[sy * font->atlas.width + sx];
                    auto& dst = out[py * width + px];
                    dst.a = std::max(dst.a, texel.a);
                }
            }
            x += glyph_width;
        }
    } else {
        ImageDrawTextEx(&glyphs, GetFontDefault(), text, {8.0F, 16.0F}, 32.0F, 2.0F, WHITE);
    }
    Image label = GenImageColor(width, height, Color{0, 64, 0, 0});
    auto* dst = static_cast<Color*>(label.data);
    const auto* src = static_cast<const Color*>(glyphs.data);
    for (int i = 0; i < width * height; ++i) {
        const float a = static_cast<float>(src[i].a) / 255.0F;
        dst[i].r = static_cast<unsigned char>(255.0F * a);
        dst[i].g = static_cast<unsigned char>(64.0F + (255.0F - 64.0F) * a);
        dst[i].b = static_cast<unsigned char>(255.0F * a);
    }
    UnloadImage(glyphs);
    // The label was drawn y-down into a framebuffer, so v runs bottom-up.
    ImageFlipVertical(&label);
    auto texture = LoadTextureFromImage(label);
    UnloadImage(label);
    GenTextureMipmaps(&texture);
    SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);
    return texture;
}

// Every light is an instance of one model; signs are grouped by street name.
void draw_city_traffic_light_poles(const EditorProject& project, const MapScene& scene) {
    auto& light = traffic_light_model().mesh;
    auto& sign = street_sign_model().mesh;
    static std::filesystem::path label_root;
    static std::array<Texture2D, native_street_names.size()> labels{};
    if (label_root != project.root) {
        label_root = project.root;
        auto font = load_game_font(project.root, "overpass");
        for (std::size_t i = 0; i < labels.size(); ++i) {
            if (labels[i].id != 0U) UnloadTexture(labels[i]);
            labels[i] = render_street_label(native_street_names[i], font ? &*font : nullptr);
        }
        if (font) UnloadImage(font->atlas);
    }
    light.parts.front().texture = traffic_light_signal_texture().id;

    static std::vector<Matrix> transforms;
    static std::array<std::vector<Matrix>, native_street_names.size()> by_name;
    transforms.clear();
    for (auto& group : by_name) group.clear();
    for (const auto& pole : scene.traffic_light_poles) {
        const auto transform = MatrixMultiply(MatrixRotateY(street_object_yaw(pole.arm_direction)),
            MatrixTranslate(pole.base.x, pole.base.y, pole.base.z));
        transforms.push_back(transform);
        by_name[static_cast<std::size_t>(std::clamp(pole.sign_name, 0, static_cast<int>(by_name.size()) - 1))].push_back(transform);
    }
    set_game_render_depth_output(true);
    draw_gpu_mesh_instances(light, transforms, GpuSurface::Model);
    for (std::size_t i = 0; i < by_name.size(); ++i) {
        sign.parts.front().texture = labels[i].id;
        draw_gpu_mesh_instances(sign, by_name[i], GpuSurface::Model);
    }
    set_game_render_depth_output(false);
}

void draw_city_cursor(CityGridPoint point, float unit_size, CityTool tool) {
    const auto position = Vector3{
        static_cast<float>(point.x) * unit_size,
        static_cast<float>(point.y) * unit_size + unit_size * 0.5F,
        static_cast<float>(point.z) * unit_size,
    };
    auto color = Color{235, 190, 78, 230};
    if (tool == CityTool::Road) {
        color = Color{82, 196, 130, 230};
    } else if (tool == CityTool::Piece) {
        color = Color{91, 166, 206, 230};
    }
    DrawCubeWiresV(position, {unit_size, unit_size, unit_size}, color);
    DrawSphere(position, unit_size * 0.22F, color);
}

// The placement grid around the cursor for the city tools. `center` is the cell
// (piece, building) or grid point (road node) the tool will use. The plane sits on
// the floor of world cells at level*4 + 0.25, where roads, pieces, and buildings
// rest. Lines every 8 cells mark district sectors, the lines through the cursor are
// its X (red) and Z (blue) axes, and `cell_fill` tints the cell.
void draw_city_edit_grid(CityGridPoint center, const MapScene& scene, Color accent, bool cell_fill) {
    const auto unit = scene.unit_size > 0.0F ? scene.unit_size : 4.0F;
    constexpr std::int32_t half_cells = 24;
    WorkGrid grid;
    grid.unit = unit;
    grid.y = static_cast<float>(center.y) * unit + 0.25F + 0.03F;
    grid.x0 = center.x - half_cells;
    grid.x1 = center.x + half_cells;
    grid.z0 = center.z - half_cells;
    grid.z1 = center.z + half_cells;
    grid.fade_center = {(static_cast<float>(center.x) + (cell_fill ? 0.5F : 0.0F)) * unit,
        (static_cast<float>(center.z) + (cell_fill ? 0.5F : 0.0F)) * unit};
    grid.fade_radius = static_cast<float>(half_cells) * unit;
    grid.style = [&](bool along_x, std::int32_t line) {
        const auto cursor_line = along_x ? center.z : center.x;
        if (!cell_fill && line == cursor_line) return GridLineStyle{along_x ? kGridAxisX : kGridAxisZ, 2.4F};
        if (floor_mod(line, 8) == 0) return GridLineStyle{kGridMajor, 1.8F};
        return GridLineStyle{kGridMinor, 1.1F};
    };
    draw_work_grid(grid, [&](float alpha_scale) {
        if (!cell_fill) return;
        // The target cell: a tinted tile with a sharp coloured border.
        const auto x0 = static_cast<float>(center.x) * unit;
        const auto z0 = static_cast<float>(center.z) * unit;
        const Vector3 c{x0 + unit * 0.5F, grid.y, z0 + unit * 0.5F};
        const Color fill{accent.r, accent.g, accent.b, static_cast<unsigned char>(60.0F * alpha_scale)};
        DrawTriangle3D({x0, grid.y, z0}, {x0, grid.y, z0 + unit}, {x0 + unit, grid.y, z0 + unit}, fill);
        DrawTriangle3D({x0, grid.y, z0}, {x0 + unit, grid.y, z0 + unit}, {x0 + unit, grid.y, z0}, fill);
        (void)c;
        WorkGrid border;
        border.unit = unit;
        border.y = grid.y + 0.01F;
        border.x0 = center.x; border.x1 = center.x + 1;
        border.z0 = center.z; border.z1 = center.z + 1;
        border.style = [&](bool, std::int32_t) {
            return GridLineStyle{Color{accent.r, accent.g, accent.b, 235}, 2.6F};
        };
        emit_work_grid(border, alpha_scale);
    });
}

void draw_city_block_preview(EditorProject& project, const SbcCityBlock& city_block, const std::vector<NamedBlockMesh>& meshes, std::size_t city_block_index, std::size_t selected_layer, std::size_t selected_slot) {
    DrawGrid(24, 4.0F);
    if (!project.cached_city_block_preview_index.has_value() ||
        *project.cached_city_block_preview_index != city_block_index) {
        project.cached_city_block_preview_scene = build_city_block_preview_scene(city_block, meshes);
        project.cached_city_block_preview_index = city_block_index;
    }
    const auto& scene = project.cached_city_block_preview_scene;
    if (scene.solid_cell.has_value()) {
        draw_render_buffer_faces(scene.solid_cell->buffer, {98, 148, 204, 220});
    }
    for (const auto& piece : scene.pieces) {
        draw_mesh_faces(piece.mesh, piece.origin, scene.unit_size, {98, 148, 204, 220});
    }
    for (std::size_t layer = 0; layer < city_block.layers.size(); ++layer) {
        for (std::size_t slot = 0; slot < city_block.layers[layer].size(); ++slot) {
            const auto selected = layer == selected_layer && slot == selected_slot;
            const auto x = static_cast<float>(slot % 4) * 4.5F;
            const auto z = static_cast<float>(slot / 4) * 4.5F;
            const auto y = static_cast<float>(layer) * 1.4F;
            const auto occupied = !city_block.layers[layer][slot].empty();
            const auto fill = selected ? Color{235, 200, 86, 130} : (occupied ? Color{108, 151, 184, 70} : Color{48, 54, 60, 55});
            const auto wire = selected ? Color{255, 240, 150, 255} : Color{26, 34, 42, 255};
            DrawCubeV({x, y + 0.5F, z}, {3.6F, 1.0F, 3.6F}, fill);
            DrawCubeWiresV({x, y + 0.5F, z}, {3.6F, 1.0F, 3.6F}, wire);
        }
    }
}

void draw_buildblock_preview_3d(const BuildblockPreviewScene& scene) {
    std::size_t max_layer = 0;
    bool has_any = false;
    for (const auto& rect : scene.rects) {
        max_layer = std::max(max_layer, rect.layer_index);
        has_any = true;
    }
    for (const auto& door : scene.doors) {
        max_layer = std::max(max_layer, door.layer_index);
        has_any = true;
    }

    if (has_any) {
        for (std::size_t i = 0; i <= max_layer; ++i) {
            float ox = static_cast<float>(i) * scene.layer_spacing;
            Vector3 center = { ox + 2.0F, 2.0F, 0.0F };
            DrawCubeWiresV(center, { 4.0F, 4.0F, 0.02F }, Color{ 212, 220, 229, 110 });
        }
    }

    for (const auto& rect : scene.rects) {
        Color color;
        Color wire_color;
        if (rect.kind == BuildblockPreviewRectKind::Bounds) {
            color = Color{ 97, 183, 239, 100 };
            wire_color = Color{ 97, 183, 239, 220 };
        } else if (rect.kind == BuildblockPreviewRectKind::Window) {
            color = Color{ 240, 164, 84, 150 };
            wire_color = Color{ 240, 164, 84, 255 };
        } else if (rect.kind == BuildblockPreviewRectKind::Beam) {
            color = Color{ 132, 216, 147, 150 };
            wire_color = Color{ 132, 216, 147, 255 };
        } else {
            color = GRAY;
            wire_color = DARKGRAY;
        }

        Vector3 pos = { rect.center.x, rect.center.y, rect.center.z };
        Vector3 sz = { rect.size.x, rect.size.y, rect.size.z };
        DrawCubeV(pos, sz, color);
        DrawCubeWiresV(pos, sz, wire_color);
    }

    for (const auto& door : scene.doors) {
        Vector3 pos = { door.position.x, door.position.y, door.position.z };
        Vector3 sz = { 0.5F, 0.5F, 0.3F };
        DrawCubeV(pos, sz, Color{ 255, 210, 115, 255 });
        DrawCubeWiresV(pos, sz, WHITE);
    }
}

// Goes up whenever city_map_image reloads or redraws its image.
std::uint64_t g_map_image_revision = 0;

// The map image (GenerateMapTextureFromVisibleSectors @124300): 512x512 of the
// world sectors (road and curb tiles, buildings, pieces, but not the landscape),
// drawn without lighting from straight above on black. The view is orthographic,
// half-size 1024 around (1920, 1920), so one pixel per cell over world x and z
// 896..2944, with +x to the right and +z down. The round map
// (isMapAutoGenerated) keeps its map.png instead.
const Texture2D* city_map_image(EditorProject& project) {
    static RenderTexture2D target{};
    static Texture2D loaded_png{};
    static std::filesystem::path png_root;
    static std::uint64_t rendered_key = 0;
    if (project.root.filename() == "round") {
        if (png_root != project.root) {
            if (loaded_png.id != 0U) UnloadTexture(loaded_png);
            loaded_png = {};
            png_root = project.root;
            const auto path = project.root / "map.png";
            if (std::filesystem::exists(path)) loaded_png = LoadTexture(path.string().c_str());
            ++g_map_image_revision;
        }
        return loaded_png.id != 0U ? &loaded_png : nullptr;
    }
    if (!project.city || !project.city_scene_gpu_valid) return target.id != 0U ? &target.texture : nullptr;
    std::uint64_t key = project.city_scene_cache_fingerprint ^ (project.city_tile_gpu_model.mesh.index_count * 0x9e3779b97f4a7c15ULL) ^
        (project.portal_gpu_model.mesh.index_count * 0xc2b2ae3d27d4eb4fULL);
    for (const auto& model : project.building_gpu_models) key = key * 31U + model.mesh.index_count;
    if (target.id != 0U && key == rendered_key) return &target.texture;
    rendered_key = key;
    ++g_map_image_revision;
    if (target.id == 0U) target = LoadRenderTexture(512, 512);

    Camera3D camera{};
    camera.position = {1920.0F, 1024.0F, 1920.0F};
    camera.target = {1920.0F, 0.0F, 1920.0F};
    camera.up = {0.0F, 0.0F, -1.0F};
    camera.fovy = 2048.0F;
    camera.projection = CAMERA_ORTHOGRAPHIC;
    const auto near_plane = rlGetCullDistanceNear();
    const auto far_plane = rlGetCullDistanceFar();
    rlSetClipPlanes(0.01, 2048.0);
    BeginTextureMode(target);
    ClearBackground(BLACK);
    BeginMode3D(camera);
    draw_gpu_mesh_instances(project.city_tile_gpu_model.mesh, {MatrixIdentity()}, GpuSurface::Unlit);
    draw_gpu_mesh_instances(project.portal_gpu_model.mesh, {MatrixTranslate(0.0F, 0.25F, 0.0F)}, GpuSurface::Unlit);
    const auto& placement_assets = resolve_placement_asset_indices(project);
    std::vector<std::vector<Matrix>> by_asset(project.building_gpu_models.size());
    for (std::size_t i = 0; i < project.city->data.buildings.size(); ++i) {
        if (!placement_assets[i].has_value() || *placement_assets[i] >= by_asset.size()) continue;
        by_asset[*placement_assets[i]].push_back(city_placement_matrix(project.city->data.buildings[i]));
    }
    for (std::size_t asset = 0; asset < by_asset.size(); ++asset) {
        draw_gpu_mesh_instances(project.building_gpu_models[asset].mesh, by_asset[asset], GpuSurface::Unlit);
    }
    EndMode3D();
    EndTextureMode();
    rlSetClipPlanes(near_plane, far_plane);
    return &target.texture;
}

// The game's fonts in load order (Program_InitializeAndRunClient @1330): index,
// file, and load flags (1 = nearest filtering and raw widths, 2 = lowercase drawn
// with the uppercase glyphs).
struct GameFontInfo {
    const char* name;
    int flags;
};
constexpr std::array<GameFontInfo, 5> kGameFonts{{
    {"vcr", 0}, {"osd-white", 2}, {"rockwell", 0}, {"fixedsys", 1}, {"overpass", 0},
}};

std::vector<MapFontChoice> map_font_choices(const EditorProject& project) {
    std::vector<MapFontChoice> choices;
    for (const auto& info : kGameFonts) {
        for (const auto& dir : {project.root.parent_path().parent_path() / "font", project.root.parent_path() / "font"}) {
            if (std::filesystem::exists(dir / (std::string(info.name) + ".csv")) &&
                std::filesystem::exists(dir / (std::string(info.name) + ".png"))) {
                choices.push_back({std::string("game:") + info.name, std::string(info.name) + " (game)"});
                break;
            }
        }
    }
    std::vector<std::filesystem::path> dirs;
#if defined(__APPLE__)
    dirs = {"/System/Library/Fonts", "/System/Library/Fonts/Supplemental", "/Library/Fonts"};
    if (const char* home = std::getenv("HOME")) dirs.push_back(std::filesystem::path(home) / "Library/Fonts");
#elif defined(_WIN32)
    dirs = {"C:/Windows/Fonts"};
#elif !defined(__EMSCRIPTEN__)
    dirs = {"/usr/share/fonts", "/usr/local/share/fonts"};
    if (const char* home = std::getenv("HOME")) dirs.push_back(std::filesystem::path(home) / ".local/share/fonts");
#endif
    std::vector<MapFontChoice> system;
    for (const auto& dir : dirs) {
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            auto ext = it->path().extension().string();
            std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (it->is_regular_file(ec) && (ext == ".ttf" || ext == ".otf")) {
                system.push_back({"file:" + it->path().string(), it->path().stem().string()});
            }
        }
    }
    std::ranges::sort(system, {}, &MapFontChoice::label);
    choices.insert(choices.end(), system.begin(), system.end());
    return choices;
}

namespace {

// One loaded label font: a game atlas font, or a system TrueType font drawn at the
// label's pixel size.
struct MapLabelFont {
    std::string key;
    std::filesystem::path root;
    int pixel_size{};
    std::optional<GameFont> game;
    Texture2D game_texture{};
    int game_flags{};
    Font system{};

    void unload() {
        if (game_texture.id != 0U) UnloadTexture(game_texture);
        if (game) UnloadImage(game->atlas);
        if (system.texture.id != 0U) UnloadFont(system);
        *this = {};
    }
};

void load_map_label_font(MapLabelFont& font, const EditorProject& project, const std::string& key, int pixel_size) {
    const bool is_game = key.starts_with("game:");
    if (font.root == project.root && font.key == key && (is_game || font.pixel_size == pixel_size)) return;
    font.unload();
    font.key = key;
    font.root = project.root;
    font.pixel_size = pixel_size;
    if (is_game) {
        const auto name = key.substr(5);
        const auto info = std::ranges::find_if(kGameFonts, [&](const GameFontInfo& f) { return name == f.name; });
        font.game_flags = info != kGameFonts.end() ? info->flags : 0;
        font.game = load_game_font(project.root, name.c_str(), (font.game_flags & 1) != 0);
        if (font.game) {
            font.game_texture = LoadTextureFromImage(font.game->atlas);
            SetTextureFilter(font.game_texture, (font.game_flags & 1) != 0 ? TEXTURE_FILTER_POINT : TEXTURE_FILTER_BILINEAR);
        }
    } else if (key.starts_with("file:")) {
        font.system = LoadFontEx(key.substr(5).c_str(), std::max(8, pixel_size), nullptr, 0);
        SetTextureFilter(font.system.texture, TEXTURE_FILTER_BILINEAR);
    }
}

// Draws `text` centred on (x, y) at `size` map units, like renderText flags 5.
void draw_map_label(const MapLabelFont& font, const char* text, float x, float y, float size) {
    if (font.game) {
        const auto glyph = [&](unsigned char c) {
            // Flag 2 fonts use the uppercase glyph rows for a..z.
            return (font.game_flags & 2) != 0 && c >= 'a' && c <= 'z' ? static_cast<unsigned char>(c - 32) : c;
        };
        float advance = 0.0F;
        for (const char* c = text; *c != '\0'; ++c) advance += size * font.game->width[glyph(static_cast<unsigned char>(*c))];
        float pen = x - advance * 0.5F;
        const float top = y - size * 0.5F;
        // _cs_drawtextstring: a size x size cell per glyph, cut to its
        // advance width, sampled from the 16x16 atlas.
        rlSetTexture(font.game_texture.id);
        rlBegin(RL_QUADS);
        rlColor4ub(255, 255, 255, 255);
        for (const char* c = text; *c != '\0'; ++c) {
            const auto code = glyph(static_cast<unsigned char>(*c));
            const float w = font.game->width[code];
            const float u0 = static_cast<float>(code & 15U) / 16.0F;
            const float v0 = static_cast<float>(code >> 4U) / 16.0F;
            const float u1 = u0 + w / 16.0F;
            const float v1 = v0 + 1.0F / 16.0F;
            rlTexCoord2f(u0, v0); rlVertex2f(pen, top);
            rlTexCoord2f(u0, v1); rlVertex2f(pen, top + size);
            rlTexCoord2f(u1, v1); rlVertex2f(pen + w * size, top + size);
            rlTexCoord2f(u1, v0); rlVertex2f(pen + w * size, top);
            pen += w * size;
        }
        rlEnd();
        rlSetTexture(0);
        return;
    }
    const Font& face = font.system.texture.id != 0U ? font.system : GetFontDefault();
    const float spacing = font.system.texture.id != 0U ? 0.0F : size * 0.1F;
    const auto measured = MeasureTextEx(face, text, size, spacing);
    DrawTextEx(face, text, {x - measured.x * 0.5F, y - size * 0.5F}, size, spacing, WHITE);
}

// Draws the map screen into `target` (square, `pixels` wide).
void compose_map_menu(RenderTexture2D target, const Texture2D& image, EditorProject& project, const MapMenuOptions& options, MapLabelFont& font) {
    const auto pixels = target.texture.width;
    const bool auto_generated = project.root.filename() == "round";
    const float center = auto_generated ? 128.0F : 120.0F;
    const float half = static_cast<float>(pixels) * 0.5F;
    constexpr float size = 6.0F;
    if (!auto_generated && options.street_labels) {
        load_map_label_font(font, project, options.font, static_cast<int>(std::lround(size * half / 128.0F)));
    }
    BeginTextureMode(target);
    ClearBackground(BLACK);
    BeginMode2D(Camera2D{{half, half}, {center, center}, 0.0F, half / 128.0F});
    // Drawn images are stored bottom-up; round's map.png is a normal image.
    const auto height = static_cast<float>(image.height);
    DrawTexturePro(image, {0.0F, 0.0F, static_cast<float>(image.width), auto_generated ? height : -height},
        {0.0F, 0.0F, 256.0F, 256.0F}, {}, 0.0F, Fade(WHITE, 0.75F));
    if (!auto_generated && options.street_labels) {
        for (const auto& label : project.city_scene_cache.map_labels) {
            const char* text = native_street_names[static_cast<std::size_t>(
                std::clamp(label.name, 0, static_cast<int>(native_street_names.size()) - 1))];
            draw_map_label(font, text, label.x, label.y, size);
        }
    }
    EndMode2D();
    EndTextureMode();
}

MapLabelFont& map_label_font() {
    static MapLabelFont font;
    return font;
}

} // namespace

// The map screen as DrawMapMenu @88130 draws it into its 512x512 viewport (in
// 1024x768 virtual units, so at the screen's own pixel density), at `pixels`. It is
// an orthographic view of map space, half-size 128 around (120, 120), or (128, 128)
// when isMapAutoGenerated, with y going down. The map image fills (0, 0)-(256, 256)
// at alpha 0.75. On city maps, each street label follows in font 2 ("rockwell", set
// by RenderWorld before the HUD) at size 6, in white, with renderText flags 5: x
// moves back by half the advance width and y by half the size. Labels sit at
// (world - 896) * 0.125 (see build_map_scene).
const Texture2D* map_menu_texture(EditorProject& project, int pixels, const MapMenuOptions& options) {
    const auto* image = city_map_image(project);
    if (image == nullptr) return nullptr;
    static RenderTexture2D target{};
    static std::uint64_t composed_key = 0;
    pixels = std::clamp(pixels, 64, 4096);
    std::uint64_t key = g_map_image_revision * 0x9e3779b97f4a7c15ULL;
    key = key * 31U + static_cast<std::uint64_t>(pixels);
    key = key * 31U + (options.street_labels ? 1U : 0U);
    key = key * 31U + std::hash<std::string>{}(options.font);
    key = key * 31U + std::hash<std::string>{}(project.root.string());
    for (const auto& label : project.city_scene_cache.map_labels) {
        key = key * 31U + static_cast<std::uint64_t>(label.name);
        key = key * 31U + static_cast<std::uint64_t>(std::lround(label.x * 8.0F));
        key = key * 31U + static_cast<std::uint64_t>(std::lround(label.y * 8.0F));
    }
    if (target.id != 0U && key == composed_key) return &target.texture;
    composed_key = key;
    if (target.id != 0U && target.texture.width != pixels) {
        UnloadRenderTexture(target);
        target = {};
    }
    if (target.id == 0U) {
        target = LoadRenderTexture(pixels, pixels);
        SetTextureFilter(target.texture, TEXTURE_FILTER_BILINEAR);
    }
    compose_map_menu(target, *image, project, options, map_label_font());
    return &target.texture;
}

std::vector<std::uint8_t> map_menu_png(EditorProject& project, int pixels, const MapMenuOptions& options) {
    const auto* image = city_map_image(project);
    if (image == nullptr) return {};
    auto target = LoadRenderTexture(pixels, pixels);
    compose_map_menu(target, *image, project, options, map_label_font());
    auto shot = LoadImageFromTexture(target.texture);
    UnloadRenderTexture(target);
    ImageFlipVertical(&shot);
    int size = 0;
    auto* bytes = ExportImageToMemory(shot, ".png", &size);
    UnloadImage(shot);
    std::vector<std::uint8_t> png(bytes, bytes + std::max(0, size));
    MemFree(bytes);
    return png;
}

void draw_gpu_building_model_public(GpuBuildingRenderModel& model, Vector3 origin, std::int32_t rotation) {
    draw_gpu_building_model(model, origin, rotation, false);
}

} // namespace pafta::app_internal
