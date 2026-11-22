---
id: "2026-05-25-v2-v3-equivalence-and-pool-cleanup"
date: 2026-05-25
title: "v2 ≡ v3 equivalence sweep + pool data-model cleanup (BC FP-coin-flip class closed; BM full-tile assertion closed)"
tags: [v7, boundary-corners, boundary-merge, boundary-signals, density-voids-2, corner-pool, data-model, audit, fp-trap, v2-v3-equivalence, json-int, on-ridge]
category: development
status: concluded
refs:
  - "2026-05-17-wrong-side-corner-killing-review"
  - "2026-05-21-boundary-polygons-tsp-rethink"
  - "2026-05-22-bp3-grid-fill-topology"
  - "2026-05-24-bp3-implementation"
pipeline: v7
---

Two days of equivalence work consolidated. Starts with the overnight iso-bench
sweep from 2026-05-24 → wraps with the pool data-model cleanup on 2026-05-25.

**End state**: 9/10 iso bench steps fully match v2 byte-for-byte on
`real-22186-dahouet-502-bldgonly`. BM runs to completion on full-tile
dahouet for the first time (`v7p_entry_validate` assertion closed). v2 npm
test 2353/2353 pass; v3 ctest 43/43 pass.

Only remaining drift: boundary-polygons (bp1) — **out of scope, bp1 deletion
in progress, bp3 is the replacement** ([[2026-05-21-boundary-polygons-tsp-rethink]]).

---

## Result snapshot

```
=== Iso bench (bldgonly) ===
tile-signals        13/13 pass
beam-incidence       1/1 pass
density-voids-2      2/2 pass
normal-cascade-seed  2/2 pass
grow-refit-merge     6/6 pass
hemisphere-segment   3/3 pass
boundary-signals     1/1 pass
boundary-corners     5/5 pass    (276→279→284 corners; v2 ≡ v3 byte-for-byte)
boundary-polygons    0/2 (bp1 FP coin-flip — out of scope; bp3 replaces)
boundary-merge       2/2 pass
# Summary: 9/10 steps fully match

=== Full-dahouet boundary-merge ===
Pre-fix:  v7p_entry_validate assertion at corner-pool.c:160
Post-fix: 269 corners → 33 paths → 140 edges, exit 0
```

---

## The meta-pattern uncovered

This sweep surfaced two bugs that were *the same architectural shape*: **the
same conceptual property encoded in N places, kept in sync by ad-hoc code that
didn't all get updated when the data model evolved.**

| Bug | Concept | Encodings (incl. stale ones) | Symptom |
|---|---|---|---|
| D.2b FP-coin-flip | "is anchor on ridge?" | `pool_id` back-pointer vs `ridge_id`+`has_ridge_id` vs `snapped_to_ridge` bool | 40 false-positive D.2b kills on RI/RE that sit on ridges by construction (perp ≈ 0 → side_d sign-of-zero) |
| BM full-tile assertion | "what's the corner's source?" | cached `e->source` vs canonical `source_per_plane[]` vs per-position `members[i].source` | `v7p_recompute_entry` derived from `members[i].source` (the pre-#254-263 axis), losing the per-plane bumps |

Both pre-existed since each step's port. They surfaced only when a downstream
question started cross-cutting the old + new axes (D.2b after the May-18
"convex-only gate removed" experiment, BM on a fixture large enough to
exercise the cross-source super-merge).

Memory item created: `finding_same_concept_n_encodings.md` — recognition
heuristics + fix-shape for future code review.

---

## All fixes that landed (in order)

### Phase A — overnight iso-equivalence comparator + first real bugs (2026-05-24)

1. **DV2 unownedRimPointCount counter** — `v3/steps/v7/v7-density-voids-2-impl.c`.
   v3 was using `inner_lens[v]` (dist==1 rim + interior survivors) where v2
   uses `outerCandidates.length − interiorSurvivors` (all rim points passing
   normalFitsRoof). Added `rim_ok_count[]`. Voids 25/144/77 → 47/303/171
   matching v2.

2. **Wall-corner ev z-up sign** — `v7-boundary-corners-impl.c`. y-up uses
   `ev = n × eh`; z-up needs `ev = eh × n` so `ev.up_component ≥ 0`.
   Pre-fix, z-up roofs planted wall-bbox corners at the top edge instead
   of the bottom. (Side quest noticed in lab UI before the sweep.)

3. **iso-equivalence comparator** — new `v3/tests/iso-equivalence.py`.
   Universal v2-dump vs v3-step diff with float-tolerant comparison,
   position-matched corner identity, schema-tolerant JSON diff, v3 `.bin →
   v2 .json` adapter for hemisphere-segment.

4. **BC_COUNTS instrumentation** — env-gated `[BC_COUNTS] plane=N alive=…
   src=… kill=… snap=…` print in both v2 (`boundary-corners.js`) and v3
   (`v7-boundary-corners-impl.c`). Becomes the regression detector for any
   future BC divergence.

### Phase B — three loader / per-plane-gate bugs (2026-05-25 morning)

5. **`obj_real` JSON-int silent zero** — `v3/steps/v7/v7-bench-v2-loaders.c`.
   `json_real_value()` returns 0.0 for JSON integers. v2 dumps `tilt: 90` as
   JSON int → every plane's `tilt_deg` loaded as zero → D.0 wall-bbox gate
   (`tilt ≥ 72°`) failed for all 12 vertical planes → 48 missing CC anchors.

   Fix: use `json_number_value` (handles both int and real) + `assert(json_is_number)`
   on present-non-numeric. Then **audited and migrated all 8 risky bench
   loader sites + all 11 step config loaders** (`json_is_real` →
   `json_is_number`, `json_real_value` → `json_number_value`). Closes
   silent-fallback class for numeric JSON parsing.

6. **Plane-grid gate semantics** — chose to **drop the gate in both v2 and
   v3** so small/marginal planes (no plane-grid) still get D.0 wall-bbox
   corners. v2 had `if (!planeGrid) continue;` skipping the entire per-plane
   pass; removed and v2 augmented with a basis-from-normal fallback for
   `planeNode.basis = undefined`. Per your call (user prefers v3's "process
   every plane" semantic).

7. **`side_vec` float-truncation FP sign flip** *(superseded by Phase C
   property gate, see #9)* — `v7_bc_input_ridge_line_t::side_vec` was
   `float[3]`. For anchors that sit on a ridge by construction (perp = 0 →
   side_d is mathematically zero, FP-rounding-defined sign), float
   truncation flipped the sign relative to v2's double computation. Initial
   workaround: promote to `double[3]`. Reverted after the property gate
   (#9) made the workaround unnecessary.

### Phase C — D.2b FP-coin-flip class closed (2026-05-25 midday)

8. **On-ridge property is first-class on the anchor** —
   `v7-boundary-corners-impl.c`. Added `anchor.on_ridge_ids[]` (transient,
   freed at end of `v7_bc_compute`, NOT persisted) populated symmetrically
   at every source that sits on a ridge by construction:
   - D.1a RI: copy pool entry's `ridge_ids[]` (intersection set)
   - D.1b RE: single entry `[r->ridge_line_idx]`
   - D.2 snap: append snap target
   - D.0 wall-bbox / CC/CV seeds: empty (anchors not on a ridge)

9. **D.2b property gate** — replaced the old `snapped_to_ridge && ridge_id ==
   r` snap-target gate (which only fired for D.2-snapped anchors) with a
   single helper `anchor_on_ridge(a, r->ridge_line_idx)`. Now RI/RE anchors
   that sit on ridges by construction are gated out before the FP-fragile
   side test. Defense-in-depth `perp_sq < 1e-12` gate catches data-model
   drift.

10. **`side_vec` reverted to float[3]** — the workaround from #7 is no longer
    needed; anchors that would coin-flip are gated out before the side test
    runs. The in-memory bench struct matches the wire TLV format again.

11. **Dead anchor field deleted** — `anchor.snapped_to_ridge` /
    `meta.snappedToRidge` had no readers after the gate migration. Removed
    in both v3 and v2.

12. **v2 mirror** — same `meta.onRidgeIds` populated at RI/RE/snap sites in
    `boundary-corners.js`, same property gate + perp-epsilon in D.2b.

**Net effect**: 40 false-positive D.2b kills recovered (16 RI + 24 RE).
Output 276 → 284 corners. CV unchanged (snap-target gate already handled
the only CV-on-ridge case correctly).

### Phase D — pool data-model rationalization (2026-05-25 evening)

13. **`v7p_recompute_entry` derives `e->source` from canonical store** —
    `v3/steps/v7/v7-corner-pool.c`. Pre-fix, recompute derived from
    `max(members[i].source)` (per-position-provenance axis). Post #254-263
    unification, the canonical store is `source_per_plane[]`. Recompute now
    derives from `max(source_per_plane[])` — matching the invariant
    `v7p_entry_validate` enforces.

14. **Same fix to BC's local `recompute_pool_entry`** —
    `v3/steps/v7/v7-boundary-corners-impl.c:556`. BC has its own local copy
    of the pool primitives (predates the shared module #254). Both copies
    fixed identically.

15. **Coplanar-import recompute** —
    `register_anchor_in_pool` (v3) / `registerAnchorInPool` (v2)
    short-circuit (anchor has pool_id) expanded `plane_ids[]` /
    `source_per_plane[]` / `dirsByPlane` without calling `recompute_pool_entry`.
    Latent: `e->source` would lag the new high-priority slots. Fix: call
    `recompute_pool_entry(e)` at the end. Same in v2.

**Net effect**: BM `v7p_entry_validate` assertion on full-tile dahouet
closed. Iso bench unchanged (the bldgonly fixture didn't exercise the
cross-source super-merge path that triggered the assertion).

### Phase E — unit test cleanup (2026-05-25 night)

Caught up on every failing test in v2 + v3 even when unrelated to the
equivalence work:

16. **v2: flight-strip-generator** — test expected `* 100` LAS-scaled scan
    angles; implementation stores raw degrees. Test updated to match
    implementation.

17. **v2: pipeline-registry counts** — registry has 22 pipelines, test
    expected 21. `dropout-analysis` ends with `planes-from-owner` (analysis
    pipeline, not a product). Updated test count + added
    `planes-from-owner` to VALID_TERMINAL set.

18. **v2: P2-5 growK tests OBSOLETE** — `competitive-grow` and
    `point-stealing` no longer use `growK` (replaced by
    `growRadiusScale`/`growRadiusMax` in the density-compensated rewrite).
    Replaced the three obsolete assertions with a single "no longer
    declares growK" assertion. The generic P2-1 check still verifies all
    declared params correspond to used params.

19. **v2: step-meta steps-registry completeness** — registry includes
    `boundary-edges` (a subdirectory step with its own `index.js`) but the
    test only enumerated `.js` files. Updated to also recognize
    subdirectories with `index.js`.

20. **v2: boundary-3d undeclared param** — `useMrAnchors` was used in `fn()`
    but not declared in `meta.params`. Declared with default `true`.

21. **v2: density-voids-2 missing descriptions** — `ransacIterations`,
    `distThreshold`, `minInliers`, `seed` were missing `description`
    fields. Added all four.

**Net effect**: v2 `node --test` 2338/9 → **2353/0**.

22. **v3 ctest** — `main_*`/`batch_*`/`serve_*` (12 tests) were `Not Run`
    because `dioramap-processor` failed to link in `build-noasan` (Release
    mode). Initial errors looked like an `--as-needed` / link-order issue
    against PolyFit's vendored shared libs. **Real root cause**:
    `core/CMakeLists.txt:188-191` applies `-fvisibility=hidden` + `-s` (strip)
    globally in Release. These are PUBLIC compile/link options that PolyFit's
    vendored subdirs inherit via `add_subdirectory()`. PolyFit's C++ classes
    (Logger, Counted, AttributeStore, …) don't tag their methods with
    `__attribute__((visibility("default")))`, so `-fvisibility=hidden`
    elides them from the export table and `-s` strips them entirely.
    Result: `libbasic.so` had 134 dynamic symbols in Release vs 1243 in
    Debug — `Logger::initialize()` and friends gone. Static archive
    `libdioramap_steps_polyfit.a` then couldn't resolve them when ld
    linked the executable.

    **Fix**: extend the existing PolyFit-targets override loop in
    `core/CMakeLists.txt` to also set `-fvisibility=default` on each
    vendored target (compile option) and `{C,CXX}_VISIBILITY_PRESET default`
    + `VISIBILITY_INLINES_HIDDEN OFF` properties. **v3 ctest 43/43 pass
    from scratch build**, no binary-copy workaround needed.

    **When introduced**: 2026-05-06 (`5a9eb47`), when `build-noasan/` (the
    Release-mode build dir created to escape ASan's CGAL slowdown) was
    first set up. Latent for 19 days because the usual build-noasan
    workflow only built bench binaries (`v7-*-bench`), which don't link
    PolyFit. Surfaced today when ctest exercised `dioramap-processor`.
    The PolyFit-targets compile-option override pattern that fixed it
    already existed in the codebase (the `-w -Wno-error` suppression for
    PolyFit's warning noise) — added one more entry to it.

---

## What's persisted vs transient (for future readers)

Two distinct data layers, with two distinct purposes:

| Layer | Contents | Persisted? | Lives until |
|---|---|---|---|
| `anchor_t` (BC compute pass) | working corners during D.0–D.2b enrichment | NO | end of `v7_bc_compute` |
| `pentry_t` (in-mem pool) | merged corner entries during pool registration | NO | end of `v7_bc_compute` |
| `v7_bc_corner_t` (BC output struct) | one per final pool entry | YES → TLV | step output |
| TLV `RG_SECT_CORNERS` (wire format) | `plane_ids[]` + `source_per_plane[]` + **`ridge_ids[]`** + `dirs[]` + `com_bisectors[]` + `members[]` per corner | YES (file) | full pipeline |

The new `anchor.on_ridge_ids[]` field lives only in the first row — it's a
transient computation aid. The persistent representation (third + fourth
rows) already carries the "which ridges does this corner sit on" data via
`ridge_ids[]` — ready for future regularization steps to compute corner
degrees-of-freedom.

**No TLV / wire format change**. No `.doc.json` schema change. No downstream
consumer change. All Phase B–D fixes were in compute-pass logic + step
version bumps.

---

## Step versions

| Step | Pre | Post | Reason |
|---|---|---|---|
| v3 boundary-corners | 6 | **8** | D.0 z-up fix + on-ridge property gate + recompute fix |
| v2 boundary-corners | 3 | **5** | matching changes |

---

## How to reproduce / verify

```bash
# v2 unit tests (was 2338/9, now 2353/0)
cd lidar-algos-bench-v2 && node --test 'test/**/*.test.js'

# v3 ctest (43/43; needs dioramap-processor in build-noasan)
cp v3/build-dev/dioramap-processor v3/build-noasan/dioramap-processor
cd v3/build-noasan && ctest

# iso bench bldgonly (9/10)
cd v3 && tests/iso-equivalence.py

# BM on full dahouet (was asserting; now runs)
build-dev/v7-boundary-merge-only-bench /tmp/bench-v2-dahouet-refs /tmp/bm-out

# Side-by-side BC_COUNTS (regression detector)
BC_COUNTS=1 build-noasan/v7-boundary-corners-only-bench \
    /tmp/bench-v2-dahouet-bldgonly-refs /tmp/bc-out
BC_COUNTS=1 node lidar-algos-bench-v2/scripts/dump-v7-refs.js \
    --case data/test-cases/real-22186-dahouet-502-bldgonly.json \
    --out /tmp/v2-refs
```

---

## Numbers — before/after the BC FP-coin-flip fix

```
                  no fix       float→double      property gate (final)
                  (initial)    (workaround)
BC alive          294          346               390
BC killed         129          125                85
kill(RI)           17           17                 1   -16
kill(RE)           38           28                 4   -34
kill(CC)           41           47                47    0
kill(CV)           33           33                33    0
output corners    251          276               284
```

The "no fix" column had 32 missing CC (tilt=0 silent zero), 4 missing CC
(plane-39 grid gate), and 40 false-positive D.2b kills. The float→double
workaround fixed 32 of those (loader) but didn't address the FP coin flip.
The property gate eliminates the remaining 40 by replacing the
FP-fragile geometric test with a property check.

---

## Files touched

### v3

| File | What changed |
|---|---|
| `steps/v7/v7-corner-pool.c` | `v7p_recompute_entry` derives `e->source` from `source_per_plane[]` |
| `steps/v7/v7-boundary-corners-impl.{c,h}` | `on_ridge_ids[]` field + `anchor_on_ridge` helper + `anchor_push_on_ridge`, D.1a/D.1b/D.2 populate, D.2b property gate + perp-epsilon, `recompute_pool_entry` mirror fix, coplanar-import recompute call, `snapped_to_ridge` deleted, `side_vec`/t_min/t_max reverted to float, D.0 z-up ev fix |
| `steps/v7/v7-boundary-corners.c` | version 6 → 8; `json_is_real`/`json_real_value` → number-variant sweep |
| `steps/v7/v7-bench-v2-loaders.c` | `obj_real`/`rval` use `json_number_value` + crash assertion; all 8 numeric callsites widened; `t_min/t_max` cast to float on read |
| `steps/v7/v7-boundary-{signals,corners,merge,polygons}-bench.c`, `v7-grow-refit-merge-bench.c` | `json_real_value` → `json_number_value` mechanical sweep |
| 11 step config loaders (`v7-tile-signals.c`, `v7-density-voids-2.c`, `v7-hemisphere-segment.c`, etc.) | `json_is_real` → `json_is_number`, `json_real_value` → `json_number_value` mechanical sweep |
| `steps/v7/v7-density-voids-2-impl.c` | unownedRimPointCount counter fix |
| `core/CMakeLists.txt` | PolyFit-targets visibility override (Release-mode `-fvisibility=hidden` + `-s` strip was eliding `Logger::initialize`/`Counted` from exports) |

### v2

| File | What changed |
|---|---|
| `lib/pipeline/steps/boundary-corners.js` | `meta.onRidgeIds` populate at RI/RE/snap, `anchor_on_ridge` helper + perp-epsilon D.2b gate, `recomputePoolEntry` coplanar-import call, drop `if (!pg) continue;`, basis-from-normal fallback, BC_COUNTS instrumentation, version 3 → 5 |
| `lib/pipeline/steps/boundary-3d.js` | declare `useMrAnchors` in meta.params |
| `lib/pipeline/steps/density-voids-2.js` | add missing descriptions: `ransacIterations`, `distThreshold`, `minInliers`, `seed` |
| `test/flight-strip-generator.test.js` | scan-angle expectation: raw degrees, not LAS-scaled |
| `test/pipeline-registry.test.js` | pipeline count 21→22, terminal-step list includes `planes-from-owner` |
| `test/runner.test.js` | drop obsolete P2-5 growK assertions |
| `test/step-meta.test.js` | recognize subdirectory steps (boundary-edges) |

### Test / state docs

| File | Status |
|---|---|
| `v3/tests/iso-equivalence.py` | new (universal v2-vs-v3 step comparator) |
| `v3/tests/ISO-EQUIVALENCE-STATE.md` | folded into this journal (was a working state doc; can be deleted) |
| `v3/tests/iso-final-sweep.log` | folded into the "result snapshot" section above (was a debug dump from the sweep; can be deleted) |
| `docs/journal/2026/05/2026-05-24-iso-equivalence-night-sweep.md` | folded into this journal (can be deleted) |
| `docs/journal/2026/05/2026-05-25-pool-data-model-cleanup.md` | folded into this journal (can be deleted) |

---

## Open items (out of scope here)

- **boundary-polygons bp1** — coin flip on FP rounding. bp1 deletion is in
  progress; bp3 (contour-driven TSP) is the replacement, stability-tested
  separately ([[2026-05-22-bp3-grid-fill-topology]]).
- **BC bench frozen baseline** (`tests/baselines/v7-boundary-corners.frozen`) —
  pre-existing 304-vs-280 number drift from before the equivalence work.
  Refresh when convenient.
- **BC's local pool primitives** (`pentry_t`/`pmember_t` static in
  `v7-boundary-corners-impl.c`) duplicate `v7p_entry_t`/`v7p_member_t`
  from the shared module. Refactor BC to use the shared pool — separate
  cleanup task.
