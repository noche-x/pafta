# SBC city-map format

An SBC file holds the road network, the placed buildings, the city-block definitions, portal voxel regions, and portal connections. Pafta reads versions 7 to 12. Which sections exist depends on the version, so a reader has to go through the file in order.

## Section order

```text
version
version 10+: unknown name table
intersections
streets
buildings
version 11+: city-block definitions
version 8+: portals
version 9+: portal connections
```

Every count is a `u32`. Every number in a record is little-endian.

## Header and unknown name list

| Offset | Type | Field |
|---:|---|---|
| `0x00` | `u32` | version |

Versions 10 to 12 follow this with a count and that many `ascii[64]` values. Pafta keeps this section when editing the layout but does not show it. What the game uses it for is unknown.

## Intersections

This section starts with a count. Each record is `0x0c` bytes.

| Relative offset | Type | Field |
|---:|---|---|
| `0x00` | `i32` | X cell coordinate |
| `0x04` | `i32` | Y cell coordinate |
| `0x08` | `i32` | Z cell coordinate |

The renderer multiplies these by the city unit size.

## Streets

This section starts with a count. Each record is `0x34` bytes.

| Relative offset | Type | Field |
|---:|---|---|
| `0x00` | `i32` | first intersection index |
| `0x04` | `i32` | second intersection index |
| `0x08` | `i32` | district or other data |
| `0x0c` | `i32` | width on the first side |
| `0x10` | `i32` | width on the second side |
| `0x14` | `ascii[32]` | street name |

The original editor's overlay uses the two intersection indices directly and uses the widths to widen the ends. What the `0x08` value means is still not proven.

## Placed buildings

This section starts with a count. Each record is `0x50` bytes.

| Relative offset | Type | Field |
|---:|---|---|
| `0x00` | `ascii[64]` | SBB asset name |
| `0x40` | `i32` | X cell coordinate |
| `0x44` | `i32` | Y cell coordinate |
| `0x48` | `i32` | Z cell coordinate |
| `0x4c` | `i32` | rotation in quarter turns |

The renderer uses the rotation modulo four.

## City-block definitions

Only versions 11 and 12 have this section. It starts with a count, and each definition holds:

| Order | Type | Field |
|---:|---|---|
| 1 | `ascii[64]` | block mesh name; empty means the game's `nblock` display name |
| 2 | `u32` | layer count (version 12 only) |
| 3 | `ascii[64][8]` per layer | eight material or texture names |

Version 11 always has one layer. Pafta rejects more than 64 layers and needs every name to fit in the file. The limit of 64 is Pafta's own; nobody has shown the game has the same limit.

## Portals

Versions 8 to 12 have portals. Each portal starts with four `i32` fields, which Pafta shows without proper names yet. The scene code treats fields 1, 2, and 3 as the X, Y, and Z of the portal region. Field 0 still needs a clean trace through the game's code.

What comes after those fields depends on the version:

| Version | `u32[512]` blocks | `u32[512]` old-style blocks | `u16[4096]` textures | `u32[512]` item sets |
|---:|:---:|:---:|:---:|:---:|
| 8-9 | no | yes | yes | no |
| 10 | no | yes | yes | yes |
| 11-12 | yes | no | no | yes |

Both block arrays cover an `8 x 8 x 8` grid of voxels. The index of a voxel is:

```text
index = local_x + local_z * 8 + local_y * 64
```

Block words use the same top-bit kinds as building cells: `0x40000000` is a solid cell and `0x80000000` is a mesh. What the other bits mean depends on the city-block catalog, and still needs a full description from the game's code.

The old texture array has eight `u16` values per voxel. The exact meaning of each slot is not proven yet.

## Portal connections

Versions 9 to 12 end with a count and that many `0x0c`-byte records, each holding three `i32` values. The parser keeps them as raw fields because nobody knows yet what the game does with them.

## How Pafta writes it

The parser needs every section the version calls for, and every counted record has to fit in the file. A cut-off section is never treated as empty or partly filled.

`rebuild_sbc` always writes version 12. It writes an empty unknown name list, then every section Pafta understands. `rebuild_sbc_layout` keeps the original version and keeps everything after the intersections, streets, and buildings as it was.

Patches that edit in place leave unrelated bytes alone. Rebuilding portals rewrites the whole portal section, because a different portal count moves every later offset, and then adds the original connection section back on the end.

## Coordinate safety

Pafta converts portal sector coordinates to cell coordinates with checked 64-bit arithmetic. It rejects coordinates whose eight-cell sector and immediate neighbours would overflow a signed 32-bit cell coordinate. Scene construction and picking use the same checked conversion for in-memory edits. This is an arithmetic safety check; it does not prove a coordinate fits the game's world storage.
