// SPDX-License-Identifier: GPL-2.0-only
#include "app_internal.hpp"

#ifndef __EMSCRIPTEN__
#include "file_dialogs.hpp"
#endif

#include <imgui.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pafta::app_internal {

namespace {
void checked_name(const std::string& name) {
    if (name.empty() || name.size() > 51 || name == "." || name == ".." ||
        name.find_first_of("/\\:") != std::string::npos) {
        throw std::runtime_error("Asset name must be 1-51 characters without path separators");
    }
}
} // namespace

namespace {

void entries_changed(ArchiveWorkspace& workspace) {
    workspace.sbl_labels.clear();
    workspace.texture_previews.clear();
}

// Changes whenever the map's assets could have changed: another map, assets added or
// removed, or a new undo step anywhere.
std::uint64_t map_fingerprint(const EditorProject& project) {
    std::uint64_t hash = std::hash<std::string>{}(project.root.string());
    const auto mix = [&](std::uint64_t value) { hash = (hash ^ value) * 0x100000001b3ULL; };
    mix(project.archive_entries.size());
    mix(project.buildings.size());
    mix(project.blocks.size());
    mix(project.undo_stack.size());
    mix(project.redo_stack.size());
    mix(project.action_log.size());
    for (const auto& stack : project.asset_undo_stacks) mix(stack.size());
    for (const auto& stack : project.asset_redo_stacks) mix(stack.size());
    return hash;
}

std::string byte_size(std::size_t bytes) {
    char text[32];
    if (bytes >= 1024U * 1024U) std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else if (bytes >= 1024U) std::snprintf(text, sizeof(text), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    else std::snprintf(text, sizeof(text), "%zu B", bytes);
    return text;
}

const char* kind_label(const CsxEntry& entry) {
    return entry.kind == CsxEntryKind::Sbb ? "Building" : entry.kind == CsxEntryKind::Texture ? "Texture" :
        entry.kind == CsxEntryKind::Sbl ? "Block" : "Unknown";
}

} // namespace

void import_project_archive(ArchiveWorkspace& workspace, const EditorProject& project) {
    workspace.archive = project_archive(project);
    workspace.selected.clear();
    workspace.anchor.reset();
    entries_changed(workspace);
    workspace.dirty = true;
    workspace.message.clear();
}

void save_archive_to(ArchiveWorkspace& workspace, std::filesystem::path path) {
    if (path.empty()) throw std::runtime_error("Choose an archive output path");
    if (lower_extension(path) != ".csx") path += ".csx";
    const auto& entries = workspace.archive.entries;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        checked_name(entries[i].name);
        for (std::size_t j = 0; j < i; ++j) {
            if (entries[j].type == entries[i].type && lower_name(entries[j].name) == lower_name(entries[i].name))
                throw std::runtime_error("Rename duplicate asset: " + entries[i].name);
        }
    }
    const auto bytes = build_csx(workspace.archive);
#ifdef __EMSCRIPTEN__
    web_download(path.filename().string(), bytes);
    workspace.message = "Download requested: " + path.filename().string();
#else
    write_file_bytes(path, bytes);
    workspace.message = "Wrote " + path.string();
#endif
    workspace.dirty = false;
}

// The game only reads <map folder>/test.csx, so the archive is always written under
// that name into the chosen map folder.
bool save_archive_workspace(ArchiveWorkspace& workspace) {
    std::filesystem::path folder{workspace.output_path.data()};
#ifndef __EMSCRIPTEN__
    if (folder.empty()) {
        const auto selected_folder = dialog_pick_folder();
        if (!selected_folder) return false;
        folder = *selected_folder;
        const auto text = folder.string();
        workspace.output_path.fill('\0');
        std::copy_n(text.data(), std::min(text.size(), workspace.output_path.size() - 1U), workspace.output_path.data());
    }
    if (!std::filesystem::is_directory(folder)) throw std::runtime_error("The map folder does not exist: " + folder.string());
#endif
    save_archive_to(workspace, folder / "test.csx");
    return true;
}

// This workspace has its own archive. It follows the open map (refilled a few
// entries per frame whenever the map's assets change) until you add, remove, or
// rename something here; after that it keeps your edits until you reload it.
void draw_csx_editor(ArchiveWorkspace& workspace, bool& open, EditorProject* project) {
    auto& archive = workspace.archive;
    auto& input_path = workspace.input_path;
    auto& output_path = workspace.output_path;
    auto& selected = workspace.selected;
    auto& message = workspace.message;
    auto& dirty = workspace.dirty;
    auto& sbl_labels = workspace.sbl_labels;
    if (!open) return;
    ImGui::SetNextWindowSize(ImVec2(820, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FA_BOX_ARCHIVE " CSX archive###CsxArchive", &open)) {
        ImGui::End();
        return;
    }
    // File operations talk to the outside world, so show their failures in the
    // workspace and keep the archive, so one bad file can't throw away other edits.
    const auto file_action = [&](auto operation) {
        try {
            operation();
        } catch (const std::exception& error) {
            message = error.what();
        }
    };
    const auto edited_by_hand = [&] {
        entries_changed(workspace);
        dirty = true;
        workspace.follows_map = false;
        workspace.refill.reset();
    };

    // Follow the open map: start a refill when its assets changed, then run steps for
    // a few milliseconds each frame.
    if (project != nullptr && workspace.follows_map) {
        if (output_path[0] == '\0') {
            const auto text = project->root.string();
            std::copy_n(text.data(), std::min(text.size(), output_path.size() - 1U), output_path.data());
        }
        const auto fingerprint = map_fingerprint(*project);
        if (!workspace.refill && fingerprint != workspace.map_fingerprint) {
            workspace.map_fingerprint = fingerprint;
            try {
                workspace.refill = project_archive_begin(*project);
                workspace.refill_step = 0;
            } catch (const std::exception& error) {
                // The map can't be rebuilt into an archive (a source asset failed to
                // load), but its archives can still be listed as stored on disk.
                archive = CsxArchive{0xc5f17e01U, 0, 0, {}};
                for (const auto& path : project_archive_paths(*project)) {
                    try {
                        auto stored = parse_csx_file(path);
                        archive.entries.insert(archive.entries.end(), std::make_move_iterator(stored.entries.begin()),
                            std::make_move_iterator(stored.entries.end()));
                    } catch (const std::exception&) {
                    }
                }
                selected.clear();
                workspace.anchor.reset();
                entries_changed(workspace);
                dirty = false;
                message = std::string{"Showing the stored archive. Writing is blocked: "} + error.what();
            }
        }
        if (workspace.refill) {
            const auto steps = project_archive_step_count(*project);
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(8);
            try {
                while (workspace.refill_step < steps && std::chrono::steady_clock::now() < until) {
                    project_archive_step(*project, *workspace.refill, workspace.refill_step);
                    ++workspace.refill_step;
                }
            } catch (const std::exception& error) {
                // Stop here and keep the last good list; the message says why.
                workspace.refill.reset();
                message = std::string{"Could not read the map's assets: "} + error.what();
            }
            if (workspace.refill && workspace.refill_step >= steps) {
                project_archive_finish(*workspace.refill);
                archive = std::move(*workspace.refill);
                workspace.refill.reset();
                selected.erase(std::remove_if(selected.begin(), selected.end(), [&](std::size_t i) { return i >= archive.entries.size(); }), selected.end());
                entries_changed(workspace);
                dirty = false;
                message = "Filled from the open map: " + std::to_string(archive.entries.size()) + " assets";
            }
        }
    }

    const auto texture_parts = [&](const CsxEntry& entry) {
        const auto image_size = read_le_u32(entry.payload, 0x44);
        const auto material_size = read_le_u32(entry.payload, 0x48);
        if (entry.payload.size() < 0x4c || static_cast<std::uint64_t>(image_size) + material_size > entry.payload.size() - 0x4c)
            throw std::runtime_error("Texture payload is truncated: " + entry.name);
        return std::pair{image_size, material_size};
    };
    const auto put = [](auto& bytes, std::size_t at, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
    };
    const auto import = [&](const std::filesystem::path& path) {
        const auto extension = lower_extension(path);
        auto bytes = read_file_bytes(path);
        if (extension == ".csx") {
            auto loaded = parse_csx(bytes);
            if (loaded.magic != 0xc5f17e01U || loaded.entries.size() != loaded.entry_count)
                throw std::runtime_error("Invalid CSX directory");
            for (const auto& entry : loaded.entries) {
                checked_name(entry.name);
                if (entry.payload.size() != entry.size) throw std::runtime_error("Truncated CSX entry: " + entry.name);
                if (entry.kind == CsxEntryKind::Texture) texture_parts(entry);
            }
            archive.entries.insert(archive.entries.end(), loaded.entries.begin(), loaded.entries.end());
        } else {
            CsxEntry entry{};
            entry.name = path.stem().string();
            checked_name(entry.name);
            if (extension == ".sbb") {
                entry.kind = CsxEntryKind::Sbb;
                entry.type = 0x585a0002;
                entry.payload = rebuild_sbb(parse_sbb(bytes));
                entry.payload[0] = 1; // CSX geometry marker, not a loose file's version.
            } else if (extension == ".sbl") {
                entry.kind = CsxEntryKind::Sbl;
                entry.type = 0x585a0001;
                const auto model = parse_sbl(bytes);
                if (!model.block) bytes = rebuild_sbl_buildblock(model);
                else if (model.version == 1) {
                    // A loose v1 block has no v2 records at the end. Add the empty section
                    // before putting in the archive's leading 1.
                    bytes.resize(bytes.size() + 4, 0);
                }
                entry.payload = std::move(bytes);
                entry.payload[0] = 1;
            } else if (extension == ".png") {
                if (bytes.size() < 8 || bytes[0] != 137 || bytes[1] != 'P' || bytes[2] != 'N' || bytes[3] != 'G')
                    throw std::runtime_error("Expected a PNG image");
                entry.kind = CsxEntryKind::Texture;
                entry.type = 0x585a0004;
                entry.payload.resize(0x4c, 0);
                put(entry.payload, 0, 1);
                put(entry.payload, 0x44, static_cast<std::uint32_t>(bytes.size()));
                entry.payload.insert(entry.payload.end(), bytes.begin(), bytes.end());
            } else throw std::runtime_error("Choose a CSX, SBB, SBL or PNG file");
            archive.entries.push_back(std::move(entry));
        }
        edited_by_hand();
        message = "Added " + path.filename().string();
    };
    const auto remove_selected = [&] {
        std::sort(selected.rbegin(), selected.rend());
        for (const auto index : selected) {
            if (index < archive.entries.size()) archive.entries.erase(archive.entries.begin() + static_cast<std::ptrdiff_t>(index));
        }
        selected.clear();
        workspace.anchor.reset();
        edited_by_hand();
    };
    // Writes the entries as loose files: textures as PNG (and their material PNG),
    // buildings as .sbb, blocks as .sbl. Asks for a folder on the desktop.
    const auto extract = [&](std::vector<std::size_t> indices) {
        std::filesystem::path folder;
#ifndef __EMSCRIPTEN__
        const auto selected_folder = dialog_pick_folder(std::filesystem::path{output_path.data()});
        if (!selected_folder) return;
        folder = *selected_folder;
#endif
        const auto write = [&](const std::string& name, const std::vector<std::uint8_t>& bytes) {
#ifdef __EMSCRIPTEN__
            web_download(name, bytes);
#else
            write_file_bytes(folder / name, bytes);
#endif
        };
        for (const auto i : indices) {
            if (i >= archive.entries.size()) continue;
            const auto& entry = archive.entries[i];
            checked_name(entry.name);
            if (entry.kind == CsxEntryKind::Texture) {
                const auto [image_size, material_size] = texture_parts(entry);
                write(entry.name + ".png", {entry.payload.begin() + 0x4c, entry.payload.begin() + 0x4c + image_size});
                if (material_size) write(entry.name + ".material.png",
                    {entry.payload.begin() + 0x4c + image_size, entry.payload.begin() + 0x4c + image_size + material_size});
            } else {
                auto bytes = entry.payload;
                if (entry.kind == CsxEntryKind::Sbb) bytes = normalize_packed_sbb_bytes(bytes);
                if (entry.kind == CsxEntryKind::Sbl) bytes = normalize_packed_sbl_bytes(bytes);
                write(entry.name + (entry.kind == CsxEntryKind::Sbb ? ".sbb" : entry.kind == CsxEntryKind::Sbl ? ".sbl" : ".bin"), bytes);
            }
        }
#ifdef __EMSCRIPTEN__
        message = "Downloads requested for " + std::to_string(indices.size()) + " files";
#else
        message = "Extracted " + std::to_string(indices.size()) + " files to " + folder.string();
#endif
    };
    std::vector<std::size_t> all(archive.entries.size());
    for (std::size_t i = 0; i < all.size(); ++i) all[i] = i;

    // Top row: where the archive comes from, and adding files.
    if (project != nullptr) {
        if (workspace.follows_map) {
            ImGui::TextDisabled(ICON_FA_LINK " Follows the open map");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The list refreshes by itself when the map's assets change.\nAdding, removing, or renaming here stops that.");
        } else {
            if (ImGui::Button(ICON_FA_ROTATE " Reload from map")) {
                workspace.follows_map = true;
                workspace.map_fingerprint = 0;
                message.clear();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Throw away the edits here and follow the open map again.");
        }
        ImGui::SameLine();
    }
#ifndef __EMSCRIPTEN__
    if (ImGui::Button(ICON_FA_PLUS " Add files...")) file_action([&] {
        const auto paths = dialog_open_archive_assets();
        for (const auto& path : paths) import(path);
    });
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add CSX, SBB, SBL, or PNG files");
    ImGui::SameLine();
#else
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0F);
    ImGui::InputTextWithHint("##input_file", "File path", input_path.data(), input_path.size());
    ImGui::SameLine();
    if (ImGui::Button("Add file")) file_action([&] { import(input_path.data()); });
    ImGui::SameLine();
#endif
    if (ImGui::Button(ICON_FA_ERASER " Clear")) ImGui::OpenPopup("Clear archive?");
    if (ImGui::BeginPopupModal("Clear archive?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Discard all files in this archive workspace?");
        if (ImGui::Button("Clear")) {
            archive.entries.clear();
            selected.clear();
            workspace.anchor.reset();
            edited_by_hand();
            dirty = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Progress while the list fills from the map.
    if (workspace.refill && project != nullptr) {
        const auto steps = std::max<std::size_t>(1U, project_archive_step_count(*project));
        const auto fraction = static_cast<float>(workspace.refill_step) / static_cast<float>(steps);
        const auto label = "Reading map assets " + std::to_string(workspace.refill_step) + " / " + std::to_string(steps);
        ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0.0F), label.c_str());
    }

    // Filter: matches the name or the type.
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 12.0F);
    ImGui::InputTextWithHint("##archive_filter", ICON_FA_MAGNIFYING_GLASS " Filter by name or type", workspace.filter.data(), workspace.filter.size());
    const auto needle = lower_name(workspace.filter.data());
    std::vector<std::size_t> visible;
    for (std::size_t i = 0; i < archive.entries.size(); ++i) {
        const auto& entry = archive.entries[i];
        if (needle.empty() || lower_name(entry.name).find(needle) != std::string::npos ||
            lower_name(kind_label(entry)).find(needle) != std::string::npos) visible.push_back(i);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu shown, %zu selected", visible.size(), selected.size());

    // Clicking: plain click picks one row, Ctrl/Cmd adds or removes a row, Shift picks
    // the range from the last clicked row, Ctrl+A picks every shown row.
    const auto click_row = [&](std::size_t entry_index) {
        const auto& io = ImGui::GetIO();
        const bool toggle = io.KeyCtrl || io.KeySuper;
        if (io.KeyShift && workspace.anchor) {
            const auto from = std::ranges::find(visible, *workspace.anchor);
            const auto to = std::ranges::find(visible, entry_index);
            if (from != visible.end() && to != visible.end()) {
                if (!toggle) selected.clear();
                for (auto it = std::min(from, to); it <= std::max(from, to); ++it) {
                    if (std::ranges::find(selected, *it) == selected.end()) selected.push_back(*it);
                }
                return;
            }
        }
        if (toggle) {
            const auto it = std::ranges::find(selected, entry_index);
            if (it == selected.end()) selected.push_back(entry_index);
            else selected.erase(it);
        } else {
            selected = {entry_index};
        }
        workspace.anchor = entry_index;
    };
    std::optional<std::size_t> rename_request;
    const float footer = ImGui::GetFrameHeightWithSpacing() * 3.0F + ImGui::GetTextLineHeightWithSpacing() * 2.0F;
    if (ImGui::BeginTable("##archive_files", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_BordersInnerV, ImVec2(0, std::max(160.0F, ImGui::GetContentRegionAvail().y - footer)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 8.0F);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.5F);
        ImGui::TableSetupColumn("Material", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 5.5F);
        ImGui::TableHeadersRow();
        sbl_labels.resize(archive.entries.size());
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(visible.size()));
        while (clipper.Step()) {
            for (auto row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto i = visible[static_cast<std::size_t>(row)];
                const auto& entry = archive.entries[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool is_selected = std::ranges::find(selected, i) != selected.end();
                if (ImGui::Selectable(entry.name.empty() ? "(no name)" : entry.name.c_str(), is_selected, ImGuiSelectableFlags_SpanAllColumns)) click_row(i);
                if (ImGui::BeginPopupContextItem("##row_menu")) {
                    if (!is_selected) {
                        selected = {i};
                        workspace.anchor = i;
                    }
                    ImGui::TextDisabled("%zu selected", selected.size());
                    if (ImGui::MenuItem(ICON_FA_FILE_EXPORT " Extract...")) file_action([&] { extract(selected); });
                    if (ImGui::MenuItem(ICON_FA_PEN " Rename", nullptr, false, selected.size() == 1U)) rename_request = i;
                    if (ImGui::MenuItem(ICON_FA_TRASH " Remove", "Delete")) remove_selected();
                    ImGui::EndPopup();
                }
                // Hover preview: the image, the building or block thumbnail, or the
                // buildblock's faces.
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && i < archive.entries.size()) {
                    ImGui::BeginTooltip();
                    ImGui::TextUnformatted(entry.name.c_str());
                    ImGui::TextDisabled("%s, %s", kind_label(entry), byte_size(entry.payload.size()).c_str());
                    constexpr float kPreview = 192.0F;
                    if (entry.kind == CsxEntryKind::Texture) {
                        auto found = workspace.texture_previews.find(i);
                        if (found == workspace.texture_previews.end()) {
                            Texture2D texture{};
                            try {
                                const auto [image_size, material_size] = texture_parts(entry);
                                (void)material_size;
                                Image image = LoadImageFromMemory(".png", entry.payload.data() + 0x4c, static_cast<int>(image_size));
                                if (image.data != nullptr) {
                                    texture = LoadTextureFromImage(image);
                                    UnloadImage(image);
                                }
                            } catch (const std::exception&) {
                            }
                            found = workspace.texture_previews.emplace(i, LoadedTexture{entry.name, texture}).first;
                        }
                        const auto& texture = found->second.texture;
                        if (texture.id != 0U) {
                            const float scale = kPreview / static_cast<float>(std::max(texture.width, texture.height));
                            ImGui::Image(ImTextureID(texture.id), ImVec2(static_cast<float>(texture.width) * scale, static_cast<float>(texture.height) * scale));
                            ImGui::TextDisabled("%d x %d", texture.width, texture.height);
                        } else {
                            ImGui::TextDisabled("Could not read the image");
                        }
                    } else if (project != nullptr && (entry.kind == CsxEntryKind::Sbb || entry.kind == CsxEntryKind::Sbl)) {
                        const auto key = lower_name(entry.name);
                        const RenderTexture2D* thumb = nullptr;
                        if (entry.kind == CsxEntryKind::Sbb) {
                            for (std::size_t b = 0; b < project->buildings.size() && thumb == nullptr; ++b) {
                                if (lower_name(project->buildings[b].name) == key) thumb = building_thumbnail(*project, b);
                            }
                        } else {
                            for (std::size_t b = 0; b < project->blocks.size() && thumb == nullptr; ++b) {
                                if (lower_name(project->blocks[b].name) == key) thumb = block_thumbnail(*project, b);
                            }
                        }
                        if (thumb != nullptr && thumb->texture.id != 0U) {
                            ImGui::Image(ImTextureID(thumb->texture.id), ImVec2(kPreview, kPreview), ImVec2(0.0F, 1.0F), ImVec2(1.0F, 0.0F));
                        } else if (entry.kind == CsxEntryKind::Sbl) {
                            try {
                                const auto model = parse_sbl(entry.payload);
                                if (!model.block) {
                                    for (std::size_t l = 0; l < model.layers.size(); ++l) {
                                        if (l % 4U != 0U) ImGui::SameLine();
                                        const auto at = ImGui::GetCursorScreenPos();
                                        draw_buildblock_layer(ImGui::GetWindowDrawList(), at, 56.0F, model.layers[l]);
                                        ImGui::Dummy(ImVec2(56.0F, 56.0F));
                                    }
                                } else {
                                    ImGui::TextDisabled("Not in the open map, so no preview");
                                }
                            } catch (const std::exception& error) {
                                ImGui::TextDisabled("%s", error.what());
                            }
                        } else {
                            ImGui::TextDisabled("Not in the open map, so no preview");
                        }
                    }
                    ImGui::EndTooltip();
                }
                ImGui::TableNextColumn();
                if (entry.kind == CsxEntryKind::Sbl) {
                    auto& label = sbl_labels[i];
                    if (!label) {
                        try {
                            label = parse_sbl(entry.payload).block ? "Block mesh" : "Buildblock";
                        } catch (const std::exception& error) {
                            label = std::string{"Invalid SBL: "} + error.what();
                        }
                    }
                    ImGui::TextUnformatted(label->c_str());
                } else ImGui::TextUnformatted(kind_label(entry));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(byte_size(entry.payload.size()).c_str());
                ImGui::TableNextColumn();
                if (entry.kind == CsxEntryKind::Texture && entry.payload.size() >= 0x4c)
                    ImGui::TextUnformatted(read_le_u32(entry.payload, 0x48) ? "Attached" : "None");
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    // Keys for the list while the window has focus.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput) {
        const auto& io = ImGui::GetIO();
        if ((io.KeyCtrl || io.KeySuper) && ImGui::IsKeyPressed(ImGuiKey_A)) selected = visible;
        if (!selected.empty() && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) remove_selected();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) selected.clear();
    }

    // The selected asset's name and material.
    if (selected.size() == 1 && selected[0] < archive.entries.size()) {
        auto& entry = archive.entries[selected[0]];
        std::array<char, 52> name{};
        std::copy_n(entry.name.data(), std::min(entry.name.size(), name.size() - 1), name.data());
        if (rename_request) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0F);
        if (ImGui::InputText("Name##asset_name", name.data(), name.size(), ImGuiInputTextFlags_EnterReturnsTrue)) file_action([&] {
            checked_name(name.data());
            entry.name = name.data();
            edited_by_hand();
        });
        if (entry.kind == CsxEntryKind::Texture) {
            ImGui::SameLine();
            if (ImGui::Button("Attach material PNG...")) file_action([&] {
                std::filesystem::path path{input_path.data()};
#ifndef __EMSCRIPTEN__
                const auto selected_path = dialog_open_material_image();
                if (!selected_path) return;
                path = *selected_path;
#endif
                const auto bytes = read_file_bytes(path);
                if (lower_extension(path) != ".png" || bytes.size() < 8 || bytes[0] != 137 || bytes[1] != 'P' || bytes[2] != 'N' || bytes[3] != 'G')
                    throw std::runtime_error("Material must be a PNG image");
                const auto [image_size, material_size] = texture_parts(entry);
                const auto material = path.stem().string();
                if (material.size() > 63) throw std::runtime_error("Material name is too long");
                entry.payload.resize(0x4c + image_size);
                std::fill(entry.payload.begin() + 4, entry.payload.begin() + 0x44, 0);
                std::copy(material.begin(), material.end(), entry.payload.begin() + 4);
                put(entry.payload, 0x48, static_cast<std::uint32_t>(bytes.size()));
                entry.payload.insert(entry.payload.end(), bytes.begin(), bytes.end());
                edited_by_hand();
            });
            ImGui::SameLine();
            if (ImGui::Button("Remove material")) file_action([&] {
                const auto [image_size, material_size] = texture_parts(entry);
                entry.payload.resize(0x4c + image_size);
                std::fill(entry.payload.begin() + 4, entry.payload.begin() + 0x44, 0);
                put(entry.payload, 0x48, 0);
                edited_by_hand();
            });
        }
    } else {
        ImGui::TextDisabled("Click, Shift+click, or Ctrl+click to select. Right-click a row for more.");
    }

    // Output: the map folder that gets test.csx, the only archive name the game reads.
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 7.0F);
    ImGui::InputTextWithHint("##output_folder", "Map folder", output_path.data(), output_path.size());
    ImGui::SameLine();
#ifndef __EMSCRIPTEN__
    if (ImGui::Button("Browse...##output_folder")) file_action([&] {
        if (const auto folder = dialog_pick_folder(std::filesystem::path{output_path.data()})) {
            const auto text = folder->string();
            output_path.fill('\0');
            std::copy_n(text.data(), std::min(text.size(), output_path.size() - 1U), output_path.data());
        }
    });
#endif
    ImGui::BeginDisabled(workspace.refill.has_value());
    if (ImGui::Button(dirty ? ICON_FA_FLOPPY_DISK " Write test.csx *" : ICON_FA_FLOPPY_DISK " Write test.csx")) {
        file_action([&] { save_archive_workspace(workspace); });
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Writes test.csx into the map folder; the game reads no other name.");
    ImGui::SameLine();
    ImGui::BeginDisabled(selected.empty());
    if (ImGui::Button(ICON_FA_FILE_EXPORT " Extract selected...")) file_action([&] { extract(selected); });
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(archive.entries.empty());
    if (ImGui::Button(ICON_FA_FILE_EXPORT " Extract all...")) file_action([&] { extract(all); });
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(selected.empty());
    if (ImGui::Button(ICON_FA_TRASH " Remove")) remove_selected();
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", message.c_str());
    ImGui::End();
}

} // namespace pafta::app_internal
