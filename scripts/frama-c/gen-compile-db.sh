#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# Generate compile_commands.json for Frama-C without compiling the program.
# Idempotent. Safe to re-run.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build-framac"

cd "$REPO_ROOT"

# Configure-only. -DDIORAMAP_BUILD_TESTS=ON so test sources are included
# in compile_commands.json (we may need their flags to compile micro-tests).
# DIORAMAP_RESEARCH=ON pulls in polyfit/ksr which are normally off; we
# leave them OFF here — Frama-C can analyze them via direct invocation if
# needed, but they pull C++ deps that bloat configure time.
cmake -S core -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DDIORAMAP_BUILD_TESTS=ON \
    -DDIORAMAP_RESEARCH=OFF \
    -DCMAKE_C_FLAGS="-DDIORAMAP_FRAMA_C_AUDIT=1" >/dev/null

cp "$BUILD_DIR/compile_commands.json" "$REPO_ROOT/compile_commands.json"
echo "wrote: $REPO_ROOT/compile_commands.json ($(wc -l < "$REPO_ROOT/compile_commands.json") lines)"
