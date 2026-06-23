#include "mesh_export.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

// raylib already links stb_image / stb_image_write; let tinygltf use the existing
// symbols rather than recompiling them (avoids duplicate-symbol link errors). We
// write texture PNGs via raylib's ExportImage, so tinygltf only needs glTF I/O.
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#define TINYGLTF_NO_EXTERNAL_IMAGE
#include "tiny_gltf.h"

#include "raylib.h"

#include "app_internal.hpp"

namespace map_editor {
namespace {

// Engine and glTF are both Y-up; geometry renders correctly as-authored, so we
// export positions verbatim and keep the original (CCW) winding. No axis mirror is
// applied, so no winding reversal is needed to keep faces outward.
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

// Flat-normal, de-indexed triangle soup for one material across all groups that
// reference it. Each triangle contributes 3 fresh vertices so faces never share
// normals/UVs.
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

// Procedural material colors the city view renders without a texture (render_gpu
// city_tile_material_color / terrain_material_color). Returns true and fills rgb
// (0..1) when the name is one of these, so the glTF material gets a matching
// baseColorFactor instead of plain white.
bool procedural_material_color(const std::string& name, double rgb[3]) {
    auto set = [&](int r, int g, int b) {
        rgb[0] = r / 255.0;
        rgb[1] = g / 255.0;
        rgb[2] = b / 255.0;
        return true;
    };
    if (name == "city_road_asphalt") return set(43, 47, 50);
    if (name == "city_curb_concrete") return set(150, 154, 151);
    if (name == "city_terrain_grass") return set(64, 83, 67);
    if (name == "__window") return set(128, 128, 128);
    return false;
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

// Reads a project texture back off the GPU and re-flips it to upright. The loaded
// GPU texture is vertically flipped (editor convention applied at load), so flip
// once more to restore the original PNG orientation for DCC tools.
bool write_texture_png(const Texture2D& texture, const std::filesystem::path& out) {
    if (texture.id == 0U) {
        return false;
    }
    Image image = LoadImageFromTexture(texture);
    if (!IsImageValid(image)) {
        return false;
    }
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

ExportResult export_groups_to_gltf(
    const std::filesystem::path& gltf_path,
    const std::vector<ExportMeshGroup>& groups,
    const EditorProject& project,
    const ExportOptions& options) {
    ExportResult result;

    // Collect material name -> primitive accumulator. Material 0 with no name still
    // gets a default bucket so untextured geometry exports.
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
            // Window/door panes are the city_window_surface triangles; drop them
            // entirely when the option is off so openings read as holes.
            if (tri.city_window_surface && !options.include_window_panes) {
                continue;
            }
            const auto& v0 = buffer.vertices[tri.indices[0]];
            const auto& v1 = buffer.vertices[tri.indices[1]];
            const auto& v2 = buffer.vertices[tri.indices[2]];
            const Vec3 p0 = transform_vertex(group, v0.position);
            const Vec3 p1 = transform_vertex(group, v1.position);
            const Vec3 p2 = transform_vertex(group, v2.position);
            const Vec3 n = face_normal(p0, p1, p2);

            // Resolve the triangle's material to a texture name. Window/door panes
            // (city_window_surface) all share one solid-gray material. Buildings
            // index a material slot into building_materials; terrain/tile buffers
            // use material_names.
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
        return result;
    }

    const auto out_dir = gltf_path.parent_path();
    const auto stem = gltf_path.stem().string();

    tinygltf::Model model;
    model.asset.version = "2.0";
    model.asset.generator = "map_editor_cpp";
    tinygltf::Scene scene;
    tinygltf::Mesh mesh;
    mesh.name = stem;

    // One shared .bin buffer holds all attribute + index data.
    tinygltf::Buffer gltf_buffer;
    auto& bin = gltf_buffer.data;

    auto append_float_view = [&](const std::vector<float>& src) {
        const std::size_t offset = bin.size();
        const std::size_t bytes = src.size() * sizeof(float);
        bin.resize(offset + bytes);
        std::memcpy(bin.data() + offset, src.data(), bytes);
        tinygltf::BufferView view;
        view.buffer = 0;
        view.byteOffset = offset;
        view.byteLength = bytes;
        view.target = TINYGLTF_TARGET_ARRAY_BUFFER;
        model.bufferViews.push_back(view);
        return static_cast<int>(model.bufferViews.size() - 1);
    };
    auto append_index_view = [&](const std::vector<std::uint32_t>& src) {
        const std::size_t offset = bin.size();
        const std::size_t bytes = src.size() * sizeof(std::uint32_t);
        bin.resize(offset + bytes);
        std::memcpy(bin.data() + offset, src.data(), bytes);
        tinygltf::BufferView view;
        view.buffer = 0;
        view.byteOffset = offset;
        view.byteLength = bytes;
        view.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;
        model.bufferViews.push_back(view);
        return static_cast<int>(model.bufferViews.size() - 1);
    };

    std::size_t total_vertices = 0;
    std::size_t total_triangles = 0;
    std::size_t texture_count = 0;
    std::unordered_map<std::string, int> texture_cache; // material name -> gltf texture index

    for (std::size_t pi = 0; pi < primitives.size(); ++pi) {
        auto& prim = primitives[pi];
        const auto vertex_count = prim.positions.size() / 3;
        if (vertex_count == 0) {
            continue;
        }
        total_vertices += vertex_count;
        total_triangles += vertex_count / 3;

        std::vector<std::uint32_t> indices(vertex_count);
        for (std::uint32_t i = 0; i < vertex_count; ++i) {
            indices[i] = i;
        }

        const int pos_view = append_float_view(prim.positions);
        const int nrm_view = append_float_view(prim.normals);
        const int uv_view = append_float_view(prim.uvs);
        const int idx_view = append_index_view(indices);

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

        // Material + optional texture.
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
        if (mat_name != "_untextured") {
            const Texture2D* tex = app_internal::find_project_texture(project, mat_name);
            if (tex != nullptr && tex->id != 0U) {
                int gltf_tex_index = -1;
                auto cached = texture_cache.find(mat_name);
                if (cached != texture_cache.end()) {
                    gltf_tex_index = cached->second;
                } else {
                    const auto png_name = sanitize_filename(mat_name) + ".png";
                    if (write_texture_png(*tex, out_dir / png_name)) {
                        tinygltf::Image image;
                        image.uri = png_name;
                        model.images.push_back(image);
                        tinygltf::Texture gltf_tex;
                        gltf_tex.source = static_cast<int>(model.images.size() - 1);
                        model.textures.push_back(gltf_tex);
                        gltf_tex_index = static_cast<int>(model.textures.size() - 1);
                        texture_cache.emplace(mat_name, gltf_tex_index);
                        ++texture_count;
                    }
                }
                if (gltf_tex_index >= 0) {
                    material.pbrMetallicRoughness.baseColorTexture.index = gltf_tex_index;
                }
            }
        }
        model.materials.push_back(material);
        const int mat_idx = static_cast<int>(model.materials.size() - 1);

        tinygltf::Primitive primitive;
        primitive.attributes["POSITION"] = pos_idx;
        primitive.attributes["NORMAL"] = nrm_idx;
        primitive.attributes["TEXCOORD_0"] = uv_idx;
        primitive.indices = idx_idx;
        primitive.material = mat_idx;
        primitive.mode = TINYGLTF_MODE_TRIANGLES;
        mesh.primitives.push_back(primitive);
    }

    model.buffers.push_back(gltf_buffer);
    model.meshes.push_back(mesh);

    tinygltf::Node node;
    node.mesh = 0;
    model.nodes.push_back(node);
    scene.nodes.push_back(0);
    model.scenes.push_back(scene);
    model.defaultScene = 0;

    tinygltf::TinyGLTF writer;
    const bool ok = writer.WriteGltfSceneToFile(
        &model, gltf_path.string(),
        /*embedImages=*/false,
        /*embedBuffers=*/false,
        /*prettyPrint=*/true,
        /*writeBinary=*/false);

    if (!ok) {
        result.message = "Failed to write glTF to " + gltf_path.string();
        return result;
    }

    result.ok = true;
    result.stats = {total_vertices, total_triangles, material_order.size(), texture_count};
    result.message = "Exported " + std::to_string(total_triangles) + " triangles, " +
                     std::to_string(material_order.size()) + " materials, " +
                     std::to_string(texture_count) + " textures to " + gltf_path.filename().string();
    return result;
}

BuildingRenderBuffer build_block_mesh_render_buffer(const SblBlockMesh& mesh, float unit_size) {
    BuildingRenderBuffer buffer;
    buffer.unit_size = unit_size;
    // Standalone block has no cell to resolve material slots against, so name groups
    // by slot. Authored UVs still export; textures only bind if a project material of
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

ExportResult export_block_to_gltf(
    const std::filesystem::path& gltf_path,
    const SblBlockMesh& mesh,
    const std::string& name,
    const EditorProject& project) {
    const auto buffer = build_block_mesh_render_buffer(mesh);
    return export_building_to_gltf(gltf_path, buffer, name, project);
}

ExportResult export_building_to_gltf(
    const std::filesystem::path& gltf_path,
    const BuildingRenderBuffer& building,
    const std::string& name,
    const EditorProject& project,
    const std::vector<std::string>* building_materials,
    const ExportOptions& options) {
    ExportMeshGroup group;
    group.name = name;
    group.buffer = &building;
    group.building_materials = building_materials;
    return export_groups_to_gltf(gltf_path, {group}, project, options);
}

ExportResult export_city_to_gltf(
    const std::filesystem::path& gltf_path,
    const MapScene& scene,
    const std::vector<BuildingRenderBuffer>& placement_buffers,
    const std::vector<const SbbBuilding*>& placement_buildings,
    const BuildingRenderBuffer& piece_buffer,
    const EditorProject& project,
    const ExportOptions& options) {
    std::vector<ExportMeshGroup> groups;

    // Recenter the whole city on the origin: subtract the bounds center (X/Z; keep
    // Y so the ground stays at its height) from every group's world vertices.
    const Vec3 recenter{
        (scene.bounds.min_x + scene.bounds.max_x) * 0.5F,
        0.0F,
        (scene.bounds.min_z + scene.bounds.max_z) * 0.5F,
    };

    // Terrain + road/curb tile geometry export as untransformed world-space groups.
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
    // City piece/portal geometry (districts placed with the Piece tool). The live
    // view draws this at a +0.25 Y lift, so subtract -0.25 in world_offset.y.
    if (options.include_pieces && !piece_buffer.triangles.empty()) {
        ExportMeshGroup pieces;
        pieces.name = "pieces";
        pieces.buffer = &piece_buffer;
        pieces.world_offset = {recenter.x, -0.25F, recenter.z};
        groups.push_back(pieces);
    }

    // Each placed building, transformed into world space by its placement basis.
    if (options.include_buildings) {
        for (std::size_t i = 0; i < scene.buildings.size() && i < placement_buffers.size(); ++i) {
            const auto& placement = scene.buildings[i];
            if (placement.source == nullptr || placement_buffers[i].triangles.empty()) {
                continue;
            }
            ExportMeshGroup group;
            group.name = "building_" + std::to_string(i);
            group.buffer = &placement_buffers[i];
            group.basis = native_city_building_rotation_basis(static_cast<std::int32_t>(placement.rotation));
            group.origin = native_city_building_render_origin(*placement.source, scene.unit_size);
            group.world_offset = recenter;
            group.transformed = true;
            if (i < placement_buildings.size() && placement_buildings[i] != nullptr) {
                group.building_materials = &placement_buildings[i]->materials;
            }
            groups.push_back(group);
        }
    }

    return export_groups_to_gltf(gltf_path, groups, project, options);
}

} // namespace map_editor
