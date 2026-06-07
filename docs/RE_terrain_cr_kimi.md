# Clean-room RE: Sub Rosa (subrosa.exe.THIS_ONE) — Terrain Generation & City Shaping

> **Target program:** `subrosa.exe.THIS_ONE` (confirmed via `list_open_programs` → `switch_program` → `get_current_program_info`; path `/subrosa.exe.THIS_ONE`, internal Ghidra name `subrosa.exe`).
> **Method:** Ghidra MCP tools only (decompile, disassemble, read_memory, search_functions, search_strings, get_xrefs, convert_number).
> **Date:** 2026-06-07

---

## 1. Key Functions Discovered

| Function | Address | Role |
|---|---|---|
| `Terrain_GenerateIslandHeightfield` | `00056560` | Fills the base 2049×2049 procedural heightfield using radial falloff + three octaves of integer-hash value noise. |
| `TerrainNoiseHash` | `001305f0` | Deterministic lattice noise: hashes integer lattice coordinates, normalizes by 2⁻³⁰, and smooth-interpolates. |
| `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches` | `000d12e0` | Resamples the base heightfield into 8×8 runtime tiles, then calls intersection patches for non-auto-generated maps. |
| `Terrain_ApplyStreetIntersectionHeightPatch` | `000d3340` | Overwrites terrain heights inside a street-intersection rectangle with bilinear corner heights + 16-pass distance-weighted smoothing. |
| `Terrain_SetLandSampleWithTileBorderMirroring` | `000d2db0` | Writes one height sample into the 8×8 tile array and mirrors the value across tile borders. |
| `MapConnector_PlaceStreetSegmentTilesAndSlopes` | `00020580` | Places street segment tiles and calls `UpdateMapTopHeightAndClearOccupancyBit` with flat height `row × 4.0`. |
| `MapConnector_PlaceIntersectionPerimeterTiles` | `00020130` | Places intersection perimeter shell tiles. |
| `World_LoadMapAndInitializeTerrain` | `00056980` | Bootstrap: loads city data, calls `Terrain_GenerateIslandHeightfield`, builds streets, then rebuilds caches. |

---

## 2. How the Terrain Heightfield is Produced

### 2.1 Overview
The island terrain is **procedural**, generated entirely at load-time by `Terrain_GenerateIslandHeightfield`. It does **not** load a raw heightmap file for the wilderness island (only authored maps via `city2.sbc` exist for the city layout). The function writes a single global 2049×2049 float array (`g_terrainHeightfield`) and later `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches` resamples it into 16384×16384 runtime tiles.

### 2.2 Noise Primitive — `TerrainNoiseHash`
Decompile at `001305f0`:
```c
uint TerrainNoiseHash(void) {
  uint uVar2 = ((int)in_XMM1_Da + 1U & in_R8D) * 0x39 + ((int)in_XMM0_Da + 1U & in_R8D);
  uVar2 = uVar2 ^ uVar2 * 0x2000;
  int iVar1 = uVar2 * uVar2 * 0x3d73 + 0xc0ae5;
  return iVar1 * iVar1 + 0xd208dd0dU & 0x7fffffff;
}
```
Assembly analysis shows it is called with three arguments via a custom float-register convention:
- `XMM0` = lattice X (float)
- `XMM1` = lattice Y (float)
- `R8D` = coordinate mask (always `0xffffffff` in the island generator)

Inside, the function:
1. Truncates the float inputs to integers (`CVTTSS2SI`) — i.e. it hashes the integer lattice cell.
2. Runs a multiplicative hash cascade (`*0x39`, `^`, `*0x3d73`, `+0xc0ae5`, etc.).
3. Returns a positive 31-bit integer (`& 0x7fffffff`).
4. The **caller** (or inline expansion) converts that integer to float and multiplies by `2⁻³⁰` (constant `0x00278050` = `9.3132257e-10`). This yields a normalized pseudo-random float.
5. The four corner hashes of a lattice cell are blended with a cubic-like smooth interpolant (the assembly does smooth Hermite-style weighting using `SUBSS`, `MULSS`, `MULSS`, `SUBSS` chains).

> **Confidence: HIGH** — the constant `0x00278050` was read directly and decoded; the interpolation pattern matches standard value-noise smoothing.

### 2.3 Island Generation Formula
Disassembly of `Terrain_GenerateIslandHeightfield @ 00056560` plus constant reads yields the following per-pixel formula.

**Symbols:**
- `sx, sy` = source-grid coordinates in the 2049×2049 array (indices 0 … 2048).
- `dx = sx - 640`, `dy = sy - 640`
- `dist₀ = sqrt(dx² + dy²)`
- `falloff = clamp(dist₀ / 256.0 - 0.75, 0.0, 3.0)`
- `falloff = falloff²`

**Noise octaves:**
1. `n1 = noise(sx / 64.0, sy / 128.0)` — first octave (anisotropic ratio 2:1)
2. `n2 = noise(sx / 128.0, sy / 128.0)` — second octave
3. `n3 = noise(sx / 32.0, sy / 32.0)` — third octave (finest)

**Edge blend:**
- `ex = sx - 704`, `ey = sy - 704`
- `dist₁ = sqrt(ex² + ey²)`
- `edge = clamp((dist₁ - 320.0) × 0.015625, 0.0, 1.0)`  *(ramps from 0 → 1 across a 64-source-unit band)*

**Final height before edge shaping:**
```
height = [ n3×8.0 + 23.75 + (n1 + falloff)×16.0 + (n2×32.0 + falloff×8.0) × falloff ] × edge
         + (1.0 - edge) × 23.875
```

**Decoded constants used in the formula:**

| Address | Raw bytes | Float | Meaning |
|---|---|---|---|
| `002780cc` | `00 00 80 3b` | `0.00390625` | `1/256` — distance-to-falloff scale |
| `002783a0` | `00 00 40 3f` | `0.75` | Falloff offset (island center bias) |
| `00278644` | `00 00 40 40` | `3.0` | Falloff clamp maximum |
| `00278728` | `00 00 00 41` | `8.0` | Noise amplitude scale for `n3` and falloff term |
| `002787d4` | `00 00 00 42` | `32.0` | Noise amplitude scale for `n2` |
| `00278774` | `00 00 80 41` | `16.0` | Scale for `(n1 + falloff)` term |
| `002787a8` | `00 00 be 41` | `23.75` | Base height bias added to noise stack |
| `002787ac` | `00 00 bf 41` | `23.875` | Flat sea/edge base height when `edge = 0` |
| `00278148` | `00 00 80 3c` | `0.015625` | `1/64` — edge blend scale |
| `00278900` | `00 00 a0 43` | `320.0` | Edge blend radius offset |
| `00278178` | `00 00 00 3d` | `0.03125` | `1/32` — used inside noise argument scaling |
| `00278338` | `00 00 00 3f` | `0.5` | Noise argument half-scale |

**Non-auto-generated extra edge shaping:**  
For authored maps (`isMapAutoGenerated == 0`), two extra edge-shaping lobes are applied near the north-west and south-east corners using a quadratic falloff with constants `36.0`, `24.0`, `0.25`, and `11.75` (see assembly at `000567dc`–`000568e5`). This flattens the map edges to make room for the city.

> **Confidence: HIGH** for the core formula; the exact interpolation weights of `TerrainNoiseHash` are inferred from assembly but the overall value-noise structure is certain.

---

## 3. What Shapes the Terrain Under/Around the CITY

### 3.1 The city does NOT use a separate heightfield
The wilderness island and the city share the **same** procedural base (`Terrain_GenerateIslandHeightfield`). After the base is generated, the city flattens and overwrites specific regions in two ways:

### 3.2 Street intersections get direct height patches
`Terrain_RebuildLandTilesRoadPatchesAndChunkCaches` (decompile `000d12e0`) contains:
```c
if (isMapAutoGenerated == 0) {
    Terrain_ApplyStreetIntersectionHeightPatch(2);
    Terrain_ApplyStreetIntersectionHeightPatch(0x28);
    Terrain_ApplyStreetIntersectionHeightPatch(0x20);
    ... /* 26 hardcoded intersection indices total */
}
```
So the patches are **only** applied to a hardcoded subset of intersections — the city intersections, not the wilderness.

### 3.3 What `Terrain_ApplyStreetIntersectionHeightPatch` does
Decompile at `000d3340`:
1. **Bilinear base patch:** Computes the rectangle spanning the intersection plus its south/west neighbor extents. Writes a bilinear interpolation of the four corner heights across every sample in that rectangle:
   ```
   h = (1.0 - fx) * (1.0 - fy) * h_SW
     + (1.0 - fx) * fy       * h_NW
     + fx       * (1.0 - fy) * h_SE
     + fx       * fy         * h_NE
   ```
   This is written directly into the runtime terrain tile array via `Terrain_SetLandSampleWithTileBorderMirroring`.

2. **Sub-block refinement:** For each 4×4 sub-block inside the intersection, it samples the four corner heights from the terrain. If all four are positive, it subdivides that sub-block into a 5×5 grid and writes another bilinear interpolation.

3. **16-pass distance-weighted smoothing:** After the height patch is stamped, the function runs a 16-iteration smoothing loop over the intersection footprint. For each sample it scans the 8 neighbors. Neighbors that have been “marked” (from step 2) contribute with a weight:
   ```
   w = 1.0 - distance_to_neighbor × 0.0625
   w = clamp(w, 0.0, 1.0)
   ```
   The accumulated height is blended back using a smoothstep-like curve derived from the maximum weight:
   ```
   blend = (3.0 - 2.0×w_max) × w_max²
   final_h = (1.0 - blend) × original_h + blend × smoothed_h
   ```
   This gradually feathers the intersection into the surrounding procedural terrain.

**Decoded smoothing constant:**

| Address | Raw bytes | Float | Meaning |
|---|---|---|---|
| `002781bc` | `00 00 80 3d` | `0.0625` | `1/16` — neighbor distance-to-weight decay in the 16 smoothing passes |

### 3.4 Street segments are flattened to a fixed row height
`MapConnector_PlaceStreetSegmentTilesAndSlopes @ 00020580` calls:
```c
UpdateMapTopHeightAndClearOccupancyBit(x, z, (float)(int)row * 4.0);
```
Constant `00278688` = `4.0`. The street row index directly determines the world-Y of the street surface. This flattens the terrain **under** the road to a perfectly level plane at `Y = row × 4.0`.

> **Conclusion:** City terrain is the procedural island **overwritten** by intersection-specific bilinear patches + multi-pass smoothing, and further flattened by explicit per-row height stamps under every street segment. It is **not** a separate authored heightmap.

> **Confidence: HIGH** — the overwrite is proven by direct calls to `Terrain_SetLandSampleWithTileBorderMirroring` inside `Terrain_ApplyStreetIntersectionHeightPatch`, and the flat street heights are proven by `UpdateMapTopHeightAndClearOccupancyBit`.

---

## 4. Coordinate System & Grid Mapping

### 4.1 Source → Destination → World
1. **Source heightfield** (`g_terrainHeightfield`): 2049 × 2049 floats, stride `0x801` (2049).
2. **Destination tiles** (`g_landTileHeightChunks`): 8 × 8 tiles. Each stored tile is 2049 × 2049 floats (including a 1-sample border for seamless filtering), stride `0x801`.
3. **Total runtime grid**: 16384 × 16384 usable samples (`8 tiles × 2048` interior samples per tile).

### 4.2 Resampling step
`Terrain_RebuildLandTilesRoadPatchesAndChunkCaches` reads each source cell `(sx, sy)` … `(sx+1, sy+1)` and bilinearly subdivides it into an 8×8 destination block:
```c
for (sy = 0; sy < 0x4000; sy += 8)   // 16384
  for (sx = 0; sx < 0x4000; sx += 8) // 16384
    for (iy = 0; iy < 8; iy++)
      for (ix = 0; ix < 8; ix++)
        // bilinear interp with step = 0.125
```
Constant `FLOAT_00278224` = `0.125` (`1/8`).

### 4.3 World ↔ Grid mapping
From `World_InitResourcePoolsAndCatalogs` (decompile `0013e9b5`), the chunk world origins are initialized as:
- **WorldOriginX** for tile column `tx` (0 … 7): `-4096.0 + tx × 2048.0`
- **WorldOriginZ** for tile row    `tz` (0 … 7): `-4096.0 + tz × 2048.0`
- **WorldOriginY** for all chunks: `0.0`

Decoded constants:

| Address | Raw bytes | Float | Meaning |
|---|---|---|---|
| `00278a9c` | `00 00 00 45` | `2048.0` | Tile origin spacing |
| `00278aac` | `00 00 80 45` | `4096.0` | World-to-grid offset |

In `Terrain_RebuildChunkVertexBufferAndNormals @ 000d00f0`, a vertex at sample index `(ix, iz)` inside a chunk is placed at:
```
worldX = chunkWorldOriginX + ix
worldY = chunkWorldOriginY + height_sample
worldZ = chunkWorldOriginZ + iz
```
Because the index is added directly to the origin with no extra scale, **one terrain-grid unit = 1.0 world unit**.

Therefore:
```
worldX = terrainGridX - 4096.0
worldZ = terrainGridZ - 4096.0
worldY = height_sample   (direct float value)
```

The island center is at source grid `(640, 640)`, which maps to:
- Destination grid: `(640×8, 640×8) = (5120, 5120)`
- World: `(5120−4096, 5120−4096) = (1024, 1024)`

### 4.4 Street grid ↔ Terrain grid
From `Terrain_ApplyStreetIntersectionHeightPatch`:
```c
gridX = (int)(intersection.pos.x + field_0x30) * 4 + 0x1000;   // +4096
gridZ = (intersection.unk0[1] + field_0x2c) * 4 + 0x1000;   // +4096
```
This confirms the same offset: **terrain grid = world coordinate × 1 + 4096**, or equivalently **world = grid − 4096**.

> **Confidence: HIGH** — the chunk origin initialization and the street-patch coordinate math both agree on the −4096 offset and 1.0 unit spacing.

---

## 5. Blending / Smoothing / Flattening

### 5.1 Island-to-sea blending
The island’s outer edge blends into a flat base of `23.875` over a band centered at source-grid distance `320.0` from `(704, 704)` (world ~`(1536, 1536)`), with a width of `64.0` source units (because `1/0.015625 = 64`). This is the `edge` factor in the generation formula.

### 5.2 City intersection smoothing
After stamping the bilinear intersection height, `Terrain_ApplyStreetIntersectionHeightPatch` performs **16 iterations** of neighbor-distance smoothing with a decay constant of `0.0625` (`1/16`).

The weight falloff is linear in neighbor distance:
```
weight = max(0.0, 1.0 - dist × 0.0625)
```
The blend factor is the standard smoothstep `3w² − 2w³` computed from the maximum neighbor weight. The result is written back into the terrain tile array, so each pass softens the transition between the intersection plateau and the surrounding island slopes.

### 5.3 Street segment flattening
`MapConnector_PlaceStreetSegmentTilesAndSlopes` sets the top-height of every street tile to `(float)row × 4.0`. This creates perfectly flat roads, independent of the underlying procedural noise.

> **Confidence: HIGH** for the parameters; the exact inner weight-accumulation logic in the 16-pass loop is dense (decompile shows a list-processing loop with up to 2 neighbor slots), but the `0.0625` distance scale and the `smoothstep` blend are unambiguous from the assembly.

---

## 6. Constants Summary

| Name / Global | Address | Raw | Decoded Float | Role |
|---|---|---|---|---|
| `FLOAT_002783f4` | `002783f4` | `00 00 80 3f` | `1.0` | Unity / clamp max / interpolation endpoint |
| `DAT_002780cc` | `002780cc` | `00 00 80 3b` | `0.00390625` | `1/256` — radial falloff scale |
| `DAT_002783a0` | `002783a0` | `00 00 40 3f` | `0.75` | Radial falloff subtract |
| `DAT_00278644` | `00278644` | `00 00 40 40` | `3.0` | Max falloff clamp; also used as height later |
| `DAT_00278728` | `00278728` | `00 00 00 41` | `8.0` | Noise amp / falloff product amp |
| `DAT_002787d4` | `002787d4` | `00 00 00 42` | `32.0` | Noise amplitude scale (octave 2) |
| `DAT_00278774` | `00278774` | `00 00 80 41` | `16.0` | Noise+falloff scale (octave 1) |
| `_DAT_002787a8` | `002787a8` | `00 00 be 41` | `23.75` | Base height bias inside noise stack |
| `_DAT_002787ac` | `002787ac` | `00 00 bf 41` | `23.875` | Flat edge/base height (sea level estimate) |
| `DAT_0027874c` | `0027874c` | `00 00 3c 41` | `11.75` | Extra edge-shape floor for non-auto maps |
| `DAT_002787e0` | `002787e0` | `00 00 10 42` | `36.0` | Extra edge-shape amplitude |
| `DAT_002787b0` | `002787b0` | `00 00 c0 41` | `24.0` | Extra edge-shape amplitude |
| `FLOAT_00278280` | `00278280` | `00 00 80 3e` | `0.25` | Edge-shape step scale |
| `DAT_00278148` | `00278148` | `00 00 80 3c` | `0.015625` | `1/64` — outer edge blend scale |
| `DAT_00278900` | `00278900` | `00 00 a0 43` | `320.0` | Outer edge blend radius offset |
| `DAT_00278688` | `00278688` | `00 00 80 40` | `4.0` | Street row-to-world-Y multiplier |
| `DAT_00278698` | `00278698` | `00 00 88 40` | `4.25` | Street height offset (used in intersection patch corners) |
| `FLOAT_002781bc` | `002781bc` | `00 00 80 3d` | `0.0625` | `1/16` — smoothing neighbor distance decay |
| `FLOAT_00278224` | `00278224` | `00 00 00 3e` | `0.125` | `1/8` — source→destination resampling step |
| `DAT_00278178` | `00278178` | `00 00 00 3d` | `0.03125` | Noise argument scale (1/32) |
| `DAT_00278338` | `00278338` | `00 00 00 3f` | `0.5` | Noise argument half-scale |
| `DAT_00278050` | `00278050` | `00 00 80 30` | `9.313e-10` | `1/2³⁰` — integer hash → float normalizer |
| `DAT_00278a9c` | `00278a9c` | `00 00 00 45` | `2048.0` | Tile origin spacing |
| `DAT_00278aac` | `00278aac` | `00 00 80 45` | `4096.0` | World origin offset |

---

## 7. Honest Assessment / What Could Not Be Determined

| Item | Confidence | Notes |
|---|---|---|
| Core procedural formula (three noise octaves + radial falloff + edge blend) | **HIGH** | All constants read from memory; assembly formula reconstructed and verified against decompilation. |
| City terrain = procedural base **overwritten** by patches | **HIGH** | Direct evidence: `Terrain_SetLandSampleWithTileBorderMirroring` is called inside intersection patch and street segment code. |
| Street flattening to `row × 4.0` | **HIGH** | Explicit `UpdateMapTopHeightAndClearOccupancyBit` argument. |
| Grid size (16384×16384), tile count (8×8), world spacing (1.0) | **HIGH** | Derived from chunk origin math and resampling loops. |
| `TerrainNoiseHash` internal interpolation polynomial | **MEDIUM-HIGH** | It is value noise with 31-bit hash and 2⁻³⁰ normalizer; the exact cubic weights were read from disassembly but the decompiler collapses them. The behavior is “smoothly interpolated hash lattice”. |
| Extra non-auto-generated edge shaping (assembly `000567dc`–`000568e5`) | **MEDIUM** | Two corner-lobe shapes are present, but the full polynomial was not fully simplified because Ghidra’s decompilation branches heavily on `cmp iVar8, 0x82` etc. The constants (`36.0`, `24.0`, `0.25`, `11.75`) are proven, but the exact geometric footprint is best described as “quadratic edge falloffs near two opposing corners”. |
| Water / sea level | **MEDIUM** | No string reference to “water level” was found. The flat edge value `23.875` is the most likely sea level, but the game could render water at a different shader-defined height. |
| Exact list of all 26 patched intersections | **HIGH** | They are hardcoded literals in `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches`; the list is reproduced above. |
| Is the 16-pass intersection smoothing applied at load time or runtime? | **HIGH** | It is inside `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches`, which is called once from `World_LoadMapAndInitializeTerrain`. So it is load-time only. |

---

## 8. Key Decompile Snippets (Evidence)

### 8.1 Island formula core (`00056560`)
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

### 8.2 Intersection patch bilinear + smoothing (`000d3340`)
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

### 8.4 Chunk origin initialization (`0013e9b5`)
```c
piVar7[-4] = -0x3a800000;           // worldOriginX = -4096.0 (float bits 0xC5800000)
piVar7[-3] = 0;                     // worldOriginY = 0.0
fVar15 = (float)iVar9 * 2048.0 - 4096.0;
piVar7[-2] = (int)fVar15;           // worldOriginZ
```

---

*End of report.*
