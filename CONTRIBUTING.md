# Contributing to Pafta

Pafta is beta software. A change can affect files that users save or the way a map appears in game, so start with the behavior you can verify. [README.md](README.md) covers the current workflow and limits; [STYLE_GUIDE.md](STYLE_GUIDE.md) describes how to write code that fits this project.

## Before editing

Find the entry point for the behavior, then follow the calls that read state and write results. The main areas are:

| Area | Files |
| --- | --- |
| Desktop and command-line entry points | `src/main.cpp`, `src/app.cpp` |
| Binary formats and focused edits | `src/formats.hpp`, `src/formats.cpp` |
| Editor state and history | `src/editor_state.*`, `src/app.cpp` |
| Scene construction | `src/scene.*`, `src/buildblock_catalog.*` |
| Rendering and UI | `src/render_gpu.cpp`, `src/viewport.cpp`, `src/ui_*.cpp` |

For a format change, read the matching page under [docs/file_formats/](docs/file_formats/README.md) and a real fixture. Keep confirmed fields, native-code evidence, and unknown bytes distinct. An apparent value in one fixture is not a format rule.

Keep the change focused. If it crosses parsing, editing, and saving, trace the complete path so the editor does not display one value and write another. Preserve unrelated bytes and existing map data. Fail with a useful error when the input cannot be handled safely; do not call an unchanged output a successful edit.

## Build and check

From the repository root:

```sh
cmake --preset dev
cmake --build --preset dev --parallel
ctest --preset dev
```

Use the `dev-msvc` preset on Windows. Portable format, scene, editor-state, and export tests run in a standalone checkout. The larger fixture suite runs when `../server/data/test2` exists; set `MAP_EDITOR_CPP_FIXTURE_ROOT` if that SRC workspace lives elsewhere. The CI presets build Release on macOS, Linux, and Windows; keep test checks active in Release. CTest covers parsers, scene construction, state edits, binary and glTF exports, and focused command-line edits when their fixtures are present. It does not prove the viewport, GPU driver, browser downloads, or in-game parity.

For LLVM source coverage, run `bash tests/coverage.sh` with Clang and matching `llvm-profdata` and `llvm-cov` on the path. The report and HTML are saved under `build/coverage/`. CI gates the CPU core at 75% line coverage. Treat that percentage as the covered file set, not the whole application; UI and GPU behavior need direct app checks.

Match checks to the change:

- For a binary reader or writer, add a fixture or a small constructed case that proves the boundary or round trip. Check exact bytes for formats that promise lossless writing. If a full rebuild normalizes a supported source file, report that drift and check that a second export is stable. Keep the no-op patch path byte-identical.
- For a UI or rendering change, run the relevant build and inspect the changed workflow in the app. Use the same map, camera, and world time when comparing a game view with Pafta.
- For a browser change, follow [docs/WEB_BUILD.md](docs/WEB_BUILD.md), serve the result over HTTP, and check the actual browser workflow. Keep `MAP_EDITOR_CPP_WEB_PRELOAD_PROJECT` for local QA; do not ship its map data.
- For a documentation-only change, check the stated behavior against current source and review the Markdown diff. A native build adds no evidence for prose alone.

State what you ran and what remains untested when you hand off a change. A host test, a successful link, a screenshot, and an in-game comparison answer different questions.

## Changes to maps and assets

Work on copies of valuable maps. Desktop Save writes `city2.sbc`, `test.csx`, and buildblock files directly into the selected folder; it does not replace the entire set as one transaction. Keep test output in the build tree or a temporary directory. Do not commit generated browser QA packages, private map data, or credentials.

When you change a parsed field or writer, update the matching format page in the same contribution. Document the supported versions and writer behavior, then identify anything still unknown. Short source comments can name the native function or byte offset that explains a non-obvious rule; put the longer evidence trail in `docs/`.

## AI assistance

AI-assisted contributions are welcome, but the author owns the result. If an AI tool materially helped with code, tests, research, or prose, disclose that in the contribution summary. Say which parts it helped produce and which checks you performed yourself. Do not describe an AI explanation as native-format evidence, and do not claim that generated tests prove game behavior they did not exercise.
