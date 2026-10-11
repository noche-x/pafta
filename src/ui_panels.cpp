#include "app_internal.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <rlImGui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdint>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace map_editor::app_internal {

// Left-hand label for the next widget at a shared column, so fields line up
// down a panel; the widget takes the rest of the row (give it a "##" id).
void field_label(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 7.5F);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

void draw_struct_inspector_header(const char* file_type, const char* struct_name, const std::string& record_name) {
    (void)file_type;
    ImGui::SeparatorText(struct_name);
    ImGui::TextUnformatted(record_name.c_str());
}

void draw_struct_section(const char* name) {
    ImGui::SeparatorText(name);
}

void invalidate_buildblock_preview_scene(EditorProject& project) {
    project.cached_buildblock_preview_index.reset();
}

const BuildblockPreviewScene& buildblock_preview_scene(EditorProject& project, std::size_t index) {
    if (!project.cached_buildblock_preview_index.has_value() ||
        *project.cached_buildblock_preview_index != index) {
        project.cached_buildblock_preview_scene =
            build_buildblock_preview_scene(project.buildblocks[index].data);
        project.cached_buildblock_preview_index = index;
    }
    return project.cached_buildblock_preview_scene;
}

std::string hex_u32(std::uint32_t value) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return stream.str();
}

// Compact catalog combo: shows entries[selected], lets you pick another. Empty
// names render as their index so unnamed blocks/materials stay selectable. Used by
// both the viewport toolbar and the block tool panel.
bool catalog_combo(const char* label, const std::vector<std::string>& entries, std::size_t& selected) {
    if (entries.empty()) {
        ImGui::TextDisabled("%s: (none)", label);
        return false;
    }
    selected = std::min(selected, entries.size() - 1U);
    const auto entry_text = [&](std::size_t i) {
        return entries[i].empty() ? ("#" + std::to_string(i)) : entries[i];
    };
    bool changed = false;
    ImGui::SetNextItemWidth(118.0F);
    if (ImGui::BeginCombo(label, entry_text(selected).c_str())) {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const bool is_selected = (i == selected);
            if (ImGui::Selectable(entry_text(i).c_str(), is_selected)) {
                selected = i;
                changed = true;
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

const char* building_cell_field_label(std::size_t field_index) {
    static constexpr std::array labels{
        "Exterior Block",
        "Interior Block",
        "Cell Type",
        "Exterior Portal Face",
        "Interior Portal Face",
        "Placed Flag",
        "Itemset",
    };
    return labels[std::min(field_index, labels.size() - 1U)];
}

const char* payload_kind_label(std::uint32_t word) {
    switch ((word >> 29U) & 7U) {
        case 0:
            return word == 0U ? "empty" : "raw";
        case 2:
            return "buildblock";
        case 4:
            return "piece";
        default:
            return "other";
    }
}

std::uint32_t payload_id(std::uint32_t word) {
    return ((word >> 29U) & 7U) == 4U ? (word & 0x3ffU) : (word & 0xffffU);
}

std::uint32_t payload_rotation(std::uint32_t word) {
    return (word >> 24U) & 3U;
}

std::string payload_name(const SbbBuilding& building, const SbbCell& cell, std::uint32_t word) {
    const auto payload_class = (word >> 29U) & 7U;
    const auto id = payload_id(word);
    if (payload_class == 4U && id < building.piece_names.size()) {
        return building.piece_names[id];
    }
    if (payload_class == 2U && cell.cell_type() < building.block_names.size()) {
        return building.block_names[cell.cell_type()];
    }
    return "(unresolved)";
}

const LoadedAsset<SblTemplate>* find_buildblock_asset(const EditorProject& project, std::string_view name) {
    const auto match = std::ranges::find_if(project.buildblocks, [name](const LoadedAsset<SblTemplate>& asset) {
        return asset.name == name;
    });
    return match == project.buildblocks.end() ? nullptr : &*match;
}

const LoadedAsset<SblTemplate>* buildblock_for_cell(const EditorProject& project, const SbbBuilding& building, const SbbCell& cell) {
    if (cell.interior_block() != 0xffffU || cell.cell_type() >= building.block_names.size()) {
        return nullptr;
    }
    return find_buildblock_asset(project, building.block_names[cell.cell_type()]);
}

const SbbCell* cell_at_or_null(const SbbBuilding& building, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    const auto match = std::ranges::find_if(building.cells, [x, y, z](const SbbCell& cell) {
        return cell.x == x && cell.y == y && cell.z == z;
    });
    return match == building.cells.end() ? nullptr : &*match;
}

struct PortalSideInspectorRow {
    const char* side{};
    std::uint32_t code{};
    const char* source{};
};

struct PreviewExtents {
    float min_x{std::numeric_limits<float>::max()};
    float min_y{std::numeric_limits<float>::max()};
    float max_x{std::numeric_limits<float>::lowest()};
    float max_y{std::numeric_limits<float>::lowest()};
    bool has_content{};
};

std::array<PortalSideInspectorRow, 4> portal_side_rows(const SbbBuilding& building, const SbbCell& cell) {
    const auto* east = cell_at_or_null(building, cell.x + 1U, cell.y, cell.z);
    return {
        PortalSideInspectorRow{"North", cell.exterior_portal_face(), "cell.exterior"},
        PortalSideInspectorRow{"West", cell.interior_portal_face(), "cell.interior"},
        PortalSideInspectorRow{"South", cell.exterior_portal_face(), "cell.exterior"},
        PortalSideInspectorRow{"East", east == nullptr ? 0U : east->interior_portal_face(), "east.interior"},
    };
}

std::string portal_layer_label(std::uint32_t side_code, const LoadedAsset<SblTemplate>* buildblock) {
    if (side_code < 3U) {
        return "closed";
    }
    const auto layer_index = static_cast<std::size_t>(side_code - 3U);
    if (buildblock == nullptr) {
        return "layer " + std::to_string(layer_index) + " (template unresolved)";
    }
    if (layer_index >= buildblock->data.layers.size()) {
        return "layer " + std::to_string(layer_index) + " (missing)";
    }
    const auto& layer = buildblock->data.layers[layer_index];
    return "layer " + std::to_string(layer.index) + " type " + std::to_string(layer.portal_type);
}

bool input_u32(const char* label, std::uint32_t& value) {
    constexpr std::uint32_t step = 1U;
    constexpr std::uint32_t step_fast = 16U;
    ImGui::SetNextItemWidth(-FLT_MIN);
    return ImGui::InputScalar(label, ImGuiDataType_U32, &value, &step, &step_fast);
}

bool drag_unit_float(const char* label, float& value) {
    ImGui::SetNextItemWidth(-FLT_MIN);
    return ImGui::DragFloat(label, &value, 0.0625F, 0.0F, 1.0F, "%.4f", ImGuiSliderFlags_AlwaysClamp);
}

void property_row(std::string_view label, std::string_view value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(label.data(), label.data() + label.size());
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value.data(), value.data() + value.size());
}

void include_preview_point(PreviewExtents& extents, float x, float y) {
    extents.min_x = std::min(extents.min_x, x);
    extents.min_y = std::min(extents.min_y, y);
    extents.max_x = std::max(extents.max_x, x);
    extents.max_y = std::max(extents.max_y, y);
    extents.has_content = true;
}

bool preview_layer_matches(std::optional<std::size_t> layer_index, std::size_t candidate) {
    return !layer_index.has_value() || *layer_index == candidate;
}

ImU32 buildblock_rect_fill_color(BuildblockPreviewRectKind kind) {
    switch (kind) {
        case BuildblockPreviewRectKind::Bounds:
            return IM_COL32(63, 183, 184, 95);
        case BuildblockPreviewRectKind::Window:
            return IM_COL32(92, 154, 222, 145);
        case BuildblockPreviewRectKind::Beam:
            return IM_COL32(216, 136, 68, 150);
    }
    return IM_COL32(160, 160, 160, 130);
}

ImU32 buildblock_rect_border_color(BuildblockPreviewRectKind kind) {
    switch (kind) {
        case BuildblockPreviewRectKind::Bounds:
            return IM_COL32(63, 183, 184, 255);
        case BuildblockPreviewRectKind::Window:
            return IM_COL32(92, 154, 222, 255);
        case BuildblockPreviewRectKind::Beam:
            return IM_COL32(216, 136, 68, 255);
    }
    return IM_COL32(160, 160, 160, 255);
}

void draw_buildblock_preview_contents(
    ImDrawList* draw_list,
    const BuildblockPreviewScene& scene,
    std::optional<std::size_t> layer_index,
    ImVec2 canvas_min,
    ImVec2 canvas_max) {
    draw_list->AddRectFilled(canvas_min, canvas_max, IM_COL32(13, 15, 17, 255), 3.0F);

    PreviewExtents extents;
    for (const auto& rect : scene.rects) {
        if (!preview_layer_matches(layer_index, rect.layer_index)) {
            continue;
        }
        include_preview_point(extents, rect.center.x - rect.size.x * 0.5F, rect.center.y - rect.size.y * 0.5F);
        include_preview_point(extents, rect.center.x + rect.size.x * 0.5F, rect.center.y + rect.size.y * 0.5F);
    }
    for (const auto& door : scene.doors) {
        if (!preview_layer_matches(layer_index, door.layer_index)) {
            continue;
        }
        include_preview_point(extents, door.position.x - 0.25F, door.position.y - 0.25F);
        include_preview_point(extents, door.position.x + 0.25F, door.position.y + 0.25F);
    }
    if (!extents.has_content) {
        include_preview_point(extents, 0.0F, 0.0F);
        include_preview_point(extents, 4.0F, 4.0F);
    }

    const auto range_x = std::max(0.001F, extents.max_x - extents.min_x);
    const auto range_y = std::max(0.001F, extents.max_y - extents.min_y);
    const auto pad = 8.0F;
    const auto canvas_w = std::max(1.0F, canvas_max.x - canvas_min.x - pad * 2.0F);
    const auto canvas_h = std::max(1.0F, canvas_max.y - canvas_min.y - pad * 2.0F);
    const auto scale = std::min(canvas_w / range_x, canvas_h / range_y);
    const auto origin = ImVec2{
        canvas_min.x + pad + (canvas_w - range_x * scale) * 0.5F,
        canvas_min.y + pad + (canvas_h - range_y * scale) * 0.5F,
    };
    auto map_point = [&](float x, float y) {
        return ImVec2{
            origin.x + (x - extents.min_x) * scale,
            origin.y + (extents.max_y - y) * scale,
        };
    };

    for (const auto& rect : scene.rects) {
        if (!preview_layer_matches(layer_index, rect.layer_index)) {
            continue;
        }
        const auto a = map_point(rect.center.x - rect.size.x * 0.5F, rect.center.y + rect.size.y * 0.5F);
        const auto b = map_point(rect.center.x + rect.size.x * 0.5F, rect.center.y - rect.size.y * 0.5F);
        draw_list->AddRectFilled(a, b, buildblock_rect_fill_color(rect.kind), 1.0F);
        draw_list->AddRect(a, b, buildblock_rect_border_color(rect.kind), 1.0F);
    }
    for (const auto& door : scene.doors) {
        if (!preview_layer_matches(layer_index, door.layer_index)) {
            continue;
        }
        const auto center = map_point(door.position.x, door.position.y);
        draw_list->AddCircleFilled(center, 3.5F, IM_COL32(180, 108, 225, 230));
        draw_list->AddCircle(center, 3.5F, IM_COL32(245, 245, 245, 230));
    }
}

void draw_payload_table(const SbbBuilding& building, const SbbCell& cell) {
    if (!ImGui::BeginTable("##cell_payloads", 5, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        return;
    }
    ImGui::TableSetupColumn("Slot");
    ImGui::TableSetupColumn("Raw");
    ImGui::TableSetupColumn("Kind");
    ImGui::TableSetupColumn("Id / Rot");
    ImGui::TableSetupColumn("Resolved");
    ImGui::TableHeadersRow();

    auto draw_row = [&](const char* slot, std::uint32_t word) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(slot);
        ImGui::TableNextColumn();
        const auto raw = hex_u32(word);
        ImGui::TextUnformatted(raw.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(payload_kind_label(word));
        ImGui::TableNextColumn();
        ImGui::Text("%u / %u", payload_id(word), payload_rotation(word));
        ImGui::TableNextColumn();
        const auto name = payload_name(building, cell, word);
        ImGui::TextUnformatted(name.c_str());
    };

    draw_row("Exterior", cell.exterior_block());
    draw_row("Interior", cell.interior_block());
    ImGui::EndTable();
}

void draw_texture_slot_table(const SbbBuilding& building, const SbbCell& cell) {
    if (!ImGui::BeginTable("##cell_textures", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        return;
    }
    ImGui::TableSetupColumn("Set");
    ImGui::TableSetupColumn("Slot");
    ImGui::TableSetupColumn("Value");
    ImGui::TableSetupColumn("Material");
    ImGui::TableHeadersRow();
    for (std::size_t slot = 0; slot < cell.shorts.size(); ++slot) {
        const auto material = static_cast<std::size_t>(cell.shorts[slot]);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(slot < 8U ? "Exterior" : "Interior");
        ImGui::TableNextColumn();
        ImGui::Text("%zu", slot % 8U);
        ImGui::TableNextColumn();
        ImGui::Text("%u", cell.shorts[slot]);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(material < building.materials.size() ? building.materials[material].c_str() : "(unresolved)");
    }
    ImGui::EndTable();
}

void draw_portal_side_table(const EditorProject& project, const SbbBuilding& building, const SbbCell& cell) {
    const auto* buildblock = buildblock_for_cell(project, building, cell);
    if (buildblock != nullptr) {
        ImGui::Text("Buildblock: %s", buildblock->name.c_str());
    } else {
        ImGui::TextUnformatted("Buildblock: (unresolved)");
    }

    if (!ImGui::BeginTable("##portal_side_codes", 4, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        return;
    }
    ImGui::TableSetupColumn("Side");
    ImGui::TableSetupColumn("Code");
    ImGui::TableSetupColumn("Source");
    ImGui::TableSetupColumn("Layer");
    ImGui::TableHeadersRow();
    for (const auto& row : portal_side_rows(building, cell)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.side);
        ImGui::TableNextColumn();
        ImGui::Text("%u", row.code);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(row.source);
        ImGui::TableNextColumn();
        const auto layer = portal_layer_label(row.code, buildblock);
        ImGui::TextUnformatted(layer.c_str());
    }
    ImGui::EndTable();
}

void reset_buildblock_template_selection(EditorUi& ui, std::size_t buildblock_index) {
    ui.selected_buildblock = buildblock_index;
    ui.selected_buildblock_layer = 0;
    ui.selected_buildblock_door = 0;
    ui.selected_buildblock_window = 0;
    ui.selected_buildblock_beam = 0;
    ui.selected_buildblock_field = 0;
}

void select_buildblock_face(EditorUi& ui, std::size_t face_index) {
    ui.selected_buildblock_layer = face_index;
    ui.selected_buildblock_door = 0;
    ui.selected_buildblock_window = 0;
    ui.selected_buildblock_beam = 0;
}

void draw_buildblock_face_preview(
    ImDrawList* draw_list,
    const BuildblockPreviewScene& scene,
    const SblLayer& layer,
    std::size_t face_index,
    ImVec2 min,
    ImVec2 max,
    bool selected,
    bool hovered) {
    const auto bg = ImGui::GetColorU32(selected ? ImGuiCol_HeaderActive : hovered ? ImGuiCol_HeaderHovered : ImGuiCol_FrameBg);
    const auto border = ImGui::GetColorU32(selected ? ImGuiCol_Text : ImGuiCol_Border);
    draw_list->AddRectFilled(min, max, bg);
    draw_list->AddRect(min, max, border);

    const auto title = "Face " + std::to_string(face_index);
    const auto title_width = ImGui::CalcTextSize(title.c_str()).x;
    draw_list->AddText(
        ImVec2{min.x + 8.0F, min.y + 6.0F},
        ImGui::GetColorU32(ImGuiCol_Text),
        title.c_str(),
        title.c_str() + title.size());
    const auto type = "type " + std::to_string(layer.portal_type);
    const auto type_width = ImGui::CalcTextSize(type.c_str()).x;
    if (title_width + type_width + 28.0F <= max.x - min.x) {
        draw_list->AddText(
            ImVec2{max.x - type_width - 8.0F, min.y + 6.0F},
            ImGui::GetColorU32(ImGuiCol_TextDisabled),
            type.c_str(),
            type.c_str() + type.size());
    }

    const auto canvas_min = ImVec2{min.x + 8.0F, min.y + 28.0F};
    const auto canvas_max = ImVec2{max.x - 8.0F, max.y - 8.0F};
    draw_buildblock_preview_contents(draw_list, scene, std::optional<std::size_t>{layer.index}, canvas_min, canvas_max);
}


void draw_buildblock_preview_panel(EditorProject& project, EditorUi& ui) {
    if (project.buildblocks.empty()) {
        ImGui::TextUnformatted("No buildblocks loaded.");
        return;
    }

    ui.selected_buildblock = std::min(ui.selected_buildblock, project.buildblocks.size() - 1U);
    const auto& asset = project.buildblocks[ui.selected_buildblock];
    const auto& buildblock = asset.data;
    if (buildblock.layers.empty()) {
        ImGui::Text("Buildblock: %s", asset.name.c_str());
        ImGui::TextUnformatted("No portal faces.");
        return;
    }

    ui.selected_buildblock_layer = std::min(ui.selected_buildblock_layer, buildblock.layers.size() - 1U);
    const auto layer_id = buildblock.layers[ui.selected_buildblock_layer].index;
    ImGui::Text("Buildblock: %s", asset.name.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("Face %zu", layer_id);

    const auto& scene = buildblock_preview_scene(project, ui.selected_buildblock);
    const auto canvas_min = ImGui::GetCursorScreenPos();
    const auto avail = ImGui::GetContentRegionAvail();
    const auto canvas_size = ImVec2{
        std::max(160.0F, avail.x),
        std::max(180.0F, avail.y),
    };
    ImGui::InvisibleButton("##buildblock_2d_preview_canvas", canvas_size);
    draw_buildblock_preview_contents(
        ImGui::GetWindowDrawList(),
        scene,
        std::optional<std::size_t>{layer_id},
        canvas_min,
        ImVec2{canvas_min.x + canvas_size.x, canvas_min.y + canvas_size.y});
}

// A stable pseudo-color for a material with no loaded texture, so the swatch is
// still distinguishable instead of a flat grey.
bool material_swatch_picker(const EditorProject& project, const char* id, const std::vector<std::string>& materials, std::size_t& selected) {
    if (materials.empty()) {
        ImGui::TextDisabled("(no materials)");
        return false;
    }
    selected = std::min(selected, materials.size() - 1U);
    bool changed = false;
    constexpr float swatch = 40.0F;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int per_row = std::max(1, static_cast<int>(avail / (swatch + 6.0F)));

    ImGui::PushID(id);
    for (std::size_t i = 0; i < materials.size(); ++i) {
        if (i % static_cast<std::size_t>(per_row) != 0) {
            ImGui::SameLine();
        }
        ImGui::PushID(static_cast<int>(i));
        const std::string name = materials[i].empty() ? ("material " + std::to_string(i)) : materials[i];
        const auto* tex = texture_for_material(project, materials[i]);
        bool clicked = false;
        if (tex != nullptr && tex->id != 0U) {
            clicked = rlImGuiImageButtonSize(("##sw" + std::to_string(i)).c_str(), tex, Vector2{swatch, swatch});
        } else {
            // No texture: a colored button stands in, labelled with the short name.

            const std::string label = name.substr(0, 3) + "##sw" + std::to_string(i);
            clicked = ImGui::Button(label.c_str(), ImVec2(swatch, swatch));

        }
        if (clicked) {
            selected = i;
            changed = true;
        }
        if (selected == i) {
            ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                IM_COL32(255, 200, 80, 255), 2.0F, 0, 3.0F);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", name.c_str());
        }
        ImGui::PopID();
    }
    ImGui::PopID();
    ImGui::TextDisabled("Selected: %s", materials[selected].empty() ? ("material " + std::to_string(selected)).c_str() : materials[selected].c_str());
    return changed;
}

std::optional<std::size_t> search_grid(const char* id, std::array<char, 128>& search, std::size_t count, std::size_t selected,
    float size, float height, const std::function<const std::string&(std::size_t)>& name,
    const std::function<std::pair<unsigned int, bool>(std::size_t)>& image) {
    std::optional<std::size_t> clicked;
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", ICON_FA_MAGNIFYING_GLASS " Search", search.data(), search.size());
    const auto needle = lower_name(search.data());
    if (ImGui::BeginChild("##grid", ImVec2(-FLT_MIN, height), ImGuiChildFlags_Borders)) {
        const int per_row = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (size + ImGui::GetStyle().ItemSpacing.x + 8.0F)));
        int column = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const std::string& label = name(i);
            if (!needle.empty() && lower_name(label).find(needle) == std::string::npos) continue;
            if (column++ % per_row != 0) ImGui::SameLine();
            ImGui::PushID(static_cast<int>(i));
            const auto [texture, flipped] = image(i);
            const bool hit = texture != 0U
                ? ImGui::ImageButton("##i", ImTextureID(texture), ImVec2(size, size), ImVec2(0.0F, flipped ? 1.0F : 0.0F), ImVec2(1.0F, flipped ? 0.0F : 1.0F))
                : ImGui::Button(fit_label(label, size).c_str(), ImVec2(size + 8.0F, size + 8.0F));
            if (hit) clicked = i;
            if (i == selected) {
                ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(235, 190, 78, 255), 2.0F, 0, 3.0F);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", label.c_str());
            ImGui::PopID();
        }
        if (column == 0) ImGui::TextDisabled("Nothing matches.");
    }
    ImGui::EndChild();
    ImGui::PopID();
    return clicked;
}

// Place Block panel: Exterior and Interior, each switched on separately and
// each either normal (solid shell / generated room) or a piece. A click writes
// only the enabled layers (BuildingTool_WriteCellFromCurrentSelection
// @1400672a0).
void draw_block_tool_panel(EditorProject& project, EditorUi& ui) {
    ui.selected_building = std::min(ui.selected_building, project.buildings.size() - 1U);
    auto& building = project.buildings[ui.selected_building].data;
    auto& cfg = ui.block_config;

    // What each texture slot paints. The shell's sides use slots 1..4 as
    // S/W/N/E; the room's walls use 1..4 as N/E/S/W and slot 6 for opening
    // frames (add_native_interior_room). Pieces use the slots their faces name.
    static constexpr std::array<const char*, 8> exterior_slots{"Bottom", "South", "West", "North", "East", "Top", "Slot 6", "Slot 7"};
    static constexpr std::array<const char*, 8> interior_slots{"Floor", "North wall", "East wall", "South wall", "West wall", "Ceiling", "Opening frame", "Slot 7"};

    const auto piece_picker = [&](const char* id, std::array<char, 128>& search, std::size_t& block) {
        if (project.blocks.empty()) {
            ImGui::TextDisabled("No blocks loaded.");
            return;
        }
        block = std::min(block, project.blocks.size() - 1U);
        ImGui::Text("Piece: %s", project.blocks[block].name.c_str());
        const auto picked = search_grid(id, search, project.blocks.size(), block, 48.0F, 132.0F,
            [&](std::size_t i) -> const std::string& { return project.blocks[i].name; },
            [&](std::size_t i) {
                const auto* t = block_thumbnail(project, i);
                return std::pair{t != nullptr ? t->texture.id : 0U, true};
            });
        if (picked) block = *picked;
    };
    // Each slot shows its material as a thumbnail; clicking opens a swatch grid.
    const auto texture_grid = [&](const char* label, std::array<std::size_t, 8>& materials, const std::array<const char*, 8>& names) {
        if (!ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) return;
        if (building.materials.empty()) {
            ImGui::TextDisabled("No materials yet. Add some under Materials above.");
            ImGui::TreePop();
            return;
        }
        for (std::size_t slot = 0; slot < materials.size(); ++slot) {
            ImGui::PushID(static_cast<int>(slot));
            materials[slot] = std::min(materials[slot], building.materials.size() - 1U);
            const auto* tex = texture_for_material(project, building.materials[materials[slot]]);
            const bool open = tex != nullptr && tex->id != 0U
                ? rlImGuiImageButtonSize("##slot", tex, Vector2{30.0F, 30.0F})
                : ImGui::Button("##slot", ImVec2(30.0F, 30.0F));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s", names[slot], building.materials[materials[slot]].c_str());
            ImGui::SameLine();
            ImGui::TextUnformatted(names[slot]);
            if (open) ImGui::OpenPopup("slot_swatches");
            if (ImGui::BeginPopup("slot_swatches")) {
                if (material_swatch_picker(project, "pop", building.materials, materials[slot])) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            if (slot % 2U == 0U) ImGui::SameLine(170.0F);
            ImGui::PopID();
        }
        if (ImGui::SmallButton("Use the first slot for all")) materials.fill(materials[0]);
        ImGui::TreePop();
    };

    static std::array<char, 128> exterior_search{};
    static std::array<char, 128> interior_search{};
    ImGui::SeparatorText("Block to place");
    ImGui::Checkbox("Exterior", &cfg.exterior);
    ImGui::SameLine(110.0F);
    ImGui::BeginDisabled(!cfg.exterior);
    ImGui::Checkbox("Piece##exterior", &cfg.exterior_piece);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Off: a plain solid block. On: a piece mesh (.sbl).");
    if (cfg.exterior) {
        if (cfg.exterior_piece) piece_picker("exterior_piece", exterior_search, cfg.exterior_piece_block);
        texture_grid("Exterior textures", cfg.exterior_textures, exterior_slots);
    }
    ImGui::EndDisabled();

    ImGui::Checkbox("Interior", &cfg.interior);
    ImGui::SameLine(110.0F);
    ImGui::BeginDisabled(!cfg.interior);
    ImGui::Checkbox("Piece##interior", &cfg.interior_piece);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Off: a room. On: a piece mesh (.sbl) inside the block.");
    if (cfg.interior) {
        if (cfg.interior_piece) {
            piece_picker("interior_piece", interior_search, cfg.interior_piece_block);
        } else if (!building.block_names.empty()) {
            field_label("Room windows and doors from");
            catalog_combo("##block_buildblock", building.block_names, cfg.buildblock_index);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The buildblock (.sbl in buildblock/) whose layers give the room's walls their windows and doors.");
        }
        texture_grid("Interior textures", cfg.interior_textures, interior_slots);
    }
    ImGui::EndDisabled();

    if ((cfg.exterior && cfg.exterior_piece) || (cfg.interior && cfg.interior_piece)) {
        static constexpr std::array<const char*, 4> rotations{"0\xC2\xB0", "90\xC2\xB0", "180\xC2\xB0", "270\xC2\xB0"};
        int rotation = static_cast<int>(cfg.rotation & 3U);
        field_label("Piece rotation");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##block_rotation", &rotation, rotations.data(), 4)) cfg.rotation = static_cast<std::uint32_t>(rotation);
    }

    ImGui::Spacing();
    ImGui::Checkbox("Change the clicked block", &cfg.apply_to_clicked);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("On: a click writes the enabled layers into the block you click, e.g. to add a room to a shell.\n"
                          "Off: a click adds a block in the empty cell in front of the face.");
    }
    if (!cfg.exterior && !cfg.interior) ImGui::TextColored(ImVec4(1.0F, 0.8F, 0.2F, 1.0F), "Tick Exterior or Interior to place.");
}

void draw_building_inspector(EditorProject& project, EditorUi& ui, std::string& status) {
    if (project.buildings.empty()) return;
    ui.selected_building = std::min(ui.selected_building, project.buildings.size() - 1U);
    auto& asset = project.buildings[ui.selected_building];
    auto& building = asset.data;
    if (ui.material_selection_building != ui.selected_building) {
        ui.selected_materials.clear();
        ui.material_selection_building = ui.selected_building;
    }
    // Placements and the map archive use the asset name; the name stored inside
    // the .sbb is separate, usually blank, and unused by the game.
    std::array<char, 52> asset_name{};
    std::copy_n(asset.name.data(), std::min(asset.name.size(), asset_name.size() - 1), asset_name.data());
    field_label("Name");
    if (ImGui::InputText("##asset_name", asset_name.data(), asset_name.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
        rename_building(project, ui, asset_name.data(), status);
    }
    if (ImGui::IsItemActive()) ImGui::TextDisabled("Press Enter to rename. Every placement of it is renamed too.");
    std::array<char, 64> name{};
    std::copy_n(building.name.data(), std::min(building.name.size(), name.size() - 1), name.data());
    field_label("Name in file (unused by the game)");
    if (ImGui::InputTextWithHint("##name_in_file", "(blank)", name.data(), name.size())) {
        building.name = name.data();
        mark_building_changed(project, ui);
    }
    ImGui::Text("Size: %u x %u x %u", building.size_x, building.size_z, building.size_y);
    ImGui::TextDisabled("Size follows blocks. Materials: %zu", building.materials.size());
    if (ImGui::Button("Move anchor")) ui.building_tool = BuildingTool::Anchor;
    ImGui::SameLine();
    if (ImGui::Button("Anchor at origin")) {
        building.anchor_x = building.anchor_y = building.anchor_z = 0;
        mark_building_changed(project, ui);
    }
    if (ui.building_tool == BuildingTool::Anchor)
        ImGui::TextWrapped("Drag an anchor arrow. The anchor snaps to whole cells.");
    if (ImGui::CollapsingHeader("Materials", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Ctrl/Cmd-click to select several materials.");
        if (ImGui::BeginListBox("##materials", ImVec2(-FLT_MIN, 150))) {
            for (std::size_t i = 0; i < building.materials.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                const auto* tex = texture_for_material(project, building.materials[i]);
                if (tex != nullptr && tex->id != 0U) rlImGuiImageSize(tex, 22, 22);
                else ImGui::Dummy(ImVec2(22.0F, 22.0F));
                ImGui::SameLine();
                const auto it = std::find(ui.selected_materials.begin(), ui.selected_materials.end(), i);
                const auto label = std::to_string(i) + "  " + building.materials[i] + (tex == nullptr ? "  (no texture)" : "");
                if (ImGui::Selectable(label.c_str(), it != ui.selected_materials.end(), 0, ImVec2(0.0F, 22.0F))) {
                    if (!ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeySuper) ui.selected_materials = {i};
                    else if (it == ui.selected_materials.end()) ui.selected_materials.push_back(i);
                    else ui.selected_materials.erase(it);
                    ui.paint_material_index = i;
                }
                ImGui::PopID();
            }
            ImGui::EndListBox();
        }
        // Add: click a texture, or type a name that is not loaded and press Enter.
        const auto add_material = [&](const std::string& material) {
            if (material.empty() || building.materials.size() >= 1024) return;
            const auto key = lower_name(material);
            if (std::ranges::none_of(building.materials, [&](const std::string& m) { return lower_name(m) == key; })) {
                building.materials.push_back(material);
                mark_building_changed(project, ui);
            }
        };
        ImGui::SeparatorText("Add material");
        const auto picked = search_grid("add_material", ui.new_material_name, project.textures.size(), project.textures.size(), 36.0F, 120.0F,
            [&](std::size_t i) -> const std::string& { return project.textures[i].name; },
            [&](std::size_t i) { return std::pair{project.textures[i].texture.id, false}; });
        if (picked) add_material(project.textures[*picked].name);
        ImGui::BeginDisabled(ui.new_material_name[0] == '\0' || building.materials.size() >= 1024);
        if (ImGui::Button("Add typed name")) {
            add_material(ui.new_material_name.data());
            ui.new_material_name.fill(0);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Adds the search text as a material name, for a texture that is not loaded here.");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(ui.selected_materials.empty());
        if (ImGui::Button("Remove selected")) {
            std::vector<std::uint16_t> remap(building.materials.size(), 0xffffU);
            std::vector<std::string> kept;
            for (std::size_t i = 0; i < building.materials.size(); ++i) {
                if (std::find(ui.selected_materials.begin(), ui.selected_materials.end(), i) == ui.selected_materials.end()) {
                    remap[i] = static_cast<std::uint16_t>(kept.size());
                    kept.push_back(building.materials[i]);
                }
            }
            for (auto& cell : building.cells) {
                for (const auto base : {0U, 8U}) for (std::size_t face = 0; face < 8; ++face) {
                    auto& value = cell.shorts[base + face];
                    if (value < remap.size()) value = remap[value];
                }
            }
            building.materials = std::move(kept);
            ui.selected_materials.clear();
            ui.paint_material_index = 0;
            ui.block_config.exterior_textures.fill(0);
            ui.block_config.interior_textures.fill(0);
            mark_building_changed(project, ui);
        }
        ImGui::EndDisabled();
    }
    // The buildblocks (cell_type) and piece meshes (piece payloads) this building
    // may use. Entries are referenced by index, so only an unused buildblock can
    // be removed; pieces are append-only.
    const auto name_list = [&](const char* header, std::vector<std::string>& names, const std::vector<std::string>& library,
                               bool removable) {
        if (!ImGui::CollapsingHeader(header)) return;
        ImGui::PushID(header);
        if (names.empty()) ImGui::TextDisabled("None yet. Add one below.");
        for (std::size_t i = 0; i < names.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("%zu  %s", i, names[i].c_str());
            if (removable) {
                const bool used = std::ranges::any_of(building.cells, [&](const SbbCell& c) { return c.occupied && c.cell_type() == i; });
                ImGui::SameLine();
                ImGui::BeginDisabled(used);
                if (ImGui::SmallButton(ICON_FA_XMARK)) {
                    names.erase(names.begin() + static_cast<std::ptrdiff_t>(i));
                    for (auto& c : building.cells) if (c.cell_type() > i) --c.cell_type();
                    mark_building_changed(project, ui);
                    ImGui::EndDisabled();
                    ImGui::PopID();
                    break;
                }
                ImGui::EndDisabled();
                if (used && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("In use by blocks");
            }
            ImGui::PopID();
        }
        catalog_combo("##add", library, ui.building_list_add);
        ImGui::SameLine();
        ImGui::BeginDisabled(library.empty());
        if (ImGui::Button("Add") && ui.building_list_add < library.size()) {
            const auto& name = library[ui.building_list_add];
            if (std::ranges::find(names, name) == names.end()) {
                names.push_back(name);
                mark_building_changed(project, ui);
            }
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    };
    std::vector<std::string> buildblock_library;
    for (const auto& asset : project.buildblocks) buildblock_library.push_back(asset.name);
    name_list("Buildblocks", building.block_names, buildblock_library, true);
    name_list("Pieces", building.piece_names, block_names(project), false);

    if (ImGui::CollapsingHeader("Item sets", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Names stored in this building. The game reads definitions from data/itemset/*.sis.");
        for (std::size_t i = 0; i < building.item_set_names.size(); ++i) {
            const auto label = std::to_string(i) + "  " + building.item_set_names[i];
            if (ImGui::Selectable(label.c_str(), ui.selected_item_set == i)) ui.selected_item_set = i;
        }
        const auto add_name = [&](std::string_view name) {
            if (lower_name(name).ends_with(".sis")) name.remove_suffix(4);
            if (name.empty() || name.size() > 63U || name == "." || name == ".." ||
                name.find_first_of("/\\:") != std::string_view::npos) {
                status = "An item-set name needs 1-63 bytes and no / \\ or :";
                return false;
            }
            const auto found = std::ranges::find_if(building.item_set_names, [&](const std::string& existing) {
                return lower_name(existing) == lower_name(name);
            });
            if (found != building.item_set_names.end()) {
                ui.selected_item_set = static_cast<std::size_t>(found - building.item_set_names.begin());
                return true;
            }
            if (building.item_set_names.size() >= 1024U) {
                status = "A building can have at most 1024 item-set names";
                return false;
            }
            building.item_set_names.emplace_back(name);
            ui.selected_item_set = building.item_set_names.size() - 1U;
            mark_building_changed(project, ui);
            return true;
        };
        ImGui::SeparatorText("Add item set");
        if (project.item_set_catalog.empty()) {
            ImGui::TextDisabled("No local .sis files found. Enter a name below.");
        } else {
            catalog_combo("##item_set_catalog", project.item_set_catalog, ui.item_set_catalog_add);
            ImGui::SameLine();
            ImGui::BeginDisabled(building.item_set_names.size() >= 1024U);
            if (ImGui::Button("Add##item_set_catalog")) add_name(project.item_set_catalog[ui.item_set_catalog_add]);
            ImGui::EndDisabled();
        }
        ImGui::InputTextWithHint("##new_item_set_name", "Other item-set name", ui.new_item_set_name.data(), ui.new_item_set_name.size());
        ImGui::SameLine();
        ImGui::BeginDisabled(ui.new_item_set_name[0] == '\0' || building.item_set_names.size() >= 1024U);
        if (ImGui::Button("Add##typed_item_set") && add_name(ui.new_item_set_name.data())) ui.new_item_set_name.fill(0);
        ImGui::EndDisabled();
    }

    if (ui.building_tool == BuildingTool::ItemSet) {
        ImGui::SeparatorText("Place item set");
        if (ImGui::RadioButton("Place", !ui.clear_item_set)) ui.clear_item_set = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Clear", ui.clear_item_set)) ui.clear_item_set = true;
        if (!ui.clear_item_set) {
            catalog_combo("##item_set_to_place", building.item_set_names, ui.selected_item_set);
            static constexpr std::array<const char*, 4> rotations{"0\xC2\xB0", "90\xC2\xB0", "180\xC2\xB0", "270\xC2\xB0"};
            int rotation = static_cast<int>(ui.item_set_rotation & 3U);
            field_label("Rotation");
            if (ImGui::Combo("##item_set_rotation", &rotation, rotations.data(), 4)) ui.item_set_rotation = static_cast<std::uint32_t>(rotation);
        }
        ImGui::TextWrapped("Click a block to %s its item set. Yellow markers show placed sets.", ui.clear_item_set ? "clear" : "place");
    }

    const auto occupied = occupied_cell_indices(building);
    if (!occupied.empty()) {
        const auto& cell = building.cells[occupied[std::min(ui.selected_building_cell, occupied.size() - 1U)]];
        if (cell.item_set_index != 0U) {
            if (is_sbb_item_set_payload(cell.item_set_index) &&
                sbb_item_set_name_index(cell.item_set_index) < building.item_set_names.size()) {
                const auto name_index = sbb_item_set_name_index(cell.item_set_index);
                ImGui::Text("Selected block item set: %s (%u\xC2\xB0)",
                    building.item_set_names[name_index].c_str(),
                    sbb_item_set_rotation(cell.item_set_index) * 90U);
                if (ImGui::SmallButton("Use selected block's item set")) {
                    ui.selected_item_set = name_index;
                    ui.item_set_rotation = sbb_item_set_rotation(cell.item_set_index);
                    ui.clear_item_set = false;
                    ui.building_tool = BuildingTool::ItemSet;
                }
            } else {
                ImGui::Text("Selected block item set: %s", hex_u32(cell.item_set_index).c_str());
            }
        } else ImGui::TextDisabled("Selected block item set: none");
    }

    if (ui.building_tool == BuildingTool::PlaceBlock) draw_block_tool_panel(project, ui);
    if (ui.building_tool == BuildingTool::PaintTexture) {
        ImGui::SeparatorText("Paint");
        if (ImGui::RadioButton("Texture", !ui.paint_side_code)) ui.paint_side_code = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Wall type", ui.paint_side_code)) ui.paint_side_code = true;
        if (!ui.paint_side_code) {
            material_swatch_picker(project, "paint_material", building.materials, ui.paint_material_index);
        } else {
            // Side code 0 leaves the side open, 1 is a plain wall, 3.. picks a
            // layer (windows, doors) of the block's buildblock.
            ImGui::TextUnformatted("Buildblock");
            catalog_combo("##paint_buildblock", building.block_names, ui.block_config.buildblock_index);
            const auto* asset = building.block_names.empty()
                ? nullptr
                : find_buildblock_asset(project, building.block_names[std::min(ui.block_config.buildblock_index, building.block_names.size() - 1U)]);
            if (ImGui::Selectable("Open (no wall)", ui.paint_code == 0U)) ui.paint_code = 0U;
            if (ImGui::Selectable("Plain wall", ui.paint_code == 1U)) ui.paint_code = 1U;
            if (asset != nullptr) {
                for (std::size_t layer = 0; layer < asset->data.layers.size() && layer < 13U; ++layer) {
                    const auto& l = asset->data.layers[layer];
                    const auto label = "Layer " + std::to_string(layer) + ": " + std::to_string(l.windows.size()) + " windows, " +
                        std::to_string(l.doors.size()) + " doors";
                    const auto code = static_cast<std::uint32_t>(layer + 3U);
                    if (ImGui::Selectable(label.c_str(), ui.paint_code == code)) ui.paint_code = code;
                }
            } else if (!building.block_names.empty()) {
                ImGui::TextDisabled("Buildblock not loaded.");
            }
            ImGui::TextWrapped("Click a wall face. Layers also set the block's buildblock.");
        }
    }
}

const char* block_field_label(std::size_t field) {
    static constexpr std::array labels{
        "Width",
        "Height",
        "Depth",
        "Flag 0",
        "Flag 1",
        "Flag 2",
        "Flag 3",
        "Flag 4",
        "Flag 5",
        "Patch Point X",
        "Patch Point Y",
        "Patch Point Z",
        "Patch Point U",
        "Patch Point V",
    };
    return labels[std::min(field, labels.size() - 1U)];
}

void draw_portal_details(const SbcPortal& portal) {
    if (ImGui::BeginTable("##portal_summary", 2, ImGuiTableFlags_SizingStretchProp)) {
        property_row("Index", std::to_string(portal.index));
        property_row("Fields", std::to_string(portal.fields[0]) + ", " + std::to_string(portal.fields[1]) + ", " + std::to_string(portal.fields[2]) + ", " + std::to_string(portal.fields[3]));
        property_row("Blocks", std::to_string(portal.blocks.size()));
        property_row("Textures", std::to_string(portal.textures.size()));
        property_row("Itemsets", std::to_string(portal.itemsets.size()));
        ImGui::EndTable();
    }

    const auto rows = std::max(portal.blocks.size(), std::max(portal.textures.size(), portal.itemsets.size()));
    if (rows == 0U) {
        return;
    }
    if (!ImGui::BeginTable("##portal_slots", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        return;
    }
    ImGui::TableSetupColumn("Slot");
    ImGui::TableSetupColumn("Block Word");
    ImGui::TableSetupColumn("Texture");
    ImGui::TableSetupColumn("Itemset");
    ImGui::TableHeadersRow();
    for (std::size_t i = 0; i < rows; ++i) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%zu", i);
        ImGui::TableNextColumn();
        if (i < portal.blocks.size()) {
            const auto value = hex_u32(portal.blocks[i]);
            ImGui::TextUnformatted(value.c_str());
        } else {
            ImGui::TextUnformatted("-");
        }
        ImGui::TableNextColumn();
        if (i < portal.textures.size()) {
            ImGui::Text("%u", portal.textures[i]);
        } else {
            ImGui::TextUnformatted("-");
        }
        ImGui::TableNextColumn();
        if (i < portal.itemsets.size()) {
            ImGui::Text("%u", portal.itemsets[i]);
        } else {
            ImGui::TextUnformatted("-");
        }
    }
    ImGui::EndTable();
}

const char* buildblock_field_label(std::size_t field) {
    static constexpr std::array labels{
        "Bounds X",
        "Bounds Width",
        "Bounds Y",
        "Bounds Height",
        "Door X",
        "Door Y",
        "Door Type",
        "Window X",
        "Window Width",
        "Window Y",
        "Window Height",
        "Window Type",
        "Beam X",
        "Beam Width",
        "Beam Y",
        "Beam Height",
        "Beam Type",
    };
    return labels[std::min(field, labels.size() - 1U)];
}

const char* buildblock_field_label_getter(void*, int field) {
    return buildblock_field_label(static_cast<std::size_t>(std::max(field, 0)));
}

void draw_rect_collection_table(
    const char* table_id,
    const std::vector<SblPortalRect>& rects,
    std::size_t& selected_index,
    const char* label) {
    if (rects.empty()) {
        ImGui::Text("%s: none", label);
        return;
    }
    selected_index = std::min(selected_index, rects.size() - 1U);
    if (!ImGui::BeginTable(table_id, 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        return;
    }
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("X");
    ImGui::TableSetupColumn("W");
    ImGui::TableSetupColumn("Y");
    ImGui::TableSetupColumn("H");
    ImGui::TableSetupColumn("Type");
    ImGui::TableHeadersRow();
    for (std::size_t i = 0; i < rects.size(); ++i) {
        const auto& rect = rects[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (ImGui::Selectable(std::to_string(i).c_str(), selected_index == i, ImGuiSelectableFlags_SpanAllColumns)) {
            selected_index = i;
        }
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", rect.x);
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", rect.width);
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", rect.y);
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", rect.height);
        ImGui::TableNextColumn();
        ImGui::Text("%u", rect.type);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void draw_buildblock_inspector(EditorProject& project, EditorUi& ui, std::string& status) {
    if (project.buildblocks.empty()) return;
    ui.selected_buildblock = std::min(ui.selected_buildblock, project.buildblocks.size() - 1U);
    auto& asset = project.buildblocks[ui.selected_buildblock];
    auto& model = asset.data;
    ImGui::SeparatorText(asset.name.c_str());
    ImGui::TextDisabled("All sizes use the same 0-1 cell scale.");
    if (model.layers.empty()) {
        if (ImGui::Button("Add face")) {
            SblLayer face{}; face.bounds = {0, 1, 0, 1}; model.layers.push_back(face);
            mark_buildblock_changed(project, ui);
        }
        return;
    }
    float overall_size = 0;
    for (const auto& face : model.layers) {
        overall_size = std::max({overall_size, face.bounds[0] + face.bounds[1], face.bounds[2] + face.bounds[3]});
    }
    float requested_size = overall_size;
    if (ImGui::SliderFloat("Overall size", &requested_size, .0625F, 1.0F, "%.3f cells") && overall_size > 0) {
        const auto factor = requested_size / overall_size;
        for (auto& face : model.layers) {
            for (auto& value : face.bounds) value *= factor;
            for (auto& door : face.doors) { door.x *= factor; door.y *= factor; }
            for (auto* rects : {&face.windows, &face.beams}) for (auto& rect : *rects) {
                rect.x *= factor; rect.y *= factor; rect.width *= factor; rect.height *= factor;
            }
        }
        mark_buildblock_changed(project, ui);
    }
    ui.selected_buildblock_layer = std::min(ui.selected_buildblock_layer, model.layers.size() - 1);
    const auto face_name = "Face " + std::to_string(ui.selected_buildblock_layer + 1);
    if (ImGui::BeginCombo("Face", face_name.c_str())) {
        for (std::size_t i = 0; i < model.layers.size(); ++i)
            if (ImGui::Selectable(("Face " + std::to_string(i + 1)).c_str(), i == ui.selected_buildblock_layer))
                ui.selected_buildblock_layer = i;
        ImGui::EndCombo();
    }
    ImGui::BeginDisabled(model.layers.size() >= 16);
    if (ImGui::Button("Add face")) {
        SblLayer face{}; face.index = model.layers.size(); face.bounds = {0, 1, 0, 1};
        model.layers.push_back(face); ui.selected_buildblock_layer = model.layers.size() - 1;
        mark_buildblock_changed(project, ui);
    }
    ImGui::EndDisabled();
    // Face numbers are referenced by buildings. Clear in place instead of
    // shifting every following face and silently changing those references.
    ImGui::SameLine();
    if (ImGui::Button("Clear face")) {
        model.layers[ui.selected_buildblock_layer] = SblLayer{};
        model.layers[ui.selected_buildblock_layer].index = ui.selected_buildblock_layer;
        mark_buildblock_changed(project, ui);
    }
    auto& layer = model.layers[ui.selected_buildblock_layer];
    bool changed = false;
    auto rectangle = [&](const char* id, float& x, float& y, float& width, float& height) {
        ImGui::PushID(id);
        float position[]{x, y}, size[]{width, height};
        bool edit = ImGui::SliderFloat2("Position", position, 0, 1, "%.3f");
        edit |= ImGui::SliderFloat2("Size", size, 0, 1, "%.3f");
        if (edit) {
            x = position[0]; y = position[1];
            width = std::min(size[0], 1 - x); height = std::min(size[1], 1 - y);
        }
        ImGui::PopID();
        return edit;
    };
    changed |= rectangle("face", layer.bounds[0], layer.bounds[2], layer.bounds[1], layer.bounds[3]);
    // Only portal type 1's mirror behavior is understood; preserve other values.
    if (layer.portal_type <= 1) {
        bool mirrored = layer.portal_type == 1;
        if (ImGui::Checkbox("Mirror on opposite sides", &mirrored)) { layer.portal_type = mirrored ? 1 : 0; changed = true; }
    }
    if (ImGui::CollapsingHeader("Doors", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BeginDisabled(layer.doors.size() >= 2);
        if (ImGui::Button("Add door")) { layer.doors.push_back({0, .5F, 0}); changed = true; }
        ImGui::EndDisabled();
        for (std::size_t i = 0; i < layer.doors.size();) {
            ImGui::PushID(static_cast<int>(i));
            float position[]{layer.doors[i].x, layer.doors[i].y};
            if (ImGui::SliderFloat2("Position", position, 0, 1, "%.3f")) {
                layer.doors[i].x = position[0]; layer.doors[i].y = position[1]; changed = true;
            }
            if (ImGui::Button("Remove door")) { layer.doors.erase(layer.doors.begin() + i); changed = true; }
            else ++i;
            ImGui::PopID();
        }
    }
    auto openings = [&](const char* label, std::vector<SblPortalRect>& rects) {
        ImGui::PushID(label);
        if (ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::BeginDisabled(rects.size() >= 16);
            if (ImGui::Button("Add")) { rects.push_back({0, .25F, .5F, .25F, .5F}); changed = true; }
            ImGui::EndDisabled();
            for (std::size_t i = 0; i < rects.size();) {
                ImGui::PushID(static_cast<int>(i));
                auto& rect = rects[i];
                changed |= rectangle("opening", rect.x, rect.y, rect.width, rect.height);
                if (ImGui::Button("Remove")) { rects.erase(rects.begin() + i); changed = true; }
                else ++i;
                ImGui::PopID();
            }
        }
        ImGui::PopID();
    };
    openings("Windows", layer.windows); openings("Beams", layer.beams);
    if (changed) mark_buildblock_changed(project, ui);
}

} // namespace map_editor::app_internal
