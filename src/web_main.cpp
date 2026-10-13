// SPDX-License-Identifier: GPL-2.0-only
#include "app.hpp"

#include <emscripten/emscripten.h>

int main() {
    // The browser build uses the same app, dockspace, panels, renderer, and editing
    // controls as the desktop program. app.cpp yields through Asyncify once per frame,
    // so this loop stays responsive without a second, web-only UI.
    return pafta::run_editor_app();
}
