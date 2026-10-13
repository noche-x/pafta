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
    bool slow{};  // Shift
    bool fast{};  // Z: speeds up, or with Shift slows further
};

// Speed multiplier: Shift 1/4, Z 4x, Shift+Z 1/16.
float fly_speed_multiplier(bool slow, bool fast);

// One frame of flying: W/S along where you look (including up and down), A/D along
// the level right direction, and Space/Ctrl straight up and down. Several keys
// together are normalized, so going diagonally isn't faster.
Vec3 fly_camera_step(Vec3 forward, const FlyInput& input, float speed, float seconds);

} // namespace pafta
