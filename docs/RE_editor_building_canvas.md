# editor.exe Building Canvas Notes

**Source binary:** `editor.exe`  
**Fresh Ghidra program:** `/codex_temp/editor.exe`  
**Analysis date:** 2026-08-24

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

The current port has no native-equivalent portal, itemset, or agent tool mode. Their mode-specific markers are therefore not drawn. The native cursor cross also depends on a free grid-plane cursor; the current picker only identifies occupied cells or hit faces. Adding marker-only controls without the matching edit operation would give false feedback, so this remains open.

The preview-camera marker also remains open because its per-view runtime fields are not represented in the file model.
