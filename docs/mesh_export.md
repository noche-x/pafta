# Mesh export (glTF)

Exports the active view (Building / Block / City) to glTF via `File > Export Mesh...`.
Writes `<name>.gltf` + `.bin` + loose `.png` textures into the chosen folder.

## Coordinate convention (verified against the Blender addon)

The user's own fork of the Blender addon (`github.com/noche-x/io_scene_subrosa`,
checked out at `/Users/noche/repos/blender_subrosa/io_scene_subrosa`) establishes
the canonical Sub-Rosa <-> Blender axis mapping. The fork covers the model formats
(.cmc/.cmo/.it3/.itm/.sbv/.sit/.srv/.tst) only, NOT .sbb/.sbl/.sbc, so it is an axis
convention reference, not a format to match:

- Import (`shared.py:40`): file `(x, y, z)` -> Blender `(x, z, y)`.
- Export (`export_sbv.py:244,260,279`): Blender `(x, y, z)` -> file `(x, z, y)`.

So Sub Rosa is **Y-up**; Blender is **Z-up**; the addon swaps Y and Z.

glTF is **Y-up** by spec, and Blender's glTF importer auto-rotates Y-up -> Z-up on
import. Therefore exporting in **raw engine space (Y-up, no transform)** lands in the
same final Blender orientation the addon produces. That is what this exporter does:
positions verbatim, original (CCW) winding, no axis mirror.

Open risk to confirm visually: the Y-up->Z-up importer rotation must not leave the
mesh mirrored/inside-out. If it does, apply an axis mirror + winding reversal in
`mesh_export.cpp` (transform_vertex + reverse triangle index order together).

The addon handles a different format family (.cmo/.cmc/.sbv/.itm/.it3/.srv/.tst
model formats), not .sbb/.sbl/.sbc, so it is a convention reference only, not a
format to match byte-for-byte.

## Contents
- Flat per-face normals (mesh de-indexed so faces never share normals).
- UVs preserved from the render buffer.
- One glTF primitive + material per material name; split from `material_runs`.
- Textures read back off the GPU and re-flipped to upright (undoing the editor's
  load-time vertical flip) so they read correctly in DCC tools.

## Scope builders
- Building: `build_native_building_render_buffer`.
- Block: `build_block_mesh_render_buffer` (materials named `material_<slot>`; textures
  bind only if a project material of that name exists).
- City: terrain mesh + road/curb tile buffer + each placement transformed into world
  space via `native_city_building_rotation_basis` + `native_city_building_render_origin`.
  Warns before exporting > 2M triangles.

## Dependency
tinygltf (header-only) via CMake FetchContent. Uses raylib's already-linked stb and
raylib `ExportImage` for PNG writes to avoid duplicate stb symbols.
