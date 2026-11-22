---
id: "2026-07-02-corner-insertion-lands"
date: 2026-07-02
title: "In-loop corner insertion lands: implementation + results"
tags: [boundary-regularize, corner-insertion, cost-function, wsd, repair, development, v7]
category: development
status: concluded
refs:
  - "2026-06-29-regularize-corner-insertion-edge-fit-metric"
  - "2026-06-29-corner-slide-runaway-and-wsd-coverage-audit"
  - "2026-06-30-wsd-normalization-deadend-curriculum"
pipeline: v7-bp2
design_doc: "v3/docs/plan/CORNER-INSERTION-IMPL-PLAN.md"
---

## What this is

Realizes the proposal in [[2026-06-29-regularize-corner-insertion-edge-fit-metric]]:
`boundary-regularize` can now **add** corners mid-optimization, not just move/merge/split
them. This is the principled fix for the *under-coverage* failure mode — an upstream
polygon too small to cover its plane's points, which the un-normalized WSD curriculum
could only "solve" by spiking a corner (see
[[2026-06-29-corner-slide-runaway-and-wsd-coverage-audit]]). Prototype quality,
behind the `cornerInsert` param; **not yet committed as production**.

## Architecture — 3 layers

Corner insertion is harder than the make-shared ops (T5–T7) because those only *re-used*
existing corner ids; this **grows** the corner set, and held indices must stay valid.

1. **Optimizer array growth** — `corners_in` was `const` with ~13 corner-parallel arrays
   sized `n_corners`. Rebind to a mutable over-sized copy `corners_owned` of size
   `n_cap = n_corners + N_INSERT_MAX (64)`; over-size every corner-parallel allocation to
   `n_cap` so `n_corners++` on insert never reallocates (a realloc would dangle indices
   held across the iteration). `corner_map` sized `max_corner + 1 + N_INSERT_MAX`.
   Single-point `free(corners_owned)` at the `cleanup:` label (covers all 15 error paths).
2. **Emit** — `corner_positions`/`corner_planes` size to the grown `n_corners`; new
   corners flow to `out->dof`/`n_corner_positions` automatically.
3. **Wrapper serialization** — `reemit_corners` rewrites CORNERS 1:1 from *upstream*
   records; a minted corner has no upstream record and would be dropped → dangling id in
   the polygon path → classify/edges break. Fix: after the 1:1 loop, **append a fresh
   RG_SECT_CORNERS record** for each inserted id (position yup→zup + world shift, single
   plane block, empty ridge block, source tag), mirroring `parse_corners`' layout.

**Fail loud, don't degrade.** Exceeding `n_cap` means the sizing assumption is wrong; a
graceful "stop inserting" would silently ship an under-covered polygon. So it's a FATAL
`fprintf` + `abort()` — NOT `assert()` (the noasan build is Release/NDEBUG → assert
compiles out → silent overflow). Per-plane (≤24) and buffer (64) budgets keep normal
operation far under the cap; the abort is the invariant backstop.

## The insertion op + in-loop sweep

- **Metric** (free decomposition of WSD): per polygon edge,
  `misfit = Σ(owner OUTSIDE charged to edge: +w·d)` via `pt_nearest_edge_uv`. Worst
  edge by misfit/length; `mass_uv` = weighted centroid of that edge's outside points.
- **Placement**: insert one corner at `mass_uv` (a displaced, real kink — a geometric
  midpoint would be 180° collinear and die to collinear-removal). `uv_to_3d` inverts
  `project_uv`; splice into the ring after the worst edge; `RETAG_DOF` gives it DOF-2 +
  in-plane axes; `REBUILD_CTXS`/`REBUILD_PW_EDGES`.
- **In-loop, not static** — the sweep runs every 12 iters (up to 20 before the end): one
  insert per under-covered plane per pass, self-int-guarded (`poly_self_int_uv` on the
  would-be ring), per-plane budget. The optimizer refines each new corner over the next
  iters. This interleaving is **required**: the static sim (`BR_SIM_INSERT`) proved that
  stacking inserts without refinement saturates and self-intersects at ~2 corners.
  Collinear-removal is the GC for inserts that prove useless.

## Results (dahouet bldg0)

Plane 0 — the terrain ring, 4807 points, a 6-vertex polygon covering ~33 %:

| | before | after insertion |
|---|---|---|
| vertices | 6 | 12 (+5–7) |
| points outside | 3203 (67 %) | ~43 (1 %) |
| own_out (Σw·d) | 12364 | ~7.6 |
| mean misfit | 3.98 m | 0.002 m |

Across the building: **8 inserts over 4 under-covered planes**; gate green
(`dead_id_refs=0 edge_orphans=0`), ASan clean (multi-insert + per-insert REBUILD path).

A bonus effect: insertion **partly tamed the id=30 slide spike on its own** (t≈−85 →
−6 m) — covering plane 0 removed the WSD gradient that was driving the slide. But a
residual drift class survived (id=136 / walls) where coverage is already fine and the
corner drifts into *empty* space — that needed a second fix, the marginal-compactness
gate (see the companion entry).

## Answers to the idea entry's open questions

- *Collinear-removal as GC* — confirmed: useless inserts (180° midpoints) are removed;
  we insert at displaced `mass_uv` precisely to survive it where the kink is real.
- *In-loop vs static* — must be in-loop; static saturates at ~2 (sim-proven).
- *Array growth safety* — fixed pre-alloc to `n_cap` (pointer-stable), FATAL abort past it.

## Status / cost

- Param `cornerInsert` (default on); set explicitly in `v7-bp2.json`. Step `.version` 18.
- **Perf**: the per-insert `REBUILD_CTXS`/`REBUILD_PW_EDGES` + per-sweep misfit
  re-measure push the step from seconds to ~60–75 s on dahouet. Acceptable for the
  coverage win; wants profiling before default-on in production.
- **Uncommitted prototype.** Broader-than-dahouet validation still owed.
