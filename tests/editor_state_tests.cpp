// SPDX-License-Identifier: GPL-2.0-only
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

void test_building_grid_expansion() {
    pafta::SbbBuilding building;
    building.size_x = 63;
    building.size_y = 63;
    building.size_z = 64;
    building.anchor_x = building.anchor_y = building.anchor_z = 4;
    pafta::SbbCell cell;
    cell.x = cell.y = cell.z = 63;
    cell.exterior_block() = 0x40000000U;
    pafta::refresh_sbb_cell_payloads(cell);
    building.cells.push_back(cell);
    const auto original = building;
    for (const auto target : {std::array<std::int64_t, 3>{64, 63, 63}, {63, 64, 63}, {63, 63, 64},
                             {-1, 63, 63}, {63, -1, 63}, {63, 63, -1}}) {
        require(!pafta::shift_sbb_grid_for_cell(building, target[0], target[1], target[2]),
            "unsupported building grid expansion was accepted");
        require(building == original, "rejected grid expansion changed the building");
    }

    building.cells[0].x = building.cells[0].y = building.cells[0].z = 62;
    const auto coordinate = pafta::shift_sbb_grid_for_cell(building, -1, -1, -1);
    require(coordinate == std::array<std::uint32_t, 3>{0, 0, 0}, "negative expansion did not return the shifted target");
    require(building.cells[0].x == 63 && building.cells[0].y == 63 && building.cells[0].z == 63,
        "supported expansion did not move the existing cell");
    require(building.anchor_x == 5 && building.anchor_y == 5 && building.anchor_z == 5,
        "supported expansion did not preserve the anchor-relative cell position");
}

} // namespace

int main() {
    try {
        test_building_grid_expansion();
        std::cout << "Editor state tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
