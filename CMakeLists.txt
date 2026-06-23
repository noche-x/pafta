cmake_minimum_required(VERSION 3.24)

project(map_editor_cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MAP_EDITOR_CPP_BUILD_TESTS "Build map_editor_cpp tests" ON)

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

find_package(raylib QUIET)
if(NOT raylib_FOUND)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    raylib
    URL https://github.com/raysan5/raylib/archive/refs/tags/5.5.tar.gz
  )
  FetchContent_MakeAvailable(raylib)
endif()

# Dear ImGui (docking branch)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG docking
)
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
  FetchContent_Populate(imgui)
endif()

# rlImGui (raylib-imgui bridge)
FetchContent_Declare(
  rlimgui
  GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
  GIT_TAG main
)
FetchContent_GetProperties(rlimgui)
if(NOT rlimgui_POPULATED)
  FetchContent_Populate(rlimgui)
endif()

add_library(imgui_rlimgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${rlimgui_SOURCE_DIR}/rlImGui.cpp
)
target_include_directories(imgui_rlimgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${rlimgui_SOURCE_DIR}
)
target_link_libraries(imgui_rlimgui PUBLIC raylib)

FetchContent_Declare(
  portable_file_dialogs
  URL https://github.com/samhocevar/portable-file-dialogs/archive/refs/tags/0.1.0.tar.gz
)
FetchContent_GetProperties(portable_file_dialogs)
if(NOT portable_file_dialogs_POPULATED)
  if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
  endif()
  FetchContent_Populate(portable_file_dialogs)
endif()

# tinygltf ships tiny_gltf.h + json.hpp + stb_image{,_write}.h in its repo root;
# header-only, used by src/mesh_export.cpp to write glTF.
FetchContent_Declare(
  tinygltf
  URL https://github.com/syoyo/tinygltf/archive/refs/tags/v2.9.3.tar.gz
)
FetchContent_GetProperties(tinygltf)
if(NOT tinygltf_POPULATED)
  if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
  endif()
  FetchContent_Populate(tinygltf)
endif()

# nativefiledialog-extended: real native save/open dialogs (Cocoa NSSavePanel on
# macOS) instead of pfd's unreliable osascript shell-out. Provides the `nfd` target.
FetchContent_Declare(
  nfd
  URL https://github.com/btzy/nativefiledialog-extended/archive/refs/tags/v1.3.0.tar.gz
)
FetchContent_MakeAvailable(nfd)

add_library(map_editor_core
  src/editor_state.cpp
  src/formats.cpp
  src/scene.cpp
)
target_include_directories(map_editor_core PUBLIC src)
target_compile_options(map_editor_core PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

add_executable(map_editor_cpp
  src/app.cpp
  src/main.cpp
  src/mesh_export.cpp
  src/render_gpu.cpp
  src/ui_panels.cpp
  src/viewport.cpp
)
target_include_directories(map_editor_cpp PRIVATE
  "${portable_file_dialogs_SOURCE_DIR}"
  "${tinygltf_SOURCE_DIR}"
)
target_link_libraries(map_editor_cpp PRIVATE map_editor_core imgui_rlimgui raylib nfd)
target_compile_options(map_editor_cpp PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

if(MAP_EDITOR_CPP_BUILD_TESTS)
  enable_testing()
  add_executable(map_editor_tests tests/parser_tests.cpp)
  target_link_libraries(map_editor_tests PRIVATE map_editor_core)
  target_compile_definitions(map_editor_tests PRIVATE
    MAP_EDITOR_CPP_FIXTURE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
  )
  target_compile_options(map_editor_tests PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
  )
  add_test(NAME map_editor_tests COMMAND map_editor_tests)
  add_test(
    NAME map_editor_cpp_check_sbb
    COMMAND map_editor_cpp --check "${CMAKE_CURRENT_SOURCE_DIR}/../server/data/test2/building/test.sbb"
  )
  add_test(
    NAME map_editor_cpp_check_sbc
    COMMAND map_editor_cpp --check "${CMAKE_CURRENT_SOURCE_DIR}/../server/data/test2/city2.sbc"
  )
  add_test(
    NAME map_editor_cpp_check_sbl_buildblock
    COMMAND map_editor_cpp --check "${CMAKE_CURRENT_SOURCE_DIR}/../server/data/test2/buildblock/test.sbl"
  )
  add_test(
    NAME map_editor_cpp_check_sbl_block
    COMMAND map_editor_cpp --check "${CMAKE_CURRENT_SOURCE_DIR}/../server/data/test2/block/astairwellfloor.sbl"
  )
  add_test(
    NAME map_editor_cpp_check_csx
    COMMAND map_editor_cpp --check "${CMAKE_CURRENT_SOURCE_DIR}/../server/data/test2/test.csx"
  )
  add_test(
    NAME map_editor_cpp_edit_sbb_cell
    COMMAND "${CMAKE_COMMAND}"
      -D "MAP_EDITOR_CPP_EXE=$<TARGET_FILE:map_editor_cpp>"
      -D "FIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/../server/data/test2/building/test.sbb"
      -D "WORK=${CMAKE_CURRENT_BINARY_DIR}/edit_sbb_cell"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/edit_sbb_cell.cmake"
  )
  add_test(
    NAME map_editor_cpp_edit_sbc_building
    COMMAND "${CMAKE_COMMAND}"
      -D "MAP_EDITOR_CPP_EXE=$<TARGET_FILE:map_editor_cpp>"
      -D "FIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/../server/data/test2/city2.sbc"
      -D "WORK=${CMAKE_CURRENT_BINARY_DIR}/edit_sbc_building"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/edit_sbc_building.cmake"
  )
  add_test(
    NAME map_editor_cpp_edit_sbl_word
    COMMAND "${CMAKE_COMMAND}"
      -D "MAP_EDITOR_CPP_EXE=$<TARGET_FILE:map_editor_cpp>"
      -D "FIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/../server/data/test2/buildblock/test.sbl"
      -D "WORK=${CMAKE_CURRENT_BINARY_DIR}/edit_sbl_word"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/edit_sbl_word.cmake"
  )
  add_test(
    NAME map_editor_cpp_edit_sbl_layer_portal
    COMMAND "${CMAKE_COMMAND}"
      -D "MAP_EDITOR_CPP_EXE=$<TARGET_FILE:map_editor_cpp>"
      -D "FIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/../server/data/test2/buildblock/test.sbl"
      -D "WORK=${CMAKE_CURRENT_BINARY_DIR}/edit_sbl_layer_portal"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/edit_sbl_layer_portal.cmake"
  )
endif()
