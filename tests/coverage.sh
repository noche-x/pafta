#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${PAFTA_COVERAGE_BUILD_DIR:-$repo_dir/build/coverage}"
compiler="${CXX:-clang++}"
profdata="${LLVM_PROFDATA:-llvm-profdata}"
cov="${LLVM_COV:-llvm-cov}"

cmake -S "$repo_dir" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER="$compiler" \
  -DPAFTA_BUILD_TESTS=ON \
  -DPAFTA_ENABLE_COVERAGE=ON \
  "$@"
cmake --build "$build_dir" --target pafta pafta_tests binary_export_tests editor_state_tests editor_workflow_tests selection_tests project_file_jobs_tests mesh_export_tests --parallel

mkdir -p "$build_dir/profiles"
rm -f "$build_dir"/profiles/*.profraw
LLVM_PROFILE_FILE="$build_dir/profiles/%m-%p.profraw" ctest --test-dir "$build_dir" --output-on-failure --no-tests=error

"$profdata" merge -sparse "$build_dir"/profiles/*.profraw -o "$build_dir/coverage.profdata"
core_sources=(
  "$repo_dir/src/buildblock_catalog.cpp"
  "$repo_dir/src/editor_state.cpp"
  "$repo_dir/src/formats.cpp"
  "$repo_dir/src/scene.cpp"
)
"$cov" report "$build_dir/pafta_tests" \
  "-instr-profile=$build_dir/coverage.profdata" \
  "${core_sources[@]}" > "$build_dir/coverage.txt"
"$cov" show "$build_dir/pafta_tests" \
  "-instr-profile=$build_dir/coverage.profdata" \
  -show-branches=count -format=html "-output-dir=$build_dir/coverage-html" \
  "${core_sources[@]}"
"$cov" report "$build_dir/mesh_export_tests" \
  "-instr-profile=$build_dir/coverage.profdata" \
  "$repo_dir/src/mesh_export.cpp" > "$build_dir/coverage-export.txt"
"$cov" report "$build_dir/editor_workflow_tests" \
  "-instr-profile=$build_dir/coverage.profdata" -show-functions \
  '-name-regex=save_map_to|undo_city|redo_city|commit_city_edits|rename_building|save_archive_to|import_project_archive' \
  "$repo_dir/src/project_io.cpp" "$repo_dir/src/edit_history.cpp" "$repo_dir/src/ui_archive.cpp" > "$build_dir/coverage-workflows.txt"
cat "$build_dir/coverage.txt"
cat "$build_dir/coverage-export.txt"
cat "$build_dir/coverage-workflows.txt"
# Preserve the line floor; branch and export floors keep their gaps visible in CI.
awk '$1 == "TOTAL" { found = 1; if ($10 + 0 < 75 || $13 + 0 < 70) exit 1 } END { if (!found) exit 1 }' "$build_dir/coverage.txt"
awk '$1 == "TOTAL" { found = 1; if ($10 + 0 < 75 || $13 + 0 < 50) exit 1 } END { if (!found) exit 1 }' "$build_dir/coverage-export.txt"
