# V7 test strategy — implementation tests + 3D model validity tests

Two parallel work streams. Different inputs, different assertions,
different cadences, different consequences when red.  This doc is the
kickoff brief; both streams can be picked up independently from §8.

## Status (2026-05-25, post origin/main merge)

- **Stream A — implementation tests**: **complete**.  17 files in
  `core/tests/v7_*_test.c` exercise every v7 step + kd-tree at the
  spec-invariant level; ~14.7k check assertions; green under release +
  ASan + clang. The 2026-05-25 merge with `main` added 2 files
  (`v7_polygon_primitives_test`, `v7_grid_contour_test`) for main's new
  shared helpers, and aligned `v7_boundary_signals_test` /
  `v7_boundary_merge_test` with main's new `lidar_density` contract and
  corner-source unification (#254-263). See §3.3.1 for the two test
  conventions now in play.
- **Stream B — 3D model validity tests**: **B.1 skeleton delivered,
  B.2+ paused.**  The harness at `tests/v7-model-validity/` runs
  end-to-end on `flat-default` and emits a scorecard JSON; one metric
  (`plane_count_match`) is implemented.  Further metric build-out
  (B.2), tier expansion (B.3–B.4), nightly wiring (B.5), and
  dashboard (B.6) are **deferred until the v3 v7 port is
  feature-complete**.  Scoring algorithm quality on a half-ported
  pipeline conflates port-divergence with algorithm-quality and so
  isn't meaningful yet; resume when the port reaches parity with
  bench-v2 on real inputs.

The doc below remains the kickoff brief for both streams.

## TL;DR

- **Implementation tests** — does the v3 C code do what the v7 algorithm
  spec says it should?  Hand-crafted micro-inputs, tight tolerances,
  binary pass/fail.  Live in `core/tests/v7_*_test.c`.  Run every PR.
- **3D model validity tests** — does the v7 pipeline produce a valid
  3D model of the building it was fed?  Synthetic roof inputs from
  bench-v2's analytic generators, scored against the generator's
  ground truth.  Live in `tests/v7-model-validity/`.  Run on release
  + nightly.  Score is tracked over time; CI gates on regression, not
  on absolute pass/fail.

Both are *quality* — that word stays as the umbrella covering every
form of validation (unit, integration, sanitizer, formal, fuzz,
coverage, the two streams below, and the other layers in
`docs/quality/README.md`).

## 1. Concepts and vocabulary

### 1.1 Implementation tests

The question: **"Does the implementation do what the algorithm says it does?"**

The algorithm is a spec.  We treat it as fixed and given.  An
implementation test asks: feed input X to the implementation, does it
produce output Y that matches the spec's prediction for X?

Examples of what an implementation test asserts:
- `sphericalKdeWeights` weights always sum to 1.0 (a stated invariant
  of the algorithm).
- Weighted PCA on 10 coplanar points produces a normal aligned with
  the plane to within `1e-6`.
- Hemisphere-segment on 100 normals all equal to `(0,1,0)` returns
  exactly 1 segment with all 100 points in it.

Failure semantic: **bug in the C port**.  Fix the code; the algorithm
hasn't changed.

These tests do **not** ask "is the algorithm good?".  They ask "is the
algorithm correctly implemented?".  A correct implementation of a bad
algorithm passes every implementation test.

### 1.2 3D model validity tests

The question: **"Does the v7 pipeline produce a valid 3D model of the
building it was fed?"**

We feed a synthetic point cloud generated from a known analytic roof
geometry.  We compare the v7 output (planes, polygons, edges, corners)
against the generator's known ground truth.  We score on multiple
metrics — plane count match, normal angle error, polygon IOU, footprint
IOU, etc.

Failure semantic: **the algorithm got worse** OR **the input got
harder than the algorithm can handle**.  No automatic fix; engineers
investigate, retune, or accept a new baseline.

These tests are research-grade — score goes up by improving the
algorithm, not by fixing bugs.  They live alongside the impl tests
because they need the impl to be correct first.

### 1.3 Why "quality" is wrong for either of them

"Quality" is the umbrella over everything in `docs/quality/`:
- ctest unit tests, integration scenarios, fuzz, coverage
- ASan, TSan, UBSan, Valgrind
- Frama-C audit, ACSL contracts
- the two streams below

Both streams are forms of quality work.  Don't conflate the term.

### 1.4 What changes when the v2 retirement happens

Today, the v3 v7 port is being matched against bench-v2 on the
Dahouet test case.  That matching effort is a debugging tool, not a
quality discipline — the v2 baseline is a target-during-port, not a
validated ground truth.

After v2 retires, the matching effort retires with it.  What stays:
- The bench-v2 algorithm spec (the `lib/pipeline/steps/*.js` files
  and `test/*.test.js`) becomes the recorded definition of what each
  v7 step is meant to do.  Implementation tests stay green because
  they test against the spec.
- The bench-v2 synthetic roof generators (`lib/synthetic/`) become
  the validated input source for 3D model validity tests.  Scorecard
  tracking continues.

Neither stream depends on bench-v2 staying alive for end-to-end
diffs.  Both reuse bench-v2 **assets**: test files (ported once into
C), and the synthetic generators (potentially as a forever-vendored
input generator, or ported to C).

## 2. v2 resources we leverage

bench-v2 lives at `../lidar-algos-bench-v2/` (sibling repo, not in v3).

### 2.1 Algorithm unit tests — for implementation tests

`test/*.test.js` — 56 test files.  Each tests one piece of one step in
isolation, with synthetic micro-inputs and tight assertions.

Files relevant to v7:

| v3 step | bench-v2 test file(s) | What they assert |
|---|---|---|
| `v7-tile-signals` | `test/tile-signals.test.js` | sphericalKdeWeights invariants (sum=1, positive); weightedPCA on coplanar points; weighted PCA on anisotropic distribution |
| `v7-hemisphere-segment` | `test/hemisphere-segment.test.js` | clustering on known separated normals; partition completeness |
| `v7-normal-cascade-seed` | `test/ridge-and-alignment.test.js`, `test/ridge-knn-density.test.js`, `test/edge-detection.test.js` | seed selection criterion on input where the criterion is unambiguous |
| `v7-density-voids-2` | (no direct file; relevant: `test/contour.test.js`, `test/edges.test.js`) | density-gap detection |
| `v7-grow-refit-merge` | `test/plane-extractor.test.js`, `test/shape-fitting.test.js`, `test/pca.test.js`, `test/pca-extra.test.js` | plane fitting on coplanar input; region growing; merge tolerances |
| `v7-beam-incidence` | `test/sensor-params.test.js`, `test/geometry.test.js` | incidence angle from known normal + beam |
| `v7-boundary-topology` | `test/contour.test.js`, `test/hull-helpers.test.js` | edge count + adjacency on known polygons |
| `v7-boundary-signals` | `test/edges.test.js`, `test/edge-detection.test.js` | per-edge signals on synthetic edges |
| `v7-boundary-corners` | `test/edges.test.js`, `test/metrics-edges.test.js` | corner count + position on rectangles, squares |
| `v7-boundary-polygons` | `test/postprocess.test.js`, `test/postprocess-extra.test.js`, `test/lod2-simplify.test.js` | polygon assembly + simplification |
| `v7-boundary-merge` | `test/composite.test.js`, `test/scorer-v3.test.js` | polygon union, merging coplanar regions |
| kd-tree (`steps/normals/kd_tree.c`) | `test/kd-tree-global.test.js`, `test/min-heap.test.js` | tree structure, neighbour query correctness |

Plus generic geometry / math libs that v7 uses:
`test/geometry.test.js`, `test/geometry-extra.test.js`,
`test/primitives-extra.test.js`, `test/convex-hull-extra.test.js`,
`test/weighted-kde.test.js`, `test/lambert.test.js`.

### 2.2 Synthetic generators — for 3D model validity tests

`lib/synthetic/` — the analytic roof + LiDAR simulation library.

```
lib/synthetic/
  roof-types.js               30+ roof generators (flat, gable, hip,
                              asymmetric_gable, l_shaped, t_shaped,
                              multi_section, mansard, sawtooth,
                              mono_pitch, dome, cone_spire, cylinder_silo,
                              flat_with_skylight, flat_with_obstacles,
                              flat_multi_level_2, flat_multi_level_3,
                              ogive_dome, etc.)
                              Each returns typed analytic ground truth:
                              { type, footprint, planes[], surfaces[],
                                edges[], total_roof_area_m2, ground_level }
                              where planes[] has { id, normal, d, tilt_deg,
                              azimuth_deg, vertices, area_m2, boundary_2d }
                              and edges[] has { type ∈ {ridge,eave,hip,
                              valley,step}, from, to, plane_ids[] }.
  point-cloud-generator.js    Deterministic aerial-LiDAR simulator.
                              Configurable density, noise sigma, outlier
                              ratio, ground inclusion, misclassification
                              rate, RNG seed.  Output is the raw bytes a
                              v3 step would consume (positions Float32,
                              classifications Uint8).
  test-case-builder.js        Composition helper.
  curved-surface-sampler.js   For dome / cone / cylinder / ogive types.
  flight-strip-generator.js   Sensor-line bias simulator.
  ortho-generator.js          Synthetic ortho images for downstream tests.
  sensor-profiles.js          Realistic LiDAR sensor parameter sets.
  mock-metadata.js            Geo-metadata stubs.
```

### 2.3 Pre-built test cases — for 3D model validity tests

`test-cases-v2/*.json` — 309 cases, all small JSON files.  Each names a
roof type + roof params + cloud params + seed.  Point cloud is
generated on demand from these specs.

```
test-cases-v2/
  flat-default.json           roof + cloud spec; deterministic
  flat-small.json
  flat-large.json
  gable-default.json
  ... 305 more
```

Each is ~30 lines of JSON.  We can curate per-tier subsets ("easy",
"standard", "hard") without copying inputs.

### 2.4 Existing dump scripts — useful for both streams

`scripts/` in bench-v2:
- `dump-step-input.js` — emits the input to a named step from a chosen
  test case, in v3's expected wire format.
- `dump-v7-refs.js` — emits the reference outputs of a v7 pipeline run
  on a chosen test case.
- `backfill-footprints.js` — computes analytic footprints for cases.
- `benchmark-worker.js` — already drives the v2 pipeline against the
  309 cases; useful pattern.

## 3. Implementation tests — design

### 3.1 Where they live

```
core/tests/
  v7_tile_signals_test.c
  v7_hemisphere_segment_test.c
  v7_normal_cascade_seed_test.c
  v7_density_voids_2_test.c
  v7_grow_refit_merge_test.c
  v7_beam_incidence_test.c
  v7_boundary_topology_test.c
  v7_boundary_signals_test.c
  v7_boundary_corners_test.c
  v7_boundary_polygons_test.c
  v7_boundary_merge_test.c
  v7_kd_tree_test.c
```

17 files (14 v7 steps incl. boundary-classify + wall-detect +
boundary-regularize + kd-tree + 2 shared helpers: polygon-primitives,
grid-contour).  Registered in `core/tests/CMakeLists.txt` under the
existing ctest matrix — exercised by release / asan / clang variants in
the Fast tier already.

### 3.2 Standard test-file shape

```c
// core/tests/v7_<step>_test.c
//
// Implementation tests for v7-<step>.  Each test asserts a property
// the algorithm spec claims to hold.  Inputs are hand-crafted,
// minimal, deterministic.  Tolerances are tight (binary pass/fail).
//
// Spec source: bench-v2 lib/pipeline/steps/<step>.js +
//              bench-v2 test/<step>.test.js (the JS unit tests
//              encode the same invariants).

#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "v7-<step>-impl.h"

// ─── harness ────────────────────────────────────────────────────────
static int g_checks = 0; static int g_failed = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failed++; } \
} while (0)

#define CHECK_CLOSE(a, b, eps) do { \
    double _a = (a), _b = (b); \
    g_checks++; \
    if (fabs(_a - _b) > (eps)) { \
        fprintf(stderr, "FAIL %s:%d: %s ≈ %s (got %.12g, want %.12g, eps=%g)\n", \
            __FILE__, __LINE__, #a, #b, _a, _b, (eps)); \
        g_failed++; \
    } \
} while (0)

// ─── input builders ─────────────────────────────────────────────────
static void build_unit_dirs_5(double dirs[15]) { /* ... */ }
// ... step-specific helpers ...

// ─── tests ──────────────────────────────────────────────────────────
static void test_<invariant_1>(void) { /* ... */ }
static void test_<invariant_2>(void) { /* ... */ }
// ...

int main(void) {
    test_<invariant_1>();
    test_<invariant_2>();
    // ...
    printf("%d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed == 0 ? 0 : 1;
}
```

Pattern matches the existing `core/tests/*_test.c` files (hash_test,
store_test, etc.).  Same `CHECK` / `CHECK_CLOSE` macros where possible.

### 3.3 CMake registration

In `core/tests/CMakeLists.txt`, alongside the existing per-step blocks:

```cmake
# v7 implementation tests
foreach(_v7step
    tile_signals hemisphere_segment normal_cascade_seed density_voids_2
    grow_refit_merge beam_incidence boundary_topology boundary_signals
    boundary_corners boundary_polygons boundary_merge kd_tree)
    set(_t v7_${_v7step}_test)
    add_executable(${_t} ${_t}.c)
    target_link_libraries(${_t} PRIVATE dioramap_steps_v7 m)
    add_test(NAME ${_t} COMMAND ${_t})
endforeach()
```

The as-built foreach links `dioramap_steps_normals m` (not
`dioramap_steps_v7`): each test `#include`s the impl `.c` directly to
reach file-statics, so linking the full v7 lib would duplicate the
impl's public symbols. Steps whose impl pulls in a shared helper source
(e.g. boundary-polygons → `v7-polygon-primitives.c`, boundary-signals →
`v7-grid-contour.c`) add that helper as a per-test extra source in the
foreach.

### 3.3.1 Two complementary test conventions (post-merge, 2026-05-25)

The 2026-05-25 merge with `main` brought in a *second* test convention.
Both are kept — they test different surfaces and both run under ctest:

| | **White-box impl-conformance** (this doc / Stream A) | **Public-API / fixture** (main) |
|---|---|---|
| Location | `core/tests/v7_<step>_test.c` | `steps/v7/v7-<step>-test.c` |
| Registration | foreach in `core/tests/CMakeLists.txt` | explicit in `core/CMakeLists.txt` |
| Reaches | file-statics (`#include`s impl `.c`) | public API only (links `dioramap_steps_v7`) |
| Links | `dioramap_steps_normals m` | `dioramap_steps_v7 [jansson m]` |
| Examples | the 12 `v7_*_test` | `v7-roof-graph-format-test` (TLV machinery), `v7-bp2-test` (794-line JSON-fixture driver) |

Rule of thumb when adding a new step's tests: if the invariant lives in
a file-static helper, use the white-box convention; if it's a public
entry point or needs JSON fixtures, main's convention is fine. Don't
migrate existing tests between conventions — it buys nothing.

### 3.4 Tolerance policy

- **Counts, names, enums, types**: exact.  Off-by-one in a count IS
  the bug.
- **Floats**: tolerances explicit per assertion, drawn from the bench-v2
  JS test's `closeTo(a, b, delta)` invocations.  Typical: 1e-6 for
  angle/normal/position assertions; 1e-10 for normalised-to-one
  invariants like KDE weight sums.
- **Integers and indices**: exact.
- **Sets** (e.g. "the seed indices set"): order-independent comparison
  via sort + memcmp.

### 3.5 What to derive from each v2 test file

For each `test/<step>.test.js`:

1. Read the `describe(...)` and `it(...)` blocks.  Each `it` is one
   invariant.
2. Read the `assert.ok(...)` / `closeTo(...)` calls.  Each is one
   `CHECK` in the C port.
3. Read the input-builder helpers (`buildDirBuf`, `flatRoofMesh`,
   etc.).  Port them as C static helpers.
4. The Python skim of a typical file: `lib/synthetic` calls become
   C input builders; pipeline-step calls become C calls into the
   `v7-<step>-impl.h` API; assertions stay as `CHECK_CLOSE`.

A faithful port is 1:1.  No new tests in the first pass; no missing
tests in the first pass.  When v2 had `it(...)` ×17 in a file, v3
gets 17 `test_*()` functions.

### 3.6 Per-step content sketch

Compact catalog of what each test file covers.  Detail is to be filled
in PR-by-PR by reading the matching v2 test file.

| Test file | Headline invariants (from v2's test file) |
|---|---|
| `v7_tile_signals_test.c` | `sphericalKdeWeights` returns N weights, all positive, sum=1; isotropic neighborhoods → near-uniform weights; anisotropic (scan-line) → biased; `weightedPCA` recovers correct normal on known plane; PCA corrects scan-line bias |
| `v7_hemisphere_segment_test.c` | 100 normals = (0,1,0) → 1 segment; 2 distinct clusters → 2 segments; partition completeness (every input in one segment); empty input handled |
| `v7_normal_cascade_seed_test.c` | Seed selection on input where the criterion is unambiguous; seed normals match selected indices; ordering deterministic |
| `v7_density_voids_2_test.c` | Uniform 4×4 grid → 0 voids; grid with 1 missing cell → 1 void at that cell; outlier in void is robust |
| `v7_grow_refit_merge_test.c` | 100 coplanar points + 1 seed → 1 region, normal within ε; 200 points across 2 known planes → 2 regions, normals matching; coplanar regions merge per spec |
| `v7_beam_incidence_test.c` | Beam ⊥ surface → cos=1; beam ∥ surface → cos=0; analytic angle within ε; reverse-beam symmetry |
| `v7_boundary_topology_test.c` | Triangle (3 vertices) → 3 edges; rectangle → 4 edges; empty → 0 edges; closed curve invariant (every vertex degree=2) |
| `v7_boundary_signals_test.c` | One edge of known length → exact signal; signal array length = edge count; ordering preserved |
| `v7_boundary_corners_test.c` | Square (4 pts) → 4 corners at exact positions; sub-threshold input → 0 corners; deduplication on overlapping inputs |
| `v7_boundary_polygons_test.c` | 5 convex corners → 1 polygon length 5; concave → spec-defined behavior (1 polygon with hole, or 2 polygons); winding-order convention |
| `v7_boundary_merge_test.c` | 2 adjacent rectangles → 1 merged rectangle; 2 disjoint → 2 polygons; coplanar misaligned → spec-defined tolerance behavior |
| `v7_kd_tree_test.c` | Build + query correctness on 100 known points; nearest-N query against brute force on small N; balanced within k; rebuild idempotent |

### 3.7 What success looks like for the implementation tests stream

When this stream is complete:

- 12 new test files exercise every public function of every v7-impl
  module and the kd-tree.
- Every PR runs them as part of `./scripts/ci/run-tier.sh fast`
  (release + asan + clang).
- A diff between v2 and v3 on Dahouet that's traceable to one step:
  the corresponding `v7_<step>_test.c` either had a missing assertion
  (add it), or already failed (the bug is localised).
- When v2 retires, these test files become the canonical record of
  what each v7 step does.

## 4. 3D model validity tests — design

### 4.1 The pipeline

```
test-case-v2 JSON spec
    ↓ bench-v2 generator (Node)
synthetic point cloud bytes + analytic ground truth
    ↓ v3 v7 pipeline (existing v7-*-bench drivers, or a single sweep harness)
v3 output 3D model (planes, polygons, edges, corners)
    ↓ comparator (Python or C — easier in Python)
metrics per metric kind per case
    ↓ scorecard generator
scorecard.json + history line
    ↓ baseline-diff
PR comment / dashboard plot / regression alarm
```

### 4.2 Inputs

Reuse `test-cases-v2/*.json` from bench-v2 directly.  No duplication of
synthetic inputs into v3.

Curate per-tier subsets via case-id lists:

```
tests/v7-model-validity/
  cases/
    easy.txt          ~10 cases: flat-default, gable-default, hip-default,
                                 mono_pitch-default, etc. — simple geometry,
                                 default noise.  Target: 100% pass.
    standard.txt      ~50 cases: all roof types, default params.  Target:
                                 baseline score, regression-tracked.
    hard.txt          ~80 cases: complex shapes (l_shaped, t_shaped,
                                 multi_section, with_obstacles), high noise,
                                 ghost points.  Target: improvement-tracked.
    all.txt           all 309 cases.  Nightly only.
```

Per-tier scoring keeps "regressed on easy" distinct from "didn't
improve on hard".  Easy = bug class.  Hard = algorithm class.

### 4.3 Per-case metrics

Computed by comparing v3's output (planes / polygons / edges / corners)
against the generator's ground truth structures.

| Metric | Type | Default tolerance | What a real change looks like |
|---|---|---|---|
| `plane_count_match` | bool | exact | algorithm splits or fuses planes wrongly |
| `plane_normal_angle_error_max` | degrees | 5° | PCA / fit / weighting bug |
| `plane_normal_angle_error_p95` | degrees | 3° | systematic bias |
| `plane_area_error_pct_max` | % | 10 % | grow/merge boundary drift |
| `plane_area_error_pct_total` | % | 5 % | scale / coverage gap |
| `polygon_count_match` | bool | exact | merge or split error |
| `polygon_iou_min` | [0,1] | 0.85 | boundary-corners or polygons misshape |
| `polygon_iou_mean` | [0,1] | 0.90 | aggregate boundary quality |
| `edge_count_by_type_match` | bool per type | exact | topology miscount per (ridge,eave,hip,valley,step) |
| `edge_length_total_error_pct` | % | 5 % | geometry drift |
| `corner_count_match` | bool | exact (with merge tol) | over- / under-clustering of corners |
| `corner_position_error_max` | meters | 0.1 m | corner-precision drift |
| `footprint_iou` | [0,1] | 0.85 | bench-against-analytic footprint |
| `run_duration_ms` | int | — (tracked, not gated) | perf regression alarm |

Metric matching is one-to-one between v3 output items and ground-truth
items — use Hungarian matching on plane-normal-angle for plane
correspondence, polygon centroid distance for polygon correspondence,
corner distance for corner correspondence.

A case passes iff every gated metric is within tolerance.  Tolerances
are per-tier (easy is strict; standard is permissive; hard is
informative-only by default).

### 4.4 Scorecard format

One JSON file per run, machine-readable + diffable:

```json
{
  "schema_version": "1.0",
  "git_sha": "abc123",
  "git_branch": "feat/v7-corner-stability",
  "timestamp": "2026-05-20T14:21:00Z",
  "v3_v7_pipeline_version": "0.2.2",
  "bench_v2_commit": "a30f9bf",
  "tier": "standard",
  "case_count": 52,

  "summary": {
    "passed": 47,
    "failed": 5,
    "failed_by_metric": {
      "polygon_iou_min": 3,
      "corner_position_error_max": 2
    },
    "metric_means": {
      "plane_normal_angle_error_p95": 1.42,
      "polygon_iou_mean": 0.912,
      "footprint_iou": 0.961
    }
  },

  "cases": {
    "flat-default": {
      "passed": true,
      "metrics": {
        "plane_count_match": true,
        "plane_normal_angle_error_max": 0.18,
        ...
      },
      "duration_ms": 142
    },
    "gable-default": { ... },
    ...
  }
}
```

Diff against baseline produces a markdown table consumable as a PR
comment (after CI is wired): newly-failing cases, newly-passing cases,
metric-mean shifts.

### 4.5 Baseline + drift

- The first run on the curated subset becomes
  `tests/v7-model-validity/baselines/<tier>.json`.  Committed.
- Every subsequent run diffs against the baseline.
- Regression rule per tier:
  - `easy`: zero regression allowed.  If a case that passed in baseline
    fails now, the tier fails.
  - `standard`: regression on individual cases reports but does not
    block; aggregate metric-mean regression > 5 % blocks.
  - `hard`: regression on individual cases does not block; metric-mean
    regression is reported only.
- Baseline is rewritten manually when the algorithm changes
  legitimately — operator commits the new scorecard as the new baseline
  with a message explaining why.

### 4.6 Where it lives

```
tests/v7-model-validity/
  README.md                       — what this is + how to read scorecards
  run.sh                          — entry point: bash, calls Python for
                                    the heavy lifting.  Same shape as
                                    the existing tests/run-v7-*-bench-
                                    regression.py scripts.
  cases/
    easy.txt
    standard.txt
    hard.txt
    all.txt                       — every bench-v2 case id
  metrics/
    compare.py                    — per-case scoring (plane matching,
                                    polygon IOU, etc.)
    scorecard.py                  — aggregate + format
  baselines/
    easy.json                     — committed baseline for the easy tier
    standard.json
    hard.json
  history/                        — accumulated scorecards over time,
                                    suitable for a trend dashboard.
                                    Gitignored or .lfs'd depending on
                                    eventual size.
```

### 4.7 CI tier wiring

A new tier added to `scripts/ci/run-tier.sh`:

```bash
case "$TIER" in
    fast)    ... ;;
    full)    ... ;;
    release) # build + full + v7-model-validity standard
             run_build release; run_ctest release
             run_build asan;    run_ctest asan
             run_integration asan
             run_build clang;   run_ctest clang
             run_build cov; run_coverage
             run_frama_c
             run_v7_model_validity standard ;;
    nightly) ... + run_v7_model_validity all ;;
esac
```

`run_v7_model_validity()` reads `tests/v7-model-validity/cases/<tier>.txt`,
calls `node ../lidar-algos-bench-v2/scripts/dump-step-input.js` for each
case to materialise the synthetic point cloud, runs the v3 v7 pipeline
on it, calls `metrics/compare.py` for the per-case score, accumulates
into a scorecard, and diffs against the baseline.

### 4.8 Hosting + dashboard

Scorecards land in `ci-artifacts/<run>/v7-model-validity/scorecard.json`
plus a copy at `tests/v7-model-validity/history/<timestamp>.json` when
the tier passes on `main`.  A future dashboard (gh-pages) plots
per-metric trend lines over the history.

## 5. Why use bench-v2 (and what about the eventual retirement?)

bench-v2's generators are the only source of trustable analytic ground
truth for the v7 pipeline today.  The choice is:

A. **Call bench-v2 from Python** during the 3D model validity tests.
   Pros: zero porting risk; reuse 309 cases as-is; trivial to add new
   roof types.  Cons: node runtime dep; slower than C-direct.

B. **Port bench-v2's generators to C / Python**.  Pros: removes the
   node dep; faster.  Cons: ~2 weeks of careful porting; new bugs
   become possible.

**Decision: A for now.**  bench-v2 stays as the source of the inputs
even after the v3 v7 pipeline replaces v2's *processing*.  The
synthetic generators are well-tested data tooling, not algorithm
code — there's no engineering case for replacing them.

When v2 retires as an algorithm, fork the `lib/synthetic/` subtree
into v3 (or vendor it) and retire the rest of v2.  Approximately 5
.js files, ~1000 lines total, no external dependencies; very portable.

## 6. Streams in detail

### 6.1 Stream A — implementation tests

#### A.1 — `v7_tile_signals_test.c` (first PR, ~3 h)

Port `test/tile-signals.test.js`.  Specifically:

- `sphericalKdeWeights` test block (~6 invariants)
- `weightedPCA` test block (~5 invariants)
- Anisotropic-correction test block (~3 invariants)

Deliverables:
- `core/tests/v7_tile_signals_test.c`
- `core/tests/CMakeLists.txt` change to register
- ctest run shows the test executing under `release`, `asan`, `clang`,
  `cov` builds; all green

This PR is the **template**.  Subsequent PRs follow the same shape.

#### A.2 — `v7_hemisphere_segment_test.c` (~3 h)

Port `test/hemisphere-segment.test.js`.

#### A.3 to A.11 — one per remaining step (~3-4 h each)

Same pattern.  Each PR adds one file + one CMake entry.  Parallelizable.

#### A.12 — `v7_kd_tree_test.c` (~3 h)

Port `test/kd-tree-global.test.js`.  This one's standalone (under
`steps/normals/`, not `steps/v7/`); still ports the same way.

Stream A complete: ~12 PRs over ~1-2 weeks of focused work, fully
parallelizable.

### 6.2 Stream B — 3D model validity tests

#### B.1 — harness skeleton  **(DONE — 2026-05-20)**

Delivered:
- `tests/v7-model-validity/run.sh`                    — bash orchestrator
- `tests/v7-model-validity/scorecard.py`              — JSON aggregator
- `tests/v7-model-validity/metrics/compare.py`        — implements
  `plane_count_match` (parses v3's ARRAY_INLINE + RGRAPH TLV)
- `tests/v7-model-validity/tools/materialize-case.mjs` — Node helper
  that calls bench-v2's `loadTestCase` and writes a v3-readable TCBD
  `.bin` + v2-shaped spec JSON + ground-truth JSON
- `tests/v7-model-validity/cases/easy.txt` — one case (`flat-default`)
- `tests/v7-model-validity/README.md`

Smoke verified end-to-end: materialize → v3 v7 pipeline (15 steps) →
compare → scorecard.  Exit code reflects whether the metric matched.

Notes from the smoke:
- v3's `v7-load-real-test-case` declares the 4 optional TCBD columns
  (scan-angles, point-source-ids, return-numbers, number-of-returns)
  as REQUIRED writes.  Worked around in `materialize-case.mjs` by
  zero-filling the columns when bench-v2 doesn't supply them.  The
  step's `writes_arr` could mark them optional in a follow-up.
- bench-v2 needs `npm install` once inside
  `../lidar-algos-bench-v2/`  — required for transitive deps
  (`polygon-clipping`, `proj4`, etc.) when materialize-case.mjs runs.

#### B.2 – B.6 — **paused until the v7 port reaches parity**

Reasoning: B.2+ scores algorithm quality (the "does v7 produce a
valid 3D model?" question of §1.2).  While the v3 v7 port is still
being aligned with bench-v2 on real inputs, the scorecard would
conflate two distinct sources of disagreement — port divergence (a
bug-fixing concern) and algorithm-quality drift (a research
concern).  Wiring the full metric set + baselines + tier integration
now would lock in a baseline that isn't a meaningful target.

Resume B.2–B.6 when:
- the v3 v7 port matches bench-v2's output on the Dahouet real case
  (the current divergence-analysis effort completes), AND
- bench-v2 retirement is in sight (so the synthetic generators become
  v3's source-of-truth input rather than a shared sibling).

When resumed, the work breakdown stays as originally drafted:

- B.2 — full metric set on the easy tier (~2 days).  Implement all
  metrics listed in §4.3.  Curate easy.txt to ~10 cases.
- B.3 — standard tier + baseline (~2 days).  Curate standard.txt.
  Run, commit the first passing scorecard as the baseline.  Build
  the baseline-diff tool.
- B.4 — hard tier + nightly wiring (~2 days).  Curate hard.txt.
  Wire `run-tier.sh release` and `run-tier.sh nightly`.
- B.5 — dashboard rendering (~half day).  Static HTML page consuming
  the history/ JSONs, served out of `gh-pages`.

B.1's skeleton stays in place; B.2+ resume by extending it.

## 7. CI tier mapping

| Tier | What it runs | Implementation tests | 3D model validity tests |
|---|---|---|---|
| `fast` | < 30 s; release + asan ctest + 4 essential integration scenarios | runs all 17 v7 impl tests under release + asan ctest | — |
| `full` | < 5 min; adds clang, all integration scenarios, coverage, Frama-C | also under clang | — |
| `release` (new) | a few minutes more; on tag push and pre-release | (covered by full) | runs `standard` tier, blocks on regression of any easy case OR aggregate metric > 5 % |
| `nightly` | < 8 h; full + valgrind + fuzz + TSan | (covered by full) | runs `all` cases, records scorecard, opens issue on regression |

The implementation tests are gating; the 3D model validity tests are
reporting + selectively gating.  Mirrors the user's intent: PRs are
blocked by impl bugs; releases are blocked by algorithm regressions.

## 8. Agent-startable work breakdown

Pick from this list.  Each item is a self-contained PR; minimal
overlap between concurrent agents.

### Implementation tests (Stream A)

| ID | PR scope | Inputs to read | Outputs |
|---|---|---|---|
| A.1 | `v7_tile_signals_test.c` + CMake | bench-v2 `test/tile-signals.test.js`, `lib/pipeline/steps/tile-signals.js`, v3 `steps/v7/v7-tile-signals-impl.h` | new test file; CMake entry; ctest green |
| A.2 | `v7_hemisphere_segment_test.c` | `test/hemisphere-segment.test.js`, `v7-hemisphere-segment-impl.h` | same |
| A.3 | `v7_normal_cascade_seed_test.c` | `test/ridge-and-alignment.test.js`, `test/ridge-knn-density.test.js`, `v7-normal-cascade-seed-impl.h` | same |
| A.4 | `v7_density_voids_2_test.c` | `test/contour.test.js`, `v7-density-voids-2-impl.h` | same |
| A.5 | `v7_grow_refit_merge_test.c` | `test/plane-extractor.test.js`, `test/shape-fitting.test.js`, `test/pca.test.js`, `v7-grow-refit-merge-impl.h` | same |
| A.6 | `v7_beam_incidence_test.c` | `test/sensor-params.test.js`, `test/geometry.test.js`, `v7-beam-incidence-impl.h` | same |
| A.7 | `v7_boundary_topology_test.c` | `test/contour.test.js`, `test/hull-helpers.test.js`, `v7-boundary-topology-impl.h` | same |
| A.8 | `v7_boundary_signals_test.c` | `test/edges.test.js`, `v7-boundary-signals-impl.h` | same |
| A.9 | `v7_boundary_corners_test.c` | `test/edges.test.js`, `test/metrics-edges.test.js`, `v7-boundary-corners-impl.h` | same |
| A.10 | `v7_boundary_polygons_test.c` | `test/postprocess.test.js`, `test/lod2-simplify.test.js`, `v7-boundary-polygons-impl.h` | same |
| A.11 | `v7_boundary_merge_test.c` | `test/composite.test.js`, `test/scorer-v3.test.js`, `v7-boundary-merge-impl.h` | same |
| A.12 | `v7_kd_tree_test.c` | `test/kd-tree-global.test.js`, `steps/normals/kd_tree.h` | same |

For each:
1. Read the matching v2 `.js` test file + algorithm `.js`.
2. Read the v3 `.h` impl module.
3. Port the JS test fixtures + asserts to C using the §3.2 template.
4. CMake registration as in §3.3.
5. Run `./scripts/ci/run-tier.sh fast` locally; verify the new
   testcase appears in ctest output and is green.
6. Commit.

### 3D model validity tests (Stream B)

| ID | PR scope | Inputs | Outputs |
|---|---|---|---|
| B.1 | harness + 1 case + 1 metric (smoke) | bench-v2 `scripts/dump-step-input.js`, v3 `steps/v7/v7-grow-refit-merge-bench.c` | `tests/v7-model-validity/{run.sh, cases/easy.txt[1 entry], metrics/compare.py, scorecard.py}` |
| B.2 | all per-case metrics on flat-default | §4.3 metric list | full metrics/compare.py |
| B.3 | easy tier (~10 cases) + baseline | bench-v2 test-cases-v2/*.json | baselines/easy.json committed |
| B.4 | standard tier + baseline | same | baselines/standard.json committed |
| B.5 | hard tier + nightly wiring | same | run-tier.sh nightly updated |
| B.6 | dashboard rendering | history/*.json | static HTML on gh-pages |

For each: documented step-by-step in the `tests/v7-model-validity/README.md`
the harness PR (B.1) introduces.

## 9. Decisions made in this doc

For the record, to avoid re-litigating:

1. **Two test classes** named **implementation tests** and **3D model
   validity tests**.  Not "spec/quality".  Not "unit/bench".  Quality
   is the umbrella.
2. **Implementation tests live in `core/tests/v7_*_test.c`** and are
   ctest-driven, alongside the existing per-module tests.  No separate
   directory.
3. **3D model validity tests live in `tests/v7-model-validity/`** as a
   new top-level test surface.  Driven by bash + Python.  Reuse
   bench-v2 generators via Node.  Not ported to C.
4. **Bench-v2 stays as the input source** indefinitely (or until
   someone wants to fork `lib/synthetic/` into v3).  It does NOT serve
   as the output oracle — the analytic ground truth from
   `roof-types.js` is the oracle.
5. **Dahouet-as-baseline retires when v2 retires**.  The current
   `tests/baselines/v7-*` + `tests/run-v7-*-bench-regression.py` are
   debugging tooling for the port-divergence work, not part of either
   stream above.
6. **Streams A and B are independent**.  No cross-PR dependency.
   Both can land any time after A.1 and B.1.

## 10. Cross-links

- `docs/quality/README.md` — the umbrella + overview of all quality work
- `docs/quality/FRAMA-C-FALSE-POSITIVES.md` — Eva limitations doc; cross-references because some Frama-C alarms surface inside v7 impl modules
- `docs/quality/THIRD-PARTY.md` — bench-v2 isn't a third-party dep today (it's a sibling repo); when it formalises as one, add an entry
- `docs/quality/ROADMAP.md` — the implementation tests + 3D model validity tests should each appear here when work starts
- `core/tests/integration/README.md` — integration scenario authoring; separate test class from implementation tests
