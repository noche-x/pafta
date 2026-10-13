// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "formats.hpp"

// Port of the game's built-in buildblock template catalog, made by
// Buildblock_InitTemplateMeshCatalog @0x10110 (subrosa.exe). v10 district piece
// words point into this catalog with (word & 0xffff). The round map's railings and
// curbs are built-ins made here, NOT loose buildblock/*.sbl files.
//
// The game's catalog holds built-in curbs, road edges, railings, ramps, and beams
// made from patches and boxes. This port makes those families in the game's slot
// order. Reserved slots stay empty. The curved floor patch at ids 16..19 is still a
// flat plate, because its control mesh is missing.
namespace pafta {

// One baked catalog entry: a block mesh that can be drawn, plus its size in cells.
struct BuildblockCatalogEntry {
    std::int32_t size_x{1};
    std::int32_t size_y{1};
    std::int32_t size_z{1};
    SblBlockMesh mesh;
};

// Builds the supported built-in catalog in the game's slot order. See the flat floor
// note above: putting things in the right slot doesn't mean the shapes match.
std::vector<BuildblockCatalogEntry> build_native_builtin_catalog();

// The street slope lookup tables as they sit in memory from DAT_0500aa14. Ints
// [0, 1024) are the lane surface table (aa14) and [1024, 2048) the side curb table
// (ba14). Entries the catalog never fills stay 0, like the zeroed globals.
// MapConnector_PlaceStreetSegmentTilesAndSlopes reads them from four starting
// points: aa14 (+0), aa54 (+16), aa90 (+31), aad0 (+47), and the same for ba.
std::array<std::int32_t, 2048> native_grade_slot_table();

// DAT_0500a70c[15]: the curb block (a top plate with all four skirts) that streets
// and intersection edges place one level above the road.
inline constexpr std::uint32_t native_curb_block_id = 15U;

} // namespace pafta
