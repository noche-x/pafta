# How editor.exe draws roads and intersections

Source binary: `editor.exe` (the Sub Rosa map editor, NOT `subrosa.exe`, the game)

Ghidra program: `/editor.exe`, image base `0x140000000`, x64 PE

Analysis date: 2026-06-07

Method: Ghidra MCP tools only (`decompile_function`, `disassemble_function`, `search_functions`, `search_strings`, `get_xrefs_to`, `get_function_callers`, `read_memory`, `convert_number`).

## Checked again on 2026-08-24

The old Ghidra project would not open after a processor-language version change, so `editor.exe` was imported and analysed again as `/codex_temp/editor.exe`. The new decompilation confirms the main finding of this page:

- `StreetPieceTool_CanvasRender @ 0x14006c400` calls the mesh-bucket pass and the street overlay pass separately.
- `BuildStreetToolMeshBuckets @ 0x140076e70` only collects world objects that already exist. It does not make road tiles.
- `RenderStreetToolNodeAndSegmentOverlays @ 0x14007c990` draws node markers at scale `2.0`, at the node's stored height `gridY * 4`.
- The same overlay function sends four vertices for each node footprint and street guide. `FUN_14003af20(1)` and `FUN_14003b410` turn each group of four into six indices and draw them as triangles, so these are filled quads, not centre lines.
- The node extent fields are known exactly: `+0x30/+0x38` are positive/negative X and `+0x34/+0x3c` are positive/negative Z. `FUN_14005ae20` grows them to fit the widest connected street, and `FUN_14005ab60` then adds one cell to every positive extent.
- `FUN_14005b540` cuts each street short at the grown footprints of its two ends and moves its two sides out by `(leftWidth + 1) * 4` and `(rightWidth + 1) * 4`.

Pafta keeps its two views apart. The Editor view hides the terrain and all road-tile geometry and draws the grown node and street guide quads at their stored height. The In game view draws the terrain and the generated road tiles, with no editor markers.

New checks on the game side also confirm that road tiles are made at runtime by the game. `/subrosa.exe.THIS_ONE` uses `MapConnector_PlaceStreetSegmentTilesAndSlopes @ 0x00020580` and its perimeter function at `0x00020130`. `/subrosa.x64` shows the older slope-block path separately, in `buildcitystreet @ 0x000866d2`.

## 1. How roads are drawn

editor.exe does NOT draw roads or intersections as placed voxel tiles, block meshes, or curved road strips. It keeps the street network as plain node and segment records in the SBC file and only draws them as guide quads and markers in the street tool's preview. No road geometry ever goes into the world voxel grid.

The game is what makes `0x40000000` solid cells and `0x80000000|code` block meshes for lane, curb, and intersection edge tiles.

What this means for `pafta`: the editor side should keep streets as plain node and segment data (the same as the SBC layout) and draw them as guide quads and markers. It should not place editor roads as solid voxels or block meshes, because the real editor does not. Making road tiles is the game's job (subrosa.exe), not the editor's.

## 2. Functions related to roads, streets, and intersections

| Function Name | Address | Role |
|---------------|---------|------|
| `EditorPanel_StreetPieceTool` | `0x14006a1c0` | UI panel for the combined Street/Building/Piece tool, with radio buttons for `Street`, `Building`, and `Piece`. |
| `StreetPieceTool_CanvasRender` | `0x14006c400` | Main drawing pass for the placement tool. Calls `BuildStreetToolMeshBuckets`, `RenderStreetToolMeshBuckets`, and `RenderStreetToolNodeAndSegmentOverlays`, and draws building previews and the ground grid. |
| `StreetPieceTool_CanvasUpdate` | `0x14006a870` | Input and update pass. Handles the camera, picking grid cells, and adding or deleting street nodes and segments. Calls `StreetTool_FindOrCreateStreetSegment` on click. |
| `StreetTool_UpdatePiecePlacementMode` | `0x14006ea00` | Handles the Piece mode (placing custom blocks). Calls `SetPortalVoxelData` and `UpdatePieceVoxelData`. Not used for streets. |
| `StreetTool_FindOrCreateNodeXZ` | `0x14006efe0` | Finds or makes a node at grid X/Z. |
| `StreetTool_FindNodeAtGrid` | `0x14006f0c0` | Finds the node at a grid position. |
| `StreetTool_FindOrCreateStreetSegment` | `0x14006f150` | Makes a new `SBC_StreetPiece` record between two nodes if they line up on an axis. Stores the widths in `pData[4]` and `pData[8]`. Does not place any geometry. |
| `StreetTool_DeleteStreetSegment` | `0x14006f650` | Removes a segment by swapping it with the last one. |
| `StreetTool_DeleteNodeAndAttachedSegments` | `0x14006f490` | Deletes a node and every segment attached to it. |
| `RayIntersectsStreetSegmentWithDistance` | `0x1400703c0` | Tests a ray against the line between two node positions. |
| `BuildStreetToolMeshBuckets` | `0x140076e70` | Groups world objects that already exist (buildings, pieces, terrain) by material for the preview. Does not include streets. |
| `RenderStreetToolMeshBuckets` | `0x140077450` | Draws the groups built above. |
| `RenderStreetToolNodeAndSegmentOverlays` | `0x14007c990` | The function that actually draws streets. Builds temporary node and segment guide quads, then draws them and the markers. |
| `RenderWidget_Or_StreetPiece` | `0x1400319b0` | General widget drawing, used by the tool UI. |
| `LoadCity_SBC` | `0x14005f160` | Loads SBC v7..v12. Reads `g_NodeCount`, `g_pNodesArray`, `g_StreetPieceCount`, `g_pStreetPiecesArray`. After loading, calls `UpdatePieceVoxelData()` and `ProcessAndPlacePortalVoxels()`. Never turns streets into voxels. |
| `SaveCity_SBC` | `0x14005e980` | Writes SBC v12. Saves nodes and street pieces as plain records. |
| `SetPortalVoxelData` | `0x14005d6f0` | Places tiles for portals and city pieces. Calls `SetVoxelCellData`. Not used for streets. |
| `SetVoxelCellData` | `0x140043a60` | The main voxel cell writer. Handles `0x40000000` (solid) and `0x80000000|code` (block mesh), and updates the faces of neighbouring cells. In the editor's data path, only `SetPortalVoxelData` calls it. |
| `UpdatePieceVoxelData` | `0x14005fc90` | Matches city-block names to loaded SBL block templates. Sets `nCount = 0x40000000` by default, or `0x80000000 | blockIndex` when a named `.sbl` template matches. |

What is missing: editor.exe has nothing like the game's `MapConnector_PlaceStreetSegmentTilesAndSlopes` or `MapConnector_PlaceIntersectionPerimeterTiles`. No function walks `g_pStreetPiecesArray` and calls `SetVoxelCellData`.

## 3. Road geometry in editor.exe

### 3.1 What the editor draws

The street tool draws two things:

1. Node markers: small boxes drawn with `RenderEditorMarkerModel` at each node's grid position.
2. Guide quads: four-vertex node footprints and street strips that take the width into account. The overlay buffer turns each one into two indexed triangles.

From `RenderStreetToolNodeAndSegmentOverlays @ 0x14007c990`:

- It walks `g_pNodesArray`, calls `FUN_14005a8a0` (node overlay setup), and then draws markers.
- It walks `g_pStreetPiecesArray` and calls `FUN_14005ae20` to grow each end's extents to fit the two street widths.
- `FUN_14005ab60` adds one cell to each positive extent.
- `FUN_14005b540` shortens and widens the street guide, and `FUN_14003b270` sends the quad's vertices.
- It never calls `SetVoxelCellData`, `SetPortalVoxelData`, or anything that places into the world grid.

Decompiled code, simplified:
```c
for (local_78 = 0; local_78 < g_NodeCount; local_78++) {
    FUN_14005a8a0((int)g_pNodesArray[local_78].flX,
                  (int)g_pNodesArray[local_78].flY,
                  (int)g_pNodesArray[local_78].flZ);
}
for (local_78 = 0; local_78 < g_StreetPieceCount; local_78++) {
    FUN_14005ae20(g_pStreetPiecesArray[local_78].nNodeIdx1,
                  g_pStreetPiecesArray[local_78].nNodeIdx2,
                  *(int *)(g_pStreetPiecesArray[local_78].pData + 4),
                  *(int *)(g_pStreetPiecesArray[local_78].pData + 8),
                  ...);
}
// ... then renders markers and indexed guide quads
```

### 3.2 What the editor does NOT draw

- Solid voxel cells (`0x40000000`) for road surfaces, curbs, or filled intersections.
- Block meshes (`0x80000000 | code`) for sloped lanes or curb blocks.
- Curved Bezier strips for bending roads.
- Real intersection edges: the node guide has a footprint, but there is no voxel fill, curb row, or corner piece.

The editor shows its street network with guides, not with real road tiles.

## 4. Intersections

In the editor, an "intersection" is just a node.

- The SBC file stores intersections in `g_pNodesArray` (12-byte records: `{flX, flY, flZ}`).
- `StreetTool_FindOrCreateStreetSegment` makes segments between nodes.
- editor.exe has no separate code for building intersection geometry.
- The widths of connected streets grow four extent fields on the node's scratch record. Every positive extent then grows by one more cell.
- The overlay draws the resulting node footprint as a filled quad. This is a guide, not world geometry.
- There are no voxel edges, curb rows, or corner pieces.

On screen, a node is a `RenderEditorMarkerModel` marker at `(gridX * 4.0, gridY * 4.0, gridZ * 4.0)` plus the grown guide quad at the same height.

## 5. Slopes and heights

Since the editor does not place road tiles, it also has none of these:

- Stepping slopes over a run of cells.
- Looking up sloped block meshes in a table.
- Whole-number height changes between intersections.
- Patching terrain height.

The editor stores each node's `flY` as a whole-number grid level in the SBC file. It is used to:

1. Place the node marker in the world (`worldY = gridY * 4.0`).
2. Place each end of the node and street guide quads.
3. Save the height in the SBC file.

Float constants used by the street tool, decoded:

| Address | Raw bytes | IEEE-754 float | Usage |
|---------|-----------|----------------|-------|
| `DAT_1400ec9b4` | `00 00 80 40` | 4.0 | Grid cell size in world units. |
| `DAT_1400ec9c0` | `00 00 00 41` | 8.0 | Camera distance multiplier. |
| `DAT_1400ec9b0` | `00 00 70 40` | 3.75 | Wireframe box size in the piece brush preview. |
| `DAT_1400ec990` | `00 00 00 40` | 2.0 | Marker model scale. |
| `DAT_1400ec900` | `00 00 00 3e` | 0.125 | Camera Y offset fraction. |
| `DAT_1400eca0c` | `00 00 80 43` | 256.0 | UI scroll-list height. |
| `DAT_1400eca20` | `00 00 00 44` | 512.0 | UI dropdown height. |

editor.exe has no DAT_ floats matching the game's slope-table constants (`DAT_0500ba14`, `DAT_0500aa90`, and so on), because those tables are not in the editor binary.

## 6. What the editor does place as tiles

Streets are not placed as tiles, but the editor does place tiles for city pieces, portals, and buildings through a clear API. `pafta` may need to copy it for the Piece tool.

### 6.1 Entry point

```c
void SetPortalVoxelData(int layer, uint gx, uint gy, uint gz, uint packedValue);
```

Address: `0x14005d6f0`

`packedValue` holds:
- Bits `0..11` (`& 0xfff`): city block index.
- Bits `16..21` (`>> 16 & 0x3f`): layer count minus one (`(layerCount + 1) * 0x10000`).
- Bits `24..25` (`>> 24 & 3`): rotation (0..3).

`SetPortalVoxelData` calls `SetVoxelCellData(layer, gx, gy, gz, blockValue, faceArray[8])`.

### 6.2 What block values mean

From `SetVoxelCellData @ 0x140043a60`:

```c
if ((blockValue & 0xe0000000) == 0x40000000) {
    // Solid voxel cell
    // Stores faceArray[8] as 8 uint16 texture/face codes
    // Neighbor-link resolution: clears adjacent faces on neighboring solid cells
} else if ((blockValue & 0xe0000000) == 0x80000000) {
    // Block-mesh instance
    // blockValue & 0xffff = index into loaded SBL block template array
    // Also fills multiblock occupancy with 0xc0000000 | offset markers
}
```

### 6.3 Matching city blocks to templates

From `UpdatePieceVoxelData @ 0x14005fc90`:

```c
g_pCityBlocksArray[i].nCount = 0x40000000; // default: solid voxel
if (g_pCityBlocksArray[i].pHeader[0] != 0) {
    for (blockIdx = 0; blockIdx < g_BuildingTemplateCount; blockIdx++) {
        if (strcmp(cityBlockName, buildingTemplateName) == 0) {
            g_pCityBlocksArray[i].nCount = blockIdx | 0x80000000; // block mesh
        }
    }
}
```

In the editor, a city piece starts as a solid voxel (`0x40000000`) with face texture lists. It only becomes a block mesh (`0x80000000|idx`) if the city block's name matches a loaded `.sbl` template.

## 7. Compared with subrosa.exe

| Aspect | subrosa.exe (game) | editor.exe (map editor) |
|--------|--------------------|--------------------------|
| Street storage | Runtime `streetIntersection` and `street` structs with extents, neighbours, and widths. | SBC `g_pNodesArray` and `g_pStreetPiecesArray` (plain records). |
| Placing road tiles | `MapConnector_PlaceStreetSegmentTilesAndSlopes` writes `0x40000000` and `0x80000000\|code` tiles into the world grid. | None. There is no function that places road tiles. |
| Filling intersections | `MapConnector_PlaceIntersectionPerimeterTiles` fills the footprint with solid cells, plus curb rows where a neighbour is missing, plus corner pieces. | None. Intersections are just node markers. |
| Slopes | Stepping over runs in whole numbers, with slope lookup tables (`DAT_0500ba14`, `DAT_0500aa90`, and so on). | None. Height is stored but not used to build slopes. |
| Terrain patching | `Terrain_ApplyStreetIntersectionHeightPatch` blends intersection heights into the ground and softens the edges. | None. The terrain is shown as it is. |
| Piece and portal tiles | Uses the same `SetVoxelCellData` / `SetPortalVoxelData` API. | Uses the same API. |
| Drawing | Draws the placed road tiles as part of the world mesh. | Only draws guide quads and markers. |

`editor.exe` and `subrosa.exe` share the voxel API (`SetVoxelCellData`, `SetPortalVoxelData`, `UpdatePieceVoxelData`). The editor uses it for pieces, not for streets or intersections; the game makes the road geometry.

## 8. The original porting advice

Based on how `editor.exe` really behaves:

1. Storing streets: keep them as plain node and segment records, the same as the SBC layout. Do not put them in a voxel grid in the editor.
2. Drawing streets: draw nodes as marker gizmos plus the grown node and street guide quads, exactly as `RenderStreetToolNodeAndSegmentOverlays` sets them up.
3. Placing pieces: use `SetPortalVoxelData` → `SetVoxelCellData` for custom city pieces, with the same default of a solid voxel (`0x40000000`) and the block-mesh fallback (`0x80000000|idx`) from `UpdatePieceVoxelData`.
4. Keeping the game separate: when the port later loads the same SBC in a game-like view, that part should make road tiles the way subrosa.exe does (`MapConnector_PlaceStreetSegmentTilesAndSlopes` and the rest), as described in `docs/slop/RE_roads_spec.md` and `docs/slop/RE_terrain_spec.md`.
5. Do not place editor roads as `0x40000000` solid cells in the editor preview. The real editor does not, so doing it in the port would be wrong.

## 9. Open questions

1. Curved roads: the editor only allows segments along an axis (equal `flX` or equal `flZ`) and refuses diagonal ones. If Sub Rosa supports curved roads, the editor may not offer them, or only the game may make them. No curved-road code was found in editor.exe.
2. Building template preview: the editor calls `RenderBuildingTemplatePreview`, but it was not fully decompiled here. It uses the same block-mesh and solid-cell path.
3. Exact overlay colors: not all of the constants used for the `FUN_14003af50` / `FUN_14003b270` color state were decoded.
4. Overlay array layout: the fixed indices `0x22f6` and `0x277e` into `g_pBuildingInstancesArray` are used as scratch space for overlay geometry. Their exact layout was not fully worked out.
5. SBC v12 layer table: `LoadCity_SBC` reads layer names and city-block slot names. How the game uses layer indices beyond `SetPortalVoxelData` was not traced.
