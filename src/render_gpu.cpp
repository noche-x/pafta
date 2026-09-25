#include "app_internal.hpp"
#include "profiler.hpp"

#include <rlgl.h>
#include <raymath.h>
#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#else
#include <external/glad.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef __EMSCRIPTEN__
#define MAPED_GLSL_VERSION "#version 300 es\nprecision highp float;\nprecision highp int;\n"
#else
#define MAPED_GLSL_VERSION "#version 330 core\n"
#endif

namespace map_editor::app_internal {

GfxDebugStats* g_gfx_debug_stats = nullptr;
float g_world_time_hours = 12.0F;

Vector3 game_light_direction() {
    constexpr float reset_angle = 6.02138591938F;
    const float angle = reset_angle + (g_world_time_hours - 12.0F)*(2.0F*PI/24.0F);
    return Vector3RotateByAxisAngle(
        Vector3Normalize(Vector3{0.0F, 1.0F, 0.5F}),
        Vector3{0.0F, 0.4472135901F, -0.8944271803F},
        angle);
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

Color lerp_color(Color from, Color to, float t) {
    auto lerp_channel = [t](unsigned char a, unsigned char b) {
        return static_cast<unsigned char>(
            static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t);
    };
    return {
        lerp_channel(from.r, to.r),
        lerp_channel(from.g, to.g),
        lerp_channel(from.b, to.b),
        lerp_channel(from.a, to.a),
    };
}

struct GameRenderResources {
    Texture2D packed_depth{};
    Texture2D shadow_factor{};
    RenderTexture2D display{};
    unsigned int stencil_buffer{};
    Texture2D direct_light{};
};

std::unordered_map<unsigned int, GameRenderResources>& game_render_resources() {
    static std::unordered_map<unsigned int, GameRenderResources> resources;
    return resources;
}

namespace {
bool g_depth_output_cache_valid = false;
bool g_depth_output_cached_enabled = false;
}  // namespace

void reset_game_render_depth_output_cache() {
    // begin_game_texture_mode()/end_game_texture_mode() rebind draw buffers
    // directly, so the cached state below would otherwise go stale.
    g_depth_output_cache_valid = false;
}

void set_game_render_depth_output(bool enabled) {
    // Repeated calls with the same value are extremely common (once per
    // building per frame); skip the batch flush and GL queries when the
    // state already matches.
    if (g_depth_output_cache_valid && g_depth_output_cached_enabled == enabled) return;
    g_depth_output_cache_valid = true;
    g_depth_output_cached_enabled = enabled;

    rlDrawRenderBatchActive();
    // The game surface shader writes both the lit color and the packed linear
    // distance used by the atmosphere pass. WebGL2 requires every active draw
    // buffer to have a matching fragment output, so enable both attachments
    // only while that shader is submitting geometry. Raylib's utility shaders
    // (grids, lines, cursors, and ImGui) write one output and must use color 0.
    GLint framebuffer = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    GLint depth_attachment_type = GL_NONE;
    if (framebuffer != 0) {
        glGetFramebufferAttachmentParameteriv(
            GL_DRAW_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT1,
            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
            &depth_attachment_type);
    }
    if (enabled && depth_attachment_type != GL_NONE) {
        const GLenum color_and_depth[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2};
        glDrawBuffers(3, color_and_depth);
    } else {
        const GLenum color_only[] = {GL_COLOR_ATTACHMENT0};
        glDrawBuffers(1, color_only);
    }
}

RenderTexture2D load_game_render_texture(int width, int height) {
#ifdef __EMSCRIPTEN__
    // WebGL2 supports the same two color outputs as the native path.  Raylib's
    // default web target only has a depth renderbuffer, so replace it with one
    // packed depth/stencil renderbuffer before adding the depth color map.
    auto target = LoadRenderTexture(width, height);
    if (target.id == 0U) return target;

    glBindFramebuffer(GL_FRAMEBUFFER, target.id);
    if (target.depth.id != 0U) {
        glDeleteRenderbuffers(1, &target.depth.id);
    }
    glGenRenderbuffers(1, &target.depth.id);
    glBindRenderbuffer(GL_RENDERBUFFER, target.depth.id);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, target.depth.id);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, target.depth.id);

    Texture2D packed_depth{
        rlLoadTexture(nullptr, width, height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1),
        width,
        height,
        1,
        PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
    };
    Texture2D shadow_factor{
        rlLoadTexture(nullptr, width, height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1),
        width,
        height,
        1,
        PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
    };
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, packed_depth.id, 0);
    const GLenum draw_buffers[] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, draw_buffers);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        TRACELOG(LOG_WARNING, "MAP EDITOR: WebGL2 game render target is incomplete");
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
        UnloadTexture(packed_depth);
        UnloadTexture(shadow_factor);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return target;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    game_render_resources()[target.id] = {packed_depth, shadow_factor, LoadRenderTexture(width, height), 0};
    return target;
#else
    auto target = LoadRenderTexture(width, height);
    rlUnloadTexture(target.texture.id);
    target.texture = {
        rlLoadTexture(nullptr, width, height, PIXELFORMAT_UNCOMPRESSED_R16G16B16A16, 1),
        width,
        height,
        1,
        PIXELFORMAT_UNCOMPRESSED_R16G16B16A16,
    };
    Texture2D packed_depth{
        rlLoadTexture(nullptr, width, height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1),
        width,
        height,
        1,
        PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
    };
    Texture2D shadow_factor{
        rlLoadTexture(nullptr, width, height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1),
        width,
        height,
        1,
        PIXELFORMAT_UNCOMPRESSED_R8G8B8A8,
    };
    glBindFramebuffer(GL_FRAMEBUFFER, target.id);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture.id, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, packed_depth.id, 0);
    glDeleteRenderbuffers(1, &target.depth.id);
    glGenRenderbuffers(1, &target.depth.id);
    glBindRenderbuffer(GL_RENDERBUFFER, target.depth.id);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, target.depth.id);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, target.depth.id);
    const unsigned int stencil_buffer = 0; // Owned by target.depth and released with the target.
    const GLenum draw_buffers[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, draw_buffers);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    game_render_resources()[target.id] = {packed_depth, shadow_factor, LoadRenderTexture(width, height), stencil_buffer};
    return target;
#endif
}

void begin_game_texture_mode(RenderTexture2D target) {
    reset_game_render_depth_output_cache();
    BeginTextureMode(target);
    rlDrawRenderBatchActive();
#ifdef __EMSCRIPTEN__
    // Raylib's rlEnableFramebuffer() omits GRAPHICS_API_OPENGL_ES3 in this
    // bundled release. BeginTextureMode still sets the viewport and matrices,
    // but leaves the default framebuffer bound, so explicitly bind the target.
    glBindFramebuffer(GL_FRAMEBUFFER, target.id);
#else
    glBindFramebuffer(GL_FRAMEBUFFER, target.id);
#endif
    // The sky pass detaches color attachment 1 so it can sample the packed
    // scene depth without an attachment feedback loop. Reattach it at the
    // start of every frame on every platform. Previously this happened only
    // on WebGL, leaving native sky and shadow composition stuck on frame one.
    if (const auto it = game_render_resources().find(target.id); it != game_render_resources().end()) {
        glFramebufferTexture2D(
            GL_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT1,
            GL_TEXTURE_2D,
            it->second.packed_depth.id,
            0);
        if (it->second.direct_light.id == 0U) {
            it->second.direct_light = {rlLoadTexture(nullptr, target.texture.width, target.texture.height, target.texture.format, 1),
                target.texture.width, target.texture.height, 1, target.texture.format};
        }
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, it->second.direct_light.id, 0);
        const GLenum color_only[] = {GL_COLOR_ATTACHMENT0};
        glDrawBuffers(1, color_only);
    }
    // A tab change can interrupt the city shadow pass. Never carry its write
    // masks or stencil test into the next viewport frame.
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xff);
    glDisable(GL_STENCIL_TEST);
}

void end_game_texture_mode() {
    reset_game_render_depth_output_cache();
    rlDrawRenderBatchActive();
    const auto target = rlGetActiveFramebuffer();
    if (game_render_resources().contains(target)) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, 0, 0);
        const GLenum color_only[] = {GL_COLOR_ATTACHMENT0};
        glDrawBuffers(1, color_only);
    }
#ifdef __EMSCRIPTEN__
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif
    EndTextureMode();
}

void unload_game_render_texture(RenderTexture2D target) {
    if (target.id == 0U) return;
    auto& resources = game_render_resources();
    if (const auto it = resources.find(target.id); it != resources.end()) {
        glDeleteRenderbuffers(1, &it->second.stencil_buffer);
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
        value.shader = LoadShaderFromMemory(nullptr, MAPED_GLSL_VERSION R"glsl(
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
    // Native world.frag shadows direct diffuse/specular only. Preserve sky,
    // ambient light and reflections instead of darkening the final image.
    return max(texture(texture0, uv).rgb - texture(directLight, uv).rgb * (1.0-texture(shadowFactor, uv).r), vec3(0.0));
}
vec3 fxaa(vec2 uv) {
    vec2 inverseResolution=1.0/resolution;
    vec3 rgbNW=sceneColor(uv+vec2(-1.0,-1.0)*inverseResolution);
    vec3 rgbNE=sceneColor(uv+vec2( 1.0,-1.0)*inverseResolution);
    vec3 rgbSW=sceneColor(uv+vec2(-1.0, 1.0)*inverseResolution);
    vec3 rgbSE=sceneColor(uv+vec2( 1.0, 1.0)*inverseResolution);
    vec3 rgbM =sceneColor(uv);
    vec3 luma=vec3(0.299,0.587,0.114);
    float lumaNW=dot(rgbNW,luma);
    float lumaNE=dot(rgbNE,luma);
    float lumaSW=dot(rgbSW,luma);
    float lumaSE=dot(rgbSE,luma);
    float lumaM=dot(rgbM,luma);
    float lumaMin=min(lumaM,min(min(lumaNW,lumaNE),min(lumaSW,lumaSE)));
    float lumaMax=max(lumaM,max(max(lumaNW,lumaNE),max(lumaSW,lumaSE)));
    vec2 direction=vec2(-((lumaNW+lumaNE)-(lumaSW+lumaSE)),
                         (lumaNW+lumaSW)-(lumaNE+lumaSE));
    float reduce=max((lumaNW+lumaNE+lumaSW+lumaSE)*0.03125,0.0078125);
    float inverseDirection=1.0/(min(abs(direction.x),abs(direction.y))+reduce);
    direction=clamp(direction*inverseDirection,vec2(-8.0),vec2(8.0))*inverseResolution;
    vec3 resultA=0.5*(
        sceneColor(uv+direction*(1.0/3.0-0.5))+
        sceneColor(uv+direction*(2.0/3.0-0.5)));
    vec3 resultB=resultA*0.5+0.25*(
        sceneColor(uv+direction*-0.5)+
        sceneColor(uv+direction* 0.5));
    float lumaB=dot(resultB,luma);
    return (lumaB<lumaMin || lumaB>lumaMax) ? resultA : resultB;
}
void main() {
    vec3 color=fxaa(fragTexCoord);
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
        0;
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
    MAPED_PROFILE_FUNCTION();
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
    // Keep the current models drawing until each replacement uploads (the
    // upload move-assigns over them); clearing here blanked the whole city.
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
        MAPED_PROFILE_THREAD("Building render rebuild");
        MAPED_PROFILE_BLOCK("build async building render buffers");
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
            auto city_buffer = build_city_building_full_render_buffer(building.data, blocks, templates);
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
    // Keep the current models drawing until each replacement uploads.
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
        buildblock_templates(project));
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
    MAPED_PROFILE_FUNCTION();
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
    const auto end = Clock::now();
    const auto ms = std::chrono::duration<double, std::milli>(end - begin).count();
    const auto name = index < project.buildings.size() ? project.buildings[index].name : std::to_string(index);
    const auto& model = project.building_gpu_models[index];
    append_render_log(
        project,
        "gpu upload " + name +
        ": " + std::to_string(model.chunks.size()) + " chunks, " +
        std::to_string(model.uploaded_vertices) + " verts, " +
        std::to_string(model.uploaded_indices) + " indices, " +
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
    MAPED_PROFILE_FUNCTION();
    if (project.render_buffers_dirty && !project.render_build_in_progress) {
#ifdef __EMSCRIPTEN__
        start_incremental_render_buffer_rebuild(project);
#else
        start_async_render_buffer_rebuild(project);
#endif
    }

#ifdef __EMSCRIPTEN__
    build_next_browser_render_buffer(project);
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
        if (g_gfx_debug_stats != nullptr) {
            GfxDebugScopedTimer timer{*g_gfx_debug_stats, GfxDebugTimingPhase::GpuUpload};
            upload_next_pending_building(project);
        } else {
            upload_next_pending_building(project);
        }
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

// Unified selection highlight: a pulsing, layered wireframe box that reads as a
// glow without needing a framebuffer bloom pass (which would mean a shader over
// the whole scene and a fragile cross-platform GL path). Drawn for the selected
// building, road, and piece so selection feedback is consistent everywhere.
void draw_selection_glow_box(Vector3 center, Vector3 size, Color tint) {
    const auto pulse = 0.5F + 0.5F * static_cast<float>(std::sin(GetTime() * 4.0));
    const auto inner = Color{tint.r, tint.g, tint.b, static_cast<unsigned char>(235)};
    const auto outer = Color{tint.r, tint.g, tint.b, static_cast<unsigned char>(60 + 70 * pulse)};
    // Two concentric boxes: a crisp inner edge plus a softer, larger pulsing halo.
    DrawCubeWiresV(center, size, inner);
    const auto halo = Vector3{size.x + 1.4F + pulse, size.y + 1.4F + pulse, size.z + 1.4F + pulse};
    DrawCubeWiresV(center, halo, outer);
    if (g_gfx_debug_stats != nullptr) {
        g_gfx_debug_stats->add_draw(GfxDebugDrawCategory::WireOverlay, 0U, 48U, 24U);
    }
}

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
        // client/src/hooks/landscapeGenerateTexture.hpp reproduces the game's
        // generated 0x400-square green landscape color texture as RGBA
        // (0x40, 0x54, 0x26, 0xff). bc_grass01b is a building texture and was
        // the source of the bright green distant landscape patches.
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
    return find_project_texture(project, game_surface_texture_name(buffer.material_names[material_id]));
}

const Texture2D* material_texture_for_buffer_material(const EditorProject& project, const BuildingRenderBuffer& buffer, std::uint32_t material_id) {
    if (material_id >= buffer.material_names.size()) {
        return nullptr;
    }
    return find_project_material_texture(project, game_surface_texture_name(buffer.material_names[material_id]));
}

Shader game_surface_shader();
Texture2D default_game_material_texture();

GpuBuildingMeshChunk upload_mesh_chunk(
    const BuildingRenderBuffer& buffer,
    std::uint32_t first_triangle,
    std::uint32_t triangle_count,
    std::uint32_t material_id,
    const Texture2D* texture,
    const Texture2D* material_texture,
    bool window_material = false
) {
    GpuBuildingMeshChunk chunk;
    chunk.material_id = material_id;
    chunk.window_material = window_material;

    const auto clamped_end = std::min<std::uint32_t>(
        first_triangle + triangle_count,
        static_cast<std::uint32_t>(buffer.triangles.size()));
    const auto clamped_count = clamped_end > first_triangle ? clamped_end - first_triangle : 0U;
    if (clamped_count == 0U) {
        return chunk;
    }

    auto upload_unindexed_chunk = [&]() {
        GpuBuildingMeshChunk fallback;
        fallback.material_id = material_id;
        fallback.window_material = window_material;
        fallback.mesh.vertexCount = static_cast<int>(clamped_count * 3U);
        fallback.mesh.triangleCount = static_cast<int>(clamped_count);
        fallback.mesh.vertices = static_cast<float*>(MemAlloc(static_cast<unsigned int>(fallback.mesh.vertexCount * 3 * sizeof(float))));
        fallback.mesh.texcoords = static_cast<float*>(MemAlloc(static_cast<unsigned int>(fallback.mesh.vertexCount * 2 * sizeof(float))));
        fallback.mesh.colors = static_cast<unsigned char*>(MemAlloc(static_cast<unsigned int>(fallback.mesh.vertexCount * 4 * sizeof(unsigned char))));
        fallback.mesh.normals = static_cast<float*>(MemAlloc(static_cast<unsigned int>(fallback.mesh.vertexCount * 3 * sizeof(float))));
        if (fallback.mesh.vertices == nullptr || fallback.mesh.texcoords == nullptr || fallback.mesh.colors == nullptr ||
            fallback.mesh.normals == nullptr) {
            if (fallback.mesh.vertices != nullptr) {
                MemFree(fallback.mesh.vertices);
            }
            if (fallback.mesh.texcoords != nullptr) {
                MemFree(fallback.mesh.texcoords);
            }
            if (fallback.mesh.colors != nullptr) {
                MemFree(fallback.mesh.colors);
            }
            if (fallback.mesh.normals != nullptr) {
                MemFree(fallback.mesh.normals);
            }
            fallback.mesh.vertices = nullptr;
            fallback.mesh.texcoords = nullptr;
            fallback.mesh.colors = nullptr;
            fallback.mesh.normals = nullptr;
            fallback.mesh.vertexCount = 0;
            fallback.mesh.triangleCount = 0;
            return fallback;
        }

        auto* out = fallback.mesh.vertices;
        auto* out_uv = fallback.mesh.texcoords;
        auto* out_color = fallback.mesh.colors;
        auto* out_normal = fallback.mesh.normals;
        for (auto triangle_index = first_triangle; triangle_index < clamped_end; ++triangle_index) {
            const auto& triangle = buffer.triangles[triangle_index];
            std::array<RenderBufferVertex, 3> tri_vertices{};
            std::array<Vec3, 3> positions{};
            for (std::size_t index = 0; index < triangle.indices.size(); ++index) {
                tri_vertices[index] = triangle.indices[index] < buffer.vertices.size()
                    ? buffer.vertices[triangle.indices[index]]
                    : RenderBufferVertex{};
                positions[index] = tri_vertices[index].position;
            }
            const Vector3 e1{
                positions[1].x - positions[0].x,
                positions[1].y - positions[0].y,
                positions[1].z - positions[0].z,
            };
            const Vector3 e2{
                positions[2].x - positions[0].x,
                positions[2].y - positions[0].y,
                positions[2].z - positions[0].z,
            };
            const Vector3 face_normal{
                e1.y * e2.z - e1.z * e2.y,
                e1.z * e2.x - e1.x * e2.z,
                e1.x * e2.y - e1.y * e2.x,
            };
            const auto fn_len = std::sqrt(face_normal.x * face_normal.x + face_normal.y * face_normal.y + face_normal.z * face_normal.z);
            const auto fn_inv = fn_len > 1e-6F ? 1.0F / fn_len : 0.0F;
            const Vector3 unit_normal{face_normal.x * fn_inv, face_normal.y * fn_inv, face_normal.z * fn_inv};
            for (const auto vertex_index : triangle.indices) {
                const auto vertex = vertex_index < buffer.vertices.size()
                    ? buffer.vertices[vertex_index]
                    : RenderBufferVertex{};
                const auto position = vertex.position;
                *out++ = position.x;
                *out++ = position.y;
                *out++ = position.z;
                *out_uv++ = vertex.u;
                *out_uv++ = vertex.v;
                *out_color++ = 255;
                *out_color++ = 255;
                *out_color++ = 255;
                *out_color++ = 255;
                *out_normal++ = unit_normal.x;
                *out_normal++ = unit_normal.y;
                *out_normal++ = unit_normal.z;
            }
        }

        UploadMesh(&fallback.mesh, false);
        fallback.material = LoadMaterialDefault();
        fallback.material.shader = game_surface_shader();
        fallback.material.maps[MATERIAL_MAP_DIFFUSE].color = native_material_color(material_id, false, window_material);
        fallback.material.maps[MATERIAL_MAP_METALNESS].texture = material_texture != nullptr && material_texture->id != 0U
            ? *material_texture
            : default_game_material_texture();
        if (texture != nullptr && texture->id != 0U) {
            fallback.material.maps[MATERIAL_MAP_DIFFUSE].texture = *texture;
            fallback.source_texture = *texture;
            fallback.textured = true;
        }
        return fallback;
    };

    std::vector<float> vertices;
    std::vector<float> texcoords;
    std::vector<unsigned char> colors;
    std::vector<unsigned short> indices;
    std::vector<float> normals;
    std::unordered_map<std::uint32_t, unsigned short> local_indices;
    std::vector<Vector3> vertex_normals(buffer.vertices.size());
    vertices.reserve(static_cast<std::size_t>(clamped_count) * 9U);
    texcoords.reserve(static_cast<std::size_t>(clamped_count) * 6U);
    colors.reserve(static_cast<std::size_t>(clamped_count) * 12U);
    normals.reserve(static_cast<std::size_t>(clamped_count) * 9U);
    indices.reserve(static_cast<std::size_t>(clamped_count) * 3U);
    local_indices.reserve(static_cast<std::size_t>(clamped_count) * 3U);

    for (auto triangle_index = first_triangle; triangle_index < clamped_end; ++triangle_index) {
        const auto& triangle = buffer.triangles[triangle_index];
        if (triangle.indices[0] >= buffer.vertices.size() ||
            triangle.indices[1] >= buffer.vertices.size() ||
            triangle.indices[2] >= buffer.vertices.size()) {
            continue;
        }
        const auto a = buffer.vertices[triangle.indices[0]].position;
        const auto b = buffer.vertices[triangle.indices[1]].position;
        const auto c = buffer.vertices[triangle.indices[2]].position;
        const Vector3 e1{b.x - a.x, b.y - a.y, b.z - a.z};
        const Vector3 e2{c.x - a.x, c.y - a.y, c.z - a.z};
        const Vector3 face_normal{
            e1.y * e2.z - e1.z * e2.y,
            e1.z * e2.x - e1.x * e2.z,
            e1.x * e2.y - e1.y * e2.x,
        };
        for (const auto vertex_index : triangle.indices) {
            auto& normal = vertex_normals[vertex_index];
            normal.x += face_normal.x;
            normal.y += face_normal.y;
            normal.z += face_normal.z;
        }
    }

    auto add_index = [&](std::uint32_t source_index) -> std::optional<unsigned short> {
        if (const auto existing = local_indices.find(source_index); existing != local_indices.end()) {
            return existing->second;
        }
        if (local_indices.size() > static_cast<std::size_t>(std::numeric_limits<unsigned short>::max())) {
            return std::nullopt;
        }

        const auto local_index = static_cast<unsigned short>(local_indices.size());
        const auto vertex = source_index < buffer.vertices.size()
            ? buffer.vertices[source_index]
            : RenderBufferVertex{};
        const auto position = vertex.position;
        vertices.push_back(position.x);
        vertices.push_back(position.y);
        vertices.push_back(position.z);
        texcoords.push_back(vertex.u);
        texcoords.push_back(vertex.v);
        colors.push_back(255);
        colors.push_back(255);
        colors.push_back(255);
        colors.push_back(255);
        // Per-pixel lighting normal (smoothed). The lit shader recomputes the same
        // model from this; the baked color above stays as a per-vertex fallback.
        const auto raw_n = source_index < vertex_normals.size() ? vertex_normals[source_index] : Vector3{0.0F, 1.0F, 0.0F};
        const auto nlen = std::sqrt(raw_n.x * raw_n.x + raw_n.y * raw_n.y + raw_n.z * raw_n.z);
        const auto inv = nlen > 1e-6F ? 1.0F / nlen : 0.0F;
        normals.push_back(raw_n.x * inv);
        normals.push_back(raw_n.y * inv);
        normals.push_back(raw_n.z * inv);
        local_indices.emplace(source_index, local_index);
        return local_index;
    };

    for (auto triangle_index = first_triangle; triangle_index < clamped_end; ++triangle_index) {
        const auto& triangle = buffer.triangles[triangle_index];
        for (const auto vertex_index : triangle.indices) {
            const auto local_index = add_index(vertex_index);
            if (!local_index.has_value()) {
                return upload_unindexed_chunk();
            }
            indices.push_back(*local_index);
        }
    }

    chunk.mesh.vertexCount = static_cast<int>(vertices.size() / 3U);
    chunk.mesh.triangleCount = static_cast<int>(indices.size() / 3U);
    chunk.mesh.vertices = static_cast<float*>(MemAlloc(static_cast<unsigned int>(vertices.size() * sizeof(float))));
    chunk.mesh.texcoords = static_cast<float*>(MemAlloc(static_cast<unsigned int>(texcoords.size() * sizeof(float))));
    chunk.mesh.colors = static_cast<unsigned char*>(MemAlloc(static_cast<unsigned int>(colors.size() * sizeof(unsigned char))));
    chunk.mesh.normals = static_cast<float*>(MemAlloc(static_cast<unsigned int>(normals.size() * sizeof(float))));
    chunk.mesh.indices = static_cast<unsigned short*>(MemAlloc(static_cast<unsigned int>(indices.size() * sizeof(unsigned short))));
    if (chunk.mesh.vertices == nullptr || chunk.mesh.texcoords == nullptr || chunk.mesh.colors == nullptr ||
        chunk.mesh.normals == nullptr || chunk.mesh.indices == nullptr) {
        if (chunk.mesh.vertices != nullptr) {
            MemFree(chunk.mesh.vertices);
        }
        if (chunk.mesh.texcoords != nullptr) {
            MemFree(chunk.mesh.texcoords);
        }
        if (chunk.mesh.colors != nullptr) {
            MemFree(chunk.mesh.colors);
        }
        if (chunk.mesh.normals != nullptr) {
            MemFree(chunk.mesh.normals);
        }
        if (chunk.mesh.indices != nullptr) {
            MemFree(chunk.mesh.indices);
        }
        chunk.mesh.vertices = nullptr;
        chunk.mesh.texcoords = nullptr;
        chunk.mesh.colors = nullptr;
        chunk.mesh.normals = nullptr;
        chunk.mesh.indices = nullptr;
        chunk.mesh.vertexCount = 0;
        chunk.mesh.triangleCount = 0;
        return chunk;
    }

    std::ranges::copy(vertices, chunk.mesh.vertices);
    std::ranges::copy(texcoords, chunk.mesh.texcoords);
    std::ranges::copy(colors, chunk.mesh.colors);
    std::ranges::copy(normals, chunk.mesh.normals);
    std::ranges::copy(indices, chunk.mesh.indices);

    UploadMesh(&chunk.mesh, false);
    chunk.material = LoadMaterialDefault();
    chunk.material.shader = game_surface_shader();
    chunk.material.maps[MATERIAL_MAP_DIFFUSE].color = native_material_color(material_id, false, window_material);
    chunk.material.maps[MATERIAL_MAP_METALNESS].texture = material_texture != nullptr && material_texture->id != 0U
        ? *material_texture
        : default_game_material_texture();
    if (texture != nullptr && texture->id != 0U) {
        chunk.material.maps[MATERIAL_MAP_DIFFUSE].texture = *texture;
        chunk.source_texture = *texture;
        chunk.textured = true;
    }
    return chunk;
}

// Uploads a whole city render buffer (roads/curbs or terrain) as GPU mesh chunks,
// one per material run, with per-face directional lighting baked into vertex
// colors. Replaces the per-triangle immediate-mode DrawTriangle3D path that was
// submitting hundreds of thousands of tiny draw calls per frame.
GpuBuildingRenderModel upload_city_buffer_model(
    const BuildingRenderBuffer& buffer,
    const EditorProject& project,
    bool smooth_normals,
    const std::function<Color(std::uint32_t)>& color_for_material) {
    GpuBuildingRenderModel model;
    model.wire_lines = buffer.wire_lines;
    model.chunks.reserve(buffer.material_runs.size());

    std::vector<Vector3> vertex_normals(buffer.vertices.size(), Vector3{});
    if (smooth_normals) {
        for (const auto& triangle : buffer.triangles) {
            if (triangle.indices[0] >= buffer.vertices.size() ||
                triangle.indices[1] >= buffer.vertices.size() ||
                triangle.indices[2] >= buffer.vertices.size()) {
                continue;
            }
            const auto& a = buffer.vertices[triangle.indices[0]].position;
            const auto& b = buffer.vertices[triangle.indices[1]].position;
            const auto& c = buffer.vertices[triangle.indices[2]].position;
            const Vector3 face = Vector3CrossProduct(
                {b.x-a.x, b.y-a.y, b.z-a.z},
                {c.x-a.x, c.y-a.y, c.z-a.z});
            for (const auto index : triangle.indices) {
                vertex_normals[index] = Vector3Add(vertex_normals[index], face);
            }
        }
        for (auto& normal : vertex_normals) {
            normal = Vector3LengthSqr(normal) > 1e-12F ? Vector3Normalize(normal) : Vector3{0.0F, 1.0F, 0.0F};
        }
    }

    for (const auto& run : buffer.material_runs) {
        const auto clamped_end = std::min<std::uint32_t>(
            run.first_triangle + run.triangle_count,
            static_cast<std::uint32_t>(buffer.triangles.size()));
        if (clamped_end <= run.first_triangle) {
            continue;
        }
        const auto tri_count = clamped_end - run.first_triangle;

        // Per-face (unindexed) so each triangle keeps its own flat normal and
        // hard edges shade crisply; terrain reads fine flat-shaded at this scale.
        GpuBuildingMeshChunk chunk;
        chunk.material_id = run.material;
        chunk.window_material = run.city_window_surface;
        chunk.mesh.vertexCount = static_cast<int>(tri_count * 3U);
        chunk.mesh.triangleCount = static_cast<int>(tri_count);
        chunk.mesh.vertices = static_cast<float*>(MemAlloc(static_cast<unsigned int>(chunk.mesh.vertexCount * 3 * sizeof(float))));
        chunk.mesh.texcoords = static_cast<float*>(MemAlloc(static_cast<unsigned int>(chunk.mesh.vertexCount * 2 * sizeof(float))));
        chunk.mesh.colors = static_cast<unsigned char*>(MemAlloc(static_cast<unsigned int>(chunk.mesh.vertexCount * 4 * sizeof(unsigned char))));
        chunk.mesh.normals = static_cast<float*>(MemAlloc(static_cast<unsigned int>(chunk.mesh.vertexCount * 3 * sizeof(float))));
        if (chunk.mesh.vertices == nullptr || chunk.mesh.texcoords == nullptr || chunk.mesh.colors == nullptr || chunk.mesh.normals == nullptr) {
            if (chunk.mesh.vertices != nullptr) { MemFree(chunk.mesh.vertices); }
            if (chunk.mesh.texcoords != nullptr) { MemFree(chunk.mesh.texcoords); }
            if (chunk.mesh.colors != nullptr) { MemFree(chunk.mesh.colors); }
            if (chunk.mesh.normals != nullptr) { MemFree(chunk.mesh.normals); }
            continue;
        }

        const auto base_color = color_for_material(run.material);
        auto* out = chunk.mesh.vertices;
        auto* out_uv = chunk.mesh.texcoords;
        auto* out_col = chunk.mesh.colors;
        auto* out_normal = chunk.mesh.normals;
        for (auto triangle_index = run.first_triangle; triangle_index < clamped_end; ++triangle_index) {
            const auto& triangle = buffer.triangles[triangle_index];
            std::array<Vec3, 3> p{};
            std::array<RenderBufferVertex, 3> v{};
            for (int i = 0; i < 3; ++i) {
                v[i] = triangle.indices[i] < buffer.vertices.size()
                    ? buffer.vertices[triangle.indices[i]]
                    : RenderBufferVertex{};
                p[i] = v[i].position;
            }
            const Vector3 e1{p[1].x - p[0].x, p[1].y - p[0].y, p[1].z - p[0].z};
            const Vector3 e2{p[2].x - p[0].x, p[2].y - p[0].y, p[2].z - p[0].z};
            const Vector3 face_normal{
                e1.y * e2.z - e1.z * e2.y,
                e1.z * e2.x - e1.x * e2.z,
                e1.x * e2.y - e1.y * e2.x,
            };
            const auto normal_length = std::sqrt(
                face_normal.x * face_normal.x +
                face_normal.y * face_normal.y +
                face_normal.z * face_normal.z);
            const auto normal_scale = normal_length > 1e-6F ? 1.0F / normal_length : 0.0F;
            for (int i = 0; i < 3; ++i) {
                *out++ = p[i].x;
                *out++ = p[i].y;
                *out++ = p[i].z;
                *out_uv++ = v[i].u;
                *out_uv++ = v[i].v;
                *out_col++ = 255;
                *out_col++ = 255;
                *out_col++ = 255;
                *out_col++ = 255;
                const auto normal = smooth_normals && triangle.indices[i] < vertex_normals.size()
                    ? vertex_normals[triangle.indices[i]]
                    : Vector3{face_normal.x * normal_scale, face_normal.y * normal_scale, face_normal.z * normal_scale};
                *out_normal++ = normal.x;
                *out_normal++ = normal.y;
                *out_normal++ = normal.z;
            }
        }

        UploadMesh(&chunk.mesh, false);
        chunk.material = LoadMaterialDefault();
        chunk.material.shader = game_surface_shader();
        chunk.material.maps[MATERIAL_MAP_DIFFUSE].color = base_color;
        if (const auto* texture = texture_for_buffer_material(project, buffer, run.material);
            texture != nullptr && texture->id != 0U) {
            chunk.material.maps[MATERIAL_MAP_DIFFUSE].texture = *texture;
            chunk.source_texture = *texture;
            chunk.textured = true;
        }
        const auto* material_texture = material_texture_for_buffer_material(project, buffer, run.material);
        chunk.material.maps[MATERIAL_MAP_METALNESS].texture = material_texture != nullptr && material_texture->id != 0U
            ? *material_texture
            : default_game_material_texture();
        model.uploaded_vertices += static_cast<std::size_t>(chunk.mesh.vertexCount);
        model.uploaded_bytes += static_cast<std::size_t>(chunk.mesh.vertexCount) * (3U * sizeof(float) + 2U * sizeof(float) + 4U);
        model.chunks.push_back(std::move(chunk));
    }
    return model;
}

void cache_shadow_faces(GpuBuildingRenderModel& model, const BuildingRenderBuffer& buffer) {
    // Cache opaque face adjacency once. Window triangles do not occlude, so
    // their openings remain openings in the building shadow silhouette.
    struct EdgeKey {
        std::array<std::int32_t, 3> first{};
        std::array<std::int32_t, 3> second{};
        bool operator==(const EdgeKey&) const = default;
    };
    struct EdgeHash {
        std::size_t operator()(const EdgeKey& edge) const {
            std::size_t hash = 1469598103934665603ULL;
            for (const auto value : {edge.first[0], edge.first[1], edge.first[2], edge.second[0], edge.second[1], edge.second[2]}) {
                hash ^= static_cast<std::uint32_t>(value);
                hash *= 1099511628211ULL;
            }
            return hash;
        }
    };
    struct EdgeOwner { std::size_t face{}; std::size_t edge{}; };
    const auto point_key = [](Vec3 point) {
        return std::array<std::int32_t, 3>{
            static_cast<std::int32_t>(std::lround(point.x * 1024.0F)),
            static_cast<std::int32_t>(std::lround(point.y * 1024.0F)),
            static_cast<std::int32_t>(std::lround(point.z * 1024.0F)),
        };
    };
    std::unordered_map<EdgeKey, EdgeOwner, EdgeHash> open_edges;
    model.shadow_faces.reserve(buffer.triangles.size());
    for (const auto& triangle : buffer.triangles) {
        if (triangle.city_window_surface ||
            triangle.indices[0] >= buffer.vertices.size() ||
            triangle.indices[1] >= buffer.vertices.size() ||
            triangle.indices[2] >= buffer.vertices.size()) continue;
        GpuBuildingRenderModel::ShadowFace face;
        for (std::size_t vertex = 0; vertex < 3; ++vertex) {
            face.vertices[vertex] = buffer.vertices[triangle.indices[vertex]].position;
        }
        const auto face_index = model.shadow_faces.size();
        model.shadow_faces.push_back(face);
        for (std::size_t edge = 0; edge < 3; ++edge) {
            auto first = point_key(face.vertices[edge]);
            auto second = point_key(face.vertices[(edge + 1U) % 3U]);
            if (second < first) std::swap(first, second);
            const EdgeKey key{first, second};
            if (const auto owner = open_edges.find(key); owner != open_edges.end()) {
                model.shadow_faces[face_index].adjacent[edge] = static_cast<std::int32_t>(owner->second.face);
                model.shadow_faces[owner->second.face].adjacent[owner->second.edge] = static_cast<std::int32_t>(face_index);
                open_edges.erase(owner);
            } else {
                open_edges.emplace(key, EdgeOwner{face_index, edge});
            }
        }
    }
}

GpuBuildingRenderModel upload_building_render_model(const BuildingRenderBuffer& buffer, const SbbBuilding& building, const EditorProject& project) {
    GpuBuildingRenderModel model;
    model.wire_lines = buffer.wire_lines;
    cache_shadow_faces(model, buffer);
    model.chunks.reserve(buffer.material_runs.size());
    for (const auto& run : buffer.material_runs) {
        auto chunk = upload_mesh_chunk(
            buffer,
            run.first_triangle,
            run.triangle_count,
            run.material,
            texture_for_material(project, building, run.material),
            material_texture_for_material(project, building, run.material),
            run.city_window_surface);
        if (chunk.mesh.vertexCount > 0) {
            model.uploaded_vertices += static_cast<std::size_t>(chunk.mesh.vertexCount);
            model.uploaded_bytes += static_cast<std::size_t>(chunk.mesh.vertexCount) * 3U * sizeof(float);
            model.uploaded_bytes += static_cast<std::size_t>(chunk.mesh.vertexCount) * 2U * sizeof(float);
            if (chunk.mesh.colors != nullptr) {
                model.uploaded_bytes += static_cast<std::size_t>(chunk.mesh.vertexCount) * 4U * sizeof(unsigned char);
            }
            if (chunk.mesh.indices != nullptr) {
                model.uploaded_indices += static_cast<std::size_t>(chunk.mesh.triangleCount) * 3U;
                model.uploaded_bytes += static_cast<std::size_t>(chunk.mesh.triangleCount) * 3U * sizeof(unsigned short);
            }
            model.chunks.push_back(std::move(chunk));
        }
    }
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

// Flat mask shader used by the screen-space outline pass. It renders the selected
// mesh into an offscreen alpha mask without changing the real scene color/depth.
struct MaskShader {
    Shader shader{};
    bool loaded{false};
};

MaskShader& mask_shader() {
    static MaskShader cache;
    if (!cache.loaded) {
        const char* vs =
            MAPED_GLSL_VERSION
            "in vec3 vertexPosition;\n"
            "uniform mat4 mvp;\n"
            "void main() { gl_Position = mvp * vec4(vertexPosition, 1.0); }\n";
        const char* fs =
            MAPED_GLSL_VERSION
            "out vec4 finalColor;\n"
            "void main() { finalColor = vec4(1.0); }\n";
        cache.shader = LoadShaderFromMemory(vs, fs);
        cache.loaded = true;
    }
    return cache;
}

// Sub Rosa's world shader reads color + material maps, lights them with its world
// light, and uses the color alpha as window shine for sky reflection. The editor
// keeps that contract and evaluates the native atmosphere used by its backdrop.
struct LitShader {
    Shader shader{};
    int view_position_loc{-1};
    int window_surface_loc{-1};
    int building_surface_loc{-1};
    int terrain_surface_loc{-1};
    int atmosphere_world_position_loc{-1};
    int light_direction_loc{-1};
    int light_ambient_loc{-1};
    int light_diffuse_loc{-1};
    int env_face_loc{-1};
    int ground_color_loc{-1};
    bool loaded{false};
};

struct GameLightColors {
    Vector3 ambient{};
    Vector3 diffuse{};
};

// Port of subrosa.exe WorldShading @0xa61f0. Optical depth through a sphere of
// radius 512 seen from radius 496 (constants @0x278aec/@0x278ae8/@0x278958),
// scaled by 0.029296875 (@0x278174); the fixed axis probes carry an extra
// sqrt(0.09765625) (@0x2781e8). Wavelengths @0x2b23d0 are (0.5, 0.625, 1.0).
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

// Lit landscape below the horizon: landscape.frag with the generated grass
// color (0x40,0x54,0x26) and a mid-grey detail texel, n = up.
Vector3 game_ground_color(Vector3 light) {
    const auto colors = game_light_colors(light);
    const Vector3 grass_srgb{0x40/255.0F, 0x54/255.0F, 0x26/255.0F};
    const auto to_linear = [](float c) {
        return c < 0.04045F ? c/12.92F : std::pow((c + 0.055F)/1.055F, 2.4F);
    };
    float ground_scale = 0.25F + std::max(light.y, 0.0F)*0.75F;
    ground_scale = (4.0F/3.0F)*(1.0F - 1.0F/(1.0F + 3.0F*ground_scale));
    Vector3 ground{};
    for (int i = 0; i < 3; ++i) {
        const float base = to_linear((&grass_srgb.x)[i])*0.5F;
        (&ground.x)[i] = base*(&colors.ambient.x)[i]*(0.375F + light.y*0.125F) +
                         base*(&colors.diffuse.x)[i]*ground_scale*0.625F;
    }
    return ground;
}

// CPU twin of the shader skyColor() without the sun disc, used to average the
// sky over each cube face the way the game's blurred landscape cubemap does.
Vector3 analytic_sky_color(Vector3 direction, Vector3 light, Vector3 world_position) {
    constexpr float atmo_x = 0.0502275936F;
    constexpr float atmo_z = 6.4F;
    constexpr float atmo_w = 637.1F;
    const Vector3 wavelength{0.625F, 0.75F, 1.0F};
    const auto ray_distance = [](Vector3 position, Vector3 dir, float radius) {
        const float scale = Vector3DotProduct(position, dir);
        const float b2 = Vector3DotProduct(position, position) - scale*scale;
        return std::sqrt(std::max(0.0F, radius*radius - b2)) - scale;
    };
    const auto ray_distance2 = [](Vector3 position, Vector3 dir, float radius) {
        const float scale = -Vector3DotProduct(position, dir);
        if (scale < 0.0F) return 0.0F;
        const float b2 = Vector3DotProduct(position, position) - scale*scale;
        if (b2 > radius*radius) return 0.0F;
        return scale - std::sqrt(std::max(0.0F, radius*radius - b2));
    };
    const auto view = Vector3Normalize(direction);
    float view_length = ray_distance2(world_position, view, atmo_w);
    if (view_length <= 0.0F) view_length = ray_distance(world_position, view, atmo_w + atmo_z);
    float phase = Vector3DotProduct(view, light);
    phase = 3.0F/(16.0F*PI)*(1.0F + phase*phase);
    const Vector3 out_scatter = Vector3Scale(wavelength, atmo_x);
    const Vector3 in_scatter = Vector3Scale(wavelength, phase*atmo_x);
    Vector3 color{};
    float dist = 1.0F - 0.125F;
    for (int count = 0; count < 4; ++count) {
        const auto ray_position = Vector3Add(world_position, Vector3Scale(view, view_length*dist));
        const float ray_length = ray_distance(ray_position, light, atmo_w + atmo_z);
        float h = 1.0F - (Vector3Length(ray_position) - atmo_w)/atmo_z;
        h = std::clamp(h, 0.0F, 1.0F);
        h *= h;
        const float density = h*8.0F;
        for (int i = 0; i < 3; ++i) {
            const float rayleigh = (&in_scatter.x)[i]*density*8.0F*std::exp(-ray_length*density*0.25F*atmo_x*(&wavelength.x)[i]);
            const float extinction = (&out_scatter.x)[i]*density*0.5F;
            const float transmit = std::exp(-extinction*view_length*0.25F);
            (&color.x)[i] = (&color.x)[i]*transmit + (1.0F - transmit)*(rayleigh/std::max(extinction, 1e-8F));
        }
        dist -= 0.25F;
    }
    return color;
}

LitShader& lit_shader() {
    static LitShader cache;
    if (!cache.loaded) {
        const char* vs = MAPED_GLSL_VERSION R"glsl(
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;
out vec2 fragTexCoord;
out vec3 fragNormal;
out vec3 fragPosition;
void main() {
    fragTexCoord = vertexTexCoord;
    fragNormal = normalize((matNormal * vec4(vertexNormal, 0.0)).xyz);
    fragPosition = (matModel * vec4(vertexPosition, 1.0)).xyz;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)glsl";
        const char* fs = MAPED_GLSL_VERSION R"glsl(
in vec2 fragTexCoord;
in vec3 fragNormal;
in vec3 fragPosition;
uniform sampler2D texture0;
uniform sampler2D texture1;
uniform vec4 colDiffuse;
uniform vec3 viewPosition;
uniform vec3 atmosphereWorldPosition;
uniform int windowSurface;
uniform int buildingSurface;
uniform int terrainSurface;
uniform vec3 lightDirection;
// subrosa.exe WorldShading @0xa61f0 derives these from the sun height every
// frame; the world shader only reads lightdiffuse, the landscape shader both.
uniform vec3 lightAmbient;
uniform vec3 lightDiffuse;
// Per-face averages of the game's blurred landscape cubemap (+x,-x,+y,-y,+z,-z)
// and the lit ground color below the horizon. world.frag samples the cubemap
// at lod 8 (one texel per face) for diffuse environment light.
uniform vec3 envFace[6];
uniform vec3 groundColor;
layout(location=0) out vec4 finalColor;
layout(location=1) out vec4 packedDepth;
layout(location=2) out vec4 directLight;
const float PI = 3.1415926;
vec3 envDiffuse(vec3 n) {
    vec3 w = n*n;
    return w.x*(n.x > 0.0 ? envFace[0] : envFace[1])
         + w.y*(n.y > 0.0 ? envFace[2] : envFace[3])
         + w.z*(n.z > 0.0 ? envFace[4] : envFace[5]);
}
const vec4 ATMOSPHERE = vec4(0.0502275936, 0.0001, 6.4, 637.1);
const vec3 WAVELENGTH = vec3(0.625, 0.75, 1.0);
vec3 toLinear(vec3 color) {
    bvec3 cutoff = lessThan(color, vec3(0.04045));
    return mix(pow((color + vec3(0.055))/vec3(1.055), vec3(2.4)), color/vec3(12.92), cutoff);
}
vec3 toSrgb(vec3 color) {
    bvec3 cutoff = lessThan(color, vec3(0.0031308));
    return mix(vec3(1.055)*pow(max(color, vec3(0.0)), vec3(1.0/2.4))-vec3(0.055), color*vec3(12.92), cutoff);
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
float atmosphereRayDistance(vec3 position, vec3 direction, float radius) {
    float scale = dot(position, direction);
    float b2 = dot(position, position)-scale*scale;
    return sqrt(max(0.0, radius*radius-b2))-scale;
}
float atmosphereRayDistance2(vec3 position, vec3 direction, float radius) {
    float scale = -dot(position, direction);
    if (scale < 0.0) return 0.0;
    float b2 = dot(position, position)-scale*scale;
    if (b2 > radius*radius) return 0.0;
    return scale-sqrt(max(0.0, radius*radius-b2));
}
vec3 skyColor(vec3 direction) {
    vec3 viewNormal = normalize(direction);
    float viewLength = atmosphereRayDistance2(atmosphereWorldPosition, viewNormal, ATMOSPHERE.w);
    if (viewLength <= 0.0) viewLength = atmosphereRayDistance(atmosphereWorldPosition, viewNormal, ATMOSPHERE.w+ATMOSPHERE.z);
    float phase = dot(viewNormal, lightDirection);
    phase = 3.0/(16.0*PI)*(1.0+phase*phase);
    vec3 outScatter = WAVELENGTH*ATMOSPHERE.x;
    vec3 inScatter = phase*WAVELENGTH*ATMOSPHERE.x;
    vec3 color = vec3(0.0);
    if (viewLength > 4.0) {
        float sun = max(dot(viewNormal, lightDirection)-0.9998, 0.0)*262144.0;
        color = vec3(min(sun, 16.0));
    }
    float inverseSamples = 0.25;
    float dist = 1.0-inverseSamples*0.5;
    for (int count = 0; count < 4; ++count) {
        vec3 rayPosition = atmosphereWorldPosition+viewNormal*viewLength*dist;
        float rayLength = atmosphereRayDistance(rayPosition, lightDirection, ATMOSPHERE.w+ATMOSPHERE.z);
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
    return color;
}
void main() {
    // The game uses mipmapped linear minification. Add a small distance-only
    // bias so sub-pixel facade and window texels converge on a stable mip
    // instead of alternating between bright and dark samples while the camera
    // moves. Near surfaces retain their full-resolution texture.
    float textureDistance = length(viewPosition-fragPosition);
    float mipBias = smoothstep(160.0, 900.0, textureDistance)*0.65;
    vec4 textureSample = texture(texture0, fragTexCoord, mipBias);
    vec4 sampled = textureSample * colDiffuse;
    vec3 albedo = toLinear(sampled.rgb);
    // Native world.frag applies the same sRGB-to-linear conversion to the
    // material texture before reading its metallic and roughness channels.
    vec3 material = toLinear(texture(texture1, fragTexCoord, mipBias).rgb);
    float metallic = clamp(material.r, 0.0, 1.0);
    float roughness = clamp(material.g, 0.0, 1.0);
    vec3 n = buildingSurface == 1
        ? normalize(cross(dFdx(fragPosition), dFdy(fragPosition)))
        : normalize(fragNormal);
    if (!gl_FrontFacing) n = -n;
    vec3 v = normalize(viewPosition-fragPosition);
    vec3 color;
    vec3 direct;
    if (terrainSurface == 1) {
        // landscape.frag: wrapped diffuse with rho=3, ambient keyed to the sun
        // height, and a detail-driven view shine. texture0 is tex_detail and
        // colDiffuse stands in for the generated tex_color landscape map.
        vec3 base = toLinear(textureSample.rgb)*toLinear(colDiffuse.rgb);
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
        // Diffuse environment: one cubemap texel per face (lod 8).
        diffuse += envDiffuse(n)*0.75;
        diffuse *= 1.5;
        // Specular environment: lod roughness^2*8, so smooth surfaces see the
        // sharp sky/ground and rough ones converge on the face average.
        vec3 r = reflect(-v, n);
        vec3 sharp = r.y >= 0.0 ? skyColor(r) : groundColor;
        vec3 reflected = mix(sharp, envDiffuse(r), clamp(roughness*roughness*8.0/6.0, 0.0, 1.0));
        specular += reflected*0.75*fresnelFactor(mix(vec3(0.02), albedo, metallic), ndv)*(1.0-roughness*0.5);
        color = diffuse*mix(albedo, vec3(0.0), metallic)+specular;
        if (windowSurface == 1) {
            // array_world.frag window shine: sharp cubemap reflection faded
            // toward half the ambient blue at grazing angles.
            float scale = 1.0-abs(dot(-v, n));
            scale = 0.5+pow(scale, 2.5)*0.5;
            vec3 reflection = sharp*scale+vec3((1.0-scale)*0.5*lightAmbient.b);
            color = mix(color, reflection, textureSample.a);
            direct *= 1.0-textureSample.a;
        }
    }
    finalColor = vec4(color, windowSurface == 1 ? 1.0 : sampled.a);
    directLight = vec4(direct, windowSurface == 1 ? 1.0 : sampled.a);
    float fragmentDepth = length(viewPosition-fragPosition);
    vec3 encodedDepth = fract(vec3(fragmentDepth*0.0000152587890625, fragmentDepth*0.00390625, fragmentDepth));
    packedDepth = vec4(floor(encodedDepth*256.0)/255.0, 1.0);
}
)glsl";
        cache.shader = LoadShaderFromMemory(vs, fs);
        cache.shader.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocation(cache.shader, "matModel");
        cache.shader.locs[SHADER_LOC_MATRIX_NORMAL] = GetShaderLocation(cache.shader, "matNormal");
        cache.shader.locs[SHADER_LOC_MAP_METALNESS] = GetShaderLocation(cache.shader, "texture1");
        cache.view_position_loc = GetShaderLocation(cache.shader, "viewPosition");
        cache.window_surface_loc = GetShaderLocation(cache.shader, "windowSurface");
        cache.building_surface_loc = GetShaderLocation(cache.shader, "buildingSurface");
        cache.terrain_surface_loc = GetShaderLocation(cache.shader, "terrainSurface");
        cache.atmosphere_world_position_loc = GetShaderLocation(cache.shader, "atmosphereWorldPosition");
        cache.light_direction_loc = GetShaderLocation(cache.shader, "lightDirection");
        cache.light_ambient_loc = GetShaderLocation(cache.shader, "lightAmbient");
        cache.light_diffuse_loc = GetShaderLocation(cache.shader, "lightDiffuse");
        cache.env_face_loc = GetShaderLocation(cache.shader, "envFace");
        cache.ground_color_loc = GetShaderLocation(cache.shader, "groundColor");
        cache.loaded = true;
    }
    return cache;
}

Shader game_surface_shader() {
    return lit_shader().shader;
}

Texture2D default_game_material_texture() {
    static Texture2D texture = [] {
        // The material map is decoded from sRGB by world.frag. 188 encodes a
        // linear roughness of about 0.5; zero red keeps fallback surfaces
        // dielectric instead of making every missing material partly metallic.
        Image image = GenImageColor(1, 1, Color{0, 188, 0, 255});
        auto loaded = LoadTextureFromImage(image);
        UnloadImage(image);
        SetTextureWrap(loaded, TEXTURE_WRAP_REPEAT);
        SetTextureFilter(loaded, TEXTURE_FILTER_BILINEAR);
        return loaded;
    }();
    return texture;
}

void set_game_surface_camera(Vector3 position) {
    auto& shader = lit_shader();
    if (shader.shader.id != 0U && shader.view_position_loc >= 0) {
        SetShaderValue(shader.shader, shader.view_position_loc, &position.x, SHADER_UNIFORM_VEC3);
        const Vector3 atmosphere_position{
            position.x*0.0001F,
            position.y*0.0001F+637.1F,
            position.z*0.0001F,
        };
        SetShaderValue(shader.shader, shader.atmosphere_world_position_loc, &atmosphere_position.x, SHADER_UNIFORM_VEC3);
        const auto light = Vector3Normalize(game_light_direction());
        SetShaderValue(shader.shader, shader.light_direction_loc, &light.x, SHADER_UNIFORM_VEC3);
        const auto colors = game_light_colors(light);
        SetShaderValue(shader.shader, shader.light_ambient_loc, &colors.ambient.x, SHADER_UNIFORM_VEC3);
        SetShaderValue(shader.shader, shader.light_diffuse_loc, &colors.diffuse.x, SHADER_UNIFORM_VEC3);

        const Vector3 ground = game_ground_color(light);
        SetShaderValue(shader.shader, shader.ground_color_loc, &ground.x, SHADER_UNIFORM_VEC3);

        // Cube-face averages of sky (upper hemisphere) and ground (lower), the
        // lod-8 cubemap texel world.frag reads. Cached on the light direction;
        // the 0.0001-scaled camera offset is negligible at this blur level.
        static Vector3 cached_light{0.0F, 0.0F, 0.0F};
        static std::array<Vector3, 6> faces{};
        static Vector3 cached_ground{};
        if (Vector3LengthSqr(Vector3Subtract(cached_light, light)) > 1e-8F ||
            Vector3LengthSqr(Vector3Subtract(cached_ground, ground)) > 1e-10F) {
            cached_light = light;
            cached_ground = ground;
            const Vector3 origin{0.0F, 637.1F, 0.0F};
            constexpr int grid = 6;
            for (int face = 0; face < 6; ++face) {
                Vector3 sum{};
                for (int u = 0; u < grid; ++u) {
                    for (int w = 0; w < grid; ++w) {
                        const float a = (static_cast<float>(u) + 0.5F)/grid*2.0F - 1.0F;
                        const float b = (static_cast<float>(w) + 0.5F)/grid*2.0F - 1.0F;
                        const float sign = (face & 1) != 0 ? -1.0F : 1.0F;
                        Vector3 dir{};
                        switch (face >> 1) {
                            case 0: dir = {sign, a, b}; break;
                            case 1: dir = {a, sign, b}; break;
                            default: dir = {a, b, sign}; break;
                        }
                        dir = Vector3Normalize(dir);
                        sum = Vector3Add(sum, dir.y >= 0.0F ? analytic_sky_color(dir, light, origin) : ground);
                    }
                }
                faces[static_cast<std::size_t>(face)] = Vector3Scale(sum, 1.0F/(grid*grid));
            }
        }
        SetShaderValueV(shader.shader, shader.env_face_loc, faces.data(), SHADER_UNIFORM_VEC3, 6);
    }
}

void set_game_surface_terrain(bool enabled) {
    auto& shader = lit_shader();
    const int value = enabled ? 1 : 0;
    if (shader.shader.id != 0U && shader.terrain_surface_loc >= 0) {
        SetShaderValue(shader.shader, shader.terrain_surface_loc, &value, SHADER_UNIFORM_INT);
    }
}

void set_game_world_time(float hours) {
    g_world_time_hours = std::clamp(hours, 0.0F, 24.0F);
}

void set_game_surface_window(bool enabled) {
    auto& shader = lit_shader();
    const int value = enabled ? 1 : 0;
    if (shader.shader.id != 0U && shader.window_surface_loc >= 0) {
        SetShaderValue(shader.shader, shader.window_surface_loc, &value, SHADER_UNIFORM_INT);
    }
}

void set_game_surface_building(bool enabled) {
    auto& shader = lit_shader();
    const int value = enabled ? 1 : 0;
    if (shader.shader.id != 0U && shader.building_surface_loc >= 0) {
        SetShaderValue(shader.shader, shader.building_surface_loc, &value, SHADER_UNIFORM_INT);
    }
}

struct SkyShader {
    Shader shader{};
    int light_direction_loc{-1};
    int world_position_loc{-1};
    int depth_texture_loc{-1};
    int inverse_resolution_loc{-1};
    int ground_color_loc{-1};
    int mvp_loc{-1};
    unsigned int vao{0};
    unsigned int vbo{0};
    bool loaded{false};
};

SkyShader& sky_shader() {
    static SkyShader cache;
    if (!cache.loaded) {
        const char* vs = MAPED_GLSL_VERSION R"glsl(
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
        const char* fs = MAPED_GLSL_VERSION R"glsl(
out vec4 finalColor;
in vec3 lightvec;
in vec3 viewvec;
uniform vec3 worldPosition;
uniform vec3 groundColor;
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
    // The game always has landscape under the horizon; without terrain loaded
    // show the lit ground color instead of the black inside of the atmosphere.
    if (skyPixel && viewNormal.y < 0.0) {
        finalColor.rgb = mix(finalColor.rgb, groundColor, clamp(-viewNormal.y*8.0, 0.0, 1.0));
    }
}
)glsl";
        cache.shader = LoadShaderFromMemory(vs, fs);
        cache.light_direction_loc = GetShaderLocation(cache.shader, "lightdirection");
        cache.world_position_loc = GetShaderLocation(cache.shader, "worldPosition");
        cache.depth_texture_loc = GetShaderLocation(cache.shader, "depthTexture");
        cache.inverse_resolution_loc = GetShaderLocation(cache.shader, "inverseResolution");
        cache.ground_color_loc = GetShaderLocation(cache.shader, "groundColor");
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

void draw_game_sky(const Camera3D& camera, Vector2 viewport_size, Texture2D depth_texture) {
    if (viewport_size.x <= 0.0F || viewport_size.y <= 0.0F) {
        return;
    }
    if (depth_texture.id == 0U) {
        return;
    }
    auto& sky = sky_shader();
    const auto forward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    const auto right = Vector3Normalize(Vector3CrossProduct(forward, camera.up));
    const auto up = Vector3Normalize(Vector3CrossProduct(right, forward));
    const float aspect = viewport_size.x / viewport_size.y;
    const float vertical_fov = std::tan(camera.fovy * DEG2RAD * 0.5F);
    const float horizontal_fov = vertical_fov*aspect;
    // Native reset-time daylight: counter 0x244140 converted to the world-light
    // angle, rotating normalized (0, 1, 0.5) around (0, 1/sqrt(5), -2/sqrt(5)).
    const Vector3 light_direction = game_light_direction();
    const Vector3 world_position{
        camera.position.x*0.0001F,
        camera.position.y*0.0001F+637.1F,
        camera.position.z*0.0001F,
    };
    const Vector2 inverse_resolution{1.0F/viewport_size.x, 1.0F/viewport_size.y};
    const auto ray = [&](float horizontal, float vertical) {
        return Vector3Add(forward, Vector3Add(Vector3Scale(right, horizontal), Vector3Scale(up, vertical)));
    };
    const Vector3 rays[] = {
        ray(-horizontal_fov, -vertical_fov),
        ray( horizontal_fov, -vertical_fov),
        ray( horizontal_fov,  vertical_fov),
        ray(-horizontal_fov,  vertical_fov),
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
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_SRC_ALPHA);
    glUseProgram(sky.shader.id);
    glUniform3fv(sky.light_direction_loc, 1, &light_direction.x);
    glUniform3fv(sky.world_position_loc, 1, &world_position.x);
    glUniform2fv(sky.inverse_resolution_loc, 1, &inverse_resolution.x);
    const auto ground = game_ground_color(Vector3Normalize(light_direction));
    glUniform3fv(sky.ground_color_loc, 1, &ground.x);
    glUniformMatrix4fv(sky.mvp_loc, 1, GL_FALSE, identity);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, depth_texture.id);
    glUniform1i(sky.depth_texture_loc, 0);
    glBindVertexArray(sky.vao);
    glBindBuffer(GL_ARRAY_BUFFER, sky.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), vertices);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(rlGetShaderIdDefault());
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
            MAPED_GLSL_VERSION
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
    auto& mask = mask_shader();
    if (mask.shader.id == 0) {
        return;
    }
    constexpr auto unit_size = 4.0F;
    const auto render_origin = native_city_building_render_origin(placement, unit_size);
    const auto origin = Vector3{render_origin.x, render_origin.y, render_origin.z};
    const auto transform = building_transform_matrix(origin, placement.rotation);

    rlDisableBackfaceCulling();
    for (auto& chunk : model.chunks) {
        if (chunk.mesh.vertexCount <= 0 || chunk.material.maps == nullptr) {
            continue;
        }
        const auto saved = chunk.material.shader;
        chunk.material.shader = mask.shader;
        DrawMesh(chunk.mesh, chunk.material, transform);
        chunk.material.shader = saved;
        if (g_gfx_debug_stats != nullptr) {
            g_gfx_debug_stats->add_draw(GfxDebugDrawCategory::WireOverlay,
                static_cast<std::uint64_t>(std::max(chunk.mesh.triangleCount, 0)),
                static_cast<std::uint64_t>(std::max(chunk.mesh.vertexCount, 0)), 1U);
        }
    }
    rlEnableBackfaceCulling();
}

// Fills the footprint of one road into the active mask target as solid geometry,
// so draw_outline_from_mask traces a silhouette outline around it (the same look
// city buildings get). Drawn at the road's terrain heights.
void draw_city_road_mask(const EditorProject& project, const MapScene& scene, std::size_t street_index) {
    if (!project.city || street_index >= project.city->data.streets.size()) {
        return;
    }
    // Fill the street's actual road tiles (the asphalt you clicked) into the mask,
    // so the silhouette outline traces the real geometry, not an abstract ribbon.
    const auto unit = scene.unit_size > 0.0F ? scene.unit_size : 4.0F;
    rlDisableBackfaceCulling();
    bool drew = false;
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
        // Match the real tilted asphalt top: each corner gets the tile's slope so
        // the mask sits flush with the rendered road (a flat quad floated above the
        // up-slope edge and below the down-slope edge, so the outline only traced
        // part of it).
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
        drew = true;
    }
    rlEnableBackfaceCulling();
    (void)drew;
}

// Fills one building cell (a cube at its anchored centre) into the active mask
// target, so the cell gets the same silhouette-outline hover/selection look.
void draw_building_cell_mask(const SbbBuilding& building, const SbbCell& cell) {
    constexpr auto unit_size = 4.0F;
    const Vector3 center{
        (static_cast<float>(cell.x) - static_cast<float>(building.anchor_x) + 0.5F) * unit_size,
        (static_cast<float>(cell.y) - static_cast<float>(building.anchor_y) + 0.5F) * unit_size,
        (static_cast<float>(cell.z) - static_cast<float>(building.anchor_z) + 0.5F) * unit_size,
    };
    DrawCubeV(center, {unit_size, unit_size, unit_size}, WHITE);
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
    bool selected,
    bool draw_wires,
    GfxDebugDrawCategory category = GfxDebugDrawCategory::CityCells
) {
    const auto transform = building_transform_matrix(origin, rotation);
    set_game_render_depth_output(true);
    rlDisableBackfaceCulling();
    for (auto& chunk : model.chunks) {
        if (chunk.material.maps == nullptr || chunk.mesh.vertexCount <= 0) {
            continue;
        }
        chunk.material.maps[MATERIAL_MAP_DIFFUSE].texture = chunk.source_texture;
        chunk.material.maps[MATERIAL_MAP_DIFFUSE].color = chunk.textured
            ? (selected ? Color{255, 236, 170, 255} : WHITE)
            : native_material_color(chunk.material_id, selected, chunk.window_material);
        set_game_surface_window(chunk.window_material);
        set_game_surface_building(true);
        DrawMesh(chunk.mesh, chunk.material, transform);
        if (g_gfx_debug_stats != nullptr) {
            g_gfx_debug_stats->add_draw(
                category,
                static_cast<std::uint64_t>(std::max(chunk.mesh.triangleCount, 0)),
                static_cast<std::uint64_t>(std::max(chunk.mesh.vertexCount, 0)),
                1U);
        }
    }
    rlEnableBackfaceCulling();
    set_game_render_depth_output(false);

    if (!draw_wires) {
        return;
    }
    const auto wire = selected ? Color{255, 240, 150, 255} : Color{40, 45, 50, 255};
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

void draw_sbb_scene_for_app(
    const SbbBuilding& building,
    GpuBuildingRenderModel& model,
    const SbbCell* selected,
    std::uint32_t guide_floor) {
    constexpr auto unit_size = 4.0F;
    constexpr std::uint32_t grid_cells = 64;
    const auto floor = std::min(guide_floor, building.size_z > 0U ? building.size_z - 1U : 0U);
    const auto grid_y = (static_cast<float>(floor) - static_cast<float>(building.anchor_y)) * unit_size + 0.02F;
    const auto grid_min_x = -static_cast<float>(building.anchor_x) * unit_size;
    const auto grid_min_z = -static_cast<float>(building.anchor_z) * unit_size;
    const auto grid_max_x = (static_cast<float>(grid_cells) - static_cast<float>(building.anchor_x)) * unit_size;
    const auto grid_max_z = (static_cast<float>(grid_cells) - static_cast<float>(building.anchor_z)) * unit_size;

    const Vector3 anchor_offset{
        -static_cast<float>(building.anchor_x) * unit_size,
        -static_cast<float>(building.anchor_y) * unit_size,
        -static_cast<float>(building.anchor_z) * unit_size,
    };
    draw_gpu_building_model(model, anchor_offset, 0, false, true, GfxDebugDrawCategory::CityCells);
    rlDrawRenderBatchActive();
    rlDisableDepthTest();
    rlDisableDepthMask();

    // editor.exe draws a fixed 64-cell work grid with 65 lines in each axis.
    // Every fourth line is stronger so block-sized groups remain legible.
    for (std::uint32_t index = 0; index <= grid_cells; ++index) {
        const auto color = index % 4U == 0U
            ? Color{105, 125, 140, 180}
            : Color{70, 82, 92, 120};
        const auto x = (static_cast<float>(index) - static_cast<float>(building.anchor_x)) * unit_size;
        const auto z = (static_cast<float>(index) - static_cast<float>(building.anchor_z)) * unit_size;
        DrawLine3D({x, grid_y, grid_min_z}, {x, grid_y, grid_max_z}, color);
        DrawLine3D({grid_min_x, grid_y, z}, {grid_max_x, grid_y, z}, color);
        if (index % 4U == 0U) {
            DrawCylinderEx({x, grid_y, grid_min_z}, {x, grid_y, grid_max_z},
                unit_size * 0.025F, unit_size * 0.025F, 4, color);
            DrawCylinderEx({grid_min_x, grid_y, z}, {grid_max_x, grid_y, z},
                unit_size * 0.025F, unit_size * 0.025F, 4, color);
        }
    }

    // Show the authored footprint in the same anchor-relative coordinates as
    // the mesh and picker.
    const auto bound_max_x = (static_cast<float>(building.size_x) - static_cast<float>(building.anchor_x)) * unit_size;
    const auto bound_max_z = (static_cast<float>(building.size_y) - static_cast<float>(building.anchor_z)) * unit_size;
    const auto bound_color = Color{220, 74, 72, 230};
    DrawLine3D({grid_min_x, grid_y, grid_min_z}, {bound_max_x, grid_y, grid_min_z}, bound_color);
    DrawLine3D({bound_max_x, grid_y, grid_min_z}, {bound_max_x, grid_y, bound_max_z}, bound_color);
    DrawLine3D({bound_max_x, grid_y, bound_max_z}, {grid_min_x, grid_y, bound_max_z}, bound_color);
    DrawLine3D({grid_min_x, grid_y, bound_max_z}, {grid_min_x, grid_y, grid_min_z}, bound_color);
    // The tool preview mesh is built in un-anchored cell space (cell.x*unit), but
    // the picker and selection box live in anchored space ((cell.x-anchor)*unit).
    // Translate the model by -anchor*unit so geometry, selection, and clicks all
    // share one origin (fixes the constant anchor offset between them).
    rlDrawRenderBatchActive();
    rlEnableDepthMask();
    rlEnableDepthTest();

    if (selected != nullptr) {
        const auto center = Vector3{
            (static_cast<float>(selected->x) - static_cast<float>(building.anchor_x) + 0.5F) * unit_size,
            (static_cast<float>(selected->y) - static_cast<float>(building.anchor_y) + 0.5F) * unit_size,
            (static_cast<float>(selected->z) - static_cast<float>(building.anchor_z) + 0.5F) * unit_size,
        };
        draw_selection_glow_box(center, {unit_size, unit_size, unit_size}, Color{255, 225, 90, 255});
    }
}

void invalidate_building_thumbnail(EditorProject& project, std::size_t index) {
    if (index < project.building_thumbnail_ready.size()) {
        project.building_thumbnail_ready[index] = false;
    }
}

// Rebuilds and re-uploads just one building's city mesh, instead of dirtying the
// whole project (which reloads every building). Used when a single building
// template is edited in the Buildings tab.
void rebuild_single_building_render(EditorProject& project, std::size_t index) {
    if (index >= project.buildings.size()) {
        return;
    }
    if (project.building_render_buffers.size() != project.buildings.size()) {
        project.building_render_buffers.resize(project.buildings.size());
    }
    if (project.building_gpu_models.size() != project.buildings.size()) {
        project.building_gpu_models.resize(project.buildings.size());
    }
    const auto blocks = block_meshes(project);
    const auto templates = buildblock_templates(project);
    project.building_render_buffers[index] =
        build_city_building_full_render_buffer(project.buildings[index].data, blocks, templates);
    project.building_gpu_models[index] =
        upload_building_render_model(project.building_render_buffers[index], project.buildings[index].data, project);
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

    constexpr int kSize = 192; // render larger than displayed for crisp downscale.
    constexpr float unit_size = 4.0F;
    auto& building = project.buildings[index].data;

    // Reuse the city GPU model when it is already uploaded; only fall back to a
    // fresh CPU build for buildings whose render model is not ready yet.
    GpuBuildingRenderModel local_model;
    GpuBuildingRenderModel* model_ptr = nullptr;
    if (index < project.building_gpu_models.size() && !project.building_gpu_models[index].chunks.empty()) {
        model_ptr = &project.building_gpu_models[index];
    } else {
        const auto& blocks = block_meshes(project);
        const auto& templates = buildblock_templates(project);
        const auto buffer = build_native_building_tool_preview_render_buffer(building, blocks, templates, std::nullopt);
        local_model = upload_building_render_model(buffer, building, project);
        model_ptr = &local_model;
    }
    auto& model = *model_ptr;

    auto& target = project.building_thumbnails[index];
    if (target.id == 0U) {
        target = LoadRenderTexture(kSize, kSize);
    }

    // Frame the whole building from a 3/4 view.
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

    const Vector3 anchor_offset{
        -static_cast<float>(building.anchor_x) * unit_size,
        -static_cast<float>(building.anchor_y) * unit_size,
        -static_cast<float>(building.anchor_z) * unit_size,
    };
    begin_game_texture_mode(target);
    ClearBackground(Color{32, 36, 41, 255});
    BeginMode3D(cam);
    set_game_surface_camera(cam.position);
    rlEnableDepthTest();
    draw_gpu_building_model(model, anchor_offset, 0, false, false, GfxDebugDrawCategory::CityCells);
    // A subtle ground plane gives the building a sense of scale/orientation.
    DrawGrid(static_cast<int>(std::max(building.size_x, building.size_y)) + 2, unit_size);
    EndMode3D();
    end_game_texture_mode();
    GenTextureMipmaps(&target.texture);
    SetTextureFilter(target.texture, TEXTURE_FILTER_BILINEAR);

    if (model_ptr == &local_model) {
        unload_city_render_model(local_model);
    }
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
    // Archive filenames are identities. A shared display name must never steal
    // a placement from an exact asset-name match.
    const auto key = lower_name(name);
    const auto exact = std::ranges::find_if(project.buildings, [&](const auto& asset) {
        return lower_name(asset.name) == key;
    });
    if (exact != project.buildings.end()) return &*exact;
    const LoadedAsset<SbbBuilding>* alias = nullptr;
    for (const auto& asset : project.buildings) if (lower_name(asset.data.name) == key) {
        if (alias != nullptr) return nullptr; // Ambiguous legacy display name.
        alias = &asset;
    }
    return alias;
}

const std::vector<std::optional<std::size_t>>& resolve_placement_asset_indices(EditorProject& project) {
    const auto building_count = project.city ? project.city->data.buildings.size() : 0U;
    const auto fingerprint = project.city ? city_scene_fingerprint(project.city->data) : 0U;
    // ponytail: fingerprint already walks every placement's name/transform for
    // the scene-rebuild check above, so reusing it here to detect renames is
    // cheap and avoids threading a dirty flag through every mutation site.
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

void draw_city_building_model(
    const SbbBuilding& template_file,
    GpuBuildingRenderModel& model,
    const SbcBuilding& placement,
    bool selected,
    bool use_lod,
    bool hovered) {
    constexpr auto unit_size = 4.0F;
    (void)hovered;
    const auto render_origin = native_city_building_render_origin(placement, unit_size);
    const auto origin = Vector3{render_origin.x, render_origin.y, render_origin.z};
    // Solid neutral grey: a see-through blue box read as a hover/ghost highlight.
    const auto fill = selected ? Color{235, 194, 82, 255} : Color{118, 122, 128, 255};
    const auto wire = selected ? Color{255, 240, 150, 255} : Color{40, 44, 50, 255};

    if (use_lod) {
        const float min_x = -static_cast<float>(template_file.anchor_x) * unit_size;
        const float max_x = (static_cast<float>(template_file.size_x) - static_cast<float>(template_file.anchor_x)) * unit_size;
        const float min_y = -static_cast<float>(template_file.anchor_y) * unit_size;
        const float max_y = (static_cast<float>(template_file.size_z) - static_cast<float>(template_file.anchor_y)) * unit_size;
        const float min_z = -static_cast<float>(template_file.anchor_z) * unit_size;
        const float max_z = (static_cast<float>(template_file.size_y) - static_cast<float>(template_file.anchor_z)) * unit_size;

        const std::array<Vector3, 8> corners{
            Vector3{min_x, min_y, min_z},
            Vector3{max_x, min_y, min_z},
            Vector3{max_x, max_y, min_z},
            Vector3{min_x, max_y, min_z},
            Vector3{min_x, min_y, max_z},
            Vector3{max_x, min_y, max_z},
            Vector3{max_x, max_y, max_z},
            Vector3{min_x, max_y, max_z},
        };

        std::array<Vector3, 8> world_corners;
        for (std::size_t i = 0; i < 8; ++i) {
            world_corners[i] = city_transform(origin, corners[i], placement.rotation);
        }

        // Draw rotated cube faces
        // Front face
        DrawTriangle3D(world_corners[0], world_corners[1], world_corners[2], fill);
        DrawTriangle3D(world_corners[0], world_corners[2], world_corners[3], fill);
        // Back face
        DrawTriangle3D(world_corners[5], world_corners[4], world_corners[7], fill);
        DrawTriangle3D(world_corners[5], world_corners[7], world_corners[6], fill);
        // Left face
        DrawTriangle3D(world_corners[4], world_corners[0], world_corners[3], fill);
        DrawTriangle3D(world_corners[4], world_corners[3], world_corners[7], fill);
        // Right face
        DrawTriangle3D(world_corners[1], world_corners[5], world_corners[6], fill);
        DrawTriangle3D(world_corners[1], world_corners[6], world_corners[2], fill);
        // Top face
        DrawTriangle3D(world_corners[3], world_corners[2], world_corners[6], fill);
        DrawTriangle3D(world_corners[3], world_corners[6], world_corners[7], fill);
        // Bottom face
        DrawTriangle3D(world_corners[4], world_corners[5], world_corners[1], fill);
        DrawTriangle3D(world_corners[4], world_corners[1], world_corners[0], fill);
        if (g_gfx_debug_stats != nullptr) {
            g_gfx_debug_stats->add_draw(GfxDebugDrawCategory::CityCells, 12U, 36U, 12U);
        }

        // Wireframe edges
        DrawLine3D(world_corners[0], world_corners[1], wire);
        DrawLine3D(world_corners[1], world_corners[2], wire);
        DrawLine3D(world_corners[2], world_corners[3], wire);
        DrawLine3D(world_corners[3], world_corners[0], wire);

        DrawLine3D(world_corners[4], world_corners[5], wire);
        DrawLine3D(world_corners[5], world_corners[6], wire);
        DrawLine3D(world_corners[6], world_corners[7], wire);
        DrawLine3D(world_corners[7], world_corners[4], wire);

        DrawLine3D(world_corners[0], world_corners[4], wire);
        DrawLine3D(world_corners[1], world_corners[5], wire);
        DrawLine3D(world_corners[2], world_corners[6], wire);
        DrawLine3D(world_corners[3], world_corners[7], wire);
        if (g_gfx_debug_stats != nullptr) {
            g_gfx_debug_stats->add_draw(GfxDebugDrawCategory::WireOverlay, 0U, 24U, 12U);
        }
    } else {
        draw_gpu_building_model(model, origin, placement.rotation, false, false, GfxDebugDrawCategory::CityCells);
    }

    if (selected && use_lod) {
        // The far LOD has no mesh to shell; fall back to a glow box on the bounds.
        const auto sx = static_cast<float>(std::max(1U, template_file.size_x)) * unit_size;
        const auto sy = static_cast<float>(std::max(1U, template_file.size_z)) * unit_size;
        const auto sz = static_cast<float>(std::max(1U, template_file.size_y)) * unit_size;
        const Vector3 local_center{
            (static_cast<float>(template_file.size_x) * 0.5F - static_cast<float>(template_file.anchor_x)) * unit_size,
            (static_cast<float>(template_file.size_z) * 0.5F - static_cast<float>(template_file.anchor_y)) * unit_size,
            (static_cast<float>(template_file.size_y) * 0.5F - static_cast<float>(template_file.anchor_z)) * unit_size,
        };
        const auto center = city_transform(origin, local_center, placement.rotation);
        const auto rotated = (placement.rotation & 1) != 0;
        const auto box = Vector3{rotated ? sz : sx, sy, rotated ? sx : sz};
        draw_selection_glow_box(center, box, Color{255, 225, 90, 255});
    }
}

void draw_city_building_models(EditorProject& project, const SbcBuilding* selected_building, const Camera3D& camera) {
    if (!project.city) {
        return;
    }
    constexpr auto unit_size = 4.0F;
    for (const auto& placement : project.city->data.buildings) {
        const auto* asset = find_building_asset(project, placement.name);
        if (asset == nullptr) {
            continue;
        }
        const auto asset_index = static_cast<std::size_t>(asset - project.buildings.data());
        if (asset_index >= project.building_gpu_models.size()) {
            continue;
        }

        const auto render_origin = native_city_building_render_origin(placement, unit_size);
        const auto origin = Vector3{render_origin.x, render_origin.y, render_origin.z};
        const float dx = camera.position.x - origin.x;
        const float dy = camera.position.y - origin.y;
        const float dz = camera.position.z - origin.z;
        const float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
        const bool use_lod = (selected_building != &placement) && (dist > 150.0F);

        draw_city_building_model(
            asset->data,
            project.building_gpu_models[asset_index],
            placement,
            selected_building == &placement,
            use_lod,
            false);
    }
}

const std::vector<Vec3>& cached_shadow_volume_vertices(
    const GpuBuildingRenderModel& model,
    std::int32_t rotation,
    Vector3 light,
    Vector3 extrusion) {
    const Vec3 light_value{light.x, light.y, light.z};
    const auto light_delta = Vector3Subtract(
        Vector3{model.shadow_volume_light.x, model.shadow_volume_light.y, model.shadow_volume_light.z},
        light);
    if (!model.shadow_volume_light_valid || Vector3LengthSqr(light_delta) > 1e-10F ||
        Vector3LengthSqr(Vector3Subtract(extrusion, {model.shadow_volume_extrusion.x, model.shadow_volume_extrusion.y, model.shadow_volume_extrusion.z})) > 1e-10F) {
        model.shadow_volume_light = light_value;
        model.shadow_volume_extrusion = {extrusion.x, extrusion.y, extrusion.z};
        model.shadow_volume_light_valid = true;
        model.shadow_volume_valid.fill(false);
        model.shadow_volume_mesh_uploaded.fill(false);
        for (auto& vertices : model.shadow_volume_vertices) vertices.clear();
    }

    const auto rotation_index = static_cast<std::size_t>(rotation & 3);
    auto& vertices = model.shadow_volume_vertices[rotation_index];
    if (model.shadow_volume_valid[rotation_index]) return vertices;

    std::vector<std::uint8_t> faces_light(model.shadow_faces.size(), 0U);
    const auto append_triangle = [&vertices](Vector3 a, Vector3 b, Vector3 c) {
        vertices.push_back({a.x, a.y, a.z});
        vertices.push_back({b.x, b.y, b.z});
        vertices.push_back({c.x, c.y, c.z});
    };
    vertices.reserve(model.shadow_faces.size() * 6U);
    for (std::size_t face_index = 0; face_index < model.shadow_faces.size(); ++face_index) {
        const auto& face = model.shadow_faces[face_index];
        std::array<Vector3, 3> p{};
        std::array<Vector3, 3> q{};
        for (std::size_t index = 0; index < 3; ++index) {
            const auto& local = face.vertices[index];
            p[index] = city_transform(Vector3{}, {local.x, local.y, local.z}, rotation);
            q[index] = Vector3Add(p[index], extrusion);
        }
        const auto normal = Vector3CrossProduct(Vector3Subtract(p[1], p[0]), Vector3Subtract(p[2], p[0]));
        if (Vector3DotProduct(normal, light) < 0.0F) continue;
        faces_light[face_index] = 1U;
        append_triangle(p[0], p[1], p[2]);
        append_triangle(q[0], q[2], q[1]);
        for (std::size_t edge = 0; edge < 3; ++edge) {
            const auto next = (edge + 1U) % 3U;
            const auto adjacent = face.adjacent[edge];
            if (adjacent >= 0 && static_cast<std::size_t>(adjacent) < faces_light.size()) continue;
            append_triangle(p[next], p[edge], q[next]);
            append_triangle(q[next], p[edge], q[edge]);
        }
    }
    for (std::size_t face_index = 0; face_index < model.shadow_faces.size(); ++face_index) {
        if (!faces_light[face_index]) continue;
        const auto& face = model.shadow_faces[face_index];
        for (std::size_t edge = 0; edge < 3; ++edge) {
            const auto adjacent = face.adjacent[edge];
            if (adjacent < 0 || static_cast<std::size_t>(adjacent) >= faces_light.size() || faces_light[adjacent]) continue;
            const auto next = (edge + 1U) % 3U;
            const auto& local_from = face.vertices[edge];
            const auto& local_to = face.vertices[next];
            const auto from = city_transform(Vector3{}, {local_from.x, local_from.y, local_from.z}, rotation);
            const auto to = city_transform(Vector3{}, {local_to.x, local_to.y, local_to.z}, rotation);
            const auto far_from = Vector3Add(from, extrusion);
            const auto far_to = Vector3Add(to, extrusion);
            append_triangle(to, from, far_to);
            append_triangle(far_to, from, far_from);
        }
    }
    model.shadow_volume_valid[rotation_index] = true;
    return vertices;
}

// GPU-mesh counterpart of cached_shadow_volume_vertices(): same cache key
// (light + extrusion + rotation), but uploads the vertex list into a Mesh
// once instead of resubmitting it through rlVertex3f every frame.
const Mesh& cached_shadow_volume_mesh(
    const GpuBuildingRenderModel& model,
    std::int32_t rotation,
    Vector3 light,
    Vector3 extrusion) {
    const auto& vertices = cached_shadow_volume_vertices(model, rotation, light, extrusion);
    const auto rotation_index = static_cast<std::size_t>(rotation & 3);
    auto& mesh = model.shadow_volume_meshes[rotation_index];
    if (model.shadow_volume_mesh_uploaded[rotation_index]) {
        return mesh;
    }
    if (mesh.vaoId != 0U || mesh.vboId != nullptr) {
        UnloadMesh(mesh);
    }
    mesh = {};
    mesh.vertexCount = static_cast<int>(vertices.size());
    mesh.triangleCount = static_cast<int>(vertices.size() / 3U);
    if (!vertices.empty()) {
        mesh.vertices = static_cast<float*>(MemAlloc(static_cast<unsigned int>(vertices.size() * 3U * sizeof(float))));
        for (std::size_t i = 0; i < vertices.size(); ++i) {
            mesh.vertices[i * 3U + 0U] = vertices[i].x;
            mesh.vertices[i * 3U + 1U] = vertices[i].y;
            mesh.vertices[i * 3U + 2U] = vertices[i].z;
        }
        UploadMesh(&mesh, false);
    }
    model.shadow_volume_mesh_uploaded[rotation_index] = true;
    return mesh;
}

void draw_city_shadow_volumes(EditorProject& project, const Camera3D& camera, float max_distance) {
    if (!project.city) return;
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
    glDisable(GL_CULL_FACE);
    glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_DECR_WRAP, GL_KEEP);
    glStencilOpSeparate(GL_BACK, GL_KEEP, GL_INCR_WRAP, GL_KEEP);

    const auto light = Vector3Normalize(game_light_direction());

    // Color writes are masked off (glColorMask above) so the material's shader
    // only needs to exist and take a position; reuse the outline mask shader
    // rather than defining a dedicated one.
    Material mask_material = LoadMaterialDefault();
    mask_material.shader = mask_shader().shader;

    for (std::size_t placement_index = 0; placement_index < project.city->data.buildings.size(); ++placement_index) {
        const auto& placement = project.city->data.buildings[placement_index];
        if (!placement_asset_indices[placement_index].has_value()) continue;
        const auto asset_index = *placement_asset_indices[placement_index];
        if (asset_index >= project.building_gpu_models.size()) continue;
        const auto* asset = &project.buildings[asset_index];
        const auto& model = project.building_gpu_models[asset_index];
        const auto origin_value = native_city_building_render_origin(placement, 4.0F);
        const Vector3 origin{origin_value.x, origin_value.y, origin_value.z};
        const auto camera_delta = Vector3Subtract(origin, camera.position);
        if (max_distance > 0.0F && Vector3LengthSqr(camera_delta) > max_distance * max_distance) continue;

        const float length = static_cast<float>(asset->data.size_z + 1U) * 8.0F / std::max(light.y, 0.125F);
        const auto extrusion = Vector3Scale(light, -length);
        const auto& mesh = cached_shadow_volume_mesh(model, placement.rotation, light, extrusion);
        if (mesh.vertexCount > 0) {
            DrawMesh(mesh, mask_material, MatrixTranslate(origin.x, origin.y, origin.z));
        }
    }
    // Native queued district meshes use a 32-unit extrusion (0x2787d4).
    // Vertices are already world-space, so no translation is needed.
    const auto& pieces_mesh = cached_shadow_volume_mesh(project.portal_gpu_model, 0, light, Vector3Scale(light, -32.0F));
    if (pieces_mesh.vertexCount > 0) {
        DrawMesh(pieces_mesh, mask_material, MatrixIdentity());
    }
    rlDrawRenderBatchActive();
    MemFree(mask_material.maps);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
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

    // Native shaders consume a screen-space shadow factor texture. Preserve
    // the completed scene color in attachment 0 and write the stencil result
    // into a separate factor attachment instead of painting black over the
    // frame. The resolve shader samples this factor after all scene drawing.
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
    cache_shadow_faces(model, buffer);
    model.chunks.reserve(buffer.material_runs.size());
    for (const auto& run : buffer.material_runs) {
        const auto* texture = textured ? texture_for_buffer_material(project, buffer, run.material) : nullptr;
        auto chunk = upload_mesh_chunk(
            buffer,
            run.first_triangle,
            run.triangle_count,
            run.material,
            texture,
            material_texture_for_buffer_material(project, buffer, run.material));
        if (chunk.mesh.vertexCount > 0) {
            model.uploaded_vertices += static_cast<std::size_t>(chunk.mesh.vertexCount);
            model.uploaded_bytes += static_cast<std::size_t>(chunk.mesh.vertexCount) * 3U * sizeof(float);
            model.uploaded_bytes += static_cast<std::size_t>(chunk.mesh.vertexCount) * 2U * sizeof(float);
            if (chunk.mesh.colors != nullptr) {
                model.uploaded_bytes += static_cast<std::size_t>(chunk.mesh.vertexCount) * 4U * sizeof(unsigned char);
            }
            if (chunk.mesh.indices != nullptr) {
                model.uploaded_indices += static_cast<std::size_t>(chunk.mesh.triangleCount) * 3U;
                model.uploaded_bytes += static_cast<std::size_t>(chunk.mesh.triangleCount) * 3U * sizeof(unsigned short);
            }
            model.chunks.push_back(std::move(chunk));
        }
    }
    return model;
}

GpuBuildingRenderModel& portal_render_model(EditorProject& project, bool textured, std::optional<std::size_t> selected_portal) {
    MAPED_PROFILE_FUNCTION();
    // References into the project's cached render inputs (built once at load, not
    // per frame). Only the async-rebuild branch below copies them, and only when a
    // rebuild is actually queued — the steady-state path touches no allocations.
    // v10 district pieces index the game's absolute texture table (built-ins + CSX
    // in archive order), not the editor's load order — see project_grid_texture_names.
    const auto& blocks = block_meshes(project);
    const auto& bb_templates = buildblock_templates(project);
    const auto& tex_names = project_grid_texture_names(project);
    if (selected_portal.has_value()) {
        if (project.portal_preview_render_dirty ||
            project.portal_preview_render_textured != textured ||
            project.portal_preview_index != selected_portal) {
            MAPED_PROFILE_BLOCK("portal_render_model rebuild selected portal preview");
            project.portal_preview_render_buffer = build_native_portal_render_buffer(project.city->data, blocks, selected_portal, bb_templates, tex_names);
            project.portal_preview_gpu_model = upload_portal_render_model(project.portal_preview_render_buffer, project, textured);
            MAPED_PROFILE_END_BLOCK;
            project.portal_preview_index = selected_portal;
            project.portal_preview_render_textured = textured;
            project.portal_preview_render_dirty = false;
        }
        return project.portal_preview_gpu_model;
    }

    // Kick a worker to rebuild the (potentially large) all-portals buffer when the
    // pieces changed; the previous mesh keeps rendering until the new one lands, so
    // placing a piece never blocks the frame.
    // Rebuilding every piece is heavy, so wait for a paint stroke to finish
    // instead of rebuilding on each stamped cell.
    if ((project.portal_render_dirty || project.portal_render_textured != textured) &&
        !project.portal_rebuild_in_progress && !project.defer_heavy_rebuilds) {
        MAPED_PROFILE_BLOCK("portal_render_model queue async rebuild");
#ifdef __EMSCRIPTEN__
        // The browser build has no pthreads (std::async aborts there), so
        // rebuild inline.
        project.portal_render_dirty = false;
        project.portal_render_textured = textured;
        project.portal_render_buffer = build_native_portal_render_buffer(project.city->data, blocks, std::nullopt, bb_templates, tex_names);
        project.portal_gpu_model = upload_portal_render_model(project.portal_render_buffer, project, textured);
#else
        const auto map = project.city->data;
        const auto blocks_copy = blocks;
        const auto bb_copy = bb_templates;
        const auto tex_copy = tex_names;
        project.portal_rebuild_in_progress = true;
        project.portal_render_dirty = false;
        project.portal_render_textured = textured;
        project.portal_rebuild_future = std::async(std::launch::async, [map, blocks_copy, bb_copy, tex_copy]() {
            MAPED_PROFILE_THREAD("Portal render rebuild");
            MAPED_PROFILE_BLOCK("build async portal render buffer");
            return build_native_portal_render_buffer(map, blocks_copy, std::nullopt, bb_copy, tex_copy);
        });
#endif
        MAPED_PROFILE_END_BLOCK;
    }
    if (project.portal_rebuild_future.has_value() &&
        project.portal_rebuild_future->wait_for(std::chrono::milliseconds{0}) == std::future_status::ready) {
        MAPED_PROFILE_BLOCK("portal_render_model adopt async rebuild");
        project.portal_render_buffer = project.portal_rebuild_future->get();
        project.portal_rebuild_future.reset();
        project.portal_rebuild_in_progress = false;
        project.portal_gpu_model = upload_portal_render_model(project.portal_render_buffer, project, textured);
        MAPED_PROFILE_END_BLOCK;
    }
    return project.portal_gpu_model;
}

GpuBuildingRenderModel& building_tool_floor_preview_model(
    EditorProject& project,
    std::size_t building_index,
    std::optional<std::uint32_t> floor
) {
    if (project.building_floor_preview_dirty ||
        project.building_floor_preview_building != building_index ||
        project.building_floor_preview_floor != floor) {
        const auto& blocks = block_meshes(project);
        const auto& templates = buildblock_templates(project);
        project.building_floor_preview_render_buffer = build_native_building_tool_preview_render_buffer(
            project.buildings[building_index].data,
            blocks,
            templates,
            floor);
        project.building_floor_preview_gpu_model = upload_building_render_model(
            project.building_floor_preview_render_buffer,
            project.buildings[building_index].data,
            project);
        project.building_floor_preview_building = building_index;
        project.building_floor_preview_floor = floor;
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
    // Native world cells sit 0.25 above their level; without the lift the piece
    // ground cells are coplanar with their stamped terrain plots and z-fight.
    draw_gpu_building_model(model, {0.0F, 0.25F, 0.0F}, 0, false, draw_wires, GfxDebugDrawCategory::Pieces);

    // Per-voxel selection/hover glow is drawn by the caller (draw_city_piece_glow)
    // now that pieces are selected one block at a time, not by the whole portal.
}

// Glow box over a single piece voxel (one world grid cell), so pieces highlight
// individually instead of as the whole 8x8x8 portal they are stored in.
void draw_city_piece_glow(CityGridPoint cell, Color color) {
    constexpr auto unit_size = 4.0F;
    const Vector3 center{
        (static_cast<float>(cell.x) + 0.5F) * unit_size,
        (static_cast<float>(cell.y) + 0.5F) * unit_size,
        (static_cast<float>(cell.z) + 0.5F) * unit_size,
    };
    draw_selection_glow_box(center, {unit_size, unit_size, unit_size}, color);
}

Color city_tile_material_color(const BuildingRenderBuffer& buffer, std::uint32_t material) {
    if (material < buffer.material_names.size()) {
        const auto& name = buffer.material_names[material];
        if (name == "city_road_line_yellow") return Color{226, 190, 72, 255};
        if (name == "city_road_line_white") return Color{224, 229, 226, 255};
        if (name == "city_curb_concrete") {
            return {150, 154, 151, 255};
        }
        if (name == "city_road_asphalt") {
            return {43, 47, 50, 255};
        }
    }
    return {88, 96, 104, 255};
}

Color terrain_material_color(const BuildingRenderBuffer&, std::uint32_t) {
    return Color{0x40, 0x54, 0x26, 0xff};
}

// Draws pre-uploaded GPU chunks with an identity transform. One DrawMesh per
// material run instead of one immediate-mode call per triangle.
void draw_uploaded_city_model(GpuBuildingRenderModel& model, GfxDebugDrawCategory category, bool terrain = false) {
    const auto transform = MatrixIdentity();
    set_game_render_depth_output(true);
    rlDisableBackfaceCulling();
    set_game_surface_window(false);
    set_game_surface_building(false);
    set_game_surface_terrain(terrain);
    for (auto& chunk : model.chunks) {
        if (chunk.material.maps == nullptr || chunk.mesh.vertexCount <= 0) {
            continue;
        }
        DrawMesh(chunk.mesh, chunk.material, transform);
        if (g_gfx_debug_stats != nullptr) {
            g_gfx_debug_stats->add_draw(
                category,
                static_cast<std::uint64_t>(std::max(chunk.mesh.triangleCount, 0)),
                static_cast<std::uint64_t>(std::max(chunk.mesh.vertexCount, 0)),
                1U);
        }
    }
    rlEnableBackfaceCulling();
    set_game_surface_terrain(false);
    set_game_render_depth_output(false);
}

// Frees the GPU meshes/materials a city model owns before it is replaced, so the
// per-edit re-upload doesn't leak VRAM. (Buffers are MemAlloc'd by UploadMesh.)
void unload_city_render_model(GpuBuildingRenderModel& model) {
    // GpuBuildingMeshChunk owns its mesh/material via RAII: its destructor calls
    // UnloadMesh and MemFree(material.maps) (and intentionally leaves the shared
    // project texture alone). Clearing the vector runs those destructors. A manual
    // UnloadMesh here would leave each mesh's stale vbo/vertex pointers in place,
    // so the chunk destructor would then UnloadMesh the same buffers a second time
    // and abort with a double free.
    model.chunks.clear();
    model.wire_lines.clear();
    model.unload_shadow_volume_meshes();
    model.shadow_volume_valid.fill(false);
    model.shadow_volume_light_valid = false;
    model.uploaded_vertices = 0;
    model.uploaded_indices = 0;
    model.uploaded_bytes = 0;
}

GpuBuildingRenderModel upload_city_tile_model(const BuildingRenderBuffer& buffer, const EditorProject& project) {
    return upload_city_buffer_model(buffer, project, false, [&buffer](std::uint32_t material) {
        return city_tile_material_color(buffer, material);
    });
}

GpuBuildingRenderModel upload_city_terrain_model(const BuildingRenderBuffer& buffer, const EditorProject& project) {
    return upload_city_buffer_model(buffer, project, true, [&buffer](std::uint32_t material) {
        return terrain_material_color(buffer, material);
    });
}

void draw_city_road_markings(const MapScene& scene) {
    constexpr float line_width = 0.26F;
    const auto center_color = Color{226, 190, 72, 230};
    const auto edge_color = Color{224, 229, 226, 210};
    std::uint64_t triangles = 0U;

    rlDisableBackfaceCulling();
    for (const auto& road : scene.roads) {
        const auto dx = road.to.x - road.from.x;
        const auto dz = road.to.z - road.from.z;
        const auto length = std::sqrt(dx * dx + dz * dz);
        if (length <= scene.unit_size) {
            continue;
        }
        const Vector3 direction{dx / length, 0.0F, dz / length};
        const Vector3 perpendicular{-direction.z, 0.0F, direction.x};
        const auto trim = std::min(road.width * 0.5F, length * 0.22F);
        const auto start = trim;
        const auto end = length - trim;
        if (end <= start) {
            continue;
        }

        const auto point = [&](float distance, float offset) {
            const auto t = distance / length;
            return Vector3{
                road.from.x + direction.x * distance + perpendicular.x * offset,
                road.from.y + (road.to.y - road.from.y) * t + 0.10F,
                road.from.z + direction.z * distance + perpendicular.z * offset,
            };
        };
        const auto stripe = [&](float from, float to, float offset, float width, Color color) {
            const auto a = point(from, offset - width * 0.5F);
            const auto b = point(from, offset + width * 0.5F);
            const auto c = point(to, offset + width * 0.5F);
            const auto d = point(to, offset - width * 0.5F);
            DrawTriangle3D(a, b, c, color);
            DrawTriangle3D(a, c, d, color);
            triangles += 2U;
        };

        const auto edge_offset = std::max(scene.unit_size * 0.75F, road.width * 0.5F - scene.unit_size * 0.22F);
        stripe(start, end, -edge_offset, line_width, edge_color);
        stripe(start, end, edge_offset, line_width, edge_color);

        const auto dash = scene.unit_size * 1.45F;
        const auto gap = scene.unit_size * 0.65F;
        for (auto distance = start; distance < end; distance += dash + gap) {
            stripe(distance, std::min(distance + dash, end), 0.0F, line_width * 1.35F, center_color);
        }
    }
    rlEnableBackfaceCulling();

    if (g_gfx_debug_stats != nullptr && triangles > 0U) {
        g_gfx_debug_stats->add_draw(
            GfxDebugDrawCategory::RoadsCurbs,
            triangles,
            triangles * 3U,
            triangles / 2U);
    }
}

void draw_city_scene(
    EditorProject& project,
    const MapScene& scene,
    const SbcStreet* selected_street,
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
        draw_uploaded_city_model(project.terrain_gpu_model, GfxDebugDrawCategory::Terrain, true);
    }

    {
        GfxDebugMaybeTimer timer{GfxDebugTimingPhase::CityRoadDraw};
        if (render_roads) {
            draw_uploaded_city_model(project.city_tile_gpu_model, GfxDebugDrawCategory::RoadsCurbs);
        }
    }

    for (const auto& road : scene.roads) {
        const auto selected = selected_street != nullptr && road.source == selected_street;
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
        position.y = static_cast<float>(intersection.source->y) * scene.unit_size;
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

// Builds the traffic-light pole as ONE reusable model (post + arm + signal-head
// boxes, the game's 3-box shape), oriented along -Z so a Y-rotation can aim the
// arm. Built once and instanced per pole via DrawModelEx, instead of re-emitting
// cylinders/cubes per pole every frame.
Model build_traffic_light_pole_model(float unit_size) {
    (void)unit_size;
    // Native street-object type 1 geometry (World_InitResourcePoolsAndCatalogs
    // @13dcd0, BuildBlock_GenerateCubeVerticesFaces boxes, world units, centered
    // boxes), expressed relative to the pole's ground point: a 6-tall post, a
    // 10-long arm across the street at the post top, and two signal heads at 6
    // and 10 units out — one over each lane. The arm extends along local -Z.
    struct BoxSpec {
        Vector3 center;
        Vector3 size;
        bool lens;
    };
    const std::array<BoxSpec, 6> boxes{{
        {{0.0F, 3.0F, 0.0F}, {0.375F, 6.0F, 0.375F}, false},   // post
        {{0.0F, 6.0F, -5.0F}, {0.25F, 0.25F, 10.0F}, false},   // arm
        {{0.0F, 6.0F, -6.0F}, {0.3125F, 1.0F, 0.5F}, false},   // head over lane 1
        {{0.0F, 6.0F, -10.0F}, {0.3125F, 1.0F, 0.5F}, false},  // head over lane 2
        // Signal lenses (editor stand-in for the game's red default state
        // texture): a thin band through each head, proud on the +/-x faces the
        // signal textures occupy.
        {{0.0F, 6.3F, -6.0F}, {0.36F, 0.26F, 0.3F}, true},
        {{0.0F, 6.3F, -10.0F}, {0.36F, 0.26F, 0.3F}, true},
    }};

    Model model{};
    model.transform = MatrixIdentity();
    model.meshCount = static_cast<int>(boxes.size());
    model.materialCount = 2;
    model.meshes = static_cast<Mesh*>(MemAlloc(static_cast<unsigned int>(boxes.size() * sizeof(Mesh))));
    model.meshMaterial = reinterpret_cast<int*>(MemAlloc(static_cast<unsigned int>(boxes.size() * sizeof(int))));
    model.materials = static_cast<Material*>(MemAlloc(2 * sizeof(Material)));
    model.materials[0] = LoadMaterialDefault();
    model.materials[0].shader = game_surface_shader();
    model.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = Color{40, 43, 45, 255};
    model.materials[0].maps[MATERIAL_MAP_METALNESS].texture = default_game_material_texture();
    model.materials[1] = LoadMaterialDefault();
    model.materials[1].shader = game_surface_shader();
    model.materials[1].maps[MATERIAL_MAP_DIFFUSE].color = Color{205, 48, 38, 255};
    model.materials[1].maps[MATERIAL_MAP_METALNESS].texture = default_game_material_texture();

    for (std::size_t index = 0; index < boxes.size(); ++index) {
        const auto& box = boxes[index];
        Mesh mesh = GenMeshCube(box.size.x, box.size.y, box.size.z);
        UploadMesh(&mesh, false);
        for (int i = 0; i < mesh.vertexCount; ++i) {
            mesh.vertices[i * 3 + 0] += box.center.x;
            mesh.vertices[i * 3 + 1] += box.center.y;
            mesh.vertices[i * 3 + 2] += box.center.z;
        }
        UpdateMeshBuffer(mesh, 0, mesh.vertices, mesh.vertexCount * 3 * static_cast<int>(sizeof(float)), 0);
        model.meshes[index] = mesh;
        model.meshMaterial[index] = box.lens ? 1 : 0;
    }
    return model;
}

// Bakes every pole's boxes of one material into world-space meshes, split so
// each stays under raylib's 16-bit index limit.
std::vector<Mesh> merge_traffic_light_poles(const Model& pole_model, const std::vector<CityTrafficLightPole>& poles, int material) {
    constexpr int kMaxVertices = 60000;
    std::vector<Mesh> merged;
    std::vector<float> positions, normals, texcoords;
    std::vector<unsigned short> indices;
    const auto flush = [&] {
        if (positions.empty()) return;
        Mesh mesh{};
        mesh.vertexCount = static_cast<int>(positions.size() / 3);
        mesh.triangleCount = static_cast<int>(indices.size() / 3);
        mesh.vertices = static_cast<float*>(MemAlloc(static_cast<unsigned int>(positions.size() * sizeof(float))));
        mesh.normals = static_cast<float*>(MemAlloc(static_cast<unsigned int>(normals.size() * sizeof(float))));
        mesh.texcoords = static_cast<float*>(MemAlloc(static_cast<unsigned int>(texcoords.size() * sizeof(float))));
        mesh.indices = static_cast<unsigned short*>(MemAlloc(static_cast<unsigned int>(indices.size() * sizeof(unsigned short))));
        std::copy(positions.begin(), positions.end(), mesh.vertices);
        std::copy(normals.begin(), normals.end(), mesh.normals);
        std::copy(texcoords.begin(), texcoords.end(), mesh.texcoords);
        std::copy(indices.begin(), indices.end(), mesh.indices);
        UploadMesh(&mesh, false);
        merged.push_back(mesh);
        positions.clear(); normals.clear(); texcoords.clear(); indices.clear();
    };
    for (const auto& pole : poles) {
        // Same transform DrawModelEx applied per pole: the arm points along -Z,
        // and raylib's +Y yaw is right-handed, so X is negated to aim it along
        // arm_direction (the same handedness flip as the building rotations).
        const auto yaw = std::atan2(-pole.arm_direction.x, -pole.arm_direction.z);
        const auto rotation = MatrixRotateY(yaw);
        const auto transform = MatrixMultiply(rotation, MatrixTranslate(pole.base.x, pole.base.y, pole.base.z));
        for (int m = 0; m < pole_model.meshCount; ++m) {
            if (pole_model.meshMaterial[m] != material) continue;
            const auto& box = pole_model.meshes[m];
            if (static_cast<int>(positions.size() / 3) + box.vertexCount > kMaxVertices) flush();
            const auto base_index = static_cast<unsigned short>(positions.size() / 3);
            for (int v = 0; v < box.vertexCount; ++v) {
                const auto p = Vector3Transform({box.vertices[v * 3], box.vertices[v * 3 + 1], box.vertices[v * 3 + 2]}, transform);
                const auto n = Vector3Transform({box.normals[v * 3], box.normals[v * 3 + 1], box.normals[v * 3 + 2]}, rotation);
                positions.insert(positions.end(), {p.x, p.y, p.z});
                normals.insert(normals.end(), {n.x, n.y, n.z});
                texcoords.insert(texcoords.end(), {box.texcoords[v * 2], box.texcoords[v * 2 + 1]});
            }
            for (int i = 0; i < box.triangleCount * 3; ++i) {
                indices.push_back(static_cast<unsigned short>(base_index + box.indices[i]));
            }
        }
    }
    flush();
    return merged;
}

void draw_city_traffic_light_poles(const MapScene& scene) {
    static float cached_unit = -1.0F;
    static Model pole_model{};
    if (cached_unit != scene.unit_size) {
        if (cached_unit > 0.0F) {
            UnloadModel(pole_model);
        }
        pole_model = build_traffic_light_pole_model(scene.unit_size);
        cached_unit = scene.unit_size;
    }

    // Every pole shares one model, so bake them all into a few meshes (one
    // draw per chunk and material) instead of a draw per box per pole. Rebake
    // when the pole set changes.
    static std::array<std::vector<Mesh>, 2> merged;
    static std::size_t merged_key = 0;
    std::size_t key = scene.traffic_light_poles.size();
    for (const auto& pole : scene.traffic_light_poles) {
        key = key * 31U + std::hash<float>{}(pole.base.x) + std::hash<float>{}(pole.base.y) * 7U +
            std::hash<float>{}(pole.base.z) * 13U + std::hash<float>{}(pole.arm_direction.x) * 17U +
            std::hash<float>{}(pole.arm_direction.z) * 19U;
    }
    if (key != merged_key) {
        for (auto& chunks : merged) {
            for (auto& mesh : chunks) UnloadMesh(mesh);
            chunks.clear();
        }
        for (int material = 0; material < 2; ++material) {
            merged[static_cast<std::size_t>(material)] = merge_traffic_light_poles(pole_model, scene.traffic_light_poles, material);
        }
        merged_key = key;
    }

    set_game_surface_window(false);
    set_game_surface_building(false);
    set_game_render_depth_output(true);
    for (int material = 0; material < 2; ++material) {
        for (const auto& mesh : merged[static_cast<std::size_t>(material)]) {
            DrawMesh(mesh, pole_model.materials[material], MatrixIdentity());
            if (g_gfx_debug_stats != nullptr) {
                g_gfx_debug_stats->add_draw(
                    GfxDebugDrawCategory::TrafficLightPoles,
                    static_cast<std::uint64_t>(mesh.triangleCount),
                    static_cast<std::uint64_t>(mesh.vertexCount),
                    1U);
            }
        }
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

void draw_city_edit_grid(CityGridPoint center, const MapScene& scene) {
    const auto unit = scene.unit_size > 0.0F ? scene.unit_size : 4.0F;
    constexpr std::int32_t half_cells = 20;
    // Flat plane fixed at the hovered cell's height, not terrain-following: a
    // single working plane is easier to read and to place against. Depth test is
    // disabled below so it overlays the scene ("goes through everything") instead
    // of being clipped by terrain/buildings.
    const float plane_y = static_cast<float>(center.y) * unit + unit * 0.02F;
    const float min_x = static_cast<float>(center.x - half_cells) * unit;
    const float max_x = static_cast<float>(center.x + half_cells) * unit;
    const float min_z = static_cast<float>(center.z - half_cells) * unit;
    const float max_z = static_cast<float>(center.z + half_cells) * unit;

    // Modelled on raylib's DrawGrid: one rlBegin(RL_LINES) batch with raw
    // rlVertex3f/rlColor4ub instead of N DrawLine3D calls. Major lines get
    // world-space width below; GL line width is unreliable on core profiles.
    const auto is_major = [](std::int32_t coord) { return coord % 10 == 0; };
    const auto set_color = [](bool axis, bool major) {
        if (axis) {
            rlColor4ub(210, 225, 245, 220);
        } else if (major) {
            rlColor4ub(170, 195, 220, 180);
        } else {
            rlColor4ub(120, 140, 160, 90);
        }
    };

    // Overlay: draw on top of everything, depth-write off so it never occludes.
    rlDrawRenderBatchActive();
    rlDisableDepthTest();
    rlDisableDepthMask();
    rlBegin(RL_LINES);
    for (std::int32_t gx = center.x - half_cells; gx <= center.x + half_cells; ++gx) {
        const bool axis = gx == center.x;
        const bool major = axis || is_major(gx);
        set_color(axis, major);
        const float wx = static_cast<float>(gx) * unit;
        rlVertex3f(wx, plane_y, min_z);
        rlVertex3f(wx, plane_y, max_z);
    }
    for (std::int32_t gz = center.z - half_cells; gz <= center.z + half_cells; ++gz) {
        const bool axis = gz == center.z;
        const bool major = axis || is_major(gz);
        set_color(axis, major);
        const float wz = static_cast<float>(gz) * unit;
        rlVertex3f(min_x, plane_y, wz);
        rlVertex3f(max_x, plane_y, wz);
    }
    rlEnd();
    rlDrawRenderBatchActive();
    for (std::int32_t gx = center.x - half_cells; gx <= center.x + half_cells; ++gx) {
        if (!is_major(gx) && gx != center.x) continue;
        const float x = static_cast<float>(gx) * unit;
        DrawCylinderEx({x, plane_y, min_z}, {x, plane_y, max_z}, unit * 0.025F,
            unit * 0.025F, 4, Color{170, 195, 220, 180});
    }
    for (std::int32_t gz = center.z - half_cells; gz <= center.z + half_cells; ++gz) {
        if (!is_major(gz) && gz != center.z) continue;
        const float z = static_cast<float>(gz) * unit;
        DrawCylinderEx({min_x, plane_y, z}, {max_x, plane_y, z}, unit * 0.025F,
            unit * 0.025F, 4, Color{170, 195, 220, 180});
    }
    rlEnableDepthMask();
    rlEnableDepthTest();
}

void draw_city_road_selection(const EditorProject& project, const EditorUi& ui, const MapScene& scene) {
    if (!project.city) {
        return;
    }
    const auto& map = project.city->data;
    const auto unit = scene.unit_size > 0.0F ? scene.unit_size : 4.0F;
    const auto lift = unit * 0.6F;
    const auto node_world = [&](std::size_t node_index) {
        const auto& n = map.intersections[node_index];
        const auto y = scene.terrain.has_value()
            ? terrain_height_at(*scene.terrain, static_cast<float>(n.x) * unit, static_cast<float>(n.z) * unit)
            : static_cast<float>(n.y) * unit;
        return Vector3{static_cast<float>(n.x) * unit, y + lift, static_cast<float>(n.z) * unit};
    };
    // Same pulsing glow box buildings use, so node and road selection read the
    // same way. A node is one box; a road is a chain of boxes stepped along the
    // segment (covering its full length and drawn width) so the whole road glows.
    const auto glow_node = [&](std::size_t node_index, Color color) {
        if (node_index >= map.intersections.size()) {
            return;
        }
        const auto pos = node_world(node_index);
        draw_selection_glow_box(pos, {unit, unit, unit}, color);
    };
    const auto glow_road = [&](std::size_t street_index, Color color) {
        if (street_index >= map.streets.size()) {
            return;
        }
        const auto& s = map.streets[street_index];
        if (s.from_intersection < 0 || s.to_intersection < 0 ||
            static_cast<std::size_t>(s.from_intersection) >= map.intersections.size() ||
            static_cast<std::size_t>(s.to_intersection) >= map.intersections.size()) {
            return;
        }
        const auto a = node_world(static_cast<std::size_t>(s.from_intersection));
        const auto b = node_world(static_cast<std::size_t>(s.to_intersection));
        // A translucent glowing strip over the road footprint plus a pulsing
        // border - one clean highlighted ribbon along the road, the road analogue
        // of the building's glow box (not a noisy stack of cubes).
        const auto half_w = std::max(static_cast<float>(std::max(s.left_width, s.right_width)), 1.0F) * unit * 0.5F;
        const auto len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z));
        if (len < 0.001F) {
            return;
        }
        // Unit perpendicular in the ground plane.
        const auto px = -(b.z - a.z) / len * half_w;
        const auto pz = (b.x - a.x) / len * half_w;
        const Vector3 a0{a.x + px, a.y, a.z + pz};
        const Vector3 a1{a.x - px, a.y, a.z - pz};
        const Vector3 b0{b.x + px, b.y, b.z + pz};
        const Vector3 b1{b.x - px, b.y, b.z - pz};
        const auto pulse = 0.5F + 0.5F * static_cast<float>(std::sin(GetTime() * 4.0));
        const Color fill{color.r, color.g, color.b, static_cast<unsigned char>(70 + 60 * pulse)};
        rlDisableBackfaceCulling();
        DrawTriangle3D(a0, a1, b1, fill);
        DrawTriangle3D(a0, b1, b0, fill);
        rlEnableBackfaceCulling();
        const Color edge{color.r, color.g, color.b, 255};
        DrawLine3D(a0, b0, edge);
        DrawLine3D(a1, b1, edge);
    };

    // Roads get the silhouette outline (composited after the 3D pass, like
    // buildings), so here we only glow the nodes - the outline shader cannot
    // trace the thin node markers as cleanly as a glow box does.
    (void)glow_road;
    if (!map.intersections.empty()) {
        glow_node(std::min(ui.selected_city_intersection, map.intersections.size() - 1U), Color{120, 210, 255, 255});
    }
    if (ui.hover_city_intersection.has_value()) {
        glow_node(*ui.hover_city_intersection, Color{200, 240, 255, 255});
    }
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

void draw_gpu_building_model_public(GpuBuildingRenderModel& model, Vector3 origin, std::int32_t rotation) {
    draw_gpu_building_model(model, origin, rotation, false, false, GfxDebugDrawCategory::CityCells);
}

} // namespace map_editor::app_internal
