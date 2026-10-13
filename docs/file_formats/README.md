# Pafta file-format reference

These pages describe the binary layouts that `src/formats.cpp` reads and writes. All numbers longer than one byte are little-endian. Strings sit in fixed-size ASCII fields, end at the first NUL byte, and may have trailing spaces.

## Formats

| Format | What it is | Versions read | Page |
|---|---|---:|---|
| SBB | Building | 12 to 14 | [sbb.md](sbb.md) |
| SBC | City map | 7 to 12 | [sbc.md](sbc.md) |
| SBL | Block mesh or buildblock template | block 1 to 2; how to tell templates apart is unknown | [sbl.md](sbl.md) |
| CSX | Asset archive | no version check on the container | [csx.md](csx.md) |

[building_render_spec.md](building_render_spec.md) explains how SBB cells and SBL records turn into geometry. It does not replace the byte layouts above.

Game function names and addresses come from earlier research. The game's executables and decompiled code are not in this checkout. The [building-canvas note](../slop/RE_editor_building_canvas.md) gives the hash of the `editor.exe` it used; these pages do not name every binary. You can check Pafta's own code against its source and test files. Checking a rule against the game needs the same binary and the research behind it.

## How sure we are

- Confirmed by parser and writer: the editor reads and writes this field.
- Confirmed by parser: the editor reads the field but does not rebuild it.
- Recovered from native code: the section names the game function or address.
- Unknown: the bytes are kept or shown, but nobody knows what they mean.

The parsers reject records that are cut off and counts above what they support. Those limits are Pafta's, not the game's, and they do not mean every game file is supported. When a file is rejected during loading, the map can still open for inspection. If geometry failed to load, Save and archive rebuilding stay off until the file is fixed or removed and the map is reopened. Item-preview warnings do not stop Save.
