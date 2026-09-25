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

## Version-2 light section

Version 2 stores a `u32` light count immediately after the authored patches.
Each light record occupies `0x7c` bytes:

| Relative offset | Type | Field |
|---:|---|---|
| `0x00` | `Vec3f[8]` | eight points, 96 bytes |
| `0x60` | `u32[6]` | scalar or material words |
| `0x78` | `u32` | enabled-face mask |

Native `LoadAndInitBlockTemplates_SBL` uses the mask to select faces, takes the
material from the first scalar word, and derives orientation from each emitted
face normal. The remaining scalar meanings need more evidence.

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

