// SPDX-License-Identifier: GPL-2.0-only
#include "mesh_export.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <future>
#include <stdexcept>
#include <unordered_map>

#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#include "tiny_gltf.h"

#include "raylib.h"

#include "app_internal.hpp"

namespace pafta {
namespace {

// The game and glTF both have Y pointing up. Apply the placement basis, then the
// export offset. Keeping these axes also keeps the triangle winding.
Vec3 transform_vertex(const ExportMeshGroup& group, const Vec3& v) {
    Vec3 world = v;
    if (group.transformed) {
        world = {
            group.origin.x + group.basis.x_axis.x * v.x + group.basis.y_axis.x * v.y + group.basis.z_axis.x * v.z,
            group.origin.y + group.basis.x_axis.y * v.x + group.basis.y_axis.y * v.y + group.basis.z_axis.y * v.z,
            group.origin.z + group.basis.x_axis.z * v.x + group.basis.y_axis.z * v.y + group.basis.z_axis.z * v.z,
        };
    }
    return {world.x - group.world_offset.x, world.y - group.world_offset.y, world.z - group.world_offset.z};
}

Vec3 face_normal(const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 ab{b.x - a.x, b.y - a.y, b.z - a.z};
    const Vec3 ac{c.x - a.x, c.y - a.y, c.z - a.z};
    Vec3 n{
        ab.y * ac.z - ab.z * ac.y,
        ab.z * ac.x - ab.x * ac.z,
        ab.x * ac.y - ab.y * ac.x,
    };
    const auto len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    if (len > 1e-8F) {
        n.x /= len;
        n.y /= len;
        n.z /= len;
    }
    return n;
}

// Loose triangles with flat normals and no shared vertices, for one material across
// all the groups that use it. Each triangle adds 3 new vertices, so faces never
// share normals or UVs.
struct PrimitiveData {
    std::vector<float> positions; // xyz per vertex
    std::vector<float> normals;   // xyz per vertex
    std::vector<float> uvs;       // uv per vertex
    Vec3 min{};
    Vec3 max{};
    bool has_bounds{false};

    void push_vertex(const Vec3& p, const Vec3& n, float u, float v) {
        positions.insert(positions.end(), {p.x, p.y, p.z});
        normals.insert(normals.end(), {n.x, n.y, n.z});
        uvs.insert(uvs.end(), {u, v});
        if (!has_bounds) {
            min = max = p;
            has_bounds = true;
        } else {
            min = {std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z)};
            max = {std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z)};
        }
    }
};

bool procedural_material_color(const std::string& name, double rgb[3]) {
    const auto color = procedural_material_rgb(name);
    if (!color) return false;
    for (std::size_t i = 0; i < color->size(); ++i) rgb[i] = (*color)[i] / 255.0;
    return true;
}

std::string sanitize_filename(std::string s) {
    for (auto& ch : s) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|' || ch == ' ') {
            ch = '_';
        }
    }
    return s;
}

// Turns a texture read back from the GPU into an upright PNG. The editor flips
// textures upside down when loading them, so flip once more to get the PNG's
// original way up for modelling tools. CPU only; takes ownership of `image`.
bool encode_texture_png(Image image, const std::filesystem::path& out) {
    ImageFlipVertical(&image);
    ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    const bool ok = ExportImage(image, out.string().c_str());
    UnloadImage(image);
    return ok;
}

} // namespace

std::size_t estimate_city_export_triangles(
    const SbcMap& map,
    const std::vector<const SbbBuilding*>& placement_buildings,
    const std::vector<NamedBlockMesh>& block_meshes,
    const std::vector<NamedBuildblockTemplate>& buildblock_templates) {
    std::size_t total = 0;
    for (std::size_t i = 0; i < map.buildings.size(); ++i) {
        const SbbBuilding* sbb = i < placement_buildings.size() ? placement_buildings[i] : nullptr;
        if (sbb == nullptr) {
            continue;
        }
        const auto buffer = build_native_building_render_buffer(*sbb, block_meshes, buildblock_templates);
        total += buffer.triangles.size();
    }
    return total;
}

struct GltfExport {
    std::filesystem::path path;
    tinygltf::Model model;
    // The index and name of each material that may use a project texture.
    std::vector<std::pair<int, std::string>> textures;
    std::size_t next_texture{};
    // PNG encodes running off the GL thread, with their file names.
    std::vector<std::pair<std::future<bool>, std::string>> encodes;
    ExportStats stats;
};

void GltfExportDeleter::operator()(GltfExport* job) const {
    delete job;
}

GltfExportPtr build_gltf_export(
    const std::filesystem::path& gltf_path,
    const std::vector<ExportMeshGroup>& groups,
    const ExportOptions& options,
    ExportResult& result) {
    // Collect material name -> triangles for that material. Material 0 with no name
    // still gets a default group, so geometry without textures is exported too.
    std::vector<std::string> material_order;
    std::unordered_map<std::string, std::size_t> material_index;
    std::vector<PrimitiveData> primitives;

    auto bucket_for = [&](const std::string& mat_name) -> PrimitiveData& {
        const auto key = mat_name.empty() ? std::string("_untextured") : mat_name;
        auto it = material_index.find(key);
        if (it == material_index.end()) {
            material_index.emplace(key, material_order.size());
            material_order.push_back(key);
            primitives.emplace_back();
            return primitives.back();
        }
        return primitives[it->second];
    };

    for (const auto& group : groups) {
        if (group.buffer == nullptr) {
            continue;
        }
        const auto& buffer = *group.buffer;
        for (const auto& tri : buffer.triangles) {
            // Window and door panes are the city_window_surface triangles. Leave them
            // out completely when the option is off, so openings show as holes.
            if (tri.city_window_surface && !options.include_window_panes) {
                continue;
            }
            for (const auto index : tri.indices) {
                if (index >= buffer.vertices.size()) {
                    throw std::runtime_error("Invalid triangle vertex in export group " + group.name);
                }
            }
            const auto& v0 = buffer.vertices[tri.indices[0]];
            const auto& v1 = buffer.vertices[tri.indices[1]];
            const auto& v2 = buffer.vertices[tri.indices[2]];
            const Vec3 p0 = transform_vertex(group, v0.position);
            const Vec3 p1 = transform_vertex(group, v1.position);
            const Vec3 p2 = transform_vertex(group, v2.position);
            const Vec3 n = face_normal(p0, p1, p2);

            // Find the texture name for the triangle's material. Window and door panes
            // (city_window_surface) all share one solid grey material. Buildings look up
            // a material slot in building_materials; terrain and tile buffers use
            // material_names.
            std::string mat_name;
            if (tri.city_window_surface) {
                mat_name = "__window";
            } else if (group.building_materials != nullptr) {
                const auto slot = static_cast<std::size_t>(tri.material & 0xffU);
                if (slot < group.building_materials->size()) {
                    mat_name = (*group.building_materials)[slot];
                }
            } else if (tri.material < buffer.material_names.size()) {
                mat_name = buffer.material_names[tri.material];
            }
            auto& prim = bucket_for(mat_name);
            prim.push_vertex(p0, n, v0.u, v0.v);
            prim.push_vertex(p1, n, v1.u, v1.v);
            prim.push_vertex(p2, n, v2.u, v2.v);
        }
    }

    if (primitives.empty()) {
        result.message = "Nothing to export (no geometry in view).";
        return nullptr;
    }

    GltfExportPtr job{new GltfExport};
    job->path = gltf_path;
    auto& model = job->model;
    model.asset.version = "2.0";
    model.asset.generator = "Pafta " PAFTA_VERSION;
    tinygltf::Mesh mesh;
    mesh.name = gltf_path.stem().string();

    // One shared .bin buffer holds all the attribute and index data.
    tinygltf::Buffer gltf_buffer;
    auto& bin = gltf_buffer.data;

    // Attributes and indices share one binary buffer. The target tells glTF how to use
    // the bytes; it doesn't change how they are laid out.
    const auto append_buffer_view = [&](const auto& values, int target) {
        const auto offset = bin.size();
        const auto bytes = values.size() * sizeof(values.front());
        bin.resize(offset + bytes);
        std::memcpy(bin.data() + offset, values.data(), bytes);
        tinygltf::BufferView view;
        view.buffer = 0;
        view.byteOffset = offset;
        view.byteLength = bytes;
        view.target = target;
        model.bufferViews.push_back(view);
        return static_cast<int>(model.bufferViews.size() - 1);
    };

    for (std::size_t pi = 0; pi < primitives.size(); ++pi) {
        auto& prim = primitives[pi];
        const auto vertex_count = prim.positions.size() / 3;
        if (vertex_count == 0) {
            continue;
        }
        job->stats.vertex_count += vertex_count;
        job->stats.triangle_count += vertex_count / 3;

        std::vector<std::uint32_t> indices(vertex_count);
        for (std::uint32_t i = 0; i < vertex_count; ++i) {
            indices[i] = i;
        }

        const int pos_view = append_buffer_view(prim.positions, TINYGLTF_TARGET_ARRAY_BUFFER);
        const int nrm_view = append_buffer_view(prim.normals, TINYGLTF_TARGET_ARRAY_BUFFER);
        const int uv_view = append_buffer_view(prim.uvs, TINYGLTF_TARGET_ARRAY_BUFFER);
        const int idx_view = append_buffer_view(indices, TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);

        tinygltf::Accessor pos_acc;
        pos_acc.bufferView = pos_view;
        pos_acc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
        pos_acc.count = vertex_count;
        pos_acc.type = TINYGLTF_TYPE_VEC3;
        pos_acc.minValues = {prim.min.x, prim.min.y, prim.min.z};
        pos_acc.maxValues = {prim.max.x, prim.max.y, prim.max.z};
        model.accessors.push_back(pos_acc);
        const int pos_idx = static_cast<int>(model.accessors.size() - 1);

        tinygltf::Accessor nrm_acc;
        nrm_acc.bufferView = nrm_view;
        nrm_acc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
        nrm_acc.count = vertex_count;
        nrm_acc.type = TINYGLTF_TYPE_VEC3;
        model.accessors.push_back(nrm_acc);
        const int nrm_idx = static_cast<int>(model.accessors.size() - 1);

        tinygltf::Accessor uv_acc;
        uv_acc.bufferView = uv_view;
        uv_acc.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
        uv_acc.count = vertex_count;
        uv_acc.type = TINYGLTF_TYPE_VEC2;
        model.accessors.push_back(uv_acc);
        const int uv_idx = static_cast<int>(model.accessors.size() - 1);

        tinygltf::Accessor idx_acc;
        idx_acc.bufferView = idx_view;
        idx_acc.componentType = TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
        idx_acc.count = vertex_count;
        idx_acc.type = TINYGLTF_TYPE_SCALAR;
        model.accessors.push_back(idx_acc);
        const int idx_idx = static_cast<int>(model.accessors.size() - 1);

        // Material; its texture is added later on the GL thread.
        tinygltf::Material material;
        material.name = material_order[pi];
        material.pbrMetallicRoughness.metallicFactor = 0.0;
        material.pbrMetallicRoughness.roughnessFactor = 1.0;
        material.doubleSided = true;

        const auto& mat_name = material_order[pi];
        double proc_rgb[3];
        if (procedural_material_color(mat_name, proc_rgb)) {
            material.pbrMetallicRoughness.baseColorFactor = {proc_rgb[0], proc_rgb[1], proc_rgb[2], 1.0};
        }
        model.materials.push_back(material);
        const int mat_idx = static_cast<int>(model.materials.size() - 1);
        if (mat_name != "_untextured") job->textures.emplace_back(mat_idx, mat_name);

        tinygltf::Primitive primitive;
        primitive.attributes["POSITION"] = pos_idx;
        primitive.attributes["NORMAL"] = nrm_idx;
        primitive.attributes["TEXCOORD_0"] = uv_idx;
        primitive.indices = idx_idx;
        primitive.material = mat_idx;
        primitive.mode = TINYGLTF_MODE_TRIANGLES;
        mesh.primitives.push_back(primitive);
    }
    job->stats.material_count = material_order.size();

    model.buffers.push_back(std::move(gltf_buffer));
    model.meshes.push_back(mesh);

    tinygltf::Node node;
    node.mesh = 0;
    model.nodes.push_back(node);
    tinygltf::Scene scene;
    scene.nodes.push_back(0);
    model.scenes.push_back(scene);
    model.defaultScene = 0;
    return job;
}

std::size_t gltf_export_texture_count(const GltfExport& job) {
    return job.textures.size();
}

std::size_t gltf_export_textures_done(const GltfExport& job) {
    return job.next_texture;
}

bool write_next_gltf_texture(GltfExport& job, const EditorProject& project) {
    if (job.next_texture >= job.textures.size()) return false;
    const auto& [material_index, mat_name] = job.textures[job.next_texture++];
    const Texture2D* tex = app_internal::find_project_texture(project, mat_name);
    if (tex == nullptr || tex->id == 0U) return true;
    // build_gltf_export queues one readback per material name. The image number keeps
    // file names different when cleanup turns different names into the same text.
    const auto png_name = std::to_string(job.model.images.size()) + "_" + sanitize_filename(mat_name) + ".png";
    // Only the readback needs the GL thread; encoding runs on a worker.
    job.encodes.reserve(job.textures.size());
    auto encode_name = png_name;
    auto output = job.path.parent_path() / png_name;
    Image pixels = LoadImageFromTexture(*tex);
    if (!IsImageValid(pixels)) {
        throw std::runtime_error("Cannot read back texture " + mat_name);
    }
    // The worker owns the pixels once async succeeds. Make the file name and reserve
    // the job slot first, so adding it afterwards only moves things.
    std::future<bool> encode;
    try {
        encode = std::async(std::launch::async, [pixels, out = std::move(output)] {
            return encode_texture_png(pixels, out);
        });
    } catch (...) {
        UnloadImage(pixels);
        throw;
    }
    job.encodes.emplace_back(std::move(encode), std::move(encode_name));
    tinygltf::Image image;
    image.uri = png_name;
    job.model.images.push_back(image);
    tinygltf::Texture gltf_tex;
    gltf_tex.source = static_cast<int>(job.model.images.size() - 1);
    job.model.textures.push_back(gltf_tex);
    const auto gltf_tex_index = static_cast<int>(job.model.textures.size() - 1);
    ++job.stats.texture_count;
    job.model.materials[static_cast<std::size_t>(material_index)].pbrMetallicRoughness.baseColorTexture.index = gltf_tex_index;
    return true;
}

ExportResult write_gltf_export(GltfExport& job) {
    ExportResult result;
    for (auto& [encode, name] : job.encodes) {
        if (!encode.get()) {
            result.message = "Failed to write texture " + name;
            return result;
        }
    }
    job.encodes.clear();
    tinygltf::TinyGLTF writer;
    const bool ok = writer.WriteGltfSceneToFile(
        &job.model, job.path.string(),
        /*embedImages=*/false,
        /*embedBuffers=*/false,
        /*prettyPrint=*/true,
        /*writeBinary=*/false);
    if (!ok) {
        result.message = "Failed to write glTF to " + job.path.string();
        return result;
    }
    result.ok = true;
    result.stats = job.stats;
    result.message = "Exported " + std::to_string(job.stats.triangle_count) + " triangles, " +
                     std::to_string(job.stats.material_count) + " materials, " +
                     std::to_string(job.stats.texture_count) + " textures to " + job.path.filename().string();
    return result;
}

ExportResult export_groups_to_gltf(
    const std::filesystem::path& gltf_path,
    const std::vector<ExportMeshGroup>& groups,
    const EditorProject& project,
    const ExportOptions& options) {
    ExportResult result;
    auto job = build_gltf_export(gltf_path, groups, options, result);
    if (!job) return result;
    while (write_next_gltf_texture(*job, project)) {}
    return write_gltf_export(*job);
}

BuildingRenderBuffer build_block_mesh_render_buffer(const SblBlockMesh& mesh, float unit_size) {
    BuildingRenderBuffer buffer;
    buffer.unit_size = unit_size;
    // A block on its own has no cell to look up material slots in, so name the groups
    // by slot. Stored UVs still export; textures only attach if a project material with
    // this name exists.
    auto material_for_slot = [&](std::uint32_t slot) {
        const auto name = "material_" + std::to_string(slot);
        for (std::uint32_t i = 0; i < buffer.material_names.size(); ++i) {
            if (buffer.material_names[i] == name) {
                return i;
            }
        }
        buffer.material_names.push_back(name);
        return static_cast<std::uint32_t>(buffer.material_names.size() - 1U);
    };

    for (const auto& face : mesh.faces) {
        if (face.vertices.size() < 4) {
            continue;
        }
        const auto material = material_for_slot(face.material & 0xffffU);
        std::array<std::uint32_t, 4> idx{};
        for (std::size_t i = 0; i < 4; ++i) {
            const auto& v = face.vertices[i];
            const Vec2f uv = i < face.uvs.size() ? face.uvs[i] : Vec2f{};
            idx[i] = static_cast<std::uint32_t>(buffer.vertices.size());
            buffer.vertices.push_back({{v.x * unit_size, v.y * unit_size, v.z * unit_size}, uv.u, uv.v});
        }
        buffer.triangles.push_back({{idx[0], idx[1], idx[2]}, material, false});
        buffer.triangles.push_back({{idx[0], idx[2], idx[3]}, material, false});
    }
    return buffer;
}

std::vector<ExportMeshGroup> city_export_groups(
    const MapScene& scene,
    const std::vector<const BuildingRenderBuffer*>& placement_buffers,
    const std::vector<const SbbBuilding*>& placement_buildings,
    const BuildingRenderBuffer& piece_buffer,
    const ExportOptions& options) {
    std::vector<ExportMeshGroup> groups;

    // Move the whole city so it is centred on the origin: subtract the centre of its
    // bounds (X and Z only, so the ground stays at its height) from every group's
    // world vertices.
    const Vec3 recenter{
        (scene.bounds.min_x + scene.bounds.max_x) * 0.5F,
        0.0F,
        (scene.bounds.min_z + scene.bounds.max_z) * 0.5F,
    };

    // Terrain and road and curb tiles export as world-space groups, unmoved.
    if (options.include_terrain && scene.terrain.has_value()) {
        ExportMeshGroup terrain;
        terrain.name = "terrain";
        terrain.buffer = &scene.terrain->mesh;
        terrain.world_offset = recenter;
        groups.push_back(terrain);
    }
    if (options.include_roads) {
        ExportMeshGroup tiles;
        tiles.name = "roads";
        tiles.buffer = &scene.city_tile_buffer;
        tiles.world_offset = recenter;
        groups.push_back(tiles);
    }
    // City piece and portal geometry (districts placed with the Piece tool). The live
    // view draws it raised 0.25 in Y, so world_offset.y subtracts -0.25.
    if (options.include_pieces && !piece_buffer.triangles.empty()) {
        ExportMeshGroup pieces;
        pieces.name = "pieces";
        pieces.buffer = &piece_buffer;
        pieces.world_offset = {recenter.x, -0.25F, recenter.z};
        groups.push_back(pieces);
    }

    // Each placed building, moved into world space by its placement basis.
    if (options.include_buildings) {
        for (std::size_t i = 0; i < scene.buildings.size() && i < placement_buffers.size(); ++i) {
            const auto& placement = scene.buildings[i];
            if (placement_buffers[i] == nullptr || placement_buffers[i]->triangles.empty()) {
                continue;
            }
            ExportMeshGroup group;
            group.name = "building_" + std::to_string(i);
            group.buffer = placement_buffers[i];
            group.basis = native_city_building_rotation_basis(static_cast<std::int32_t>(placement.rotation));
            group.origin = native_city_building_render_origin(placement.source, scene.unit_size);
            group.world_offset = recenter;
            group.transformed = true;
            if (i < placement_buildings.size() && placement_buildings[i] != nullptr) {
                group.building_materials = &placement_buildings[i]->materials;
            }
            groups.push_back(group);
        }
    }

    return groups;
}

} // namespace pafta
