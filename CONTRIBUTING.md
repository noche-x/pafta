# Contributing to Pafta

Pafta is in beta. A change can affect the files people save or how a map looks in the game, so start from behavior you can check. The [usage guide](docs/USAGE.md) explains how the editor works today and its limits. [STYLE_GUIDE.md](STYLE_GUIDE.md) explains how code here is written.

## Before you edit

Find where the behavior starts, then follow the calls that read state and write results. The main parts are:

| Area | Files |
| --- | --- |
| Desktop and command-line entry points | `src/main.cpp`, `src/app.cpp` |
| Project loading and saving | `src/project_io.cpp` |
| Binary formats and small field edits | `src/formats.hpp`, `src/formats.cpp` |
| Editor state and undo history | `src/edit_history.cpp`, `src/editor_state.*`, `src/app.cpp` |
| Building the scene | `src/scene.*`, `src/buildblock_catalog.*` |
| Rendering and UI | `src/render_gpu.cpp`, `src/viewport.cpp`, `src/ui_*.cpp` |

Before changing a file format, read its page in [docs/file_formats/](docs/file_formats/README.md) and look at a real file. Keep three things apart: fields we know, evidence from the game's code, and bytes we don't understand. A value you saw in one file is not a rule of the format.

Keep each change small. If it touches parsing, editing, and saving, follow the whole path so the editor never shows one value and saves another. Leave unrelated bytes and map data alone. If the input can't be handled safely, fail with a clear error. Never report an unchanged file as a successful edit.

## Build and test

From the repository root:

```sh
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

On Windows, use the `dev-msvc` preset. Format, scene, editor-state, export, and Save/history tests that need no game data run in any checkout. The larger suite runs when `../server/data/test2` exists; if your SRC workspace is somewhere else, set `PAFTA_FIXTURE_ROOT`. The CI presets build Release on macOS, Linux, and Windows, so test checks must stay on in Release. CTest covers the parsers, scene building, state edits, binary and glTF export, Save/reopen and failed Save, undo/redo, and command-line field edits when their test files are there. Chromium checks browser startup, WebGL2, canvas focus, and continued frames. These checks do not prove viewport appearance, browser downloads, or parity with the game.

For LLVM source coverage, run `bash tests/coverage.sh` with Clang and matching `llvm-profdata` and `llvm-cov` on your path. The reports and HTML go in `build/coverage/`. CI requires 75% line and 70% branch coverage on the four CPU core files, and 75% line and 50% branch coverage on the glTF exporter. These are floors, not a correctness score. Read missed branches and test decision combinations in the function you changed. The Save/history report is separate; UI and GPU changes still need checking in the app itself.

The `quality` preset builds the native tests with AddressSanitizer and UndefinedBehaviorSanitizer, rejects compiler warnings in Pafta code, and adds four 15-second parser fuzz tests. Use a full Clang installation with libFuzzer; [README.md](README.md#development) gives the commands and the macOS toolchain note. Run `bash tests/check_cpp.sh` after building to check the CPU parsing, state, scene, Save, and export paths with clang-tidy. Keep sanitizer errors fatal. A parser fuzzer catches only the `runtime_error` used to reject external input; it must not hide allocation failures or other unexpected exceptions.

When fixing a boundary check, try the opposite comparison in a temporary source copy and confirm that a relevant test fails. [Jason Turner's mutation-testing example](https://www.youtube.com/watch?v=lhcXAnNgzlo) explains why this is useful. Do this for a specific risky rule rather than mutating the whole application on every build. If a fuzzer finds a crash, reduce its input and add a deterministic regression test before fixing it.

Pick checks that fit the change:

- Binary reader or writer: add a test file, or a small built-in case, that covers the edge or the round trip. Compare exact bytes for formats that promise to save without loss. If a full rebuild changes some bytes in a supported file, report it and check that a second export changes nothing. A patch that changes nothing must give identical bytes.
- UI or rendering: build it and try the changed workflow in the app. When comparing with the game, use the same map, camera, and time of day.
- Browser: follow [docs/WEB_BUILD.md](docs/WEB_BUILD.md), serve the build over HTTP, and try it in a real browser. `PAFTA_WEB_PRELOAD_PROJECT` is for local testing only; never ship its map data.
- Docs only: check what the text says against the current code and read the Markdown diff. A native build proves nothing about prose.

When you hand off a change, say what you ran and what is still untested. A test on your machine, a clean link, a screenshot, and a comparison in the game each prove different things.

## Maps and assets

Work on copies of maps you care about. Desktop Save prepares `city2.sbc`, `test.csx`, and the buildblock files before it replaces anything, and tries to undo a replacement that fails. It cannot protect a multi-file save from a crash or power cut. If you are looking into a failed save, report any backup paths it left. Save also does not copy every loose game file to a new folder, so check extra textures and item files when you test a saved map. Keep test output in the build folder or a temp folder. Do not commit browser test packages, private maps, or passwords and keys.

When you change a parsed field or a writer, update its format page in the same change. Write down which versions work and how saving behaves, then list what is still unknown. A short code comment can name the game function or byte offset behind a rule that isn't obvious. Put the longer research in `docs/`.

## AI help

The [README's AI disclosure](README.md#ai-disclosure) describes how AI helped build Pafta and the ongoing review of that code.

You can use AI tools, but you are responsible for the result. If an AI tool helped a lot with code, tests, research, or writing, say so in your summary. Say which parts it helped make and which checks you ran yourself. An AI's explanation is not evidence of how the game's files work, and a generated test only proves what it actually ran.
