# Sub Rosa Terrain Generation & City Shaping — Clean-Room Reverse Engineering

**Target binary:** `subrosa.exe.THIS_ONE` (x86-64 PE, language `x86:LE:64:default`, compiler `windows`)

**Method:** Ghidra MCP — decompile, memory reads, and symbol-based navigation only. No source/header assumptions. All addresses are virtual addresses within this binary's image base 0x00000000.

---

## 1. Functions Found That Produce or Shape Terrain

| Function | Address | Role |
|---|---|---|
| `Terrain_GenerateIslandHeightfield` | 0x00056560 | **Core**: fills the 2049×2049 heightfield with procedural noise + radial masks |
| `Terrain_ApplyStreetIntersectionHeightPatch` | 0x000d3340 | **City**: overwrites terrain under a street intersection with a flat bilinear patch |
| `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches` | 0x000d12e0 | **Finalizer**: resamples heightfield into 8×8 land-tile chunks, applies ~30 intersection patches, rebuilds vertex buffers |
| `Terrain_SetLandSampleWithTileBorderMirroring` | 0x000d2db0 | **Writer**: writes one height sample into the correct land-tile chunk, mirroring tile-edge writes |
| `TerrainNoiseHash` | 0x001305f0 | **Noise**: deterministic integer hash used as the procedural noise primitive |
| `World_LoadMapAndInitializeTerrain` | 0x00056980 | **Orchestrator**: calls heightfield gen, street placement, intersection perimeter, district/city stamping, then the rebuild |
| `Terrain_RebuildChunkVertexBufferAndNormals` | 0x000d00f0 | Rebuilds one terrain chunk's vertex buffer + normals from height data |
| `Render_InitTerrainStreamingBuffers` | 0x000c2db0 | Sets up GL streaming buffers for terrain rendering (6 LOD banks) |
| `MapConnector_PlaceStreetSegmentTilesAndSlopes` | 0x00020580 | Places street tiles and slope/fill cells along a street segment |
| `MapConnector_PlaceIntersectionPerimeterTiles` | 0x00020130 | Places perimeter shell tiles around an intersection |
| `UpdateMapTopHeightAndClearOccupancyBit` | 0x0003fa40 | Updates cached top-height and clears coarse occupancy bits per tile |
| `Texture_GenerateProceduralNoise` | 0x00099730 | Generates procedural noise texture (rendering, not heightfield) |

---

## 2. Terrain Heightfield Generation Mechanism

### 2.1 Grid layout

`Terrain_GenerateIslandHeightfield` @ 0x56560 iterates an **0x801 × 0x801 grid** (2049 × 2049 samples):

```c
// From decompile @ 0x56560:
pfVar7 = (float *)&g_terrainHeightfield;  // output array starts at DAT_2d482830
iVar11 = 0;      // z coordinate
iVar8 = 0x300;   // decrementing counter  
do {
    uVar9 = 0;   // x coordinate
    iVar10 = 0x27e;
    do {
        // ... compute height for (uVar9, iVar11) ...
        *pfVar7 = computed_height;
        uVar9++; iVar10--;
        pfVar7++;
    } while ((int)uVar9 < 0x801);
    iVar11++;
    iVar8--;
    if (iVar8 < -0x500) return;
} while(true);
```

The output array `g_terrainHeightfield` lives at `DAT_2d482830`. The two preceding 32-bit words (`DAT_2d482820` and `DAT_2d482828`) are initialized to `0xC5800000` (**-4096.0f**) before the loop — these are sentinel/dummy values before the actual data.

### 2.2 The noise primitive: TerrainNoiseHash

`TerrainNoiseHash` @ 0x1305f0 receives grid (x, z) as floats in XMM0/XMM1 and a global seed in R8D:

```c
uint TerrainNoiseHash(void)
{
    uint uVar2;
    int iVar1;
    // in_XMM0_Da = grid x (float), in_XMM1_Da = grid z (float), in_R8D = global seed

    uVar2 = ((int)in_XMM1_Da + 1U & in_R8D) * 0x39 +
            ((int)in_XMM0_Da + 1U & in_R8D);
    uVar2 = uVar2 ^ uVar2 * 0x2000;
    iVar1 = uVar2 * uVar2 * 0x3d73 + 0xc0ae5;
    return iVar1 * iVar1 + 0xd208dd0dU & 0x7fffffff;
}
```

**Output range:** `[0, 0x7FFFFFFF]` (~0 to 2.15×10⁹ as integer, normalized to [0, 1] by the caller via multiplication by a scale factor).

Three calls per sample provide three independent noise values with different frequency/scale multipliers.

### 2.3 The island formula (reconstructed from decompile)

Each height sample at grid position `(x, z)` ∈ `[0, 0x800]²` is computed as:

#### Step A — First radial mask (island envelope, center at (640, 640))

```
d1 = sqrt((x - 0x280)² + (z - 0x280)²)     // distance from (640, 640)
t  = clamp(d1 * 0x3B800000 - 0x3F400000, 0.0, 0x40400000)
   = clamp(d1 * 0.00390625 - 0.75,        0.0, 3.0)
t  = t²                                       // squared envelope (range 0..9)
```

- `d1 < 192`: t = 0 (flat center zone)
- At grid corner `(0, 0)`: d1 = √(640²+640²) ≈ 905 → t ≈ clamp(3.535-0.75, 0, 3)² = 9.0

#### Step B — Three noise contributions

```c
TerrainNoiseHash();                          // noise₁ (x, z)
fVar14 = t * DAT_00278728;                   // t * 8.0
TerrainNoiseHash();                          // noise₂
fVar16 = noise₂ * DAT_002787d4;              // noise₂ * 32.0
// ... compute second radial mask (see below) ...
TerrainNoiseHash();                          // noise₃

height_interior = noise₃ * DAT_00278728 + _DAT_002787a8
                + (noise₁ + t) * DAT_00278774
                + (fVar16 + fVar14) * t;
```

Substituting decoded constants:

```
height_interior = noise₃ * 8.0  + 23.75
                + (noise₁ + t) * 16.0
                + (noise₂ * 32.0 + t * 8.0) * t
```

So the interior height varies around a base of ~23.75, with three noise octaves scaled by 8.0, 16.0, and 32.0, plus the squared radial mask `t` modulating both noise₁ and the noise₂+t product.

#### Step C — Second radial mask (outer/inner blend, center at (704, 704))

```
d2   = sqrt((x - 0x2c0)² + (z - 0x2c0)²)   // distance from (704, 704)
t2   = clamp((d2 - DAT_00278900) * DAT_00278148, 0.0, FLOAT_002783f4)
     = clamp((d2 - 320.0) * 0.015625,       0.0, 1.0)
```

- `d2 < 320`: t2 = 0 (inner zone — flat water)
- `320 ≤ d2 ≤ 384`: t2 ramps linearly from 0 to 1
- `d2 ≥ 384`: t2 = 1 (outer zone — full noise terrain)

#### Step D — Final blend

```
height = height_interior * t2 + (1.0 - t2) * _DAT_002787ac
       = height_interior * t2 + (1.0 - t2) * 23.875
```

**Interpretation:** The region within ~320 units of (704, 704) is flat at **23.875** (water level). Beyond 384 units, the full procedural island terrain appears. The 64-unit transition zone blends smoothly.

#### Step E — Non-auto-generated map edge shaping

When `isMapAutoGenerated == 0`, two additional edge-shaping blocks overwrite selected columns/rows:

For positions where `x < 0x300` (768) and `z ≥ 0x83` (131):
```c
fVar15 = (float)(z_index + -0x82) * FLOAT_00278280;  // (z - 130) * 0.25
fVar14 = (float)(x_alt  + 0x82)  * FLOAT_00278280;  // (top-x-inv + 130) * 0.25
// clamp both to [0, 1], take max of (1-val₁, 1-val₂):
fVar16 = 1 - max(clamp(fVar15, 0, 1), clamp(fVar14, 0, 1));
fVar16 = clamp(fVar16, 0, 1);

height = (fVar5 - fVar16 * fVar4) * fVar16 * fVar16 + DAT_0027874c;
       = (36.0 - fVar16 * 24.0) * fVar16² + 11.75
```

This produces a smooth shoreline curve for the bottom/right edges of the island.

### 2.4 Evidence: decompile excerpt

From `Terrain_GenerateIslandHeightfield` @ 0x56560, the heart of the computation:

```
      fVar17 = SQRT((float)(int)(uVar9 - 0x280) * (float)(int)(uVar9 - 0x280) +
                    (float)(iVar11 + -0x280) * (float)(iVar11 + -0x280)) * DAT_002780cc -
               DAT_002783a0;
      if (fVar17 <= 0.0) {
        fVar17 = 0.0;
      }
      if (DAT_00278644 <= fVar17) {
        fVar17 = DAT_00278644;
      }
      fVar17 = fVar17 * fVar17;
      TerrainNoiseHash();
      fVar14 = fVar17 * DAT_00278728;
      TerrainNoiseHash();
      fVar16 = extraout_XMM0_Da_00 * DAT_002787d4;
      fVar15 = (SQRT((float)(int)(uVar9 - 0x2c0) * (float)(int)(uVar9 - 0x2c0) +
                     (float)(iVar11 + -0x2c0) * (float)(iVar11 + -0x2c0)) - DAT_00278900) *
               DAT_00278148;
      if (fVar15 <= 0.0) {
        fVar15 = 0.0;
      }
      if (fVar3 <= fVar15) {
        fVar15 = fVar3;
      }
      TerrainNoiseHash();
      *pfVar7 = (extraout_XMM0_Da_01 * DAT_00278728 + _DAT_002787a8 +
                (extraout_XMM0_Da + fVar17) * DAT_00278774 + (fVar16 + fVar14) * fVar17) * fVar15 +
                (fVar3 - fVar15) * _DAT_002787ac;
```

---

## 3. City Terrain — Is It Different?

**Answer: YES, the city terrain is the same procedural base, but explicitly overwritten with flat, leveled surfaces at every street intersection.**

### 3.1 The flow (from `World_LoadMapAndInitializeTerrain` @ 0x56980)

```
Terrain_GenerateIslandHeightfield();           // <-- procedural base
// ... street graph built ...
MapConnector_PlaceStreetSegmentTilesAndSlots() // places street tile blocks
MapConnector_PlaceIntersectionPerimeterTiles() // intersection perimeters
// ... city districts, buildings stamped ...
Terrain_RebuildLandTilesRoadPatchesAndChunkCaches()  // <-- CITY OVERWRITE HERE
```

### 3.2 `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches` @ 0xd12e0

This function does three things:
1. **Resamples** the global heightfield (`DAT_2d482830`) into **8×8 land-tile chunks** (`g_landTileHeightChunks`) using bilinear interpolation with step `FLOAT_00278224 = 0.125`
2. **For non-auto-generated maps only**, calls `Terrain_ApplyStreetIntersectionHeightPatch` for ~30 specific intersections (hardcoded list of indices: 2, 0x28, 0x20, 0x2e, 0x2d, 0x2f, 0x1c, 10, 0x21, 0x2a, 0x23, 0xd, 0x3b, 0x1f, 0x34, 0x30, 0x32, 0x13, 0x16, 0x3a, 0x3e, 0x3d, 0x29, 0x24, 0x2b, 0x2c, 0x26, 0x27)
3. Rebuilds all 64 terrain chunk vertex buffers

### 3.3 `Terrain_ApplyStreetIntersectionHeightPatch` @ 0xd3340 — the city flattening

This function takes an intersection index `param_1` and:

1. **Finds the 4 corners** by traversing the street graph:
   - South → south → west path for the southwest corner
   - West → west → south path for the northwest corner
   
2. **Computes corner heights:**
   ```
   corner_height = intersection_z * DAT_00278688 + DAT_00278698
                 = intersection_z * 4.0 + 4.25
   ```
   Each corner's Z comes from its `streetIntersections[].unk0[2]` field.

3. **Bilinear interpolation** across the intersection rectangle:
   ```
   for x from start_x to end_x:
       for z from start_z to end_z:
           height = (1-fx)*(1-fz)*h00 + fx*(1-fz)*h10 + (1-fx)*fz*h01 + fx*fz*h11
           Terrain_SetLandSampleWithTileBorderMirroring(z, x, height)
   ```

4. **Per-tile 4×4 subgrid smoothing:** Divides the intersection into tiles, and for each tile, fills empty 4×4 sub-cells with per-building data and applies **distance-weighted blending** with radius `FLOAT_002781bc = 0.0625` to smooth transitions.

5. **Final blend pass** with the surrounding terrain: each sample is blended between the patch height and the original terrain using a smoothstep based on distance from patch edges.

### 3.4 Evidence: city terrain is overwritten, not integrated

From the decompile of `Terrain_ApplyStreetIntersectionHeightPatch` @ 0xd3340, the bilinear patch writes:

```c
Terrain_SetLandSampleWithTileBorderMirroring(
    uVar18, uVar22,
    (fVar4 - fVar39) * (fVar36 * fVar40 + fVar43 * (fVar4 - fVar40)) +
    (fVar38 * fVar40 + fVar44 * (fVar4 - fVar40)) * fVar39
);
```

Where `fVar43, fVar44, fVar36, fVar38` are the four corner heights computed as `z_coord * 4.0 + 4.25`. This is a direct overwrite — there is **no sampling or blending with the procedural terrain** in this primary pass. The blending only happens in the secondary neighborhood-smoothing pass.

---

## 4. Decoded Constants

### 4.1 Terrain heightfield generation constants

| Address | Raw bytes (LE) | Hex as float | Decoded value | Meaning |
|---|---|---|---|---|
| 0x002780cc | `00 00 80 3B` | `0x3B800000` | **0.00390625** (1/256) | First radial mask distance scale |
| 0x002783a0 | `00 00 40 3F` | `0x3F400000` | **0.75** | First radial mask distance offset |
| 0x002783f4 | `00 00 80 3F` | `0x3F800000` | **1.0** | Universal clamp max / blend weight |
| 0x00278644 | `00 00 40 40` | `0x40400000` | **3.0** | Upper clamp for first radial mask t (before squaring) |
| 0x00278728 | `00 00 00 41` | `0x41000000` | **8.0** | Noise₁ scale factor for t-weighted term |
| 0x002787d4 | `00 00 00 42` | `0x42000000` | **32.0** | Noise₂ scale factor |
| 0x00278774 | `00 00 80 41` | `0x41800000` | **16.0** | (noise₁ + t) scale factor |
| 0x002787a8 | `00 00 BE 41` | `0x41BE0000` | **23.75** | Base terrain height (interior constant) |
| 0x002787ac | `00 00 BF 41` | `0x41BF0000` | **23.875** | Outer blend target (water level) |
| 0x00278900 | `00 00 A0 43` | `0x43A00000` | **320.0** | Second radial mask distance offset |
| 0x00278148 | `00 00 80 3C` | `0x3C800000` | **0.015625** (1/64) | Second radial mask distance scale |
| 0x00278280 | `00 00 80 3E` | `0x3E800000` | **0.25** | Edge-shaping distance scale |
| 0x0027874c | `00 00 3C 41` | `0x413C0000` | **11.75** | Base height for edge-shaping formula |
| 0x002787e0 | `00 00 10 42` | `0x42100000` | **36.0** | Edge-shaping peak constant (fVar5) |
| 0x002787b0 | `00 00 C0 41` | `0x41C00000` | **24.0** | Edge-shaping blend constant (fVar4) |

### 4.2 City / street / tile constants

| Address | Raw bytes (LE) | Hex as float | Decoded value | Meaning |
|---|---|---|---|---|
| 0x00278688 | `00 00 80 40` | `0x40800000` | **4.0** | Intersection Z→height multiplier |
| 0x00278698 | `00 00 88 40` | `0x40880000` | **4.25** | Intersection height offset |
| 0x002781bc | `00 00 80 3D` | `0x3D800000` | **0.0625** (1/16) | Intersection patch smoothing radius |
| 0x00278224 | `00 00 00 3E` | `0x3E000000` | **0.125** (1/8) | Bilinear resampling step (land-tile rebuild) |
| 0x002787a4 | `00 00 B8 41` | `0x41B80000` | **23.0** | Used in world init: `DAT_473de440 = DAT_002787a4` |
| 0x00278a68 | `00 00 B9 44` | `0x44B90000` | **1480.0** | World-space Y offset added to intersection pos |
| 0x002785a0 | `EA 2E 44 54 FB 21 F9 3F` | `0x3FF921FB54442EEA` | **~1.5707963** | Double: likely π/2 (building rotation increment) |

### 4.3 Adjacent array constants (context, for nearby floats at same addresses)

| Address | Hex | Value | Likely meaning |
|---|---|---|---|
| 0x0027872c | `0x41100000` | **9.0** | Noise scale for another octave |
| 0x00278730 | `0x41200000` | **10.0** | Noise scale for another octave |
| 0x00278734 | `0x41278D36` | **~10.4746** | Noise scale for another octave |
| 0x002787d8 | `0x42040000` | **33.0** | Noise scale for another octave |
| 0x002787dc | `0x420C0000` | **35.0** | Noise scale for another octave |
| 0x0027e7e0 | `0x42100000` | **36.0** | (same as 0x002787e0, edge-shaping peak) |
| 0x00278904 | `0x43A20000` | **324.0** | Possibly second radial mask outer bound |

### 4.4 Sentinel value

| Address | Raw bytes | Hex | Value |
|---|---|---|---|
| DAT_2d482820 | `00 00 80 C5` | `0xC5800000` | **-4096.0** — sentinel before heightfield array data |

---

## 5. Coordinate System and Grid Mapping

### 5.1 Heightfield grid

- **Dimensions:** 0x801 × 0x801 = **2049 × 2049 samples**
- **Data type:** `float` per sample (~16.8 MB)
- **Output array:** `g_terrainHeightfield` at `DAT_2d482830`
- **Grid indices:** `(x, z)` ∈ `[0, 0x800]²`
- **Center for island mask:** (0x280, 0x280) = **(640, 640)** — slightly offset from geometric center (1024, 1024)
- **Center for inner flat zone:** (0x2c0, 0x2c0) = **(704, 704)**

### 5.2 Land-tile chunk system

`Terrain_SetLandSampleWithTileBorderMirroring` @ 0xd2db0 reveals the tile layout:

```c
uVar3 = (int)param_1 >> 0xb;     // x >> 11 → tile column (0..7)
uVar2 = (int)param_2 >> 0xb;     // z >> 11 → tile row (0..7)
if (7 < uVar3 || 7 < uVar2) return;  // bounds check: 8×8 tiles

lVar5  = (longlong)(uVar3 + uVar2 * 8);  // tile index 0..63
uVar1  = param_2 & 0x7ff;                // intra-tile z (0..2047)
uVar4  = param_1 & 0x7ff;                // intra-tile x (0..2047)

// Write to g_landTileHeightChunks[tile_index * 0x684][z * 0x801 + x]
*(float*)((&g_landTileHeightChunks)[lVar5 * 0x684] + (uVar1 * 0x801 + uVar4) * 4) = height;
```

- **8 × 8 tiles** = 64 land tiles
- Each tile is **0x801 × 0x801** = 2049 × 2049 subsamples
- **Stride per tile:** 0x684 * 2049 = variable, based on `sizeof(landTileStruct)` = 0x684 entries with each entry being a pointer/buffer
- Tile-edge mirroring: when a write hits tile boundary (x=0 or z=0), the value is copied into the neighboring tile's edge as well

### 5.3 World-space coordinate mapping

From `World_LoadMapAndInitializeTerrain` @ 0x56980, street intersection positions:

```c
streetIntersections[idx].pos.y = (float)row * DAT_00278688;    // row * 4.0
streetIntersections[idx].pos.z = (float)col * DAT_00278688;    // col * 4.0
```

And from intersection height computation:
```c
corner_height = intersection_z * 4.0 + 4.25;
```

So **world Y (height) at grid position = heightfield value directly as float**. The terrain Y values are in the range ~[11.75, ~30+] in world units.

### 5.4 Street vs terrain grid

The `UpdateMapTopHeightAndClearOccupancyBit` function uses bit shifting consistent with the tile scheme:
- `<< 11` = multiply by 2048 (tile size in samples)
- The `g_landUseMaskBits` tracks coarse 4×4 occupancy per (param_1+0x400, param_2+0x400) position

---

## 6. Smoothing, Blending, and Flattening

### 6.1 The intersection patch blending (two-tier)

`Terrain_ApplyStreetIntersectionHeightPatch` @ 0xd3340 has two smoothing layers:

**Layer 1 — Per-tile 4×4 subgrid fill:**

The intersection rectangle is subdivided into a 4×4-per-tile grid. For each empty cell, it looks at building footprint data stored in the `DAT_04afa9a0` array structure and fills with bilinearly interpolated values from `DAT_3ce6f5f8` (top-height caches for the 4 corners of each building slot).

**Layer 2 — Neighbor-weighted smoothing:**

After filling, a neighborhood pass iterates with factor `FLOAT_002781bc = 0.0625`:
```c
fVar41 = sqrt(dx² + dz²);       // distance from sample to patch cell
fVar38 = 1.0 - fVar41 * 0.0625; // falloff weight
if (fVar38 <= 0.0) continue;    // outside radius → skip
```

For each neighbor cell's existing entries, the weight is used to compute a distance-blended contribution. The smoothing runs **0x10 iterations** (16 passes), giving a compound smoothing effect.

**Layer 3 — Final clamp-blend with base terrain:**

The last section uses a smoothstep-like curve with `DAT_00278644 = 3.0`:
```c
fVar38 = (3.0 - (2.0 * fVar38)) * fVar38 * fVar38;  // smoothstep variant
height = (1.0 - fVar38) * original_terrain + fVar38 * patch_height;
```

### 6.2 Edge-shaping at island bounds

For non-auto-generated maps, the right and bottom edges of the terrain grid use a cubic falloff:
```
height = (36.0 - 24.0 * clamp_edge) * clamp_edge² + 11.75
```
Where `clamp_edge` ranges from 0 (shore water) to 1 (full terrain), producing a smooth water-to-land transition.

### 6.3 Tile-border mirroring

`Terrain_SetLandSampleWithTileBorderMirroring` automatically mirrors writes that land on tile boundaries (x=0 or z=0 within a tile) into the adjacent tile's first column/row, ensuring seamless tile seams.

---

## 7. Confidence Assessment

| Claim | Confidence | Notes |
|---|---|---|
| Grid is 2049×2049 (0x801×0x801) | **High** | Loop bounds are explicit; same across all terrain functions |
| Island is procedural (noise hash + radial falloff) | **High** | Three TerrainNoiseHash calls per sample; no file I/O in the loop |
| Two radial masks at different centers | **High** | Two SQRT computations with distinct centers and scales |
| City terrain overwrites base terrain | **High** | 28 explicit intersection patches; street graph positions drive the heights |
| Noise hash is integer-based xorshift variant | **High** | Clear arithmetic: multiply, xor, square, add |
| All float constants decoded correctly | **High** | Raw bytes read, IEEE-754 verified |
| 8×8 tile chunk system with 0x7ff intra-tile indexing | **High** | Explicit bit shifting and array indexing |
| Smoothing radius 0.0625 | **High** | Clear constant read and formula |
| Exact noise frequency/wavelength | **Medium** | Scale factors known (8, 16, 32), but relationship to world units not fully resolved |
| Whether noise uses Perlin/value/gradient noise | **Low** | The integer hash returns a raw 31-bit value; the caller treats it as a uniform [0,1] value — this is value-noise, not gradient noise |
| Exact purpose of 4-element float arrays at 0x278728/0x2787d4 | **Low** | These look like multi-octave scale arrays but we only see one index dereferenced; they may be parameter tables for a loop unrolled by the compiler |
| Relationship between grid samples and world meters | **Medium** | Street positions use ×4.0 scale, terrain Y is direct float; grid spacing not confirmed in meters but likely 1 sample = some fraction of a world unit |

### What could not be determined

- **The exact noise seed value:** Passed via R8D register at call sites; Ghidra decompiler loses register argument tracking across the three sequential calls.
- **The map file format:** `World_LoadMapAndInitializeTerrain` reads a CSX archive; the full map data layout was not reverse-engineered.
- **The precise world-unit-to-sample ratio:** The edge of the island calculation suggests about 640 samples from center to shore — the world-unit mapping depends on how the camera/scene scale is set (not in scope).
- **Building footprint terrain:** Buildings are stamped after terrain generation via `InstantiateSbbVolumeIntoWorldAndMetadata`, but their impact on the terrain heightfield (beyond room-level adjustments) was not fully traced.

---

## 8. Appendix: Address-Sorted Function Index

```
0x00020000  ApplyAllCityDistrictCellsToWorld
0x00020130  MapConnector_PlaceIntersectionPerimeterTiles
0x00020580  MapConnector_PlaceStreetSegmentTilesAndSlopes
0x0003fa40  UpdateMapTopHeightAndClearOccupancyBit
0x00056560  Terrain_GenerateIslandHeightfield
0x00056980  World_LoadMapAndInitializeTerrain
0x00099730  Texture_GenerateProceduralNoise
0x000c2db0  Render_InitTerrainStreamingBuffers
0x000d00f0  Terrain_RebuildChunkVertexBufferAndNormals
0x000d12e0  Terrain_RebuildLandTilesRoadPatchesAndChunkCaches
0x000d2db0  Terrain_SetLandSampleWithTileBorderMirroring
0x000d3340  Terrain_ApplyStreetIntersectionHeightPatch
0x001305f0  TerrainNoiseHash
```
