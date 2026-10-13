// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "formats.hpp"
#include "scene.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace pafta {

// Makes a target cell with negative coordinates fit by moving the existing grid and
// its anchor together. Refuses a growth we don't support without changing anything.
// On success, returns the target's new X/Y/Z.
std::optional<std::array<std::uint32_t, 3>> shift_sbb_grid_for_cell(
    SbbBuilding& building, std::int64_t x, std::int64_t y, std::int64_t z);

// The free-fly keys held this frame.
struct FlyInput {
    bool forward{};
    bool back{};
    bool left{};
    bool right{};
    bool up{};
    bool down{};
    bool slow{};  // Blender: Shift. Alex: Ctrl.
    bool fast{};  // Blender: Z. Alex: Shift.
    bool alex_controls{};
};

// Blender: slow 1/4, fast 4x, both 1/16. Alex: slow 1/4, fast 4x, both 16x.
float fly_speed_multiplier(bool slow, bool fast, bool alex_controls = false);

// One frame of flying: Blender follows the view, Alex follows its level heading.
// A/D strafes and the vertical keys move straight up/down. Several keys together
// are normalized, so going diagonally isn't faster.
Vec3 fly_camera_step(Vec3 forward, const FlyInput& input, float speed, float seconds);

// The forward ray's hit on a horizontal placement plane, or no hit when parallel
// to the plane or pointing away from it.
std::optional<Vec3> ray_horizontal_plane_hit(Vec3 origin, Vec3 direction, float y);

} // namespace pafta
