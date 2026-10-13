# CSX asset-archive format

A CSX file packs SBL, SBB, texture, and unknown files into one archive. The file data comes first. A directory of fixed-size entries sits at the offset given in the header.

## Header

| Offset | Type | Field |
|---:|---|---|
| `0x00` | `u32` | magic or container value |
| `0x04` | `u32` | directory offset |
| `0x08` | `u32` | directory entry count |

Pafta's parser stores the first word but does not check it. The game's loader requires `0xc5f17e01`, and map Save writes that value.

## File data

Each entry stores an absolute offset into the file, so file data can sit anywhere, before or after the directory. Pafta's writer puts all the data in one block starting at `0x0c`, in directory order, and then writes the directory.

SBL and SBB records inside an archive can have the value 1 in their first word, which marks them as archive entries. When loading a project, Pafta treats SBL entries as version 2 and SBB entries as version 14 before handing them to the normal parsers. This only happens while loading from an archive; the archive bytes stay the same.

## Directory entry

Each entry is `0x40` bytes.

| Relative offset | Type | Field |
|---:|---|---|
| `0x00` | `u32` | type |
| `0x04` | `u32` | absolute data offset |
| `0x08` | `u32` | data size in bytes |
| `0x0c` | `ascii[52]` | entry name |

Known types:

| Value | Contents |
|---:|---|
| `0x585a0001` | SBL block or buildblock template |
| `0x585a0002` | SBB building |
| `0x585a0004` | texture |

Any other type stays a raw entry. The parser copies its bytes without reading them.

## Range checks

The parser only reads an entry when both of these hold:

```text
payload_offset <= file_size
payload_size <= file_size - payload_offset
```

A bad data range or a cut-off directory rejects the whole archive. The directory offset must be at least `0x0c`, and the full directory must fit in the file before the parser allocates its entry list. The parser never fills in empty data or accepts part of a directory.

## How the game finds it

`World_LoadMapAndInitializeTerrain` (`@56980`) switches into `data/<map>`, and `Csx_OpenAndIndexAssetArchive` (`@1979d0`) opens the file named `test.csx`. It refuses the file unless the first word is `0xc5f17e01`. The game reads no other archive. So Pafta saves the archive as `test.csx` next to `city2.sbc` (the only map name the game loads), and when a folder has a `test.csx`, Pafta loads only that one.

Buildblock templates never come from the archive. `BuildblockTemplate_LoadFromSbl` (`@27200`) reads loose `buildblock/<name>.sbl` files (header 1). Pafta writes buildblocks there, not into the archive.

## How Pafta writes it

`build_csx` works out every data offset, data size, the directory offset, and the entry count again. It keeps the archive's first header word, and each entry's type, name, bytes, and directory position. It does not keep gaps or the original order of the data.

Map Save starts by making a new archive with `project_archive`. That keeps the texture and unknown entries from the loaded archives, replaces the building and block entries Pafta understands, and adds the loose map textures. It sorts entries by asset type and writes the first word the game needs. Buildblocks stay as loose files. These rules are not the same as calling `build_csx` on an archive that was parsed and left alone, and neither path promises the exact bytes of the original archive.

## Allocation limits

Pafta checks every payload range before copying entries. The sum of copied payload sizes must not exceed the input file size or 512 MiB, whichever is smaller. This is an editor memory limit, not a recovered native-format rule. A rejected archive reports the limit before allocating its payload copies. The writer applies the same 512 MiB payload limit, so it cannot produce an archive the reader refuses for exceeding that limit.

The editor retains loaded texture and unknown-entry payloads for subsequent saves. Moving or deleting the source archive does not remove those assets from the open project.
