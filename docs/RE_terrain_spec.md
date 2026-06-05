# Sub Rosa City Terrain RE Spec

## Executive summary

The current `map_editor_cpp` city terrain model is strategically wrong. It builds the city terrain mesh by sampling `native_like_city_terrain_height()`, which is a port of the base island heightfield generator (`Terrain_GenerateIslandHeightfield @56560`). That function is not the source of the city's authored ups and downs.

The game starts from a base land field, but the actual city shape is produced later by `Terrain_ApplyStreetIntersectionHeightPatch @d3340`. That function overwrites city heightfield samples with authored intersection heights, bilinearly fills each four-intersection rectangle, then feathers the stamped patch into the surrounding land. The city is an authored street/intersection heightfield, not procedural noisy hills.

Port direction: `build_city_terrain()` should stop treating noise as the city shape. It should build a cached terrain field from the SBC intersection graph and the same authored height contract already used by road tiles.

## Evidence checked in this pass

- `/tmp/re_terrain_patch.c` is a 54-line reconstructed decompile summary, not a full raw Ghidra dump in this workspace. It contains the key constants, rectangle math, bilinear fill loop, and high-level smoothing pass notes for `Terrain_ApplyStreetIntersectionHeightPatch @d3340`.
- `src/scene.cpp` currently samples `native_like_city_terrain_height(world_x, world_z)` inside `build_city_terrain()`.
- `src/scene.cpp` already computes road tile `authored_y` from intersection levels and street endpoint interpolation. This is the right ownership model: roads carry authored height; terrain should conform.
- `tests/parser_tests.cpp` currently has a test named `city_terrain_is_non_flat_noise_mesh_padded_to_city_bounds()`. That test encodes the old, wrong product assumption and will need replacement during Terrain v2.

## Plain-language model

The city terrain pipeline is:

1. Generate or load a base island land heightfield.
2. In the city region, that base is effectively water-level/flat for the relevant native coordinate band. The base field is not where road grades, plateaus, and neighborhood slopes come from.
3. For each valid four-corner street-intersection block, stamp an authored terrain patch:
   - determine the rectangle between the four intersection footprints;
   - convert each corner's authored grid-Y level to world height with `level * 4.0 + 4.25`;
   - bilinearly fill every native heightfield sample in that rectangle;
   - write samples through `Terrain_SetLandSampleWithTileBorderMirroring()`.
4. Run a feather/smooth stage so the road plateau and block interior ease into nearby land instead of forming a vertical step.
5. Final terrain is the base field plus these authored intersection patches.

Operationally: the terrain is a heightfield built from city topology. The island noise can remain as a base/fallback outside the stamped region, but it must not be the primary city terrain signal.

## Coordinate and height contracts

Known decoded constants:

```text
native heightfield sample index = grid_coord * 4 + 0x1000
                               = grid_coord * 4 + 4096

map_editor grid cell size       = 4.0 world units
native samples per grid cell    = 4
native sample spacing           = 1.0 world unit

world terrain Y from level h    = h * 4.0 + 4.25
DAT_00278688                    = 4.0
DAT_00278698                    = 4.25
```

Implications for the port:

- `SbcIntersection.y` is the authored level. Terrain patch corner height is not just `intersection.y * unit_size`; it is `intersection.y * 4.0 + 4.25`.
- Existing `CityTileCell::base_y` is a solid-cell base convention. The rendered top of a flat road tile is `base_y + unit_size` in the current mesh path. When validating terrain/road alignment, compare against the rendered road surface convention, not blindly against `base_y`.
- The native patch operates at 4x finer resolution than the map tile grid. `map_editor_cpp` does not need to upload a 1-world-unit terrain mesh everywhere, but its cached CPU heightfield or evaluator must be able to reproduce the authored plateaus and ramps at road/contact points.

## `Terrain_ApplyStreetIntersectionHeightPatch @d3340`

### Inputs and topology guard

The function takes one street-intersection index, `param_1`. It only applies when a closed four-intersection block can be reached through street chains:

```text
A = streetIntersections[param_1]
B = A.streetSouth.intersectionB
C = A.streetWest.intersectionB
D = C.streetSouth.intersectionB
```

The decompile summary names those locals as:

```text
A = param_1
B = lVar28
C = lVar14
D = lVar15
```

If that south/west/south chain is missing, the patch is skipped. This is important: this function stamps four-corner city blocks, not isolated single intersections.

### Rectangle bounds

The patch rectangle is computed in native heightfield sample index space:

```c
gx0 = (A.unk0[1] + A.field_0x2c) * 4 + 0x1000;
gx1 = (B.unk0[1] - B.field_0x2c) * 4 + 0x1000;
gz0 = (A.field_0x30 + (int)A.pos.x) * 4 + 0x1000;
gz1 = ((int)C.pos.x - C.field_0x30) * 4 + 0x1000;

width_samples = gx1 - gx0;
depth_samples = gz1 - gz0;
```

The field names are still decompiler names. Do not over-normalize them until the next Ghidra pass confirms native struct layout. The actionable behavior is clear:

- `field_0x2c` and `field_0x30` are footprint/half-width offsets used to inset the patch rectangle away from each intersection center.
- Bounds are inclusive in the fill loops: `gx0..gx1` and `gz0..gz1`.
- Bounds are in the native terrain sample grid, not directly in `map_editor_cpp` terrain mesh vertex indices.

### Corner heights

The four authored corner heights are:

```c
hA = A.unk0[2] * 4.0f + 4.25f;
hB = B.unk0[2] * 4.0f + 4.25f;
hC = C.unk0[2] * 4.0f + 4.25f;
hD = D.unk0[2] * 4.0f + 4.25f;
```

For the current SBC parser, this maps to the intersection Y level concept already exposed as `SbcIntersection.y`, subject to field-layout confirmation.

### Bilinear fill

Native loop shape:

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

Equivalent to the decompile expression:

```c
h = (1.0f - tx) * (hC * tz + hA * (1.0f - tz))
  + (hD * tz + hB * (1.0f - tz)) * tx;
```

This is the core shape generator. If the four intersection levels differ, the terrain inside the block becomes a planar/bilinear grade matching those corners. If all four levels match, the block becomes a flat plateau at `level * 4.0 + 4.25`.

### Feather and smooth stage

The handed decompile summary proves these facts:

- Scratch buffer: `DAT_04afa9a0`.
- Scratch record stride: `0x0c`, described as `{count, x, z, weight}` in the summary.
- Stamp footprint: 5 by 5 cells.
- Cell/row layout notes: `0x1c` bytes per cell and `0x1c00` per row, implying a `0x100`-wide scratch lattice.
- The rectangle is walked in 4-sample steps (`>> 2` relationship), matching one map grid cell per coarse smoothing step.
- Corner-validity / gate tables around `DAT_3ce6f5f8` decide whether a contribution is legal.
- Multiple outer iterations: `local_120 = 0x10`, so 16 smoothing/feather passes.
- Falloff form: `weight = 1.0 - distance * FLOAT_002781bc`, with non-positive weights rejected/clamped out.
- Writes go back through `Terrain_SetLandSampleWithTileBorderMirroring()`.

Port-level pseudocode that stays inside the known evidence:

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

Spread/radius, with confidence labels:

- Direct kernel radius is proven as 5 by 5 coarse cells, i.e. radius 2 on the 4-sample lattice.
- The function runs 16 outer iterations.
- If each pass feeds the next pass's updated heightfield, the maximum effective outward propagation is up to `16 * 2 = 32` coarse grid cells, or `32` map cells, or `128` native samples.
- That 32-cell propagation is an implementation inference from the summarized decompile, not a verified exact edge radius. The raw Ghidra pass needs to confirm whether each pass expands from newly written values or repeatedly restamps from the same source set.

### Final land merge

The final pass reads existing land tile heights:

```c
chunk = g_landTileHeightChunks[chunkX + chunkY * 8];
land_height = chunk[(z & 0x7ff) * 0x801 + (x & 0x7ff)];
```

The blend form is smoothstep-like:

```c
f = (DAT_00278644 - 2.0f * t) * t * t;  // DAT_00278644 = 3.0
out = (1.0f - f) * land_height + f * patch_height;
```

So with the decoded constant:

```c
f = (3.0f - 2.0f * t) * t * t;
```

This is the standard `t*t*(3 - 2*t)` easing curve. The unresolved part is how native computes `t` for each edge sample and what exact distance band it uses.

## Why the current port is wrong

Current `build_city_terrain()` does this:

```c++
height = native_like_city_terrain_height(world_x, world_z);
```

That makes the mesh depend on the island base generator. The island base generator may be useful as a background land field, but it does not know:

- intersection heights;
- street widths;
- intersection footprint insets;
- four-corner city blocks;
- road grades;
- authored city ramps;
- feathered transition from roads to surrounding land.

The current road tile path is already closer to the game than the terrain path. Road tiles use intersection Y levels and interpolate between street endpoints. Terrain v2 should make the terrain agree with that authored city surface, instead of asking the road network to visually coexist with unrelated island noise.

## Port plan for `map_editor_cpp`

This is a report-only task; do not apply these steps in this pass.

### 1. Keep the base field, but demote it

In `src/scene.cpp`, keep `native_like_city_terrain_height()` only as:

- optional base island field outside city patches; or
- flat fallback initialized to `23.875f` inside the city working area.

For Terrain v2, the base is the substrate. It is not the city shape. A practical first implementation should initialize the terrain grid to `23.875f` across the cached city bounds, then stamp the authored patches. After that works, re-enable the island base as an outer fallback if screenshots prove it matters near city edges.

### 2. Build an authored terrain working grid

Modify `build_city_terrain(const SbcMap& map, float unit_size)` so it creates a CPU working height grid before creating mesh vertices.

Recommended first-pass grid:

- Coordinate domain: map grid/world coordinates aligned with existing `CityTerrain` bounds.
- Height value: world Y.
- Mesh spacing: use `unit_size` (`4.0`) for initial correctness at road/grid contact points, not the current `unit_size * 4.0` (`16.0`). The terrain mesh is cached per `city_scene_fingerprint()`, so the cost is acceptable.
- Optional exact mode later: native sample spacing (`1.0`) if visual comparisons show 4.0 spacing misses curb/feather detail.

The working grid can store terrain heights at mesh vertices. For exact native semantics, add conversion helpers:

```c++
native_sample_x = grid_x * 4 + 4096;
native_sample_z = grid_z * 4 + 4096;
world_x = grid_x * unit_size;
world_z = grid_z * unit_size;
```

If the mesh grid is coarser than native samples, evaluate the patch formula analytically at each mesh vertex instead of trying to literally replay every native sample write.

### 3. Derive four-corner patch rectangles from city topology

Add a helper near the existing city tile helpers, not in a new architecture layer:

```text
build_city_intersection_patches(map)
```

Responsibilities:

- find closed four-intersection blocks equivalent to `A -> south B`, `A -> west C`, `C -> south D`;
- compute footprint insets matching native `field_0x2c` and `field_0x30`;
- emit patch records with four corner intersections, inclusive rectangle bounds, and four world heights.

Use existing `SbcStreet` endpoint data to build directional adjacency. Direction naming must be verified against native:

- current code has `side_from_intersection_delta()` and `CityIntersectionSide`;
- native decompile uses `streetSouth` and `streetWest`;
- do not assume current side naming exactly matches native memory names until the lead Ghidra pass confirms struct layout.

### 4. Stamp bilinear plateaus

For each patch record:

1. Convert each corner level to world terrain Y:

```c++
height = static_cast<float>(intersection.y) * 4.0f + 4.25f;
```

2. For each terrain grid sample inside the patch rectangle, compute normalized `tx/tz`.
3. Write the bilinear height into the working terrain grid.

This must overwrite the base field for interior samples. Do not average with noise in the plateau. The native function calls `Terrain_SetLandSampleWithTileBorderMirroring()` during fill, so the patch owns those samples.

### 5. Feather edges

Implement a conservative v2 feather that mirrors the proven native shape but keeps unresolved constants isolated:

- Use a radius-2, 5-by-5 kernel on the coarse grid.
- Run 16 passes.
- Use `weight = max(0, 1 - distance * feather_falloff)`.
- Use smoothstep edge blend `t*t*(3 - 2*t)` for final merge where distance-to-patch edge is known.

Important: put the falloff/radius constants in one small helper or local config struct. Do not bake guesses across the codebase. The next Ghidra pass should be able to replace those values without a refactor.

If exact native feathering is still unknown at implementation time, ship the first implementation as:

- exact rectangle bounds and bilinear fill;
- approximate feather with explicit comments and tests for broad properties only.

Do not claim exact native smoothing until `FLOAT_002781bc`, the gate table behavior, and the edge distance `t` calculation are confirmed.

### 6. Align road and terrain height queries

Current dependent paths:

- `build_city_tile_cells()` already produces `CityTileCell::base_y` and slopes from authored intersection heights.
- `build_city_tile_render_buffer()` renders road/curb solid-cell tops from `base_y + unit_size` plus slope displacement.
- `build_city_traffic_light_poles()`, `scene.roads`, and `scene.intersections` call `terrain_height_at()`.

After Terrain v2:

- `terrain_height_at()` at an intersection center should return the patch terrain height for that authored level, using the terrain convention `level * 4.0 + 4.25`.
- Road visual placement tests must compare against the actual rendered road top convention. If road tiles need a `+0.25` correction to match the native terrain surface, make that a separate vertical-alignment fix with screenshot/native evidence.
- Roads should not float above unrelated noise or sink below the authored terrain. The road tile grade and terrain patch grade should come from the same intersection levels.

### 7. Update tests when implementing

Expected test changes:

- Replace `city_terrain_is_non_flat_noise_mesh_padded_to_city_bounds()` with a Terrain v2 test suite.
- Keep `terrain_noise_hash_matches_native_all_ones_mask()` if the island base helper remains.
- Add a synthetic four-corner block test:
  - four intersections forming a rectangle;
  - corner Y levels differ;
  - terrain at the four patch corners equals `level * 4.0 + 4.25`;
  - terrain at the rectangle center equals the bilinear average;
  - patch interior is not sampled from `native_like_city_terrain_height()`.
- Add a flat city-base test:
  - no valid four-corner patch means city terrain initializes to `23.875` or optional base field;
  - no procedural hill claim in the middle of the city.
- Add a road-alignment test:
  - compare `terrain_height_at()` to the rendered top-surface convention for a road/intersection, not just `CityTileCell::base_y`.
- Add a feather property test:
  - heights outside the patch approach base land;
  - adjacent terrain deltas are bounded;
  - do not assert exact weights until the lead Ghidra pass closes the unknowns.

### 8. Cache and performance

The scene is already cached by `city_scene_fingerprint()`, which hashes intersections, streets, widths, and buildings. Terrain v2 should stay inside that lifecycle:

- Build the CPU height grid once per fingerprint.
- Generate mesh vertices/triangles from that grid.
- Upload the terrain GPU model through the existing path.

Even a `4.0` world-unit mesh grid is cheap compared with building/render-buffer work. A native `1.0` world-unit grid may still be viable for normal city bounds, but treat that as a fidelity toggle after the first correct implementation.

## Lead Ghidra pass still needed

These are the remaining blockers for exact-native parity:

1. Exact meanings of `streetIntersection.field_0x2c` and `field_0x30`.
   - Are they half-widths, footprint margins, side-specific road widths, or already-normalized inset distances?
   - How do they map to current `SbcStreet.left_width` / `right_width` and `city_intersection_footprint()`?

2. Exact native direction mapping.
   - Confirm which current axis (`SbcIntersection.x` or `.z`) corresponds to decompile `unk0[1]` and `pos.x`.
   - Confirm what native names `streetSouth` and `streetWest` mean relative to current `CityIntersectionSide`.

3. Raw feather normalization.
   - Confirm the scratch record layout at `DAT_04afa9a0`.
   - Confirm whether `{count, x, z, weight}` in the summary is literal or a semantic shorthand.
   - Confirm whether the blended result divides by total weight, count, or another accumulator.

4. `FLOAT_002781bc`.
   - This is the distance falloff multiplier in `weight = 1 - distance * falloff`.
   - Without it, exact feather radius/shape is not locked.

5. Gate table semantics around `DAT_3ce6f5f8`.
   - Need to know how corner validity masks prevent smoothing across invalid patch corners, missing roads, or block boundaries.

6. Final edge blend `t`.
   - `DAT_00278644 = 3.0` gives the smoothstep curve, but the source/range of `t` still needs confirmation.
   - Confirm the distance band and whether `t=0` is land edge or patch edge in every branch.

7. Call ordering.
   - Confirm when `Terrain_ApplyStreetIntersectionHeightPatch()` runs relative to `Terrain_GenerateIslandHeightfield()`, land-tile chunk initialization, roads, buildings, and object placement.

## Honest unknowns

- I cannot justify exact feather weights from the available `/tmp/re_terrain_patch.c`; it only gives the kernel shape, pass count, scratch-buffer hints, and falloff form.
- I cannot prove the effective feather spread is exactly 32 map cells. That is the maximum implied by a radius-2 kernel over 16 cumulative passes, not a confirmed native distance.
- I cannot prove the current `city_intersection_footprint()` odd/even anchoring matches native `field_0x2c` / `field_0x30`. The source already has a `VERIFY` comment on that footprint logic, and Terrain v2 depends on closing it.
- I cannot prove whether the terrain mesh should keep the current `height - 0.05f` render offset. The native terrain sample height is `level * 4.0 + 4.25`; visual road/ground z-fighting policy should be validated separately.
- I cannot claim the base island noise is useless everywhere. The correct statement is narrower: it is not the source of the authored city grades/plateaus. Keep it as a base/fallback until native call ordering and screenshots say otherwise.

## Implementation decision record

Recommended Terrain v2 sequencing:

1. Replace the noise-owned city mesh with an authored patch-owned mesh.
2. First ship exact rectangle/bilinear behavior from the RE.
3. Add conservative approximate feathering behind isolated constants.
4. Run a follow-up Ghidra pass to lock the feather constants and footprint fields.
5. Update tests away from "non-flat noise" and toward authored intersection terrain.

This is the practical route. It removes the wrong strategic dependency now, preserves the island generator as a supporting asset, and contains the remaining unknowns behind a small surface area.

---

## LEAD GHIDRA GAP-FILL ADDENDUM (2026-06-05)

Resolved several "lead Ghidra pass still needed" items by reading subrosa.exe directly.
These are VERIFIED facts — use them in the implementation instead of the placeholders above.

### StreetIntersection struct (confirmed layout, size 136)
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
RESOLVES gap #1 (footprint fields) + gap #2 (axis mapping): unk0[1]=X, pos.x=Z, CONFIRMED by
StreetIntersection_Process @3760 which computes footprint edges as:
  east  edge = unk0[1] + field_0x2c
  west  edge = unk0[1] - field_0x34
  south edge = pos.x   + field_0x30
  north edge = pos.x   - field_0x38
i.e. the four fields are per-side half-extents (E/W/S/N), NOT a single half-width.

### Feather constants (RESOLVES gap #4 and #6)
- FLOAT_002781bc = 0.0625 (= 1/16)  -> feather weight = 1 - dist*0.0625; reaches 0 at 16 samples.
- DAT_00278644   = 3.0              -> final smoothstep edge blend f = (3 - 2t)*t*t.
- FLOAT_00278280 = 0.25             -> 5x5 feather stamp sub-step (quarter native sample).
Combined with the proven 16 outer passes + radius-2 5x5 kernel: feather propagates up to ~16
native samples per the falloff (not the 32-cell upper bound guessed above; the 1/16 falloff caps
single-stamp reach at 16 samples = 4 map cells, multi-pass widens it but each stamp is bounded).

### Height/coord (already confirmed)
world Y = level*4.0 + 4.25; heightfield index = grid*4 + 4096; map cell = 4.0 units.

### STILL genuinely open (lower priority, not blocking a first correct impl)
- gate table DAT_3ce6f5f8 semantics (corner-validity masks) — affects only edge cases between
  blocks; a first impl can feather uniformly and refine later.
- exact scratch-buffer weight normalization (divide by total weight vs count) — affects feather
  smoothness, not plateau correctness.
- call ordering vs island gen — for map_editor it doesn't matter; we build terrain fresh.

### IMPLEMENTATION GUIDANCE for Terrain v2 (lead)
The bilinear plateau core + per-side footprint rectangles are now FULLY SPECIFIED and should be
implemented faithfully. The feather is conservative-but-grounded (constants known). map_editor's
SbcIntersection.x/.y/.z map to grid X / height-level / grid Z; the four per-side extents are not
currently parsed from SBC — use the existing city_intersection_footprint() extents as the stand-in
(it already derives a footprint from street widths) until/unless SBC carries them. Terrain must
agree with road authored_y (level*4+4.25) so roads sit in the ground.
