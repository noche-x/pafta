// SPDX-License-Identifier: GPL-2.0-only
// Floating tool windows, shortcuts you can change, and preferences.
#include "app_internal.hpp"

#include <imgui.h>
#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <cerrno>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <functional>
#include <sstream>

#include <extras/FA6FreeSolidFontData.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#endif

extern const unsigned char pafta_editor_font[];
extern const std::size_t pafta_editor_font_size;

namespace pafta::app_internal {
namespace {

// The default keys avoid ones the browser or OS grabs first. Chrome keeps
// Ctrl+T/N/W, so the toggles for the docked Tools, Library, and Properties windows
// have no key (the Windows menu has them). macOS hides the app on Cmd+H, so Changes
// is Ctrl/Cmd+Shift+H. Rows follow the EditorAction order.
constexpr std::array<ShortcutInfo, static_cast<std::size_t>(EditorAction::Count)> kShortcutInfos{{
    {"save", "Save map", {KEY_S, true, false, false}},
    {"undo", "Undo", {KEY_Z, true, false, false}},
    {"redo", "Redo", {KEY_Y, true, false, false}},
    {"focus_selection", "Frame the selection", {KEY_F, false, false, false}},
    {"frame_city", "Show the whole city", {KEY_HOME, false, false, false}},
    {"tool_select", "Tool 1: Select (city and building)", {KEY_ONE, false, false, false}},
    {"tool_building", "Tool 2: Place building / Add block", {KEY_TWO, false, false, false}},
    {"tool_road", "Tool 3: Road / Paint", {KEY_THREE, false, false, false}},
    {"tool_piece", "Tool 4: Piece / Anchor", {KEY_FOUR, false, false, false}},
    {"tool_erase", "Tool 5: Erase / Item set", {KEY_FIVE, false, false, false}},
    {"rotate", "Rotate selection / piece brush", {KEY_R, false, false, false}},
    {"duplicate", "Duplicate selection", {KEY_D, true, false, false}},
    {"delete", "Delete selection / selected item set", {KEY_DELETE, false, false, false}},
    {"nudge_left", "Nudge selection -X", {KEY_LEFT, false, false, false}},
    {"nudge_right", "Nudge selection +X", {KEY_RIGHT, false, false, false}},
    {"nudge_forward", "Nudge selection -Z", {KEY_UP, false, false, false}},
    {"nudge_back", "Nudge selection +Z", {KEY_DOWN, false, false, false}},
    {"nudge_up", "Nudge selection up", {KEY_PAGE_UP, false, false, false}},
    {"nudge_down", "Nudge selection down", {KEY_PAGE_DOWN, false, false, false}},
    {"next_cell", "Next building cell / buildblock face (Shift reverses)", {KEY_TAB, false, false, false}},
    // Keep the old Preferences alias so saved bindings that use it still load.
    {"tab_editor", "Mode: Preferences window", {0, false, false, false}},
    {"tab_city", "Mode: City", {KEY_ONE, true, false, false}},
    {"tab_buildings", "Mode: Building", {KEY_TWO, true, false, false}},
    {"window_building_browser", "Window: Building browser", {KEY_B, true, false, false}},
    {"window_block_browser", "Window: Block browser", {KEY_K, true, false, false}},
    {"window_buildblock", "Window: Buildblock", {KEY_L, true, false, false}},
    {"window_changes", "Window: Changes since save", {KEY_H, true, true, false}},
    {"window_shortcuts", "Window: Keyboard shortcuts", {KEY_SLASH, true, false, false}},
    {"window_preferences", "Window: Preferences", {KEY_COMMA, true, false, false}},
    {"toggle_shadows", "Toggle shadows", {KEY_S, false, true, false}},
    {"toggle_sky", "Toggle sky", {KEY_K, false, true, false}},
    {"graphics_debug", "Graphics debug overlay", {KEY_F3, false, false, false}},
    {"window_tools", "Window: Asset drawer", {0, false, false, false}},
    {"window_library", "Window: Outliner", {0, false, false, false}},
    {"window_properties", "Window: Properties", {0, false, false, false}},
}};

std::size_t index_of(EditorAction action) {
    return static_cast<std::size_t>(action);
}

bool modifier_down(int left, int right) {
    return IsKeyDown(left) || IsKeyDown(right);
}

bool ctrl_down() {
#ifdef __APPLE__
    return modifier_down(KEY_LEFT_CONTROL, KEY_RIGHT_CONTROL) || modifier_down(KEY_LEFT_SUPER, KEY_RIGHT_SUPER);
#else
    return modifier_down(KEY_LEFT_CONTROL, KEY_RIGHT_CONTROL);
#endif
}

bool is_modifier_key(int key) {
    return key == KEY_LEFT_CONTROL || key == KEY_RIGHT_CONTROL || key == KEY_LEFT_SHIFT ||
        key == KEY_RIGHT_SHIFT || key == KEY_LEFT_ALT || key == KEY_RIGHT_ALT ||
        key == KEY_LEFT_SUPER || key == KEY_RIGHT_SUPER;
}

std::string named_key(int key) {
    switch (key) {
        case KEY_ESCAPE: return "Esc";
        case KEY_ENTER: return "Enter";
        case KEY_BACKSPACE: return "Backspace";
        case KEY_INSERT: return "Ins";
        case KEY_HOME: return "Home";
        case KEY_END: return "End";
        case KEY_F1: return "F1";
        case KEY_F2: return "F2";
        case KEY_F3: return "F3";
        case KEY_F4: return "F4";
        case KEY_F5: return "F5";
        case KEY_F6: return "F6";
        case KEY_F7: return "F7";
        case KEY_F8: return "F8";
        case KEY_F9: return "F9";
        case KEY_F10: return "F10";
        case KEY_F11: return "F11";
        case KEY_F12: return "F12";
        default: return key_label(key);
    }
}

// Builds and draws one offscreen view you can orbit. `draw` runs inside BeginMode3D,
// with the game surface shader's camera already set.
void draw_preview_view(PreviewView& view, const char* id, ImVec2 size, Vector3 center, float radius,
                       const std::function<void()>& draw) {
    const int width = std::max(16, static_cast<int>(size.x));
    const int height = std::max(16, static_cast<int>(size.y));
    if (view.target.id == 0U || view.target.texture.width != width || view.target.texture.height != height) {
        if (view.target.id != 0U) UnloadRenderTexture(view.target);
        view.target = LoadRenderTexture(width, height);
    }
    const float distance = std::max(8.0F, radius * 2.4F) * view.zoom;
    const float cp = std::cos(view.pitch);
    Camera3D cam{};
    cam.position = {
        center.x + distance * cp * std::sin(view.yaw),
        center.y + distance * std::sin(view.pitch),
        center.z + distance * cp * std::cos(view.yaw),
    };
    cam.target = center;
    cam.up = {0.0F, 1.0F, 0.0F};
    cam.fovy = 45.0F;
    cam.projection = CAMERA_PERSPECTIVE;

    begin_game_texture_mode(view.target);
    ClearBackground(Color{30, 33, 38, 255});
    BeginMode3D(cam);
    set_game_surface_camera(cam.position);
    rlEnableDepthTest();
    draw();
    EndMode3D();
    end_game_texture_mode();

    ImGui::Image(
        ImTextureID(view.target.texture.id),
        ImVec2(static_cast<float>(width), static_cast<float>(height)),
        ImVec2(0.0F, 1.0F), ImVec2(1.0F, 0.0F));
    if (ImGui::IsItemHovered()) {
        const auto& io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0F) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0F)) {
            view.yaw -= io.MouseDelta.x * 0.01F;
            view.pitch = std::clamp(view.pitch + io.MouseDelta.y * 0.01F, -1.4F, 1.4F);
        }
        if (io.MouseWheel != 0.0F) {
            view.zoom = std::clamp(view.zoom * std::pow(0.9F, io.MouseWheel), 0.2F, 6.0F);
        }
    }
    ImGui::PushID(id);
    ImGui::TextDisabled("Drag to orbit, wheel to zoom");
    ImGui::PopID();
}

void building_bounds(const SbbBuilding& building, Vector3& center, float& radius) {
    constexpr float unit = 4.0F;
    const float sx = static_cast<float>(std::max(1U, building.size_x)) * unit;
    const float sy = static_cast<float>(std::max(1U, building.size_z)) * unit;
    const float sz = static_cast<float>(std::max(1U, building.size_y)) * unit;
    radius = 0.5F * std::sqrt(sx * sx + sy * sy + sz * sz);
    center = {
        (static_cast<float>(building.size_x) * 0.5F - static_cast<float>(building.anchor_x)) * unit,
        (static_cast<float>(building.size_z) * 0.5F - static_cast<float>(building.anchor_y)) * unit,
        (static_cast<float>(building.size_y) * 0.5F - static_cast<float>(building.anchor_z)) * unit,
    };
}

void block_bounds(const SblBlockMesh& block, float scale, Vector3& center, float& radius) {
    Vector3 lo{1e9F, 1e9F, 1e9F};
    Vector3 hi{-1e9F, -1e9F, -1e9F};
    bool any = false;
    for (const auto& face : block.faces) {
        for (const auto& v : face.vertices) {
            lo = Vector3Min(lo, {v.x * scale, v.y * scale, v.z * scale});
            hi = Vector3Max(hi, {v.x * scale, v.y * scale, v.z * scale});
            any = true;
        }
    }
    if (!any) {
        lo = {0.0F, 0.0F, 0.0F};
        hi = {scale, scale, scale};
    }
    center = Vector3Scale(Vector3Add(lo, hi), 0.5F);
    radius = std::max(1.0F, 0.5F * Vector3Length(Vector3Subtract(hi, lo)));
}


// Only apply the staggered positions on first use or when the layout is reset.
void place_floating_window(const EditorUi& ui, int slot, ImVec2 size) {
    const auto* viewport = ImGui::GetMainViewport();
    const ImGuiCond cond = ui.reset_layout ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowSize(size, cond);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5F + static_cast<float>(slot) * 40.0F,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5F + static_cast<float>(slot) * 40.0F),
        cond, ImVec2(0.5F, 0.5F));
}

} // namespace

// A name cut to fit a thumbnail's width (ending in "..."), so labels never widen a
// grid cell and push the last column out of view.
std::string fit_label(const std::string& name, float width) {
    if (ImGui::CalcTextSize(name.c_str()).x <= width) return name;
    std::string label = name;
    while (!label.empty() && ImGui::CalcTextSize((label + "...").c_str()).x > width) label.pop_back();
    return label + "...";
}

const RenderTexture2D* block_thumbnail(EditorProject& project, std::size_t index) {
    if (index >= project.blocks.size()) return nullptr;
    if (project.block_thumbnails.size() != project.blocks.size()) {
        project.block_thumbnails.resize(project.blocks.size());
        project.block_thumbnail_ready.assign(project.blocks.size(), false);
    }
    if (project.block_thumbnail_ready[index]) return &project.block_thumbnails[index];
    if (project.thumbnail_budget <= 0) return nullptr;
    --project.thumbnail_budget;

    constexpr int kSize = 128;
    auto& target = project.block_thumbnails[index];
    if (target.id == 0U) target = LoadRenderTexture(kSize, kSize);
    Vector3 center{};
    float radius = 1.0F;
    block_bounds(project.blocks[index].data, 4.0F, center, radius);
    const float dist = radius * 2.4F;
    Camera3D cam{};
    cam.position = {center.x + dist * 0.7F, center.y + dist * 0.6F, center.z + dist * 0.7F};
    cam.target = center;
    cam.up = {0.0F, 1.0F, 0.0F};
    cam.fovy = 45.0F;
    cam.projection = CAMERA_PERSPECTIVE;
    begin_game_texture_mode(target);
    ClearBackground(Color{32, 36, 41, 255});
    BeginMode3D(cam);
    rlEnableDepthTest();
    draw_mesh_faces(project.blocks[index].data, Vec3{}, 4.0F, {98, 148, 204, 220});
    EndMode3D();
    end_game_texture_mode();
    project.block_thumbnail_ready[index] = true;
    return &target;
}

namespace {

enum class MapChange { Added, Removed, Edited };

struct MapDiffTarget {
    MapChange change{MapChange::Edited};
    CitySelectionKind kind{CitySelectionKind::None};
    std::size_t index{};
};

// Works out what an undo step changed and picks the first thing to focus on.
MapDiffTarget diff_maps(const SbcMap& before, const SbcMap& after) {
    MapDiffTarget t;
    if (after.buildings.size() > before.buildings.size()) {
        t.change = MapChange::Added; t.kind = CitySelectionKind::Building; t.index = before.buildings.size(); return t;
    }
    if (after.buildings.size() < before.buildings.size()) {
        t.change = MapChange::Removed; t.kind = CitySelectionKind::Building;
        t.index = before.buildings.empty() ? 0 : std::min(after.buildings.size(), before.buildings.size() - 1); return t;
    }
    for (std::size_t i = 0; i < after.buildings.size(); ++i) {
        const auto& x = before.buildings[i];
        const auto& y = after.buildings[i];
        if (x.name != y.name || x.x != y.x || x.y != y.y || x.z != y.z || x.rotation != y.rotation) {
            t.kind = CitySelectionKind::Building; t.index = i; return t;
        }
    }
    if (after.streets.size() != before.streets.size()) {
        t.change = after.streets.size() > before.streets.size() ? MapChange::Added : MapChange::Removed;
        t.kind = CitySelectionKind::Road; t.index = std::min(before.streets.size(), after.streets.empty() ? 0 : after.streets.size() - 1); return t;
    }
    for (std::size_t i = 0; i < after.streets.size(); ++i) {
        const auto& x = before.streets[i];
        const auto& y = after.streets[i];
        if (x.from_intersection != y.from_intersection || x.to_intersection != y.to_intersection ||
            x.left_width != y.left_width || x.right_width != y.right_width) {
            t.kind = CitySelectionKind::Road; t.index = i; return t;
        }
    }
    if (after.intersections.size() != before.intersections.size()) {
        t.change = after.intersections.size() > before.intersections.size() ? MapChange::Added : MapChange::Removed;
        t.kind = CitySelectionKind::Node; t.index = std::min(before.intersections.size(), after.intersections.empty() ? 0 : after.intersections.size() - 1); return t;
    }
    for (std::size_t i = 0; i < after.intersections.size(); ++i) {
        const auto& x = before.intersections[i];
        const auto& y = after.intersections[i];
        if (x.x != y.x || x.y != y.y || x.z != y.z) { t.kind = CitySelectionKind::Node; t.index = i; return t; }
    }
    if (after.portals.size() != before.portals.size()) {
        t.change = after.portals.size() > before.portals.size() ? MapChange::Added : MapChange::Removed;
        t.kind = CitySelectionKind::Piece; t.index = std::min(before.portals.size(), after.portals.empty() ? 0 : after.portals.size() - 1); return t;
    }
    for (std::size_t i = 0; i < after.portals.size(); ++i) {
        const auto& x = before.portals[i].voxel_blocks();
        const auto& y = after.portals[i].voxel_blocks();
        if (x != y) {
            // Added if any cell went from empty to set, and none went the other way.
            bool added = false;
            bool removed = false;
            for (std::size_t k = 0; k < std::min(x.size(), y.size()); ++k) {
                if (x[k] == 0 && y[k] != 0) added = true;
                if (x[k] != 0 && y[k] == 0) removed = true;
            }
            t.change = added && !removed ? MapChange::Added : removed && !added ? MapChange::Removed : MapChange::Edited;
            t.kind = CitySelectionKind::Piece; t.index = i; return t;
        }
    }
    return t;
}

void focus_diff_target(EditorProject& project, EditorUi& ui, const MapDiffTarget& target) {
    if (!project.city || target.kind == CitySelectionKind::None) return;
    const auto& map = project.city->data;
    ui.requested_tab = EditorTab::City;
    ui.city_tool = CityTool::Select;
    switch (target.kind) {
        case CitySelectionKind::Building:
            if (target.index >= map.buildings.size()) return;
            ui.city_selection = CitySelectionKind::Building;
            ui.selected_city_building = target.index;
            ui.selected_city_buildings = {target.index};
            break;
        case CitySelectionKind::Road:
            if (target.index >= map.streets.size()) return;
            ui.city_selection = CitySelectionKind::Road;
            ui.selected_city_street = target.index;
            break;
        case CitySelectionKind::Node:
            if (target.index >= map.intersections.size()) return;
            ui.city_selection = CitySelectionKind::Node;
            ui.selected_city_intersection = target.index;
            break;
        case CitySelectionKind::Piece: {
            if (target.index >= map.portals.size()) return;
            const auto& portal = map.portals[target.index];
            ui.city_selection = CitySelectionKind::Piece;
            ui.selected_portal = target.index;
            ui.selected_piece_voxel = CityGridPoint{sbc_portal_cell_base(portal.fields[1]) + 4, sbc_portal_cell_base(portal.fields[2]), sbc_portal_cell_base(portal.fields[3]) + 4};
            break;
        }
        default:
            return;
    }
    ui.focus_selection_requested = true;
}

} // namespace

const ShortcutInfo& shortcut_info(EditorAction action) {
    return kShortcutInfos[index_of(action)];
}

void reset_shortcuts(EditorUi& ui) {
    for (std::size_t i = 0; i < kShortcutInfos.size(); ++i) {
        ui.shortcuts[i] = kShortcutInfos[i].default_binding;
    }
}

bool action_pressed(const EditorUi& ui, EditorAction action) {
    // While flying (right mouse held, cursor captured), the keys steer the camera,
    // so Ctrl+S or Ctrl+Z must not save or undo in the middle of a flight.
    if (ui.capturing_shortcut.has_value() || IsCursorHidden()) return false;
    const auto& binding = ui.shortcuts[index_of(action)];
    if (binding.key == 0) return false;
    if (!IsKeyPressed(binding.key)) return false;
    const bool ctrl = ctrl_down();
    const bool shift = modifier_down(KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT);
    const bool alt = modifier_down(KEY_LEFT_ALT, KEY_RIGHT_ALT);
    // Shift may differ for actions that list a Shift version.
    const bool shift_free = action == EditorAction::NextCell || action == EditorAction::RotateSelection;
    return ctrl == binding.ctrl && alt == binding.alt && (shift_free || shift == binding.shift);
}

std::string shortcut_label(const Shortcut& shortcut) {
    if (shortcut.key == 0) return "unbound";
    std::string label;
    if (shortcut.ctrl) {
#ifdef __APPLE__
        label += "Cmd+";
#else
        label += "Ctrl+";
#endif
    }
    if (shortcut.shift) label += "Shift+";
    if (shortcut.alt) label += "Alt+";
    return label + named_key(shortcut.key);
}

std::string action_shortcut_label(const EditorUi& ui, EditorAction action) {
    return shortcut_label(ui.shortcuts[index_of(action)]);
}

std::filesystem::path preferences_path() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) home = std::getenv("USERPROFILE");
    const std::filesystem::path base = home == nullptr ? std::filesystem::temp_directory_path() : std::filesystem::path{home};
    return base / ".pafta" / "prefs.txt";
}

void load_preferences(EditorUi& ui) {
    reset_shortcuts(ui);
#ifdef __EMSCRIPTEN__
    web_restore_file(preferences_path(), "pafta.prefs");
#endif
    std::ifstream in(preferences_path());
    if (!in) return;
    // Preferences can be edited outside Pafta. Reject broken values and keep the
    // defaults or the last good setting.
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const auto key = line.substr(0, eq);
        const auto value = line.substr(eq + 1);
        const auto read_float = [&](float& setting, float minimum, float maximum) {
            char* end = nullptr;
            errno = 0;
            const auto number = std::strtof(value.c_str(), &end);
            if (end == value.c_str() || errno == ERANGE || !std::isfinite(number)) {
                return;
            }
            const auto finish = value.c_str() + value.size();
            while (end != finish && std::isspace(static_cast<unsigned char>(*end))) {
                ++end;
            }
            if (end != finish) {
                return;
            }
            setting = std::clamp(number, minimum, maximum);
        };
        const auto read_int = [&](int& setting, int minimum, int maximum) {
            char* end = nullptr;
            errno = 0;
            const auto number = std::strtol(value.c_str(), &end, 10);
            if (end == value.c_str() || errno == ERANGE || number < std::numeric_limits<int>::min() ||
                number > std::numeric_limits<int>::max()) {
                return;
            }
            const auto finish = value.c_str() + value.size();
            while (end != finish && std::isspace(static_cast<unsigned char>(*end))) {
                ++end;
            }
            if (end != finish) {
                return;
            }
            setting = std::clamp(static_cast<int>(number), minimum, maximum);
        };
        const auto read_bool = [&](bool& setting) {
            if (value == "0") {
                setting = false;
            } else if (value == "1") {
                setting = true;
            }
        };
        if (key.rfind("shortcut.", 0) == 0) {
            const auto name = key.substr(9);
            for (std::size_t i = 0; i < kShortcutInfos.size(); ++i) {
                if (name != kShortcutInfos[i].name) continue;
                Shortcut shortcut{};
                int ctrl = 0;
                int shift = 0;
                int alt = 0;
                char first_comma = 0;
                char second_comma = 0;
                char third_comma = 0;
                std::istringstream fields(value);
                if (!(fields >> shortcut.key >> first_comma >> ctrl >> second_comma >> shift >> third_comma >> alt)) continue;
                fields >> std::ws;
                if (!fields.eof() || first_comma != ',' || second_comma != ',' || third_comma != ',' || shortcut.key < 0 ||
                    (ctrl != 0 && ctrl != 1) || (shift != 0 && shift != 1) || (alt != 0 && alt != 1)) continue;
                shortcut.ctrl = ctrl == 1;
                shortcut.shift = shift == 1;
                shortcut.alt = alt == 1;
                ui.shortcuts[i] = shortcut;
            }
        } else if (key == "font_size") {
            read_float(ui.ui_font_size, 10.0F, 28.0F);
        } else if (key == "render.shadows") {
            read_bool(ui.render_shadows);
        } else if (key == "render.sky") {
            read_bool(ui.render_sky);
        } else if (key == "render.shadow_distance") {
            read_float(ui.shadow_distance, 64.0F, 2000.0F);
        } else if (key == "render.terrain") {
            read_bool(ui.render_terrain);
        } else if (key == "render.near_lod") {
            read_bool(ui.near_lod);
        } else if (key == "render.scale") {
            read_float(ui.render_scale, 0.5F, 1.0F);
        } else if (key == "render.building_distance") {
            read_float(ui.building_distance, 0.0F, 4000.0F);
        } else if (key == "render.half_textures") {
            read_bool(ui.half_textures);
        } else if (key == "render.vsync") {
            read_bool(ui.vsync);
        } else if (key == "render.fps_limit") {
            read_int(ui.fps_limit, 0, 240);
        } else if (key == "render.background_throttle") {
            read_bool(ui.background_throttle);
        } else if (key == "render.street_objects") {
            read_bool(ui.street_objects);
        } else if (key == "render.live_reflections") {
            read_bool(ui.live_reflections);
        } else if (key == "render.anisotropy") {
            read_int(ui.anisotropy, 1, 16);
        } else if (key == "render.world_time") {
            read_float(ui.world_time_hours, 0.0F, 24.0F);
        } else if (key == "view.coordinates") {
            read_bool(ui.show_coordinates);
        } else if (key == "map.street_labels") {
            read_bool(ui.map_menu.street_labels);
        } else if (key == "map.font") {
            ui.map_menu.font = value;
        } else if (key == "map.export_size") {
            read_int(ui.map_export_size, 64, 4096);
        } else if (key == "camera.fly_speed") {
            read_float(ui.camera_fly_speed, 2.0F, 512.0F);
        } else if (key == "camera.blender_mouse") {
            read_bool(ui.blender_mouse);
        } else if (key == "render.interiors_selected_only") {
            read_bool(ui.interiors_selected_only);
        } else if (key == "window.toolbar") {
            read_bool(ui.show_toolbar_window);
        } else if (key == "window.outliner") {
            read_bool(ui.show_library_window);
        } else if (key == "window.properties") {
            read_bool(ui.show_properties_window);
        } else if (key == "window.assets") {
            read_bool(ui.show_asset_drawer);
        } else if (key == "load.loose_first") {
            read_bool(ui.loose_files_win);
        } else if (key == "export.mesh_folder") {
            ui.mesh_export_folder = value;
        }
    }
}

#ifdef __EMSCRIPTEN__
// The browser build's file system only lives in memory, so settings files are
// copied into localStorage to survive a reload.
void web_restore_file(const std::filesystem::path& path, const char* key) {
    EM_ASM({
        try {
            const value = localStorage.getItem(UTF8ToString($0));
            if (value === null) return;
            const file = UTF8ToString($1);
            const dir = file.substring(0, file.lastIndexOf('/'));
            if (dir) FS.mkdirTree(dir);
            FS.writeFile(file, value);
        } catch (error) { console.warn('restore ' + UTF8ToString($0) + ': ' + error); }
    }, key, path.string().c_str());
}

void web_store_file(const std::filesystem::path& path, const char* key) {
    EM_ASM({
        try {
            if (!FS.analyzePath(UTF8ToString($1)).exists) return;  // not written yet
            localStorage.setItem(UTF8ToString($0), FS.readFile(UTF8ToString($1), {encoding: 'utf8'}));
        } catch (error) { console.warn('store ' + UTF8ToString($0) + ': ' + error); }
    }, key, path.string().c_str());
}
#endif

void save_preferences(const EditorUi& ui) {
    std::ostringstream out;
    out << "font_size=" << ui.ui_font_size << '\n';
    out << "render.shadows=" << (ui.render_shadows ? 1 : 0) << '\n';
    out << "render.sky=" << (ui.render_sky ? 1 : 0) << '\n';
    out << "render.shadow_distance=" << ui.shadow_distance << '\n';
    out << "render.terrain=" << (ui.render_terrain ? 1 : 0) << '\n';
    out << "render.near_lod=" << (ui.near_lod ? 1 : 0) << '\n';
    out << "render.scale=" << ui.render_scale << '\n';
    out << "render.building_distance=" << ui.building_distance << '\n';
    out << "render.half_textures=" << (ui.half_textures ? 1 : 0) << '\n';
    out << "render.world_time=" << ui.world_time_hours << '\n';
    out << "render.vsync=" << (ui.vsync ? 1 : 0) << '\n';
    out << "render.fps_limit=" << ui.fps_limit << '\n';
    out << "render.background_throttle=" << (ui.background_throttle ? 1 : 0) << '\n';
    out << "render.street_objects=" << (ui.street_objects ? 1 : 0) << '\n';
    out << "render.live_reflections=" << (ui.live_reflections ? 1 : 0) << '\n';
    out << "render.anisotropy=" << ui.anisotropy << '\n';
    out << "camera.fly_speed=" << ui.camera_fly_speed << '\n';
    out << "camera.blender_mouse=" << (ui.blender_mouse ? 1 : 0) << '\n';
    out << "render.interiors_selected_only=" << (ui.interiors_selected_only ? 1 : 0) << '\n';
    out << "window.toolbar=" << (ui.show_toolbar_window ? 1 : 0) << '\n';
    out << "window.outliner=" << (ui.show_library_window ? 1 : 0) << '\n';
    out << "window.properties=" << (ui.show_properties_window ? 1 : 0) << '\n';
    out << "window.assets=" << (ui.show_asset_drawer ? 1 : 0) << '\n';
    out << "export.mesh_folder=" << ui.mesh_export_folder << '\n';
    out << "load.loose_first=" << (ui.loose_files_win ? 1 : 0) << '\n';
    out << "view.coordinates=" << (ui.show_coordinates ? 1 : 0) << '\n';
    out << "map.street_labels=" << (ui.map_menu.street_labels ? 1 : 0) << '\n';
    out << "map.font=" << ui.map_menu.font << '\n';
    out << "map.export_size=" << ui.map_export_size << '\n';
    for (std::size_t i = 0; i < kShortcutInfos.size(); ++i) {
        const auto& s = ui.shortcuts[i];
        out << "shortcut." << kShortcutInfos[i].name << '=' << s.key << ',' << (s.ctrl ? 1 : 0) << ','
            << (s.shift ? 1 : 0) << ',' << (s.alt ? 1 : 0) << '\n';
    }
    const auto text = out.str();
    try {
        const auto path = preferences_path();
        pafta::write_file_bytes(path, std::vector<std::uint8_t>{text.begin(), text.end()});
#ifdef __EMSCRIPTEN__
        web_store_file(path, "pafta.prefs");
#endif
    } catch (const std::exception& error) {
        std::string failure;
        report_failure(failure, std::string{"Preferences save failed: "} + error.what());
        std::cerr << failure << '\n';
    }
}

// Light style by Pacome Danhiez (itamago), imgui PR #511, mapped onto today's
// ImGuiCol names. Fonts must be added before rlImGuiEndInitImGui.
void apply_editor_style(float font_size) {
    auto& io = ImGui::GetIO();
    // The font is compiled in (CMakeLists.txt makes editor_font.cpp), so no asset
    // folder has to sit next to the binary. The atlas only reads it.
    ImFontConfig text;
    text.FontDataOwnedByAtlas = false;
    io.FontDefault = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(pafta_editor_font),
        static_cast<int>(pafta_editor_font_size), font_size, &text);
    // Giving every icon the same width keeps labels lined up across toolbar buttons.
    static const ImWchar icon_ranges[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
    ImFontConfig icons;
    icons.MergeMode = true;
    icons.PixelSnapH = true;
    icons.FontDataOwnedByAtlas = false;
    icons.GlyphMinAdvanceX = font_size * 1.3F;
    io.Fonts->AddFontFromMemoryCompressedTTF(fa_solid_900_compressed_data, static_cast<int>(fa_solid_900_compressed_size),
        font_size * 0.9F, &icons, icon_ranges);

    ImGuiStyle& style = ImGui::GetStyle();
    style.Alpha = 1.0f;
    style.FrameRounding = 3.0f;
    style.Colors[ImGuiCol_Text]                  = ImVec4(0.00f, 0.00f, 0.00f, 1.00f);
    style.Colors[ImGuiCol_TextDisabled]          = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
    style.Colors[ImGuiCol_WindowBg]              = ImVec4(0.94f, 0.94f, 0.94f, 0.94f);
    style.Colors[ImGuiCol_ChildBg]               = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    style.Colors[ImGuiCol_PopupBg]               = ImVec4(1.00f, 1.00f, 1.00f, 0.94f);
    style.Colors[ImGuiCol_Border]                = ImVec4(0.00f, 0.00f, 0.00f, 0.39f);
    style.Colors[ImGuiCol_BorderShadow]          = ImVec4(1.00f, 1.00f, 1.00f, 0.10f);
    style.Colors[ImGuiCol_FrameBg]               = ImVec4(1.00f, 1.00f, 1.00f, 0.94f);
    style.Colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.26f, 0.59f, 0.98f, 0.40f);
    style.Colors[ImGuiCol_FrameBgActive]         = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
    style.Colors[ImGuiCol_TitleBg]               = ImVec4(0.96f, 0.96f, 0.96f, 1.00f);
    style.Colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(1.00f, 1.00f, 1.00f, 0.51f);
    style.Colors[ImGuiCol_TitleBgActive]         = ImVec4(0.82f, 0.82f, 0.82f, 1.00f);
    style.Colors[ImGuiCol_MenuBarBg]             = ImVec4(0.86f, 0.86f, 0.86f, 1.00f);
    style.Colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.98f, 0.98f, 0.98f, 0.53f);
    style.Colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.69f, 0.69f, 0.69f, 1.00f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.59f, 0.59f, 0.59f, 1.00f);
    style.Colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.49f, 0.49f, 0.49f, 1.00f);
    style.Colors[ImGuiCol_CheckMark]             = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    style.Colors[ImGuiCol_SliderGrab]            = ImVec4(0.24f, 0.52f, 0.88f, 1.00f);
    style.Colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    style.Colors[ImGuiCol_Button]                = ImVec4(0.26f, 0.59f, 0.98f, 0.40f);
    style.Colors[ImGuiCol_ButtonHovered]         = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    style.Colors[ImGuiCol_ButtonActive]          = ImVec4(0.06f, 0.53f, 0.98f, 1.00f);
    style.Colors[ImGuiCol_Header]                = ImVec4(0.26f, 0.59f, 0.98f, 0.31f);
    style.Colors[ImGuiCol_HeaderHovered]         = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
    style.Colors[ImGuiCol_HeaderActive]          = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    style.Colors[ImGuiCol_Separator]             = ImVec4(0.39f, 0.39f, 0.39f, 1.00f);
    style.Colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.26f, 0.59f, 0.98f, 0.78f);
    style.Colors[ImGuiCol_SeparatorActive]       = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    style.Colors[ImGuiCol_ResizeGrip]            = ImVec4(1.00f, 1.00f, 1.00f, 0.50f);
    style.Colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.26f, 0.59f, 0.98f, 0.67f);
    style.Colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.26f, 0.59f, 0.98f, 0.95f);
    style.Colors[ImGuiCol_Tab]                   = ImVec4(0.76f, 0.80f, 0.84f, 0.93f);
    style.Colors[ImGuiCol_TabHovered]            = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
    style.Colors[ImGuiCol_TabSelected]           = ImVec4(0.60f, 0.73f, 0.88f, 1.00f);
    style.Colors[ImGuiCol_TabDimmed]             = ImVec4(0.92f, 0.93f, 0.94f, 0.99f);
    style.Colors[ImGuiCol_TabDimmedSelected]     = ImVec4(0.74f, 0.82f, 0.91f, 1.00f);
    style.Colors[ImGuiCol_PlotLines]             = ImVec4(0.39f, 0.39f, 0.39f, 1.00f);
    style.Colors[ImGuiCol_PlotLinesHovered]      = ImVec4(1.00f, 0.43f, 0.35f, 1.00f);
    style.Colors[ImGuiCol_PlotHistogram]         = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
    style.Colors[ImGuiCol_PlotHistogramHovered]  = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
    style.Colors[ImGuiCol_TextSelectedBg]        = ImVec4(0.26f, 0.59f, 0.98f, 0.35f);
    style.Colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.20f, 0.20f, 0.20f, 0.35f);
    style.Colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.86f, 0.87f, 0.88f, 1.00f);
    style.Colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.57f, 0.57f, 0.64f, 1.00f);
    style.Colors[ImGuiCol_TableBorderLight]      = ImVec4(0.68f, 0.68f, 0.74f, 1.00f);
    style.Colors[ImGuiCol_TableRowBg]            = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    style.Colors[ImGuiCol_TableRowBgAlt]         = ImVec4(0.30f, 0.30f, 0.30f, 0.09f);
    style.Colors[ImGuiCol_DockingPreview]        = ImVec4(0.26f, 0.59f, 0.98f, 0.70f);
    style.Colors[ImGuiCol_DockingEmptyBg]        = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);

    // Dark mode: flip the brightness of every dull colour (greys, text, backgrounds),
    // so the palette turns dark while the blue accents keep their hue and brightness.
    for (int i = 0; i < ImGuiCol_COUNT; ++i) {
        ImVec4& c = style.Colors[i];
        float h = 0.0F;
        float sat = 0.0F;
        float v = 0.0F;
        ImGui::ColorConvertRGBtoHSV(c.x, c.y, c.z, h, sat, v);
        if (sat < 0.25F) {
            ImGui::ColorConvertHSVtoRGB(h, sat, 1.0F - v, c.x, c.y, c.z);
        }
    }
    style.Colors[ImGuiCol_Text]        = ImVec4(0.92f, 0.92f, 0.92f, 1.00f);
    // Solid backgrounds keep text readable over the 3D scene.
    style.Colors[ImGuiCol_WindowBg]    = ImVec4(0.11f, 0.11f, 0.12f, 1.00f);
    style.Colors[ImGuiCol_PopupBg]     = ImVec4(0.09f, 0.09f, 0.10f, 1.00f);
    style.Colors[ImGuiCol_FrameBg]     = ImVec4(0.18f, 0.18f, 0.20f, 1.00f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.20f, 0.22f, 0.26f, 1.00f);
    style.Colors[ImGuiCol_Tab]         = ImVec4(0.18f, 0.20f, 0.24f, 1.00f);
    style.Colors[ImGuiCol_TabSelected] = ImVec4(0.26f, 0.42f, 0.66f, 1.00f);
    style.Colors[ImGuiCol_TabDimmed]   = ImVec4(0.13f, 0.14f, 0.16f, 1.00f);
    style.Colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.22f, 0.32f, 0.48f, 1.00f);
    style.Colors[ImGuiCol_Border]      = ImVec4(1.00f, 1.00f, 1.00f, 0.18f);
}

void draw_shortcuts_table(EditorUi& ui, bool editable) {
    if (editable && ui.capturing_shortcut.has_value()) {
        ImGui::TextColored(ImVec4(0.1F, 0.4F, 0.9F, 1.0F), "Press the new key combination for \"%s\" (Esc cancels, Backspace unbinds)",
            shortcut_info(*ui.capturing_shortcut).description);
        if (IsKeyPressed(KEY_ESCAPE)) {
            ui.capturing_shortcut.reset();
        } else if (IsKeyPressed(KEY_BACKSPACE)) {
            ui.shortcuts[index_of(*ui.capturing_shortcut)] = Shortcut{};
            ui.capturing_shortcut.reset();
            save_preferences(ui);
        } else {
            for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
                if (is_modifier_key(key)) continue;
                ui.shortcuts[index_of(*ui.capturing_shortcut)] = Shortcut{
                    key,
                    ctrl_down(),
                    modifier_down(KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT),
                    modifier_down(KEY_LEFT_ALT, KEY_RIGHT_ALT),
                };
                ui.capturing_shortcut.reset();
                save_preferences(ui);
                break;
            }
        }
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_ScrollY;
    // The old Preferences alias still loads, but isn't shown in this list.
    const auto group_of = [](EditorAction action) -> int {
        switch (action) {
            case EditorAction::Save: case EditorAction::Undo: case EditorAction::Redo: return 0;
            case EditorAction::FocusSelection: case EditorAction::FrameCity: return 1;
            case EditorAction::ToolSelect: case EditorAction::ToolBuilding: case EditorAction::ToolRoad:
            case EditorAction::ToolPiece: case EditorAction::ToolErase: return 2;
            case EditorAction::RotateSelection: case EditorAction::DuplicateSelection: case EditorAction::DeleteSelection:
            case EditorAction::NudgeLeft: case EditorAction::NudgeRight: case EditorAction::NudgeForward:
            case EditorAction::NudgeBack: case EditorAction::NudgeUp: case EditorAction::NudgeDown:
            case EditorAction::NextCell: return 3;
            case EditorAction::TabCity: case EditorAction::TabBuildings: return 4;
            case EditorAction::ToggleShadows: case EditorAction::ToggleSky: return 5;
            case EditorAction::TabEditor: return -1;
            default: return 6;
        }
    };
    constexpr const char* kGroups[] = {"General", "Camera", "Tools", "Editing", "Modes", "View", "Windows"};
    if (ImGui::BeginTable("##shortcuts", editable ? 3 : 2, flags, ImVec2(0.0F, 0.0F))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        // Sized for the docked Preferences tab: the key column fits "Ctrl+Shift+H",
        // and action names wrap instead of being cut off.
        ImGui::TableSetupColumn("Action");
        ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Ctrl+Shift+PgDn").x + 12.0F);
        if (editable) ImGui::TableSetupColumn("##reset", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Reset").x + 12.0F);
        ImGui::TableHeadersRow();
        for (int group = 0; group < static_cast<int>(std::size(kGroups)); ++group) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(ImVec4(0.55F, 0.72F, 1.0F, 1.0F), "%s", kGroups[group]);
        for (std::size_t i = 0; i < kShortcutInfos.size(); ++i) {
            const auto action = static_cast<EditorAction>(i);
            if (group_of(action) != group) continue;
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", kShortcutInfos[i].description);
            ImGui::TableNextColumn();
            const auto label = action_shortcut_label(ui, action);
            if (editable) {
                const bool capturing = ui.capturing_shortcut == action;
                if (ImGui::Button(capturing ? "press keys..." : label.c_str(), ImVec2(-FLT_MIN, 0.0F))) {
                    ui.capturing_shortcut = action;
                }
                ImGui::TableNextColumn();
                if (ui.shortcuts[i] != kShortcutInfos[i].default_binding && ImGui::SmallButton("Reset")) {
                    ui.shortcuts[i] = kShortcutInfos[i].default_binding;
                    save_preferences(ui);
                }
            } else {
                ImGui::TextUnformatted(label.c_str());
            }
            ImGui::PopID();
        }
        }
        // Keys the viewport handles itself, listed so every key can be found.
        constexpr const char* kFixed[][2] = {
            {"Orbit camera", "Alt + left drag"},
            {"Pan camera", "Middle drag"},
            {"Zoom toward cursor", "Mouse wheel"},
            {"Fly camera (along view)", "Hold right mouse + WASD"},
            {"Fly up / down", "Space / Ctrl"},
            {"Fly slower / faster / slowest", "Shift / Z / Shift+Z"},
            {"Fly speed", "Mouse wheel while flying"},
            {"Road left width", "[  ]"},
            {"Road right width", ";  '"},
            {"Cancel drag / end road", "Esc"},
        };
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(ImVec4(0.55F, 0.72F, 1.0F, 1.0F), "Fixed keys");
        for (const auto& [what, keys] : kFixed) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", what);
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", keys);
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }
    // Warning for two actions on the same key.
    for (std::size_t i = 0; i < kShortcutInfos.size(); ++i) {
        for (std::size_t j = i + 1; j < kShortcutInfos.size(); ++j) {
            if (ui.shortcuts[i].key != 0 && ui.shortcuts[i] == ui.shortcuts[j]) {
                ImGui::TextColored(ImVec4(0.8F, 0.2F, 0.1F, 1.0F), "\"%s\" and \"%s\" share %s",
                    kShortcutInfos[i].description, kShortcutInfos[j].description, shortcut_label(ui.shortcuts[i]).c_str());
            }
        }
    }
}

// The world clock that places the sun (set_game_world_time), shown as hh:mm.
bool draw_world_time_slider(EditorUi& ui) {
    const auto minutes = static_cast<int>(std::lround(ui.world_time_hours * 60.0F)) % (24 * 60);
    char label[16];
    std::snprintf(label, sizeof(label), "%02d:%02d", minutes / 60, minutes % 60);
    field_label("Time of day");
    return ImGui::SliderFloat("##world_time", &ui.world_time_hours, 0.0F, 24.0F, label);
}

// The building interior setting: off, the game's distance rule, or only the
// selected buildings. True when it changed.
bool draw_interior_mode(EditorUi& ui) {
    int mode = !ui.near_lod ? 0 : ui.interiors_selected_only ? 2 : 1;
    constexpr const char* modes[] = {"Never (shell only)", "Up close (game LOD)", "Selected buildings only"};
    field_label("Interiors");
    if (!ImGui::Combo("##interior_mode", &mode, modes, 3)) {
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Up close: like the game, buildings within 192 units show their rooms through open\n"
                              "doors and see-through windows. Selected only: the selected city buildings show their\n"
                              "rooms at any distance and every other building keeps its shell.");
        }
        return false;
    }
    ui.near_lod = mode != 0;
    ui.interiors_selected_only = mode == 2;
    return true;
}

void draw_preferences_contents(EditorUi& ui) {
    bool changed = false;
    ImGui::SeparatorText("Rendering");
    changed |= draw_world_time_slider(ui);
    changed |= ImGui::Checkbox("Draw terrain", &ui.render_terrain);
    changed |= ImGui::Checkbox("Shadows", &ui.render_shadows);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Sky", &ui.render_sky);
    ImGui::BeginDisabled(!ui.render_shadows);
    field_label("Shadow range");
    changed |= ImGui::SliderFloat("##shadow_distance", &ui.shadow_distance, 64.0F, 2000.0F, "%.0f");
    ImGui::EndDisabled();
    changed |= ImGui::Checkbox("Street lights and signs", &ui.street_objects);
    changed |= ImGui::Checkbox("Live sky reflections", &ui.live_reflections);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("On: windows reflect the sky around the camera and update as you move.\n"
                          "Off: the reflection is captured once and kept (cheaper while flying).");
    }
    field_label("Filtering");
    {
        static constexpr std::array<int, 5> levels{1, 2, 4, 8, 16};
        static constexpr std::array<const char*, 5> names{"Off", "2x", "4x", "8x", "16x (game)"};
        auto current = static_cast<int>(std::ranges::find(levels, ui.anisotropy) - levels.begin());
        if (current >= static_cast<int>(levels.size())) current = 4;
        if (ImGui::Combo("##anisotropy", &current, names.data(), static_cast<int>(names.size()))) {
            ui.anisotropy = levels[static_cast<std::size_t>(current)];
            changed = true;
        }
    }

    ImGui::SeparatorText("Frame rate");
    changed |= ImGui::Checkbox("V-Sync", &ui.vsync);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Waits for the display refresh, like the game's V-Sync option.");
    field_label("Max FPS");
    {
        static constexpr std::array<int, 5> limits{30, 60, 120, 144, 0};
        static constexpr std::array<const char*, 5> names{"30", "60", "120", "144", "Unlimited"};
        auto current = static_cast<int>(std::ranges::find(limits, ui.fps_limit) - limits.begin());
        if (current >= static_cast<int>(limits.size())) current = 1;
        if (ImGui::Combo("##fps_limit", &current, names.data(), static_cast<int>(names.size()))) {
            ui.fps_limit = limits[static_cast<std::size_t>(current)];
            changed = true;
        }
    }
    changed |= ImGui::Checkbox("Save power in the background", &ui.background_throttle);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drops to 15 frames per second while another window has focus.");

    ImGui::SeparatorText("Performance");
    changed |= draw_interior_mode(ui);
    field_label("Render scale");
    changed |= ImGui::SliderFloat("##render_scale", &ui.render_scale, 0.5F, 1.0F, "%.2fx");
    field_label("Building distance");
    changed |= ImGui::SliderFloat("##building_distance", &ui.building_distance, 0.0F, 4000.0F,
        ui.building_distance <= 0.0F ? "unlimited" : "%.0f");
    changed |= ImGui::Checkbox("Half-resolution textures", &ui.half_textures);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Quarters texture memory. Takes effect the next time a map loads.");
    if (ImGui::Button("Low-end preset")) {
        ui.near_lod = false;
        ui.render_scale = 0.75F;
        ui.building_distance = 900.0F;
        ui.render_shadows = false;
        ui.half_textures = true;
        ui.live_reflections = false;
        ui.anisotropy = 4;
        ui.fps_limit = 30;
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Game preset")) {
        ui.near_lod = true;
        ui.render_scale = 1.0F;
        ui.building_distance = 0.0F;
        ui.render_shadows = true;
        ui.render_sky = true;
        ui.street_objects = true;
        ui.live_reflections = true;
        ui.anisotropy = 16;
        changed = true;
    }

    ImGui::SeparatorText("Camera");
    field_label("Fly speed");
    changed |= ImGui::SliderFloat("##fly_speed", &ui.camera_fly_speed, 2.0F, 512.0F, "%.0f");
    changed |= ImGui::Checkbox("Blender-style mouse", &ui.blender_mouse);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("On: middle drag orbits, Shift+middle drag pans, Ctrl+middle drag zooms.\n"
                          "Off: middle drag pans. Alt+left drag orbits either way.");
    }

    ImGui::SeparatorText("Interface");
    field_label("Font size");
    // ImGui 1.92 and later resize fonts on the fly (rlImGui supports dynamic font
    // textures).
    if (ImGui::SliderFloat("##font_size", &ui.ui_font_size, 10.0F, 28.0F, "%.0f px")) {
        ImGui::GetStyle().FontSizeBase = ui.ui_font_size;
        changed = true;
    }

    ImGui::SeparatorText("Keyboard shortcuts");
    if (ImGui::Button("Reset all shortcuts")) {
        reset_shortcuts(ui);
        changed = true;
    }
    if (changed) save_preferences(ui);
    draw_shortcuts_table(ui, true);
}

void draw_preferences_window(EditorUi& ui) {
    if (!ui.show_preferences_window) return;
    ImGui::SetNextWindowSize(ImVec2(520.0F, 620.0F), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(ICON_FA_GEAR " Preferences###Preferences", &ui.show_preferences_window)) {
        draw_preferences_contents(ui);
    }
    ImGui::End();
}

void draw_shortcuts_window(EditorUi& ui) {
    if (!ui.show_shortcuts_window) return;
    ImGui::SetNextWindowSize(ImVec2(460.0F, 520.0F), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(ICON_FA_KEYBOARD " Keyboard shortcuts###Shortcuts", &ui.show_shortcuts_window)) {
        ImGui::TextWrapped("Change bindings in Preferences. Camera and road-width keys are listed under Fixed keys.");
        if (ImGui::Button("Open Preferences")) ui.show_preferences_window = true;
        ImGui::Separator();
        draw_shortcuts_table(ui, false);
    }
    ImGui::End();
}

void draw_building_browser_window(EditorProject& project, EditorUi& ui) {
    if (!ui.show_building_browser) return;
    place_floating_window(ui, -1, ImVec2(760.0F, 520.0F));
    if (!ImGui::Begin(ICON_FA_BUILDING " Building browser###BuildingBrowser", &ui.show_building_browser)) {
        ImGui::End();
        return;
    }
    static std::array<char, 128> search{};
    ImGui::SetNextItemWidth(220.0F);
    ImGui::InputTextWithHint("##bb_search", "Filter by name", search.data(), search.size());
    ImGui::SameLine();
    ImGui::TextDisabled("%zu buildings", project.buildings.size());
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Click to preview, double-click to edit, drag into the city.");
    ImGui::PopStyleColor();

    const float preview_width = std::clamp(ImGui::GetContentRegionAvail().x * 0.42F, 220.0F, 420.0F);
    if (ImGui::BeginChild("##bb_grid", ImVec2(ImGui::GetContentRegionAvail().x - preview_width - 8.0F, 0.0F), true)) {
        constexpr float cell = 96.0F;
        const int per_row = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (cell + 12.0F)));
        int shown = 0;
        const auto key = lower_name(search.data());
        for (std::size_t i = 0; i < project.buildings.size(); ++i) {
            if (!key.empty() && lower_name(project.buildings[i].name).find(key) == std::string::npos) continue;
            if (shown % per_row != 0) ImGui::SameLine();
            ++shown;
            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginGroup();
            const auto* thumb = building_thumbnail(project, i);
            const bool selected = ui.browser_building == i;
            if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.26F, 0.59F, 0.98F, 0.9F));
            bool clicked = false;
            if (thumb != nullptr && thumb->texture.id != 0U) {
                clicked = ImGui::ImageButton("##thumb", ImTextureID(thumb->texture.id), ImVec2(cell, cell), ImVec2(0.0F, 1.0F), ImVec2(1.0F, 0.0F));
            } else {
                clicked = ImGui::Button("...", ImVec2(cell + 6.0F, cell + 6.0F));
            }
            if (selected) ImGui::PopStyleColor();
            if (clicked) ui.browser_building = i;
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) edit_building(project, ui, i);
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("MAP_BUILDING", &i, sizeof(i));
                ImGui::TextUnformatted(project.buildings[i].name.c_str());
                ImGui::EndDragDropSource();
            }
            // Keep the grid width fixed and show the full name on hover.
            ImGui::TextUnformatted(fit_label(project.buildings[i].name, cell).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", project.buildings[i].name.c_str());
            ImGui::EndGroup();
            ImGui::PopID();
        }
        if (shown == 0) ImGui::TextDisabled("No building matches.");
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##bb_preview", ImVec2(0.0F, 0.0F), true)) {
        if (!project.buildings.empty()) {
            ui.browser_building = std::min(ui.browser_building, project.buildings.size() - 1U);
            auto& asset = project.buildings[ui.browser_building];
            ImGui::TextUnformatted(asset.name.c_str());
            ImGui::TextDisabled("%u x %u cells, %u floors, %zu blocks", asset.data.size_x, asset.data.size_y, asset.data.size_z,
                occupied_cell_indices(asset.data).size());
            if (ImGui::Button(ICON_FA_PEN " Edit building")) edit_building(project, ui, ui.browser_building);
            ImGui::SameLine();
            if (project.city && ImGui::Button(ICON_FA_MAP_PIN " Place in city")) {
                ui.place_building_template = ui.browser_building;
                ui.city_tool = CityTool::Building;
                ui.requested_tab = EditorTab::City;
            }
            const float side = std::max(120.0F, std::min(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y - 22.0F));
            Vector3 center{};
            float radius = 1.0F;
            building_bounds(asset.data, center, radius);
            const Vector3 anchor_offset{
                -static_cast<float>(asset.data.anchor_x) * 4.0F,
                -static_cast<float>(asset.data.anchor_y) * 4.0F,
                -static_cast<float>(asset.data.anchor_z) * 4.0F,
            };
            const bool ready = ui.browser_building < project.building_gpu_models.size() &&
                !project.building_gpu_models[ui.browser_building].empty();
            draw_preview_view(ui.building_preview, "bb", ImVec2(side, side), center, radius, [&] {
                if (ready) {
                    draw_gpu_building_model_public(project.building_gpu_models[ui.browser_building], anchor_offset, 0);
                }
                DrawGrid(static_cast<int>(std::max(asset.data.size_x, asset.data.size_y)) + 2, 4.0F);
            });
            if (!ready) ImGui::TextDisabled("Mesh is still being prepared.");
        } else {
            ImGui::TextDisabled("No buildings loaded.");
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void draw_block_browser_window(EditorProject& project, EditorUi& ui) {
    if (!ui.show_block_browser) return;
    place_floating_window(ui, 0, ImVec2(720.0F, 500.0F));
    if (!ImGui::Begin(ICON_FA_CUBES " Block browser###BlockBrowser", &ui.show_block_browser)) {
        ImGui::End();
        return;
    }
    static std::array<char, 128> search{};
    ImGui::SetNextItemWidth(220.0F);
    ImGui::InputTextWithHint("##blk_search", "Filter by name", search.data(), search.size());
    ImGui::SameLine();
    ImGui::TextDisabled("%zu block meshes", project.blocks.size());
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Click to preview, double-click to use as the piece brush.");
    ImGui::PopStyleColor();

    const float preview_width = std::clamp(ImGui::GetContentRegionAvail().x * 0.42F, 220.0F, 420.0F);
    if (ImGui::BeginChild("##blk_grid", ImVec2(ImGui::GetContentRegionAvail().x - preview_width - 8.0F, 0.0F), true)) {
        constexpr float cell = 80.0F;
        const int per_row = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (cell + 12.0F)));
        int shown = 0;
        const auto key = lower_name(search.data());
        for (std::size_t i = 0; i < project.blocks.size(); ++i) {
            if (!key.empty() && lower_name(project.blocks[i].name).find(key) == std::string::npos) continue;
            if (shown % per_row != 0) ImGui::SameLine();
            ++shown;
            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginGroup();
            const auto* thumb = block_thumbnail(project, i);
            const bool selected = ui.browser_block == i;
            if (selected) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.26F, 0.59F, 0.98F, 0.9F));
            bool clicked = false;
            if (thumb != nullptr && thumb->texture.id != 0U) {
                clicked = ImGui::ImageButton("##thumb", ImTextureID(thumb->texture.id), ImVec2(cell, cell), ImVec2(0.0F, 1.0F), ImVec2(1.0F, 0.0F));
            } else {
                clicked = ImGui::Button("...", ImVec2(cell + 6.0F, cell + 6.0F));
            }
            if (selected) ImGui::PopStyleColor();
            if (clicked) {
                ui.browser_block = i;
                ui.selected_block = i;
                ui.place_piece_mesh = i;
                ui.place_piece_solid = false;
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && project.city) {
                ui.place_piece_mesh = i;
                ui.place_piece_solid = false;
                ui.city_tool = CityTool::Piece;
                ui.requested_tab = EditorTab::City;
            }
            // Keep the grid width fixed and show the full name on hover.
            ImGui::TextUnformatted(fit_label(project.blocks[i].name, cell).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", project.blocks[i].name.c_str());
            ImGui::EndGroup();
            ImGui::PopID();
        }
        if (shown == 0) ImGui::TextDisabled("No block matches.");
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##blk_preview", ImVec2(0.0F, 0.0F), true)) {
        if (!project.blocks.empty()) {
            ui.browser_block = std::min(ui.browser_block, project.blocks.size() - 1U);
            const auto& asset = project.blocks[ui.browser_block];
            ImGui::TextUnformatted(asset.name.c_str());
            ImGui::TextDisabled("%zu faces", asset.data.faces.size());
            if (project.city && ImGui::Button(ICON_FA_CUBE " Use as piece brush")) {
                ui.place_piece_mesh = ui.browser_block;
                ui.place_piece_solid = false;
                ui.city_tool = CityTool::Piece;
                ui.requested_tab = EditorTab::City;
            }
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_PEN " Edit block")) {
                ui.selected_block = ui.browser_block;
                ui.block_element = BlockElementKind::None;
                ui.requested_tab = EditorTab::Blocks;
            }
            const float side = std::max(120.0F, std::min(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y - 22.0F));
            Vector3 center{};
            float radius = 1.0F;
            block_bounds(asset.data, 4.0F, center, radius);
            draw_preview_view(ui.block_preview, "blk", ImVec2(side, side), center, radius, [&] {
                draw_mesh_faces(asset.data, Vec3{}, 4.0F, {98, 148, 204, 220});
                DrawGrid(6, 4.0F);
            });
        } else {
            ImGui::TextDisabled("No block meshes loaded.");
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void draw_buildblock_window(EditorProject& project, EditorUi& ui, std::string& status) {
    (void)status;
    ui.buildblock_window_focused = false;
    if (!ui.show_buildblock_window) return;
    place_floating_window(ui, 1, ImVec2(620.0F, 620.0F));
    // The window keeps a 1:1 shape while floating, so the face canvas stays square and
    // fills it.
    ImGui::SetNextWindowSizeConstraints(ImVec2(360.0F, 360.0F), ImVec2(FLT_MAX, FLT_MAX), [](ImGuiSizeCallbackData* data) {
        const float side = std::max(data->DesiredSize.x, data->DesiredSize.y);
        data->DesiredSize = ImVec2(side, side);
    });
    if (!ImGui::Begin(ICON_FA_TABLE_CELLS " Buildblock###Buildblock", &ui.show_buildblock_window)) {
        ImGui::End();
        return;
    }
    ui.buildblock_window_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (project.buildblocks.empty()) {
        ImGui::TextDisabled("No buildblocks loaded.");
        ImGui::End();
        return;
    }
    ui.selected_buildblock = std::min(ui.selected_buildblock, project.buildblocks.size() - 1U);

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0F);
    ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 22.0F, 0.0F), ImVec2(FLT_MAX, ImGui::GetFontSize() * 30.0F));
    if (ImGui::BeginCombo("##bbk_pick", project.buildblocks[ui.selected_buildblock].name.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (std::size_t i = 0; i < project.buildblocks.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const auto& layers = project.buildblocks[i].data.layers;
            if (ImGui::Selectable("##pick", ui.selected_buildblock == i, 0, ImVec2(0.0F, ImGui::GetFrameHeight()))) {
                ui.selected_buildblock = i;
                ui.selected_buildblock_layer = 0;
                ui.bb_selected_kind = 0;
                ui.bb_selected_index = 0;
            }
            // Each buildblock with small previews of its first faces.
            ImGui::SameLine(0.0F, 0.0F);
            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x);
            for (std::size_t l = 0; l < std::min<std::size_t>(layers.size(), 4U); ++l) {
                const auto p = ImGui::GetCursorScreenPos();
                draw_buildblock_layer(ImGui::GetWindowDrawList(), p, ImGui::GetFrameHeight(), layers[l]);
                ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
                ImGui::SameLine(0.0F, 2.0F);
            }
            ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x + 4.0F * (ImGui::GetFrameHeight() + 2.0F) + 6.0F);
            ImGui::TextUnformatted(project.buildblocks[i].name.c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    // While editing a building, jump to the buildblock under the selected cell, so a
    // face can be adjusted without leaving the mode.
    if (ui.tab == EditorTab::Buildings && !project.buildings.empty()) {
        const auto& building = project.buildings[std::min(ui.selected_building, project.buildings.size() - 1U)].data;
        const auto occupied = occupied_cell_indices(building);
        const LoadedAsset<SblTemplate>* cell_block = nullptr;
        if (!occupied.empty()) {
            const auto& cell = building.cells[occupied[std::min(ui.selected_building_cell, occupied.size() - 1U)]];
            cell_block = buildblock_for_cell(project, building, cell);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(cell_block == nullptr);
        if (ImGui::Button(ICON_FA_ARROW_POINTER " Selected block's")) {
            ui.selected_buildblock = static_cast<std::size_t>(cell_block - project.buildblocks.data());
            ui.selected_buildblock_layer = 0;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(cell_block != nullptr ? "Edit the buildblock of the selected block's room"
                                                    : "The selected block has no room with a buildblock");
        }
    }
    draw_buildblock_face_editor(project, ui);
    draw_buildblock_face_canvas(project, ui);
    ImGui::End();
}

// The in-game map screen as DrawMapMenu draws it (map_menu_texture), with a switch
// for street and avenue names, their font, and a PNG export.
void draw_map_image_window(EditorProject& project, EditorUi& ui) {
    if (!ui.show_map_image_window) return;
    place_floating_window(ui, 2, ImVec2(560.0F, 640.0F));
    if (!ImGui::Begin(ICON_FA_MAP " Map image###MapImage", &ui.show_map_image_window)) {
        ImGui::End();
        return;
    }
    const bool auto_generated = project.root.filename() == "round";
    bool changed = false;
    // The game shows no names on auto-generated maps (DrawMapMenu).
    ImGui::BeginDisabled(auto_generated);
    changed |= ImGui::Checkbox("Street names", &ui.map_menu.street_labels);
    ImGui::SameLine();
    ImGui::BeginDisabled(!ui.map_menu.street_labels);
    static std::vector<MapFontChoice> choices;
    static std::filesystem::path choices_root;
    if (choices_root != project.root) {
        choices = map_font_choices(project);
        choices_root = project.root;
    }
    const auto current = std::ranges::find(choices, ui.map_menu.font, &MapFontChoice::key);
    const std::string preview = current != choices.end() ? current->label
        : ui.map_menu.font.starts_with("game:") ? ui.map_menu.font.substr(5) + " (missing)" : "Default";
    ImGui::SetNextItemWidth(220.0F);
    if (ImGui::BeginCombo("##map_font", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        static char filter[64] = "";
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputTextWithHint("##font_filter", "Search fonts", filter, sizeof(filter));
        const auto lower = [](std::string text) {
            std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        const auto needle = lower(filter);
        for (const auto& choice : choices) {
            if (!needle.empty() && lower(choice.label).find(needle) == std::string::npos) continue;
            if (ImGui::Selectable(choice.label.c_str(), choice.key == ui.map_menu.font)) {
                ui.map_menu.font = choice.key;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    static constexpr int sizes[] = {512, 1024, 2048, 4096};
    ImGui::SetNextItemWidth(90.0F);
    if (ImGui::BeginCombo("##map_export_size", (std::to_string(ui.map_export_size) + " px").c_str())) {
        for (const auto size : sizes) {
            if (ImGui::Selectable((std::to_string(size) + " px").c_str(), size == ui.map_export_size)) {
                ui.map_export_size = size;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FILE_EXPORT " Export PNG")) ui.map_export_requested = true;
    if (changed) save_preferences(ui);

    const auto avail = ImGui::GetContentRegionAvail();
    const float side = std::max(64.0F, std::min(avail.x, avail.y));
    const auto* image = map_menu_texture(project,
        static_cast<int>(std::lround(side * ImGui::GetIO().DisplayFramebufferScale.x)), ui.map_menu);
    if (image == nullptr) {
        ImGui::TextDisabled(auto_generated ? "This map has no map.png." : "The city is still being built.");
        ImGui::End();
        return;
    }
    // Render textures are stored bottom-up.
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::Image(ImTextureID(image->id), ImVec2(side, side), ImVec2(0.0F, 1.0F), ImVec2(1.0F, 0.0F));

    // Map units to world, the way DrawMapMenu places things: map = (world - legend) *
    // scale, over a view of half-size 128 around map (120, 120). On auto-generated maps
    // it is (128, 128), with legend (960, 896) and scale 0.25.
    const float center = auto_generated ? 128.0F : 120.0F;
    const float scale = auto_generated ? 0.25F : 0.125F;
    const ImVec2 legend = auto_generated ? ImVec2(960.0F, 896.0F) : ImVec2(896.0F, 896.0F);
    const auto to_screen = [&](float world_x, float world_z) {
        return ImVec2(origin.x + ((world_x - legend.x) * scale - center + 128.0F) / 256.0F * side,
                      origin.y + ((world_z - legend.y) * scale - center + 128.0F) / 256.0F * side);
    };
    // The editor camera: a dot with a small line for its heading, like the game's
    // chevron.
    auto* draw = ImGui::GetWindowDrawList();
    const auto camera = to_screen(ui.view_camera_position.x, ui.view_camera_position.z);
    const Vector2 heading{ui.view_camera_target.x - ui.view_camera_position.x, ui.view_camera_target.z - ui.view_camera_position.z};
    const auto heading_length = std::hypot(heading.x, heading.y);
    if (heading_length > 1e-3F) {
        const ImVec2 tip{camera.x + heading.x / heading_length * 12.0F, camera.y + heading.y / heading_length * 12.0F};
        draw->AddLine(camera, tip, IM_COL32(255, 215, 90, 255), 2.5F);
    }
    draw->AddCircleFilled(camera, 5.0F, IM_COL32(255, 215, 90, 255));
    draw->AddCircle(camera, 5.0F, IM_COL32(20, 20, 20, 255), 0, 1.5F);

    if (ImGui::IsItemHovered()) {
        const auto mouse = ImGui::GetMousePos();
        const auto map_x = center - 128.0F + (mouse.x - origin.x) / side * 256.0F;
        const auto map_z = center - 128.0F + (mouse.y - origin.y) / side * 256.0F;
        const auto world_x = map_x / scale + legend.x;
        const auto world_z = map_z / scale + legend.y;
        ImGui::SetTooltip("X %.1f  Z %.1f\ncell %d, %d", world_x, world_z,
            static_cast<int>(std::floor(world_x / 4.0F)), static_cast<int>(std::floor(world_z / 4.0F)));
    }
    ImGui::End();
}

void draw_changes_window(EditorProject& project, EditorUi& ui, std::string& status) {
    if (!ui.show_changes_window) return;
    ImGui::SetNextWindowSize(ImVec2(460.0F, 400.0F), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FA_CLOCK_ROTATE_LEFT " Changes since last save###Changes", &ui.show_changes_window)) {
        ImGui::End();
        return;
    }
    if (!project.city) {
        ImGui::TextDisabled("No city loaded.");
        ImGui::End();
        return;
    }
    if (project.saved_undo_depth && *project.saved_undo_depth > project.undo_stack.size()) {
        ImGui::TextWrapped("The map is %zu step(s) behind the last save. Redo to return to it.",
            *project.saved_undo_depth - project.undo_stack.size());
        ImGui::End();
        return;
    }
    // With no saved point in the history, every recorded step is unsaved.
    const auto saved = project.saved_undo_depth.value_or(0U);
    const auto pending = project.undo_stack.size() - saved;
    if (!project.saved_undo_depth) {
        ImGui::TextWrapped("The saved version is older than the undo history; only the last %zu change(s) are listed.", pending);
    }
    if (pending == 0 && project.saved_undo_depth) {
        ImGui::TextDisabled("No map changes since the last save.");
        if (ui.building_dirty || ui.buildblock_dirty || ui.block_dirty) {
            ImGui::TextWrapped("Asset edits (buildings, buildblocks, block meshes) are unsaved; undo them from their own editor.");
        }
        ImGui::End();
        return;
    }
    ImGui::Text("%zu change(s). Double-click to jump to it. " ICON_FA_ROTATE_LEFT " reverts back to before it.", pending);
    if (project.saved_undo_depth && ImGui::Button(ICON_FA_TRASH " Discard all changes since save##changes_discard")) {
        while (project.undo_stack.size() > saved) undo_city(project, ui, status);
    }
    ImGui::Separator();
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##changes_table", 3, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 36.0F);
        ImGui::TableSetupColumn("Action");
        ImGui::TableSetupColumn("##revert", ImGuiTableColumnFlags_WidthFixed, 34.0F);
        ImGui::TableHeadersRow();
        for (std::size_t n = project.undo_stack.size(); n > saved; --n) {
            const auto i = n - 1;
            const auto& after = i + 1 < project.undo_stack.size() ? project.undo_stack[i + 1].second : project.city->data;
            const auto target = diff_maps(project.undo_stack[i].second, after);
            const ImVec4 color = target.change == MapChange::Added ? ImVec4(0.45F, 0.9F, 0.45F, 1.0F)
                : target.change == MapChange::Removed ? ImVec4(1.0F, 0.45F, 0.4F, 1.0F)
                : ImVec4(1.0F, 0.85F, 0.35F, 1.0F);
            const char* icon = target.change == MapChange::Added ? ICON_FA_PLUS
                : target.change == MapChange::Removed ? ICON_FA_MINUS : ICON_FA_PEN;
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%zu", i + 1 - saved);
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            const std::string label = std::string{icon} + " " + project.undo_stack[i].first;
            ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                focus_diff_target(project, ui, target);
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(ICON_FA_ROTATE_LEFT "##revert_row")) {
                while (project.undo_stack.size() > i) undo_city(project, ui, status);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Undo this and everything after it");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void unload_preview_views(EditorUi& ui) {
    for (auto* view : {&ui.building_preview, &ui.block_preview, &ui.buildblock_preview}) {
        if (view->target.id != 0U) {
            UnloadRenderTexture(view->target);
            view->target = {};
        }
    }
}

} // namespace pafta::app_internal
