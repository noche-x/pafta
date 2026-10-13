# UV bug investigation

## Research on texture placement (2026-06-18, Claude, without Codex)

This pass compared the port with `editor.exe` and `subrosa.exe.THIS_ONE`. The source line numbers below are from the checkout as it was that day.

- The port's UV formulas matched the game's formulas found in this pass.
  - `PostProcessBlockMesh_TextureCoordinatesAndBuffers @ 14004eaa0` (decompiled, with the 0x20000 `1.0 - z` branch also checked in the disassembly): the six orientation-tag UV formulas are exactly the port's `native_oriented_uv` (scene.cpp:456 / formats.cpp:539).
  - `EmitPatchedBezierFaceGeometry @ 14004fad0`: the stored Bezier UVs are interpolated, then REPLACED by a projection from the position whenever the face tag's high nibble is not 0. The port does the same with the `fallback` argument at formats.cpp:631-646. Match.
  - `LoadBlock_SBL @ 140053740`: control points are read as 16 × (3-float position + 2-float uv), row by row, u before v. Port formats.cpp:765-781 matches byte for byte. Nothing flips V or swaps axes while loading.
  - `RotateBlockMeshGeometry_90Degrees_CounterClockwise @ 1400507b0` (full disassembly decoded): for tags 0x30000/0x40000 the corners rotate as `{old3,old0,old1,old2}`, the same as port scene.cpp:496. Materials ONLY cycle 1->2->3->4->1, and 9..12 stay the same (the port's fixed helper at scene.cpp:434 is right).

- The game's default-cube paths (subrosa.exe.THIS_ONE) have NO stored UVs.
  - `Voxel_GenerateBlockFaceGeometry @ 0xe8b0` (collision and voxels) and `BuildBlock_GenerateCubeVerticesFaces @ 0x3fb90` (the drawn cube) both build boxes from the shared corner table `DAT_002b34f0` and face table `DAT_002b3660`. Every face has the fixed tag `4` and there is NO UV per vertex. These are textured by projecting the position at draw time, not with a stored UV quad.
  - Corner table `DAT_002b34f0`, decoded: c0(0,0,0) c1(0,0,1) c2(0,1,0) c3(0,1,1) c4(1,0,0) c5(1,0,1) c6(1,1,0) c7(1,1,1).

- A difference that was probably missed (default cube preview only): the port's SBB cube-cell preview uses one FIXED uv quad, `native_preview_face_uvs = {(0,0),(0,1),(1,1),(1,0)}`, on ALL 6 faces (scene.cpp:362). It also uses a guessed corner table, `native_preview_cube_corners` (scene.cpp:342), which its own comment calls a "planar fallback", and which is NOT the game's `DAT_002b34f0` order. A fixed UV quad cannot match the engine's per-face projection, so faces whose U or V runs the other way show up flipped. This is the most likely cause of a single "upside-down" face on default cube cells. NOT fixed yet: to confirm, the preview needs the per-face orientation-tag projection (or the game's corner and face tables).
  - Note: buildings made of SBL block-mesh pieces (scene.cpp:2639/2715 via add_rotated_block_mesh_to_buffer) already use the checked projected UVs. So this preview difference only affects default cube cells, not authored block-mesh pieces.

## Causes found

- The port's main UV projection formula is not the cause. `editor.exe` works out block-face UVs exactly as `src/formats.cpp:539-555` and `src/scene.cpp:449-465` do.
- The port's Bezier patch sampler does not swap rows and columns. `editor.exe` reads the 4x4 grid row by row as `row * 4 + column`, works along the rows first at a fixed column, then along the columns. This matches `src/formats.cpp:513-536` and the face output at `src/formats.cpp:631-646`.
- The loader for `.sbb` files packed in a CSX archive does not use a different UV or texture layout. `subrosa.exe` and `subrosa.exe.THIS_ONE` both read the same 0x3c-byte cell order as loose `.sbb` files; the packed path only checks that the first value is `1`.
- The mild "one texture flipped or wrong" case has one real difference in the port. The game's SBL block rotation only cycles the low material slots `1..4`, but the port's shared `rotate_native_material_slot` also cycles `9..12` at `src/scene.cpp:428-430`. If that block face uses a slot in `9..12`, rotation can pick the wrong material even though the UV math is right.
- The severe "CSX map has nearly every block texture wrong" case is most likely the wrong textures being paired with buildings, not a CSX UV decoding problem. For geometry, the CSX copy wins (`src/app.cpp:489-541`). But texture loading skips a CSX texture when a loose texture with the same name was loaded first (`src/app.cpp:430-440`). That can pair CSX buildings with old loose textures and make every block look like its UVs are wrong.

## Fixes made (2026-06-18)

- Problem 1: `src/scene.cpp:434-438` adds `rotate_native_block_mesh_material_slot`, and `src/scene.cpp:505` uses it only when rotating SBL block meshes. `rotate_native_material_slot` stays the same for the procedural and tool buildblock path. The test is `tests/parser_tests.cpp:2158-2182`.
- Problem 2: `src/app.cpp:381-395` adds a way to insert or replace CSX textures by lower-case name, and `src/app.cpp:452-453` now lets textures inside the CSX replace loose textures with the same name. It reuses the existing slot in the list through `LoadedTexture` move assignment.

## Evidence from `editor.exe`

### Oriented UV formula

Function: `PostProcessBlockMesh_TextureCoordinatesAndBuffers @ 14004eaa0`.

Callers:
- `LoadAndInitBlockTemplates_SBL @ 14004d73b` calls it after making the built-in default blocks and after adding loaded SBL patches.
- `RotateBlockMeshGeometry_90Degrees_CounterClockwise @ 1400507b0` changes orientation tags, and its callers then post-process the rotated copies.
- `FUN_140051070 @ 140051423` also calls it on a buildblock path.

What the decompiled code does:

```c
switch (faceMaterialAndOrientation & 0xf0000) {
case 0x10000: uv = { position.z,       position.y       }; break;
case 0x20000: uv = { 1.0 - position.z, position.y       }; break;
case 0x30000: uv = { position.x,       position.z       }; break;
case 0x40000: uv = { position.x,       1.0 - position.z }; break;
case 0x50000: uv = { 1.0 - position.x, position.y       }; break;
case 0x60000: uv = { position.x,       position.y       }; break;
default:      keep existing authored/fallback UV;
}
```

Checked in the assembly: `14004eaf7` masks `0xf0000`, `14004eafc` compares with `0x10000`, and `14004eca3-14004ecab` loads `1.0` and subtracts the chosen coordinate for the `0x20000` case. The other decompiled branches follow the six formulas above.

Compared with the port:
- `src/formats.cpp:539-555` matches this formula.
- `src/scene.cpp:449-465` matches this formula for placed and rotated block meshes.
- `src/render_gpu.cpp:519-520`, `src/render_gpu.cpp:592-593`, and `src/render_gpu.cpp:768-769` upload `u,v` unchanged; nothing flips V later on the way to the GPU.

### Sampling Bezier patch UVs

Function: `EmitPatchedBezierFaceGeometry @ 14004fad0`.

`LoadBlock_SBL @ 140053740` reads each SBL face like this:

```c
for (row = 0; row < 4; ++row) {
  for (column = 0; column < 4; ++column) {
    fread(position[row][column], 4, 3, file);
    fread(uv[row][column],       4, 2, file);
  }
}
```

`EmitPatchedBezierFaceGeometry @ 14004fad0` then:

1. Loops over rows on the outside: `local_50 = 0..row_tess`, `local_44 += 1.0 / row_tess`.
2. For each fixed column `local_54`, works out `param_6` (the row order) across offsets `0x00, 0x30, 0x60, 0x90`.
3. Loops over columns on the inside: `local_4c = 0..column_tess`, `local_48 += 1.0 / column_tess`.
4. Works out `param_4` (the column order) across the four in-between columns.
5. Walks the UVs the same way: source UV offsets `0x00, 0x20, 0x40, 0x60` at a fixed column, then blends across columns.
6. Orders quad corners as `{row,col}`, `{row,col+1}`, `{row+1,col+1}`, `{row+1,col}`.

Compared with the port:
- `src/formats.cpp:513-536` walks row by row the same way: `control_uvs[column]`, `control_uvs[4 + column]`, `control_uvs[8 + column]`, `control_uvs[12 + column]`.
- `src/formats.cpp:622-647` uses the same quad corner order.
- The game does not flip V before post-processing, so the port does not need a blanket `1.0 - v` in this sampler.

### Where SBL rotation materials differ

Function: `RotateBlockMeshGeometry_90Degrees_CounterClockwise @ 1400507b0`.

What the game does:
- Coordinates: `x' = previous_depth - z`, `z' = x`; width and depth swap.
- For orientation tags `0x30000` or `0x40000`, it rotates the quad's corner indices.
- For the low material slots, it only cycles `1 -> 2 -> 3 -> 4 -> 1`. It does not cycle `9..12`.
- Orientation tags cycle `0x60000 -> 0x10000 -> 0x50000 -> 0x20000 -> 0x60000`.

How the port differed:

```cpp
// src/scene.cpp:421-430
if (material >= 1U && material <= 4U) {
    return rotate_range(material, 1U);
}
if (material >= 9U && material <= 12U) {
    return rotate_range(material, 9U);
}
```

The wrong lines:

```cpp
if (material >= 9U && material <= 12U) {
    return rotate_range(material, 9U);
}
```

What SBL block meshes should do there:

```cpp
return material;
```

Smallest fix suggested for the mild case:

```diff
+std::uint32_t rotate_native_block_mesh_material_slot(std::uint32_t material, std::int32_t rotation) {
+    auto rotate_range = [rotation](std::uint32_t value, std::uint32_t first) {
+        return ((value - first + static_cast<std::uint32_t>(rotation & 3)) & 3U) + first;
+    };
+    return material >= 1U && material <= 4U ? rotate_range(material, 1U) : material;
+}
 ...
-            rotated.material = rotate_native_material_slot(rotated.material, 1);
+            rotated.material = rotate_native_block_mesh_material_slot(rotated.material, 1);
```

Keep the existing `rotate_native_material_slot` for the procedural and tool buildblock box code until that separate game path has been checked, because `src/scene.cpp:803-830` uses it outside SBL block-mesh rotation.

## Evidence from `subrosa.exe` and `subrosa.exe.THIS_ONE`

### Loose `.sbb`

Function: `Sbb_LoadFromFile @ 00026830` in both `subrosa.exe` and `subrosa.exe.THIS_ONE`.

Evidence:
- It accepts versions `12..14` through `local_258 - 0xcU < 3`.
- It reads the sizes at struct offsets `+0x40`, `+0x44`, `+0x48`.
- For each cell it reads 0x3c bytes in this order: dwords into `+0x5c`, `+0x60`, `+0x58`, `+0x64`, `+0x68`, `+0x6c`; then shorts `+0x70..+0x8e`; then a dword into `+0x90`.

Compared with the port:
- `src/formats.cpp:194-238` reads exactly six dwords, sixteen shorts, and one last dword per 0x3c-byte cell.
- `src/formats.hpp:41-48` notes that the file order is not the order of the game's struct.

### `.sbb` packed in a CSX

Function: `SbbContainer_ReadWrite @ 00196d60` in both `subrosa.exe` and `subrosa.exe.THIS_ONE`.

Evidence:
- A packed `.sbb` starts with the dword `1`, and reading fails if it is anything else.
- After that it reads the same 0x40 header, sizes and anchors, name lists, cells, and extra records as a loose v14 file.
- Its cell loop reads dwords and shorts in the same order as `Sbb_LoadFromFile @ 00026830`.

Compared with the port:
- `src/app.cpp:470-481` only changes the leading `1` into loose version `14`.
- `src/formats.cpp:953-998` then reads the data with the loose-file parser.
- Checked against a real file: the SBB entries in `/Users/noche/repos/SRC/server/data/test2/test.csx` start with the dword `1`, with sizes at `+0x44/+0x48/+0x4c`, anchors at `+0x50/+0x54/+0x58`, and the material count at `+0x5c`, just like the binary expects.

### CSX textures

Function: `Csx_ReadWriteTypedBlobPayload @ 00197630`.

The game's texture record:

```c
uint32 sentinel = 1;
byte metadata[0x40];
uint32 color_png_size;     // +0x44
uint32 material_png_size;  // +0x48
byte color_png[color_png_size];
byte material_png[material_png_size];
```

Compared with the port:
- `src/app.cpp:381-405` only reads the first (color) PNG, from `+0x4c`. That is enough to show the color, but it skips the material PNG that the game also uploads.
- This is not a UV bug, but it is another way CSX rendering differs from the game.

## The exact differences and the fixes suggested

### 1. One block face flipped or wrong (mild)

Confirmed difference:
- `src/scene.cpp:428-430` rotates material slots `9..12` when rotating SBL block meshes.
- The game's `RotateBlockMeshGeometry_90Degrees_CounterClockwise @ 1400507b0` only cycles `1..4`.

Smallest fix suggested:
- Add a material rotation helper just for block meshes that only cycles `1..4`.
- Use it at `src/scene.cpp:498` in `rotate_block_face_native_impl`.
- Leave `native_oriented_uv` alone; it already matches the game.

How to check:
- Load the map or block with the one flipped or wrong face.
- Check whether the bad face's `SblBlockFace::material` is in `9..12` and whether the building is placed with a rotation other than zero.
- After splitting the helper, check that the face picks the same local material slot as the rotated SBL output from `editor.exe` or the game.

### 2. Wrong textures across a whole CSX map (severe)

Confirmed difference:
- `src/app.cpp:430-440` loads loose textures first, then skips CSX textures with the same names.
- `src/app.cpp:489-541` lets the CSX copy win for geometry.
- The game's `World_LoadMapAndInitializeTerrain @ 000579a0` goes through the CSX texture records (type `0x585a0004`) and loads them from the archive for the map.

The wrong lines:

```cpp
if (loaded_names.contains(lower_name(entry.name))) {
    continue;
}
```

What it should do:

```cpp
// CSX texture entries should upsert/override by name, matching CSX geometry.
upsert_texture(project, entry.name, *texture, loaded_names);
```

Smallest fix suggested:

```diff
 for (const auto& entry : archive.entries) {
     if (entry.kind != CsxEntryKind::Texture) {
         continue;
     }
-    if (loaded_names.contains(lower_name(entry.name))) {
-        continue;
-    }
     if (auto texture = load_embedded_png_texture(entry.payload)) {
-        add_texture_if_valid(project, entry.name, *texture, loaded_names);
+        upsert_texture_by_name(project, entry.name, *texture, loaded_names);
     }
 }
```

When `upsert_texture_by_name` replaces an entry, it should unload the old `Texture2D`, reuse the same slot in the list, and keep the `loaded_names` index mapping.

How to check:
- Use `/Users/noche/repos/SRC/server/data/test2/test.csx` or the CSX map from the report.
- Log a texture name that exists both as a loose file and in the CSX, and check that the CSX one wins after the change.
- Load one affected CSX building before and after, and compare the material names in `SbbBuilding::materials` with `project.textures`.
- Check that the problem on screen changes without touching any UV formula.

## What not to change yet

- Do not add a blanket `v = 1.0F - v` in `native_oriented_uv`, `bezier_patch_uv_sample`, or the GPU upload. Nothing in the game supports it.
- Do not reorder Bezier control rows or columns. `editor.exe @ 14004fad0` matches the current row-by-row walk.
- Do not invent a special cell size, byte order, winding, or UV order for `.sbb` files in a CSX. `subrosa.exe @ 00196d60` shows that idea is wrong.
