#include "app.hpp"

#include <emscripten/emscripten.h>

int main() {
    // The browser target uses the same application, dockspace, panels,
    // renderer, and editing controls as the desktop executable. app.cpp yields
    // through Asyncify once per frame so this existing stateful loop remains
    // responsive without a second web-only UI implementation.
    return map_editor::run_editor_app();
}
