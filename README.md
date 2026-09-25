# map_editor_cpp

C++23/raylib port slice of the SRC `map_editor`.

## Scope

Current implementation:

- Parses `.sbb` building files using the same header/table/cell-volume layout as the TypeScript editor.
- Parses `.sbc` city files for intersections, streets, buildings, city-block records, portal voxel block words, legacy portal texture words, portal itemset words, and portal connections.
- Parses `.sbl` block mesh faces from all three native geometry sources: per-voxel exterior faces, tessellated authored Bezier patch faces, and version-2 light-record box faces. Also parses buildblock portal layers with door/window/beam counts.
- Parses `.csx` archives for directory metadata, typed entries, names, sizes, and payload slices.
- Supports focused binary edits for SBB cell fields, SBC roads/building placements/city-block layers/portal voxel blocks, SBL words, SBL layer portal types, and CSX archive rebuilds.
- Exposes portable SBB cell, SBC building, SBL word, and SBL layer portal edit/save commands for persisted field changes.
- Launches as a no-argument editor app with folder drag/drop and native folder selection.
- Loads map-folder assets from `city2.sbc`, `building/*.sbb`, `block/*.sbl`, and `buildblock/*.sbl`.
- Provides two modes, City and Building (pieces are placed with the City Piece tool), plus the Buildblock window.
- Uses a right-mouse freecam workflow instead of orbit camera control.
- Supports City-tab road selection, endpoint/width/district edits, road duplication/deletion, viewport click-to-lay road intersections/segments, placed-building selection, X/Y/Z nudging, rotation, viewport click-to-move, Shift-click copy placement, building duplication/deletion, native portal-voxel piece placement/clearing.
- Supports Buildings-tab occupied-cell selection, SBB field nudging.
- Supports Buildblocks-tab layer selection, bounds/window/beam preview, door markers, portal-type edits, selected door/window/beam record selection, full selected-record field nudging in 1/16 units.
- Supports Blocks-tab block header inspection, width/height/depth nudging, exterior flag toggling, authored patch/control-point selection, authored x/y/z/u/v control-point nudging.
- Builds a simple 4-unit cell scene from occupied SBB cells.
- Resolves SBB piece payloads to sibling `block/*.sbl` meshes when opening an `.sbb` from a map folder.
- Resolves SBB portal-edge cells to sibling `buildblock/*.sbl` templates and overlays native north/west/south/east side codes.
- Builds portal wall strip rectangles from SBL layer bounds/windows so portal edge faces can show aperture cutouts.
- Builds a simple 4-unit city scene from SBC roads, intersections, building placements, and portal-voxel piece words, including matching `building/*.sbb` geometry and resolved `block/*.sbl` piece meshes when those names resolve to loaded files.
- Renders SBB cells/resolved piece meshes, SBC, SBL block faces/layer metadata, and CSX archive metadata views with direct raylib calls.
- Supports in-window SBB cell selection, field adjustment, and save-to-copy from the raylib viewer.
- Supports in-window SBC building selection, placement adjustment, rotation adjustment, and save-to-copy from the raylib viewer.
- Supports in-window SBL u32 word selection, adjustment, reparsed preview, and save-to-copy from the raylib viewer.
- Includes parser/scene tests against `server/data/test2/building/test.sbb`, `server/data/test2/city2.sbc`, `server/data/test2/buildblock/test.sbl`, `server/data/test2/block/astairwellfloor.sbl`, and `server/data/test2/test.csx`.

Not implemented yet:

- Native portal/buildblock mesh parity still needs deeper proof for in-game generated meshes beyond the parsed layer rectangles used by the editor/importer.
- Buildblock record insertion/deletion; current editing preserves the existing record counts and edits selected records in place.
- Authored block face record insertion/deletion and Bezier order/tessellation edits; current Blocks tab edits existing control points plus the documented load-time header.
- Native-equivalent click/drag box-brush placement workflow; current viewport support covers city roads, building placement, single-cell `SetPortalVoxelData`-style piece placement/clearing, and city-block slot selection/replacement.
- Direct text-entry editing for names and city-block slot strings.

## Build

```bash
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

If raylib is not installed, CMake fetches raylib `5.5` from its release archive.
Profiling is opt-in:

```bash
cmake --preset dev -DMAP_EDITOR_CPP_ENABLE_PROFILER=ON
cmake --build --preset dev --parallel
```

When enabled, CMake fetches pinned `yse/easy_profiler` source, the editor starts
the easy_profiler listener on the default port `28077`, and scoped blocks cover
project loading, scene/render-buffer rebuilds, GPU upload pacing, export, and
the main frame loop. Connect with easy_profiler's GUI to start/stop captures.
For a GUI-free capture, set `MAPED_PROFILE_OUT` and quit the editor normally:

```bash
MAPED_PROFILE_OUT=captures/maped.prof ./build/dev/map_editor_cpp
./build/easy_profiler_gui_v2_1_0/bin/profiler_converter captures/maped.prof captures/maped.json
```

On Windows with Visual Studio, use the `dev-msvc` preset instead. The CTest suite expects this folder to live in the SRC workspace layout, with `server/data/test2` available one directory above `map_editor_cpp`. The root workflow `.github/workflows/map_editor_cpp.yml` runs the `ci`/`ci-msvc` preset gate on macOS, Ubuntu, and Windows.

### Performance notes

- **File reads are bulk.** `read_file_bytes` sizes the file and pulls it in one
  `read()`; large CSX archives (the round map ships a ~73 MB `test.csx`) decode an
  order of magnitude faster than the old byte-at-a-time stream-iterator read.
- **Derived render inputs are cached on the project, not rebuilt per frame.** The
  block-mesh catalog, buildblock templates, and texture-name lists live in
  `EditorProject::cached_*` and are built once by `ensure_render_input_cache`. The
  grid (CSX) texture names used to re-parse every `.csx` from disk inside the
  per-frame portal render path; they are now cached and only rebuilt on load.
  Anything that edits block/buildblock data must call
  `invalidate_render_input_cache` (the `mark_*_changed` hooks already do).
- **Project loading fans out across threads.** Loose SBB/SBL parsing, CSX archive
  parsing, and PNG decode run through a bounded `parallel_for`; the order-dependent
  merges (CSX-wins upsert, index-aligned asset/byte vectors) and all GL texture
  uploads stay serial on the main thread.

## Run

```bash
./build/dev/map_editor_cpp
```

Drop a map folder into the window or click `Open Folder`. The expected folder layout is the native editor layout: `city2.sbc`, `building/*.sbb`, `block/*.sbl`, and `buildblock/*.sbl`.

### Controls

Camera: **Alt+left drag** orbits around the point under the cursor, **middle drag** pans, and the **mouse wheel** zooms toward the cursor. Freecam: hold **right mouse**, then **WASD** to move, **E/Space** up, **Q** down, **Shift** for 3x speed, and the **mouse wheel** to change fly speed. The keyboard edit shortcuts below are inactive while flying or while typing in a field.

Undo (**Ctrl+Z**, **Ctrl+Y** or **Ctrl+Shift+Z**) follows what you are editing: the Buildblock window (while focused) undoes buildblock edits, the Building and Block editors undo their own assets, and City mode undoes map edits. Each has its own history. A click or drag that changed nothing adds no undo step, and one drag or slider sweep is one step. The Building and Road tools place on mouse release; drag away before releasing to cancel.

Layout: the 3D view fills the window. A toolbar floats at its top (mode, tools, view, undo/redo) with a one-line hint for the active tool under it; an **asset drawer** along the bottom appears when a tool needs one (buildings to place, pieces to stamp, or which building to edit) and collapses with its arrow. The right-hand column holds the **Outliner** (every placed building and road) above **Properties** (the selection, or what the active tool will place). Changes since save, Keyboard shortcuts, and Preferences open as tabs beside Properties. **Ctrl+/** opens the full shortcut list.

Selection is click-to-select: a plain left-click in the Scene picks whatever is under the cursor. A selected building gets a shader silhouette outline (inverted-hull, no bloom); selected roads and pieces get a pulsing highlight.

Key labels in the help text and tooltips are localized to your keyboard layout (an AZERTY user sees ZQSD, not WASD). The keys still fire on the same physical positions on every layout — only the printed labels change.

A selected city building shows a **drag gizmo**: grab the red/green/blue axis handles to slide it on X/Y/Z, or the yellow handle to rotate it to face the cursor. **Esc** cancels an in-progress drag (it does not quit the app).

Closing the window, **Alt+F4**, or File → Exit is blocked while there are unsaved changes: a prompt offers **Save & Exit**, **Discard & Exit**, or **Cancel**. In the browser build, closing or reloading the tab with unsaved changes asks first.

Saving (**Ctrl+S**) writes the map as a pair: the chosen `.sbc` and a `.csx` asset archive with the same name. The desktop editor asks where to save (defaulting to the loaded map, and the system dialog confirms before replacing it); the browser build downloads both files.

Global: **Ctrl+1/2** switch to the City and Building modes, **Ctrl+S** saves the map, **F** frames the current selection (or the city when nothing is selected), **Ctrl+O** opens a folder, and **Ctrl+Shift+H** opens Changes since save (Ctrl reads as Cmd on macOS). The Outliner, Properties, and asset drawer toggles have no default key; use the Windows menu. The status bar at the bottom keeps the latest message; hover it for recent ones. In the browser build, preferences and window layout are kept between visits.

City tools (**1**–**5**, or the toolbar):

- **Select**: click picks whatever is nearest under the cursor (the hover outline shows it first); drag a box to select many; **Shift**/**Ctrl** (**Cmd** on macOS) adds to the selection. Double-click a building to edit its template. With a building selected: arrows move X/Z, **PgUp/PgDn** move Y, **R** rotates (**Shift+R** reverses), **Ctrl+D** duplicates, **Del** deletes. With a road selected: **`[` `]`** change the left width, **`;` `'`** the right width, **Ctrl+D** duplicates, **Del** deletes. With a piece selected: arrows and **PgUp/PgDn** move it one cell, **Del** removes it.
- **Building**: pick a template in the asset drawer (or drag one into the view). An outline and footprint box show exactly where it will land, centred on the cursor, and turn red (with the reason in Properties) when it would overlap a building or road; **R** turns it before placing. It places on mouse release; drag away first to cancel.
- **Road**: the first click marks the start (snapped to a nearby node or road; nothing is created until the next click), each further click adds a straight, axis-aligned segment (the end snaps to whichever of X/Z you moved more), and the new end node becomes the next start. New nodes take the terrain height, so roads run up and down with the land. Click the start again or press **Esc** to end the chain. New roads use the width set in Properties and continue the district of a road already at the start node.
- **Piece**: pick a piece (and its texture) in the asset drawer. A see-through ghost shows the piece, turned by **R**, in the cell a click fills; pointing at a piece's side or top targets the empty cell next to it. Drag to paint a run on the same layer.
- **Erase**: click or drag over the highlighted building, road, or piece to remove exactly that one thing.

Building editor (tools **1**–**4**; pick which building to edit in the asset drawer, and step through floors with the layers button): **Select** picks a block (**Del** removes it, **Tab/Shift+Tab** cycles blocks), **Add block** fills the cell against the clicked face (drag to keep adding along the surface), **Paint** applies the Properties material to the clicked face (drag to paint every face you pass over), and **Anchor** drags the building's anchor. In the Buildblock window **Tab/Shift+Tab** cycles layers.

The Buildings tab has a **View** switch: **Whole building** renders every floor, or **Floor by floor** shows a single floor picked with a slider plus **-/+** step buttons (selecting a cell jumps the view to that cell's floor).

The argument-based file viewer and edit commands are retained as dev/test utilities so CTest can validate binary parsing and patching behavior; they are not the main editor workflow.

Focused SBB cell edits can be made without opening the viewer:

```bash
./build/dev/map_editor_cpp --dump-sbb-cell ../server/data/test2/building/test.sbb 0
./build/dev/map_editor_cpp --set-sbb-cell ../server/data/test2/building/test.sbb 0 exterior_portal_face 12345 edited.sbb
```

Supported SBB cell fields are `exterior_block`, `interior_block`, `cell_type`, `exterior_portal_face`, `interior_portal_face`, `placed_flag`, and `item_set`. The older `exterior`, `interior`, `portal`, `edge_x`, `edge_z`, and `floor` aliases are still accepted by the edit command.

Focused SBC building edits follow the same separate-output-file pattern:

```bash
./build/dev/map_editor_cpp --dump-sbc-building ../server/data/test2/city2.sbc 0
./build/dev/map_editor_cpp --set-sbc-building ../server/data/test2/city2.sbc 0 x 4321 edited.sbc
```

Supported SBC building fields are `name`, `x`, `y`, `z`, and `rotation`.

Focused SBL word edits are available for low-level buildblock/block patching:

```bash
./build/dev/map_editor_cpp --dump-sbl-word ../server/data/test2/buildblock/test.sbl 1
./build/dev/map_editor_cpp --set-sbl-word ../server/data/test2/buildblock/test.sbl 1 77 edited.sbl
```

SBL word indices are zero-based u32 slots in the file.

Structured SBL buildblock layer portal type edits avoid raw word offsets:

```bash
./build/dev/map_editor_cpp --dump-sbl-layer ../server/data/test2/buildblock/test.sbl 1
./build/dev/map_editor_cpp --set-sbl-layer-portal ../server/data/test2/buildblock/test.sbl 1 123 edited.sbl
```

The same CMake project is intended for macOS, Linux, and Windows. Current local validation was performed on macOS; Linux and Windows are covered by the CI workflow but still need an actual workflow run before they should be treated as proven.
