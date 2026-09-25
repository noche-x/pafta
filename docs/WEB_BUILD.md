# Browser build

The browser target builds the same one-page editor application as the desktop
target. It shares the dockspace, scene viewport, panels, parser, scene loader,
and GPU renderer. Browser file selection copies the selected map directory into
Emscripten's virtual filesystem before the shared deferred loader opens it.

From `map_editor_cpp`, configure and build with a writable Emscripten cache:

```sh
EM_CACHE="$PWD/build/web/emcache" emcmake cmake -S . -B build/web \
  -DPLATFORM=Web \
  -DGRAPHICS=GRAPHICS_API_OPENGL_ES3 \
  -DMAP_EDITOR_CPP_BUILD_TESTS=OFF \
  -DMAP_EDITOR_CPP_ENABLE_PROFILER=OFF
EM_CACHE="$PWD/build/web/emcache" EMCC_BATCH_BUILD=0 cmake --build build/web \
  --target map_editor_web --parallel
```

The output is `build/web/map_editor_web.html` with adjacent JavaScript and
WebAssembly files. Serve the directory over HTTP. Opening the HTML directly
from `file://` is not supported by browser WebAssembly security rules.

The web target excludes native file dialogs and native glTF save dialogs. Use
the `Open map folder` button or drop a folder on the canvas. Files stay in the
browser's virtual filesystem. Mesh export remains a desktop-only operation.

## Browser QA with a real map

Normal web builds do not include map data. For a local QA build, pass a real
map folder explicitly. CMake embeds that folder at `/web_project`, and the
editor opens it automatically after WebAssembly startup:

```sh
EM_CACHE="$PWD/build/web-qa/emcache" emcmake cmake -S . -B build/web-qa \
  -DPLATFORM=Web \
  -DGRAPHICS=GRAPHICS_API_OPENGL_ES3 \
  -DMAP_EDITOR_CPP_BUILD_TESTS=OFF \
  -DMAP_EDITOR_CPP_WEB_PRELOAD_PROJECT="$PWD/../server/data/test2"
EM_CACHE="$PWD/build/web-qa/emcache" EMCC_BATCH_BUILD=0 cmake --build build/web-qa \
  --target map_editor_web --parallel
```

Serve `build/web-qa` over HTTP. Do not commit or ship the generated data
package. Reconfigure without `MAP_EDITOR_CPP_WEB_PRELOAD_PROJECT` for the
normal asset-free browser build.

The QA package omits `city.bin`, which is a large game runtime cache that the
editor does not read. The `test2` fixture also omits its duplicate `test.csx`
distribution bundle because its SBB, SBL, texture, and material sources are
present loose. Normal browser folder loads still support CSX-only maps.

Browser folder copying reports progress. Project parsing remains synchronous
because texture upload must stay on the WebGL thread; the editor yields once
before parsing and once per frame afterward.

`EMCC_BATCH_BUILD=0` works around a path issue in the Homebrew Emscripten
6.0.8 system-library cache. Newer Emscripten versions may not need it.
