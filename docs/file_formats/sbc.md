# SBC city-map format

SBC stores the road graph, placed buildings, district block definitions,
portal voxel regions, and portal connections. The editor accepts versions 7
through 12. Section presence changes with the version, so readers must advance
through the file in order.

## File order

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

All counts are `u32`. All numeric record members are little-endian.

## Header and unknown name table

| Offset | Type | Field |
|---:|---|---|
| `0x00` | `u32` | version |

Versions 10 through 12 then store a counted sequence of `ascii[64]` values.
The editor preserves this section during layout edits but does not expose its
contents. Its native purpose remains unknown.

## Intersections

The intersection section starts with a count. Each record occupies `0x0c`.

| Relative offset | Type | Field |
|---:|---|---|
| `0x00` | `i32` | X cell coordinate |
| `0x04` | `i32` | Y cell coordinate |
| `0x08` | `i32` | Z cell coordinate |

The renderer multiplies these cell coordinates by the city unit size.

## Streets

The street section starts with a count. Each record occupies `0x34`.

| Relative offset | Type | Field |
|---:|---|---|
| `0x00` | `i32` | first intersection index |
| `0x04` | `i32` | second intersection index |
| `0x08` | `i32` | district or metadata value |
| `0x0c` | `i32` | first-side width |
| `0x10` | `i32` | second-side width |
| `0x14` | `ascii[32]` | street name |

Native editor overlay evidence calls the two endpoint indices directly and
uses the width values to expand endpoint extents. The exact semantic name of
the `0x08` value remains unproven.

## Placed buildings

The building section starts with a count. Each record occupies `0x50`.

| Relative offset | Type | Field |
|---:|---|---|
| `0x00` | `ascii[64]` | SBB asset name |
| `0x40` | `i32` | X cell coordinate |
| `0x44` | `i32` | Y cell coordinate |
| `0x48` | `i32` | Z cell coordinate |
| `0x4c` | `i32` | quarter-turn rotation |

The current renderer treats the rotation as a value modulo four.

## City-block definitions

Versions 11 and 12 store this section. It starts with a count. Each definition
contains:

| Order | Type | Field |
|---:|---|---|
| 1 | `ascii[64]` | block mesh name; empty selects the native `nblock` display name |
| 2 | `u32` | layer count; version 12 only |
| 3 | `ascii[64][8]` per layer | eight material or texture names |

Version 11 has one implicit layer. The editor limits a parsed layer count to 64
as a corrupt-file guard; this is not a proven native limit.

## Portals

Versions 8 through 12 store portals. Each portal begins with four `i32` fields.
The editor currently exposes them without final semantic names. Current scene
code treats fields 1, 2, and 3 as the portal-region X, Y, and Z coordinates;
field 0 still needs a clean native trace.

The payload after those fields depends on the version:

| Version | `u32[512]` blocks | `u32[512]` legacy blocks | `u16[4096]` textures | `u32[512]` item sets |
|---:|:---:|:---:|:---:|:---:|
| 8-9 | no | yes | yes | no |
| 10 | no | yes | yes | yes |
| 11-12 | yes | no | no | yes |

Both block arrays describe an `8 x 8 x 8` voxel region. The linear index is:

```text
index = local_x + local_z * 8 + local_y * 64
```

The block word uses the same high-bit classes seen in building cells:
`0x40000000` marks a solid cell and `0x80000000` marks a mesh instance. The
meaning of the remaining bits depends on the district block catalog and still
needs a complete native description.

The legacy texture array has eight `u16` values per voxel. Exact slot names are
not yet proven.

## Portal connections

Versions 9 through 12 end with a counted array of `0x0c` records. Each record
contains three `i32` values. The parser preserves them as raw fields because
their native roles remain unresolved.

## Writer behavior

`rebuild_sbc` always emits version 12. It writes an empty unknown header table,
then every modeled section. `rebuild_sbc_layout` keeps the source version and
preserves the unmodeled tail after intersections, streets, and buildings.

In-place patch functions preserve unrelated bytes. Portal rebuilding rewrites
the whole portal section because portal count changes alter every following
offset; it then appends the original connection tail.

