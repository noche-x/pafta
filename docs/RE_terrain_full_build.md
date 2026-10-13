# How Sub Rosa builds its terrain data

Target: `subrosa.exe.THIS_ONE` (PE x86:LE:64, image base 0)
All addresses are in the `0x000xxxxx` range. Read-only analysis through Ghidra MCP.

[terrain_generation.md](terrain_generation.md) has the later, checked findings. Where they disagree with this page (for example on the island formula's constants), the later page is right.

## 1. Call order

`World_LoadMapAndInitializeTerrain` @ `0x56980` runs the whole terrain setup:

| Step | Function | Address | What it does |
|------|----------|---------|-------------|
| 1 | `Render_InitTerrainStreamingBuffers` | `0xc2db0` | Allocates GL vertex and index buffers for 7 detail-level pools and resets the slot data |
| 2 | `Terrain_GenerateIslandHeightfield` | `0x56560` | Fills `g_terrainHeightfield` (a 2049x2049 float array) with noise and a round falloff |
| 3 | `Memory_FillBytes(g_landUseMaskBits, 0, 0x4000000)` | none | Resets the 4096x4096 u16 land-use bitmask to all 0xFFFF |
| 4 | `_createarea` | `0x4db20` | Makes a 64x8x64 SBB working volume for map-area objects |
| 5 | The loop over the street graph, districts, and buildings | none | Calls `UpdateMapTopHeightAndClearOccupancyBit` for every placed cell |
| 6 | `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches` | `0xd12e0` | The main step; see §3 |
| 7 | The loop that rebuilds cell render geometry | none | `BuildCellRenderGeometryAndAdjacency` for each active world cell |
| 8 | The street label textures | none | `RenderStreetLabelTexture` for 17 labels |

## 2. Data structures

### 2.1 `g_terrainHeightfield`
- Address: `0x2d48282c` (start of the data; the label `g_terrainHeightfield` is at `0x2d482830`)
- Layout: `float[0x801 * 0x801]` = `float[2049][2049]`
- Size: `2049 * 2049 * 4 = 0x1004004` bytes
- Contents: the generated heightfield (Y is the stored height, with no scaling)
- Grid to world: `worldX = gridX - 0x280` (4096); `worldZ = gridZ - 0x280` (4096)
- Guard values: `DAT_2d482820` and `DAT_2d482828` are both set to `0xc5800000` (= –4096.0f) at the very start of the function. They are the two float slots just before the array. They are NOT part of the heightfield; they guard against reading past the edge during bilinear resampling.

### 2.2 `g_landTileHeightChunks`
- Base array: at offset `+0x18` inside the chunk record (see §2.5), label `0x411bf858`.
- Each entry: a `float[0x801 * 0x801]` heap allocation (`2049 * 2049 * 4`, about 16 MB per tile)
- Count: 64 entries (8 rows × 8 columns of tiles)
- Addressing: `chunk_tile_x = uVar3 = sample_x >> 11`, `chunk_tile_y = uVar2 = sample_y >> 11`, index = `uVar3 + uVar2 * 8`. Inside an entry: `sample_offset = (sample_y & 0x7ff) * 0x801 + (sample_x & 0x7ff)`.
- Edge copying (see §4): when `(sample_x & 0x7ff) == 0`, the value is also written to the right edge of the tile to the left; when `(sample_y & 0x7ff) == 0`, it is also written to the bottom edge of the tile above.

### 2.3 The four `mapTopHeight` buffers

Defined in `UpdateMapTopHeightAndClearOccupancyBit` @ `0x3fa40`:

| Buffer | Address | How it is found |
|--------|---------|-----------|
| `mapTopHeight0` | `0x3ce6f5f8` | `*4` as `float[0x1000 * 0x1000]` |
| `mapTopHeight1` | `0x3ce6f5fc` | +4 bytes, the next float in the cell |
| `mapTopHeight2` | `0x3ce735f8` | +0x4000, the next row (0x3ce6f5fc + 0x4000) |
| `mapTopHeight3` | `0x3ce735fc` | +4 bytes from buffer 2 |

Each is `float[0x1000 * 0x1000]` = 4096x4096 = 67 MB, so 268 MB in total. Each value only ever goes up: `if (current <= height && height != current) current = height`. Together they form a four-sample "top height" surface used to remove overlapping buildings (an occupancy check).

### 2.4 `g_landUseMaskBits`
- Address: the `g_landUseMaskBits` label
- Layout: `uint16[0x400 * 0x400]` = `uint16[1024][1024]` = 2 MB
- Coordinates: `(floorX + 0x400) >> 2`, `(floorZ + 0x400) >> 2`
- Bit: `bit = (floorX & 3) + (floorZ & 3) * 4`, a 4x4 grid inside each u16

### 2.5 Chunk world-origin record (`0x411bf840`)
- Records are `0x3420` = 13344 bytes apart (checked in the disassembly: `imul rax, rax, 0x3420`)
- Count: 64 (8x8 chunks)
- Each record:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| `+0x00` | 4 | `worldOriginX` (float) | World X of the chunk's corner |
| `+0x04` | 4 | `worldOriginY` (float) | World Y (base height) |
| `+0x08` | 4 | `worldOriginZ` (float) | World Z of the chunk's corner |
| `+0x0c` | 4 | `tileCoordX` (int) | Land-tile column (0-7) |
| `+0x10` | 4 | `tileCoordZ` (int) | Land-tile row (0-7) |
| `+0x14` | 4 | (padding or unused) | none |
| `+0x18` | 8 | `pHeightData` (ptr) | Pointer to this chunk's `float[0x801][0x801]` |
| `+0x20` | 12 | (unknown or unused) | none |
| `+0x2c` | 4 | `subSlotCoordX[i]` (int) | Grid X offset of sub-slot i |
| `+0x30` | 4 | `subSlotCoordZ[i]` (int) | Grid Z offset of sub-slot i |
| … | … | 256 sub-slot records | each `0x34` bytes |

The 256 sub-slots in each chunk make a 16x16 grid of small tiles. Each sub-slot covers `0x80 × 0x80` samples (128×128 in the land tile). A whole chunk is `16 * 128 = 2048` samples plus one shared column, so 2049.

How chunk origins are set: the world origin of chunk `(tx, tz)` is just its tile coordinate scaled into world space:
```
worldOriginX = tileCoordX * 2048.0f   (i.e., tileCoordX << 11)
worldOriginZ = tileCoordZ * 2048.0f
worldOriginY = 0 (overwritten per-subslot)
```
(Checked in `Terrain_RebuildChunkVertexBufferAndNormals` @ `0xd00f0`: `shl edx, 0xb` with EDX = tileCoordX gives `tileCoordX * 2048 → worldOriginX`.)

### 2.6 Vertex and index buffers

Scratch vertex buffer:
- Address: `0x40e7f720`
- Layout: `float[6]` per vertex, in order: X, Y, Z, Nx, Ny, Nz
- Each vertex is 24 bytes
- Big enough for a full detail-0 chunk plus sub-slot rebuilds

GL vertex buffers:
- The array at `0x43e9b0c0` holds GL buffer names
- The arrays at `0x43e98c04` / `0x43e98c8c` hold slot numbers
- Binding: `glBindBuffer(GL_ARRAY_BUFFER, names[idx])`, then `glBufferData(…, size, data, GL_DYNAMIC_DRAW)`

GL index buffers:
- The array at `0x43e9b0c4` holds GL index buffer names from `Terrain_InitLandscapeChunkLodIndexBuffers`

### 2.7 Detail-level slot pools

Seven pools of slots (detail levels 0–6), each slot with its own data:

| Pool | ChunkId Base | Age Base | SlotCount Addr | SlotsBase |
|------|-------------|---------|---------------|-----------|
| LOD0 | `0x410ff798` | `0x410ff7a0` | `0x410ff788` | `0x410ff780` |
| LOD1 | `0x411177b0` | `0x411177b8` | `0x411177a0` | `0x41117798` |
| … | … | … | … | … |

Each slot is 3 int32s: `{chunkId, subSlot, age/active}`. Active is 4, and it counts down from 4 to 0.

### 2.8 Scratch buffer for intersection patches
- Address: `0x04afa9a0` (a fixed 87 MB buffer)
- Use: holds the candidate sample positions and their distance weights while `Terrain_ApplyStreetIntersectionHeightPatch` softens and blends over several passes.
- Layout:
  - `DAT_04afa9a0[i]` (int): how many candidate weights slot i has
  - `DAT_04afa9a4[i + j*12]` (int): grid X of candidate j
  - `DAT_04afa9a8[i + j*12]` (int): grid Z of candidate j
  - `DAT_04afa9ac[i + j*12]` (float): weight of candidate j
- Each grid cell is 7 dwords (28 bytes). The grid is `(iVar34/4+1) × (iVar32/4+1)` cells, each covering 4×4 heightfield samples.
- Output for the merge: `DAT_04afc5c4` (offset `+0x1c24` from the start), the weights collected for each sample, used by the smoothstep merge

## 3. `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches` @ `0xd12e0`

### 3.1 Scaling up: 2049² into 8×8 chunks of 16384²

The function goes over 64 tile positions (8×8). For each tile `(tx, tz)`:
1. It reads 4 corner heights from `g_terrainHeightfield`:
   ```
   h00 = heightfield[tz*2049 + tx]       // tile corner NW
   h01 = heightfield[tz*2049 + tx + 1]    // NE
   h10 = heightfield[(tz+1)*2049 + tx]    // SW
   h11 = heightfield[(tz+1)*2049 + tx+1]  // SE
   ```
2. For each sub-sample `(sx, sz)` with `sx = 0..7, sz = 0..7`:
   ```
   fx = sx * 0.125   // = sx * FLOAT_00278224 (0x3e000000 = 0.125)
   fz = sz * 0.125
   height = (1-fx)*( (1-fz)*h00 + fz*h10 ) + fx*( (1-fz)*h01 + fz*h11 )
   ```
   This is plain bilinear blending in steps of 1/8, turning each source cell into 8×8 smaller cells.
3. It calls `Terrain_SetLandSampleWithTileBorderMirroring(tileX + sx, tileZ + sz, height)`; see §4.

### 3.2 Applying intersection patches

When `isMapAutoGenerated == 0`, 28 fixed intersection numbers are patched:
```
{2, 40, 32, 46, 45, 47, 28, 10, 33, 42, 35, 13, 59, 31,
 52, 48, 50, 19, 22, 58, 62, 61, 41, 36, 43, 44, 38, 39}
```
Each one calls `Terrain_ApplyStreetIntersectionHeightPatch(idx)`; see §5.

### 3.3 Rebuilding chunk vertex buffers

Calls `Terrain_RebuildChunkVertexBufferAndNormals(i)` for `i = 0..63`.

### 3.4 Clearing the slot caches

Clears all 7 detail-level pools (LOD0–LOD6) by setting every slot's `chunkId = -1` and resetting the age and stats.

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

The `0x684` step the decompiler uses is the step through the array of pointers to the data. Each pointer is 8 bytes, and the decompiler guessed a sub-structure size. The real height buffer for each tile is allocated separately and pointed to by each entry in the `0x411bf858` array.

Samples on a tile edge (localX=0 or localZ=0) are copied into the neighbouring tile's opposite edge, so bilinear sampling can cross from one tile into the next.

## 5. `Terrain_ApplyStreetIntersectionHeightPatch` @ `0xd3340`: how it works

### 5.1 The first check

The function checks that a closed block of four intersections exists. It walks the street graph: if the intersection `param_1` has south and west neighbours, and the ones south of those also have west neighbours, the block can be patched.

### 5.2 Phase 1: the bilinear base surface

The settings (in heightfield sample coordinates, around the intersection block):
```
x0 = (intersection.pos.x + field_0x30) * 4 + 0x1000
x1 = (southIntersection.pos.x - field_0x2c) * 4 + 0x1000
z0 = (intersection.unk0[1] + field_0x2c) * 4 + 0x1000      // Note: unk0[1] is grid row
z1 = (southIntersection.unk0[1] - field_0x2c) * 4 + 0x1000
```

The four corner heights at the block's four intersections:
```
hNW = intersection.unk0[2] * 4.0 + 4.25        // 4.0 = DAT_00278688, 4.25 = DAT_00278698
hNE = southIntersection.unk0[2] * 4.0 + 4.25
hSW = westIntersection.unk0[2] * 4.0 + 4.25
hSE = farSouthWestIntersection.unk0[2] * 4.0 + 4.25
```

Then a bilinear blend over the rectangle `[x0, x1] × [z0, z1]`, in steps of `1/(x1-x0)` and `1/(z1-z0)`:
```
for (x = x0; x <= x1; x++)
  for (z = z0; z <= z1; z++)
    SetLandSample(x, z, bilinear(hNW, hNE, hSW, hSE, fx, fz));
```

This writes a flat road surface you can drive on. Note the +0x1000 offset: heightfield coordinates are shifted by 4096 to cover the map around the city grid.

### 5.3 Phase 2: collecting candidates

The rectangle is split into a coarse grid of cells, each 4×4 heightfield samples:
```
cellsX = (x1 - x0) >> 2   // columns in the coarse grid
cellsZ = (z1 - z0) >> 2   // rows
```

Each cell is checked at 5×5 points in the 4 `mapTopHeight` buffers. If all four buffers are `> 0.0` there, the cell counts as "inside a road".

For each such cell, a 5×5 pattern of candidate sample positions goes into the scratch buffer at `0x04afa9a0`. Each candidate stores its grid coordinates and a starting weight of `1.0`.

### 5.4 Phase 3: spreading weights by distance (16 passes)
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

- Pattern: grows into the 8 neighbours, with weight dropping by `1/16` per unit of distance
- At most 2 candidates per cell
- Weight: `w = 1.0 - dist * (1/16)`, clamped to [0, 1]
- Direction check: a candidate only replaces an existing one if it has a higher weight AND points the same way (dot > 0)

### 5.5 Phase 4: the smoothstep merge
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

This blends the original terrain height with the patch height along a cubic smoothstep. The largest weight in each cell decides how far the patch reaches into the terrain around it.

## 6. Making vertices and normals

### 6.1 `Terrain_RebuildChunkVertexBufferAndNormals` @ `0xd00f0`

For one chunk (param_1 = 0..63), it makes a full detail-0 vertex buffer:

```
chunkIdx = param_1
tileX = g_terrainChunkTileCoordX[chunkIdx]       // tile column 0-7
tileZ = g_terrainChunkTileCoordZ[chunkIdx]       // tile row 0-7
pHeight = g_landTileHeightChunks[chunkIdx]       // pointer to float[2049][2049]
wx = g_terrainChunkWorldOriginX[chunkIdx]        // = tileX * 2048.0
wy = g_terrainChunkWorldOriginY[chunkIdx]        // base Y
wz = g_terrainChunkWorldOriginZ[chunkIdx]        // = tileZ * 2048.0
```

Grid size: `0x802 × 0x802` = 2050×2050 samples (with a half-step inset on each edge). What actually gets made at detail 0 is `129 × 129` vertices, stepping 0x80 (128) samples at a time across the 2048×2048 area (plus one extra column and row to join up the edges).

The disassembly gives these loop limits:
- Outer loop: `R14D` from −1 to 0x801 (2050 samples)
- Inner loop: `R10D` from −1 to 0x801
- The starting values below zero clamp to the edge (when `R14D < 0`, use sample 0; when `R10D < 0`, use 0)
- Steps: 0x7f (127) per loop, plus 1, so 0x80 samples per step

Each vertex is 6 floats (24 bytes):
```
vertex[idx*6 + 0] = worldX + sampleX            // world position X
vertex[idx*6 + 1] = height + worldY             // world position Y  (with -16.0 edge penalty for out-of-bounds)
vertex[idx*6 + 2] = worldZ + sampleZ            // world position Z
vertex[idx*6 + 3] = nx                           // normal X
vertex[idx*6 + 4] = ny                           // normal Y
vertex[idx*6 + 5] = nz                           // normal Z
```

Working out the normal:
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

The scaling in the pseudocode above needs checking. The decompiled lines below use `fVar8 = DAT_00278118 = 1/128` when making the X and Z parts of the normal:
```
fVar28 = fVar26 - fVar27;    // h_near - h_far
fVar26 = SQRT((fVar26*fVar26 + fVar28*fVar28) * fVar7 + fVar9);
fVar27 = 1.0 / fVar26;
fVar28 = fVar27 * fVar28 * fVar8;   // nz = (1/len) * (h_near - h_far) * (1/128)
fVar26 = fVar27 * fVar26 * fVar8;   // nx = (1/len) * (h_left - h_right) * (1/128)
```

So the normal is `(dx/128, 1.0, dz/128) / length((dx/128, 1.0, dz/128))`. The 1/128 factor allows for heightfield samples being 1/128 of a world unit apart (a land tile covers 2049 samples over about 2048 world units).

Afterwards: `glBindBuffer(GL_ARRAY_BUFFER, …)`, then `glBufferData(GL_ARRAY_BUFFER, vertexCount * 24, scratchBuffer, GL_DYNAMIC_DRAW)`.

### 6.2 `Terrain_RebuildChunkSlot` @ `0xd0c10`

For one sub-slot inside a chunk, at a given detail level.

Settings: `(chunkIdx, subSlotIdx, lodLevel)`, with `lodLevel = 0..6`

Vertex step: `step = 1 << lodLevel`
Samples per side: `0x80 / step + 2` (from `0x81` down to 3 at the lowest detail)
Loop limits: −1 to `0x80/step + 1` (= 0x81)

Where the sub-slot sits in the world:
```
subSlotCoordX = chunk_record[chunkIdx].subSlotCoordX[subSlotIdx]  // grid offset (0-15)
subSlotCoordZ = chunk_record[chunkIdx].subSlotCoordZ[subSlotIdx]
gridStartX = subSlotCoordX * 0x80 + tileCoordX * 0x800           // tile-sample X
gridStartZ = subSlotCoordZ * 0x80 + tileCoordZ * 0x800
```

Normals: the same as for whole chunks, but scaled by `fVar36 = 1.0 / (step + 1.0)` instead of `1/128`.

Reusing slots: before writing, it finds a free slot or the oldest one in the pool. If the slot it takes still points at a valid chunk and sub-slot, it clears that back-reference at `0x411bf874`.

Upload: `glBufferSubData(GL_ARRAY_BUFFER, slot_offset_bytes, vertexCount * 24, scratchBuffer)`.

## 7. Building the detail-level index buffers

### `Terrain_InitLandscapeChunkLodIndexBuffers` @ `0xd3190`

Called once while setting up the world, from `World_InitResourcePoolsAndCatalogs` @ `0x13dcd0`.

There are 7 detail levels (LOD 0 to LOD 6):

| LOD | Step (uVar4) | Verts/Side | Total Vertices | Tris | Indices |
|-----|-------------|------------|----------------|------|---------|
| 0   | 1           | 130        | 16900          | 33282 | 99846 |
| 1   | 2           | 66         | 4356           | 8450  | 25350 |
| 2   | 4           | 34         | 1156           | 2178  | 6534  |
| 3   | 8           | 18         | 324             | 578   | 1734  |
| 4   | 16          | 10         | 100             | 162   | 486   |
| 5   | 32          | 6          | 36              | 50    | 150   |
| 6   | 64          | 4          | 16              | 18    | 54    |

How the indices are laid out (a grid of triangle pairs):
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

Note: the vertex buffer has `0x81 / step + 1` vertices per side, but the index buffer expects `0x80 / step + 2`. At detail 0 that is 129 against 130, so the index buffer has one extra "ghost" row and column, probably for a skirt or a strip of empty triangles to hide seams. This is a common trick for joining terrain chunks.

The index buffer step for each slot is stored in `g_terrainChunkSlotsLod0IndexBufferStride`, at the offset of the matching pool.

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

The land-use mask is a rough 4×4 occupancy grid, so each u16 covers a 16×16 metre area. Clearing a bit marks a 4×4 metre piece as buildable (no street on top). All bits start at 0xFFFF, and clearing a bit means a street is there.

## 9. Constants

| Address | Raw (IEEE-754) | Value | Symbol / Variable | Meaning |
|---------|---------------|---------|-------------------|---------|
| `0x002783f4` | `0x3f800000` | 1.0 | `FLOAT_002783f4` | One / step divisor / base normal Y |
| `0x00278688` | `0x40800000` | 4.0 | `DAT_00278688` | Height multiplier for street intersection corners |
| `0x00278774` | `0x41800000` | 16.0 | `DAT_00278774` | Edge penalty (subtracted from out-of-range heights), and a terrain noise strength |
| `0x00278280` | `0x3e800000` | 0.25 | `FLOAT_00278280` | Edge ramp slope scale (1/4) |
| `0x00278118` | `0x3c000000` | 0.0078125 | `DAT_00278118` | Slope step in world scale = 1/128 |
| `0x0027806c` | `0x38800000` | about 0.0000610 | `DAT_0027806c` | Small epsilon for the sqrt in the normal |
| `0x00278644` | `0x40400000` | 3.0 | `DAT_00278644` | Smoothstep constant |
| `0x002781bc` | `0x3d800000` | 0.0625 | `FLOAT_002781bc` | Softening falloff = 1/16 |
| `0x00278224` | `0x3e000000` | 0.125 | `FLOAT_00278224` | Bilinear scale-up step = 1/8 |
| `0x00278698` | `0x40880000` | 4.25 | `DAT_00278698` | Street intersection base height offset (z*4.0+4.25) |
| `0x002787b0` | `0x41c00000` | 24.0 | `DAT_002787b0` | Edge ramp factor |
| `0x002787e0` | `0x42100000` | 36.0 | `DAT_002787e0` | Edge ramp base scale |
| `0x002787ac` | `0x41bf0000` | 23.875 | `DAT_002787ac` | Island base height (middle) |
| `0x002787a8` | `0x41be0000` | 23.75 | `DAT_002787a8` | Noise floor offset |
| `0x00278728` | `0x41000000` | 8.0 | `DAT_00278728` | Noise strength |
| `0x002780cc` | `0x3b800000` | 0.00390625 | `DAT_002780cc` | Island distance scale = 1/256 |
| `0x002783a0` | `0x3f400000` | 0.75 | `DAT_002783a0` | Round falloff offset |
| `0x00278900` | `0x43a00000` | 320.0 | `DAT_00278900` | Noise outer radius |
| `0x00278148` | `0x3c800000` | 0.015625 | `DAT_00278148` | Noise scale for Z = 1/64 |
| `0x002787d4` | `0x42000000` | 32.0 | `DAT_002787d4` | Noise Z strength |
| `0x0027874c` | `0x413c0000` | 11.75 | `DAT_0027874c` | Added at the end of the edge ramp |
| `0x00278338` | `0x3f000000` | 0.5 | `DAT_00278338` | Mixed round-noise term |
| `0x00278178` | `0x3d000000` | 0.03125 | `DAT_00278178` | Noise scale for X = 1/32 |
| `0x00278a68` | `0x44b90000` | 1480.0 | `DAT_00278a68` | Map centre offset |
| `0x002785a0` | `0x3ff921fb54442eea` | π/2 ≈ 1.570796 | `DAT_002785a0` | 90° turn for placing buildings |
| `0x00278814` | `0x42800000` | 64.0 | `DAT_00278814` | Chunk half-size for detail distance |
| `0x00278a1c` | `0x44800000` | 1024.0 | `DAT_00278a1c` | Chunk full size for detail distance |

Squared distances for switching detail level (from `Terrain_SelectAndRefreshVisibleChunkSlotsAroundPoint`):

| Address | Raw | Value | LOD |
|---------|-----|---------|-----|
| `0x00278aec` | `0x48800000` | 262,144 | LOD 1 (512²) |
| `0x00278af4` | `0x49100000` | 589,824 | LOD 2 (768²) |
| `0x00278afc` | `0x49800000` | 1,048,576 | LOD 3 (1024²) |
| `0x00278b00` | `0x4a100000` | 2,359,296 | LOD 4 (1536²) |
| `0x00278b04` | `0x4a800000` | 4,194,304 | LOD 5 (2048²) |
| `0x00278b08` | `0x4b100000` | 9,437,184 | LOD 6 (3072²) |
| `0x00278b0c` | `0x4b800000` | 16,777,216 | Far clip (4096²) |

## 10. Unknowns

1. The chunk record size does not add up. The decompiler guesses `0xd08` for some field steps, but the disassembly confirms `0x3420` as the real record size. The bytes `+0x20` to `+0x2b` and `+0x2c..+0x341f` in each record are only partly worked out. The decompiler's `0x684` for the `g_landTileHeightChunks` pointer array is a mistake: each pointer is 8 bytes, but the decompiler thinks there are 417 bytes of other data between them.

2. The ghost index column. The index buffer expects `0x80 / step + 2` vertices per side, but the vertex buffer makes `0x81 / step + 1`. At detail 0 that is 130 against 129. The extra row and column of indices probably make a skirt of empty triangles to hide seams between chunks. The exact skirt shape needs more work.

3. The land-tile height pointer. The height data at `+0x18` of each chunk record is an 8-byte pointer to a separate allocation of `0x801 * 0x801 * 4 = 0x1004004` bytes. Whether all 64 chunks are always allocated, or only when needed, is not known.

4. Setting the sub-slot coordinates. When and how the `subSlotCoordX/Z` values (at `+0x2c`/`+0x30`) that make the 16×16 grid inside each chunk are set was not traced. Probably in `Render_InitTerrainStreamingBuffers` or where the chunk array is allocated.

5. The 28 intersection numbers. They are fixed in `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches`. Which real intersections they are depends on the city's layout and street graph. They are single-byte numbers (2–0x3e) into the `streetIntersections` array. The pattern they make on the default map was not mapped out.

6. `DAT_00278a68` (1480.0). `World_LoadMapAndInitializeTerrain` adds it to the first street intersection's Y when working out the map centre. Where it comes from is unclear; it may relate to the city district layout or the SBB coordinates.

7. `DAT_2b2a2ecc` / `DAT_2b2a2ed0` (`0x41c00000` = 24.0, `0x44800000` = 1024). Working out the map centre adds these to the first intersection's position. The +4096 world shift suggests they centre the map on the street grid's origin.

8. The edge ramp on the island. The branch at `0x56690` (`if (isMapAutoGenerated == 0)`) shapes the edges only when the map is NOT auto-generated. Auto-generated maps keep the raw heightfield (just noise and the round falloff). The shaping uses a cubic `(36 - d*24) * d² + 11.75`, going from 11.75 in the middle to a very sharp edge near the border.

## 11. Overview diagram

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
