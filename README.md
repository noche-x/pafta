# Pafta

Pafta (pronounced PAHF-tah) is a C++23 map editor for Sub Rosa.

Pafta is in beta. Everything is subject to change at any moment.

## Building

Every platform needs CMake 3.24 or newer and a C++23 compiler. CMake uses an installed raylib when it finds one; otherwise it fetches raylib 5.5.

### Windows

Install Visual Studio 2022 with the C++ desktop workload, then run from a Developer PowerShell:

```sh
cmake --preset dev-msvc
cmake --build --preset dev-msvc --parallel
ctest --preset dev-msvc
.\build\dev-msvc\Debug\map_editor_cpp.exe
```

### macOS

Install the Xcode command-line tools and CMake (for example `brew install cmake`), then:

```sh
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
./build/dev/map_editor_cpp
```

### Linux

Install the X11, OpenGL, ALSA, and GTK 3 development packages. On Ubuntu:

```sh
sudo apt-get install -y build-essential cmake libasound2-dev libgl1-mesa-dev libglu1-mesa-dev \
  libgtk-3-dev libx11-dev libxcursor-dev libxext-dev libxi-dev libxinerama-dev \
  libxrandr-dev mesa-common-dev xorg-dev
```

Then build with the same commands as macOS.

### Web (Emscripten)

With Emscripten 6.0.8 active:

```sh
EM_CACHE="$PWD/build/web/emcache" emcmake cmake -S . -B build/web \
  -DPLATFORM=Web -DGRAPHICS=GRAPHICS_API_OPENGL_ES3 \
  -DMAP_EDITOR_CPP_BUILD_TESTS=OFF -DMAP_EDITOR_CPP_ENABLE_PROFILER=OFF
EM_CACHE="$PWD/build/web/emcache" EMCC_BATCH_BUILD=0 cmake --build build/web --target map_editor_web --parallel
```

Serve `build/web` over HTTP and open `map_editor_web.html`; browsers do not run it from `file://`. A normal browser build contains no map data. [docs/WEB_BUILD.md](docs/WEB_BUILD.md) covers GitHub Pages and local QA builds with a bundled map.

### Tests

A standalone checkout runs the portable parser, scene, editor-state, and export tests. If `../server/data/test2` exists, CTest also runs the full fixture and command-line edit suite. Set `MAP_EDITOR_CPP_FIXTURE_ROOT` to another SRC workspace root if needed.

## Open a map

Drop a map folder onto the window or choose **File → Open Folder**. Pafta reads `city2.sbc`, loose assets under `building/`, `block/`, and `buildblock/`, plus assets in `.csx` archives. The Building editor also lists `.sis` item-set names found in `itemset/` in the map folder or beside it. If the folder has one `.sbc` with another name, the loader can use it; Save still writes `city2.sbc`.

## Edit and save

The City editor handles roads, placed buildings, and pieces. The Building editor edits building cells and materials. The Block and Buildblock tools expose mesh and layer records. The viewport shows placement previews, selection, and the current scene; exact game rendering remains a work in progress.

**Ctrl+S** opens a folder picker on desktop. Save writes `city2.sbc` and `test.csx` in that folder. It also writes edited `buildblock/*.sbl` files; when saving to another folder, it writes all loaded buildblocks there. These files are written directly, one after another, so a failed write can leave a mixed set. Save to a copy when the originals matter. In the browser, Save starts separate downloads for the city, archive, and any buildblock files.

Pafta keeps separate undo histories for city edits and for the Building, Block, and Buildblock editors. Closing a dirty project opens a Save, Discard, or Cancel prompt. The browser asks before the tab closes or reloads.

### Main controls

| Action | Control |
| --- | --- |
| Orbit, pan, zoom | Alt + left drag, middle drag, mouse wheel |
| Free camera | Hold right mouse; WASD moves, E/Space rises, Q descends |
| Save, open, frame selection | Ctrl+S, Ctrl+O, F |
| Undo, redo | Ctrl+Z, Ctrl+Y or Ctrl+Shift+Z |
| City tools | Keys 1–5 or the toolbar |
| Building tools | Keys 1–5 or the toolbar; 5 selects Item set |
| Full shortcut list | Ctrl+/ |

Shift changes free-camera speed. Editing shortcuts pause while you fly or type in a field. On macOS, the app shows Cmd where appropriate; key labels also follow the current keyboard layout. The toolbar and the in-app shortcut list describe the active tools in more detail.

In City, **Select** picks and moves existing roads, buildings, or pieces. **Building** previews a template and places it on mouse release; red means the position conflicts with another object. Drag away before release to cancel that placement. **Road** starts at the first click and adds axis-aligned segments with later clicks. **Piece** places or paints cells from the chosen asset, while **Erase** removes the highlighted object. Esc ends a road chain or cancels a gizmo drag.

In Building, **Select** picks a block, **Add block** fills a neighboring cell, **Paint** applies the chosen material, **Anchor** moves the building anchor, and **Item set** places or clears a set on a clicked block. Add a `.sis` name to the building in Properties, then choose its rotation. Placed sets show their items from `item/*.it3`, untextured; a set with no item meshes shows a yellow diamond. Save writes their names and placements into the building data in `test.csx`; it does not copy `.sis` or `.it3` files. The floor control can show one floor or the whole building. The Block and Buildblock editors expose the selected record's fields. **Windows → Map image** opens the top-down map view.

## File support and limits

Pafta reads `.sbb` building volumes, `.sbc` city maps, `.sbl` blocks and buildblocks, and `.csx` asset archives. It can edit item-set references in buildings and previews their `.it3` item meshes, but does not edit `.sis` definitions. The byte layouts, version ranges, writer behavior, and unresolved fields are recorded in [docs/file_formats/](docs/file_formats/README.md).

Current limits include:

- Game-equivalent rendering of every generated portal and buildblock mesh has not been proven.
- Buildblock door, window, and beam records can be edited in place, but the editor does not insert or delete those records.
- The Block editor changes existing patch control points and header fields; it does not insert or delete authored face records.
- Name and city-block slot editing do not yet have direct text entry.
- The viewport does not reproduce the native box-brush placement workflow.

The command-line file viewer and patch commands remain available for development and tests. They take separate input and output paths, for example:

```sh
./build/dev/map_editor_cpp --dump-sbb-cell ../server/data/test2/building/test.sbb 0
./build/dev/map_editor_cpp --set-sbb-cell ../server/data/test2/building/test.sbb 0 exterior_portal_face 12345 edited.sbb
./build/dev/map_editor_cpp --dump-sbc-building ../server/data/test2/city2.sbc 0
./build/dev/map_editor_cpp --set-sbc-building ../server/data/test2/city2.sbc 0 x 4321 edited.sbc
```

The SBL commands are `--dump-sbl-word`, `--set-sbl-word`, `--dump-sbl-layer`, and `--set-sbl-layer-portal`. Their arguments are listed in `src/main.cpp`. These utilities inspect specific fields; they do not replace a full map save.

## Development

Read [CONTRIBUTING.md](CONTRIBUTING.md) before changing behavior and [STYLE_GUIDE.md](STYLE_GUIDE.md) for the code and documentation rules. The format specifications live under [docs/file_formats/](docs/file_formats/README.md); [docs/WEB_BUILD.md](docs/WEB_BUILD.md) covers Emscripten.

CI builds and tests Linux, macOS, and Windows, builds the browser editor, and attaches beta build archives to the run. A `v*` tag creates a draft prerelease with the four platform archives after the checks pass. LLVM source coverage runs on Linux. On a push to the repository's default branch, the same browser build is deployed to GitHub Pages after the native tests, coverage gate, and browser build pass. The Pages site serves the editor directly and contains no bundled map data. Repository setup and the expected site URL are in [docs/WEB_BUILD.md](docs/WEB_BUILD.md).

To generate the same coverage report locally with Clang and matching `llvm-profdata` and `llvm-cov` tools:

```sh
bash tests/coverage.sh
```

The script writes `build/coverage/coverage.txt`, `coverage-export.txt`, and `coverage-html/`. Its 75% line gate covers the four CPU core files; the glTF exporter has a separate report. The gate does not include the interactive UI or GPU renderer. The binary-export suite compares frozen small files and scans real map fixtures when available. It reports exact exports separately from files whose first full rebuild normalizes bytes, and checks that a second export is stable.

Project loading parses loose assets in bounded worker batches, then merges results in order; GPU uploads stay on the render thread. Derived render inputs are cached, and city geometry uses instanced draws. Changes to block or buildblock data must invalidate the matching render caches. For a repeatable desktop timing run after a scene settles:

```sh
MAPED_AUTOLOAD=/path/to/map-folder MAPED_BENCH=300 ./build/dev/map_editor_cpp
```

Profiling is optional. Configure with `-DMAP_EDITOR_CPP_ENABLE_PROFILER=ON`, then set `MAPED_PROFILE_OUT` when running the editor to save a capture on normal exit.

## AI disclosure

AI tools have been used with parts of Pafta's code, research, and documentation. Every change made with AI tools has been checked. A passing build or fixture test also does not prove behavior in the game. Contributions that materially use AI should say what the tool helped produce and what the author checked; [CONTRIBUTING.md](CONTRIBUTING.md) explains what to report.
