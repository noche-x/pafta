#include "editor_state.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void test_building_state() {
    map_editor::SbbEditorState empty{{}};
    require(empty.selected_cell() == nullptr, "empty building has a selected cell");
    empty.select_next_occupied();
    empty.adjust_active_field(1);
    require(!empty.dirty(), "empty building became dirty");

    map_editor::SbbBuilding building{};
    building.cells.resize(3);
    building.cells[1].occupied = true;
    building.cells[2].occupied = true;
    map_editor::SbbEditorState state{building};
    require(state.selected_index() == 1 && !state.dirty(), "first occupied cell was not selected");
    state.select_next_occupied();
    require(state.selected_index() == 2, "next occupied cell was not selected");
    state.select_next_occupied();
    require(state.selected_index() == 1, "occupied cell navigation did not wrap");
    state.select_previous_occupied();
    require(state.selected_index() == 2, "previous occupied cell did not wrap");
    state.set_active_field(map_editor::SbbCellField::ItemSet);
    state.adjust_active_field(-1);
    require(!state.dirty(), "clamped no-op dirtied a building");
    state.adjust_active_field(2);
    require(state.dirty() && state.selected_cell()->item_set_index == 2,
        "item-set field edit did not reach the selected cell");
    state.adjust_active_field(-9);
    require(state.selected_cell()->item_set_index == 0, "item-set field did not clamp at zero");
}

void test_city_state() {
    map_editor::SbcEditorState empty{{}};
    require(empty.selected_building() == nullptr, "empty city has a selected building");
    empty.select_next_building();
    empty.adjust_active_field(1);
    require(!empty.dirty(), "empty city became dirty");

    map_editor::SbcMap map{};
    map.buildings.push_back({0, "a", 10, 0, 0, 0});
    map.buildings.push_back({1, "b", 20, 0, 0, 1});
    map_editor::SbcEditorState state{map};
    state.select_previous_building();
    require(state.selected_index() == 1, "city building navigation did not wrap");
    state.set_active_field(map_editor::SbcBuildingField::Rotation);
    state.adjust_active_field(1);
    require(state.dirty() && state.selected_building()->rotation == 2,
        "city rotation edit did not reach the selected building");
    state.select_next_building();
    require(state.selected_index() == 0 && state.selected_building()->x == 10,
        "city next-building selection is wrong");
}

void test_block_word_state() {
    map_editor::SblEditorState empty{{1, 2, 3}};
    empty.select_next_word();
    empty.adjust_selected_word(1);
    require(!empty.dirty(), "incomplete SBL word became editable");

    map_editor::SblEditorState state{{1, 0, 0, 0, 255, 255, 255, 255}};
    require(state.selected_word_value() == 1, "first SBL word was decoded incorrectly");
    state.select_previous_word();
    require(state.selected_word_index() == 1, "SBL word navigation did not wrap");
    state.adjust_selected_word(1);
    require(!state.dirty() && state.selected_word_value() == std::numeric_limits<std::uint32_t>::max(),
        "clamped SBL word edit changed bytes");
    state.adjust_selected_word(-1);
    require(state.dirty() && state.selected_word_value() == std::numeric_limits<std::uint32_t>::max() - 1,
        "SBL word edit did not write little-endian bytes");
    state.select_next_word();
    require(state.selected_word_index() == 0 && state.selected_word_value() == 1,
        "SBL next-word navigation is wrong");
}

} // namespace

int main() {
    try {
        test_building_state();
        test_city_state();
        test_block_word_state();
        std::cout << "Editor state tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
