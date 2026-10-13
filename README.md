# Pafta

Pafta (prounounced PAHF-tah) is a map editor for Sub Rosa, written in C++23.

Pafta is in beta. Backup your maps before working on them with Pafta, and test saved maps in Sub Rosa before you share them. The editor view can look different from the game.

- [Downloading](#artifact-builds)
- [Building](#building)
- [Using](docs/USAGE.md)
- [Edit and save](docs/USAGE.md#edit-and-save)
- [File support and limits](docs/USAGE.md#file-support-and-limits)
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

Here, `-S` selects the source folder and `-B` selects the build folder. For CTest, `-C Debug` selects the build configuration. For CMake, `-C` instead loads an initial cache script and is not needed here. To build Release, replace `Debug` with `Release` in the build, test, and executable commands. Use a separate build folder for each Visual Studio generator. The 2026 commands follow CMake's generator documentation but have not been tested with Pafta.

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

The parser, scene, editor-state, and export tests run without game files. If `../server/data/test2` exists, CTest also runs tests that use real map files and command-line edits. To use a different map folder, set `PAFTA_FIXTURE_ROOT` to its path.

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

## Using

See the [usage guide](docs/USAGE.md) for opening maps, controls, editing, saving, supported files, and current limits.

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

AI helped write a substantial part of Pafta. Reconstructing Sub Rosa's file formats and editor behavior is a lot of work, and I'm one person with limited time. I used AI to help get the editor built, and I take responsibility for the result.

I'm reviewing the code, fixing mistakes, and cutting things that don't belong. Generated code doesn't get a pass because it compiles. I check changes against the formats and tests we have, where game behavior or browser workflows still need verification, the docs say so. I won't claim tests prove more than they do.

If using AI this way puts you off, you're free to skip Pafta. If you find broken behavior, confusing code, or something unnecessary, please open an issue and point to it. Useful feedback is appreciated, and I'll act on it.

Most of the docs have not been checked for AI-generated content yet. The AI-generated reverse-engineering notes are kept apart in [docs/slop](docs/slop/README.md); treat them as leads, not facts.

This is still a beta. As I work through the code, I intend to replace generated implementations with code I understand and can maintain myself. That takes time. [CONTRIBUTING.md](CONTRIBUTING.md) explains what contributors should disclose and verify.

## License

Pafta's source code is licensed under [GNU GPL version 2 only](LICENSE) (`GPL-2.0-only`). If you share a modified version, it has to stay under the GPL, and if you share a binary, you have to offer the matching full source and build scripts as the license describes. You do not have to publish changes you keep to yourself. The [GNU GPL FAQ](https://www.gnu.org/licenses/gpl-faq.en.html#GPLRequireSourcePostedPublic) explains the difference.

Third-party libraries and bundled fonts keep their own licenses, listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Preparing a release does not make the repository public or publish anything.
