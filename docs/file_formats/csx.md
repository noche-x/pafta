# CSX asset-archive format

CSX combines SBL, SBB, texture, and unknown payloads in one file. Payloads come
first; a fixed-size directory sits at the offset stored in the header.

## Header

| Offset | Type | Field |
|---:|---|---|
| `0x00` | `u32` | magic or container value |
| `0x04` | `u32` | directory offset |
| `0x08` | `u32` | directory entry count |

The current parser records the first word but does not validate it. Its exact
native meaning remains unresolved.

## Payload area

Payload bytes may occur anywhere before or after the directory because each
entry carries an absolute file offset. The editor's writer emits every payload
contiguously from `0x0c`, in directory order, then writes the directory.

Embedded SBL and SBB records can carry a CSX sentinel value of 1 in their first
word. Project loading normalizes SBL payloads to version 2 and SBB payloads to
version 14 before calling the standalone parsers. This normalization belongs to
the container-loading path; it does not change the archive bytes.

## Directory entry

Each entry occupies `0x40` bytes.

| Relative offset | Type | Field |
|---:|---|---|
| `0x00` | `u32` | type |
| `0x04` | `u32` | absolute payload offset |
| `0x08` | `u32` | payload size in bytes |
| `0x0c` | `ascii[52]` | entry name |

Known type values:

| Value | Payload |
|---:|---|
| `0x585a0001` | SBL block or buildblock template |
| `0x585a0002` | SBB building |
| `0x585a0004` | texture |

Other values remain raw archive entries. The parser copies their payload bytes
without interpreting them.

## Bounds behavior

The parser reads an entry only when both conditions hold:

```text
payload_offset <= file_size
payload_size <= file_size - payload_offset
```

An invalid range yields an empty payload but leaves the directory metadata in
the parsed entry. A truncated directory stops parsing at the last complete
`0x40` record.

## Writer behavior

`build_csx` recalculates all payload offsets, payload sizes, directory offset,
and entry count. It preserves the archive's first header word, entry types,
names, payload bytes, and directory order. Gaps and the source payload order are
not preserved.

