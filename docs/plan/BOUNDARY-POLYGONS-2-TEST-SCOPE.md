# v7-boundary-polygons-2 — Test Scope & Autonomy Contract

**Status:** authoritative as of 2026-05-21. This document defines the gate
that every implementation task for `v7-boundary-polygons-2` is validated
against. It is the autonomy contract: if all gates pass, work is correct
and complete; if any gate fails, work is incomplete and the agent must
either fix it or stop and report.

**Companion docs:**
- `docs/journal/2026/05/2026-05-21-boundary-polygons-tsp-rethink.md`
  — the decision rationale, algorithm sketch, full open-question list.
- `docs/journal/_index.json` — journal index.

If anything in this doc contradicts the journal entry, the journal entry
wins (since it captures the user's reasoning); update this doc to align.

## Scope

The new step `v7-boundary-polygons-2` is being implemented in TWO
languages:

| Repo / language | Step path |
|---|---|
| `lidar-algos-bench-v2/` (Node.js) | `lib/pipeline/steps/boundary-polygons-2.js` |
| `lidar-algos/v3/` (C99) | `v3/steps/v7/v7-boundary-polygons-2.{c,h,impl.c,impl.h}` |

Both implementations target the same algorithm: per-plane component
collection → subset enumeration over the 2^K subsets, with the
**cost function deciding which subset best represents the outer
perimeter** (see journal for rationale — the per-component classify
approach was abandoned 2026-05-21 because it was unreliable on
arc-shaped fragments).
**They must produce equivalent polygons on the same input** — same vertex
set, same cyclic order modulo rotation and reflection. They are *not*
required to be FP-LSB byte-identical; the algorithm is designed to be
stable under sub-millimetre input perturbation, which is the property
that makes both implementations converge to the same global optimum.

## Test architecture — two tiers

```
┌─────────────────────────────────────────────────────────────────┐
│  TIER 2 — integration                                           │
│   synthetic point cloud generator → boundary-signals →          │
│   boundary-corners → boundary-polygons-2 → assert               │
│   • ~10 fixtures, ~5–30 s per run                               │
│   • Catches integration regressions (frame conventions,         │
│     wire-format mismatches, plane-basis projection).            │
│   • Runs before declaring a multi-task milestone complete.      │
└─────────────────────┬───────────────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────────────┐
│  TIER 1 — unit                                                  │
│   hand-built fixtures fed directly to                            │
│   boundary_polygons_2_for_plane() (no upstream step).           │
│   • ~50 fixtures (6 main end-to-end + ~40 sub-function unit).   │
│   • <100 ms per run; full test set well under 30 s.             │
│   • Runs after every code change; gates task completion.        │
└─────────────────────────────────────────────────────────────────┘
```

Tier 1 is the **gate**. Tier 2 is the **safety net**.

## Decisions baked in

| Decision | Value | Source |
|---|---|---|
| **Q1 — Fixture format** | Shared JSON in `tests/fixtures/v7-boundary-polygons-2/` (a new top-level directory in the repo root, accessible from both v2 and v3 paths). | user, 2026-05-21 |
| **Q2 — Cross-validation runner** | Option A: each test framework reads fixtures, produces a polygon, dumps it to a per-fixture `*.actual.json` file in a build-local output dir. A separate node script `scripts/v7-bp2-cross-validate.mjs` diffs `<v2-actual>` against `<v3-actual>` against `<expected>` for every fixture and reports a unified pass/fail. | user, 2026-05-21 |
| **Q3 — Trace output detail** | Per-fixture trace JSON when `BP2_TRACE=1` is set. Includes: per-component candidate cycles + classifications, final cycle score breakdown (WSD, compactness, spike), TSP move history (top-N accepted moves). Off by default to keep tests fast. | agent, 2026-05-21 |
| **Q4 — Synthetic Tier-2 cloud generator** | Yes: `bp2_synth_cloud(corners[], density)` produces group points (uniform inside polygon at given density) + nearby points (a band outside each edge), so it can feed real boundary-signals → boundary-corners → boundary-polygons-2. Generator implemented in both v2 (JS) and v3 (C); fixture JSON references it by name + parameters, the cloud is reconstructed at test time. | user, 2026-05-21 |

## Fixture schema

```jsonc
{
  "id":           "t1-clean-square",
  "category":    "smoke" | "unit-cost" | "unit-wsd" | "unit-classify" |
                  "topology" | "stability" | "integration",
  "description": "one-line summary",

  // What goes in.
  "inputs": {
    "plane_basis": { "e1": [x,y,z], "e2": [x,y,z], "normal": [x,y,z] },
    "anchors": [
      { "id": 0, "x": 0.0, "y": 0.0, "z": 0.0, "source": "concave" },
      ...
    ],
    // Direct point cloud, OR a generator spec.
    "group_pts":  [...] | { "generator": "synth_cloud", "params": {...} },
    "nearby_pts": [...] | { "generator": "synth_cloud", "params": {...} },
    // Optional. If absent, all anchors → single component.
    "balance_field": null | {
      "n_cols": 50, "n_rows": 50, "cell_size": 0.2,
      "g_min_u": -5.0, "g_min_v": -5.0,
      // Either a literal array OR generator that places high-balance
      // cells along a polygon outline:
      "cells": [...] | { "generator": "ring", "outline_corner_ids": [0,1,2,3] }
    }
  },

  // What should come out.
  "expected": {
    // For deterministic tests:
    "polygons": [
      { "corner_ids": [0,1,2,3], "kind": "outer" }
    ],
    "max_wsd_m": 0.001,
    "max_spike_penalty": 0.01,

    // OPTIONAL — for stability tests:
    "stability": {
      "runs":             100,
      "noise_sigma_mm":   1.0,
      "min_match_rate":   0.99
    }
  }
}
```

## Equivalence relation on cycles

```
def cycle_equal(a: list[int], b: list[int]) -> bool:
    if set(a) != set(b): return False
    n = len(a)
    if n != len(b): return False
    if n == 0: return True
    # try every rotation, forward and reverse
    for start in range(n):
        if all(a[i] == b[(start + i) % n]      for i in range(n)): return True
        if all(a[i] == b[(start - i + n) % n]  for i in range(n)): return True
    return False
```

The cross-validation runner uses this relation. The expected polygon and
the actual polygon are compared this way; the v2 polygon and v3 polygon
are compared this way.

## Tier-1 test catalogue

### Sub-tier 1a — pure-function unit tests

| Test | Function | What it asserts |
|---|---|---|
| `cost_geometric_square` | `cost_geometric` | CCW square scores lower than 8-shape (self-intersecting → +∞) |
| `cost_geometric_compactness_order` | `cost_geometric` | thin rectangle > square > circle-approx (lower=better) |
| `wsd_zero_for_perfect_enclose` | `wsd_multi` | group inside + nearby outside → wsd = 0 |
| `wsd_known_outside_distance` | `wsd_multi` | 1 group point 0.5m outside → wsd = 0.5 |
| `wsd_with_hole_excludes_correctly` | `wsd_multi(holes=[...])` | group point inside the hole counts as wrong-side |
| `wsd_two_sided` | `wsd_multi` | nearby point inside outer → counts as wrong-side |
| `spike_penalty_30deg_penalised` | `spike_penalty` | 30° interior angle yields positive penalty |
| `spike_penalty_flat_not_penalised` | `spike_penalty` | 175° interior angle yields 0 (regularization handles flat) |
| `spike_penalty_90deg_zero` | `spike_penalty` | right angle = 0 penalty |
| ~~classify_*~~ | (removed) | classify primitive deleted 2026-05-21; cost-function competition replaces it. |
| `collect_components_one_ring` | `collect_components` | single ring of high-balance cells → 1 component with the corners near it |
| `collect_components_two_separate` | `collect_components` | two disjoint rings → 2 components |
| `collect_components_overlap_ok` | `collect_components` | overlapping rings → corners belong to both components, no error |
| `best_cycle_three_corners_trivial` | `best_cycle` | n=3 → returns the only triangle |
| `best_cycle_four_square` | `best_cycle` | 4 unit-square corners → CCW cycle |
| `best_cycle_self_cross_avoided` | `best_cycle` | adversarial init avoids self-intersection in final |

### Sub-tier 1b — end-to-end fixtures (from journal)

| Test | What it asserts |
|---|---|
| `t1-clean-square` | 4 corners + 100 group + 20 nearby → 4-vertex outer polygon, WSD < 1mm |
| `t2-clean-square-noise` | T1 + σ=1mm corner noise, 100 runs → ≥99 produce same vertex set + cyclic order |
| `t3-extra-collinear-corner` | T1 + 5th corner at edge midpoint → 5-vertex polygon (no deletion in this step) |
| `t4-interior-hole` | T1 + 2×2 interior cutout → 4-vertex outer polygon (subset enum rejects {hole} and {outer ∪ hole}; picks {outer}) |
| `t5-signal-fragmentation` | Outer ring split into two overlapping arc components → subset enum picks the UNION = full 4-vertex polygon. Hard case for the old per-component classify. |
| `t6-rotated-noisy-3d` | T1 randomly rotated + translated + heavy noise, 1000 runs → ≥99% WSD < 0.1m (deferred — needs rotation harness) |
| `t7-spurious-noise-component` | Real outer + spurious noise-component far away → subset enum rejects {noise} and {outer ∪ noise} (high WSD / self-cross); picks {outer}. |
| `t8-outer-with-two-holes` | K=3: outer + 2 separate interior holes → 7 candidate subsets, subset enum picks {outer} (lowest cost; hole-only and unions either weave or have huge WSD). |

## Tier-2 test catalogue

| Test | What it asserts |
|---|---|
| `t2int-square-through-signals` | synthetic square cloud → real signals → real corners → bp2 → 4-vertex polygon |
| `t2int-lshape-through-signals` | L-shape cloud → 6-vertex polygon, no self-intersect |
| `t2int-square-with-real-hole` | synthetic cloud minus a 2×2 cutout → real signals show a hole → bp2 emits 4-vertex outer (K=1) |
| `t2int-dahouet-bldgonly-regression` | the dahouet bldgonly test case fed through the new step — produces SOME polygon for every active plane, no crashes; not a correctness gate (no ground truth on dahouet) |

## Gate definition

For autonomous execution to mark a task complete:

```
GATE(task) = ALL of:
   1. All tier-1 sub-1a unit tests pass in v3   (or v2, whichever is being implemented at the task's scope)
   2. All tier-1 sub-1b end-to-end fixtures pass in v3 (or v2)
   3. When BOTH v2 and v3 exist: cross-validation runner reports 0 disagreements across all tier-1 fixtures
   4. Stability tests (t2, t6) pass at ≥99% match rate
   5. No regression in the rest of the build:
        v3: `ninja` clean + `ctest` green
        v2: `npm test` green (vitest of metrics + pipeline tests)
```

Steps 1-2 gate single-language tasks. Step 3 gates the dual-language port
milestone. Steps 4-5 are continuous; any regression stops the loop.

## Implementation order (autonomous plan)

The agent executes these tasks in order. Each task ends with a commit
referencing its task #. If a task gate fails, the agent stops and reports.

| # | Task | Gate |
|---|------|------|
| 298 | Synthetic test harness skeleton (v3 first, v2 next). Fixture loader, assertion helpers, cross-validation runner skeleton. NO algorithm code. | Tier-1 framework runs and reports "0 fixtures present" cleanly. |
| 295 | Cost function primitives + WSD-multi + spike penalty + sub-1a unit tests for those. v3 first. | All sub-1a unit tests for cost / wsd / spike pass. |
| 296 | best_cycle TSP solver (full-permutation enumeration for small n + multi-start 2-opt + or-opt above). v3 first. | sub-1a tests for `best_cycle*` pass. Full-enum max-n cap measured + documented. |
| 297 | Top-level: `collect_components` + subset enumerator + `boundary_polygons_2_for_plane`. **No per-component classify** — the cost function decides which component subset is the outer perimeter by direct competition. v3 first. | All sub-1b end-to-end tier-1 fixtures pass. Stability tests t2, t6 pass. |
| 297b | v3 step wrapper (TLV section emit) + bench harness (`v7-boundary-polygons-2-bench`). | bench runs on dahouet without crash; produces output for inspection. |
| 297c | v2 JS port (same algorithm). | All tier-1 fixtures pass in v2 + cross-validation across v2/v3 is 0 disagreements. |
| 297d | Tier-2 integration tests through real boundary-signals. | All tier-2 fixtures pass in both v2 and v3. |
| 297e | Pipeline switch (research-v7-tile-signals-real.json) to use boundary-polygons-2 instead of boundary-polygons. | Pipeline runs cleanly on dahouet; merge / classify / regularize downstream consume the new output without error. |

## Decisions left to the agent during autonomous execution

The agent has authority on:

- **TSP move set details** within the family {2-opt, or-opt, 3-opt} — start with 2-opt + or-opt, add 3-opt if multi-start convergence is insufficient.
- **Full-enum max-n cap** (V7_BP2_FULL_ENUM_MAX) — current 8 (8!/2 = 20160 cost evaluations). Above this the algorithm switches to multi-start local search. Tune based on actual per-plane runtime on dahouet.
- **K_RANDOM** initial-cycle seed count — start at 5, increase if stability tests fail.
- **Cost-function weight tuning** within ±50% of the journal's `0.8 / 0.1 / 0.1` — must remain pareto-reasonable and pass all fixtures.
- **Synthetic balance-field generator details** — exact cell-placement strategy for the ring along a polygon outline.
- **Fixture additions** — agent can add more fixtures if they catch real bugs (must document each addition).

The agent does NOT have authority on:

- Algorithm architecture (TSP-based, K=1-for-now, topology-then-ordering split).
- Validation criterion (stability ≥ 99%, not byte parity).
- Step name (`v7-boundary-polygons-2`).
- Dual-implementation requirement (both v2 and v3).
- Cost-function signature (must accept group, nearby, holes).

Deviations from any of the above must stop the loop and require user
sign-off.

## Reporting

Every commit message references its task # and the gate status:

```
<scope>: <subject>

Task #<n>: <task title>
Gate: <pass | partial-pass + open items | fail + diagnosis>

<body>
```

A final summary commit at the end of the autonomous run:
- Final task statuses
- Decisions taken under "agent authority"
- Any gates left red and why
- Pointer to subsequent work
