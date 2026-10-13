// SPDX-License-Identifier: GPL-2.0-only
// The editor's shared pickers. Every place that picks a texture, block, building,
// material, buildblock, or piece uses these, so they all look and behave the same:
// a grid of thumbnails with a search box, or a thumbnail button that opens that grid.
#include "app_internal.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <string>
#include <unordered_map>

namespace pafta::app_internal {
namespace {

// Each grid keeps its own search text, keyed by its ImGui ID.
std::array<char, 128>& grid_search(ImGuiID id) {
    static std::unordered_map<ImGuiID, std::array<char, 128>> searches;
    return searches[id];
}

constexpr ImU32 kSelectedOutline = IM_COL32(235, 190, 78, 255);

bool thumb_button(const char* id, const Thumb& thumb, const std::string& label, float size) {
    if (thumb.texture != 0U) {
        return ImGui::ImageButton(id, ImTextureID(thumb.texture), ImVec2(size, size),
            ImVec2(0.0F, thumb.flipped ? 1.0F : 0.0F), ImVec2(1.0F, thumb.flipped ? 0.0F : 1.0F));
    }
    const auto& padding = ImGui::GetStyle().FramePadding;
    return ImGui::Button((fit_label(label, size) + id).c_str(), ImVec2(size + padding.x * 2.0F, size + padding.y * 2.0F));
}

} // namespace

std::optional<std::size_t> asset_grid(const char* id, std::size_t count, std::size_t selected, const GridOptions& options,
    const std::function<std::string(std::size_t)>& name, const std::function<Thumb(std::size_t)>& thumb,
    const std::function<void(std::size_t)>& extra) {
    std::optional<std::size_t> clicked;
    ImGui::PushID(id);
    auto* search = options.search;
    if (search == nullptr) {
        search = &grid_search(ImGui::GetID("##search_text"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##search", ICON_FA_MAGNIFYING_GLASS " Search", search->data(), search->size());
    }
    const auto needle = lower_name(search->data());
    if (ImGui::BeginChild("##grid", ImVec2(-FLT_MIN, options.height), options.height > 0.0F ? ImGuiChildFlags_Borders : ImGuiChildFlags_None)) {
        const auto& style = ImGui::GetStyle();
        const float step = options.size + style.FramePadding.x * 2.0F + style.ItemSpacing.x;
        const int per_row = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + style.ItemSpacing.x) / step));
        int column = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto label = name(i);
            if (!needle.empty() && lower_name(label).find(needle) == std::string::npos) continue;
            if (column++ % per_row != 0) ImGui::SameLine();
            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginGroup();
            if (thumb_button("##item", thumb(i), label, options.size)) clicked = i;
            const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            if (options.paint) options.paint(i, ImGui::GetWindowDrawList(), min, max);
            if (i == selected) ImGui::GetWindowDrawList()->AddRect(min, max, kSelectedOutline, 3.0F, 0, 3.0F);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s\n%s%s", label.c_str(), options.action, i == selected ? " (selected)" : "");
            if (extra) extra(i);
            if (options.labels) ImGui::TextUnformatted(fit_label(label, options.size + style.FramePadding.x * 2.0F).c_str());
            ImGui::EndGroup();
            ImGui::PopID();
        }
        if (column == 0) ImGui::TextDisabled(count == 0 ? "Nothing here yet." : "Nothing matches.");
    }
    ImGui::EndChild();
    ImGui::PopID();
    return clicked;
}

bool grid_button(const char* id, float size, std::size_t count, std::size_t& selected,
    const std::function<std::string(std::size_t)>& name, const std::function<Thumb(std::size_t)>& thumb, const char* tooltip,
    const GridOptions& popup_options) {
    ImGui::PushID(id);
    const bool valid = selected < count;
    const auto current = valid ? name(selected) : std::string{"(none)"};
    const bool open = thumb_button("##button", valid ? thumb(selected) : Thumb{}, current, size);
    if (valid && popup_options.paint) popup_options.paint(selected, ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemHovered()) {
        if (tooltip != nullptr) ImGui::SetTooltip("%s: %s", tooltip, current.c_str());
        else ImGui::SetTooltip("%s", current.c_str());
    }
    if (open) ImGui::OpenPopup("##pick");
    bool changed = false;
    if (ImGui::BeginPopup("##pick")) {
        ImGui::BeginChild("##pick_area", ImVec2(360.0F, popup_options.height > 0.0F ? popup_options.height + 40.0F : 300.0F));
        auto options = popup_options;
        if (options.height <= 0.0F) options.height = 250.0F;
        if (const auto picked = asset_grid("grid", count, selected, options, name, thumb)) {
            selected = *picked;
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::PopID();
    return changed;
}

bool texture_button(const EditorProject& project, const char* id, std::string& texture_name, float size, const char* tooltip) {
    const auto key = lower_name(texture_name);
    auto selected = static_cast<std::size_t>(std::ranges::find_if(project.textures, [&](const LoadedTexture& texture) {
        return lower_name(texture.name) == key;
    }) - project.textures.begin());
    const bool changed = grid_button(id, size, project.textures.size(), selected,
        [&](std::size_t i) { return project.textures[i].name; },
        [&](std::size_t i) { return Thumb{project.textures[i].texture.id, false}; }, tooltip);
    if (changed) texture_name = project.textures[selected].name;
    return changed;
}

bool texture_slots(const EditorProject& project, const char* id, std::array<std::string, 8>& names,
    const std::array<const char*, 8>* slot_labels) {
    bool changed = false;
    ImGui::PushID(id);
    constexpr float size = 30.0F;
    for (std::size_t slot = 0; slot < names.size(); ++slot) {
        ImGui::PushID(static_cast<int>(slot));
        const auto label = slot_labels != nullptr ? std::string{(*slot_labels)[slot]} : "Slot " + std::to_string(slot);
        changed |= texture_button(project, "##slot", names[slot], size, label.c_str());
        if (slot_labels != nullptr) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label.c_str());
            if (slot % 2U == 0U) ImGui::SameLine(170.0F);
        } else if (slot % 4U != 3U) {
            ImGui::SameLine();
        }
        ImGui::PopID();
    }
    // One pick for every slot.
    std::string all = names[0];
    if (texture_button(project, "##all", all, size, "Every slot")) {
        names.fill(all);
        changed = true;
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("all slots");
    ImGui::PopID();
    return changed;
}

} // namespace pafta::app_internal
