# Pafta

Pafta (prounounced PAHF-tah) is a map editor for Sub Rosa, written in C++23.

Pafta is in beta. Backup your maps before working on them with Pafta, and test saved maps in Sub Rosa before you share them. The editor view can look different from the game.

- [Downloading](#beta-builds)
- [Building](#building)
- [Using](#open-a-map)
- [Edit and save](#edit-and-save)
- [File support and limits](#file-support-and-limits)
- [Development](#development)

## Artifact builds

GitHub CI makes `pafta-linux.zip`, `pafta-macos.zip`, `pafta-windows.zip`, `pafta-web.zip`, and the matching `pafta-source.tar.gz`. Unzip the desktop archive and run `pafta` (`pafta.exe` on Windows).

The browser build has to be served over HTTP. Open its `index.html`. [docs/WEB_BUILD.md](docs/WEB_BUILD.md) explains how to serve it locally and how to deploy it. [docs/BETA_RELEASE.md](docs/BETA_RELEASE.md) lists the release checks and known limits.

## Building

Requirements:

- CMake 3.24 or newer.
- A C++23 compiler (recommended: GCC 13 or newer, Clang 17 or newer, or MSVC from Visual Studio 2022 17.8 or newer, AppleClang 21 has been tested on macOS).
- The system packages listed below for your platform.
- Emscripten for a browser build.

CMake downloads dependencies automatically through `FetchContent` when you first configure a Git checkout.

- [raylib](https://github.com/raysan5/raylib) 5.5: graphics and windowing.
- [Dear ImGui](https://github.com/ocornut/imgui) and [rlImGui](https://github.com/raylib-extras/rlImGui): UI and its raylib integration, pinned to commits in `CMakeLists.txt`.
- [tinygltf](https://github.com/syoyo/tinygltf) 2.9.3: desktop glTF export.
- [Native File Dialog Extended](https://github.com/btzy/nativefiledialog-extended) 1.3.0: desktop file dialogs.

The source archive includes these libraries in `third_party/`, and CMake uses them automatically. Enabling profiling also downloads [easy_profiler](https://github.com/yse/easy_profiler), pinned in `CMakeLists.txt`.

### Quick build scripts

Once the requirements for your platform (below) are installed, the scripts do the rest:

| | macOS and Linux | Windows (Developer Command Prompt) |
| --- | --- | --- |
| Debug build | `./build.sh` | `build.bat` |
| Build and start | `./build.sh run` | `build.bat run` |
| Build and test | `./build.sh test` | `build.bat test` |
| Release build | `./build.sh release` | `build.bat release` |
| Browser build | `./build.sh web` | |
| Delete build folders | `./build.sh clean` | `build.bat clean` |

The first run configures the build folder and later runs only compile what changed. On macOS, `build.sh` uses the selected Xcode SDK, because some Command Line Tools SDKs fail to link. The sections below show the same steps as plain CMake commands.

### Windows

Install Visual Studio 2022 with the C++ desktop workload. Then run this from a Developer PowerShell:

```sh
cmake --preset dev-msvc
cmake --build --preset dev-msvc --parallel
ctest --preset dev-msvc
.\build\dev-msvc\Debug\pafta.exe
```

The `dev-msvc` preset selects Visual Studio 2022. For Visual Studio 2026, install its C++ desktop workload and CMake 4.2 or newer, then run these commands from its Developer PowerShell:

```sh
cmake -S . -B build/dev-vs2026 -G "Visual Studio 18 2026" -A x64 -DPAFTA_BUILD_TESTS=ON
cmake --build build/dev-vs2026 --config Debug --parallel
ctest --test-dir build/dev-vs2026 -C Debug --output-on-failure
.\build\dev-vs2026\Debug\pafta.exe
```

Here, `-S` selects the source folder and `-B` selects the build folder. For CTest, `-C Debug` selects the build configuration; for CMake, `-C` instead loads an initial cache script and is not needed here. To build Release, replace `Debug` with `Release` in the build, test, and executable commands. Use a separate build folder for each Visual Studio generator. The 2026 commands follow CMake's generator documentation but have not been tested with Pafta.

### macOS

Install the Xcode command-line tools and CMake (for example with `brew install cmake`). Then run:

```sh
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
./build/dev/pafta
```

### Linux

Install the X11, OpenGL, ALSA, and GTK 3 development packages. On Ubuntu:

```sh
sudo apt-get install -y build-essential cmake libasound2-dev libgl1-mesa-dev libglu1-mesa-dev \
  libgtk-3-dev libx11-dev libxcursor-dev libxext-dev libxi-dev libxinerama-dev \
  libxrandr-dev mesa-common-dev xorg-dev
```

Then build with the macOS commands.

### Web (Emscripten)

With Emscripten 6.0.8 active:

```sh
EM_CACHE="$PWD/build/web/emcache" emcmake cmake -S . -B build/web \
  -DPLATFORM=Web -DGRAPHICS=GRAPHICS_API_OPENGL_ES3 \
  -DPAFTA_BUILD_TESTS=OFF -DPAFTA_ENABLE_PROFILER=OFF
EM_CACHE="$PWD/build/web/emcache" EMCC_BATCH_BUILD=0 cmake --build build/web --target pafta_web --parallel
```

Serve `build/web` over HTTP and open `pafta_web.html`. Browsers will not run it from a `file://` link. A normal browser build has no map data in it. [docs/WEB_BUILD.md](docs/WEB_BUILD.md) covers GitHub Pages and local test builds that include a map.

### Tests

A plain checkout runs the parser, scene, editor-state, and export tests that need no game data. If `../server/data/test2` exists, CTest also runs the larger suite that uses real map files and command-line edits. If your SRC workspace is somewhere else, point `PAFTA_FIXTURE_ROOT` at it.

### Package a release candidate

After a native Release build, install the same component CI uses:

```sh
cmake --preset ci
cmake --build --preset ci --parallel
ctest --preset ci
cmake --install build/ci --config Release --prefix build/package --component Pafta
python3 scripts/package_source.py build/ci build/pafta-source.tar.gz
```

On Windows, use `ci-msvc` and `build/ci-msvc`. Ship the source archive next to the program archives. It holds the source tree, the library sources it needs, the licenses, and `SOURCE_REVISION.txt`. Make it again whenever the candidate changes.

## Open a map

Drop a map folder onto the window, or choose File → Open map folder. Pafta reads `city2.sbc`, the loose files in `building/`, `block/`, and `buildblock/`, and the files inside `.csx` archives. The Building editor also lists the `.sis` item sets it finds in `itemset/`, either inside the map folder or next to it. If the folder has a single `.sbc` file with a different name, Pafta opens that one, but Save still writes `city2.sbc`.

When an asset is both loose and in `test.csx`, the archive copy wins by default, as in the game. File → When loading, prefer → Loose folders first makes the loose file win instead; Reopen this map applies the change to the open map. A map always opens in the City view.

## Edit and save

There are four editors:

- City: roads, placed buildings, and pieces.
- Building: building cells and materials.
- Block and Buildblock: the mesh and layer records.

The viewport shows the scene, what is selected, and a preview of what you are about to place. It does not yet match the game's rendering exactly.

### Saving

On desktop, Ctrl+S asks for a folder. Save writes `city2.sbc` and `test.csx` there. It also writes `buildblock/*.sbl` files for every buildblock you edited and every buildblock that came from an archive. If you save to a different folder, it writes all loaded buildblocks.

Save prepares every file before it replaces anything. If replacing fails, it tries to put the old files back, and if that fails too, the error message says where the backups are. A crash or power cut can still stop a save halfway through several files, so keep copies.

Save rebuilds the asset archive, so the bytes can change even when the content does not. Old city maps with district cells keep their version until you agree to a Piece edit that upgrades them. Other city maps are saved as version 12. Save does not copy `asphalt2.png`, `asphalt2_material.png`, `map.png`, or loose `.sis` and `.it3` files, so a new folder may need those copied over by hand.

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
| Free camera | Hold right mouse; WASD moves where you look, Space goes up, Ctrl goes down |
| Context menu | Quick right-click on the view, a list row, or an asset |
| Save, open, frame selection | Ctrl+S, Ctrl+O, F |
| Undo, redo | Ctrl+Z, Ctrl+Y or Ctrl+Shift+Z |
| City tools | Keys 1–5 or the toolbar |
| Building tools | Keys 1–5 or the toolbar; 5 is Item set |
| All shortcuts | Ctrl+/ |

Turn off Blender-style mouse in Preferences to make the middle button pan again. In the free camera, Shift slows you down and Z speeds you up. The mouse wheel changes flying speed while you hold the right button. Editing shortcuts are off while you fly or type in a field. On macOS, the labels say Cmd where it applies, and key labels follow your keyboard layout. The toolbar and the shortcut list (Ctrl+/) explain each tool in more detail.

Every panel is a window you can move, dock anywhere around the view, or close; Pafta remembers the layout and which windows are open. Windows → Reset window layout puts them back.

### City tools

- Select picks and moves roads, buildings, and pieces.
- Building shows a preview of a building and places it when you let go of the mouse. Red means it overlaps something. Drag away before you let go to cancel.
- Road starts at your first click and adds a straight segment with each click after that.
- Piece places or paints cells using the chosen asset.
- Erase removes the highlighted object.

Esc ends a road or cancels a gizmo drag.

### Building tools

- Select picks a block.
- Add block fills the cell next to it.
- Paint applies the chosen material.
- Anchor moves the building's anchor point.
- Item set places or clears an item set on the block you click.

To use an item set, first add its `.sis` name to the building in Properties, then pick a rotation. Placed sets show their items from `item/*.it3` without textures. A set with no item meshes shows as a yellow diamond. Save writes the set names and positions into the building data in `test.csx`. It does not copy the `.sis` or `.it3` files.

The floor control shows one floor or the whole building. A room's Floor setting (in Add block and on the selected block) leaves out its floor, which also removes the ceiling of the room below, so stacked rooms join into one tall room. editor.exe always writes a floor.

The Buildblock window (Ctrl+L) draws each face as the game builds it: grey wall around the opening, glass windows, beams, and door markers. Drag items and corners on the square canvas; right-click to add, copy, or delete. Exterior and Interior set the face's portal type, as the same buttons do in editor.exe.

In Block mode, drag the arrows to move the selected patch or box, the white cubes to resize a box, and the orange dots to move patch points. G, S, and R move, scale, and rotate; X, Y, or Z locks an axis; a click or Enter keeps the change and Esc cancels it.

View → Interiors chooses when buildings show their rooms: never, up close like the game, or only for the selected buildings. The Block and Buildblock editors show the fields of the selected record. Windows → Map image opens a top-down view of the map. File → CSX archive lists the map's assets and refreshes itself as you edit. It has a filter, Shift and Ctrl selection, hover previews, and Extract all, and it writes `test.csx`, the only archive name the game reads.

### Time of day

View → Time of day moves the sun in the preview. The same setting is in Preferences. It starts at 11:00 and follows the game's World-mode sun. It only changes the viewport, not the map's time in game.

## File support and limits

Pafta reads `.sbb` buildings, `.sbc` city maps, `.sbl` blocks and buildblocks, and `.csx` asset archives. It can edit which item sets a building uses and show their `.it3` item meshes, but it cannot edit `.sis` files. [docs/file_formats/](docs/file_formats/README.md) has the byte layouts, supported versions, how saving works, and the fields nobody understands yet.

Current limits:

- Nobody has proven that every generated portal and buildblock mesh looks the same as in the game.
- You can edit buildblock door, window, and beam records, but not add or delete them.
- The Block editor changes existing control points and header fields. It cannot add or delete faces.
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

## Development

Read [CONTRIBUTING.md](CONTRIBUTING.md) before changing how the editor behaves, and [STYLE_GUIDE.md](STYLE_GUIDE.md) for how to write code and docs here. The [documentation index](docs/README.md) links the file formats, browser builds, and older research notes. [docs/BETA_RELEASE.md](docs/BETA_RELEASE.md) lists the beta checks, known limits, and what still needs testing.

CI builds and tests Linux, macOS, and Windows, builds the browser editor, and attaches the beta archives to each run. Linux also runs LLVM source coverage, AddressSanitizer, UndefinedBehaviorSanitizer, bounded parser fuzzing, and focused clang-tidy checks. The browser job starts the compiled editor in Chromium and checks its WebGL2 context and frame loop. Pushing a `v*` tag makes a draft prerelease with the four platform archives and the source archive once the checks pass. GitHub Pages only publishes when someone starts the workflow by hand on the default branch with `publish_pages` turned on, and only after all checks pass. The Pages site runs the editor and has no map data. [docs/WEB_BUILD.md](docs/WEB_BUILD.md) explains the repository setup and the site address.

To make the same coverage report locally, with Clang and matching `llvm-profdata` and `llvm-cov`:

```sh
bash tests/coverage.sh
```

The script writes `build/coverage/coverage.txt`, `coverage-export.txt`, `coverage-workflows.txt`, and `coverage-html/`, including branch counts. It requires 75% line and 70% branch coverage on the four CPU core files, and 75% line and 50% branch coverage on the glTF exporter. A separate function report covers Save and city history; its percentages do not describe the whole application. Inline headers, the interactive UI, and the GPU renderer are outside these coverage gates. The binary-export tests compare small saved files byte for byte, and also scan real maps when they are available. They report files that export exactly separately from files whose first rebuild changes some bytes, and they check that a second export does not change anything. The workflow tests load a synthetic map, edit it, save and reopen it, undo and redo across a save, and check that failed saves preserve unsaved changes.

For the native quality checks, use a full Clang installation with libFuzzer and clang-tidy:

```sh
cmake --preset quality
cmake --build --preset quality --parallel
ctest --preset quality
bash tests/check_cpp.sh
```

The quality build treats Pafta compiler warnings as errors and runs the native tests under ASan and UBSan. Each of the SBB, SBC, SBL, and CSX fuzz tests runs for 15 seconds, starts with a synthetic valid file, and keeps its corpus and crash inputs in `build/quality/fuzz/`. Apple Clang does not ship libFuzzer; with Homebrew LLVM, configure on macOS using `cmake --preset quality -DCMAKE_CXX_COMPILER="$(brew --prefix llvm)/bin/clang++" -DCMAKE_OSX_SYSROOT="$(xcrun --sdk macosx --show-sdk-path)"`. No game files are needed for these checks.

LLVM can still warn about zero-hash mappings for unused inline header functions when profiles from several executables are merged. `llvm-cov report --dump` identifies those functions. Keep the warning visible and inspect new mismatches; the coverage gates above measure the listed `.cpp` files.

Loading keeps the encoded archive and texture payloads in memory, so later saves do not depend on the original archive staying on disk. Reopen the map to pick up external asset changes.

Loading a map parses loose files in small batches on worker threads, then merges the results in order. GPU uploads stay on the render thread. Render inputs are cached, and city geometry is drawn with instancing. Any change to block or buildblock data has to clear the matching render caches. To time the desktop app the same way each run, once the scene has finished loading:

```sh
PAFTA_AUTOLOAD=/path/to/map-folder PAFTA_BENCH=300 ./build/dev/pafta
```

Profiling is optional. Configure with `-DPAFTA_ENABLE_PROFILER=ON`, then set `PAFTA_PROFILE_OUT` when you run the editor. It saves a capture when the editor closes normally.

## AI disclosure

I'll be upfront: AI helped write a substantial part of Pafta. Reconstructing Sub Rosa's file formats and editor behavior is a lot of work, and I'm one person with limited time. I used AI to help get the editor built, and I take responsibility for the result.

I'm reviewing the code, fixing mistakes, and cutting things that don't belong. Generated code doesn't get a pass because it compiles. I check changes against the format evidence and regression tests we have; where game behavior or browser workflows still need verification, the docs say so. I won't claim tests prove more than they do.

If using AI this way puts you off, you're free to skip Pafta. If you find broken behavior, confusing code, or something unnecessary, please open an issue and point to it. That's useful feedback, and I'll act on it.

This is still a beta. As I work through the code, I intend to replace generated implementations with code I understand and can maintain myself. That takes time. [CONTRIBUTING.md](CONTRIBUTING.md) explains what contributors should disclose and verify.

## License

Pafta's source code is licensed under [GNU GPL version 2 only](LICENSE) (`GPL-2.0-only`). If you share a modified version, it has to stay under the GPL, and if you share a binary, you have to offer the matching full source and build scripts as the license describes. You do not have to publish changes you keep to yourself. The [GNU GPL FAQ](https://www.gnu.org/licenses/gpl-faq.en.html#GPLRequireSourcePostedPublic) explains the difference.

Third-party libraries and bundled fonts keep their own licenses, listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Preparing a release does not make the repository public or publish anything.
