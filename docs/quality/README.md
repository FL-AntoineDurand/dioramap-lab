# Quality — what's tested, how to run it, what's measured

Single entry point for everything CI-shaped: how to reproduce the test
matrix locally, what each layer attests, what's not yet covered.

## How to run

Three commands cover every loop:

```bash
# One-time on a fresh clone — fetches third-party deps into v3/third_party/
./scripts/fetch-third-party.sh

# Fast tier (~30 s on a warm box): release + asan ctest, 4 essential scenarios
./scripts/ci/run-tier.sh fast

# Full tier (~5 min): adds clang, coverage, all scenarios, Frama-C audit,
# code-duplication detection (CPD for C, jscpd for lab-ui JS — report-only)
./scripts/ci/run-tier.sh full
```

Every run lands in `ci-artifacts/<timestamp>-<sha>-<tier>/`:

```
ci-artifacts/<run-id>/
  summary.json                     overall outcome + paths
  junit/                           JUnit XML per suite (ctest, integration, frama-c, cpd-c, jscpd-js)
  integration-logs-<variant>/      per-scenario stdout
  build-logs/                      configure + ninja output per variant
  coverage/                        all.profdata + html/ + summary.json (Full only)
  cpd-report/                      PMD CPD raw reports (Full only; report-only)
  jscpd-report/                    jscpd JSON report (Full only; report-only)
  allure-report/index.html         polished HTML aggregate (auto-generated)
```

Open the report:

```bash
# In a browser:
firefox $(ls -td ci-artifacts/*-full | head -1)/allure-report/index.html

# Or serve on localhost (also auto-rebuilds the report from junit/):
allure serve $(ls -td ci-artifacts/*-full | head -1)/junit
```

The artifact tree is gitignored. Each run is ~3 MB.

## What's tested

Fourteen surfaces, top to bottom. The README in each subdir or script
header is the authoritative spec; this table just maps them.

| # | Layer | Driven by | Quality property attested |
|---|---|---|---|
| 1 | ctest under **gcc release** | `core/tests/CMakeLists.txt` | functional correctness baseline |
| 2 | ctest under **gcc ASan + UBSan** | `-DDIORAMAP_ASAN=ON` | heap/stack memory safety, LSan leaks, signed-overflow, alignment |
| 3 | ctest under **clang (plain)** | `CC=clang` | static lock-pairing via `-Wthread-safety` |
| 4 | **Integration scenarios** under ASan | `core/tests/integration/scenarios/*.sh` | protocol correctness, cache + journal correctness under fan-out, race regression |
| 5 | **Coverage measurement** | `-DDIORAMAP_COVERAGE=ON` (clang) | test reach into `core/src/` + `steps/` |
| 6 | **Frama-C audit (modules)** | `scripts/frama-c/modules.tsv` | runtime safety: null-deref, OOB, signed overflow, divide-by-zero, uninit read, alignment |
| 7 | **Frama-C audit (v7 benches)** | `scripts/frama-c/v7-benches.tsv` | same, against v7 step impls via bench-as-harness |
| 8 | **ACSL contracts** on cross-subsystem headers | `core/include/v3/*.h` | by-construction public-API contracts (preconditions + postconditions) |
| 9 | **Valgrind memcheck** | `core/tests/integration/run_scenario_under.sh memcheck` | uninit reads ASan misses; validates uninstrumented binary |
| 10 | **Fuzz targets (libFuzzer)** | `core/tests/fuzz/` + `DIORAMAP_FUZZ=ON` | input-handling parser robustness |
| 11 | **TSan** (real-Linux only) | `-DDIORAMAP_TSAN=ON` (clang) | data races, lock-order inversion, missing happens-before |
| 12 | **`-Wthread-safety` annotations** | `core/include/v3/thread_safety.h` | compile-time proof of documented lock pattern |
| 13 | **Third-party reproducibility** | `scripts/fetch-third-party.sh --check` | "fresh clone builds" guarantee |
| 14 | **Code-duplication detection** (report-only) | `scripts/ci/steps/run-cpd.sh` (C: `steps/` + `core/src/`) + `scripts/ci/steps/run-jscpd.sh` (JS: `apps/lab-ui/`) | "one concept, N encodings" smell — copy-pasted decode loops / drift-prone clones surfaced for review (NON-gating) |

## What's measured (definitions, not current values)

Specific values for the last run are in `ci-artifacts/<run-id>/summary.json` and the Allure report. The definitions:

- **`tests passed / total`** — from `<testsuite tests="N" failures="F">` in the JUnit XMLs.
- **`scenarios passed / total`** — same shape, one testcase per scenario.
- **`frama-c clean / total`** — testcase passes iff (audit-script exit 0) ∧ (no "sure alarm (invalid status)" in `eva.log`). Unproven properties that aren't sure alarms are not counted as failures (most are stub-related — see `FRAMA-C-FALSE-POSITIVES.md`).
- **`line / function / region / branch %`** — from `llvm-cov report` over `core/src/` + `steps/`. Third-party opted out via `dioramap_opt_out_of_coverage()` in `core/CMakeLists.txt`.
- **`unproven-property count` per Frama-C target** — the `Unproven properties (count): N` line in `.frama-c-out/<target>/summary.txt`. Includes harmless stub-spec FPs; not gating.

## Build variants

Mutually-exclusive sanitizer flags are enforced at CMake configure time.

| Variant | CMake flag | Compiler | What it catches uniquely | Slowdown |
|---|---|---|---|---|
| release | `-DCMAKE_BUILD_TYPE=Release` | gcc | perf baseline; races that close under instrumentation | 1× |
| asan | `-DDIORAMAP_ASAN=ON` | gcc or clang | UAF, heap overflow, double-free, leaks (LSan), signed overflow, alignment | 2-3× |
| tsan | `-DDIORAMAP_TSAN=ON` | clang | data races, lock-order inversion, missing happens-before, condvar misuse | 5-10× |
| clang | (default with `CC=clang`) | clang | static `-Wthread-safety` annotations | ~1× compile cost only |
| cov | `-DDIORAMAP_COVERAGE=ON` | clang | gaps in test coverage | 1.5× |
| fuzz | `-DDIORAMAP_FUZZ=ON` | clang | input-handling bugs in parsers (length math, recursion, overflow) | n/a (separate binaries) |

Build-variant logic lives in `scripts/ci/lib/variants.sh`; add a new
variant by adding one branch per function there.

## Tier cadence

Per `scripts/ci/run-tier.sh`:

| Tier | Trigger | Wall time (warm) | Variants + scenarios | Failures block |
|---|---|---|---|---|
| `fast` | every push (when CI is wired) | < 30 s | release ctest, asan ctest, 4 essentials | merge |
| `full` | PR-ready / post-merge to `main` | < 5 min | release + asan + clang ctest, all 10 integration scenarios, coverage, Frama-C audit, code-duplication detection (CPD + jscpd, report-only) | merge |
| `nightly` | cron 02:00 (not yet wired) | < 8 h | Full + valgrind on critical scenarios + 1 h fuzz per target + TSan if real-Linux runner exists | opens issue |
| `weekly` | cron Sun (not yet wired) | < 24 h | + 24 h fuzz refresh + helgrind/drd on every scenario + 8 h random-workload soak under TSan | opens issue |

Cold-runner first-time cost is ~+2 min (fetch + build from scratch).

## Integration scenarios

The 10 scenarios under `core/tests/integration/scenarios/`. Each guards a specific bug class.

| Scenario | Bug class |
|---|---|
| `batch_demo_chain_warm` | cache-hit class, batch mode |
| `serve_all_cache_hit_burst` | journal-acc race repro (`BUG-cache-hit-runner-cleanup.md`) |
| `serve_concurrent_clients` | `sessions_lock` stress; N parallel sessions |
| `serve_late_subscribe` | run-twice path on one session |
| `serve_cancel_during_run` | CANCEL vs worker-callback race |
| `serve_disconnect_during_run` | abrupt close mid-run; grace path |
| `serve_invalidate_then_rerun` | cache invalidation correctness |
| `serve_list_runs` | LIST_RUNS handler / `run_journal_list` |
| `serve_reconnect_within_grace` | sessions_lock → session_t::lock nesting |
| `serve_rehydrate_run` | REHYDRATE_RUN (journal_id path) |

**Isolation contract**: every scenario gets its own `mktemp -d` cache/output/log
plus a kernel-ephemeral port via `alloc_port`. Safe to run alongside dev
or prod processors on the same host. Per-scenario tmpdirs at
`/tmp/dioramap-it-*` are auto-cleaned on success, preserved on failure.
Authoring template: `core/tests/integration/README.md`.

## Frama-C audit targets

Source of truth lives in two files:

- `scripts/frama-c/modules.tsv` — 21 cross-subsystem modules + 9 v7 impl libs (key | label | sources | harness).
- `scripts/frama-c/v7-benches.tsv` — 10 v7 bench files used as Eva entry points.

Run any one in isolation:

```bash
bash scripts/frama-c/audit-module.sh log
bash scripts/frama-c/audit-v7-bench.sh v7-tile-signals-bench
```

Outputs land at `.frama-c-out/<key>/eva.log` + `summary.txt`. The Full tier wraps this loop into JUnit XML (`scripts/ci/steps/run-frama-c.sh`).

**ACSL-annotated public-API headers** (18 of 19 cross-subsystem; the
exception `session.h`/`session_mgr.h` is internal-only): the contracts
in `core/include/v3/*.h` are the documentation of each function's
preconditions and postconditions. Don't duplicate them in prose docs.

**Known analyzer false positives**: `FRAMA-C-FALSE-POSITIVES.md`.

## Code-duplication detection (report-only)

Two language-split clone detectors run in the **Full** tier, on disjoint file
sets. Both are **report-only / NON-gating**: they always exit 0, produce a
JUnit XML (one `<skipped/>` testcase per clone cluster, carrying the cluster's
size + locations as `system-out`, plus a roll-up `*-summary` testcase with the
headline counts) and a raw report under the artifact dir. They never fail a
build. This is the automated enforcement of the **"one concept, N encodings"**
smell in `REVIEW-HEURISTICS.md` — clones get surfaced for a human to decide
whether to unify, not auto-rejected.

| Step | Tool | Scope | Threshold | Output |
|---|---|---|---|---|
| `scripts/ci/steps/run-cpd.sh` | PMD CPD (`cpp` lexer, covers C) | `steps/` + `core/src/` — **excludes** `third_party/` | `--minimum-tokens 75` | `<art>/junit/cpd-c.xml`, `<art>/cpd-report/{cpd.csv,cpd.txt}` |
| `scripts/ci/steps/run-jscpd.sh` | jscpd (`javascript` format) | `apps/lab-ui/` only — **excludes** `node_modules/`; deliberately NOT `lidar-algos-bench-v2/` (frozen) | `--min-tokens 50 --min-lines 30` | `<art>/junit/jscpd-js.xml`, `<art>/jscpd-report/jscpd-report.json` |

Run either standalone:

```bash
bash scripts/ci/steps/run-cpd.sh   /tmp/cpd-c.xml   /tmp/cpd-report
bash scripts/ci/steps/run-jscpd.sh /tmp/jscpd-js.xml /tmp/jscpd-report
```

**Tooling**: PMD CPD needs a JRE + the PMD distribution. The CPD step looks
for `cpd`/`pmd` on `PATH`, then a cached dist under `scripts/ci/.tools/pmd-bin-*/`
(gitignored; download once — see the `run-cpd.sh` header). jscpd uses `jscpd`
on `PATH` or `npx --yes jscpd`. If a tool is genuinely absent, the step
degrades gracefully: emits one skipped testcase, prints an install hint, and
still exits 0.

**Baseline (initial run).** C side at 75 tokens: **409 clone clusters**,
≈6,900 duplicated lines / ≈66,000 duplicated tokens — dominated by the v7
boundary step wrappers, which is expected: the `parse_corners` /
`parse_edges` / `parse_polygon_paths` `rg_record_iter` + `safe_calloc_array`
decode loops are copy-pasted across `v7-boundary-merge.c`,
`v7-boundary-classify.c`, `v7-boundary-polygons.c`, `v7-boundary-polygons-2.c`
and `v7-wall-detect.c`, and CPD flags them as the top offenders. JS side
(lab-ui) at 50 tokens / 30 lines: **0 clones** — currently clean.

## Conventions

### When to add an integration scenario

A scenario joins the matrix only when:

- (a) it catches a class of bug we've actually seen (regression net), OR
- (b) it exercises a code path no other scenario covers (use the coverage
  report to find these — anything below ~50 % in the encapsulated
  subsystems is a candidate).

The PR that adds the scenario must include:

- The scenario script under `core/tests/integration/scenarios/`.
- A header comment: bug class it covers, variants it most needs.
- Update to the table above.

Speculative scenarios get rejected.  A 20-scenario matrix everyone
understands beats a 200-scenario matrix nobody trusts.

### When to add a fuzz target

A new fuzz target ships with the parser it covers — same PR. PR must
include: target source under `core/tests/fuzz/`, 5–20 seed corpus
inputs, cron-schedule entry (when CI is wired).

### When to bump a third-party pin

See `THIRD-PARTY.md`.

### When to relax `-Werror` on a target

Only for upstream third-party (PolyFit, Roofer, copc-lib, laz-perf). Our
own code stays under `-Werror -Wpedantic -Wshadow -Wstrict-prototypes
-Wmissing-prototypes -Wthread-safety`. If a clang warning fires on our
code, fix the code, don't suppress.

### TSan on WSL2

Won't launch — the kernel refuses to disable ASLR. Build succeeds, exec
fails with `tsan_platform_linux.cpp:282 "...ADDR_NO_RANDOMIZE..."`. For
now, TSan is run by shipping the binary as a self-contained tarball to a
real-Linux host. Once a runner exists, `run-tier.sh nightly` will dispatch it.

## File map

```
scripts/ci/                          tier dispatcher + step scripts
  run-tier.sh                          fast | full | nightly | weekly
  lib/
    junit.sh                           bash-only JUnit XML emitter
    variants.sh                        build-variant registry
  steps/
    build-variant.sh                   configure + ninja per variant
    run-ctest.sh                       ctest --output-junit wrapper
    run-integration.sh                 sweep scenarios → JUnit
    run-coverage-sweep.sh              ctest + scenarios under LLVM_PROFILE_FILE
    run-coverage-report.sh             llvm-profdata + llvm-cov → HTML + summary.json
    run-frama-c.sh                     audit-module.sh + audit-v7-bench.sh → JUnit
    run-cpd.sh                         PMD CPD over C (steps/ + core/src/) → JUnit (report-only)
    run-jscpd.sh                       jscpd over apps/lab-ui/ JS → JUnit (report-only)
  .tools/                              cached CI tooling (gitignored; e.g. PMD dist for CPD)

scripts/frama-c/                     Frama-C audit tooling
  audit-module.sh                      single-module Eva + RTE run
  audit-v7-bench.sh                    bench-as-harness Eva run
  modules.tsv, v7-benches.tsv          target registries
  harnesses/                           per-module Eva entry points
  stubs/                               shadow headers for parser-unfriendly third-party

scripts/fetch-third-party.sh         pinned third-party clone + patch

core/tests/integration/              integration test infrastructure
  run_scenario.sh                      orchestrator (one scenario × one binary)
  run_scenario_under.sh                same, wrapped in valgrind
  lib/isolation.sh                     port + tmpdir isolation primitives
  clients/node-ws.js                   shared WS client
  scenarios/                           the 10 scripts

core/tests/fuzz/                     libFuzzer targets + seed corpus
```

## Where to find out what's not yet covered

`ROADMAP.md` — items by gap, with the cost of doing each.

## Review heuristics

`REVIEW-HEURISTICS.md` — cross-cutting smells distilled from bugs we've
shipped + caught (e.g. "one concept, N encodings" / cache-vs-canonical
drift — the root cause of the corner-`source` divergence).
