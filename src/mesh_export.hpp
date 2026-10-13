// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "scene.hpp"

namespace pafta {

namespace app_internal {
struct EditorProject;
}
using app_internal::EditorProject;

// What to include in an export. The options popup sets it, and everything is on by
// default. Building and block exports only look at include_window_panes.
struct ExportOptions {
    bool include_buildings{true};
    bool include_pieces{true};
    bool include_terrain{true};
    bool include_roads{true};
    bool include_window_panes{true};
};

// One textured world-space mesh group, ready to write out. The geometry is already
// in the game's raw space (Y up, unit_size applied). Exporting keeps coordinates and
// winding as they are; material slots are turned into project texture names.
struct ExportMeshGroup {
    std::string name;
    const BuildingRenderBuffer* buffer{};
    // The world transform applied to every vertex in the buffer before export. Identity
    // for a single building or block; the placement's transform for city buildings.
    CityBuildingBasis basis{};
    Vec3 origin{};
    bool transformed{false};
    // An offset subtracted from every exported vertex (after the basis transform). Used
    // to centre a city export on the origin.
    Vec3 world_offset{};
    // Building material slots -> texture name. Building buffers leave
    // buffer.material_names empty and look up triangle.material in the building's
    // material list (slot & 0xff). When this is set, the exporter uses it; otherwise it
    // uses buffer.material_names (terrain and city tile buffers fill that in).
    const std::vector<std::string>* building_materials{};
};

struct ExportStats {
    std::size_t vertex_count{};
    std::size_t triangle_count{};
    std::size_t material_count{};
    std::size_t texture_count{};
};

struct ExportResult {
    bool ok{false};
    std::string message;
    ExportStats stats;
};

// A glTF export in three steps, so the slow parts can run off the main thread:
// build_gltf_export (CPU only, any thread), write_next_gltf_texture (GPU readback,
// GL thread, one texture per call), and write_gltf_export (file writing, any
// thread).
struct GltfExport;
struct GltfExportDeleter {
    void operator()(GltfExport* job) const;
};
using GltfExportPtr = std::unique_ptr<GltfExport, GltfExportDeleter>;
// Null, with result.message set, when there is nothing to export.
GltfExportPtr build_gltf_export(
    const std::filesystem::path& gltf_path,
    const std::vector<ExportMeshGroup>& groups,
    const ExportOptions& options,
    ExportResult& result);
std::size_t gltf_export_texture_count(const GltfExport& job);
std::size_t gltf_export_textures_done(const GltfExport& job);
// Writes the next texture PNG; false once every texture is done.
bool write_next_gltf_texture(GltfExport& job, const EditorProject& project);
ExportResult write_gltf_export(GltfExport& job);

// Counts the building triangles of city placements whose building was found.
std::size_t estimate_city_export_triangles(
    const SbcMap& map,
    const std::vector<const SbbBuilding*>& placement_buildings,
    const std::vector<NamedBlockMesh>& block_meshes,
    const std::vector<NamedBuildblockTemplate>& buildblock_templates);

// Writes <out_path>.gltf, .bin, and the .png textures they use (copied, and flipped
// upright again). Call on the GL thread when the project has loaded textures. It
// waits for the PNG encoders to finish before writing the glTF files.
ExportResult export_groups_to_gltf(
    const std::filesystem::path& gltf_path,
    const std::vector<ExportMeshGroup>& groups,
    const EditorProject& project,
    const ExportOptions& options = {});

// Turns a single block mesh's faces into a render buffer (positions, UVs, and each
// face's material slot turned into a name) for export.
BuildingRenderBuffer build_block_mesh_render_buffer(const SblBlockMesh& mesh, float unit_size = 4.0F);

// The city's export groups: terrain, roads, pieces, and each placed building
// (placement_buffers[i] is placement i's building mesh, or null when it wasn't found).
std::vector<ExportMeshGroup> city_export_groups(
    const MapScene& scene,
    const std::vector<const BuildingRenderBuffer*>& placement_buffers,
    const std::vector<const SbbBuilding*>& placement_buildings,
    const BuildingRenderBuffer& piece_buffer,
    const ExportOptions& options = {});

} // namespace pafta
