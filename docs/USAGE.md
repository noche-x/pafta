# Using Pafta

Always backup your maps and work with copies of your maps, and test saved maps in Sub Rosa before sharing them. The editor preview can differ from the game. Build and download instructions are in the [README](../README.md).

## Version compatibility and limitations

Pafta is made for Sub Rosa 0.38. Broader backward compatibility with older game versions is planned. Some older file formats already load, but that does not guarantee that a saved map will work in the game version it came from.

| File | Formats currently read |
| --- | --- |
| `.sbb` building | Versions 12-14. Version 12 has no anchor field, so Pafta uses `(4, 4, 4)`. |
| `.sbc` city map | Versions 7-12. Older layouts are read according to their version. |
| `.sbl` block | Versions 1-2. |
| `.sbl` buildblock | The layer-based layout described in the [SBL format notes](file_formats/sbl.md). Its header values are not fully understood. |
| `.csx` archive | The directory layout described in the [CSX format notes](file_formats/csx.md). Archived SBB and SBL entries can use a container marker instead of the loose-file version. |

These are file-format numbers, not Sub Rosa release numbers. SBB and SBC versions outside the listed ranges are rejected. If a building or block cannot be parsed, Pafta can still open the map for inspection, but Save stays disabled until the broken asset is fixed or removed and the map is reopened. An unreadable city file or archive can prevent the map from opening.

Loading an older file does not mean Save preserves its original format. Older city maps with district cells keep their version until you accept a Piece edit that upgrades them; other city maps save as version 12. Full block writes use SBL version 2, and map Save rebuilds `test.csx`. Keep the original files and test saved maps in Sub Rosa 0.38. Compatibility with earlier game versions is not guaranteed.

## Open a map

1. Make a copy of the map folder you want to edit.
2. Start Pafta. In the browser, open the editor's web page.
3. Choose File -> Open map folder and select the copied folder, or drag the folder onto the editor. In the browser, you can also use the Open map folder button.
4. Choose whether the archive or loose files should win when both contain the same asset. Then wait for loading to finish. The map opens in the City view. Check any loading errors before editing; a building or block that failed to load can disable Save.

Select the whole map folder, not an individual file. Pafta reads `city2.sbc`, loose assets in `building/`, `block/`, and `buildblock/`, and assets inside `.csx` archives. The Building editor also lists `.sis` item sets in `itemset/`, either inside the map folder or next to it. If the folder contains a single `.sbc` file with a different name, Pafta opens it, but Save still writes `city2.sbc`.

Before a map opens, Pafta asks which asset copy should win. Choose **test.csx archive** for the game's load order, or **Loose files** to work from the editable folders. **Do not ask again** remembers that order for later loads. File -> Ask for load order before opening turns the prompt back on. Load order is not in the Preferences window.

## Edit and save

There are four editors:

- City: roads, placed buildings, and pieces.
- Building: building cells and materials.
- Block and Buildblock: the mesh and layer records.

The viewport shows the scene, what is selected, and a preview of what you are about to place. It does not yet match the game's rendering exactly.

### Saving

On desktop, Ctrl+S asks for a folder. Save runs in the background, so you can keep editing. Edits made after Save starts remain unsaved. Save writes `city2.sbc` and `test.csx` there. It also writes `buildblock/*.sbl` files for every buildblock you edited and every buildblock that came from an archive. If you save to a different folder, it writes all loaded buildblocks.

Save prepares every file before it replaces anything. If replacing fails, it tries to put the old files back, and if that fails too, the error message says where the backups are. A crash or power cut can still stop a save halfway through several files, so keep copies.

Save rebuilds the asset archive, so the bytes can change even when the content does not. Old city maps with district cells keep their version until you agree to a Piece edit that upgrades them. Other city maps are saved as version 12. Save also writes `asphalt2.png` and `asphalt2_material.png`, which the game loads from the map folder. If the map has none, they are copied from another map in the same data folder. `map.png` is copied when the map has one. Loose `.sis` and `.it3` files live in the game's shared `data/itemset` and `data/item` folders, so Save does not copy them.

If some geometry failed to load, the map still opens so you can look at it. Save and archive rebuilding stay off until you fix or remove the broken file and open the map again. Warnings about item previews do not stop Save.

In the browser, Save starts one download each for the city, the archive, and every buildblock. Allow multiple downloads. Put each `.sbl` file in a `buildblock/` folder next to `city2.sbc` and `test.csx`, and keep any other files the map needs. The browser cannot tell Pafta whether you kept every download.

### Undo and unsaved changes

City edits have their own undo history, and so do the Building, Block, and Buildblock editors. The separate CSX archive workspace also asks before exit if its edits are unsaved; Save map does not save that workspace. If you have unsaved map edits, Pafta asks whether to save, discard, or cancel before it quits or opens another map. If a map fails to load, the current one stays open. In the browser, Pafta asks the browser to warn you before closing or reloading the tab, but the browser decides when to show that warning. The map only lives in memory there, so keep your downloads before closing the tab.

### Main controls

| Action | Control |
| --- | --- |
| Orbit, pan, zoom | Middle drag, Shift + middle drag, mouse wheel (Alt + left drag also orbits) |
| Look along an axis | Click an axis on the view gizmo (top right), or Numpad 1, 3, 7 (Ctrl: the opposite side); axis views are orthographic until you orbit |
| Orthographic on or off | Numpad 5 |
| Free camera | Hold right mouse; movement keys follow your Alex or Blender preset |
| Context menu | Quick right-click on the view, a list row, or an asset |
| Save, open, frame selection | Ctrl+S, Ctrl+O, F |
| Undo, redo | Ctrl+Z, Ctrl+Y or Ctrl+Shift+Z |
| City tools | Keys 1-5 or the toolbar |
| Building tools | Keys 1-5 or the toolbar; 5 is Item set |
| All shortcuts | Ctrl+/ |

On first launch, choose Alex or Blender controls. You can change the preset in Preferences. Alex uses level WASD movement, Q/Z up/down, Shift for 4× speed, Ctrl for ¼ speed, and Shift+Ctrl for 16× speed. Middle drag pans and Alt+left drag orbits. Blender uses middle drag to orbit, Shift+middle to pan, and Ctrl+middle to zoom. Its free camera follows your view with WASD, uses Space/Ctrl up/down, Shift for ¼ speed, and Z for 4× speed.

Each editor mode has its own flying speed in Preferences. Vertical scrolling changes the current mode's speed while you hold right mouse. Horizontal scrolling does not zoom the camera or change flying speed. Flying and camera animations use elapsed seconds, so movement does not depend on frame rate. Clicking the viewport hands input back from a text field. Editing shortcuts stay off while you fly or type in a field. On macOS, shortcut labels use Cmd where it applies. Pafta -> About Pafta opens the version, credits, licenses, and GitHub links. Inspector and preference sections can be collapsed. Preferences -> Rendering -> Terrain distance skips terrain chunks outside that distance before drawing; zero means unlimited distance. Terrain outside the view frustum is also skipped. It changes submitted triangles, not the saved terrain or resident GPU buffers.

Every panel is a window you can move, dock anywhere around the view, or close; Pafta remembers the layout and which windows are open. Windows -> Reset window layout puts them back.

### City tools

- Select picks and moves roads, buildings, and pieces.
- Building shows a preview of a building and places it when you let go of the mouse. Red means it overlaps something. Drag away before you let go to cancel.
- Road starts at your first click and adds a straight segment with each click after that.
- Piece places or paints cells using the chosen asset.
- Erase removes the highlighted object.

The Select tool's Properties panel has separate Buildings, Pieces and Roads box-selection filters. A box can select all three together. Arrow and Page Up/Down keys move the group; Delete removes the group. Piece selection affects only the cells caught by the box.

Building, Road and Piece placement supports a surface height offset or a locked Y cell level. Choose Lock Y level in Properties to place on a horizontal plane regardless of the terrain. Page Up/Down adjusts the active height.

While you lay a road, the preview draws the road the click will make, with the game's own road tiles and markings. Green means the game builds it cleanly. Red means it cannot: the slope does not fit, the road leaves the game's map area, it runs over another road (red boxes), or a building or piece covers part of it (orange boxes). Existing roads that the new road splits or reshapes are tinted yellow and listed in the cursor readout.

To move a road junction, click its crossing (or the end of a road) with the Select tool and drag it, drag the arrows of its gizmo (the green arrow changes its level), or use the arrow keys. Roads stay straight: a road along the move stretches or shrinks, and a road across it carries its other junction along. The preview uses the same colours, and tints the road tiles that the move removes dark red. A move that would shrink a road below two cells is refused.

Esc ends a road, or cancels a junction or gizmo drag.

### Building tools

- Select picks a block.
- Add block fills the cell next to it.
- Paint applies the chosen material, or with Wall type, a wall type to one side of a block.
- Anchor moves the building's anchor point.
- Item set places or clears an item set on the block you click.

In Wall type mode, the yellow panel shows the side a click sets. Clicking a wall sets that wall. Clicking a floor, a piece, or an open side sets the side of that block nearest the click, so you can add a wall where there is none. A wall between two blocks is stored in one of them; if that block has no record yet, Pafta adds an empty one to hold it.

An interior piece replaces the block's room, as it does in the game: that block has no room walls or floor. Its sides can still take wall types for the rooms next to it.

To use an item set, first add its `.sis` name to the building in Properties, then pick a rotation. Placed sets show their items from `item/*.it3` without textures. A set with no item meshes shows as a yellow diamond. Save writes the set names and positions into the building data in `test.csx`. It does not copy the `.sis` or `.it3` files.

The floor control shows one floor or the whole building. A room's Floor setting (in Add block and on the selected block) leaves out its floor, which also removes the ceiling of the room below, so stacked rooms join into one tall room. editor.exe always writes a floor.

The Buildblock window (Ctrl+L) draws each face as the game builds it: grey wall around the opening, glass windows, and beams. The opening (blue edge) is the one hole in the wall. Windows are glass panes inside it, so glass outside the opening is hidden by the wall; the editor outlines that part in red and offers Grow opening to fit. From far away the game shows the whole opening as reflective glass. Doorway turns a face into an Interior opening from the floor with no glass, which is how the shipped buildblocks make doors. The game never reads door records, so old ones show as purple markers you can delete. Drag items and corners on the square canvas; right-click to add, copy, or delete. Exterior and Interior set the face's portal type, as the same buttons do in editor.exe. A new buildblock starts with face 1 as a window, face 2 as a doorway, and the other faces as plain walls.

Block mode's toolbar has Select, Add box, Add patch and Paint faces; Assets -> New block makes a new block. A patch with Stored UVs shows its UVs over its slot texture in Properties: drag a point to move it, or empty space to move them all. Paint chooses a texture slot from 0 to 7 for authored patches and boxes; the preview's slot textures show how a placed piece will look. Assets lists meshes and textures, with Import and New block actions. Export .sbl writes the selected block as a loose SBL file.

The Piece tool's Assets panel lists the map's presets or all meshes, including imported SBL meshes and the solid cube. Choose a mesh and its slot textures, then stamp it into the city.

A piece is a mesh with eight slot textures. The map's presets are stored in `city2.sbc`; another texture set for the same mesh is stored as another layer of that preset. Add preset in the City Piece Assets panel (or Save as map piece in Properties) adds the current mesh and textures as a preset without placing it. Placing a piece that is not a preset adds it automatically.

The Block toolbar shows the paint slot's texture name and thumbnail. Assets -> Textures changes its preview texture. SBL faces store slot numbers; the building cell or city piece chooses the actual textures when placing that block. Switching block assets cancels an unfinished transform before new patches or boxes are added.

In Block mode, drag the arrows to move the selected patch or box, the white cubes to resize a box, and the orange dots to move patch points. G, S, and R move, scale, and rotate; X, Y, or Z locks an axis; a click or Enter keeps the change and Esc cancels it.

Preferences -> Rendering -> Terrain detail chooses how the ground is meshed. Like the game (the default) samples the ground near the camera every unit and gets coarser at 512, 768, 1024, 1536, 2048 and 3072 units, as the game does, so hills meet buildings the way they do in the game. Fixed builds one mesh with detail only near the city's centre, which is lighter. Detail distance scales those distances, and Finest step sets the nearest sample spacing.

View -> Interiors chooses when buildings show their rooms: never, up close like the game, or only for the selected buildings. The Block and Buildblock editors show the fields of the selected record. Windows -> Map image opens a top-down view of the map. File -> CSX archive lists the map's assets and refreshes itself as you edit. It has a filter, Shift and Ctrl selection, hover previews, and Extract all, and it writes `test.csx`, the only archive name the game reads. To add a building, block, or texture to the map, use Assets -> Import into map (or Import into map in the archive window); it shows up in Assets right away and is saved with the map. Add to archive only changes the archive workspace, not the map. Files that fail to load or import are listed in Windows -> Load problems, with Try auto-fixing where a fix is known.

### Time of day

View -> Time of day moves the sun in the preview. The same setting is in Preferences. It starts at 11:00 and follows the game's World-mode sun. It only changes the viewport, not the map's time in game.

## File support and limits

Pafta reads `.sbb` buildings, `.sbc` city maps, `.sbl` blocks and buildblocks, and `.csx` asset archives. It can edit which item sets a building uses and show their `.it3` item meshes, but it cannot edit `.sis` files. [file formats](file_formats/README.md) has the byte layouts, supported versions, how saving works, and the fields nobody understands yet.

Current limits:

- Nobody has proven that every generated portal and buildblock mesh looks the same as in the game.
- You can edit buildblock door, window, and beam records, but not add or delete them.
- You cannot type names or city-block slots directly yet.
- The viewport does not have the original editor's box-brush placement.

The command-line viewer and patch commands are still there for development and tests. They take separate input and output paths, for example:

```sh
./build/dev/pafta --dump-sbb-cell ../server/data/test2/building/test.sbb 0
./build/dev/pafta --set-sbb-cell ../server/data/test2/building/test.sbb 0 exterior_portal_face 12345 edited.sbb
./build/dev/pafta --dump-sbc-building ../server/data/test2/city2.sbc 0
./build/dev/pafta --set-sbc-building ../server/data/test2/city2.sbc 0 x 4321 edited.sbc
```

The SBL commands are `--dump-sbl-word`, `--set-sbl-word`, `--dump-sbl-layer`, and `--set-sbl-layer-portal`. `src/main.cpp` lists their arguments. These commands look at or change single fields. They are not a replacement for saving a whole map.


### Placement and performance checks

Road previews build the affected roads and junctions, then transfer that prepared mesh when clicked. Piece stamps use cached native meshes while the full scene rebuild runs. Desktop scene workers share immutable SBB snapshots; repeated placements do not copy the same building geometry again. GPU uploads still run on the rendering thread. The browser build has no worker threads and can pause during full rebuilds.

For a headless Release CPU check, run `build/ci/editor_workflow_tests --road-bench ../server/data/test2/city2.sbc`. It reports preparation, local tile diff with loaded buildings, CPU mesh generation, commit, worker SBB snapshot, and history timings over 100 samples. It does not measure drawing, GPU uploads, or full-frame latency.

When a valid slope fills the entire street run, Pafta preserves sloped curb cells at the junction and does not put flat end-cap plates over them. This corrects the editor geometry; it does not change the SBC street record or the game's native road generator.

Graphics Debug (F3) groups integer counters with commas. Its frame-time graph adds a half-second average per point and keeps 120 points (about one minute). The headline also shows the slowest frame in the latest interval. GPU memory lists tracked buffer allocations and estimated texture/render-target storage. Supported NVIDIA drivers can additionally report approximate dedicated VRAM use through `GL_NVX_gpu_memory_info`; other drivers show that figure as unavailable. Driver padding, swapchain storage, and driver-held copies are outside the Pafta allocation estimate.

`build/ci/editor_workflow_tests --terrain-bench ../server/data/test2/city2.sbc` checks the real map's distance-only terrain draw ranges at 128, 512 and 1,024 units. The viewport additionally culls the view frustum; this headless check does not measure GPU frame time.
