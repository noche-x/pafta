// SPDX-License-Identifier: GPL-2.0-only
#include "editor_state.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace pafta {
std::optional<std::array<std::uint32_t, 3>> shift_sbb_grid_for_cell(
    SbbBuilding& building, std::int64_t x, std::int64_t y, std::int64_t z) {
    constexpr auto side = static_cast<std::int64_t>(supported_sbb_grid_side);
    if (x <= -side || x >= side || y <= -side || y >= side || z <= -side || z >= side) return std::nullopt;
    const auto sx = static_cast<std::uint32_t>(std::max<std::int64_t>(0, -x));
    const auto sy = static_cast<std::uint32_t>(std::max<std::int64_t>(0, -y));
    const auto sz = static_cast<std::uint32_t>(std::max<std::int64_t>(0, -z));
    for (const auto& cell : building.cells) {
        if (cell.x >= supported_sbb_grid_side - sx || cell.y >= supported_sbb_grid_side - sy ||
            cell.z >= supported_sbb_grid_side - sz) return std::nullopt;
    }
    if (building.anchor_x > std::numeric_limits<std::uint32_t>::max() - sx ||
        building.anchor_y > std::numeric_limits<std::uint32_t>::max() - sy ||
        building.anchor_z > std::numeric_limits<std::uint32_t>::max() - sz) return std::nullopt;

    // Moving both the cells and the anchor keeps every world position the same.
    // Check the whole move first, so a refused placement can't half move it.
    for (auto& cell : building.cells) {
        cell.x += sx;
        cell.y += sy;
        cell.z += sz;
    }
    building.anchor_x += sx;
    building.anchor_y += sy;
    building.anchor_z += sz;
    return std::array<std::uint32_t, 3>{static_cast<std::uint32_t>(x + sx),
        static_cast<std::uint32_t>(y + sy), static_cast<std::uint32_t>(z + sz)};
}

float fly_speed_multiplier(bool slow, bool fast) {
    if (slow) {
        return fast ? 1.0F / 16.0F : 0.25F;
    }
    return fast ? 4.0F : 1.0F;
}

Vec3 fly_camera_step(Vec3 forward, const FlyInput& input, float speed, float seconds) {
    const auto length = [](Vec3 vector) {
        return std::sqrt(vector.x * vector.x + vector.y * vector.y + vector.z * vector.z);
    };
    const auto forward_length = length(forward);
    if (forward_length <= 0.0F) {
        return {};
    }
    const Vec3 direction{forward.x / forward_length, forward.y / forward_length, forward.z / forward_length};
    // Level right (forward x up), so moving sideways never climbs or sinks.
    Vec3 right{-direction.z, 0.0F, direction.x};
    const auto right_length = length(right);
    if (right_length > 1e-6F) {
        right = {right.x / right_length, 0.0F, right.z / right_length};
    } else {
        right = {1.0F, 0.0F, 0.0F};
    }
    const auto axis = [](bool plus, bool minus) {
        return (plus ? 1.0F : 0.0F) - (minus ? 1.0F : 0.0F);
    };
    const auto along = axis(input.forward, input.back);
    const auto side = axis(input.right, input.left);
    const auto lift = axis(input.up, input.down);
    const Vec3 move{direction.x * along + right.x * side, direction.y * along + lift, direction.z * along + right.z * side};
    const auto move_length = length(move);
    if (move_length <= 1e-6F) {
        return {};
    }
    const auto step = speed * fly_speed_multiplier(input.slow, input.fast) * seconds / move_length;
    return {move.x * step, move.y * step, move.z * step};
}

} // namespace pafta
