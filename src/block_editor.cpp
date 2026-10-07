// Block (.sbl block mesh) editor: the Blocks tab's properties, viewport
// selection and keyboard edits. Formats are documented in formats.hpp and
// docs/file_formats; a block is a cell box with optional generated sides,
// Bezier patches, and extra 8-corner boxes (BlockSbl_LoadFromFile @e6e50).
#include "app_internal.hpp"

#include <imgui.h>
#include <raymath.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <optional>
#include <string>

namespace map_editor::app_internal {
namespace {

constexpr float kUnit = 4.0F;  // world units per block cell in the viewport

// Box record corners: 0..3 the low ring, 4..7 the ring above them
// (kNativeCubeCorner order); side bits follow the game's face table.
constexpr std::array<const char*, 6> kBoxSideNames{"Bottom (-Y)", "+Z side", "-X side", "-Z side", "+X side", "Top (+Y)"};
// Header side flags, in file order (build_exterior_block_faces).
constexpr std::array<const char*, 6> kExteriorSideNames{"Bottom (-Y)", "Top (+Y)", "+Z side", "-X side", "-Z side", "+X side"};

SblBlockMesh* selected_block(EditorProject& project, EditorUi& ui) {
    if (project.blocks.empty()) return nullptr;
    ui.selected_block = std::min(ui.selected_block, project.blocks.size() - 1U);
    return &project.blocks[ui.selected_block].data;
}

std::array<Vec3f, 4> patch_corners(const SblAuthoredPatch& patch) {
    const auto& p = patch.control_points;
    return {p[0].position, p[3].position, p[15].position, p[12].position};
}

bool patch_is_flat(const SblAuthoredPatch& patch) {
    return patch.column_order == 1U && patch.row_order == 1U && patch.control_points.size() == 16U;
}

// Corner UVs of a patch: control points 0, 3, 15, 12 (the corners in order).
std::array<Vec2f, 4> patch_corner_uvs(const SblAuthoredPatch& patch) {
    const auto& p = patch.control_points;
    if (p.size() != 16) return {{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
    return {{{p[0].u, p[0].v}, {p[3].u, p[3].v}, {p[15].u, p[15].v}, {p[12].u, p[12].v}}};
}

// A flat patch over the quad a-b-c-d (order 1): the 4x4 control grid is the
// bilinear blend of the corners and of their UVs, as linear patches store it.
void set_patch_quad(SblAuthoredPatch& patch, Vec3f a, Vec3f b, Vec3f c, Vec3f d, std::array<Vec2f, 4> uvs) {
    patch.column_order = patch.row_order = 1;
    patch.column_tess = std::max(1U, patch.column_tess);
    patch.row_tess = std::max(1U, patch.row_tess);
    patch.control_points.resize(16);
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            const float u = static_cast<float>(column) / 3.0F, v = static_cast<float>(row) / 3.0F;
            const auto blend = [&](float pa, float pb, float pc, float pd) {
                return (pa * (1.0F - u) + pb * u) * (1.0F - v) + (pd * (1.0F - u) + pc * u) * v;
            };
            auto& point = patch.control_points[static_cast<std::size_t>(row * 4 + column)];
            point.position = {blend(a.x, b.x, c.x, d.x), blend(a.y, b.y, c.y, d.y), blend(a.z, b.z, c.z, d.z)};
            point.u = blend(uvs[0].u, uvs[1].u, uvs[2].u, uvs[3].u);
            point.v = blend(uvs[0].v, uvs[1].v, uvs[2].v, uvs[3].v);
        }
    }
}

// Same, keeping the patch's current corner UVs.
void set_patch_quad(SblAuthoredPatch& patch, Vec3f a, Vec3f b, Vec3f c, Vec3f d) {
    set_patch_quad(patch, a, b, c, d, patch_corner_uvs(patch));
}

float distance(Vec3f a, Vec3f b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// Stored-UV tools: every control point's UV through one mapping.
void map_patch_uvs(SblAuthoredPatch& patch, const auto& mapping) {
    for (auto& point : patch.control_points) {
        const auto uv = mapping(Vec2f{point.u, point.v});
        point.u = uv.u;
        point.v = uv.v;
    }
}

void draw_patch_uv_tools(SblAuthoredPatch& patch, bool& changed) {
    ImGui::SeparatorText("UVs (stored)");
    if (patch_is_flat(patch)) {
        auto corners = patch_corners(patch);
        auto uvs = patch_corner_uvs(patch);
        static constexpr std::array<const char*, 4> names{"Corner 1 UV", "Corner 2 UV", "Corner 3 UV", "Corner 4 UV"};
        bool moved = false;
        for (std::size_t i = 0; i < uvs.size(); ++i) {
            field_label(names[i]);
            ImGui::PushID(static_cast<int>(i));
            std::array<float, 2> uv{uvs[i].u, uvs[i].v};
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragFloat2("##uv", uv.data(), 0.01F, -64.0F, 64.0F, "%.3g")) {
                uvs[i] = {uv[0], uv[1]};
                moved = true;
            }
            ImGui::PopID();
        }
        if (ImGui::Button("Fit 0-1")) {
            uvs = {{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
            moved = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stretch the texture once over the whole surface.");
        ImGui::SameLine();
        if (ImGui::Button("One per cell")) {
            // As many repeats as the edges are long in cells, like projected walls.
            const auto across = distance(corners[0], corners[1]), up = distance(corners[0], corners[3]);
            uvs = {{{0, up}, {across, up}, {across, 0}, {0, 0}}};
            moved = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Repeat the texture once per cell, so it matches the walls next to it.");
        if (moved) {
            set_patch_quad(patch, corners[0], corners[1], corners[2], corners[3], uvs);
            changed = true;
        }
    }
    if (ImGui::Button("Rotate 90")) {
        map_patch_uvs(patch, [](Vec2f uv) { return Vec2f{uv.v, -uv.u}; });
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Flip U")) {
        map_patch_uvs(patch, [](Vec2f uv) { return Vec2f{-uv.u, uv.v}; });
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Flip V")) {
        map_patch_uvs(patch, [](Vec2f uv) { return Vec2f{uv.u, -uv.v}; });
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("x2")) {
        map_patch_uvs(patch, [](Vec2f uv) { return Vec2f{uv.u * 2.0F, uv.v * 2.0F}; });
        changed = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Repeat the texture twice as often.");
    ImGui::SameLine();
    if (ImGui::Button("/2")) {
        map_patch_uvs(patch, [](Vec2f uv) { return Vec2f{uv.u * 0.5F, uv.v * 0.5F}; });
        changed = true;
    }
    static std::array<float, 2> offset{};
    field_label("Shift by");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 60.0F);
    ImGui::DragFloat2("##uv_shift", offset.data(), 0.01F, -8.0F, 8.0F, "%.3g");
    ImGui::SameLine();
    if (ImGui::Button("Apply##shift")) {
        map_patch_uvs(patch, [](Vec2f uv) { return Vec2f{uv.u + offset[0], uv.v + offset[1]}; });
        changed = true;
    }
}

void set_box_bounds(SblBlockLight& box, Vec3f lo, Vec3f hi) {
    box.points = {{
        {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z},
        {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z},
    }};
}

std::pair<Vec3f, Vec3f> box_bounds(const SblBlockLight& box) {
    Vec3f lo{FLT_MAX, FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const auto& p : box.points) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    return {lo, hi};
}

void translate_element(SblBlockMesh& block, BlockElementKind kind, std::size_t index, Vec3f delta) {
    const auto move = [&](Vec3f& p) { p = {p.x + delta.x, p.y + delta.y, p.z + delta.z}; };
    if (kind == BlockElementKind::Patch && index < block.authored_patches.size()) {
        for (auto& point : block.authored_patches[index].control_points) move(point.position);
    } else if (kind == BlockElementKind::Box && index < block.lights.size()) {
        for (auto& point : block.lights[index].points) move(point);
    }
}

std::string unique_block_name(const EditorProject& project, std::string base) {
    const auto taken = [&](const std::string& name) {
        return std::ranges::any_of(project.blocks, [&](const auto& asset) { return lower_name(asset.name) == lower_name(name); });
    };
    if (!taken(base)) return base;
    for (int i = 2;; ++i) {
        const auto name = base + std::to_string(i);
        if (!taken(name)) return name;
    }
}

// Thumbnails are indexed by block; any add, remove, or edit makes them stale.
void reset_block_thumbnails(EditorProject& project) {
    for (auto& target : project.block_thumbnails) {
        if (target.id != 0U) UnloadRenderTexture(target);
    }
    project.block_thumbnails.clear();
    project.block_thumbnail_ready.clear();
}

void add_block(EditorProject& project, EditorUi& ui, std::string name, SblBlockMesh block) {
    refresh_sbl_block_faces(block);
    project.block_bytes.push_back(write_sbl_block(block));
    project.blocks.push_back({project.root / "block" / (name + ".sbl"), std::move(name), std::move(block), true, true});
    ui.selected_block = project.blocks.size() - 1U;
    ui.block_element = BlockElementKind::None;
    reset_block_thumbnails(project);
    mark_block_changed(project, ui, project.blocks.back().data);
}

// How many buildings and city pieces use the block by name.
std::size_t block_users(const EditorProject& project, const std::string& name) {
    std::size_t users = 0;
    for (const auto& building : project.buildings) {
        users += static_cast<std::size_t>(std::ranges::count_if(building.data.piece_names,
            [&](const std::string& piece) { return lower_name(piece) == lower_name(name); }));
    }
    if (project.city) {
        users += static_cast<std::size_t>(std::ranges::count_if(project.city->data.city_blocks,
            [&](const SbcCityBlock& block) { return lower_name(block.name) == lower_name(name); }));
    }
    return users;
}

void rename_block(EditorProject& project, EditorUi& ui, const std::string& name, std::string& status) {
    auto& asset = project.blocks[ui.selected_block];
    if (name == asset.name) return;
    if (name.empty() || name.size() > 51 || name.find_first_of("/\\:") != std::string::npos) {
        status = "A block name needs 1-51 characters and no / \\ or :";
        return;
    }
    if (std::ranges::any_of(project.blocks, [&](const auto& other) { return &other != &asset && lower_name(other.name) == lower_name(name); })) {
        status = "Another block is already named " + name;
        return;
    }
    // Buildings name their pieces, and city pieces name their block.
    const auto old_key = lower_name(asset.name);
    for (std::size_t i = 0; i < project.buildings.size(); ++i) {
        bool changed = false;
        for (auto& piece : project.buildings[i].data.piece_names) {
            if (lower_name(piece) == old_key) {
                piece = name;
                changed = true;
            }
        }
        if (changed) {
            project.buildings[i].dirty = true;
            ui.building_dirty = true;
        }
    }
    const auto rename_in = [&](SbcMap& map) {
        for (auto& block : map.city_blocks) if (lower_name(block.name) == old_key) block.name = name;
    };
    if (project.city) {
        rename_in(project.city->data);
        if (project.city_baseline) rename_in(*project.city_baseline);
        for (auto& [label, map] : project.undo_stack) rename_in(map);
        for (auto& [label, map] : project.redo_stack) rename_in(map);
        ui.city_dirty = true;
    }
    for (auto* stacks : {&project.asset_undo_stacks, &project.asset_redo_stacks}) {
        for (auto& snapshot : (*stacks)[2]) if (snapshot.index == ui.selected_block && snapshot.name == asset.name) snapshot.name = name;
    }
    if (auto& baseline = project.asset_baselines[2]; baseline && baseline->index == ui.selected_block) baseline->name = name;
    status = "renamed " + asset.name + " to " + name;
    asset.name = name;
    asset.dirty = true;
    reset_block_thumbnails(project);
    mark_block_changed(project, ui, asset.data);
}

void delete_block(EditorProject& project, EditorUi& ui, std::string& status) {
    const auto& name = project.blocks[ui.selected_block].name;
    if (const auto users = block_users(project, name); users > 0) {
        status = name + " is used " + std::to_string(users) + (users == 1 ? " time" : " times") + "; remove those uses first";
        return;
    }
    status = "deleted block " + name;
    project.blocks.erase(project.blocks.begin() + static_cast<std::ptrdiff_t>(ui.selected_block));
    project.block_bytes.erase(project.block_bytes.begin() + static_cast<std::ptrdiff_t>(ui.selected_block));
    ui.selected_block = project.blocks.empty() ? 0U : std::min(ui.selected_block, project.blocks.size() - 1U);
    ui.block_element = BlockElementKind::None;
    ui.block_dirty = true;
    reset_block_thumbnails(project);
    project.asset_baselines[2].reset();
    project.render_buffers_dirty = true;
    invalidate_render_input_cache(project);
    invalidate_portal_render_models(project);
}

bool drag_vec3(const char* id, Vec3f& value, float step = 0.0625F) {
    std::array<float, 3> v{value.x, value.y, value.z};
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!ImGui::DragFloat3(id, v.data(), step * 0.25F, -64.0F, 128.0F, "%.4g")) return false;
    value = {v[0], v[1], v[2]};
    return true;
}

// Texture slot of a face: 0..7 pick one of the eight textures of the building
// cell that places the block (material_for_piece_face).
bool slot_combo(const char* id, std::uint32_t& material) {
    int slot = static_cast<int>(material & 0xffffU);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!ImGui::SliderInt(id, &slot, 0, 7, "slot %d")) return false;
    material = (material & ~0xffffU) | static_cast<std::uint32_t>(slot);
    return true;
}

void draw_patch_properties(SblBlockMesh& block, SblAuthoredPatch& patch, bool& changed) {
    field_label("Texture");
    changed |= slot_combo("##patch_slot", patch.normal_flag_a);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Which of the 8 textures of the building cell that places this block.");
    field_label("Texture fit");
    int uv_mode = patch.normal_flag_b != 0U ? 0 : 1;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##uv_mode", &uv_mode, "Projected from facing\0Stored UVs\0")) {
        patch.normal_flag_b = uv_mode == 0 ? 1U : 0U;
        // Stored UVs: clear the projection bits the game would otherwise apply.
        if (uv_mode == 1) patch.normal_flag_a &= 0xffffU;
        changed = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Projected: the game lines the texture up with the world like a wall (recommended).\n"
                          "Stored: uses each control point's u, v.");
    }
    if (patch.normal_flag_b == 0U && (patch.normal_flag_a & 0xf0000U) == 0U) {
        draw_patch_uv_tools(patch, changed);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("UVs are projected from the facing: one texture repeat per cell, lined up with the walls. "
                           "Choose Stored UVs to set them yourself.");
        ImGui::PopStyleColor();
    }
    if (patch_is_flat(patch)) {
        ImGui::SeparatorText("Corners (cells)");
        auto corners = patch_corners(patch);
        static constexpr std::array<const char*, 4> names{"Corner 1", "Corner 2", "Corner 3", "Corner 4"};
        bool moved = false;
        for (std::size_t i = 0; i < corners.size(); ++i) {
            field_label(names[i]);
            ImGui::PushID(static_cast<int>(i));
            moved |= drag_vec3("##corner", corners[i]);
            ImGui::PopID();
        }
        if (moved) {
            set_patch_quad(patch, corners[0], corners[1], corners[2], corners[3]);
            changed = true;
        }
        if (ImGui::Button("Fill block side...")) ImGui::OpenPopup("##fill_side");
        if (ImGui::BeginPopup("##fill_side")) {
            const float w = static_cast<float>(block.width), h = static_cast<float>(block.height), d = static_cast<float>(block.depth);
            const auto fill = [&](const char* label, Vec3f a, Vec3f b, Vec3f c, Vec3f e) {
                if (ImGui::Selectable(label)) {
                    set_patch_quad(patch, a, b, c, e);
                    changed = true;
                }
            };
            fill("-Z side", {0, 0, 0}, {0, h, 0}, {w, h, 0}, {w, 0, 0});
            fill("+Z side", {w, 0, d}, {w, h, d}, {0, h, d}, {0, 0, d});
            fill("-X side", {0, 0, d}, {0, h, d}, {0, h, 0}, {0, 0, 0});
            fill("+X side", {w, 0, 0}, {w, h, 0}, {w, h, d}, {w, 0, d});
            fill("Top", {0, h, 0}, {0, h, d}, {w, h, d}, {w, h, 0});
            fill("Bottom", {0, 0, 0}, {w, 0, 0}, {w, 0, d}, {0, 0, d});
            fill("Ramp (rises toward +Z)", {0, 0, 0}, {0, h, d}, {w, h, d}, {w, 0, 0});
            ImGui::EndPopup();
        }
    }
    ImGui::SeparatorText("Curve");
    int orders[2]{static_cast<int>(patch.column_order), static_cast<int>(patch.row_order)};
    field_label("Degree (u, v)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt2("##orders", orders, 1, 3)) {
        patch.column_order = static_cast<std::uint32_t>(orders[0]);
        patch.row_order = static_cast<std::uint32_t>(orders[1]);
        changed = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("1 flat, 2 curved through the middle points, 3 a full cubic curve.");
    int tess[2]{static_cast<int>(patch.column_tess), static_cast<int>(patch.row_tess)};
    field_label("Pieces (u, v)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt2("##tess", tess, 1, 32)) {
        patch.column_tess = static_cast<std::uint32_t>(tess[0]);
        patch.row_tess = static_cast<std::uint32_t>(tess[1]);
        changed = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How many quads the game cuts the surface into. More = smoother curves, more triangles.");
    if (ImGui::TreeNode("Control points (4 x 4)")) {
        for (std::size_t i = 0; i < patch.control_points.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            auto& point = patch.control_points[i];
            ImGui::Text("%zu,%zu", i / 4U, i % 4U);
            ImGui::SameLine(48.0F);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.62F);
            std::array<float, 3> p{point.position.x, point.position.y, point.position.z};
            if (ImGui::DragFloat3("##p", p.data(), 0.01F, -64.0F, 128.0F, "%.3g")) {
                point.position = {p[0], p[1], p[2]};
                changed = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-FLT_MIN);
            std::array<float, 2> uv{point.u, point.v};
            if (ImGui::DragFloat2("##uv", uv.data(), 0.01F, -64.0F, 64.0F, "%.3g")) {
                point.u = uv[0];
                point.v = uv[1];
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
}

void draw_box_properties(SblBlockLight& box, bool& changed) {
    field_label("Texture");
    changed |= slot_combo("##box_slot", box.scalars[0]);
    ImGui::TextDisabled("Box textures are always projected from each side's facing.");
    auto [lo, hi] = box_bounds(box);
    ImGui::SeparatorText("Bounds (cells)");
    field_label("From");
    bool resized = drag_vec3("##box_lo", lo);
    field_label("To");
    resized |= drag_vec3("##box_hi", hi);
    if (resized) {
        set_box_bounds(box, lo, hi);
        changed = true;
    }
    ImGui::SeparatorText("Sides");
    for (std::size_t bit = 0; bit < kBoxSideNames.size(); ++bit) {
        bool on = (box.final_value & (1U << bit)) != 0U;
        if (ImGui::Checkbox(kBoxSideNames[bit], &on)) {
            box.final_value = on ? box.final_value | (1U << bit) : box.final_value & ~(1U << bit);
            changed = true;
        }
        if (bit % 2U == 0U) ImGui::SameLine(160.0F);
    }
    if (ImGui::TreeNode("Corners (8)")) {
        for (std::size_t i = 0; i < box.points.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("%zu", i);
            ImGui::SameLine(32.0F);
            changed |= drag_vec3("##c", box.points[i], 0.01F);
            ImGui::PopID();
        }
        ImGui::TextDisabled("0-3 the bottom ring, 4-7 the ring above them.");
        ImGui::TreePop();
    }
}

} // namespace

void draw_block_inspector(EditorProject& project, EditorUi& ui, std::string& status) {
    if (ImGui::Button(ICON_FA_PLUS " New block")) {
        SblBlockMesh block{};
        block.width = block.height = block.depth = 1;
        block.exterior_flags.fill(1);
        add_block(project, ui, unique_block_name(project, "new_block"), std::move(block));
        status = "created " + project.blocks.back().name;
    }
    auto* block = selected_block(project, ui);
    if (block == nullptr) {
        ImGui::TextWrapped("No blocks yet. A block is a mesh building cells and city pieces place; create one above.");
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_COPY " Duplicate")) {
        const auto copy = *block;
        add_block(project, ui, unique_block_name(project, project.blocks[ui.selected_block].name + "_copy"), copy);
        status = "duplicated block";
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH " Delete")) {
        delete_block(project, ui, status);
        return;
    }

    auto& asset = project.blocks[ui.selected_block];
    bool changed = false;
    std::array<char, 52> name{};
    std::copy_n(asset.name.data(), std::min(asset.name.size(), name.size() - 1), name.data());
    field_label("Name");
    if (ImGui::InputText("##block_name", name.data(), name.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
        rename_block(project, ui, name.data(), status);
        block = &project.blocks[ui.selected_block].data;
    }
    if (ImGui::IsItemActive()) ImGui::TextDisabled("Press Enter to rename. Buildings and pieces using it follow.");
    ImGui::TextDisabled("Used %zu times", block_users(project, asset.name));

    ImGui::SeparatorText("Box");
    int size[3]{static_cast<int>(block->width), static_cast<int>(block->height), static_cast<int>(block->depth)};
    field_label("Size (cells)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt3("##block_size", size, 1, 16)) {
        block->width = static_cast<std::uint32_t>(size[0]);
        block->height = static_cast<std::uint32_t>(size[1]);
        block->depth = static_cast<std::uint32_t>(size[2]);
        changed = true;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Generated sides: one quad per cell, built by the game from the size.");
    ImGui::PopStyleColor();
    for (std::size_t side = 0; side < kExteriorSideNames.size(); ++side) {
        bool on = block->exterior_flags[side] != 0U;
        if (ImGui::Checkbox(kExteriorSideNames[side], &on)) {
            block->exterior_flags[side] = on ? 1U : 0U;
            changed = true;
        }
        if (side % 2U == 0U) ImGui::SameLine(160.0F);
    }

    ImGui::SeparatorText("Surfaces");
    if (ImGui::Button(ICON_FA_PLUS " Patch")) {
        SblAuthoredPatch patch{};
        patch.column_tess = patch.row_tess = 1;
        patch.normal_flag_b = 1;
        const float w = static_cast<float>(block->width), h = static_cast<float>(block->height);
        set_patch_quad(patch, {0, 0, 0}, {0, h, 0}, {w, h, 0}, {w, 0, 0});
        block->authored_patches.push_back(std::move(patch));
        ui.block_element = BlockElementKind::Patch;
        ui.block_element_index = block->authored_patches.size() - 1U;
        changed = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A free surface: flat quad by default, curved with a higher degree.");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PLUS " Box")) {
        SblBlockLight box{};
        box.final_value = 63U;
        set_box_bounds(box, {0.25F, 0.0F, 0.25F}, {0.75F, 0.5F, 0.75F});
        block->lights.push_back(box);
        ui.block_element = BlockElementKind::Box;
        ui.block_element_index = block->lights.size() - 1U;
        changed = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("An extra box from 8 corners, each side a quad; good for tables, posts, steps.");

    if (ImGui::BeginListBox("##block_elements", ImVec2(-FLT_MIN, 6.0F * ImGui::GetTextLineHeightWithSpacing()))) {
        for (std::size_t i = 0; i < block->authored_patches.size(); ++i) {
            const auto& patch = block->authored_patches[i];
            const auto label = "Patch " + std::to_string(i + 1U) + (patch_is_flat(patch) ? "  flat" : "  curved") +
                "  slot " + std::to_string(patch.normal_flag_a & 0xffffU);
            if (ImGui::Selectable((label + "##p" + std::to_string(i)).c_str(), ui.block_element == BlockElementKind::Patch && ui.block_element_index == i)) {
                ui.block_element = BlockElementKind::Patch;
                ui.block_element_index = i;
            }
        }
        for (std::size_t i = 0; i < block->lights.size(); ++i) {
            const auto label = "Box " + std::to_string(i + 1U) + "  slot " + std::to_string(block->lights[i].scalars[0] & 0xffffU);
            if (ImGui::Selectable((label + "##b" + std::to_string(i)).c_str(), ui.block_element == BlockElementKind::Box && ui.block_element_index == i)) {
                ui.block_element = BlockElementKind::Box;
                ui.block_element_index = i;
            }
        }
        ImGui::EndListBox();
    }

    const bool patch_selected = ui.block_element == BlockElementKind::Patch && ui.block_element_index < block->authored_patches.size();
    const bool box_selected = ui.block_element == BlockElementKind::Box && ui.block_element_index < block->lights.size();
    if (patch_selected || box_selected) {
        if (ImGui::Button(ICON_FA_COPY " Copy")) {
            duplicate_block_element(project, ui);
            block = &project.blocks[ui.selected_block].data;
        }
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_TRASH " Remove")) {
            delete_block_element(project, ui);
            return;
        }
        if (patch_selected) {
            ImGui::SameLine();
            if (ImGui::Button("Flip")) {
                auto& points = block->authored_patches[ui.block_element_index].control_points;
                for (std::size_t row = 0; row < 4; ++row) {
                    std::swap(points[row * 4], points[row * 4 + 3]);
                    std::swap(points[row * 4 + 1], points[row * 4 + 2]);
                }
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Turn the surface to face the other way.");
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Arrow keys and PgUp/PgDn move it 1/8 cell; with Shift, a whole cell.");
        ImGui::PopStyleColor();
        if (patch_selected) draw_patch_properties(*block, block->authored_patches[ui.block_element_index], changed);
        else draw_box_properties(block->lights[ui.block_element_index], changed);
    } else {
        ImGui::TextDisabled("Click a surface in the view, or pick one above.");
    }
    if (changed) mark_block_changed(project, ui, *block);
}

void duplicate_block_element(EditorProject& project, EditorUi& ui) {
    auto* block = selected_block(project, ui);
    if (block == nullptr) return;
    if (ui.block_element == BlockElementKind::Patch && ui.block_element_index < block->authored_patches.size()) {
        block->authored_patches.push_back(block->authored_patches[ui.block_element_index]);
        ui.block_element_index = block->authored_patches.size() - 1U;
    } else if (ui.block_element == BlockElementKind::Box && ui.block_element_index < block->lights.size()) {
        block->lights.push_back(block->lights[ui.block_element_index]);
        ui.block_element_index = block->lights.size() - 1U;
    } else {
        return;
    }
    translate_element(*block, ui.block_element, ui.block_element_index, {0.125F, 0.0F, 0.125F});
    mark_block_changed(project, ui, *block);
}

void delete_block_element(EditorProject& project, EditorUi& ui) {
    auto* block = selected_block(project, ui);
    if (block == nullptr) return;
    if (ui.block_element == BlockElementKind::Patch && ui.block_element_index < block->authored_patches.size()) {
        block->authored_patches.erase(block->authored_patches.begin() + static_cast<std::ptrdiff_t>(ui.block_element_index));
    } else if (ui.block_element == BlockElementKind::Box && ui.block_element_index < block->lights.size()) {
        block->lights.erase(block->lights.begin() + static_cast<std::ptrdiff_t>(ui.block_element_index));
    } else {
        return;
    }
    ui.block_element = BlockElementKind::None;
    mark_block_changed(project, ui, *block);
}

void nudge_block_element(EditorProject& project, EditorUi& ui, Vec3f delta) {
    auto* block = selected_block(project, ui);
    if (block == nullptr || ui.block_element == BlockElementKind::None) return;
    translate_element(*block, ui.block_element, ui.block_element_index, delta);
    mark_block_changed(project, ui, *block);
}

void draw_block_editor_view(EditorProject& project, const EditorUi& ui) {
    if (project.blocks.empty()) return;
    const auto& block = project.blocks[std::min(ui.selected_block, project.blocks.size() - 1U)].data;
    draw_mesh_faces(block, kUnit, {98, 148, 204, 220});
    // The block's cell box and its cell grid on the floor.
    const Vector3 size{static_cast<float>(block.width) * kUnit, static_cast<float>(block.height) * kUnit, static_cast<float>(block.depth) * kUnit};
    DrawCubeWiresV(Vector3Scale(size, 0.5F), size, Color{235, 235, 240, 140});
    for (std::uint32_t x = 1; x < block.width; ++x) {
        DrawLine3D({static_cast<float>(x) * kUnit, 0.0F, 0.0F}, {static_cast<float>(x) * kUnit, 0.0F, size.z}, Color{235, 235, 240, 70});
    }
    for (std::uint32_t z = 1; z < block.depth; ++z) {
        DrawLine3D({0.0F, 0.0F, static_cast<float>(z) * kUnit}, {size.x, 0.0F, static_cast<float>(z) * kUnit}, Color{235, 235, 240, 70});
    }
    // The selected surface over its faces, plus its outline.
    std::vector<SblBlockFace> faces;
    if (ui.block_element == BlockElementKind::Patch && ui.block_element_index < block.authored_patches.size()) {
        faces = sbl_patch_faces(block.authored_patches[ui.block_element_index]);
    } else if (ui.block_element == BlockElementKind::Box && ui.block_element_index < block.lights.size()) {
        faces = sbl_box_faces(block.lights[ui.block_element_index]);
    }
    const auto world = [](Vec3f p) { return Vector3{p.x * kUnit, p.y * kUnit, p.z * kUnit}; };
    for (const auto& face : faces) {
        if (face.vertices.size() < 4) continue;
        const auto a = world(face.vertices[0]), b = world(face.vertices[1]), c = world(face.vertices[2]), d = world(face.vertices[3]);
        const Color fill{255, 214, 70, 200};
        DrawTriangle3D(a, b, c, fill); DrawTriangle3D(c, b, a, fill);
        DrawTriangle3D(a, c, d, fill); DrawTriangle3D(d, c, a, fill);
        const Color edge{255, 240, 160, 255};
        DrawLine3D(a, b, edge); DrawLine3D(b, c, edge); DrawLine3D(c, d, edge); DrawLine3D(d, a, edge);
    }
}

void frame_block_camera(const EditorProject& project, const EditorUi& ui, Camera3D& camera) {
    if (project.blocks.empty()) return;
    // The block from the front-right, a little above.
    const auto& block = project.blocks[std::min(ui.selected_block, project.blocks.size() - 1U)].data;
    const Vector3 size{static_cast<float>(block.width) * kUnit, static_cast<float>(block.height) * kUnit, static_cast<float>(block.depth) * kUnit};
    const auto reach = std::max({size.x, size.y, size.z}) * 1.6F + 4.0F;
    camera.target = Vector3Scale(size, 0.5F);
    camera.position = Vector3Add(camera.target, {reach * 0.8F, reach * 0.6F, -reach});
}

void pick_block_element(EditorProject& project, EditorUi& ui, Ray ray) {
    if (project.blocks.empty()) return;
    const auto& block = project.blocks[std::min(ui.selected_block, project.blocks.size() - 1U)].data;
    float nearest = FLT_MAX;
    BlockElementKind kind = BlockElementKind::None;
    std::size_t index = 0;
    const auto test = [&](const std::vector<SblBlockFace>& faces, BlockElementKind element, std::size_t i) {
        for (const auto& face : faces) {
            if (face.vertices.size() < 4) continue;
            std::array<Vector3, 4> q{};
            for (std::size_t k = 0; k < 4; ++k) q[k] = {face.vertices[k].x * kUnit, face.vertices[k].y * kUnit, face.vertices[k].z * kUnit};
            for (const auto& [x, y, z] : {std::array{0, 1, 2}, std::array{0, 2, 3}}) {
                for (const auto& hit : {GetRayCollisionTriangle(ray, q[x], q[y], q[z]), GetRayCollisionTriangle(ray, q[z], q[y], q[x])}) {
                    if (hit.hit && hit.distance < nearest) {
                        nearest = hit.distance;
                        kind = element;
                        index = i;
                    }
                }
            }
        }
    };
    for (std::size_t i = 0; i < block.authored_patches.size(); ++i) test(sbl_patch_faces(block.authored_patches[i]), BlockElementKind::Patch, i);
    for (std::size_t i = 0; i < block.lights.size(); ++i) test(sbl_box_faces(block.lights[i]), BlockElementKind::Box, i);
    ui.block_element = kind;
    ui.block_element_index = index;
}

} // namespace map_editor::app_internal
