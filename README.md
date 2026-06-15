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
- Provides raygui tabs for City, Buildings, Pieces, Buildblocks, and Blocks.
- Uses a right-mouse freecam workflow instead of orbit camera control.
- Supports City-tab road selection, endpoint/width/district edits, road duplication/deletion, viewport click-to-lay road intersections/segments, placed-building selection, X/Y/Z nudging, rotation, viewport click-to-move, Shift-click copy placement, building duplication/deletion, native portal-voxel piece placement/clearing, and save-to-copy as `city2.edited.sbc`.
- Supports Buildings-tab occupied-cell selection, SBB field nudging, and save-to-copy as `*.edited.sbb`.
- Supports Pieces-tab SBC city-block layer/slot inspection, resolved `block/*.sbl` mesh preview, viewport slot selection, Shift-click slot replacement from loaded block names, clearing, and save-to-copy through `city2.edited.sbc`.
- Supports Buildblocks-tab layer selection, bounds/window/beam preview, door markers, portal-type edits, selected door/window/beam record selection, full selected-record field nudging in 1/16 units, and save-to-copy as `*.edited.sbl`.
- Supports Blocks-tab block header inspection, width/height/depth nudging, exterior flag toggling, authored patch/control-point selection, authored x/y/z/u/v control-point nudging, and save-to-copy as `*.edited.sbl`.
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

On Windows with Visual Studio, use the `dev-msvc` preset instead. The CTest suite expects this folder to live in the SRC workspace layout, with `server/data/test2` available one directory above `map_editor_cpp`. The root workflow `.github/workflows/map_editor_cpp.yml` runs the `ci`/`ci-msvc` preset gate on macOS, Ubuntu, and Windows.

## Run

```bash
./build/dev/map_editor_cpp
```

Drop a map folder into the window or click `Open Folder`. The expected folder layout is the native editor layout: `city2.sbc`, `building/*.sbb`, `block/*.sbl`, and `buildblock/*.sbl`.

### Controls

Camera (freecam): hold **right mouse**, then **WASD** to move, **Space/Shift** for up/down, **Ctrl** for 2x speed, and the **mouse wheel** to change fly speed. The keyboard edit shortcuts below are inactive while flying or while typing in a field.

Selection is click-to-select: a plain left-click in the viewport selects whatever is under the cursor, and placement actions sit on modifiers so they no longer fight selection. The selected building, road, or piece gets a pulsing glow outline. The Inspector buttons remain as a secondary path and show their hotkey in tooltips; a collapsible **Controls** section at the top of the Inspector lists the bindings for the current tab/mode.

Key labels in the help text and tooltips are localized to your keyboard layout (an AZERTY user sees ZQSD, not WASD). The keys still fire on the same physical positions on every layout — only the printed labels change.

A selected city building shows a **drag gizmo**: grab the red/green/blue axis handles to slide it on X/Y/Z, or the yellow handle to rotate it to face the cursor. **Esc** cancels an in-progress drag (it does not quit the app).

Closing the window, **Alt+F4**, or File → Exit is blocked while there are unsaved changes: a prompt offers **Save & Exit**, **Discard & Exit**, or **Cancel**. Saving writes the usual `*.edited.*` copies.

Global: **Ctrl+S** saves the active tab's `*.edited.*` copy, **F** focuses the camera on the city, **Ctrl+O** opens a folder.

City tab:

- **Buildings** mode: click selects a placement; **Shift+click** moves the selected one to the cursor; **Ctrl+Shift+click** stamps a copy. Arrows move X/Z, **PgUp/PgDn** move Y, **R** rotates (**Shift+R** reverses), **Ctrl+D** duplicates, **Del** deletes.
- **Roads** mode: click selects a road; **Shift+click** adds a road point/intersection. Arrows adjust the from/to nodes, **`[` `]`** the left width, **`;` `'`** the right width, **`-` `=`** the district, **Ctrl+D** duplicates, **Del** deletes.
- **Pieces** mode: **Shift+click** places a voxel, **Ctrl+click** clears it, **R** rotates the voxel before placing.

Buildings / Blocks / Buildblocks tabs: **Left/Right** nudge the selected field, **Up/Down** take the coarser step (and cycle the active field on the Buildings tab), **Tab/Shift+Tab** cycle the selected cell/field/layer record, and **Ctrl+S** saves the `*.edited.*` copy.

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
