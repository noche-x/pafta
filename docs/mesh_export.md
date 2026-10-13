# Mesh export (glTF)

On desktop, choose `File > Export Mesh...` to export the Building, Block, or City view you have open. Pafta writes `<name>.gltf`, a `.bin` file, and loose `.png` textures into the folder you pick. Texture file names start with the image number, so two textures whose names clean up to the same text still get separate files. If reading a texture back or writing a file fails, the export reports an error.

## Axes (checked against the Blender add-on)

Noche's fork of the Blender add-on (`github.com/noche-x/io_scene_subrosa`, checked out at `/Users/noche/repos/blender_subrosa/io_scene_subrosa`) shows how Sub Rosa axes map to Blender axes. The fork handles the model formats (.cmc/.cmo/.it3/.itm/.sbv/.sit/.srv/.tst) but not .sbb/.sbl/.sbc. We only use it as a reference for the axes, not as a format to copy:

- Import (`shared.py:40`): file `(x, y, z)` -> Blender `(x, z, y)`.
- Export (`export_sbv.py:244,260,279`): Blender `(x, y, z)` -> file `(x, z, y)`.

Sub Rosa has Y pointing up, Blender has Z pointing up, and the add-on swaps Y and Z.

glTF always has Y up, and Blender's glTF importer turns Y-up into Z-up on its own. So if we export the game's coordinates unchanged, the model ends up facing the same way in Blender as it does with the add-on. That is what the exporter does: positions stay the same, triangles keep their original counter-clockwise order, and no axis is mirrored.

Check the imported model for mirroring or faces turned inside out. If the Y-up to Z-up turn causes either one, change the axis transform and the triangle order together in `mesh_export.cpp` (`transform_vertex` and the reversed index order).

## What gets exported

- Flat normals per face. Vertices are not shared, so faces never share normals.
- The UVs from the render buffer, unchanged.
- One glTF primitive and one material for each material name, split using `material_runs`.
- Textures read back from the GPU and flipped upright again (the editor flips them when loading), so they look right in modeling tools.

## Where each view's mesh comes from

- Building: `build_native_building_render_buffer`.
- Block: `build_block_mesh_render_buffer`. Materials are named `material_<slot>`, and textures only attach when the project has a material with that name.
- City: the terrain mesh, the road and curb tiles, and each placed building moved into world space with `native_city_building_rotation_basis` and `native_city_building_render_origin`.

## Library

tinygltf (header-only) comes in through CMake FetchContent. PNG files are written with raylib's `ExportImage`, using the stb that raylib already links, so stb symbols are not duplicated.
