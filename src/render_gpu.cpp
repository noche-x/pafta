#include "app_internal.hpp"

#include <rlgl.h>
#include <raymath.h>

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
#include <unordered_map>
#include <vector>

namespace map_editor::app_internal {

GfxDebugStats* g_gfx_debug_stats = nullptr;

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

Texture2D load_city_window_gradient_texture() {
    constexpr auto width = 32;
    constexpr auto height = 96;
    constexpr auto mullion_step_x = 8;
    constexpr auto mullion_step_y = 16;
    const auto bottom = Color{21, 39, 63, 255};
    const auto top = Color{92, 205, 226, 255};
    Image image = GenImageColor(width, height, bottom);
    for (auto y = 0; y < height; ++y) {
        const auto t = 1.0F - static_cast<float>(y) / static_cast<float>(height - 1);
        for (auto x = 0; x < width; ++x) {
            auto color = lerp_color(bottom, top, t);
            if ((x % mullion_step_x) == 0 || (y % mullion_step_y) == 0) {
                color = lerp_color(color, Color{8, 18, 30, 255}, 0.45F);
            }
            if (((x + y) % 19) == 0) {
                color = lerp_color(color, Color{180, 238, 246, 255}, 0.18F);
            }
            ImageDrawPixel(&image, x, y, color);
        }
    }

    auto texture = LoadTextureFromImage(image);
    if (texture.id != 0U) {
        SetTextureFilter(texture, TEXTURE_FILTER_BILINEAR);
        SetTextureWrap(texture, TEXTURE_WRAP_REPEAT);
    }
    UnloadImage(image);
    return texture;
}

void append_render_log(EditorProject& project, std::string message) {
    std::cerr << "[render] " << message << '\n';
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
    const auto buildings = project.buildings;
    const auto blocks = block_meshes(project);
    const auto templates = buildblock_templates(project);
    const auto generation = ++project.render_build_generation;
    auto progress = std::make_shared<RenderBuildProgress>();
    progress->generation = generation;
    progress->total = buildings.size();
    project.render_build_progress = progress;
    project.render_buffers_dirty = false;
    project.render_build_in_progress = true;
    project.building_render_buffers.clear();
    project.building_render_buffers.resize(buildings.size());
    project.building_gpu_models.clear();
    project.building_gpu_models.resize(buildings.size());
    project.building_floor_preview_dirty = true;
    project.pending_gpu_upload_indices.clear();
    project.pending_gpu_upload_index = 0;
    append_render_log(project, "render build queued: " + std::to_string(buildings.size()) + " buildings");

    project.render_build_future = std::async(std::launch::async, [buildings, blocks, templates, generation, progress]() {
        using Clock = std::chrono::steady_clock;
        RenderBuildResult result;
        result.generation = generation;
        const auto total_begin = Clock::now();
        for (std::size_t building_index = 0; building_index < buildings.size(); ++building_index) {
            const auto& building = buildings[building_index];
            {
                std::scoped_lock lock(progress->mutex);
                progress->current = building.name;
            }
            const auto begin = Clock::now();
            auto city_buffer = build_native_building_render_buffer(building.data, blocks, templates);
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

void service_render_rebuild(EditorProject& project) {
    if (project.render_buffers_dirty && !project.render_build_in_progress) {
        start_async_render_buffer_rebuild(project);
    }

    drain_render_build_progress(project);

    if (project.render_build_future.has_value()) {
        const auto ready = project.render_build_future->wait_for(std::chrono::milliseconds{0});
        if (ready == std::future_status::ready) {
            auto result = project.render_build_future->get();
            project.render_build_future.reset();
            accept_render_build_result(project, std::move(result));
        }
    }

    if (!project.render_build_in_progress) {
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
    if (material == 0x100U) {
        return {0, 0, 0, 191};
    }
    if (window_material) {
        return {92, 205, 226, 185};
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

const Texture2D* texture_for_material(const EditorProject& project, const SbbBuilding& building, std::uint32_t material_id) {
    if (material_id == 0x100U) {
        return nullptr;
    }
    const auto local_slot = static_cast<std::size_t>(material_id & 0xffU);
    if (local_slot < building.materials.size()) {
        return find_project_texture(project, building.materials[local_slot]);
    }
    return project.textures.empty() ? nullptr : &project.textures.front().texture;
}

const Texture2D* texture_for_buffer_material(const EditorProject& project, const BuildingRenderBuffer& buffer, std::uint32_t material_id) {
    if (material_id >= buffer.material_names.size()) {
        return nullptr;
    }
    return find_project_texture(project, buffer.material_names[material_id]);
}

GpuBuildingMeshChunk upload_mesh_chunk(
    const BuildingRenderBuffer& buffer,
    std::uint32_t first_triangle,
    std::uint32_t triangle_count,
    std::uint32_t material_id,
    const Texture2D* texture,
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
        if (fallback.mesh.vertices == nullptr || fallback.mesh.texcoords == nullptr) {
            if (fallback.mesh.vertices != nullptr) {
                MemFree(fallback.mesh.vertices);
            }
            if (fallback.mesh.texcoords != nullptr) {
                MemFree(fallback.mesh.texcoords);
            }
            fallback.mesh.vertices = nullptr;
            fallback.mesh.texcoords = nullptr;
            fallback.mesh.vertexCount = 0;
            fallback.mesh.triangleCount = 0;
            return fallback;
        }

        auto* out = fallback.mesh.vertices;
        auto* out_uv = fallback.mesh.texcoords;
        for (auto triangle_index = first_triangle; triangle_index < clamped_end; ++triangle_index) {
            const auto& triangle = buffer.triangles[triangle_index];
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
            }
        }

        UploadMesh(&fallback.mesh, false);
        fallback.material = LoadMaterialDefault();
        fallback.material.maps[MATERIAL_MAP_DIFFUSE].color = native_material_color(material_id, false, window_material);
        if (texture != nullptr && texture->id != 0U) {
            fallback.material.maps[MATERIAL_MAP_DIFFUSE].texture = *texture;
            fallback.source_texture = *texture;
            fallback.textured = true;
        }
        return fallback;
    };

    std::vector<float> vertices;
    std::vector<float> texcoords;
    std::vector<unsigned short> indices;
    std::unordered_map<std::uint32_t, unsigned short> local_indices;
    vertices.reserve(static_cast<std::size_t>(clamped_count) * 9U);
    texcoords.reserve(static_cast<std::size_t>(clamped_count) * 6U);
    indices.reserve(static_cast<std::size_t>(clamped_count) * 3U);
    local_indices.reserve(static_cast<std::size_t>(clamped_count) * 3U);

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
    chunk.mesh.indices = static_cast<unsigned short*>(MemAlloc(static_cast<unsigned int>(indices.size() * sizeof(unsigned short))));
    if (chunk.mesh.vertices == nullptr || chunk.mesh.texcoords == nullptr || chunk.mesh.indices == nullptr) {
        if (chunk.mesh.vertices != nullptr) {
            MemFree(chunk.mesh.vertices);
        }
        if (chunk.mesh.texcoords != nullptr) {
            MemFree(chunk.mesh.texcoords);
        }
        if (chunk.mesh.indices != nullptr) {
            MemFree(chunk.mesh.indices);
        }
        chunk.mesh.vertices = nullptr;
        chunk.mesh.texcoords = nullptr;
        chunk.mesh.indices = nullptr;
        chunk.mesh.vertexCount = 0;
        chunk.mesh.triangleCount = 0;
        return chunk;
    }

    std::ranges::copy(vertices, chunk.mesh.vertices);
    std::ranges::copy(texcoords, chunk.mesh.texcoords);
    std::ranges::copy(indices, chunk.mesh.indices);

    UploadMesh(&chunk.mesh, false);
    chunk.material = LoadMaterialDefault();
    chunk.material.maps[MATERIAL_MAP_DIFFUSE].color = native_material_color(material_id, false, window_material);
    if (texture != nullptr && texture->id != 0U) {
        chunk.material.maps[MATERIAL_MAP_DIFFUSE].texture = *texture;
        chunk.source_texture = *texture;
        chunk.textured = true;
    }
    return chunk;
}

// Bakes a simple directional + ambient term for a world-space normal into a
// brightness multiplier. One fixed sun direction; faces toward it are brighter,
// faces away sit at the ambient floor. Cheap, asset-free shading that makes
// terrain slopes, road grades, and curbs read as 3D without a custom GLSL shader.
float baked_directional_light(Vector3 normal) {
    constexpr Vector3 sun{0.40F, 0.82F, 0.41F};  // normalized-ish down-ish from one side
    constexpr float ambient = 0.55F;
    const auto len = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (len <= 0.0001F) {
        return 1.0F;
    }
    const auto ndotl = (normal.x * sun.x + normal.y * sun.y + normal.z * sun.z) / len;
    return ambient + (1.0F - ambient) * std::max(0.0F, ndotl);
}

Color shade_color(Color base, float light) {
    const auto scale = [light](unsigned char c) {
        return static_cast<unsigned char>(std::min(255.0F, static_cast<float>(c) * light));
    };
    return {scale(base.r), scale(base.g), scale(base.b), base.a};
}

// Uploads a whole city render buffer (roads/curbs or terrain) as GPU mesh chunks,
// one per material run, with per-face directional lighting baked into vertex
// colors. Replaces the per-triangle immediate-mode DrawTriangle3D path that was
// submitting hundreds of thousands of tiny draw calls per frame.
GpuBuildingRenderModel upload_city_buffer_model(
    const BuildingRenderBuffer& buffer,
    const EditorProject& project,
    const std::function<Color(std::uint32_t)>& color_for_material) {
    GpuBuildingRenderModel model;
    model.wire_lines = buffer.wire_lines;
    model.chunks.reserve(buffer.material_runs.size());

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
        if (chunk.mesh.vertices == nullptr || chunk.mesh.texcoords == nullptr || chunk.mesh.colors == nullptr) {
            if (chunk.mesh.vertices != nullptr) { MemFree(chunk.mesh.vertices); }
            if (chunk.mesh.texcoords != nullptr) { MemFree(chunk.mesh.texcoords); }
            if (chunk.mesh.colors != nullptr) { MemFree(chunk.mesh.colors); }
            continue;
        }

        const auto base_color = color_for_material(run.material);
        auto* out = chunk.mesh.vertices;
        auto* out_uv = chunk.mesh.texcoords;
        auto* out_col = chunk.mesh.colors;
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
            const auto light = baked_directional_light(face_normal);
            const auto shaded = shade_color(base_color, light);
            for (int i = 0; i < 3; ++i) {
                *out++ = p[i].x;
                *out++ = p[i].y;
                *out++ = p[i].z;
                *out_uv++ = v[i].u;
                *out_uv++ = v[i].v;
                *out_col++ = shaded.r;
                *out_col++ = shaded.g;
                *out_col++ = shaded.b;
                *out_col++ = shaded.a;
            }
        }

        UploadMesh(&chunk.mesh, false);
        chunk.material = LoadMaterialDefault();
        chunk.material.maps[MATERIAL_MAP_DIFFUSE].color = WHITE;  // baked into vertex colors
        if (const auto* texture = texture_for_buffer_material(project, buffer, run.material);
            texture != nullptr && texture->id != 0U) {
            chunk.material.maps[MATERIAL_MAP_DIFFUSE].texture = *texture;
            chunk.source_texture = *texture;
            chunk.textured = true;
        }
        model.uploaded_vertices += static_cast<std::size_t>(chunk.mesh.vertexCount);
        model.uploaded_bytes += static_cast<std::size_t>(chunk.mesh.vertexCount) * (3U * sizeof(float) + 2U * sizeof(float) + 4U);
        model.chunks.push_back(std::move(chunk));
    }
    return model;
}

GpuBuildingRenderModel upload_building_render_model(const BuildingRenderBuffer& buffer, const SbbBuilding& building, const EditorProject& project) {
    GpuBuildingRenderModel model;
    model.wire_lines = buffer.wire_lines;
    model.chunks.reserve(buffer.material_runs.size());
    for (const auto& run : buffer.material_runs) {
        auto chunk = upload_mesh_chunk(
            buffer,
            run.first_triangle,
            run.triangle_count,
            run.material,
            texture_for_material(project, building, run.material),
            run.city_window_surface);
        if (chunk.mesh.vertexCount > 0) {
            model.uploaded_vertices += static_cast<std::size_t>(chunk.mesh.vertexCount);
            model.uploaded_bytes += static_cast<std::size_t>(chunk.mesh.vertexCount) * 3U * sizeof(float);
            model.uploaded_bytes += static_cast<std::size_t>(chunk.mesh.vertexCount) * 2U * sizeof(float);
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

void draw_gpu_building_model(
    GpuBuildingRenderModel& model,
    Vector3 origin,
    std::int32_t rotation,
    bool selected,
    bool draw_wires,
    const Texture2D* city_window_texture = nullptr,
    GfxDebugDrawCategory category = GfxDebugDrawCategory::CityCells
) {
    const auto transform = building_transform_matrix(origin, rotation);
    rlDisableBackfaceCulling();
    for (auto& chunk : model.chunks) {
        if (chunk.material.maps == nullptr || chunk.mesh.vertexCount <= 0) {
            continue;
        }
        const auto use_window_texture = city_window_texture != nullptr &&
            city_window_texture->id != 0U &&
            chunk.window_material;
        chunk.material.maps[MATERIAL_MAP_DIFFUSE].texture = use_window_texture
            ? *city_window_texture
            : chunk.source_texture;
        chunk.material.maps[MATERIAL_MAP_DIFFUSE].color = (chunk.textured || use_window_texture)
            ? (selected ? Color{255, 236, 170, 255} : WHITE)
            : native_material_color(chunk.material_id, selected, false);
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

void draw_sbb_scene_for_app(const SbbBuilding& building, GpuBuildingRenderModel& model, const SbbCell* selected) {
    constexpr auto unit_size = 4.0F;
    DrawGrid(96, unit_size);
    draw_gpu_building_model(model, Vector3{0.0F, 0.0F, 0.0F}, 0, false, true, nullptr, GfxDebugDrawCategory::CityCells);

    if (selected != nullptr) {
        const auto center = Vector3{
            (static_cast<float>(selected->x) - static_cast<float>(building.anchor_x) + 0.5F) * unit_size,
            (static_cast<float>(selected->y) - static_cast<float>(building.anchor_y) + 0.5F) * unit_size,
            (static_cast<float>(selected->z) - static_cast<float>(building.anchor_z) + 0.5F) * unit_size,
        };
        DrawCubeWiresV(center, {unit_size * 1.16F, unit_size * 1.16F, unit_size * 1.16F}, {255, 225, 80, 255});
    }
}

const LoadedAsset<SbbBuilding>* find_building_asset(const EditorProject& project, std::string_view name) {
    const auto match = std::ranges::find_if(project.buildings, [name](const LoadedAsset<SbbBuilding>& asset) {
        return asset.name == name || asset.data.name == name;
    });
    return match == project.buildings.end() ? nullptr : &*match;
}

void draw_city_building_model(
    const SbbBuilding& template_file,
    GpuBuildingRenderModel& model,
    const SbcBuilding& placement,
    bool selected,
    bool use_lod,
    const Texture2D* city_window_texture) {
    constexpr auto unit_size = 4.0F;
    const auto render_origin = native_city_building_render_origin(placement, unit_size);
    const auto origin = Vector3{render_origin.x, render_origin.y, render_origin.z};
    const auto fill = selected ? Color{235, 194, 82, 210} : Color{126, 151, 174, 170};
    const auto wire = selected ? Color{255, 240, 150, 255} : Color{30, 36, 42, 170};

    if (use_lod) {
        const float min_x = -static_cast<float>(template_file.anchor_x) * unit_size;
        const float max_x = (static_cast<float>(template_file.size_x) - static_cast<float>(template_file.anchor_x)) * unit_size;
        const float min_y = -static_cast<float>(template_file.anchor_y) * unit_size;
        const float max_y = (static_cast<float>(template_file.size_y) - static_cast<float>(template_file.anchor_y)) * unit_size;
        const float min_z = -static_cast<float>(template_file.anchor_z) * unit_size;
        const float max_z = (static_cast<float>(template_file.size_z) - static_cast<float>(template_file.anchor_z)) * unit_size;

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
        draw_gpu_building_model(model, origin, placement.rotation, selected, selected, city_window_texture, GfxDebugDrawCategory::CityCells);
    }

    if (selected) {
        DrawCubeWiresV(origin, {unit_size * 2.0F, unit_size * 2.0F, unit_size * 2.0F}, fill);
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
            nullptr);
    }
}

GpuBuildingRenderModel upload_portal_render_model(const BuildingRenderBuffer& buffer, const EditorProject& project, bool textured) {
    GpuBuildingRenderModel model;
    model.wire_lines = buffer.wire_lines;
    model.chunks.reserve(buffer.material_runs.size());
    for (const auto& run : buffer.material_runs) {
        const auto* texture = textured ? texture_for_buffer_material(project, buffer, run.material) : nullptr;
        auto chunk = upload_mesh_chunk(buffer, run.first_triangle, run.triangle_count, run.material, texture);
        if (chunk.mesh.vertexCount > 0) {
            model.uploaded_vertices += static_cast<std::size_t>(chunk.mesh.vertexCount);
            if (chunk.mesh.indices != nullptr) {
                model.uploaded_indices += static_cast<std::size_t>(chunk.mesh.triangleCount) * 3U;
            }
            model.chunks.push_back(std::move(chunk));
        }
    }
    return model;
}

GpuBuildingRenderModel& portal_render_model(EditorProject& project, bool textured, std::optional<std::size_t> selected_portal) {
    const auto blocks = block_meshes(project);
    if (selected_portal.has_value()) {
        if (project.portal_preview_render_dirty ||
            project.portal_preview_render_textured != textured ||
            project.portal_preview_index != selected_portal) {
            project.portal_preview_render_buffer = build_native_portal_render_buffer(project.city->data, blocks, selected_portal);
            project.portal_preview_gpu_model = upload_portal_render_model(project.portal_preview_render_buffer, project, textured);
            project.portal_preview_index = selected_portal;
            project.portal_preview_render_textured = textured;
            project.portal_preview_render_dirty = false;
        }
        return project.portal_preview_gpu_model;
    }

    if (project.portal_render_dirty || project.portal_render_textured != textured) {
        project.portal_render_buffer = build_native_portal_render_buffer(project.city->data, blocks);
        project.portal_gpu_model = upload_portal_render_model(project.portal_render_buffer, project, textured);
        project.portal_render_textured = textured;
        project.portal_render_dirty = false;
    }
    return project.portal_gpu_model;
}

GpuBuildingRenderModel& building_tool_floor_preview_model(
    EditorProject& project,
    std::size_t building_index,
    std::uint32_t floor
) {
    if (project.building_floor_preview_dirty ||
        project.building_floor_preview_building != building_index ||
        project.building_floor_preview_floor != floor) {
        const auto blocks = block_meshes(project);
        const auto templates = buildblock_templates(project);
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

void draw_city_piece_voxels(EditorProject& project, bool textured, std::optional<std::size_t> selected_portal) {
    if (!project.city) {
        return;
    }
    auto& model = portal_render_model(project, textured, selected_portal);
    draw_gpu_building_model(model, {0.0F, 0.0F, 0.0F}, 0, false, true, nullptr, GfxDebugDrawCategory::Pieces);
}

Color city_tile_material_color(const BuildingRenderBuffer& buffer, std::uint32_t material) {
    if (material < buffer.material_names.size()) {
        const auto& name = buffer.material_names[material];
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
    return {64, 83, 67, 255};
}

// Draws pre-uploaded GPU chunks with an identity transform. One DrawMesh per
// material run instead of one immediate-mode call per triangle.
void draw_uploaded_city_model(GpuBuildingRenderModel& model, GfxDebugDrawCategory category) {
    const auto transform = MatrixIdentity();
    rlDisableBackfaceCulling();
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
}

// Frees the GPU meshes/materials a city model owns before it is replaced, so the
// per-edit re-upload doesn't leak VRAM. (Buffers are MemAlloc'd by UploadMesh.)
void unload_city_render_model(GpuBuildingRenderModel& model) {
    for (auto& chunk : model.chunks) {
        if (chunk.mesh.vertexCount > 0) {
            UnloadMesh(chunk.mesh);
        }
        if (chunk.material.maps != nullptr) {
            // Don't unload the shared project texture; just the default material maps.
            chunk.material.maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{};
            UnloadMaterial(chunk.material);
        }
    }
    model.chunks.clear();
    model.wire_lines.clear();
    model.uploaded_vertices = 0;
    model.uploaded_indices = 0;
    model.uploaded_bytes = 0;
}

GpuBuildingRenderModel upload_city_tile_model(const BuildingRenderBuffer& buffer, const EditorProject& project) {
    return upload_city_buffer_model(buffer, project, [&buffer](std::uint32_t material) {
        return city_tile_material_color(buffer, material);
    });
}

GpuBuildingRenderModel upload_city_terrain_model(const BuildingRenderBuffer& buffer, const EditorProject& project) {
    return upload_city_buffer_model(buffer, project, [&buffer](std::uint32_t material) {
        return terrain_material_color(buffer, material);
    });
}

// Simple-mode blockout: flat colored rectangles for road/curb tiles and flat
// squares for intersections, drawn immediate-mode at the authored height. Only
// used when the roads toggle is OFF, so the triangle count is small.
void draw_city_roads_flat_blockout(const MapScene& scene) {
    const auto half = scene.unit_size * 0.5F;
    for (const auto& tile : scene.city_tiles) {
        const auto cx = (static_cast<float>(tile.x) + 0.5F) * scene.unit_size;
        const auto cz = (static_cast<float>(tile.z) + 0.5F) * scene.unit_size;
        const auto y = tile.base_y + scene.unit_size + 0.05F;
        const Vector3 a{cx - half, y, cz - half};
        const Vector3 b{cx + half, y, cz - half};
        const Vector3 c{cx + half, y, cz + half};
        const Vector3 d{cx - half, y, cz + half};
        const Color color = tile.kind == CityTileKind::Curb
            ? Color{150, 154, 151, 255}
            : Color{43, 47, 50, 255};
        DrawTriangle3D(a, b, c, color);
        DrawTriangle3D(a, c, d, color);
        if (g_gfx_debug_stats != nullptr) {
            g_gfx_debug_stats->add_draw(GfxDebugDrawCategory::RoadsCurbs, 2U, 6U, 2U);
        }
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
        draw_uploaded_city_model(project.terrain_gpu_model, GfxDebugDrawCategory::Terrain);
    }

    {
        GfxDebugMaybeTimer timer{GfxDebugTimingPhase::CityRoadDraw};
        if (render_roads) {
            draw_uploaded_city_model(project.city_tile_gpu_model, GfxDebugDrawCategory::RoadsCurbs);
        } else {
            draw_city_roads_flat_blockout(scene);
        }
    }

    for (const auto& road : scene.roads) {
        const auto selected = selected_street != nullptr && road.source == selected_street;
        if (selected) {
            auto from = to_raylib(road.from);
            auto to = to_raylib(road.to);
            from.y += scene.unit_size * 1.1F;
            to.y += scene.unit_size * 1.1F;
            DrawSphere(from, scene.unit_size * 0.35F, {245, 205, 82, 220});
            DrawSphere(to, scene.unit_size * 0.35F, {245, 205, 82, 220});
            if (g_gfx_debug_stats != nullptr) {
                g_gfx_debug_stats->add_draw(GfxDebugDrawCategory::WireOverlay, 0U, 2U, 2U);
            }
        }
    }
}

void draw_city_editor_overlay(const EditorProject&, const MapScene& scene) {
    GfxDebugMaybeTimer timer{GfxDebugTimingPhase::CityRoadDraw};
    const auto marker_size = scene.unit_size * 0.42F;
    const auto marker_lift = marker_size * 0.5F + 0.04F;
    const auto marker_fill = Color{76, 139, 214, 170};
    const auto marker_wire = Color{218, 236, 255, 235};
    const auto segment_color = Color{255, 225, 112, 235};

    DrawGrid(96, scene.unit_size);

    for (const auto& intersection : scene.intersections) {
        auto position = to_raylib(intersection.position);
        position.y += marker_lift;
        const Vector3 size{marker_size, marker_size, marker_size};
        DrawCubeV(position, size, marker_fill);
        DrawCubeWiresV(position, size, marker_wire);
    }

    for (const auto& road : scene.roads) {
        DrawLine3D(to_raylib(road.from), to_raylib(road.to), segment_color);
    }

    if (g_gfx_debug_stats != nullptr) {
        const auto marker_count = static_cast<std::uint64_t>(scene.intersections.size());
        const auto segment_count = static_cast<std::uint64_t>(scene.roads.size());
        g_gfx_debug_stats->add_draw(
            GfxDebugDrawCategory::WireOverlay,
            marker_count * 12U,
            marker_count * 60U + segment_count * 2U,
            marker_count * 2U + segment_count + 1U);
    }
}

// Builds the traffic-light pole as ONE reusable model (post + arm + signal-head
// boxes, the game's 3-box shape), oriented along -Z so a Y-rotation can aim the
// arm. Built once and instanced per pole via DrawModelEx, instead of re-emitting
// cylinders/cubes per pole every frame.
Model build_traffic_light_pole_model(float unit_size) {
    const auto post_h = unit_size * 1.75F;
    const auto post_w = unit_size * 0.10F;
    const auto arm_len = unit_size * 0.85F;
    const auto arm_w = unit_size * 0.08F;
    const auto head_w = unit_size * 0.18F;
    const auto head_h = unit_size * 0.34F;

    // Three boxes in local space (+Y up, arm extends along -Z toward target):
    Mesh post = GenMeshCube(post_w, post_h, post_w);
    Mesh arm = GenMeshCube(arm_w, arm_w, arm_len);
    Mesh head = GenMeshCube(head_w, head_h, head_w);
    // Offset arm and head into place by baking translation into their vertices.
    auto translate_mesh = [](Mesh& m, float dx, float dy, float dz) {
        for (int i = 0; i < m.vertexCount; ++i) {
            m.vertices[i * 3 + 0] += dx;
            m.vertices[i * 3 + 1] += dy;
            m.vertices[i * 3 + 2] += dz;
        }
        UpdateMeshBuffer(m, 0, m.vertices, m.vertexCount * 3 * static_cast<int>(sizeof(float)), 0);
    };
    UploadMesh(&post, false);
    translate_mesh(post, 0.0F, post_h * 0.5F, 0.0F);
    UploadMesh(&arm, false);
    translate_mesh(arm, 0.0F, post_h, -arm_len * 0.5F);
    UploadMesh(&head, false);
    translate_mesh(head, 0.0F, post_h - head_h * 0.4F, -arm_len);

    // Combine into one model with three meshes sharing one material.
    Model model{};
    model.transform = MatrixIdentity();
    model.meshCount = 3;
    model.materialCount = 1;
    model.meshes = static_cast<Mesh*>(MemAlloc(3 * sizeof(Mesh)));
    model.meshes[0] = post;
    model.meshes[1] = arm;
    model.meshes[2] = head;
    model.materials = static_cast<Material*>(MemAlloc(sizeof(Material)));
    model.materials[0] = LoadMaterialDefault();
    model.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = Color{42, 46, 48, 255};
    model.meshMaterial = reinterpret_cast<int*>(MemAlloc(3 * sizeof(int)));
    model.meshMaterial[0] = 0;
    model.meshMaterial[1] = 0;
    model.meshMaterial[2] = 0;
    return model;
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
    for (const auto& pole : scene.traffic_light_poles) {
        const auto base = to_raylib(pole.base);
        // arm_direction is the inward (toward-intersection) XZ vector; the model's
        // arm points along -Z, so rotate about Y to aim it inward.
        const auto yaw = std::atan2(pole.arm_direction.x, -pole.arm_direction.z) * RAD2DEG;
        DrawModelEx(pole_model, base, Vector3{0.0F, 1.0F, 0.0F}, yaw, Vector3{1.0F, 1.0F, 1.0F}, WHITE);
        if (g_gfx_debug_stats != nullptr) {
            std::uint64_t triangles = 0;
            std::uint64_t vertices = 0;
            for (int mesh_index = 0; mesh_index < pole_model.meshCount; ++mesh_index) {
                triangles += static_cast<std::uint64_t>(std::max(pole_model.meshes[mesh_index].triangleCount, 0));
                vertices += static_cast<std::uint64_t>(std::max(pole_model.meshes[mesh_index].vertexCount, 0));
            }
            g_gfx_debug_stats->add_draw(
                GfxDebugDrawCategory::TrafficLightPoles,
                triangles,
                vertices,
                static_cast<std::uint64_t>(std::max(pole_model.meshCount, 0)));
        }
    }
}

void draw_city_cursor(CityGridPoint point, float unit_size, CityEditMode mode) {
    const auto position = Vector3{
        static_cast<float>(point.x) * unit_size,
        static_cast<float>(point.y) * unit_size + unit_size * 0.5F,
        static_cast<float>(point.z) * unit_size,
    };
    auto color = Color{235, 190, 78, 230};
    if (mode == CityEditMode::Roads) {
        color = Color{82, 196, 130, 230};
    } else if (mode == CityEditMode::Pieces) {
        color = Color{91, 166, 206, 230};
    }
    DrawCubeWiresV(position, {unit_size, unit_size, unit_size}, color);
    DrawSphere(position, unit_size * 0.22F, color);
}



void draw_city_block_preview(const SbcCityBlock& city_block, const std::vector<NamedBlockMesh>& meshes, std::size_t selected_layer, std::size_t selected_slot) {
    DrawGrid(24, 4.0F);
    const auto scene = build_city_block_preview_scene(city_block, meshes);
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

} // namespace map_editor::app_internal
