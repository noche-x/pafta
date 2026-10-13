# Clean-room research: how subrosa.exe.THIS_ONE makes terrain and shapes the city

Target program: `subrosa.exe.THIS_ONE` (confirmed with `list_open_programs` → `switch_program` → `get_current_program_info`; path `/subrosa.exe.THIS_ONE`, Ghidra's own name `subrosa.exe`).
Method: Ghidra MCP tools only (decompile, disassemble, read_memory, search_functions, search_strings, get_xrefs, convert_number).
Date: 2026-06-07

This is one of several independent reports. Some of its numbers (for example the radius offset 0.75, the base 23.75, and the count of 26 patched intersections) differ from the other reports and from [terrain_generation.md](terrain_generation.md), which has the later, checked findings.

## 1. Functions traced

| Function | Address | Role |
|---|---|---|
| `Terrain_GenerateIslandHeightfield` | `00056560` | Fills the base 2049×2049 heightfield using a round falloff and three layers of integer-hash value noise. |
| `TerrainNoiseHash` | `001305f0` | Fixed grid noise: hashes whole-number grid coordinates, scales by 2⁻³⁰, and blends smoothly. |
| `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches` | `000d12e0` | Resamples the base heightfield into 8×8 runtime tiles, then patches intersections on maps that are not auto-generated. |
| `Terrain_ApplyStreetIntersectionHeightPatch` | `000d3340` | Overwrites terrain heights inside an intersection rectangle with a blend of its corner heights, then smooths for 16 passes weighted by distance. |
| `Terrain_SetLandSampleWithTileBorderMirroring` | `000d2db0` | Writes one height into the 8×8 tile array and copies it across tile edges. |
| `MapConnector_PlaceStreetSegmentTilesAndSlopes` | `00020580` | Places street segment tiles and calls `UpdateMapTopHeightAndClearOccupancyBit` with a flat height of `row × 4.0`. |
| `MapConnector_PlaceIntersectionPerimeterTiles` | `00020130` | Places the tiles around each intersection's edge. |
| `World_LoadMapAndInitializeTerrain` | `00056980` | Startup: loads city data, calls `Terrain_GenerateIslandHeightfield`, builds streets, then rebuilds caches. |

## 2. How the heightfield is made

### 2.1 Overview

The island is fully made in code when the map loads, by `Terrain_GenerateIslandHeightfield`. No heightmap file is loaded for the wild island; only the city layout comes from a file (`city2.sbc`). The function writes one global 2049×2049 float array (`g_terrainHeightfield`), and later `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches` resamples it into 16384×16384 runtime tiles.

### 2.2 The noise function: `TerrainNoiseHash`

Decompiled at `001305f0`:
```c
uint TerrainNoiseHash(void) {
  uint uVar2 = ((int)in_XMM1_Da + 1U & in_R8D) * 0x39 + ((int)in_XMM0_Da + 1U & in_R8D);
  uVar2 = uVar2 ^ uVar2 * 0x2000;
  int iVar1 = uVar2 * uVar2 * 0x3d73 + 0xc0ae5;
  return iVar1 * iVar1 + 0xd208dd0dU & 0x7fffffff;
}
```

The assembly shows it takes three arguments in an unusual way, through float registers:
- `XMM0` = grid X (float)
- `XMM1` = grid Y (float)
- `R8D` = coordinate mask (always `0xffffffff` in the island generator)

Inside, the function:
1. Cuts the float inputs down to integers (`CVTTSS2SI`), so it hashes the whole-number grid cell.
2. Runs a chain of multiply-and-mix steps (`*0x39`, `^`, `*0x3d73`, `+0xc0ae5`, and so on).
3. Returns a positive 31-bit integer (`& 0x7fffffff`).
4. The caller (or inlined code) turns that integer into a float and multiplies by `2⁻³⁰` (constant `0x00278050` = `9.3132257e-10`), giving a random-looking float.
5. The hashes of the four corners of a grid cell are blended with a smooth cubic-like curve (the assembly does Hermite-style weighting with chains of `SUBSS`, `MULSS`, `MULSS`, `SUBSS`).

Confidence: HIGH. The constant `0x00278050` was read directly and decoded, and the blending follows the usual value-noise smoothing.

### 2.3 Island formula

The disassembly of `Terrain_GenerateIslandHeightfield @ 00056560`, plus the constants read from memory, give this formula for each sample.

Symbols:
- `sx, sy` = coordinates in the 2049×2049 array (0 … 2048).
- `dx = sx - 640`, `dy = sy - 640`
- `dist₀ = sqrt(dx² + dy²)`
- `falloff = clamp(dist₀ / 256.0 - 0.75, 0.0, 3.0)`
- `falloff = falloff²`

Noise layers:
1. `n1 = noise(sx / 64.0, sy / 128.0)`: first layer (stretched 2:1)
2. `n2 = noise(sx / 128.0, sy / 128.0)`: second layer
3. `n3 = noise(sx / 32.0, sy / 32.0)`: third layer (finest)

Edge blend:
- `ex = sx - 704`, `ey = sy - 704`
- `dist₁ = sqrt(ex² + ey²)`
- `edge = clamp((dist₁ - 320.0) × 0.015625, 0.0, 1.0)` (goes from 0 to 1 across a band 64 source units wide)

Height before edge shaping:
```
height = [ n3×8.0 + 23.75 + (n1 + falloff)×16.0 + (n2×32.0 + falloff×8.0) × falloff ] × edge
         + (1.0 - edge) × 23.875
```

Constants used in the formula:

| Address | Raw bytes | Float | Meaning |
|---|---|---|---|
| `002780cc` | `00 00 80 3b` | `0.00390625` | `1/256`, turns distance into falloff |
| `002783a0` | `00 00 40 3f` | `0.75` | Falloff offset (moves the island centre) |
| `00278644` | `00 00 40 40` | `3.0` | Largest falloff |
| `00278728` | `00 00 00 41` | `8.0` | Strength of `n3` and the falloff term |
| `002787d4` | `00 00 00 42` | `32.0` | Strength of `n2` |
| `00278774` | `00 00 80 41` | `16.0` | Scale of the `(n1 + falloff)` term |
| `002787a8` | `00 00 be 41` | `23.75` | Base height added to the noise |
| `002787ac` | `00 00 bf 41` | `23.875` | Flat sea or edge height when `edge = 0` |
| `00278148` | `00 00 80 3c` | `0.015625` | `1/64`, edge blend scale |
| `00278900` | `00 00 a0 43` | `320.0` | Edge blend radius |
| `00278178` | `00 00 00 3d` | `0.03125` | `1/32`, used when scaling noise arguments |
| `00278338` | `00 00 00 3f` | `0.5` | Half-scale for noise arguments |

Extra edge shaping on maps that are not auto-generated: on maps made by hand (`isMapAutoGenerated == 0`), two extra edge shapes are applied near the north-west and south-east corners, using a quadratic falloff with the constants `36.0`, `24.0`, `0.25`, and `11.75` (see the assembly at `000567dc`–`000568e5`). This flattens the map's edges to make room for the city.

Confidence: HIGH for the main formula. The exact blend weights inside `TerrainNoiseHash` are worked out from assembly, but it is certainly value noise.

## 3. Terrain in and around the city

### 3.1 The city does NOT have its own heightfield

The wild island and the city start from the same base (`Terrain_GenerateIslandHeightfield`). After that, the city flattens and overwrites some areas in two ways.

### 3.2 Street intersections get height patches

`Terrain_RebuildLandTilesRoadPatchesAndChunkCaches` (decompiled at `000d12e0`) contains:
```c
if (isMapAutoGenerated == 0) {
    Terrain_ApplyStreetIntersectionHeightPatch(2);
    Terrain_ApplyStreetIntersectionHeightPatch(0x28);
    Terrain_ApplyStreetIntersectionHeightPatch(0x20);
    ... /* 26 hardcoded intersection indices total */
}
```

So only a fixed set of intersections get patched: the city ones, not the wild land.

### 3.3 What `Terrain_ApplyStreetIntersectionHeightPatch` does

Decompiled at `000d3340`:

1. Bilinear patch: it works out the rectangle covering the intersection and the extents of its south and west neighbours. It writes a bilinear blend of the four corner heights into every sample of that rectangle:
   ```
   h = (1.0 - fx) * (1.0 - fy) * h_SW
     + (1.0 - fx) * fy       * h_NW
     + fx       * (1.0 - fy) * h_SE
     + fx       * fy         * h_NE
   ```
   This goes straight into the runtime terrain tiles through `Terrain_SetLandSampleWithTileBorderMirroring`.

2. Finer pass: for each 4×4 sub-block inside the intersection, it reads the four corner heights from the terrain. If all four are positive, it splits that sub-block into a 5×5 grid and writes another bilinear blend.

3. 16 passes of distance-weighted smoothing: after stamping, it runs 16 smoothing passes over the intersection area. For each sample it looks at the 8 neighbours. Neighbours "marked" in step 2 count with a weight:
   ```
   w = 1.0 - distance_to_neighbor × 0.0625
   w = clamp(w, 0.0, 1.0)
   ```
   The result is blended back with a smoothstep-like curve based on the largest weight:
   ```
   blend = (3.0 - 2.0×w_max) × w_max²
   final_h = (1.0 - blend) × original_h + blend × smoothed_h
   ```
   This slowly blends the intersection into the terrain around it.

Smoothing constant:

| Address | Raw bytes | Float | Meaning |
|---|---|---|---|
| `002781bc` | `00 00 80 3d` | `0.0625` | `1/16`, how fast weight drops with distance in the 16 passes |

### 3.4 Street segments are flattened to their row height

`MapConnector_PlaceStreetSegmentTilesAndSlopes @ 00020580` calls:
```c
UpdateMapTopHeightAndClearOccupancyBit(x, z, (float)(int)row * 4.0);
```

The constant at `00278688` is `4.0`. The street's row number sets the world Y of the road surface directly, which flattens the ground under the road to a level plane at `Y = row × 4.0`.

Conclusion: city terrain is the island, overwritten by bilinear patches at each intersection with several smoothing passes, and flattened again by a height stamp under every street segment. It is not a separate hand-made heightmap.

Confidence: HIGH. The overwrite is proven by the direct calls to `Terrain_SetLandSampleWithTileBorderMirroring` inside `Terrain_ApplyStreetIntersectionHeightPatch`, and the flat street heights are proven by `UpdateMapTopHeightAndClearOccupancyBit`.

## 4. Coordinates and grid mapping

### 4.1 Source, destination, world

1. Source heightfield (`g_terrainHeightfield`): 2049 × 2049 floats, rows `0x801` (2049) apart.
2. Destination tiles (`g_landTileHeightChunks`): 8 × 8 tiles. Each tile stores 2049 × 2049 floats (including a 1-sample edge used for filtering across tiles), rows `0x801` apart.
3. The whole runtime grid has 16384 × 16384 usable samples (8 tiles × 2048 inner samples each).

### 4.2 Resampling

`Terrain_RebuildLandTilesRoadPatchesAndChunkCaches` reads each source cell `(sx, sy)` … `(sx+1, sy+1)` and splits it into an 8×8 block with bilinear blending:
```c
for (sy = 0; sy < 0x4000; sy += 8)   // 16384
  for (sx = 0; sx < 0x4000; sx += 8) // 16384
    for (iy = 0; iy < 8; iy++)
      for (ix = 0; ix < 8; ix++)
        // bilinear interp with step = 0.125
```

The constant `FLOAT_00278224` is `0.125` (`1/8`).

### 4.3 World and grid

`World_InitResourcePoolsAndCatalogs` (decompiled at `0013e9b5`) sets each chunk's world origin as:
- WorldOriginX for tile column `tx` (0 … 7): `-4096.0 + tx × 2048.0`
- WorldOriginZ for tile row `tz` (0 … 7): `-4096.0 + tz × 2048.0`
- WorldOriginY for every chunk: `0.0`

Constants:

| Address | Raw bytes | Float | Meaning |
|---|---|---|---|
| `00278a9c` | `00 00 00 45` | `2048.0` | Distance between tile origins |
| `00278aac` | `00 00 80 45` | `4096.0` | World-to-grid offset |

In `Terrain_RebuildChunkVertexBufferAndNormals @ 000d00f0`, a vertex at sample `(ix, iz)` inside a chunk is placed at:
```
worldX = chunkWorldOriginX + ix
worldY = chunkWorldOriginY + height_sample
worldZ = chunkWorldOriginZ + iz
```

The index is added straight to the origin with no scaling, so one terrain grid step is 1.0 world unit. That gives:
```
worldX = terrainGridX - 4096.0
worldZ = terrainGridZ - 4096.0
worldY = height_sample   (direct float value)
```

The island centre is at source grid `(640, 640)`, which is:
- destination grid `(640×8, 640×8) = (5120, 5120)`
- world `(5120−4096, 5120−4096) = (1024, 1024)`

### 4.4 Street grid and terrain grid

From `Terrain_ApplyStreetIntersectionHeightPatch`:
```c
gridX = (int)(intersection.pos.x + field_0x30) * 4 + 0x1000;   // +4096
gridZ = (intersection.unk0[1] + field_0x2c) * 4 + 0x1000;   // +4096
```

This uses the same offset: terrain grid = world coordinate + 4096, so world = grid − 4096.

Confidence: HIGH. The chunk origin setup and the street patch math both agree on the −4096 offset and 1.0 unit spacing.

## 5. Blending, smoothing, and flattening

### 5.1 Island to sea

The island's outer edge blends into a flat `23.875` across a band centred `320.0` source units from `(704, 704)` (world around `(1536, 1536)`), `64.0` source units wide (`1/0.015625 = 64`). This is the `edge` factor in the formula.

### 5.2 Smoothing at city intersections

After stamping the intersection's bilinear height, `Terrain_ApplyStreetIntersectionHeightPatch` runs 16 passes of neighbour smoothing with a drop-off of `0.0625` (`1/16`).

Weight falls in a straight line with distance:
```
weight = max(0.0, 1.0 - dist × 0.0625)
```

The blend factor is the usual smoothstep `3w² − 2w³`, using the largest neighbour weight. The result is written back into the tiles, so each pass softens the step between the flat intersection and the island slopes around it.

### 5.3 Flattening street segments

`MapConnector_PlaceStreetSegmentTilesAndSlopes` sets the top height of every street tile to `(float)row × 4.0`. That makes roads perfectly flat, whatever the noise underneath.

Confidence: HIGH for the settings. The weight-collecting code inside the 16-pass loop is dense (the decompile shows a list-processing loop with up to 2 neighbour slots), but the `0.0625` distance scale and the smoothstep blend are clear in the assembly.

## 6. Constants

| Name / Global | Address | Raw | Decoded Float | Role |
|---|---|---|---|---|
| `FLOAT_002783f4` | `002783f4` | `00 00 80 3f` | `1.0` | One / clamp maximum / blend end |
| `DAT_002780cc` | `002780cc` | `00 00 80 3b` | `0.00390625` | `1/256`, round falloff scale |
| `DAT_002783a0` | `002783a0` | `00 00 40 3f` | `0.75` | Subtracted in the round falloff |
| `DAT_00278644` | `00278644` | `00 00 40 40` | `3.0` | Largest falloff; also used as a height later |
| `DAT_00278728` | `00278728` | `00 00 00 41` | `8.0` | Noise and falloff strength |
| `DAT_002787d4` | `002787d4` | `00 00 00 42` | `32.0` | Noise strength (layer 2) |
| `DAT_00278774` | `00278774` | `00 00 80 41` | `16.0` | Noise plus falloff scale (layer 1) |
| `_DAT_002787a8` | `002787a8` | `00 00 be 41` | `23.75` | Base height inside the noise sum |
| `_DAT_002787ac` | `002787ac` | `00 00 bf 41` | `23.875` | Flat edge or base height (guessed sea level) |
| `DAT_0027874c` | `0027874c` | `00 00 3c 41` | `11.75` | Lowest point of the extra edge shape on hand-made maps |
| `DAT_002787e0` | `002787e0` | `00 00 10 42` | `36.0` | Extra edge shape strength |
| `DAT_002787b0` | `002787b0` | `00 00 c0 41` | `24.0` | Extra edge shape strength |
| `FLOAT_00278280` | `00278280` | `00 00 80 3e` | `0.25` | Edge shape step |
| `DAT_00278148` | `00278148` | `00 00 80 3c` | `0.015625` | `1/64`, outer edge blend scale |
| `DAT_00278900` | `00278900` | `00 00 a0 43` | `320.0` | Outer edge blend radius |
| `DAT_00278688` | `00278688` | `00 00 80 40` | `4.0` | Street row to world Y |
| `DAT_00278698` | `00278698` | `00 00 88 40` | `4.25` | Street height offset (used at intersection patch corners) |
| `FLOAT_002781bc` | `002781bc` | `00 00 80 3d` | `0.0625` | `1/16`, how fast smoothing weight drops with distance |
| `FLOAT_00278224` | `00278224` | `00 00 00 3e` | `0.125` | `1/8`, resampling step |
| `DAT_00278178` | `00278178` | `00 00 00 3d` | `0.03125` | Noise argument scale (1/32) |
| `DAT_00278338` | `00278338` | `00 00 00 3f` | `0.5` | Half-scale for noise arguments |
| `DAT_00278050` | `00278050` | `00 00 80 30` | `9.313e-10` | `1/2³⁰`, turns the hash into a float |
| `DAT_00278a9c` | `00278a9c` | `00 00 00 45` | `2048.0` | Distance between tile origins |
| `DAT_00278aac` | `00278aac` | `00 00 80 45` | `4096.0` | World origin offset |

## 7. How sure, and what is unknown

| Item | Confidence | Notes |
|---|---|---|
| Main formula (three noise layers, round falloff, edge blend) | HIGH | Every constant was read from memory, and the formula rebuilt from assembly was checked against the decompile. |
| City terrain = island base overwritten by patches | HIGH | `Terrain_SetLandSampleWithTileBorderMirroring` is called directly in the intersection patch and street code. |
| Streets flattened to `row × 4.0` | HIGH | It is the argument passed to `UpdateMapTopHeightAndClearOccupancyBit`. |
| Grid size (16384×16384), 8×8 tiles, 1.0 spacing | HIGH | Comes from the chunk origin math and the resampling loops. |
| The blend curve inside `TerrainNoiseHash` | MEDIUM-HIGH | It is value noise with a 31-bit hash and a 2⁻³⁰ scale. The exact cubic weights were read from the disassembly, but the decompiler hides them. In short, it is a smoothly blended hash grid. |
| Extra edge shaping on hand-made maps (assembly `000567dc`–`000568e5`) | MEDIUM | Two corner shapes are there, but the full formula was not simplified because Ghidra's decompile branches a lot on `cmp iVar8, 0x82` and similar. The constants (`36.0`, `24.0`, `0.25`, `11.75`) are proven, but the shape is best described as quadratic edge falloffs near two opposite corners. |
| Water or sea level | MEDIUM | No "water level" string was found. The flat edge value `23.875` is the most likely sea level, but the shader could draw water at another height. |
| The full list of 26 patched intersections | HIGH | They are fixed values in `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches`; the list is above. |
| Does the 16-pass smoothing run at load time or every frame? | HIGH | It is in `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches`, which `World_LoadMapAndInitializeTerrain` calls once. So only at load time. |

## 8. Decompiled evidence

### 8.1 Island formula (`00056560`)
```c
fVar17 = SQRT((float)(int)(uVar9 - 0x280) * (float)(int)(uVar9 - 0x280) +
              (float)(iVar11 + -0x280) * (float)(iVar11 + -0x280)) * DAT_002780cc -
         DAT_002783a0;
if (fVar17 <= 0.0) { fVar17 = 0.0; }
if (DAT_00278644 <= fVar17) { fVar17 = DAT_00278644; }
fVar17 = fVar17 * fVar17;
TerrainNoiseHash();    // n1
fVar14 = fVar17 * DAT_00278728;
TerrainNoiseHash();    // n2
fVar16 = extraout_XMM0_Da_00 * DAT_002787d4;
fVar15 = (SQRT((float)(int)(uVar9 - 0x2c0) * ... ) - DAT_00278900) * DAT_00278148;
if (fVar15 <= 0.0) { fVar15 = 0.0; }
if (fVar3 <= fVar15) { fVar15 = fVar3; }
TerrainNoiseHash();    // n3
*pfVar7 = (extraout_XMM0_Da_01 * DAT_00278728 + _DAT_002787a8 +
          (extraout_XMM0_Da + fVar17) * DAT_00278774 +
          (fVar16 + fVar14) * fVar17) * fVar15 +
          (fVar3 - fVar15) * _DAT_002787ac;
```

### 8.2 Intersection patch blend and smoothing (`000d3340`)
```c
for (uVar22 = uVar2; (int)uVar22 <= iVar25; uVar22 = uVar22 + 1) {
  fVar39 = 0.0;
  for (uVar18 = uVar1; (int)uVar18 <= iVar23; uVar18 = uVar18 + 1) {
    Terrain_SetLandSampleWithTileBorderMirroring(
      uVar18, uVar22,
      (fVar4 - fVar39) * (fVar36 * fVar40 + fVar43 * (fVar4 - fVar40)) +
      (fVar38 * fVar40 + fVar44 * (fVar4 - fVar40)) * fVar39);
    fVar39 = fVar39 + fVar41;
  }
  fVar40 = fVar40 + fVar42;
}
// ... 16 smoothing iterations ...
fVar38 = fVar4 - fVar41 * fVar40;   // fVar40 = 0.0625
if (fVar38 <= 0.0) goto LAB_000d3bf1;
// ... smoothstep blend ...
Terrain_SetLandSampleWithTileBorderMirroring(
  uVar18, uVar22,
  (fVar4 - fVar41) * fVar40 + fVar38 * fVar41);
```

### 8.3 Street flattening (`00020580`)
```c
UpdateMapTopHeightAndClearOccupancyBit(
  uVar3 + iVar13, uVar9 + (int)fVar1,
  (float)(int)uVar19 * fVar2);   // fVar2 = 4.0
```

### 8.4 Chunk origin setup (`0013e9b5`)
```c
piVar7[-4] = -0x3a800000;           // worldOriginX = -4096.0 (float bits 0xC5800000)
piVar7[-3] = 0;                     // worldOriginY = 0.0
fVar15 = (float)iVar9 * 2048.0 - 4096.0;
piVar7[-2] = (int)fVar15;           // worldOriginZ
```
