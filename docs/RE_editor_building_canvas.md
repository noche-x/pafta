# editor.exe Building Canvas Notes

**Source binary:** `editor.exe`  
**Fresh Ghidra program:** `/codex_temp/editor.exe`  
**Source SHA-256:** `957af3bd6ea6e990aecbd1f9ba49887a7d1312bee8f6931b12c272087f4608eb`

**Analysis dates:** 2026-08-24, 2026-10-11

## Main render path

`BuildingTool_CanvasRender @ 0x140065b40` draws the building mesh and a separate set of editing guides:

- a 64-cell floor grid, represented by 65 lines in each axis;
- a stronger grid line every fourth cell;
- editable extent guides;
- a cursor cross on its floor;
- a preview-camera marker;
- portal-wall, itemset-cell, agent-path, and brush-volume guides for their matching tool modes.

The canvas sets its work plane to `floor * 4`. All cell positions use the native four-world-unit cell size.

`BuildBuildingBlockRenderBuffers @ 0x1400799d0` builds the visible cell faces. It checks neighbor occupancy before it emits faces 1 through 5, then offsets generated vertices by the live volume center. The port now emits face 5 for an exposed solid-cell top and suppresses it when the cell above is solid. The building-preview origin still needs a field-level mapping before it changes.

## Port status

The Buildings viewport now draws the proven 65-line grid and fourth-line cadence in the same anchor-relative coordinate space as its mesh and picker. It also draws the saved SBB footprint as a red work bound. Its floor selector uses `size_z`, which is the parsed SBB height. The old selector used `size_y`, which is map depth. Thumbnail framing, selection focus, and city frustum bounds now use the same axis mapping.

The Building editor now has an item-set tool. It edits the packed item-set word
on a clicked block, offers the building's item-set name table, and draws a
yellow placement guide for nonzero words. It discovers loose `.sis` names in
the map's `itemset/` folder or the adjacent `data/itemset/` folder. The game
loads the `.sis` definition and its `.it3` items separately; Pafta does not
render those item meshes or author `.sis` files.

The native `FUN_140059150 @ 0x140059150` reads item-set definitions, and
`FUN_14007be50 @ 0x14007be50` resolves each placed set's low 10 name-index
bits and bits 24-25 for rotation. The latter bit operations were checked in
the instruction bytes at `0x14007be90`-`0x14007bea6`. Shipped SBB files
confirm the `0x40000000 | rotation << 24 | index` form. The new tool's click
flow and yellow guide are Pafta interface choices, not claimed native UI
parity.

The port still has no native-equivalent portal or agent tool mode. The native
cursor cross also depends on a free grid-plane cursor; the current picker
only identifies occupied cells or hit faces. Their markers remain open.

The preview-camera marker also remains open because its per-view runtime fields are not represented in the file model.
