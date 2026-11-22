#!/usr/bin/env bash
# setup-quality-vm.sh — provision a fresh Ubuntu 26.04 box to run the FULL
# dioramap v3 quality suite (every tier: ctest, ASan/UBSan, TSan, Valgrind,
# coverage, Frama-C audit, fuzz, CPD/jscpd).
#
# Target: a real-Linux host (e.g. a Hyper-V Ubuntu 26.04 VM, or a hosted
# ubuntu-26.04 CI runner) — NOT a restricted/WSL2 sandbox, which can't run
# TSan. See docs/quality/ROADMAP.md §3 and docs/quality/UPGRADE-26.04.md.
#
# 26.04 toolchain (confirmed): gcc/g++ 15, clang/llvm 21, cmake 4.2, ninja 1.13.
# The coverage scripts resolve llvm-cov/llvm-profdata version-agnostically and
# pick up the 21 series here (was 18 on 24.04).
#
# Run as a NORMAL user that has sudo (not root):
#   # fetched standalone:
#   curl -fsSL https://raw.githubusercontent.com/FL-AntoineDurand/lidar-algos/main/v3/docs/quality/setup-quality-vm.sh -o setup-quality-vm.sh
#   bash setup-quality-vm.sh
#   # or from a checkout (also runs the repo bootstrap automatically):
#   bash v3/docs/quality/setup-quality-vm.sh
#
# Flags:
#   --system-only   install system packages/tools only; skip the repo bootstrap
#   --no-opam       skip the opam/Frama-C install (use `apt install frama-c` yourself)
#
# Idempotent: safe to re-run. Sizing reminder: >=8 GB RAM, ~100 GB disk.

set -euo pipefail

SYSTEM_ONLY=0
USE_OPAM=1
for a in "$@"; do
    case "$a" in
        --system-only) SYSTEM_ONLY=1 ;;
        --no-opam)     USE_OPAM=0 ;;
        -h|--help)     sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "unknown flag: $a" >&2; exit 2 ;;
    esac
done

if [ "$(id -u)" = "0" ]; then
    echo "Run as a normal user with sudo, not as root (opam dislikes root)." >&2
    exit 1
fi

say() { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }

# ── 0. sanity ────────────────────────────────────────────────────────
if [ -r /etc/os-release ]; then
    . /etc/os-release
    [ "${VERSION_ID:-}" = "26.04" ] || echo "WARN: tuned for Ubuntu 26.04, found ${PRETTY_NAME:-unknown}"
fi

# ── 1. APT: build + dev libs + research geo stack + quality toolchain ──
say "apt: build toolchain, dev libs, CGAL/PolyFit/roofer geo stack, quality tools"
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config git ca-certificates curl wget unzip zip jq \
    clang clang-format clang-tidy lld-21 llvm-21 gdb valgrind \
    libwebsockets-dev libcurl4-openssl-dev libproj-dev libjpeg-dev \
    libjansson-dev libsqlite3-dev libssl-dev libxxhash-dev sqlite3 \
    libcgal-dev libgmp-dev libmpfr-dev libfmt-dev libeigen3-dev libboost-dev \
    python3 python3-websockets default-jdk
#  clang/llvm 21 (26.04 default) → llvm-cov-21 + llvm-profdata-21; the coverage
#                     scripts resolve these version-agnostically (unversioned
#                     fallback = 21 here). Also the ASan/UBSan/TSan/libFuzzer
#                     runtimes. Bump lld-21/llvm-21 in lockstep with the distro
#                     clang major on future LTS upgrades.
#  default-jdk      → PMD CPD (PMD itself is auto-downloaded by run-cpd.sh).
#  python3-websockets → the serve_* integration tests (Python WS clients).
#  NOT installed (not needed by the quality suite): postgresql, CUDA, libscip-dev.

# ── 2. Node LTS (NodeSource — apt's nodejs is too old for lab-ui/jscpd/bench-v2) ──
if ! command -v node >/dev/null 2>&1 || [ "$(node -v 2>/dev/null | sed 's/v\([0-9]*\).*/\1/')" -lt 20 ] 2>/dev/null; then
    say "node: NodeSource LTS"
    curl -fsSL https://deb.nodesource.com/setup_lts.x | sudo -E bash -
    sudo apt-get install -y nodejs
else
    echo "node $(node -v) already present — skipping NodeSource"
fi

# ── 3. Frama-C via opam (gets a controllable version + the WP plugin) ──
if [ "$USE_OPAM" = "1" ]; then
    say "opam + Frama-C"
    sudo apt-get install -y opam
    if [ ! -d "$HOME/.opam" ]; then
        opam init --disable-sandboxing -y
    fi
    eval "$(opam env)"
    opam install -y frama-c
    # Persist the opam env so frama-c is on PATH in future interactive shells
    # (run-tier.sh inherits the invoking shell's PATH; without this the
    # Frama-C tier step can't find `frama-c`).
    if ! grep -q 'opam env' "$HOME/.bashrc" 2>/dev/null; then
        echo 'eval "$(opam env 2>/dev/null)"' >> "$HOME/.bashrc"
    fi
    # For the WP proofs (ROADMAP backlog #6 / gap #11), later run:
    #   opam install -y alt-ergo why3 && why3 config detect
else
    echo "skipping opam (--no-opam). Install Frama-C yourself: sudo apt install frama-c"
fi

# ── 4. Sanitizer ASLR fix — high mmap_rnd_bits breaks TSan AND ASan ────
# This was needed on 24.04 (default 32). Re-confirm on the 26.04 kernel:
# if `cat /proc/sys/vm/mmap_rnd_bits` is already <=28, this is a no-op.
say "sysctl: vm.mmap_rnd_bits=28 (TSan/ASan ASLR fix)"
echo 'vm.mmap_rnd_bits=28' | sudo tee /etc/sysctl.d/10-sanitizers.conf >/dev/null
sudo sysctl --system >/dev/null

# ── 5. Repo bootstrap (auto-detected when run from a checkout) ─────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
V3_DIR="$(cd "$SCRIPT_DIR/../.." 2>/dev/null && pwd || true)"   # docs/quality -> v3
if [ "$SYSTEM_ONLY" = "0" ] && [ -x "$V3_DIR/scripts/fetch-third-party.sh" ]; then
    say "repo bootstrap: third-party deps + npm installs"
    ( cd "$V3_DIR" && ./scripts/fetch-third-party.sh )
    BENCH="$(cd "$V3_DIR/../lidar-algos-bench-v2" 2>/dev/null && pwd || true)"
    [ -f "$BENCH/package.json" ] && ( echo "npm install (bench-v2)"; cd "$BENCH" && npm install --no-audit --no-fund )
    [ -f "$V3_DIR/apps/lab-ui/package.json" ] && ( echo "npm install (lab-ui)"; cd "$V3_DIR/apps/lab-ui" && npm install --no-audit --no-fund )
else
    say "repo bootstrap: SKIPPED"
    echo "  Run from a checkout (or after cloning) to bootstrap the repo:"
    echo "    cd v3 && ./scripts/fetch-third-party.sh"
    echo "    (cd ../lidar-algos-bench-v2 && npm install)   # Stream-B harness"
    echo "    (cd apps/lab-ui && npm install)               # lab-ui / jscpd target"
fi

# ── 6. Verify ─────────────────────────────────────────────────────────
say "versions"
for t in gcc clang-21 cmake ninja llvm-cov-21 llvm-profdata-21 valgrind \
         node npm python3 jq java frama-c; do
    p="$(command -v "$t" 2>/dev/null || true)"
    printf "  %-18s %s\n" "$t" "${p:-MISSING}"
done

# Cross-check against the canonical target manifest — the same gate run-tier.sh
# enforces (scripts/ci/expected-toolchain.tsv). Informational here (don't abort
# the provisioner), but it surfaces any version mismatch before the first run.
if [ -f "$V3_DIR/scripts/ci/steps/precheck-toolchain.sh" ]; then
    say "toolchain precheck (vs scripts/ci/expected-toolchain.tsv)"
    bash "$V3_DIR/scripts/ci/steps/precheck-toolchain.sh" \
        || echo "WARN: toolchain precheck reported mismatches (see table above)"
fi

say "done"
echo "Next:  cd v3 && ./scripts/ci/run-tier.sh full"
echo "       (TSan/Valgrind/Frama-C now run — unlike a WSL2 sandbox.)"
