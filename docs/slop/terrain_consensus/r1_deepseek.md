# City terrain research, DeepSeek round 1

Target binary: `subrosa.exe.THIS_ONE` (x86-64 PE, image base 0x00000000, 3779 functions)
Date: 2026-06-08
Method: Ghidra MCP decompiling and full disassembly, decoding constants, and reading struct layouts

[terrain_generation.md](../terrain_generation.md) has the later, checked findings. Where they disagree with this round (for example on axes and on how far the smoothing reaches), the later page is right.

## 0. Key constants (decoded as IEEE-754 from read_memory)

| Address     | Hex           | IEEE-754  | Symbol              |
|-------------|---------------|-----------|---------------------|
| 0x00278688  | 0x40800000    | 4.0       | `DAT_00278688`      |
| 0x00278698  | 0x40880000    | 4.25      | `DAT_00278698`      |
| 0x002783f4  | 0x3F800000    | 1.0       | `FLOAT_002783f4`    |
| 0x00278280  | 0x3E800000    | 0.25      | `FLOAT_00278280`    |
| 0x002781bc  | 0x3D800000    | 0.0625    | `FLOAT_002781bc`    |
| 0x00278644  | 0x40400000    | 3.0       | `DAT_00278644`      |
| 0x00278224  | 0x3E000000    | 0.125     | `FLOAT_00278224`    |

## 1. Corner heights

### Question: is it `intersection.unk0[2]*4.0+4.25`, or does it come from the mapTopHeight buffers?

The corner height is `streetIntersection.unk0[2] * 4.0 + 4.25`. The mapTopHeight buffers are only used to check the four corners of each sub-cell and as samples for the 5×5 sub-cell softening. They do not give the bilinear corner heights.

### Proof: decompile and disassembly at 0xd349e–0xd3650

From the decompile (0xd3340):

```c
fVar43 = (float)streetIntersections[param_1].unk0[2] * DAT_00278688 + DAT_00278698;
fVar44 = (float)streetIntersections[lVar28].unk0[2] * DAT_00278688 + DAT_00278698;
fVar36 = (float)streetIntersections[lVar14].unk0[2] * DAT_00278688 + DAT_00278698;
fVar38 = (float)streetIntersections[lVar15].unk0[2] * DAT_00278688 + DAT_00278698;
```

`DAT_00278688 = 4.0` and `DAT_00278698 = 4.25`, so each corner is `unk0[2] * 4.0 + 4.25`.

The four corners are the four intersections around the city block:
- TL: `param_1` (the "NE" intersection, top-left in the patch's own order)
- TR: `lVar28` (the far-south intersection, which ends up top-right in the bilinear fill)
- BL: `lVar14` (the far-west intersection)
- BR: `lVar15` (the far-south-west intersection)

The mapTopHeight buffers (`DAT_3ce6f5f8`, `DAT_3ce6f5fc`, `DAT_3ce735f8`, `DAT_3ce735fc`) are read in the softening phase at 0xd382f–0xd3850, as a check that must pass first and as the source for the 5×5 sub-samples. They are NOT where the corner heights come from.

### What writes the mapTopHeight buffers?

`UpdateMapTopHeightAndClearOccupancyBit` @0x3fa40 writes `(int)uVar19 * 4.0` (the street surface height, WITHOUT the +4.25) to ALL FOUR buffers at once. It is called by:
- `MapConnector_PlaceStreetSegmentTilesAndSlopes` @0x20580
- `ApplyAllCityDistrictCellsToWorld` @0x20000
- `InstantiateSbbVolumeIntoWorldAndMetadata` @0x23ac0
- `World_LoadMapAndInitializeTerrain` @0x56980

The street surface height is `intersection.unk0[2] * 4.0` (no +4.25).

So the patch writes a surface 0.25 world units HIGHER than the street tiles (`4.0*level + 4.25` against `4.0*level` for the street floor). That is the road SURFACE, on top of the tile floor, and probably the height you drive at.

## 2. Patch rectangle

### Question: which fields set the rectangle? Is it the gap between FOUR intersections, or one intersection's footprint?

The rectangle covers the gap between the four intersections around a city block. `*4 + 0x1000` turns world coordinates into grid samples (1 unit = 4 samples, offset 4096).

### Rectangle formula

From the decompile at 0xd34e0–0xd366b:

```
x_min = (pos.x + field_0x30) * 4 + 0x1000      // east edge of param_1 (NE corner)
x_max = (pos.x - field_0x30) * 4 + 0x1000      // west edge of far-west corner (lVar14)

y_min = (unk0[1] + field_0x2c) * 4 + 0x1000    // south edge of param_1
y_max = (unk0[1] - field_0x2c) * 4 + 0x1000    // north edge of far-south corner (lVar28)
```

### The fields (StreetIntersection struct, 136 bytes)

From `get_struct_layout("StreetIntersection")`:

| Offset | Size | Type       | Name           | Role                        |
|--------|------|------------|----------------|-----------------------------|
| 0      | 12   | int[3]     | unk0           | [0]=X?, [1]=Y position, [2]=height level |
| 12     | 12   | Vector     | pos            | .x = world X coordinate     |
| 40     | 4    | int (unnamed) | field_0x28 | connection flag (-1 = dead end) |
| 44     | 4    | int (unnamed) | field_0x2c | reach in +Y (south)         |
| 48     | 4    | int (unnamed) | field_0x30 | reach in +X (east)          |
| 52     | 4    | int (unnamed) | field_0x34 | reach in -Y (north)         |
| 56     | 4    | int (unnamed) | field_0x38 | reach in -X (west)          |

Confidence: HIGH. Both the struct layout and how MapConnector @0x20580 uses the fields agree:
```c
// Horizontal street: uses +field_0x2c for south end, -field_0x34 for north end
uVar3 = streetIntersections[iVar6].unk0[1] + *(int *)&streetIntersections[iVar6].field_0x2c;
iVar18 = (streetIntersections[iVar13].unk0[1] - *(int *)&streetIntersections[iVar13].field_0x34) - uVar3;
```

### Size

```
width  = iVar32 = x_max - x_min  (grid samples)
height = iVar34 = y_max - y_min  (grid samples)
```

This covers the INSIDE of the city block, from the inner road edge of one intersection to the inner road edge of the one across from it. The streets themselves are placed separately by `MapConnector_PlaceStreetSegmentTilesAndSlopes`.

## 3. Checking for a four-cornered block

### Proof from the decompile at 0xd3367–0xd3498

Before patching, the function checks that all FOUR corners of the block exist and are joined by streets:

```
streetIntersections[param_1].streetSouth != -1
  AND (recursive south or west check on the southern intersection)
  AND streetIntersections[param_1].streetWest != -1
  AND (recursive west or south check on the western intersection)
  AND streetIntersections[lVar14].streetSouth != -1  (far-west has south street)
  AND streetIntersections[lVar15].field_0x28 != -1    (far-SW corner is valid)
```

This makes sure the block is closed on all four sides (param_1 → south → SW, and param_1 → west → SW). The recursion allows one extra step: if a corner has no direct west or south street, it checks once more one intersection further on. If the block is not closed, the patch is skipped.

## 4. Softening and smoothing passes

### 4.1 Scratch buffer layout

Base address: `DAT_04afa9bc` (the count array starts at `DAT_04afa9a0` = `DAT_04afa9bc - 0x1c`)

Memory map:
```
DAT_04afa9a0  count[row*0x100 + col]   (int32, at byte offset cell_idx * 0x1c)
DAT_04afa9a4  col_slot[row*0x100 + col].col[0..1]  (int32*2)
DAT_04afa9a8  row_slot[row*0x100 + col].row[0..1]  (int32*2)
DAT_04afa9ac  weight[row*0x100 + col].weight[0..1] (float*2)
```

- Each "cell" is 0x1c = 28 bytes: a count (4) and 2 entries of 12 bytes (col, row, weight).
- A cell holds at most 2 entries (4 + 24 = 28).
- Rows are 0x100 cells × 0x1c = 0x1c00 bytes = 7168 apart.
- The grid is (iVar32>>2) × (iVar34>>2) cells, each covering 4×4 grid samples.
- In memory: `buffer[row * 0x100 + col]`, 256 columns per row.

### 4.2 Phase 1: bilinear fill

At 0xd3680–0xd3710: for every grid sample in the rectangle, it writes a height blended from the four corner heights, using `Terrain_SetLandSampleWithTileBorderMirroring`:

```
for y in [y_min..y_max]:
    t_y = (y - y_min) / (y_max - y_min)
    for x in [x_min..x_max]:
        t_x = (x - x_min) / (x_max - x_min)
        h = (1-t_y)*((1-t_x)*h_TL + t_x*h_TR) + t_y*((1-t_x)*h_BL + t_x*h_BR)
        Terrain_SetLandSampleWithTileBorderMirroring(y, x, h)
```

### 4.3 Phase 2: clearing the scratch buffer

At 0xd371b–0xd37a4: it zeroes the scratch area covering (iVar32>>2)×(iVar34>>2) cells plus a ring of extra cells around it. Each step zeroes 5 cells in a column (one extra cell plus 4 real ones) across 5 columns (one extra plus 4 real).

### 4.4 Phase 3: filling 5×5 sub-cells

At 0xd37b9–0xd3a0c: for each 4×4 block where all four mapTopHeight buffers are > 0.0:

```c
// Index into mapTopHeight buffers
index = ((iVar25 + pos.x + field_0x30) << 12) + (local_res18 + unk0[1] + field_0x2c)

fVar36 = *(float*)(&DAT_3ce6f5f8 + index*4)   // buffer[0]
fVar38 = *(float*)(&DAT_3ce6f5fc + index*4)   // buffer[1]
fVar41 = *(float*)(&DAT_3ce735f8 + index*4)   // buffer[2]
fVar42 = *(float*)(&DAT_3ce735fc + index*4)   // buffer[3]

if (fVar36 > 0 && fVar38 > 0 && fVar41 > 0 && fVar42 > 0):
    // 5×5 sub-sample kernel within the 4×4 block
    for sub_y in 0..4 (step = 0.25):
        t_y = sub_y * 0.25
        for sub_x in 0..4 (step = 0.25):
            t_x = sub_x * 0.25
            h = (1-t_y)*((1-t_x)*fVar36 + t_x*fVar38) + t_y*((1-t_x)*fVar41 + t_x*fVar42)
            
            cell = scratch[(block_col + sub_x) * 0x100 + (block_row + sub_y)]
            if cell.count == 0:
                cell.entry[0] = {col: block_col+sub_x, row: block_row+sub_y, weight: 1.0}
                cell.count = 1
                Terrain_SetLandSampleWithTileBorderMirroring(y_sample, x_sample, h)
```

The 5×5 pattern reads the same four mapTopHeight buffers as a four-corner bilinear blend, making 25 sub-samples per 4×4 block. That samples the street surface more finely, so detail smaller than a block shows up.

### 4.5 Phase 4: spreading to neighbours (16 passes)

At 0xd3a1f–0xd3c8d (outer loop `local_120 = 0x10` = 16 passes):

For each cell where `count == 0` OR `max_weight <= 1.0`:

```
For each of 8 neighbors (offsets -1..+1 in both x and y, excluding (0,0)):
    For each entry in neighbor cell:
        dx = entry.col - cell_col
        dy = entry.row - cell_row
        dist = sqrt(dx*dx + dy*dy)
        weight = 1.0 - dist * 0.0625
        if weight <= 0: skip
        
        if dist == 0:
            ndx = 0, ndy = 0
        else:
            ndx = dx / dist, ndy = dy / dist  (normalized direction)
        
        if current cell has entries:
            // Dot product test: check if this candidate is "in front" of existing
            for each existing entry:
                e_dx = entry.col - cell_col, e_dy = entry.row - cell_row
                e_dist = sqrt(e_dx² + e_dy²)
                if e_dist == 0: e_ndx=0, e_ndy=0
                else: e_ndx = e_dx/e_dist, e_ndy = e_dy/e_dist
                
                dot = ndx * e_ndx + ndy * e_ndy
                if dot > 0 AND candidate.weight <= existing.weight:
                    // Replace existing entry with better (closer) candidate
                    insert_or_replace at position where weight is maintained sorted
        else:
            // First entry — insert directly
            cell.count = 1, cell.entry[0] = {col: neighbor.col, row: neighbor.row, weight: weight}
```

The dot-product test keeps directions consistent: data only spreads from neighbours that "point at" the empty cell. With a factor of `0.0625`, influence drops to zero at a distance of `1.0 / 0.0625 = 16` cells (that is, `16 * 4 = 64` grid samples).

There are 16 passes (confidence: HIGH, confirmed in the disassembly at 0xd3a28: `MOV EDX, 0x10`).

### 4.6 Phase 5: final smoothstep merge with the original terrain

At 0xd3c93–0xd4048: for every cell in the scratch area where count > 0:

```
// Step 1: Accumulate total weight
total_weight = 0
for entry in cell.entries:
    total_weight += entry.weight

// Step 2: If count < 2 (single entry): use that entry's terrain sample directly
if count < 2:
    h_result = *(float*)(g_landTileHeightChunks at entry[0].row/col)

// Step 3: If count >= 2 (two entries): blend using smoothstep
else:
    t = entry[0].weight / total_weight          // normalized first-entry weight
    t = clamp(t, 0, 1)
    
    h1 = *(float*)(g_landTileHeightChunks at entry[0].row/col)
    h2 = *(float*)(g_landTileHeightChunks at entry[1].row/col)
    
    // Smoothstep: t' = (3 - 2t) * t²
    t_smooth = (3.0 - (t + t)) * t * t
    
    blend = (1 - t_smooth) * h2 + t_smooth * h1

// Step 4: Find maximum individual weight
max_weight = 0
for entry in cell.entries:
    max_weight = max(max_weight, entry.weight)
max_weight = clamp(max_weight, 0, 1)

// Step 5: Smoothstep on max_weight
t2 = (3.0 - 2*max_weight) * max_weight²

// Step 6: Read original terrain at this cell's grid position
h_orig = *(float*)(g_landTileHeightChunks at cell.row/col)

// Step 7: Final blend
result = (1 - t2) * h_orig + blend * t2

Terrain_SetLandSampleWithTileBorderMirroring(cell.row, cell.col, result)
```

This is how the edge gets softened. The smoothstep of `max_weight` decides how much patched terrain and how much original terrain to use. When a cell picked up strong samples (high max_weight), t2 is near 1 and the result is near the blended road height. When a cell has little data (low max_weight), t2 is near 0 and the result is near the original island height. The result is a smooth, natural-looking change.

### Smoothstep formula (confirmed in the disassembly at 0xd3f40–0xd3f76)

```
XMM9 = 3.0           // DAT_00278644
XMM0 = t + t         // 2*t
XMM1 = 3.0 - XMM0    // 3 - 2t
XMM2 = t * t         // t²
XMM1 = XMM1 * XMM2   // (3-2t)*t² = smoothstep output
```

## 5. Which intersections get patched

From `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches` @0xd12e0:

```
if (isMapAutoGenerated == 0):
    Terrain_ApplyStreetIntersectionHeightPatch(2);
    Terrain_ApplyStreetIntersectionHeightPatch(0x28);
    Terrain_ApplyStreetIntersectionHeightPatch(0x20);
    Terrain_ApplyStreetIntersectionHeightPatch(0x2e);
    Terrain_ApplyStreetIntersectionHeightPatch(0x2d);
    Terrain_ApplyStreetIntersectionHeightPatch(0x2f);
    Terrain_ApplyStreetIntersectionHeightPatch(0x1c);
    Terrain_ApplyStreetIntersectionHeightPatch(10);
    Terrain_ApplyStreetIntersectionHeightPatch(0x21);
    Terrain_ApplyStreetIntersectionHeightPatch(0x2a);
    Terrain_ApplyStreetIntersectionHeightPatch(0x23);
    Terrain_ApplyStreetIntersectionHeightPatch(0xd);
    Terrain_ApplyStreetIntersectionHeightPatch(0x3b);
    Terrain_ApplyStreetIntersectionHeightPatch(0x1f);
    Terrain_ApplyStreetIntersectionHeightPatch(0x34);
    Terrain_ApplyStreetIntersectionHeightPatch(0x30);
    Terrain_ApplyStreetIntersectionHeightPatch(0x32);
    Terrain_ApplyStreetIntersectionHeightPatch(0x13);
    Terrain_ApplyStreetIntersectionHeightPatch(0x16);
    Terrain_ApplyStreetIntersectionHeightPatch(0x3a);
    Terrain_ApplyStreetIntersectionHeightPatch(0x3e);
    Terrain_ApplyStreetIntersectionHeightPatch(0x3d);
    Terrain_ApplyStreetIntersectionHeightPatch(0x29);
    Terrain_ApplyStreetIntersectionHeightPatch(0x24);
    Terrain_ApplyStreetIntersectionHeightPatch(0x2b);
    Terrain_ApplyStreetIntersectionHeightPatch(0x2c);
    Terrain_ApplyStreetIntersectionHeightPatch(0x26);
    Terrain_ApplyStreetIntersectionHeightPatch(0x27);
```

That is 28 calls with 28 different intersection numbers, only on maps that are not auto-generated. Each call's own four-corner check decides whether the patch really runs.

Why only some intersections? These are the intersections in the central city grid. The check at the top of `Terrain_ApplyStreetIntersectionHeightPatch` only lets through intersections that close a full block, with all four corners present and joined. The 28 listed are the possible north-east corners; the ones that do not close a block just return.

## 6. World and grid coordinates

From `Terrain_SetLandSampleWithTileBorderMirroring` @0xd2db0:

```c
void Terrain_SetLandSampleWithTileBorderMirroring(uint param_1, uint param_2, float height)
{
    uVar3 = (int)param_1 >> 0xb;   // chunk_col = x / 2048
    uVar2 = (int)param_2 >> 0xb;   // chunk_row = y / 2048
    
    if (uVar3 > 7 || uVar2 > 7) return;  // 8×8 chunk grid max
    
    lVar5 = (longlong)(int)(uVar3 + uVar2 * 8);          // chunk index
    uVar1 = param_2 & 0x7ff;                             // intra-chunk y
    uVar4 = param_1 & 0x7ff;                             // intra-chunk x
    
    *(float*)(g_landTileHeightChunks[lVar5*0x684] + (uVar1*0x801 + uVar4)*4) = height;
    
    // Tile-border mirroring (writes into neighboring tiles when on edge)
}
```

Main points:
- `>> 0xb` (= >> 11 = / 2048): each terrain chunk is 2049×2049 samples (0x801 = 2049).
- `chunk_stride = 0x684` bytes = 1668, and `1668 / 4 = 417` floats, which is NOT 2049². The chunk is an array of pointers, not stored inline.
- 8×8 = 64 chunks in total.
- World to grid: `grid = (world + 4096) * 4`, from the `*4 + 0x1000` pattern in the rectangle math.
  - That is, `grid_sample = world_unit * 4 + 0x1000`.
  - So `world = (grid_sample - 0x1000) / 4`.
- The grid origin offset of +4096 samples (+1024 world units) puts the world origin in the middle of the grid.
- The largest extent is 0x7ff × 0x801 ≈ 2047 × 2049 per chunk, in 8×8 chunks, so 16384 × 16384 in total.

The `0x1000` (=4096) offset shifts the grid so that negative world coordinates (down to -1024) become positive grid samples. The `*4` gives 4 samples per world unit. The CITY uses this same grid.

## 7. Scratch buffer: exact layout

Confirmed in the disassembly at 0xd372f–0xd37a4 and 0xd37f8–0xd3986:

| Symbol              | Address     | Offset in cell | Type  | Description              |
|---------------------|-------------|----------------|-------|--------------------------|
| `DAT_04afa9a0`     | 0x04afa9a0  | +0x00          | int32 | Entry count (0–2)        |
| `DAT_04afa9a4`     | 0x04afa9a4  | +0x04          | int32 | Entry[0].col (x_base+col)|
| `DAT_04afa9a8`     | 0x04afa9a8  | +0x08          | int32 | Entry[0].row (y_base+row)|
| `DAT_04afa9ac`     | 0x04afa9ac  | +0x0C          | float | Entry[0].weight          |
|                     |             | +0x10          | int32 | Entry[1].col             |
|                     |             | +0x14          | int32 | Entry[1].row             |
|                     |             | +0x18          | float | Entry[1].weight          |

Each cell is 0x1C = 28 bytes (a count int plus 2 entries of 12 bytes).

Indexing: `cell[row][col] = base + (row * 0x100 + col) * 0x1c`

`row` and `col` are in block coordinates (one block = 4×4 grid samples). The scratch array covers the patch rectangle, rounded up to the next multiple of 4 samples.

## 8. Decompiler mistakes and register reuse

The Ghidra decompiler mixes up some registers in the softening code. Checked against the raw disassembly:

| Location           | Decompiler says              | Disassembly says               | Note |
|--------------------|------------------------------|--------------------------------|------|
| Clearing loop step | `puVar29 += 0x1c00`         | `ADD R9, 0x7000`               | The real row step is 0x7000, so the decompiled 0x1c00 is WRONG for the outer loop. 0x1c00 is the row step of the 5×5 pattern. |
| 5×5 cell index     | `(local_118+local_120)*0x70`| `IMUL R14, RAX, 0x70`         | Checked: 0x70 = 112 = 4 cells. |
| Softening passes   | `local_120 = 0x10`          | `MOV EDX, 0x10`                | 16 passes confirmed. |

About the clearing step: the assembly at 0xd3799 shows `ADD R9, 0x7000`, but the decompiler shows `puVar29 = puVar29 + 0x1c00`. That would make the decompiler wrong: the clearing phase's outer row step would be 0x7000 bytes, not 0x1c00. But the clearing phase uses 0x7000 while the softening phase uses 0x1c00 as its row step, because the clearing loop works at a different grid size. The softening's `(local_118 + local_120) * 0x70` with `local_118 += 0x100` gives `row * 0x100 * 0x70 = row * 0x7000`, which matches the clearing step. Confidence: MEDIUM on the exact clearing step. The decompiler may be right and I may be misreading register reuse in the assembly. 0x1c00 (256 cells × 28 bytes) makes more sense, since the softening phase clearly uses 0x100 cells per row.

## 9. Summary: the formulas the port needs

### Corner heights (confirmed)
```
h_corner = intersection.unk0[2] * 4.0 + 4.25
```
Each corner uses its own intersection's `unk0[2]`.

### Rectangle (confirmed)
```
x_min = (pos.x + field_0x30) * 4 + 0x1000    // world→grid
x_max = (far_west.pos.x - far_west.field_0x30) * 4 + 0x1000
y_min = (unk0[1] + field_0x2c) * 4 + 0x1000
y_max = (far_south.unk0[1] - far_south.field_0x2c) * 4 + 0x1000
```

### Softening settings (NEW, not known before)
```
kernel_radius            = 1/0.0625 = 16 cells = 64 grid samples
feather_passes           = 16
cells_per_row            = 256 (0x100)
cell_size                = 28 bytes (0x1c)
entries_per_cell         = max 2
smoothstep               = (3 - 2t) * t²
final_blend(t)           = (1 - smoothstep(max_weight)) * h_orig + smoothstep(max_weight) * h_blend
```

### World and grid (confirmed)
```
grid_sample = world_unit * 4 + 0x1000
world_unit  = (grid_sample - 0x1000) / 4
```

## 10. Open questions (less sure)

1. Why four mapTopHeight buffers? All four get the same value in every call to `UpdateMapTopHeightAndClearOccupancyBit`, so checking that all four are > 0 is the same as checking one. My guess is that they are used differently somewhere else (for example at other detail levels or in other terrain phases), but in the CITY ground patch they act as one occupancy check and as the corners of a 2×2 blend inside the 5×5 pattern.

2. What is `field_0x28`? The check uses it as a connection flag (`!= -1` for the far south-west corner). The struct layout puts it at offset 40 (the first unnamed field after `streetNorth`). My guess is it could be a "diagonal street" index or a padding or height field.

3. The exact clearing-loop step. The decompiler says 0x1c00 but the disassembly shows 0x7000. I think the decompiler is right (0x1c00) and I misread the register reuse in the assembly, because 0x7000 is 7 × 0x1000, which does not fit the cell grid. Confidence: MEDIUM-LOW; another reverser should check.

4. Why a 5×5 pattern inside 4×4 blocks? The 5×5 pattern reads the mapTopHeight buffers and blends bilinearly inside the 4×4 block. With 5 sub-samples in each direction (step 0.25), neighbouring 5×5 patterns share their edge samples. That covers the area with no gaps and without working out the same points twice (the shared samples handle the edges).
