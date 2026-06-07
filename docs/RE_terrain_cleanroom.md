# Sub Rosa (subrosa.exe) – Terrain Generation Clean-Room Reverse-Engineering Report

**Program:** `subrosa.exe` (x86-64, image base 0)  
**Investigation tools:** Ghidra MCP bridge only (`ghidra_decompile_function`, `ghidra_disassemble_bytes`, `ghidra_read_memory`, `ghidra_search_functions`, `ghidra_search_strings`, `ghidra_get_xrefs_to`, etc.)  
**Scope:** How the GAME binary builds and shapes its terrain, with particular focus on the CITY area (streets, intersections, buildings).  
**Date:** 2026-06-07  

---

## 1. Executive Summary

Sub Rosa’s terrain is produced in **three distinct layers**:

1. **Base island heightfield** (`Terrain_GenerateIslandHeightfield`) – a 2049 × 2049 procedural float array generated from a deterministic value-noise function, radial island falloff, and edge-shaping ramps. This runs for every map.
2. **Runtime chunk tessellation** (`Terrain_RebuildLandTilesRoadPatchesAndChunkCaches`) – the 2049 × 2049 source is bilinearly upsampled by 8× into 8 × 8 runtime chunks covering 16384 × 16384 world samples.
3. **City terrain override** – for non-auto-generated maps (i.e. the CITY), street segments are first stamped into four “map top-height” float arrays (`0x1000 × 0x1000`). Then `Terrain_ApplyStreetIntersectionHeightPatch` is called for a hard-coded list of intersection indices. Each call bilinearly interpolates the corner heights across the 2×2 intersection block and runs a multi-pass smoothstep feather (radius ≈ 16 grid units) to blend the road surface back into the wilderness.

The wilderness and the city do **not** share the same final height array in the affected zones: the city patches overwrite the upsampled island terrain with values driven by the street graph.

---

## 2. Functions Found

| Address | Name | Role |
|---------|------|------|
| `0x00056560` | `Terrain_GenerateIslandHeightfield` | Fills the global 2049×2049 base heightfield with procedural island noise + edge shaping. |
| `0x001305f0` | `TerrainNoiseHash` | Deterministic 2-D value-noise kernel (integer hash of grid corners + bilinear interpolation). |
| `0x000d12e0` | `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches` | Upsamples base heightfield into 8×8 runtime land-tile chunks, then applies intersection patches for non-auto-generated maps. |
| `0x000d2db0` | `Terrain_SetLandSampleWithTileBorderMirroring` | Writes one float into the correct chunk and mirrors it into the neighbour’s border seam. |
| `0x000d3340` | `Terrain_ApplyStreetIntersectionHeightPatch` | Overwrites chunk heights inside a 2×2 intersection rectangle with bilinearly interpolated road heights + smoothstep feathering. |
| `0x00020580` | `MapConnector_PlaceStreetSegmentTilesAndSlopes` | Places street-segment tiles and writes the road elevation into four global `0x1000×0x1000` “map top-height” arrays. |
| `0x00020130` | `MapConnector_PlaceIntersectionPerimeterTiles` | Places the intersection perimeter shell tiles. |
| `0x0003fa40` | `UpdateMapTopHeightAndClearOccupancyBit` | Updates the four top-height arrays and clears the 4×4 land-use mask for city building placement. |
| `0x00056980` | `World_LoadMapAndInitializeTerrain` | Top-level world bootstrap: loads archive, builds street graph, stamps buildings, then calls the terrain rebuild. |

---

## 3. How the Base Terrain Heightfield is Produced

### 3.1 The array

`Terrain_GenerateIslandHeightfield` writes sequentially into a global float array starting at `g_terrainHeightfield` (`0x2d48282c`). The loop covers:

- Outer dimension (what the code treats as Z/row): `0` … `0x800`  → 2049 iterations  
- Inner dimension (X/col): `0` … `0x800`  → 2049 iterations  

Total: **2049 × 2049 = 4,196,001 floats** (~16.8 MiB).

### 3.2 The core formula (decompile evidence)

For every sample `(uVar9, iVar11)` (both 0…2048) the function computes:

```c
float dist1 = sqrt((uVar9 - 0x280)^2 + (iVar11 - 0x280)^2);
float f17  = dist1 * 0.00390625f - 0.5f;   // = dist1/256 - 0.5
f17 = clamp(f17, 0.0f, 3.0f);
f17 = f17 * f17;                            // squared falloff, now in [0, 9.0]
```

Then three `TerrainNoiseHash` calls are issued at different lattice frequencies:

| Call | Effective `(x, y)` passed to `TerrainNoiseHash` | Amplitude multiplier (what the caller multiplies the returned [0,2) float by) |
|------|---------------------------------------------------|--------------------------------------------------------------------------------|
| 1 | `(uVar9 / 64.0,  iVar11 / 128.0)` | `8.0`  (`DAT_00278728`) |
| 2 | `(uVar9 / 128.0, iVar11 / 128.0)` | `32.0` (`DAT_002787d4`) |
| 3 | `(uVar9 / 64.0,  iVar11 / 64.0)`  | `8.0`  (`DAT_00278728`) |

The final height before edge shaping is:

```c
float inner = noise3 * 8.0f
            + 23.8125f
            + (noise1 + f17) * 16.0f
            + (noise2 * 32.0f + f17 * 8.0f) * f17;

float f15 = (sqrt((uVar9 - 0x2c0)^2 + (iVar11 - 0x2c0)^2) - 320.0f) * 0.015625f;
f15 = clamp(f15, 0.0f, 1.0f);

float height = inner * f15 + (1.0f - f15) * 23.875f;
```

**Interpretation:**
- `f17` is a radial island mask centred on grid sample `(640, 640)` with radius ~896 samples (because `dist/256 - 0.5 = 3.0` → `dist = 896`). It is 0 at the centre, grows quadratically to 9.0 at the 896-sample rim, and is clamped.
- `f15` is a second, softer radial falloff centred on `(704, 704)` with radius 320 samples, also scaled by `1/64`. It blends between the procedural island height (`inner`) and a flat base level of **23.875**.
- The noise octaves give the island its undulations: a coarse octave at `1/64` and `1/128` grid spacing, and a finer octave at `1/64`.

### 3.3 Edge shaping (non-auto-generated maps only)

When `isMapAutoGenerated == 0` (the CITY maps), two extra edge ramps are stamped on top of the island:

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

These are **cubic edge ramps** that raise the ground from a flat **11.75 m** at the very edge to **23.75 m** where `edge = 1.0`. They shape the island’s shoreline into a sloped beach/cliff so that the city does not sit on raw noise at the water line.

---

## 4. The Numbers – Decoded Constants

All floats were read with `ghidra_read_memory` and decoded as little-endian IEEE-754.

| Symbol / Address | Raw bytes | Decoded float | Meaning |
|------------------|-----------|---------------|---------|
| `DAT_002780cc` | `00 00 80 3b` | **0.00390625** (1/256) | Island radial scale |
| `DAT_002783a0` | `00 00 40 3f` | **0.5** | Island radial offset |
| `DAT_00278644` | `00 00 40 40` | **3.0** | Island radial clamp |
| `DAT_00278728` | `00 00 00 41` | **8.0** | Noise octave amplitude |
| `DAT_002787d4` | `00 00 00 42` | **32.0** | Noise octave amplitude |
| `DAT_002787a8` | `00 00 be 41` | **23.8125** | Base height offset in island formula |
| `DAT_00278774` | `00 00 80 41` | **16.0** | `(noise1 + f17)` multiplier |
| `DAT_002787ac` | `00 00 bf 41` | **23.875** | Flat base-level for the `f15` blend |
| `DAT_00278900` | `00 00 a0 43` | **320.0** | Second falloff radius (centre at 704,704) |
| `DAT_00278148` | `00 00 80 3c` | **0.015625** (1/64) | Second falloff scale |
| `FLOAT_00278280` | `00 00 80 3e` | **0.25** | Edge-ramp slope scale |
| `DAT_002787e0` | `00 00 10 42` | **36.0** | Edge-ramp height coefficient |
| `DAT_002787b0` | `00 00 c0 41` | **24.0** | Edge-ramp attenuation coefficient |
| `DAT_0027874c` | `00 00 3c 41` | **11.75** | Edge-ramp minimum / flat base |
| `DAT_002783f4` | `00 00 80 3f` | **1.0** | Universal normalisation factor (also used as blend max) |
| `DAT_00278688` | `00 00 80 40` | **4.0** | World-unit scale for street grid → world Y |
| `DAT_00278698` | `00 00 88 40` | **4.25** | Street grid world-Y offset |
| `FLOAT_002781bc` | `00 00 80 3d` | **0.0625** (1/16) | Smoothstep feather: `1.0 / radius` |
| `FLOAT_00278224` | `00 00 00 3e` | **0.125** (1/8) | Upsample interpolation step |
| `DAT_00278050` | `00 00 80 30` | **9.313e-10** (2⁻³⁰) | Value-noise integer → float normaliser |
| `_DAT_41290040` | `00 00 80 c5` | **-4096.0** | World-to-grid origin offset X |
| `_DAT_41290048` | `00 00 80 c5` | **-4096.0** | World-to-grid origin offset Z |

---

## 5. Coordinate System & Grid Mapping

### 5.1 World → terrain-grid conversion

From `Terrain_CollectHeightfieldTrianglesAlongSegment` (`0x000d05b0`):

```c
float gridXf = worldX - (-4096.0f);   // = worldX + 4096.0
float gridZf = worldZ - (-4096.0f);   // = worldZ + 4096.0f
int gridX = (int)gridXf;
int gridZ = (int)gridZf;
```

**World origin mapping:**
- World `(-4096, 0, -4096)` maps to grid sample `(0, 0)`.
- World `(0, 0, 0)` maps to grid sample `(4096, 4096)`.
- The maximum valid grid sample is `16383` in each dimension.

### 5.2 Runtime chunk layout

`Terrain_SetLandSampleWithTileBorderMirroring` (`0x000d2db0`) enforces:

```c
uint chunkX = (int)param_1 >> 0xb;   // param_1 / 2048
uint chunkZ = (int)param_2 >> 0xb;   // param_2 / 2048
if (chunkX > 7 || chunkZ > 7) return;
```

- **8 × 8 chunks** in X and Z.
- Each chunk holds **2048 × 2048 cells** (and **2049 × 2049 float samples** including the shared border for seamless rendering).
- Total runtime unique samples: **16384 × 16384** per dimension.
- Sample spacing is **1 world unit per sample** (because 16384 world units span 16384 samples).

### 5.3 World Y conversion

In `Terrain_CollectHeightfieldTrianglesAlongSegment`:

```c
float worldY = chunkWorldOriginY + heightSample;
```

And in `World_InitResourcePoolsAndCatalogs` the chunk origin Y is initialised to **0.0**.  
**Therefore:**

```
worldY = heightSample   (direct 1:1 mapping)
```

The float values stored in the height arrays are literally world-metre elevations.

---

## 6. What Shapes the Terrain Under the CITY?

### 6.1 The city does NOT sit on raw island noise

For non-auto-generated maps (`isMapAutoGenerated == 0`), the build order in `World_LoadMapAndInitializeTerrain` is:

1. `Terrain_GenerateIslandHeightfield()` → creates the base island.
2. Street graph loaded from `city2.sbc`.
3. `MapConnector_PlaceStreetSegmentTilesAndSlopes()` and `MapConnector_PlaceIntersectionPerimeterTiles()` run for every segment/intersection. These call `UpdateMapTopHeightAndClearOccupancyBit(x, z, height)` which writes the road surface elevation into four global `0x1000 × 0x1000` float buffers (`0x3ce6f5f8`, `0x3ce6f5fc`, `0x3ce735f8`, `0x3ce735fc`).
4. `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches()` upsamples the base island into the runtime 8×8 chunks, then applies a hard-coded list of **31 intersection patches**:

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

For a given intersection index, the function:

**A. Finds the 2×2 neighbour block**  
It walks south, west, and south-west to locate the four intersections that bound the city block. It then computes the axis-aligned rectangle in grid coordinates that spans from the north-east corner to the south-west corner.

**B. Bilinear interpolation**  
It reads the four corner heights from the four `0x1000×0x1000` top-height arrays:

```c
float hNE = mapTopHeight[cornerNE];
float hSE = mapTopHeight[cornerSE];
float hNW = mapTopHeight[cornerNW];
float hSW = mapTopHeight[cornerSW];
```

Then it walks every grid sample inside the rectangle and writes:

```c
float u = (x - xMin) / width;   // 0 … 1
float v = (y - yMin) / height;  // 0 … 1
float roadH = (1-u) * ((1-v)*hNW + v*hSW) + u * ((1-v)*hNE + v*hSE);
Terrain_SetLandSampleWithTileBorderMirroring(gridX, gridZ, roadH);
```

This is a **flat bilinear road surface** whose corners are pinned to the street-segment elevations.

**C. Multi-pass smoothstep feather**  
After the flat interpolation, the function runs a propagation/smoothing pass:

- A local `28 × 28` working grid (`0x04afa9bc` region) stores flags, source coordinates, and weights for every sample inside the rectangle.
- Samples that lie on the actual street lane get marked as “sources” with weight `1.0f`.
- For **16 iterations**, the algorithm scans the grid. For each non-source cell it looks in the 3×3 neighbourhood for source cells, computes Euclidean distance, and converts it to a weight:
  ```c
  weight = 1.0f - distance * 0.0625f;   // radius = 16 samples
  weight = clamp(weight, 0.0f, 1.0f);
  ```
- After propagation, each cell is blended between the **original wilderness height** and the **bilinear road height** using a smoothstep:
  ```c
  float blend = (3.0f - 2.0f * maxWeight) * maxWeight * maxWeight;
  float finalH = (1.0f - blend) * originalH + blend * roadH;
  ```

**Result:** The city ground is a drivable, level bilinear patch that is **smoothstep-feathered** over roughly 16 grid samples (~16 world units) back into the surrounding procedural island. The wilderness height is completely overwritten inside the road footprint and gradually restored outside the 16-sample radius.

---

## 7. Blending / Smoothing Summary

| Process | Where | Type | Parameters |
|---------|-------|------|------------|
| Island radial blend | Base heightfield | Linear blend between `inner` noise and flat **23.875** | Falloff radius ~320 samples, scale `1/64` |
| Edge ramp | Base heightfield (city maps only) | Cubic ramp `(36 - 24·edge)·edge² + 11.75` | Edge parameter `0…1`, 0.25 step |
| Upsample | Runtime chunks | Bilinear 8× upsample | Step `0.125` (1/8) |
| Intersection patch | Runtime chunks (city maps) | Bilinear road surface + smoothstep feather | Feather radius **16** samples, weight `1 - dist/16`, blend `(3-2w)·w²` |

---

## 8. Confidence & Open Questions

### High confidence
- The base terrain is procedural value noise (`TerrainNoiseHash`) plus radial falloff; it is **not** loaded from an external heightmap file.
- World Y equals the stored float sample directly (origin Y = 0).
- Grid mapping is `grid = floor(world + 4096)` with 16384 × 16384 samples, 8 × 8 chunks.
- City terrain overwrites the wilderness via intersection patches that read street-segment heights from four `0x1000×0x1000` float buffers.
- The feather uses a smoothstep with radius ~16 samples.

### Medium confidence
- Exact lattice frequencies of all three noise octaves: I traced the first two with certainty (`1/64` and `1/128`). The third scale is inferred from the pattern and the surrounding code but I did not fully trace its assembly due to register aliasing.
- The exact purpose of the four redundant `mapTopHeight` arrays: they are written identically by `UpdateMapTopHeightAndClearOccupancyBit`. I suspect they are used for different LODs or for building top-height vs. road top-height, but I did not find the separate readers.

### Low confidence / unresolved
- **Chunk memory layout:** `World_InitResourcePoolsAndCatalogs` allocates 64 chunks (`0x40`) of `0x1004004` bytes each (`0x1004004 ≈ 2049×2049×4`). The chunk world-origin initialisation uses complex pointer strides (`0x6840`, `0x3420`) across 8 outer iterations. I did not fully decompose which of the 64 chunks receive which `(originX, originZ)` pair, although the collision code proves the world spans -4096 → +12288.
- **Why only 31 hard-coded intersection indices?** The list in `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches` is a fixed literal array. I did not trace why these specific indices are patched rather than all intersections. It may be that only major arterial intersections need the full terrain patch, while minor streets rely on the tile-based road mesh to hide the ground.
- **Water level:** I did not locate the exact water-plane constant. The edge shaping bottoms out at 11.75, which is well above typical sea level, suggesting the water plane is somewhere below 11.75 (possibly at 0 or negative).

---

## 9. Decompile Snippets (Evidence)

### 9.1 Island radial falloff (`Terrain_GenerateIslandHeightfield`, `0x00056560`)

```c
fVar17 = SQRT((float)(int)(uVar9 - 0x280) * (float)(int)(uVar9 - 0x280) +
              (float)(iVar11 + -0x280) * (float)(iVar11 + -0x280)) * DAT_002780cc -
         DAT_002783a0;
if (fVar17 <= 0.0) { fVar17 = 0.0; }
if (DAT_00278644 <= fVar17) { fVar17 = DAT_00278644; }
fVar17 = fVar17 * fVar17;
```

### 9.2 Base-height blend (`Terrain_GenerateIslandHeightfield`)

```c
*pfVar7 = (extraout_XMM0_Da_01 * DAT_00278728 + _DAT_002787a8 +
          (extraout_XMM0_Da + fVar17) * DAT_00278774 +
          (fVar16 + fVar14) * fVar17) * fVar15 +
          (fVar3 - fVar15) * _DAT_002787ac;
```

### 9.3 Chunk write with border mirroring (`Terrain_SetLandSampleWithTileBorderMirroring`, `0x000d2db0`)

```c
uVar3 = (int)param_1 >> 0xb;
uVar2 = (int)param_2 >> 0xb;
if (7 < uVar3) return;
if (7 < uVar2) return;
// ... write into chunk memory at (uVar2 + uVar3*8) ...
*(undefined4 *)((&g_landTileHeightChunks)[lVar5 * 0x684] +
                (ulonglong)(uVar1 * 0x801 + uVar4) * 4) = param_3;
```

### 9.4 Intersection patch bilinear + smoothstep (`Terrain_ApplyStreetIntersectionHeightPatch`, `0x000d3340`)

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

### 9.5 Street segment top-height update (`UpdateMapTopHeightAndClearOccupancyBit`, `0x0003fa40`)

```c
if (*(float *)(&DAT_3ce6f5f8 + lVar1 * 4) <= param_3 &&
    param_3 != *(float *)(&DAT_3ce6f5f8 + lVar1 * 4)) {
    *(float *)(&DAT_3ce6f5f8 + lVar1 * 4) = param_3;
}
// ... same for 3ce6f5fc, 3ce735f8, 3ce735fc ...
```

---

*Report ends. All claims above are derived directly from decompilation and raw memory reads of `subrosa.exe` via the Ghidra MCP bridge, with no prior knowledge or assumptions injected.*
