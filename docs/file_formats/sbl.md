# SBL block and buildblock format

The `.sbl` extension is used for two different kinds of file. A block SBL holds a mesh that can be drawn. A buildblock SBL holds up to 16 opening layers of varying length (doors, windows, and beams). Both start with a `u32`, and no known magic value tells them apart. Pafta treats a file as a block only when the first word is 1 or 2 and the next three words are sizes between 1 and 64.

## Block header

| Offset | Type | Field |
|---:|---|---|
| `0x00` | `u32` | version, 1 or 2 |
| `0x04` | `u32` | width in cells |
| `0x08` | `u32` | height in cells |
| `0x0c` | `u32` | depth in cells |
| `0x10` | `u32[6]` | outside-face flags |
| `0x28` | `u32` | patch count |

Each outside-face flag that is not zero adds one or more flat faces sized from the block's dimensions. The six positions follow the game's face-direction table. The names for each direction should come from tracing the game's loader, not from guessing based on the editor's cube helper.

## Patch

Each patch is `0x158` bytes.

| Relative offset | Type | Field |
|---:|---|---|
| `0x00` | `u32` | column order |
| `0x04` | `u32` | column subdivision |
| `0x08` | `u32` | row order |
| `0x0c` | `u32` | row subdivision |
| `0x10` | `u32` | normal or material flag A |
| `0x14` | `u32` | normal or material flag B |
| `0x18` | `ControlPoint[16]` | Bezier control grid |

Each control point is 20 bytes:

| Relative offset | Type | Field |
|---:|---|---|
| `0x00` | `f32` | X |
| `0x04` | `f32` | Y |
| `0x08` | `f32` | Z |
| `0x0c` | `f32` | U |
| `0x10` | `f32` | V |

In the game's memory each record is `0x15c` bytes. The extra four bytes only exist at runtime, not in the file, so readers must step `0x158` bytes per patch.

Pafta rejects more than `0x400` patches or a patch that is cut off, and it caps the subdivision when building preview triangles. These are Pafta's limits, not confirmed limits of the game.

## Version-2 boxes

Version 2 stores a `u32` box count right after the patches. `BlockSbl_LoadFromFile @e6e50` only reads it when the version is above 1. These records are extra textured boxes, not lights: `Buildblock_InitTemplateMeshCatalog` hands each one to `BuildblockMeshBuilder_AddBoxFaces @19c10`. Pafta's code still calls them `SblBlockLight`. Each record is `0x7c` bytes:

| Relative offset | Type | Field |
|---:|---|---|
| `0x00` | `Vec3f[8]` | eight corners, 96 bytes: 0-3 are one ring, 4-7 are the ring above it |
| `0x60` | `u32` | material: the texture slot for its faces |
| `0x64` | `u32[5]` | read and kept, but not used to build the mesh |
| `0x78` | `u32` | side mask: bit n draws side n (63 = all six) |

Pafta needs every counted box to fit in the file and rejects more than 256 boxes.

For bits 0 to 5, the sides use corners `3 2 1 0`, `7 6 2 3`, `4 7 3 0`, `5 4 0 1`, `6 5 1 2`, and `4 5 6 7`. The game adds a UV projection to each side's material based on its normal (the component above 0.7071): +Y `0x40000`, -Y `0x30000`, +X `0x20000`, -X `0x10000`, +Z `0x60000`, and `0x50000` for anything else. The first word is the material and the last is the mask, so a record written as `63, 0, 0, 0, 0, 0, 0` has no sides.

A block's face materials (patch flag A and the box material) are texture slots 0 to 7 of the building cell that places the block. They are not global texture numbers. See `material_for_piece_face` in `scene.cpp`.

Inside a CSX archive, `BlockSblContainer_ReadWrite @1967e0` requires version 1 but always reads the box count and boxes. So an archived block uses the version-2 layout with 1 in the version field.

## Buildblock layers

If the file does not pass the block-header test, Pafta reads layers of varying length from offset `0x04` to the end of the file. It rejects more than 16 layers or a layer that is cut off. The first word is shown as the template version, but nobody knows which values are allowed or what the game does with it.

Each layer starts with:

| Relative offset | Type | Field |
|---:|---|---|
| `0x00` | `u32` | portal type |
| `0x04` | `f32` | bounds X |
| `0x08` | `f32` | bounds width |
| `0x0c` | `f32` | bounds Y |
| `0x10` | `f32` | bounds height |
| `0x14` | `u32` | door count |

The doors come right after, 12 bytes each:

```text
type u32, x f32, y f32
```

Next is a `u32` window count, and each window is 20 bytes:

```text
type u32, x f32, width f32, y f32, height f32
```

Then a `u32` beam count, with beams in the same 20-byte shape as windows. There is no byte count per layer, no padding, and no fixed step through the file.

Pafta rejects more than 2 doors, 16 windows, or 16 beams in one layer, and needs every counted record to fit in the file. The game stores layers in fixed-size structures in memory, but those memory offsets do not describe this file layout.

Side codes 3 to 15 pick `layers[side_code - 3]`. Portal type 1 changes how sides with a code above 1 are mirrored horizontally.

## How Pafta writes it

The patch functions change block headers, patches, or existing buildblock layer records directly in the original bytes. Bytes outside the changed ranges stay the same. They cannot add or delete records.

`write_sbl_block` writes a block as version 2, including its patches and boxes. Every patch needs 16 control points. Writing a whole block and patching bytes in place are separate paths.

Map Save uses `rebuild_sbl_buildblock` for loose buildblocks. It preserves the loaded header (new templates start with header 1) and writes the layers Pafta understands, with limits of 16 layers, 2 doors per layer, and 16 windows and 16 beams per layer. This rebuild does not keep bytes Pafta does not understand or anything left at the end. The editor can change existing opening records but cannot add or delete them.
