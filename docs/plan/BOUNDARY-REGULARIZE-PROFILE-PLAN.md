# v7-boundary-regularize profile plan

**Started:** 2026-05-29 (proposed, not started)
**Status:** awaiting review
**Goal:** know per-metric *time* and per-metric *functional* contribution
so we can pick optimization targets quantitatively, not by gut.

## Why this matters

On `dahouet-bldgonly` (16092 pts, 1 building), the regularize step
takes **~15 min wall-clock** under `build-noasan/`. It dominates every
end-to-end pipeline run. The work happens inside one outer loop bounded
by `max_iterations = 1000`, and v3 currently *hits* the cap on dahouet
(per [[2026-05-27-boundary-regularize-iso-baseline]]: `r1_iterations:
v2=772 v3=1000`). So we're spending the full budget AND not converging.

The cost function is a sum of seven metrics, each gated by a boolean
param and weighted by a lambda. Today we don't know how much each
metric costs to evaluate, how much each contributes to the converged
score, or how quickly each individually would converge alone. Without
that data, any optimization is guessing.

## What we already know (no profiling needed)

- Cost surface: `v3/steps/v7/v7-boundary-regularize-impl.c` lines
  1023–1064 — `if (params->use_X)` block per metric inside the
  `EVALUATE_TOTAL_COST_AT` macro.
- The 7 metrics with their params:

  | Metric | `use_*_cost` | `lambda_*` | `sigma_*` |
  |---|---|---|---|
  | WSD (point-on-plane) | `use_wsd_cost` | `lambda_wsd` | `wsd_sigma_m=1.0` |
  | Angle (corner sharpness) | `use_angle_cost` | `lambda_angle` | — |
  | Neighbor-angle | `use_neighbor_angle_cost` | `lambda_neighbor_angle` | — |
  | Horizontal-edge | `use_horizontal_edge_cost` | `lambda_horizontal_edge` | — |
  | Horizontal-coplanar (var-of-y) | `use_horiz_coplanar_cost` | `lambda_horiz_coplanar` | `horiz_coplanar_sigma_m=0.10` |
  | Vertical-coplanar (xz-line-fit residual) | `use_vert_coplanar_cost` | `lambda_vert_coplanar` | `vert_coplanar_sigma_m=0.10` |
  | Collinear | `use_collinear_cost` | `lambda_collinear` | — |

- Single iteration loop at line 1217 (`while (iter < max_iterations)`).
  Cost is recomputed inside per-corner proposal evaluation; not cached
  between iterations.
- v2 reference at `lidar-algos-bench-v2/lib/pipeline/steps/boundary-regularize.js`
  (1496 LOC, smaller surface — different convergence properties, but
  same metric set).

## Phase 1 — Functional contribution (1–2 days)

**Goal:** answer "if I remove metric X, what does it cost me?" with
quantitative measures: final score, iterations, polygon quality on
real fixtures.

### A1. Baseline + per-metric ablation matrix

Run the regularize-only-bench on each test fixture, sweeping these
configs:

| Config | Description |
|---|---|
| `all` | all 7 metrics on (current default) |
| `no_wsd` | `use_wsd_cost=false`, rest on |
| `no_angle` | `use_angle_cost=false`, rest on |
| `no_nbr_angle` | `use_neighbor_angle_cost=false`, rest on |
| `no_horiz_edge` | `use_horizontal_edge_cost=false`, rest on |
| `no_horiz_coplanar` | `use_horiz_coplanar_cost=false`, rest on |
| `no_vert_coplanar` | `use_vert_coplanar_cost=false`, rest on |
| `no_collinear` | `use_collinear_cost=false`, rest on |
| `only_wsd` | wsd on, rest off |
| `only_angle` | angle on, rest off |
| … 7 × `only_X` configs … |  |

For each (fixture × config) run, record:

- `iterations_used` (capped at 1000)
- `final_total_cost` (sum of enabled metrics)
- **`final_per_metric_cost`** — re-evaluate ALL 7 metrics on the final
  corner positions, so we get apples-to-apples comparison even when one
  was disabled during search.
- `final_corner_positions` (small, dump to JSON for visual review)
- Wall time

This costs **N_fixtures × 17 configs × runtime**. With 5 representative
fixtures and ~5 min each on noasan, that's ~7h compute. Fits in a
batch overnight.

### A2. Convergence-speed-per-metric

For each `only_X` config above, also dump a per-iteration cost trace
(or at least sample every 10 iterations). This shows whether a single
metric alone converges in 50 iter or 500, and how much the others
*accelerate* convergence (gradient information) vs *change the
optimum* (cost-surface shape).

We'd plot:
- iter → cost for `only_wsd`
- iter → cost for `only_angle`
- iter → cost for `all`
- one curve per `no_X` showing the "removal cost"

### A3. Output

`v3/docs/research/regularize-functional-ablation-<date>.md` with:
- Filled ablation table
- Convergence curves (matplotlib SVG)
- Per-fixture commentary: which metrics matter on dahouet vs
  flat-roof vs L-shape, etc.
- Recommendation: any metric we can drop entirely (lambda=0 OR
  toggle off) without measurable quality loss.

## Phase 2 — Per-metric timing (1 day, blocked by A1 only by data
analysis order)

**Goal:** know wall-clock cost of each metric evaluation.

### B1. Instrument the cost macro

Wrap each `if (params->use_X)` block in
`v7-boundary-regularize-impl.c:1023-1064` with a pair of
`__rdtsc()` or `clock_gettime(CLOCK_MONOTONIC_RAW)` calls. Accumulate
into a per-metric `uint64_t total_ns` and per-metric `uint64_t calls`
counter. Emit on step completion.

Gate behind `getenv("BR_PROFILE")` to keep the hot path clean for
production. Per-metric counters live in a small static struct emitted
via `fprintf(stderr, ...)` matching the existing diagnostic style.

### B2. Per-phase timing

In addition to per-metric, instrument the broader phases:
- R.0 (DOF tagging)
- R.0b (redundant ridge filter)
- R.1 (move-and-score outer loop)
- R.2 (pool merge)
- R.3 (collinear removal)

Use the same env-gated counter pattern. Emit a summary like:

```
[br-profile] R.0       :   12 ms (1 call)
[br-profile] R.1 outer : 9870 ms (1 call, 1000 iter)
[br-profile] R.1 wsd   : 4521 ms (45872 calls)  ← 46% of R.1
[br-profile] R.1 angle :  234 ms (45872 calls)
…
```

### B3. Hotspot drill-down

For whichever metric tops the time list, add sub-instrumentation. The
likely candidate is WSD because (a) its `lambda_wsd=1.0` is highest,
(b) its evaluation walks all group points × all polygon edges, and
(c) the journal already flags it as the FP-sensitive surface. Likely
sub-hotspots:
- per-point projection
- per-edge `seg_seg_dist`
- the wrong-side sign computation

### B4. Output

`v3/docs/research/regularize-timing-<date>.md` with:
- Per-metric ns and call counts
- Per-phase wall-clock breakdown
- The hotspot sub-breakdown for the worst offender
- Compare to v2 JS timing if we can extract similar numbers (v2's
  `console.time` works if we add labels; cheap to add)

## Phase 3 — Composite analysis + optimization shortlist (0.5 day)

Combine A and B into a single 2D plot per metric:
- **X axis:** wall-clock cost (from B)
- **Y axis:** quality contribution (from A — drop in re-eval cost when
  this metric is included vs not)

Top-right quadrant = expensive AND important → caching / approximation
Top-left quadrant = expensive AND unimportant → drop or relax sigma
Bottom-right = cheap AND important → leave alone
Bottom-left = cheap AND unimportant → leave alone

This is the **single chart** that should drive the optimization
decision. Without it, we'd be optimizing the wrong thing.

## Phase 4 — Candidate optimizations (post-data)

Listed *here* so we have a known menu when Phase 3 picks the targets.
The data will tell us which ones to pursue.

| Strategy | When applicable | Reversible? |
|---|---|---|
| **Drop or λ→0 a metric** | Phase 3 shows it's top-left quadrant | yes, param flip |
| **Per-corner WSD caching** | WSD top-right, recomputed but most corners didn't move | yes, layer behind a flag |
| **Per-subspace plane caching** (memo by plane-id of points-per-plane) | WSD top-right, plane assignments stable across iter | yes |
| **Approximate WSD via grid lookup** | WSD top-right, sub-cm precision not needed | yes, additive |
| **SIMD over polygon edges** in WSD inner loop | WSD top-right, edge count > vector width | requires alignment work |
| **In-step parallelism** (per-plane parallel R.1 proposals — the "something exists for this" the user mentioned) | R.1 dominates and proposals are independent across planes | yes |
| **Early-stop on plateau** | iter→cost shows flat tail after iter ~300 | yes, just shrink max_iter |
| **Different optimizer** (BFGS, ADMM, Lin-Kernighan-style) | move-and-score grid sampling is the issue, not the cost | larger lift |

## Open questions — settled 2026-05-29

1. **Fixtures.** **Dahouet-bldgonly only.** Generalization to other
   fixtures comes later if at all; the immediate question is making
   this one fast and good. Reduces compute from 5+ × 17 configs to
   1 × 17 configs (~85 min on noasan).

2. **Stop criterion.** Two parallel runs per ablation config:
   - **Convergence run** — raise `max_iterations` to 5000 (or higher
     if needed; 1000 was the *artificial* cap and we want the *natural*
     stop). Add NO Δcost early-stop yet — risk of oscillation cutting
     us short. Just raise the ceiling.
   - **Snapshot run** — same config but capped at **200 iter**. Gives
     us "speed-of-quality-improvement" — which metrics still have
     useful gradient at 200 vs which have plateaued.
   Both reuse the same instrumentation, just different param overrides.

3. **Score for "quality"** in Phase 1 A1. The existing
   `EVALUATE_TOTAL_COST_AT` macro is parameterized by the same
   `use_*_cost` booleans the search uses, so a *post-hoc* re-eval with
   all 7 toggled true requires either (a) calling it twice with two
   param structs or (b) splitting it into the "search cost" used by
   the inner loop and a "shared score" called once at convergence.
   Plan: confirm (a) works by reading the function carefully; if it's
   single-shot, do (b) — small refactor. Settle this before the
   ablation matrix runs.

4. **Parallelism prior art.** Defer until after Phase 3 analysis.
   Phase 4 will inspect `v3/core/` for the prior parallelism path
   (likely per-bldg or per-plane in the step runner) but Phase 1+2
   don't depend on it.

## Profiling tool choice — settled 2026-05-29

Question: do we need to instrument C code, or can off-the-shelf tools
get us the data?

**Answer: staged — perf first (no code change), instrument only if perf
can't break out the data we need.**

### Why not pure `perf`?

`perf record` is the standard Linux sampling profiler. It needs no
code change, ~5% overhead, gives per-function and per-line
attribution on `-g`-built optimized code. **But:** our 7 cost metrics
are inside a single macro `EVALUATE_TOTAL_COST_AT` that gets inlined
into the hot path. `perf` sees that as one symbol and reports
aggregate time — no per-metric breakdown.

### Staged approach

**Step P1: `perf record` on an unmodified `build-noasan/` run
(free, ~30 min).**

```bash
cd v3
sudo perf record -F 997 -g -- ./build-noasan/dioramap-processor batch \
  --pipeline pipelines/smoke/v7-bp2.json --input-json '{}' \
  --produce boundary-regularize-roof-graph \
  --output-dir /tmp/br-perf-out/ \
  --cache-dir /tmp/br-perf-cache
sudo perf report --stdio | head -80
```

Read the top-20 symbols. We expect to see something like:

- `v7_br_propose_move`  → if dominant, the cost macro isn't the issue
- `v7_br_evaluate_cost_at` → if dominant, we need step P2
- `compute_wsd_point_to_polygon` → if dominant, drill into WSD
- `proj_to_plane_uv` / `seg_seg_dist` → if dominant, projection or
  edge-geometry is the bottleneck

**Decision tree from P1 results:**

- If a non-macro function dominates → we know where to focus. Skip
  P2 instrumentation; go straight to optimization.
- If `evaluate_total_cost_at` (the inlined macro caller) dominates
  → we don't know which metric it is. Run P2.
- If iteration count itself is the issue (lots of time in proposal
  generation, not cost eval) → revise plan; the optimisation target
  is the search, not the metrics.

**Step P2: light instrumentation (~30 LOC), only if needed.**

Wrap each `if (params->use_X_cost)` block in
`v7-boundary-regularize-impl.c:1023-1064` with:

```c
struct timespec t0, t1;
if (br_profile_enabled) clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
if (params->use_wsd_cost) { ... }
if (br_profile_enabled) {
    clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
    br_prof.wsd_ns += (t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec);
    br_prof.wsd_calls++;
}
```

`br_prof` is a static struct flushed via `atexit` or step completion.
`br_profile_enabled = getenv("BR_PROFILE") != NULL`.

**Why `CLOCK_MONOTONIC_RAW` and not `__rdtsc()`:** TSC requires
calibration to convert to wall time, and gets weird on
frequency-scaling CPUs. `CLOCK_MONOTONIC_RAW` is straightforward
nanoseconds with ~10–30 ns overhead per call — comparable to a couple
of FP ops, negligible against metric evaluations measured in μs.

### Coarse "move vs score" split

User flagged: also want to know **wall-clock spent in corner-move
proposal generation vs scoring**. Expected outcome is that scoring
dominates by 10×+ — but we shouldn't assume; cheap to confirm.

This split should fall out of P1's `perf report` for free. Likely
symbols:

| Time bucket | Likely symbol(s) |
|---|---|
| Corner-move generation | `propose_*_move`, `nudge_along_axis`, `r1_propose_*` |
| Scoring | `evaluate_total_cost_at`, `compute_wsd_*`, `compute_*_cost` macros |
| Other | `propagate_alive_mask`, `update_dof_axes`, `r2_*`, `r3_*` |

If the boundary is fuzzy because of inlining, we add a coarse pair of
counters around "propose this corner's move" vs "score this proposal"
in P2 alongside the per-metric counters.

### Other tools considered

| Tool | Why rejected for this step |
|---|---|
| `callgrind` (valgrind) | 20–50× slowdown — a 15-min step becomes overnight |
| `gprof` | Requires `-pg` rebuild + needs to rule out re-entrancy in the macro; perf is strictly better |
| Tracy / Easy_Profiler | Requires code annotations like P2 anyway; gains the GUI but the data we need is small enough to text-dump |
| eBPF / bpftrace | Powerful for tracing syscalls and kernel; overkill for a single-process CPU-bound loop |
| `valgrind --tool=cachegrind` | Useful *later* if we find a cache-miss-heavy hotspot, not for first-pass |

## Time budget

| Phase | Estimate | Blocked by |
|---|---|---|
| Pre-plan (settle open questions) | 0.5 day | — |
| A — functional ablation | 2 days (incl. overnight compute) | pre-plan |
| B — timing instrumentation | 1 day | parallel with A |
| Phase 3 analysis | 0.5 day | A + B |
| Phase 4 (pick + implement winner) | 2–5 days | Phase 3 |
| **Total** | **~1 week** | |

## Risks

- **Convergence cap masks results.** If we don't bump `max_iterations`
  before running, the "final cost" numbers in Phase 1 are stuck at
  whatever-1000-iter-gets, which doesn't differentiate slow-but-good
  metrics from fast-but-incorrect ones. Need to raise the cap (or add
  convergence check) first.
- **Metric coupling.** Some metrics may be *negatively* coupled — e.g.,
  WSD and collinear pull corners in different directions. Pure
  ablation might say "metric X doesn't matter" when really it
  *cancels* metric Y; turn both off and quality collapses. The
  `only_X` configs in A1 catch this.
- **Visual quality not captured by total cost.** A polygon that scores
  well on total cost may still look bad if e.g. WSD dropped 0.1 m but
  produced a degenerate spike. Need to keep the lab UI rendering as
  the sanity check.
