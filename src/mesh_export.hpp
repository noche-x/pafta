#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "scene.hpp"

namespace map_editor {

namespace app_internal {
struct EditorProject;
}
using app_internal::EditorProject;

// What to include in an export. Set by the in-app options dialog; defaults are
// "everything on". Building/block exports only consult include_window_panes.
struct ExportOptions {
    bool include_buildings{true};
    bool include_pieces{true};
    bool include_terrain{true};
    bool include_roads{true};
    bool include_window_panes{true};
};

// One textured, world-space mesh group ready to serialize. Geometry is already in
// raw engine space (Y-up, unit_size baked); the exporter applies the glTF
// handedness/winding fix. texture_name resolves to a project texture (empty = no
// texture for this group).
struct ExportMeshGroup {
    std::string name;
    const BuildingRenderBuffer* buffer{};
    // World transform applied to every buffer vertex before export. Identity for
    // single building/block; per-placement transform for city buildings.
    CityBuildingBasis basis{};
    Vec3 origin{};
    bool transformed{false};
    // Uniform world offset subtracted from every exported vertex (after the basis
    // transform). Used to recenter a city export on the origin.
    Vec3 world_offset{};
    // Building material slots -> texture name. Building buffers leave
    // buffer.material_names empty and resolve triangle.material via the building's
    // materials list (slot & 0xff). When set, the exporter uses this; otherwise it
    // falls back to buffer.material_names (terrain / city-tile buffers populate it).
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

// Rough triangle count for a city, used to warn before a large export.
std::size_t estimate_city_export_triangles(
    const SbcMap& map,
    const std::vector<const SbbBuilding*>& placement_buildings,
    const std::vector<NamedBlockMesh>& block_meshes,
    const std::vector<NamedBuildblockTemplate>& buildblock_templates);

// Writes <out_path>.gltf + .bin + referenced .png textures (copied, re-flipped to
// upright). gltf_path is the chosen .gltf file path from the save dialog.
ExportResult export_groups_to_gltf(
    const std::filesystem::path& gltf_path,
    const std::vector<ExportMeshGroup>& groups,
    const EditorProject& project,
    const ExportOptions& options = {});

// Convenience builders for the three view scopes. Each gathers the right
// buffer(s) and calls export_groups_to_gltf.
ExportResult export_building_to_gltf(
    const std::filesystem::path& gltf_path,
    const BuildingRenderBuffer& building,
    const std::string& name,
    const EditorProject& project,
    const std::vector<std::string>* building_materials = nullptr,
    const ExportOptions& options = {});

// Converts a single block mesh's faces into a render buffer (positions, UVs, and
// per-face material slots resolved to names) for export.
BuildingRenderBuffer build_block_mesh_render_buffer(const SblBlockMesh& mesh, float unit_size = 4.0F);

ExportResult export_block_to_gltf(
    const std::filesystem::path& gltf_path,
    const SblBlockMesh& mesh,
    const std::string& name,
    const EditorProject& project);

ExportResult export_city_to_gltf(
    const std::filesystem::path& gltf_path,
    const MapScene& scene,
    const std::vector<BuildingRenderBuffer>& placement_buffers,
    const std::vector<const SbbBuilding*>& placement_buildings,
    const BuildingRenderBuffer& piece_buffer,
    const EditorProject& project,
    const ExportOptions& options = {});

} // namespace map_editor
