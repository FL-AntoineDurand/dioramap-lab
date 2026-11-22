#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# fetch-third-party.sh — clone + pin + patch every vendored dep under v3/third_party/.
#
# Idempotent: skips a dep if its directory already exists at the pinned SHA.
# Re-run after bumping a pin to update. Re-run after wiping the dir to refetch.
#
# Usage:
#   ./scripts/fetch-third-party.sh                       # fetch everything missing
#   ./scripts/fetch-third-party.sh --force               # delete + re-fetch everything
#   ./scripts/fetch-third-party.sh --check               # verify all deps present + at right SHA, exit non-zero if not
#   ./scripts/fetch-third-party.sh --only copc-lib laz-perf   # restrict to a subset (by registry name)
#
# --only <name>... limits every mode (fetch / --force / --check) to the named
# deps. Names match the registry's first field (copc-lib, laz-perf).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP="${REPO_ROOT}/third_party"

FORCE=0
CHECK=0
ONLY=()
parsing_only=0
for arg in "$@"; do
    if [[ "$parsing_only" -eq 1 ]]; then
        case "$arg" in
            --force) FORCE=1; parsing_only=0 ;;
            --check) CHECK=1; parsing_only=0 ;;
            --only)  : ;;  # tolerate a repeated flag; keep consuming names
            -*) echo "unknown arg: $arg" >&2; exit 2 ;;
            *) ONLY+=("$arg") ;;
        esac
        continue
    fi
    case "$arg" in
        --force) FORCE=1 ;;
        --check) CHECK=1 ;;
        --only)  parsing_only=1 ;;
        -h|--help) sed -n '1,40p' "$0"; exit 0 ;;
        *) echo "unknown arg: $arg" >&2; exit 2 ;;
    esac
done

# Returns 0 if $1 should be processed given the --only filter (empty = all).
selected() {
    [[ ${#ONLY[@]} -eq 0 ]] && return 0
    local n
    for n in "${ONLY[@]}"; do
        [[ "$n" == "$1" ]] && return 0
    done
    return 1
}

# ── Dep registry ────────────────────────────────────────────────────────────
# name | path (relative to repo root) | upstream URL | pinned SHA or tag

DEPS=(
    "copc-lib|third_party/copc-lib|https://github.com/RockRobotic/copc-lib.git|v2.6.3"
    "laz-perf|third_party/copc-lib/libs/laz-perf|https://github.com/hobu/laz-perf.git|14522addcb01125499119990cc8fbc6b1e43b148"
)

# ── Patches ─────────────────────────────────────────────────────────────────
# Functions: apply_patches_<name> runs after clone + checkout for that dep.
# Keep them tiny — if a patch grows beyond a few lines, promote to a real
# .patch file under scripts/patches/ and `git apply` it.

apply_patches_copc_lib() {
    # copc-lib's CMakeLists conditionally adds libs/laz-perf via
    #   if(EXISTS "${CMAKE_SOURCE_DIR}/libs/laz-perf")
    # which only works when copc-lib is the top-level project. We add_subdirectory()
    # it from core/CMakeLists.txt, so the top-level is core/, not copc-lib/.
    # Use CMAKE_CURRENT_SOURCE_DIR so the check resolves against copc-lib's own dir.
    local f="${TP}/copc-lib/CMakeLists.txt"
    if grep -q 'CMAKE_CURRENT_SOURCE_DIR}/libs/laz-perf' "$f"; then
        return 0   # already patched
    fi
    sed -i 's|CMAKE_SOURCE_DIR}/libs/laz-perf|CMAKE_CURRENT_SOURCE_DIR}/libs/laz-perf|' "$f"
}
apply_patches_laz_perf()  { :; }   # no patches today
# ── Helpers ─────────────────────────────────────────────────────────────────

current_sha() {
    git -C "$1" rev-parse HEAD 2>/dev/null || echo ""
}

# Resolve a possibly-tag-or-sha pin to the SHA in an existing checkout.
resolve_pin() {
    local dir="$1" pin="$2"
    git -C "$dir" rev-parse "$pin^{commit}" 2>/dev/null \
        || git -C "$dir" rev-parse "$pin" 2>/dev/null \
        || echo ""
}

fetch_one() {
    local name="$1" path="$2" url="$3" pin="$4"
    local abs="${REPO_ROOT}/${path}"
    local pinned_sha

    if [[ "$FORCE" -eq 1 && -d "$abs" ]]; then
        echo "[$name] --force, removing $path"
        rm -rf "$abs"
    fi

    if [[ -d "$abs/.git" ]]; then
        pinned_sha=$(resolve_pin "$abs" "$pin")
        local cur
        cur=$(current_sha "$abs")
        if [[ -n "$pinned_sha" && "$cur" == "$pinned_sha" ]]; then
            echo "[$name] up-to-date at ${pin} (${cur:0:12})"
            return 0
        fi
        echo "[$name] exists but not at $pin (was ${cur:0:12}); checking out..."
        git -C "$abs" fetch --depth 1 origin "$pin" 2>/dev/null \
            || git -C "$abs" fetch origin
        git -C "$abs" checkout --detach FETCH_HEAD 2>/dev/null \
            || git -C "$abs" checkout --detach "$pin"
    elif [[ -d "$abs" && ! -d "$abs/.git" ]]; then
        # Directory exists but isn't a git checkout (e.g. left over from the
        # old vendored copc-lib).  Refuse to clobber; ask user.
        echo "[$name] ERROR: $abs exists and isn't a git checkout. Move or delete it, then re-run." >&2
        return 1
    else
        mkdir -p "$(dirname "$abs")"
        echo "[$name] cloning $url @ $pin"
        # Try a shallow clone with the pin; fall back to full clone if pin
        # isn't a branch/tag the remote advertises (which is the case for raw SHAs).
        if git clone --depth 1 --branch "$pin" "$url" "$abs" 2>/dev/null; then
            :
        else
            git clone "$url" "$abs"
            git -C "$abs" checkout --detach "$pin"
        fi
    fi

    # Apply patches — function name = name with '-' → '_'.
    local fn="apply_patches_${name//-/_}"
    if declare -F "$fn" >/dev/null; then
        echo "[$name] applying patches via $fn"
        "$fn"
    fi
}

check_one() {
    local name="$1" path="$2" _url="$3" pin="$4"
    local abs="${REPO_ROOT}/${path}"
    if [[ ! -d "$abs" ]]; then
        echo "[$name] MISSING ($path)"
        return 1
    fi
    if [[ ! -d "$abs/.git" ]]; then
        # Vendored / non-git checkout; can't verify SHA.  Just confirm presence.
        echo "[$name] present (non-git)"
        return 0
    fi
    local pinned cur
    pinned=$(resolve_pin "$abs" "$pin")
    cur=$(current_sha "$abs")
    if [[ "$cur" == "$pinned" ]]; then
        echo "[$name] OK ($pin)"
        return 0
    fi
    echo "[$name] DRIFT: have ${cur:0:12}, want $pin (${pinned:0:12})"
    return 1
}

# ── Main ────────────────────────────────────────────────────────────────────

mkdir -p "$TP"

rc=0
for entry in "${DEPS[@]}"; do
    IFS='|' read -r name path url pin <<<"$entry"
    selected "$name" || continue
    if [[ "$CHECK" -eq 1 ]]; then
        check_one "$name" "$path" "$url" "$pin" || rc=1
    else
        fetch_one "$name" "$path" "$url" "$pin"
    fi
done

# Guard against an --only typo silently fetching nothing.
if [[ ${#ONLY[@]} -gt 0 ]]; then
    matched=0
    for entry in "${DEPS[@]}"; do
        IFS='|' read -r name _ _ _ <<<"$entry"
        selected "$name" && matched=$((matched + 1))
    done
    if [[ "$matched" -eq 0 ]]; then
        echo "ERROR: --only matched no registry deps: ${ONLY[*]}" >&2
        echo "       known: copc-lib laz-perf" >&2
        exit 2
    fi
fi

exit "$rc"
