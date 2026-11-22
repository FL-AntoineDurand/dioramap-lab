#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# Build-variant registry.
#
# Each variant maps to (cmake flags, compiler env, build-dir suffix).
# Adding a new variant = one new branch in each function.  Keeps the
# tier scripts compositional — nothing else needs to change.

set -euo pipefail

variant_known() {
    case "$1" in
        release|asan|tsan|clang|cov|fuzz) return 0 ;;
        *) return 1 ;;
    esac
}

variant_cmake_flags() {
    case "$1" in
        release)  echo "-DCMAKE_BUILD_TYPE=Release" ;;
        asan)     echo "-DDIORAMAP_ASAN=ON" ;;
        tsan)     echo "-DDIORAMAP_TSAN=ON" ;;
        clang)    echo "" ;;
        cov)      echo "-DDIORAMAP_COVERAGE=ON" ;;
        fuzz)     echo "-DDIORAMAP_FUZZ=ON -DDIORAMAP_BUILD_TESTS=OFF" ;;
        *) echo "unknown variant: $1" >&2; return 1 ;;
    esac
}

# Extra cmake flags per tier, surfaced as a DIORAMAP_EXTRA_CMAKE_FLAGS value
# that build-variant.sh honours (it threads the env through its `env -i`
# configure). The CMake defaults are DIORAMAP_V7=ON, DIORAMAP_RESEARCH=OFF —
# i.e. every variant builds the v7 step lib + processor by default, but the
# dev-only research libs (PolyFit/CGAL/Roofer) stay OFF unless asked for.
#
# The full tier opts INTO the research libs so they're built + Frama-C-audited;
# the fast tier leaves them off (keeps it under its time budget and avoids the
# CGAL/C++ configure cost). Usage in the tier dispatcher's `full)` branch:
#
#     export DIORAMAP_EXTRA_CMAKE_FLAGS="$(tier_extra_cmake_flags full)"
#
# then run the builds as usual. fast/nightly/weekly return empty → research OFF.
tier_extra_cmake_flags() {
    case "$1" in
        *)    echo "" ;;
    esac
}

variant_needs_clang() {
    case "$1" in
        clang|tsan|cov|fuzz) return 0 ;;
        *) return 1 ;;
    esac
}

# Where this variant's build dir lives (relative to repo root).
variant_build_dir() {
    echo "core/build-$1"
}

# Whether this variant produces a working dioramap-processor binary.
# Fuzz builds don't; TSan builds may not on WSL2 (separate concern).
variant_has_binary() {
    case "$1" in
        fuzz) return 1 ;;
        *) return 0 ;;
    esac
}
