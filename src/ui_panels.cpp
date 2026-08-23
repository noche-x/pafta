#include "app_internal.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <rlImGui.h>
#include <extras/IconsFontAwesome6.h>

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

std::string hex_u32(std::uint32_t value) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return stream.str();
}

bool workspace_search_matches(const EditorUi& ui, EditorTab tab, std::string_view name) {
    const auto& query = ui.asset_search[static_cast<std::size_t>(tab)];
    if (query[0] == '\0') {
        return true;
    }
    return lower_name(name).find(lower_name(query.data())) != std::string::npos;
}

std::string compact_asset_name(std::string_view name) {
    constexpr std::size_t max_length = 14;
    if (name.size() <= max_length) {
        return std::string{name};
    }
    return std::string{name.substr(0, max_length - 3U)} + "...";
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
            const bool is_selected = i == selected;
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
    const auto bg = selected ? IM_COL32(37, 54, 57, 255) : (hovered ? IM_COL32(29, 36, 39, 255) : IM_COL32(18, 21, 23, 255));
    const auto border = selected ? IM_COL32(255, 225, 80, 255) : IM_COL32(65, 74, 78, 255);
    draw_list->AddRectFilled(min, max, bg, 5.0F);
    draw_list->AddRect(min, max, border, 5.0F, 0, selected ? 2.0F : 1.0F);

    const auto title = "Face " + std::to_string(face_index);
    const auto title_width = ImGui::CalcTextSize(title.c_str()).x;
    draw_list->AddText(
        ImGui::GetFont(),
        ImGui::GetFontSize(),
        ImVec2{min.x + 8.0F, min.y + 6.0F},
        IM_COL32(229, 234, 235, 255),
        title.c_str(),
        title.c_str() + title.size());
    const auto type = "type " + std::to_string(layer.portal_type);
    const auto type_width = ImGui::CalcTextSize(type.c_str()).x;
    if (title_width + type_width + 28.0F <= max.x - min.x) {
        draw_list->AddText(
            ImGui::GetFont(),
            ImGui::GetFontSize(),
            ImVec2{max.x - type_width - 8.0F, min.y + 6.0F},
            IM_COL32(139, 153, 157, 255),
            type.c_str(),
            type.c_str() + type.size());
    }

    const auto canvas_min = ImVec2{min.x + 8.0F, min.y + 28.0F};
    const auto canvas_max = ImVec2{max.x - 8.0F, max.y - 8.0F};
    draw_buildblock_preview_contents(draw_list, scene, std::optional<std::size_t>{layer.index}, canvas_min, canvas_max);
}

void draw_building_asset_panel(EditorProject& project, EditorUi& ui) {
    const auto names = building_names(project);
    std::vector<std::size_t> visible;
    visible.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (workspace_search_matches(ui, EditorTab::Buildings, names[i])) {
            visible.push_back(i);
        }
    }
    ImGui::Text("Buildings (*.sbb)");
    const auto building_list_height = std::max(130.0F, ImGui::GetContentRegionAvail().y * 0.34F);
    if (ImGui::BeginChild("##building_templates_panel", ImVec2(0, building_list_height), true)) {
        if (ImGui::BeginTable("##building_templates_table", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Cells", ImGuiTableColumnFlags_WidthFixed, 54.0F);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(visible.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const auto i = visible[static_cast<std::size_t>(row)];
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const bool selected = ui.selected_building == i;
                    if (ImGui::Selectable(names[i].c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                        ui.selected_building = i;
                        ui.selected_building_cell = 0;
                        project.building_floor_preview_dirty = true;
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", project.buildings[i].data.cells.size());
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    if (project.buildings.empty()) {
        return;
    }

    ui.selected_building = std::min(ui.selected_building, project.buildings.size() - 1U);
    auto& building = project.buildings[ui.selected_building].data;
    const auto occupied = occupied_cell_indices(building);

    // View: whole building, or one floor chosen with a slider + step buttons.
    ImGui::SeparatorText("View");
    int view_mode = ui.building_view_floor_only ? 1 : 0;
    ImGui::RadioButton("Whole building", &view_mode, 0); ImGui::SameLine();
    ImGui::RadioButton("Floor by floor", &view_mode, 1);
    const bool was_floor_only = ui.building_view_floor_only;
    ui.building_view_floor_only = view_mode == 1;
    if (ui.building_view_floor_only) {
        const int max_floor = std::max(0, static_cast<int>(building.size_y) - 1);
        ui.building_view_floor = std::clamp(ui.building_view_floor, 0, max_floor);
        bool changed = ui.building_view_floor_only != was_floor_only;
        if (ImGui::Button("-") && ui.building_view_floor > 0) { --ui.building_view_floor; changed = true; }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-40.0F);
        if (ImGui::SliderInt("##floor", &ui.building_view_floor, 0, max_floor, "Floor %d")) { changed = true; }
        ImGui::SameLine();
        if (ImGui::Button("+") && ui.building_view_floor < max_floor) { ++ui.building_view_floor; changed = true; }
        if (changed) {
            project.building_floor_preview_dirty = true;
        }
    } else if (was_floor_only) {
        project.building_floor_preview_dirty = true;  // switched back to whole
    }

    ImGui::SeparatorText("Occupied Cells");
    if (ImGui::BeginChild("##building_cells_panel", ImVec2(0, 0), true)) {
        if (ImGui::BeginTable("##building_cells_table", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Index");
            ImGui::TableSetupColumn("X");
            ImGui::TableSetupColumn("Y");
            ImGui::TableSetupColumn("Z");
            ImGui::TableSetupColumn("Type");
            ImGui::TableHeadersRow();
            for (std::size_t row = 0; row < occupied.size(); ++row) {
                const auto cell_index = occupied[row];
                if (cell_index >= building.cells.size()) {
                    continue;
                }
                const auto& cell = building.cells[cell_index];
                ImGui::PushID(static_cast<int>(row));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool selected = ui.selected_building_cell == row;
                if (ImGui::Selectable(std::to_string(cell.index).c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                    ui.selected_building_cell = row;
                    // In floor mode, jump the view to the picked cell's floor.
                    if (ui.building_view_floor_only) {
                        ui.building_view_floor = static_cast<int>(cell.y);
                    }
                    project.building_floor_preview_dirty = true;
                }
                ImGui::TableNextColumn();
                ImGui::Text("%u", cell.x);
                ImGui::TableNextColumn();
                ImGui::Text("%u", cell.y);
                ImGui::TableNextColumn();
                ImGui::Text("%u", cell.z);
                ImGui::TableNextColumn();
                ImGui::Text("%u", cell.cell_type());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}

void draw_buildblock_gallery(EditorProject& project, EditorUi& ui) {
    ImGui::Text("Buildblocks (*.sbl)");
    ImGui::TextDisabled("%zu templates", project.buildblocks.size());
    ImGui::Separator();
    if (project.buildblocks.empty()) {
        ImGui::TextUnformatted("No buildblocks loaded.");
        return;
    }

    ui.selected_buildblock = std::min(ui.selected_buildblock, project.buildblocks.size() - 1U);
    std::vector<std::size_t> visible;
    visible.reserve(project.buildblocks.size());
    for (std::size_t i = 0; i < project.buildblocks.size(); ++i) {
        if (workspace_search_matches(ui, EditorTab::Buildblocks, project.buildblocks[i].name)) {
            visible.push_back(i);
        }
    }
    const auto total_y = std::max(1.0F, ImGui::GetContentRegionAvail().y);
    const auto list_height = std::clamp(total_y * 0.36F, 120.0F, 220.0F);
    if (ImGui::BeginChild("##buildblock_list_panel", ImVec2(0, list_height), true)) {
        if (ImGui::BeginTable("##buildblock_list_table", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Faces", ImGuiTableColumnFlags_WidthFixed, 56.0F);
            ImGui::TableHeadersRow();
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(visible.size()));
            while (clipper.Step()) {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                    const auto i = visible[static_cast<std::size_t>(row)];
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    const bool selected = ui.selected_buildblock == i;
                    if (ImGui::Selectable(project.buildblocks[i].name.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                        if (ui.selected_buildblock != i) {
                            reset_buildblock_template_selection(ui, i);
                        } else {
                            ui.selected_buildblock = i;
                        }
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", project.buildblocks[i].data.layers.size());
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

    ui.selected_buildblock = std::min(ui.selected_buildblock, project.buildblocks.size() - 1U);
    const auto& selected_asset = project.buildblocks[ui.selected_buildblock];
    const auto& buildblock = selected_asset.data;
    ImGui::SeparatorText("Portal Faces");
    if (buildblock.layers.empty()) {
        ImGui::TextUnformatted("Selected buildblock has no portal faces.");
        return;
    }

    ui.selected_buildblock_layer = std::min(ui.selected_buildblock_layer, buildblock.layers.size() - 1U);
    const auto scene = build_buildblock_preview_scene(buildblock);
    const auto spacing = ImGui::GetStyle().ItemSpacing.x;
    const auto avail_x = std::max(1.0F, ImGui::GetContentRegionAvail().x);
    const auto min_tile_w = 92.0F;
    auto columns = std::max(1, static_cast<int>((avail_x + spacing) / (min_tile_w + spacing)));
    columns = std::min(columns, static_cast<int>(buildblock.layers.size()));
    const auto rows = std::max(1, static_cast<int>((buildblock.layers.size() + static_cast<std::size_t>(columns) - 1U) / static_cast<std::size_t>(columns)));
    const auto grid_avail_y = std::max(1.0F, ImGui::GetContentRegionAvail().y);
    const auto fit_tile_h = (grid_avail_y - spacing * static_cast<float>(rows - 1)) / static_cast<float>(rows);
    const auto tile_h = std::clamp(fit_tile_h, 74.0F, 116.0F);
    const auto tile_w = std::max(1.0F, std::floor((avail_x - spacing * static_cast<float>(columns - 1)) / static_cast<float>(columns)));

    ImGui::PushID("buildblock_faces");
    for (std::size_t i = 0; i < buildblock.layers.size(); ++i) {
        const auto min = ImGui::GetCursorScreenPos();
        const auto size = ImVec2{tile_w, tile_h};
        const auto id = "##face" + std::to_string(i);
        ImGui::InvisibleButton(id.c_str(), size);
        if (ImGui::IsItemClicked()) {
            select_buildblock_face(ui, i);
        }
        draw_buildblock_face_preview(
            ImGui::GetWindowDrawList(),
            scene,
            buildblock.layers[i],
            i,
            min,
            ImVec2{min.x + size.x, min.y + size.y},
            ui.selected_buildblock_layer == i,
            ImGui::IsItemHovered());
        if (i + 1U < buildblock.layers.size() && (static_cast<int>(i) + 1) % columns != 0) {
            ImGui::SameLine();
        }
    }
    ImGui::PopID();
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

    const auto scene = build_buildblock_preview_scene(buildblock);
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
ImU32 material_fallback_color(std::size_t index) {
    const auto hue = static_cast<float>((index * 47U) % 360U) / 360.0F;
    const auto h6 = hue * 6.0F;
    const auto c = 0.55F;
    const auto x = c * (1.0F - std::fabs(std::fmod(h6, 2.0F) - 1.0F));
    float r = 0.3F;
    float g = 0.3F;
    float b = 0.3F;
    if (h6 < 1.0F) { r += c; g += x; }
    else if (h6 < 2.0F) { r += x; g += c; }
    else if (h6 < 3.0F) { g += c; b += x; }
    else if (h6 < 4.0F) { g += x; b += c; }
    else if (h6 < 5.0F) { r += x; b += c; }
    else { r += c; b += x; }
    return IM_COL32(static_cast<int>(r * 255), static_cast<int>(g * 255), static_cast<int>(b * 255), 255);
}

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
        const bool is_selected = i == selected;
        // Highlight the selected swatch with a bright frame.
        if (is_selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(255, 210, 90, 255));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3.0F, 3.0F));
        }
        const std::string name = materials[i].empty() ? ("material " + std::to_string(i)) : materials[i];
        const auto* tex = texture_for_material(project, materials[i]);
        bool clicked = false;
        if (tex != nullptr && tex->id != 0U) {
            clicked = rlImGuiImageButtonSize(("##sw" + std::to_string(i)).c_str(), tex, Vector2{swatch, swatch});
        } else {
            // No texture: a colored button stands in, labelled with the short name.
            ImGui::PushStyleColor(ImGuiCol_Button, material_fallback_color(i));
            const std::string label = name.substr(0, 3) + "##sw" + std::to_string(i);
            clicked = ImGui::Button(label.c_str(), ImVec2(swatch, swatch));
            ImGui::PopStyleColor();
        }
        if (is_selected) {
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }
        if (clicked) {
            selected = i;
            changed = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", name.c_str());
        }
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

// editor.exe-style Place Block panel: choose exterior/interior shells, the
// buildblock template, and a material for each of the six faces on each enabled
// side. Edits ui.block_config in place; placement reads it.
void draw_block_tool_panel(EditorProject& project, EditorUi& ui) {
    ui.selected_building = std::min(ui.selected_building, project.buildings.size() - 1U);
    auto& building = project.buildings[ui.selected_building].data;
    auto& cfg = ui.block_config;

    // Face order matches the renderer's slot order (0..5).
    static constexpr std::array<const char*, 6> face_names{"Bottom", "South", "West", "North", "East", "Top"};

    ImGui::SeparatorText("Block to place");
    ImGui::Checkbox("Exterior", &cfg.exterior);
    ImGui::SameLine();
    ImGui::Checkbox("Interior", &cfg.interior);
    if (!cfg.exterior && !cfg.interior) {
        ImGui::TextColored(ImVec4(0.95F, 0.65F, 0.3F, 1.0F), "Enable a shell to place.");
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Buildblock");
    ImGui::SetNextItemWidth(-FLT_MIN);
    catalog_combo("##block_buildblock", building.block_names, cfg.buildblock_index);

    // Each face shows its current material as a thumbnail button; clicking opens a
    // popup swatch grid to choose a new one. A "set all" copies the first face.
    const auto face_material_grid = [&](const char* label, std::array<std::size_t, 6>& materials) {
        if (!ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen)) {
            return;
        }
        if (building.materials.empty()) {
            ImGui::TextDisabled("No materials in this building.");
            ImGui::TreePop();
            return;
        }
        for (std::size_t face = 0; face < 6; ++face) {
            ImGui::PushID(static_cast<int>(face));
            materials[face] = std::min(materials[face], building.materials.size() - 1U);
            const auto* tex = texture_for_material(project, building.materials[materials[face]]);
            bool open = false;
            if (tex != nullptr && tex->id != 0U) {
                open = rlImGuiImageButtonSize("##facesw", tex, Vector2{34.0F, 34.0F});
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, material_fallback_color(materials[face]));
                open = ImGui::Button("##facesw", ImVec2(34.0F, 34.0F));
                ImGui::PopStyleColor();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(face_names[face]);
            if (open) {
                ImGui::OpenPopup("face_swatches");
            }
            if (ImGui::BeginPopup("face_swatches")) {
                if (material_swatch_picker(project, "pop", building.materials, materials[face])) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (ImGui::SmallButton("Copy Bottom to all faces")) {
            materials.fill(materials[0]);
        }
        ImGui::TreePop();
    };

    ImGui::Spacing();
    if (cfg.exterior) {
        face_material_grid("Exterior textures", cfg.exterior_face_material);
    }
    if (cfg.interior) {
        face_material_grid("Interior textures", cfg.interior_face_material);
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Click a block face in the viewport to place against it.");
}

void draw_city_building_template_gallery(EditorProject& project, EditorUi& ui) {
    if (project.buildings.empty()) {
        ImGui::TextDisabled("No building templates loaded.");
        return;
    }
    ui.place_building_template = std::min(ui.place_building_template, project.buildings.size() - 1U);
    ImGui::Text("Place which building:");
    std::vector<std::size_t> visible;
    visible.reserve(project.buildings.size());
    for (std::size_t i = 0; i < project.buildings.size(); ++i) {
        if (workspace_search_matches(ui, EditorTab::City, project.buildings[i].name)) {
            visible.push_back(i);
        }
    }
    constexpr float cell = 96.0F;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int per_row = std::max(1, static_cast<int>(avail / (cell + 12.0F)));
    if (ImGui::BeginChild("##bldg_gallery", ImVec2(-FLT_MIN, -FLT_MIN), false)) {
        for (std::size_t row = 0; row < visible.size(); ++row) {
            const auto i = visible[row];
            if (row % static_cast<std::size_t>(per_row) != 0) {
                ImGui::SameLine();
            }
            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginGroup();
            const bool selected = ui.place_building_template == i;
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(255, 210, 90, 255));
            }
            const auto* thumb = building_thumbnail(project, i);
            bool clicked = false;
            if (thumb != nullptr && thumb->texture.id != 0U) {
                // RenderTextures are Y-flipped in GL, so flip the UVs (uv0.y=1,
                // uv1.y=0) to show the thumbnail upright.
                clicked = ImGui::ImageButton(
                    ("##bt" + std::to_string(i)).c_str(),
                    ImTextureID(thumb->texture.id),
                    ImVec2(cell, cell),
                    ImVec2(0.0F, 1.0F), ImVec2(1.0F, 0.0F));
            } else {
                clicked = ImGui::Button(("?##bt" + std::to_string(i)).c_str(), ImVec2(cell, cell));
            }
            if (selected) {
                ImGui::PopStyleColor();
            }
            if (clicked) {
                ui.place_building_template = i;
            }
            ImGui::TextUnformatted(compact_asset_name(project.buildings[i].name).c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", project.buildings[i].name.c_str());
            }
            ImGui::EndGroup();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void draw_city_piece_template_gallery(EditorProject& project, EditorUi& ui) {
    if (!project.city || project.city->data.city_blocks.empty()) {
        ImGui::TextDisabled("No piece blocks.");
        return;
    }
    ui.selected_city_block = std::min(ui.selected_city_block, project.city->data.city_blocks.size() - 1U);
    ImGui::Text("Place which piece:");
    auto names = city_block_names(project.city->data);
    std::vector<std::size_t> visible;
    visible.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (workspace_search_matches(ui, EditorTab::City, names[i])) {
            visible.push_back(i);
        }
    }
    constexpr float cell = 80.0F;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int per_row = std::max(1, static_cast<int>(avail / (cell + 8.0F)));
    const float gallery_h = project.city->data.portals.empty() ? -FLT_MIN : 200.0F;
    if (ImGui::BeginChild("##piece_gallery", ImVec2(-FLT_MIN, gallery_h), false)) {
        for (std::size_t row = 0; row < visible.size(); ++row) {
            const auto i = visible[row];
            if (row % static_cast<std::size_t>(per_row) != 0) {
                ImGui::SameLine();
            }
            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginGroup();
            const bool selected = ui.selected_city_block == i;
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(255, 210, 90, 255));
            }
            const auto* thumb = city_block_thumbnail(project, i);
            bool clicked = false;
            if (thumb != nullptr && thumb->texture.id != 0U) {
                clicked = ImGui::ImageButton(
                    ("##pt" + std::to_string(i)).c_str(),
                    ImTextureID(thumb->texture.id),
                    ImVec2(cell, cell),
                    ImVec2(0.0F, 1.0F), ImVec2(1.0F, 0.0F));
            } else {
                clicked = ImGui::Button(("?##pt" + std::to_string(i)).c_str(), ImVec2(cell, cell));
            }
            if (selected) {
                ImGui::PopStyleColor();
            }
            if (clicked) {
                ui.selected_city_block = i;
            }
            if (i < names.size()) {
                ImGui::TextUnformatted(compact_asset_name(names[i]).c_str());
            }
            if (ImGui::IsItemHovered() && i < names.size()) {
                ImGui::SetTooltip("%s", names[i].c_str());
            }
            ImGui::EndGroup();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void draw_building_inspector(EditorProject& project, EditorUi& ui, std::string& status) {
    if (project.buildings.empty()) {
        ImGui::TextUnformatted("No building templates loaded.");
        return;
    }

    // When the Place Block tool is active, the inspector becomes the block panel
    // (the editor.exe layout) instead of the per-cell field editor.
    if (ui.building_tool == BuildingTool::PlaceBlock) {
        draw_block_tool_panel(project, ui);
        return;
    }

    ui.selected_building = std::min(ui.selected_building, project.buildings.size() - 1U);
    auto& asset = project.buildings[ui.selected_building];
    auto& building = asset.data;
    const auto occupied = occupied_cell_indices(building);

    ImGui::Text("Building: %s", asset.name.c_str());
    if (ImGui::BeginTable("##building_summary", 2, ImGuiTableFlags_SizingStretchProp)) {
        property_row("Size", std::to_string(building.size_x) + " x " + std::to_string(building.size_y) + " x " + std::to_string(building.size_z));
        property_row("Anchor", std::to_string(building.anchor_x) + ", " + std::to_string(building.anchor_y) + ", " + std::to_string(building.anchor_z));
        property_row("Occupied", std::to_string(occupied.size()) + " / " + std::to_string(building.cells.size()));
        ImGui::EndTable();
    }

    if (occupied.empty()) {
        ImGui::SeparatorText("Selection");
        ImGui::TextUnformatted("No occupied cells.");
        return;
    }

    ui.selected_building_cell = std::min(ui.selected_building_cell, occupied.size() - 1U);
    auto& cell = building.cells[occupied[ui.selected_building_cell]];

    ImGui::SeparatorText("Selected Cell");
    if (ImGui::BeginTable("##cell_identity", 2, ImGuiTableFlags_SizingStretchProp)) {
        property_row("Index", std::to_string(cell.index));
        property_row("Coords", std::to_string(cell.x) + ", " + std::to_string(cell.y) + ", " + std::to_string(cell.z));
        property_row("Placed", std::to_string(cell.placed_flag()));
        ImGui::EndTable();
    }

    const char* fields[] = {
        "Exterior Block",
        "Interior Block",
        "Cell Type",
        "Exterior Portal Face",
        "Interior Portal Face",
        "Placed Flag",
        "Itemset",
    };
    int active = static_cast<int>(std::min<std::size_t>(ui.selected_building_field, std::size(fields) - 1U));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("Edit Field", &active, fields, static_cast<int>(std::size(fields)))) {
        ui.selected_building_field = static_cast<std::size_t>(active);
    }
    auto& active_value = building_cell_field(cell, ui.selected_building_field);
    if (input_u32(building_cell_field_label(ui.selected_building_field), active_value)) {
        mark_building_changed(project, ui);
    }
    if (ImGui::Button("-1")) nudge_building_cell(project, ui, -1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decrement selected field (Left)");
    ImGui::SameLine();
    if (ImGui::Button("+1")) nudge_building_cell(project, ui, 1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Increment selected field (Right)");
    ImGui::SameLine();
    if (ImGui::Button("+16")) nudge_building_cell(project, ui, 16);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Increment selected field by 16 (=)");

    if (ImGui::CollapsingHeader("Blocks", ImGuiTreeNodeFlags_DefaultOpen)) {
        draw_payload_table(building, cell);
    }
    if (ImGui::CollapsingHeader("Portal Faces", ImGuiTreeNodeFlags_DefaultOpen)) {
        draw_portal_side_table(project, building, cell);
    }
    if (ImGui::CollapsingHeader("Texture Sets", ImGuiTreeNodeFlags_DefaultOpen)) {
        draw_texture_slot_table(building, cell);
    }
    if (ImGui::CollapsingHeader("Itemset", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("##cell_itemset", 2, ImGuiTableFlags_SizingStretchProp)) {
            const auto item_name = cell.item_set_index < building.item_set_names.size()
                ? building.item_set_names[cell.item_set_index]
                : std::string{"(unresolved)"};
            property_row("Index", std::to_string(cell.item_set_index));
            property_row("Name", item_name);
            ImGui::EndTable();
        }
    }

    ImGui::Separator();
    if (ImGui::Button(ui.building_dirty ? "Save Building *" : "Save Building", ImVec2(-FLT_MIN, 35))) {
        save_building(project, ui, status);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save cell modifications to building/*.edited.sbb");
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

void draw_block_inspector(EditorProject& project, EditorUi& ui, std::string& status) {
    if (project.blocks.empty()) {
        ImGui::TextUnformatted("No block meshes loaded.");
        return;
    }

    ui.selected_block = std::min(ui.selected_block, project.blocks.size() - 1U);
    auto& asset = project.blocks[ui.selected_block];
    auto& block = asset.data;

    ImGui::Text("Block: %s", asset.name.c_str());
    if (ImGui::BeginTable("##block_summary", 2, ImGuiTableFlags_SizingStretchProp)) {
        property_row("Faces", std::to_string(block.face_count));
        property_row("Patches", std::to_string(block.authored_patches.size()));
        property_row("Lights", std::to_string(block.light_count));
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Dimensions");
    auto edit_dimension = [&](const char* label, std::uint32_t& value) {
        std::uint32_t next = value;
        if (input_u32(label, next)) {
            value = std::clamp(next, 1U, 0x40U);
            mark_block_changed(project, ui, block);
        }
    };
    if (ImGui::BeginTable("##block_dimensions", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        edit_dimension("Width", block.width);
        ImGui::TableNextColumn();
        edit_dimension("Height", block.height);
        ImGui::TableNextColumn();
        edit_dimension("Depth", block.depth);
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Exterior Flags");
    if (ImGui::BeginTable("##block_flags", 6, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextRow();
        for (std::size_t i = 0; i < block.exterior_flags.size(); ++i) {
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            std::uint32_t value = block.exterior_flags[i];
            if (input_u32(block_field_label(i + 3U), value)) {
                block.exterior_flags[i] = value;
                mark_block_changed(project, ui, block);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Patch Selection");
    if (block.authored_patches.empty()) {
        ImGui::TextUnformatted("No authored patches.");
    } else {
        ui.selected_block_patch = std::min(ui.selected_block_patch, block.authored_patches.size() - 1U);
        if (ImGui::BeginTable("##block_patches", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("#");
            ImGui::TableSetupColumn("Cols");
            ImGui::TableSetupColumn("Rows");
            ImGui::TableSetupColumn("Points");
            ImGui::TableSetupColumn("Offset");
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < block.authored_patches.size(); ++i) {
                const auto& patch = block.authored_patches[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(std::to_string(patch.index).c_str(), ui.selected_block_patch == i, ImGuiSelectableFlags_SpanAllColumns)) {
                    ui.selected_block_patch = i;
                    ui.selected_block_point = 0;
                }
                ImGui::TableNextColumn();
                ImGui::Text("%u/%u", patch.column_order, patch.column_tess);
                ImGui::TableNextColumn();
                ImGui::Text("%u/%u", patch.row_order, patch.row_tess);
                ImGui::TableNextColumn();
                ImGui::Text("%zu", patch.control_points.size());
                ImGui::TableNextColumn();
                ImGui::Text("%zu", patch.offset);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        auto& patch = block.authored_patches[ui.selected_block_patch];
        if (!patch.control_points.empty()) {
            ui.selected_block_point = std::min(ui.selected_block_point, patch.control_points.size() - 1U);
            auto& point = patch.control_points[ui.selected_block_point];
            ImGui::Text("Point %zu / %zu", ui.selected_block_point, patch.control_points.size());
            if (ImGui::BeginTable("##patch_point", 5, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("X %.3f", point.position.x);
                ImGui::TableNextColumn();
                ImGui::Text("Y %.3f", point.position.y);
                ImGui::TableNextColumn();
                ImGui::Text("Z %.3f", point.position.z);
                ImGui::TableNextColumn();
                ImGui::Text("U %.3f", point.u);
                ImGui::TableNextColumn();
                ImGui::Text("V %.3f", point.v);
                ImGui::EndTable();
            }
        }
    }

    const char* field_labels[] = {
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
    int active_field = static_cast<int>(std::min<std::size_t>(ui.selected_block_field, std::size(field_labels) - 1U));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("Nudge Field", &active_field, field_labels, static_cast<int>(std::size(field_labels)))) {
        ui.selected_block_field = static_cast<std::size_t>(active_field);
    }
    ImGui::Text("Selected Value: %s", block_field_value(block, ui).c_str());
    if (ImGui::Button("-1")) nudge_block(project, ui, -1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decrement (Left); Up/Down = -/+4, Tab cycles field");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decrement selected property");
    ImGui::SameLine();
    if (ImGui::Button("+1")) nudge_block(project, ui, 1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Increment selected property");
    ImGui::SameLine();
    if (ImGui::Button("+4")) nudge_block(project, ui, 4);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Increment selected property by 4");

    ImGui::Separator();
    if (ImGui::Button(ui.block_dirty ? "Save Block *" : "Save Block", ImVec2(-FLT_MIN, 35))) {
        save_block(project, ui, status);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save block parameters to block/*.edited.sbl");
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
    if (project.buildblocks.empty()) {
        ImGui::TextUnformatted("No buildblocks loaded.");
        return;
    }

    ui.selected_buildblock = std::min(ui.selected_buildblock, project.buildblocks.size() - 1U);
    auto& asset = project.buildblocks[ui.selected_buildblock];
    auto& buildblock = asset.data;
    ImGui::Text("Buildblock: %s", asset.name.c_str());
    if (ImGui::BeginTable("##buildblock_summary", 2, ImGuiTableFlags_SizingStretchProp)) {
        property_row("Layers", std::to_string(buildblock.layers.size()));
        property_row("Version", std::to_string(buildblock.version));
        property_row("Has Block Mesh", buildblock.block.has_value() ? "yes" : "no");
        ImGui::EndTable();
    }

    if (buildblock.layers.empty()) {
        ImGui::SeparatorText("Layer");
        ImGui::TextUnformatted("No layers.");
        return;
    }

    ui.selected_buildblock_layer = std::min(ui.selected_buildblock_layer, buildblock.layers.size() - 1U);
    auto& layer = buildblock.layers[ui.selected_buildblock_layer];
    if (!layer.doors.empty()) ui.selected_buildblock_door = std::min(ui.selected_buildblock_door, layer.doors.size() - 1U);
    if (!layer.windows.empty()) ui.selected_buildblock_window = std::min(ui.selected_buildblock_window, layer.windows.size() - 1U);
    if (!layer.beams.empty()) ui.selected_buildblock_beam = std::min(ui.selected_buildblock_beam, layer.beams.size() - 1U);

    ImGui::SeparatorText("Layers");
    if (ImGui::BeginTable("##buildblock_layers_table", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("Type");
        ImGui::TableSetupColumn("Doors");
        ImGui::TableSetupColumn("Windows");
        ImGui::TableSetupColumn("Beams");
        ImGui::TableHeadersRow();
        for (std::size_t i = 0; i < buildblock.layers.size(); ++i) {
            const auto& candidate = buildblock.layers[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(std::to_string(candidate.index).c_str(), ui.selected_buildblock_layer == i, ImGuiSelectableFlags_SpanAllColumns)) {
                ui.selected_buildblock_layer = i;
                ui.selected_buildblock_door = 0;
                ui.selected_buildblock_window = 0;
                ui.selected_buildblock_beam = 0;
            }
            ImGui::TableNextColumn();
            ImGui::Text("%u", candidate.portal_type);
            ImGui::TableNextColumn();
            ImGui::Text("%zu", candidate.doors.size());
            ImGui::TableNextColumn();
            ImGui::Text("%zu", candidate.windows.size());
            ImGui::TableNextColumn();
            ImGui::Text("%zu", candidate.beams.size());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Layer Properties");
    if (input_u32("Portal Type", layer.portal_type)) {
        mark_buildblock_changed(project, ui);
    }
    if (ImGui::BeginTable("##layer_bounds_table", 4, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextRow();
        for (std::size_t i = 0; i < layer.bounds.size(); ++i) {
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(i));
            if (drag_unit_float(buildblock_field_label(i), layer.bounds[i])) {
                mark_buildblock_changed(project, ui);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    int active_field = static_cast<int>(std::min<std::size_t>(ui.selected_buildblock_field, 16U));
    if (ImGui::Combo("Nudge Field", &active_field, buildblock_field_label_getter, nullptr, 17)) {
        ui.selected_buildblock_field = static_cast<std::size_t>(active_field);
    }
    if (ImGui::Button("-1/16")) nudge_buildblock_geometry(project, ui, -1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decrement (Left); Up/Down = -/+4/16, Tab cycles layer");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nudge selected geometry field -1/16 unit");
    ImGui::SameLine();
    if (ImGui::Button("+1/16")) nudge_buildblock_geometry(project, ui, 1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nudge selected geometry field +1/16 unit");
    ImGui::SameLine();
    if (ImGui::Button("+4/16")) nudge_buildblock_geometry(project, ui, 4);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Nudge selected geometry field +4/16 unit");

    if (ImGui::CollapsingHeader("Doors", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (layer.doors.empty()) {
            ImGui::TextUnformatted("No doors.");
        } else {
            ui.selected_buildblock_door = std::min(ui.selected_buildblock_door, layer.doors.size() - 1U);
            if (ImGui::BeginTable("##doors_table", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("#");
                ImGui::TableSetupColumn("X");
                ImGui::TableSetupColumn("Y");
                ImGui::TableSetupColumn("Type");
                ImGui::TableHeadersRow();
                for (std::size_t i = 0; i < layer.doors.size(); ++i) {
                    const auto& door = layer.doors[i];
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(std::to_string(i).c_str(), ui.selected_buildblock_door == i, ImGuiSelectableFlags_SpanAllColumns)) {
                        ui.selected_buildblock_door = i;
                    }
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", door.x);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.3f", door.y);
                    ImGui::TableNextColumn();
                    ImGui::Text("%u", door.type);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
    }
    if (ImGui::CollapsingHeader("Windows", ImGuiTreeNodeFlags_DefaultOpen)) {
        draw_rect_collection_table("##windows_table", layer.windows, ui.selected_buildblock_window, "Windows");
    }
    if (ImGui::CollapsingHeader("Beams", ImGuiTreeNodeFlags_DefaultOpen)) {
        draw_rect_collection_table("##beams_table", layer.beams, ui.selected_buildblock_beam, "Beams");
    }

    ImGui::Separator();
    if (ImGui::Button(ui.buildblock_dirty ? "Save Buildblock *" : "Save Buildblock", ImVec2(-FLT_MIN, 35))) {
        save_buildblock(project, ui, status);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save buildblock layers to buildblock/*.edited.sbl");
}

namespace {

// Font Awesome 6 Solid icon prefixes for tool buttons (the atlas merges FA in
// load_editor_fonts). Trailing space separates the glyph from the label.
#define ICON_SELECT ICON_FA_ARROW_POINTER " "
#define ICON_DRAW   ICON_FA_ROAD " "
#define ICON_MOVE   ICON_FA_UP_DOWN_LEFT_RIGHT " "
#define ICON_COPY   ICON_FA_COPY " "
#define ICON_PLACE  ICON_FA_CUBE " "
#define ICON_ERASE  ICON_FA_ERASER " "
#define ICON_PAINT  ICON_FA_FILL_DRIP " "

// One tool button: highlighted when active, sets the target on click. Returns
// true if it was clicked. Uniform width so the column reads as a tidy stack.
template <typename T>
bool tool_button(const char* label, const char* tip, T& target, T value) {
    const bool active = target == value;
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
    }
    const bool clicked = ImGui::Button(label, ImVec2(118.0F, 26.0F));
    if (active) {
        ImGui::PopStyleColor();
    }
    if (clicked) {
        target = value;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tip);
    }
    return clicked;
}

// Compact catalog combo: shows entries[selected], lets you pick another. Empty
// names render as their 1-based index so unnamed blocks/materials stay selectable.
} // namespace

bool draw_viewport_toolbar(EditorProject& project, EditorUi& ui, float origin_x, float origin_y) {
    // City and Buildings are the editors with viewport tools; other tabs have none.
    const bool city = ui.tab == EditorTab::City && project.city;
    const bool building = ui.tab == EditorTab::Buildings && !project.buildings.empty();
    if (!city && !building) {
        return false;
    }

    ImGui::SetNextWindowPos(ImVec2(origin_x + 12.0F, origin_y + 12.0F), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88F);
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    // Roomier padding + rounding so the overlay reads as a floating tool panel
    // rather than buttons jammed into the corner.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0F, 10.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0F, 6.0F));

    bool hovered = false;
    if (ImGui::Begin("##viewport_toolbar", nullptr, flags)) {
        hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

        if (city) {
            // Unified city tools (no Roads/Buildings/Pieces sub-mode).
            const auto prev_tool = ui.city_tool;
            tool_button(ICON_SELECT "Select", "Pick anything (building/road/node/piece); gizmo to move", ui.city_tool, CityTool::Select);
            tool_button(ICON_MOVE "Building", "Place the chosen building template on the ground", ui.city_tool, CityTool::Building);
            tool_button(ICON_DRAW "Road", "Draw road chains (snaps to nodes, splits roads)", ui.city_tool, CityTool::Road);
            tool_button(ICON_PLACE "Piece", "Place/draw the chosen piece (hold-drag)", ui.city_tool, CityTool::Piece);
            tool_button(ICON_ERASE "Erase", "Delete whatever you click (hold-drag erases pieces)", ui.city_tool, CityTool::Erase);
            if (ui.city_tool != prev_tool && ui.city_tool != CityTool::Road) {
                ui.pending_road_intersection.reset(); // leaving the road tool ends any chain
            }
        } else { // Buildings editor
            tool_button(ICON_SELECT "Select", "Pick a cell (raycast)", ui.building_tool, BuildingTool::Select);
            tool_button(ICON_PLACE "Place", "Add a block against the clicked face (click-drag for fast fill)", ui.building_tool, BuildingTool::PlaceBlock);
            tool_button(ICON_PAINT "Paint", "Paint the clicked face's texture", ui.building_tool, BuildingTool::PaintTexture);
            ImGui::Separator();
            auto& bld = project.buildings[std::min(ui.selected_building, project.buildings.size() - 1U)].data;
            if (ui.building_tool == BuildingTool::PlaceBlock) {
                ImGui::TextDisabled("Configure the block in\nthe Inspector, then click\na face to place it.");
            } else if (ui.building_tool == BuildingTool::PaintTexture) {
                int side = ui.paint_interior ? 1 : 0;
                const char* sides[] = {"Exterior", "Interior"};
                ImGui::SetNextItemWidth(118.0F);
                if (ImGui::Combo("##tb_side", &side, sides, 2)) {
                    ui.paint_interior = side == 1;
                }
                ImGui::TextDisabled("Texture:");
                material_swatch_picker(project, "tb_paint", bld.materials, ui.paint_material_index);
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    return hovered;
}

namespace {

struct ControlHint {
    std::string key;
    std::string action;
};

// Bindings for the active tab AND the active tool only - a small, context-aware
// set so the overlay never crowds the corner. Key labels come from key_label so
// they match the user's keyboard layout (e.g. the W position shows "Z" on AZERTY).
std::vector<ControlHint> controls_for(const EditorUi& ui) {
    const auto k = [](int key) { return key_label(key); };
    const std::string ctrl = k(KEY_LEFT_CONTROL);
    std::vector<ControlHint> hints;

    if (ui.tab == EditorTab::City) {
        switch (ui.city_tool) {
            case CityTool::Select:
                hints.push_back({"LMB", "select / gizmo"});
                hints.push_back({k(KEY_R), "rotate"});
                hints.push_back({k(KEY_DELETE), "delete"});
                break;
            case CityTool::Building:
                hints.push_back({"LMB", "place building"});
                hints.push_back({k(KEY_R), "rotate"});
                break;
            case CityTool::Road:
                hints.push_back({"LMB", "lay road"});
                hints.push_back({k(KEY_ESCAPE), "end chain"});
                break;
            case CityTool::Piece:
                hints.push_back({"LMB", "place (hold drag)"});
                hints.push_back({k(KEY_R), "rotate"});
                break;
            case CityTool::Erase:
                hints.push_back({"LMB", "delete (hold drag)"});
                break;
        }
    } else if (ui.tab == EditorTab::Buildings) {
        if (ui.building_tool == BuildingTool::PlaceBlock) {
            hints.push_back({"LMB", "grow block (hold drag)"});
            hints.push_back({k(KEY_TAB), "next cell"});
        } else if (ui.building_tool == BuildingTool::PaintTexture) {
            hints.push_back({"LMB", "paint face"});
        } else {
            hints.push_back({"LMB", "select cell"});
            hints.push_back({k(KEY_TAB), "next cell"});
            hints.push_back({k(KEY_LEFT) + "/" + k(KEY_RIGHT), "edit field"});
        }
    } else if (ui.tab == EditorTab::Blocks) {
        hints.push_back({k(KEY_LEFT) + "/" + k(KEY_RIGHT), "value -/+1"});
        hints.push_back({k(KEY_TAB), "cycle field"});
    } else if (ui.tab == EditorTab::Buildblocks) {
        hints.push_back({k(KEY_LEFT) + "/" + k(KEY_RIGHT), "-/+ 1/16"});
        hints.push_back({k(KEY_TAB), "cycle layer"});
    }
    return hints;
}

} // namespace

void draw_controls_overlay(const EditorUi& ui, float origin_x, float origin_y, float width, float height) {
    const auto hints = controls_for(ui);
    if (hints.empty()) {
        return;
    }

    auto* draw = ImGui::GetForegroundDrawList();
    const float pad = 9.0F;
    const float row_h = ImGui::GetTextLineHeight() + 8.0F;
    const float cap_h = ImGui::GetTextLineHeight() + 4.0F;
    const float gap = 8.0F; // between key cap and action text.

    // Measure: widest row = caps width + gap + action width.
    float panel_w = 0.0F;
    for (const auto& hint : hints) {
        const float cap_w = ImGui::CalcTextSize(hint.key.c_str()).x + 12.0F;
        const float act_w = ImGui::CalcTextSize(hint.action.c_str()).x;
        panel_w = std::max(panel_w, cap_w + gap + act_w);
    }
    panel_w += pad * 2.0F;
    const float panel_h = pad * 2.0F + row_h * static_cast<float>(hints.size());

    const float x1 = origin_x + width - 12.0F;
    const float y1 = origin_y + height - 12.0F;
    const float x0 = x1 - panel_w;
    const float y0 = y1 - panel_h;

    draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(20, 23, 27, 205), 7.0F);
    draw->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(80, 90, 102, 220), 7.0F);

    float y = y0 + pad;
    for (const auto& hint : hints) {
        const float cap_w = ImGui::CalcTextSize(hint.key.c_str()).x + 12.0F;
        const float cap_x = x0 + pad;
        const ImVec2 cap_min(cap_x, y + (row_h - cap_h) * 0.5F);
        const ImVec2 cap_max(cap_x + cap_w, cap_min.y + cap_h);
        // Key-cap shape: rounded filled rect with a lighter top edge.
        draw->AddRectFilled(cap_min, cap_max, IM_COL32(58, 64, 72, 255), 4.0F);
        draw->AddRect(cap_min, cap_max, IM_COL32(120, 130, 142, 255), 4.0F);
        const ImVec2 key_text_size = ImGui::CalcTextSize(hint.key.c_str());
        draw->AddText(
            ImVec2(cap_x + (cap_w - key_text_size.x) * 0.5F, cap_min.y + (cap_h - key_text_size.y) * 0.5F),
            IM_COL32(235, 240, 245, 255),
            hint.key.c_str());
        draw->AddText(
            ImVec2(cap_x + cap_w + gap, y + (row_h - ImGui::GetTextLineHeight()) * 0.5F),
            IM_COL32(205, 212, 220, 255),
            hint.action.c_str());
        y += row_h;
    }
}

void apply_editor_imgui_style() {
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(10.0F, 10.0F);
    style.FramePadding = ImVec2(9.0F, 6.0F);
    style.CellPadding = ImVec2(8.0F, 5.0F);
    style.ItemSpacing = ImVec2(8.0F, 7.0F);
    style.ItemInnerSpacing = ImVec2(6.0F, 5.0F);
    style.ScrollbarSize = 13.0F;
    style.GrabMinSize = 10.0F;
    style.WindowBorderSize = 1.0F;
    style.ChildBorderSize = 1.0F;
    style.PopupBorderSize = 1.0F;
    style.FrameBorderSize = 0.0F;
    style.TabBorderSize = 0.0F;
    style.WindowRounding = 5.0F;
    style.ChildRounding = 5.0F;
    style.FrameRounding = 4.0F;
    style.PopupRounding = 5.0F;
    style.ScrollbarRounding = 8.0F;
    style.GrabRounding = 4.0F;
    style.TabRounding = 4.0F;

    auto& colors = style.Colors;
    colors[ImGuiCol_Text] = ImVec4(0.89F, 0.91F, 0.91F, 1.00F);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.43F, 0.48F, 0.50F, 1.00F);
    colors[ImGuiCol_WindowBg] = ImVec4(0.075F, 0.083F, 0.090F, 1.00F);
    colors[ImGuiCol_ChildBg] = ImVec4(0.055F, 0.061F, 0.067F, 1.00F);
    colors[ImGuiCol_PopupBg] = ImVec4(0.070F, 0.078F, 0.086F, 0.98F);
    colors[ImGuiCol_Border] = ImVec4(0.18F, 0.22F, 0.24F, 1.00F);
    colors[ImGuiCol_FrameBg] = ImVec4(0.12F, 0.14F, 0.15F, 1.00F);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.18F, 0.22F, 0.24F, 1.00F);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.24F, 0.30F, 0.33F, 1.00F);
    colors[ImGuiCol_TitleBg] = ImVec4(0.060F, 0.066F, 0.073F, 1.00F);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.10F, 0.12F, 0.13F, 1.00F);
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.055F, 0.061F, 0.067F, 1.00F);
    colors[ImGuiCol_Button] = ImVec4(0.18F, 0.25F, 0.28F, 1.00F);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.24F, 0.34F, 0.38F, 1.00F);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.32F, 0.48F, 0.52F, 1.00F);
    colors[ImGuiCol_Header] = ImVec4(0.18F, 0.28F, 0.31F, 1.00F);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.25F, 0.39F, 0.43F, 1.00F);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.31F, 0.50F, 0.55F, 1.00F);
    colors[ImGuiCol_Separator] = ImVec4(0.22F, 0.27F, 0.29F, 1.00F);
    colors[ImGuiCol_ResizeGrip] = ImVec4(0.24F, 0.34F, 0.38F, 0.65F);
    colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.31F, 0.50F, 0.55F, 0.85F);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.40F, 0.64F, 0.68F, 1.00F);
    colors[ImGuiCol_Tab] = ImVec4(0.12F, 0.15F, 0.16F, 1.00F);
    colors[ImGuiCol_TabHovered] = ImVec4(0.25F, 0.39F, 0.43F, 1.00F);
    colors[ImGuiCol_TabActive] = ImVec4(0.20F, 0.31F, 0.34F, 1.00F);
    colors[ImGuiCol_DockingPreview] = ImVec4(0.34F, 0.62F, 0.66F, 0.70F);
}

void load_default_imgui_layout() {
    const auto dockspace = ImHashStr("SRCMapEditorDockSpace");
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodePos(dockspace, viewport->WorkPos);
    ImGui::DockBuilderSetNodeSize(dockspace, viewport->WorkSize);

    ImGuiID center = dockspace;
    const auto left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22F, nullptr, &center);
    const auto right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.26F, nullptr, &center);
    const auto bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.18F, nullptr, &center);

    ImGui::DockBuilderDockWindow("Asset Browser", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Console / Status", bottom);
    ImGui::DockBuilderDockWindow("History", bottom);
    ImGui::DockBuilderDockWindow("Viewport 3D", center);
    ImGui::DockBuilderDockWindow("2D Preview", center);
    ImGui::DockBuilderDockWindow("Buildblock 2D Preview", center);
    ImGui::DockBuilderFinish(dockspace);
}

void draw_editor_dockspace(bool reset_layout) {
    if (reset_layout) {
        load_default_imgui_layout();
    }
    ImGui::DockSpaceOverViewport(ImHashStr("SRCMapEditorDockSpace"), ImGui::GetMainViewport());
}

} // namespace map_editor::app_internal
