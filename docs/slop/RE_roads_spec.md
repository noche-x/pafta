# Sub Rosa road and street tiles

Sources: Ghidra decompiles of `subrosa.exe`, provided by the lead:

- `/tmp/re_place_street_segment.c`: `MapConnector_PlaceStreetSegmentTilesAndSlopes @ 0x20580`
- `/tmp/re_place_intersection_perimeter.c`: `MapConnector_PlaceIntersectionPerimeterTiles @ 0x20130`

`/tmp/re_bezier_road_mesh.c` was asked for but was not on this machine during this pass, so curved (Bezier) road meshes are left out until that file exists.

This page describes what the `pafta` port has to do. It does not change any source by itself.

The [2026-09-25 section](#resolved-2026-09-25-subrosaexethis_one-ghidra) at the end answers most of the open questions below, and that work is in the code now.

## How roads worked when this was written

The road code checked in this pass made roads out of made-up `Road`/`Curb` box tiles with `authored_y` ramped in a straight line. The game builds road tiles through `World_SetTileBlockAndResolveNeighborLinks(layer=0, gx, gy, gz, blockValue, faceArray[8])`. The porting advice below is about that earlier code; check the current source before treating any of it as still to do.

Each placed tile carries more than "road or curb":

- `blockValue == 0x40000000` for solid cells.
- `blockValue == 0x80000000 | code` for road block meshes.
- `faceArray[8]`, short block-type or face codes. Flat road tops use this list, especially `faceArray[5]`.

The decompile does still use `0x40000000` for flat lane cells and filled intersections, so having solid cells is not the bug. The bug is that `pafta` throws away the game's road block codes, sloped blocks, face codes, run-length stepping, neighbour-aware placement, and intersection extents. A faithful port has to store the game's tile records, not its own tilted boxes.

## Coordinates and heights

Constants decoded from the binary:

- `DAT_00278688 = 4.0`: world units per grid step.
- `DAT_00278698 = 4.25`: base height offset for placing tiles and blocks.
- A grid cell is 4.0 world units across in X and Z.
- The world Y of grid level `h` is:

```text
worldY = h * 4.0 + 4.25
```

Street and intersection heights are whole-number grid levels (`streetIntersection.unk0[2]`). The terrain-top update in `MapConnector_PlaceStreetSegmentTilesAndSlopes` writes `heightLevel * 4.0`, without the `+4.25` offset.

## Fields the game builds at runtime

The decompile shows a runtime `streetIntersection` structure, which holds much more than the saved `SbcIntersection {x,y,z}` that `pafta` currently reads.

Names to use in the port:

| Game field | Name here | Meaning |
| --- | --- | --- |
| `pos.x` | `gridZ` | Grid Z of the intersection's centre, used as a whole number. |
| `unk0[1]` | `gridX` | Grid X of the intersection's centre. |
| `unk0[2]` | `heightLevel` | Whole-number grid Y level. World Y = `heightLevel * 4.0 + 4.25`. |
| `field_0x2c` | `extentXPos` | How far the footprint reaches in +X. |
| `field_0x34` | `extentXNeg` | How far the footprint reaches in -X. |
| `field_0x30` | `extentZPos` | How far the footprint reaches in +Z. |
| `field_0x38` | `extentZNeg` | How far the footprint reaches in -Z. |
| `streetNorth` | `streetNorth` | Index of the neighbouring street, `-1` if none. |
| `streetSouth` | `streetSouth` | Index of the neighbouring street, `-1` if none. |
| `streetWest` | `streetWest` | Index of the neighbouring street, `-1` if none. |
| `field_0x28` | `streetEast` | Index of the east neighbouring street, `-1` if none. |

Street fields at runtime:

| Game field | Name here | Meaning |
| --- | --- | --- |
| `intersectionA` | `intersectionA` | Index of the starting intersection. |
| `intersectionB` | `intersectionB` | Index of the ending intersection. |
| `unk1[0]` | `orientation` | `0` = the game's "Z-major" branch, `1` = its "X-major" branch. Note: branch `0` actually steps along grid X in the provided decompile. |
| `unk1[1]` | `leftWidth` | Width on the left side, in cells. |
| `unk1[2]` | `rightWidth` | Width on the right side, in cells. |

`pafta` does not store `extentXPos/Neg`, `extentZPos/Neg`, or the neighbour fields. The game builds them at runtime. To draw roads faithfully, the port needs code that builds them the same way the game does, or a dump of the values from the lead.

## Tile record

Every placed road and intersection tile should be stored as:

```cpp
struct NativeRoadTile {
    int layer;              // always 0 in these routines
    int gx, gy, gz;         // grid tile coordinates
    uint32_t blockValue;    // 0x40000000 or 0x80000000 | roadBlockCode
    uint16_t face[8];       // local face block-type codes
};
```

The game's placement call:

```text
World_SetTileBlockAndResolveNeighborLinks(0, gx, gy, gz, blockValue, faceArray)
```

What it does to neighbours matters. A hash map in the port that just overwrites tiles is fine as a stepping stone, but matching the game in the end means handling its conflict and neighbour rules.

## Known road block codes

These codes show up either as the low bits of `blockValue` (`0x80000000 | code`) or as `faceArray[]` entries on solid cells.

| Code / source | Meaning | Where it goes | Evidence |
| --- | --- | --- | --- |
| `DAT_0500a748` | Curb or side-cap block mesh. Number not dumped. | Used as `0x80000000 | DAT_0500a748` for side curbs and for intersection edge rows where a neighbour is missing. | Street and intersection functions. |
| `0x4101` | Straight road top, centre. | Branch `orientation == 0`, lane offset `0`, written to `faceArray[5]` on flat solid lane cells. | Z-major lane loop. |
| `0x4201` | Road edge top at offset `-1`. | Branch `orientation == 0`, lane offset `-1`. | Z-major lane loop. |
| `0x4501` | Road edge top on the negative side, away from the centre. | Branch `orientation == 0`, lane offsets `< -1`, except the outermost negative filler slot. | Z-major lane loop. |
| `0x4401` | Road edge top on the positive side, away from the centre. | Branch `orientation == 0`, positive lane offsets, except the centre and the outermost positive filler slot. | Z-major lane loop. |
| `0x4801` | Straight face for the intersection or end-cap edge. | Branch `orientation == 0`, written to `faceArray[5]` on end-cap solid cells. The exact loops are not in the provided file. | Comment in the street function. |
| `0x4601` | X-major version of `0x4101`. | Branch `orientation == 1`. | Lead's branch summary; full branch not available. |
| `0x4701` | X-major version of `0x4201`. | Branch `orientation == 1`. | Lead's branch summary; full branch not available. |
| `0x4a01` | X-major version of `0x4501`. | Branch `orientation == 1`. | Lead's branch summary; full branch not available. |
| `0x4901` | X-major version of `0x4401`. | Branch `orientation == 1`. | Lead's branch summary; full branch not available. |
| `0x4301` | X-major face for the intersection or end-cap edge. | Branch `orientation == 1`, same job as `0x4801`. | Lead's branch summary. |
| `1` | Inside, filler, or empty face. | Used in `faceArray` for intersection fill and outer lane filler slots. | Both functions. |

Do not make up geometry for these codes. Their roles above come from where they are placed, not from mesh data. Seeing the real shapes for `DAT_0500a748` and the slope-table codes needs a dump of the game's road block mesh catalog.

## Slope tables

Sloped road cells are not a smooth float ramp. They use a centred stretch of whole-number steps and pick block mesh codes from tables.

Tables known for the Z branch:

| Table | Role | Direction | Index from the decompile |
| --- | --- | --- | --- |
| `DAT_0500ba14` | Sloped side or curb block mesh. | Uphill (`heightDelta > 0`) | `runLen * 0x40 + runPos` |
| `DAT_0500ba90` | Sloped side or curb block mesh. | Downhill (`heightDelta < 0`) | `runLen * 0x40 + (runLen - runPos)` |
| `DAT_0500aa14` | Sloped lane or road-top block mesh. | Uphill (`heightDelta > 0`) | `runLen * 0x40 + runPos` |
| `DAT_0500aa90` | Sloped lane or road-top block mesh. | Downhill (`heightDelta < 0`) | `runLen * 0x41 - runPos`, the same as `runLen * 0x40 + (runLen - runPos)` |

Tables for the X branch, from the branch summary:

| Table | Expected role | Confidence |
| --- | --- | --- |
| `DAT_0500ba54` | X-major uphill side or curb slope table. | Guessed from the Z table offsets; check with the full X branch. |
| `DAT_0500bad0` | X-major downhill side or curb slope table. | Guessed from the Z table offsets; check with the full X branch. |
| `DAT_0500aa54` | X-major uphill lane or road-top slope table. | Guessed from the Z table offsets; check with the full X branch. |
| `DAT_0500aad0` | X-major downhill lane or road-top slope table. | Guessed from the Z table offsets; check with the full X branch. |

What the next Ghidra dump needs to cover:

- Treat each table as rows keyed by `runLen`.
- Each row is `0x40` entries long.
- The column is `runPos` for uphill.
- Downhill reads the columns backwards, `runLen - runPos`, so column `0` seems to be skipped on normal downhill runs.
- Dump the entries as unsigned block codes, wide enough to keep the value before it is ORed with `0x80000000`.
- Dump at least the rows for `runLen = 1..63`. Auto-generated maps force `runLen` to `6` or `10`, but other maps use `segmentLen / abs(heightDelta)`.

## How a street segment is placed

### Branch 0: `orientation == 0`

The decompile calls this the Z-major branch, but it steps along grid X and keeps Z centred on `intersectionA.pos.x`. Keep the game's branch name in comments, but write the code from the coordinate math.

Setup:

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

Run length and the centred slope area:

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

Details that matter:

- `runLen` uses whole-number division.
- If `heightDelta == 0`, then `absDelta == 0` and `halfSlopeCells == 0`, so there is no slope area and the whole segment is flat.
- The slope area is centred in the gap between the two intersection footprints.
- The area includes its start but not its end: `slopeStart <= along < slopeEnd`.
- Going uphill, the height goes up before the first cell of each run is placed.
- Going downhill, the height goes down after the last cell of each run is placed.

For each cell:

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

After the main run, the function places solid end-cap cells at the two intersection ends. The provided file leaves out the exact loop, but the lead's note says it writes `0x40000000` with `faceArray[5] = 0x4801` across a Z span for branch 0. That loop needs to be read directly before it is implemented.

### Branch 1: `orientation == 1`

This branch mirrors branch 0:

- Step along grid Z instead of grid X.
- Use `A.gridZ + A.extentZPos` and `B.gridZ - B.extentZNeg` for the gap along the street.
- Keep X centred on `A.gridX`.
- Apply lane offsets along X.
- Use the X-major face codes: `0x4601`, `0x4701`, `0x4a01`, `0x4901`, and end cap `0x4301`.
- Use the X-major slope tables: `DAT_0500ba54`, `DAT_0500bad0`, `DAT_0500aa54`, `DAT_0500aad0`.

The X-major decompile provided is only a summary, not the real loop. The port should not hard-code the guessed table pairs without one more direct Ghidra pass.

Expected setup for branch 1, from the field roles:

```text
startZ = A.gridZ + A.extentZPos
centerX = A.gridX
segmentLen = (B.gridZ - B.extentZNeg) - startZ
```

Everything else is the branch-0 algorithm with X and Z swapped.

## Picking lane face codes

For branch 0 (`orientation == 0`), lane offsets go:

```text
offset = -leftWidth .. rightWidth-1
```

The game picks face codes like this:

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

So the outermost lane slot on each side can get face code `1` on purpose instead of a visible road edge code. Keep it; it is not a typo.

For branch 1 (`orientation == 1`), use the same offset tests with the X-major codes. Suggested mapping:

```text
0x4101 -> 0x4601
0x4201 -> 0x4701
0x4501 -> 0x4a01
0x4401 -> 0x4901
0x4801 -> 0x4301
```

This mapping follows the lead's summary and the pattern of the codes. Dump the full X-major branch before relying on it.

## Intersection footprint and edges

`MapConnector_PlaceIntersectionPerimeterTiles(intersectionIndex)` first fills the footprint rectangle with solid cells.

Filling the footprint:

```text
ix = intersection.gridX
iz = intersection.gridZ
h  = intersection.heightLevel

gx in [ix - extentXNeg, ix + extentXPos)   // inclusive equivalent: ix-extentXNeg .. ix+extentXPos-1
gz in [iz - extentZNeg, iz + extentZPos)   // inclusive equivalent: iz-extentZNeg .. iz+extentZPos-1

faceArray = [1,1,1,1,1,1,1,1]
blockValue = 0x40000000
```

Then, on each side with no street, it lays a row of curbs one level above the intersection, using `faceArray = [0,0,0,0,0,0,0,0]` and `blockValue = 0x80000000 | DAT_0500a748`.

Rules from the provided decompile:

| Missing neighbour | Row position | Span | Height |
| --- | --- | --- | --- |
| `streetSouth == -1` | `gx = gridX + extentXPos` | `gz` over the Z footprint grown by 1 | `heightLevel + 1` |
| `streetNorth == -1` | `gx = gridX - extentXNeg - 1`, expected from the field roles | `gz` over the Z footprint grown by 1 | `heightLevel + 1` |
| `streetWest == -1` | `gz = gridZ + extentZPos` | `gx` over the X footprint grown by 1 | `heightLevel + 1` |
| `streetEast == -1` (`field_0x28`) | `gz = gridZ - extentZNeg - 1` | `gx` over the X footprint grown by 1 | `heightLevel + 1` |

The extracted note for `streetNorth` says `(unk0[1] - field_0x2c) - 1`, but by the field roles the negative X extent is `field_0x34`. This could be a copying mistake or a decompiler note error; check it before implementing.

"Grown by 1" means the curb row reaches one cell past both ends of the footprint across from it. Including the start and not the end:

```text
Z-span for X-edge curb rows: [gridZ - extentZNeg - 1, gridZ + extentZPos + 1)
X-span for Z-edge curb rows: [gridX - extentXNeg - 1, gridX + extentXPos + 1)
```

Corner objects:

- Count how many of `streetSouth`, `streetWest`, `streetNorth`, `streetEast` exist.
- If more than 2 exist, call `_area_interior_setobject(0, gx, heightLevel + 1, gz, objectCode)`.
- The object code is `(intersectionIndex << 12) | directionCode`.

Known direction codes:

| Neighbour present | Direction code |
| --- | --- |
| North | `0x00000001` |
| South | `0x00000801` |
| East (`field_0x28`) | `0x01000401` |
| West | `0x01000c01` |

The provided summary does not give the corner objects' positions or shapes. Do not make them up.

## Plan for `pafta`

### 1. Change what `build_city_tile_cells` returns

What it returns now:

- `CityTileCell {x,y,z, base_y, slope_x, slope_z, kind}`
- These are later turned into made-up solid cells with `native_city_block_solid_cell()`.

What it should return:

- The game's road tile records: `{gx, gy, gz, blockValue, face[8]}`.
- Labels like `RoadSurface`, `Curb`, or `Slope` can be kept on the side for debugging, but drawing must not depend on them.
- The exact road block codes, including `faceArray[5]` and the slope branch's `faceArray[0]`.

A practical layout:

```text
build_city_tile_cells()
    build native RoadConnectorRuntime from SbcMap
    for each street:
        place_city_street_tiles_native(...)
    for each intersection:
        place_city_intersection_tiles_native(...)
    return vector<NativeRoadTile>
```

Blocking issue: `SbcMap` only has the saved intersection points. It has no extents or neighbour fields. Only add code that builds them after working out how Sub Rosa fills in `field_0x2c/30/34/38` and `streetNorth/South/West/East`, or accept a fallback that is clearly labelled as not faithful.

### 2. Rewrite `place_city_street_tiles`

Replace the straight-line ramp:

```text
authored_y = lerp(from.y, to.y)
slope_x/slope_z = continuous gradient
Road/Curb kind
```

with the game's run-length method:

- Use `intersectionA/intersectionB`, the extents, `orientation`, `leftWidth`, and `rightWidth`.
- Work out `segmentLen` from the edges of the intersection footprints, not from centre to centre.
- Work out `runLen`, `slopeStart`, and `slopeEnd` with exact whole-number math.
- Step the height in whole runs.
- Place curb side blocks, flat solid lane cells, sloped block-mesh lane cells, and terrain-top updates.
- Look up every sloped block code in its table.

What can be done faithfully now:

- The whole-number slope math, from this page.
- Flat branch-0 lane face codes, from this page.
- Intersection fill and edge rules, once the runtime extents and neighbours exist.

What blocks full accuracy:

- The number behind `DAT_0500a748`.
- What is in the slope tables.
- The full X-major table pairs and the exact end-cap loops.
- Code that builds the game's intersection runtime fields.

### 3. Rewrite `place_city_intersection_tiles`

Replace the current square-width guess:

```text
width = max(leftWidth + rightWidth of connected streets)
min_offset = -(width / 2)
max_offset = min_offset + width - 1
```

with the game's extents:

```text
gx: gridX - extentXNeg .. gridX + extentXPos - 1
gz: gridZ - extentZNeg .. gridZ + extentZPos - 1
```

Place:

- Solid fill: `0x40000000`, `faceArray = all 1`, at `heightLevel`.
- Curb rows on sides with no street: `0x80000000 | DAT_0500a748`, `faceArray = all 0`, at `heightLevel + 1`.
- Corner object records when more than 2 neighbours exist. It is fine to store them as placeholder records until their positions and shapes are dumped.

### 4. Stop drawing roads as made-up boxes

`build_city_tile_render_buffer` currently makes a `native_city_block_solid_cell()` for every city tile and then draws its own top and side faces with road and curb materials. That can never give the game's slopes or its curb shapes.

What the renderer should do:

- For `blockValue == 0x40000000`, draw a solid cell and use `faceArray` to pick the road or intersection top and edge faces. At the very least, `faceArray[5]` has to pick the road top face.
- For `(blockValue & 0x80000000) != 0`, place the road block mesh picked by `blockValue & 0x7fffffff`.
- Build or import a catalog of road block meshes keyed by these codes (`0x4101`, slope table entries, `DAT_0500a748`, and so on).
- Keep tiles on the grid. Do not bring back smooth float ramps.

If the mesh catalog is not ready yet, the honest fallback is to output or log the game's tile records and draw a debug version that is clearly labelled. Do not call that a match with the game.

### 5. Terrain

The street function updates the terrain top and occupancy for every cell along the street and every offset across it:

```text
offset in [~leftWidth, rightWidth]   // includes left curb at -leftWidth-1 and right curb at rightWidth
topHeight = currentHeightLevel * 4.0
```

`pafta` currently ignores terrain in `build_city_tile_cells`. Matching the game needs a road-top layer that flattens or clears the ground under the road, at the same stepped height, not the made-up ramp height.

### 6. How to check the work afterwards

No build was run for this analysis. When porting later:

- Add a mode that prints the road tile records sorted by `(gy,gz,gx,blockValue,face[])`.
- Use small test maps:
  - a flat street between 2 intersections;
  - uphill, `heightDelta=1`;
  - downhill, `heightDelta=-1`;
  - a wide road where the outermost offsets get face code `1`;
  - T and 4-way intersections, for the edges and corner objects.
- Compare the output against tile dumps from the game's `World_SetTileBlockAndResolveNeighborLinks`, provided by the lead.
- Only then hook up the render buffer.

## What the next Ghidra pass should get

The most useful things to ask the lead for:

1. The number at `DAT_0500a748`.
2. The contents of:
   - `DAT_0500ba14`
   - `DAT_0500ba90`
   - `DAT_0500aa14`
   - `DAT_0500aa90`
   - `DAT_0500ba54`
   - `DAT_0500bad0`
   - `DAT_0500aa54`
   - `DAT_0500aad0`
3. The full `orientation == 1` branch, not just the summary.
4. The end-cap loops left out after the main branch-0 and branch-1 runs.
5. The code that builds the `streetIntersection` runtime fields, especially the extents and how neighbours are assigned.
6. The positions passed to `_area_interior_setobject` for corner objects.
7. `/tmp/re_bezier_road_mesh.c`, if curved roads matter.

## Open questions

- These placement decompiles do not prove the exact shape behind any road block code. This page names where codes are placed, not what the meshes look like.
- The number behind `DAT_0500a748` is not known.
- What is in the slope tables is not known. Without it, sloped roads cannot be drawn faithfully.
- The full X-major branch is not available, so some code and table pairings are guesses.
- What `faceArray[0]` and `faceArray[5]` mean exactly needs a Ghidra pass on the renderer side. Placement shows they are used, not which direction each one is.
- How intersection extents are built is not available. The current `SbcIntersection` fields are not enough for faithful placement.
- The names `streetNorth/streetSouth/streetWest/streetEast` do not line up neatly with the coordinate rows in the summary. Check the compass names against the game's coordinates before using them as UI labels.
- In the extracted note, the curb position for a missing `streetNorth` seems to use `field_0x2c` where the field roles say `field_0x34`. Treat this as open until Ghidra confirms it.
- How `World_SetTileBlockAndResolveNeighborLinks` overwrites or merges tiles is not decoded here.
- The positions and purpose of the corner objects are not decoded here.
- Curved (Bezier) roads are not covered, because the decompile file asked for was missing.

## Resolved 2026-09-25 (subrosa.exe.THIS_ONE, Ghidra)

The open items above are answered, and the answers are in `scene.cpp` (`place_native_street_tiles`, `build_city_tile_render_buffer`), `buildblock_catalog.cpp`, and `render_gpu.cpp` (`road_overlay_arrays`).

- `DAT_0500a748` = `DAT_0500a70c[15]` = catalog id 15, the curb plate with all four skirts.
- Slope tables: starting at `DAT_0500aa14`, row `4n + edge` holds 16 ids (int offset `n*64 + edge*16`), for n = 4..12. `ba14` is the next `0x1000` bytes. The bases `aa14/aa54/aa90/aad0` are +0/+16/+31/+47 ints. So uphill reads edge 0 (street along X) or 1 (along Z) at step `runPos`, and downhill reads edge 2 or 3 at step `n-1-runPos`. Entries that are not used are 0.
- Slope shapes (`Buildblock_InitTemplateMeshCatalog @10110`): edge 0 rises along +X, 1 rises along +Z, 2 falls along +X, 3 falls along +Z. `aa` is one flat patch from `k/n` to `(k+1)/n`, with UV (x, z) and material slot 0. `ba` is a slab 1/16 thick with its top at `k/n+1/16 .. (k+1)/n+1/16`, faces 1..5, material slot 0.
- The X branch is an exact mirror of branch 0 (codes `0x46/0x47/0x4a/0x49`, tables `aa54/aad0/ba54/bad0`). End caps: for each lane row `[c-L, c+R)`, a solid cell with `face[5] = 0x4801` (`0x4301` on the X branch) at the first cell along the street when A has `ext_along_pos > 0` and a cross extent, and likewise at `B.pos - ext_along_neg - 1` on B's level. The extents used are the ones before the +1 growth.
- Face codes (`BuildCellRenderGeometryAndAdjacency @c555`): if bit 14 is set, the texture is the low byte and the overlay layer is `(code >> 8) & 0x1f`. A negative code means default texture 3 with overlay `code & 0x3ff`. Texture 1 is `asphalt2.png` and texture 0 is `concrete`. Solid-cell faces use the tangent and bitangent tables `DAT_002b35a0/35f0`, and UV = position within the sector · 0.25 (`DAT_64cffce4`). Block quads use `face[quad material]`.
- Road markings are not a file. The function at `@0x98ff0` (named `Texture_GenerateCubemapFace`) fills an 11-layer 128x128 array texture (units 4/5, `tex_atlas_color/material`). Layers: 1/2 are yellow rows 5..10 / 117..122; 6/7 are yellow columns; 4/5 are white dashes (rows, columns 32..95); 9/10 are dashes on columns; 3 is white rows 16..31 and 96..111; 8 is white columns 11..26 and 91..106. `world.frag` mixes it over the color and material using its alpha.
- The corner table `DAT_002b34f0` is the bottom ring then the top ring, each going (0,0) (1,0) (1,1) (0,1) in x,z. Face 0 is the bottom and face 5 is the top.

## Traffic lights and street signs (2026-09-25)

Done in `scene.cpp` (`build_city_traffic_light_poles`, street name index) and `render_gpu.cpp` (`draw_city_traffic_light_poles`).

- Objects: `_area_interior_setobject` uses codes `(intersection << 12) | dir`. The type is `code & 0x7f` (1 = light), and the quarter turn is `(code >> 10) & 3`. The object goes at the centre of the corner cell plus the type's offset `(0, 3.25, -1)`, rotated (`_area_interior_buildsectormodel @13120`).
- Model: `GeneratedModel_BuildFromStaticTableSet675f @5b840` draws the type's collision boxes (`BuildBlock_GenerateCubeVerticesFaces @3fb90`, cubes centred on their position).
  - Type 1: post (0,0,0) 0.375x6x0.375, arm (0,3,-5) 0.25x0.25x10, heads (0,3,-6) and (0,3,-10) 0.3125x1x0.5.
  - Type 4 (the sign, drawn with the light's transform): (0,2.625,-8) 0.0625x0.5x2.
  - All faces use UV (0,0), except the -X face of the last two boxes of types 1 and 4: (0,1) (1,1) (1,0) (0,0) on corners 4,7,3,0.
- Signal textures (`_generatetexture @9a8cc`): 128x128 filled with (32,32,32,a=32). Lamp columns 40..87 hold red (255,32,32) at rows 84..107, yellow (255,255,32) at 52..75, and green (32,255,32) at 20..43, all a=255. `model.frag` draws alpha 1 without lighting and alpha < 0.5 as a non-metal with roughness 0.5. Each direction's light state is at `StreetIntersection` +0x54 + dir*4 at runtime; the editor shows state 0 (red).
- Sign texture (`RenderStreetLabelTexture @9da10`): 256x64, cleared to (0, 0.25, 0, 0), with the street name in font 4 = `font/overpass` at size 52, centred on (128, 32). The colour mask leaves alpha at 0. Glyph advance = size * (base width - 0.5) / cell height. The street shown is the pole's neighbour at `unk0[7 + ((dir + 1) & 3)]` (south, west, north, east at +0x1c..+0x28); if there is none, `(dir - 1) & 3`; if there is still none, street 0. Here `dir = (rotation - 2) & 3`.
- Street names (`MapConnector_AddStreetSegment @38e0`): along X, `(A.gz - 256) / 32` gives 0..8 for First to Ninth Street. Along Z, `(A.gx - 256) / 56` gives 0..7 for Abrash, Bunten, Carmack, David, Eugene, Flinn, Garriott, and Holland Ave. Otherwise the field stays 0.
