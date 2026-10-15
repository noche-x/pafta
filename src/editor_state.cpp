// SPDX-License-Identifier: GPL-2.0-only
#include "editor_state.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
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

float fly_speed_multiplier(bool slow, bool fast, bool alex_controls) {
    if (alex_controls && fast) return slow ? 16.0F : 4.0F;
    if (slow) {
        return fast ? 1.0F / 16.0F : 0.25F;
    }
    return fast ? 4.0F : 1.0F;
}

Vec3 fly_camera_step(Vec3 forward, const FlyInput& input, float speed, float seconds) {
    const auto length = [](Vec3 vector) {
        return std::sqrt(vector.x * vector.x + vector.y * vector.y + vector.z * vector.z);
    };
    // The native editor moves WASD in its horizontal plane. Blender-style fly
    // follows the view instead, so looking up and pressing W climbs.
    if (input.alex_controls) forward.y = 0.0F;
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
    const auto step = speed * fly_speed_multiplier(input.slow, input.fast, input.alex_controls) * seconds / move_length;
    return {move.x * step, move.y * step, move.z * step};
}

std::optional<Vec3> ray_horizontal_plane_hit(Vec3 origin, Vec3 direction, float y) {
    if (std::abs(direction.y) < 0.0001F) return std::nullopt;
    const auto distance = (y - origin.y) / direction.y;
    if (distance < 0.0F) return std::nullopt;
    return Vec3{origin.x + direction.x * distance, y, origin.z + direction.z * distance};
}

std::vector<GridCell> brush_box_cells(GridCell a, GridCell b) {
    const auto x1 = std::clamp(b[0], a[0] - kMaxBrushSide + 1, a[0] + kMaxBrushSide - 1);
    const auto z1 = std::clamp(b[2], a[2] - kMaxBrushSide + 1, a[2] + kMaxBrushSide - 1);
    std::vector<GridCell> cells;
    for (auto x = std::min(a[0], x1); x <= std::max(a[0], x1); ++x)
        for (auto z = std::min(a[2], z1); z <= std::max(a[2], z1); ++z) cells.push_back({x, a[1], z});
    return cells;
}

std::vector<GridCell> brush_disc_cells(GridCell center, int radius) {
    radius = std::clamp(radius, 0, kMaxBrushSide / 2);
    std::vector<GridCell> cells;
    for (int dx = -radius; dx <= radius; ++dx)
        for (int dz = -radius; dz <= radius; ++dz)
            // + radius keeps the rim cells a circle drawn at that radius mostly covers.
            if (dx * dx + dz * dz <= radius * radius + radius) cells.push_back({center[0] + dx, center[1], center[2] + dz});
    return cells;
}

std::vector<std::size_t> brush_face_loop(const std::vector<BrushFace>& faces, std::size_t start) {
    if (start >= faces.size()) return {};
    const auto kind = [](std::uint32_t slot) {
        const auto face = slot % 8U;
        return (slot >= 8U ? 16U : 0U) + (face >= 1U && face <= 4U ? 1U : face);
    };
    const auto level = faces[start].cell[1];
    const auto wanted = kind(faces[start].slot);
    std::map<GridCell, std::vector<std::size_t>> by_cell;
    for (std::size_t i = 0; i < faces.size(); ++i)
        if (faces[i].cell[1] == level && kind(faces[i].slot) == wanted) by_cell[faces[i].cell].push_back(i);
    std::set<GridCell> seen{faces[start].cell};
    std::vector<GridCell> todo{faces[start].cell};
    std::vector<std::size_t> loop;
    while (!todo.empty()) {
        const auto cell = todo.back();
        todo.pop_back();
        const auto& here = by_cell[cell];
        loop.insert(loop.end(), here.begin(), here.end());
        for (int dx = -1; dx <= 1; ++dx)
            for (int dz = -1; dz <= 1; ++dz) {
                const GridCell next{cell[0] + dx, level, cell[2] + dz};
                if (by_cell.contains(next) && seen.insert(next).second) todo.push_back(next);
            }
    }
    std::ranges::sort(loop);
    return loop;
}

} // namespace pafta
