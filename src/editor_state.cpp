#include "editor_state.hpp"

#include <algorithm>
#include <limits>

namespace map_editor {
namespace {

std::size_t first_occupied_index(const SbbBuilding& building) {
    const auto match = std::ranges::find_if(building.cells, [](const SbbCell& cell) {
        return cell.occupied;
    });
    return match == building.cells.end() ? 0U : static_cast<std::size_t>(match - building.cells.begin());
}

std::uint32_t add_clamped(std::uint32_t value, std::int32_t delta) {
    if (delta < 0 && value < static_cast<std::uint32_t>(-delta)) {
        return 0;
    }
    if (delta > 0 && value > std::numeric_limits<std::uint32_t>::max() - static_cast<std::uint32_t>(delta)) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(value) + delta);
}

std::uint32_t& field_ref(SbbCell& cell, SbbCellField field) {
    switch (field) {
        case SbbCellField::ExteriorBlock:
            return cell.exterior_block();
        case SbbCellField::InteriorBlock:
            return cell.interior_block();
        case SbbCellField::CellType:
            return cell.cell_type();
        case SbbCellField::ExteriorPortalFace:
            return cell.exterior_portal_face();
        case SbbCellField::InteriorPortalFace:
            return cell.interior_portal_face();
        case SbbCellField::PlacedFlag:
            return cell.placed_flag();
        case SbbCellField::ItemSet:
            return cell.item_set_index;
    }
    return cell.exterior_block();
}

std::int32_t& field_ref(SbcBuilding& building, SbcBuildingField field) {
    switch (field) {
        case SbcBuildingField::X:
            return building.x;
        case SbcBuildingField::Y:
            return building.y;
        case SbcBuildingField::Z:
            return building.z;
        case SbcBuildingField::Rotation:
            return building.rotation;
    }
    return building.x;
}

std::uint32_t read_word(const std::vector<std::uint8_t>& bytes, std::size_t word_index) {
    const auto offset = word_index * 4U;
    if (offset + 4 > bytes.size()) {
        return 0;
    }
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

void write_word(std::vector<std::uint8_t>& bytes, std::size_t word_index, std::uint32_t value) {
    const auto offset = word_index * 4U;
    if (offset + 4 > bytes.size()) {
        return;
    }
    bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    bytes[offset + 3] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
}

} // namespace

SbbEditorState::SbbEditorState(SbbBuilding building)
    : building_{std::move(building)}, selected_index_{first_occupied_index(building_)} {}

const SbbBuilding& SbbEditorState::building() const {
    return building_;
}

std::size_t SbbEditorState::selected_index() const {
    return selected_index_;
}

SbbCellField SbbEditorState::active_field() const {
    return active_field_;
}

bool SbbEditorState::dirty() const {
    return dirty_;
}

const SbbCell* SbbEditorState::selected_cell() const {
    if (selected_index_ >= building_.cells.size()) {
        return nullptr;
    }
    return &building_.cells[selected_index_];
}

void SbbEditorState::set_active_field(SbbCellField field) {
    active_field_ = field;
}

void SbbEditorState::select_next_occupied() {
    if (building_.cells.empty()) {
        return;
    }
    for (std::size_t step = 1; step <= building_.cells.size(); ++step) {
        const auto index = (selected_index_ + step) % building_.cells.size();
        if (building_.cells[index].occupied) {
            selected_index_ = index;
            return;
        }
    }
}

void SbbEditorState::select_previous_occupied() {
    if (building_.cells.empty()) {
        return;
    }
    for (std::size_t step = 1; step <= building_.cells.size(); ++step) {
        const auto index = (selected_index_ + building_.cells.size() - step) % building_.cells.size();
        if (building_.cells[index].occupied) {
            selected_index_ = index;
            return;
        }
    }
}

void SbbEditorState::adjust_active_field(std::int32_t delta) {
    if (selected_index_ >= building_.cells.size()) {
        return;
    }
    auto& value = field_ref(building_.cells[selected_index_], active_field_);
    const auto next = add_clamped(value, delta);
    if (next != value) {
        value = next;
        dirty_ = true;
    }
}

SbcEditorState::SbcEditorState(SbcMap map) : map_{std::move(map)} {}

const SbcMap& SbcEditorState::map() const {
    return map_;
}

std::size_t SbcEditorState::selected_index() const {
    return selected_index_;
}

SbcBuildingField SbcEditorState::active_field() const {
    return active_field_;
}

bool SbcEditorState::dirty() const {
    return dirty_;
}

const SbcBuilding* SbcEditorState::selected_building() const {
    if (selected_index_ >= map_.buildings.size()) {
        return nullptr;
    }
    return &map_.buildings[selected_index_];
}

void SbcEditorState::set_active_field(SbcBuildingField field) {
    active_field_ = field;
}

void SbcEditorState::select_next_building() {
    if (map_.buildings.empty()) {
        return;
    }
    selected_index_ = (selected_index_ + 1U) % map_.buildings.size();
}

void SbcEditorState::select_previous_building() {
    if (map_.buildings.empty()) {
        return;
    }
    selected_index_ = (selected_index_ + map_.buildings.size() - 1U) % map_.buildings.size();
}

void SbcEditorState::adjust_active_field(std::int32_t delta) {
    if (selected_index_ >= map_.buildings.size()) {
        return;
    }
    auto& value = field_ref(map_.buildings[selected_index_], active_field_);
    value += delta;
    dirty_ = true;
}

SblEditorState::SblEditorState(std::vector<std::uint8_t> bytes) : bytes_{std::move(bytes)} {}

const std::vector<std::uint8_t>& SblEditorState::bytes() const {
    return bytes_;
}

std::size_t SblEditorState::selected_word_index() const {
    return selected_word_index_;
}

std::uint32_t SblEditorState::selected_word_value() const {
    return read_word(bytes_, selected_word_index_);
}

bool SblEditorState::dirty() const {
    return dirty_;
}

void SblEditorState::select_next_word() {
    const auto word_count = bytes_.size() / 4U;
    if (word_count == 0) {
        return;
    }
    selected_word_index_ = (selected_word_index_ + 1U) % word_count;
}

void SblEditorState::select_previous_word() {
    const auto word_count = bytes_.size() / 4U;
    if (word_count == 0) {
        return;
    }
    selected_word_index_ = (selected_word_index_ + word_count - 1U) % word_count;
}

void SblEditorState::adjust_selected_word(std::int32_t delta) {
    const auto word_count = bytes_.size() / 4U;
    if (selected_word_index_ >= word_count) {
        return;
    }
    const auto current = read_word(bytes_, selected_word_index_);
    const auto next = add_clamped(current, delta);
    if (next != current) {
        write_word(bytes_, selected_word_index_, next);
        dirty_ = true;
    }
}

} // namespace map_editor
