# How Sub Rosa makes terrain and shapes the city: clean-room research

Target binary: `subrosa.exe.THIS_ONE` (x86-64 PE, language `x86:LE:64:default`, compiler `windows`)

Method: Ghidra MCP only: decompiling, reading memory, and following symbols. No source or header files were assumed. All addresses are virtual addresses in this binary, image base 0x00000000.

This is one of several independent reports. Some of its numbers (for example the radius offset 0.75 and the base 23.75) differ from the other reports and from [terrain_generation.md](terrain_generation.md), which has the later, checked findings.

## 1. Functions that make or shape terrain

| Function | Address | Role |
|---|---|---|
| `Terrain_GenerateIslandHeightfield` | 0x00056560 | Main: fills the 2049×2049 heightfield with noise and round masks |
| `Terrain_ApplyStreetIntersectionHeightPatch` | 0x000d3340 | City: overwrites the terrain under a street intersection with a flat bilinear patch |
| `Terrain_RebuildLandTilesRoadPatchesAndChunkCaches` | 0x000d12e0 | Last step: resamples the heightfield into 8×8 land-tile chunks, applies about 30 intersection patches, rebuilds vertex buffers |
| `Terrain_SetLandSampleWithTileBorderMirroring` | 0x000d2db0 | Writer: writes one height into the right land-tile chunk, copying writes on tile edges |
| `TerrainNoiseHash` | 0x001305f0 | Noise: a fixed integer hash used to make the noise |
| `World_LoadMapAndInitializeTerrain` | 0x00056980 | Runs everything: heightfield, street placement, intersection edges, district and city stamping, then the rebuild |
| `Terrain_RebuildChunkVertexBufferAndNormals` | 0x000d00f0 | Rebuilds one chunk's vertex buffer and normals from the heights |
| `Render_InitTerrainStreamingBuffers` | 0x000c2db0 | Sets up GL streaming buffers for drawing terrain (6 detail levels) |
| `MapConnector_PlaceStreetSegmentTilesAndSlopes` | 0x00020580 | Places street tiles and slope or fill cells along a street segment |
| `MapConnector_PlaceIntersectionPerimeterTiles` | 0x00020130 | Places the tiles around the edge of an intersection |
| `UpdateMapTopHeightAndClearOccupancyBit` | 0x0003fa40 | Updates the cached top height and clears rough occupancy bits for each tile |
| `Texture_GenerateProceduralNoise` | 0x00099730 | Makes a noise texture (for drawing, not the heightfield) |

## 2. How the heightfield is made

### 2.1 The grid

`Terrain_GenerateIslandHeightfield` @ 0x56560 loops over an 0x801 × 0x801 grid (2049 × 2049 samples):

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

The output array `g_terrainHeightfield` is at `DAT_2d482830`. The two 32-bit words just before it (`DAT_2d482820` and `DAT_2d482828`) are set to `0xC5800000` (-4096.0f) before the loop. They are guard values placed before the real data.

### 2.2 The noise: TerrainNoiseHash

`TerrainNoiseHash` @ 0x1305f0 gets the grid (x, z) as floats in XMM0/XMM1 and a global seed in R8D:

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

It returns `[0, 0x7FFFFFFF]` (0 to about 2.15×10⁹ as an integer). The caller scales it to [0, 1] by multiplying by a factor.

Each sample calls it three times, for three separate noise values with different scales.

### 2.3 The island formula (rebuilt from the decompile)

Each height at grid position `(x, z)` in `[0, 0x800]²` is worked out in five steps.

#### Step A: first round mask (the island's outline, centred at (640, 640))

```
d1 = sqrt((x - 0x280)² + (z - 0x280)²)     // distance from (640, 640)
t  = clamp(d1 * 0x3B800000 - 0x3F400000, 0.0, 0x40400000)
   = clamp(d1 * 0.00390625 - 0.75,        0.0, 3.0)
t  = t²                                       // squared envelope (range 0..9)
```

- When `d1 < 192`, t = 0 (flat middle).
- At the grid corner `(0, 0)`, d1 = √(640²+640²) ≈ 905, so t ≈ clamp(3.535-0.75, 0, 3)² = 9.0.

#### Step B: three noise values

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

With the decoded constants:

```
height_interior = noise₃ * 8.0  + 23.75
                + (noise₁ + t) * 16.0
                + (noise₂ * 32.0 + t * 8.0) * t
```

So the inner height moves around a base of about 23.75. The three noise values are scaled by 8.0, 16.0, and 32.0, and the squared mask `t` changes both noise₁ and the noise₂+t product.

#### Step C: second round mask (inner and outer blend, centred at (704, 704))

```
d2   = sqrt((x - 0x2c0)² + (z - 0x2c0)²)   // distance from (704, 704)
t2   = clamp((d2 - DAT_00278900) * DAT_00278148, 0.0, FLOAT_002783f4)
     = clamp((d2 - 320.0) * 0.015625,       0.0, 1.0)
```

- When `d2 < 320`, t2 = 0 (inner area, flat water).
- When `320 ≤ d2 ≤ 384`, t2 goes up in a straight line from 0 to 1.
- When `d2 ≥ 384`, t2 = 1 (outer area, full noisy terrain).

#### Step D: final blend

```
height = height_interior * t2 + (1.0 - t2) * _DAT_002787ac
       = height_interior * t2 + (1.0 - t2) * 23.875
```

So the area within about 320 units of (704, 704) is flat at 23.875 (water level). Past 384 units, the full noisy island appears, with a smooth 64-unit blend between the two.

#### Step E: edge shaping on maps that are not auto-generated

When `isMapAutoGenerated == 0`, two more shaping blocks overwrite some columns and rows.

Where `x < 0x300` (768) and `z ≥ 0x83` (131):
```c
fVar15 = (float)(z_index + -0x82) * FLOAT_00278280;  // (z - 130) * 0.25
fVar14 = (float)(x_alt  + 0x82)  * FLOAT_00278280;  // (top-x-inv + 130) * 0.25
// clamp both to [0, 1], take max of (1-val₁, 1-val₂):
fVar16 = 1 - max(clamp(fVar15, 0, 1), clamp(fVar14, 0, 1));
fVar16 = clamp(fVar16, 0, 1);

height = (fVar5 - fVar16 * fVar4) * fVar16 * fVar16 + DAT_0027874c;
       = (36.0 - fVar16 * 24.0) * fVar16² + 11.75
```

This gives a smooth shoreline along the bottom and right edges of the island.

### 2.4 Evidence: decompiled code

From `Terrain_GenerateIslandHeightfield` @ 0x56560, working out one sample:

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

## 3. The city overwrites the terrain

City terrain starts from the noisy base, and then street-intersection patches overwrite parts of it. The fixed list of patched intersections is below.

### 3.1 The order (from `World_LoadMapAndInitializeTerrain` @ 0x56980)

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
1. Resamples the global heightfield (`DAT_2d482830`) into 8×8 land-tile chunks (`g_landTileHeightChunks`) with bilinear blending, in steps of `FLOAT_00278224 = 0.125`.
2. On maps that are not auto-generated, calls `Terrain_ApplyStreetIntersectionHeightPatch` for about 30 specific intersections (a fixed list: 2, 0x28, 0x20, 0x2e, 0x2d, 0x2f, 0x1c, 10, 0x21, 0x2a, 0x23, 0xd, 0x3b, 0x1f, 0x34, 0x30, 0x32, 0x13, 0x16, 0x3a, 0x3e, 0x3d, 0x29, 0x24, 0x2b, 0x2c, 0x26, 0x27).
3. Rebuilds the vertex buffers of all 64 terrain chunks.

### 3.3 `Terrain_ApplyStreetIntersectionHeightPatch` @ 0xd3340: flattening the city

This function takes an intersection number `param_1` and:

1. Finds the 4 corners by walking the street graph:
   - south → south → west for the south-west corner;
   - west → west → south for the north-west corner.

2. Works out the corner heights:
   ```
   corner_height = intersection_z * DAT_00278688 + DAT_00278698
                 = intersection_z * 4.0 + 4.25
   ```
   Each corner's Z comes from its `streetIntersections[].unk0[2]` field.

3. Blends bilinearly across the intersection rectangle:
   ```
   for x from start_x to end_x:
       for z from start_z to end_z:
           height = (1-fx)*(1-fz)*h00 + fx*(1-fz)*h10 + (1-fx)*fz*h01 + fx*fz*h11
           Terrain_SetLandSampleWithTileBorderMirroring(z, x, height)
   ```

4. Smooths each tile on a 4×4 sub-grid: it splits the intersection into tiles, fills empty 4×4 sub-cells with per-building data, and blends by distance with radius `FLOAT_002781bc = 0.0625` to soften the transitions.

5. Does a final blend with the terrain around it: each sample is blended between the patch height and the original terrain with a smoothstep based on distance from the patch's edges.

### 3.4 Evidence that the city overwrites the terrain

From the decompile of `Terrain_ApplyStreetIntersectionHeightPatch` @ 0xd3340, the bilinear patch writes:

```c
Terrain_SetLandSampleWithTileBorderMirroring(
    uVar18, uVar22,
    (fVar4 - fVar39) * (fVar36 * fVar40 + fVar43 * (fVar4 - fVar40)) +
    (fVar38 * fVar40 + fVar44 * (fVar4 - fVar40)) * fVar39
);
```

Here `fVar43, fVar44, fVar36, fVar38` are the four corner heights, worked out as `z_coord * 4.0 + 4.25`. This writes straight over the terrain: in this first pass nothing is read from or blended with the noisy terrain. Blending only happens later, in the neighbour smoothing pass.

## 4. Decoded constants

### 4.1 Heightfield constants

| Address | Raw bytes (LE) | Hex as float | Value | Meaning |
|---|---|---|---|---|
| 0x002780cc | `00 00 80 3B` | `0x3B800000` | 0.00390625 (1/256) | First round mask distance scale |
| 0x002783a0 | `00 00 40 3F` | `0x3F400000` | 0.75 | First round mask distance offset |
| 0x002783f4 | `00 00 80 3F` | `0x3F800000` | 1.0 | General clamp maximum and blend weight |
| 0x00278644 | `00 00 40 40` | `0x40400000` | 3.0 | Upper limit for the first mask t (before squaring) |
| 0x00278728 | `00 00 00 41` | `0x41000000` | 8.0 | Noise₁ scale for the t-weighted term |
| 0x002787d4 | `00 00 00 42` | `0x42000000` | 32.0 | Noise₂ scale |
| 0x00278774 | `00 00 80 41` | `0x41800000` | 16.0 | Scale of (noise₁ + t) |
| 0x002787a8 | `00 00 BE 41` | `0x41BE0000` | 23.75 | Base terrain height (inner constant) |
| 0x002787ac | `00 00 BF 41` | `0x41BF0000` | 23.875 | Outer blend target (water level) |
| 0x00278900 | `00 00 A0 43` | `0x43A00000` | 320.0 | Second round mask distance offset |
| 0x00278148 | `00 00 80 3C` | `0x3C800000` | 0.015625 (1/64) | Second round mask distance scale |
| 0x00278280 | `00 00 80 3E` | `0x3E800000` | 0.25 | Edge shaping distance scale |
| 0x0027874c | `00 00 3C 41` | `0x413C0000` | 11.75 | Base height for edge shaping |
| 0x002787e0 | `00 00 10 42` | `0x42100000` | 36.0 | Edge shaping peak (fVar5) |
| 0x002787b0 | `00 00 C0 41` | `0x41C00000` | 24.0 | Edge shaping blend (fVar4) |

### 4.2 City, street, and tile constants

| Address | Raw bytes (LE) | Hex as float | Value | Meaning |
|---|---|---|---|---|
| 0x00278688 | `00 00 80 40` | `0x40800000` | 4.0 | Intersection Z to height multiplier |
| 0x00278698 | `00 00 88 40` | `0x40880000` | 4.25 | Intersection height offset |
| 0x002781bc | `00 00 80 3D` | `0x3D800000` | 0.0625 (1/16) | Intersection patch smoothing radius |
| 0x00278224 | `00 00 00 3E` | `0x3E000000` | 0.125 (1/8) | Bilinear resampling step (land-tile rebuild) |
| 0x002787a4 | `00 00 B8 41` | `0x41B80000` | 23.0 | Used when setting up the world: `DAT_473de440 = DAT_002787a4` |
| 0x00278a68 | `00 00 B9 44` | `0x44B90000` | 1480.0 | World Y offset added to intersection positions |
| 0x002785a0 | `EA 2E 44 54 FB 21 F9 3F` | `0x3FF921FB54442EEA` | about 1.5707963 | Double, probably π/2 (a building rotation step) |

### 4.3 Nearby constants (context for floats next to the ones above)

| Address | Hex | Value | Possible meaning |
|---|---|---|---|
| 0x0027872c | `0x41100000` | 9.0 | Noise scale for another layer |
| 0x00278730 | `0x41200000` | 10.0 | Noise scale for another layer |
| 0x00278734 | `0x41278D36` | about 10.4746 | Noise scale for another layer |
| 0x002787d8 | `0x42040000` | 33.0 | Noise scale for another layer |
| 0x002787dc | `0x420C0000` | 35.0 | Noise scale for another layer |
| 0x0027e7e0 | `0x42100000` | 36.0 | (same as 0x002787e0, the edge shaping peak) |
| 0x00278904 | `0x43A20000` | 324.0 | Maybe the outer edge of the second round mask |

### 4.4 Guard value

| Address | Raw bytes | Hex | Value |
|---|---|---|---|
| DAT_2d482820 | `00 00 80 C5` | `0xC5800000` | -4096.0, a guard value before the heightfield data |

## 5. Coordinates and grid mapping

### 5.1 The heightfield grid

- Size: 0x801 × 0x801 = 2049 × 2049 samples
- One `float` per sample (about 16.8 MB)
- Output array: `g_terrainHeightfield` at `DAT_2d482830`
- Grid indices: `(x, z)` in `[0, 0x800]²`
- Centre of the island mask: (0x280, 0x280) = (640, 640), a little off the grid's middle (1024, 1024)
- Centre of the flat inner area: (0x2c0, 0x2c0) = (704, 704)

### 5.2 Land-tile chunks

`Terrain_SetLandSampleWithTileBorderMirroring` @ 0xd2db0 shows how tiles are laid out:

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

- 8 × 8 = 64 land tiles.
- Each tile is 0x801 × 0x801 = 2049 × 2049 samples.
- The step between tiles is 0x684 * 2049, which depends on `sizeof(landTileStruct)`: 0x684 entries, each a pointer or buffer.
- Copying at tile edges: when a write lands on a tile edge (x=0 or z=0), the value is also copied into the neighbouring tile's edge.

### 5.3 World coordinates

From `World_LoadMapAndInitializeTerrain` @ 0x56980, street intersection positions are:

```c
streetIntersections[idx].pos.y = (float)row * DAT_00278688;    // row * 4.0
streetIntersections[idx].pos.z = (float)col * DAT_00278688;    // col * 4.0
```

And the intersection height is:
```c
corner_height = intersection_z * 4.0 + 4.25;
```

So the world Y (height) at a grid position is the heightfield value itself. Terrain Y values run from about 11.75 to 30 or more world units.

### 5.4 Street grid and terrain grid

`UpdateMapTopHeightAndClearOccupancyBit` uses bit shifts that match the tile layout:
- `<< 11` multiplies by 2048 (the tile size in samples).
- `g_landUseMaskBits` tracks rough 4×4 occupancy for each (param_1+0x400, param_2+0x400) position.

## 6. Smoothing, blending, and flattening

### 6.1 Blending the intersection patch (in layers)

`Terrain_ApplyStreetIntersectionHeightPatch` @ 0xd3340 smooths in three layers.

Layer 1, filling a 4×4 sub-grid per tile: the intersection rectangle is split into a 4×4 grid per tile. Each empty cell looks up building footprint data in the `DAT_04afa9a0` array and is filled with values blended bilinearly from `DAT_3ce6f5f8` (the top-height caches for the 4 corners of each building slot).

Layer 2, neighbour-weighted smoothing: after filling, a pass over the neighbours uses the factor `FLOAT_002781bc = 0.0625`:
```c
fVar41 = sqrt(dx² + dz²);       // distance from sample to patch cell
fVar38 = 1.0 - fVar41 * 0.0625; // falloff weight
if (fVar38 <= 0.0) continue;    // outside radius → skip
```

For each neighbour cell's entries, that weight sets how much it adds, based on distance. The smoothing runs 0x10 times (16 passes), so the effect builds up.

Layer 3, the final blend with the base terrain: the last part uses a smoothstep-like curve with `DAT_00278644 = 3.0`:
```c
fVar38 = (3.0 - (2.0 * fVar38)) * fVar38 * fVar38;  // smoothstep variant
height = (1.0 - fVar38) * original_terrain + fVar38 * patch_height;
```

### 6.2 Shaping the island's edges

On maps that are not auto-generated, the right and bottom edges of the grid use a cubic falloff:
```
height = (36.0 - 24.0 * clamp_edge) * clamp_edge² + 11.75
```

`clamp_edge` goes from 0 (shallow water) to 1 (full terrain), giving a smooth change from water to land.

### 6.3 Copying across tile edges

`Terrain_SetLandSampleWithTileBorderMirroring` copies any write that lands on a tile edge (x=0 or z=0 inside a tile) into the first column or row of the neighbouring tile, so neighbouring tiles share their edge samples.

## 7. How sure the findings are

| Claim | Confidence | Notes |
|---|---|---|
| The grid is 2049×2049 (0x801×0x801) | High | The loop limits are written out, and every terrain function uses the same ones |
| The island is made in code (noise hash plus round falloff) | High | Three TerrainNoiseHash calls per sample, and no file reading in the loop |
| Two round masks with different centres | High | Two SQRT calls with different centres and scales |
| The city overwrites the base terrain | High | 28 intersection patches written out; street graph positions set the heights |
| The noise hash is an integer xorshift-like hash | High | The arithmetic is plain: multiply, xor, square, add |
| All float constants are decoded right | High | Raw bytes read and checked as IEEE-754 |
| 8×8 tile chunks with 0x7ff indexing inside a tile | High | The bit shifts and array indexing are written out |
| Smoothing radius 0.0625 | High | The constant and formula are clear |
| The exact noise frequency or wavelength | Medium | The scale factors are known (8, 16, 32), but how they relate to world units is not fully worked out |
| Whether the noise is Perlin, value, or gradient noise | Low | The hash returns a raw 31-bit value that the caller treats as a uniform [0,1] value, so it is value noise, not gradient noise |
| What the 4-float arrays at 0x278728/0x2787d4 are for | Low | They look like scale tables for several noise layers, but only one entry is ever read; they may be settings for a loop the compiler unrolled |
| How grid samples relate to world metres | Medium | Street positions use a ×4.0 scale and terrain Y is the float itself; the grid spacing in metres is not confirmed, but one sample is probably some fraction of a world unit |

### What could not be found

- The exact noise seed. It is passed in the R8D register at each call, and Ghidra's decompiler loses track of register arguments across the three calls in a row.
- The map file format. `World_LoadMapAndInitializeTerrain` reads a CSX archive, but the full map data layout was not worked out.
- The exact number of world units per sample. The island maths suggests about 640 samples from the centre to the shore; the mapping to world units depends on how the camera and scene scale are set, which was out of scope.
- How buildings affect the terrain. Buildings are stamped after the terrain is made, through `InstantiateSbbVolumeIntoWorldAndMetadata`, but their effect on the heightfield (beyond room-level changes) was not fully traced.

## 8. Appendix: functions sorted by address

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
