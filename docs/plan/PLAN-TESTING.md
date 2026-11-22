# Testing Subsystem Plan

*Companion to [UNIFIED-ARCHITECTURE.md](../UNIFIED-ARCHITECTURE.md) Unit + golden + smoke integration tests.*

## Scope

- **Unit test framework**: minimal custom harness.
- **Golden test harness**: fixed-input / expected-output step validation.
- **Smoke integration tests**: end-to-end pipelines via batch and WS.
- **Event trace fixtures**: golden log-event sequences.
- **Fuzz infrastructure** for codec hot paths.
- **Sanitizer integration** in CI.
- **Test data management**: fixtures in repo vs fetched on CI.

## Key decisions

1. **Custom minimal framework**, ~200 LOC. No CMocka / Unity / GoogleTest. Rationale: external frameworks add build complexity disproportionate to value for this scale.
2. **Golden format is the same binary layout.** Writing expected outputs uses the production codec; readers use production readers. No parallel "test-only" format.
3. **Epsilon tolerance per column, declared per golden.** Numerical steps may legitimately differ from reference within bounds; `meta.json` of the golden declares per-column tolerances.
4. **Goldens are migration-phase only.** A small handful of real test cases (Dahouet primary; 2–3 others). Bench's 223 synthetic + procedural cases are **not** migrated to v3; they stay in bench-v2 under bench-v2's own test infrastructure. Post-migration, per-step goldens retire; only the harness stays as infrastructure.
5. **Real test cases are tiny JSON pipeline-args files.** Data flows through the normal fetch chain at test time (and caches after first fetch). No embedded LAZ/OBJ payloads in the repo.
6. **Smoke pipeline tests run on every PR.** Cheap, catch 80% of regressions.

## Data structures

### Unit test framework

```c
// core/tests/harness.h

#define TEST(NAME) \
    static void test_##NAME(test_ctx_t* ctx); \
    static test_entry_t ENTRY_##NAME \
        __attribute__((section("test_entries"), used)) \
        = {.name = #NAME, .fn = test_##NAME}; \
    static void test_##NAME(test_ctx_t* ctx)

#define ASSERT_TRUE(COND) do { \
    if (!(COND)) test_fail(ctx, __FILE__, __LINE__, #COND); \
} while (0)

#define ASSERT_EQ(A, B) /* ... */
#define ASSERT_BYTES_EQ(A, A_LEN, B, B_LEN) /* ... */
#define ASSERT_EVENT_EMITTED(SUBSYS, KIND) /* uses log test harness */

// Main runner:
int main(int argc, char** argv);  // discovers tests, runs, reports
```

A test binary links one or more test source files and gets a main that walks the `test_entries` section.

### Golden harness (migration-phase only)

```
steps/<name>/golden/
├── dahouet.input.json         # pipeline args (coords, tile size, etc.)
├── dahouet.expected.bin       # expected output of this step, captured from JS reference
└── dahouet.meta.json          # { step_version, params, epsilon_per_column }
```

Test driver:

```c
// Runs the full pipeline up to and including this step using the real fetcher chain,
// captures this step's output, compares against the expected bytes with per-column epsilon.

result_t golden_run_case(const char* step_name, const char* case_dir);
```

Every step with a `golden/` directory gets CTest targets enabling `ninja test-golden-<step>-<case>`.

Goldens retire once the C port is stable — the C impl becomes its own reference; there's no longer an alternate version to diff against. The harness itself stays for anyone wanting to pin specific algorithm behavior.

### Smoke integration tests

Shell scripts + a test WS client (written in Python for simplicity — test-side tools don't need to be C).

```
v3/tests/smoke/
├── verify-batch.sh            # runs batch mode, checks output structure
├── verify-serve.py            # WS client, exercises RUN/SUBSCRIBE/RECONNECT
├── verify-cuda-parity.sh      # runs on both builds, diffs outputs
└── fixtures/
    └── smoke-input.bin        # input artifact for the smoke pipeline
```

### Event trace fixtures

```
v3/tests/event-traces/
├── smoke-fresh-run.jsonl       # expected event stream for a fresh smoke run
├── smoke-cache-hit.jsonl       # expected stream for a repeat (cache-hit) run
└── smoke-late-subscribe.jsonl
```

Fixtures are JSON-lines files; each line has fields we assert match (subsystem, event, and a subset of other fields; timestamps are wildcarded).

Test runs the scenario, captures logs, compares.

## Status (2026-04-28)

| Task | State | Notes |
|---|---|---|
| T1 Custom harness | ⚠️ partial (A1/A2/A3) | Per-file hand-rolled `CHECK`/`CHECK_CONTAINS` macros (e.g. `core/tests/log_test.c`); each test binary has its own `main()` and is registered via `add_test` in `core/tests/CMakeLists.txt`. The linker-section `TEST(NAME)` discovery pattern from §"Data structures" was not built — the simpler per-binary pattern is sufficient and is what every existing test uses. |
| T2 Golden case loader | ❌ not started | No `golden_run_case` helper; no `steps/<name>/golden/` directories. Tests build pipelines directly via the runner API. |
| T3 Codec round-trip + fuzz | ✅ done (A4 round-trip `22aae7f`; libFuzzer 2026-05-04 `5bee4cb`) | `artifact_test.c` does write→read round-trip. **2026-05-04**: libFuzzer infrastructure under `core/tests/fuzz/` (gated on `DIORAMAP_FUZZ=ON`, clang only).  Two targets shipped: `fuzz_hash_hex` (REHYDRATE-path hex parser), `fuzz_run_journal_parse` (regression net for the parse_produced_json UAF fix from the 2026-05-01 audit).  Seed corpora under `core/tests/fuzz/corpus/`; fuzzer-discovered SHA-named files are gitignored.  Initial validation: ~5M random inputs through `fuzz_run_journal_parse`, ~16M through `fuzz_hash_hex`, zero crashes.  See docs/quality/README.md for the full workflow + cron cadence. |
| T4 Concurrency stress tests | ✅ done (A3 + A5; matrix expanded 2026-05-04 `5bee4cb`) | `thread_pool_test.c`, `refcount_test.c`, `store_test.c` cover concurrent submits / acquire-release / readers-writers.  ASan + TSan build configs present (`build-asan/`, `build-tsan/`); CI matrix is set up but workflow file is `.disabled`. **2026-05-04**: dedicated integration matrix at `core/tests/integration/` covering 10 scenarios across 4+ build variants — see docs/quality/README.md.  Headline scenarios for concurrency: `serve_all_cache_hit_burst` (journal-acc race repro generalised — 100 identical RUNs on one socket), `serve_concurrent_clients` (sessions_lock stress with N parallel sessions), `serve_reconnect_within_grace` (sessions_lock → session_t::lock nesting per CONCURRENCY.md).  Validated under TSan on prod 2026-05-04 (TSan won't run on the WSL2 dev box — bundle/scp/run procedure documented in docs/quality/README.md). |
| T5 Smoke batch test | ✅ done (A8 + B3.5) | `batch_demo_chain`, `batch_gpu_smoke`, `batch_array_demo`, `batch_cube_memoized_demo` ctests. Cache-hit assertions via `batch_viewer_fetchers`'s `grep -q 'cache hit'`. **2026-05-04**: `core/tests/integration/scenarios/batch_demo_chain_warm.sh` adds an isolation-aware variant that asserts cold=4/0 + warm=0/4 via the structured run_complete log line (no grep brittleness). |
| T6 Smoke WS test | ✅ done (A7, `0b18787`; matrix coverage 2026-05-04 `5bee4cb`) | `serve_smoke.py`, `serve_full.py` (RUN + late SUBSCRIBE + RECONNECT), `serve_stream.py` (B9 streaming).  **2026-05-04**: 10 integration scenarios at `core/tests/integration/scenarios/serve_*.sh` covering every WS handler the server ships (RUN, SUBSCRIBE, CANCEL, INVALIDATE, LIST_RUNS, REHYDRATE_RUN, RECONNECT) plus the abrupt-disconnect grace path.  Per-scenario isolation (ephemeral port + per-scenario tmpdir) means safe to run alongside dev or prod processors on the same host. |
| T7 Event trace capture + diff | ❌ not started | No JSON-lines event trace fixtures committed. The `log_sink_swap_for_testing` harness exists (PLAN-LOGGING L9) but no golden trace test uses it. |
| T8 CI integration | ⚠️ partial (A1, `0b3a4df`) | `.github/workflows/v3-ci.yml.disabled` — workflow file written, intentionally disabled to avoid Actions minutes (see MAIN-IMPL-PLAN). Re-enable via rename. |
| T9 Test data management | ✅ done (B1 onwards) | `core/tests/fixtures/` for HTTP stub + `DIORAMAP_TEST_IGN`-gated live tests; cache-dir reuse keeps subsequent runs offline. |
| T10 Performance regression tracking | ❌ deferred | Not Phase A blocking; no harness yet. |

## Implementation tasks (ordered)

### T1. Custom harness ⚠️ partial
- `TEST(NAME)` macro with linker-section collection.
- Minimal assertion macros.
- Test runner main.
- Colored output, timing.
- Parallel execution (one test per thread; optional).

### T2. Golden case loader ❌ not started
- Reads in/expected/meta from a golden dir.
- Wraps in a minimal one-step pipeline.
- Runs via runner.
- Diffs artifact bytes / columns with epsilon.

### T3. Codec round-trip + fuzz ⚠️ partial
- Build random artifacts, write, read, compare.
- AFL/libFuzzer target for the decoder (corrupt input should reject cleanly).

### T4. Concurrency stress tests ✅ done
- Thread pool under high submit rate.
- Store under concurrent readers + writers.
- TSan + ASan CI variants.

### T5. Smoke batch test ✅ done (A8 + B3.5)
- Shell script wrapping `dioramap-processor batch`.
- Asserts manifest structure, artifact presence, byte content.
- Rerun asserts cache-hit path via log check.

### T6. Smoke WS test ✅ done (A7, commit `0b18787`)
- Python WS client.
- Opens session, RUNs, receives artifacts, closes.
- Second test: reconnects with saved token, verifies rehydration.
- Third test: late-SUBSCRIBE after run complete.

### T7. Event trace capture + diff ❌ not started
- Extract log events from a run into a JSON-lines file.
- Diff against golden fixture with wildcard support.
- Regenerate golden is an explicit command (not automatic).

### T8. CI integration ⚠️ partial (workflow file disabled)
- GitHub Actions (or other) workflow with build matrix.
- Job definitions per MAIN-IMPL-PLAN
- Artifact upload (logs, failing-test traces).
- PR status checks.

### T9. Test data management ✅ done (B1 onwards)
- Fixtures live in `v3/tests/fixtures/` and are small JSON pipeline-args files.
- Real-world data is fetched through the normal chain at test time; disk cache keeps subsequent runs offline-fast.
- No large LAZ/OBJ payloads in repo; no separate download pipeline.

### T10. Performance regression tracking (deferred) ❌ deferred
- Benchmark harness that runs each step on a fixture and records time.
- Stores results; PR-diff posts regression %.
- Nice-to-have; not Phase A blocking.

## Dependencies

- **Before**: logging (for event capture), all subsystems (for what's being tested).
- **After**: each algorithm port in Phase B gets its golden suite; the harness is already there.

## Definition of done

- Unit test framework functional; ≥ 30 tests across subsystems passing.
- Smoke pipeline passes in all CI matrix jobs.
- Event-trace fixture for smoke-fresh-run committed and enforced.
- Fuzz target runs in CI for ≥ 15 minutes per PR touching codec.
- Golden harness is used by the `gpu-smoke` step's single golden case, paving the pattern for future ports.

## Open questions

- **Test isolation.** Each test may share the `store` instance or spin up its own. Default: own. Slower but simpler.  Resolved for the integration tier 2026-05-04: every scenario at `core/tests/integration/` uses a kernel-allocated ephemeral port + per-scenario `mktemp -d` for cache/output/log; safe to run alongside dev or prod processors on the same host. See `core/tests/integration/lib/isolation.sh`.
- **Flakiness policy.** Zero tolerance for flaky tests; a flake is a bug, not a retry target. Concurrency tests especially.
- **Coverage tracking.** ~~`gcov`/`llvm-cov`. Nice to have, not Phase A.~~ **Resolved 2026-05-04**: llvm-cov chosen (precise on inlines/templates, exports lcov format for dashboards, same toolchain as the matrix's other clang variants).  `DIORAMAP_COVERAGE=ON` build profile + `LLVM_PROFILE_FILE` runtime env + `llvm-profdata-18 merge` + `llvm-cov-18 report`.  See docs/quality/README.md (coverage variant) and docs/quality/README.md for the workflow.

## See also

- [docs/quality/README.md](docs/quality/README.md) — shipped 2026-05-03.  PLAN-TESTING covers *what* to test (units, goldens, smokes); the validation matrix covers *how to exercise the same tests under every form of runtime instrumentation we have access to* (release / ASan+UBSan / TSan+UBSan / coverage / fuzz / valgrind), plus the bundle-build-ship-run procedure for variants we can't run on the dev box.
- [docs/quality/README.md](../docs/quality/README.md) — quick reference for running the matrix manually.

## Risk / unknowns

- **Linker-section collection** (for test discovery) behaves differently on macOS / Windows. Given Linux-only prod, dev on Linux or WSL; stricter platform support deferred.
- **Python WS client** is fine for tests but if we ever want to dogfood the WS protocol from a C client, we'll need to write one. Deferred.
