# Round 1 RE: CITY GROUND Terrain — deepseek

**Target binary**: `subrosa.exe.THIS_ONE` (x86-64 PE, image base 0x00000000, 3779 functions)
**Date**: 2026-06-08
**Method**: Ghidra MCP decompile + full disassembly, constant decoding, struct layout inspection

---

## 0. KEY CONSTANTS (IEEE-754 decoded from read_memory)

| Address     | Hex           | IEEE-754  | Symbol              |
|-------------|---------------|-----------|---------------------|
| 0x00278688  | 0x40800000    | **4.0**   | `DAT_00278688`      |
| 0x00278698  | 0x40880000    | **4.25**  | `DAT_00278698`      |
| 0x002783f4  | 0x3F800000    | **1.0**   | `FLOAT_002783f4`    |
| 0x00278280  | 0x3E800000    | **0.25**  | `FLOAT_00278280`    |
| 0x002781bc  | 0x3D800000    | **0.0625**| `FLOAT_002781bc`    |
| 0x00278644  | 0x40400000    | **3.0**   | `DAT_00278644`      |
| 0x00278224  | 0x3E000000    | **0.125** | `FLOAT_00278224`    |

---

## 1. CORNER HEIGHTS — Proven source

### Question: `intersection.unk0[2]*4.0+4.25` OR from mapTopHeight buffers?

**Answer: Direct from `streetIntersection.unk0[2] * 4.0 + 4.25`. The mapTopHeight buffers are read ONLY as a 4-corner occupancy gate and as sample-source for the 5×5 sub-cell feather; they do NOT serve as corner heights for the bilinear fill.**

### Proof: Decompile + Disassembly at 0xd349e–0xd3650

From the decompile (0xd3340):

```c
fVar43 = (float)streetIntersections[param_1].unk0[2] * DAT_00278688 + DAT_00278698;
fVar44 = (float)streetIntersections[lVar28].unk0[2] * DAT_00278688 + DAT_00278698;
fVar36 = (float)streetIntersections[lVar14].unk0[2] * DAT_00278688 + DAT_00278698;
fVar38 = (float)streetIntersections[lVar15].unk0[2] * DAT_00278688 + DAT_00278698;
```

Where `DAT_00278688 = 4.0` and `DAT_00278698 = 4.25`. So each corner = `unk0[2] * 4.0 + 4.25`.

The four corners correspond to the four intersections forming the city block:
- **TL**: `param_1` (the "NE" intersection — top-left in the patch's param order)
- **TR**: `lVar28` (far-south intersection — but mapped to top-right in bilinear fill)
- **BL**: `lVar14` (far-west intersection)
- **BR**: `lVar15` (far-southwest intersection)

The mapTopHeight buffers (`DAT_3ce6f5f8`, `DAT_3ce6f5fc`, `DAT_3ce735f8`, `DAT_3ce735fc`) are read in the feather phase at 0xd382f–0xd3850 as a **precondition gate** and as a **5×5 sub-sample source** — NOT as corner height sources.

### What writes the mapTopHeight buffers?

`UpdateMapTopHeightAndClearOccupancyBit` @0x3fa40 writes `(int)uVar19 * 4.0` (street surface height, WITHOUT the +4.25) to ALL FOUR buffers simultaneously. Called by:
- `MapConnector_PlaceStreetSegmentTilesAndSlopes` @0x20580
- `ApplyAllCityDistrictCellsToWorld` @0x20000
- `InstantiateSbbVolumeIntoWorldAndMetadata` @0x23ac0
- `World_LoadMapAndInitializeTerrain` @0x56980

The street surface height = `intersection.unk0[2] * 4.0` (no +4.25).

The patch writes a surface **0.25 world-units HIGHER** than the street tiles (`4.0*level + 4.25` vs `4.0*level` for street floors). This is the road SURFACE (above the tile floor) — likely the drivable road level.

---

## 2. PATCH RECTANGLE — Exact bounds

### Question: Which fields define the rectangle? Gap between FOUR intersections or single intersection footprint?

**Answer: The rectangle spans the gap between FOUR intersections forming a city block — NOT a single intersection. The `*4 + 0x1000` maps world coordinates to grid-sample coordinates (1 unit = 4 samples, bias 4096).**

### Rectangle bounds formula

From the decompile at 0xd34e0–0xd366b:

```
x_min = (pos.x + field_0x30) * 4 + 0x1000      // east edge of param_1 (NE corner)
x_max = (pos.x - field_0x30) * 4 + 0x1000      // west edge of far-west corner (lVar14)

y_min = (unk0[1] + field_0x2c) * 4 + 0x1000    // south edge of param_1
y_max = (unk0[1] - field_0x2c) * 4 + 0x1000    // north edge of far-south corner (lVar28)
```

### Field identification (StreetIntersection struct, 136 bytes)

From `get_struct_layout("StreetIntersection")`:

| Offset | Size | Type       | Name           | Role                        |
|--------|------|------------|----------------|-----------------------------|
| 0      | 12   | int[3]     | unk0           | [0]=X?, [1]=Y-pos, [2]=height-level |
| 12     | 12   | Vector     | pos            | .x = world X-coord          |
| 40     | 4    | int (unnamed) | field_0x28 | connectivity flag (-1=dead) |
| 44     | 4    | int (unnamed) | field_0x2c | half-extent +Y (south)      |
| 48     | 4    | int (unnamed) | field_0x30 | half-extent +X (east)       |
| 52     | 4    | int (unnamed) | field_0x34 | half-extent -Y (north)      |
| 56     | 4    | int (unnamed) | field_0x38 | half-extent -X (west)       |

**Confidence: HIGH** — confirmed by both struct layout and usage patterns in MapConnector @0x20580:
```c
// Horizontal street: uses +field_0x2c for south end, -field_0x34 for north end
uVar3 = streetIntersections[iVar6].unk0[1] + *(int *)&streetIntersections[iVar6].field_0x2c;
iVar18 = (streetIntersections[iVar13].unk0[1] - *(int *)&streetIntersections[iVar13].field_0x34) - uVar3;
```

### Size calculation

```
width  = iVar32 = x_max - x_min  (grid samples)
height = iVar34 = y_max - y_min  (grid samples)
```

This covers the INTERIOR of the city block — from the inner road edge of one intersection to the inner road edge of the opposite intersection. The streets themselves are placed separately by `MapConnector_PlaceStreetSegmentTilesAndSlopes`.

---

## 3. PRECONDITION — 4-corner block gate

### Proof from decompile at 0xd3367–0xd3498

The precondition verifies that all FOUR corners of the city block exist and are connected by streets:

```
streetIntersections[param_1].streetSouth != -1
  AND (recursive south or west check on the southern intersection)
  AND streetIntersections[param_1].streetWest != -1
  AND (recursive west or south check on the western intersection)
  AND streetIntersections[lVar14].streetSouth != -1  (far-west has south street)
  AND streetIntersections[lVar15].field_0x28 != -1    (far-SW corner is valid)
```

This ensures a complete 4-way city block (param_1 → south → SW, param_1 → west → SW). The recursion allows for one level of indirection (if a street corner lacks a direct west/south, it checks again one step further). Without this "closure," the patch is skipped.

---

## 4. FEATHER/SMOOTHING — Complete pass breakdown

### 4.1 Scratch buffer layout

Base address: `DAT_04afa9bc` (count array starts at `DAT_04afa9a0` = `DAT_04afa9bc - 0x1c`)

Memory map:
```
DAT_04afa9a0  count[row*0x100 + col]   (int32, at byte offset cell_idx * 0x1c)
DAT_04afa9a4  col_slot[row*0x100 + col].col[0..1]  (int32*2)
DAT_04afa9a8  row_slot[row*0x100 + col].row[0..1]  (int32*2)
DAT_04afa9ac  weight[row*0x100 + col].weight[0..1] (float*2)
```

- Each "cell" occupies **0x1c = 28 bytes**: count (4) + 2 entries × 12 bytes (col+row+weight)
- Max **2 entries per cell** (total size = 4 + 24 = 28)
- **Row stride**: 0x100 cells × 0x1c = **0x1c00 bytes** = 7168
- **Grid dimensions**: (iVar32>>2) × (iVar34>>2) cells, where each cell maps to **4×4 grid samples**
- Physical layout: `buffer[row * 0x100 + col]` — 256 columns per row

### 4.2 Phase 1 — Bilinear fill (initial patch)

At 0xd3680–0xd3710: For every grid sample within the rectangle, writes a bilinearly interpolated height from the four intersection corner heights using `Terrain_SetLandSampleWithTileBorderMirroring`:

```
for y in [y_min..y_max]:
    t_y = (y - y_min) / (y_max - y_min)
    for x in [x_min..x_max]:
        t_x = (x - x_min) / (x_max - x_min)
        h = (1-t_y)*((1-t_x)*h_TL + t_x*h_TR) + t_y*((1-t_x)*h_BL + t_x*h_BR)
        Terrain_SetLandSampleWithTileBorderMirroring(y, x, h)
```

### 4.3 Phase 2 — Scratch buffer init (zeroing)

At 0xd371b–0xd37a4: Zeros the scratch buffer region covering (iVar32>>2)×(iVar34>>2) cells plus one-ring ghost cells. Each init zeroes 5 cells in a column (ghost cell + 4 actual cells) across 5 columns (ghost + 4 actual).

### 4.4 Phase 3 — 5×5 sub-cell feather ACCUMULATION

At 0xd37b9–0xd3a0c: For each 4×4 block where **all four mapTopHeight buffers > 0.0**:

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

**Key insight**: The 5×5 kernel reads from the same four mapTopHeight buffers as 4-corner bilinear interpolation, generating 25 sub-samples per 4×4 block. This creates a higher-resolution sampling of the street surface that can resolve sub-block detail.

### 4.5 Phase 4 — Scattered neighbor propagation (16 passes)

At 0xd3a1f–0xd3c8d (outer `local_120 = 0x10` = 16 iterations):

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

The dot-product test ensures directional consistency: only propagate data from neighbors that "point toward" the empty cell. The `0.0625` kernel radius means influence decays to zero at distance = `1.0 / 0.0625 = 16` cells (i.e., `16 * 4 = 64` grid samples).

**16 passes** (confidence: HIGH, confirmed by disassembly at 0xd3a28: `MOV EDX, 0x10`).

### 4.6 Phase 5 — Final smoothstep merge with original terrain

At 0xd3c93–0xd4048: For every cell in the scratch region where count > 0:

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

**This is the KEY feather mechanism**: The `max_weight` smoothstep controls how much of the patched terrain vs. original terrain is used. When a cell accumulated many samples (high max_weight), t2 ≈ 1 → result ≈ blend (the patched road). When a cell has sparse data (low max_weight), t2 ≈ 0 → result ≈ h_orig (the island terrain). This creates a smooth, natural-looking transition.

### Smoothstep formula (confirmed from disassembly at 0xd3f40–0xd3f76)

```
XMM9 = 3.0           // DAT_00278644
XMM0 = t + t         // 2*t
XMM1 = 3.0 - XMM0    // 3 - 2t
XMM2 = t * t         // t²
XMM1 = XMM1 * XMM2   // (3-2t)*t² = smoothstep output
```

---

## 5. WHICH INTERSECTIONS GET PATCHED

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

**28 calls to 28 distinct intersection indices** (only for non-autogenerated maps). Each call's internal precondition (4-corner connectivity test) independently determines whether the patch actually executes.

**Why a subset?** These are the intersections that form the central city grid. The precondition at the top of `Terrain_ApplyStreetIntersectionHeightPatch` ensures only intersections that form a complete 4-way city block (with all four corners existing and interconnected) actually get patched. The 28 listed intersections are those candidate NE-corners; those that don't form a complete block silently return.

---

## 6. WORLD↔GRID COORDINATE MAPPING

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

**Key facts**:
- `>> 0xb` (= >> 11 = / 2048): Each terrain chunk is **2049×2049** samples (0x801 = 2049)
- `chunk_stride = 0x684` bytes = 1668 → `1668 / 4 = 417` floats → This is NOT 2049². The chunk is a pointer array, not directly allocated inline
- 8×8 = 64 chunks total
- World→grid: `grid = (world + 4096) * 4` ← the `*4 + 0x1000` pattern in rectangle computation
  - That is: `grid_sample = world_unit * 4 + 0x1000`
  - So: `world = (grid_sample - 0x1000) / 4`
- Grid origin offset: +4096 samples (= +1024 world units) places the world origin in the center of the grid
- Max grid extent: 0x7ff × 0x801 ≈ 2047 × 2049 per chunk in 8×8 = 16384 × 16384 total

The `0x1000` (=4096) offset shifts the grid so that negative world coordinates (down to -1024) map to positive grid samples. The `*4` gives 4 samples per world unit. The CITY region operates in this same unified grid.

---

## 7. SCRATCH BUFFER ADDRESSING — precise layout

Confirmed from disassembly at 0xd372f–0xd37a4 and 0xd37f8–0xd3986:

| Symbol              | Address     | Offset within cell | Type  | Description              |
|---------------------|-------------|---------------------|-------|--------------------------|
| `DAT_04afa9a0`     | 0x04afa9a0  | +0x00               | int32 | Entry count (0–2)        |
| `DAT_04afa9a4`     | 0x04afa9a4  | +0x04               | int32 | Entry[0].col (x_base+col)|
| `DAT_04afa9a8`     | 0x04afa9a8  | +0x08               | int32 | Entry[0].row (y_base+row)|
| `DAT_04afa9ac`     | 0x04afa9ac  | +0x0C               | float | Entry[0].weight          |
|                     |             | +0x10               | int32 | Entry[1].col             |
|                     |             | +0x14               | int32 | Entry[1].row             |
|                     |             | +0x18               | float | Entry[1].weight          |

**Total cell size**: 0x1C = 28 bytes (count int + 2 entries × 12 bytes)

**Indexing**: `cell[row][col] = base + (row * 0x100 + col) * 0x1c`

Where `row` and `col` are in "block" coordinates (each block = 4×4 grid samples). The scratch array covers the patch rectangle rounded up to the nearest 4-sample boundary.

---

## 8. DECOMPILER BUGS / REGISTER ALIASING CORRECTIONS

The Ghidra decompiler incorrectly aliases several registers in the feather kernel. Verified against the raw disassembly:

| Location          | Decompiler claim            | Actual from disassembly        | Correction |
|--------------------|------------------------------|--------------------------------|------------|
| Init loop stride  | `puVar29 += 0x1c00`         | `ADD R9, 0x7000`               | Actual row stride = 0x7000. Decompiled 0x1c00 is WRONG for the outer loop. The 0x1c00 is the 5×5 kernel row stride.|
| 5×5 cell index    | `(local_118+local_120)*0x70`| `IMUL R14, RAX, 0x70`         | Verified: 0x70 = 112 = 4 cells. |
| Feather iterations| `local_120 = 0x10`          | `MOV EDX, 0x10`                | 16 passes confirmed.        |

**Important correction on init stride**: The assembly at 0xd3799 shows `ADD R9, 0x7000`, but the decompiler shows `puVar29 = puVar29 + 0x1c00`. The decompiler is wrong here — the actual outer row stride for the init phase is **0x7000 bytes** (not 0x1c00). However, the INIT phase uses 0x7000 while the FEATHER phase uses 0x1c00 as the cell-grid row stride. This is because the init loop operates on a different grid granularity than the feather. The feather's `(local_118 + local_120) * 0x70` with `local_118 += 0x100` gives `row * 0x100 * 0x70 = row * 0x7000`, matching the init stride. **Confidence: MEDIUM** on the exact init-stride value — the decompiler may be correct and I may be misreading the assembly register reuse. The 0x1c00 (256 cells × 28 bytes) is the more logical value since the feather phase clearly uses 0x100 cells per row.

---

## 9. SUMMARY — Critical formulas for port accuracy

### Corner heights (GOOD — confirmed)
```
h_corner = intersection.unk0[2] * 4.0 + 4.25
```
Each corner uses its own intersection's `unk0[2]`.

### Rectangle bounds (GOOD — confirmed)
```
x_min = (pos.x + field_0x30) * 4 + 0x1000    // world→grid
x_max = (far_west.pos.x - far_west.field_0x30) * 4 + 0x1000
y_min = (unk0[1] + field_0x2c) * 4 + 0x1000
y_max = (far_south.unk0[1] - far_south.field_0x2c) * 4 + 0x1000
```

### Feather parameters (NEW — previously unknown)
```
kernel_radius            = 1/0.0625 = 16 cells = 64 grid samples
feather_passes           = 16
cells_per_row            = 256 (0x100)
cell_size                = 28 bytes (0x1c)
entries_per_cell         = max 2
smoothstep               = (3 - 2t) * t²
final_blend(t)           = (1 - smoothstep(max_weight)) * h_orig + smoothstep(max_weight) * h_blend
```

### World↔grid (GOOD — confirmed)
```
grid_sample = world_unit * 4 + 0x1000
world_unit  = (grid_sample - 0x1000) / 4
```

---

## 10. OPEN QUESTIONS (lower confidence)

1. **Why four mapTopHeight buffers?** All four are written with the same value in every call to `UpdateMapTopHeightAndClearOccupancyBit`. The 4-corner test `all > 0` is therefore equivalent to testing a single buffer. **Speculation**: They may be used differently elsewhere (e.g., at different LODs or for different terrain generation phases), but within the CITY GROUND patch they serve as a unified occupancy test and a 2×2 interpolation source within the 5×5 kernel.

2. **What does `field_0x28` represent?** It's used in the precondition as a connectivity flag (`!= -1` for the far-SW corner). The struct layout shows it at offset 40 (the first unnamed field after `streetNorth`). **Speculation**: It may be the "diagonal street" index or a "padding/height" field.

3. **Exact init-loop stride**: The decompiler says 0x1c00 but the disassembly shows 0x7000. I believe the decompiler is correct (0x1c00) and I've misread the register reuse in the assembly. The 0x7000 would correspond to 7 × 0x1000 which doesn't fit the cell grid model. **Confidence: MEDIUM-LOW**; recommend cross-checking with another reverser.

4. **Why 5×5 sub-kernel within 4×4 blocks?** The 5×5 kernel reads the mapTopHeight buffers and bilinearly interpolates within the 4×4 block. With 5 sub-samples in each direction (step 0.25), the corners of adjacent 5×5 kernels overlap by 1 sample — creating continuous coverage without gaps and without redundant computation at the exact same sample points (the overlap handles the edges).

---
