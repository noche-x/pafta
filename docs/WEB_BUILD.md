# Browser build

The browser build is the same editor as the desktop one. It uses the same layout, viewport, panels, parsers, scene loader, and GPU renderer. When you pick a map folder in the browser, Pafta copies it into Emscripten's virtual file system and then opens it with the normal loader.

From the `pafta` folder, configure and build with an Emscripten cache it can write to:

```sh
EM_CACHE="$PWD/build/web/emcache" emcmake cmake -S . -B build/web \
  -DPLATFORM=Web \
  -DGRAPHICS=GRAPHICS_API_OPENGL_ES3 \
  -DPAFTA_BUILD_TESTS=OFF \
  -DPAFTA_ENABLE_PROFILER=OFF
EM_CACHE="$PWD/build/web/emcache" EMCC_BATCH_BUILD=0 cmake --build build/web \
  --target pafta_web --parallel
```

This makes `build/web/pafta_web.html` plus a JavaScript file and a WebAssembly file next to it. Serve the folder over HTTP. Browsers block WebAssembly when you open the HTML straight from `file://`.

For example, from the repository root:

```sh
python3 -m http.server 8000 --directory build/web
```

Then open `http://localhost:8000/pafta_web.html`. To test an unzipped browser archive, serve its folder and open `index.html`.

## Using the browser build

The browser build has no native file dialogs and no glTF save dialog. Use the `Open map folder` button or drop a folder on the canvas. The active map stays inside the browser's virtual file system. Once a replacement map is accepted, the editor removes the old upload folder; cancelled or failed uploads also release their copied files. A newer upload supersedes an older one still copying. Mesh export only works on desktop.

Save starts separate downloads instead of writing back into the folder you opened. Allow multiple downloads, and put the downloaded buildblock `.sbl` files in `buildblock/` next to `city2.sbc` and `test.csx`. Keep the other files from the original map too, such as the root map textures and item files, because Save does not copy all of them. Starting a download does not prove you saved the file. The CSX archive workspace also requests downloads when you build an archive or extract selected entries. Its edits are separate from Save map.

Copying a folder into the browser shows its progress. The browser build loads the map on one thread, and GPU uploads also run on the WebGL thread. The editor pauses once before parsing and once per frame after that, so a big map can still freeze the page for a moment while it loads.

## GitHub Pages

The GitHub Actions workflow builds with Emscripten 6.0.8 on every push and pull request. It zips `index.html`, `pafta_web.js`, and `pafta_web.wasm`, with the docs and notices, into `pafta-web.zip`. Publishing only happens when someone starts the workflow by hand on the default branch with `publish_pages` turned on. It then waits for the native builds, tests, coverage, and browser build to pass. Normal pushes and tags only produce downloadable files; release tags also make a draft prerelease.

To set it up, connect this checkout to a GitHub repository, open Settings → Pages, and choose GitHub Actions as the source. The site address is usually `https://OWNER.github.io/REPOSITORY/`. The deployed `index.html` uses relative paths, so the JavaScript and WebAssembly files load fine under that path. The Pages deployment job shows the exact address.

Never set `PAFTA_WEB_PRELOAD_PROJECT` in CI. Before publishing, the workflow checks that the build did not produce a `.data` package.

## Testing in the browser with a real map

Normal browser builds have no map data. For a local test build, give CMake a real map folder. CMake packs that folder in at `/web_project`, and the editor opens it as soon as WebAssembly starts:

```sh
EM_CACHE="$PWD/build/web-qa/emcache" emcmake cmake -S . -B build/web-qa \
  -DPLATFORM=Web \
  -DGRAPHICS=GRAPHICS_API_OPENGL_ES3 \
  -DPAFTA_BUILD_TESTS=OFF \
  -DPAFTA_WEB_PRELOAD_PROJECT="$PWD/../server/data/test2"
EM_CACHE="$PWD/build/web-qa/emcache" EMCC_BATCH_BUILD=0 cmake --build build/web-qa \
  --target pafta_web --parallel
```

Serve `build/web-qa` over HTTP. Never commit or ship the data package it makes. To get a normal build again, configure without `PAFTA_WEB_PRELOAD_PROJECT`.

The test package leaves out `city.bin`, a large cache the game makes at runtime that the editor never reads. It keeps `test.csx`, so maps that only use a CSX archive work the same way as maps with loose files.

## Emscripten cache workaround

`EMCC_BATCH_BUILD=0` works around a path bug in the system-library cache of Homebrew's Emscripten 6.0.8. Newer Emscripten versions may not need it.
