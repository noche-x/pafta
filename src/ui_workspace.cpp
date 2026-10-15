// SPDX-License-Identifier: GPL-2.0-only
// Workspace windows that follow the context: Tools, Library, and Properties. Each
// is a normal ImGui window you can float or dock, and its contents follow the
// current mode, tool, and selection.
#include "app_internal.hpp"
#include "file_dialogs.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <string>

namespace pafta::app_internal {
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

// A tooltip with the action's current shortcut added to the end.
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
    const auto query = lower_name(search.data());
    std::vector<std::size_t> visible;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (query.empty() || lower_name(names[i]).find(query) != std::string::npos) visible.push_back(i);
    }
    ImGui::TextDisabled("%zu of %zu", visible.size(), names.size());
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", roads ? "Click selects; double-click frames." : "Click selects; double-click edits.");
    ImGui::PopStyleColor();
    // Scroll by record index when the viewport selection changes. Names can repeat.
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
                    if (ImGui::BeginPopupContextItem("##road_menu")) {
                        ui.selected_city_street = i;
                        ui.city_selection = CitySelectionKind::Road;
                        if (ImGui::MenuItem(ICON_FA_CROSSHAIRS " Frame")) ui.focus_selection_requested = true;
                        if (ImGui::MenuItem(ICON_FA_COPY " Duplicate")) {
                            begin_city_action(project, ui, "duplicate road");
                            duplicate_city_street(project, ui);
                        }
                        if (ImGui::MenuItem(ICON_FA_TRASH " Delete")) {
                            begin_city_action(project, ui, "delete road");
                            delete_city_street(project, ui);
                        }
                        ImGui::EndPopup();
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
                    if (ImGui::BeginPopupContextItem("##building_menu")) {
                        if (!active) {
                            ui.selected_city_building = i;
                            ui.selected_city_buildings = {i};
                            ui.city_selection = CitySelectionKind::Building;
                        }
                        if (ImGui::MenuItem(ICON_FA_PEN " Edit building")) edit_city_building(project, ui, i);
                        if (ImGui::MenuItem(ICON_FA_CROSSHAIRS " Frame")) ui.focus_selection_requested = true;
                        if (ImGui::MenuItem(ICON_FA_COPY " Duplicate")) {
                            begin_city_action(project, ui, "duplicate building");
                            duplicate_city_building(project, ui);
                        }
                        if (ImGui::MenuItem(ICON_FA_TRASH " Delete")) {
                            begin_city_action(project, ui, "delete building");
                            delete_city_building(project, ui);
                        }
                        ImGui::EndPopup();
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

// A square icon button with an "on" state and a tooltip naming its key.
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

// One row of tool buttons, the same in every mode: the number keys 1-5 pick them.
template <typename ToolT>
struct ToolEntry {
    const char* icon;
    ToolT tool;
    const char* tip;
};
constexpr EditorAction kToolKeys[] = {EditorAction::ToolSelect, EditorAction::ToolBuilding, EditorAction::ToolRoad,
    EditorAction::ToolPiece, EditorAction::ToolErase};

template <typename ToolT, std::size_t N>
bool tool_row(const ToolEntry<ToolT> (&tools)[N], ToolT& current, const EditorUi& ui) {
    bool changed = false;
    for (std::size_t t = 0; t < N; ++t) {
        if (t != 0) ImGui::SameLine();
        if (tool_button(tools[t].icon, current == tools[t].tool, ui, tools[t].tip, kToolKeys[t]) && current != tools[t].tool) {
            current = tools[t].tool;
            changed = true;
        }
    }
    return changed;
}

// A level with - and + buttons.
bool level_stepper(const char* label, int& level, int lo, int hi) {
    bool changed = false;
    ImGui::PushID(label);
    if (ImGui::Button(ICON_FA_MINUS, ImVec2(kToolButton, kToolButton)) && level > lo) { --level; changed = true; }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s %d", label, level);
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PLUS, ImVec2(kToolButton, kToolButton)) && level < hi) { ++level; changed = true; }
    ImGui::PopID();
    return changed;
}

// The brush strip shared by Add Block, Paint, and Piece: how a click applies, the
// circle's size, and the level lock. Every brush tool draws the same strip.
void brush_strip(EditorUi& ui) {
    auto* brush = active_brush(ui);
    if (brush == nullptr && !(ui.tab == EditorTab::City && (ui.city_tool == CityTool::Road || ui.city_tool == CityTool::Building))) return;
    toolbar_divider();
    if (brush != nullptr) {
        const bool paint = brush == &ui.paint_brush;
        struct Mode { const char* icon; BrushMode mode; const char* tip; };
        const Mode modes[] = {
            {ICON_FA_HAND_POINTER, BrushMode::Single, "Single: one per click"},
            {ICON_FA_SIGNATURE, BrushMode::Drag, "Drag: hold and move to keep going"},
            {ICON_FA_BORDER_ALL, BrushMode::Box, paint ? "Box: drag a rectangle; paints the faces you can see in it"
                                                        : "Box: drag from one corner to the other to fill it"},
            paint ? Mode{ICON_FA_RING, BrushMode::Loop, "Loop: paints the connected faces of the clicked kind on that floor"}
                  : Mode{ICON_FA_CIRCLE_DOT, BrushMode::Circle, "Circle: hold to stamp a disc"},
        };
        for (std::size_t m = 0; m < std::size(modes); ++m) {
            if (m != 0) ImGui::SameLine();
            if (tool_button(modes[m].icon, *brush == modes[m].mode, ui, modes[m].tip)) *brush = modes[m].mode;
        }
        if (*brush == BrushMode::Circle) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0F);
            ImGui::SliderInt("##brush_radius", &ui.brush_radius, 1, 16, "Radius %d");
        }
        if (paint) return;
    }
    // Level lock: building mode locks to the shown floor (stepped in the view group).
    ImGui::SameLine();
    if (ui.tab == EditorTab::Buildings) {
        if (tool_button(ui.building_level_locked ? ICON_FA_LOCK : ICON_FA_LOCK_OPEN, ui.building_level_locked, ui,
                "Lock the level: place on the chosen floor instead of against faces")) {
            ui.building_level_locked = !ui.building_level_locked;
        }
        return;
    }
    if (tool_button(ui.place_height_locked ? ICON_FA_LOCK : ICON_FA_LOCK_OPEN, ui.place_height_locked, ui,
            "Lock the level: place on one Y level instead of on the surface (Page Up/Down change it)")) {
        ui.place_height_locked = !ui.place_height_locked;
        save_preferences(ui);
    }
    if (ui.place_height_locked) {
        ImGui::SameLine();
        if (level_stepper("Y", ui.place_height_y, -512, 512)) save_preferences(ui);
    }
}

// The Piece tool's brush: a preview, its mesh, and its eight slot textures. The
// textures and mesh can be changed freely; placing reuses the map's piece that
// matches, or adds one to city2.sbc.
void draw_piece_brush(EditorProject& project, EditorUi& ui, std::string& status) {
    if (ImGui::CollapsingHeader("Placing piece", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (std::ranges::all_of(ui.place_piece_textures, &std::string::empty) && !project.textures.empty()) {
            ui.place_piece_textures.fill(project.textures.front().name);
        }
        const auto mesh_name = piece_brush_mesh_name(project, ui);
        if (const auto* preview = piece_thumbnail(project, mesh_name, ui.place_piece_textures,
                static_cast<std::int32_t>(ui.selected_piece_rotation))) {
            ImGui::Image(ImTextureID(preview->texture.id), ImVec2(96.0F, 96.0F), ImVec2(0.0F, 1.0F), ImVec2(1.0F, 0.0F));
            ImGui::SameLine();
                }
        ImGui::BeginGroup();
        ImGui::TextUnformatted("Mesh");
        std::size_t mesh = ui.place_piece_solid || project.blocks.empty() ? 0U : std::min(ui.place_piece_mesh, project.blocks.size() - 1U) + 1U;
        if (grid_button("##piece_mesh", 44.0F, project.blocks.size() + 1U, mesh,
                [&](std::size_t i) { return i == 0U ? std::string{"Solid block (nblock)"} : project.blocks[i - 1U].name; },
                [&](std::size_t i) {
                    const auto* t = piece_thumbnail(project, i == 0U ? std::string{} : project.blocks[i - 1U].name,
                        ui.place_piece_textures, static_cast<std::int32_t>(ui.selected_piece_rotation));
                    return Thumb{t != nullptr ? t->texture.id : 0U, true};
                }, "Mesh")) {
            ui.place_piece_solid = mesh == 0U;
            if (mesh > 0U) ui.place_piece_mesh = mesh - 1U;
                }
        ImGui::TextUnformatted(mesh_name.empty() ? "Solid block" : mesh_name.c_str());
        // Whether the map already has this piece, or placing adds it.
        const auto& city_blocks = project.city->data.city_blocks;
        std::optional<std::size_t> existing;
        for (std::size_t block = 0; block < city_blocks.size() && !existing; ++block) {
            if (lower_name(city_blocks[block].name) != lower_name(mesh_name)) continue;
            if (std::ranges::find(city_blocks[block].layers, ui.place_piece_textures) != city_blocks[block].layers.end()) existing = block;
        }
        if (existing) {
            ImGui::TextDisabled("Map piece %zu", *existing);
        } else if (ImGui::Button(ICON_FA_PLUS " Save as map piece")) {
            // Adds the brush to city2.sbc's pieces, so it shows in Assets without placing it.
            begin_city_action(project, ui, "add piece");
            if (!find_or_add_city_block(project.city->data, mesh_name, ui.place_piece_textures)) {
                status = "the map already has 4096 city blocks";
            } else {
                ui.city_dirty = true;
            }
        }
        ImGui::EndGroup();
        ImGui::TextDisabled("Textures (slots 0-7)");
        texture_slots(project, "piece_brush_slots", ui.place_piece_textures);
    }
}

// A one-line hint for the active tool, drawn under the toolbar.
const char* tool_hint(const EditorUi& ui) {
    if (ui.tab == EditorTab::Blocks) {
        if (ui.block_paint_faces) return "Choose a texture in Assets > Textures. Click a patch or box to paint the displayed slot.";
        return ui.block_vertex_mode
            ? "Points: click or drag a box to pick (Shift adds, Ctrl+A all), drag a point to move, G/S/R transform. Tab: surfaces."
            : "Surfaces: click to pick. Drag the arrows and handles, or G move, S scale, R rotate (X/Y/Z lock). Tab: points.";
    }
    if (ui.tab == EditorTab::Pieces) return "Choose a map preset, then edit its mesh and layer textures in Properties.";
    if (ui.tab == EditorTab::Buildblocks) return "Choose a buildblock template, then edit its layers, cells, and connections in Properties.";
    if (ui.tab == EditorTab::Buildings) {
        switch (ui.building_tool) {
            case BuildingTool::Select: return "Click a block to select it. Delete removes it; Tab cycles blocks.";
            case BuildingTool::PlaceBlock: return "Click a face to add a block against it. Brush: single, drag, box, or circle; the lock keeps it on one floor.";
            case BuildingTool::PaintTexture: return ui.paint_side_code ? "Pick a wall type in Properties, then click walls. Brush: single, drag, box, or loop."
                                                                       : "Pick a material in Properties, then click faces. Brush: single, drag, box, or loop.";
            case BuildingTool::ItemSet: return "Pick an item set in Properties, then click a block. Delete clears the selected block's set.";
            case BuildingTool::Anchor: return "Drag an arrow to move the building's anchor. Esc cancels.";
        }
        return "";
    }
    switch (ui.city_tool) {
        case CityTool::Select: return "Click to select, drag a box for many. Right-click for actions. Middle-drag orbits, Shift+middle pans.";
        case CityTool::Building: return "Pick a building below; the outline shows where it lands. R turns it, click to place.";
        case CityTool::Road: return ui.pending_road_start ? "Click to lay the next straight segment. Esc or clicking the end finishes."
                                                          : "Click to start a road.";
        case CityTool::Piece: return "Pick a piece below; the ghost shows where it lands. R turns it. Brush: single, drag, box, or circle.";
        case CityTool::Erase: return "Click or drag over the red-highlighted building, road, or piece to delete it.";
    }
    return "";
}

} // namespace

// The toolbar is a normal window. It starts floating over the top of the 3D view;
// drag it by its edge to move it, or onto a dock edge to dock it.
void draw_toolbar_window(EditorProject* project, EditorUi& ui, Camera3D& camera, std::string& status, bool& open_folder_requested) {
    if (!ui.show_toolbar_window) return;
    const auto* viewport = ImGui::GetMainViewport();
    const ImGuiCond cond = ui.reset_layout ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.38F, viewport->WorkPos.y + 12.0F), cond, ImVec2(0.5F, 0.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0F, 6.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0F);
    const bool open = ImGui::Begin(ICON_FA_TOOLBOX " Toolbar###Toolbar", &ui.show_toolbar_window,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    if (open) {
        // A grip to drag the toolbar by; right-click it to hide the toolbar.
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::Button(ICON_FA_GRIP_VERTICAL, ImVec2(16.0F, kToolButton));
        ImGui::PopStyleColor();
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            ImGui::StartMouseMovingWindowOrNode(ImGui::GetCurrentWindow(), ImGui::GetCurrentWindow()->DockNode, true);
        }
        help_tooltip(ui, "Drag to move the toolbar. Right-click for options.");
        if (ImGui::BeginPopupContextItem("##toolbar_menu")) {
            if (ImGui::MenuItem("Hide toolbar")) ui.show_toolbar_window = false;
            if (ImGui::MenuItem("Put back over the view")) ui.reset_layout = true;
            ImGui::EndPopup();
        }
        ImGui::SameLine();
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
        ImGui::SameLine();
        if (tool_button(ICON_FA_CUBES, ui.tab == EditorTab::Blocks, ui, "Block mode: make and edit block meshes") &&
            ui.tab != EditorTab::Blocks) {
            ui.requested_tab = EditorTab::Blocks;
            }
        toolbar_divider();
        // Every mode: tools, then the brush strip, then view toggles, then undo.
        if (ui.tab == EditorTab::City && project->city) {
            static constexpr ToolEntry<CityTool> tools[] = {
                {ICON_FA_ARROW_POINTER, CityTool::Select, "Select"},
                {ICON_FA_HOUSE, CityTool::Building, "Place building"},
                {ICON_FA_ROAD, CityTool::Road, "Lay road"},
                {ICON_FA_CUBE, CityTool::Piece, "Place pieces"},
                {ICON_FA_ERASER, CityTool::Erase, "Erase"},
            };
            if (tool_row(tools, ui.city_tool, ui) && ui.city_tool != CityTool::Road) ui.pending_road_start.reset();
            brush_strip(ui);
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
            static constexpr ToolEntry<BuildingTool> tools[] = {
                {ICON_FA_ARROW_POINTER, BuildingTool::Select, "Select block"},
                {ICON_FA_SQUARE_PLUS, BuildingTool::PlaceBlock, "Add blocks"},
                {ICON_FA_PAINTBRUSH, BuildingTool::PaintTexture, "Paint faces"},
                {ICON_FA_ANCHOR, BuildingTool::Anchor, "Move anchor"},
                {ICON_FA_OBJECT_GROUP, BuildingTool::ItemSet, "Place item set"},
            };
            tool_row(tools, ui.building_tool, ui);
            brush_strip(ui);
            toolbar_divider();
            // The whole building, or one floor; the floor is also Add Block's locked level.
            ui.selected_building = std::min(ui.selected_building, project->buildings.size() - 1U);
            const int max_floor = std::max(0, static_cast<int>(project->buildings[ui.selected_building].data.size_z) - 1);
            if (tool_button(ICON_FA_LAYER_GROUP, ui.building_view_floor_only, ui, "Show one floor at a time")) {
                ui.building_view_floor_only = !ui.building_view_floor_only;
                project->building_floor_preview_dirty = true;
            }
            if (ui.building_view_floor_only || (ui.building_level_locked && ui.building_tool == BuildingTool::PlaceBlock)) {
                ui.building_view_floor = std::clamp(ui.building_view_floor, 0, ui.building_view_floor_only ? max_floor : max_floor + 1);
                ImGui::SameLine();
                if (level_stepper("Floor", ui.building_view_floor, 0, ui.building_view_floor_only ? max_floor : max_floor + 1)) {
                    project->building_floor_preview_dirty = true;
                }
            }
            ImGui::SameLine();
            if (tool_button(ICON_FA_HOUSE, ui.building_hide_roofs, ui, "Hide roofs (see inside)")) {
                ui.building_hide_roofs = !ui.building_hide_roofs;
            }
            ImGui::SameLine();
            if (tool_button(ICON_FA_TABLE_CELLS, ui.show_buildblock_window, ui, "Buildblock editor", EditorAction::WindowBuildblock)) {
                ui.show_buildblock_window = !ui.show_buildblock_window;
            }
        } else if (ui.tab == EditorTab::Blocks) {
            ImGui::BeginDisabled(project->blocks.empty());
            static constexpr ToolEntry<bool> tools[] = {
                {ICON_FA_ARROW_POINTER, false, "Select block surfaces"},
                {ICON_FA_PAINTBRUSH, true, "Paint faces with a texture slot"},
            };
            if (tool_row(tools, ui.block_paint_faces, ui) && ui.block_paint_faces) ui.block_vertex_mode = false;
            toolbar_divider();
            // New blocks come from Assets > New block; these add to the open block.
            if (tool_button(ICON_FA_CUBE, false, ui, "Add box: a solid box surface")) add_block_box(*project, ui);
            ImGui::SameLine();
            if (tool_button(ICON_FA_VECTOR_SQUARE, false, ui, "Add patch: a flat or curved surface")) add_block_patch(*project, ui);
            ImGui::EndDisabled();
        }
        if (ui.tab == EditorTab::Blocks && ui.block_paint_faces) draw_block_paint_texture(*project, ui);
        toolbar_divider();
        if (tool_button(ICON_FA_ROTATE_LEFT, false, ui, "Undo", EditorAction::Undo)) undo_edit(*project, ui, status, false);
        ImGui::SameLine();
        if (tool_button(ICON_FA_ROTATE_RIGHT, false, ui, "Redo", EditorAction::Redo)) undo_edit(*project, ui, status, true);
    }
    }
    ImGui::End();
}

// The asset drawer: buildings in City and Building mode, pieces with the Piece tool.
// It is a normal window, docked under the 3D view at first.
void draw_asset_drawer_window(EditorProject* project, EditorUi& ui, std::string& status) {
    if (!ui.show_asset_drawer) return;
    if (!ImGui::Begin(ICON_FA_IMAGES " Assets###Assets", &ui.show_asset_drawer)) {
        ImGui::End();
        return;
    }
    const bool has_city = project != nullptr && project->city.has_value();
    const bool drawer_pieces = has_city && ui.tab == EditorTab::City && ui.city_tool == CityTool::Piece;
    const bool drawer_blocks = project != nullptr && ui.tab == EditorTab::Blocks;
    const bool drawer_buildings = project != nullptr && !drawer_pieces && (has_city || ui.tab == EditorTab::Buildings) &&
        (ui.tab == EditorTab::City || ui.tab == EditorTab::Buildings) && !project->buildings.empty();
    if (drawer_blocks || drawer_pieces) {
        if (ImGui::Button(ICON_FA_PLUS " New block")) {
            create_new_block(*project, ui, status);
            ui.place_piece_mesh = ui.selected_block;
            ui.place_piece_solid = false;
            ui.requested_tab = EditorTab::Blocks;
        }
#ifndef __EMSCRIPTEN__
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_FILE_IMPORT " Import SBL / textures...")) {
            try {
                const auto paths = dialog_open_files("Blocks and textures", "sbl,png");
                if (!paths.empty()) import_files_into_project(*project, ui, paths, status);
            } catch (const std::exception& error) {
                status = std::string{"Asset import failed: "} + error.what();
            }
        }
#endif
    }
    if (drawer_blocks) {
        static bool textures = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Meshes", !textures)) textures = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Textures", textures)) textures = true;
        auto& search = ui.asset_search[static_cast<std::size_t>(EditorTab::Blocks)];
        ImGui::SetNextItemWidth(200.0F);
        ImGui::InputTextWithHint("##block_drawer_search", ICON_FA_MAGNIFYING_GLASS " Search", search.data(), search.size());
        if (textures) {
            int slot = static_cast<int>(ui.block_paint_slot);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150.0F);
            if (ImGui::SliderInt("##texture_slot", &slot, 0, 7, "Slot %d")) ui.block_paint_slot = static_cast<std::uint32_t>(slot);
            const auto& name = ui.block_slot_textures[ui.block_paint_slot];
            const auto selected = static_cast<std::size_t>(std::ranges::find_if(project->textures, [&](const auto& texture) {
                return lower_name(texture.name) == lower_name(name);
            }) - project->textures.begin());
            if (const auto picked = asset_grid("##block_drawer_textures", project->textures.size(), selected,
                    {.size = 64.0F, .labels = true, .search = &search, .action = "Use this texture for the paint slot shown above"},
                    [&](std::size_t i) { return project->textures[i].name; },
                    [&](std::size_t i) { return Thumb{project->textures[i].texture.id, false}; })) {
                ui.block_slot_textures[ui.block_paint_slot] = project->textures[*picked].name;
                ui.block_textured = true;
                    }
        } else if (const auto picked = asset_grid("##block_drawer_meshes", project->blocks.size(), ui.selected_block,
                {.size = 88.0F, .labels = true, .search = &search},
                [&](std::size_t i) { return project->blocks[i].name; },
                [&](std::size_t i) {
                    const auto* t = block_thumbnail(*project, i);
                    return Thumb{t != nullptr ? t->texture.id : 0U, true};
                },
                [&](std::size_t i) {
                    if (ImGui::BeginPopupContextItem("##block_menu")) {
                        if (ImGui::MenuItem(ICON_FA_FILE_EXPORT " Export .sbl...")) export_block_sbl(*project, i, status);
                        ImGui::EndPopup();
                    }
                })) {
            select_block_asset(ui, *picked);
            ui.focus_selection_requested = true;
                }
        ImGui::End();
        return;
    }
    if (!drawer_buildings && !drawer_pieces) {
        ImGui::TextDisabled(project == nullptr ? "Open a map to see its buildings and pieces here." : "Nothing to pick in this mode.");
        ImGui::End();
        return;
    }
    static std::array<char, 128> building_search{};
    static std::array<char, 128> piece_search{};
    auto& search = drawer_pieces ? piece_search : building_search;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(drawer_pieces ? "Pieces" : ui.tab == EditorTab::Buildings ? "Edit which building" : "Buildings");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0F);
    ImGui::InputTextWithHint("##drawer_search", ICON_FA_MAGNIFYING_GLASS " Search", search.data(), search.size());
    if (drawer_pieces) {
        static bool all_meshes = false;
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PLUS " Add preset")) {
            if (std::ranges::all_of(ui.place_piece_textures, &std::string::empty) && !project->textures.empty()) ui.place_piece_textures.fill(project->textures.front().name);
            begin_city_action(*project, ui, "add piece preset");
            if (find_or_add_city_block(project->city->data, piece_brush_mesh_name(*project, ui), ui.place_piece_textures)) {
                ui.city_dirty = true;
                all_meshes = false;
                status = "Piece preset added to the map";
            } else status = "the map already has 4096 city blocks";
        }
        help_tooltip(ui, "Save the current mesh and its eight textures in this map's piece presets.");
        ImGui::SameLine();
        if (ImGui::RadioButton("Map presets", !all_meshes)) all_meshes = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("All meshes", all_meshes)) all_meshes = true;
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ROTATE)) ui.selected_piece_rotation = (ui.selected_piece_rotation + 1) & 3;
        help_tooltip(ui, "Turn the piece", EditorAction::RotateSelection);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%zu deg", ui.selected_piece_rotation * 90);
        if (all_meshes) {
            const auto current = ui.place_piece_solid ? 0U : ui.place_piece_mesh + 1U;
            if (const auto picked = asset_grid("##piece_mesh_library", project->blocks.size() + 1U, current,
                    {.size = 88.0F, .labels = true, .search = &search, .action = "Use this mesh for the Piece brush; Add preset saves it in the map"},
                    [&](std::size_t i) { return i == 0U ? std::string{"Solid block"} : project->blocks[i - 1U].name; },
                    [&](std::size_t i) {
                        const auto* t = piece_thumbnail(*project, i == 0U ? std::string{} : project->blocks[i - 1U].name,
                            ui.place_piece_textures, static_cast<std::int32_t>(ui.selected_piece_rotation));
                        return Thumb{t != nullptr ? t->texture.id : 0U, true};
                    })) {
                ui.place_piece_solid = *picked == 0U;
                if (*picked > 0U) ui.place_piece_mesh = *picked - 1U;
                    }
            ImGui::End();
            return;
        }
    } else if (ui.tab == EditorTab::City) {
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ROTATE)) ui.place_building_rotation = (ui.place_building_rotation + 1) & 3;
        help_tooltip(ui, "Turn the building", EditorAction::RotateSelection);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%d deg", ui.place_building_rotation * 90);
    }
    {
        if (drawer_pieces) {
            // The map's pieces: each city block of city2.sbc with one of its texture
            // layers, shown as it looks. Picking one makes it the brush.
            struct PieceEntry {
                std::size_t block;
                std::size_t layer;
            };
            const auto& city_blocks = project->city->data.city_blocks;
            std::vector<PieceEntry> entries;
            for (std::size_t block = 0; block < city_blocks.size(); ++block) {
                for (std::size_t layer = 0; layer < std::min<std::size_t>(city_blocks[block].layers.size(), 63U); ++layer) {
                    entries.push_back({block, layer});
                }
            }
            const auto brush_name = lower_name(piece_brush_mesh_name(*project, ui));
            std::size_t current = entries.size();
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const auto& block = city_blocks[entries[i].block];
                if (lower_name(block.name) == brush_name && block.layers[entries[i].layer] == ui.place_piece_textures) current = i;
            }
            if (entries.empty()) ImGui::TextDisabled("This map has no pieces yet. Make one in Properties and place it.");
            const auto picked = asset_grid("##piece_presets", entries.size(), current, {.size = 88.0F, .labels = true, .search = &search, .action = "Use this mesh and texture layer for the Piece brush"},
                [&](std::size_t i) {
                    const auto& block = city_blocks[entries[i].block];
                    auto label = std::string{city_block_display_name(block)};
                    if (block.layers.size() > 1U) label += " #" + std::to_string(entries[i].layer + 1U);
                    return label;
                },
                [&](std::size_t i) {
                    const auto& block = city_blocks[entries[i].block];
                    const auto* t = piece_thumbnail(*project, block.name, block.layers[entries[i].layer],
                        static_cast<std::int32_t>(ui.selected_piece_rotation));
                    return Thumb{t != nullptr ? t->texture.id : 0U, true};
                });
            if (picked) {
                const auto& block = city_blocks[entries[*picked].block];
                ui.place_piece_textures = block.layers[entries[*picked].layer];
                ui.place_piece_solid = block.name.empty();
                const auto mesh = std::ranges::find_if(project->blocks, [&](const auto& asset) { return lower_name(asset.name) == lower_name(block.name); });
                if (mesh != project->blocks.end()) {
                    ui.place_piece_mesh = static_cast<std::size_t>(mesh - project->blocks.begin());
                } else if (!block.name.empty()) {
                    status = "piece mesh " + block.name + " is not loaded; the brush keeps its mesh";
                }
            }
        } else {
            const bool editing = ui.tab == EditorTab::Buildings;
            const auto current = editing ? ui.selected_building : ui.place_building_template;
            const auto picked = asset_grid("##building_strip", project->buildings.size(), current, {.size = 88.0F, .labels = true, .search = &search},
                [&](std::size_t i) { return project->buildings[i].name; },
                [&](std::size_t i) {
                    const auto* t = building_thumbnail(*project, i);
                    return Thumb{t != nullptr ? t->texture.id : 0U, true};
                },
                [&](std::size_t i) {
                    if (ImGui::BeginPopupContextItem("##building_menu")) {
                        ImGui::TextDisabled("%s", project->buildings[i].name.c_str());
                        if (has_city && ImGui::MenuItem(ICON_FA_HOUSE " Place in the city")) {
                            ui.place_building_template = i;
                            ui.city_tool = CityTool::Building;
                            ui.requested_tab = EditorTab::City;
                        }
                        if (ImGui::MenuItem(ICON_FA_PEN " Edit building")) edit_building(*project, ui, i);
                        if (ImGui::MenuItem(ICON_FA_FILE_EXPORT " Export .sbb...")) export_building_sbb(*project, i, status);
                        ImGui::EndPopup();
                    }
                    if (editing) return;
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) edit_building(*project, ui, i);
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("MAP_BUILDING", &i, sizeof(i));
                        ImGui::TextUnformatted(project->buildings[i].name.c_str());
                        ImGui::EndDragDropSource();
                    }
                });
            if (picked) {
                if (editing) {
                    edit_building(*project, ui, *picked);
                } else {
                    // Picking a building gets the Building tool ready to place it.
                    ui.place_building_template = *picked;
                    ui.city_tool = CityTool::Building;
                    ui.pending_road_start.reset();
                }
            }
        }
    }
    ImGui::End();
}

// Drawn over the 3D view: the hint for the active tool and map warnings, under
// `top` (screen y). Returns the bottom of what it drew.
float draw_viewport_overlays(EditorProject* project, EditorUi& ui) {
    const ImVec2 origin = ImGui::GetWindowPos();
    const ImVec2 extent = ImGui::GetWindowSize();
    float top_overlay_bottom = origin.y;
    if (project == nullptr) return top_overlay_bottom;
    const char* hint = tool_hint(ui);
    const ImVec2 size = ImGui::CalcTextSize(hint);
    const ImVec2 at{origin.x + (extent.x - size.x) * 0.5F, extent.y + origin.y - size.y - 12.0F};
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(ImVec2(at.x - 8.0F, at.y - 3.0F), ImVec2(at.x + size.x + 8.0F, at.y + size.y + 3.0F), IM_COL32(0, 0, 0, 150), 4.0F);
    draw->AddText(at, IM_COL32(225, 225, 225, 255), hint);
    if (ui.tab == EditorTab::City) {
        std::string warning;
        for (const auto& problem : game_world_fit_problems(*project)) warning += (warning.empty() ? "" : "\n") + problem;
        if (!warning.empty()) {
            const float wrap = std::min(600.0F, extent.x - 32.0F);
            const ImVec2 warning_size = ImGui::CalcTextSize(warning.c_str(), nullptr, false, wrap);
            const ImVec2 warning_at{origin.x + (extent.x - warning_size.x) * 0.5F, origin.y + 10.0F};
            draw->AddRectFilled(ImVec2(warning_at.x - 8.0F, warning_at.y - 3.0F),
                ImVec2(warning_at.x + warning_size.x + 8.0F, warning_at.y + warning_size.y + 3.0F), IM_COL32(90, 10, 10, 200), 4.0F);
            draw->AddText(nullptr, 0.0F, warning_at, IM_COL32(255, 150, 140, 255), warning.c_str(), nullptr, wrap);
            top_overlay_bottom = warning_at.y + warning_size.y + 3.0F;
        }
    }
    return top_overlay_bottom;
}

// Outliner: what is in the map (placed buildings and roads), docked above
// Properties.
void draw_outliner_window(EditorProject* project, EditorUi& ui) {
    if (!ui.show_library_window) return;
    if (!ImGui::Begin(ICON_FA_LIST " Outliner###Outliner", &ui.show_library_window)) {
        ImGui::End();
        return;
    }
    if (project != nullptr && ui.tab == EditorTab::Blocks) {
        // Block mode lists the blocks; the selection is the block being edited.
        static std::array<char, 128> search{};
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##block_search", "Filter by name", search.data(), search.size());
        const auto key = lower_name(search.data());
        if (ImGui::BeginListBox("##block_list", ImVec2(-FLT_MIN, -FLT_MIN))) {
            for (std::size_t i = 0; i < project->blocks.size(); ++i) {
                const auto& name = project->blocks[i].name;
                if (!key.empty() && lower_name(name).find(key) == std::string::npos) continue;
                if (ImGui::Selectable((name + "##block" + std::to_string(i)).c_str(), ui.selected_block == i) && ui.selected_block != i) {
                    select_block_asset(ui, i);
                }
            }
            ImGui::EndListBox();
        }
    } else if (project == nullptr || !project->city) {
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

// How high the Road, Piece, and Building tools place things, relative to the surface
// under the cursor.
void draw_place_height(EditorUi& ui) {
    field_label("Height");
    bool changed = ImGui::Checkbox("Lock Y level", &ui.place_height_locked);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ui.place_height_locked) {
        changed |= ImGui::InputInt("##place_height_y", &ui.place_height_y);
        ui.place_height_y = std::clamp(ui.place_height_y, -512, 512);
        help_tooltip(ui, "Place on this horizontal cell level regardless of the surface");
    } else {
        changed |= ImGui::SliderInt("##place_height", &ui.place_height_offset, -32, 32,
            ui.place_height_offset == 0 ? "On the surface" : "%+d cells");
        help_tooltip(ui, "Place above or below the surface under the cursor");
    }
    if (changed) save_preferences(ui);
}

void draw_properties_window(EditorProject* project, EditorUi& ui, std::string& status) {
    if (!ui.show_properties_window) return;
    std::string context;
    if (project != nullptr && project->city && ui.tab == EditorTab::City && ui.city_tool != CityTool::Select) {
        // A placement tool's settings replace the selection's.
        switch (ui.city_tool) {
            case CityTool::Building: context = "Place building"; break;
            case CityTool::Road: context = "Lay road"; break;
            case CityTool::Piece: context = "Place piece"; break;
            case CityTool::Erase: context = "Erase"; break;
            default: break;
        }
    } else if (project != nullptr && project->city && ui.tab == EditorTab::City) {
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
        if (ui.city_tool == CityTool::Select) {
            if (ImGui::CollapsingHeader("Box selection", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox("Buildings", &ui.box_select_buildings);
                ImGui::Checkbox("Pieces", &ui.box_select_pieces);
                ImGui::Checkbox("Roads", &ui.box_select_roads);
                if (!ui.selected_piece_cells.empty() || !ui.selected_city_streets.empty()) {
                    ImGui::Text("%zu buildings, %zu pieces, %zu roads selected", ui.selected_city_buildings.size(),
                        ui.selected_piece_cells.size(), ui.selected_city_streets.size());
                }
            }
        }
        if (!project->city) {
            ImGui::TextDisabled("No city layout loaded.");
        } else if (ui.city_selection == CitySelectionKind::None || ui.city_tool != CityTool::Select) {
            // A placement tool shows what it will do; the selection's details show
            // with the Select tool.
            if (ui.city_tool == CityTool::Building && !project->buildings.empty()) {
                const auto& tmpl = project->buildings[std::min(ui.place_building_template, project->buildings.size() - 1U)];
                if (ImGui::CollapsingHeader("Placing", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::TextUnformatted(tmpl.name.c_str());
                    ImGui::Text("Size: %u x %u x %u", tmpl.data.size_x, tmpl.data.size_z, tmpl.data.size_y);
                    if (ImGui::Button(ICON_FA_ROTATE " Turn")) ui.place_building_rotation = (ui.place_building_rotation + 1) & 3;
                    help_tooltip(ui, "Turn the building before placing it", EditorAction::RotateSelection);
                    ImGui::SameLine();
                    ImGui::Text("%d degrees", (ui.place_building_rotation & 3) * 90);
                    draw_place_height(ui);
                    if (!ui.placement_conflict.empty()) {
                        ImGui::TextColored(ImVec4(1.0F, 0.4F, 0.35F, 1.0F), ICON_FA_TRIANGLE_EXCLAMATION " %s", ui.placement_conflict.c_str());
                    }
                }
            } else if (ui.city_tool == CityTool::Piece && !project->blocks.empty()) {
                draw_piece_brush(*project, ui, status);
                if (ImGui::Button(ICON_FA_ROTATE " Turn")) ui.selected_piece_rotation = (ui.selected_piece_rotation + 1) & 3;
                help_tooltip(ui, "Turn the piece before placing it", EditorAction::RotateSelection);
                ImGui::SameLine();
                ImGui::Text("%zu degrees", ui.selected_piece_rotation * 90);
                draw_place_height(ui);
            } else if (ui.city_tool == CityTool::Road) {
                if (ImGui::CollapsingHeader("Laying roads", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::SliderInt("##new_road_width", &ui.new_road_width, 2, 24, "Width: %d cells");
                    draw_place_height(ui);
                    ImGui::TextWrapped(ui.pending_road_start ? "Click to lay the next segment; Esc finishes." : "Click to start a road.");
                }
            } else if (ui.city_tool == CityTool::Erase) {
                ImGui::TextWrapped("Click or drag over the highlighted building, road, or piece to delete it.");
            } else {
                ImGui::TextWrapped("Nothing selected. Click a building, road, junction, or piece.");
            }
            if (ImGui::CollapsingHeader("Map", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::TextDisabled("%zu buildings, %zu roads, %zu pieces", project->city->data.buildings.size(),
                    project->city->data.streets.size(), project->city->data.portals.size());
                draw_map_limit_warnings(*project);
            }
        } else if (ui.city_selection == CitySelectionKind::Node && !project->city->data.intersections.empty()) {
            ui.selected_city_intersection = std::min(ui.selected_city_intersection, project->city->data.intersections.size() - 1U);
            auto& node = project->city->data.intersections[ui.selected_city_intersection];
            if (ImGui::CollapsingHeader("Road junction", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("Position: %d, %d, %d", node.x, node.y, node.z);
                int level = node.y;
                field_label("Level");
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputInt("##junction_level", &level) && level != node.y) {
                    begin_city_action(*project, ui, "move junction");
                    nudge_city_junction_level(*project, ui, static_cast<std::int64_t>(level) - node.y);
                }
                help_tooltip(ui, "Junction height in cells; its roads slope to meet it", EditorAction::NudgeUp);
                if (ImGui::Button("Start road here")) {
                    ui.city_tool = CityTool::Road;
                    ui.pending_road_start = CityGridPoint{node.x, node.y, node.z};
                }
            }
        } else if (ui.city_selection == CitySelectionKind::Road && !project->city->data.streets.empty()) {
            ui.selected_city_street = std::min(ui.selected_city_street, project->city->data.streets.size() - 1U);
            auto& street = project->city->data.streets[ui.selected_city_street];
            draw_struct_inspector_header("SBC", "Street", street.name);
            if (draw_struct_section("geometry")) {
                int width = street.left_width + street.right_width;
                field_label("Road width");
                if (ImGui::SliderInt("##road_width", &width, 2, 24, "%d cells")) {
                    begin_city_action(*project, ui, "resize road");
                    street.left_width = width / 2;
                    street.right_width = width - street.left_width;
                    ui.city_dirty = true;
                }
            }
            if (ImGui::CollapsingHeader("Height", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::Button(ICON_FA_ARROW_UP " Raise")) {
                    begin_city_action(*project, ui, "raise road");
                    nudge_city_junction_level(*project, ui, 1);
                }
                help_tooltip(ui, "Move both ends of the road up one cell", EditorAction::NudgeUp);
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_ARROW_DOWN " Lower")) {
                    begin_city_action(*project, ui, "lower road");
                    nudge_city_junction_level(*project, ui, -1);
                }
                help_tooltip(ui, "Move both ends of the road down one cell", EditorAction::NudgeDown);
                if (ImGui::Button("Continue road")) {
                    ui.city_tool = CityTool::Road;
                    if (street.to_intersection >= 0 && static_cast<std::size_t>(street.to_intersection) < project->city->data.intersections.size()) {
                        const auto& node = project->city->data.intersections[static_cast<std::size_t>(street.to_intersection)];
                        ui.pending_road_start = CityGridPoint{node.x, node.y, node.z};
                    }
                }
            }
            if (ImGui::CollapsingHeader("Actions", ImGuiTreeNodeFlags_DefaultOpen)) {
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
            }
        } else if (ui.city_selection == CitySelectionKind::Building && !project->city->data.buildings.empty()) {
            ui.selected_city_building = std::min(ui.selected_city_building, project->city->data.buildings.size() - 1U);
            auto& building = project->city->data.buildings[ui.selected_city_building];
            draw_struct_inspector_header("SBC", "Building", building.name);
            if (ui.selected_city_buildings.size() > 1U) {
                ImGui::TextDisabled("%zu buildings selected; actions apply to all.", ui.selected_city_buildings.size());
            }
            if (draw_struct_section("transform")) {
                ImGui::Text("Position: %d, %d, %d", building.x, building.y, building.z);
                ImGui::TextWrapped("Drag an arrow to move, the ring to rotate. Arrow keys nudge.");
                int rotation = building.rotation & 3;
                constexpr const char* rotations[] = {"0 degrees", "90 degrees", "180 degrees", "270 degrees"};
                field_label("Rotation");
                if (ImGui::Combo("##rotation", &rotation, rotations, 4)) {
                    begin_city_action(*project, ui, "rotate building");
                    nudge_city_building(*project, ui, 0, 0, 0, (rotation - building.rotation) & 3);
                }
            }
            if (ImGui::CollapsingHeader("Actions", ImGuiTreeNodeFlags_DefaultOpen)) {
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
            }
        } else if (ui.city_selection == CitySelectionKind::Piece) {
            if (ImGui::CollapsingHeader("Piece", ImGuiTreeNodeFlags_DefaultOpen)) {
                // The voxel word holds the city-block record (low 12 bits) and the
                // quarter-turn rotation (bits 24-25).
                const auto& city_blocks = project->city->data.city_blocks;
                const auto block_index = static_cast<std::size_t>(ui.selected_piece_word & 0xfffU);
                if (block_index < city_blocks.size()) {
                    const auto& city_block = city_blocks[block_index];
                    ImGui::Text("Piece: %s", std::string{city_block_display_name(city_block)}.c_str());
                    // Repaint: the 8 texture names of the layer the voxel uses.
                    const auto layer = std::min<std::size_t>(std::max(1U, (ui.selected_piece_word >> 16U) & 0x3fU) - 1U,
                        city_block.layers.empty() ? 0U : city_block.layers.size() - 1U);
                    auto textures = city_block.layers.empty() ? std::array<std::string, 8>{} : city_block.layers[layer];
                    if (ImGui::CollapsingHeader("Textures", ImGuiTreeNodeFlags_DefaultOpen)) {
                        const bool repaint = texture_slots(*project, "repaint", textures);
                        if (repaint) {
                            begin_city_action(*project, ui, "repaint piece");
                            repaint_city_piece(*project, ui, textures, status);
                        }
                    }
                }
                ImGui::Text("Rotation: %u degrees", ((ui.selected_piece_word >> 24U) & 3U) * 90U);
                if (ui.selected_piece_voxel.has_value()) {
                    ImGui::Text("Cell: %d, %d, %d", ui.selected_piece_voxel->x, ui.selected_piece_voxel->y, ui.selected_piece_voxel->z);
                }
                ImGui::TextWrapped("Arrow keys and PgUp/PgDn move it one cell; Delete removes it.");
            }
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

} // namespace pafta::app_internal
