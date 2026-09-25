# SBB building-volume format

SBB stores one building template: its name, volume dimensions, local asset
tables, fixed-size cell records, and an optional version-14 LOD tail. The parser
accepts versions 12, 13, and 14.

## Scalar types

| Name | Size | Meaning |
|---|---:|---|
| `u16` | 2 | unsigned integer |
| `u32` | 4 | unsigned integer |
| `ascii[64]` | 64 | fixed ASCII string |

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

Version 12 has no stored anchor. The editor supplies `(4, 4, 4)`.

The dimensions do not map directly to the loop axes. Cell storage uses:

```text
width  = min(64, size_x + 1)
depth  = min(64, size_y + 1)
height = min(64, size_z)
count  = width * depth * height
```

Cell order is X-fastest, then Z, then Y.

## Name tables

The material table starts at `0x54` for version 12 and `0x60` for versions 13
and 14. It contains `material_count` consecutive `ascii[64]` values.

Three counted tables follow it. Each table starts with a `u32` count followed
by that many `ascii[64]` values:

1. piece mesh names
2. buildblock template names
3. item-set names

Piece payload indices select the piece-name table. `cell_type` selects the
buildblock-name table. `item_set_index` selects the item-set table.

## Cell record

Each cell occupies `0x3c` bytes.

| Relative offset | Type | Field | Evidence |
|---:|---|---|---|
| `0x00` | `u32` | exterior block payload | native save order; parser and writer |
| `0x04` | `u32` | interior block payload | native save order; parser and writer |
| `0x08` | `u32` | cell type | native save order; parser and writer |
| `0x0c` | `u32` | exterior portal-face code | native save order; parser and writer |
| `0x10` | `u32` | interior portal-face code | native save order; parser and writer |
| `0x14` | `u32` | placed flags | native save order; parser and writer |
| `0x18` | `u16[16]` | material slots | parser and writer |
| `0x38` | `u32` | item-set index | native save order; parser and writer |

Material slots `0..5` cover exterior faces. Slots `8..13` cover interior
faces. Slots 6, 7, 14, and 15 exist on disk, but their exact native use remains
unproven.

The file order differs from the native in-memory `BuildingCell` layout. Native
evidence places `cellType` at memory offset `+0x00`, then exterior and interior
payloads at `+0x04` and `+0x08`. Do not copy the first six file words directly
into the native structure.

## Payload words

The high three bits classify exterior and interior payload words:

| Masked value | Meaning | Index bits |
|---:|---|---|
| `0x00000000` | empty or another unclassified value | unresolved |
| `0x40000000` | solid/buildblock cell | low 16 bits retained by the editor |
| `0x80000000` | standalone SBL mesh instance | low 10 bits |

`0xffff` in the interior word means no interior block. Piece rotation uses bits
24 and 25 (`(word >> 24) & 3`) in the native rendering path. Other payload bits
need more native-code evidence.

Placed-flag bit 0 requests the floor surface in the building-tool preview; bit
1 requests the ceiling surface. The runtime building renderer derives exposed
faces from neighboring solid cells and does not treat this preview rule as the
complete runtime rule.

## Version-14 tail

After the cell array, version 14 may contain an LOD section. The editor
currently preserves every remaining byte and reads only:

| Relative offset | Type | Field |
|---:|---|---|
| `+0x00` | `u32` | marker |
| `+0x04` | `u32` | group count, only when marker equals 1 |

The group records have not been decoded. Rewriters must preserve this tail
byte-for-byte until its layout is proven.

## Writer behavior

`rebuild_sbb` emits the parsed header, all four name tables, every `0x3c` cell,
and the preserved LOD tail. Patch operations update cells in place and leave
unknown bytes untouched. New strings longer than 64 bytes are truncated.

