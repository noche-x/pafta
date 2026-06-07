# editor.exe Road & Intersection Rendering RE Spec

**Source binary:** `editor.exe` (Sub Rosa map editor, NOT `subrosa.exe` the game)  
**Ghidra program:** `/editor.exe`, image base `0x140000000`, x64 PE  
**Analysis date:** 2026-06-07  
**Method:** Ghidra MCP tools only (`decompile_function`, `disassemble_function`, `search_functions`, `search_strings`, `get_xrefs_to`, `get_function_callers`, `read_memory`, `convert_number`).

---

## 1. Executive Verdict

**editor.exe does NOT render roads or intersections as placed voxel tiles, block-mesh instances, or tessellated road ribbons.**  
The editor stores the street network as abstract node/segment records in the SBC file and draws them only as **overlay lines and marker glyphs** inside the street-tool preview canvas. No road geometry is ever written to the world voxel grid.

This is a **fundamental difference** from `subrosa.exe`, which generates actual `0x40000000` solid cells and `0x80000000|code` block-mesh instances for every lane, curb, and intersection perimeter tile.

**Implication for `map_editor_cpp`:** The port's editor layer should represent streets as abstract node/segment data (matching the SBC schema) and render them as overlay lines/markers. The port should **not** try to place editor roads as solid voxels or block meshes, because the real editor does not do that. Road tile generation belongs to the **game runtime** (subrosa.exe), not the editor.

---

## 2. Function Inventory (Road / Street / Intersection Related)

| Function Name | Address | Role |
|---------------|---------|------|
| `EditorPanel_StreetPieceTool` | `0x14006a1c0` | UI panel for the combined Street/Building/Piece tool. Contains mode radio buttons (`Street`, `Building`, `Piece`). |
| `StreetPieceTool_CanvasRender` | `0x14006c400` | Main canvas render pass for the placement tool. Calls `BuildStreetToolMeshBuckets`, `RenderStreetToolMeshBuckets`, `RenderStreetToolNodeAndSegmentOverlays`, and renders building previews + ground grid. |
| `StreetPieceTool_CanvasUpdate` | `0x14006a870` | Canvas input/update pass. Handles camera, grid picking, and street node/segment creation/deletion. Calls `StreetTool_FindOrCreateStreetSegment` on click. |
| `StreetTool_UpdatePiecePlacementMode` | `0x14006ea00` | Handles the Piece sub-mode (custom block placement). Calls `SetPortalVoxelData` and `UpdatePieceVoxelData`. **Not used for streets.** |
| `StreetTool_FindOrCreateNodeXZ` | `0x14006efe0` | Creates or finds a node at grid X/Z. |
| `StreetTool_FindNodeAtGrid` | `0x14006f0c0` | Finds a node index at a given grid coordinate. |
| `StreetTool_FindOrCreateStreetSegment` | `0x14006f150` | Creates a new `SBC_StreetPiece` record between two node indices if axis-aligned. Stores width parameters in `pData[4]` and `pData[8]`. Does **not** place geometry. |
| `StreetTool_DeleteStreetSegment` | `0x14006f650` | Removes a segment by swapping with the last entry. |
| `StreetTool_DeleteNodeAndAttachedSegments` | `0x14006f490` | Deletes a node and all attached segments. |
| `RayIntersectsStreetSegmentWithDistance` | `0x1400703c0` | Ray/segment intersection test against the abstract line segment between two node positions. |
| `BuildStreetToolMeshBuckets` | `0x140076e70` | Batches **existing world objects** (buildings, pieces, terrain) by material for the preview renderer. Does **not** include street geometry. |
| `RenderStreetToolMeshBuckets` | `0x140077450` | Draws the batched world-object mesh buckets built above. |
| `RenderStreetToolNodeAndSegmentOverlays` | `0x14007c990` | **The actual street renderer.** Generates temporary overlay geometry for nodes and segments into hardcoded scratch slots inside `g_pBuildingInstancesArray`, then draws markers and lines. |
| `RenderWidget_Or_StreetPiece` | `0x1400319b0` | Generic widget renderer; used by the tool UI. |
| `LoadCity_SBC` | `0x14005f160` | Loads SBC v7..v12. Reads `g_NodeCount`, `g_pNodesArray`, `g_StreetPieceCount`, `g_pStreetPiecesArray`. After loading, calls `UpdatePieceVoxelData()` and `ProcessAndPlacePortalVoxels()`. **Never calls any street-to-voxel conversion.** |
| `SaveCity_SBC` | `0x14005e980` | Writes SBC v12. Serializes nodes and street pieces as abstract records. |
| `SetPortalVoxelData` | `0x14005d6f0` | Tile placement API for **portals / city pieces**. Calls `SetVoxelCellData`. Not used for streets. |
| `SetVoxelCellData` | `0x140043a60` | Core voxel cell writer. Handles `0x40000000` (solid) and `0x80000000|code` (block mesh) with neighbor-face resolution. Only caller in editor data path is `SetPortalVoxelData`. |
| `UpdatePieceVoxelData` | `0x14005fc90` | Resolves city-block names to loaded SBL block templates. Sets `nCount = 0x40000000` default, or `0x80000000 | blockIndex` when a named `.sbl` template matches. |

**Notable absence:** There is **no** function in editor.exe equivalent to `subrosa.exe`'s `MapConnector_PlaceStreetSegmentTilesAndSlopes` or `MapConnector_PlaceIntersectionPerimeterTiles`. No function iterates over `g_pStreetPiecesArray` and calls `SetVoxelCellData`.

---

## 3. Road Geometry Model in editor.exe

### 3.1 What the editor actually draws

The editor's street tool renders two things:

1. **Node markers** — small 3D axis-aligned boxes rendered with `RenderEditorMarkerModel` at each node's grid position.
2. **Segment guide lines** — axis-aligned line strips rendered with `FUN_14003b270` (an immediate-mode line drawer) between connected nodes.

Evidence from `RenderStreetToolNodeAndSegmentOverlays @ 0x14007c990`:

- It iterates `g_pNodesArray` and calls `FUN_14005a8a0` (node overlay setup) → then draws markers.
- It iterates `g_pStreetPiecesArray` and calls `FUN_14005ae20` (segment overlay setup) → then draws line segments with `FUN_14003b270`.
- The function does **not** call `SetVoxelCellData`, `SetPortalVoxelData`, or any world-grid placement routine.

Decompile excerpt (simplified):
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
// ... then renders markers and lines
```

### 3.2 What the editor does NOT draw

- **Solid voxel cells (`0x40000000`)** for road surfaces, curbs, or intersection fills.
- **Block-mesh instances (`0x80000000 | code`)** for sloped lanes or curb blocks.
- **Tessellated/bezier ribbon meshes** for curved roads.
- **Intersection perimeter geometry** — intersections are just node points; no footprint fill, no curb rows, no corner helpers.

**Conclusion:** The editor's road geometry model is **(d) something else** — it is purely abstract overlay graphics, not a physical tile placement.

---

## 4. Intersection Rendering

In the editor, an "intersection" is **identical to a node**.

- The SBC file stores intersections as entries in `g_pNodesArray` (12-byte records: `{flX, flY, flZ}`).
- `StreetTool_FindOrCreateStreetSegment` creates segments between nodes.
- There is **no separate intersection geometry builder** in editor.exe.
- No perimeter handling, no curb rows, no corner pieces, no footprint fill.

The visual representation is a single `RenderEditorMarkerModel` glyph at the node's `(gridX * 4.0, gridY * 4.0, gridZ * 4.0)` position.

---

## 5. Slope / Height Handling

Because the editor does not place road tiles, it also does **not** implement:

- Run-length slope stepping.
- Sloped block-mesh table lookups.
- Integer height delta between intersections.
- Terrain height patching.

The editor stores each node's `flY` as an integer grid level in the SBC file, but this value is only used for:

1. Positioning the node marker in world space (`worldY = gridY * 4.0`).
2. Persisting the authored height in the SBC save file.

**Decoded float constants used in the street tool:**

| Address | Raw bytes | IEEE-754 float | Usage |
|---------|-----------|----------------|-------|
| `DAT_1400ec9b4` | `00 00 80 40` | **4.0** | Grid cell size in world units. |
| `DAT_1400ec9c0` | `00 00 00 41` | **8.0** | Camera distance multiplier. |
| `DAT_1400ec9b0` | `00 00 70 40` | **3.75** | Used for wireframe box size in piece brush preview. |
| `DAT_1400ec990` | `00 00 00 40` | **2.0** | Marker model scale factor. |
| `DAT_1400ec900` | `00 00 00 3e` | **0.125** | Camera Y offset fraction. |
| `DAT_1400eca0c` | `00 00 80 43` | **256.0** | UI scroll-list height. |
| `DAT_1400eca20` | `00 00 00 44` | **512.0** | UI dropdown height. |

There are **no DAT_ floats** in editor.exe matching subrosa's slope-table constants (`DAT_0500ba14`, `DAT_0500aa90`, etc.) because those tables do not exist in the editor binary.

---

## 6. Tile Placement API (What the editor DOES place)

Although streets are not placed as tiles, the editor **does** place tiles for **city pieces / portals / buildings** via a well-defined API. This is relevant because `map_editor_cpp` may need to replicate it for the Piece tool.

### 6.1 API entry point

```c
void SetPortalVoxelData(int layer, uint gx, uint gy, uint gz, uint packedValue);
```

**Address:** `0x14005d6f0`

`packedValue` layout:
- Bits `0..11` (`& 0xfff`): city block index.
- Bits `16..21` (`>> 16 & 0x3f`): layer count minus one (`(layerCount + 1) * 0x10000`).
- Bits `24..25` (`>> 24 & 3`): rotation (0..3).

`SetPortalVoxelData` calls `SetVoxelCellData(layer, gx, gy, gz, blockValue, faceArray[8])`.

### 6.2 Block value semantics

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

### 6.3 City-block resolution

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

**Key insight:** In the editor, a city piece defaults to a **solid voxel** (`0x40000000`) with face texture arrays. It only becomes a **block-mesh instance** (`0x80000000|idx`) if the city block's name matches a loaded `.sbl` build-block template.

---

## 7. Comparison to subrosa.exe

| Aspect | subrosa.exe (game) | editor.exe (map editor) |
|--------|--------------------|--------------------------|
| **Street storage** | Runtime `streetIntersection` + `street` structs with extents, neighbors, widths. | SBC `g_pNodesArray` + `g_pStreetPiecesArray` (abstract records). |
| **Road tile placement** | `MapConnector_PlaceStreetSegmentTilesAndSlopes` writes `0x40000000` and `0x80000000\|code` tiles into the world grid. | **None.** No road tile placement function exists. |
| **Intersection fill** | `MapConnector_PlaceIntersectionPerimeterTiles` fills footprint with solid cells + missing-neighbor curb rows + corner helpers. | **None.** Intersections are just node markers. |
| **Slope model** | Run-length integer stepping, slope lookup tables (`DAT_0500ba14`, `DAT_0500aa90`, etc.). | **None.** Height is stored but not used for slope geometry. |
| **Terrain patching** | `Terrain_ApplyStreetIntersectionHeightPatch` bilinearly stamps intersection heights and feathers edges. | **None.** Terrain is shown as-is from the base world. |
| **Piece/portal tiles** | Uses same `SetVoxelCellData` / `SetPortalVoxelData` API. | Uses same API. |
| **Rendering** | Renders actual placed road tiles as part of the world mesh. | Renders overlay lines/markers only. |

**Verdict:** `editor.exe` and `subrosa.exe` share the **same underlying voxel API** (`SetVoxelCellData`, `SetPortalVoxelData`, `UpdatePieceVoxelData`), but the editor **does not invoke that API for streets or intersections.** Street geometry generation is a **game-runtime concern**, not an editor concern.

---

## 8. What `map_editor_cpp` Should Do

Based on the actual behavior of `editor.exe`:

1. **Editor street representation:** Store streets as abstract node/segment records matching the SBC schema. Do not place them into a voxel grid in the editor.
2. **Editor street rendering:** Draw nodes as marker gizmos and segments as axis-aligned line overlays (exactly what `RenderStreetToolNodeAndSegmentOverlays` does).
3. **Editor piece placement:** Use `SetPortalVoxelData` → `SetVoxelCellData` for custom city pieces, with the same solid-voxel default (`0x40000000`) and block-mesh fallback (`0x80000000|idx`) logic found in `UpdatePieceVoxelData`.
4. **Game-runtime separation:** When the port later loads the same SBC into a game-like mode, *that* layer should implement the subrosa.exe road tile generation (`MapConnector_PlaceStreetSegmentTilesAndSlopes`, etc.) documented in `docs/RE_roads_spec.md` and `docs/RE_terrain_spec.md`.
5. **Do not place editor roads as `0x40000000` solid cells** in the editor preview. The real editor does not do that, and doing so in the port would be unfaithful.

---

## 9. Honest Unknowns / Follow-Up Passes

1. **Curved roads:** The editor only allows axis-aligned segments (`flX` equal or `flZ` equal). It rejects diagonal segments. If Sub Rosa supports Bezier/curved roads, the editor may not expose them, or they may be generated only in the game runtime. No curved-road code was found in editor.exe.
2. **Building template preview rendering:** `RenderBuildingTemplatePreview` is called in the editor but was not fully decompiled here. It uses the same block-mesh/solid-cell pipeline.
3. **Exact overlay line colors/widths:** The constants used for `FUN_14003af50` / `FUN_14003b270` color state were not all decoded.
4. **Node/segment overlay array layout:** The hardcoded indices `0x22f6` and `0x277e` into `g_pBuildingInstancesArray` are used as scratch buffers for overlay geometry. Their exact struct layout was not fully recovered.
5. **SBC v12 layer table semantics:** `LoadCity_SBC` reads layer names and city-block slot names. The full runtime usage of layer indices beyond `SetPortalVoxelData` was not traced.

---

*End of spec.*
