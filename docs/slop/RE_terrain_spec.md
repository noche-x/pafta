# Early city terrain analysis and porting proposal

This proposal was written before the terrain patches were implemented. For the later findings and how Pafta does it now, see [terrain_generation.md](terrain_generation.md). The notes and steps below are the earlier analysis, not a list of things wrong with Pafta today.

## How terrain worked when this was written

Back then, Pafta built city terrain by sampling `native_like_city_terrain_height()`, a port of the base island generator (`Terrain_GenerateIslandHeightfield @56560`). That left out the later city patches that use the heights stored on intersections.

The game starts with a base land heightfield, but the city's real shape comes later, from `Terrain_ApplyStreetIntersectionHeightPatch @d3340`. That function overwrites city heightfield samples with the stored intersection heights, fills each rectangle between four intersections with a bilinear blend, and then softens the edge of the patch into the land around it. The city ground follows its streets and intersections; it is not noisy procedural hills.

The proposal was to build a cached terrain heightfield from the SBC intersection graph, using the same heights the road tiles use.

## Evidence checked in this pass

- `/tmp/re_terrain_patch.c` is a 54-line summary rebuilt from the decompile, not a full Ghidra dump. It has the key constants, the rectangle math, the bilinear fill loop, and rough notes on the smoothing pass of `Terrain_ApplyStreetIntersectionHeightPatch @d3340`.
- `src/scene.cpp` samples `native_like_city_terrain_height(world_x, world_z)` inside `build_city_terrain()`.
- `src/scene.cpp` already works out road tile `authored_y` from intersection levels by blending between the two ends of each street. That is the right way round: roads carry the stored height, and the terrain should follow them.
- `tests/parser_tests.cpp` has a test called `city_terrain_is_non_flat_noise_mesh_padded_to_city_bounds()`. It is built on the old, wrong idea of what the terrain should be, and has to be replaced as part of Terrain v2.

## The idea in plain words

The game builds city terrain like this:

1. Make or load a base island heightfield.
2. In the city, that base is basically flat, near water level. Road slopes, raised areas, and hilly neighbourhoods do not come from it.
3. For each block closed in by four street intersections, stamp a terrain patch:
   - find the rectangle between the four intersection footprints;
   - turn each corner's grid-Y level into a world height with `level * 4.0 + 4.25`;
   - fill every heightfield sample in the rectangle with a bilinear blend of the corners;
   - write each sample with `Terrain_SetLandSampleWithTileBorderMirroring()`.
4. Run a softening pass so the road surface and the inside of the block ease into the nearby land instead of ending in a cliff.
5. The final terrain is the base heightfield plus these patches.

In practice, the terrain is a heightfield built from the city's layout. The island noise can stay as a background outside the patched area, but it must not be what shapes the city.

## Coordinates and heights

Decoded constants:

```text
native heightfield sample index = grid_coord * 4 + 0x1000
                               = grid_coord * 4 + 4096

pafta grid cell size       = 4.0 world units
native samples per grid cell    = 4
native sample spacing           = 1.0 world unit

world terrain Y from level h    = h * 4.0 + 4.25
DAT_00278688                    = 4.0
DAT_00278698                    = 4.25
```

What this means for the port:

- `SbcIntersection.y` is the stored level. A patch corner's height is not just `intersection.y * unit_size`; it is `intersection.y * 4.0 + 4.25`.
- `CityTileCell::base_y` is where a solid cell starts. In the current mesh code, the top of a flat road tile is drawn at `base_y + unit_size`. When checking that terrain and roads line up, compare against where the road top is drawn, not against `base_y`.
- The game's patch works at 4x finer resolution than the map tile grid. `pafta` does not need a 1-unit terrain mesh everywhere, but its cached heightfield (or whatever works out heights) must reproduce the flat areas and ramps where roads touch the ground.

## `Terrain_ApplyStreetIntersectionHeightPatch @d3340`

### Input and the block check

The function takes one intersection index, `param_1`. It only does anything when it can follow streets around a closed block of four intersections:

```text
A = streetIntersections[param_1]
B = A.streetSouth.intersectionB
C = A.streetWest.intersectionB
D = C.streetSouth.intersectionB
```

The decompile summary calls these locals:

```text
A = param_1
B = lVar28
C = lVar14
D = lVar15
```

If the south, west, south chain is broken, the patch is skipped. So this function stamps whole four-cornered blocks, never a single intersection on its own.

### The rectangle

The patch rectangle is worked out in heightfield sample coordinates:

```c
gx0 = (A.unk0[1] + A.field_0x2c) * 4 + 0x1000;
gx1 = (B.unk0[1] - B.field_0x2c) * 4 + 0x1000;
gz0 = (A.field_0x30 + (int)A.pos.x) * 4 + 0x1000;
gz1 = ((int)C.pos.x - C.field_0x30) * 4 + 0x1000;

width_samples = gx1 - gx0;
depth_samples = gz1 - gz0;
```

These are still the decompiler's field names. Don't rename them until the next Ghidra pass confirms the struct layout. What they do is clear enough:

- `field_0x2c` and `field_0x30` are footprint offsets that pull the patch rectangle in from each intersection's centre.
- The fill loops include both ends: `gx0..gx1` and `gz0..gz1`.
- The bounds are in the game's terrain sample grid, not in `pafta` terrain mesh vertex indices.

### Corner heights

The four corner heights are:

```c
hA = A.unk0[2] * 4.0f + 4.25f;
hB = B.unk0[2] * 4.0f + 4.25f;
hC = C.unk0[2] * 4.0f + 4.25f;
hD = D.unk0[2] * 4.0f + 4.25f;
```

In the current SBC parser this is the intersection's Y level, `SbcIntersection.y`, once the field layout is confirmed.

### Bilinear fill

The game's loop:

```c
tx_step = 1.0f / (float)(gx1 - gx0);
tz_step = 1.0f / (float)(gz1 - gz0);

tz = 0.0f;
for (gz = gz0; gz <= gz1; ++gz) {
    tx = 0.0f;
    for (gx = gx0; gx <= gx1; ++gx) {
        h_z0 = hA * (1.0f - tx) + hB * tx;
        h_z1 = hC * (1.0f - tx) + hD * tx;
        h    = h_z0 * (1.0f - tz) + h_z1 * tz;

        Terrain_SetLandSampleWithTileBorderMirroring(gx, gz, h);
        tx += tx_step;
    }
    tz += tz_step;
}
```

This is the same as the decompiled expression:

```c
h = (1.0f - tx) * (hC * tz + hA * (1.0f - tz))
  + (hD * tz + hB * (1.0f - tz)) * tx;
```

This is what gives the city its shape. If the four intersection levels differ, the ground inside the block becomes a bilinear slope that meets each corner. If all four are the same, the block is a flat area at `level * 4.0 + 4.25`.

### Softening the edges

The decompile summary we were given shows:

- Scratch buffer: `DAT_04afa9a0`.
- Scratch records are `0x0c` bytes apart, described as `{count, x, z, weight}` in the summary.
- The stamp covers 5 by 5 cells.
- Layout notes: `0x1c` bytes per cell and `0x1c00` per row, which suggests a scratch grid `0x100` wide.
- The rectangle is walked 4 samples at a time (a `>> 2` relationship), one map grid cell per coarse smoothing step.
- Corner validity tables around `DAT_3ce6f5f8` decide whether a contribution is allowed.
- There are several outer passes: `local_120 = 0x10`, so 16 passes.
- Falloff: `weight = 1.0 - distance * FLOAT_002781bc`, and weights of zero or below are thrown out.
- Results are written back with `Terrain_SetLandSampleWithTileBorderMirroring()`.

Pseudocode for the port that sticks to what is known:

```c
for (pass = 0; pass < 16; ++pass) {
    clear_scratch_accumulators();

    for each coarse sample on/near the stamped rectangle, stepping 4 native samples {
        source_height = current_heightfield[source_gx, source_gz];

        for dz = -2..2 {
            for dx = -2..2 {
                if (!native_corner_gate_allows(dx, dz, source_gx, source_gz)) {
                    continue;
                }

                distance = native_distance_for_stamp(dx, dz, source_gx, source_gz);
                weight = 1.0f - distance * FLOAT_002781bc;
                if (weight <= 0.0f) {
                    continue;
                }

                accumulate scratch[target_gx, target_gz] with source_height and weight;
            }
        }
    }

    for each scratch cell with contributions {
        blended = normalize_weighted_height(scratch_cell);
        Terrain_SetLandSampleWithTileBorderMirroring(target_gx, target_gz, blended);
    }
}
```

How far it spreads, and how sure we are:

- The kernel is proven to be 5 by 5 coarse cells, so radius 2 on the 4-sample grid.
- The function runs 16 outer passes.
- If each pass builds on the heights written by the pass before, the furthest it could spread is `16 * 2 = 32` coarse cells, which is 32 map cells or 128 samples.
- That 32-cell figure is a guess from the summary, not a proven radius. A direct Ghidra pass needs to show whether each pass spreads from the new values or keeps stamping from the same starting set.

### Final blend with the land

The last pass reads the existing land heights:

```c
chunk = g_landTileHeightChunks[chunkX + chunkY * 8];
land_height = chunk[(z & 0x7ff) * 0x801 + (x & 0x7ff)];
```

It blends with a smoothstep-like curve:

```c
f = (DAT_00278644 - 2.0f * t) * t * t;  // DAT_00278644 = 3.0
out = (1.0f - f) * land_height + f * patch_height;
```

With the decoded constant, that is:

```c
f = (3.0f - 2.0f * t) * t * t;
```

This is the usual `t*t*(3 - 2*t)` easing curve. What is still unknown is how the game works out `t` for each edge sample, and over what distance.

## What the earlier port got wrong

The earlier `build_city_terrain()` did this:

```c++
height = native_like_city_terrain_height(world_x, world_z);
```

So the mesh only followed the island generator. That generator may be fine for background land, but it knows nothing about:

- intersection heights;
- street widths;
- how far footprints pull the patch in;
- blocks with four corners;
- road slopes;
- ramps in the city;
- the soft edge between roads and the land around them.

The road tile code was already closer to the game than the terrain code. Road tiles use intersection Y levels and blend between the ends of each street. Terrain v2 should make the ground match that city surface, instead of asking the roads to sit on top of unrelated island noise.

## The original porting proposal

The task produced this proposal; nothing was implemented.

### 1. Keep the base heightfield, but only in the background

In `src/scene.cpp`, keep `native_like_city_terrain_height()` only as:

- an optional base island outside the city patches; or
- a flat value of `23.875f` across the city area.

In Terrain v2 the base is what the patches sit on, not the city's shape. A practical first version would fill the terrain grid with `23.875f` across the cached city bounds and then stamp the patches. Once that works, bring the island back as an outer background if screenshots show it matters near the city's edges.

### 2. Build a working height grid

Change `build_city_terrain(const SbcMap& map, float unit_size)` so it makes a height grid on the CPU before making mesh vertices.

Suggested first version:

- Coordinates: map grid or world coordinates, lined up with the existing `CityTerrain` bounds.
- Values: world Y.
- Mesh spacing: `unit_size` (`4.0`) at first, so it is right where roads meet the grid, instead of the current `unit_size * 4.0` (`16.0`). The terrain mesh is cached by `city_scene_fingerprint()`, so the extra cost is fine.
- Later, optionally: the game's sample spacing (`1.0`), if comparisons show that 4.0 spacing loses curb or edge detail.

The working grid can store heights at mesh vertices. For the game's exact behaviour, add conversion helpers:

```c++
native_sample_x = grid_x * 4 + 4096;
native_sample_z = grid_z * 4 + 4096;
world_x = grid_x * unit_size;
world_z = grid_z * unit_size;
```

If the mesh grid is coarser than the game's samples, work out the patch formula directly at each mesh vertex instead of replaying every sample write the game does.

### 3. Find the four-cornered blocks from the city layout

Add a helper next to the existing city tile helpers, without a new layer of architecture:

```text
build_city_intersection_patches(map)
```

It should:

- find closed blocks of four intersections, the same as `A -> south B`, `A -> west C`, `C -> south D`;
- work out how far footprints pull the patch in, matching `field_0x2c` and `field_0x30`;
- output patch records with the four corner intersections, the rectangle bounds (both ends included), and four world heights.

Use the existing `SbcStreet` end data to work out which way each neighbour lies. The direction names must be checked against the game:

- the current code has `side_from_intersection_delta()` and `CityIntersectionSide`;
- the decompile uses `streetSouth` and `streetWest`;
- don't assume the current side names match the game's names until the lead's Ghidra pass confirms the struct layout.

### 4. Stamp the bilinear patches

For each patch:

1. Turn each corner level into a world terrain Y:

```c++
height = static_cast<float>(intersection.y) * 4.0f + 4.25f;
```

2. For each terrain sample inside the rectangle, work out `tx/tz` from 0 to 1.
3. Write the bilinear height into the working grid.

Inside the patch, this must replace the base height, not average with the noise. The game calls `Terrain_SetLandSampleWithTileBorderMirroring()` while filling, so the patch fully owns those samples.

### 5. Soften the edges

Write a careful first version that follows the known shape, and keep the unknown constants in one place:

- Use a radius-2, 5-by-5 kernel on the coarse grid.
- Run 16 passes.
- Use `weight = max(0, 1 - distance * feather_falloff)`.
- Use the smoothstep blend `t*t*(3 - 2*t)` for the final merge where the distance to the patch edge is known.

Put the falloff and radius constants in one small helper or local settings struct. Don't scatter guesses across the code. The next Ghidra pass should be able to swap the values without a refactor.

If the game's exact softening is still unknown when this is built, ship the first version with:

- exact rectangle bounds and bilinear fill;
- a rough softening, with clear comments, and tests that only check the general behaviour.

Don't claim it matches the game's smoothing until `FLOAT_002781bc`, the validity tables, and how the edge distance `t` is worked out are all confirmed.

### 6. Make road and terrain heights agree

Code that depends on this:

- `build_city_tile_cells()` already works out `CityTileCell::base_y` and slopes from intersection heights.
- `build_city_tile_render_buffer()` draws road and curb tops at `base_y + unit_size`, plus the slope.
- `build_city_traffic_light_poles()`, `scene.roads`, and `scene.intersections` call `terrain_height_at()`.

After Terrain v2:

- `terrain_height_at()` at an intersection's centre should return the patch height for its level, using `level * 4.0 + 4.25`.
- Tests of road placement must compare against where the road top is actually drawn. If road tiles need a `+0.25` shift to sit on the game's terrain, make that a separate height fix backed by screenshots or game evidence.
- Roads should not float above unrelated noise or sink below the ground. Road slopes and terrain slopes should both come from the same intersection levels.

### 7. Update the tests

Expected test changes:

- Replace `city_terrain_is_non_flat_noise_mesh_padded_to_city_bounds()` with Terrain v2 tests.
- Keep `terrain_noise_hash_matches_native_all_ones_mask()` if the island helper stays.
- Add a made-up four-corner block test:
  - four intersections forming a rectangle;
  - different Y levels at the corners;
  - terrain at the four patch corners equals `level * 4.0 + 4.25`;
  - terrain at the centre equals the bilinear average;
  - the inside of the patch does not come from `native_like_city_terrain_height()`.
- Add a flat city test:
  - with no valid four-corner block, city terrain is `23.875` (or the optional base);
  - no procedural hills in the middle of the city.
- Add a road alignment test:
  - compare `terrain_height_at()` with where the road or intersection top is drawn, not just `CityTileCell::base_y`.
- Add a softening test:
  - heights outside the patch move towards the base land;
  - the difference between neighbouring heights stays small;
  - don't check exact weights until the lead's Ghidra pass settles the unknowns.

### 8. Caching and speed

The scene is already cached by `city_scene_fingerprint()`, which hashes intersections, streets, widths, and buildings. Terrain v2 should fit into that:

- Build the height grid once per fingerprint.
- Make the mesh vertices and triangles from that grid.
- Upload the terrain to the GPU the existing way.

Even a mesh grid at `4.0` world units is cheap next to the building and render-buffer work. A grid at the game's `1.0` units may still be fine for normal city sizes, but treat that as a quality option after the first correct version.

## What still needs a Ghidra pass from the lead

These block matching the game exactly:

1. What `streetIntersection.field_0x2c` and `field_0x30` really mean.
   - Are they half-widths, footprint margins, road widths per side, or already-scaled inset distances?
   - How do they relate to `SbcStreet.left_width` / `right_width` and `city_intersection_footprint()`?

2. Which direction is which in the game.
   - Confirm which axis (`SbcIntersection.x` or `.z`) is the decompile's `unk0[1]` and which is `pos.x`.
   - Confirm what the game's `streetSouth` and `streetWest` mean compared with `CityIntersectionSide`.

3. How softening is averaged.
   - Confirm the scratch record layout at `DAT_04afa9a0`.
   - Confirm whether `{count, x, z, weight}` in the summary is the real layout or just a description.
   - Confirm whether the result is divided by the total weight, the count, or something else.

4. `FLOAT_002781bc`.
   - This is the falloff multiplier in `weight = 1 - distance * falloff`.
   - Without it, the softening's reach and shape are not fixed.

5. What the tables around `DAT_3ce6f5f8` do.
   - We need to know how the corner validity masks stop smoothing across bad corners, missing roads, or block edges.

6. The final edge blend `t`.
   - `DAT_00278644 = 3.0` gives the smoothstep curve, but where `t` comes from and its range still need confirming.
   - Confirm the distance range, and whether `t=0` is the land edge or the patch edge in every case.

7. Call order.
   - Confirm when `Terrain_ApplyStreetIntersectionHeightPatch()` runs compared with `Terrain_GenerateIslandHeightfield()`, setting up land-tile chunks, roads, buildings, and placing objects.

## Unknowns

- The available `/tmp/re_terrain_patch.c` is not enough to work out exact softening weights. It only gives the kernel shape, the number of passes, hints about the scratch buffer, and the form of the falloff.
- I cannot prove the softening reaches exactly 32 map cells. That is the most a radius-2 kernel could reach over 16 passes that build on each other, not a confirmed distance.
- I cannot prove that the odd/even anchoring in `city_intersection_footprint()` matches the game's `field_0x2c` / `field_0x30`. The code already has a `VERIFY` comment on that footprint logic, and Terrain v2 depends on settling it.
- I cannot prove whether the terrain mesh should keep its `height - 0.05f` render offset. The game's sample height is `level * 4.0 + 4.25`. How to stop roads and ground flickering against each other should be checked on its own.
- I cannot say the island noise is useless everywhere. All I can say is that the city's slopes and flat areas do not come from it. Keep it as a background until the call order and screenshots say otherwise.

## Decision record

Suggested order for Terrain v2:

1. Replace the noise-driven city mesh with one driven by the patches.
2. First ship the exact rectangle and bilinear behaviour from the research.
3. Add a careful, rough softening, with its constants kept in one place.
4. Do a follow-up Ghidra pass to fix the softening constants and footprint fields.
5. Change the tests from "the ground is noisy" to "the ground follows the intersections".

This removes the wrong dependency now, keeps the island generator as a helper, and keeps the remaining unknowns in a small part of the code.

## Lead's Ghidra follow-up (2026-06-05)

The lead answered several of the "still needs a Ghidra pass" items by reading subrosa.exe directly. This later pass corrected the placeholders above as follows.

### StreetIntersection struct (layout confirmed, size 136)
```
off 0:  int unk0[3]   -> unk0[1] = grid X base, unk0[2] = height LEVEL (int)
off 12: Vector pos    -> pos.x (float-as-int) = grid Z base
off 24: int streetEast    (== the "field_0x28" East neighbor; -1 = none)
off 28: int streetSouth
off 32: int streetWest
off 36: int streetNorth
off 44: int field_0x2c -> +X (east)  footprint half-extent
off 48: int field_0x30 -> +Z (south) footprint half-extent
off 52: int field_0x34 -> -X (west)  footprint half-extent
off 56: int field_0x38 -> -Z (north) footprint half-extent
```

This answers item 1 (footprint fields) and item 2 (axes): unk0[1] is X and pos.x is Z. `StreetIntersection_Process @3760` confirms it, because it works out the footprint edges as:

```text
east  edge = unk0[1] + field_0x2c
west  edge = unk0[1] - field_0x34
south edge = pos.x   + field_0x30
north edge = pos.x   - field_0x38
```

So the four fields are separate reaches for each side (E/W/S/N), not one half-width.

### Softening constants (answers items 4 and 6)

- FLOAT_002781bc = 0.0625 (= 1/16), so the softening weight is 1 - dist*0.0625 and reaches 0 at 16 samples.
- DAT_00278644 = 3.0, so the final smoothstep edge blend is f = (3 - 2t)*t*t.
- FLOAT_00278280 = 0.25, the sub-step of the 5x5 softening stamp (a quarter of a sample).

Together with the 16 outer passes and the radius-2 5x5 kernel, the softening reaches about 16 samples, as set by the falloff. That replaces the 32-cell upper limit guessed above: the 1/16 falloff stops a single stamp at 16 samples (4 map cells). Several passes widen it, but each stamp has that limit.

### Heights and coordinates (already confirmed)

world Y = level*4.0 + 4.25; heightfield index = grid*4 + 4096; a map cell is 4.0 units.

### Still open (lower priority, does not block a first correct version)

- What the table at DAT_3ce6f5f8 does (corner validity masks). It only affects edge cases between blocks; a first version can soften evenly and improve later.
- Exactly how the scratch buffer averages weights (by total weight or by count). It affects how smooth the softening is, not whether the flat areas are right.
- Call order against the island generator. For pafta it doesn't matter, since we build the terrain from scratch.

### Lead's guidance for Terrain v2

The bilinear patch and the per-side footprint rectangles are now FULLY KNOWN and should be implemented exactly. The softening is careful but backed by evidence (the constants are known). pafta's SbcIntersection.x/.y/.z are grid X, height level, and grid Z. The four per-side reaches are not read from the SBC yet, so use the existing city_intersection_footprint() reaches in their place (it already works out a footprint from street widths) until the SBC provides them, if ever. The terrain must agree with the road's authored_y (level*4+4.25) so roads sit in the ground.
