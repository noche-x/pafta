# Sub Rosa building, block, and buildblock specification

Status: reverse-engineering specification, 2026-08-26

Evidence sources:

- `editor.exe`, decompiled functions under `editor_exe_reversed/decomp/` and
  `editor_exe_reversed/ede_reversed/`.
- `editor.exe` function `BuildBuildingBlockRenderBuffers @ 0x1400799d0`.
- `editor.exe` function `RenderBuildingCellFaceGroupWorld @ 0x140061800`.
- `editor.exe` function `RenderBuildingCellFaceGroupPreview @ 0x140078840`.
- `editor.exe` function `RenderBuildingToolCellPreview @ 0x140078060`.
- `editor.exe` function `LoadBuilding_SBB @ 0x1400556f0` and
  `SaveBuilding_SBB @ 0x1400548d0`.
- `editor.exe` function `LoadAssets_TexturesBuildings @ 0x140057a60`.

This document separates observed behavior from port assumptions. It does not
claim that the current OpenGL port is pixel-identical to the game.

## Coordinate and cell model

The building editor owns a 64 x 64 x 64 voxel workspace. Cells are 60 bytes and
are addressed in `[y][z][x]` order:

```text
cell = grid_base + y * 0x3c000 + z * 0xf00 + x * 0x3c
```

The four-world-unit cell size is `DAT_1400ec9b4`. The editor preview uses
`floor * 4.0` as its work plane. The building preview grid has 65 lines in both
horizontal directions and every fourth line is a major line.

The live editor cell layout, relative to the cell's `cellType` field, is:

| Offset | Field | Type | Meaning |
|---:|---|---|---|
| `0x00` | `cellType` | `u32` | Buildblock/template selection. |
| `0x04` | `exteriorBlock` | `u32` | Exterior piece or solid payload. |
| `0x08` | `interiorBlock` | `u32` | Interior piece or `0xffff` for no interior. |
| `0x0c` | `exteriorPortalFace` | `u32` | Side code for exterior wall profile. |
| `0x10` | `interiorPortalFace` | `u32` | Side code for interior wall profile. |
| `0x14` | `placedFlag` | `u32` | Placement bits used by floor/ceiling preview. |
| `0x18` | `exteriorTextureSet` | `u16[8]` | Exterior logical face texture slots. |
| `0x28` | `interiorTextureSet` | `u16[8]` | Interior logical face texture slots. |
| `0x38` | `itemsetPayload` | `u32` | Itemset or itemset-boundary data. |

The file's serialized cell order is different from the live memory order. The
SBB writer emits the first six words in this order:

```text
exteriorBlock, interiorBlock, cellType,
exteriorPortalFace, interiorPortalFace, placedFlag
```

It then emits 16 little-endian `u16` texture slots and one `u32` itemset value.
The serialized cell record is therefore `0x3c` bytes.

## SBB building file

`LoadBuilding_SBB` accepts versions `0x0c` through `0x0e`. `SaveBuilding_SBB`
always writes version `0x0e`.

The fixed header is:

| File offset | Size | Field |
|---:|---:|---|
| `0x00` | `u32` | Version. |
| `0x04` | `0x40` | Building name, NUL/space-padded ASCII. |
| `0x44` | `u32` | X extent used for cell serialization. |
| `0x48` | `u32` | Z/depth extent used for cell serialization. |
| `0x4c` | `u32` | Y/height extent used for cell serialization. |
| `0x50` | `u32` | X anchor, version `> 0x0c`. |
| `0x54` | `u32` | Y anchor, version `> 0x0c`. |
| `0x58` | `u32` | Z anchor, version `> 0x0c`. |

For version `0x0c`, the loader defaults anchors to `(4, 4, 4)`. The cell stream
uses `(size_x + 1) * (size_y + 1) * size_z` records, with the native 64-cell
cap. The loop order is `y`, then `z`, then `x`.

After the header, the loader reads four counted name tables. Each table is a
`u32` count followed by 64-byte ASCII names:

```text
materials
building texture names
building block names
itemset names
```

The material table begins at `0x54` for version `0x0c` and `0x60` for later
versions. The exact table count is stored immediately before the table. The
remaining three tables are read sequentially after the material names.

For version `0x0e`, `ReadOrWriteBuildingLODs` is called after the cell stream.
The exact version-14 tail is:

```text
u32 lod_marker                 // must be 1 when loading
u32 group_count
repeat group_count:
    u32 entry_count
    repeat entry_count:
        f32 position.x
        f32 position.y
        f32 position.z
        f32 value
```

Each group occupies `4 + 0x400` bytes in the native workspace: the count plus
up to 64 entries of 16 bytes. The native group stride is `0x404`. The loader
returns without loading the tail when the marker is not `1`.
`ReadOrWriteBuildingLODs @ 0x1400563d0` is the evidence for this layout. The
current parser preserves the tail bytes but only exposes the marker and group
count; it does not yet decode the group entries.

## Payload encoding

The building render builder tests the top three payload bits:

```text
payload & 0xe0000000 == 0x40000000  -> solid cell
payload & 0xe0000000 == 0x80000000  -> block/piece mesh instance
```

For a piece payload, the low 10 bits select a block mesh and bits 24-25 select
one of four rotations. The editor preview resolves the mesh through the loaded
block table before calling `RenderBuildingCellFaceGroupPreview`.

The native load/preprocess path is important. `FUN_140053d00` copies the live
building cells into a scratch volume, then replaces each piece word with
`0x80000000 | rotatedBlockIndex`, where `rotatedBlockIndex` is the base block
table index for the low-10-bit mesh plus the rotation `(word >> 24) & 3`.
`BuildBuildingBlockRenderBuffers` therefore consumes the normalized table index
and does not rotate prototype vertices inside its voxel loop. A port that both
pre-rotates the table index and rotates the copied vertices applies the rotation
twice.

For a solid cell, the native builder emits only exposed faces 1 through 5. It
does not emit face 0. The neighbor checks are:

| Face | Neighbor |
|---:|---|
| 1 | `z + 1` |
| 2 | `x - 1` |
| 3 | `z - 1` |
| 4 | `x + 1` |
| 5 | `y + 1` |

A face is skipped only when the neighbor payload is also a solid
`0x40000000` cell. A piece neighbor is not treated as a solid occluder.

## Native block mesh generation

`BuildBuildingBlockRenderBuffers @ 0x1400799d0` is the authoritative SBB mesh
construction path. It performs these operations:

1. Clear temporary vertex, quad, and triangle accumulators.
2. Scan the source voxel volume using the native `[y][z][x]` strides.
3. Emit exposed solid-cell faces.
4. For each piece cell, copy the referenced prototype vertices and UVs, then
   remap prototype quad and triangle indices to the copied vertex range.
5. For solid side faces with edge flags `3..15`, emit an inset profile quad using
   the buildblock/itemset geometry table. The inset material slot is `0x100`.
6. Triangulate each quad as `(0,1,2)` and `(0,2,3)`.
7. Recenter every vertex by `-volumeCenter * cellSize` on all three axes.
8. Pack uploaded vertices as five floats: `position.xyz, uv.xy`.
9. Sort triangles into contiguous material runs for slots `0..0x100`.
10. Upload one VBO and one index buffer for the block record.

Before this builder runs, `FUN_140053d00` also resolves the eight exterior and
eight interior texture slots. For each slot, the low byte is remapped through
the global texture-index table and the high byte is retained as orientation or
face metadata. The resulting 16-bit values are what the builder uses as
`cellTexSet`; treating an SBB short as a direct GL texture index loses this
remap.

The native per-block GPU record stride is `0xc30`. Its important fields are:

| Offset | Field |
|---:|---|
| `0x00` | Vertex count. |
| `0x04` | Vertex stride in floats, `5`. |
| `0x08` | Packed vertex pointer. |
| `0x10` | Index/triangle count. |
| `0x18` | Packed index pointer. |
| `0x20` | Material-run count. |
| `0x24` | Material runs, each `{slot, triangleCount, firstIndex}` at stride `0x0c`. |

The editor's upload path uses separate GL names for the array and element buffers.
`RenderBuildingTemplatePreview @ 0x140079420` binds both buffers, uses position
attribute 0 with three floats and UV attribute 1 with two floats at a 20-byte
stride, then draws each material run separately.

## Texture and material selection

`LoadAssets_TexturesBuildings @ 0x140057a60` scans `texture/*.png`, strips
extensions, loads textures in scan order, and loads every SBB building. Texture
parameters observed at the upload call are:

```text
wrap S/T: GL_REPEAT (0x2901)
min filter: GL_LINEAR_MIPMAP_NEAREST (0x2703)
mag filter: GL_LINEAR (0x2601)
```

`ResolveBuildingTextureIndices @ 0x140057970` compares each SBB texture name
against the global loaded texture names. Unmatched slots resolve to index zero.

`RenderBuildingCellFaceGroupWorld @ 0x140061800` and
`RenderBuildingCellFaceGroupPreview @ 0x140078840` resolve each mesh face's
logical material slot as follows:

- Face slot values are clamped to `0..7`.
- Exterior rendering reads the exterior eight-slot array.
- Interior rendering reads the interior eight-slot array for slots `0..7`.
- Interior mesh face slots `8..15` read exterior slot `slot - 8`.
- The resolved SBB texture index maps through the global texture-index table to
  the GL texture object.

The preview path batches adjacent faces by resolved GL texture. It binds the
texture before emitting the face group. The world path uses the same texture
resolution but renders in fixed-function immediate mode in this editor build.

## Buildblock `.sbl` layers

An SBL file is either a block mesh or a buildblock template. Both start with a
`u32` version. Block meshes use versions `1` or `2` and have this header:

| Offset | Size | Field |
|---:|---:|---|
| `0x00` | `u32` | Version. |
| `0x04` | `u32` | Width in cells. |
| `0x08` | `u32` | Height in cells. |
| `0x0c` | `u32` | Depth in cells. |
| `0x10..0x24` | `6*u32` | Exterior face flags. |
| `0x28` | `u32` | Authored patch count. |

Each authored patch has four `u32` controls followed by two normal/material
flags and sixteen 20-byte control points. Each control point is:

```text
position.x, position.y, position.z, uv.u, uv.v  // five f32 values
```

The file record payload is 344 bytes (`0x158`): 24 bytes of controls plus
`16 * 20` bytes of points. The native in-memory face stride is `0x15c`, but the
four-byte difference is workspace layout, not a serialized record pad. For a
version-2 file, the light-face count follows immediately after the 344-byte
record payload. Do not use the in-memory `0x15c` stride as a file offset.

For version greater than `1`, the mesh may contain a light-face section. It
starts with a count, then each light record contains eight `Vec3` points, six
scalar words, and one final face mask word. A light face is emitted only when
its corresponding final-mask bit is set. Its material is the first scalar
word. The native loader computes a normalized triangle normal from the face's
first three lookup corners and derives the orientation tag from that normal
(`+Y=0x40000`, `-Y=0x30000`, `+X=0x20000`, `-X=0x10000`, `+Z=0x60000`, and
`-Z=0x50000`) before the face reaches the texture-coordinate postprocess
step. The orientation is therefore not a fixed function of the face index;
authored light points can be rotated or non-cubic. `formats.cpp` follows this
normal-derived rule when it emits parsed light faces.

The native generated-face order is observable in
`LoadAndInitBlockTemplates_SBL @ 0x14004d4c0`: enabled light faces are emitted
first, authored Bezier faces second, and enabled axis-aligned exterior planes
last. This order differs from a parser that stores exterior faces first and
appends light faces later. Material batching can reduce the visual impact, but
the order must be preserved if draw order or face-range diagnostics are used.

Buildblock templates use up to 16 sequential layers. Each layer is:

```text
portal_type       u32
bounds            4 * f32  // x, width, y, height in cell fractions
door_count        u32
doors             {type u32, x f32, y f32}
window_count      u32
windows           {type u32, x f32, width f32, y f32, height f32}
beam_count        u32
beams             {type u32, x f32, width f32, y f32, height f32}
```

The editor consumes side code `3..15` as layer index `sideCode - 3`. Portal
type `1` changes the x-mirroring rule for sides greater than `1`; this is why a
single buildblock layer cannot be mirrored by simply negating its world x.

The native wall builder is `FUN_140051c60`. For a layer with bounds
`(x, width, y, height)`, it emits up to four frame rectangles around the
opening whenever a corresponding gap is greater than the native epsilon:

```text
left   = (0, 0,             x,                         1)
right  = (x + width, 0,     1 - (x + width),           1)
bottom = (0, 0,             1,                         y)
top    = (0, y + height,    1,                         1 - (y + height))
```

The tuple order above is `(x, y, width, height)` in layer-local coordinates.
It then emits every beam record as a rectangle. The beam record's type selects
the generated face/material flags. Window records are not read by this
function; they belong to the separate opening/mask path. For
`portal_type == 1` and side
indices greater than `1`, the x origin is mirrored as
`1 - (x + width)`; otherwise it is `x`. `FUN_1400523b0` expands each selected
face mask into the standard six-face vertex layout and rotates the face
orientation for the portal side. This is the exact source of the editor's
portal wall frame and opening geometry.

## Windows, doors, and portals

`RenderBuildingToolCellPreview @ 0x140078060` selects exterior and interior
piece meshes first. For solid cells it computes exposed side bits from portal
face values and neighbor solid state, then invokes the itemset-boundary preview.

The native world-buffer path has a separate guard that the port must preserve.
`BuildBuildingBlockRenderBuffers @ 0x1400799d0` emits the ordinary exposed
solid faces for every `0x40000000` cell, but it expands the side-code profile
(`3..15`) only when the cell's interior block is `0xffff`. An interior block
therefore does not receive the exterior buildblock profile or its window
metadata. The city buffer follows this rule by applying the window mask only
to a solid cell with no interior block.

For side codes `3..15`, the native editor builds an inset wall profile from the
layer bounds. Door, window, and beam records are metadata used by the portal and
buildblock tools. The current decompile proves the rectangle record shape and
the side mirroring rule. It does not prove that a window is a solid box in the
game renderer.

The city renderer's window treatment must therefore remain a separate parity
target. A correct implementation needs to reproduce the game's generated
wall-mask/texture-array path, not replace a window with an opaque quad. The
current port's frame-strip approximation is useful for editor visibility but is
not evidence of game rendering.

`LoadBuildBlock_SBL @ 0x140057010` stores each of 16 layers in a fixed `0x300`
byte runtime record. The observed runtime arrays are: door count at `+0x18`
with `0x0c`-byte records, window count at `+0x34` with `0x14`-byte records,
and beam count at `+0x178` with `0x18`-byte records. The file reader is a
variable-length stream, so these runtime offsets must not be copied as file
offsets. `FUN_140051c60` uses the layer bounds and beam records to construct
the inset profile; window records are consumed by the separate opening/mask
path. This distinction is why a full opaque window box is not a safe parity
fix.

## Editor preview versus world rendering

The editor preview uses a dedicated GLSL 3.30 shader initialized by
`InitPreviewTextureColorShader @ 0x1400779f0`. Its known uniforms are:

```text
modelviewprojectionmatrix
lightdirection
modelmatrix
clipplane
tex_color (bound to texture unit 0)
```

`SetPreviewLightDirectionsFromAxes @ 0x140075f30` rebuilds the preview transform
from the two light-axis values and the current preview yaw/target. This is not
the same as the game's city material shader. The editor's fixed-function world
path and the preview shader must not be treated as interchangeable evidence for
the game's final lighting.

## Port parity gaps to fix

The following gaps are directly actionable:

- Use both exterior and interior piece payloads in every world/building draw
  path that corresponds to the native face-group calls.
- Preserve the 4-rotation piece transform from payload bits 24-25.
- Match the 5-float position/UV vertex layout and contiguous per-texture runs.
- Apply volume-center recentering exactly once. Do not combine it with a second
  anchor subtraction.
- Use all six native face directions for block templates, but keep solid-cell
  world emission at native faces 1..5 where the native function does so.
- Treat windows as openings or masks until the game-side wall-mask path is
  traced. An opaque window surface is incorrect.
- Decode `.sbb` LOD group entries using the version-14 tail layout above; the
  current parser still preserves the bytes but does not expose each entry.
- Add adjacency or exact triangle silhouette data before claiming exact building
  shadow parity. Bounds-only shadow casters cannot reproduce roofs, beams, or
  window cutouts.
