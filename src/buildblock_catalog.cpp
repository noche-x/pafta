#include "buildblock_catalog.hpp"

#include <array>
#include <cmath>
#include <cstdint>

// Faithful port of the native procedural buildblock template catalog
// (Buildblock_InitTemplateMeshCatalog @0x10110, subrosa.exe THIS_ONE).
//
// The native code builds g_buildblockTemplateCatalog @0x05098c00 by:
//   1. assigning sequential ids to named slot globals,
//   2. running geometry phases that fill global scratch buffers
//      (DAT_110ab760 vertex count, DAT_112db764 quad count, position/normal/uv
//      arrays) via AddBezierPatch / AddBoxFaces / _block_addbeam /
//      _block_addbasicsurface,
//   3. baking the scratch into catalog[id] (BakeScratchEntry @0x17ca0),
//      classifying faces by axis and deriving per-orientation UVs,
//   4. emitting 4 Y90 rotations of each entry (RotateEntryY90 @0x4d170).
//
// This port mirrors that flow phase-for-phase so the index assignment falls out
// exactly as the game computes it (id20 = railing, etc.). Collision/tri data is
// NOT serialized: the editor renderer consumes only quad faces (SblBlockMesh).
//
// Verified id map (corrected prologue emulation): ids 0-15 = curb/edge family;
// id16 = first floor patch (+3 rot = 16..19); id20 = third beam group base
// (+3 rot = 20..23) = the railing. Beam group bases: id81, id85, id20.

namespace map_editor {
namespace {

constexpr float kOne = 1.0F;

// Native quad-orientation codes (axis/sign of the face normal), used as the
// high bits of a quad material and consumed by the bake's UV derivation.
constexpr std::uint32_t kOrientPosX = 0x10000;
constexpr std::uint32_t kOrientNegX = 0x20000;
constexpr std::uint32_t kOrientPosY = 0x30000;  // top
constexpr std::uint32_t kOrientNegY = 0x40000;  // bottom
constexpr std::uint32_t kOrientPosZ = 0x50000;
constexpr std::uint32_t kOrientNegZ = 0x60000;
constexpr std::uint32_t kOrientMask = 0xf0000;

// NOTE: the curb/edge/grade phases (ids 0-15, grade tiles, floor patches) need
// the bicubic Bezier tessellator (Bezier_Evaluate1D @0x187180,
// Bezier_Evaluate1DDerivative @0x1872f0, AddBezierPatch @0x1f20). Those are
// fully decoded in the project memory but NOT yet emitted by this port. The beam
// family (railings/posts) below needs only the box primitives.

// ---------------------------------------------------------------------------
// Data tables read verbatim from subrosa.exe.
//
// Cube corners DAT_002b34f0 (8 corners, 3 floats each). The native ordering is
// the binary cube: corner i = (x = i>>2 & 1, y = i>>1 & 1, z = i & 1). Verified
// by reading the raw bytes at 0x002b34f0.
//
// Edge/face table DAT_002b3660 (6 faces x 4 corner indices). _block_addbeam
// @0x19940 walks it via piVar9 starting at &DAT_002b3664 and reading
// [-1],[0],[1],[2] with stride 4 ints, i.e. face f reads the four ints at
// 0x002b3660 + f*0x10. Verified by reading the raw bytes at 0x002b3660.
// ---------------------------------------------------------------------------
constexpr Vec3f kCubeCorner[8] = {
    {0, 0, 0},  // C0
    {0, 0, 1},  // C1
    {0, 1, 0},  // C2
    {0, 1, 1},  // C3
    {1, 0, 0},  // C4
    {1, 0, 1},  // C5
    {1, 1, 0},  // C6
    {1, 1, 1},  // C7
};

// Face -> 4 corner indices, matching DAT_002b3660 exactly.
constexpr int kFaceCorners[6][4] = {
    {3, 2, 1, 0},  // F0
    {7, 6, 2, 3},  // F1
    {4, 7, 3, 0},  // F2
    {5, 4, 0, 1},  // F3
    {6, 5, 1, 2},  // F4
    {4, 5, 6, 7},  // F5
};

// Per-face orientation code, indexed by _block_addbeam's loop counter (the same
// index that selects the face above). This is a UV/material projection tag, NOT
// the geometric normal — the bake derives UVs from it. Verified from the
// iVar7 == 0..5 chain in _block_addbeam @0x19940.
constexpr std::uint32_t kBeamFaceOrient[6] = {
    0x30000,  // face 0
    0x60000,  // face 1
    0x10000,  // face 2
    0x50000,  // face 3
    0x20000,  // face 4
    0x40000,  // face 5
};

// ---------------------------------------------------------------------------
// Scratch mesh: the vertex/quad buffers the generators write into. Quads hold 4
// vertex indices + a packed material (orientation in the high bits).
//
// The native scratch also tracks per-vertex normals, but the editor renderer
// recomputes face normals from the emitted quad, so only positions + the quad's
// orientation (which drives UVs and material) need to survive the bake.
// ---------------------------------------------------------------------------
struct ScratchMesh {
    std::vector<Vec3f> pos;
    std::vector<Vec2f> uv;  // authored UV per vertex (used when orient nibble == 0)
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
// faces gated by face_mask. The loop counter walks all 6 faces; for each enabled
// bit it emits a quad whose corners come from kFaceCorners (scaled+offset by the
// cube corner table) and whose UV/material projection tag is kBeamFaceOrient.
//
// Material: the native reads materials[face] (advancing per emitted face) and
// rotates it by param_1 (the beam rotation, always 0 for the built-ins here, so
// the rotate is a no-op). materials is a 6-entry array; pass {0,0,0,0,0,0} for
// the procedural built-ins that carry no per-face texture, or a filled array for
// the textured base slab.
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

// NOTE: the general bicubic tessellator (Bezier_Evaluate1D @0x187180, degree
// 1/2/3 with constants 3.0/6.0) is only exercised by the single tessellated
// floor-patch call AddBezierPatch(...,1,1,8,3,0x30000), whose exact control mesh
// lives in scattered globals. That patch's id (16) is approximated as a flat top
// plate below; until its control mesh is transcribed there is no caller for the
// curved evaluator, so it is intentionally not carried here. Every other catalog
// call uses order-1 tessellation, which collapses to a single quad (add_quad_patch).

// add_quad_patch: the AddBezierPatch(...,1,1,1,1,orient) case — order-1
// tessellation collapses the 4x4 control grid to its 4 corners, emitting one
// quad. UVs are the authored corner UVs (the bake keeps them when orient nibble
// is 0, overwrites them by projection otherwise).
void add_quad_patch(ScratchMesh& m, const Vec3f& p0, const Vec3f& p1, const Vec3f& p2,
                    const Vec3f& p3, const Vec2f& t0, const Vec2f& t1, const Vec2f& t2,
                    const Vec2f& t3, std::uint32_t orient) {
    const int i0 = m.add_vertex(p0, t0);
    const int i1 = m.add_vertex(p1, t1);
    const int i2 = m.add_vertex(p2, t2);
    const int i3 = m.add_vertex(p3, t3);
    m.quads.push_back({{i0, i1, i2, i3}, orient});
}

// add_box_faces: BuildblockMeshBuilder_AddBoxFaces @0x19c10. Emits one quad per
// enabled face from an explicit 8-corner box, classifying the per-face
// orientation from the face normal (the -0.707/+0.707 thresholds DAT_00278be4 /
// DAT_0027838c). corners are indexed by the same kFaceCorners/kCubeCorner tables.
constexpr float kNormLo = -0.70709884F;  // DAT_00278be4
constexpr float kNormHi = 0.70709884F;   // DAT_0027838c

std::uint32_t classify_orient(const Vec3f& n) {
    // Mirror of the AddBoxFaces/AddBezierPatch normal-classify ladder.
    if (n.y <= kNormHi) {
        if (kNormLo <= n.y) {
            if (n.x <= kNormHi) {
                if (kNormLo <= n.x) {
                    return kNormHi < n.z ? 0x60000U : 0x50000U;
                }
                return 0x10000U;
            }
            return 0x20000U;
        }
        return 0x40000U;  // -Y (bottom)
    }
    return 0x30000U;  // +Y (top)
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
        // Face normal from the first three corners (matches the native cross
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

// BakeScratchEntry @0x17ca0 UV derivation: project each vertex position onto the
// plane selected by the quad's orientation code. Mirrors the 0x10000..0x60000
// switch (positions feed UVs; some axes are mirrored via 1-coord).
Vec2f derive_uv(std::uint32_t orient, const Vec3f& p) {
    switch (orient & kOrientMask) {
        case kOrientPosX:
            return {p.z, p.y};
        case kOrientNegX:
            return {kOne - p.z, p.y};
        case kOrientPosY:
            return {p.x, p.z};
        case kOrientNegY:
            return {p.x, kOne - p.z};
        case kOrientPosZ:
            return {kOne - p.x, p.y};
        case kOrientNegZ:
            return {p.x, p.y};
        default:
            return {p.x, p.y};
    }
}

// BakeScratchEntry @0x17ca0: flatten the shared-vertex scratch into the
// per-face SblBlockMesh the editor renderer consumes. Derives UVs per quad
// orientation, masks materials to the low 12 bits, and records size.
BuildblockCatalogEntry bake_entry(const ScratchMesh& m, int sx, int sy, int sz,
                                  const std::string& name) {
    BuildblockCatalogEntry entry;
    entry.size_x = sx;
    entry.size_y = sy;
    entry.size_z = sz;
    entry.name = name;
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
        const bool has_orient = (q.material & kOrientMask) != 0;
        for (int k = 0; k < 4; ++k) {
            const auto vi = static_cast<std::size_t>(q.v[k]);
            const Vec3f& p = m.pos[vi];
            face.vertices.push_back(p);
            // BakeScratchEntry overwrites UVs by position-projection iff the quad's
            // orientation nibble is set (0x10000..0x60000); orientation 0 keeps the
            // authored UV carried from the bezier control grid.
            face.uvs.push_back(has_orient ? derive_uv(q.material, p) : m.uv[vi]);
        }
        entry.mesh.faces.push_back(std::move(face));
    }
    entry.mesh.face_count = static_cast<std::uint32_t>(entry.mesh.faces.size());
    return entry;
}

// RotateEntryY90 @0x4d170: produce a copy of an entry rotated 90 deg about Y.
// Swaps x<->z dims; vertex (x,z) -> (size_x' - z, x); recomputes UVs by re-bake
// of the rotated geometry (the native rotates orientation codes; re-deriving
// UVs from rotated positions is equivalent for the unit geometry here).
BuildblockCatalogEntry rotate_entry_y90(const BuildblockCatalogEntry& src) {
    BuildblockCatalogEntry dst = src;
    dst.size_x = src.size_z;
    dst.size_z = src.size_x;
    dst.mesh.width = static_cast<std::uint32_t>(dst.size_x);
    dst.mesh.depth = static_cast<std::uint32_t>(dst.size_z);
    const float new_x_extent = static_cast<float>(src.size_z);
    for (auto& face : dst.mesh.faces) {
        // Rotate orientation code through the native side-face cycle. Top/bottom
        // (0x30000/0x40000) and orient-0 (authored-UV) faces keep their axis.
        std::uint32_t orient = face.orientation;
        switch (orient) {
            case kOrientNegZ: orient = kOrientPosX; break;  // 0x60000 -> 0x10000
            case kOrientPosX: orient = kOrientPosZ; break;  // 0x10000 -> 0x50000
            case kOrientPosZ: orient = kOrientNegX; break;  // 0x50000 -> 0x20000
            case kOrientNegX: orient = kOrientNegZ; break;  // 0x20000 -> 0x60000
            default: break;                                 // top/bottom/authored keep axis
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
            // Position-projected faces re-derive UVs from the rotated geometry;
            // authored-UV faces (orient 0) carry their UV through the rotation
            // unchanged, matching the native bake (which only overwrites UVs for
            // a set orientation nibble).
            if (has_orient) {
                face.uvs[k] = derive_uv(orient, p);
            }
        }
    }
    return dst;
}

// Ensure out has room for the given id.
void ensure_id(std::vector<BuildblockCatalogEntry>& out, int id) {
    if (id >= 0 && static_cast<std::size_t>(id) >= out.size()) {
        out.resize(static_cast<std::size_t>(id) + 1);
    }
}

// Bake the scratch into catalog[base_id], then write its 3 Y90 rotations into
// the next three explicit ids (the native bakes id, then RotateEntryY90+Bake for
// id+1, id+2, id+3 using consecutive slot globals — which are consecutive ids
// for every 4-rotation group in the driver).
void emit_rot_group(std::vector<BuildblockCatalogEntry>& out, ScratchMesh& m, int base_id, int sx,
                    int sy, int sz) {
    ensure_id(out, base_id + 3);
    BuildblockCatalogEntry base = bake_entry(m, sx, sy, sz, "");
    BuildblockCatalogEntry r1 = rotate_entry_y90(base);
    BuildblockCatalogEntry r2 = rotate_entry_y90(r1);
    BuildblockCatalogEntry r3 = rotate_entry_y90(r2);
    out[static_cast<std::size_t>(base_id)] = std::move(base);
    out[static_cast<std::size_t>(base_id) + 1] = std::move(r1);
    out[static_cast<std::size_t>(base_id) + 2] = std::move(r2);
    out[static_cast<std::size_t>(base_id) + 3] = std::move(r3);
    m.clear();
}

// Bake the scratch into a single catalog[id] (no rotation group).
void emit_single(std::vector<BuildblockCatalogEntry>& out, ScratchMesh& m, int id, int sx, int sy,
                 int sz) {
    ensure_id(out, id);
    out[static_cast<std::size_t>(id)] = bake_entry(m, sx, sy, sz, "");
    m.clear();
}

// Standard unit-quad authored UVs (the curb/floor patches set a (0,0)-(1,1) UV
// control block; with orient nibble 0 the bake keeps these).
constexpr Vec2f kUV00{0.0F, 0.0F};
constexpr Vec2f kUV10{1.0F, 0.0F};
constexpr Vec2f kUV11{1.0F, 1.0F};
constexpr Vec2f kUV01{0.0F, 1.0F};

// --- Phase: curb/edge family (ids 0..15) -----------------------------------
// The native curb loop (piVar11 = &DAT_0500a70c, uVar9 = 0..15) emits a flat top
// plate at y=0.0625 spanning the cell (material 5), plus up to four vertical edge
// skirts dropping to y=0, gated by the 4 bits of the variant index uVar9:
//   bit0 (&1): the z=0 (-Z) edge      bit3 (&8): the x=0 (-X) edge
//   bit2 (&4): the z=1 (+Z) edge      bit1 (&2): the x=1 (+X) edge
// Control points transcribed verbatim from Buildblock_InitTemplateMeshCatalog.
// Edges carry material 0 (orient nibble 0 -> authored UVs survive).
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
        out[variant] = bake_entry(m, 1, 1, 1, "");
    }
}

// --- Phase: floor patch (ids 16..19) ---------------------------------------
// The native phase emits a single AddBezierPatch(...,1,1,8,3,0x30000) — an 8x1
// tessellated, order-3 curved top surface (the rounded floor/ramp) — then bakes
// it plus three Y90 rotations. The exact bicubic control mesh lives in scattered
// globals; round never renders this id, so (best-effort) we approximate it as a
// flat top plate at y=0.0625 with the same +Y (0x30000) orientation. Geometry is
// sane (correct footprint/orientation); the subtle curvature is the only loss.
void build_floor_patch(std::vector<BuildblockCatalogEntry>& out) {
    constexpr float kT = 0.0625F;
    ScratchMesh m;
    add_quad_patch(m, {0, kT, 0}, {1, kT, 0}, {1, kT, 1}, {0, kT, 1}, kUV00, kUV10, kUV11, kUV01,
                   0x30000);
    BuildblockCatalogEntry base = bake_entry(m, 1, 1, 1, "");
    BuildblockCatalogEntry r1 = rotate_entry_y90(base);
    BuildblockCatalogEntry r2 = rotate_entry_y90(r1);
    BuildblockCatalogEntry r3 = rotate_entry_y90(r2);
    out[16] = std::move(base);
    out[17] = std::move(r1);
    out[18] = std::move(r2);
    out[19] = std::move(r3);
}

// Build an 8-corner box (in kCubeCorner index order) spanning the unit cell in
// the flat axes, from y=0 up to a top that slopes from h0 to h1 along the axis
// selected by edge (edge<2 -> slope along +Z, else along +X). Used by the grade
// (ramp) tiles.
std::array<Vec3f, 8> kCubeCornerBox(float h0, float h1, int edge) {
    // Corner i = (x=i>>2&1, y=i>>1&1, z=i&1). Top corners (y==1) take the sloped
    // height; bottom corners stay at 0.
    std::array<Vec3f, 8> c{};
    for (int i = 0; i < 8; ++i) {
        const float x = static_cast<float>((i >> 2) & 1);
        const float yb = static_cast<float>((i >> 1) & 1);
        const float z = static_cast<float>(i & 1);
        const float t = (edge < 2) ? z : x;             // slope parameter
        const float top = (edge & 1) ? (h1 - (h1 - h0) * t) : (h0 + (h1 - h0) * t);
        c[static_cast<std::size_t>(i)] = {x, yb == 0.0F ? 0.0F : top, z};
    }
    return c;
}

// A flat top quad whose far edge is raised from h0 to h1 (the aa14 grade quad),
// tagged +Y so the bake position-projects its UVs.
void add_grade_quad(ScratchMesh& m, float h0, float h1, int edge) {
    Vec3f p0, p1, p2, p3;
    if (edge < 2) {  // slope along Z
        p0 = {0, h0, 0};
        p1 = {1, h0, 0};
        p2 = {1, h1, 1};
        p3 = {0, h1, 1};
    } else {  // slope along X
        p0 = {0, h0, 0};
        p1 = {1, h1, 0};
        p2 = {1, h1, 1};
        p3 = {0, h0, 1};
    }
    add_quad_patch(m, p0, p1, p2, p3, kUV00, kUV10, kUV11, kUV01, 0x30000);
}

// --- Prologue id model -----------------------------------------------------
// The driver assigns catalog ids to named slot globals in textual order, then
// runs geometry phases that write catalog[slotId]. Rather than hardcode ids, we
// replay the assignment with a running counter so every id lands exactly where
// the game computes it. Only the slots consumed by a geometry phase are recorded.
struct SlotIds {
    std::array<int, 16> curb{};         // a70c[0..15]   = ids 0..15
    int floor1_base = 0;                // a74c          = id 16 (+3 rot)
    int railing_base = 0;               // a75c          = id 20 (+3 rot)
    // Grade tiles: 9 grade levels (n = 4..12), each with an aa14 group and a
    // ba14 group of 4 edge-variants x n ids. Indexed [grade][group][edge][step].
    // We store the flat id per (grade, group, edge, step).
    std::array<std::array<std::array<std::vector<int>, 4>, 2>, 9> grade{};
    std::array<int, 4> floor2{};        // a7c4[0..3]
    int beam1_base = 0;                 // cb84          (+3 rot)
    int beam2_base = 0;                 // cb94          (+3 rot)
    int next = 0;                       // first free id after the prologue
};

// Replays Buildblock_InitTemplateMeshCatalog's prologue id assignment.
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
    // The driver then assigns a long run of sequential reserved slots
    // (a76c..a798, cad4..cb20, a79c..a7c0 = ids 24..65) with no procedural
    // geometry; skip the counter past them.
    count = 66;  // = the grade-tile fill start (verified from the prologue math)

    // Grade-tile fill loop: lVar29(per-group count)=4, lVar26(array offset)=0x10,
    // repeated 9 times; each iteration fills aa14 then ba14 for 4 edge-variants of
    // (count) ids, with count growing by 1 each grade level.
    int per = 4;             // ids per edge-variant at this grade level
    for (int grade = 0; grade < 9; ++grade) {
        const int n = per;   // grade level n = 4..12
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

    // DAT_0500a704 = align up to 4; floor2 + beam groups follow.
    count = (count + 3) & ~3;
    for (int i = 0; i < 4; ++i) {
        s.floor2[static_cast<std::size_t>(i)] = count++;  // a7c4[0..3]
    }
    // (cb24..cb80 reserved ids skipped; cb84/cb94 are the next two beam groups.)
    count = (count + 0x2c);  // advance past cb24..cb80 reserved span
    s.beam1_base = count;
    count += 4;
    s.beam2_base = count;
    count += 4;
    s.next = count;
    return s;
}

// --- Phase: grade-tile family (sloped ramp transition tiles) ----------------
// The native grade loops build sloped wedge tiles: for grade level n the top
// surface rises by step = 1/n across the cell, with the sloped edge chosen by the
// edge index. The aa14 group is a flat bezier quad (one raised edge); the ba14
// group is a box (AddBoxFaces, mask 0x3e) whose size_y becomes 2 when the slope
// crosses a full cell. Round renders none of these; the id placement is exact and
// the slope geometry follows the decoded fVar34/step parameterization.
void build_grade_tiles(std::vector<BuildblockCatalogEntry>& out, const SlotIds& s) {
    constexpr float kBase = 0.0625F;  // fVar5 = FLOAT_002781bc
    for (int grade = 0; grade < 9; ++grade) {
        const int n = grade + 4;
        const float step = 1.0F / static_cast<float>(n);
        for (int edge = 0; edge < 4; ++edge) {
            // ba14 group: sloped box.
            const auto& ba = s.grade[static_cast<std::size_t>(grade)][1][static_cast<std::size_t>(edge)];
            for (int k = 0; k < static_cast<int>(ba.size()); ++k) {
                const float h0 = kBase + static_cast<float>(k) * step;
                const float h1 = h0 + step;
                // A box from y=0 up to a sloped top edge (h0..h1 along the chosen
                // axis). edge<2 slopes along Z, else along X.
                std::array<Vec3f, 8> c = kCubeCornerBox(h0, h1, edge);
                ScratchMesh m;
                add_box_faces(m, c, 0, 0x3e);
                const int sy = (h1 >= 1.0F) ? 2 : 1;
                emit_single(out, m, ba[static_cast<std::size_t>(k)], 1, sy, 1);
            }
            // aa14 group: flat sloped quad.
            const auto& aa = s.grade[static_cast<std::size_t>(grade)][0][static_cast<std::size_t>(edge)];
            for (int k = 0; k < static_cast<int>(aa.size()); ++k) {
                const float h0 = static_cast<float>(k) * step;
                const float h1 = h0 + step;
                ScratchMesh m;
                add_grade_quad(m, h0, h1, edge);
                emit_single(out, m, aa[static_cast<std::size_t>(k)], 1, 1, 1);
            }
        }
    }
}

// --- Phase: beam groups 1 & 2 (barrier/bench pieces) ------------------------
// Args resolved from the disassembly (xmm register tracing):
//   XMM12 = FLOAT_002783f4 = 1.0, XMM8 = FLOAT_002781bc = 0.0625,
//   XMM7  = 1.0 (= XMM12), XMM6 = DAT_002782b0 = 0.3125, XMM13 = 0.0.
// Group 1 (cb84):
//   addbeam(0,0,0,0, 1.0,    1.0,    1.0,    0x22, mat={1..})
//   addbeam(0,0,0,0, 1.0,    0.3125, 0.0625, 0x2a, mat={1..})
// Group 2 (cb94):
//   addbeam(0,0,0,0, 1.0,    1.0,    1.0,    0x32, mat={1..})
//   addbeam(0,0,0,0, 0.0625, 0.3125, 0.0625, 0x32, mat={1..})
// Each is baked at its slot id then 3 Y90 rotations.
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
// The native phase emits a flat bezier quad (AddBezierPatch(...,1,1,1,1,0)) per
// lVar29 = 0..3, each a unit top quad at y=0 oriented by lVar29. Best-effort: a
// flat top quad with authored UVs (orient 0). Round renders none.
void build_floor_patch_2(std::vector<BuildblockCatalogEntry>& out, const SlotIds& s) {
    for (int i = 0; i < 4; ++i) {
        ScratchMesh m;
        add_quad_patch(m, {0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}, kUV00, kUV10, kUV11, kUV01, 0);
        emit_single(out, m, s.floor2[static_cast<std::size_t>(i)], 1, 1, 1);
    }
}

// --- Phase: third beam group (ids 20..23) = the railing ---------------------
// Verified from Buildblock_InitTemplateMeshCatalog @0x10110, exact float
// constants read from subrosa.exe (fVar6=FLOAT_002783f4=1.0, fVar5=FLOAT_002781bc
// =0.0625, fVar33=DAT_002782b0=0.3125):
//   _block_addbeam(0, 0,   0,    0,      1.0,    0.0625, 1.0,   0x2a, mat={1..})
//   _block_addbeam(0, 0.5, 0,    0.5,    0.0625, 0.3125, 0.0625,0x3f, mat={0..})
//   _block_addbeam(0, 0,   0.25, 0.4375, 1.0,    0.1875, 0.125, 0x3f, mat={0..})
// The base slab material array is _DAT_00278ee0 = {1,1,1,1,1,1}; post/bar zeroed.
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

std::vector<BuildblockCatalogEntry> build_native_builtin_catalog() {
    // Faithful structural port of Buildblock_InitTemplateMeshCatalog: replays the
    // prologue id assignment (assign_slot_ids) so every procedural id lands at its
    // true catalog index, then runs each geometry phase writing catalog[slotId]:
    //   ids   0..15 = curb/edge family            (build_curb_family)
    //   ids  16..19 = floor patch + 3 rots         (build_floor_patch)
    //   ids  20..23 = railing beam + 3 rots         (build_railing_group)
    //   ids  66..   = grade-tile ramp family        (build_grade_tiles)
    //   beam groups 1 & 2 + floor-patch-2 at their computed ids.
    // The reserved/runtime/garage/CSX slots (ids 24..65 and the CSX 0x585a0001
    // append) carry no procedural geometry; those indices resolve to empty slots
    // that render nothing, exactly as the native instancing loop does for an
    // unbuilt catalog slot.
    const SlotIds slots = assign_slot_ids();
    std::vector<BuildblockCatalogEntry> out;
    out.resize(static_cast<std::size_t>(slots.next));
    build_curb_family(out);             // ids 0..15
    build_floor_patch(out);             // ids 16..19
    build_railing_group(out, slots);    // ids 20..23
    build_grade_tiles(out, slots);      // grade ramp family
    build_beam_groups_1_2(out, slots);  // barrier/bench beams
    build_floor_patch_2(out, slots);    // floor-patch-2 group
    return out;
}

}  // namespace map_editor
