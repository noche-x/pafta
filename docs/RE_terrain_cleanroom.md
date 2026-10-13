# How subrosa.exe makes its terrain: clean-room report

Program: `subrosa.exe` (x86-64, image base 0)
Tools: Ghidra MCP bridge only (`ghidra_decompile_function`, `ghidra_disassemble_bytes`, `ghidra_read_memory`, `ghidra_search_functions`, `ghidra_search_strings`, `ghidra_get_xrefs_to`, and so on)
Scope: how the GAME builds and shapes its terrain, especially in the CITY (streets, intersections, buildings).
Date: 2026-06-07

[terrain_generation.md](terrain_generation.md) has later findings. Where they disagree with this report (for example the number of patched intersections), the later page is right.

## 1. Overview

Sub Rosa's terrain is made in three layers:

1. The base island (`Terrain_GenerateIslandHeightfield`): a 2049 × 2049 grid of floats made from a fixed value-noise function, a round island falloff, and ramps at the edges. This runs on every map.
2. Splitting into runtime chunks (`Terrain_RebuildLandTilesRoadPatchesAndChunkCaches`): the 2049 × 2049 grid is scaled up 8× with bilinear blending into 8 × 8 chunks, covering 16384 × 16384 world samples.
3. The city on top: on maps that are not auto-generated (the CITY), street segments are first stamped into four "map top-height" float arrays (`0x1000 × 0x1000`). Then `Terrain_ApplyStreetIntersectionHeightPatch` runs for a fixed list of intersection numbers. Each call blends the corner heights across the 2×2 intersection block and runs a multi-pass smoothstep softening (radius about 16 grid units) to blend the road surface back into the wild land around it.

In the patched areas, the city and the wild land do not end up sharing the same heights: the city patches overwrite the scaled-up island with heights that come from the street graph.

## 2. Functions found

| Address | Name | Role |
|---------|------|------|
| `0x00056560` | `Terrain_GenerateIslandHeightfield` | Fills the global 2049×2049 base heightfield with island noise and edge shaping. |
| `0x001305f0` | `TerrainNoiseHash` | Fixed 2-D value noise (an integer hash of grid corners, blended bilinearly). |
| `0x000d12e0` | `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches` | Scales the base heightfield up into 8×8 runtime chunks, then applies intersection patches on maps that are not auto-generated. |
| `0x000d2db0` | `Terrain_SetLandSampleWithTileBorderMirroring` | Writes one float into the right chunk and copies it into the neighbouring chunk's shared edge. |
| `0x000d3340` | `Terrain_ApplyStreetIntersectionHeightPatch` | Overwrites chunk heights inside a 2×2 intersection rectangle with blended road heights, then softens the edges with smoothstep. |
| `0x00020580` | `MapConnector_PlaceStreetSegmentTilesAndSlopes` | Places street-segment tiles and writes the road height into four global `0x1000×0x1000` "map top-height" arrays. |
| `0x00020130` | `MapConnector_PlaceIntersectionPerimeterTiles` | Places the tiles around the edge of each intersection. |
| `0x0003fa40` | `UpdateMapTopHeightAndClearOccupancyBit` | Updates the four top-height arrays and clears the 4×4 land-use mask where city buildings go. |
| `0x00056980` | `World_LoadMapAndInitializeTerrain` | Sets up the world: loads the archive, builds the street graph, stamps buildings, then rebuilds the terrain. |

## 3. How the base heightfield is made

### 3.1 The array

`Terrain_GenerateIslandHeightfield` writes in order into a global float array starting at `g_terrainHeightfield` (`0x2d48282c`). The loops go:

- Outer loop (rows, which the code treats as Z): `0` … `0x800`, so 2049 steps
- Inner loop (columns, X): `0` … `0x800`, so 2049 steps

That is 2049 × 2049 = 4,196,001 floats (about 16.8 MiB).

### 3.2 The main formula (from the decompile)

For every sample `(uVar9, iVar11)` (both 0…2048), the function works out:

```c
float dist1 = sqrt((uVar9 - 0x280)^2 + (iVar11 - 0x280)^2);
float f17  = dist1 * 0.00390625f - 0.5f;   // = dist1/256 - 0.5
f17 = clamp(f17, 0.0f, 3.0f);
f17 = f17 * f17;                            // squared falloff, now in [0, 9.0]
```

Then it calls `TerrainNoiseHash` three times at different scales:

| Call | `(x, y)` passed to `TerrainNoiseHash` | What the [0,2) result is multiplied by |
|------|---------------------------------------|----------------------------------------|
| 1 | `(uVar9 / 64.0,  iVar11 / 128.0)` | `8.0`  (`DAT_00278728`) |
| 2 | `(uVar9 / 128.0, iVar11 / 128.0)` | `32.0` (`DAT_002787d4`) |
| 3 | `(uVar9 / 64.0,  iVar11 / 64.0)`  | `8.0`  (`DAT_00278728`) |

The height before edge shaping is:

```c
float inner = noise3 * 8.0f
            + 23.8125f
            + (noise1 + f17) * 16.0f
            + (noise2 * 32.0f + f17 * 8.0f) * f17;

float f15 = (sqrt((uVar9 - 0x2c0)^2 + (iVar11 - 0x2c0)^2) - 320.0f) * 0.015625f;
f15 = clamp(f15, 0.0f, 1.0f);

float height = inner * f15 + (1.0f - f15) * 23.875f;
```

What this means:
- `f17` is a round island mask centred on sample `(640, 640)` with a radius of about 896 samples (`dist/256 - 0.5 = 3.0` gives `dist = 896`). It is 0 in the middle, grows with the square of the distance to 9.0 at the 896-sample edge, and is clamped there.
- `f15` is a second, softer round falloff centred on `(704, 704)` with a radius of 320 samples, also scaled by `1/64`. It blends between the noisy island height (`inner`) and a flat 23.875.
- The noise layers give the island its bumps: a rough layer at `1/64` and `1/128` grid spacing, and a finer one at `1/64`.

### 3.3 Edge shaping (maps that are not auto-generated)

When `isMapAutoGenerated == 0` (the CITY maps), two extra edge ramps are stamped over the island:

```c
if (isMapAutoGenerated == 0) {
    // Region A: rows where outer-loop counter iVar11 is 638..767
    //           AND columns uVar9 < 768
    if (/* ... */) {
        float edge = min(clamp((iVar8 - 130) * 0.25f, 0, 1),
                         clamp((iVar10 + 130) * 0.25f, 0, 1));
        *pfVar7 = (36.0f - edge * 24.0f) * edge * edge + 11.75f;
    }
    // Region B: columns uVar9 < 638
    if (uVar9 < 0x27e) {
        float edge = min(clamp(iVar8 * 0.25f, 0, 1),
                         clamp(iVar10 * 0.25f, 0, 1));
        *pfVar7 = (36.0f - edge * 24.0f) * edge * edge + 11.75f;
    }
}
```

These cubic ramps raise the ground from a flat 11.75 m right at the edge up to 23.75 m where `edge = 1.0`. They turn the island's shoreline into a sloped beach or cliff, so the city does not sit on raw noise at the water's edge.

## 4. Decoded constants

All floats were read with `ghidra_read_memory` and decoded as little-endian IEEE-754.

| Symbol / Address | Raw bytes | Decoded float | Meaning |
|------------------|-----------|---------------|---------|
| `DAT_002780cc` | `00 00 80 3b` | 0.00390625 (1/256) | Island radius scale |
| `DAT_002783a0` | `00 00 40 3f` | 0.5 | Island radius offset |
| `DAT_00278644` | `00 00 40 40` | 3.0 | Island radius clamp |
| `DAT_00278728` | `00 00 00 41` | 8.0 | Noise layer strength |
| `DAT_002787d4` | `00 00 00 42` | 32.0 | Noise layer strength |
| `DAT_002787a8` | `00 00 be 41` | 23.8125 | Base height in the island formula |
| `DAT_00278774` | `00 00 80 41` | 16.0 | Multiplier for `(noise1 + f17)` |
| `DAT_002787ac` | `00 00 bf 41` | 23.875 | Flat height for the `f15` blend |
| `DAT_00278900` | `00 00 a0 43` | 320.0 | Second falloff radius (centred at 704,704) |
| `DAT_00278148` | `00 00 80 3c` | 0.015625 (1/64) | Second falloff scale |
| `FLOAT_00278280` | `00 00 80 3e` | 0.25 | Edge ramp slope scale |
| `DAT_002787e0` | `00 00 10 42` | 36.0 | Edge ramp height factor |
| `DAT_002787b0` | `00 00 c0 41` | 24.0 | Edge ramp reduction factor |
| `DAT_0027874c` | `00 00 3c 41` | 11.75 | Edge ramp minimum (flat base) |
| `DAT_002783f4` | `00 00 80 3f` | 1.0 | General 1.0 (also the maximum blend) |
| `DAT_00278688` | `00 00 80 40` | 4.0 | Scale from street grid to world Y |
| `DAT_00278698` | `00 00 88 40` | 4.25 | Street grid world-Y offset |
| `FLOAT_002781bc` | `00 00 80 3d` | 0.0625 (1/16) | Smoothstep softening: `1.0 / radius` |
| `FLOAT_00278224` | `00 00 00 3e` | 0.125 (1/8) | Step for the 8× scale-up |
| `DAT_00278050` | `00 00 80 30` | 9.313e-10 (2⁻³⁰) | Turns the noise integer into a float |
| `_DAT_41290040` | `00 00 80 c5` | -4096.0 | World-to-grid origin offset, X |
| `_DAT_41290048` | `00 00 80 c5` | -4096.0 | World-to-grid origin offset, Z |

## 5. Coordinates and grid mapping

### 5.1 World to terrain grid

From `Terrain_CollectHeightfieldTrianglesAlongSegment` (`0x000d05b0`):

```c
float gridXf = worldX - (-4096.0f);   // = worldX + 4096.0
float gridZf = worldZ - (-4096.0f);   // = worldZ + 4096.0f
int gridX = (int)gridXf;
int gridZ = (int)gridZf;
```

So:
- World `(-4096, 0, -4096)` is grid sample `(0, 0)`.
- World `(0, 0, 0)` is grid sample `(4096, 4096)`.
- The largest valid grid sample is `16383` on each axis.

### 5.2 Runtime chunks

`Terrain_SetLandSampleWithTileBorderMirroring` (`0x000d2db0`) does:

```c
uint chunkX = (int)param_1 >> 0xb;   // param_1 / 2048
uint chunkZ = (int)param_2 >> 0xb;   // param_2 / 2048
if (chunkX > 7 || chunkZ > 7) return;
```

- There are 8 × 8 chunks in X and Z.
- Each chunk holds 2048 × 2048 cells (2049 × 2049 float samples, counting the edge shared with the next chunk for filtering).
- In total there are 16384 × 16384 different samples.
- Samples are 1 world unit apart (16384 world units across 16384 samples).

### 5.3 World Y

In `Terrain_CollectHeightfieldTrianglesAlongSegment`:

```c
float worldY = chunkWorldOriginY + heightSample;
```

`World_InitResourcePoolsAndCatalogs` sets the chunk origin Y to 0.0, so:

```
worldY = heightSample   (direct 1:1 mapping)
```

The floats in the height arrays are the world heights in metres.

## 6. Terrain under the city

### 6.1 City patches overwrite the island

On maps that are not auto-generated (`isMapAutoGenerated == 0`), `World_LoadMapAndInitializeTerrain` builds things in this order:

1. `Terrain_GenerateIslandHeightfield()` makes the base island.
2. The street graph is loaded from `city2.sbc`.
3. `MapConnector_PlaceStreetSegmentTilesAndSlopes()` and `MapConnector_PlaceIntersectionPerimeterTiles()` run for every segment and intersection. They call `UpdateMapTopHeightAndClearOccupancyBit(x, z, height)`, which writes the road height into four global `0x1000 × 0x1000` float buffers (`0x3ce6f5f8`, `0x3ce6f5fc`, `0x3ce735f8`, `0x3ce735fc`).
4. `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches()` scales the island up into the 8×8 runtime chunks, then applies a fixed list of 31 intersection patches:

```c
if (isMapAutoGenerated == 0) {
    Terrain_ApplyStreetIntersectionHeightPatch(2);
    Terrain_ApplyStreetIntersectionHeightPatch(0x28);
    Terrain_ApplyStreetIntersectionHeightPatch(0x20);
    Terrain_ApplyStreetIntersectionHeightPatch(0x2e);
    Terrain_ApplyStreetIntersectionHeightPatch(0x2d);
    Terrain_ApplyStreetIntersectionHeightPatch(0x2f);
    Terrain_ApplyStreetIntersectionHeightPatch(0x1c);
    Terrain_ApplyStreetIntersectionHeightPatch(10);
    // ... 23 more indices ...
}
```

### 6.2 How `Terrain_ApplyStreetIntersectionHeightPatch` works

For one intersection number, the function does three things.

First, it finds the 2×2 block. It walks south, west, and south-west to find the four intersections around the city block, then works out the rectangle in grid coordinates from the north-east corner to the south-west corner.

Second, it blends the corners. It reads the four corner heights from the four `0x1000×0x1000` top-height arrays:

```c
float hNE = mapTopHeight[cornerNE];
float hSE = mapTopHeight[cornerSE];
float hNW = mapTopHeight[cornerNW];
float hSW = mapTopHeight[cornerSW];
```

Then it goes through every grid sample in the rectangle and writes:

```c
float u = (x - xMin) / width;   // 0 … 1
float v = (y - yMin) / height;  // 0 … 1
float roadH = (1-u) * ((1-v)*hNW + v*hSW) + u * ((1-v)*hNE + v*hSE);
Terrain_SetLandSampleWithTileBorderMirroring(gridX, gridZ, roadH);
```

That gives a flat, bilinear road surface whose corners are pinned to the street heights.

Third, it softens the edges over several passes:

- A local `28 × 28` working grid (around `0x04afa9bc`) keeps flags, source coordinates, and weights for every sample in the rectangle.
- Samples that sit on a street lane are marked as "sources" with weight `1.0f`.
- For 16 passes, it scans the grid. For each cell that is not a source, it looks for sources in the 3×3 cells around it, works out the straight-line distance, and turns it into a weight:
  ```c
  weight = 1.0f - distance * 0.0625f;   // radius = 16 samples
  weight = clamp(weight, 0.0f, 1.0f);
  ```
- After that, each cell is blended between its original wild height and the road height using smoothstep:
  ```c
  float blend = (3.0f - 2.0f * maxWeight) * maxWeight * maxWeight;
  float finalH = (1.0f - blend) * originalH + blend * roadH;
  ```

The result: the city ground is a level, drivable bilinear patch whose edges blend back into the island over about 16 grid samples (about 16 world units). Inside the road area the wild height is fully replaced, and it comes back gradually outside the 16-sample radius.

## 7. Blending and smoothing at a glance

| Step | Where | Kind | Settings |
|---------|-------|------|------------|
| Island blend | Base heightfield | Straight blend between `inner` noise and a flat 23.875 | Falloff radius about 320 samples, scale `1/64` |
| Edge ramp | Base heightfield (city maps only) | Cubic ramp `(36 - 24·edge)·edge² + 11.75` | `edge` from 0 to 1, in 0.25 steps |
| Scale-up | Runtime chunks | Bilinear, 8× | Step `0.125` (1/8) |
| Intersection patch | Runtime chunks (city maps) | Bilinear road surface plus smoothstep softening | Radius 16 samples, weight `1 - dist/16`, blend `(3-2w)·w²` |

## 8. How sure I am, and open questions

### Sure
- The base terrain is value noise (`TerrainNoiseHash`) plus a round falloff. It is not loaded from a heightmap file.
- World Y is the stored float, unchanged (origin Y = 0).
- Grid mapping is `grid = floor(world + 4096)`, with 16384 × 16384 samples in 8 × 8 chunks.
- City terrain overwrites the wild land through intersection patches, which read street heights from four `0x1000×0x1000` float buffers.
- The softening uses smoothstep with a radius of about 16 samples.

### Fairly sure
- The exact scales of all three noise layers. I traced the first two for certain (`1/64` and `1/128`). The third is guessed from the pattern and the code around it, but I did not fully trace its assembly because registers were reused.
- Why there are four `mapTopHeight` arrays that look the same. `UpdateMapTopHeightAndClearOccupancyBit` writes them all the same way. My guess is they are for different detail levels, or for building tops versus road tops, but I did not find the separate code that reads them.

### Not sure or still open
- Chunk memory layout. `World_InitResourcePoolsAndCatalogs` allocates 64 chunks (`0x40`) of `0x1004004` bytes each (`0x1004004 ≈ 2049×2049×4`). Setting up each chunk's world origin uses complicated pointer steps (`0x6840`, `0x3420`) over 8 outer loops. I did not fully work out which of the 64 chunks gets which `(originX, originZ)`, although the collision code proves the world runs from -4096 to +12288.
- Why only 31 fixed intersection numbers? The list in `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches` is a fixed array. I did not find out why these intersections are patched and not all of them. Maybe only the main intersections need the full terrain patch, and smaller streets rely on the road tiles to hide the ground.
- Water level. I did not find the water-plane constant. The edge shaping bottoms out at 11.75, which is well above normal sea level, so the water is probably somewhere below 11.75 (maybe 0 or below).

## 9. Decompiled snippets (evidence)

### 9.1 Round island falloff (`Terrain_GenerateIslandHeightfield`, `0x00056560`)

```c
fVar17 = SQRT((float)(int)(uVar9 - 0x280) * (float)(int)(uVar9 - 0x280) +
              (float)(iVar11 + -0x280) * (float)(iVar11 + -0x280)) * DAT_002780cc -
         DAT_002783a0;
if (fVar17 <= 0.0) { fVar17 = 0.0; }
if (DAT_00278644 <= fVar17) { fVar17 = DAT_00278644; }
fVar17 = fVar17 * fVar17;
```

### 9.2 Base height blend (`Terrain_GenerateIslandHeightfield`)

```c
*pfVar7 = (extraout_XMM0_Da_01 * DAT_00278728 + _DAT_002787a8 +
          (extraout_XMM0_Da + fVar17) * DAT_00278774 +
          (fVar16 + fVar14) * fVar17) * fVar15 +
          (fVar3 - fVar15) * _DAT_002787ac;
```

### 9.3 Writing a chunk sample and its shared edge (`Terrain_SetLandSampleWithTileBorderMirroring`, `0x000d2db0`)

```c
uVar3 = (int)param_1 >> 0xb;
uVar2 = (int)param_2 >> 0xb;
if (7 < uVar3) return;
if (7 < uVar2) return;
// ... write into chunk memory at (uVar2 + uVar3*8) ...
*(undefined4 *)((&g_landTileHeightChunks)[lVar5 * 0x684] +
                (ulonglong)(uVar1 * 0x801 + uVar4) * 4) = param_3;
```

### 9.4 Intersection patch blend and smoothstep (`Terrain_ApplyStreetIntersectionHeightPatch`, `0x000d3340`)

```c
Terrain_SetLandSampleWithTileBorderMirroring(
    uVar18, uVar22,
    (fVar4 - fVar39) * (fVar36 * fVar40 + fVar43 * (fVar4 - fVar40)) +
    (fVar38 * fVar40 + fVar44 * (fVar4 - fVar40)) * fVar39);
```

```c
fVar38 = fVar4 - fVar41 * fVar40;   // fVar40 = 0.0625
if (fVar38 <= 0.0) goto skip;
// ... accumulate nearest-source weights ...
float blend = (DAT_00278644 - (maxWeight + maxWeight)) * maxWeight * maxWeight;
Terrain_SetLandSampleWithTileBorderMirroring(
    uVar18, uVar22, (fVar4 - blend) * originalH + blendedH * blend);
```

### 9.5 Street top-height update (`UpdateMapTopHeightAndClearOccupancyBit`, `0x0003fa40`)

```c
if (*(float *)(&DAT_3ce6f5f8 + lVar1 * 4) <= param_3 &&
    param_3 != *(float *)(&DAT_3ce6f5f8 + lVar1 * 4)) {
    *(float *)(&DAT_3ce6f5f8 + lVar1 * 4) = param_3;
}
// ... same for 3ce6f5fc, 3ce735f8, 3ce735fc ...
```
