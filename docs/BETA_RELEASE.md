# Pafta beta release record

Review date: 2026-10-13. Target: 0.1.0 beta.

This page keeps the checks that are done apart from the work still needed before release. Use it while preparing a release candidate, and write down the commit, platform, toolchain, map, and result for each check. For everyday use, see [Using](../README.md#open-a-map) and [Edit and save](../README.md#edit-and-save). The [Downloading](../README.md#beta-builds) section explains the beta archives.

## Checks needed before release

| Check | Passes when |
| --- | --- |
| Native Release builds | Linux, macOS, and Windows build and pass their CTest presets, with checks still on in Release. |
| Browser build | Emscripten builds the normal editor with no map and no `.data` file, and the served page starts with no console errors. |
| Release archives | Unzip each archive outside the checkout and start Pafta. Fonts, icons, dialogs, and the needed runtime libraries all work. The archives include user docs, licenses, third-party notices, and the matching full source. The source archive builds on its own after unzipping. |
| Editing and saving | Open copies of typical maps, both loose-file and CSX. Edit roads, buildings, materials, pieces, and buildblock records. Undo and redo, then save and reopen. A failed save keeps the changes marked unsaved and reports the error. Switching maps asks before throwing away edits, and a failed load keeps the current map open. |
| Browser workflow | Open or drop a folder, switch between City and Building, look at interiors up close, edit, and save. Accept every download, put the files back in the right folders, and reopen the result. |
| Game compatibility | Load saved maps in Sub Rosa. Compare geometry, roads, item sets, windows, and shadows with the same camera and time of day. |
| Release details | The tag matches the version the app shows. The release names the CPU architecture and what each build needs to run. Every program and browser archive has matching full source. The release stays marked as a prerelease. |

The GitHub Actions workflow has jobs for native build and test, coverage, the browser build, packaging, and the draft prerelease. Having a job set up is not the same as a passing run. Record the results from the final candidate here before releasing.

## Results from this preparation

A read of the source checked what the README says about controls, loading paths, output file names, old city maps, archive building, and the SBL writers. The docs now separate patches that keep every byte from full rebuilds, and separate browser download requests from files that were actually saved.

Pafta is licensed `GPL-2.0-only`, with separate notices for third-party parts. Matching source has to ship next to any released program. This preparation keeps the repository private and does not publish a release; the README explains the license.

The working tree has uncommitted changes on top of `79e57b8827de50d9ed4a1ef54e7cade92f68bc7c`. The table below records the earlier preparation snapshot, not a published tag. The audit follow-up at the end records later changes; the earlier packaged archives need rebuilding before release.

| Local check | Result |
| --- | --- |
| macOS Apple Silicon Debug and Release | After the cleanup, both CTest presets passed 17 of 17, with AppleClang 21 and the Xcode macOS SDK. |
| Emscripten 6.0.8 normal and map-preloaded test builds | Both built. The normal build includes the editor fonts and has no map `.data` package. Test data stays in the ignored build folder. |
| Browser at runtime | An earlier snapshot started normally and drew the preloaded `test2` map in City mode, including wheel zoom near buildings. The later framebuffer and cache changes have a fresh web build but still need another run in a served WebGL page. |
| Native at runtime | The current Release build loaded and drew `test2` using `PAFTA_AUTOLOAD` and `PAFTA_SCREENSHOT`. It wrote City and `base06` PNGs and exited with status 0 both times. City logged one driver warning about a Float cubemap sampler on texture unit 4 that could not load. `base06` logged no `WARNING`, `ERROR`, or `UNSUPPORTED` lines. Nobody touched the input during these runs. An earlier try inside the sandbox got stuck in macOS application services; the same run worked outside the sandbox. GPU resources were freed before the graphics context closed. |
| Parser and writer safety | Tests for broken input and failed reads and writes passed, and all 57 SBL test blocks stayed byte-identical. The parser, core, and SBB state checks also passed under AddressSanitizer and UndefinedBehaviorSanitizer, with assertions on and no sanitizer reports. |
| Direct app and state checks | The real app code refused to save when a source file was incomplete and left the bytes unchanged. It allowed item-preview warnings, kept buildblocks from archives, restored undo and redo snapshots without changing bytes, rebuilt the picking grid for a replaced buffer, refused full piece-name tables without changing anything, and handled signed junction limits. The preferences test kept the defaults for broken or non-finite values and reported a real failure to write a folder. None of these opened a window. |
| Folder upload | Node tests passed for keeping picker paths, reading folders in batches, and skipping the runtime cache. Files are copied one at a time. |
| LLVM source coverage | The rebuilt coverage target passed 17 of 17 tests and the 75% core limit. LLVM measured 93.53% line coverage for the core and 78.08% for the exporter. It also warned about profile hashes of zero in some header functions; the dump points at SBB payload comparison and accessors and the placement helpers. These numbers only cover the listed `.cpp` files, not the UI or GPU. |
| Release workflow | actionlint passed. Pages only publishes when asked to by hand. No release was made and the repository's visibility was not changed. |
| Packaged macOS and browser archives | Both archives were rebuilt from the cleaned source. The macOS ZIP unzipped outside the checkout, its command line read a test CSX, and the fonts, docs, and notices were all there. The browser ZIP's JS and WASM links matched and it had no map data. The packaged docs matched the working tree. |
| Matching source | The current code was unzipped outside the checkout, configured with FetchContent's network access turned off, and the whole editor built from the five bundled library trees. All 6 standalone tests passed. All 43 code files matched the working tree. The refreshed source archive matched all 87 project files and had all five library trees with no Git data. |

The committed base also passed the GitHub macOS, Linux, Windows, web, coverage, and Pages jobs in [run 38313850570](https://github.com/noche-x/pafta/actions/runs/38313850570). That run does not cover the uncommitted changes. Final Linux and Windows CI, and trying save, open, and undo by hand, are still required before release. Attempts to automate the folder picker and prompts did not check those steps. Screenshot hooks and direct code tests cover rendering and some app paths, but someone still has to click through save, open, and undo. Loading saved maps in the game and comparing game and editor screenshots has not been done.

The upstream raymath headers give C++ initializer warnings. The browser build also warns about Emscripten's `$0` argument syntax inside `EM_ASM`. This review does not claim the libraries build without warnings, or that every shader line is proven correct.

The City cubemap warning is still open. Reading the source suggests it comes from terrain capture, which binds cubemap zero on purpose while a cubemap sampler is active. Whether it affects reflections is not known.

## Known beta limits

Save rebuilds the asset archive, so bytes can change. Maps with old district cells keep their version until you approve a Piece upgrade; other maps are saved as SBC version 12. A new output folder may need the root `asphalt2` textures, `map.png`, and `.sis` or `.it3` files from the original map, because Save does not copy every loose game file.

Desktop Save prepares every output file and tries to roll back if replacing them fails. A crash or power cut can still leave only some files replaced, because replacing several files can never happen in one step. Leftover `.<filename>.pafta-save-*` folders can hold the original files under `old`, and recovery errors give their paths.

In the browser, the map lives in memory and Save starts separate downloads. Put each downloaded buildblock in `buildblock/`, allow multiple downloads, and check the saved folder yourself. The browser decides whether the warning on closing the tab appears, and an accepted download does not prove the file reached the disk.

Opening records and block patches can be edited in place, but their tools cannot add or delete records. Typing every name and city-block slot by hand, and the original editor's box-brush placement, are not finished. Item-set editing changes which sets a building uses, not the `.sis` files themselves, and item previews have no textures.

Built-in floor catalog IDs 16 to 19 still use flat stand-in shapes. Their real control meshes are unknown, so these shapes may not match the game. The catalog comments note this.

Nobody has proven that the editor draws things exactly like the game. The format notes separate rules taken from the game's code from unknown fields, but that is not a visual comparison. Native and WebGL rendering both need their own checks near the switch between interior and distant views, with see-through windows, and around objects that cast shadows.

Some research pages have no binary hash or point to decompiled code kept outside this repository. [The building-canvas note](RE_editor_building_canvas.md) gives the `editor.exe` hash. Checking the other addresses needs the matching original binary; the test files cannot prove where those addresses came from.

## Code and design review

The CPU code for formats, scenes, and editor state has its own library and test targets. Keeping binary parsing apart from GPU uploads makes it easy to test. The code also keeps the raw bytes for small patches and marks many rules as coming from the game, so the formats can be reviewed without guessing.

This pass followed Noche's reader-first guidelines and Ponytail for code, and plain-writing guidance for the docs. The archive workspace now lives in `ui_archive.cpp`. Native dialogs go through one NFD adapter, and checked disk writes and native UV orientation each have a single implementation. Compatibility code stays only where a platform or format really needs it. Rules we could not find in the game are written down as unknown, not treated as confirmed.

The cleanup removed unreachable record tables, an unused 2D preview, old selection helpers, and the retired Pieces shortcut. It kept the current inspectors, the buildblock canvas, the 3D preview, and the named Preferences shortcut. Comments now explain who owns which state, source-record indices, packed-file rules, and input limits.

The review also fixed bugs that a style-only pass would have missed:

- Undo keeps a restored building's version and LOD data.
- SBB word edits refresh which cells are filled and the decoded render data at the same time, including when a cell goes from solid to empty or back.
- Map scenes keep their own copies of source records and vector indices. Copying or moving a scene no longer depends on its input map staying alive, and node and road picking reject records that changed or moved.
- Placing a building refuses to grow the grid past what is supported, and refuses a full piece-name table, before changing anything. The writer refuses unsupported sizes.
- Unsigned command-line values refuse a minus sign, and word offsets and city-level edits cannot wrap around.
- Geometry buffers own their picking and highlight caches. Scene, font, reflection, and worker results are keyed on the state that actually decides their contents.
- Exported texture file names stay different after name cleanup. Failed readbacks, exports, framebuffers, and preference writes report errors.
- Erase acts on the building under the mouse, box selection clears an old active voxel, and adding a buildblock record keeps the new record selected.
- A newly opened map cannot take over while an export is still reading textures from the current one. Failed loads keep the current project.

`app.cpp` still mixes loading, saving, editing actions, and the frame loop, so following one action often means jumping between those parts. Pull out a whole operation when that makes the order of steps clearer or gives a shared rule a single home. File length alone is not a reason to split it, and a big refactor right before beta would risk new bugs without testing the user workflows.

Renderer caches and the kept source bytes are copies of state on purpose. Every edit has to keep their cache clearing, unsaved flags, undo state, and saved model in step. The CPU tests do not cover every interactive path, so save and reopen, and undo and redo, still have to be checked before release.

The scene-first UI has map modes, an asset browser, properties, shortcuts, and preview time. Reading the source confirms those controls exist. Whether the layout is good, easy to find your way around, and accurate to the game can only be judged by using the candidate at the supported window sizes.

## Audit follow-up

The later local audit fixes retain loaded archive and texture payloads across Save As, keep the archive workspace separate from map conversion, expose its unsaved state to the close workflow, and request browser downloads for archive outputs. Uploads keep the latest request and release cancelled, failed, or replaced project folders. Building renames have undo and redo steps. Buildblock writes preserve the loaded header; CSX reads and writes share a 512 MiB payload limit, and portal coordinates use a checked cell conversion. Project I/O and history now have their own source files, and the unused editor-state classes are gone.

The updated macOS Release build passed all 18 CTest checks with warnings treated as errors for Pafta code. Debug and sanitizer checks also passed 18 of 18; AddressSanitizer leak detection was disabled. Homebrew Clang 22 passed the four bounded parser fuzz tests, and clang-tidy passed the CPU and archive checks. LLVM measured 93.81% line and 81.26% branch coverage on the CPU core, and 79.29% line and 54.62% branch coverage on the exporter. These percentages exclude the interactive UI and GPU renderer.

Emscripten 6.0.8 built the normal browser editor without map data. A local Chrome test passed WASM startup, WebGL2, canvas focus, and frame-loop checks. A separate test opened two synthetic maps and verified that the first upload folder disappeared while the second remained, with no runtime errors. The Save shortcut requested both `test.csx` and `city2.sbc` downloads for the synthetic map. The JavaScript upload tests also cover out-of-order completion and cleanup after read failures.

The browser tests do not establish that every archive download reaches disk or that every save and exit prompt works when clicked by a person. Manual archive-download and exit-prompt checks, fresh Linux and Windows builds, rebuilding the release archives, and loading the saved maps in Sub Rosa remain required before release. No release or commit was made.
