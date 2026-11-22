# Quality roadmap — what's not yet covered

What `./scripts/ci/run-tier.sh full` does **not** attest, what it would
take to add each gap, and why we haven't yet.

The list is ordered by ratio of (value) / (cost). Top items are the
cheapest, highest-impact next steps.

## 1. CI wired into GitHub on push + PR

- **Gap**: today the tiers are runnable only by hand. No automatic gate.
- **Need**: `.github/workflows/{ci-fast,ci-full,nightly,weekly}.yml`. Each is a ~30-line wrapper calling `./scripts/ci/run-tier.sh <tier>` and uploading `ci-artifacts/`.
- **Decision pending**: hosted runners (free for ~3000 min/mo; Fast tier alone fits) vs self-hosted (free for any duration, needs the dev box or a VPS).
- **Effort**: 1 evening.
- **Why not yet**: design choice on the runner side, plus the underlying branching / PR policy (`CONTRIBUTING.md`).

## 2. `actions/cache` (or persistent volume) for `third_party/` + ccache

- **Gap**: first-ever CI run on a fresh runner pays the full ~1 min 26 s fetch + cold compile.
- **Need**: `actions/cache` step keyed on `scripts/fetch-third-party.sh` content (so cache invalidates when a pin bumps); same for ccache dir. On self-hosted runners, just mount the dirs.
- **Effort**: ~1 hour once CI is wired.
- **Why not yet**: depends on (1).

## 3. TSan runtime coverage

- **Gap**: TSan binary builds cleanly but WSL2 refuses to launch it. Concurrency-safety axis attested today only by `-Wthread-safety` (compile time) and ASan (which catches some races).
- **Need**: a real-Linux runner. Options: a second self-hosted runner on `dioramap.com` (free; load-controlled via systemd CPUWeight/IOWeight), or a small dedicated VPS (~€5/mo).
- **Effort**: ~1 hour to register the runner; nightly tier already has the `run_build tsan` stub.
- **Why not yet**: needs a runner-placement decision.
- **Local option (confirmed 2026-05-27)**: a **Hyper-V Ubuntu 24.04 VM** on the dev laptop is a full real kernel and runs TSan/Valgrind/heavy-Frama-C fine — unlike the WSL2 sandbox. Two requirements: (a) apply the 24.04 sanitizer fix `sudo sysctl -w vm.mmap_rnd_bits=28` (24.04 defaults to 32, which breaks TSan **and** ASan), and (b) size it: **≥8 GB RAM** (TSan ~5–10× memory) and a **dynamically-expanding ~100–128 GB** virtual disk (OS+toolchain ~15 GB, third_party incl. CGAL ~5 GB, the multi-variant build matrix with PolyFit/CGAL/roofer compiled is the big consumer at ~20–30 GB, plus coverage/artifacts). Hosted `ubuntu-24.04` runners also work for TSan in CI with the same sysctl step. **Provisioning script: `docs/quality/setup-quality-vm.sh`** installs the whole toolchain (apt + Node + opam/Frama-C + the sysctl fix + repo bootstrap) — `bash setup-quality-vm.sh` on a fresh VM.

## 4. Coverage trend graph

- **Gap**: today we have one coverage data point per run (line 31.41 %, function 50.00 %, etc.). No history, no trend.
- **Need**: `gh-pages` (or local static-site) accumulating `coverage/summary.json` across runs, plotted as a time series.
- **Effort**: ~2 hours.
- **Why not yet**: low priority until CI is actually accumulating runs.

## 5. Fuzz cadence

- **Gap**: two fuzz targets exist (`fuzz_hash_hex`, `fuzz_run_journal_parse`) but there's no schedule. We only run them manually.
- **Need**: a `weekly` (or `nightly`) tier step that runs each target for N hours and commits any new crash inputs to `core/tests/fuzz/corpus/<target>/`.
- **Effort**: ~3 hours (cron wiring + corpus-replay scenario for per-PR regression).
- **Why not yet**: depends on (1); cadence makes no sense without automation.

## 6. The 3 missing fuzz targets

- **Gap**: `core/tests/fuzz/` ships only 2 of the 5 targets the project's parser surfaces need.
- **Need**: `fuzz_ws_message`, `fuzz_pipeline_load`, `fuzz_intent_map_replay`. Each is a `LLVMFuzzerTestOneInput(uint8_t*, size_t)` wrapping one parser entry-point, plus a 5–20-input seed corpus.
- **Effort**: ~1 day per target.
- **Why not yet**: low ROI until fuzz cadence (5) is established — these would just sit on disk.

## 7. Mutation testing

- **Gap**: coverage tells you the test reaches the line; it doesn't tell you the test would catch a change to that line.
- **Need**: a mutation-testing tool (e.g. `mutate` for C, or rolling our own with the existing test suite). Generates source-mutated builds; flags any mutation the suite doesn't catch.
- **Effort**: ~1 week (tool selection + integration + tuning).
- **Why not yet**: probably premature until line coverage is past ~60 %. Mutation testing on poorly-covered code just rediscovers the missing-coverage gaps.

## 8. ARM / Apple-silicon toolchain validation

- **Gap**: only x86-64 Linux is exercised today. Any portability assumptions in our own code (alignment, endianness, SIMD intrinsics) are unverified.
- **Need**: cross-compile build on aarch64, or a runner of that arch.
- **Effort**: ~1 day to add the build variant; ongoing maintenance cost ~1 day/year.
- **Why not yet**: the project doesn't ship to ARM today. Adding when ARM is on the roadmap.

## 9. Performance regression net

- **Gap**: the v7 bench files (`tests/run-v7-*-bench-regression.py`) compare output bytes against checked-in baselines but don't measure performance. We have no automated way to catch a 2× slowdown in a hot path.
- **Need**: per-bench timing + history; threshold alert.
- **Effort**: ~1 day.
- **Why not yet**: explicitly out of scope per the validation matrix; performance benchmarking is a separate plan.

## 10. ACSL on internal-only headers

- **Gap**: 18 of 19 cross-subsystem public headers have ACSL contracts. The exceptions `session.h` + `session_mgr.h` are internal-only (used only inside `core/src/server/`).
- **Need**: same contract pattern as the other headers — null/valid checks on pointer arguments, postcondition on result.
- **Effort**: ~3 hours.
- **Why not yet**: internal-only, low priority; can be done opportunistically.

## 11. WP-plugin proofs (beyond Eva)

- **Gap**: Eva proves freedom from runtime alarms. WP (the deductive-verification plugin) can prove stronger functional properties — kd-tree balance, polygon-merge associativity, ridge orientation consistency.
- **Need**: WP install + per-function preconditions/postconditions formalized at the algorithmic level (much richer than the public-API contracts).
- **Effort**: ~1 month for a single algorithm, indefinitely for the rest.
- **Why not yet**: deep research investment; not justified by today's risk profile.

## v7-port quality backlog (added 2026-05-27)

Tracked tasks from the v7-port quality cadence. Some overlap the gap items
above (cross-referenced); the rest are port-specific.

1. **Quality pass on each remaining ported step** (ongoing). As the port lands
   the remaining post-merge steps (LOD2-simplify, fit-solar-panels, …), apply
   the standard pass: review → safe_alloc + `json_number_or_default` fail-loud
   params → ACSL on the compute entry → dedicated Frama-C module+harness →
   docs counts → `-Werror` build (catches the wall-detect-class uninit bugs).
   Confirm artifact byte-equivalence.
2. **Delete `v7-boundary-polygons` (bp1)** — bp2 supersedes it. Confirm the
   pipeline + boundary-merge consume bp2 end-to-end, then remove bp1 + its
   test/bench/registry/CMake/`v7-benches.tsv` entries. Erases the #1 CPD clone
   (the 154-line bp↔bp2 `parse_corners` duplication).
3. **Chase & kill duplication, then gate CPD.** Baseline today: **409 C clone
   clusters** (jscpd: 0 in lab-ui). Shared `v7_rg_upstream_parse` helper
   (per-section RGRAPH readers — kills the `parse_*` clones across all six
   boundary-* wrappers and centralizes the safe_alloc fix bp1/bp2 still lack);
   PolyFit hypothesis/MIP common-setup helper (~211 dup lines); split the
   monolithic ~1250-line `v7_br_compute` (FP #11 + CPD both flag it). Then flip
   `run-cpd.sh` / `run-jscpd.sh` report-only → **gating**; keep benches +
   `third_party/` excluded.
4. **Port v2 scoring + set up Stream B** (3D model-validity). Build B.2 metric
   set → B.6 dashboard. Gated on the port being feature-complete (item 1) —
   scoring a half-ported pipeline conflates port-divergence with algorithm
   quality, which is why Stream B is paused at the B.1 skeleton.
5. **Finish GitHub Actions** — see gap #1. Additionally: the heavy v7 Frama-C
   module audits (regularize ~minutes each) blow the full-tier <5 min budget →
   move slow audits to nightly; sanitizer jobs need the 24.04
   `vm.mmap_rnd_bits=28` step; TSan/Valgrind need the real-Linux runner (gap #3).
6. **WP-prove the key ACSL invariants** — see gap #11. The v7 ACSL added this
   cycle is EVA-*checked* (no counterexample), not WP-*proven*. Discharge the
   corner-pool canonical-source invariant (#254-263), redirect-chain
   acyclicity, and the bounded-output postconditions left as TODO. Easier after
   item 3 (smaller functions = smaller proof obligations).
7. **Retire v2-divergence tooling when v2 freezes** — the `*-bench.c` benches
   (also the largest CPD clones, but not worth refactoring since they retire),
   `tests/iso-equivalence.py`, and the `v7-benches.tsv` bench-as-harness
   entries that depend on them. Keep the dedicated `modules.tsv` harnesses
   (v2-independent). Leaves the audit on a clean footing.

## Items already addressed (for cross-reference)

These were on the matrix when this project started; they're done.

- **Phase 0** (clang `-Wthread-safety` annotations) — shipped 2026-05-02.
- **Phase 1** (integration harness skeleton) — shipped, 10 scenarios.
- **Phase 3** (full scenario set + coverage tooling) — shipped, modulo trend.
- **Phase 4 partial** (fuzz targets seeded) — 2/5 targets, infrastructure works.
- **Phase 5 partial** (Valgrind memcheck wrapper) — works on demand.

The "phases" framing in the original `PLAN-VALIDATION-MATRIX.md` was a
forward plan from when the harness didn't exist. The roadmap above is
the descendant of that plan, restated as gaps + cost.
