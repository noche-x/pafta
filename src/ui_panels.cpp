#include "app_internal.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdint>
#include <cmath>
#include <iomanip>
#include <limits>
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

std::string clipped_label(std::string_view value, std::size_t max_chars) {
    if (value.size() <= max_chars) {
        return std::string{value};
    }
    if (max_chars <= 3U) {
        return std::string{value.substr(0, max_chars)};
    }
    std::string clipped{value.substr(0, max_chars - 3U)};
    clipped += "...";
    return clipped;
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

void draw_buildblock_thumbnail(
    ImDrawList* draw_list,
    const SblTemplate& buildblock,
    ImVec2 min,
    ImVec2 max,
    bool selected,
    bool hovered,
    std::string_view label) {
    const auto bg = selected ? IM_COL32(40, 58, 61, 255) : (hovered ? IM_COL32(30, 38, 41, 255) : IM_COL32(20, 23, 25, 255));
    const auto border = selected ? IM_COL32(255, 225, 80, 255) : IM_COL32(67, 78, 83, 255);
    draw_list->AddRectFilled(min, max, bg, 5.0F);
    draw_list->AddRect(min, max, border, 5.0F, 0, selected ? 2.0F : 1.0F);

    const auto canvas_min = ImVec2{min.x + 8.0F, min.y + 8.0F};
    const auto canvas_max = ImVec2{max.x - 8.0F, max.y - 28.0F};
    draw_list->AddRectFilled(canvas_min, canvas_max, IM_COL32(13, 15, 17, 255), 3.0F);

    const auto scene = build_buildblock_preview_scene(buildblock);
    float min_x = std::numeric_limits<float>::max();
    float min_y = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float max_y = std::numeric_limits<float>::lowest();
    auto include_point = [&](float x, float y) {
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
    };
    for (const auto& rect : scene.rects) {
        include_point(rect.center.x - rect.size.x * 0.5F, rect.center.y - rect.size.y * 0.5F);
        include_point(rect.center.x + rect.size.x * 0.5F, rect.center.y + rect.size.y * 0.5F);
    }
    for (const auto& door : scene.doors) {
        include_point(door.position.x - 0.25F, door.position.y - 0.25F);
        include_point(door.position.x + 0.25F, door.position.y + 0.25F);
    }
    if (min_x > max_x || min_y > max_y) {
        min_x = 0.0F;
        min_y = 0.0F;
        max_x = 4.0F;
        max_y = 4.0F;
    }

    const auto range_x = std::max(0.001F, max_x - min_x);
    const auto range_y = std::max(0.001F, max_y - min_y);
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
            origin.x + (x - min_x) * scale,
            origin.y + (max_y - y) * scale,
        };
    };

    auto rect_color = [](BuildblockPreviewRectKind kind) {
        switch (kind) {
            case BuildblockPreviewRectKind::Bounds:
                return IM_COL32(63, 183, 184, 95);
            case BuildblockPreviewRectKind::Window:
                return IM_COL32(92, 154, 222, 145);
            case BuildblockPreviewRectKind::Beam:
                return IM_COL32(216, 136, 68, 150);
        }
        return IM_COL32(160, 160, 160, 130);
    };
    auto rect_border_color = [](BuildblockPreviewRectKind kind) {
        switch (kind) {
            case BuildblockPreviewRectKind::Bounds:
                return IM_COL32(63, 183, 184, 255);
            case BuildblockPreviewRectKind::Window:
                return IM_COL32(92, 154, 222, 255);
            case BuildblockPreviewRectKind::Beam:
                return IM_COL32(216, 136, 68, 255);
        }
        return IM_COL32(160, 160, 160, 255);
    };

    for (const auto& rect : scene.rects) {
        const auto a = map_point(rect.center.x - rect.size.x * 0.5F, rect.center.y + rect.size.y * 0.5F);
        const auto b = map_point(rect.center.x + rect.size.x * 0.5F, rect.center.y - rect.size.y * 0.5F);
        draw_list->AddRectFilled(a, b, rect_color(rect.kind), 1.0F);
        draw_list->AddRect(a, b, rect_border_color(rect.kind), 1.0F);
    }
    for (const auto& door : scene.doors) {
        const auto center = map_point(door.position.x, door.position.y);
        draw_list->AddCircleFilled(center, 3.5F, IM_COL32(180, 108, 225, 230));
        draw_list->AddCircle(center, 3.5F, IM_COL32(245, 245, 245, 230));
    }

    const auto visible_label = clipped_label(label, 22U);
    draw_list->AddText(
        ImGui::GetFont(),
        ImGui::GetFontSize(),
        ImVec2{min.x + 8.0F, max.y - 22.0F},
        IM_COL32(220, 228, 229, 255),
        visible_label.c_str(),
        visible_label.c_str() + visible_label.size(),
        max.x - min.x - 16.0F);
}

void draw_building_asset_panel(EditorProject& project, EditorUi& ui) {
    const auto names = building_names(project);
    ImGui::Text("Buildings (*.sbb)");
    const auto building_list_height = std::max(130.0F, ImGui::GetContentRegionAvail().y * 0.34F);
    if (ImGui::BeginChild("##building_templates_panel", ImVec2(0, building_list_height), true)) {
        if (ImGui::BeginTable("##building_templates_table", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Cells", ImGuiTableColumnFlags_WidthFixed, 54.0F);
            ImGui::TableHeadersRow();
            for (std::size_t i = 0; i < project.buildings.size(); ++i) {
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
    const auto spacing = ImGui::GetStyle().ItemSpacing.x;
    const auto avail_x = std::max(1.0F, ImGui::GetContentRegionAvail().x);
    const auto min_tile_w = 118.0F;
    const auto columns = std::max(1, static_cast<int>((avail_x + spacing) / (min_tile_w + spacing)));
    const auto tile_w = std::floor((avail_x - spacing * static_cast<float>(columns - 1)) / static_cast<float>(columns));
    const auto tile_h = 126.0F;
    if (ImGui::BeginChild("##buildblock_gallery_panel", ImVec2(0, 0), true)) {
        for (std::size_t i = 0; i < project.buildblocks.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const auto min = ImGui::GetCursorScreenPos();
            const auto size = ImVec2{tile_w, tile_h};
            ImGui::InvisibleButton("##buildblock_thumb", size);
            if (ImGui::IsItemClicked()) {
                ui.selected_buildblock = i;
                ui.selected_buildblock_layer = 0;
                ui.selected_buildblock_door = 0;
                ui.selected_buildblock_window = 0;
                ui.selected_buildblock_beam = 0;
                ui.selected_buildblock_field = 0;
            }
            draw_buildblock_thumbnail(
                ImGui::GetWindowDrawList(),
                project.buildblocks[i].data,
                min,
                ImVec2{min.x + size.x, min.y + size.y},
                ui.selected_buildblock == i,
                ImGui::IsItemHovered(),
                project.buildblocks[i].name);
            if ((static_cast<int>(i) + 1) % columns != 0) {
                ImGui::SameLine();
            }
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
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Decrement selected field");
    ImGui::SameLine();
    if (ImGui::Button("+1")) nudge_building_cell(project, ui, 1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Increment selected field");
    ImGui::SameLine();
    if (ImGui::Button("+16")) nudge_building_cell(project, ui, 16);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Increment selected field by 16");

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
    static constexpr char default_layout[] = R"ini(
[Window][WindowOverViewport_11111111]
Pos=0,19
Size=1440,881
Collapsed=0

[Window][Debug##Default]
Pos=60,60
Size=400,400
Collapsed=0

[Window][Viewport 3D]
Pos=322,19
Size=756,663
Collapsed=0
DockId=0x00000005,0

[Window][Asset Browser]
Pos=0,19
Size=320,881
Collapsed=0
DockId=0x00000001,0

[Window][Console / Status]
Pos=322,684
Size=756,216
Collapsed=0
DockId=0x00000006,0

[Window][Inspector]
Pos=1080,19
Size=360,881
Collapsed=0
DockId=0x00000004,0

[Window][2D Preview]
Pos=322,19
Size=756,663
Collapsed=0
DockId=0x00000005,1

[Docking][Data]
DockSpace       ID=0x08BD597D Window=0x1BBC0F80 Pos=0,19 Size=1440,881 Split=X Selected=0x13DC1525
  DockNode      ID=0x00000003 Parent=0x08BD597D SizeRef=1080,881 Split=X
    DockNode    ID=0x00000001 Parent=0x00000003 SizeRef=320,881 Selected=0x36AF052B
    DockNode    ID=0x00000002 Parent=0x00000003 SizeRef=756,881 Split=Y Selected=0x13DC1525
      DockNode  ID=0x00000005 Parent=0x00000002 SizeRef=756,663 CentralNode=1 Selected=0x13DC1525
      DockNode  ID=0x00000006 Parent=0x00000002 SizeRef=756,216 Selected=0x8C63CC03
  DockNode      ID=0x00000004 Parent=0x08BD597D SizeRef=360,881 Selected=0x36DC96AB
)ini";
    ImGui::LoadIniSettingsFromMemory(default_layout, sizeof(default_layout) - 1U);
}

} // namespace map_editor::app_internal
