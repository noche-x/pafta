// SPDX-License-Identifier: GPL-2.0-only
#include "app_internal.hpp"

#include <imgui.h>
#include <rlImGui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace pafta::app_internal {

// A label on the left, in a shared column, for the next widget, so fields line up
// down a panel. The widget takes the rest of the row (give it a "##" id).
void field_label(const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 7.5F);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

void draw_struct_inspector_header(const char* file_type, const char* struct_name, const std::string& record_name) {
    (void)file_type;
    if (ImGui::CollapsingHeader(struct_name, ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextUnformatted(record_name.c_str());
    }
}

bool draw_struct_section(const char* name) {
    return ImGui::CollapsingHeader(name, ImGuiTreeNodeFlags_DefaultOpen);
}

void invalidate_buildblock_preview_scene(EditorProject& project) {
    project.cached_buildblock_preview_index.reset();
}

const BuildblockPreviewScene& buildblock_preview_scene(EditorProject& project, std::size_t index) {
    // Changing the selection rebuilds here; edits go through mark_buildblock_changed.
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

// The building's buildblocks as a grid; each tile shows its first four faces.
// Changes `selected` (an index into building.block_names); true when it changed.
bool buildblock_picker(const EditorProject& project, const SbbBuilding& building, std::size_t& selected) {
    if (building.block_names.empty()) {
        ImGui::TextDisabled("(no buildblocks; add one under Buildblocks)");
        return false;
    }
    selected = std::min(selected, building.block_names.size() - 1U);
    GridOptions options{.size = 52.0F, .height = 128.0F};
    options.paint = [&](std::size_t i, ImDrawList* draw_list, ImVec2 min, ImVec2 max) {
        const auto* asset = find_buildblock_asset(project, building.block_names[i]);
        if (asset == nullptr) return;
        const float half = (max.x - min.x - 8.0F) * 0.5F;
        for (std::size_t l = 0; l < std::min<std::size_t>(asset->data.layers.size(), 4U); ++l) {
            draw_buildblock_layer(draw_list, ImVec2(min.x + 4.0F + static_cast<float>(l % 2U) * half, min.y + 4.0F + static_cast<float>(l / 2U) * half),
                half - 1.0F, asset->data.layers[l]);
        }
    };
    const auto picked = asset_grid("buildblock_picker", building.block_names.size(), selected, options,
        [&](std::size_t i) {
            return find_buildblock_asset(project, building.block_names[i]) == nullptr
                ? building.block_names[i] + " (not loaded)" : building.block_names[i];
        },
        [](std::size_t) { return Thumb{}; });
    if (picked) selected = *picked;
    return picked.has_value();
}

// A building's materials as a grid (a material with no loaded texture shows its name).
std::optional<std::size_t> material_grid(const EditorProject& project, const char* id, const std::vector<std::string>& materials,
    std::size_t selected, float height) {
    return asset_grid(id, materials.size(), selected, {.size = 40.0F, .height = height},
        [&](std::size_t i) { return materials[i].empty() ? "material " + std::to_string(i) : materials[i]; },
        [&](std::size_t i) {
            const auto* texture = texture_for_material(project, materials[i]);
            return Thumb{texture != nullptr ? texture->id : 0U, false};
        });
}

// Each layer that is turned on replaces its own block word and textures. A layer
// that is off leaves the cell's data alone (BuildingTool_WriteCellFromCurrentSelection).
void draw_block_tool_panel(EditorProject& project, EditorUi& ui) {
    ui.selected_building = std::min(ui.selected_building, project.buildings.size() - 1U);
    auto& building = project.buildings[ui.selected_building].data;
    auto& cfg = ui.block_config;

    // What each texture slot paints. The shell's sides use slots 1..4 as S/W/N/E. The
    // room's walls use 1..4 as N/E/S/W, and slot 6 for the frames around openings
    // (add_native_interior_room). Pieces use whatever slots their faces name.
    static constexpr std::array<const char*, 8> exterior_slots{"Bottom", "South", "West", "North", "East", "Top", "Slot 6", "Slot 7"};
    static constexpr std::array<const char*, 8> interior_slots{"Floor", "North wall", "East wall", "South wall", "West wall", "Ceiling", "Opening frame", "Slot 7"};

    const auto piece_picker = [&](const char* id, std::size_t& block) {
        if (project.blocks.empty()) {
            ImGui::TextDisabled("No blocks loaded.");
            return;
        }
        block = std::min(block, project.blocks.size() - 1U);
        ImGui::Text("Piece: %s", project.blocks[block].name.c_str());
        const auto picked = asset_grid(id, project.blocks.size(), block, {.size = 48.0F, .height = 132.0F},
            [&](std::size_t i) { return project.blocks[i].name; },
            [&](std::size_t i) {
                const auto* t = block_thumbnail(project, i);
                return Thumb{t != nullptr ? t->texture.id : 0U, true};
            });
        if (picked) block = *picked;
    };
    // Each slot shows its material as a thumbnail; clicking opens a grid of swatches.
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
            grid_button("##slot", 30.0F, building.materials.size(), materials[slot],
                [&](std::size_t i) { return building.materials[i].empty() ? "material " + std::to_string(i) : building.materials[i]; },
                [&](std::size_t i) {
                    const auto* texture = texture_for_material(project, building.materials[i]);
                    return Thumb{texture != nullptr ? texture->id : 0U, false};
                }, names[slot], {.size = 40.0F});
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(names[slot]);
            if (slot % 2U == 0U) ImGui::SameLine(170.0F);
            ImGui::PopID();
        }
        if (ImGui::SmallButton("Use the first slot for all")) materials.fill(materials[0]);
        ImGui::TreePop();
    };

    if (ImGui::CollapsingHeader("Block to place", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Exterior", &cfg.exterior);
        ImGui::SameLine(110.0F);
        ImGui::BeginDisabled(!cfg.exterior);
        ImGui::Checkbox("Piece##exterior", &cfg.exterior_piece);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Off: a plain solid block. On: a piece mesh (.sbl).");
        if (cfg.exterior) {
            if (cfg.exterior_piece) piece_picker("exterior_piece", cfg.exterior_piece_block);
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
                piece_picker("interior_piece", cfg.interior_piece_block);
            } else if (!building.block_names.empty()) {
                ImGui::TextUnformatted("Room walls from buildblock");
                buildblock_picker(project, building, cfg.buildblock_index);
            }
            if (!cfg.interior_piece) {
                ImGui::Checkbox("Floor", &cfg.room_floor);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Off: the room has no floor and the room below has no ceiling,\n"
                                      "so stacked rooms join into one tall room.");
                }
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
    // Placements and archive entries use the asset name. The name in the SBB header is
    // stored separately, and changing it doesn't rename those references.
    std::array<char, 52> asset_name{};
    std::copy_n(asset.name.data(), std::min(asset.name.size(), asset_name.size() - 1), asset_name.data());
    field_label("Name");
    if (ImGui::InputText("##asset_name", asset_name.data(), asset_name.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
        rename_building(project, ui, asset_name.data(), status);
    }
    if (ImGui::IsItemActive()) ImGui::TextDisabled("Press Enter to rename. Every placement of it is renamed too.");
    std::array<char, 64> name{};
    std::copy_n(building.name.data(), std::min(building.name.size(), name.size() - 1), name.data());
    field_label("Name in file");
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
        // Add: click a texture, or type a name that isn't loaded and press Enter.
        const auto add_material = [&](const std::string& material) {
            if (material.empty() || building.materials.size() >= 1024) return;
            const auto key = lower_name(material);
            if (std::ranges::none_of(building.materials, [&](const std::string& m) { return lower_name(m) == key; })) {
                building.materials.push_back(material);
                mark_building_changed(project, ui);
            }
        };
        if (ImGui::CollapsingHeader("Add material", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##add_material_search", ICON_FA_MAGNIFYING_GLASS " Search or type a name", ui.new_material_name.data(),
                ui.new_material_name.size());
            const auto picked = asset_grid("add_material", project.textures.size(), project.textures.size(),
                {.size = 36.0F, .height = 120.0F, .search = &ui.new_material_name},
                [&](std::size_t i) { return project.textures[i].name; },
                [&](std::size_t i) { return Thumb{project.textures[i].texture.id, false}; });
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
                // Material indices are in both texture sets, so renumber them together.
                // A removed material becomes the format's 0xffff missing-texture slot.
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
    }
    // The buildblocks (cell_type) and piece meshes (piece words) this building can use.
    // Entries are used by index, so only a buildblock nothing uses can be removed, and
    // pieces can only be added.
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
        // Add: pick one from the map's library (the same grid as everywhere else).
        if (library.empty()) {
            ImGui::TextDisabled("Nothing to add.");
        } else {
            ImGui::TextUnformatted("Add:");
            ImGui::SameLine();
            std::size_t add = library.size();
            if (grid_button("##add", 22.0F, library.size(), add, [&](std::size_t i) { return library[i]; },
                    [](std::size_t) { return Thumb{}; }, "Add")) {
                if (std::ranges::find(names, library[add]) == names.end()) {
                    names.push_back(library[add]);
                    mark_building_changed(project, ui);
                }
                    }
        }
        ImGui::PopID();
                               };
    std::vector<std::string> buildblock_library;
    for (const auto& asset : project.buildblocks) buildblock_library.push_back(asset.name);
    name_list("Buildblocks", building.block_names, buildblock_library, true);
    name_list("Pieces", building.piece_names, block_names(project), false);

    if (ImGui::CollapsingHeader("Item sets", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Names stored in this building. The game reads definitions from data/itemset/*.sis.");
        // Removing a name clears the blocks that place it and renumbers the blocks that
        // place a later name, since blocks store the name's position in this list.
        const auto remove_item_set = [&](std::size_t index) {
            for (auto& cell : building.cells) {
                if (!is_sbb_item_set_payload(cell.item_set_index)) continue;
                const auto used = sbb_item_set_name_index(cell.item_set_index);
                if (used == index) cell.item_set_index = 0U;
                else if (used > index) cell.item_set_index = (cell.item_set_index & ~0x3ffU) | (used - 1U);
                else continue;
                refresh_sbb_cell_payloads(cell);
            }
            building.item_set_names.erase(building.item_set_names.begin() + static_cast<std::ptrdiff_t>(index));
            if (ui.selected_item_set >= index && ui.selected_item_set > 0U) --ui.selected_item_set;
            mark_building_changed(project, ui);
        };
        std::optional<std::size_t> remove;
        for (std::size_t i = 0; i < building.item_set_names.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const auto uses = static_cast<std::size_t>(std::ranges::count_if(building.cells, [&](const SbbCell& cell) {
                return cell.occupied && is_sbb_item_set_payload(cell.item_set_index) && sbb_item_set_name_index(cell.item_set_index) == i;
            }));
            const auto label = std::to_string(i) + "  " + building.item_set_names[i] + (uses ? "  (" + std::to_string(uses) + " placed)" : "");
            // A small preview of the set; hover the row for a bigger one.
            const float thumb = ImGui::GetFrameHeight() * 1.4F;
            const auto* preview = item_set_thumbnail(project, building.item_set_names[i]);
            if (preview != nullptr) {
                ImGui::Image(ImTextureID(preview->texture.id), ImVec2(thumb, thumb), ImVec2(0.0F, 1.0F), ImVec2(1.0F, 0.0F));
            } else {
                ImGui::Dummy(ImVec2(thumb, thumb));
            }
            ImGui::SameLine();
            if (ImGui::Selectable(label.c_str(), ui.selected_item_set == i, ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0F, thumb))) ui.selected_item_set = i;
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(building.item_set_names[i].c_str());
                if (preview != nullptr) {
                    ImGui::Image(ImTextureID(preview->texture.id), ImVec2(192.0F, 192.0F), ImVec2(0.0F, 1.0F), ImVec2(1.0F, 0.0F));
                } else {
                    ImGui::TextDisabled("No %s.sis found next to the map, so no preview", building.item_set_names[i].c_str());
                }
                ImGui::EndTooltip();
            }
            if (ImGui::BeginPopupContextItem("##item_set_menu")) {
                if (ImGui::MenuItem(ICON_FA_OBJECT_GROUP " Place this set")) {
                    ui.selected_item_set = i;
                    ui.clear_item_set = false;
                    ui.building_tool = BuildingTool::ItemSet;
                }
                if (ImGui::MenuItem(ICON_FA_TRASH " Remove")) remove = i;
                ImGui::EndPopup();
            }
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
            if (ImGui::SmallButton(ICON_FA_XMARK)) remove = i;
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(uses ? "Remove this set and clear the %zu blocks that place it" : "Remove this set", uses);
            }
            ImGui::PopID();
        }
        if (remove) remove_item_set(*remove);
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
        if (ImGui::CollapsingHeader("Add item set", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (project.item_set_catalog.empty()) {
                ImGui::TextDisabled("No local .sis files found. Enter a name below.");
            } else {
                // Every .sis found, as previews; click one to add it to this building.
                const auto picked = asset_grid("item_set_catalog", project.item_set_catalog.size(),
                    project.item_set_catalog.size(), {.size = 56.0F, .height = 150.0F},
                    [&](std::size_t i) { return project.item_set_catalog[i]; },
                    [&](std::size_t i) {
                        const auto* t = item_set_thumbnail(project, project.item_set_catalog[i]);
                        return Thumb{t != nullptr ? t->texture.id : 0U, true};
                    });
                if (picked && building.item_set_names.size() < 1024U) add_name(project.item_set_catalog[*picked]);
            }
            ImGui::InputTextWithHint("##new_item_set_name", "Other item-set name", ui.new_item_set_name.data(), ui.new_item_set_name.size());
            ImGui::SameLine();
            ImGui::BeginDisabled(ui.new_item_set_name[0] == '\0' || building.item_set_names.size() >= 1024U);
            if (ImGui::Button("Add##typed_item_set") && add_name(ui.new_item_set_name.data())) ui.new_item_set_name.fill(0);
            ImGui::EndDisabled();
        }
    }

    if (ui.building_tool == BuildingTool::ItemSet) {
        if (ImGui::CollapsingHeader("Place item set", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::RadioButton("Place", !ui.clear_item_set)) ui.clear_item_set = false;
            ImGui::SameLine();
            if (ImGui::RadioButton("Clear", ui.clear_item_set)) ui.clear_item_set = true;
            if (!ui.clear_item_set) {
                const auto picked = asset_grid("item_set_to_place", building.item_set_names.size(),
                    ui.selected_item_set, {.size = 56.0F, .height = 150.0F},
                    [&](std::size_t i) { return building.item_set_names[i]; },
                    [&](std::size_t i) {
                        const auto* t = item_set_thumbnail(project, building.item_set_names[i]);
                        return Thumb{t != nullptr ? t->texture.id : 0U, true};
                    });
                if (picked) ui.selected_item_set = *picked;
                static constexpr std::array<const char*, 4> rotations{"0\xC2\xB0", "90\xC2\xB0", "180\xC2\xB0", "270\xC2\xB0"};
                int rotation = static_cast<int>(ui.item_set_rotation & 3U);
                field_label("Rotation");
                if (ImGui::Combo("##item_set_rotation", &rotation, rotations.data(), 4)) ui.item_set_rotation = static_cast<std::uint32_t>(rotation);
            }
            ImGui::TextWrapped("Click a block to %s its item set. Yellow markers show placed sets.", ui.clear_item_set ? "clear" : "place");
        }
    }

    const auto occupied = occupied_cell_indices(building);
    if (!occupied.empty()) {
        auto& cell = building.cells[occupied[std::min(ui.selected_building_cell, occupied.size() - 1U)]];
        if (ImGui::CollapsingHeader("Selected block", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Cell %u, %u, %u", cell.x, cell.y, cell.z);
            if (cell.interior_block() == 0xffffU) {
                bool floor = (cell.placed_flag() & 1U) != 0U;
                if (ImGui::Checkbox("Room floor", &floor)) {
                    cell.placed_flag() = floor ? (cell.placed_flag() | 1U) : (cell.placed_flag() & ~1U);
                    refresh_sbb_cell_payloads(cell);
                    mark_building_changed(project, ui);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Off: no floor here and no ceiling on the room below,\n"
                                      "so the two rooms join into one tall room.");
                }
            }
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
    }

    if (ui.building_tool == BuildingTool::PlaceBlock) draw_block_tool_panel(project, ui);
    if (ui.building_tool == BuildingTool::PaintTexture) {
        if (ImGui::CollapsingHeader("Paint", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::RadioButton("Texture", !ui.paint_side_code)) ui.paint_side_code = false;
            ImGui::SameLine();
            if (ImGui::RadioButton("Wall type", ui.paint_side_code)) ui.paint_side_code = true;
            if (!ui.paint_side_code) {
                if (const auto picked = material_grid(project, "paint_material", building.materials, ui.paint_material_index, 140.0F)) {
                    ui.paint_material_index = *picked;
                }
            } else {
                // Side code 0 leaves the side open, 1 is a plain wall, and 3 and up pick a
                // face of the block's buildblock, shown as previews.
                ImGui::TextUnformatted("Buildblock");
                buildblock_picker(project, building, ui.block_config.buildblock_index);
                const auto* asset = building.block_names.empty()
                    ? nullptr
                    : find_buildblock_asset(project, building.block_names[std::min(ui.block_config.buildblock_index, building.block_names.size() - 1U)]);
                ImGui::TextUnformatted("Wall type");
                // Entry 0 is open, 1 a plain wall, then the buildblock's faces (codes 3 and up).
                SblLayer open_face{};
                open_face.bounds = {0.0F, 1.0F, 0.0F, 1.0F};
                const auto layers = asset != nullptr ? std::min<std::size_t>(asset->data.layers.size(), 13U) : 0U;
                const auto layer_of = [&](std::size_t i) -> const SblLayer& {
                    static const SblLayer wall{};
                    return i == 0U ? open_face : i == 1U ? wall : asset->data.layers[i - 2U];
                };
                const auto code_of = [](std::size_t i) { return static_cast<std::uint32_t>(i < 2U ? i : i + 1U); };
                GridOptions options{.size = ImGui::GetFrameHeight() * 2.2F, .height = 120.0F};
                options.paint = [&](std::size_t i, ImDrawList* draw_list, ImVec2 min, ImVec2 max) {
                    draw_buildblock_layer(draw_list, min, max.x - min.x, layer_of(i));
                };
                const auto selected = static_cast<std::size_t>(ui.paint_code == 0U ? 0U : ui.paint_code == 1U ? 1U : ui.paint_code - 1U);
                if (const auto picked = asset_grid("wall_type", 2U + layers, selected, options,
                        [&](std::size_t i) {
                            return i == 0U ? std::string{"Open: no wall on this side"} : i == 1U ? std::string{"Plain wall"}
                                : "Face " + std::to_string(i - 1U) + " of " + asset->name;
                        },
                        [](std::size_t) { return Thumb{}; })) {
                    ui.paint_code = code_of(*picked);
                        }
                if (asset == nullptr && !building.block_names.empty()) {
                    ImGui::TextDisabled("Buildblock not loaded.");
                }
                ImGui::TextWrapped("Click a wall face. Layers also set the block's buildblock.");
            }
        }
    }
}

void draw_buildblock_inspector(EditorProject& project, EditorUi& ui, std::string&) {
    if (project.buildblocks.empty()) return;
    ui.selected_buildblock = std::min(ui.selected_buildblock, project.buildblocks.size() - 1U);
    auto& asset = project.buildblocks[ui.selected_buildblock];
    auto& model = asset.data;
    bool changed = false;
    if (ImGui::CollapsingHeader(asset.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
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
        // Buildings refer to faces by number. Clear in place instead of moving every
        // later face down, which would quietly change those references.
        ImGui::SameLine();
        if (ImGui::Button("Clear face")) {
            model.layers[ui.selected_buildblock_layer] = SblLayer{};
            model.layers[ui.selected_buildblock_layer].index = ui.selected_buildblock_layer;
            mark_buildblock_changed(project, ui);
        }
        auto& layer = model.layers[ui.selected_buildblock_layer];
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
        // Only how portal type 1 mirrors is understood; keep any other value as it is.
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
    }
    if (changed) mark_buildblock_changed(project, ui);
}

} // namespace pafta::app_internal
