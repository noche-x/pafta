#include "app_internal.hpp"
#include "profiler.hpp"

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
    // Flat mid-gray window surface (no gradient/mullions) to match the exported
    // window planes.
    Image image = GenImageColor(4, 4, Color{128, 128, 128, 255});
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
    MAPED_PROFILE_FUNCTION();
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
        MAPED_PROFILE_THREAD("Building render rebuild");
        MAPED_PROFILE_BLOCK("build async building render buffers");
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

float baked_directional_light(Vector3 normal);
Color shade_color(Color base, float light);
Shader scene_lit_shader();

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
            const auto vertex_light = shade_color(WHITE, baked_directional_light(face_normal));
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
                *out_color++ = vertex_light.r;
                *out_color++ = vertex_light.g;
                *out_color++ = vertex_light.b;
                *out_color++ = vertex_light.a;
                *out_normal++ = unit_normal.x;
                *out_normal++ = unit_normal.y;
                *out_normal++ = unit_normal.z;
            }
        }

        UploadMesh(&fallback.mesh, false);
        fallback.material = LoadMaterialDefault();
        fallback.material.shader = scene_lit_shader();
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
        const auto light = source_index < vertex_normals.size()
            ? baked_directional_light(vertex_normals[source_index])
            : 1.0F;
        const auto vertex_light = shade_color(WHITE, light);
        colors.push_back(vertex_light.r);
        colors.push_back(vertex_light.g);
        colors.push_back(vertex_light.b);
        colors.push_back(vertex_light.a);
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
    chunk.material.shader = scene_lit_shader();
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
//
// City slopes are shallow (1/16..1/7 grades, 3-8 degrees), so a physically
// scaled term barely changes brightness; the horizontal normal components are
// exaggerated before lighting so grades and terrain undulation actually read.
// Hemisphere + key sun + fill light, returned as a single luminance multiplier so
// it stays a drop-in for the baked-vertex-color path. Compared to the old single
// sun + flat ambient floor, surfaces facing away from the sun now read by their
// orientation (sky-vs-ground hemisphere) and a dim opposite fill keeps shadow
// sides from going flat. Tuned so a flat up-facing ground reads ~1.0.
float baked_directional_light(Vector3 normal) {
    constexpr Vector3 sun{0.40F, 0.78F, 0.48F};   // key light, high and off-axis
    constexpr Vector3 fill{-0.45F, 0.30F, -0.40F};  // dim fill, roughly opposite
    constexpr float key_strength = 0.55F;
    constexpr float fill_strength = 0.18F;
    constexpr float sky_amount = 0.50F;   // hemisphere brightness facing up
    constexpr float ground_amount = 0.30F;  // hemisphere brightness facing down
    constexpr float slope_boost = 2.0F;

    const auto len = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (len <= 0.0001F) {
        return 1.0F;
    }
    Vector3 n{normal.x / len, normal.y / len, normal.z / len};
    // Backface culling is off and ground top faces wind so their geometric normal
    // points DOWN; treat those as up-facing so slopes read instead of clamping.
    if (n.y < 0.0F) {
        n = {-n.x, -n.y, -n.z};
    }
    // Exaggerate tilt only for mostly-up faces (ground) so grade reads; walls keep
    // their true angle.
    Vector3 lit_n = n;
    if (n.y > 0.7F) {
        lit_n.x *= slope_boost;
        lit_n.z *= slope_boost;
        const auto boosted = std::sqrt(lit_n.x * lit_n.x + lit_n.y * lit_n.y + lit_n.z * lit_n.z);
        lit_n = {lit_n.x / boosted, lit_n.y / boosted, lit_n.z / boosted};
    }

    const auto sun_len = std::sqrt(sun.x * sun.x + sun.y * sun.y + sun.z * sun.z);
    const auto fill_len = std::sqrt(fill.x * fill.x + fill.y * fill.y + fill.z * fill.z);
    const auto ndotl = (lit_n.x * sun.x + lit_n.y * sun.y + lit_n.z * sun.z) / sun_len;
    const auto ndotf = (lit_n.x * fill.x + lit_n.y * fill.y + lit_n.z * fill.z) / fill_len;

    // Hemisphere ambient: lerp ground->sky by the up component (remapped 0..1).
    const auto up = n.y * 0.5F + 0.5F;
    const auto hemisphere = ground_amount + (sky_amount - ground_amount) * up;
    // Wrapped diffuse (half-Lambert) on the key so the terminator is soft, not a
    // hard cut to black on the shadow side.
    const auto key = key_strength * std::max(0.0F, ndotl * 0.5F + 0.5F);
    const auto fill_light = fill_strength * std::max(0.0F, ndotf);

    return std::min(1.35F, hemisphere + key + fill_light);
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
            "#version 330\n"
            "in vec3 vertexPosition;\n"
            "uniform mat4 mvp;\n"
            "void main() { gl_Position = mvp * vec4(vertexPosition, 1.0); }\n";
        const char* fs =
            "#version 330\n"
            "out vec4 finalColor;\n"
            "void main() { finalColor = vec4(1.0); }\n";
        cache.shader = LoadShaderFromMemory(vs, fs);
        cache.loaded = true;
    }
    return cache;
}

// Per-pixel scene lighting: the same hemisphere + key sun + fill model as the
// baked baked_directional_light, but evaluated per fragment from the interpolated
// world normal so large faces shade smoothly instead of per-vertex. The vertex
// color still arrives (used as ambient occlusion / tint); the fragment multiplies
// texture * colDiffuse * vertexColor * light.
struct LitShader {
    Shader shader{};
    bool loaded{false};
};

LitShader& lit_shader() {
    static LitShader cache;
    if (!cache.loaded) {
        const char* vs =
            "#version 330\n"
            "in vec3 vertexPosition;\n"
            "in vec2 vertexTexCoord;\n"
            "in vec3 vertexNormal;\n"
            "in vec4 vertexColor;\n"
            "uniform mat4 mvp;\n"
            "uniform mat4 matModel;\n"
            "uniform mat4 matNormal;\n"
            "out vec2 fragTexCoord;\n"
            "out vec4 fragColor;\n"
            "out vec3 fragNormal;\n"
            "void main() {\n"
            "    fragTexCoord = vertexTexCoord;\n"
            "    fragColor = vertexColor;\n"
            "    fragNormal = normalize((matNormal * vec4(vertexNormal, 0.0)).xyz);\n"
            "    gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
            "}\n";
        const char* fs =
            "#version 330\n"
            "in vec2 fragTexCoord;\n"
            "in vec4 fragColor;\n"
            "in vec3 fragNormal;\n"
            "uniform sampler2D texture0;\n"
            "uniform vec4 colDiffuse;\n"
            "out vec4 finalColor;\n"
            // Mirror baked_directional_light: hemisphere + half-Lambert key + fill.
            "const vec3 SUN = normalize(vec3(0.40, 0.78, 0.48));\n"
            "const vec3 FILL = normalize(vec3(-0.45, 0.30, -0.40));\n"
            "void main() {\n"
            "    vec3 n = normalize(fragNormal);\n"
            "    if (n.y < 0.0) n = -n;\n"   // ground faces wind downward; treat as up
            "    float up = n.y * 0.5 + 0.5;\n"
            "    float hemisphere = mix(0.30, 0.50, up);\n"
            "    float key = 0.55 * max(0.0, dot(n, SUN) * 0.5 + 0.5);\n"
            "    float fill = 0.18 * max(0.0, dot(n, FILL));\n"
            "    float light = min(1.35, hemisphere + key + fill);\n"
            "    vec4 tex = texture(texture0, fragTexCoord);\n"
            // The mesh vertex colors already bake the same light term; the shader
            // recomputes it per-pixel from the normal, so DON'T multiply fragColor
            // again or lighting would be applied twice (light^2).
            "    finalColor = tex * colDiffuse * vec4(vec3(light), 1.0);\n"
            "}\n";
        cache.shader = LoadShaderFromMemory(vs, fs);
        // matModel/matNormal are standard raylib locations; bind them so the shader
        // gets the model + normal matrices for world-space normals.
        cache.shader.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocation(cache.shader, "matModel");
        cache.shader.locs[SHADER_LOC_MATRIX_NORMAL] = GetShaderLocation(cache.shader, "matNormal");
        cache.loaded = true;
    }
    return cache;
}

Shader scene_lit_shader() {
    return lit_shader().shader;
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
            "#version 330\n"
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
    const Vector3 anchor_offset{
        -static_cast<float>(building.anchor_x) * unit_size,
        -static_cast<float>(building.anchor_y) * unit_size,
        -static_cast<float>(building.anchor_z) * unit_size,
    };
    draw_gpu_building_model(model, anchor_offset, 0, false, true, nullptr, GfxDebugDrawCategory::CityCells);

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

    constexpr int kSize = 192; // render larger than displayed for crisp downscale.
    constexpr float unit_size = 4.0F;
    auto& building = project.buildings[index].data;

    // Build the building mesh fresh (the shared tool-preview cache is keyed to a
    // single building, so we don't reuse it here) and upload a throwaway model.
    const auto blocks = block_meshes(project);
    const auto templates = buildblock_templates(project);
    const auto buffer = build_native_building_tool_preview_render_buffer(building, blocks, templates, std::nullopt);
    auto model = upload_building_render_model(buffer, building, project);

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
    BeginTextureMode(target);
    ClearBackground(Color{32, 36, 41, 255});
    BeginMode3D(cam);
    rlEnableDepthTest();
    draw_gpu_building_model(model, anchor_offset, 0, false, false, nullptr, GfxDebugDrawCategory::CityCells);
    // A subtle ground plane gives the building a sense of scale/orientation.
    DrawGrid(static_cast<int>(std::max(building.size_x, building.size_y)) + 2, unit_size);
    EndMode3D();
    EndTextureMode();
    GenTextureMipmaps(&target.texture);
    SetTextureFilter(target.texture, TEXTURE_FILTER_BILINEAR);

    unload_city_render_model(model);
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

    BeginTextureMode(target);
    ClearBackground(Color{32, 36, 41, 255});
    BeginMode3D(cam);
    rlEnableDepthTest();
    if (scene.solid_cell.has_value()) {
        draw_render_buffer_faces(scene.solid_cell->buffer, {120, 160, 210, 255});
    }
    for (const auto& piece : scene.pieces) {
        draw_mesh_faces(piece.mesh, piece.origin, scene.unit_size, {120, 160, 210, 255});
    }
    EndMode3D();
    EndTextureMode();
    GenTextureMipmaps(&target.texture);
    SetTextureFilter(target.texture, TEXTURE_FILTER_BILINEAR);

    project.city_block_thumbnail_ready[index] = true;
    return &target;
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
    const Texture2D* city_window_texture,
    bool hovered) {
    constexpr auto unit_size = 4.0F;
    (void)hovered;
    const auto render_origin = native_city_building_render_origin(placement, unit_size);
    const auto origin = Vector3{render_origin.x, render_origin.y, render_origin.z};
    const auto fill = selected ? Color{235, 194, 82, 210} : Color{126, 151, 174, 170};
    const auto wire = selected ? Color{255, 240, 150, 255} : Color{30, 36, 42, 170};

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
        draw_gpu_building_model(model, origin, placement.rotation, false, false, city_window_texture, GfxDebugDrawCategory::CityCells);
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
    if ((project.portal_render_dirty || project.portal_render_textured != textured) &&
        !project.portal_rebuild_in_progress) {
        MAPED_PROFILE_BLOCK("portal_render_model queue async rebuild");
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
    draw_gpu_building_model(model, {0.0F, 0.25F, 0.0F}, 0, false, draw_wires, nullptr, GfxDebugDrawCategory::Pieces);

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
    // GpuBuildingMeshChunk owns its mesh/material via RAII: its destructor calls
    // UnloadMesh and MemFree(material.maps) (and intentionally leaves the shared
    // project texture alone). Clearing the vector runs those destructors. A manual
    // UnloadMesh here would leave each mesh's stale vbo/vertex pointers in place,
    // so the chunk destructor would then UnloadMesh the same buffers a second time
    // and abort with a double free.
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
    constexpr float road_surface_offset = 0.25F;
    const auto marker_fill = Color{76, 139, 214, 170};
    const auto marker_wire = Color{218, 236, 255, 235};
    const auto segment_color = Color{255, 225, 112, 235};

    for (const auto& intersection : scene.intersections) {
        auto position = to_raylib(intersection.position);
        position.y = static_cast<float>(intersection.source->y) * scene.unit_size;
        const Vector3 size{marker_size, marker_size, marker_size};
        DrawCubeV(position, size, marker_fill);
        DrawCubeWiresV(position, size, marker_wire);
    }

    std::uint64_t segment_count = 0;
    for (const auto& road : scene.roads) {
        const auto seg_len = std::sqrt(
            (road.to.x - road.from.x) * (road.to.x - road.from.x) +
            (road.to.z - road.from.z) * (road.to.z - road.from.z));
        const auto steps = std::clamp(static_cast<int>(seg_len / scene.unit_size), 1, 256);
        auto height_at = [&](float t) {
            return road.from.y + (road.to.y - road.from.y) * t - road_surface_offset;
        };

        auto prev = Vector3{
            road.from.x,
            height_at(0.0F),
            road.from.z,
        };
        for (int i = 1; i <= steps; ++i) {
            const auto t = static_cast<float>(i) / static_cast<float>(steps);
            const auto x = road.from.x + (road.to.x - road.from.x) * t;
            const auto z = road.from.z + (road.to.z - road.from.z) * t;
            const auto cur = Vector3{x, height_at(t), z};
            DrawLine3D(prev, cur, segment_color);
            prev = cur;
            ++segment_count;
        }
    }

    if (g_gfx_debug_stats != nullptr) {
        const auto marker_count = static_cast<std::uint64_t>(scene.intersections.size());
        g_gfx_debug_stats->add_draw(
            GfxDebugDrawCategory::WireOverlay,
            marker_count * 12U,
            marker_count * 60U + segment_count * 2U,
            marker_count * 2U + segment_count);
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
    model.materials[0].shader = scene_lit_shader();
    model.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = Color{40, 43, 45, 255};
    model.materials[1] = LoadMaterialDefault();
    model.materials[1].shader = scene_lit_shader();
    model.materials[1].maps[MATERIAL_MAP_DIFFUSE].color = Color{205, 48, 38, 255};

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
        // The model's arm points along -Z; raylib's +Y rotation is right-handed
        // (yaw +90 maps -Z to -X), so the X component is negated to aim the arm
        // along arm_direction (same handedness flip as the building rotations).
        const auto yaw = std::atan2(-pole.arm_direction.x, -pole.arm_direction.z) * RAD2DEG;
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
    // rlVertex3f/rlColor4ub instead of N DrawLine3D calls. Lines on every 10th
    // absolute grid coordinate are "major": brighter and a 3-line bundle so they
    // read as thicker without per-line width.
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
        const int bundle = major ? 1 : 0;
        for (int b = -bundle; b <= bundle; ++b) {
            const float off = static_cast<float>(b) * unit * 0.05F;
            rlVertex3f(wx + off, plane_y, min_z);
            rlVertex3f(wx + off, plane_y, max_z);
        }
    }
    for (std::int32_t gz = center.z - half_cells; gz <= center.z + half_cells; ++gz) {
        const bool axis = gz == center.z;
        const bool major = axis || is_major(gz);
        set_color(axis, major);
        const float wz = static_cast<float>(gz) * unit;
        const int bundle = major ? 1 : 0;
        for (int b = -bundle; b <= bundle; ++b) {
            const float off = static_cast<float>(b) * unit * 0.05F;
            rlVertex3f(min_x, plane_y, wz + off);
            rlVertex3f(max_x, plane_y, wz + off);
        }
    }
    rlEnd();
    rlDrawRenderBatchActive();
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

} // namespace map_editor::app_internal
