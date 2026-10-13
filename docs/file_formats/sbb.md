# SBB building format

An SBB file holds one building template. It has a name, the size of the building, its own lists of asset names, a fixed-size record for each cell, and, in version 14, an optional LOD section at the end. The parser reads versions 12, 13, and 14.

## Basic types

| Name | Size | Meaning |
|---|---:|---|
| `u16` | 2 | unsigned integer |
| `u32` | 4 | unsigned integer |
| `ascii[64]` | 64 | fixed-length ASCII string |

## Header

| Offset | Type | Field | Versions | Evidence |
|---:|---|---|---|---|
| `0x00` | `u32` | version | 12-14 | parser and writer |
| `0x04` | `ascii[64]` | building name | 12-14 | parser and writer |
| `0x44` | `u32` | `size_x` | 12-14 | parser and writer |
| `0x48` | `u32` | `size_y` | 12-14 | parser and writer |
| `0x4c` | `u32` | `size_z` | 12-14 | parser and writer |
| `0x50` | `u32` | material count | 12 | parser and writer |
| `0x50` | `u32` | anchor X | 13-14 | parser and writer |
| `0x54` | `u32` | anchor Y | 13-14 | parser and writer |
| `0x58` | `u32` | anchor Z | 13-14 | parser and writer |
| `0x5c` | `u32` | material count | 13-14 | parser and writer |

Version 12 does not store an anchor, so Pafta uses `(4, 4, 4)`.

The size fields do not line up one-to-one with the loop axes. The cells are stored like this:

```text
width  = min(64, size_x + 1)
depth  = min(64, size_y + 1)
height = min(64, size_z)
count  = width * depth * height
```

Cells are ordered with X changing fastest, then Z, then Y.

Pafta rejects `size_x` or `size_y` above 63 and `size_z` above 64, so the grid fits in the 64-cell space it supports. It needs every name list and the whole cell array to be present. Empty slots in the lists are kept so the indices stay the same. The piece, buildblock, and item-set lists can each hold up to 1024 names.

## Name lists

The material list starts at `0x54` in version 12 and at `0x60` in versions 13 and 14. It holds `material_count` `ascii[64]` names in a row.

Three more lists come after it. Each one starts with a `u32` count, followed by that many `ascii[64]` names:

1. piece mesh names
2. buildblock template names
3. item-set names

Piece indices point into the piece list. `cell_type` points into the buildblock list. The low 10 bits of the item-set word point into the item-set list. That list starts at index zero, but a word of zero means there is no item set.

## Cell record

Each cell is `0x3c` bytes.

| Relative offset | Type | Field | Evidence |
|---:|---|---|---|
| `0x00` | `u32` | exterior block word | game's save order; parser and writer |
| `0x04` | `u32` | interior block word | game's save order; parser and writer |
| `0x08` | `u32` | cell type | game's save order; parser and writer |
| `0x0c` | `u32` | exterior portal-face code | game's save order; parser and writer |
| `0x10` | `u32` | interior portal-face code | game's save order; parser and writer |
| `0x14` | `u32` | placed flags | game's save order; parser and writer |
| `0x18` | `u16[16]` | material slots | parser and writer |
| `0x38` | `u32` | item-set word | game's save order; parser and writer |

The item-set word packs several values, even though the parser calls the field `item_set_index`. Placed sets in the buildings that ship with the game use `0x40000000 | (rotation << 24) | name_index`. The renderer in `editor.exe` reads `(word >> 24) & 3` as the number of quarter turns and `word & 0x3ff` as the index into the name list. A word of zero removes the set. Pafta keeps any other value as it is when reading and writing, and its item-set tool writes the form above.

Material slots `0..5` are the outside faces, and slots `8..13` are the inside faces. Slots 6, 7, 14, and 15 are in the file, but nobody has proven what the game uses them for.

The order in the file is different from the game's `BuildingCell` layout in memory. In memory, `cellType` is at `+0x00`, followed by the exterior and interior words at `+0x04` and `+0x08`. Do not copy the first six file words straight into the game's structure.

## Block words

The top three bits sort exterior and interior words into kinds:

| Masked value | Meaning | Index bits |
|---:|---|---|
| `0x00000000` | empty, or some value not yet sorted out | unknown |
| `0x40000000` | solid or buildblock cell | Pafta keeps the low 16 bits |
| `0x80000000` | a single SBL mesh (a piece) | low 10 bits |

An interior word of `0xffff` means the cell has no interior block. In the game's renderer, bits 24 and 25 (`(word >> 24) & 3`) give the piece's rotation. The other bits need more evidence from the game's code.

In the building-tool preview, bit 0 of the placed flags asks for a floor and bit 1 asks for a ceiling. The game's building renderer works out which faces show from the solid cells around each cell, so this preview rule is not the full story at runtime.

## Version-14 end section

After the cells, version 14 may have an LOD section. Pafta keeps every byte that is left and only reads:

| Relative offset | Type | Field |
|---:|---|---|
| `+0x00` | `u32` | marker |
| `+0x04` | `u32` | group count, only when the marker is 1 |

Nobody has decoded the group records yet. Anything that rewrites the file must keep this section byte for byte until its layout is known.

## How Pafta writes it

`rebuild_sbb` writes the parsed header, all four name lists, every `0x3c`-byte cell, and the kept LOD section. Patch commands change cells in place and leave unknown bytes alone. New names longer than 64 bytes are cut short.
