// SPDX-License-Identifier: GPL-2.0-only
// Game map limits (from the sizes of subrosa.exe's arrays) and the 2D buildblock
// face editor, whose corners you can drag.
#include "app_internal.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace pafta::app_internal {
namespace {

// Limits found in subrosa.exe (see docs/RE_render_shader_spec.md and the notes on
// CitySbc_Load @0xd5a50 / Sbb_LoadFromFile @0x26830 / _createarea @0x4db20).
// "Sectors" are the 8x8x8-cell blocks the world sets up when needed.
constexpr int kMaxIntersections = 1024;   // file array 0x2e48687c..0x2e48987c, 12 B each
constexpr int kMaxStreetsSafe = 512;      // no runtime check; file array slack ends at 669
constexpr int kMaxStreetsHard = 669;
constexpr int kMaxBuildings = 8192;       // g_cityBuildingRecords, 0x60 B each up to g_cityPathCount
constexpr int kMaxCityBlocks = 4096;      // g_cityBuildingDataName, 0x4248 B each up to g_cityDistrictCount
constexpr int kMaxPortals = 16384;        // g_cityDistrictId, 0xe04 B each up to the header records
constexpr int kMaxHeaderRecords = 1088;   // 0x35eea090..0x35efb090, 64 B each
constexpr int kMaxChunks = 512;           // World_AllocateAreaChunkIndex slot table
constexpr int kMaxSectors = 4096;         // sector tables loop to 0x1000
constexpr unsigned kMaxBuildingSide = 63; // the SBB cell grid is 64 x 64 (loops run from 0 to size, both included)
constexpr unsigned kMaxBuildingFloors = 192;
constexpr unsigned kMaxBuildingMaterials = 320;
constexpr unsigned kMaxBuildingBlockNames = 1088;

struct MapLimitReport {
    std::uint64_t fingerprint{};
    std::size_t building_count{};
    std::vector<std::pair<bool, std::string>> lines; // true = error (over), false = warning (near)
    int chunks{};
    int sectors{};
};

MapLimitReport& cached_report() {
    static MapLimitReport report;
    return report;
}

void rebuild_report(EditorProject& project, MapLimitReport& report) {
    report.lines.clear();
    report.chunks = 0;
    report.sectors = 0;
    if (!project.city) return;
    const auto& map = project.city->data;
    const auto count_line = [&](const char* what, std::size_t n, int soft, int hard) {
        if (static_cast<int>(n) > hard) {
            report.lines.emplace_back(true, std::string{what} + ": " + std::to_string(n) + " exceeds the game's " + std::to_string(hard));
        } else if (static_cast<int>(n) >= soft) {
            report.lines.emplace_back(false, std::string{what} + ": " + std::to_string(n) + " of " + std::to_string(hard));
        }
    };
    count_line("Junctions", map.intersections.size(), kMaxIntersections * 9 / 10, kMaxIntersections);
    count_line("Roads", map.streets.size(), kMaxStreetsSafe, kMaxStreetsHard);
    count_line("Buildings", map.buildings.size(), kMaxBuildings * 9 / 10, kMaxBuildings);
    count_line("City blocks", map.city_blocks.size(), kMaxCityBlocks * 9 / 10, kMaxCityBlocks);
    count_line("Piece portals", map.portals.size(), kMaxPortals * 9 / 10, kMaxPortals);
    count_line("Header records", map.header_records.size(), kMaxHeaderRecords * 9 / 10, kMaxHeaderRecords);

    // World bounds and space: the scene's replay of how the game loads (GameWorldFit).
    for (auto& problem : game_world_fit_problems(project)) report.lines.emplace_back(true, std::move(problem));
    const auto& fit = project.city_scene_cache.world_fit;
    report.sectors = static_cast<int>(fit.sectors_used);
    report.chunks = static_cast<int>(fit.chunks_used);
    if (fit.road_tiles_no_room + fit.building_cells_no_room == 0) {
        count_line("Game space used", fit.sectors_used, kMaxSectors * 9 / 10, kMaxSectors);
        count_line("Game space used (large areas)", fit.chunks_used, kMaxChunks * 9 / 10, kMaxChunks);
    }

    for (const auto& asset : project.buildings) {
        const auto& b = asset.data;
        if (b.size_x > kMaxBuildingSide || b.size_y > kMaxBuildingSide) {
            report.lines.emplace_back(true, asset.name + ": footprint " + std::to_string(b.size_x) + "x" + std::to_string(b.size_y) + " exceeds 63x63 cells");
        }
        if (b.size_z > kMaxBuildingFloors) {
            report.lines.emplace_back(true, asset.name + ": " + std::to_string(b.size_z) + " floors exceeds 192");
        }
        if (b.materials.size() > kMaxBuildingMaterials) {
            report.lines.emplace_back(true, asset.name + ": " + std::to_string(b.materials.size()) + " materials exceeds 320");
        }
        if (b.block_names.size() > kMaxBuildingBlockNames) {
            report.lines.emplace_back(true, asset.name + ": " + std::to_string(b.block_names.size()) + " buildblock names exceeds 1088");
        }
    }
}

constexpr ImU32 kColBounds = IM_COL32(97, 183, 239, 255);
constexpr ImU32 kColSelected = IM_COL32(255, 230, 90, 255);

const char* kind_name(int kind) {
    switch (kind) {
        case 0: return "Face bounds";
        case 1: return "Window";
        case 2: return "Beam";
        default: return "Door";
    }
}

float snap_value(float v, int steps) {
    if (steps <= 0) return std::clamp(v, 0.0F, 1.0F);
    return std::clamp(std::round(v * static_cast<float>(steps)) / static_cast<float>(steps), 0.0F, 1.0F);
}

// A rectangle view, scaled the same in both directions, over the face bounds
// (x,w,y,h array) and the portal rectangles.
struct RectRef {
    float* x;
    float* w;
    float* y;
    float* h;
};

std::optional<RectRef> rect_ref(SblLayer& layer, int kind, int index) {
    if (kind == 0) return RectRef{&layer.bounds[0], &layer.bounds[1], &layer.bounds[2], &layer.bounds[3]};
    auto& rects = kind == 1 ? layer.windows : layer.beams;
    if (index < 0 || static_cast<std::size_t>(index) >= rects.size()) return std::nullopt;
    auto& r = rects[static_cast<std::size_t>(index)];
    return RectRef{&r.x, &r.width, &r.y, &r.height};
}

} // namespace

int draw_map_limit_warnings(EditorProject& project) {
    auto& report = cached_report();
    const auto fp = project.city_scene_cache_fingerprint ^ (static_cast<std::uint64_t>(project.buildings.size()) << 48) ^
        (project.city ? static_cast<std::uint64_t>(project.city->data.buildings.size()) : 0ULL);
    if (report.fingerprint != fp || report.building_count != project.buildings.size()) {
        report.fingerprint = fp;
        report.building_count = project.buildings.size();
        rebuild_report(project, report);
    }
    if (report.lines.empty()) {
        ImGui::TextDisabled(ICON_FA_CIRCLE_CHECK " Everything fits in the game. Space used: %d of %d", report.sectors, kMaxSectors);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Game limits: 1024 junctions, about 669 roads, 8192 buildings, 4096 city blocks,\n"
                "16384 piece portals, a map area 16384 wide, 16384 long and 2048 high with 4096 units of space,\n"
                "buildings up to 63x63 cells, 192 floors and 320 materials.");
        }
        return 0;
    }
    ImGui::PushTextWrapPos(0.0F);
    for (const auto& [error, text] : report.lines) {
        ImGui::TextColored(error ? ImVec4(1.0F, 0.35F, 0.3F, 1.0F) : ImVec4(1.0F, 0.8F, 0.2F, 1.0F),
            "%s %s", error ? ICON_FA_TRIANGLE_EXCLAMATION : ICON_FA_CIRCLE_EXCLAMATION, text.c_str());
    }
    ImGui::PopTextWrapPos();
    return static_cast<int>(report.lines.size());
}

// Draws one buildblock layer in a square the way the game builds the room wall
// (BuildBlock_AddBeams @1a1c0): wall frames around the bounds opening, beam boxes,
// glass for each window record, and a marker for each door record. Cell x runs
// left to right and y bottom to top.
void draw_buildblock_layer(ImDrawList* dl, ImVec2 min, float side, const SblLayer& layer) {
    const auto at = [&](float x, float y) { return ImVec2{min.x + x * side, min.y + (1.0F - y) * side}; };
    const auto fill = [&](float x, float y, float w, float h, ImU32 color) {
        if (w <= 0.0F || h <= 0.0F) return;
        dl->AddRectFilled(at(x, y + h), at(x + w, y), color);
    };
    // The room behind the opening.
    dl->AddRectFilled(min, ImVec2(min.x + side, min.y + side), IM_COL32(28, 30, 36, 255));
    const float x = std::clamp(layer.bounds[0], 0.0F, 1.0F), w = std::clamp(layer.bounds[1], 0.0F, 1.0F - x);
    const float y = std::clamp(layer.bounds[2], 0.0F, 1.0F), h = std::clamp(layer.bounds[3], 0.0F, 1.0F - y);
    // The four frames of FUN_140051c60 / BuildBlock_AddBeams.
    constexpr ImU32 wall = IM_COL32(168, 160, 148, 255);
    fill(0.0F, 0.0F, x, 1.0F, wall);
    fill(x + w, 0.0F, 1.0F - (x + w), 1.0F, wall);
    fill(0.0F, 0.0F, 1.0F, y, wall);
    fill(0.0F, y + h, 1.0F, 1.0F - (y + h), wall);
    if (layer.portal_type == 1U) {
        // An interior layer is an open doorway: show the floor line through it.
        dl->AddLine(at(x, y), at(x + w, y), IM_COL32(110, 104, 96, 255), std::max(1.0F, side * 0.012F));
    }
    for (const auto& beam : layer.beams) {
        // Wall, overhang, and second overhang beams are boxes; any other type is a thin
        // mullion (the editor's Wall/Overhang/Overhang2/Mullion Beam buttons).
        const ImU32 color = beam.type == 0U ? IM_COL32(150, 142, 130, 255) : beam.type == 1U ? IM_COL32(120, 112, 100, 255)
            : beam.type == 2U ? IM_COL32(96, 88, 78, 255) : IM_COL32(70, 70, 74, 255);
        fill(beam.x, beam.y, beam.width, beam.height, color);
        dl->AddRect(at(beam.x, beam.y + beam.height), at(beam.x + beam.width, beam.y), IM_COL32(60, 56, 50, 200));
    }
    for (const auto& window : layer.windows) {
        const auto a = at(window.x, window.y + window.height), b = at(window.x + window.width, window.y);
        dl->AddRectFilled(a, b, IM_COL32(120, 175, 225, 150));
        // A highlight across the glass so windows read as glass at a glance.
        dl->AddLine(ImVec2(a.x + (b.x - a.x) * 0.2F, b.y), ImVec2(a.x + (b.x - a.x) * 0.6F, a.y), IM_COL32(230, 245, 255, 120), 1.5F);
        dl->AddRect(a, b, IM_COL32(50, 70, 90, 255));
    }
    for (const auto& door : layer.doors) {
        // A door record is a point (where the doorway is), drawn as a small door.
        const auto p = at(door.x, door.y);
        const float dw = std::max(4.0F, side * 0.07F), dh = std::max(7.0F, side * 0.13F);
        dl->AddRectFilled(ImVec2(p.x - dw * 0.5F, p.y - dh), ImVec2(p.x + dw * 0.5F, p.y), IM_COL32(150, 95, 200, 230), 1.5F);
        dl->AddCircleFilled(ImVec2(p.x + dw * 0.25F, p.y - dh * 0.5F), std::max(1.0F, dw * 0.12F), IM_COL32(240, 220, 120, 255));
    }
    dl->AddRect(min, ImVec2(min.x + side, min.y + side), IM_COL32(200, 205, 210, 255));
}

bool buildblock_layer_button(const char* id, const SblLayer& layer, float size, bool selected) {
    const auto min = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    auto* dl = ImGui::GetWindowDrawList();
    draw_buildblock_layer(dl, min, size, layer);
    if (selected) dl->AddRect(min, ImVec2(min.x + size, min.y + size), kColSelected, 0.0F, 0, 3.0F);
    else if (ImGui::IsItemHovered()) dl->AddRect(min, ImVec2(min.x + size, min.y + size), IM_COL32(120, 200, 255, 255), 0.0F, 0, 2.0F);
    return pressed;
}

namespace {

void select_item(EditorUi& ui, int kind, int index) {
    ui.bb_selected_kind = kind;
    ui.bb_selected_index = index;
}

// Copies the selected window, beam, or door a little up and right; false when the
// layer is full.
bool duplicate_item(SblLayer& layer, EditorUi& ui) {
    const float step = 1.0F / 16.0F;
    if (ui.bb_selected_kind == 1 && static_cast<std::size_t>(ui.bb_selected_index) < layer.windows.size() && layer.windows.size() < 16) {
        auto copy = layer.windows[static_cast<std::size_t>(ui.bb_selected_index)];
        copy.x = std::min(copy.x + step, 1.0F - copy.width);
        layer.windows.push_back(copy);
        select_item(ui, 1, static_cast<int>(layer.windows.size()) - 1);
    } else if (ui.bb_selected_kind == 2 && static_cast<std::size_t>(ui.bb_selected_index) < layer.beams.size() && layer.beams.size() < 16) {
        auto copy = layer.beams[static_cast<std::size_t>(ui.bb_selected_index)];
        copy.y = std::min(copy.y + step, 1.0F - copy.height);
        layer.beams.push_back(copy);
        select_item(ui, 2, static_cast<int>(layer.beams.size()) - 1);
    } else if (ui.bb_selected_kind == 3 && static_cast<std::size_t>(ui.bb_selected_index) < layer.doors.size() && layer.doors.size() < 2) {
        auto copy = layer.doors[static_cast<std::size_t>(ui.bb_selected_index)];
        copy.x = std::min(copy.x + step * 4.0F, 1.0F);
        layer.doors.push_back(copy);
        select_item(ui, 3, static_cast<int>(layer.doors.size()) - 1);
    } else {
        return false;
    }
    return true;
}

bool delete_item(SblLayer& layer, EditorUi& ui) {
    const auto index = static_cast<std::size_t>(std::max(0, ui.bb_selected_index));
    if (ui.bb_selected_kind == 1 && index < layer.windows.size()) layer.windows.erase(layer.windows.begin() + static_cast<std::ptrdiff_t>(index));
    else if (ui.bb_selected_kind == 2 && index < layer.beams.size()) layer.beams.erase(layer.beams.begin() + static_cast<std::ptrdiff_t>(index));
    else if (ui.bb_selected_kind == 3 && index < layer.doors.size()) layer.doors.erase(layer.doors.begin() + static_cast<std::ptrdiff_t>(index));
    else return false;
    select_item(ui, 0, 0);
    return true;
}

// Adds a window, beam, or door centred on (cx, cy), or in the middle of the face.
bool add_item(SblLayer& layer, EditorUi& ui, int kind, float cx = 0.5F, float cy = 0.5F) {
    const auto clamp01 = [](float v, float size) { return std::clamp(v - size * 0.5F, 0.0F, 1.0F - size); };
    if (kind == 1 && layer.windows.size() < 16) {
        layer.windows.push_back({0, clamp01(cx, 0.25F), 0.25F, clamp01(cy, 0.25F), 0.25F});
        select_item(ui, 1, static_cast<int>(layer.windows.size()) - 1);
    } else if (kind == 2 && layer.beams.size() < 16) {
        layer.beams.push_back({0, 0.0F, 1.0F, clamp01(cy, 0.0625F), 0.0625F});
        select_item(ui, 2, static_cast<int>(layer.beams.size()) - 1);
    } else if (kind == 3 && layer.doors.size() < 2) {
        layer.doors.push_back({0, std::clamp(cx, 0.0F, 1.0F), std::clamp(cy, 0.0F, 1.0F)});
        select_item(ui, 3, static_cast<int>(layer.doors.size()) - 1);
    } else {
        return false;
    }
    return true;
}

constexpr const char* kBeamTypes[] = {"Wall beam", "Overhang beam", "Overhang 2 beam", "Mullion"};

} // namespace

// The rows above the canvas: the face picker (previews of each face), the face
// settings, the add buttons, and the selected item's numbers.
void draw_buildblock_face_editor(EditorProject& project, EditorUi& ui) {
    if (project.buildblocks.empty()) return;
    ui.selected_buildblock = std::min(ui.selected_buildblock, project.buildblocks.size() - 1U);
    auto& model = project.buildblocks[ui.selected_buildblock].data;
    bool changed = false;

    // Faces as previews; click one to edit it.
    const float thumb = ImGui::GetFrameHeight() * 1.6F;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const int per_row = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (thumb + spacing)));
    for (std::size_t i = 0; i < model.layers.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (i != 0U && i % static_cast<std::size_t>(per_row) == 0U) ImGui::NewLine();
        if (buildblock_layer_button("##face", model.layers[i], thumb, i == ui.selected_buildblock_layer)) {
            ui.selected_buildblock_layer = i;
            select_item(ui, 0, 0);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Face %zu (wall type %zu)\n%zu windows, %zu beams, %zu doors", i + 1, i + 3,
                model.layers[i].windows.size(), model.layers[i].beams.size(), model.layers[i].doors.size());
        }
        ImGui::PopID();
        ImGui::SameLine();
    }
    if (!model.layers.empty() && model.layers.size() % static_cast<std::size_t>(per_row) == 0U) ImGui::NewLine();
    ImGui::BeginDisabled(model.layers.size() >= 16);
    if (ImGui::Button(ICON_FA_PLUS "##bbk_add_face", ImVec2(thumb, thumb))) {
        SblLayer face{};
        face.index = model.layers.size();
        face.bounds = {0.25F, 0.5F, 0.25F, 0.5F};
        model.layers.push_back(face);
        ui.selected_buildblock_layer = model.layers.size() - 1;
        select_item(ui, 0, 0);
        changed = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Add a face (at most 16)");
    if (model.layers.empty()) {
        if (changed) mark_buildblock_changed(project, ui);
        return;
    }
    ui.selected_buildblock_layer = std::min(ui.selected_buildblock_layer, model.layers.size() - 1U);
    auto& layer = model.layers[ui.selected_buildblock_layer];

    // editor.exe's portal editor has Exterior and Interior buttons for the portal type:
    // exterior (0) gets an outside face and its windows on every side; interior (1)
    // is an open doorway with no outside face, mirrored on the south and west sides.
    if (layer.portal_type <= 1U) {
        int type = static_cast<int>(layer.portal_type);
        changed |= ImGui::RadioButton("Exterior", &type, 0);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("A wall with an outside face; windows show on every side.");
        ImGui::SameLine();
        changed |= ImGui::RadioButton("Interior", &type, 1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("An inside opening such as a doorway: no outside face, mirrored on the south and west sides.");
        layer.portal_type = static_cast<std::uint32_t>(type);
    } else {
        ImGui::TextDisabled("Portal type %u", layer.portal_type);
    }
    ImGui::SameLine(0.0F, 16.0F);
    ImGui::BeginDisabled(layer.windows.size() >= 16);
    if (ImGui::Button(ICON_FA_WINDOW_MAXIMIZE " Window")) changed |= add_item(layer, ui, 1);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(layer.beams.size() >= 16);
    if (ImGui::Button(ICON_FA_GRIP_LINES " Beam")) changed |= add_item(layer, ui, 2);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(layer.doors.size() >= 2);
    if (ImGui::Button(ICON_FA_DOOR_OPEN " Door")) changed |= add_item(layer, ui, 3, 0.5F, layer.bounds[2]);
    ImGui::EndDisabled();
    ImGui::SameLine();
    const char* snap_items[] = {"Free", "1/8", "1/16", "1/32"};
    int snap_index = ui.bb_snap_steps == 0 ? 0 : ui.bb_snap_steps == 8 ? 1 : ui.bb_snap_steps == 16 ? 2 : 3;
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("1/16").x + ImGui::GetFrameHeight() * 1.8F);
    if (ImGui::Combo("##bbk_snap", &snap_index, snap_items, 4)) {
        ui.bb_snap_steps = snap_index == 0 ? 0 : snap_index == 1 ? 8 : snap_index == 2 ? 16 : 32;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Snap grid. Hold Shift while dragging to move freely.");
    ImGui::SameLine();
    ImGui::TextDisabled(ICON_FA_CIRCLE_QUESTION);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Click an item to select it; drag it to move, drag a corner to resize.\n"
                          "Drag the opening's edge to change the wall's opening.\n"
                          "Right-click for more. Delete removes, Ctrl+D copies, arrows nudge.\n"
                          "Grey: wall. Blue glass: window. Darker strips: beams. Purple: door.");
    }

    // The selected item's numbers, all in cell fractions.
    if (auto r = rect_ref(layer, ui.bb_selected_kind, ui.bb_selected_index); r && ui.bb_selected_kind != 3) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(ui.bb_selected_kind == 0 ? "Opening" : ui.bb_selected_kind == 1 ? "Window" : "Beam");
        ImGui::SameLine();
        std::array<float, 4> v{*r->x, *r->y, *r->w, *r->h};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0F);
        if (ImGui::DragFloat4("##bbk_rect", v.data(), 0.005F, 0.0F, 1.0F, "%.3f")) {
            *r->x = std::clamp(v[0], 0.0F, 1.0F);
            *r->y = std::clamp(v[1], 0.0F, 1.0F);
            *r->w = std::clamp(v[2], 0.0F, 1.0F - *r->x);
            *r->h = std::clamp(v[3], 0.0F, 1.0F - *r->y);
            changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("x, y, width, height (1 = the whole cell)");
        if (ui.bb_selected_kind == 2) {
            auto& beam = layer.beams[static_cast<std::size_t>(ui.bb_selected_index)];
            int type = static_cast<int>(std::min<std::uint32_t>(beam.type, 3U));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0F);
            if (ImGui::Combo("##beam_type", &type, kBeamTypes, 4)) {
                beam.type = static_cast<std::uint32_t>(type);
                changed = true;
            }
        }
    } else if (ui.bb_selected_kind == 3 && static_cast<std::size_t>(ui.bb_selected_index) < layer.doors.size()) {
        auto& door = layer.doors[static_cast<std::size_t>(ui.bb_selected_index)];
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Door");
        ImGui::SameLine();
        std::array<float, 2> v{door.x, door.y};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0F);
        if (ImGui::DragFloat2("##bbk_door", v.data(), 0.005F, 0.0F, 1.0F, "%.3f")) {
            door.x = std::clamp(v[0], 0.0F, 1.0F);
            door.y = std::clamp(v[1], 0.0F, 1.0F);
            changed = true;
        }
    }
    if (ui.bb_selected_kind != 0) {
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_TRASH "##bbk_delete")) changed |= delete_item(layer, ui);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete (Delete key)");
    }
    if (changed) mark_buildblock_changed(project, ui);
}

void draw_buildblock_face_canvas(EditorProject& project, EditorUi& ui) {
    if (project.buildblocks.empty()) return;
    ui.selected_buildblock = std::min(ui.selected_buildblock, project.buildblocks.size() - 1U);
    auto& model = project.buildblocks[ui.selected_buildblock].data;
    if (model.layers.empty()) {
        ImGui::TextDisabled("This buildblock has no faces. Add one with +.");
        return;
    }
    ui.selected_buildblock_layer = std::min(ui.selected_buildblock_layer, model.layers.size() - 1U);
    auto& layer = model.layers[ui.selected_buildblock_layer];

    // The canvas is always square and centred in the space left.
    const auto avail = ImGui::GetContentRegionAvail();
    const float side = std::max(120.0F, std::min(avail.x, avail.y));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0F, (avail.x - side) * 0.5F));
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##bbk_canvas", ImVec2(side, side), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    auto* dl = ImGui::GetWindowDrawList();
    const float pad = 12.0F;
    const float cell = side - pad * 2.0F;
    const ImVec2 c0{origin.x + pad, origin.y + pad};
    const auto to_screen = [&](float x, float y) { return ImVec2{c0.x + x * cell, c0.y + (1.0F - y) * cell}; };
    const auto to_cell = [&](ImVec2 p) { return std::pair{(p.x - c0.x) / cell, 1.0F - (p.y - c0.y) / cell}; };

    dl->AddRectFilled(origin, ImVec2(origin.x + side, origin.y + side), IM_COL32(18, 20, 24, 255), 4.0F);
    draw_buildblock_layer(dl, c0, cell, layer);
    // Snap grid over the face.
    if (ui.bb_snap_steps > 0) {
        for (int i = 1; i < ui.bb_snap_steps; ++i) {
            const float t = static_cast<float>(i) / ui.bb_snap_steps;
            const ImU32 col = (ui.bb_snap_steps % 4 == 0 && i % (ui.bb_snap_steps / 4) == 0) ? IM_COL32(0, 0, 0, 70) : IM_COL32(0, 0, 0, 28);
            dl->AddLine(to_screen(t, 0.0F), to_screen(t, 1.0F), col);
            dl->AddLine(to_screen(0.0F, t), to_screen(1.0F, t), col);
        }
    }

    const auto mouse = ImGui::GetIO().MousePos;
    const float handle = 6.0F;
    struct Hit { int kind; int index; int corner; };
    const auto rect_hit = [&](int kind, int index, const RectRef& r) -> std::optional<Hit> {
        const ImVec2 corners[4] = {to_screen(*r.x, *r.y), to_screen(*r.x + *r.w, *r.y), to_screen(*r.x + *r.w, *r.y + *r.h), to_screen(*r.x, *r.y + *r.h)};
        for (int c = 0; c < 4; ++c) {
            if (std::abs(mouse.x - corners[c].x) <= handle + 2.0F && std::abs(mouse.y - corners[c].y) <= handle + 2.0F) return Hit{kind, index, c};
        }
        const auto a = to_screen(*r.x, *r.y + *r.h);
        const auto b = to_screen(*r.x + *r.w, *r.y);
        if (kind == 0) {
            // The opening is picked on its edge, so clicks inside it reach the items.
            const bool inside = mouse.x >= a.x - 4.0F && mouse.x <= b.x + 4.0F && mouse.y >= a.y - 4.0F && mouse.y <= b.y + 4.0F;
            const bool deep = mouse.x >= a.x + 4.0F && mouse.x <= b.x - 4.0F && mouse.y >= a.y + 4.0F && mouse.y <= b.y - 4.0F;
            if (inside && !deep) return Hit{kind, index, -1};
            return std::nullopt;
        }
        if (mouse.x >= a.x && mouse.x <= b.x && mouse.y >= a.y && mouse.y <= b.y) return Hit{kind, index, -1};
        return std::nullopt;
    };
    const auto hit_test = [&]() -> std::optional<Hit> {
        std::optional<Hit> hit;
        // Doors first (they are small), then the selected item, then windows and beams,
        // then the opening's edge.
        for (std::size_t i = 0; i < layer.doors.size() && !hit; ++i) {
            const auto p = to_screen(layer.doors[i].x, layer.doors[i].y);
            const float dh = std::max(7.0F, cell * 0.13F);
            if (std::abs(mouse.x - p.x) <= handle + 3.0F && mouse.y <= p.y + 3.0F && mouse.y >= p.y - dh - 3.0F) hit = Hit{3, static_cast<int>(i), -1};
        }
        if (!hit && ui.bb_selected_kind != 3) {
            if (auto r = rect_ref(layer, ui.bb_selected_kind, ui.bb_selected_index)) hit = rect_hit(ui.bb_selected_kind, ui.bb_selected_index, *r);
        }
        for (int kind = 1; kind <= 2 && !hit; ++kind) {
            auto& rects = kind == 1 ? layer.windows : layer.beams;
            for (std::size_t i = rects.size(); i-- > 0 && !hit;) {
                if (auto r = rect_ref(layer, kind, static_cast<int>(i))) hit = rect_hit(kind, static_cast<int>(i), *r);
            }
        }
        if (!hit) {
            if (auto r = rect_ref(layer, 0, 0)) hit = rect_hit(0, 0, *r);
        }
        return hit;
    };
    const auto hover = hovered ? hit_test() : std::nullopt;
    if (hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        if (const auto hit = hover) {
            select_item(ui, hit->kind, hit->index);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                ui.bb_drag_kind = hit->kind;
                ui.bb_drag_index = hit->index;
                ui.bb_drag_corner = hit->corner;
                const auto [mx, my] = to_cell(mouse);
                if (hit->kind == 3) {
                    ui.bb_drag_off_x = layer.doors[static_cast<std::size_t>(hit->index)].x - mx;
                    ui.bb_drag_off_y = layer.doors[static_cast<std::size_t>(hit->index)].y - my;
                } else if (auto r = rect_ref(layer, hit->kind, hit->index)) {
                    ui.bb_drag_off_x = *r->x - mx;
                    ui.bb_drag_off_y = *r->y - my;
                }
                ui.bb_drag_changed = false;
            }
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            select_item(ui, 0, 0);
        }
    }
    // Dragging.
    if (active && ui.bb_drag_kind >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0F)) {
        const auto [mx, my] = to_cell(mouse);
        const bool free = ImGui::GetIO().KeyShift;
        const int steps = free ? 0 : ui.bb_snap_steps;
        if (ui.bb_drag_kind == 3) {
            if (static_cast<std::size_t>(ui.bb_drag_index) < layer.doors.size()) {
                auto& d = layer.doors[static_cast<std::size_t>(ui.bb_drag_index)];
                d.x = snap_value(mx + ui.bb_drag_off_x, steps);
                d.y = snap_value(my + ui.bb_drag_off_y, steps);
                ui.bb_drag_changed = true;
            }
        } else if (auto r = rect_ref(layer, ui.bb_drag_kind, ui.bb_drag_index)) {
            if (ui.bb_drag_corner < 0) {
                const float w = *r->w;
                const float h = *r->h;
                *r->x = std::clamp(snap_value(mx + ui.bb_drag_off_x, steps), 0.0F, 1.0F - w);
                *r->y = std::clamp(snap_value(my + ui.bb_drag_off_y, steps), 0.0F, 1.0F - h);
            } else {
                // Dragging a corner: the opposite corner stays where it is.
                float x0 = *r->x, y0 = *r->y, x1 = *r->x + *r->w, y1 = *r->y + *r->h;
                const float sx = snap_value(mx, steps);
                const float sy = snap_value(my, steps);
                const float min_size = steps > 0 ? 1.0F / steps : 0.01F;
                switch (ui.bb_drag_corner) {
                    case 0: x0 = std::min(sx, x1 - min_size); y0 = std::min(sy, y1 - min_size); break;
                    case 1: x1 = std::max(sx, x0 + min_size); y0 = std::min(sy, y1 - min_size); break;
                    case 2: x1 = std::max(sx, x0 + min_size); y1 = std::max(sy, y0 + min_size); break;
                    default: x0 = std::min(sx, x1 - min_size); y1 = std::max(sy, y0 + min_size); break;
                }
                *r->x = x0; *r->y = y0; *r->w = x1 - x0; *r->h = y1 - y0;
            }
            ui.bb_drag_changed = true;
        }
    }
    if (ui.bb_drag_kind >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (ui.bb_drag_changed) mark_buildblock_changed(project, ui);
        ui.bb_drag_kind = -1;
        ui.bb_drag_index = -1;
        ui.bb_drag_corner = -1;
        ui.bb_drag_changed = false;
    }

    // Keys while the mouse is over the canvas.
    bool changed = false;
    if (hovered) {
        const auto& io = ImGui::GetIO();
        const bool command = io.KeyCtrl || io.KeySuper;
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) changed |= delete_item(layer, ui);
        if (command && ImGui::IsKeyPressed(ImGuiKey_D)) changed |= duplicate_item(layer, ui);
        const float step = io.KeyShift ? 1.0F / 64.0F : 1.0F / static_cast<float>(ui.bb_snap_steps > 0 ? ui.bb_snap_steps : 64);
        const float dx = ImGui::IsKeyPressed(ImGuiKey_RightArrow) ? step : ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ? -step : 0.0F;
        const float dy = ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? step : ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? -step : 0.0F;
        if (dx != 0.0F || dy != 0.0F) {
            if (ui.bb_selected_kind == 3 && static_cast<std::size_t>(ui.bb_selected_index) < layer.doors.size()) {
                auto& d = layer.doors[static_cast<std::size_t>(ui.bb_selected_index)];
                d.x = std::clamp(d.x + dx, 0.0F, 1.0F);
                d.y = std::clamp(d.y + dy, 0.0F, 1.0F);
                changed = true;
            } else if (auto r = rect_ref(layer, ui.bb_selected_kind, ui.bb_selected_index)) {
                *r->x = std::clamp(*r->x + dx, 0.0F, 1.0F - *r->w);
                *r->y = std::clamp(*r->y + dy, 0.0F, 1.0F - *r->h);
                changed = true;
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
            // Tab steps through the opening, the windows, the beams, and the doors.
            std::vector<std::pair<int, int>> order{{0, 0}};
            for (int i = 0; i < static_cast<int>(layer.windows.size()); ++i) order.emplace_back(1, i);
            for (int i = 0; i < static_cast<int>(layer.beams.size()); ++i) order.emplace_back(2, i);
            for (int i = 0; i < static_cast<int>(layer.doors.size()); ++i) order.emplace_back(3, i);
            const auto it = std::ranges::find(order, std::pair{ui.bb_selected_kind, ui.bb_selected_index});
            const auto next = it == order.end() ? 0U : static_cast<std::size_t>(it - order.begin() + 1) % order.size();
            select_item(ui, order[next].first, order[next].second);
        }
    }
    // Right-click menu: add at the cursor, or act on the item under it.
    static std::pair<float, float> menu_at{0.5F, 0.5F};
    if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        menu_at = to_cell(mouse);
        ImGui::OpenPopup("##bbk_canvas_menu");
    }
    if (ImGui::BeginPopup("##bbk_canvas_menu")) {
        const auto [mx, my] = menu_at;
        if (ui.bb_selected_kind != 0) {
            ImGui::TextDisabled("%s %d", kind_name(ui.bb_selected_kind), ui.bb_selected_index + 1);
            if (ImGui::MenuItem(ICON_FA_COPY " Duplicate", "Ctrl+D")) changed |= duplicate_item(layer, ui);
            if (ui.bb_selected_kind == 2 && static_cast<std::size_t>(ui.bb_selected_index) < layer.beams.size() && ImGui::BeginMenu("Beam type")) {
                auto& beam = layer.beams[static_cast<std::size_t>(ui.bb_selected_index)];
                for (std::uint32_t t = 0; t < 4; ++t) {
                    if (ImGui::MenuItem(kBeamTypes[t], nullptr, std::min<std::uint32_t>(beam.type, 3U) == t)) {
                        beam.type = t;
                        changed = true;
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(ICON_FA_TRASH " Delete", "Delete")) changed |= delete_item(layer, ui);
            ImGui::Separator();
        }
        if (ImGui::MenuItem(ICON_FA_WINDOW_MAXIMIZE " Add window here", nullptr, false, layer.windows.size() < 16)) changed |= add_item(layer, ui, 1, mx, my);
        if (ImGui::MenuItem(ICON_FA_GRIP_LINES " Add beam here", nullptr, false, layer.beams.size() < 16)) changed |= add_item(layer, ui, 2, mx, my);
        if (ImGui::MenuItem(ICON_FA_DOOR_OPEN " Add door here", nullptr, false, layer.doors.size() < 2)) changed |= add_item(layer, ui, 3, mx, my);
        ImGui::Separator();
        if (ImGui::MenuItem("Opening fills the face")) {
            layer.bounds = {0.0F, 1.0F, 0.0F, 1.0F};
            changed = true;
        }
        if (ImGui::MenuItem("Clear this face")) {
            // Buildings refer to faces by number: clear in place, never move the others down.
            layer = SblLayer{};
            layer.index = ui.selected_buildblock_layer;
            select_item(ui, 0, 0);
            changed = true;
        }
        ImGui::EndPopup();
    }
    if (changed) mark_buildblock_changed(project, ui);

    // Selection and hover outlines with drag handles.
    const auto outline = [&](int kind, int index, ImU32 color, bool handles) {
        if (kind == 3) {
            if (static_cast<std::size_t>(index) >= layer.doors.size()) return;
            const auto p = to_screen(layer.doors[static_cast<std::size_t>(index)].x, layer.doors[static_cast<std::size_t>(index)].y);
            const float dw = std::max(4.0F, cell * 0.07F), dh = std::max(7.0F, cell * 0.13F);
            dl->AddRect(ImVec2(p.x - dw * 0.5F - 2.0F, p.y - dh - 2.0F), ImVec2(p.x + dw * 0.5F + 2.0F, p.y + 2.0F), color, 2.0F, 0, 2.5F);
            return;
        }
        const auto r = rect_ref(layer, kind, index);
        if (!r) return;
        const auto a = to_screen(*r->x, *r->y + *r->h);
        const auto b = to_screen(*r->x + *r->w, *r->y);
        dl->AddRect(a, b, color, 0.0F, 0, 2.5F);
        if (!handles) return;
        const ImVec2 corners[4] = {to_screen(*r->x, *r->y), to_screen(*r->x + *r->w, *r->y), to_screen(*r->x + *r->w, *r->y + *r->h), to_screen(*r->x, *r->y + *r->h)};
        for (const auto& c : corners) {
            dl->AddRectFilled(ImVec2(c.x - handle, c.y - handle), ImVec2(c.x + handle, c.y + handle), color);
            dl->AddRect(ImVec2(c.x - handle, c.y - handle), ImVec2(c.x + handle, c.y + handle), IM_COL32(0, 0, 0, 255));
        }
    };
    // The opening's edge is always shown, so it can be found and dragged.
    if (auto r = rect_ref(layer, 0, 0)) {
        dl->AddRect(to_screen(*r->x, *r->y + *r->h), to_screen(*r->x + *r->w, *r->y), kColBounds, 0.0F, 0, 1.5F);
    }
    if (hover && !(hover->kind == ui.bb_selected_kind && hover->index == ui.bb_selected_index)) {
        outline(hover->kind, hover->index, IM_COL32(120, 200, 255, 255), false);
    }
    outline(ui.bb_selected_kind, ui.bb_selected_index, kColSelected, true);
    if (hovered) {
        const auto [mx, my] = to_cell(mouse);
        char text[48];
        std::snprintf(text, sizeof(text), "%.3f, %.3f", std::clamp(mx, 0.0F, 1.0F), std::clamp(my, 0.0F, 1.0F));
        dl->AddText(ImVec2(origin.x + 4.0F, origin.y + side - ImGui::GetFontSize() - 2.0F), IM_COL32(200, 205, 210, 255), text);
        if (hover) ImGui::SetMouseCursor(hover->corner >= 0 ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_Hand);
    }
}

} // namespace pafta::app_internal
