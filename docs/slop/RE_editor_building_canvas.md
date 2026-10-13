# editor.exe building canvas notes

Source binary: `editor.exe`

Fresh Ghidra program: `/codex_temp/editor.exe`

Source SHA-256: `957af3bd6ea6e990aecbd1f9ba49887a7d1312bee8f6931b12c272087f4608eb`

Analysis dates: 2026-08-24, 2026-10-11

## How the canvas draws

`BuildingTool_CanvasRender @ 0x140065b40` draws the building mesh and, on top of it, a set of editing guides:

- a 64-cell floor grid, drawn as 65 lines in each direction;
- a heavier line every fourth cell;
- guides for the size you can edit;
- a cross for the cursor on its floor;
- a marker for the preview camera;
- guides for portal walls, item-set cells, agent paths, and the brush, each shown in its own tool mode.

The canvas puts its working plane at `floor * 4`. Every cell position uses the game's cell size of four world units.

`BuildBuildingBlockRenderBuffers @ 0x1400799d0` builds the visible cell faces. It checks whether neighbouring cells are filled before adding faces 1 to 5, then shifts the vertices by the centre of the building. Pafta now adds face 5 on top of a solid cell when it is exposed and leaves it out when the cell above is solid. Where the building preview's origin comes from still needs to be matched field by field before it changes.

## State of the port

The Buildings viewport now draws the confirmed 65-line grid, with every fourth line heavier, in the same anchor-based coordinates as its mesh and picking. It also draws the saved SBB size as a red box. Its floor picker uses `size_z`, which is the SBB height. The old picker used `size_y`, which is the map depth. Thumbnail framing, focusing on a selection, and city view culling now use the same axes.

The Building editor now has an item-set tool. It edits the packed item-set word on the block you click, offers the building's list of item-set names, and draws a yellow marker for any word that is not zero. It finds loose `.sis` names in the map's `itemset/` folder or in a `data/itemset/` folder next to it. The game loads the `.sis` file and its `.it3` items separately. Pafta now draws untextured `.it3` previews when the meshes exist, and falls back to the yellow marker when they don't. It cannot create `.sis` files.

In the original editor, `FUN_140059150 @ 0x140059150` reads item-set files, and `FUN_14007be50 @ 0x14007be50` takes the low 10 bits of each placed set as its name index and bits 24-25 as its rotation. Those bit operations were checked in the instruction bytes at `0x14007be90`-`0x14007bea6`. The SBB files that ship with the game use the form `0x40000000 | rotation << 24 | index`. How Pafta's tool reacts to clicks and the yellow marker are Pafta's own design, not a copy of the original editor.

Pafta still has no portal or agent tool like the original editor. The original cursor cross also needs a cursor that can sit anywhere on the grid plane, while Pafta's picker only finds filled cells or the faces it hits. Both markers are still to do.

The preview-camera marker is also still to do, because the settings it uses for each view are not stored in the file.
