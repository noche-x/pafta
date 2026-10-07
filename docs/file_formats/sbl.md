# SBL block and buildblock format

The `.sbl` extension covers two record families. A block SBL stores renderable
mesh data. A buildblock SBL stores up to 16 variable-length opening-profile
layers. Both begin with a `u32`, but no confirmed magic value distinguishes the
families; the editor recognizes a block only when the first word is 1 or 2 and
the next three words form valid nonzero dimensions no greater than 64.

## Block header

| Offset | Type | Field |
|---:|---|---|
| `0x00` | `u32` | version, 1 or 2 |
| `0x04` | `u32` | width in cells |
| `0x08` | `u32` | height in cells |
| `0x0c` | `u32` | depth in cells |
| `0x10` | `u32[6]` | exterior-plane flags |
| `0x28` | `u32` | authored patch count |

Each nonzero exterior flag generates one or more axis-aligned faces from the
dimensions. The six indices correspond to the native face-direction table, but
the final human-readable direction names should come from a direct loader trace
rather than inference from the editor cube helper.

## Authored patch

Each patch occupies `0x158` bytes.

| Relative offset | Type | Field |
|---:|---|---|
| `0x00` | `u32` | column order |
| `0x04` | `u32` | column tessellation |
| `0x08` | `u32` | row order |
| `0x0c` | `u32` | row tessellation |
| `0x10` | `u32` | normal or material flag A |
| `0x14` | `u32` | normal or material flag B |
| `0x18` | `ControlPoint[16]` | Bezier control grid |

One control point occupies 20 bytes:

| Relative offset | Type | Field |
|---:|---|---|
| `0x00` | `f32` | X |
| `0x04` | `f32` | Y |
| `0x08` | `f32` | Z |
| `0x0c` | `f32` | U |
| `0x10` | `f32` | V |

The native in-memory record uses a `0x15c` stride. That extra four-byte runtime
field does not occur in the file. Readers must advance by `0x158`.

The editor caps the parsed patch count at `0x400` and clamps tessellation while
building preview triangles. Those are local safety limits, not confirmed file
limits.

## Version-2 box section

Version 2 stores a `u32` box count immediately after the authored patches
(`BlockSbl_LoadFromFile @e6e50` reads it only when the version is above 1).
These records are extra textured boxes, not lights: the game has no lights, and
`Buildblock_InitTemplateMeshCatalog` passes each record to
`BuildblockMeshBuilder_AddBoxFaces @19c10`. The editor's code still names them
`SblBlockLight`. Each record occupies `0x7c` bytes:

| Relative offset | Type | Field |
|---:|---|---|
| `0x00` | `Vec3f[8]` | eight corners, 96 bytes: 0-3 one ring, 4-7 the ring above it |
| `0x60` | `u32` | material: the face texture slot |
| `0x64` | `u32[5]` | read and kept, not used to build the mesh |
| `0x78` | `u32` | side mask: bit n emits side n (63 = all six) |

Sides use corners `3 2 1 0`, `7 6 2 3`, `4 7 3 0`, `5 4 0 1`, `6 5 1 2` and
`4 5 6 7` for bits 0 to 5. The game ORs a UV projection into each side's
material from its normal (component beyond 0.7071): +Y `0x40000`, -Y `0x30000`,
+X `0x20000`, -X `0x10000`, +Z `0x60000`, otherwise `0x50000`. The first word is
the material and the last word the mask; a record written as `63, 0, 0, 0, 0,
0, 0` has no sides.

Face materials of a block (patch flag A, box material) are texture slots 0 to
7 of the building cell that places the block, not global texture indices
(see `material_for_piece_face` in `scene.cpp`).

Inside a CSX archive, `BlockSblContainer_ReadWrite @1967e0` requires version 1
but always reads the box count and records, so an archived block is the
version-2 layout with 1 in the version field.

## Buildblock layer stream

If the block-header test fails, the editor reads variable-length layers from
offset `0x04` until end of file or 16 layers. The first word remains exposed as
the template version, but its accepted values and native meaning are unresolved.

Each layer begins with:

| Relative offset | Type | Field |
|---:|---|---|
| `0x00` | `u32` | portal type |
| `0x04` | `f32` | bounds X |
| `0x08` | `f32` | bounds width |
| `0x0c` | `f32` | bounds Y |
| `0x10` | `f32` | bounds height |
| `0x14` | `u32` | door count |

Door records follow immediately and occupy 12 bytes:

```text
type u32, x f32, y f32
```

A `u32` window count follows the doors. Each window occupies 20 bytes:

```text
type u32, x f32, width f32, y f32, height f32
```

A `u32` beam count and the same 20-byte rectangle shape follow the windows.
There is no per-layer byte count, alignment pad, or fixed file stride.

The editor caps doors at 2 and rectangles at 16 while parsing. Native runtime
storage uses fixed-size layer structures, but those runtime offsets do not
describe this variable-length file stream.

Side codes 3 through 15 select `layers[side_code - 3]`. Portal type 1 changes
the horizontal mirroring rule on portal sides greater than 1.

## Writer behavior

The editor patches block header values, authored-patch data, and buildblock
layers in place. It does not rebuild an SBL from an empty model, resize a layer,
or change record counts beyond the space already present in the source file.
Unknown and trailing bytes remain intact.

