// Game map limits (from subrosa.exe array capacities) and the 2D buildblock
// face editor with draggable vertices.
#include "app_internal.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>

namespace map_editor::app_internal {
namespace {

// Capacities recovered from subrosa.exe (see docs/RE_render_shader_spec.md and
// the CitySbc_Load @0xd5a50 / Sbb_LoadFromFile @0x26830 / _createarea @0x4db20
// notes). "Sectors" are the 8x8x8-cell blocks the world allocates on demand.
constexpr int kMaxIntersections = 1024;   // file array 0x2e48687c..0x2e48987c, 12 B each
constexpr int kMaxStreetsSafe = 512;      // no runtime check; file array slack ends at 669
constexpr int kMaxStreetsHard = 669;
constexpr int kMaxBuildings = 8192;       // g_cityBuildingRecords, 0x60 B each up to g_cityPathCount
constexpr int kMaxCityBlocks = 4096;      // g_cityBuildingDataName, 0x4248 B each up to g_cityDistrictCount
constexpr int kMaxPortals = 16384;        // g_cityDistrictId, 0xe04 B each up to the header records
constexpr int kMaxHeaderRecords = 1088;   // 0x35eea090..0x35efb090, 64 B each
constexpr int kWorldCellsXZ = 4096;       // _createarea(64 x 8 x 64 chunks of 64 cells)
constexpr int kWorldCellsY = 512;
constexpr int kMaxChunks = 512;           // World_AllocateAreaChunkIndex slot table
constexpr int kMaxSectors = 4096;         // sector tables loop to 0x1000
constexpr unsigned kMaxBuildingSide = 63; // SBB cell grid is 64 x 64 (loops run 0..size inclusive)
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

struct CellKey {
    std::int64_t v;
    bool operator==(const CellKey& o) const { return v == o.v; }
};
struct CellKeyHash {
    std::size_t operator()(const CellKey& k) const { return std::hash<std::int64_t>{}(k.v); }
};
CellKey key3(std::int32_t x, std::int32_t y, std::int32_t z) {
    return {(static_cast<std::int64_t>(x) << 40) ^ (static_cast<std::int64_t>(y) << 20) ^ static_cast<std::int64_t>(z)};
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

    std::unordered_set<CellKey, CellKeyHash> sectors;
    std::unordered_set<CellKey, CellKeyHash> chunks;
    int out_of_world = 0;
    const auto touch = [&](std::int32_t cx, std::int32_t cy, std::int32_t cz) {
        if (cx < 0 || cz < 0 || cy < 0 || cx >= kWorldCellsXZ || cz >= kWorldCellsXZ || cy >= kWorldCellsY) {
            ++out_of_world;
            return;
        }
        sectors.insert(key3(cx >> 3, cy >> 3, cz >> 3));
        chunks.insert(key3(cx >> 6, cy >> 6, cz >> 6));
    };
    const auto placement_buildings = resolve_placement_buildings(project);
    for (std::size_t i = 0; i < map.buildings.size(); ++i) {
        const auto* b = placement_buildings[i];
        if (b == nullptr) continue;
        const auto& pl = map.buildings[i];
        for (const auto& cell : b->cells) {
            if (!cell.occupied) continue;
            const Vec3 local{
                (static_cast<float>(cell.x) - static_cast<float>(b->anchor_x) + 0.5F) * 4.0F,
                (static_cast<float>(cell.y) - static_cast<float>(b->anchor_y) + 0.5F) * 4.0F,
                (static_cast<float>(cell.z) - static_cast<float>(b->anchor_z) + 0.5F) * 4.0F,
            };
            const auto w = native_city_building_transform_point(pl, local, 4.0F);
            touch(static_cast<std::int32_t>(std::floor(w.x / 4.0F)),
                  static_cast<std::int32_t>(std::floor(w.y / 4.0F)),
                  static_cast<std::int32_t>(std::floor(w.z / 4.0F)));
        }
    }
    for (const auto& portal : map.portals) {
        // A portal is one aligned 8x8x8 block of cells.
        touch(portal.fields[1] * 8, portal.fields[2] * 8, portal.fields[3] * 8);
    }
    for (const auto& tile : project.city_scene_cache.city_tiles) {
        touch(tile.x, tile.y, tile.z);
    }
    report.sectors = static_cast<int>(sectors.size());
    report.chunks = static_cast<int>(chunks.size());
    if (out_of_world > 0) {
        report.lines.emplace_back(true, std::to_string(out_of_world) + " cell(s) outside the world (x,z 0..4095, y 0..511)");
    }
    count_line("World sectors (8x8x8 cell blocks)", sectors.size(), kMaxSectors * 9 / 10, kMaxSectors);
    count_line("World chunks (64x64x64 cell blocks)", chunks.size(), kMaxChunks * 9 / 10, kMaxChunks);

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

// ---------------------------------------------------------------------------
// Buildblock face editor helpers

constexpr ImU32 kColBounds = IM_COL32(97, 183, 239, 255);
constexpr ImU32 kColWindow = IM_COL32(240, 164, 84, 255);
constexpr ImU32 kColBeam = IM_COL32(132, 216, 147, 255);
constexpr ImU32 kColDoor = IM_COL32(190, 120, 235, 255);
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

// Uniform rectangle view over the face bounds (x,w,y,h array) and portal rects.
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
        ImGui::TextDisabled(ICON_FA_CIRCLE_CHECK " Within game limits (%d sectors, %d chunks)", report.sectors, report.chunks);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Game limits: 1024 junctions, ~669 roads, 8192 buildings, 4096 city blocks,\n"
                "16384 piece portals, world 4096x512x4096 cells in at most 512 chunks / 4096 sectors,\n"
                "building templates 63x63 cells, 192 floors, 320 materials.");
        }
        return 0;
    }
    for (const auto& [error, text] : report.lines) {
        ImGui::TextColored(error ? ImVec4(1.0F, 0.35F, 0.3F, 1.0F) : ImVec4(1.0F, 0.8F, 0.2F, 1.0F),
            "%s %s", error ? ICON_FA_TRIANGLE_EXCLAMATION : ICON_FA_CIRCLE_EXCLAMATION, text.c_str());
    }
    return static_cast<int>(report.lines.size());
}

void draw_buildblock_face_editor(EditorProject& project, EditorUi& ui) {
    if (project.buildblocks.empty()) return;
    ui.selected_buildblock = std::min(ui.selected_buildblock, project.buildblocks.size() - 1U);
    auto& asset = project.buildblocks[ui.selected_buildblock];
    auto& model = asset.data;
    bool changed = false;

    if (model.layers.empty()) {
        ImGui::TextDisabled("This buildblock has no faces.");
        if (ImGui::Button(ICON_FA_PLUS " Add face")) {
            SblLayer face{};
            face.bounds = {0, 1, 0, 1};
            model.layers.push_back(face);
            mark_buildblock_changed(project, ui);
        }
        return;
    }
    ui.selected_buildblock_layer = std::min(ui.selected_buildblock_layer, model.layers.size() - 1U);
    ImGui::SetNextItemWidth(-90.0F);
    const auto face_name = "Face " + std::to_string(ui.selected_buildblock_layer + 1);
    if (ImGui::BeginCombo("Face##bbk_face", face_name.c_str())) {
        for (std::size_t i = 0; i < model.layers.size(); ++i) {
            if (ImGui::Selectable(("Face " + std::to_string(i + 1)).c_str(), i == ui.selected_buildblock_layer)) {
                ui.selected_buildblock_layer = i;
                ui.bb_selected_kind = 0;
                ui.bb_selected_index = 0;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::BeginDisabled(model.layers.size() >= 16);
    if (ImGui::Button(ICON_FA_PLUS " Add face##bbk_add_face")) {
        SblLayer face{};
        face.index = model.layers.size();
        face.bounds = {0, 1, 0, 1};
        model.layers.push_back(face);
        ui.selected_buildblock_layer = model.layers.size() - 1;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ERASER " Clear face##bbk_clear_face")) {
        // Face numbers are referenced by buildings: clear in place, never shift.
        model.layers[ui.selected_buildblock_layer] = SblLayer{};
        model.layers[ui.selected_buildblock_layer].index = ui.selected_buildblock_layer;
        ui.bb_selected_kind = 0;
        ui.bb_selected_index = 0;
        changed = true;
    }
    auto& layer = model.layers[ui.selected_buildblock_layer];
    if (layer.portal_type <= 1) {
        bool mirrored = layer.portal_type == 1;
        if (ImGui::Checkbox("Mirror on opposite sides##bbk_mirror", &mirrored)) {
            layer.portal_type = mirrored ? 1 : 0;
            changed = true;
        }
    }

    ImGui::SeparatorText("Add");
    ImGui::BeginDisabled(layer.doors.size() >= 2);
    if (ImGui::Button(ICON_FA_DOOR_OPEN " Add door##bbk_add_door")) {
        layer.doors.push_back({0, 0.5F, 0.0F});
        ui.bb_selected_kind = 3;
        ui.bb_selected_index = static_cast<int>(layer.doors.size()) - 1;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(layer.windows.size() >= 16);
    if (ImGui::Button(ICON_FA_WINDOW_MAXIMIZE " Add window##bbk_add_window")) {
        layer.windows.push_back({0, 0.375F, 0.25F, 0.375F, 0.25F});
        ui.bb_selected_kind = 1;
        ui.bb_selected_index = static_cast<int>(layer.windows.size()) - 1;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(layer.beams.size() >= 16);
    if (ImGui::Button(ICON_FA_GRIP_LINES " Add beam##bbk_add_beam")) {
        layer.beams.push_back({0, 0.0F, 1.0F, 0.9F, 0.1F});
        ui.bb_selected_kind = 2;
        ui.bb_selected_index = static_cast<int>(layer.beams.size()) - 1;
        changed = true;
    }
    ImGui::EndDisabled();

    ImGui::SeparatorText("Snap");
    const char* snap_items[] = {"Free", "1/8 cell", "1/16 cell", "1/32 cell"};
    int snap_index = ui.bb_snap_steps == 0 ? 0 : ui.bb_snap_steps == 8 ? 1 : ui.bb_snap_steps == 16 ? 2 : 3;
    ImGui::SetNextItemWidth(-90.0F);
    if (ImGui::Combo("Grid##bbk_snap", &snap_index, snap_items, 4)) {
        ui.bb_snap_steps = snap_index == 0 ? 0 : snap_index == 1 ? 8 : snap_index == 2 ? 16 : 32;
    }

    ImGui::SeparatorText("Legend");
    const auto legend = [](ImU32 color, const char* text) {
        const auto p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 12.0F, p.y + 12.0F), color, 2.0F);
        ImGui::Dummy(ImVec2(16.0F, 12.0F));
        ImGui::SameLine();
        ImGui::TextWrapped("%s", text);
    };
    legend(kColBounds, "Face bounds: the wall area this face covers (blue outline)");
    legend(kColWindow, "Window: glass opening (orange)");
    legend(kColBeam, "Beam: solid strip across the face (green)");
    legend(kColDoor, "Door: entrance point (purple dot)");
    legend(kColSelected, "Selected item and its drag handles (yellow)");

    ImGui::SeparatorText("Items");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Click an item here or in the canvas. Drag corners to resize, the inside to move.");
    ImGui::PopStyleColor();
    if (ImGui::BeginTable("##bbk_items", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, ImVec2(0.0F, 0.0F))) {
        ImGui::TableSetupColumn("Item");
        // Wide enough for four "0.000" values; 150 px clipped the last one.
        ImGui::TableSetupColumn("x, y, w, h (cells)", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("0.000, 0.000, 0.000, 0.000").x + 8.0F);
        ImGui::TableSetupColumn("##rm", ImGuiTableColumnFlags_WidthFixed, 30.0F);
        ImGui::TableHeadersRow();
        const auto row = [&](int kind, int index, float x, float y, float w, float h, bool removable, auto&& remove) {
            ImGui::PushID(kind * 1000 + index);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const bool selected = ui.bb_selected_kind == kind && ui.bb_selected_index == index;
            const std::string label = kind == 0 ? std::string{kind_name(kind)} : std::string{kind_name(kind)} + " " + std::to_string(index + 1);
            if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                ui.bb_selected_kind = kind;
                ui.bb_selected_index = index;
            }
            ImGui::TableNextColumn();
            if (kind == 3) {
                ImGui::Text("%.3f, %.3f", x, y);
            } else {
                ImGui::Text("%.3f, %.3f, %.3f, %.3f", x, y, w, h);
            }
            ImGui::TableNextColumn();
            if (removable && ImGui::SmallButton(ICON_FA_XMARK)) {
                remove();
                changed = true;
            }
            ImGui::PopID();
        };
        row(0, 0, layer.bounds[0], layer.bounds[2], layer.bounds[1], layer.bounds[3], false, [] {});
        for (std::size_t i = 0; i < layer.windows.size(); ++i) {
            const auto& r = layer.windows[i];
            row(1, static_cast<int>(i), r.x, r.y, r.width, r.height, true, [&] { layer.windows.erase(layer.windows.begin() + static_cast<std::ptrdiff_t>(i)); });
            if (changed) break;
        }
        if (!changed) for (std::size_t i = 0; i < layer.beams.size(); ++i) {
            const auto& r = layer.beams[i];
            row(2, static_cast<int>(i), r.x, r.y, r.width, r.height, true, [&] { layer.beams.erase(layer.beams.begin() + static_cast<std::ptrdiff_t>(i)); });
            if (changed) break;
        }
        if (!changed) for (std::size_t i = 0; i < layer.doors.size(); ++i) {
            const auto& d = layer.doors[i];
            row(3, static_cast<int>(i), d.x, d.y, 0.0F, 0.0F, true, [&] { layer.doors.erase(layer.doors.begin() + static_cast<std::ptrdiff_t>(i)); });
            if (changed) break;
        }
        ImGui::EndTable();
    }
    if (changed) {
        ui.bb_selected_index = 0;
        if (ui.bb_selected_kind == 1 && layer.windows.empty()) ui.bb_selected_kind = 0;
        if (ui.bb_selected_kind == 2 && layer.beams.empty()) ui.bb_selected_kind = 0;
        if (ui.bb_selected_kind == 3 && layer.doors.empty()) ui.bb_selected_kind = 0;
        mark_buildblock_changed(project, ui);
    }
}

void draw_buildblock_face_canvas(EditorProject& project, EditorUi& ui) {
    if (project.buildblocks.empty()) return;
    ui.selected_buildblock = std::min(ui.selected_buildblock, project.buildblocks.size() - 1U);
    auto& model = project.buildblocks[ui.selected_buildblock].data;
    if (model.layers.empty()) return;
    ui.selected_buildblock_layer = std::min(ui.selected_buildblock_layer, model.layers.size() - 1U);
    auto& layer = model.layers[ui.selected_buildblock_layer];

    ImGui::Text("%s  face %zu", project.buildblocks[ui.selected_buildblock].name.c_str(), ui.selected_buildblock_layer + 1);
    ImGui::SameLine();
    ImGui::TextDisabled("(1 cell = the whole square)");
    const auto avail = ImGui::GetContentRegionAvail();
    const float side = std::max(120.0F, std::min(avail.x, avail.y));
    const auto origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##bbk_canvas", ImVec2(side, side), ImGuiButtonFlags_MouseButtonLeft);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    auto* dl = ImGui::GetWindowDrawList();
    const float pad = 10.0F;
    const float cell = side - pad * 2.0F;
    const ImVec2 c0{origin.x + pad, origin.y + pad};
    const auto to_screen = [&](float x, float y) { return ImVec2{c0.x + x * cell, c0.y + (1.0F - y) * cell}; };
    const auto to_cell = [&](ImVec2 p) { return std::pair{(p.x - c0.x) / cell, 1.0F - (p.y - c0.y) / cell}; };

    dl->AddRectFilled(origin, ImVec2(origin.x + side, origin.y + side), IM_COL32(18, 20, 24, 255), 4.0F);
    // Cell border and snap grid.
    if (ui.bb_snap_steps > 0) {
        for (int i = 1; i < ui.bb_snap_steps; ++i) {
            const float t = static_cast<float>(i) / ui.bb_snap_steps;
            const ImU32 col = (ui.bb_snap_steps % 4 == 0 && i % (ui.bb_snap_steps / 4) == 0) ? IM_COL32(70, 76, 84, 255) : IM_COL32(40, 44, 50, 255);
            dl->AddLine(to_screen(t, 0.0F), to_screen(t, 1.0F), col);
            dl->AddLine(to_screen(0.0F, t), to_screen(1.0F, t), col);
        }
    }
    dl->AddRect(to_screen(0.0F, 1.0F), to_screen(1.0F, 0.0F), IM_COL32(200, 205, 210, 255), 0.0F, 0, 2.0F);

    // Hit testing on press.
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
        if (mouse.x >= a.x && mouse.x <= b.x && mouse.y >= a.y && mouse.y <= b.y) return Hit{kind, index, -1};
        return std::nullopt;
    };
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        std::optional<Hit> hit;
        // Doors first (small), then the selected item, then windows/beams, then bounds.
        for (std::size_t i = 0; i < layer.doors.size() && !hit; ++i) {
            const auto p = to_screen(layer.doors[i].x, layer.doors[i].y);
            if (std::abs(mouse.x - p.x) <= handle + 3.0F && std::abs(mouse.y - p.y) <= handle + 3.0F) hit = Hit{3, static_cast<int>(i), -1};
        }
        if (!hit && ui.bb_selected_kind != 3) {
            if (auto r = rect_ref(layer, ui.bb_selected_kind, ui.bb_selected_index)) hit = rect_hit(ui.bb_selected_kind, ui.bb_selected_index, *r);
        }
        for (int kind = 1; kind <= 2 && !hit; ++kind) {
            auto& rects = kind == 1 ? layer.windows : layer.beams;
            for (std::size_t i = 0; i < rects.size() && !hit; ++i) {
                if (auto r = rect_ref(layer, kind, static_cast<int>(i))) hit = rect_hit(kind, static_cast<int>(i), *r);
            }
        }
        if (!hit) {
            if (auto r = rect_ref(layer, 0, 0)) hit = rect_hit(0, 0, *r);
        }
        if (hit) {
            ui.bb_selected_kind = hit->kind;
            ui.bb_selected_index = hit->index;
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
    }
    // Drag.
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
                // Corner drag: the opposite corner stays fixed.
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

    // Draw items.
    const auto draw_rect = [&](int kind, int index, const RectRef& r, ImU32 color, bool fill) {
        const bool selected = ui.bb_selected_kind == kind && ui.bb_selected_index == index;
        const auto a = to_screen(*r.x, *r.y + *r.h);
        const auto b = to_screen(*r.x + *r.w, *r.y);
        if (fill) dl->AddRectFilled(a, b, (color & 0x00FFFFFF) | 0x50000000);
        dl->AddRect(a, b, selected ? kColSelected : color, 0.0F, 0, selected ? 2.5F : 1.5F);
        if (selected) {
            const ImVec2 corners[4] = {to_screen(*r.x, *r.y), to_screen(*r.x + *r.w, *r.y), to_screen(*r.x + *r.w, *r.y + *r.h), to_screen(*r.x, *r.y + *r.h)};
            for (const auto& c : corners) {
                dl->AddRectFilled(ImVec2(c.x - handle, c.y - handle), ImVec2(c.x + handle, c.y + handle), kColSelected);
                dl->AddRect(ImVec2(c.x - handle, c.y - handle), ImVec2(c.x + handle, c.y + handle), IM_COL32(0, 0, 0, 255));
            }
        }
    };
    if (auto r = rect_ref(layer, 0, 0)) draw_rect(0, 0, *r, kColBounds, false);
    for (std::size_t i = 0; i < layer.beams.size(); ++i) {
        if (auto r = rect_ref(layer, 2, static_cast<int>(i))) draw_rect(2, static_cast<int>(i), *r, kColBeam, true);
    }
    for (std::size_t i = 0; i < layer.windows.size(); ++i) {
        if (auto r = rect_ref(layer, 1, static_cast<int>(i))) draw_rect(1, static_cast<int>(i), *r, kColWindow, true);
    }
    for (std::size_t i = 0; i < layer.doors.size(); ++i) {
        const auto p = to_screen(layer.doors[i].x, layer.doors[i].y);
        const bool selected = ui.bb_selected_kind == 3 && ui.bb_selected_index == static_cast<int>(i);
        dl->AddCircleFilled(p, handle + 1.0F, selected ? kColSelected : kColDoor);
        dl->AddCircle(p, handle + 1.0F, IM_COL32(0, 0, 0, 255));
    }
    // Cursor readout.
    if (hovered) {
        const auto [mx, my] = to_cell(mouse);
        dl->AddText(ImVec2(origin.x + 4.0F, origin.y + side - 18.0F), IM_COL32(200, 205, 210, 255),
            (std::to_string(std::round(mx * 1000.0F) / 1000.0F).substr(0, 5) + ", " + std::to_string(std::round(my * 1000.0F) / 1000.0F).substr(0, 5)).c_str());
    }
}

} // namespace map_editor::app_internal
