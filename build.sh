#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-2.0-only
# Builds Pafta on macOS and Linux. Run from anywhere:
#
#   ./build.sh            debug build (build/dev)
#   ./build.sh run        debug build, then start the editor
#   ./build.sh test       debug build, then run the tests
#   ./build.sh release    release build (build/release)
#   ./build.sh web        browser build (build/web, needs Emscripten active)
#   ./build.sh clean      delete build/dev, build/release, and build/web
#
# The first build configures the folder; later builds only compile what changed.
set -eu
cd "$(dirname "$0")"

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

# On macOS, use the selected Xcode SDK. Some Command Line Tools SDKs fail to link
# ("unknown architecture arm64e.x1").
sysroot=""
if [ "$(uname)" = "Darwin" ] && command -v xcrun >/dev/null 2>&1; then
    sysroot="-DCMAKE_OSX_SYSROOT=$(xcrun --sdk macosx --show-sdk-path)"
fi

configure() { # folder, extra cmake arguments...
    folder=$1
    shift
    if [ ! -f "$folder/CMakeCache.txt" ]; then
        cmake -S . -B "$folder" $sysroot "$@"
    fi
}

case "${1:-debug}" in
    debug|run|test)
        configure build/dev -DCMAKE_BUILD_TYPE=Debug -DPAFTA_BUILD_TESTS=ON
        cmake --build build/dev --parallel "$jobs"
        if [ "${1:-}" = "run" ]; then exec ./build/dev/pafta; fi
        if [ "${1:-}" = "test" ]; then ctest --test-dir build/dev --output-on-failure; fi
        ;;
    release)
        configure build/release -DCMAKE_BUILD_TYPE=Release -DPAFTA_BUILD_TESTS=OFF
        cmake --build build/release --parallel "$jobs"
        echo "Built build/release/pafta"
        ;;
    web)
        command -v emcmake >/dev/null 2>&1 || { echo "Activate Emscripten first (emcmake not found)" >&2; exit 1; }
        export EM_CACHE="$PWD/build/web/emcache"
        if [ ! -f build/web/CMakeCache.txt ]; then
            emcmake cmake -S . -B build/web -DPLATFORM=Web -DGRAPHICS=GRAPHICS_API_OPENGL_ES3 \
                -DPAFTA_BUILD_TESTS=OFF -DPAFTA_ENABLE_PROFILER=OFF
        fi
        EMCC_BATCH_BUILD=0 cmake --build build/web --target pafta_web --parallel "$jobs"
        echo "Serve build/web over HTTP and open pafta_web.html"
        ;;
    clean)
        rm -rf build/dev build/release build/web
        echo "Removed build/dev, build/release, and build/web"
        ;;
    *)
        echo "Usage: ./build.sh [debug|run|test|release|web|clean]" >&2
        exit 2
        ;;
esac
