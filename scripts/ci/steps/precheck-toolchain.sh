#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Antoine Durand
# precheck-toolchain.sh — fail fast if the host toolchain doesn't match the
# supported target declared in scripts/ci/expected-toolchain.tsv.
#
# Called first thing by run-tier.sh so a run on a wrong-version box (e.g. a
# stale 24.04 container with gcc-13 / cmake-3) aborts immediately with a clear
# table, rather than dying minutes deep in a cryptic compile error.
#
# Exit 0 = all checks pass.  Exit 1 = at least one mismatch / missing tool.
# Override (NOT recommended — defeats the purpose) with DIORAMAP_SKIP_PRECHECK=1.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MANIFEST="${1:-$SCRIPT_DIR/../expected-toolchain.tsv}"

if [[ "${DIORAMAP_SKIP_PRECHECK:-0}" == "1" ]]; then
    echo "[precheck] DIORAMAP_SKIP_PRECHECK=1 — skipping toolchain version gate (not recommended)" >&2
    exit 0
fi

if [[ ! -f "$MANIFEST" ]]; then
    echo "[precheck] FATAL: manifest not found: $MANIFEST" >&2
    exit 1
fi

# Return the actual MAJOR version (or "present"/"") for a tool.
query_actual() {
    local tool="$1" c
    case "$tool" in
        os)       ( . /etc/os-release 2>/dev/null && printf '%s' "${VERSION_ID:-}" ) ;;
        gcc)      command -v gcc  >/dev/null 2>&1 && gcc  -dumpversion 2>/dev/null | cut -d. -f1 ;;
        g++)      command -v g++  >/dev/null 2>&1 && g++  -dumpversion 2>/dev/null | cut -d. -f1 ;;
        node)     command -v node >/dev/null 2>&1 && node -v 2>/dev/null | sed 's/^v//' | cut -d. -f1 ;;
        clang)    command -v clang >/dev/null 2>&1 && \
                      clang --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+' | head -1 | cut -d. -f1 ;;
        cmake)    command -v cmake >/dev/null 2>&1 && \
                      cmake --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+' | head -1 | cut -d. -f1 ;;
        llvm)     # prefer versioned tool, fall back to unversioned — matches how
                  # the coverage scripts resolve llvm-profdata/llvm-cov.
                  for c in llvm-config-21 llvm-config llvm-profdata-21 llvm-profdata; do
                      if command -v "$c" >/dev/null 2>&1; then
                          "$c" --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+' | head -1 | cut -d. -f1
                          return
                      fi
                  done ;;
        *)        command -v "$tool" >/dev/null 2>&1 && echo present ;;
    esac
}

printf '[precheck] toolchain gate (target per %s)\n' "${MANIFEST#"$SCRIPT_DIR/../"}"
printf '  %-10s %-10s %-10s %s\n' TOOL EXPECTED ACTUAL RESULT

fails=0
checks=0
while IFS=$'\t' read -r tool expected mode; do
    [[ -z "${tool:-}" || "$tool" == \#* ]] && continue
    checks=$((checks + 1))
    actual="$(query_actual "$tool")"
    result=PASS
    case "$mode" in
        present) [[ -n "$actual" ]] || { result=FAIL; }; expected="(any)" ;;
        exact)   [[ "$actual" == "$expected" ]] || result=FAIL ;;
        major)   [[ -n "$actual" && "$actual" == "$expected" ]] || result=FAIL ;;
        *)       result="FAIL(bad-mode:$mode)" ;;
    esac
    [[ "$result" == PASS ]] || fails=$((fails + 1))
    printf '  %-10s %-10s %-10s %s\n' "$tool" "$expected" "${actual:-MISSING}" "$result"
done < "$MANIFEST"

if [[ "$fails" -ne 0 ]]; then
    echo "[precheck] FAIL — $fails/$checks checks failed. This host is not a supported target." >&2
    echo "[precheck] Expected Ubuntu 26.04 toolchain (gcc-15 / clang-21 / cmake-4 / node-24)." >&2
    echo "[precheck] Rebuild on a 26.04 target, or (not recommended) set DIORAMAP_SKIP_PRECHECK=1." >&2
    exit 1
fi

echo "[precheck] OK — $checks/$checks toolchain checks passed."
exit 0
