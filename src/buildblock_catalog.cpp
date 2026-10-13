// SPDX-License-Identifier: GPL-2.0-only
#include "buildblock_catalog.hpp"

#include <array>
#include <cmath>
#include <cstdint>

// Port of the game's built-in buildblock template catalog
// (Buildblock_InitTemplateMeshCatalog @0x10110, subrosa.exe THIS_ONE).
//
// The game builds g_buildblockTemplateCatalog @0x05098c00 in four steps:
//   1. give each named slot global the next id in turn,
//   2. run geometry phases that fill global scratch buffers
//      (DAT_110ab760 vertex count, DAT_112db764 quad count, position, normal, and
//      uv arrays) through AddBezierPatch / AddBoxFaces / _block_addbeam /
//      _block_addbasicsurface,
//   3. bake the scratch into catalog[id] (BakeScratchEntry @0x17ca0), sorting faces
//      by axis and working out UVs for each orientation,
//   4. make 4 copies of each entry turned 90 degrees about Y (RotateEntryY90
//      @0x4d170).
//
// This port follows the same phases in the same order, so every id ends up exactly
// where the game puts it (id 20 = railing, and so on). Collision and triangle data
// are NOT kept: the editor's renderer only uses quad faces (SblBlockMesh).
//
// Slot ids follow the recovered prologue: ids 0..15 are curbs, 16..19 the first
// floor patch, and 20..23 railings. assign_slot_ids works out the other group ids
// instead of copying them into a second id table.

namespace pafta {
namespace {

// The game's quad orientation codes (which axis the face normal points along, and
// which way). They go in the high bits of a quad's material, and the bake uses them
// to work out UVs.
constexpr std::uint32_t kOrientPosX = 0x10000;
constexpr std::uint32_t kOrientNegX = 0x20000;
constexpr std::uint32_t kOrientPosY = 0x30000;  // top
constexpr std::uint32_t kOrientNegY = 0x40000;  // bottom
constexpr std::uint32_t kOrientPosZ = 0x50000;
constexpr std::uint32_t kOrientNegZ = 0x60000;
constexpr std::uint32_t kOrientMask = 0xf0000;

// ---------------------------------------------------------------------------
// Tables copied exactly from subrosa.exe.
//
// Cube corners DAT_002b34f0 (8 corners of 3 floats each), read from the raw bytes:
// the bottom ring (y=0), then the top ring (y=1), each going (0,0)->(1,0)->(1,1)->
// (0,1) in x,z. So face 0 of DAT_002b3660 is the bottom and face 5 the top, which
// matches the normal table DAT_002b3550.
//
// Edge and face table DAT_002b3660 (6 faces x 4 corner indices). _block_addbeam
// @0x19940 walks it through piVar9, starting at &DAT_002b3664 and reading
// [-1],[0],[1],[2] in steps of 4 ints, so face f reads the four ints at
// 0x002b3660 + f*0x10. Checked against the raw bytes at 0x002b3660.
// ---------------------------------------------------------------------------
constexpr Vec3f kCubeCorner[8] = {
    {0, 0, 0},  // C0
    {1, 0, 0},  // C1
    {1, 0, 1},  // C2
    {0, 0, 1},  // C3
    {0, 1, 0},  // C4
    {1, 1, 0},  // C5
    {1, 1, 1},  // C6
    {0, 1, 1},  // C7
};

// Face -> 4 corner indices, exactly as in DAT_002b3660.
constexpr int kFaceCorners[6][4] = {
    {3, 2, 1, 0},  // F0
    {7, 6, 2, 3},  // F1
    {4, 7, 3, 0},  // F2
    {5, 4, 0, 1},  // F3
    {6, 5, 1, 2},  // F4
    {4, 5, 6, 7},  // F5
};

// The orientation code for each face, indexed by _block_addbeam's loop counter (the
// same index that picks the face above). It tags how UVs and materials are
// projected; it is NOT the geometric normal. The bake works out UVs from it.
// Checked against the iVar7 == 0..5 chain in _block_addbeam @0x19940.
constexpr std::uint32_t kBeamFaceOrient[6] = {
    0x30000,  // face 0
    0x60000,  // face 1
    0x10000,  // face 2
    0x50000,  // face 3
    0x20000,  // face 4
    0x40000,  // face 5
};

// ---------------------------------------------------------------------------
// Scratch mesh: the vertex and quad buffers the generators write into. Quads hold
// 4 vertex indices and a packed material (with the orientation in the high bits).
//
// The game's scratch also keeps a normal per vertex, but the editor's renderer works
// out face normals again from each quad. So only the positions and the quad's
// orientation (which sets UVs and material) need to survive the bake.
// ---------------------------------------------------------------------------
struct ScratchMesh {
    std::vector<Vec3f> pos;
    std::vector<Vec2f> uv;  // stored UV for each vertex (used when the orient nibble is 0)
    struct Quad {
        std::array<int, 4> v{};
        std::uint32_t material{};
    };
    std::vector<Quad> quads;

    void clear() {
        pos.clear();
        uv.clear();
        quads.clear();
    }
    int add_vertex(const Vec3f& p, const Vec2f& t = {}) {
        const int id = static_cast<int>(pos.size());
        pos.push_back(p);
        uv.push_back(t);
        return id;
    }
};

// _block_addbeam @0x19940: a box of size (sx,sy,sz) at origin (ox,oy,oz), with
// faces turned on by face_mask. The loop counter goes over all 6 faces, and for each
// bit that is on, it makes a quad. The corners come from kFaceCorners (scaled and
// moved by the cube corner table), and the UV/material projection tag comes from
// kBeamFaceOrient.
//
// Material: the game reads materials[face] (moving on for each face it makes) and
// turns it by param_1 (the beam's rotation, which is always 0 for the built-ins
// here, so turning does nothing). materials has 6 entries. Pass {0,0,0,0,0,0} for
// built-ins with no per-face texture, or a filled list for the textured base slab.
void add_beam(ScratchMesh& m, float ox, float oy, float oz, float sx, float sy, float sz,
              std::uint32_t face_mask, const std::array<std::uint32_t, 6>& materials) {
    for (int f = 0; f < 6; ++f) {
        if ((face_mask & (1u << f)) == 0) {
            continue;
        }
        Vec3f corner[4];
        for (int k = 0; k < 4; ++k) {
            const Vec3f& cc = kCubeCorner[kFaceCorners[f][k]];
            corner[k] = {sx * cc.x + ox, sy * cc.y + oy, sz * cc.z + oz};
        }
        const std::uint32_t orient = kBeamFaceOrient[f];
        const int i0 = m.add_vertex(corner[0]);
        const int i1 = m.add_vertex(corner[1]);
        const int i2 = m.add_vertex(corner[2]);
        const int i3 = m.add_vertex(corner[3]);
        m.quads.push_back({{i0, i1, i2, i3}, (materials[static_cast<std::size_t>(f)] & 0xfffU) | orient});
    }
}

// The game's floor patch uses AddBezierPatch(...,1,1,8,3,0x30000). Its control mesh
// has not been found, so slots 16..19 use a flat plate. Other catalog patches are a
// single quad and only need the corners of their control grid.

// add_quad_patch: the AddBezierPatch(...,1,1,1,1,orient) case. With order 1, the
// 4x4 control grid shrinks to its 4 corners and makes one quad. UVs are the stored
// corner UVs (the bake keeps them when the orient nibble is 0, and replaces them by
// projection otherwise).
void add_quad_patch(ScratchMesh& m, const Vec3f& p0, const Vec3f& p1, const Vec3f& p2,
                    const Vec3f& p3, const Vec2f& t0, const Vec2f& t1, const Vec2f& t2,
                    const Vec2f& t3, std::uint32_t orient) {
    const int i0 = m.add_vertex(p0, t0);
    const int i1 = m.add_vertex(p1, t1);
    const int i2 = m.add_vertex(p2, t2);
    const int i3 = m.add_vertex(p3, t3);
    m.quads.push_back({{i0, i1, i2, i3}, orient});
}

// add_box_faces: BuildblockMeshBuilder_AddBoxFaces @0x19c10. Makes one quad for
// each face that is turned on, from an explicit 8-corner box, and picks each face's
// orientation from its normal (the -0.707/+0.707 limits DAT_00278be4 /
// DAT_0027838c). Corners use the same kFaceCorners/kCubeCorner tables.
constexpr float kNormLo = -0.70709884F;  // DAT_00278be4
constexpr float kNormHi = 0.70709884F;   // DAT_0027838c

std::uint32_t classify_orient(const Vec3f& n) {
    // The game's winding makes the X and Z tags the opposite sign of the normal.
    if (n.y > kNormHi) return kOrientPosY;
    if (n.y < kNormLo) return kOrientNegY;
    if (n.x > kNormHi) return kOrientNegX;
    if (n.x < kNormLo) return kOrientPosX;
    return n.z > kNormHi ? kOrientNegZ : kOrientPosZ;
}

void add_box_faces(ScratchMesh& m, const std::array<Vec3f, 8>& corners, std::uint32_t material,
                   std::uint32_t face_mask) {
    for (int f = 0; f < 6; ++f) {
        if ((face_mask & (1u << f)) == 0) {
            continue;
        }
        const Vec3f& a = corners[static_cast<std::size_t>(kFaceCorners[f][0])];
        const Vec3f& b = corners[static_cast<std::size_t>(kFaceCorners[f][1])];
        const Vec3f& c = corners[static_cast<std::size_t>(kFaceCorners[f][2])];
        const Vec3f& d = corners[static_cast<std::size_t>(kFaceCorners[f][3])];
        // Face normal from the first three corners (the same as the game's cross
        // product of edges (b-a)x(c-a), normalized).
        const Vec3f e1{b.x - a.x, b.y - a.y, b.z - a.z};
        const Vec3f e2{c.x - a.x, c.y - a.y, c.z - a.z};
        Vec3f n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
        const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (len != 0.0F) {
            n = {n.x / len, n.y / len, n.z / len};
        }
        const std::uint32_t orient = classify_orient(n);
        const int i0 = m.add_vertex(a);
        const int i1 = m.add_vertex(b);
        const int i2 = m.add_vertex(c);
        const int i3 = m.add_vertex(d);
        m.quads.push_back({{i0, i1, i2, i3}, (material & 0xfffU) | orient});
    }
}

// BakeScratchEntry @0x17ca0: turns the scratch, whose vertices are shared, into the
// per-face SblBlockMesh the editor's renderer uses. Works out UVs from each quad's
// orientation, keeps only the low 12 bits of materials, and records the size.
BuildblockCatalogEntry bake_entry(const ScratchMesh& m, int sx, int sy, int sz) {
    BuildblockCatalogEntry entry;
    entry.size_x = sx;
    entry.size_y = sy;
    entry.size_z = sz;
    entry.mesh.width = static_cast<std::uint32_t>(sx);
    entry.mesh.height = static_cast<std::uint32_t>(sy);
    entry.mesh.depth = static_cast<std::uint32_t>(sz);
    entry.mesh.faces.reserve(m.quads.size());
    for (const auto& q : m.quads) {
        SblBlockFace face;
        face.material = q.material & 0xfffU;
        face.orientation = q.material & kOrientMask;
        face.authored = true;
        face.vertices.reserve(4);
        face.uvs.reserve(4);
        for (int k = 0; k < 4; ++k) {
            const auto vi = static_cast<std::size_t>(q.v[k]);
            const Vec3f& p = m.pos[vi];
            face.vertices.push_back(p);
            // BakeScratchEntry replaces UVs with a projection of the position only
            // when the quad's orientation nibble is set (0x10000..0x60000).
            // Orientation 0 keeps the UV that came from the Bezier control grid.
            face.uvs.push_back(native_oriented_uv(p, m.uv[vi], q.material));
        }
        entry.mesh.faces.push_back(std::move(face));
    }
    entry.mesh.face_count = static_cast<std::uint32_t>(entry.mesh.faces.size());
    return entry;
}

// RotateEntryY90 @0x4d170: makes a copy of an entry turned 90 degrees about Y.
// Swaps the x and z sizes, moves vertex (x,z) to (size_x' - z, x), and works out UVs
// again by baking the turned geometry. (The game turns the orientation codes;
// working out UVs again from the turned positions gives the same result for this
// unit-sized geometry.)
BuildblockCatalogEntry rotate_entry_y90(const BuildblockCatalogEntry& src) {
    BuildblockCatalogEntry dst = src;
    dst.size_x = src.size_z;
    dst.size_z = src.size_x;
    dst.mesh.width = static_cast<std::uint32_t>(dst.size_x);
    dst.mesh.depth = static_cast<std::uint32_t>(dst.size_z);
    const float new_x_extent = static_cast<float>(src.size_z);
    for (auto& face : dst.mesh.faces) {
        // Turn the orientation code through the game's side-face cycle. Top and
        // bottom (0x30000/0x40000) and orient-0 (stored UV) faces keep their axis.
        std::uint32_t orient = face.orientation;
        switch (orient) {
            case kOrientNegZ: orient = kOrientPosX; break;  // 0x60000 -> 0x10000
            case kOrientPosX: orient = kOrientPosZ; break;  // 0x10000 -> 0x50000
            case kOrientPosZ: orient = kOrientNegX; break;  // 0x50000 -> 0x20000
            case kOrientNegX: orient = kOrientNegZ; break;  // 0x20000 -> 0x60000
            default: break;                                 // top, bottom, and stored-UV faces keep their axis
        }
        face.orientation = orient;
        face.material = (face.material & 0xfffU) | (orient & kOrientMask);
        const bool has_orient = (orient & kOrientMask) != 0;
        for (std::size_t k = 0; k < face.vertices.size(); ++k) {
            Vec3f& p = face.vertices[k];
            const float nx = new_x_extent - p.z;
            const float nz = p.x;
            p.x = nx;
            p.z = nz;
            // Faces with projected UVs work them out again from the turned geometry.
            // Faces with stored UVs (orient 0) keep their UV unchanged through the
            // turn, like the game's bake (which only replaces UVs when the
            // orientation nibble is set).
            if (has_orient) {
                face.uvs[k] = native_oriented_uv(p, face.uvs[k], orient);
            }
        }
    }
    return dst;
}

// Bakes the scratch into catalog[base_id], then writes its 3 turned copies into the
// next three ids (the game bakes id, then RotateEntryY90 + Bake for id+1, id+2, and
// id+3, using slot globals in a row, which are ids in a row for every 4-rotation
// group in the driver).
void emit_rot_group(std::vector<BuildblockCatalogEntry>& out, ScratchMesh& m, int base_id, int sx,
                    int sy, int sz) {
    BuildblockCatalogEntry base = bake_entry(m, sx, sy, sz);
    BuildblockCatalogEntry r1 = rotate_entry_y90(base);
    BuildblockCatalogEntry r2 = rotate_entry_y90(r1);
    BuildblockCatalogEntry r3 = rotate_entry_y90(r2);
    out[static_cast<std::size_t>(base_id)] = std::move(base);
    out[static_cast<std::size_t>(base_id) + 1] = std::move(r1);
    out[static_cast<std::size_t>(base_id) + 2] = std::move(r2);
    out[static_cast<std::size_t>(base_id) + 3] = std::move(r3);
    m.clear();
}

// Bakes the scratch into one catalog[id] (no rotation group).
void emit_single(std::vector<BuildblockCatalogEntry>& out, ScratchMesh& m, int id, int sx, int sy,
                 int sz) {
    out[static_cast<std::size_t>(id)] = bake_entry(m, sx, sy, sz);
    m.clear();
}

// The usual unit-quad UVs (the curb and floor patches set a (0,0)-(1,1) UV control
// block, and with orient nibble 0 the bake keeps them).
constexpr Vec2f kUV00{0.0F, 0.0F};
constexpr Vec2f kUV10{1.0F, 0.0F};
constexpr Vec2f kUV11{1.0F, 1.0F};
constexpr Vec2f kUV01{0.0F, 1.0F};

// --- Phase: curb and edge family (ids 0..15) -------------------------------
// The game's curb loop (piVar11 = &DAT_0500a70c, uVar9 = 0..15) makes a flat top
// plate at y=0.0625 covering the cell (material 5), plus up to four upright edge
// skirts down to y=0, turned on by the 4 bits of the variant number uVar9:
//   bit0 (&1): the z=0 (-Z) edge      bit3 (&8): the x=0 (-X) edge
//   bit2 (&4): the z=1 (+Z) edge      bit1 (&2): the x=1 (+X) edge
// Control points copied exactly from Buildblock_InitTemplateMeshCatalog.
// Edges use material 0 (orient nibble 0, so the stored UVs are kept).
void build_curb_family(std::vector<BuildblockCatalogEntry>& out) {
    constexpr float kT = 0.0625F;  // top plate height
    for (std::uint32_t variant = 0; variant < 16; ++variant) {
        ScratchMesh m;
        // Top plate (material 5).
        add_quad_patch(m, {0, kT, 0}, {1, kT, 0}, {1, kT, 1}, {0, kT, 1}, kUV00, kUV10, kUV11,
                       kUV01, 5);
        // -Z edge (bit0).
        if (variant & 1U) {
            add_quad_patch(m, {1, kT, 0}, {0, kT, 0}, {0, 0, 0}, {1, 0, 0}, kUV00, kUV10, kUV11,
                           kUV01, 0);
        }
        // -X edge (bit3).
        if (variant & 8U) {
            add_quad_patch(m, {0, kT, 0}, {0, kT, 1}, {0, 0, 1}, {0, 0, 0}, kUV00, kUV10, kUV11,
                           kUV01, 0);
        }
        // +Z edge (bit2).
        if (variant & 4U) {
            add_quad_patch(m, {0, kT, 1}, {1, kT, 1}, {1, 0, 1}, {0, 0, 1}, kUV00, kUV10, kUV11,
                           kUV01, 0);
        }
        // +X edge (bit1).
        if (variant & 2U) {
            add_quad_patch(m, {1, kT, 1}, {1, kT, 0}, {1, 0, 0}, {1, 0, 1}, kUV00, kUV10, kUV11,
                           kUV01, 0);
        }
        out[variant] = bake_entry(m, 1, 1, 1);
    }
}

// --- Phase: floor patch (ids 16..19) ---------------------------------------
// The game makes one AddBezierPatch(...,1,1,8,3,0x30000) here: a curved top surface
// of order 3, split 8x1 (the rounded floor or ramp). It then bakes it plus three
// turned copies. The exact bicubic control mesh is spread over several globals and
// has not been copied out. Until then, keep the flat top plate at y=0.0625 facing
// +Y; its shape has not been checked against the game's mesh.
void build_floor_patch(std::vector<BuildblockCatalogEntry>& out, int base_id) {
    constexpr float kT = 0.0625F;
    ScratchMesh m;
    add_quad_patch(m, {0, kT, 0}, {1, kT, 0}, {1, kT, 1}, {0, kT, 1}, kUV00, kUV10, kUV11, kUV01,
                   0x30000);
    emit_rot_group(out, m, base_id, 1, 1, 1);
}

// Corner heights of a slope tile's surface, in the order of the patch control
// corners P00 (x0,z0), P03 (x1,z0), P30 (x0,z1), P33 (x1,z1). The edge picks the
// direction (the slope loops in Buildblock_InitTemplateMeshCatalog): 0 rises along
// +X, 1 along +Z, 2 falls along +X, and 3 falls along +Z.
std::array<float, 4> grade_corner_heights(int edge, float low, float high) {
    switch (edge) {
        case 0: return {low, high, low, high};
        case 1: return {low, low, high, high};
        case 2: return {high, low, high, low};
        default: return {high, high, low, low};
    }
}

// --- How the prologue hands out ids ---------------------------------------
// The driver gives catalog ids to named slot globals in the order they appear in
// the code, then runs geometry phases that write catalog[slotId]. Instead of
// hardcoding ids, we replay that with a running counter, so every id lands exactly
// where the game puts it. Only slots that a geometry phase uses are recorded.
struct SlotIds {
    std::array<int, 16> curb{};         // a70c[0..15]   = ids 0..15
    int floor1_base = 0;                // a74c          = id 16 (+3 rot)
    int railing_base = 0;               // a75c          = id 20 (+3 rot)
    // Slope tiles: 9 slope levels (n = 4..12), each with an aa14 group and a ba14
    // group of 4 edge variants x n ids, indexed [grade][group][edge][step].
    // We store the flat id for each (grade, group, edge, step).
    std::array<std::array<std::array<std::vector<int>, 4>, 2>, 9> grade{};
    std::array<int, 4> floor2{};        // a7c4[0..3]
    int beam1_base = 0;                 // cb84          (+3 rot)
    int beam2_base = 0;                 // cb94          (+3 rot)
    int next = 0;                       // first free id after the prologue
};

// Replays how Buildblock_InitTemplateMeshCatalog's prologue hands out ids.
SlotIds assign_slot_ids() {
    SlotIds s;
    int count = 0;
    for (int i = 0; i < 16; ++i) {
        s.curb[static_cast<std::size_t>(i)] = count++;  // a70c[0..15] = 0..15
    }
    s.floor1_base = count;  // a74c = 16; a750/754/758 = 17/18/19
    count += 4;
    s.railing_base = count;  // a75c = 20; a760/764/768 = 21/22/23
    count += 4;
    // The driver then hands out a long run of reserved slots in a row
    // (a76c..a798, cad4..cb20, a79c..a7c0 = ids 24..65) with no built-in
    // geometry, so skip the counter past them.
    count = 66;  // where the slope tiles start (checked against the prologue's math)

    // The slope-tile loop: lVar29 (count per group) = 4, lVar26 (array offset) = 0x10,
    // repeated 9 times. Each time it fills aa14 and then ba14 for 4 edge variants of
    // (count) ids, and the count goes up by 1 for each slope level.
    int per = 4;             // ids per edge variant at this slope level
    for (int grade = 0; grade < 9; ++grade) {
        const int n = per;   // slope level n = 4..12
        for (int edge = 0; edge < 4; ++edge) {
            auto& v = s.grade[static_cast<std::size_t>(grade)][0][static_cast<std::size_t>(edge)];
            for (int step = 0; step < n; ++step) {
                v.push_back(count++);  // aa14 group
            }
        }
        for (int edge = 0; edge < 4; ++edge) {
            auto& v = s.grade[static_cast<std::size_t>(grade)][1][static_cast<std::size_t>(edge)];
            for (int step = 0; step < n; ++step) {
                v.push_back(count++);  // ba14 group
            }
        }
        ++per;
    }

    // DAT_0500a704 = rounded up to a multiple of 4; the floor2 and beam groups follow.
    count = (count + 3) & ~3;
    for (int i = 0; i < 4; ++i) {
        s.floor2[static_cast<std::size_t>(i)] = count++;  // a7c4[0..3]
    }
    // (The reserved ids cb24..cb80 are skipped; cb84 and cb94 are the next two beam groups.)
    count = (count + 0x2c);  // skip the reserved ids cb24..cb80
    s.beam1_base = count;
    count += 4;
    s.beam2_base = count;
    count += 4;
    s.next = count;
    return s;
}

// --- Phase: slope-tile family (sloped street tiles) ------------------------
// For slope level n (4..12), each of the n steps rises 1/n across the cell. The ba14
// group is the curb next to a sloped lane: a slab 1/16 thick whose top runs from
// k/n+1/16 to (k+1)/n+1/16 (AddBoxFaces, every face except the bottom, material 0),
// with size_y 2 once the top reaches the next cell. The aa14 group is the lane
// surface: one flat patch from k/n to (k+1)/n with stored UVs (x, z) and material 0,
// so it takes slot 0 of the cell's face array (the lane code).
void build_grade_tiles(std::vector<BuildblockCatalogEntry>& out, const SlotIds& s) {
    constexpr float kCurb = 0.0625F;  // fVar5 = FLOAT_002781bc
    for (int grade = 0; grade < 9; ++grade) {
        const int n = grade + 4;
        const float step = 1.0F / static_cast<float>(n);
        for (int edge = 0; edge < 4; ++edge) {
            const auto& ba = s.grade[static_cast<std::size_t>(grade)][1][static_cast<std::size_t>(edge)];
            for (int k = 0; k < static_cast<int>(ba.size()); ++k) {
                const float low = static_cast<float>(k) * step + kCurb;
                const auto top = grade_corner_heights(edge, low, low + step);
                // Box corners in kCubeCorner order: the bottom ring, then the top
                // ring, each (x0,z0) (x1,z0) (x1,z1) (x0,z1).
                const std::array<float, 4> ring{top[0], top[1], top[3], top[2]};
                std::array<Vec3f, 8> c{};
                for (int i = 0; i < 4; ++i) {
                    const float x = (i == 1 || i == 2) ? 1.0F : 0.0F;
                    const float z = (i >= 2) ? 1.0F : 0.0F;
                    c[static_cast<std::size_t>(i)] = {x, ring[static_cast<std::size_t>(i)] - kCurb, z};
                    c[static_cast<std::size_t>(i) + 4] = {x, ring[static_cast<std::size_t>(i)], z};
                }
                ScratchMesh m;
                add_box_faces(m, c, 0, 0x3e);
                emit_single(out, m, ba[static_cast<std::size_t>(k)], 1, low + step >= 1.0F ? 2 : 1, 1);
            }
            const auto& aa = s.grade[static_cast<std::size_t>(grade)][0][static_cast<std::size_t>(edge)];
            for (int k = 0; k < static_cast<int>(aa.size()); ++k) {
                const float low = static_cast<float>(k) * step;
                const auto h = grade_corner_heights(edge, low, low + step);
                ScratchMesh m;
                add_quad_patch(m, {0, h[0], 0}, {1, h[1], 0}, {1, h[3], 1}, {0, h[2], 1},
                               kUV00, kUV10, kUV11, kUV01, 0);
                emit_single(out, m, aa[static_cast<std::size_t>(k)], 1, 1, 1);
            }
        }
    }
}

// --- Phase: beam groups 1 and 2 (barrier and bench pieces) -----------------
// Arguments worked out from the disassembly (by following the xmm registers):
//   XMM12 = FLOAT_002783f4 = 1.0, XMM8 = FLOAT_002781bc = 0.0625,
//   XMM7  = 1.0 (= XMM12), XMM6 = DAT_002782b0 = 0.3125, XMM13 = 0.0.
// Group 1 (cb84):
//   addbeam(0,0,0,0, 1.0,    1.0,    1.0,    0x22, mat={1..})
//   addbeam(0,0,0,0, 1.0,    0.3125, 0.0625, 0x2a, mat={1..})
// Group 2 (cb94):
//   addbeam(0,0,0,0, 1.0,    1.0,    1.0,    0x32, mat={1..})
//   addbeam(0,0,0,0, 0.0625, 0.3125, 0.0625, 0x32, mat={1..})
// Each is baked at its slot id, then 3 turned copies.
void build_beam_groups_1_2(std::vector<BuildblockCatalogEntry>& out, const SlotIds& s) {
    constexpr std::array<std::uint32_t, 6> kMat{1, 1, 1, 1, 1, 1};  // _DAT_00278ee0
    {
        ScratchMesh m;
        add_beam(m, 0, 0, 0, 1.0F, 1.0F, 1.0F, 0x22, kMat);
        add_beam(m, 0, 0, 0, 1.0F, 0.3125F, 0.0625F, 0x2a, kMat);
        emit_rot_group(out, m, s.beam1_base, 1, 1, 1);
    }
    {
        ScratchMesh m;
        add_beam(m, 0, 0, 0, 1.0F, 1.0F, 1.0F, 0x32, kMat);
        add_beam(m, 0, 0, 0, 0.0625F, 0.3125F, 0.0625F, 0x32, kMat);
        emit_rot_group(out, m, s.beam2_base, 1, 1, 1);
    }
}

// --- Phase: floor patch 2 group (a7c4[0..3]) --------------------------------
// The game makes a flat Bezier quad (AddBezierPatch(...,1,1,1,1,0)) for each
// lVar29 = 0..3, each a unit top quad at y=0 turned by lVar29. Our best guess: a
// flat top quad with stored UVs (orient 0). The round map draws none.
void build_floor_patch_2(std::vector<BuildblockCatalogEntry>& out, const SlotIds& s) {
    for (int i = 0; i < 4; ++i) {
        ScratchMesh m;
        add_quad_patch(m, {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}, kUV00, kUV10, kUV11, kUV01, 0);
        emit_single(out, m, s.floor2[static_cast<std::size_t>(i)], 1, 1, 1);
    }
}

// --- Phase: third beam group (ids 20..23), the railing ---------------------
// Checked against Buildblock_InitTemplateMeshCatalog @0x10110, with the exact float
// constants read from subrosa.exe (fVar6=FLOAT_002783f4=1.0,
// fVar5=FLOAT_002781bc=0.0625, fVar33=DAT_002782b0=0.3125):
//   _block_addbeam(0, 0,   0,    0,      1.0,    0.0625, 1.0,   0x2a, mat={1..})
//   _block_addbeam(0, 0.5, 0,    0.5,    0.0625, 0.3125, 0.0625,0x3f, mat={0..})
//   _block_addbeam(0, 0,   0.25, 0.4375, 1.0,    0.1875, 0.125, 0x3f, mat={0..})
// The base slab's material list is _DAT_00278ee0 = {1,1,1,1,1,1}; the post and bar
// use zeros.
void build_railing_group(std::vector<BuildblockCatalogEntry>& out, const SlotIds& s) {
    constexpr float kFull = 1.0F;
    constexpr float kS = 0.0625F;
    constexpr float kPostH = 0.3125F;
    constexpr float kPostOff = 0.5F;
    constexpr float kBarY = 0.25F;
    constexpr float kBarZ = 0.4375F;
    constexpr float kBarSy = 0.1875F;
    constexpr float kBarSz = 0.125F;

    constexpr std::array<std::uint32_t, 6> kSlabMat{1, 1, 1, 1, 1, 1};  // _DAT_00278ee0
    constexpr std::array<std::uint32_t, 6> kZeroMat{0, 0, 0, 0, 0, 0};

    ScratchMesh m;
    add_beam(m, 0, 0, 0, kFull, kS, kFull, 0x2a, kSlabMat);
    add_beam(m, kPostOff, 0, kPostOff, kS, kPostH, kS, 0x3f, kZeroMat);
    add_beam(m, 0, kBarY, kBarZ, kFull, kBarSy, kBarSz, 0x3f, kZeroMat);
    emit_rot_group(out, m, s.railing_base, 1, 1, 1);  // ids 20..23
}

}  // namespace

std::array<std::int32_t, 2048> native_grade_slot_table() {
    // DAT_0500aa14 row r = 4n + edge holds 16 ids: 16 ints per row, with rows at
    // int offset n*64 + edge*16. ba14 is the next 0x1000 bytes.
    std::array<std::int32_t, 2048> table{};
    const SlotIds slots = assign_slot_ids();
    for (int grade = 0; grade < 9; ++grade) {
        const int n = grade + 4;
        for (int group = 0; group < 2; ++group) {
            for (int edge = 0; edge < 4; ++edge) {
                const auto& ids = slots.grade[static_cast<std::size_t>(grade)][static_cast<std::size_t>(group)][static_cast<std::size_t>(edge)];
                for (std::size_t step = 0; step < ids.size(); ++step) {
                    table[static_cast<std::size_t>(group * 1024 + n * 64 + edge * 16) + step] = ids[step];
                }
            }
        }
    }
    return table;
}

std::vector<BuildblockCatalogEntry> build_native_builtin_catalog() {
    // A close port of Buildblock_InitTemplateMeshCatalog. It replays how the prologue
    // hands out ids (assign_slot_ids), so every built-in id lands at its real catalog
    // index, then runs each geometry phase, which writes catalog[slotId]:
    //   ids   0..15 = curb and edge family        (build_curb_family)
    //   ids  16..19 = floor patch + 3 turns       (build_floor_patch)
    //   ids  20..23 = railing beam + 3 turns      (build_railing_group)
    //   ids  66..   = slope-tile ramp family      (build_grade_tiles)
    //   beam groups 1 and 2 and floor patch 2, at their worked-out ids.
    // The reserved, runtime, garage, and CSX slots (ids 24..65 and the CSX 0x585a0001
    // additions) have no built-in geometry. Those indices are empty slots that draw
    // nothing, just like the game's loop does for a catalog slot that was never built.
    const SlotIds slots = assign_slot_ids();
    std::vector<BuildblockCatalogEntry> out;
    out.resize(static_cast<std::size_t>(slots.next));
    build_curb_family(out);             // ids 0..15
    build_floor_patch(out, slots.floor1_base); // ids 16..19
    build_railing_group(out, slots);    // ids 20..23
    build_grade_tiles(out, slots);      // slope ramp family
    build_beam_groups_1_2(out, slots);  // barrier/bench beams
    build_floor_patch_2(out, slots);    // floor-patch-2 group
    return out;
}

}  // namespace pafta
