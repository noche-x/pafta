// SPDX-License-Identifier: GPL-2.0-only
#include "editor_state.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
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

void test_control_presets() {
    const auto near = [](float a, float b) { return std::abs(a - b) < 0.001F; };
    const pafta::Vec3 forward{0.0F, 1.0F, -1.0F};
    const auto blender = pafta::fly_camera_step(forward, {.forward = true}, 10.0F, 1.0F);
    require(blender.y > 0.0F && near(blender.y, -blender.z), "Blender flying did not follow the view");
    const auto alex = pafta::fly_camera_step(forward, {.forward = true, .alex_controls = true}, 10.0F, 1.0F);
    require(near(alex.y, 0.0F) && near(alex.z, -10.0F), "Alex WASD did not stay level");
    const auto lift = pafta::fly_camera_step(forward, {.up = true, .alex_controls = true}, 10.0F, 1.0F);
    require(near(lift.y, 10.0F) && near(lift.z, 0.0F), "Alex vertical movement followed the view");
    require(near(pafta::fly_speed_multiplier(false, true, true), 4.0F) &&
        near(pafta::fly_speed_multiplier(true, false, true), 0.25F) &&
        near(pafta::fly_speed_multiplier(true, true, true), 16.0F), "Alex native editor speed modifiers changed");
    require(near(pafta::fly_speed_multiplier(true, true), 0.0625F), "Blender slowest movement changed");
    const auto diagonal = pafta::fly_camera_step(forward,
        {.forward = true, .right = true, .up = true, .alex_controls = true}, 10.0F, 1.0F);
    require(near(std::sqrt(diagonal.x * diagonal.x + diagonal.y * diagonal.y + diagonal.z * diagonal.z), 10.0F),
        "Alex diagonal flying changed the configured speed");
}

void test_locked_placement_plane() {
    const auto hit = pafta::ray_horizontal_plane_hit({10.0F, 20.0F, 30.0F}, {1.0F, -2.0F, 3.0F}, 4.0F);
    require(hit && hit->x == 18.0F && hit->y == 4.0F && hit->z == 54.0F,
        "placement ray did not intersect the locked Y plane");
    require(!pafta::ray_horizontal_plane_hit({0, 20, 0}, {1, 0, 0}, 4), "parallel placement ray hit a plane");
    require(!pafta::ray_horizontal_plane_hit({0, 20, 0}, {0, 1, 0}, 4), "placement plane behind the camera was accepted");
    const auto upward = pafta::ray_horizontal_plane_hit({0, -8, 0}, {0, 1, 0}, -4);
    require(upward && upward->y == -4, "negative locked Y levels did not work");
}

void test_brush_shapes() {
    const auto box = pafta::brush_box_cells({3, 2, 5}, {1, 9, 6});
    require(box.size() == 6 && std::ranges::all_of(box, [](const auto& c) { return c[1] == 2; }),
        "box brush did not fill the rectangle on its start level");
    require(pafta::brush_box_cells({0, 0, 0}, {500, 0, 0}).size() == static_cast<std::size_t>(pafta::kMaxBrushSide),
        "box brush was not capped");
    require(pafta::brush_disc_cells({0, 0, 0}, 0).size() == 1, "radius 0 disc is not one cell");
    const auto disc = pafta::brush_disc_cells({0, 4, 0}, 2);
    require(disc.size() == 21, "radius 2 disc has the wrong cells");

    // An L of three cells on level 0 with outside walls, a floor, and a wall upstairs.
    const std::vector<pafta::BrushFace> faces{
        {{0, 0, 0}, 1}, {{1, 0, 0}, 3}, {{1, 0, 1}, 4}, {{0, 0, 0}, 0}, {{0, 1, 0}, 1}, {{5, 0, 5}, 1}, {{0, 0, 0}, 9}};
    require(pafta::brush_face_loop(faces, 0) == std::vector<std::size_t>{0, 1, 2},
        "wall loop did not stay on its level, layer, kind, and connected cells");
    require(pafta::brush_face_loop(faces, 3) == std::vector<std::size_t>{3}, "floor loop picked up walls");
}

} // namespace

int main() {
    try {
        test_building_grid_expansion();
        test_control_presets();
        test_locked_placement_plane();
        test_brush_shapes();
        std::cout << "Editor state tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
