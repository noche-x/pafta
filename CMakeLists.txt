# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.24)

project(pafta VERSION 0.1.0 LANGUAGES CXX)

# Product name and version shown in the window title and menu bar.
add_compile_definitions(PAFTA_VERSION="${PROJECT_VERSION}")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PAFTA_BUILD_TESTS "Build pafta tests" ON)
option(PAFTA_ENABLE_PROFILER "Build pafta with yse/easy_profiler instrumentation" OFF)
option(PAFTA_ENABLE_COVERAGE "Instrument editor code for LLVM source coverage" OFF)
option(PAFTA_ENABLE_SANITIZERS "Check native code with AddressSanitizer and UBSan" OFF)
option(PAFTA_BUILD_FUZZERS "Build bounded native parser fuzz tests (Clang)" OFF)
option(PAFTA_WARNINGS_AS_ERRORS "Reject compiler warnings in Pafta code" OFF)
option(PAFTA_BUILD_WEB "Build the browser/WASM editor shell" OFF)
set(PAFTA_FIXTURE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH
  "SRC workspace root containing server/data/test2 for fixture tests")
set(PAFTA_WEB_PRELOAD_PROJECT "" CACHE PATH
  "QA-only map folder to embed at /web_project in the Emscripten build")

if(EMSCRIPTEN)
  set(PAFTA_BUILD_WEB ON CACHE BOOL "Build the browser/WASM editor shell" FORCE)
  # raylib builds for desktop unless these cache values are set. The renderer's
  # packed depth and shadows need ES3.
  set(PLATFORM Web CACHE STRING "raylib platform" FORCE)
  set(GRAPHICS GRAPHICS_API_OPENGL_ES3 CACHE STRING "raylib graphics API" FORCE)
  if(PAFTA_WEB_PRELOAD_PROJECT)
    if(NOT IS_DIRECTORY "${PAFTA_WEB_PRELOAD_PROJECT}")
      message(FATAL_ERROR
        "PAFTA_WEB_PRELOAD_PROJECT is not a directory: "
        "${PAFTA_WEB_PRELOAD_PROJECT}")
    endif()
  endif()
elseif(PAFTA_WEB_PRELOAD_PROJECT)
  message(FATAL_ERROR
    "PAFTA_WEB_PRELOAD_PROJECT is only supported for Emscripten builds")
endif()

if(PAFTA_ENABLE_COVERAGE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR EMSCRIPTEN)
    message(FATAL_ERROR "Source coverage requires a native Clang build")
  endif()
  function(pafta_enable_coverage target)
    get_target_property(target_type ${target} TYPE)
    if(target_type STREQUAL "EXECUTABLE")
      target_link_options(${target} PRIVATE -fprofile-instr-generate)
    else()
      target_compile_options(${target} PRIVATE -fprofile-instr-generate -fcoverage-mapping
        "-fprofile-list=${CMAKE_CURRENT_BINARY_DIR}/coverage-sources.txt")
    endif()
  endfunction()
  # Inline header mappings can differ across executables; measure the .cpp files.
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/coverage-sources.txt"
    "[clang]\nsrc:${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp\n")
endif()

if(PAFTA_ENABLE_SANITIZERS AND (MSVC OR EMSCRIPTEN))
  message(FATAL_ERROR "Sanitizer checks require a native Clang or GCC build")
endif()
if(PAFTA_BUILD_FUZZERS AND (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR EMSCRIPTEN
    OR NOT PAFTA_ENABLE_SANITIZERS OR NOT PAFTA_BUILD_TESTS))
  message(FATAL_ERROR "Parser fuzz tests require native Clang with PAFTA_ENABLE_SANITIZERS=ON and PAFTA_BUILD_TESTS=ON")
endif()

# Source archives include the exact library sources, so no download is needed.
foreach(dependency raylib imgui rlimgui tinygltf nfd)
  if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${dependency}")
    string(TOUPPER "${dependency}" dependency_key)
    set(FETCHCONTENT_SOURCE_DIR_${dependency_key}
      "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${dependency}" CACHE PATH "Bundled dependency source")
  endif()
endforeach()

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# Use the same static graphics library on every platform.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  raylib
  URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
)
# raylib 5.5 still asks for CMake policies older than 3.10. Apply the CMake 4
# compatibility setting to that library only, not to the editor's build.
function(pafta_add_raylib)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
  FetchContent_MakeAvailable(raylib)
endfunction()
pafta_add_raylib()
get_target_property(raylib_include_dirs raylib INTERFACE_INCLUDE_DIRECTORIES)
set_property(TARGET raylib APPEND PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${raylib_include_dirs}")

# Dear ImGui (docking branch)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG fd13a1e8923a0a7077b404fc36fd063b25a0c0b5
)
FetchContent_MakeAvailable(imgui)

# rlImGui (raylib-imgui bridge)
FetchContent_Declare(
  rlimgui
  GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
  GIT_TAG db8239140a68bf9a07f4499d2875d7cefa8fa7e6
)
FetchContent_MakeAvailable(rlimgui)

add_library(imgui_rlimgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${rlimgui_SOURCE_DIR}/rlImGui.cpp
)
target_include_directories(imgui_rlimgui SYSTEM PUBLIC
  ${imgui_SOURCE_DIR}
  ${rlimgui_SOURCE_DIR}
)
target_link_libraries(imgui_rlimgui PUBLIC raylib)
# The editor adds Font Awesome itself (apply_editor_style in ui_windows.cpp), at
# the text size and with a fixed icon width, so icons line up with each other and
# with the text.
target_compile_definitions(imgui_rlimgui PUBLIC NO_FONT_AWESOME)

# tinygltf has tiny_gltf.h, json.hpp, and stb_image{,_write}.h in its repo root.
# It is header-only; src/mesh_export.cpp uses it to write glTF.
if(NOT EMSCRIPTEN)
  FetchContent_Declare(
    tinygltf
    URL https://github.com/syoyo/tinygltf/archive/refs/tags/v2.9.3.tar.gz
    SOURCE_SUBDIR cmake-do-not-add
  )
  FetchContent_MakeAvailable(tinygltf)
endif()

# Native file dialogs are kept in file_dialogs.cpp. This provides the nfd target.
if(NOT EMSCRIPTEN)
  FetchContent_Declare(
    nfd
    URL https://github.com/btzy/nativefiledialog-extended/archive/refs/tags/v1.3.0.tar.gz
  )
  FetchContent_MakeAvailable(nfd)
endif()

if(PAFTA_ENABLE_PROFILER)
  set(EASY_PROFILER_NO_GUI ON CACHE BOOL "" FORCE)
  set(EASY_PROFILER_NO_SAMPLES ON CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    easy_profiler
    URL https://github.com/yse/easy_profiler/archive/cc0e154635e5433344c4fb4fe940939a05465701.tar.gz
  )
  FetchContent_MakeAvailable(easy_profiler)
endif()

# The UI font is compiled into the editor, so the binary runs without an assets
# folder next to it.
set(PAFTA_FONT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/DejaVuSansMono.ttf")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PAFTA_FONT_FILE}")
file(READ "${PAFTA_FONT_FILE}" pafta_font_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," pafta_font_bytes "${pafta_font_hex}")
string(REGEX REPLACE "((0x..,){32})" "\\1\n" pafta_font_bytes "${pafta_font_bytes}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/editor_font.cpp.in"
  "// Generated from assets/fonts/DejaVuSansMono.ttf by CMakeLists.txt.\n"
  "#include <cstddef>\n"
  "extern const unsigned char pafta_editor_font[] = {\n${pafta_font_bytes}};\n"
  "extern const std::size_t pafta_editor_font_size = sizeof(pafta_editor_font);\n")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/generated/editor_font.cpp.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/editor_font.cpp" COPYONLY)
set(PAFTA_FONT_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/editor_font.cpp")

add_library(pafta_core
  src/buildblock_catalog.cpp
  src/editor_state.cpp
  src/formats.cpp
  src/scene.cpp
)
target_include_directories(pafta_core PUBLIC src)
if(PAFTA_ENABLE_COVERAGE)
  pafta_enable_coverage(pafta_core)
endif()
if(PAFTA_ENABLE_PROFILER)
  target_compile_definitions(pafta_core PUBLIC
    BUILD_WITH_EASY_PROFILER
    PAFTA_ENABLE_PROFILER=1
  )
  target_link_libraries(pafta_core PUBLIC easy_profiler)
endif()
target_compile_options(pafta_core PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

if(NOT EMSCRIPTEN)
  add_library(pafta_app STATIC
    src/app.cpp
    src/project_io.cpp
    src/edit_history.cpp
    src/block_editor.cpp
    src/file_dialogs.cpp
    src/mesh_export.cpp
    src/tinygltf_impl.cpp
    src/render_gpu.cpp
    src/ui_archive.cpp
    src/ui_panels.cpp
    src/ui_limits_buildblock.cpp
    src/ui_windows.cpp
    src/ui_workspace.cpp
    src/viewport.cpp
    ${PAFTA_FONT_SOURCE}
  )
  target_include_directories(pafta_app SYSTEM PRIVATE
    "${tinygltf_SOURCE_DIR}"
  )
  target_link_libraries(pafta_app PUBLIC pafta_core imgui_rlimgui raylib nfd)
  add_executable(pafta src/main.cpp)
  target_link_libraries(pafta PRIVATE pafta_app)
  if(PAFTA_ENABLE_COVERAGE)
    pafta_enable_coverage(pafta_app)
    pafta_enable_coverage(pafta)
  endif()
  if(PAFTA_ENABLE_PROFILER)
    target_compile_definitions(pafta_app PRIVATE
      BUILD_WITH_EASY_PROFILER
      PAFTA_ENABLE_PROFILER=1
    )
    target_link_libraries(pafta_app PRIVATE easy_profiler)
  endif()
  target_compile_options(pafta_app PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
  )
endif()

if(PAFTA_BUILD_WEB)
  # The browser build uses the same app, panels, viewport, and renderer as the
  # desktop one. Only the OS file dialogs and glTF export are swapped for
  # browser-friendly versions in app.cpp and web_main.cpp.
  add_executable(pafta_web
    src/app.cpp
    src/project_io.cpp
    src/edit_history.cpp
    src/block_editor.cpp
    src/web_main.cpp
    src/render_gpu.cpp
    src/ui_archive.cpp
    src/ui_panels.cpp
    src/ui_limits_buildblock.cpp
    src/ui_windows.cpp
    src/ui_workspace.cpp
    src/viewport.cpp
    ${PAFTA_FONT_SOURCE}
  )
  target_link_libraries(pafta_web PRIVATE pafta_core imgui_rlimgui raylib)
  target_compile_definitions(pafta_web PRIVATE PAFTA_WEB=1)
  target_compile_options(pafta_web PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
  )
  target_link_options(pafta_web PRIVATE
    "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html"
    "-sUSE_GLFW=3"
    # The renderer uses GLSL ES 3.00, a second colour attachment, and a depth
    # texture. Don't let Emscripten fall back to WebGL 1 while running.
    "-sMIN_WEBGL_VERSION=2"
    "-sMAX_WEBGL_VERSION=2"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sASYNCIFY"
    "-sASYNCIFY_STACK_SIZE=1048576"
    "-sASSERTIONS=1"
    "-sEXPORTED_RUNTIME_METHODS=['FS','ccall']"
    "--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/web/file_upload.js"
  )
  set_property(TARGET pafta_web APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/file_upload.js")
  if(PAFTA_WEB_PRELOAD_PROJECT)
    # This option is off unless you turn it on. It is for browser test builds that
    # need a real map ready at startup. Normal web builds stay free of map files
    # and keep using the folder picker or drag and drop.
    target_compile_definitions(pafta_web PRIVATE
      PAFTA_WEB_PRELOAD_ROOT="/web_project"
    )
    target_link_options(pafta_web PRIVATE
      "--preload-file=${PAFTA_WEB_PRELOAD_PROJECT}@/web_project"
      # city.bin is a 171 MiB cache the game makes while running. The editor
      # reads city2.sbc and the CSX/SBB/SBL files instead, so leaving this cache
      # out keeps the test package small without changing how the editor loads.
      "--exclude-file=${PAFTA_WEB_PRELOAD_PROJECT}/city.bin"

    )
  endif()
  set_target_properties(pafta_web PROPERTIES SUFFIX ".html")
endif()

if(PAFTA_BUILD_TESTS AND NOT EMSCRIPTEN)
  enable_testing()
  find_program(PAFTA_NODE_EXECUTABLE node)
  if(PAFTA_NODE_EXECUTABLE)
    add_test(NAME browser_upload_tests
      COMMAND "${PAFTA_NODE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/browser_upload_tests.js")
  endif()
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME source_package_tests
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/source_package_tests.py")
  endif()
  add_executable(binary_export_tests tests/binary_export_tests.cpp)
  target_link_libraries(binary_export_tests PRIVATE pafta_core)
  if(PAFTA_ENABLE_COVERAGE)
    pafta_enable_coverage(binary_export_tests)
  endif()
  add_test(NAME binary_export_tests COMMAND binary_export_tests)

  add_executable(editor_state_tests tests/editor_state_tests.cpp)
  target_link_libraries(editor_state_tests PRIVATE pafta_core)
  if(PAFTA_ENABLE_COVERAGE)
    pafta_enable_coverage(editor_state_tests)
  endif()
  add_test(NAME editor_state_tests COMMAND editor_state_tests)

  add_executable(editor_workflow_tests tests/editor_workflow_tests.cpp)
  target_link_libraries(editor_workflow_tests PRIVATE pafta_app)
  if(PAFTA_ENABLE_COVERAGE)
    pafta_enable_coverage(editor_workflow_tests)
  endif()
  add_test(NAME editor_workflow_tests COMMAND editor_workflow_tests)

  add_executable(mesh_export_tests tests/mesh_export_tests.cpp src/mesh_export.cpp src/tinygltf_impl.cpp)
  target_include_directories(mesh_export_tests SYSTEM PRIVATE "${tinygltf_SOURCE_DIR}")
  target_link_libraries(mesh_export_tests PRIVATE pafta_core imgui_rlimgui raylib)
  target_compile_definitions(mesh_export_tests PRIVATE
    PAFTA_TEST_OUTPUT_ROOT="${CMAKE_CURRENT_BINARY_DIR}/mesh_export_test")
  if(PAFTA_ENABLE_COVERAGE)
    pafta_enable_coverage(mesh_export_tests)
    set_source_files_properties(src/mesh_export.cpp PROPERTIES COMPILE_OPTIONS
      "-fprofile-instr-generate;-fcoverage-mapping;-fprofile-list=${CMAKE_CURRENT_BINARY_DIR}/coverage-sources.txt")
  endif()
  add_test(NAME mesh_export_tests COMMAND mesh_export_tests)

  add_executable(pafta_tests tests/parser_tests.cpp)
  target_link_libraries(pafta_tests PRIVATE pafta_core)
  target_compile_definitions(pafta_tests PRIVATE
    PAFTA_FIXTURE_ROOT="${PAFTA_FIXTURE_ROOT}"
  )
  if(PAFTA_ENABLE_COVERAGE)
    pafta_enable_coverage(pafta_tests)
  endif()
  target_compile_options(pafta_tests PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
  )
  add_test(NAME pafta_portable_tests COMMAND pafta_tests --portable)
  if(EXISTS "${PAFTA_FIXTURE_ROOT}/server/data/test2/city2.sbc")
    add_test(NAME pafta_tests COMMAND pafta_tests)
    add_test(NAME binary_fixture_exports
      COMMAND binary_export_tests "${PAFTA_FIXTURE_ROOT}/server/data/test2")
    add_test(
      NAME pafta_check_sbb
      COMMAND pafta --check "${PAFTA_FIXTURE_ROOT}/server/data/test2/building/test.sbb"
    )
    add_test(
      NAME pafta_check_sbc
      COMMAND pafta --check "${PAFTA_FIXTURE_ROOT}/server/data/test2/city2.sbc"
    )
    add_test(
      NAME pafta_check_sbl_buildblock
      COMMAND pafta --check "${PAFTA_FIXTURE_ROOT}/server/data/test2/buildblock/test.sbl"
    )
    add_test(
      NAME pafta_check_sbl_block
      COMMAND pafta --check "${PAFTA_FIXTURE_ROOT}/server/data/test2/block/astairwellfloor.sbl"
    )
    add_test(
      NAME pafta_check_csx
      COMMAND pafta --check "${PAFTA_FIXTURE_ROOT}/server/data/test2/test.csx"
    )
    add_test(
      NAME pafta_edit_sbb_cell
      COMMAND "${CMAKE_COMMAND}"
        -D "PAFTA_EXE=$<TARGET_FILE:pafta>"
        -D "FIXTURE=${PAFTA_FIXTURE_ROOT}/server/data/test2/building/test.sbb"
        -D "WORK=${CMAKE_CURRENT_BINARY_DIR}/edit_sbb_cell"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/edit_sbb_cell.cmake"
    )
    add_test(
      NAME pafta_edit_sbc_building
      COMMAND "${CMAKE_COMMAND}"
        -D "PAFTA_EXE=$<TARGET_FILE:pafta>"
        -D "FIXTURE=${PAFTA_FIXTURE_ROOT}/server/data/test2/city2.sbc"
        -D "WORK=${CMAKE_CURRENT_BINARY_DIR}/edit_sbc_building"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/edit_sbc_building.cmake"
    )
    add_test(
      NAME pafta_edit_sbl_word
      COMMAND "${CMAKE_COMMAND}"
        -D "PAFTA_EXE=$<TARGET_FILE:pafta>"
        -D "FIXTURE=${PAFTA_FIXTURE_ROOT}/server/data/test2/buildblock/test.sbl"
        -D "WORK=${CMAKE_CURRENT_BINARY_DIR}/edit_sbl_word"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/edit_sbl_word.cmake"
    )
    add_test(
      NAME pafta_edit_sbl_layer_portal
      COMMAND "${CMAKE_COMMAND}"
        -D "PAFTA_EXE=$<TARGET_FILE:pafta>"
        -D "FIXTURE=${PAFTA_FIXTURE_ROOT}/server/data/test2/buildblock/test.sbl"
        -D "WORK=${CMAKE_CURRENT_BINARY_DIR}/edit_sbl_layer_portal"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/edit_sbl_layer_portal.cmake"
    )
  endif()
endif()

if(PAFTA_BUILD_FUZZERS)
  target_compile_options(pafta_core PRIVATE -fsanitize=fuzzer-no-link)
  foreach(format sbb sbc sbl csx)
    add_executable(fuzz_${format} tests/parser_fuzz.cpp)
    target_compile_definitions(fuzz_${format} PRIVATE "PAFTA_FUZZ_${format}")
    target_compile_options(fuzz_${format} PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_${format} PRIVATE -fsanitize=fuzzer)
    target_link_libraries(fuzz_${format} PRIVATE pafta_core)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fuzz/${format}")
    add_test(NAME fuzz_${format} COMMAND fuzz_${format}
      "${CMAKE_CURRENT_BINARY_DIR}/fuzz/${format}" -max_total_time=15 -max_len=65536 -timeout=5
      "-artifact_prefix=${CMAKE_CURRENT_BINARY_DIR}/fuzz/${format}/")
    set_tests_properties(fuzz_${format} PROPERTIES LABELS fuzz TIMEOUT 30)
  endforeach()
endif()

foreach(target pafta_core pafta_app pafta pafta_web pafta_tests binary_export_tests
    editor_state_tests editor_workflow_tests mesh_export_tests fuzz_sbb fuzz_sbc fuzz_sbl fuzz_csx)
  if(TARGET ${target})
    if(PAFTA_ENABLE_SANITIZERS)
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined
        -fno-sanitize-recover=all -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all)
    endif()
    if(PAFTA_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/WX>
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Werror>)
    endif()
  endif()
endforeach()

# Local and CI beta packaging use the same install component.
if(NOT EMSCRIPTEN)
  install(TARGETS pafta RUNTIME DESTINATION . COMPONENT Pafta)
  install(FILES LICENSE README.md CONTRIBUTING.md STYLE_GUIDE.md THIRD_PARTY_NOTICES.md DESTINATION . COMPONENT Pafta)
  install(DIRECTORY docs/ DESTINATION docs COMPONENT Pafta)
  install(DIRECTORY licenses/ DESTINATION licenses COMPONENT Pafta)
endif()
