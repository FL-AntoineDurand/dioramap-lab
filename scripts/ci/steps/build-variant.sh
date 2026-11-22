#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# Configure + build one variant.
#
# Usage: build-variant.sh <variant> [<build-log.txt>]
#
# Produces a working build dir at core/build-<variant>/ with the
# dioramap-processor binary (when the variant supports it) and all
# registered test executables ready for ctest.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
. "$REPO_ROOT/scripts/ci/lib/variants.sh"

VARIANT="${1:?usage: build-variant.sh <variant> [build-log]}"
LOG="${2:-/dev/stderr}"

variant_known "$VARIANT" || { echo "unknown variant: $VARIANT" >&2; exit 2; }

BUILD_DIR="$REPO_ROOT/$(variant_build_dir "$VARIANT")"
FLAGS=$(variant_cmake_flags "$VARIANT")
CC_ENV=""
if variant_needs_clang "$VARIANT"; then
    CC_ENV="CC=clang CXX=clang++"
fi

# Extra cmake flags injected by the caller (the tier dispatcher). The full
# tier sets DIORAMAP_EXTRA_CMAKE_FLAGS=-DDIORAMAP_RESEARCH=ON so the research
# step libs (PolyFit/CGAL/Roofer) get built + audited; fast tier leaves it
# empty (research stays OFF — its CMake default). v7 builds in every tier
# regardless (DIORAMAP_V7 defaults ON). The configure runs under `env -i`,
# so this var must be threaded through explicitly below.
EXTRA_FLAGS="${DIORAMAP_EXTRA_CMAKE_FLAGS:-}"

echo "[build] variant=$VARIANT  dir=${BUILD_DIR#$REPO_ROOT/}  flags='$FLAGS $EXTRA_FLAGS'  env='$CC_ENV'"
mkdir -p "$BUILD_DIR"

# Configure (idempotent — only re-runs if CMakeCache.txt is older than CMakeLists).
{
    echo "==== configure: variant=$VARIANT $(date -Is) ===="
    env -i HOME="$HOME" PATH="$PATH" $CC_ENV \
        cmake -B "$BUILD_DIR" -S "$REPO_ROOT/core" \
              -GNinja $FLAGS $EXTRA_FLAGS
    echo
    echo "==== build: variant=$VARIANT $(date -Is) ===="
    ninja -C "$BUILD_DIR"
} >>"$LOG" 2>&1

echo "[build] variant=$VARIANT done"
