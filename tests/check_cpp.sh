#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-$repo_dir/build/quality}"

# Check error-prone CPU paths; formatting and third-party code are outside this check.
for source in buildblock_catalog editor_state formats scene app project_io edit_history ui_archive mesh_export; do
  "${CLANG_TIDY:-clang-tidy}" -p "$build_dir" \
    --checks='-*,clang-analyzer-core.*,clang-analyzer-cplusplus.*,bugprone-use-after-move,bugprone-sizeof-expression' \
    --warnings-as-errors='*' "$repo_dir/src/$source.cpp"
done
