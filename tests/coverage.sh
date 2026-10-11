#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${MAP_EDITOR_CPP_COVERAGE_BUILD_DIR:-$repo_dir/build/coverage}"
compiler="${CXX:-clang++}"
profdata="${LLVM_PROFDATA:-llvm-profdata}"
cov="${LLVM_COV:-llvm-cov}"

cmake -S "$repo_dir" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER="$compiler" \
  -DMAP_EDITOR_CPP_BUILD_TESTS=ON \
  -DMAP_EDITOR_CPP_ENABLE_COVERAGE=ON \
  "$@"
cmake --build "$build_dir" --target map_editor_cpp map_editor_tests binary_export_tests editor_state_tests mesh_export_tests --parallel

mkdir -p "$build_dir/profiles"
rm -f "$build_dir"/profiles/*.profraw
LLVM_PROFILE_FILE="$build_dir/profiles/%p.profraw" ctest --test-dir "$build_dir" --output-on-failure

"$profdata" merge -sparse "$build_dir"/profiles/*.profraw -o "$build_dir/coverage.profdata"
core_sources=(
  "$repo_dir/src/buildblock_catalog.cpp"
  "$repo_dir/src/editor_state.cpp"
  "$repo_dir/src/formats.cpp"
  "$repo_dir/src/scene.cpp"
)
"$cov" report "$build_dir/map_editor_tests" \
  "-instr-profile=$build_dir/coverage.profdata" \
  "${core_sources[@]}" > "$build_dir/coverage.txt"
"$cov" show "$build_dir/map_editor_tests" \
  "-instr-profile=$build_dir/coverage.profdata" \
  -format=html "-output-dir=$build_dir/coverage-html" \
  "${core_sources[@]}"
"$cov" report "$build_dir/mesh_export_tests" \
  "-instr-profile=$build_dir/coverage.profdata" \
  "$repo_dir/src/mesh_export.cpp" > "$build_dir/coverage-export.txt"
cat "$build_dir/coverage.txt"
cat "$build_dir/coverage-export.txt"
awk '$1 == "TOTAL" { if ($10 + 0 < 75) exit 1; found = 1 } END { if (!found) exit 1 }' "$build_dir/coverage.txt"
