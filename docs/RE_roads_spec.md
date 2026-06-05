# Sub Rosa ROAD/STREET Tile System RE Spec

Source basis: lead-provided Ghidra decompiles from `subrosa.exe`:

- `/tmp/re_place_street_segment.c`: `MapConnector_PlaceStreetSegmentTilesAndSlopes @ 0x20580`
- `/tmp/re_place_intersection_perimeter.c`: `MapConnector_PlaceIntersectionPerimeterTiles @ 0x20130`

`/tmp/re_bezier_road_mesh.c` was requested but was not present on this machine during this pass. Curved/Bezier road mesh behavior is therefore out of scope until that extraction exists.

This is a port contract for `map_editor_cpp`; no source edits are implied here.

## Executive Summary

The current port road path is materially wrong. It collapses native road placement into synthetic `Road`/`Curb` AABB tiles with linearly ramped `authored_y`. Native Sub Rosa instead builds road tiles through `World_SetTileBlockAndResolveNeighborLinks(layer=0, gx, gy, gz, blockValue, faceArray[8])`.

The important payload is not just "road vs curb". Each placed tile carries:

- `blockValue == 0x40000000` for native solid cells.
- `blockValue == 0x80000000 | code` for road block-mesh instances.
- `faceArray[8]` of short block-type/face codes. Flat road surfaces use this array, especially `faceArray[5]`.

Tell-it-like-it-is nuance: the decompile does still use `0x40000000` for flat lane cells and intersection fills. The port bug is not simply "solid cells exist"; the bug is that `map_editor_cpp` throws away native road block codes, sloped block instances, face codes, run-length stepping, neighbor-link placement, and intersection extents. A faithful port must model native tile records, not custom tilted boxes.

## Coordinate and Height Model

Constants decoded from the binary:

- `DAT_00278688 = 4.0`: world units per grid step.
- `DAT_00278698 = 4.25`: base vertical offset for tile/block placement.
- Grid cell size is 4.0 world units in X/Z.
- World Y for a grid height level `h` is:

```text
worldY = h * 4.0 + 4.25
```

Street/intersection heights use integer grid levels (`streetIntersection.unk0[2]`). The road terrain-top update observed in `MapConnector_PlaceStreetSegmentTilesAndSlopes` writes `heightLevel * 4.0` without the `+4.25` block placement offset.

## Native Runtime Fields

The decompile shows a runtime `streetIntersection` structure, not just the serialized `SbcIntersection {x,y,z}` currently parsed by `map_editor_cpp`.

Use these names in the port layer:

| Native field | Spec name | Meaning |
| --- | --- | --- |
| `pos.x` | `gridZ` | Grid Z base/center of intersection, used as an integer. |
| `unk0[1]` | `gridX` | Grid X base/center of intersection. |
| `unk0[2]` | `heightLevel` | Integer grid Y level. World Y = `heightLevel * 4.0 + 4.25`. |
| `field_0x2c` | `extentXPos` | +X half-extent of intersection footprint. |
| `field_0x34` | `extentXNeg` | -X half-extent of intersection footprint. |
| `field_0x30` | `extentZPos` | +Z half-extent of intersection footprint. |
| `field_0x38` | `extentZNeg` | -Z half-extent of intersection footprint. |
| `streetNorth` | `streetNorth` | Neighbor street index, `-1` when absent. |
| `streetSouth` | `streetSouth` | Neighbor street index, `-1` when absent. |
| `streetWest` | `streetWest` | Neighbor street index, `-1` when absent. |
| `field_0x28` | `streetEast` | East-neighbor street index, `-1` when absent. |

Street runtime fields:

| Native field | Spec name | Meaning |
| --- | --- | --- |
| `intersectionA` | `intersectionA` | Start intersection index. |
| `intersectionB` | `intersectionB` | End intersection index. |
| `unk1[0]` | `orientation` | `0` = native Z-major branch label, `1` = native X-major branch label. Note: branch `0` iterates grid X in the provided decompile. |
| `unk1[1]` | `leftWidth` | Left half-width in cells. |
| `unk1[2]` | `rightWidth` | Right half-width in cells. |

Current `map_editor_cpp` does not store `extentXPos/Neg`, `extentZPos/Neg`, or neighbor fields. Those are generated/native runtime connector fields. Faithful rendering needs a parser/runtime builder that reconstructs them the same way the game does, or a lead dump of the constructed values.

## Tile Placement Record

All road/intersection placement should be represented as:

```cpp
struct NativeRoadTile {
    int layer;              // always 0 in these routines
    int gx, gy, gz;         // grid tile coordinates
    uint32_t blockValue;    // 0x40000000 or 0x80000000 | roadBlockCode
    uint16_t face[8];       // local face block-type codes
};
```

Native placement API:

```text
World_SetTileBlockAndResolveNeighborLinks(0, gx, gy, gz, blockValue, faceArray)
```

The neighbor-link side effects matter. A port-side hash map that simply overwrites tile values may be acceptable as an intermediate data structure, but final parity should account for native conflict/neighbor resolution.

## Known Road Block-Type Codes

These codes are observed as either `blockValue` low bits (`0x80000000 | code`) or as `faceArray[]` entries on solid cells.

| Code / source | Meaning | Orientation / placement | Evidence |
| --- | --- | --- | --- |
| `DAT_0500a748` | Curb / side-cap block mesh. Numeric value not dumped. | Used as `0x80000000 | DAT_0500a748` for side curbs and missing-neighbor intersection perimeter rows. | Street and intersection routines. |
| `0x4101` | Straight road center/top face. | Native branch `orientation == 0`; lane offset `0`; written to `faceArray[5]` on flat solid lane cells. | Z-major branch lane loop. |
| `0x4201` | Road edge/top face at offset `-1`. | Native branch `orientation == 0`; lane offset `-1`. | Z-major branch lane loop. |
| `0x4501` | Negative-side road edge/top face away from center. | Native branch `orientation == 0`; lane offsets `< -1`, except the outermost negative-width filler slot. | Z-major branch lane loop. |
| `0x4401` | Positive-side road edge/top face away from center. | Native branch `orientation == 0`; positive lane offsets, except center and outermost positive filler slot. | Z-major branch lane loop. |
| `0x4801` | Intersection/end-cap perimeter straight face. | Native branch `orientation == 0`; written to `faceArray[5]` on end-cap solid cells. Exact loops omitted from provided file. | Street routine comment. |
| `0x4601` | X-major equivalent of `0x4101`. | Native branch `orientation == 1`. | Lead branch summary; full branch not present. |
| `0x4701` | X-major equivalent of `0x4201`. | Native branch `orientation == 1`. | Lead branch summary; full branch not present. |
| `0x4a01` | X-major equivalent of `0x4501`. | Native branch `orientation == 1`. | Lead branch summary; full branch not present. |
| `0x4901` | X-major equivalent of `0x4401`. | Native branch `orientation == 1`. | Lead branch summary; full branch not present. |
| `0x4301` | X-major intersection/end-cap perimeter straight face. | Native branch `orientation == 1`; equivalent role to `0x4801`. | Lead branch summary. |
| `1` | Interior/filler/empty face code. | Used in `faceArray` for intersection fill and outer lane filler slots. | Both routines. |

Do not invent geometry for these codes. The code role above comes from placement behavior, not from mesh vertex dumps. Actual visible geometry for `DAT_0500a748` and slope-table codes requires dumping the native road block mesh/code catalog.

## Slope Block Tables

Sloped road cells do not use a continuous float ramp. They use a centered integer run-length region and table-selected block mesh codes.

Known Z-branch tables:

| Table | Role | Direction | Index expression from decompile |
| --- | --- | --- | --- |
| `DAT_0500ba14` | Sloped side/curb block mesh. | Uphill (`heightDelta > 0`) | `runLen * 0x40 + runPos` |
| `DAT_0500ba90` | Sloped side/curb block mesh. | Downhill (`heightDelta < 0`) | `runLen * 0x40 + (runLen - runPos)` |
| `DAT_0500aa14` | Sloped lane/surface block mesh. | Uphill (`heightDelta > 0`) | `runLen * 0x40 + runPos` |
| `DAT_0500aa90` | Sloped lane/surface block mesh. | Downhill (`heightDelta < 0`) | `runLen * 0x41 - runPos`, equivalent to `runLen * 0x40 + (runLen - runPos)` |

Known X-branch tables from the branch summary:

| Table | Expected role | Confidence |
| --- | --- | --- |
| `DAT_0500ba54` | X-major uphill side/curb slope table. | Inferred from Z table offset pattern; verify with full X branch. |
| `DAT_0500bad0` | X-major downhill side/curb slope table. | Inferred from Z table offset pattern; verify with full X branch. |
| `DAT_0500aa54` | X-major uphill lane/surface slope table. | Inferred from Z table offset pattern; verify with full X branch. |
| `DAT_0500aad0` | X-major downhill lane/surface slope table. | Inferred from Z table offset pattern; verify with full X branch. |

Table layout requirement for the next Ghidra dump:

- Treat each table as a row-major lookup keyed by `runLen`.
- Row stride is `0x40` entries.
- Column is `runPos` for uphill.
- Downhill uses reversed columns: `runLen - runPos`, which means column `0` is apparently skipped for normal downhill runs.
- Dump entries as unsigned block-type codes, with enough width to preserve values before OR-ing with `0x80000000`.
- Dump at least rows for `runLen = 1..63`; auto-generated maps force `runLen` to `6` or `10`, but non-auto maps use `segmentLen / abs(heightDelta)`.

## Street Segment Algorithm

### Branch 0: `orientation == 0`

The provided decompile calls this the Z-major branch, but it iterates grid X and keeps Z centered on `intersectionA.pos.x`. Preserve the native branch naming in comments, but implement from the coordinate math.

Native setup:

```text
A = streetIntersections[street.intersectionA]
B = streetIntersections[street.intersectionB]

startX = A.gridX + A.extentXPos
centerZ = int(A.gridZ)
height = A.heightLevel
heightDelta = B.heightLevel - A.heightLevel
absDelta = abs(heightDelta)

segmentLen = (B.gridX - B.extentXNeg) - startX
```

Run-length and centered slope region:

```text
runLen = 6
if absDelta > 0:
    runLen = segmentLen / absDelta

if isMapAutoGenerated:
    runLen = 6
    if absDelta > 0:
        runLen = (absDelta <= segmentLen / 10) ? 10 : 6
    halfSlopeCells = (runLen >> 1) * absDelta
else:
    halfSlopeCells = (absDelta * runLen) >> 1

slopeStart = (segmentLen >> 1) - halfSlopeCells
slopeEnd   = (segmentLen >> 1) + halfSlopeCells    // half-open: [slopeStart, slopeEnd)
```

Important operational details:

- `runLen` is integer division.
- If `heightDelta == 0`, `absDelta == 0`, the slope region has zero width because `halfSlopeCells == 0`; the whole segment is flat.
- The slope region is centered in the available connector gap between intersection footprints.
- Native uses a half-open region: `slopeStart <= along < slopeEnd`.
- Uphill increments the current height before placing the first cell of each run.
- Downhill decrements the current height after placing the last cell of each run.

Per-cell pseudocode:

```text
height = A.heightLevel
sideTopHeight = height + 1
runPos = 0

for along in 0 .. segmentLen-1:
    insideSlope = slopeStart <= along && along < slopeEnd

    if insideSlope && heightDelta > 0 && (runPos % runLen) == 0:
        height += 1
        sideTopHeight = height + 1

    gx = startX + along

    // Side/curb strips, one cell outside the lane span.
    leftCurbZ  = centerZ - street.leftWidth - 1
    rightCurbZ = centerZ + street.rightWidth

    face = [0,0,0,0,0,0,0,0]

    if along == 0 || along == segmentLen - 1 || !insideSlope:
        set(0, gx, sideTopHeight, leftCurbZ,  0x80000000 | DAT_0500a748, face)
        set(0, gx, sideTopHeight, rightCurbZ, 0x80000000 | DAT_0500a748, face)
    else:
        if heightDelta < 0:
            sideCode = DAT_0500ba90[runLen * 0x40 + (runLen - runPos)]
        else:
            sideCode = DAT_0500ba14[runLen * 0x40 + runPos]
        set(0, gx, height, leftCurbZ,  0x80000000 | sideCode, face)
        set(0, gx, height, rightCurbZ, 0x80000000 | sideCode, face)

    // Lane/surface strip.
    face = [1,1,1,1,1,1,1,1]
    for offset in -street.leftWidth .. street.rightWidth-1:
        faceCode =
            offset == -1 ? 0x4201 :
            offset ==  0 ? 0x4101 :
            offset <  -1 ? ((street.leftWidth - 1 < -offset) ? 1 : 0x4501) :
                           ((offset < 1 || street.rightWidth - 1 <= offset) ? 1 : 0x4401)

        face[5] = faceCode

        if !insideSlope:
            blockValue = 0x40000000
        else:
            face[0] = faceCode
            if heightDelta < 0:
                surfaceCode = DAT_0500aa90[runLen * 0x41 - runPos]
            else:
                surfaceCode = DAT_0500aa14[runLen * 0x40 + runPos]
            blockValue = 0x80000000 | surfaceCode

        set(0, gx, height, centerZ + offset, blockValue, face)

    // Terrain top/occupancy update spans both curbs.
    for offset in (~street.leftWidth) .. street.rightWidth:   // ~L == -L-1
        UpdateMapTopHeightAndClearOccupancyBit(gx, centerZ + offset, height * 4.0)

    if insideSlope:
        if heightDelta < 0 && (runPos % runLen) == runLen - 1:
            height -= 1
            sideTopHeight = height + 1
        runPos = (runPos + 1) % runLen
```

After the main run, the routine places end-cap/perimeter solid cells at the two intersection ends. The provided file omits the exact loop body, but the lead note says it writes `0x40000000` with `faceArray[5] = 0x4801` over a Z-span for the branch-0 orientation. This needs the raw loop before final implementation.

### Branch 1: `orientation == 1`

This branch is the mirror of branch 0:

- Iterate along grid Z instead of grid X.
- Use `A.gridZ + A.extentZPos` and `B.gridZ - B.extentZNeg` for the along-axis gap.
- Keep X centered on `A.gridX`.
- Apply lane offsets along X.
- Use the X-major face-code family: `0x4601`, `0x4701`, `0x4a01`, `0x4901`, and end-cap `0x4301`.
- Use X-major slope tables: `DAT_0500ba54`, `DAT_0500bad0`, `DAT_0500aa54`, `DAT_0500aad0`.

The provided X-major decompile is a summary, not the real loop. The port should not hard-code inferred table pairing without one more raw Ghidra pass.

Expected branch-1 setup, based on field roles:

```text
startZ = A.gridZ + A.extentZPos
centerX = A.gridX
segmentLen = (B.gridZ - B.extentZNeg) - startZ
```

Everything else is the branch-0 algorithm with X/Z swapped.

## Lane Face Code Selection

For branch 0 (`orientation == 0`), lane offsets run:

```text
offset = -leftWidth .. rightWidth-1
```

Native face-code selection:

```text
if offset == -1:
    faceCode = 0x4201
else if offset == 0:
    faceCode = 0x4101
else if offset < -1:
    faceCode = (leftWidth - 1 < -offset) ? 1 : 0x4501
else:
    faceCode = (offset < 1 || rightWidth - 1 <= offset) ? 1 : 0x4401
```

This means the outermost lane slot on each side can intentionally be face code `1`, not a visible road edge code. Preserve that. It is not a typo.

For branch 1 (`orientation == 1`), use the same offset tests with the X-major code family. Proposed mapping:

```text
0x4101 -> 0x4601
0x4201 -> 0x4701
0x4501 -> 0x4a01
0x4401 -> 0x4901
0x4801 -> 0x4301
```

The mapping follows the lead summary and code-family pattern, but the full X-major branch should be dumped before this becomes a committed invariant.

## Intersection Footprint and Perimeter

`MapConnector_PlaceIntersectionPerimeterTiles(intersectionIndex)` first fills the footprint rectangle as native solid cells.

Footprint fill:

```text
ix = intersection.gridX
iz = intersection.gridZ
h  = intersection.heightLevel

gx in [ix - extentXNeg, ix + extentXPos)   // inclusive equivalent: ix-extentXNeg .. ix+extentXPos-1
gz in [iz - extentZNeg, iz + extentZPos)   // inclusive equivalent: iz-extentZNeg .. iz+extentZPos-1

faceArray = [1,1,1,1,1,1,1,1]
blockValue = 0x40000000
```

Then it lays missing-neighbor curb rows one level above the intersection, using `faceArray = [0,0,0,0,0,0,0,0]` and `blockValue = 0x80000000 | DAT_0500a748`.

Rules from the provided decompile:

| Missing neighbor | Row coordinate | Span | Height |
| --- | --- | --- | --- |
| `streetSouth == -1` | `gx = gridX + extentXPos` | `gz` over the +1-extended Z footprint | `heightLevel + 1` |
| `streetNorth == -1` | `gx = gridX - extentXNeg - 1` expected by field roles | `gz` over the +1-extended Z footprint | `heightLevel + 1` |
| `streetWest == -1` | `gz = gridZ + extentZPos` | `gx` over the +1-extended X footprint | `heightLevel + 1` |
| `streetEast == -1` (`field_0x28`) | `gz = gridZ - extentZNeg - 1` | `gx` over the +1-extended X footprint | `heightLevel + 1` |

The extracted note for `streetNorth` says `(unk0[1] - field_0x2c) - 1`, but the field model says the negative X extent is `field_0x34`. Treat that as a transcription/decompiler-note risk and verify before implementation.

"+1-extended footprint" means the curb row spans one cell beyond both ends of the perpendicular footprint. In half-open terms:

```text
Z-span for X-edge curb rows: [gridZ - extentZNeg - 1, gridZ + extentZPos + 1)
X-span for Z-edge curb rows: [gridX - extentXNeg - 1, gridX + extentXPos + 1)
```

Corner helper objects:

- Count present neighbors among `streetSouth`, `streetWest`, `streetNorth`, `streetEast`.
- If present-neighbor count `> 2`, call `_area_interior_setobject(0, gx, heightLevel + 1, gz, objectCode)`.
- Object code is `(intersectionIndex << 12) | directionCode`.

Known direction codes:

| Present neighbor | Direction code |
| --- | --- |
| North | `0x00000001` |
| South | `0x00000801` |
| East (`field_0x28`) | `0x01000401` |
| West | `0x01000c01` |

The provided decompile summary does not include the helper object coordinates or geometry. Do not invent them.

## Port Plan for `map_editor_cpp`

### 1. Replace the road data contract in `build_city_tile_cells`

Current contract:

- `CityTileCell {x,y,z, base_y, slope_x, slope_z, kind}`
- Later converted into synthetic solid cells via `native_city_block_solid_cell()`.

Required contract:

- Emit native road tile records: `{gx, gy, gz, blockValue, face[8]}`.
- Preserve symbolic/debug metadata separately if useful (`RoadSurface`, `Curb`, `Slope`), but do not make it the render source of truth.
- Store road block type codes exactly, including `faceArray[5]` and the slope branch `faceArray[0]`.

Practical architecture:

```text
build_city_tile_cells()
    build native RoadConnectorRuntime from SbcMap
    for each street:
        place_city_street_tiles_native(...)
    for each intersection:
        place_city_intersection_tiles_native(...)
    return vector<NativeRoadTile>
```

Open dependency: `SbcMap` currently has only serialized intersection points. It does not have native intersection extents or neighbor fields. Add a runtime builder only after reversing how Sub Rosa derives `field_0x2c/30/34/38` and `streetNorth/South/West/East`, or accept a labelled non-faithful fallback.

### 2. Rewrite `place_city_street_tiles`

Replace the existing linear ramp:

```text
authored_y = lerp(from.y, to.y)
slope_x/slope_z = continuous gradient
Road/Curb kind
```

with the native run-length algorithm:

- Use `intersectionA/intersectionB`, native extents, `orientation`, `leftWidth`, `rightWidth`.
- Compute `segmentLen` from intersection footprint edges, not center-to-center distance.
- Compute `runLen`, `slopeStart`, `slopeEnd` exactly as integer math.
- Step height levels in integer run chunks.
- Emit curb side blocks, flat solid lane cells, sloped block-mesh lane cells, and terrain-top updates.
- Use table lookups for every sloped block code.

Faithful now:

- The integer slope-region math can be implemented from this spec.
- Flat branch-0 lane face-code selection can be implemented from this spec.
- Intersection fill/perimeter rules can be implemented once runtime extents/neighbors exist.

Blocked for full parity:

- Numeric `DAT_0500a748`.
- Slope table contents.
- Full X-major branch table pairing and exact end-cap loops.
- Native intersection runtime builder.

### 3. Rewrite `place_city_intersection_tiles`

Replace current square-width heuristic:

```text
width = max(leftWidth + rightWidth of connected streets)
min_offset = -(width / 2)
max_offset = min_offset + width - 1
```

with native extents:

```text
gx: gridX - extentXNeg .. gridX + extentXPos - 1
gz: gridZ - extentZNeg .. gridZ + extentZPos - 1
```

Emit:

- Solid fill: `0x40000000`, `faceArray = all 1`, at `heightLevel`.
- Missing-neighbor curb rows: `0x80000000 | DAT_0500a748`, `faceArray = all 0`, at `heightLevel + 1`.
- Corner helper object records when present-neighbor count is greater than 2. It is acceptable to store them as unresolved helper-object records until coordinates/geometry are dumped.

### 4. Rewrite road rendering away from synthetic boxes

`build_city_tile_render_buffer` currently creates a `native_city_block_solid_cell()` for every city tile and then draws custom top/side faces with road/curb materials. That cannot produce native slopes or block-specific curb geometry.

Required renderer strategy:

- For `blockValue == 0x40000000`, draw a native solid cell using `faceArray` to choose road/intersection top/perimeter faces. At minimum, `faceArray[5]` must select the top road face code.
- For `(blockValue & 0x80000000) != 0`, instantiate the road block mesh selected by `blockValue & 0x7fffffff`.
- Build or import a road block mesh catalog keyed by these codes (`0x4101`, slope table entries, `DAT_0500a748`, etc.).
- Keep tile placement grid-based. Do not reintroduce continuous float ramping.

If the mesh catalog is not available yet, the honest fallback is to emit/log the native tile records and render a labelled debug approximation. Do not call that visual parity.

### 5. Terrain integration

The street routine updates terrain top/occupancy for every along cell and cross offset:

```text
offset in [~leftWidth, rightWidth]   // includes left curb at -leftWidth-1 and right curb at rightWidth
topHeight = currentHeightLevel * 4.0
```

`map_editor_cpp` currently ignores terrain in `build_city_tile_cells`. Native parity needs a road-top overlay that flattens/clears the terrain under the road footprint using this same integer-stepped height, not the synthetic ramp height.

### 6. Recommended validation workflow after implementation

No build was run for this analysis-only task. When porting later:

- Add a tile-dump mode that prints native road records sorted by `(gy,gz,gx,blockValue,face[])`.
- Use small fixtures:
  - flat 2-intersection street;
  - uphill `heightDelta=1`;
  - downhill `heightDelta=-1`;
  - wide road where outermost offsets become face code `1`;
  - T and 4-way intersections for perimeter/helper behavior.
- Compare the dump against lead-provided native tile dumps from `World_SetTileBlockAndResolveNeighborLinks`.
- Only then wire the render buffer.

## Data Needed From Next Ghidra Pass

High-value asks for the lead:

1. Dump numeric value at `DAT_0500a748`.
2. Dump contents of:
   - `DAT_0500ba14`
   - `DAT_0500ba90`
   - `DAT_0500aa14`
   - `DAT_0500aa90`
   - `DAT_0500ba54`
   - `DAT_0500bad0`
   - `DAT_0500aa54`
   - `DAT_0500aad0`
3. Dump the full `orientation == 1` branch, not just the summary.
4. Dump the omitted end-cap loops after the main branch-0/branch-1 street runs.
5. Dump the constructor/builder for `streetIntersection` runtime fields, especially extents and neighbor assignment.
6. Dump `_area_interior_setobject` call coordinates for corner helpers.
7. Provide `/tmp/re_bezier_road_mesh.c` if curved road rendering is relevant.

## Open Questions / Unknowns

- Exact geometry behind every road block code is not proven by these placement decompiles. The spec labels placement roles, not mesh shapes.
- `DAT_0500a748` numeric code is not known.
- Slope table contents are not known. Without them, sloped roads cannot be faithfully rendered.
- Full X-major branch details are not available; code/table mappings are partly inferred.
- The exact native `faceArray[0]` vs `faceArray[5]` semantic mapping needs a renderer-side Ghidra pass. Placement proves usage, not face direction names.
- Intersection extent construction is not available. Current `SbcIntersection` fields are insufficient for faithful placement.
- The `streetNorth/streetSouth/streetWest/streetEast` variable names do not cleanly match the coordinate rows in the decompile summary. Verify compass naming against the native coordinate basis before using names for UI labels.
- The `streetNorth` missing-neighbor curb coordinate in the extracted note appears to use `field_0x2c` where the field model implies `field_0x34`. Treat this as unresolved until raw Ghidra confirms it.
- Native overwrite/merge rules inside `World_SetTileBlockAndResolveNeighborLinks` are not decoded here.
- Corner helper object coordinates and visible purpose are not decoded here.
- Bezier/curved road mesh behavior is not covered because the requested decompile file was absent.
