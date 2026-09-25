// Context-aware workspace windows: Tools, Library, and Properties. Each is a
// normal floating/dockable ImGui window whose contents follow the current mode,
// tool, and selection.
#include "app_internal.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <string>

namespace map_editor::app_internal {
namespace {

const char* mode_name(EditorTab tab) {
    switch (tab) {
        case EditorTab::City: return "City";
        case EditorTab::Buildings: return "Building";
        case EditorTab::Pieces: return "Pieces";
        case EditorTab::Buildblocks: return "Buildblock";
        case EditorTab::Blocks: return "Block mesh";
    }
    return "";
}

// Tooltip with the action's current shortcut appended.
void help_tooltip(const EditorUi& ui, const char* text, std::optional<EditorAction> action = std::nullopt) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) return;
    if (action.has_value()) {
        ImGui::SetTooltip("%s  [%s]", text, action_shortcut_label(ui, *action).c_str());
    } else {
        ImGui::SetTooltip("%s", text);
    }
}

void draw_placed_list(EditorProject& project, EditorUi& ui, bool roads) {
    auto& search = ui.asset_search[static_cast<std::size_t>(EditorTab::City)];
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##placed_search", "Filter by name", search.data(), search.size());
    const auto names = roads ? city_street_names(project.city->data) : city_building_names(project.city->data);
    std::vector<std::size_t> visible;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (asset_search_matches(ui, names[i])) visible.push_back(i);
    }
    ImGui::TextDisabled("%zu of %zu. Click selects, double-click edits.", visible.size(), names.size());
    // When the selection changes (e.g. picked in the viewport), scroll the list
    // to it; duplicate names made the selected row easy to miss off-screen.
    const bool list_selected = roads ? ui.city_selection == CitySelectionKind::Road : ui.city_selection == CitySelectionKind::Building;
    const auto selected_index = roads ? ui.selected_city_street : ui.selected_city_building;
    static std::optional<std::size_t> last_scrolled[2];
    auto& last = last_scrolled[roads ? 1 : 0];
    int scroll_row = -1;
    if (list_selected && last != selected_index) {
        const auto found = std::find(visible.begin(), visible.end(), selected_index);
        if (found != visible.end()) scroll_row = static_cast<int>(found - visible.begin());
        last = selected_index;
    } else if (!list_selected) {
        last.reset();
    }
    if (ImGui::BeginListBox(roads ? "##roads" : "##placed_bldgs", ImVec2(-FLT_MIN, -FLT_MIN))) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visible.size()));
        if (scroll_row >= 0) clipper.IncludeItemByIndex(scroll_row);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto i = visible[static_cast<std::size_t>(row)];
                ImGui::PushID(static_cast<int>(i));
                if (row == scroll_row) ImGui::SetScrollHereY(0.5F);
                if (roads) {
                    const bool active = ui.city_selection == CitySelectionKind::Road && ui.selected_city_street == i;
                    if (ImGui::Selectable(names[i].c_str(), active)) {
                        ui.selected_city_street = i;
                        ui.city_selection = CitySelectionKind::Road;
                        ui.city_tool = CityTool::Select;
                    }
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        ui.focus_selection_requested = true;
                    }
                } else {
                    const bool in_group = std::find(ui.selected_city_buildings.begin(), ui.selected_city_buildings.end(), i) != ui.selected_city_buildings.end();
                    const bool active = ui.city_selection == CitySelectionKind::Building && (ui.selected_city_building == i || in_group);
                    if (ImGui::Selectable(names[i].c_str(), active)) {
                        ui.selected_city_building = i;
                        ui.city_selection = CitySelectionKind::Building;
                        ui.city_tool = CityTool::Select;
                        if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift) {
                            toggle_in_selection(ui.selected_city_buildings, i);
                        } else {
                            ui.selected_city_buildings = {i};
                        }
                    }
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        edit_city_building(project, ui, i);
                    }
                }
                ImGui::PopID();
            }
        }
        ImGui::EndListBox();
    }
}

} // namespace

namespace {

constexpr float kToolButton = 34.0F;  // every toolbar button is this square, so icons line up
const ImVec4 kPanelBg{0.08F, 0.08F, 0.09F, 0.94F};

// Square icon button with an "on" state and a tooltip naming its key.
bool tool_button(const char* icon, bool active, const EditorUi& ui, const char* tip, std::optional<EditorAction> action = std::nullopt) {
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const bool pressed = ImGui::Button(icon, ImVec2(kToolButton, kToolButton));
    if (active) ImGui::PopStyleColor();
    help_tooltip(ui, tip, action);
    return pressed;
}

void toolbar_divider() {
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();
}

// Horizontal strip of thumbnails. `thumb(i)` may return null while a thumbnail
// is still rendering. Returns the clicked index, if any.
template <typename Name, typename Thumb, typename Extra>
std::optional<std::size_t> thumbnail_strip(const char* id, std::size_t count, std::size_t selected, const std::array<char, 128>& search,
                                           Name name, Thumb thumb, Extra extra) {
    constexpr float cell = 88.0F;
    std::optional<std::size_t> clicked;
    if (ImGui::BeginChild(id, ImVec2(0.0F, 0.0F), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
        bool first = true;
        for (std::size_t i = 0; i < count; ++i) {
            const std::string& label = name(i);
            if (search[0] != '\0' && lower_name(label).find(lower_name(search.data())) == std::string::npos) continue;
            if (!first) ImGui::SameLine();
            first = false;
            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginGroup();
            const RenderTexture2D* texture = thumb(i);
            const bool hit = texture != nullptr && texture->texture.id != 0U
                ? ImGui::ImageButton("##t", ImTextureID(texture->texture.id), ImVec2(cell, cell), ImVec2(0.0F, 1.0F), ImVec2(1.0F, 0.0F))
                : ImGui::Button("...##t", ImVec2(cell + 6.0F, cell + 6.0F));
            if (hit) clicked = i;
            if (i == selected) {
                ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(235, 190, 78, 255), 3.0F, 0, 3.0F);
            }
            extra(i);
            ImGui::TextUnformatted(fit_label(label, cell).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", label.c_str());
            ImGui::EndGroup();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    return clicked;
}

// One-line hint for the active tool, drawn under the toolbar.
const char* tool_hint(const EditorUi& ui) {
    if (ui.tab == EditorTab::Buildings) {
        switch (ui.building_tool) {
            case BuildingTool::Select: return "Click a block to select it. Delete removes it; Tab cycles blocks.";
            case BuildingTool::PlaceBlock: return "Click a face to add a block against it; drag to keep adding.";
            case BuildingTool::PaintTexture: return "Pick a material in Properties, then click or drag over faces.";
            case BuildingTool::Anchor: return "Drag an arrow to move the building's anchor. Esc cancels.";
        }
        return "";
    }
    switch (ui.city_tool) {
        case CityTool::Select: return "Click to select, drag a box for many. Alt+drag orbits, middle-drag pans, wheel zooms.";
        case CityTool::Building: return "Pick a building below; the outline shows where it lands. R turns it, click to place.";
        case CityTool::Road: return ui.pending_road_start ? "Click to lay the next straight segment. Esc or clicking the end finishes."
                                                          : "Click to start a road.";
        case CityTool::Piece: return "Pick a piece below; the ghost shows where it lands. R turns it, click or drag to stamp.";
        case CityTool::Erase: return "Click or drag over the red-highlighted building, road, or piece to delete it.";
    }
    return "";
}

} // namespace

// Floating toolbar across the top of the viewport and the asset drawer along
// its bottom. They are child windows of the scene canvas, so the mouse over
// them never reaches the scene.
void draw_viewport_overlays(EditorProject* project, EditorUi& ui, Camera3D& camera, std::string& status, bool& open_folder_requested) {
    const ImVec2 origin = ImGui::GetWindowPos();
    const ImVec2 extent = ImGui::GetWindowSize();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanelBg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0F, 6.0F));

    // Toolbar, centred using last frame's width.
    static float toolbar_width = 600.0F;
    ImGui::SetCursorScreenPos(ImVec2(origin.x + std::max(8.0F, (extent.x - toolbar_width) * 0.5F), origin.y + 8.0F));
    constexpr ImGuiChildFlags kFit = ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding;
    if (ImGui::BeginChild("##toolbar", ImVec2(0.0F, 0.0F), kFit, ImGuiWindowFlags_NoScrollbar)) {
        if (project == nullptr) {
            if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open a map folder", ImVec2(0.0F, kToolButton))) open_folder_requested = true;
        } else {
            // Mode.
            if (tool_button(ICON_FA_CITY, ui.tab == EditorTab::City, ui, "City mode", EditorAction::TabCity) && ui.tab != EditorTab::City) {
                ui.requested_tab = EditorTab::City;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(project->buildings.empty());
            if (tool_button(ICON_FA_BUILDING, ui.tab == EditorTab::Buildings, ui, "Building mode: edit a building template", EditorAction::TabBuildings) &&
                ui.tab != EditorTab::Buildings) {
                ui.requested_tab = EditorTab::Buildings;
            }
            ImGui::EndDisabled();
            toolbar_divider();
            if (ui.tab == EditorTab::City && project->city) {
                struct Tool { const char* icon; CityTool tool; const char* tip; EditorAction action; };
                constexpr Tool tools[] = {
                    {ICON_FA_ARROW_POINTER, CityTool::Select, "Select", EditorAction::ToolSelect},
                    {ICON_FA_HOUSE, CityTool::Building, "Place building", EditorAction::ToolBuilding},
                    {ICON_FA_ROAD, CityTool::Road, "Lay road", EditorAction::ToolRoad},
                    {ICON_FA_CUBE, CityTool::Piece, "Stamp piece", EditorAction::ToolPiece},
                    {ICON_FA_ERASER, CityTool::Erase, "Erase", EditorAction::ToolErase},
                };
                for (std::size_t t = 0; t < std::size(tools); ++t) {
                    if (t != 0) ImGui::SameLine();
                    if (tool_button(tools[t].icon, ui.city_tool == tools[t].tool, ui, tools[t].tip, tools[t].action)) {
                        ui.city_tool = tools[t].tool;
                        if (ui.city_tool != CityTool::Road) ui.pending_road_start.reset();
                    }
                }
                toolbar_divider();
                if (tool_button(ICON_FA_EYE, ui.render_roads, ui, "Game preview: terrain and finished roads")) ui.render_roads = true;
                ImGui::SameLine();
                if (tool_button(ICON_FA_PEN_RULER, !ui.render_roads, ui, "Editor guide: road network only")) ui.render_roads = false;
                ImGui::SameLine();
                if (tool_button(ICON_FA_CROSSHAIRS, false, ui, "Frame the selection", EditorAction::FocusSelection)) focus_current_selection(*project, ui, camera);
                ImGui::SameLine();
                if (tool_button(ICON_FA_EXPAND, false, ui, "Show the whole city", EditorAction::FrameCity)) focus_camera_on_city(camera, *project);
            } else if (ui.tab == EditorTab::City) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("No city layout. Use Assets > New city.");
            } else if (ui.tab == EditorTab::Buildings && !project->buildings.empty()) {
                struct Tool { const char* icon; BuildingTool tool; const char* tip; };
                constexpr Tool tools[] = {
                    {ICON_FA_ARROW_POINTER, BuildingTool::Select, "Select block"},
                    {ICON_FA_SQUARE_PLUS, BuildingTool::PlaceBlock, "Add block"},
                    {ICON_FA_PAINTBRUSH, BuildingTool::PaintTexture, "Paint faces"},
                    {ICON_FA_ANCHOR, BuildingTool::Anchor, "Move anchor"},
                };
                const EditorAction keys[] = {EditorAction::ToolSelect, EditorAction::ToolBuilding, EditorAction::ToolRoad, EditorAction::ToolPiece};
                for (std::size_t t = 0; t < std::size(tools); ++t) {
                    if (t != 0) ImGui::SameLine();
                    if (tool_button(tools[t].icon, ui.building_tool == tools[t].tool, ui, tools[t].tip, keys[t])) ui.building_tool = tools[t].tool;
                }
                toolbar_divider();
                // Whole building, or one floor stepped with -/+.
                ui.selected_building = std::min(ui.selected_building, project->buildings.size() - 1U);
                const int max_floor = std::max(0, static_cast<int>(project->buildings[ui.selected_building].data.size_z) - 1);
                if (tool_button(ICON_FA_LAYER_GROUP, ui.building_view_floor_only, ui, "Show one floor at a time")) {
                    ui.building_view_floor_only = !ui.building_view_floor_only;
                    project->building_floor_preview_dirty = true;
                }
                if (ui.building_view_floor_only) {
                    ui.building_view_floor = std::clamp(ui.building_view_floor, 0, max_floor);
                    ImGui::SameLine();
                    if (ImGui::Button(ICON_FA_MINUS, ImVec2(kToolButton, kToolButton)) && ui.building_view_floor > 0) {
                        --ui.building_view_floor;
                        project->building_floor_preview_dirty = true;
                    }
                    ImGui::SameLine();
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("Floor %d/%d", ui.building_view_floor, max_floor);
                    ImGui::SameLine();
                    if (ImGui::Button(ICON_FA_PLUS, ImVec2(kToolButton, kToolButton)) && ui.building_view_floor < max_floor) {
                        ++ui.building_view_floor;
                        project->building_floor_preview_dirty = true;
                    }
                }
                ImGui::SameLine();
                if (tool_button(ICON_FA_TABLE_CELLS, ui.show_buildblock_window, ui, "Buildblock editor", EditorAction::WindowBuildblock)) {
                    ui.show_buildblock_window = !ui.show_buildblock_window;
                }
            }
            toolbar_divider();
            if (tool_button(ICON_FA_ROTATE_LEFT, false, ui, "Undo", EditorAction::Undo)) undo_edit(*project, ui, status, false);
            ImGui::SameLine();
            if (tool_button(ICON_FA_ROTATE_RIGHT, false, ui, "Redo", EditorAction::Redo)) undo_edit(*project, ui, status, true);
        }
        toolbar_width = ImGui::GetWindowWidth();
        const ImVec2 bar_min = ImGui::GetWindowPos();
        const float bar_bottom = bar_min.y + ImGui::GetWindowHeight();
        ImGui::EndChild();
        if (project != nullptr) {
            const char* hint = tool_hint(ui);
            const ImVec2 size = ImGui::CalcTextSize(hint);
            const ImVec2 at{origin.x + (extent.x - size.x) * 0.5F, bar_bottom + 6.0F};
            auto* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(ImVec2(at.x - 8.0F, at.y - 3.0F), ImVec2(at.x + size.x + 8.0F, at.y + size.y + 3.0F), IM_COL32(0, 0, 0, 150), 4.0F);
            draw->AddText(at, IM_COL32(225, 225, 225, 255), hint);
        }
    } else {
        ImGui::EndChild();
    }

    // Asset drawer: only where a tool picks from assets.
    const bool drawer_buildings = project && ((ui.tab == EditorTab::City && project->city && ui.city_tool == CityTool::Building) ||
                                              ui.tab == EditorTab::Buildings) && !project->buildings.empty();
    const bool drawer_pieces = project && ui.tab == EditorTab::City && project->city && ui.city_tool == CityTool::Piece && !project->blocks.empty();
    if (ui.show_asset_drawer && (drawer_buildings || drawer_pieces)) {
        const float height = ui.asset_drawer_open ? 176.0F : 42.0F;
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 8.0F, origin.y + extent.y - height - 8.0F));
        if (ImGui::BeginChild("##asset_drawer", ImVec2(extent.x - 16.0F, height), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar)) {
            static std::array<char, 128> building_search{};
            static std::array<char, 128> piece_search{};
            auto& search = drawer_pieces ? piece_search : building_search;
            if (ImGui::Button(ui.asset_drawer_open ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_UP, ImVec2(28.0F, 0.0F))) {
                ui.asset_drawer_open = !ui.asset_drawer_open;
            }
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(drawer_pieces ? "Pieces" : ui.tab == EditorTab::Buildings ? "Edit which building" : "Buildings");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200.0F);
            ImGui::InputTextWithHint("##drawer_search", ICON_FA_MAGNIFYING_GLASS " Search", search.data(), search.size());
            if (drawer_pieces) {
                ImGui::SameLine();
                if (!project->textures.empty()) {
                    ui.place_piece_texture = std::min(ui.place_piece_texture, project->textures.size() - 1U);
                    ImGui::SetNextItemWidth(160.0F);
                    if (ImGui::BeginCombo("##piece_texture", project->textures[ui.place_piece_texture].name.c_str())) {
                        for (std::size_t i = 0; i < project->textures.size(); ++i) {
                            if (ImGui::Selectable(project->textures[i].name.c_str(), ui.place_piece_texture == i)) ui.place_piece_texture = i;
                        }
                        ImGui::EndCombo();
                    }
                    help_tooltip(ui, "Texture the stamped piece uses");
                    ImGui::SameLine();
                }
                if (ImGui::Button(ICON_FA_ROTATE)) ui.selected_piece_rotation = (ui.selected_piece_rotation + 1) & 3;
                help_tooltip(ui, "Turn the piece", EditorAction::RotateSelection);
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%zu deg", ui.selected_piece_rotation * 90);
            } else if (ui.tab == EditorTab::City) {
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_ROTATE)) ui.place_building_rotation = (ui.place_building_rotation + 1) & 3;
                help_tooltip(ui, "Turn the building", EditorAction::RotateSelection);
                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%d deg", ui.place_building_rotation * 90);
            }
            if (ui.asset_drawer_open) {
                if (drawer_pieces) {
                    ui.place_piece_mesh = std::min(ui.place_piece_mesh, project->blocks.size() - 1U);
                    const auto picked = thumbnail_strip("##piece_strip", project->blocks.size(), ui.place_piece_mesh, search,
                        [&](std::size_t i) -> const std::string& { return project->blocks[i].name; },
                        [&](std::size_t i) { return block_thumbnail(*project, i); },
                        [](std::size_t) {});
                    if (picked) ui.place_piece_mesh = *picked;
                } else {
                    const bool editing = ui.tab == EditorTab::Buildings;
                    const auto current = editing ? ui.selected_building : ui.place_building_template;
                    const auto picked = thumbnail_strip("##building_strip", project->buildings.size(), current, search,
                        [&](std::size_t i) -> const std::string& { return project->buildings[i].name; },
                        [&](std::size_t i) { return building_thumbnail(*project, i); },
                        [&](std::size_t i) {
                            if (editing) return;
                            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) edit_building(*project, ui, i);
                            if (ImGui::BeginDragDropSource()) {
                                ImGui::SetDragDropPayload("MAP_BUILDING", &i, sizeof(i));
                                ImGui::TextUnformatted(project->buildings[i].name.c_str());
                                ImGui::EndDragDropSource();
                            }
                        });
                    if (picked) {
                        if (editing) edit_building(*project, ui, *picked);
                        else ui.place_building_template = *picked;
                    }
                }
            }
        }
        ImGui::EndChild();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// Outliner: what is in the map (placed buildings and roads), docked above
// Properties.
void draw_outliner_window(EditorProject* project, EditorUi& ui) {
    if (!ui.show_library_window) return;
    if (!ImGui::Begin(ICON_FA_LIST " Outliner###Outliner", &ui.show_library_window)) {
        ImGui::End();
        return;
    }
    if (project == nullptr || !project->city) {
        ImGui::TextDisabled("The map's buildings and roads are listed here.");
    } else if (ImGui::BeginTabBar("##outliner")) {
        if (ImGui::BeginTabItem("Buildings")) {
            draw_placed_list(*project, ui, false);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Roads")) {
            draw_placed_list(*project, ui, true);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void draw_properties_window(EditorProject* project, EditorUi& ui, std::string& status) {
    if (!ui.show_properties_window) return;
    std::string context;
    if (project != nullptr && project->city && ui.tab == EditorTab::City) {
        switch (ui.city_selection) {
            case CitySelectionKind::Building:
                if (ui.selected_city_building < project->city->data.buildings.size()) {
                    context = "Building " + project->city->data.buildings[ui.selected_city_building].name;
                }
                break;
            case CitySelectionKind::Road: context = "Road"; break;
            case CitySelectionKind::Node: context = "Junction"; break;
            case CitySelectionKind::Piece: context = "Piece"; break;
            default: break;
        }
    } else if (project != nullptr) {
        context = mode_name(ui.tab);
    }
    const std::string title = std::string{ICON_FA_SLIDERS " Properties"} + (context.empty() ? std::string{} : " - " + context) + "###Properties";
    if (!ImGui::Begin(title.c_str(), &ui.show_properties_window)) {
        ImGui::End();
        return;
    }
    if (project == nullptr) {
        ImGui::TextDisabled("Properties appear here after you open a map.");
        ImGui::End();
        return;
    }
    if (ui.tab == EditorTab::City) {
        if (!project->city) {
            ImGui::TextDisabled("No city layout loaded.");
        } else if (ui.city_selection == CitySelectionKind::None) {
            // With nothing selected, describe what the active tool will do.
            if (ui.city_tool == CityTool::Building && !project->buildings.empty()) {
                const auto& tmpl = project->buildings[std::min(ui.place_building_template, project->buildings.size() - 1U)];
                ImGui::SeparatorText("Placing");
                ImGui::TextUnformatted(tmpl.name.c_str());
                ImGui::Text("Size: %u x %u x %u", tmpl.data.size_x, tmpl.data.size_z, tmpl.data.size_y);
                if (ImGui::Button(ICON_FA_ROTATE " Turn")) ui.place_building_rotation = (ui.place_building_rotation + 1) & 3;
                help_tooltip(ui, "Turn the building before placing it", EditorAction::RotateSelection);
                ImGui::SameLine();
                ImGui::Text("%d degrees", (ui.place_building_rotation & 3) * 90);
                if (!ui.placement_conflict.empty()) {
                    ImGui::TextColored(ImVec4(1.0F, 0.4F, 0.35F, 1.0F), ICON_FA_TRIANGLE_EXCLAMATION " %s", ui.placement_conflict.c_str());
                }
            } else if (ui.city_tool == CityTool::Piece && !project->blocks.empty()) {
                ImGui::SeparatorText("Placing piece");
                ImGui::TextUnformatted(project->blocks[std::min(ui.place_piece_mesh, project->blocks.size() - 1U)].name.c_str());
                if (!project->textures.empty()) {
                    ImGui::Text("Texture: %s", project->textures[std::min(ui.place_piece_texture, project->textures.size() - 1U)].name.c_str());
                }
                if (ImGui::Button(ICON_FA_ROTATE " Turn")) ui.selected_piece_rotation = (ui.selected_piece_rotation + 1) & 3;
                help_tooltip(ui, "Turn the piece before placing it", EditorAction::RotateSelection);
                ImGui::SameLine();
                ImGui::Text("%zu degrees", ui.selected_piece_rotation * 90);
            } else if (ui.city_tool == CityTool::Road) {
                ImGui::SeparatorText("Laying roads");
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::SliderInt("##new_road_width", &ui.new_road_width, 2, 24, "Width: %d cells");
                ImGui::TextWrapped(ui.pending_road_start ? "Click to lay the next segment; Esc finishes." : "Click to start a road.");
            } else if (ui.city_tool == CityTool::Erase) {
                ImGui::TextWrapped("Click or drag over the highlighted building, road, or piece to delete it.");
            } else {
                ImGui::TextWrapped("Nothing selected. Click a building, road, junction, or piece.");
            }
            ImGui::SeparatorText("Map");
            ImGui::TextDisabled("%zu buildings, %zu roads, %zu pieces", project->city->data.buildings.size(),
                project->city->data.streets.size(), project->city->data.portals.size());
            draw_map_limit_warnings(*project);
        } else if (ui.city_selection == CitySelectionKind::Node && !project->city->data.intersections.empty()) {
            ui.selected_city_intersection = std::min(ui.selected_city_intersection, project->city->data.intersections.size() - 1U);
            auto& node = project->city->data.intersections[ui.selected_city_intersection];
            ImGui::SeparatorText("Road junction");
            ImGui::Text("Position: %d, %d, %d", node.x, node.y, node.z);
            if (ImGui::Button("Start road here")) {
                ui.city_tool = CityTool::Road;
                ui.pending_road_start = CityGridPoint{node.x, node.y, node.z};
            }
        } else if (ui.city_selection == CitySelectionKind::Road && !project->city->data.streets.empty()) {
            ui.selected_city_street = std::min(ui.selected_city_street, project->city->data.streets.size() - 1U);
            auto& street = project->city->data.streets[ui.selected_city_street];
            draw_struct_inspector_header("SBC", "Street", street.name);
            draw_struct_section("geometry");
            int width = street.left_width + street.right_width;
            field_label("Road width");
            if (ImGui::SliderInt("##road_width", &width, 2, 24, "%d cells")) {
                begin_city_action(*project, ui, "resize road");
                street.left_width = width / 2;
                street.right_width = width - street.left_width;
                ui.city_dirty = true;
            }
            if (ImGui::Button("Continue road")) {
                ui.city_tool = CityTool::Road;
                if (street.to_intersection >= 0 && static_cast<std::size_t>(street.to_intersection) < project->city->data.intersections.size()) {
                    const auto& node = project->city->data.intersections[static_cast<std::size_t>(street.to_intersection)];
                    ui.pending_road_start = CityGridPoint{node.x, node.y, node.z};
                }
            }
            ImGui::SeparatorText("Actions");
            const auto action_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5F;
            if (ImGui::Button("Duplicate", ImVec2(action_width, 0.0F))) {
                begin_city_action(*project, ui, "duplicate road");
                duplicate_city_street(*project, ui);
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete", ImVec2(action_width, 0.0F))) {
                begin_city_action(*project, ui, "delete road");
                delete_city_street(*project, ui);
            }
        } else if (ui.city_selection == CitySelectionKind::Building && !project->city->data.buildings.empty()) {
            ui.selected_city_building = std::min(ui.selected_city_building, project->city->data.buildings.size() - 1U);
            auto& building = project->city->data.buildings[ui.selected_city_building];
            draw_struct_inspector_header("SBC", "Building", building.name);
            if (ui.selected_city_buildings.size() > 1U) {
                ImGui::TextDisabled("%zu buildings selected; actions apply to all.", ui.selected_city_buildings.size());
            }
            draw_struct_section("transform");
            ImGui::Text("Position: %d, %d, %d", building.x, building.y, building.z);
            ImGui::TextWrapped("Drag an arrow to move, the ring to rotate. Arrow keys nudge.");
            int rotation = building.rotation & 3;
            constexpr const char* rotations[] = {"0 degrees", "90 degrees", "180 degrees", "270 degrees"};
            field_label("Rotation");
            if (ImGui::Combo("##rotation", &rotation, rotations, 4)) {
                begin_city_action(*project, ui, "rotate building");
                nudge_city_building(*project, ui, 0, 0, 0, (rotation - building.rotation) & 3);
            }
            ImGui::SeparatorText("Actions");
            if (ImGui::Button(ICON_FA_PEN " Edit building", ImVec2(-FLT_MIN, 30.0F))) edit_city_building(*project, ui, ui.selected_city_building);
            help_tooltip(ui, "Open this building template in Building mode");
            const auto action_width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5F;
            if (ImGui::Button(ICON_FA_COPY " Duplicate", ImVec2(action_width, 0.0F))) {
                begin_city_action(*project, ui, "duplicate building");
                duplicate_city_building(*project, ui);
            }
            help_tooltip(ui, "Copy the selection next to itself", EditorAction::DuplicateSelection);
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_TRASH " Delete", ImVec2(action_width, 0.0F))) {
                begin_city_action(*project, ui, "delete building");
                delete_city_building(*project, ui);
            }
            help_tooltip(ui, "Remove the selection from the map", EditorAction::DeleteSelection);
        } else if (ui.city_selection == CitySelectionKind::Piece) {
            ImGui::SeparatorText("Piece");
            // The voxel word carries the city-block record (low 12 bits) and the
            // quarter-turn rotation (bits 24-25).
            const auto& city_blocks = project->city->data.city_blocks;
            const auto block_index = static_cast<std::size_t>(ui.selected_piece_word & 0xfffU);
            if (block_index < city_blocks.size()) {
                ImGui::Text("Piece: %s", std::string{city_block_display_name(city_blocks[block_index])}.c_str());
            }
            ImGui::Text("Rotation: %u degrees", ((ui.selected_piece_word >> 24U) & 3U) * 90U);
            if (ui.selected_piece_voxel.has_value()) {
                ImGui::Text("Cell: %d, %d, %d", ui.selected_piece_voxel->x, ui.selected_piece_voxel->y, ui.selected_piece_voxel->z);
            }
            ImGui::TextWrapped("Arrow keys and PgUp/PgDn move it one cell; Delete removes it.");
        }
    } else if (ui.tab == EditorTab::Buildings) {
        draw_building_inspector(*project, ui, status);
    } else if (ui.tab == EditorTab::Blocks) {
        draw_block_inspector(*project, ui, status);
    } else if (ui.tab == EditorTab::Buildblocks) {
        draw_buildblock_inspector(*project, ui, status);
    } else if (ui.tab == EditorTab::Pieces) {
        ImGui::TextDisabled("Pieces have no properties. Pick a brush in the Library.");
    }
    ImGui::End();
}

} // namespace map_editor::app_internal
