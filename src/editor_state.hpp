// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "formats.hpp"
#include "scene.hpp"

#include <array>
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

// Brush shapes for the Add Block, Paint, and Piece tools, in grid cells (x, y, z).
using GridCell = std::array<std::int32_t, 3>;
// ponytail: 64 cells per side is one building grid; the city's piece writes are linear per cell.
constexpr std::int32_t kMaxBrushSide = 64;
// The rectangle between a and b on a's level. Each side stops kMaxBrushSide cells from a.
std::vector<GridCell> brush_box_cells(GridCell a, GridCell b);
// The cells on center's level whose centres are within radius cells of center's centre.
std::vector<GridCell> brush_disc_cells(GridCell center, int radius);

// A drawn building face for the Loop paint brush: its cell and texture slot.
struct BrushFace {
    GridCell cell{};
    std::uint32_t slot{};
};
// The faces a Blender-style loop from faces[start] paints: faces on its level, in its
// layer (outside slots 0-7 or inside 8-15), and of its kind, reached cell to cell
// through neighbours (diagonals too, so the loop turns corners). The four wall slots
// are one kind, so a loop runs all the way round a floor.
std::vector<std::size_t> brush_face_loop(const std::vector<BrushFace>& faces, std::size_t start);

} // namespace pafta
