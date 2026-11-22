# Integration test scenarios

Companion to [docs/quality/README.md](../../../docs/quality/README.md).
**Manual invocation only** — not yet wired into CI.

## What's here

```
tests/integration/
├── run_scenario.sh        # orchestrator: run one scenario
├── run_scenario_under.sh  # same, wrapped in valgrind (memcheck/helgrind/drd)
├── lib/
│   └── isolation.sh       # port / cache / log / output isolation primitives
├── clients/
│   └── node-ws.js         # shared WebSocket client (commands: run, burst,
│                          #   parallel-runs, cancel-after, close-after, …)
├── scenarios/
│   ├── batch_demo_chain_warm.sh     # cache-hit class, batch mode
│   ├── serve_all_cache_hit_burst.sh # journal-acc race repro, server mode
│   ├── serve_concurrent_clients.sh  # parallel sessions; sessions_lock stress
│   ├── serve_late_subscribe.sh      # run-twice path on one session
│   ├── serve_cancel_during_run.sh   # CANCEL races worker callbacks
│   └── serve_disconnect_during_run.sh  # abrupt close mid-run; grace path
├── valgrind.supp          # suppressions (curated)
├── package.json           # ws npm dep for the node client
└── README.md              # this file
```

## Isolation contract

Every scenario MUST:

- Source `lib/isolation.sh` and call `isolation_init <name>`.
- Use `alloc_port` (kernel-assigned ephemeral port) for any TCP listener.
- Use `$SCEN_CACHE_DIR`, `$SCEN_OUTPUT_DIR`, `$SCEN_LOG` (all under one per-scenario `mktemp -d`).
- Use `$SCEN_BIN` (resolved by the orchestrator) for the processor binary.
- Call `start_processor_serve` for serve mode and `stop_processor` (or rely on the EXIT trap).

What this guarantees: **no scenario can collide with a dev or prod
processor running on the same box.** No fixed ports, no
`/tmp/dioramap-cache`, no `/tmp/dioramap-processor.log`. The
per-scenario tmpdir is auto-cleaned on success and **preserved on
failure** (look under `/tmp/dioramap-it-*` for triage).

## Running scenarios

### Manually, against any binary

```bash
# Build whichever variant you want (gcc release, ASan, TSan, …).
cd v3/core
mkdir -p build && (cd build && cmake -GNinja .. && ninja)

# One scenario:
./tests/integration/run_scenario.sh batch_demo_chain_warm \
    ./build/dioramap-processor

# All scenarios against one binary:
for s in tests/integration/scenarios/*.sh; do
    n=$(basename "$s" .sh)
    ./tests/integration/run_scenario.sh "$n" ./build/dioramap-processor || break
done
```

### Under a sanitizer

The matrix's value is running the same scenarios under different
build variants.  Pick a variant via cmake options:

```bash
# ASan + UBSan (current dev-up.sh default)
mkdir -p build-asan && (cd build-asan && \
    cmake -GNinja -DDIORAMAP_ASAN=ON .. && ninja dioramap-processor)
./tests/integration/run_scenario.sh serve_all_cache_hit_burst \
    ./build-asan/dioramap-processor

# TSan + UBSan (mutually exclusive with ASan)
mkdir -p build-tsan && (cd build-tsan && \
    CC=clang cmake -GNinja -DDIORAMAP_TSAN=ON .. && ninja dioramap-processor)
./tests/integration/run_scenario.sh serve_all_cache_hit_burst \
    ./build-tsan/dioramap-processor

# Coverage (clang only; .profraw files land in $LLVM_PROFILE_FILE)
mkdir -p build-cov && (cd build-cov && \
    CC=clang cmake -GNinja -DDIORAMAP_COVERAGE=ON .. && ninja dioramap-processor)
LLVM_PROFILE_FILE=/tmp/dioramap-%p.profraw \
    ./tests/integration/run_scenario.sh batch_demo_chain_warm \
    ./build-cov/dioramap-processor
llvm-profdata merge -sparse /tmp/dioramap-*.profraw -o /tmp/dioramap.profdata
llvm-cov report ./build-cov/dioramap-processor \
    -instr-profile=/tmp/dioramap.profdata
```

### Under valgrind

```bash
# Memcheck (uninit reads + leaks; ~10-30× slowdown)
./tests/integration/run_scenario_under.sh memcheck \
    batch_demo_chain_warm ./build/dioramap-processor

# Helgrind (data races; ~30-100× slowdown — slow!)
./tests/integration/run_scenario_under.sh helgrind \
    serve_concurrent_clients ./build/dioramap-processor

# DRD (data races + lock-order; complementary to helgrind)
./tests/integration/run_scenario_under.sh drd \
    serve_all_cache_hit_burst ./build/dioramap-processor
```

Use the **release** binary (no -fsanitize=…) for valgrind — sanitizer
runtimes confuse it, and the value of valgrind is exactly that it
runs on uninstrumented code.

## Fuzzing (clang only)

```bash
# Requires libclang-rt-18-dev (apt install libclang-rt-18-dev).
mkdir -p build-fuzz && (cd build-fuzz && \
    CC=clang CXX=clang++ cmake -GNinja \
        -DDIORAMAP_FUZZ=ON -DDIORAMAP_BUILD_TESTS=OFF .. && \
    ninja fuzz_hash_hex fuzz_run_journal_parse)

# Run for one hour (resume by reusing the same corpus dir):
./build-fuzz/tests/fuzz/fuzz_run_journal_parse \
    tests/fuzz/corpus/fuzz_run_journal_parse \
    -max_total_time=3600
# Crashes saved as crash-<sha>; reproduce with:
./build-fuzz/tests/fuzz/fuzz_run_journal_parse crash-<sha>
```

When a crash appears: fix the bug, then drop the crash file under
`tests/fuzz/corpus/<target>/` — it becomes a permanent regression
input.

## Tunables (env vars read by scenarios)

| var | default | meaning |
|---|---|---|
| `IT_BURST_N` | `100` | RUN count for `serve_all_cache_hit_burst` |
| `IT_PARALLEL_N` | `8` | client count for `serve_concurrent_clients` |
| `IT_PIPELINE` | `demo-chain` | pipeline name for the burst scenario |
| `DIORAMAP_LOG_LEVEL` | `info` | passed through to the spawned processor |

## Adding a scenario

A scenario joins the matrix only when it (a) covers a class of bug
we've actually seen, or (b) exercises a code path no other scenario
hits.  Speculative scenarios get rejected — see
[docs/quality/README.md](../../../docs/quality/README.md)
§6.2.

Template (start by copying `serve_all_cache_hit_burst.sh`):

```bash
#!/usr/bin/env bash
# <scenario-name>
#
# Bug class: <which class this defends against — name a CONCURRENCY.md
#             invariant or a BUG-*.md file>
#
# Approach: <one-paragraph what-you-do>
#
# Variants this matters most for: <asan / tsan / valgrind-mc / ...>

# shellcheck source=../lib/isolation.sh
. "$SCEN_LIB_DIR/isolation.sh"
isolation_init "<scenario-name>"
require "$SCEN_BIN" node jq

# ... scenario-specific work ...

assert_no_sanitizer_reports
pass "<short summary of success>"
```

## Dependencies

- `bash` ≥ 4
- `python3` (for `alloc_port`)
- `node` ≥ 18 (for the WS client; `parseArgs` from `node:util`)
- `jq` (assertion parsing)
- `npm install` once in `tests/integration/` (installs `ws` package)
- Optional: `valgrind` for the under-tools wrapper
- Optional: `clang` ≥ 17 + `libclang-rt-18-dev` for fuzz / TSan / coverage
