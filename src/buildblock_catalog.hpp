#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "formats.hpp"

// Port of the native procedural buildblock template catalog built by
// Buildblock_InitTemplateMeshCatalog @0x10110 (subrosa.exe). v10 district piece
// words index this catalog by (word & 0xffff); the round map's railings/curbs are
// procedural built-ins generated here, NOT loose buildblock/*.sbl files.
//
// The native catalog is a sequence of procedural built-ins (curbs, road edges,
// railings, ramps, beams) generated via bezier patches / beams / box faces and
// baked in 4 Y rotations, then CSX 0x585a0001 buildblocks appended.
//
// The round map (the only v10 SBC in this workspace) references exactly two
// catalog indices: 0 (_null, an empty slot that renders nothing) and 20 (the
// railing, the third procedural beam group). A direct replay of round's portal
// district words confirms this — there is no third index to support. So this
// port emits ids 0..19 as empty slots and ids 20..23 as the railing + its three
// Y rotations, matching the native catalog exactly for every index round can ask
// for. Adding the curb/edge bicubic family, grade tiles, and floor patches would
// be dead code for round; they can be ported if a v10 map that uses them appears.
namespace map_editor {

// One baked catalog entry: a renderable block mesh plus its native cell size.
struct BuildblockCatalogEntry {
    std::int32_t size_x{1};
    std::int32_t size_y{1};
    std::int32_t size_z{1};
    SblBlockMesh mesh;
    std::string name;  // empty for procedural built-ins
};

// Builds the full procedural built-in catalog (indices 0..N-1) in native order,
// matching Buildblock_InitTemplateMeshCatalog. CSX buildblocks are appended by the
// caller (they carry their own SBL geometry).
std::vector<BuildblockCatalogEntry> build_native_builtin_catalog();

} // namespace map_editor
