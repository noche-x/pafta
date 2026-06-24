cmake_minimum_required(VERSION 3.24)

project(map_editor_cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MAP_EDITOR_CPP_BUILD_TESTS "Build map_editor_cpp tests" ON)
option(MAP_EDITOR_CPP_ENABLE_PROFILER "Build map_editor_cpp with yse/easy_profiler instrumentation" OFF)

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
  if(DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(MAP_EDITOR_CPP_POLICY_VERSION_MINIMUM_WAS_DEFINED ON)
    set(MAP_EDITOR_CPP_POLICY_VERSION_MINIMUM_VALUE "${CMAKE_POLICY_VERSION_MINIMUM}")
  endif()
  if(DEFINED CMAKE_SUPPRESS_DEVELOPER_WARNINGS)
    set(MAP_EDITOR_CPP_SUPPRESS_DEVELOPER_WARNINGS_WAS_DEFINED ON)
    set(MAP_EDITOR_CPP_SUPPRESS_DEVELOPER_WARNINGS_VALUE "${CMAKE_SUPPRESS_DEVELOPER_WARNINGS}")
  endif()
  set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
  set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS ON)
  FetchContent_MakeAvailable(raylib)
  if(MAP_EDITOR_CPP_POLICY_VERSION_MINIMUM_WAS_DEFINED)
    set(CMAKE_POLICY_VERSION_MINIMUM "${MAP_EDITOR_CPP_POLICY_VERSION_MINIMUM_VALUE}")
  else()
    unset(CMAKE_POLICY_VERSION_MINIMUM)
  endif()
  if(MAP_EDITOR_CPP_SUPPRESS_DEVELOPER_WARNINGS_WAS_DEFINED)
    set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS "${MAP_EDITOR_CPP_SUPPRESS_DEVELOPER_WARNINGS_VALUE}")
  else()
    unset(CMAKE_SUPPRESS_DEVELOPER_WARNINGS)
  endif()
  unset(MAP_EDITOR_CPP_POLICY_VERSION_MINIMUM_WAS_DEFINED)
  unset(MAP_EDITOR_CPP_POLICY_VERSION_MINIMUM_VALUE)
  unset(MAP_EDITOR_CPP_SUPPRESS_DEVELOPER_WARNINGS_WAS_DEFINED)
  unset(MAP_EDITOR_CPP_SUPPRESS_DEVELOPER_WARNINGS_VALUE)
endif()

# Dear ImGui (docking branch)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG docking
)
FetchContent_MakeAvailable(imgui)

# rlImGui (raylib-imgui bridge)
FetchContent_Declare(
  rlimgui
  GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
  GIT_TAG main
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
target_include_directories(imgui_rlimgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${rlimgui_SOURCE_DIR}
)
target_link_libraries(imgui_rlimgui PUBLIC raylib)

FetchContent_Declare(
  portable_file_dialogs
  URL https://github.com/samhocevar/portable-file-dialogs/archive/refs/tags/0.1.0.tar.gz
  SOURCE_SUBDIR cmake-do-not-add
)
FetchContent_MakeAvailable(portable_file_dialogs)

# tinygltf ships tiny_gltf.h + json.hpp + stb_image{,_write}.h in its repo root;
# header-only, used by src/mesh_export.cpp to write glTF.
FetchContent_Declare(
  tinygltf
  URL https://github.com/syoyo/tinygltf/archive/refs/tags/v2.9.3.tar.gz
  SOURCE_SUBDIR cmake-do-not-add
)
FetchContent_MakeAvailable(tinygltf)

# nativefiledialog-extended: real native save/open dialogs (Cocoa NSSavePanel on
# macOS) instead of pfd's unreliable osascript shell-out. Provides the `nfd` target.
FetchContent_Declare(
  nfd
  URL https://github.com/btzy/nativefiledialog-extended/archive/refs/tags/v1.3.0.tar.gz
)
FetchContent_MakeAvailable(nfd)

if(MAP_EDITOR_CPP_ENABLE_PROFILER)
  set(EASY_PROFILER_NO_GUI ON CACHE BOOL "" FORCE)
  set(EASY_PROFILER_NO_SAMPLES ON CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    easy_profiler
    URL https://github.com/yse/easy_profiler/archive/cc0e154635e5433344c4fb4fe940939a05465701.tar.gz
  )
  FetchContent_MakeAvailable(easy_profiler)
endif()

add_library(map_editor_core
  src/buildblock_catalog.cpp
  src/editor_state.cpp
  src/formats.cpp
  src/scene.cpp
)
target_include_directories(map_editor_core PUBLIC src)
if(MAP_EDITOR_CPP_ENABLE_PROFILER)
  target_compile_definitions(map_editor_core PUBLIC
    BUILD_WITH_EASY_PROFILER
    MAP_EDITOR_CPP_ENABLE_PROFILER=1
  )
  target_link_libraries(map_editor_core PUBLIC easy_profiler)
endif()
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
if(MAP_EDITOR_CPP_ENABLE_PROFILER)
  target_compile_definitions(map_editor_cpp PRIVATE
    BUILD_WITH_EASY_PROFILER
    MAP_EDITOR_CPP_ENABLE_PROFILER=1
  )
  target_link_libraries(map_editor_cpp PRIVATE easy_profiler)
endif()
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
