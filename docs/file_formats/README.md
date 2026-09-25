# Sub Rosa map-editor file formats

This directory records the binary layouts implemented by `src/formats.cpp`.
All multi-byte numbers use little-endian byte order. Strings occupy fixed-size
ASCII fields, end at the first NUL byte, and may contain trailing spaces.

## Coverage

| Format | Purpose | Versions parsed | Specification |
|---|---|---:|---|
| SBB | Building volume | 12 through 14 | [sbb.md](sbb.md) |
| SBC | City map | 7 through 12 | [sbc.md](sbc.md) |
| SBL | Block mesh or buildblock template | block 1 through 2; template discriminator unresolved | [sbl.md](sbl.md) |
| CSX | Asset archive | container layout has no checked version gate | [csx.md](csx.md) |

`building_render_spec.md` describes how SBB cells and SBL records become render
geometry. It is not a substitute for the byte layouts above.

## Evidence labels

- **Confirmed by parser and writer** means the editor reads and writes the same field.
- **Confirmed by parser** means the editor reads the field but does not rebuild it.
- **Recovered from native code** names the function or address in the relevant section.
- **Unknown** means the bytes are preserved or exposed without a proven meaning.

The editor accepts malformed counts defensively in several places. Those caps
protect the editor; they are not native format limits unless the specification
says otherwise.

