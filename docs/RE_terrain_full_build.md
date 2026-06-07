# Sub Rosa TERRAIN BUILD / DATA Pipeline — Full Reverse Engineering Spec

> Target: `subrosa.exe.THIS_ONE` (PE x86:LE:64, image base 0)
> All addresses in `0x000xxxxx` range. Read-only analysis via Ghidra MCP.

---

## 1. Call Order — Numbered Pipeline

World_LoadMapAndInitializeTerrain @ `0x56980` drives the full terrain lifecycle:

| Step | Function | Address | What it does |
|------|----------|---------|-------------|
| 1 | `Render_InitTerrainStreamingBuffers` | `0xc2db0` | Allocates GL vertex/index buffers for 7 LOD pools; resets slot metadata |
| 2 | `Terrain_GenerateIslandHeightfield` | `0x56560` | Fills `g_terrainHeightfield` (2049x2049 float array) with procedural noise + radial falloff |
| 3 | `Memory_FillBytes(g_landUseMaskBits, 0, 0x4000000)` | — | Resets the 4096x4096 u16 land-use bitmask to all 0xFFFF |
| 4 | `_createarea` | `0x4db20` | Creates a 64x8x64 SBB working volume for map-area entities |
| 5 | …Street graph, district, building instantiation loop… | — | Calls `UpdateMapTopHeightAndClearOccupancyBit` for every placed cell |
| 6 | `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches` | `0xd12e0` | The big one — see §3 below |
| 7 | …Cell render geometry rebuild loop… | — | `BuildCellRenderGeometryAndAdjacency` for each active world cell |
| 8 | …Street label textures… | — | `RenderStreetLabelTexture` for 17 labels |

---

## 2. Data Structures

### 2.1 `g_terrainHeightfield`
- **Address:** `0x2d48282c` (start of data; label `g_terrainHeightfield` at `0x2d482830`)
- **Layout:** `float[0x801 * 0x801]` = `float[2049][2049]`
- **Size:** `2049 * 2049 * 4 = 0x1004004` bytes
- **Content:** Procedural heightfield (Y = stored height directly, no scaling)
- **Grid-to-world:** `worldX = gridX - 0x280` (4096); `worldZ = gridZ - 0x280` (4096)
- **Sentinel convention:** `DAT_2d482820` and `DAT_2d482828` are both set to `0xc5800000` (= –4096.0f) at the very start of the function — these are the two leading float slots just before the array body. They are NOT part of the heightfield but serve as border-guard sentinels for out-of-bounds sample lookup during bilinear resampling.

### 2.2 `g_landTileHeightChunks`
- **Base array:** at offset `+0x18` within the chunk record (see §2.5), label `0x411bf858`.
- **Per-entry:** A `float[0x801 * 0x801]` heap allocation (= `2049 * 2049 * 4 = ~16 MB` per tile)
- **Count:** 64 entries (8 row × 8 column tiles)
- **Addressing:** `chunk_tile_x = uVar3 = sample_x >> 11`, `chunk_tile_y = uVar2 = sample_y >> 11`, index = `uVar3 + uVar2 * 8`. Within an entry: `sample_offset = (sample_y & 0x7ff) * 0x801 + (sample_x & 0x7ff)`.
- **Border mirroring** (see §4): when `(sample_x & 0x7ff) == 0`, the value is also written to the *previous column tile's* right edge; when `(sample_y & 0x7ff) == 0`, it is written to the *previous row tile's* bottom edge.

### 2.3 The Four `mapTopHeight` Buffers
Defined in `UpdateMapTopHeightAndClearOccupancyBit` @ `0x3fa40`:

| Buffer | Address | Derivation |
|--------|---------|-----------|
| `mapTopHeight0` | `0x3ce6f5f8` | `*4` as `float[0x1000 * 0x1000]` |
| `mapTopHeight1` | `0x3ce6f5fc` | +4 bytes → second float per cell |
| `mapTopHeight2` | `0x3ce735f8` | +0x4000 → next row (0x3ce6f5fc + 0x4000) |
| `mapTopHeight3` | `0x3ce735fc` | +4 bytes from buffer2 |

Each is `float[0x1000 * 0x1000]` = 4096x4096 = 67 MB per buffer, total 268 MB.
They are elementwise max-updated: `if (current <= height && height != current) current = height`.
This is a 4-sample "top-height" surface used to evict overlapping buildings (occupancy check).

### 2.4 `g_landUseMaskBits`
- **Address:** `g_landUseMaskBits` label
- **Layout:** `uint16[0x400 * 0x400]` = `uint16[1024][1024]` = 2 MB
- **Coords:** `(floorX + 0x400) >> 2`, `(floorZ + 0x400) >> 2`
- **Bit:** `bit = (floorX & 3) + (floorZ & 3) * 4` — a 4x4 micro-grid per u16 cell

### 2.5 Chunk World-Origin Record (`0x411bf840`)
- **Array stride:** `0x3420` = 13344 bytes (verified in disassembly: `imul rax, rax, 0x3420`)
- **Count:** 64 entries (8x8 chunks)
- **Per-record layout:**

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| `+0x00` | 4 | `worldOriginX` (float) | World-space X of chunk corner |
| `+0x04` | 4 | `worldOriginY` (float) | World-space Y (base elevation) |
| `+0x08` | 4 | `worldOriginZ` (float) | World-space Z of chunk corner |
| `+0x0c` | 4 | `tileCoordX` (int) | Which land-tile column (0-7) |
| `+0x10` | 4 | `tileCoordZ` (int) | Which land-tile row (0-7) |
| `+0x14` | 4 | (padding / unused) | — |
| `+0x18` | 8 | `pHeightData` (ptr) | Pointer to this chunk's `float[0x801][0x801]` |
| `+0x20` | 12 | (unknown/unused) | — |
| `+0x2c` | 4 | `subSlotCoordX[i]` (int) | Grid X offset for sub-slot i |
| `+0x30` | 4 | `subSlotCoordZ[i]` (int) | Grid Z offset for sub-slot i |
| … | … | 256 sub-slot records | Each `0x34` bytes |

The 256 sub-slots per chunk encode a `16x16` grid of "mini-tiles". Each sub-slot spans `0x80 × 0x80` samples (= 128×128 in the land-tile). A full chunk = `16 * 128 = 2048` samples + 1 overlap column = 2049 total.

**Chunk world-origin assignment:** The world origin for chunk `(tx, tz)` is simply the tile coordinate scaled into world space:
```
worldOriginX = tileCoordX * 2048.0f   (i.e., tileCoordX << 11)
worldOriginZ = tileCoordZ * 2048.0f
worldOriginY = 0 (overwritten per-subslot)
```
(Verified in `Terrain_RebuildChunkVertexBufferAndNormals` @ `0xd00f0`: `shl edx, 0xb` where EDX = tileCoordX, giving `tileCoordX * 2048 → worldOriginX`.)

### 2.6 Vertex and Index Buffers

**Vertex scratch buffer:**
- **Address:** `0x40e7f720`
- **Layout:** Interleaved `float[6]` per vertex: X, Y, Z, Nx, Ny, Nz
- **Vertex stride:** 24 bytes
- **Max capacity:** enough for LOD-0 full-chunk + sub-slot rebuilds

**GL vertex buffers:**
- Array at `0x43e9b0c0` stores GL buffer object names
- Array at `0x43e98c04` / `0x43e98c8c` stores slot indices
- Binding: `glBindBuffer(GL_ARRAY_BUFFER, names[idx])` then `glBufferData(…, size, data, GL_DYNAMIC_DRAW)`

**GL index buffers:**
- Array at `0x43e9b0c4` stores GL EBO names from `Terrain_InitLandscapeChunkLodIndexBuffers`

### 2.7 LOD Slot Pools
Six LOD slot pools (LOD 0–6) with per-slot metadata:

| Pool | ChunkId Base | Age Base | SlotCount Addr | SlotsBase |
|------|-------------|---------|---------------|-----------|
| LOD0 | `0x410ff798` | `0x410ff7a0` | `0x410ff788` | `0x410ff780` |
| LOD1 | `0x411177b0` | `0x411177b8` | `0x411177a0` | `0x41117798` |
| … | … | … | … | … |

Each slot: 3 int32s = `{chunkId, subSlot, age/active}`. Active = 4, decaying = 4 → 0.

### 2.8 Intersection Patch Scratch Buffer
- **Address:** `0x04afa9a0` (static 87 MB buffer)
- **Purpose:** Holds sparse candidate sample positions + per-sample distance-weight lists during the multi-pass feather/blend algorithm in `Terrain_ApplyStreetIntersectionHeightPatch`.
- **Layout:**
  - `DAT_04afa9a0[i]` (int): count of candidate weights at slot i
  - `DAT_04afa9a4[i + j*12]` (int): grid X of j-th candidate
  - `DAT_04afa9a8[i + j*12]` (int): grid Z of j-th candidate
  - `DAT_04afa9ac[i + j*12]` (float): weight of j-th candidate
- **Stride:** 7 dwords (28 bytes) per grid cell; grid is `(iVar34/4+1) × (iVar32/4+1)` cells, each spanning 4×4 heightfield samples
- **Multi-pass output staging:** `DAT_04afc5c4` (offset `+0x1c24` from start) — the accumulated per-sample weight list for the smoothstep merge

---

## 3. `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches` @ `0xd12e0`

### 3.1 The Upsample: 2049² → 8×8 chunks at 16384²

The function iterates over 64 tile positions (8×8). For each tile `(tx, tz)`:
1. Reads 4 corner height values from `g_terrainHeightfield`:
   ```
   h00 = heightfield[tz*2049 + tx]       // tile corner NW
   h01 = heightfield[tz*2049 + tx + 1]    // NE
   h10 = heightfield[(tz+1)*2049 + tx]    // SW
   h11 = heightfield[(tz+1)*2049 + tx+1]  // SE
   ```
2. For each sub-sample `(sx, sz)` where `sx = 0..7, sz = 0..7`:
   ```
   fx = sx * 0.125   // = sx * FLOAT_00278224 (0x3e000000 = 0.125)
   fz = sz * 0.125
   height = (1-fx)*( (1-fz)*h00 + fz*h10 ) + fx*( (1-fz)*h01 + fz*h11 )
   ```
   This is standard bilinear interpolation with a 1/8 step, resampling each 2049² tile into 8×8 sub-tile cells.
3. Calls `Terrain_SetLandSampleWithTileBorderMirroring(tileX + sx, tileZ + sz, height)` — see §4.

### 3.2 Intersection Patch Application
When `isMapAutoGenerated == 0`, 28 hardcoded intersection indices are patched:
```
{2, 40, 32, 46, 45, 47, 28, 10, 33, 42, 35, 13, 59, 31,
 52, 48, 50, 19, 22, 58, 62, 61, 41, 36, 43, 44, 38, 39}
```
Each calls `Terrain_ApplyStreetIntersectionHeightPatch(idx)` — see §5.

### 3.3 Chunk Vertex Buffer Rebuild
Calls `Terrain_RebuildChunkVertexBufferAndNormals(i)` for `i = 0..63`.

### 3.4 Slot Cache Invalidation
Clears all 7 LOD slot pools (LOD0–LOD6) by marking every slot `chunkId = -1` and resetting age/stats.

---

## 4. `Terrain_SetLandSampleWithTileBorderMirroring` @ `0xd2db0`

```c
void SetLandSample(uint sampleX, uint sampleZ, float height)
{
    uint tileX = sampleX >> 11;   // 0..7
    uint tileZ = sampleZ >> 11;   // 0..7
    if (tileX > 7 || tileZ > 7) return;

    uint64 index = tileX + tileZ * 8;
    uint localX = sampleX & 0x7ff;
    uint localZ = sampleZ & 0x7ff;

    // Primary write
    uint64 offset = (localZ * 0x801 + localX) * 4;
    g_landTileHeightChunks[index * 0x684 + offset] = height;

    // Border mirroring
    if (localX == 0) {
        // Copy to right edge of the tile to the left (-X)
        if (tileZ > 0 && tileX > 0)
            g_landTileHeightChunks[(index - 9) * 0x684 + 0x10004000] = height;
        if (tileX > 0)
            g_landTileHeightChunks[(index - 1) * 0x684 + 0x2000 + localZ*0x801*4] = height;
    }
    if (localZ == 0) {
        // Copy to bottom edge of the tile above (-Z)
        if (tileZ > 0)
            g_landTileHeightChunks[(index - 8) * 0x684 + 0x10002000 + localX*4] = height;
    }
}
```

The `0x684` stride used by the decompiler is the *pointer-to-data array* stride — each pointer is 8 bytes, and the decompiler inferred a sub-structure size. The actual per-tile height buffer is separately allocated and pointed to by each entry in the `0x411bf858` array.

Key insight: samples on tile edges (localX=0 or localZ=0) are mirrored into the neighboring tile's opposite edge, ensuring seamless bilinear sampling across tile boundaries.

---

## 5. `Terrain_ApplyStreetIntersectionHeightPatch` @ `0xd3340` — Complete Algorithm

### 5.1 Precondition Check
The function verifies a fully-connected 4-way intersection block exists. It traverses the street graph: if the target intersection `param_1` has south+west neighbors and those south of those also have west neighbors, the block is valid for patching.

### 5.2 Phase 1: Bilinear Base Surface
Parameters (in heightfield sample coords, centered on the intersection block):
```
x0 = (intersection.pos.x + field_0x30) * 4 + 0x1000
x1 = (southIntersection.pos.x - field_0x2c) * 4 + 0x1000
z0 = (intersection.unk0[1] + field_0x2c) * 4 + 0x1000      // Note: unk0[1] is grid row
z1 = (southIntersection.unk0[1] - field_0x2c) * 4 + 0x1000
```

Four corner heights at the four intersections of the block:
```
hNW = intersection.unk0[2] * 4.0 + 4.25        // 4.0 = DAT_00278688, 4.25 = DAT_00278698
hNE = southIntersection.unk0[2] * 4.0 + 4.25
hSW = westIntersection.unk0[2] * 4.0 + 4.25
hSE = farSouthWestIntersection.unk0[2] * 4.0 + 4.25
```

Then bilinear interpolation over the rectangle `[x0, x1] × [z0, z1]` with steps `1/(x1-x0)` and `1/(z1-z0)`:
```
for (x = x0; x <= x1; x++)
  for (z = z0; z <= z1; z++)
    SetLandSample(x, z, bilinear(hNW, hNE, hSW, hSE, fx, fz));
```

This writes a flat drivable road surface. Note the +0x1000 offset: heightfield coords are shifted by 4096 to cover the map space centered around the city grid.

### 5.3 Phase 2: Sparse Candidate Collection
The intersection rectangle is divided into a coarse grid of 4×4-heightfield-sample cells:
```
cellsX = (x1 - x0) >> 2   // columns in the coarse grid
cellsZ = (z1 - z0) >> 2   // rows
```

Each cell is probed in a 5×5 sub-sample grid within the 4 sampled `mapTopHeight` buffers. If all four mapTopHeight buffers have `> 0.0` values at that location, the cell is a "road interior" candidate.

For each valid cell, a 5×5 pattern of candidate sample positions is generated and stored in the scratch buffer at `0x04afa9a0`. Each candidate stores its grid coordinates and an initial weight of `1.0`.

### 5.4 Phase 3: Multi-Pass Distance-Weight Propagation (16 passes)
```
for (pass = 0; pass < 16; pass++) {       // local_120 = 0x10
    for (each grid cell) {
        for (each candidate in cell) {
            // Look at 8-neighbor cells and gather neighbor candidates
            for (ny = -1; ny <= 1; ny++)
            for (nx = -1; nx <= 1; nx++) {
                if (nx == 0 && ny == 0) continue;
                for (each neighbor candidate) {
                    float dist = sqrt((nx_diff)² + (ny_diff)²);
                    float w = 1.0 - dist * 0.0625;   // FLOAT_002781bc = 1/16
                    if (w <= 0.0) continue;

                    // Directional gradient (push vector)
                    float dx = (1.0/dist) * dx_diff;
                    float dy = (1.0/dist) * dy_diff;

                    // If no candidates yet, add neighbor candidate directly
                    if (cell_empty) {
                        store candidate coords and weight w;
                        cell_count++;
                    }
                    // Otherwise, insert/replace if this weight is better
                    // (peeking per-direction using dot product test)
                    else if (dot(neighbor_vec, stored_vec) > 0 && w >= best_stored) {
                        replace_or_insert(candidate, weight w);
                    }
                    // Not enough candidates yet (cap at 2 per cell)
                    else if (cell_count < 2) {
                        append(candidate, weight w);
                    }
                }
            }
        }
    }
}
```

- **Kernel:** 8-neighbor dilation with `1/16` edge-length decay rate
- **Cap:** 2 candidates per cell
- **Distance weighting:** `w = 1.0 - dist * (1/16)`, clamped to [0, 1]
- **Directional filtering:** candidates are selected based on dot-product similarity with existing candidates, only replacing existing entries if they have higher weight AND the directions are parallel (dot > 0)

### 5.5 Phase 4: Smoothstep Merge (Final Blending)
```
float smoothstep3 = 3.0;   // FLOAT_00278644 = 0x40400000 = 3.0

for (z = z0+1; z < z1; z++) {
  for (x = x0+1; x < x1; x++) {
    // Read current tile height for this sample
    currentHeight = GetLandTileHeight(x, z);   // with tile/border fallback

    // Accumulate weighted sum from stored candidates
    float totalWeight = 0.0;
    for (each stored candidate at (x,z)) {
        totalWeight += candidate.weight;
    }

    // Normalize and compute smoothstep blend factor
    float normalizedWeight;
    if (numCandidates < 2) {
        // Direct copy: use target sample from grid as-is
        normalizedWeight = targetSampleHeight;  // from land tile
    } else {
        normalizedWeight = candidate_weights[0] / totalWeight;
        // Clamp: min(normalizedWeight, 1.0)
        normalizedWeight = min(normalizedWeight, 1.0);

        // Smoothstep: (3 - 2*w) * w * w
        // This is 3w² - 2w³  = smoothstep(w)
        float s = (smoothstep3 - (normalizedWeight + normalizedWeight)) * normalizedWeight * normalizedWeight;

        // Linear blend with smoothstep:
        // Blend the surrounding land height with a second candidate's target height
        height2 = GetLandTileHeight(candidates[1].x, candidates[1].z);
        normalizedWeight = (1.0 - s) * height2 + s * currentHeight;
    }

    // Final max-weight-seed contribution
    float maxWeight = max of all candidate weights in this cell;
    maxWeight = clamp(maxWeight, 0.0, 1.0);
    float smoothFactor = (smoothstep3 - (maxWeight + maxWeight)) * maxWeight * maxWeight;

    // Write: (1-smoothFactor)*currentHeight + smoothFactor*normalizedHeight
    SetLandSample(x, z, (1.0 - smoothFactor) * currentHeight + normalizedHeight * smoothFactor);
  }
}
```

This performs a cubic smoothstep blend between the original terrain height and the patch-induced height, with the per-cell weight maximum determining the blend ratio. The result is a smoothly feathered road surface that blends seamlessly into the surrounding terrain.

---

## 6. Vertex / Normal Generation

### 6.1 `Terrain_RebuildChunkVertexBufferAndNormals` @ `0xd00f0`

For one chunk (param_1 = 0..63), generates a full LOD-0 vertex buffer:

```
chunkIdx = param_1
tileX = g_terrainChunkTileCoordX[chunkIdx]       // tile column 0-7
tileZ = g_terrainChunkTileCoordZ[chunkIdx]       // tile row 0-7
pHeight = g_landTileHeightChunks[chunkIdx]       // pointer to float[2049][2049]
wx = g_terrainChunkWorldOriginX[chunkIdx]        // = tileX * 2048.0
wy = g_terrainChunkWorldOriginY[chunkIdx]        // base Y
wz = g_terrainChunkWorldOriginZ[chunkIdx]        // = tileZ * 2048.0
```

**Grid dimensions:** `0x802 × 0x802` = 2050×2050 samples (with a half-step inset on each edge). The actual generated coverage is `129 × 129` vertices for LOD-0, stepped at 0x80×0x80 (128) sample intervals to cover the 2048×2048 sample region (plus one extra column/row for edge stitching).

Wait — re-examining the disassembly:
- Outer loop: `R14D` from −1 to 0x801 (= 2050 samples)
- Inner loop: `R10D` from −1 to 0x801
- The negative initial values act as sentinels for border clamping (when `R14D < 0`, use sample index 0; when `R10D < 0`, use 0)
- Steps: 0x7f (127) per iteration, plus 1, for 0x80 samples per step

**Per-vertex output (6 floats = 24 bytes):**
```
vertex[idx*6 + 0] = worldX + sampleX            // world position X
vertex[idx*6 + 1] = height + worldY             // world position Y  (with -16.0 edge penalty for out-of-bounds)
vertex[idx*6 + 2] = worldZ + sampleZ            // world position Z
vertex[idx*6 + 3] = nx                           // normal X
vertex[idx*6 + 4] = ny                           // normal Y
vertex[idx*6 + 5] = nz                           // normal Z
```

**Normal computation:**
```
// Sample neighbors ±128 in X and Z
h_left  = GetHeightAt(sampleX - 0x80, sampleZ)   // or 0 if OOB
h_right = GetHeightAt(sampleX + 0x80, sampleZ)
h_near  = GetHeightAt(sampleX, sampleZ - 0x80)    // or 0 if OOB
h_far   = GetHeightAt(sampleX, sampleZ + 0x80)

dx = h_left - h_right
dz = h_near - h_far

// Convert to world-space gradient (distance between samples = 128 * 0.0078125 = 1.0 unit)
dx_world = dx * 0.0078125    // DAT_00278118 = 0x3c000000 = 1/128
dz_world = dz * 0.0078125

// Build unnormalized normal: (dx*w, dy, dz*w)
nx = dx_world
ny = 1.0                     // FLOAT_002783f4 = 0x3f800000 = 1.0
nz = dz_world

// Normalize
float len = sqrt( nx² + ny² + nz² + 0.0000610 )   // small epsilon from DAT_0027806c
if (len != 0.0) {
    inv_len = 1.0 / len
    nx = dx_world * 0.0078125 * inv_len
    ny = 1.0 * inv_len
    nz = dz_world * 0.0078125 * inv_len
}
```

Note the two-step scaling of `dx` and `dz`: first multiply by `1/128` to get the world-space slope, then re-multiply by `1/128` during normalization? No — the `fVar8 = DAT_00278118 = 1/128`, and it's applied to both the unnormalized nx/nz AND during normalization (multiplying by `fVar8` before dividing by length). Actually re-reading the decompile:
```
fVar28 = fVar26 - fVar27;    // h_near - h_far
fVar26 = SQRT((fVar26*fVar26 + fVar28*fVar28) * fVar7 + fVar9);
fVar27 = 1.0 / fVar26;
fVar28 = fVar27 * fVar28 * fVar8;   // nz = (1/len) * (h_near - h_far) * (1/128)
fVar26 = fVar27 * fVar26 * fVar8;   // nx = (1/len) * (h_left - h_right) * (1/128)
```

So the normal is: `(dx/128, 1.0, dz/128) / length((dx/128, 1.0, dz/128))`. The 1/128 factor accounts for the fact that heightfield samples are spaced 1/128 world unit apart (land tile spans 2049 samples over ~2048 world units).

**After generation:** `glBindBuffer(GL_ARRAY_BUFFER, …)` then `glBufferData(GL_ARRAY_BUFFER, vertexCount * 24, scratchBuffer, GL_DYNAMIC_DRAW)`.

### 6.2 `Terrain_RebuildChunkSlot` @ `0xd0c10`

For a specific sub-slot within a chunk at a given LOD:

**Parameters:** `(chunkIdx, subSlotIdx, lodLevel)` where `lodLevel = 0..6`

**Vertex step:** `step = 1 << lodLevel`
**Samples per side:** `0x80 / step + 2` (from `0x81` down to 3 at max LOD)
**Actual loop bounds:** −1 to `0x80/step + 1` (= 0x81)

**Sub-slot world placement:**
```
subSlotCoordX = chunk_record[chunkIdx].subSlotCoordX[subSlotIdx]  // grid offset (0-15)
subSlotCoordZ = chunk_record[chunkIdx].subSlotCoordZ[subSlotIdx]
gridStartX = subSlotCoordX * 0x80 + tileCoordX * 0x800           // tile-sample X
gridStartZ = subSlotCoordZ * 0x80 + tileCoordZ * 0x800
```

**Normal computation:** Same as the chunk-level rebuild but with `fVar36 = 1.0 / (step + 1.0)` as the gradient scale instead of `1/128`.

**Slot eviction:** Before writing, finds an unused or oldest slot in the LOD pool. If the evicted slot's chunk/subSlot are still valid, nullifies the back-reference at `0x411bf874`.

**Upload:** `glBufferSubData(GL_ARRAY_BUFFER, slot_offset_bytes, vertexCount * 24, scratchBuffer)`.

---

## 7. LOD Index Buffer Construction

### `Terrain_InitLandscapeChunkLodIndexBuffers` @ `0xd3190`

Called once during world init from `World_InitResourcePoolsAndCatalogs` @ `0x13dcd0`.

**7 LOD levels** (LOD 0 through LOD 6):

| LOD | Step (uVar4) | Verts/Side | Total Vertices | Tris | Indices |
|-----|-------------|------------|----------------|------|---------|
| 0   | 1           | 130        | 16900          | 33282 | 99846 |
| 1   | 2           | 66         | 4356           | 8450  | 25350 |
| 2   | 4           | 34         | 1156           | 2178  | 6534  |
| 3   | 8           | 18         | 324             | 578   | 1734  |
| 4   | 16          | 10         | 100             | 162   | 486   |
| 5   | 32          | 6          | 36              | 50    | 150   |
| 6   | 64          | 4          | 16              | 18    | 54    |

**Index pattern (triangle strip grid):**
```
rowStride = vertsPerSide + 1           // = (0x80 / step) + 3

For each row r (1 to vertsPerSide-1):
  For each column c (0 to vertsPerSide-1):
    bottom_left  = (r-1) * rowStride + c
    bottom_right = (r-1) * rowStride + c+1
    top_right    = r * rowStride + c+1
    top_left     = r * rowStride + c

    Triangle 1 (clockwise?): bottom_left, bottom_right, top_right
    Triangle 2:              bottom_left, top_right, top_left
```

Note: The vertex buffer has `0x81 / step + 1` vertices but the index buffer expects `0x80 / step + 2` vertices per side. At LOD 0 this is 129 vs 130 — meaning the index buffer has one extra "ghost" row/column, likely for a skirt or degenerate-triangle seam strip. This is a known technique for terrain chunk stitching.

The per-slot index buffer stride is recorded in `g_terrainChunkSlotsLod0IndexBufferStride` at the relevant LOD pool offset.

---

## 8. `UpdateMapTopHeightAndClearOccupancyBit` @ `0x3fa40`

```c
void UpdateMapTopHeightAndClearOccupancyBit(uint sampleX, uint sampleZ, float height)
{
    if (sampleX >= 0xFFF || sampleZ >= 0xFFF) return;
    if (isMapAutoGenerated) return;

    uint64 idx = (sampleZ * 0x1000 + sampleX);

    // Elementwise max-update on four mapTopHeight buffers
    MaxUpdate(&mapTopHeight0[idx], height);   // 0x3ce6f5f8
    MaxUpdate(&mapTopHeight1[idx], height);   // 0x3ce6f5fc
    MaxUpdate(&mapTopHeight2[idx], height);    // 0x3ce735f8
    MaxUpdate(&mapTopHeight3[idx], height);    // 0x3ce735fc

    // Clear occupancy bit in the land-use mask if inside the street grid bounds
    if (sampleX >= g_streetGridRowMin && sampleX <= g_streetGridRowMax &&
        sampleZ >= g_streetWorldXMin && sampleZ <= g_streetWorldXMax)
    {
        uint gx = sampleX + 0x400;   // shift from [-1024,1024] to [0,2048] domain
        uint gz = sampleZ + 0x400;
        if (gx < 0x1000 && gz < 0x1000) {
            uint cellIdx = (gz >> 2) * 0x400 + (gx >> 2);
            uint bit = 1 << ((gx & 3) + (gz & 3) * 4);
            g_landUseMaskBits[cellIdx] &= ~bit;
        }
    }
}
```

The landUseMask is a coarse 4×4 occupancy grid → each u16 cell covers a 16×16 meter region. Clearing a bit marks a 4×4 meter sub-cell as buildable (no street above). The occupancy bits are set to 0xFFFF initially; clearing specific bits indicates street presence.

---

## 9. Constants Table

| Address | Raw (IEEE-754) | Decoded | Symbol / Variable | Meaning |
|---------|---------------|---------|-------------------|---------|
| `0x002783f4` | `0x3f800000` | 1.0 | `FLOAT_002783f4` | Identity / step denominator / base normal Y |
| `0x00278688` | `0x40800000` | 4.0 | `DAT_00278688` | Height multipliers for street intersection corners |
| `0x00278774` | `0x41800000` | 16.0 | `DAT_00278774` | Edge penalty (subtracted from OOB height samples) + terrain noise amplitude |
| `0x00278280` | `0x3e800000` | 0.25 | `FLOAT_00278280` | Edge ramp gradient scale (1/4) |
| `0x00278118` | `0x3c000000` | 0.0078125 | `DAT_00278118` | World-scale gradient step = 1/128 |
| `0x0027806c` | `0x38800000` | ~0.0000610 | `DAT_0027806c` | Small epsilon for sqrt in normal calc |
| `0x00278644` | `0x40400000` | 3.0 | `DAT_00278644` | Smoothstep constant |
| `0x002781bc` | `0x3d800000` | 0.0625 | `FLOAT_002781bc` | Feather falloff rate = 1/16 |
| `0x00278224` | `0x3e000000` | 0.125 | `FLOAT_00278224` | Bilinear upsample step = 1/8 |
| `0x00278698` | `0x40880000` | 4.25 | `DAT_00278698` | Street intersection base height offset (z*4.0+4.25) |
| `0x002787b0` | `0x41c00000` | 24.0 | `DAT_002787b0` | Edge ramp coefficient |
| `0x002787e0` | `0x42100000` | 36.0 | `DAT_002787e0` | Edge ramp base scale |
| `0x002787ac` | `0x41bf0000` | 23.875 | `DAT_002787ac` | Island base height (center) |
| `0x002787a8` | `0x41be0000` | 23.75 | `DAT_002787a8` | Noise floor offset |
| `0x00278728` | `0x41000000` | 8.0 | `DAT_00278728` | Noise amplitude multiplier |
| `0x002780cc` | `0x3b800000` | 0.00390625 | `DAT_002780cc` | Island radial distance scale = 1/256 |
| `0x002783a0` | `0x3f400000` | 0.75 | `DAT_002783a0` | Radial falloff offset |
| `0x00278900` | `0x43a00000` | 320.0 | `DAT_00278900` | Noise outer radius offset |
| `0x00278148` | `0x3c800000` | 0.015625 | `DAT_00278148` | Noise scale for Z dimension = 1/64 |
| `0x002787d4` | `0x42000000` | 32.0 | `DAT_002787d4` | Noise Z amplitude |
| `0x0027874c` | `0x413c0000` | 11.75 | `DAT_0027874c` | Edge ramp final addition |
| `0x00278338` | `0x3f000000` | 0.5 | `DAT_00278338` | Radial noise mixed term |
| `0x00278178` | `0x3d000000` | 0.03125 | `DAT_00278178` | Noise scale for X dimension = 1/32 |
| `0x00278a68` | `0x44b90000` | 1480.0 | `DAT_00278a68` | Map center offset |
| `0x002785a0` | `0x3ff921fb54442eea` | π/2 ≈ 1.570796 | `DAT_002785a0` | 90° rotation for building placement |
| `0x00278814` | `0x42800000` | 64.0 | `DAT_00278814` | Chunk half-extent for LOD distance calc |
| `0x00278a1c` | `0x44800000` | 1024.0 | `DAT_00278a1c` | Chunk full extent for LOD distance calc |

**LOD Squared-Distance Thresholds (from `Terrain_SelectAndRefreshVisibleChunkSlotsAroundPoint`):**

| Address | Raw | Decoded | LOD |
|---------|-----|---------|-----|
| `0x00278aec` | `0x48800000` | 262,144 | LOD 1 (512²) |
| `0x00278af4` | `0x49100000` | 589,824 | LOD 2 (768²) |
| `0x00278afc` | `0x49800000` | 1,048,576 | LOD 3 (1024²) |
| `0x00278b00` | `0x4a100000` | 2,359,296 | LOD 4 (1536²) |
| `0x00278b04` | `0x4a800000` | 4,194,304 | LOD 5 (2048²) |
| `0x00278b08` | `0x4b100000` | 9,437,184 | LOD 6 (3072²) |
| `0x00278b0c` | `0x4b800000` | 16,777,216 | Far clip (4096²) |

---

## 10. Honest Unknowns

1. **Chunk record stride discrepancy:** The decompiler infers `0xd08` for some sub-field strides while the disassembly confirms `0x3420` as the true chunk record stride. The exact layout of bytes `+0x20` through `+0x2b` and `+0x2c..+0x341f` within each record is only partially resolved. The decompiler's `0x684` for the `g_landTileHeightChunks` pointer array is an artifact — each pointer is 8 bytes, but the decompiler thinks there are 417 bytes of other data between pointers.

2. **Ghost index column:** The LOD index buffer expects `0x80 / step + 2` vertices per side, but the vertex buffer generates `0x81 / step + 1` vertices per side. At LOD 0 this is 130 vs 129 — one extra row/column of indices likely encodes a degenerate-triangle skirt for chunk-stitching seams. The exact skirt geometry needs further analysis.

3. **LandTileHeightChunks pointer:** The per-chunk height data at offset `+0x18` of the chunk record is an 8-byte pointer to a separate heap allocation. The allocation size is `0x801 * 0x801 * 4 = 0x1004004` bytes. Whether all 64 chunks always allocate or use lazy initialization is not determined.

4. **Sub-slot coordinates initialization:** The values in `subSlotCoordX/Z` (offsets `+0x2c`/`+0x30`) that define the 16×16 sub-grid within each chunk — exactly when and how these are assigned is not traced. Likely in `Render_InitTerrainStreamingBuffers` or in the chunk array allocation logic.

5. **The 28 intersection indices:** These are hardcoded in `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches`. Their meaning (which physical intersections on the map they refer to) depends on the specific city layout and street graph topology. They are single-byte indices (2–0x3e) into the `streetIntersections` array. The exact spatial pattern they form on the default map layout is not mapped.

6. **`DAT_00278a68` (1480.0):** Used in `World_LoadMapAndInitializeTerrain` as an offset to the first street intersection's Y coordinate when computing the map center. Its exact origin is unclear — it may relate to the city district layout or the SBB coordinate system.

7. **`DAT_2b2a2ecc` / `DAT_2b2a2ed0` (`0x41c00000` = 24.0 offset, `0x44800000` = 1024 offset):** Map center computation adjusts the first intersection's position by adding these offsets. The +4096 world-space shift suggests these center the map around the street grid origin.

8. **Island heightfield edge ramp:** The branch at `0x56690` (`if (isMapAutoGenerated == 0)`) applies additional edge shaping when the map is NOT auto-generated. For auto-generated maps, the heightfield is left raw (just noise + radial falloff). The shaping uses a cubic falloff `(36 - d*24) * d² + 11.75` which transitions from 11.75 at the center to a very sharp edge near the perimeter.

---

## 11. Summary Diagram

```
World_LoadMapAndInitializeTerrain @ 0x56980
│
├─[1] Render_InitTerrainStreamingBuffers @ 0xc2db0
│       └─ Allocates 7 LOD GL pools (vertex+index buffers)
│
├─[2] Terrain_GenerateIslandHeightfield @ 0x56560
│       └─ 2049² float heightfield via noise + radial falloff + edge ramp
│          └─ TerrainNoiseHash @ 0x1305f0 (3 calls per sample)
│
├─[3] Reset g_landUseMaskBits (0x4000000 bytes → all 0xFFFF)
│
├─[4–5] Street graph / district / building instantiation
│       └─ UpdateMapTopHeightAndClearOccupancyBit calls
│
├─[6] Terrain_RebuildLandTilesRoadPatchesAndChunkCaches @ 0xd12e0
│       │
│       ├─[6a] Bilinear upsample: 2049² → 8×8 tiles, 8×8 sub-samples
│       │       └─ Terrain_SetLandSampleWithTileBorderMirroring
│       │
│       ├─[6b] 28 intersection patches (if !isMapAutoGenerated)
│       │       └─ Terrain_ApplyStreetIntersectionHeightPatch × 28
│       │           ├─ Phase 1: Bilinear base road surface
│       │           ├─ Phase 2: Sparse candidate collection (5×5 probe)
│       │           ├─ Phase 3: 16-pass distance-weight dilation (1/16 kernel)
│       │           └─ Phase 4: Smoothstep merge (3w² − 2w³ blend)
│       │
│       ├─[6c] Rebuild all 64 chunk vertex+normal buffers
│       │       └─ Terrain_RebuildChunkVertexBufferAndNormals × 64
│       │
│       └─[6d] Invalidate all 7 LOD slot pools (chunkId = -1)
│
├─[7] Cell render geometry loop
│
└─[8] Street label textures
```
