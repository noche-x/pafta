// SPDX-License-Identifier: GPL-2.0-only
#include "app_internal.hpp"
#include <imgui.h>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace pafta::app_internal {
// Names the next city undo step. commit_city_edits records the step itself once
// the edit is done, and only if the map changed.
constexpr std::size_t kMaxUndo = 100;
void begin_city_action(EditorProject& project, EditorUi& ui, std::string label) {
    if (project.pending_city_label.empty()) {
        project.pending_city_label = std::move(label);
    }
    (void)ui;
}

namespace {

void record_action(EditorProject& project, std::string label) {
    project.action_log.push_back(std::move(label));
    if (project.action_log.size() > 500U) {
        project.action_log.erase(project.action_log.begin());
    }
}

// Adds the committed map as an undo step when the live map is different.
void commit_city_edits(EditorProject& project) {
    if (!project.city) {
        project.city_baseline.reset();
        project.pending_city_label.clear();
        return;
    }
    if (project.city_baseline.has_value() && *project.city_baseline != project.city->data) {
        auto label = project.pending_city_label.empty() ? std::string{"edit"} : project.pending_city_label;
        // Editing after undoing past the save throws away the redo path back to it.
        if (project.saved_undo_depth && *project.saved_undo_depth > project.undo_stack.size()) {
            project.saved_undo_depth.reset();
        }
        project.undo_stack.emplace_back(label, std::move(*project.city_baseline));
        if (project.undo_stack.size() > kMaxUndo) {
            project.undo_stack.erase(project.undo_stack.begin());
            if (project.saved_undo_depth) {
                if (*project.saved_undo_depth == 0U) project.saved_undo_depth.reset();
                else --*project.saved_undo_depth;
            }
        }
        project.redo_stack.clear();
        project.last_edit_history = -1;
        record_action(project, std::move(label));
        project.city_baseline.reset();
    }
    if (!project.city_baseline.has_value()) {
        project.city_baseline = project.city->data;
    }
    project.pending_city_label.clear();
}

// Runs `apply(saved, live)` on the live asset a history entry names. Returns false
// when that asset has since been removed or renamed.
template <typename F>
bool with_live_asset(EditorProject& project, AssetSnapshot& snapshot, F&& apply) {
    return std::visit([&](auto& saved) {
        using T = std::decay_t<decltype(saved)>;
        const auto find = [&](auto& assets) -> T* {
            return snapshot.index < assets.size() && assets[snapshot.index].name == snapshot.name
                ? &assets[snapshot.index].data : nullptr;
        };
        T* live = nullptr;
        if constexpr (std::is_same_v<T, SbbBuilding>) live = find(project.buildings);
        else if constexpr (std::is_same_v<T, SblTemplate>) live = find(project.buildblocks);
        else live = find(project.blocks);
        if (live == nullptr) return false;
        apply(saved, *live);
        return true;
    }, snapshot.data);
}

// The selected asset for each kind of history (building, buildblock, block mesh).
std::optional<std::size_t> selected_asset_index(const EditorProject& project, const EditorUi& ui, std::size_t kind) {
    const auto clamp = [](std::size_t selected, std::size_t size) -> std::optional<std::size_t> {
        if (size == 0U) return std::nullopt;
        return std::min(selected, size - 1U);
    };
    switch (kind) {
        case 0: return clamp(ui.selected_building, project.buildings.size());
        case 1: return clamp(ui.selected_buildblock, project.buildblocks.size());
        default: return clamp(ui.selected_block, project.blocks.size());
    }
}

AssetSnapshot snapshot_asset(const EditorProject& project, std::size_t kind, std::size_t index) {
    switch (kind) {
        case 0: return {index, project.buildings[index].name, project.buildings[index].data, std::nullopt};
        case 1: return {index, project.buildblocks[index].name, project.buildblocks[index].data, std::nullopt};
        default: return {index, project.blocks[index].name, project.blocks[index].data, std::nullopt};
    }
}

void commit_asset_edits(EditorProject& project, const EditorUi& ui) {
    for (std::size_t kind = 0; kind < project.asset_baselines.size(); ++kind) {
        auto& baseline = project.asset_baselines[kind];
        if (baseline.has_value()) {
            bool changed = false;
            if (!with_live_asset(project, *baseline, [&](const auto& saved, const auto& live) { changed = saved != live; })) {
                baseline.reset();
            } else if (changed) {
                record_action(project, "edit " + baseline->name);
                project.last_edit_history = static_cast<int>(kind);
                auto& undo = project.asset_undo_stacks[kind];
                undo.push_back(std::move(*baseline));
                if (undo.size() > kMaxUndo) {
                    undo.erase(undo.begin());
                }
                project.asset_redo_stacks[kind].clear();
                baseline.reset();
            }
        }
        const auto selected = selected_asset_index(project, ui, kind);
        if (!selected.has_value()) {
            baseline.reset();
        } else if (!baseline.has_value() || baseline->index != *selected) {
            baseline = snapshot_asset(project, kind, *selected);
        }
    }
}

void apply_building_name(EditorProject& project, EditorUi& ui, std::size_t index, const std::string& name) {
    auto& asset = project.buildings[index];
    // Rename its placements first, because they find it by name. The undo history gets
    // the same rename, so undoing other edits still finds the placements.
    const auto rename_in = [&](SbcMap& map) {
        for (auto& placement : map.buildings) {
            if (find_building_asset(project, placement.name) == &asset) placement.name = name;
        }
    };
    if (project.city) {
        if (project.city->data.buildings.end() != std::ranges::find_if(project.city->data.buildings,
                [&](const SbcBuilding& p) { return find_building_asset(project, p.name) == &asset; })) {
            ui.city_dirty = true;
        }
        rename_in(project.city->data);
    }
    if (project.city_baseline) rename_in(*project.city_baseline);
    for (auto& [label, map] : project.undo_stack) rename_in(map);
    for (auto& [label, map] : project.redo_stack) rename_in(map);
    // Building edit history finds its asset by index and name.
    const auto rename_snapshot = [&](AssetSnapshot& snapshot) {
        if (!snapshot.renamed_to && std::holds_alternative<SbbBuilding>(snapshot.data) && snapshot.index == index && snapshot.name == asset.name) snapshot.name = name;
    };
    for (auto* stacks : {&project.asset_undo_stacks, &project.asset_redo_stacks}) {
        for (auto& stack : *stacks) for (auto& snapshot : stack) rename_snapshot(snapshot);
    }
    for (auto& baseline : project.asset_baselines) if (baseline) rename_snapshot(*baseline);
    asset.name = name;
    asset.dirty = true;
    ui.building_dirty = true;
    project.placement_assets_dirty = true;

}

void step_asset_history(EditorProject& project, EditorUi& ui, std::string& status, std::size_t kind, bool redo) {
    auto& from = redo ? project.asset_redo_stacks[kind] : project.asset_undo_stacks[kind];
    auto& to = redo ? project.asset_undo_stacks[kind] : project.asset_redo_stacks[kind];
    const std::string verb = redo ? "redo" : "undo";
    if (from.empty()) {
        status = "nothing to " + verb;
        return;
    }
    auto step = std::move(from.back());
    from.pop_back();
    if (step.renamed_to) {
        const bool missing = step.index >= project.buildings.size() || project.buildings[step.index].name != *step.renamed_to;
        const bool taken = std::ranges::any_of(project.buildings, [&](const auto& asset) {
            return &asset != (step.index < project.buildings.size() ? &project.buildings[step.index] : nullptr) &&
                lower_name(asset.name) == lower_name(step.name);
        });
        if (missing || taken) {
            status = verb + " rename refused: the asset changed or the name is already used";
            from.push_back(std::move(step));
            return;
        }
        auto previous_name = project.buildings[step.index].name;
        apply_building_name(project, ui, step.index, step.name);
        step.renamed_to = step.name;
        step.name = std::move(previous_name);
        ui.selected_building = step.index;
        project.asset_baselines[0] = snapshot_asset(project, 0, step.index);
        status = verb + " rename to " + project.buildings[step.index].name;
        record_action(project, status);
        to.push_back(std::move(step));
        return;
    }
    if (!with_live_asset(project, step, [](auto& saved, auto& live) { std::swap(saved, live); })) {
        status = verb + " skipped: " + step.name + " was removed or renamed";
        return;
    }
    switch (kind) {
        case 0:
            ui.selected_building = step.index;
            refresh_changed_building(project, ui);
            break;
        case 1:
            ui.selected_buildblock = step.index;
            mark_buildblock_changed(project, ui);
            break;
        default:
            ui.selected_block = step.index;
            mark_block_changed(project, ui, project.blocks[step.index].data);
            break;
    }
    record_action(project, verb + ": " + step.name);
    status = verb + " " + step.name;
    project.asset_baselines[kind] = snapshot_asset(project, kind, step.index);
    to.push_back(std::move(step));
}

// The asset history the current context edits, or nullopt for city history.
std::optional<std::size_t> active_asset_history(const EditorProject& project, const EditorUi& ui) {
    if (ui.buildblock_window_focused || ui.tab == EditorTab::Buildblocks) return 1U;
    // The Buildblock window floats over every tab. After clicking back into the view,
    // its edits still undo first when they were the last edit made.
    if (ui.show_buildblock_window && project.last_edit_history == 1) return 1U;
    if (ui.tab == EditorTab::Buildings) return 0U;
    if (ui.tab == EditorTab::Blocks) return 2U;
    return std::nullopt;
}

} // namespace

// Updates the unsaved flag and the views built from the model, without changing
// the model. Undo and redo call this directly, because their snapshot already has
// the right bounds.
void refresh_changed_building(EditorProject& project, EditorUi& ui) {
    project.buildings[ui.selected_building].dirty = true;
    ++project.city_building_revision;
    ui.building_dirty = true;
    // Only the edited building's mesh and thumbnail need rebuilding, not every city
    // building. The floor preview in the Buildings tab is its own cached model.
    project.building_floor_preview_dirty = true;
    rebuild_single_building_render(project, ui.selected_building);
    invalidate_building_thumbnail(project, ui.selected_building);
}


void commit_pending_edits(EditorProject& project, const EditorUi& ui) {
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || ImGui::IsAnyItemActive() || ui.gizmo_active != GizmoAxis::None) {
        return;
    }
    commit_city_edits(project);
    commit_asset_edits(project, ui);
}

bool can_undo(const EditorProject& project, const EditorUi& ui, bool redo) {
    if (const auto kind = active_asset_history(project, ui)) {
        return !(redo ? project.asset_redo_stacks[*kind] : project.asset_undo_stacks[*kind]).empty();
    }
    return !(redo ? project.redo_stack : project.undo_stack).empty();
}

void undo_edit(EditorProject& project, EditorUi& ui, std::string& status, bool redo) {
    if (const auto kind = active_asset_history(project, ui)) {
        commit_asset_edits(project, ui);
        step_asset_history(project, ui, status, *kind, redo);
    } else if (redo) {
        redo_city(project, ui, status);
    } else {
        undo_city(project, ui, status);
    }
}

// Undoes the last action: the snapshot from before the edit becomes current, and
// the current state goes onto the redo stack so it can be redone.
void undo_city(EditorProject& project, EditorUi& ui, std::string& status) {
    commit_city_edits(project);
    if (!project.city || project.undo_stack.empty()) {
        status = "nothing to undo";
        return;
    }
    auto [label, snapshot] = std::move(project.undo_stack.back());
    project.undo_stack.pop_back();
    project.redo_stack.emplace_back(label, project.city->data);
    project.city->data = std::move(snapshot);
    project.city_baseline = project.city->data;
    record_action(project, "undo: " + label);
    invalidate_city_block_preview_scene(project);
    invalidate_portal_render_models(project);
    ui.city_selection = CitySelectionKind::None;
    ui.selected_city_buildings.clear();
    ui.selected_city_streets.clear();
    ui.selected_portals.clear();
    ui.selected_piece_cells.clear();
    ui.selected_piece_voxel.reset();
    ui.pending_road_start.reset();
    ui.city_dirty = true;
    status = "undo " + label;
}

void redo_city(EditorProject& project, EditorUi& ui, std::string& status) {
    commit_city_edits(project);
    if (!project.city || project.redo_stack.empty()) {
        status = "nothing to redo";
        return;
    }
    auto [label, snapshot] = std::move(project.redo_stack.back());
    project.redo_stack.pop_back();
    project.undo_stack.emplace_back(label, project.city->data);
    project.city->data = std::move(snapshot);
    project.city_baseline = project.city->data;
    record_action(project, "redo: " + label);
    invalidate_city_block_preview_scene(project);
    invalidate_portal_render_models(project);
    ui.city_selection = CitySelectionKind::None;
    ui.selected_city_buildings.clear();
    ui.selected_city_streets.clear();
    ui.selected_portals.clear();
    ui.selected_piece_cells.clear();
    ui.selected_piece_voxel.reset();
    ui.pending_road_start.reset();
    ui.city_dirty = true;
    status = "redo " + label;
}


void rename_building(EditorProject& project, EditorUi& ui, const std::string& name, std::string& status) {
    auto& asset = project.buildings[ui.selected_building];
    if (name == asset.name) return;
    if (name.empty() || name.size() > 51 || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos) {
        status = "A building name needs 1-51 characters and no / \\ or :";
        return;
    }
    for (const auto& other : project.buildings) {
        if (&other != &asset && lower_name(other.name) == lower_name(name)) {
            status = "Another building is already named " + other.name;
            return;
        }
    }
    commit_city_edits(project);
    commit_asset_edits(project, ui);
    auto snapshot = snapshot_asset(project, 0, ui.selected_building);
    auto label = "rename " + asset.name + " to " + name;
    apply_building_name(project, ui, ui.selected_building, name);
    snapshot.renamed_to = name;
    auto& history = project.asset_undo_stacks[0];
    history.push_back(std::move(snapshot));
    if (history.size() > kMaxUndo) history.erase(history.begin());
    project.asset_redo_stacks[0].clear();
    project.asset_baselines[0] = snapshot_asset(project, 0, ui.selected_building);
    record_action(project, label);
    status = std::move(label);
}

} // namespace pafta::app_internal
