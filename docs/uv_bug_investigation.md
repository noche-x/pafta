# UV bug investigation

## Second pass — independent Ghidra RE of texture PLACEMENT (2026-06-18, Claude, no codex)

Verified directly against editor.exe and subrosa.exe.THIS_ONE. Findings:

- **Editor UV authoring is faithful in the port — confirmed by reading the binary, not assuming.**
  - `PostProcessBlockMesh_TextureCoordinatesAndBuffers @ 14004eaa0` (decompiled + the 0x20000 `1.0 - z` branch checked in disasm): the 6 orientation-tag UV formulas exactly equal the port's `native_oriented_uv` (scene.cpp:456 / formats.cpp:539).
  - `EmitPatchedBezierFaceGeometry @ 14004fad0`: authored bezier UVs interpolated then OVERWRITTEN by position-projection iff face tag high-nibble != 0; the port replicates this via the `fallback` arg at formats.cpp:631-646. Match.
  - `LoadBlock_SBL @ 140053740`: control points read as 16 × (3-float pos + 2-float uv), row-major, u-then-v. Port formats.cpp:765-781 matches byte-for-byte. No V-flip, no axis swap at load.
  - `RotateBlockMeshGeometry_90Degrees_CounterClockwise @ 1400507b0` (full disasm decoded): winding rotation for tags 0x30000/0x40000 is `{old3,old0,old1,old2}` = port scene.cpp:496. Material cycle is ONLY 1->2->3->4->1; 9..12 untouched (port's fixed helper at scene.cpp:434 is correct).

- **Runtime (subrosa.exe.THIS_ONE) default-cube paths carry NO baked UVs.**
  - `Voxel_GenerateBlockFaceGeometry @ 0xe8b0` (collision/voxel) and `BuildBlock_GenerateCubeVerticesFaces @ 0x3fb90` (render cube) both build box geometry from shared corner table `DAT_002b34f0` and face-index table `DAT_002b3660`, faces tagged constant `4`, with NO per-vertex UV. Texturing for these is position/planar-projected at draw, not a stored UV quad.
  - Corner table `DAT_002b34f0` decoded: c0(0,0,0) c1(0,0,1) c2(0,1,0) c3(0,1,1) c4(1,0,0) c5(1,0,1) c6(1,1,0) c7(1,1,1).

- **Likely-missed divergence (default cube preview only):** the port's SBB cube-cell preview uses a single FIXED uv quad `native_preview_face_uvs = {(0,0),(0,1),(1,1),(1,0)}` on ALL 6 faces (scene.cpp:362), plus an approximated corner table `native_preview_cube_corners` (scene.cpp:342) the comment itself calls a "planar fallback," NOT the native `DAT_002b34f0` order. A fixed UV quad cannot match the engine's per-face position projection, so faces whose native U/V axis runs opposite appear flipped. This is the strongest candidate for a single "upside-down" face on default cube cells. NOT yet fixed — needs the per-face orientation-tag projection wired into the preview path (or the native corner/face tables ported) to confirm.
  - Caveat: loaded buildings made of resolved SBL block-mesh pieces (scene.cpp:2639/2715 via add_rotated_block_mesh_to_buffer) already use the verified-correct projected UVs, so this preview divergence affects default-cube cells, not authored block-mesh pieces.

## Root cause summary

- The core UV projection formula in the port is not the root cause. `editor.exe` derives oriented block-face UVs exactly as `src/formats.cpp:539-555` and `src/scene.cpp:449-465` do.
- The Bezier patch sampler in the port is not row/column swapped. `editor.exe` samples the 4x4 grid as row-major `row * 4 + column`, evaluates row direction first at fixed column, then column direction; this matches `src/formats.cpp:513-536` and the face emission at `src/formats.cpp:631-646`.
- The CSX-packed `.sbb` loader is not using a different UV/texture-short layout. `subrosa.exe` and `subrosa.exe.THIS_ONE` both read the same per-cell 0x3c record order as loose `.sbb`; the packed path only hard-checks leading sentinel `1`.
- The mild "one texture flipped/wrong" case has one concrete port divergence: native SBL block rotation only cycles low material slots `1..4`, but the port's shared `rotate_native_material_slot` also cycles `9..12` at `src/scene.cpp:428-430`. If that block face uses material slots in `9..12`, rotation can select the wrong material while the UV math itself is correct.
- The severe "CSX map has essentially every block texture wrong" case is most plausibly an asset pairing problem, not a CSX UV decode problem. Geometry loading is CSX-wins (`src/app.cpp:489-541`), but texture loading skips CSX textures when a loose texture of the same name was loaded first (`src/app.cpp:430-440`). That can pair CSX buildings with stale loose textures and make every block look UV-wrong.

## Applied fixes (2026-06-18)

- Symptom 1: `src/scene.cpp:434-438` adds `rotate_native_block_mesh_material_slot`, and `src/scene.cpp:505` uses it only for SBL block-mesh rotation; `rotate_native_material_slot` remains unchanged for the procedural/tool buildblock path. Regression coverage is `tests/parser_tests.cpp:2158-2182`.
- Symptom 2: `src/app.cpp:381-395` adds CSX texture upsert by lowercased name, and `src/app.cpp:452-453` now lets embedded CSX textures replace same-name loose textures while reusing the existing vector slot via `LoadedTexture` move assignment.

## `editor.exe` evidence

### Oriented UV formula

Function: `PostProcessBlockMesh_TextureCoordinatesAndBuffers @ 14004eaa0`.

Relevant xrefs:
- `LoadAndInitBlockTemplates_SBL @ 14004d73b` calls it after built-in/default block generation and after loaded SBL patch emission.
- `RotateBlockMeshGeometry_90Degrees_CounterClockwise @ 1400507b0` mutates orientation tags, then callers postprocess rotated variants.
- `FUN_140051070 @ 140051423` also calls it for a buildblock path.

Decompiled behavior:

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

Assembly check: `14004eaf7` masks `0xf0000`; `14004eafc` compares `0x10000`; `14004eca3-14004ecab` loads `1.0` and subtracts the selected coordinate for the `0x20000` case. The other decompiled branches mirror the six formulas above.

Port comparison:
- `src/formats.cpp:539-555` matches this formula.
- `src/scene.cpp:449-465` matches this formula for placed/rotated block meshes.
- `src/render_gpu.cpp:519-520`, `src/render_gpu.cpp:592-593`, and `src/render_gpu.cpp:768-769` upload `u,v` unchanged; there is no later V flip in the GPU upload path.

### Bezier patch UV sampling

Function: `EmitPatchedBezierFaceGeometry @ 14004fad0`.

`LoadBlock_SBL @ 140053740` reads each authored SBL face as:

```c
for (row = 0; row < 4; ++row) {
  for (column = 0; column < 4; ++column) {
    fread(position[row][column], 4, 3, file);
    fread(uv[row][column],       4, 2, file);
  }
}
```

`EmitPatchedBezierFaceGeometry @ 14004fad0` then:

1. Outer loop samples rows: `local_50 = 0..row_tess`, `local_44 += 1.0 / row_tess`.
2. For each fixed column `local_54`, it evaluates `param_6`/row order across offsets `0x00, 0x30, 0x60, 0x90`.
3. Inner loop samples columns: `local_4c = 0..column_tess`, `local_48 += 1.0 / column_tess`.
4. It evaluates `param_4`/column order across the four intermediate columns.
5. UVs use the same traversal: source UV offsets `0x00, 0x20, 0x40, 0x60` at fixed column, then column interpolation.
6. Quad index order is `{row,col}`, `{row,col+1}`, `{row+1,col+1}`, `{row+1,col}`.

Port comparison:
- `src/formats.cpp:513-536` does the same `control_uvs[column]`, `control_uvs[4 + column]`, `control_uvs[8 + column]`, `control_uvs[12 + column]` row-major traversal.
- `src/formats.cpp:622-647` emits the same quad corner order.
- No V inversion exists in native before postprocess; the port does not need a blanket `1.0 - v` in this sampler.

### Native SBL rotation material divergence

Function: `RotateBlockMeshGeometry_90Degrees_CounterClockwise @ 1400507b0`.

Native rotation behavior:
- Coordinates: `x' = previous_depth - z`, `z' = x`; width/depth are swapped.
- For orientation tags `0x30000` or `0x40000`, native rotates quad corner indices.
- For low material slot rotation, native cycles only `1 -> 2 -> 3 -> 4 -> 1`; it does not cycle `9..12`.
- Orientation tag cycle is `0x60000 -> 0x10000 -> 0x50000 -> 0x20000 -> 0x60000`.

Port divergence:

```cpp
// src/scene.cpp:421-430
if (material >= 1U && material <= 4U) {
    return rotate_range(material, 1U);
}
if (material >= 9U && material <= 12U) {
    return rotate_range(material, 9U);
}
```

Wrong line:

```cpp
if (material >= 9U && material <= 12U) {
    return rotate_range(material, 9U);
}
```

Corrected SBL block-mesh behavior:

```cpp
return material;
```

Minimal proposed fix for the mild case:

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

Keep the existing `rotate_native_material_slot` for procedural/tool buildblock box code until that separate native path is audited, because `src/scene.cpp:803-830` uses it outside SBL block mesh rotation.

## `subrosa.exe` and `subrosa.exe.THIS_ONE` evidence

### Loose `.sbb`

Function: `Sbb_LoadFromFile @ 00026830` in both `subrosa.exe` and `subrosa.exe.THIS_ONE`.

Evidence:
- It accepts versions `12..14` via `local_258 - 0xcU < 3`.
- It reads dimensions at struct offsets `+0x40`, `+0x44`, `+0x48`.
- For each cell it reads 0x3c bytes in this order: dwords to `+0x5c`, `+0x60`, `+0x58`, `+0x64`, `+0x68`, `+0x6c`; then shorts `+0x70..+0x8e`; then dword `+0x90`.

Port comparison:
- `src/formats.cpp:194-238` reads exactly six dwords, sixteen shorts, and a final dword per 0x3c cell.
- `src/formats.hpp:41-48` documents the native non-struct file order.

### CSX-packed `.sbb`

Function: `SbbContainer_ReadWrite @ 00196d60` in both `subrosa.exe` and `subrosa.exe.THIS_ONE`.

Evidence:
- Packed `.sbb` starts with sentinel dword `1`; read path returns failure if it is not `1`.
- It then reads the same 0x40 header, dimensions/anchors, tables, cell records, and aux records as loose v14 layout.
- Its cell loop uses the same dword/short order as `Sbb_LoadFromFile @ 00026830`.

Port comparison:
- `src/app.cpp:470-481` rewrites only leading sentinel `1` to loose version `14`.
- `src/formats.cpp:953-998` then parses the payload using the loose parser.
- Fixture check: `/Users/noche/repos/SRC/server/data/test2/test.csx` SBB entries begin with dword `1`, dimensions at `+0x44/+0x48/+0x4c`, anchors at `+0x50/+0x54/+0x58`, and material table count at `+0x5c`, matching the binary.

### CSX texture blobs

Function: `Csx_ReadWriteTypedBlobPayload @ 00197630`.

Native texture record layout:

```c
uint32 sentinel = 1;
byte metadata[0x40];
uint32 color_png_size;     // +0x44
uint32 material_png_size;  // +0x48
byte color_png[color_png_size];
byte material_png[material_png_size];
```

Port comparison:
- `src/app.cpp:381-405` reads only the first/color PNG from `+0x4c`. That is enough for diffuse display, but it ignores the material PNG that native uploads too.
- This is not a UV-coordinate bug, but it is another CSX rendering parity gap.

## Exact port divergences and proposed fixes

### 1. Mild single flipped/wrong block face

Confirmed divergence:
- `src/scene.cpp:428-430` rotates material slots `9..12` for SBL block mesh rotation.
- Native `RotateBlockMeshGeometry_90Degrees_CounterClockwise @ 1400507b0` only cycles `1..4`.

Minimal proposed fix:
- Add a block-mesh-specific material rotation helper that cycles only `1..4`.
- Use it at `src/scene.cpp:498` inside `rotate_block_face_native_impl`.
- Do not change `native_oriented_uv`; it already matches native.

Validation:
- Load the map/block that has the one flipped/wrong face.
- Inspect whether the offending `SblBlockFace::material` is in `9..12` and the building placement has nonzero rotation.
- Confirm that after the proposed helper split, the face selects the same local material slot as `editor.exe`/native rotated SBL output.

### 2. Severe CSX-wide wrong textures

Confirmed divergence:
- `src/app.cpp:430-440` loads loose textures first, then skips CSX textures with duplicate names.
- `src/app.cpp:489-541` makes geometry CSX-wins.
- Native `World_LoadMapAndInitializeTerrain @ 000579a0` iterates CSX type `0x585a0004` texture records and loads them from the archive for the map runtime.

Wrong line:

```cpp
if (loaded_names.contains(lower_name(entry.name))) {
    continue;
}
```

Corrected behavior:

```cpp
// CSX texture entries should upsert/override by name, matching CSX geometry.
upsert_texture(project, entry.name, *texture, loaded_names);
```

Minimal proposed fix:

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

`upsert_texture_by_name` should unload the old `Texture2D` when replacing an existing entry, update the same vector slot, and preserve `loaded_names` index mapping.

Validation:
- Use `/Users/noche/repos/SRC/server/data/test2/test.csx` or the reported CSX map.
- Log a duplicate texture name that exists both loose and in CSX; verify CSX wins after the proposed change.
- Load one affected CSX building before/after and compare material names from `SbbBuilding::materials` against `project.textures`.
- Confirm the visual symptom changes without touching UV formulas.

## What not to change yet

- Do not add a blanket `v = 1.0F - v` in `native_oriented_uv`, `bezier_patch_uv_sample`, or GPU upload. Native evidence does not support it.
- Do not reorder Bezier control rows/columns. `editor.exe @ 14004fad0` matches the current row-major traversal.
- Do not invent a special CSX `.sbb` cell stride, endianness, winding, or UV-short order. `subrosa.exe @ 00196d60` contradicts that theory.
