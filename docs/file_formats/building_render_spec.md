# How Sub Rosa draws buildings, blocks, and buildblocks

Status: reverse-engineering notes, started 2026-08-26, with later traces from the game added.

Sources:

- `editor.exe`, decompiled functions in `editor_exe_reversed/decomp/` and `editor_exe_reversed/ede_reversed/`.
- `editor.exe` function `BuildBuildingBlockRenderBuffers @ 0x1400799d0`.
- `editor.exe` function `RenderBuildingCellFaceGroupWorld @ 0x140061800`.
- `editor.exe` function `RenderBuildingCellFaceGroupPreview @ 0x140078840`.
- `editor.exe` function `RenderBuildingToolCellPreview @ 0x140078060`.
- `editor.exe` functions `LoadBuilding_SBB @ 0x1400556f0` and `SaveBuilding_SBB @ 0x1400548d0`.
- `editor.exe` function `LoadAssets_TexturesBuildings @ 0x140057a60`.

This page keeps what was seen in the game apart from guesses made while porting. It does not claim that Pafta's OpenGL code draws exactly the same pixels as the game.

The decompiled code and the original executables live outside this repository; Pafta does not ship them. This page does not give their hashes. Treat the addresses as notes from past research, and check them against the matching original binary.

Contents:

- [Cells and coordinates](#cells-and-coordinates)
- [The SBB file](#the-sbb-file)
- [Block words](#block-words)
- [How the original editor builds block meshes](#how-the-original-editor-builds-block-meshes)
- [Textures and materials](#textures-and-materials)
- [Buildblock `.sbl` layers](#buildblock-sbl-layers)
- [Windows, doors, and portals in the original editor](#windows-doors-and-portals-in-the-original-editor)
- [How the game draws buildblock walls and windows](#how-the-game-draws-buildblock-walls-and-windows)
- [Editor preview versus world rendering](#editor-preview-versus-world-rendering)
- [Gaps left in the port](#gaps-left-in-the-port)

## Cells and coordinates

The original building editor works in a 64 x 64 x 64 grid of voxels. Each cell is 60 bytes and is stored in `[y][z][x]` order:

```text
cell = grid_base + y * 0x3c000 + z * 0xf00 + x * 0x3c
```

A cell is four world units wide (`DAT_1400ec9b4`). The editor preview uses `floor * 4.0` as its working height. The preview grid has 65 lines in each horizontal direction, and every fourth line is drawn heavier.

In the editor's memory, a cell looks like this (offsets count from the `cellType` field):

| Offset | Field | Type | Meaning |
|---:|---|---|---|
| `0x00` | `cellType` | `u32` | Which buildblock template to use. |
| `0x04` | `exteriorBlock` | `u32` | Outside piece, or the solid marker. |
| `0x08` | `interiorBlock` | `u32` | Inside piece, or `0xffff` for none. |
| `0x0c` | `exteriorPortalFace` | `u32` | Side code for the outside wall shape. |
| `0x10` | `interiorPortalFace` | `u32` | Side code for the inside wall shape. |
| `0x14` | `placedFlag` | `u32` | Bits used by the floor and ceiling preview. |
| `0x18` | `exteriorTextureSet` | `u16[8]` | Texture slots for the outside faces. |
| `0x28` | `interiorTextureSet` | `u16[8]` | Texture slots for the inside faces. |
| `0x38` | `itemsetPayload` | `u32` | Item set, or item-set boundary data. |

The file stores the fields in a different order from memory. The SBB writer writes the first six words like this:

```text
exteriorBlock, interiorBlock, cellType,
exteriorPortalFace, interiorPortalFace, placedFlag
```

Then it writes 16 little-endian `u16` texture slots and one `u32` item-set value, so a cell in the file is `0x3c` bytes.

## The SBB file

`LoadBuilding_SBB` reads versions `0x0c` to `0x0e`. `SaveBuilding_SBB` always writes version `0x0e`.

The fixed header:

| File offset | Size | Field |
|---:|---:|---|
| `0x00` | `u32` | Version. |
| `0x04` | `0x40` | Building name, ASCII padded with NULs or spaces. |
| `0x44` | `u32` | X size, used when storing cells. |
| `0x48` | `u32` | Z (depth) size, used when storing cells. |
| `0x4c` | `u32` | Y (height) size, used when storing cells. |
| `0x50` | `u32` | X anchor, version `> 0x0c`. |
| `0x54` | `u32` | Y anchor, version `> 0x0c`. |
| `0x58` | `u32` | Z anchor, version `> 0x0c`. |

For version `0x0c`, the loader sets the anchor to `(4, 4, 4)`. The file holds `(size_x + 1) * (size_y + 1) * size_z` cells, capped at 64 per axis as in the game. The loops go `y`, then `z`, then `x`.

After the header come four name lists. Each is a `u32` count followed by that many 64-byte ASCII names:

```text
materials
building texture names
building block names
itemset names
```

The material list starts at `0x54` in version `0x0c` and at `0x60` in later versions. Its count sits just before it. The other three lists follow the material names one after another.

For version `0x0e`, `ReadOrWriteBuildingLODs` runs after the cells. The version-14 end section is:

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

In the editor's memory each group takes `4 + 0x400` bytes: the count plus room for 64 entries of 16 bytes each, so groups are `0x404` apart. If the marker is not `1`, the loader skips this section. The evidence is `ReadOrWriteBuildingLODs @ 0x1400563d0`. Pafta keeps these bytes but only reads the marker and group count; it does not decode the entries yet.

## Block words

The building mesh builder looks at the top three bits of a block word:

```text
payload & 0xe0000000 == 0x40000000  -> solid cell
payload & 0xe0000000 == 0x80000000  -> block/piece mesh instance
```

For a piece, the low 10 bits pick a block mesh, and bits 24-25 pick one of four rotations. The editor preview looks the mesh up in the loaded block list before calling `RenderBuildingCellFaceGroupPreview`.

Watch out for the preprocessing step. `FUN_140053d00` copies the building cells into a scratch grid and replaces each piece word with `0x80000000 | rotatedBlockIndex`. `rotatedBlockIndex` is the list index of the low-10-bit mesh plus the rotation `(word >> 24) & 3`. So `BuildBuildingBlockRenderBuffers` gets an index that already includes the rotation, and does not rotate any vertices itself. If a port both adds the rotation to the index and rotates the copied vertices, it rotates twice.

For a solid cell, the game only makes faces 1 to 5, and only the ones that show. It never makes face 0. The neighbor checked for each face:

| Face | Neighbor |
|---:|---|
| 1 | `z + 1` |
| 2 | `x - 1` |
| 3 | `z - 1` |
| 4 | `x + 1` |
| 5 | `y + 1` |

A face is only skipped when the neighbor is also a solid `0x40000000` cell. A piece next door does not hide a face.

## How the original editor builds block meshes

`BuildBuildingBlockRenderBuffers @ 0x1400799d0` is the reference for how an SBB mesh is built. It does these steps in order:

1. Clear the temporary vertex, quad, and triangle lists.
2. Walk the voxel grid using the `[y][z][x]` strides.
3. Add the visible faces of solid cells.
4. For each piece cell, copy the piece's vertices and UVs, then shift its quad and triangle indices to point at the copies.
5. For solid side faces with edge flags `3..15`, add an inset wall-shape quad from the buildblock/item-set geometry table. The inset uses material slot `0x100`.
6. Split each quad into the triangles `(0,1,2)` and `(0,2,3)`.
7. Move every vertex by `-volumeCenter * cellSize` on all three axes, so the building is centred.
8. Pack each vertex as five floats: `position.xyz, uv.xy`.
9. Sort triangles into runs that share a material, for slots `0..0x100`.
10. Upload one vertex buffer and one index buffer for the block.

Before that, `FUN_140053d00` also resolves the eight outside and eight inside texture slots. For each slot, the low byte is looked up in the global texture table and the high byte is kept as orientation or face data. The builder uses these 16-bit results as `cellTexSet`. Using an SBB value directly as a GL texture number skips this lookup and gets the wrong texture.

The game keeps one GPU record per block, `0xc30` bytes apart. The fields that matter:

| Offset | Field |
|---:|---|
| `0x00` | Vertex count. |
| `0x04` | Floats per vertex, `5`. |
| `0x08` | Pointer to packed vertices. |
| `0x10` | Index/triangle count. |
| `0x18` | Pointer to packed indices. |
| `0x20` | Number of material runs. |
| `0x24` | Material runs, each `{slot, triangleCount, firstIndex}`, `0x0c` bytes apart. |

The editor uploads into separate GL buffers for vertices and indices. `RenderBuildingTemplatePreview @ 0x140079420` binds both, uses attribute 0 for position (three floats) and attribute 1 for UV (two floats) with a 20-byte stride, and draws each material run on its own.

## Textures and materials

`LoadAssets_TexturesBuildings @ 0x140057a60` scans `texture/*.png`, drops the extensions, loads the textures in the order it finds them, and loads every SBB building. The texture settings at upload are:

```text
wrap S/T: GL_REPEAT (0x2901)
min filter: GL_LINEAR_MIPMAP_NEAREST (0x2703)
mag filter: GL_LINEAR (0x2601)
```

`ResolveBuildingTextureIndices @ 0x140057970` matches each SBB texture name against the loaded texture names. Names with no match get index zero.

`RenderBuildingCellFaceGroupWorld @ 0x140061800` and `RenderBuildingCellFaceGroupPreview @ 0x140078840` pick a material for each mesh face like this:

- The face slot is clamped to `0..7`.
- Outside drawing reads the outside eight-slot array.
- Inside drawing reads the inside eight-slot array for slots `0..7`.
- Inside mesh faces with slots `8..15` read outside slot `slot - 8`.
- The resulting SBB texture index goes through the global texture table to get the GL texture.

The preview draws neighbouring faces with the same texture together, binding the texture before each group. The world view picks textures the same way but draws with old fixed-function immediate mode in this editor build.

## Buildblock `.sbl` layers

An SBL file is either a block mesh or a buildblock template. Both start with a `u32` version. Block meshes use version `1` or `2` and have this header:

| Offset | Size | Field |
|---:|---:|---|
| `0x00` | `u32` | Version. |
| `0x04` | `u32` | Width in cells. |
| `0x08` | `u32` | Height in cells. |
| `0x0c` | `u32` | Depth in cells. |
| `0x10..0x24` | `6*u32` | Outside-face flags. |
| `0x28` | `u32` | Patch count. |

Each patch has four `u32` settings, two normal/material flags, and sixteen 20-byte control points. Each control point is:

```text
position.x, position.y, position.z, uv.u, uv.v  // five f32 values
```

A patch in the file is 344 bytes (`0x158`): 24 bytes of settings plus `16 * 20` bytes of points. In memory the game spaces faces `0x15c` apart, but the extra four bytes are only in memory, not padding in the file. In a version-2 file, the box count comes right after the last 344-byte patch. Never use the `0x15c` memory spacing as a file offset.

When the version is above `1`, the mesh can have a box section (older notes call these "lights"). It starts with a count. Each record has eight `Vec3` corners, six number words, and a final face-mask word. A box face is only made when its bit in the mask is set, and its material is the first number word. The game's loader works out a normal from the face's first three corners and picks an orientation tag from it (`+Y=0x40000`, `-Y=0x30000`, `+X=0x20000`, `-X=0x10000`, `+Z=0x60000`, and `-Z=0x50000`) before the texture coordinates are worked out. So the orientation does not follow from the face number alone; box corners can be rotated or not even form a cube. `formats.cpp` uses the same normal-based rule.

`LoadAndInitBlockTemplates_SBL @ 0x14004d4c0` shows the order the game makes faces in: box faces first, then the Bezier patch faces, then the flat outside faces. A parser that stores the outside faces first and adds box faces last has a different order. Grouping by material can hide the difference on screen, but the order matters if anything depends on draw order or face ranges.

Buildblock templates have up to 16 layers, one after another. Each layer is:

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

The editor uses side code `3..15` as layer number `sideCode - 3`. Portal type `1` changes how sides above `1` are mirrored on x, which is why you cannot mirror a buildblock layer just by flipping its world x.

The original editor's wall builder is `FUN_140051c60`. For a layer with bounds `(x, width, y, height)`, it adds up to four frame rectangles around the opening, one for each gap that is bigger than a small epsilon:

```text
left   = (0, 0,             x,                         1)
right  = (x + width, 0,     1 - (x + width),           1)
bottom = (0, 0,             1,                         y)
top    = (0, y + height,    1,                         1 - (y + height))
```

Each tuple is `(x, y, width, height)` in the layer's own coordinates. The builder then adds a rectangle for every beam record, and the beam's type picks the face and material flags. This function does not read window records; those belong to the separate opening and mask code. For `portal_type == 1` and sides above `1`, the x start is mirrored to `1 - (x + width)`; otherwise it stays `x`. `FUN_1400523b0` turns each chosen face mask into the usual six-face vertex layout and turns the face to match the portal side. This is exactly where the original editor's portal wall frames and openings come from.

## Windows, doors, and portals in the original editor

`RenderBuildingToolCellPreview @ 0x140078060` picks the outside and inside piece meshes first. For solid cells it works out which sides show from the portal-face values and which neighbours are solid, then draws the item-set boundary preview.

The world-buffer code has one more rule a port has to keep. `BuildBuildingBlockRenderBuffers @ 0x1400799d0` adds the normal visible faces for every `0x40000000` cell, but only builds the side-code wall shape (`3..15`) when the cell's interior block is `0xffff`. So a cell with an interior block gets no outside buildblock shape and no window data. Pafta's city buffer follows this by only applying the window mask to solid cells with no interior block.

For side codes `3..15`, the original editor builds an inset wall shape from the layer bounds. Door, window, and beam records are data for the portal and buildblock tools. The decompiled code proves the rectangle record layout and the mirroring rule. It does not prove that the game draws a window as a solid box.

So the city renderer's windows have to be checked against the game on their own. A correct version has to copy the game's wall-mask and texture-array path; covering the window with a solid quad is wrong. The port's earlier frame-strip version helped make things visible in the editor but did not match the game. The game trace below explains the wall-mask and interior-window paths that replaced that guess. The viewport still needs a side-by-side comparison with the game.

`LoadBuildBlock_SBL @ 0x140057010` keeps each of the 16 layers in a fixed `0x300`-byte record in memory. In that record, the door count is at `+0x18` with `0x0c`-byte doors, the window count at `+0x34` with `0x14`-byte windows, and the beam count at `+0x178` with `0x18`-byte beams. The file is read as a stream of varying length, so never use these memory offsets as file offsets. `FUN_140051c60` builds the inset shape from the layer bounds and beams; window records go to the separate opening and mask code. That is why a solid window box is not a safe fix.

## How the game draws buildblock walls and windows

This section comes from `subrosa.exe` and has been checked.

### Two worlds

The game keeps an outside world and an inside world. `InstantiateSbbVolumeIntoWorldAndMetadata @23ac0` writes each building cell's exterior block word (solid is `0x40000000`) into the outside world. When the interior block is `0xffff`, it also builds a room (`_block_createbuildblock @1a990`) and puts it in the inside world (`_area_interior_createblock`). The room's key packs together the four side codes, the floor and ceiling bits, and the cell type. The inside world is never drawn from outside the building.

### The outside shell

Each solid cell makes one face for each bit of its world word. `World_SetTileBlockAndResolveNeighborLinks @f830` only clears the bits shared with solid neighbours, so side codes never cut holes in the shell. Instead, `_generatematerialwindowtexture @9a1a1` makes one texture-array layer for each combination of wall texture, buildblock, and layer. That layer is the wall texture with alpha set to 1 inside the layer's bounds rectangle. Window and door records are not read. `array_world.frag` uses the alpha as window shine, so from outside, the whole bounds rectangle looks like a reflective opening, door layers included.

### Rooms

Side code 1 is a plain wall (`_block_addbasicsurface`). Codes 3 to 15 go through `BuildBlock_AddBeams @1a1c0`:

- The bounds rectangle is the opening. Up to four boxes frame it (`_block_addbeam @19940`), each 1/16 deep from the cell edge. Their faces that point into the opening use slot 6.
- The outer face is only made for portal type 0.
- Beam records are boxes. Type 0 is flush and 1/16 deep. Type 1 starts at -1/16 and is 3/16 deep. Type 2 starts at -1/16 and is 5/16 deep. Any other type is a thin divider 1/64 deep at 1/128 (slot 7).
- `_block_addwindows @1a590` adds a glass quad for each window record at depth 1/64 (1/256 for portal type 1). It only does this when the portal type is 0, or when the side is north or east.
- The x start mirrors to `1 - (x + width)`, except for portal type 1 on the south and west sides.

So door layers (portal type 1, with bounds and no windows) are open doorways from inside and a reflective rectangle from outside.

### Near and far

`_render_buildsectorlist @11cc30` counts a sector as near when its centre is in view and within 192 units (36864 squared). For near sectors, the game builds and draws the inside world, and skips every outside face whose texture is the window-array texture (`DAT_64cff7e4`, flag `+0x10`). `BuildCellRenderGeometryAndAdjacency` gives that texture to faces with a buildblock side code. So up close, the rooms, open doorways, and see-through glass take the place of the reflective shell, while plain walls and roofs stay. Pafta decides per placed building (near when the closest point of its bounds is under 192 units away) and draws room glass with blending.

### Side codes

`Sbb_BuildBuildingConnectivityMetadata @29120` works out side codes, and both the shell and the rooms use them:

- north is the cell's own `+0x0c`;
- west is the cell's own `+0x10`;
- east is the `+0x10` of the `(x+1)` cell;
- south is the `+0x0c` of the `(z+1)` cell.

A missing neighbour gives code 0.

### Room glass

RenderWorld draws the glass quads after the water pass, from far to near, with blend `(ONE, ONE_MINUS_SRC_ALPHA)`, no depth writes, and no culling. It uses the glass program (vertex `@0x2f9820`, fragment `@0x2f9a10`). With `n` as the quad normal and `v` pointing at the camera:

```text
scale2 = min(0.5 + 0.5*(1-|v.n|)^3, 1)
scale  = clamp((dist-32)*0.75/64, 0, 1) * (1-scale2)
```

Front faces use the cubemap reflection, and back faces use grey `0.03125`. Then `rgb = c*scale2 + scale*ambient.b*0.5` and `a = scale2 + scale`, times 0.25 on back faces. Pafta makes one quad per pane, facing out of its cell, so the front face is the outside. Pafta works out shadows after this pass by subtracting direct light, so the pane also blends into the direct-light target (scaling it by 1 - alpha) and leaves the packed depth of the surface behind it.

### Old district faces

`World_LoadMapAndInitializeTerrain @56980` passes `(texture != 0)` for old district faces, so only textures 0 and 1 are used. Texture 1 is `asphalt2.png`/`asphalt2_material.png`. Texture 0 is a 128x128 grey noise made in code (`Texture_GenerateProceduralNoise @99730`, material `0x60,0xc0,0`), not the CSX "concrete" texture. CSX textures start at index 4.

### Sun

In `Game_RunFixedStep`, the sun angle is `pi + t*2pi/5616000` when the game type is 4 (World), and `pi + t*2pi/5184000` otherwise. `t` is the clock that `DrawGameInfo` shows as hour `t/216000`; `_resetgame` starts it at `0x244140`, which is 11:00. The server sends `sunTime` and the game type. The client reads the type as 4 bits, so World-based modes with `type = 4 + 16` (Ultimate, rosaworld) run the 26-hour World sun against a 24-hour clock. RenderWorld turns `normalize(0,1,0.5)` around `(0,0.447,-0.894)` with `Math_Vec3RotateAroundAxis @7b4d0`, which rotates by `-angle` (raylib rotates the other way). Pafta uses the World cycle.

### Old district cells and buildings

In version 10 maps, the legacy loop in `World_LoadMapAndInitializeTerrain` places district cells before the buildings. `InstantiateSbbVolumeIntoWorldAndMetadata` then writes every building cell that has an exterior block word over the world cell. So district cells under a building disappear, and the building's solid cells hide the faces of district cells next to them. Newer city blocks are placed after the buildings.

Version 12 cannot store version 10 district cells exactly. `RemapCityBuildingDataLayersToLoadedMaterials @e6960` looks up each layer name in the texture name list (the last match wins; slot 0 is "concrete", slots 1-3 have no name, and CSX names start at 4). The asphalt2 slot that every non-zero old grid entry uses has no name. So Pafta keeps a map with old district cells at its own version (saving it with `rebuild_sbc_layout`), and only upgrades it when you agree to a piece edit.

### Shadows

`RenderWorldOffscreenPrepassPipeline` calls `RenderQueuedShadowVolumeAdjacencyPass @946d0`. Its geometry shader only stretches triangles that face away from the sun, in three groups:

- outside-world sectors (`@11c950`), stretched `(sector y + 1) * 32 / max(sun.y, 0.125)`;
- the room models of near sectors, stretched a fixed 32 units (`0x2787d4`), so a room's ceiling and sun-side walls shade its own floor;
- flagged queued models (`RenderQueuedModelWithAdjacencyGeometryPass @124fe0`), 64 units.

So room floors are mostly in their own shadow, which hides outside shadow casters there (signs, overhangs). Only the floor that the slanted 32-unit room shadows miss, such as just inside a window, is lit. Room glass is a separate list of alpha quads and casts no shadow.

### Room key details

In `InstantiateSbbVolumeIntoWorldAndMetadata`, an open side becomes code 2 when a neighbour's wall ends at that side's starting corner. For north, that is the west cell's north wall or the north cell's west wall; the other sides follow by rotation. Code 2 adds two full-height strips, 1/16 wide, to fill the gap in the corner.

The ceiling bit comes from `Sbb_BuildBuildingConnectivityMetadata @29120`. It is set on the top layer, or when the cell above has no exterior block, no interior block, or its own floor.

Room wall slots turn with the side (`_block_addbeam`). Slots 0..7 index the inside set and 8..15 the outside set. North, east, south, and west walls use inside slots 1, 2, 3, and 4. Their outer faces use outside slots 3, 4, 1, and 2. Frames use slot 6 and dividers use slot 7.

### Placement along a wall

`_block_addwindows` turns its wall frame `(x, z) -> (1 - z, x)` for each side, so it runs backwards on south and west walls. The outside mask follows the face tangents (south +x, west +z, north -x, east -z). In world terms, the result is:

- type-0 rectangles are mirrored (`1 - (x + width)`) on north and east walls only, both outside and inside;
- type-1 room rectangles are mirrored on every wall.

Solid cells make their bottom face unless there is a solid cell below (`World_SetTileBlockAndResolveNeighborLinks @f830`), so you can see the underside of overhangs.

## Editor preview versus world rendering

The original editor's preview uses its own GLSL 3.30 shader, set up by `InitPreviewTextureColorShader @ 0x1400779f0`. Its known uniforms are:

```text
modelviewprojectionmatrix
lightdirection
modelmatrix
clipplane
tex_color (bound to texture unit 0)
```

`SetPreviewLightDirectionsFromAxes @ 0x140075f30` rebuilds the preview transform from the two light-axis values and the current preview yaw and target. This is not the game's city material shader. Neither the editor's fixed-function world view nor its preview shader tells you how the game finally lights things.

## Gaps left in the port

These can be worked on directly:

- Use both outside and inside pieces in every world and building draw path that matches the game's face-group calls.
- Keep the four-way piece rotation from bits 24-25.
- Match the 5-float position/UV vertex layout and the per-texture runs.
- Centre the volume exactly once. Do not also subtract the anchor.
- Use all six face directions for block templates, but only make faces 1..5 for solid cells, as the game does.
- Treat windows as openings or masks until the game's wall-mask path is traced. A solid window surface is wrong.
- Decode the `.sbb` LOD group entries using the version-14 layout above. Pafta keeps the bytes but does not show each entry yet.
- Add adjacency or exact triangle outlines before claiming building shadows match the game. Shadow casters made from bounding boxes cannot reproduce roofs, beams, or window cutouts.
