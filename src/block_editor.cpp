// SPDX-License-Identifier: GPL-2.0-only
// Block (.sbl block mesh) editor: the Blocks tab's properties, viewport selection,
// and keyboard edits. The formats are described in formats.hpp and
// docs/file_formats. A block is a box of cells with optional generated sides, Bezier
// patches, and extra 8-corner boxes (BlockSbl_LoadFromFile @e6e50).
#include "app_internal.hpp"

#include <imgui.h>
#include <raymath.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace pafta::app_internal {
namespace {

constexpr float kUnit = 4.0F;  // world units per block cell in the viewport

// Box record corners: 0..3 are the bottom ring and 4..7 the ring above it
// (kNativeCubeCorner order). Side bits follow the game's face table.
constexpr std::array<const char*, 6> kBoxSideNames{"Bottom (-Y)", "+Z side", "-X side", "-Z side", "+X side", "Top (+Y)"};
// Header side flags, in file order (build_exterior_block_faces).
constexpr std::array<const char*, 6> kExteriorSideNames{"Bottom (-Y)", "Top (+Y)", "+Z side", "-X side", "-Z side", "+X side"};

std::vector<BlockVertexRef> element_vertices(const SblBlockMesh& block, BlockElementKind kind, std::size_t index);

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

// A patch's corner UVs: control points 0, 3, 15, 12 (the corners, in order).
std::array<Vec2f, 4> patch_corner_uvs(const SblAuthoredPatch& patch) {
    const auto& p = patch.control_points;
    if (p.size() != 16) return {{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
    return {{{p[0].u, p[0].v}, {p[3].u, p[3].v}, {p[15].u, p[15].v}, {p[12].u, p[12].v}}};
}

// A flat patch over the quad a-b-c-d (order 1). The 4x4 control grid is a bilinear
// blend of the corners and their UVs, the way flat patches are stored.
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

// The same, keeping the patch's current corner UVs.
void set_patch_quad(SblAuthoredPatch& patch, Vec3f a, Vec3f b, Vec3f c, Vec3f d) {
    set_patch_quad(patch, a, b, c, d, patch_corner_uvs(patch));
}

float distance(Vec3f a, Vec3f b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// Stored-UV tools: put every control point's UV through one mapping.
void map_patch_uvs(SblAuthoredPatch& patch, const auto& mapping) {
    for (auto& point : patch.control_points) {
        const auto uv = mapping(Vec2f{point.u, point.v});
        point.u = uv.u;
        point.v = uv.v;
    }
}

// The patch's stored UVs drawn over its texture (tiled). Drag a point to move it, or
// empty space to move them all. A flat patch moves its corners; the inner points follow.
bool draw_patch_uv_canvas(SblAuthoredPatch& patch, const Texture2D* texture) {
    if (patch.control_points.size() != 16U) return false;
    auto& points = patch.control_points;
    const bool flat = patch_is_flat(patch);
    static constexpr std::array<std::size_t, 4> kCorners{0, 3, 15, 12};
    const auto editable = [&](std::size_t i) { return !flat || std::ranges::find(kCorners, i) != kCorners.end(); };
    // The view covers the 0-1 square and every point, and holds still during a drag.
    static int dragging = -1;  // a point, -2 for all of them, -1 for none
    static float view_u = 0.0F, view_v = 0.0F, view_span = 1.0F;
    if (dragging == -1) {
        float u0 = 0.0F, v0 = 0.0F, u1 = 1.0F, v1 = 1.0F;
        for (const auto& p : points) {
            u0 = std::min(u0, p.u); u1 = std::max(u1, p.u);
            v0 = std::min(v0, p.v); v1 = std::max(v1, p.v);
        }
        view_span = std::max(u1 - u0, v1 - v0) * 1.2F;
        view_u = (u0 + u1 - view_span) * 0.5F;
        view_v = (v0 + v1 - view_span) * 0.5F;
    }
    const float side = std::max(80.0F, ImGui::GetContentRegionAvail().x);
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##uv_canvas", ImVec2(side, side));
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    const auto at = [&](float u, float v) {
        return ImVec2(origin.x + (u - view_u) / view_span * side, origin.y + (v - view_v) / view_span * side);
    };
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(origin, ImVec2(origin.x + side, origin.y + side), IM_COL32(40, 44, 50, 255));
    if (texture != nullptr) {
        // The texture repeats, so the view's UV range shows it tiled.
        draw->AddImage(ImTextureID(texture->id), origin, ImVec2(origin.x + side, origin.y + side), ImVec2(view_u, view_v),
            ImVec2(view_u + view_span, view_v + view_span), IM_COL32(255, 255, 255, 170));
    }
    draw->AddRect(at(0.0F, 0.0F), at(1.0F, 1.0F), IM_COL32(255, 255, 255, 90));
    for (std::size_t row = 0; row < 4; ++row) {
        for (std::size_t column = 0; column < 4; ++column) {
            const auto& p = points[row * 4 + column];
            if (column < 3) draw->AddLine(at(p.u, p.v), at(points[row * 4 + column + 1].u, points[row * 4 + column + 1].v), IM_COL32(255, 170, 60, 220), 1.5F);
            if (row < 3) draw->AddLine(at(p.u, p.v), at(points[(row + 1) * 4 + column].u, points[(row + 1) * 4 + column].v), IM_COL32(255, 170, 60, 220), 1.5F);
        }
    }
    const auto mouse = ImGui::GetIO().MousePos;
    int hover = -1;
    float best = 8.0F;
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (!editable(i)) continue;
        const auto p = at(points[i].u, points[i].v);
        const auto d = std::hypot(p.x - mouse.x, p.y - mouse.y);
        if (hovered && d < best) { best = d; hover = static_cast<int>(i); }
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        if (!editable(i)) continue;
        const bool lit = static_cast<int>(i) == hover || static_cast<int>(i) == dragging;
        draw->AddCircleFilled(at(points[i].u, points[i].v), lit ? 6.0F : 4.5F, lit ? IM_COL32(255, 230, 90, 255) : IM_COL32(255, 150, 60, 255));
    }
    if (ImGui::IsItemActivated()) dragging = hover >= 0 ? hover : -2;
    bool changed = false;
    if (active && dragging != -1) {
        const auto delta = ImGui::GetIO().MouseDelta;
        const float du = delta.x / side * view_span, dv = delta.y / side * view_span;
        if (du != 0.0F || dv != 0.0F) {
            for (std::size_t i = 0; i < points.size(); ++i) {
                if (dragging == -2 || static_cast<int>(i) == dragging) {
                    points[i].u += du;
                    points[i].v += dv;
                }
            }
            if (flat && dragging >= 0) {
                const auto corners = patch_corners(patch);
                set_patch_quad(patch, corners[0], corners[1], corners[2], corners[3], patch_corner_uvs(patch));
            }
            changed = true;
        }
    }
    if (!active) dragging = -1;
    if (hovered) ImGui::SetTooltip(hover >= 0 ? "Drag to move this UV" : "Drag to move all UVs");
    return changed;
}

void draw_patch_uv_tools(SblAuthoredPatch& patch, const Texture2D* texture, bool& changed) {
    if (ImGui::CollapsingHeader("UVs (stored)", ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= draw_patch_uv_canvas(patch, texture);
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
                // Repeat once per cell of edge length, like projected walls.
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

// Thumbnails are indexed by block, so adding, removing, or editing any block makes
// them out of date.
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
    select_block_asset(ui, project.blocks.size() - 1U);
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

// A face's texture slot: 0..7 pick one of the eight textures of the building cell
// that places the block (material_for_piece_face).
bool slot_combo(const char* id, std::uint32_t& material) {
    int slot = static_cast<int>(material & 0xffffU);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (!ImGui::SliderInt(id, &slot, 0, 7, "slot %d")) return false;
    material = (material & ~0xffffU) | static_cast<std::uint32_t>(slot);
    return true;
}

void draw_patch_properties(SblBlockMesh& block, SblAuthoredPatch& patch, const Texture2D* texture, bool& changed) {
    field_label("Texture");
    changed |= slot_combo("##patch_slot", patch.normal_flag_a);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Which of the 8 textures of the building cell that places this block.");
    field_label("Texture fit");
    int uv_mode = patch.normal_flag_b != 0U ? 0 : 1;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##uv_mode", &uv_mode, "Projected from facing\0Stored UVs\0")) {
        patch.normal_flag_b = uv_mode == 0 ? 1U : 0U;
        // Stored UVs: clear the projection bits the game would otherwise use.
        if (uv_mode == 1) patch.normal_flag_a &= 0xffffU;
        changed = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Projected: the game lines the texture up with the world like a wall (recommended).\n"
                          "Stored: uses each control point's u, v.");
    }
    if (patch.normal_flag_b == 0U && (patch.normal_flag_a & 0xf0000U) == 0U) {
        draw_patch_uv_tools(patch, texture, changed);
    } else {
        ImGui::TextWrapped("UVs are projected from the facing: one texture repeat per cell, lined up with the walls. "
                           "Choose Stored UVs to set them yourself.");
    }
    if (patch_is_flat(patch)) {
        if (ImGui::CollapsingHeader("Corners (cells)", ImGuiTreeNodeFlags_DefaultOpen)) {
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
    }
    if (ImGui::CollapsingHeader("Curve", ImGuiTreeNodeFlags_DefaultOpen)) {
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
}

void draw_box_properties(SblBlockLight& box, bool& changed) {
    field_label("Texture");
    changed |= slot_combo("##box_slot", box.scalars[0]);
    ImGui::TextDisabled("Box textures are always projected from each side's facing.");
    auto [lo, hi] = box_bounds(box);
    if (ImGui::CollapsingHeader("Bounds (cells)", ImGuiTreeNodeFlags_DefaultOpen)) {
        field_label("From");
        bool resized = drag_vec3("##box_lo", lo);
        field_label("To");
        resized |= drag_vec3("##box_hi", hi);
        if (resized) {
            set_box_bounds(box, lo, hi);
            changed = true;
        }
    }
    if (ImGui::CollapsingHeader("Sides", ImGuiTreeNodeFlags_DefaultOpen)) {
        // The block's own side checkboxes use the same names, so these get their own ID scope.
        ImGui::PushID("box_sides");
        for (std::size_t bit = 0; bit < kBoxSideNames.size(); ++bit) {
            bool on = (box.final_value & (1U << bit)) != 0U;
            if (ImGui::Checkbox(kBoxSideNames[bit], &on)) {
                box.final_value = on ? box.final_value | (1U << bit) : box.final_value & ~(1U << bit);
                changed = true;
            }
            if (bit % 2U == 0U) ImGui::SameLine(160.0F);
        }
        ImGui::PopID();
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

void create_new_block(EditorProject& project, EditorUi& ui, std::string& status) {
    SblBlockMesh block{};
    block.width = block.height = block.depth = 1;
    block.exterior_flags.fill(1);
    add_block(project, ui, unique_block_name(project, "new_block"), std::move(block));
    status = "created " + project.blocks.back().name;
}

void draw_block_inspector(EditorProject& project, EditorUi& ui, std::string& status) {
    if (ImGui::Button(ICON_FA_PLUS " New block")) create_new_block(project, ui, status);
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
    if (ImGui::Button(ICON_FA_FILE_EXPORT " Export .sbl...")) export_block_sbl(project, ui.selected_block, status);

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

    if (ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen)) {
        int mode = ui.block_vertex_mode ? 0 : 1;
        if (ImGui::RadioButton("Points##block_mode", &mode, 0)) {
            ui.block_vertex_mode = true;
            if (ui.block_selected_vertices.empty() && ui.block_element != BlockElementKind::None) {
                ui.block_selected_vertices = element_vertices(*block, ui.block_element, ui.block_element_index);
            }
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Surfaces##block_mode", &mode, 1)) ui.block_vertex_mode = false;
        ImGui::SameLine();
        ImGui::TextDisabled("(Tab)");
        ImGui::Checkbox("Textured##block_textured", &ui.block_textured);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show each face with the texture picked for its slot below.");
        if (ui.block_textured && ImGui::TreeNode("Slot textures##block_slots")) {
            ImGui::TextDisabled("Preview only: the building cell that places a block picks its real textures.");
            texture_slots(project, "block_slot_textures", ui.block_slot_textures);
            ImGui::TreePop();
        }
        if (ui.block_paint_faces) {
            int slot = static_cast<int>(ui.block_paint_slot);
            field_label("Paint slot");
            if (ImGui::SliderInt("##block_paint_slot", &slot, 0, 7, "slot %d")) ui.block_paint_slot = static_cast<std::uint32_t>(slot);
            draw_block_paint_texture(project, ui);
            ImGui::TextWrapped("Click a patch or box to assign this texture slot. A box shares one slot across its faces. Generated sides use fixed slots.");
        }

    }
    if (ImGui::CollapsingHeader("Box", ImGuiTreeNodeFlags_DefaultOpen)) {
        int size[3]{static_cast<int>(block->width), static_cast<int>(block->height), static_cast<int>(block->depth)};
        field_label("Size (cells)");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt3("##block_size", size, 1, 16)) {
            block->width = static_cast<std::uint32_t>(size[0]);
            block->height = static_cast<std::uint32_t>(size[1]);
            block->depth = static_cast<std::uint32_t>(size[2]);
            changed = true;
        }
        ImGui::TextWrapped("Generated sides: one quad per cell, built by the game from the size.");
        for (std::size_t side = 0; side < kExteriorSideNames.size(); ++side) {
            bool on = block->exterior_flags[side] != 0U;
            if (ImGui::Checkbox(kExteriorSideNames[side], &on)) {
                block->exterior_flags[side] = on ? 1U : 0U;
                changed = true;
            }
            if (side % 2U == 0U) ImGui::SameLine(160.0F);
        }
    }

    if (ImGui::CollapsingHeader("Surfaces", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button(ICON_FA_PLUS " Patch")) {
            add_block_patch(project, ui);
            block = &project.blocks[ui.selected_block].data;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A free surface: flat quad by default, curved with a higher degree.");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PLUS " Box")) {
            add_block_box(project, ui);
            block = &project.blocks[ui.selected_block].data;
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
                    ui.block_selected_vertices = element_vertices(*block, ui.block_element, i);
                }
            }
            for (std::size_t i = 0; i < block->lights.size(); ++i) {
                const auto label = "Box " + std::to_string(i + 1U) + "  slot " + std::to_string(block->lights[i].scalars[0] & 0xffffU);
                if (ImGui::Selectable((label + "##b" + std::to_string(i)).c_str(), ui.block_element == BlockElementKind::Box && ui.block_element_index == i)) {
                    ui.block_element = BlockElementKind::Box;
                    ui.block_element_index = i;
                    ui.block_selected_vertices = element_vertices(*block, ui.block_element, i);
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
            ImGui::TextWrapped(ui.block_vertex_mode
                ? "Points: click a point (Shift adds), drag one to move it, or drag a box on empty space. Ctrl+A picks all, Alt+A none. "
                  "G moves, S scales, R rotates (X/Y/Z lock an axis, click or Enter keeps, Esc cancels, Shift skips snapping). "
                  "Arrow keys and PgUp/PgDn nudge 1/8 cell. Tab switches to whole surfaces."
                : "Surfaces: drag an arrow to move, a white cube to resize a box, an orange dot to move a point. "
                  "G moves, S scales, R rotates (X/Y/Z lock an axis, click or Enter keeps, Esc cancels, Shift skips snapping). "
                  "Arrow keys and PgUp/PgDn nudge 1/8 cell. Tab switches to points.");
            if (patch_selected) {
                auto& patch = block->authored_patches[ui.block_element_index];
                // The view's texture for the patch's slot (View > Slot textures).
                const auto& name = ui.block_slot_textures[std::min<std::size_t>(patch.normal_flag_a & 0xffffU, 7U)];
                draw_patch_properties(*block, patch, name.empty() ? nullptr : find_project_texture(project, name), changed);
            }
            else draw_box_properties(block->lights[ui.block_element_index], changed);
        } else {
            ImGui::TextDisabled("Click a surface in the view, or pick one above.");
        }
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
    ui.block_selected_vertices = element_vertices(*block, ui.block_element, ui.block_element_index);
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
    // Indices after the removed surface shift, so the old points no longer match.
    ui.block_selected_vertices.clear();
    mark_block_changed(project, ui, *block);
}

namespace {

Vector3 to_world(Vec3f p) { return {p.x * kUnit, p.y * kUnit, p.z * kUnit}; }

// Every point vertex mode shows: a flat patch's four corners (its inner points follow
// them), all 16 points of a curved patch, and a box's 8 corners.
std::vector<BlockVertexRef> block_vertices(const SblBlockMesh& block) {
    std::vector<BlockVertexRef> refs;
    for (std::size_t i = 0; i < block.authored_patches.size(); ++i) {
        const auto& patch = block.authored_patches[i];
        if (patch.control_points.size() != 16U) continue;
        if (patch_is_flat(patch)) {
            for (const std::size_t point : {0U, 3U, 12U, 15U}) refs.push_back({BlockElementKind::Patch, i, point});
        } else {
            for (std::size_t point = 0; point < 16U; ++point) refs.push_back({BlockElementKind::Patch, i, point});
        }
    }
    for (std::size_t i = 0; i < block.lights.size(); ++i) {
        for (std::size_t point = 0; point < 8U; ++point) refs.push_back({BlockElementKind::Box, i, point});
    }
    return refs;
}

std::vector<BlockVertexRef> element_vertices(const SblBlockMesh& block, BlockElementKind kind, std::size_t index) {
    auto refs = block_vertices(block);
    std::erase_if(refs, [&](const BlockVertexRef& ref) { return ref.kind != kind || ref.element != index; });
    return refs;
}

Vec3f* vertex_ptr(SblBlockMesh& block, const BlockVertexRef& ref) {
    if (ref.kind == BlockElementKind::Patch && ref.element < block.authored_patches.size() &&
        ref.point < block.authored_patches[ref.element].control_points.size()) {
        return &block.authored_patches[ref.element].control_points[ref.point].position;
        }
    if (ref.kind == BlockElementKind::Box && ref.element < block.lights.size() && ref.point < 8U) {
        return &block.lights[ref.element].points[ref.point];
    }
    return nullptr;
}

// What a transform moves: the picked points in vertex mode, otherwise every point of
// the selected patch or box, in element order (the handles index into it).
std::vector<BlockVertexRef> transform_targets(SblBlockMesh& block, const EditorUi& ui) {
    std::vector<BlockVertexRef> refs;
    if (ui.block_vertex_mode) {
        for (const auto& ref : ui.block_selected_vertices) {
            if (vertex_ptr(block, ref) != nullptr) refs.push_back(ref);
        }
        return refs;
    }
    std::size_t count = 0;
    if (ui.block_element == BlockElementKind::Patch && ui.block_element_index < block.authored_patches.size()) {
        count = block.authored_patches[ui.block_element_index].control_points.size();
    } else if (ui.block_element == BlockElementKind::Box && ui.block_element_index < block.lights.size()) {
        count = 8U;
    }
    for (std::size_t point = 0; point < count; ++point) refs.push_back({ui.block_element, ui.block_element_index, point});
    return refs;
}

std::vector<Vec3f*> points_of(SblBlockMesh& block, const std::vector<BlockVertexRef>& refs) {
    std::vector<Vec3f*> points;
    for (const auto& ref : refs) {
        if (auto* point = vertex_ptr(block, ref)) points.push_back(point);
    }
    return points;
}

// A flat patch stays flat: after its corners move, its inner points are blended
// again from them. Only patches with a moved point change, so untouched patches keep
// their stored bytes.
void reblend_moved_flat_patches(SblBlockMesh& block, const std::vector<BlockVertexRef>& moved) {
    for (const auto& ref : moved) {
        if (ref.kind != BlockElementKind::Patch || ref.element >= block.authored_patches.size()) continue;
        auto& patch = block.authored_patches[ref.element];
        if (!patch_is_flat(patch)) continue;
        const auto corners = patch_corners(patch);
        set_patch_quad(patch, corners[0], corners[1], corners[2], corners[3]);
    }
}

} // namespace

void nudge_block_element(EditorProject& project, EditorUi& ui, Vec3f delta) {
    auto* block = selected_block(project, ui);
    if (block == nullptr) return;
    if (ui.block_vertex_mode) {
        const auto refs = transform_targets(*block, ui);
        if (refs.empty()) return;
        for (auto* point : points_of(*block, refs)) *point = {point->x + delta.x, point->y + delta.y, point->z + delta.z};
        reblend_moved_flat_patches(*block, refs);
    } else {
        if (ui.block_element == BlockElementKind::None) return;
        translate_element(*block, ui.block_element, ui.block_element_index, delta);
    }
    mark_block_changed(project, ui, *block);
}

void block_select_all(EditorProject& project, EditorUi& ui, bool select) {
    auto* block = selected_block(project, ui);
    if (block == nullptr) return;
    ui.block_vertex_mode = true;
    ui.block_selected_vertices = select ? block_vertices(*block) : std::vector<BlockVertexRef>{};
}

namespace {

Vec3f center_of(const std::vector<Vec3f*>& points, const std::vector<std::size_t>& subset = {}) {
    Vec3f lo{FLT_MAX, FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    const auto take = [&](const Vec3f& p) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    };
    if (subset.empty()) for (const auto* p : points) take(*p);
    else for (const auto i : subset) take(*points[i]);
    if (lo.x > hi.x) return {};
    return {(lo.x + hi.x) * 0.5F, (lo.y + hi.y) * 0.5F, (lo.z + hi.z) * 0.5F};
}

float axis_value(const Vec3f& p, int axis) { return axis == 0 ? p.x : axis == 1 ? p.y : p.z; }
float& axis_ref(Vec3f& p, int axis) { return axis == 0 ? p.x : axis == 1 ? p.y : p.z; }

// Handles of the selection: 0-2 the move arrows (X, Y, Z). In surface mode also 10-15
// a box's faces (-X, +X, -Y, +Y, -Z, +Z) and 20-35 a patch's control points (only the
// four corners of a flat patch); vertex mode picks points directly instead.
struct BlockHandle {
    int id{};
    Vec3f at{};
    int axis{-1};                      // the axis it moves along, -1 any
    std::vector<std::size_t> points;   // indices into the transform targets; empty = all
};

float arrow_length(const Camera3D& camera, Vec3f pivot) {
    return view_half_height(camera, to_world(pivot)) * 0.28F / kUnit;
}

std::vector<BlockHandle> block_handles(SblBlockMesh& block, const EditorUi& ui, const Camera3D& camera) {
    std::vector<BlockHandle> handles;
    auto points = points_of(block, transform_targets(block, ui));
    if (points.empty()) return handles;
    const auto pivot = center_of(points);
    const float length = arrow_length(camera, pivot);
    for (int axis = 0; axis < 3; ++axis) {
        Vec3f tip = pivot;
        axis_ref(tip, axis) += length;
        handles.push_back({axis, tip, axis, {}});
    }
    if (ui.block_vertex_mode) return handles;
    if (ui.block_element == BlockElementKind::Box) {
        const auto lo_hi = box_bounds(block.lights[ui.block_element_index]);
        for (int face = 0; face < 6; ++face) {
            const int axis = face / 2;
            const bool high = face % 2 == 1;
            const float edge = axis_value(high ? lo_hi.second : lo_hi.first, axis);
            BlockHandle handle{10 + face, pivot, axis, {}};
            axis_ref(handle.at, axis) = edge;
            for (std::size_t i = 0; i < points.size(); ++i) {
                if (std::abs(axis_value(*points[i], axis) - edge) < 1e-4F) handle.points.push_back(i);
            }
            if (!handle.points.empty()) handles.push_back(std::move(handle));
        }
    } else {
        const auto& patch = block.authored_patches[ui.block_element_index];
        const std::vector<std::size_t> ids = patch_is_flat(patch) ? std::vector<std::size_t>{0, 3, 12, 15}
                                                                  : std::vector<std::size_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        for (const auto i : ids) {
            if (i < points.size()) handles.push_back({20 + static_cast<int>(i), *points[i], -1, {i}});
        }
    }
    return handles;
}

// Screen position of a block point, or nothing when it is behind the camera.
std::optional<Vector2> to_screen(const Camera3D& camera, Vec3f p, Vector2 viewport_size) {
    const auto world = to_world(p);
    if (camera.projection == CAMERA_PERSPECTIVE &&
        Vector3DotProduct(Vector3Subtract(world, camera.position), Vector3Subtract(camera.target, camera.position)) <= 0.0F) {
        return std::nullopt;
        }
    return GetWorldToScreenEx(world, camera, static_cast<int>(viewport_size.x), static_cast<int>(viewport_size.y));
}

std::optional<BlockHandle> handle_under_mouse(SblBlockMesh& block, const EditorUi& ui, const Camera3D& camera, Vector2 mouse, Vector2 viewport_size) {
    std::optional<BlockHandle> best;
    float best_distance = 10.0F;
    auto points = points_of(block, transform_targets(block, ui));
    if (points.empty()) return best;
    const auto pivot = to_screen(camera, center_of(points), viewport_size);
    if (!pivot) return best;
    for (auto& handle : block_handles(block, ui, camera)) {
        const auto at = to_screen(camera, handle.at, viewport_size);
        if (!at) continue;
        float distance = Vector2Distance(mouse, *at);
        if (handle.id < 3) {
            // Arrows can be grabbed anywhere along the shaft.
            const auto shaft = Vector2Subtract(*at, *pivot);
            const float length = std::max(1.0F, Vector2LengthSqr(shaft));
            const float t = std::clamp(Vector2DotProduct(Vector2Subtract(mouse, *pivot), shaft) / length, 0.2F, 1.0F);
            distance = Vector2Distance(mouse, Vector2Add(*pivot, Vector2Scale(shaft, t))) + 2.0F;
        }
        if (distance < best_distance) {
            best_distance = distance;
            best = std::move(handle);
        }
    }
    return best;
}

// The vertex-mode point nearest the cursor, within 8 pixels.
std::optional<BlockVertexRef> vertex_under_mouse(SblBlockMesh& block, const Camera3D& camera, Vector2 mouse, Vector2 viewport_size) {
    std::optional<BlockVertexRef> best;
    float best_distance = 8.0F;
    for (const auto& ref : block_vertices(block)) {
        const auto at = to_screen(camera, *vertex_ptr(block, ref), viewport_size);
        if (!at) continue;
        if (const float distance = Vector2Distance(mouse, *at); distance < best_distance) {
            best_distance = distance;
            best = ref;
        }
    }
    return best;
}

// A point and every other point at the same place, so shared corners of
// neighbouring surfaces stay joined when one of them is picked.
std::vector<BlockVertexRef> coincident_vertices(SblBlockMesh& block, const BlockVertexRef& picked) {
    const auto at = *vertex_ptr(block, picked);
    std::vector<BlockVertexRef> group;
    for (const auto& ref : block_vertices(block)) {
        if (distance(*vertex_ptr(block, ref), at) < 1e-4F) group.push_back(ref);
    }
    return group;
}

void add_vertices(std::vector<BlockVertexRef>& selection, const std::vector<BlockVertexRef>& refs) {
    for (const auto& ref : refs) {
        if (std::ranges::find(selection, ref) == selection.end()) selection.push_back(ref);
    }
}

float snap(float value, float step, bool free) { return free ? value : std::round(value / step) * step; }

// Works out the preview block from the block, the transform, and the mouse.
void apply_block_transform(const SblBlockMesh& source, EditorUi& ui, const Camera3D& camera, Vector2 mouse, Vector2 viewport_size) {
    auto preview = source;
    auto start = source;
    const auto refs = transform_targets(start, ui);
    auto start_points = points_of(start, refs);
    auto points = points_of(preview, refs);
    if (points.empty()) return;
    const bool free = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    std::vector<std::size_t> moving;
    int axis = ui.block_transform_axis;
    if (ui.block_transform_handle >= 0) {
        for (const auto& handle : block_handles(start, ui, camera)) {
            if (handle.id != ui.block_transform_handle) continue;
            moving = handle.points;
            if (handle.axis >= 0) axis = handle.axis;
        }
    }
    const auto pivot = center_of(start_points);
    const auto pivot_screen = to_screen(camera, pivot, viewport_size).value_or(ui.block_transform_mouse);
    const auto moved = [&](std::size_t i) { return moving.empty() || std::ranges::find(moving, i) != moving.end(); };

    if (ui.block_transform == BlockTransform::Grab) {
        Vec3f delta{};
        if (axis >= 0) {
            // Along one axis: how far the mouse moved along that axis on screen.
            Vec3f one = pivot;
            axis_ref(one, axis) += 1.0F;
            const auto direction = Vector2Subtract(to_screen(camera, one, viewport_size).value_or(pivot_screen), pivot_screen);
            const float length = Vector2LengthSqr(direction);
            if (length > 1e-3F) axis_ref(delta, axis) = Vector2DotProduct(Vector2Subtract(mouse, ui.block_transform_mouse), direction) / length;
        } else {
            // Free: across the plane facing the camera through the element.
            const auto normal = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
            const auto plane_point = to_world(pivot);
            const auto hit = [&](Vector2 at) -> std::optional<Vector3> {
                const auto ray = get_viewport_mouse_ray(camera, at, viewport_size);
                const float denom = Vector3DotProduct(ray.direction, normal);
                if (std::abs(denom) < 1e-5F) return std::nullopt;
                const float t = Vector3DotProduct(Vector3Subtract(plane_point, ray.position), normal) / denom;
                return Vector3Add(ray.position, Vector3Scale(ray.direction, t));
            };
            const auto a = hit(ui.block_transform_mouse), b = hit(mouse);
            if (a && b) delta = {(b->x - a->x) / kUnit, (b->y - a->y) / kUnit, (b->z - a->z) / kUnit};
        }
        delta = {snap(delta.x, 1.0F / 16.0F, free), snap(delta.y, 1.0F / 16.0F, free), snap(delta.z, 1.0F / 16.0F, free)};
        for (std::size_t i = 0; i < points.size(); ++i) {
            if (moved(i)) *points[i] = {start_points[i]->x + delta.x, start_points[i]->y + delta.y, start_points[i]->z + delta.z};
        }
    } else if (ui.block_transform == BlockTransform::Scale) {
        const float from = std::max(4.0F, Vector2Distance(ui.block_transform_mouse, pivot_screen));
        const float factor = snap(Vector2Distance(mouse, pivot_screen) / from, 0.125F, free);
        for (std::size_t i = 0; i < points.size(); ++i) {
            auto p = *start_points[i];
            for (int a = 0; a < 3; ++a) {
                if (axis >= 0 && a != axis) continue;
                axis_ref(p, a) = axis_value(pivot, a) + (axis_value(p, a) - axis_value(pivot, a)) * factor;
            }
            *points[i] = p;
        }
    } else if (ui.block_transform == BlockTransform::Rotate) {
        const auto a0 = std::atan2(ui.block_transform_mouse.y - pivot_screen.y, ui.block_transform_mouse.x - pivot_screen.x);
        const auto a1 = std::atan2(mouse.y - pivot_screen.y, mouse.x - pivot_screen.x);
        const float angle = snap(a0 - a1, 15.0F * DEG2RAD, free);
        const Vector3 around = axis == 0 ? Vector3{1, 0, 0} : axis == 2 ? Vector3{0, 0, 1} : Vector3{0, 1, 0};
        for (std::size_t i = 0; i < points.size(); ++i) {
            const Vector3 offset{start_points[i]->x - pivot.x, start_points[i]->y - pivot.y, start_points[i]->z - pivot.z};
            const auto turned = Vector3RotateByAxisAngle(offset, around, angle);
            *points[i] = {pivot.x + turned.x, pivot.y + turned.y, pivot.z + turned.z};
        }
    }
    std::vector<BlockVertexRef> moved_refs;
    for (std::size_t i = 0; i < refs.size(); ++i) {
        if (ui.block_transform != BlockTransform::Grab || moved(i)) moved_refs.push_back(refs[i]);
    }
    reblend_moved_flat_patches(preview, moved_refs);
    refresh_sbl_block_faces(preview);
    ui.block_transform_preview = std::move(preview);
}

void end_block_transform(EditorProject& project, EditorUi& ui, bool keep) {
    if (keep && ui.block_transform_preview && !project.blocks.empty()) {
        auto& block = project.blocks[std::min(ui.selected_block, project.blocks.size() - 1U)].data;
        // A click that didn't move anything is not an edit.
        if (!(block == *ui.block_transform_preview)) {
            block = std::move(*ui.block_transform_preview);
            mark_block_changed(project, ui, block);
        }
    }
    ui.block_transform = BlockTransform::None;
    ui.block_transform_axis = -1;
    ui.block_transform_drag = false;
    ui.block_transform_handle = -1;
    ui.block_transform_preview.reset();
}

// A new or copied surface is selected whole, in either mode.
void select_element(SblBlockMesh& block, EditorUi& ui, BlockElementKind kind, std::size_t index) {
    ui.block_element = kind;
    ui.block_element_index = index;
    ui.block_selected_vertices = kind == BlockElementKind::None ? std::vector<BlockVertexRef>{} : element_vertices(block, kind, index);
}

} // namespace

void select_block_asset(EditorUi& ui, std::size_t index) {
    ui.selected_block = ui.browser_block = ui.block_selection_block = index;
    ui.block_element = BlockElementKind::None;
    ui.block_element_index = 0;
    ui.block_selected_vertices.clear();
    ui.block_hover_vertex.reset();
    ui.block_transform = BlockTransform::None;
    ui.block_transform_preview.reset();
    ui.block_transform_drag = false;
    ui.block_transform_handle = ui.block_hover_handle = -1;
}

void select_city_piece_mesh(EditorProject& project, EditorUi& ui) {
    if (ui.city_tool == CityTool::Piece && !ui.place_piece_solid && !project.blocks.empty()) {
        select_block_asset(ui, std::min(ui.place_piece_mesh, project.blocks.size() - 1U));
    } else if (project.city && ui.city_selection == CitySelectionKind::Piece && ui.selected_piece_voxel) {
        const auto preset = static_cast<std::size_t>(ui.selected_piece_word & 0xfffU);
        if (preset >= project.city->data.city_blocks.size()) return;
        const auto& name = project.city->data.city_blocks[preset].name;
        const auto asset = std::ranges::find_if(project.blocks, [&](const auto& a) { return lower_name(a.name) == lower_name(name); });
        if (asset != project.blocks.end()) select_block_asset(ui, static_cast<std::size_t>(asset - project.blocks.begin()));
    }
}

void draw_block_paint_texture(EditorProject& project, EditorUi& ui) {
    if (std::ranges::all_of(ui.block_slot_textures, &std::string::empty) && !project.textures.empty()) {
        ui.block_slot_textures.fill(project.textures.front().name);
    }
    const auto& name = ui.block_slot_textures[ui.block_paint_slot];
    const auto found = std::ranges::find_if(project.textures, [&](const auto& t) { return lower_name(t.name) == lower_name(name); });
    if (found != project.textures.end()) {
        ImGui::Image(ImTextureID(found->texture.id), ImVec2(32.0F, 32.0F));
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("Preview for slot %u: %s. Choose its texture in Assets > Textures.", ui.block_paint_slot, name.c_str());
        ImGui::SameLine();
    }
    ImGui::TextWrapped("Paint slot %u: %s", ui.block_paint_slot, name.empty() ? "unassigned" : name.c_str());
}

void add_block_patch(EditorProject& project, EditorUi& ui) {
    end_block_transform(project, ui, false);
    auto* block = selected_block(project, ui);
    if (block == nullptr) return;
    SblAuthoredPatch patch{};
    patch.column_tess = patch.row_tess = 1;
    patch.normal_flag_b = 1;
    const float w = static_cast<float>(block->width), h = static_cast<float>(block->height);
    set_patch_quad(patch, {0, 0, 0}, {0, h, 0}, {w, h, 0}, {w, 0, 0});
    block->authored_patches.push_back(std::move(patch));
    select_element(*block, ui, BlockElementKind::Patch, block->authored_patches.size() - 1U);
    mark_block_changed(project, ui, *block);
}

void add_block_box(EditorProject& project, EditorUi& ui) {
    end_block_transform(project, ui, false);
    auto* block = selected_block(project, ui);
    if (block == nullptr) return;
    SblBlockLight box{};
    box.final_value = 63U;
    set_box_bounds(box, {0.25F, 0.0F, 0.25F}, {0.75F, 0.5F, 0.75F});
    block->lights.push_back(box);
    select_element(*block, ui, BlockElementKind::Box, block->lights.size() - 1U);
    mark_block_changed(project, ui, *block);
}

void paint_block_element(EditorProject& project, EditorUi& ui, std::uint32_t slot) {
    auto* block = selected_block(project, ui);
    if (block == nullptr || slot > 7U) return;
    std::uint32_t* material = nullptr;
    if (ui.block_element == BlockElementKind::Patch && ui.block_element_index < block->authored_patches.size()) {
        material = &block->authored_patches[ui.block_element_index].normal_flag_a;
    } else if (ui.block_element == BlockElementKind::Box && ui.block_element_index < block->lights.size()) {
        material = &block->lights[ui.block_element_index].scalars[0];
    }
    if (material == nullptr || (*material & 0xffffU) == slot) return;
    *material = (*material & ~0xffffU) | slot;
    mark_block_changed(project, ui, *block);
}

void start_block_transform(EditorProject& project, EditorUi& ui, BlockTransform mode, Vector2 mouse) {
    auto* block = selected_block(project, ui);
    if (block == nullptr || transform_targets(*block, ui).empty()) return;
    ui.block_transform = mode;
    ui.block_transform_axis = -1;
    ui.block_transform_drag = false;
    ui.block_transform_handle = -1;
    ui.block_transform_mouse = mouse;
    ui.block_transform_preview.reset();
}

void block_editor_press(EditorProject& project, EditorUi& ui, const Camera3D& camera, Vector2 mouse, Vector2 viewport_size) {
    auto* block = selected_block(project, ui);
    if (block == nullptr) return;
    if (ui.block_transform != BlockTransform::None) {
        // A click confirms a G/S/R transform.
        end_block_transform(project, ui, true);
        return;
    }
    if (ui.block_paint_faces) {
        pick_block_element(project, ui, get_viewport_mouse_ray(camera, mouse, viewport_size));
        paint_block_element(project, ui, ui.block_paint_slot);
        return;
    }
    if (const auto handle = handle_under_mouse(*block, ui, camera, mouse, viewport_size)) {
        ui.block_transform = BlockTransform::Grab;
        ui.block_transform_axis = handle->axis;
        ui.block_transform_drag = true;
        ui.block_transform_handle = handle->id;
        ui.block_transform_mouse = mouse;
        ui.block_transform_preview.reset();
        return;
    }
    if (!ui.block_vertex_mode) {
        pick_block_element(project, ui, get_viewport_mouse_ray(camera, mouse, viewport_size));
        return;
    }
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (const auto hit = vertex_under_mouse(*block, camera, mouse, viewport_size)) {
        const auto group = coincident_vertices(*block, *hit);
        auto& selection = ui.block_selected_vertices;
        const bool was_selected = std::ranges::find(selection, *hit) != selection.end();
        ui.block_element = hit->kind;
        ui.block_element_index = hit->element;
        if (shift) {
            // Shift+click adds or removes the point.
            if (was_selected) std::erase_if(selection, [&](const BlockVertexRef& ref) { return std::ranges::find(group, ref) != group.end(); });
            else add_vertices(selection, group);
            return;
        }
        // A plain click picks only this point (and any point at the same place), so a
        // drag moves just it. Shift+click builds a bigger selection; G moves that.
        selection = group;
        ui.block_transform = BlockTransform::Grab;
        ui.block_transform_axis = -1;
        ui.block_transform_drag = true;
        ui.block_transform_handle = -1;
        ui.block_transform_mouse = mouse;
        ui.block_transform_preview.reset();
        return;
    }
    // Empty space: dragging draws a selection box, and a click picks the surface under
    // the cursor (both on release, in block_editor_update).
    ui.box_select_active = true;
    ui.box_select_start = mouse;
}

void block_editor_update(EditorProject& project, EditorUi& ui, const Camera3D& camera, Vector2 mouse, Vector2 viewport_size, bool hovered) {
    auto* block = selected_block(project, ui);
    if (block == nullptr || ui.tab != EditorTab::Blocks) {
        if (ui.block_transform != BlockTransform::None) end_block_transform(project, ui, false);
        ui.block_hover_handle = -1;
        ui.block_hover_vertex.reset();
        return;
    }
    // Another block was opened: its points are not the old selection.
    if (ui.block_selection_block != ui.selected_block) select_block_asset(ui, ui.selected_block);
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if (ui.box_select_active && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        ui.box_select_active = false;
        if (Vector2Distance(mouse, ui.box_select_start) > 6.0F) {
            const float x0 = std::min(mouse.x, ui.box_select_start.x), x1 = std::max(mouse.x, ui.box_select_start.x);
            const float y0 = std::min(mouse.y, ui.box_select_start.y), y1 = std::max(mouse.y, ui.box_select_start.y);
            std::vector<BlockVertexRef> inside;
            for (const auto& ref : block_vertices(*block)) {
                const auto at = to_screen(camera, *vertex_ptr(*block, ref), viewport_size);
                if (at && at->x >= x0 && at->x <= x1 && at->y >= y0 && at->y <= y1) inside.push_back(ref);
            }
            if (!shift) ui.block_selected_vertices.clear();
            add_vertices(ui.block_selected_vertices, inside);
            if (!inside.empty()) {
                ui.block_element = inside.back().kind;
                ui.block_element_index = inside.back().element;
            }
        } else {
            // A click on a surface selects all of its points; on nothing, clears.
            pick_block_element(project, ui, get_viewport_mouse_ray(camera, mouse, viewport_size));
            if (!shift) ui.block_selected_vertices.clear();
            if (ui.block_element != BlockElementKind::None) {
                add_vertices(ui.block_selected_vertices, element_vertices(*block, ui.block_element, ui.block_element_index));
            }
        }
    }
    const bool typing = ImGui::GetIO().WantTextInput;
    const bool command = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL) || IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
    if (ui.block_paint_faces && ui.block_transform == BlockTransform::None) {
        ui.block_hover_handle = -1;
        ui.block_hover_vertex.reset();
        return;
    }
    if (ui.block_transform == BlockTransform::None) {
        const auto handle = hovered ? handle_under_mouse(*block, ui, camera, mouse, viewport_size) : std::nullopt;
        ui.block_hover_handle = handle ? handle->id : -1;
        ui.block_hover_vertex = hovered && !handle && ui.block_vertex_mode ? vertex_under_mouse(*block, camera, mouse, viewport_size) : std::nullopt;
        if (hovered && !typing && !command && !IsCursorHidden()) {
            if (IsKeyPressed(KEY_TAB)) {
                // Tab switches between picking points and picking whole surfaces.
                ui.block_vertex_mode = !ui.block_vertex_mode;
                if (ui.block_vertex_mode && ui.block_selected_vertices.empty() && ui.block_element != BlockElementKind::None) {
                    ui.block_selected_vertices = element_vertices(*block, ui.block_element, ui.block_element_index);
                }
            } else if (IsKeyPressed(KEY_G)) {
                start_block_transform(project, ui, BlockTransform::Grab, mouse);
            } else if (IsKeyPressed(KEY_S)) {
                start_block_transform(project, ui, BlockTransform::Scale, mouse);
            } else if (IsKeyPressed(KEY_R)) {
                start_block_transform(project, ui, BlockTransform::Rotate, mouse);
            }
        }
        return;
    }
    // X, Y, Z lock the transform to an axis; pressing the same key again frees it.
    if (!ui.block_transform_drag && !typing) {
        const std::array<int, 3> keys{KEY_X, KEY_Y, KEY_Z};
        for (int axis = 0; axis < 3; ++axis) {
            if (IsKeyPressed(keys[static_cast<std::size_t>(axis)])) ui.block_transform_axis = ui.block_transform_axis == axis ? -1 : axis;
        }
    }
    if (IsKeyPressed(KEY_ESCAPE) || (!ui.block_transform_drag && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT))) {
        end_block_transform(project, ui, false);
        return;
    }
    apply_block_transform(*block, ui, camera, mouse, viewport_size);
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || (ui.block_transform_drag && IsMouseButtonReleased(MOUSE_BUTTON_LEFT))) {
        end_block_transform(project, ui, true);
    }
}

namespace {

// The block's faces with the textures picked for slots 0..7, and their edges.
void draw_block_faces_textured(const EditorProject& project, const EditorUi& ui, const SblBlockMesh& block) {
    draw_block_mesh_textured(project, block, ui.block_slot_textures, {}, kUnit, 0, 255);
    for (const auto& face : block.faces) {
        for (std::size_t k = 0; k < face.vertices.size(); ++k) {
            DrawLine3D(to_world(face.vertices[k]), to_world(face.vertices[(k + 1) % face.vertices.size()]), Color{20, 23, 26, 160});
        }
    }
}

} // namespace

void draw_block_editor_view(EditorProject& project, const EditorUi& ui) {
    if (project.blocks.empty()) return;
    // While a transform runs, the preview copy is drawn in place of the block.
    auto& real = project.blocks[std::min(ui.selected_block, project.blocks.size() - 1U)].data;
    auto& block = ui.block_transform_preview ? const_cast<SblBlockMesh&>(*ui.block_transform_preview) : real;
    if (ui.block_textured) draw_block_faces_textured(project, ui, block);
    else draw_mesh_faces(block, kUnit, {98, 148, 204, 220});
    // The block's cell box and its cell grid on the floor.
    const Vector3 size{static_cast<float>(block.width) * kUnit, static_cast<float>(block.height) * kUnit, static_cast<float>(block.depth) * kUnit};
    DrawCubeWiresV(Vector3Scale(size, 0.5F), size, Color{235, 235, 240, 140});
    for (std::uint32_t x = 1; x < block.width; ++x) {
        DrawLine3D({static_cast<float>(x) * kUnit, 0.0F, 0.0F}, {static_cast<float>(x) * kUnit, 0.0F, size.z}, Color{235, 235, 240, 70});
    }
    for (std::uint32_t z = 1; z < block.depth; ++z) {
        DrawLine3D({0.0F, 0.0F, static_cast<float>(z) * kUnit}, {size.x, 0.0F, static_cast<float>(z) * kUnit}, Color{235, 235, 240, 70});
    }
    // The selected surface in surface mode: filled when untextured, outlined when
    // textured so its texture stays visible.
    if (ui.block_vertex_mode) return;
    std::vector<SblBlockFace> faces;
    if (ui.block_element == BlockElementKind::Patch && ui.block_element_index < block.authored_patches.size()) {
        faces = sbl_patch_faces(block.authored_patches[ui.block_element_index]);
    } else if (ui.block_element == BlockElementKind::Box && ui.block_element_index < block.lights.size()) {
        faces = sbl_box_faces(block.lights[ui.block_element_index]);
    }
    for (const auto& face : faces) {
        if (face.vertices.size() < 4) continue;
        const auto a = to_world(face.vertices[0]), b = to_world(face.vertices[1]), c = to_world(face.vertices[2]), d = to_world(face.vertices[3]);
        if (!ui.block_textured) {
            const Color fill{255, 214, 70, 200};
            DrawTriangle3D(a, b, c, fill); DrawTriangle3D(c, b, a, fill);
            DrawTriangle3D(a, c, d, fill); DrawTriangle3D(d, c, a, fill);
        }
        const Color edge{255, 240, 160, 255};
        DrawLine3D(a, b, edge); DrawLine3D(b, c, edge); DrawLine3D(c, d, edge); DrawLine3D(d, a, edge);
    }
}

// Drawn after the finished scene, so the handles stay visible through the block.
void draw_block_editor_handles(EditorProject& project, const EditorUi& ui, const Camera3D& camera) {
    if (project.blocks.empty() || ui.block_paint_faces) return;
    auto& real = project.blocks[std::min(ui.selected_block, project.blocks.size() - 1U)].data;
    auto& block = ui.block_transform_preview ? const_cast<SblBlockMesh&>(*ui.block_transform_preview) : real;
    if (ui.block_vertex_mode) {
        // Curved patches show their control grid, so you can see what a point pulls.
        for (const auto& patch : block.authored_patches) {
            if (patch_is_flat(patch) || patch.control_points.size() != 16U) continue;
            for (std::size_t row = 0; row < 4; ++row) {
                for (std::size_t column = 0; column < 4; ++column) {
                    const auto here = to_world(patch.control_points[row * 4 + column].position);
                    if (column < 3) DrawLine3D(here, to_world(patch.control_points[row * 4 + column + 1].position), Color{200, 200, 210, 120});
                    if (row < 3) DrawLine3D(here, to_world(patch.control_points[(row + 1) * 4 + column].position), Color{200, 200, 210, 120});
                }
            }
        }
        for (const auto& ref : block_vertices(block)) {
            const auto at = to_world(*vertex_ptr(block, ref));
            const float side = view_half_height(camera, at) * 0.014F;
            const bool selected = std::ranges::find(ui.block_selected_vertices, ref) != ui.block_selected_vertices.end();
            const Color color = ui.block_hover_vertex == ref ? Color{255, 230, 60, 255}
                : selected ? Color{255, 150, 60, 255} : Color{25, 28, 32, 255};
            DrawCubeV(at, {side, side, side}, color);
        }
    }
    if (ui.block_vertex_mode ? ui.block_selected_vertices.empty() : ui.block_element == BlockElementKind::None) return;
    // Handles: the move arrows, box faces, and patch points. The active or hovered
    // handle is yellow; an axis lock is shown as a long line through the selection.
    const auto points = points_of(block, transform_targets(block, ui));
    if (points.empty()) return;
    const auto pivot = center_of(points);
    const float length = arrow_length(camera, pivot);
    const auto handle_color = [&](int id, Color base) {
        return (ui.block_transform_handle == id || (ui.block_transform == BlockTransform::None && ui.block_hover_handle == id))
            ? Color{255, 230, 60, 255} : base;
    };
    constexpr std::array<Color, 3> axis_colors{Color{230, 70, 80, 255}, Color{120, 200, 60, 255}, Color{70, 130, 240, 255}};
    if (ui.block_transform_axis >= 0 && !ui.block_transform_drag) {
        Vec3f a = pivot, b = pivot;
        axis_ref(a, ui.block_transform_axis) -= 64.0F;
        axis_ref(b, ui.block_transform_axis) += 64.0F;
        DrawLine3D(to_world(a), to_world(b), axis_colors[static_cast<std::size_t>(ui.block_transform_axis)]);
    }
    for (const auto& handle : block_handles(block, ui, camera)) {
        const auto at = to_world(handle.at);
        if (handle.id < 3) {
            const auto color = handle_color(handle.id, axis_colors[static_cast<std::size_t>(handle.id)]);
            DrawLine3D(to_world(pivot), at, color);
            Vec3f tip = handle.at;
            axis_ref(tip, handle.id) += length * 0.25F;
            DrawCylinderEx(at, to_world(tip), length * kUnit * 0.06F, 0.0F, 8, color);
        } else if (handle.id < 20) {
            DrawCube(at, length * kUnit * 0.08F, length * kUnit * 0.08F, length * kUnit * 0.08F, handle_color(handle.id, Color{240, 240, 245, 255}));
        } else {
            DrawSphere(at, length * kUnit * 0.04F, handle_color(handle.id, Color{255, 150, 60, 255}));
        }
    }
}

std::optional<std::pair<Vector3, Vector3>> block_focus_bounds(const EditorProject& project, const EditorUi& ui) {
    if (project.blocks.empty()) return std::nullopt;
    auto block = project.blocks[std::min(ui.selected_block, project.blocks.size() - 1U)].data;
    Vector3 lo{FLT_MAX, FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    const auto add = [&](Vec3f p) {
        lo = Vector3Min(lo, {p.x * kUnit, p.y * kUnit, p.z * kUnit});
        hi = Vector3Max(hi, {p.x * kUnit, p.y * kUnit, p.z * kUnit});
    };
    // The picked points, else the picked surface, else every face of the block.
    auto refs = ui.block_selected_vertices;
    if (refs.empty() && ui.block_element != BlockElementKind::None) refs = element_vertices(block, ui.block_element, ui.block_element_index);
    for (auto* point : points_of(block, refs)) add(*point);
    if (lo.x > hi.x) {
        for (const auto& face : block.faces) for (const auto& v : face.vertices) add(v);
    }
    if (lo.x > hi.x) {
        add({0.0F, 0.0F, 0.0F});
        add({static_cast<float>(block.width), static_cast<float>(block.height), static_cast<float>(block.depth)});
    }
    return std::pair{lo, hi};
}

void frame_block_camera(const EditorProject& project, const EditorUi& ui, Camera3D& camera) {
    if (project.blocks.empty()) return;
    // The block seen from the front right, a little above.
    const auto& block = project.blocks[std::min(ui.selected_block, project.blocks.size() - 1U)].data;
    const Vector3 size{static_cast<float>(block.width) * kUnit, static_cast<float>(block.height) * kUnit, static_cast<float>(block.depth) * kUnit};
    const auto reach = std::max({size.x, size.y, size.z}) * 1.6F + 4.0F;
    camera.target = Vector3Scale(size, 0.5F);
    camera.position = Vector3Add(camera.target, {reach * 0.8F, reach * 0.6F, -reach});
    camera.fovy = 60.0F;
    camera.projection = CAMERA_PERSPECTIVE;
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

} // namespace pafta::app_internal
