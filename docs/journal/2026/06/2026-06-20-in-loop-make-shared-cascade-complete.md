---
id: "2026-06-20-in-loop-make-shared-cascade-complete"
date: 2026-06-20
title: "In-loop make-shared cascade complete: 5-op vocabulary, membership SSoT, T8 close-out"
tags: [boundary-regularize, make-shared, ridge, dof, membership, ssot, simplification, review, v7]
category: review
status: concluded
refs:
  - "2026-06-14-in-loop-make-shared-design"
pipeline: v7-bp2
---

## Summary

The in-loop make-shared refactor designed in
[[2026-06-14-in-loop-make-shared-design]] is **built and landed**. Boundary-regularize
now realises the whole 5-op vocabulary as one in-loop cascade, on a membership
architecture rewritten so the polygon paths are the single source of truth. This entry
is the close-out review: what shipped, the T8 integration/determinism/perf findings, and
the logged follow-ups.

## What shipped (commits on `main`)

| step | op | commit | note |
|---|---|---|---|
| T5 | `MERGE_CORNER` (cross-plane weld) | `5843336` | replaced the legacy R.4; pool-solver meet, type-gate, self-int reject |
| Phase B | membership = paths SSoT | `66c432c` | deleted `planes_dyn`; `planes_of(ci)` derives on demand; R.0 DOF → topological |
| T6 | `DROP_REDUNDANT_ON_RIDGE` | `efd04ed` | all-polys-agree ridge-cleanup; pre-loop + in-loop share one helper |
| L1 | orphan-kill | `9a17c1b` | drop corners left alive but in no path (the 197-class) |
| T7 | `SPLIT_TO_SHARE_CORNER` | `5e33a41` | the only growing op; one fallible path realloc + ctx pre-reserve |

Cascade priority in `CHASE_AFTER_MOVE`: same-plane MERGE → MERGE_CORNER_XPLANE →
**SPLIT** → DROP_REDUNDANT_ON_RIDGE → DROP_COLINEAR → snap-in.

## The architectural turn (Phase B)

The biggest result was not an op but a simplification. Per-corner plane membership was
stored twice — the polygon paths (canonical) and a separately-maintained `planes_dyn`
copy (seeded from `plane_ids`, unioned on merge, reconciled on shrink). A semantic audit
established that membership is a **pure function of the paths**, so the copy was deleted:
`planes_of(corner) = { plane p : corner ∈ polys[p] }`. This dissolved the seed, the
seed-reconcile, the union-up, and `RECONCILE_PLANES_DOWN` — and made the P2⇔P3
consistency invariant **true by construction**. Two facts that fell out of the audit and
are worth keeping (recorded in design-doc §2.4 DECISION UPDATE):

- `plane_ids` (upstream) is **geometric** ("lies on this plane's surface", ≤0.12 m) — a
  superset of path-membership, **not a bug**. It is no longer read anywhere in regularize.
- `ridge_ids` (geometric on-ridge) **cannot** be derived from the paths (a single-plane
  on-ridge corner is on-ridge-true / in-one-polygon) — it stays a separate field.

This dividend is what made T7 tractable: inserting an *existing* corner into a path grows
no membership store, so the design's feared "atomic multi-store transaction" collapsed to
one fallible path realloc + pre-reserving the ctx pool.

## T8 close-out findings (measured on dahouet)

- **Integration — clean.** Every path mutator does id-rewrite + RETAG_DOF +
  REBUILD_CTXS/PW + joint_dirty; no op undoes another (DROP_COLINEAR gated
  `planes_of_n!=1`; T6 all-polys-agree keeps a fresh split); no stat double-counts; env
  gates (`BR_NO_R4`, `BR_NO_SPLIT`, `BR_SNAP_IN`, `BR_DOF_COUPLE`, `BR_CHECK`) orthogonal.
- **Consistency gate GREEN** on a fresh compute: `dead_id_refs=0 P2sup=0 P3sup=0
  edge_orphans=0`.
- **Deterministic.** Two fresh-cache runs → byte-identical regularize output (same
  sha256), identical splits. The historical 3↔4 split-count variance was *upstream input*
  (boundary-classify cache state), not the op.
- **Perf — the cascade is free.** The in-loop make-shared ops total **0.15 ms = 0.0002%**
  of the regularize step; WSD + the R.7 coplanar/collinear metrics dominate ~98%. The
  scan-all-candidates SPLIT is empirically invisible (n_ridges=45, tiny per-corner work vs
  a 200-iteration × ~1.8M-call coordinate descent).
- **M4(b) meet-solve** (≥3-plane junction split) is stub-and-refuse — structurally
  unreachable from T7's single-plane precondition (`sp_junction_refuse=0` always), refused
  not mishandled. Safe to leave; the position math (`v7p_solve_member_position`) already
  exists if it's ever implemented.

## Finding: flashing lines pin DOF (a real inconsistency)

One T7 split landed its shared corner at **DOF-0**. It is mechanically correct — the
corner sits where two creases cross — but the second crease is a **flashing line (type 4)**,
which the make-shared I1 allowlist (`RIDGE_TYPE_ALLOWED` = {ridge,hip,valley}) explicitly
excludes as degenerate. The A2 DOF-coupling (`ridge_dof_for_corner` + seed) is
**deliberately not type-gated** (predates I1), so a flashing line still counts toward
`ridge_dof` and can lock a corner to DOF-0 even though it can't form a shared edge. So the
same "should flashing count?" question is answered *no* by make-shared and *yes* by
DOF-coupling. Candidate fix: type-gate `ridge_dof_for_corner`/the seed to
`RIDGE_TYPE_ALLOWED`. 1-corner effect on dahouet; logged as a low-priority follow-up.

## Status & follow-ups

The cascade is **done**, modulo broader-than-dahouet validation (deferred — dahouet is one
building and barely exercises SPLIT/T6; the synthetic unit fixtures are the real
edge-forming proof). Validated: build 0-warning (Release + ASan), 159 unit checks in both
builds incl. the T7 growth/OOM heap-safety fixture, gate green.

Logged follow-ups (none blocking; this list supersedes the now-retired working doc
`v3/docs/plan/MEMBERSHIP-SSOT-AND-MAKE-SHARED-PLAN.md`):

1. **Broader-than-dahouet validation** — the make-shared cascade is a real behaviour
   change validated on one building + synthetic fixtures; run a set of other buildings /
   the bench before treating it as broadly safe. *(Next topic.)*
2. **Flashing-line DOF type-gate (was L4)** — `ridge_dof_for_corner`/the A2 seed are not
   type-gated, so a type-4 flashing line can pin a corner to DOF-0 (the finding above).
   Candidate fix: gate to `RIDGE_TYPE_ALLOWED`. 1-corner effect on dahouet.
3. **Ridge-tag near-junctions (was L2-residual) — RESOLVED 2026-06-23: NOT A BUG, won't
   fix.** The original framing (a near-junction gets DOF-1 not DOF-0 because the 3rd-plane
   ridge is untagged; "fix" = tag it from proximity) was the WRONG lens — it assumed
   *proximity ⇒ junction*, which is exactly the geometric over-labelling Phase B removed.
   Decision: **DOF follows topology, never proximity.** A corner is locked (DOF-0) only when
   it is genuinely a vertex of ≥3 panels' polygons. If a 3rd panel's facet truly reaches the
   corner, it's make-shared's job to *wire* that polygon onto it → the corner then becomes a
   3-panel vertex and locks **automatically**. If make-shared doesn't wire it (the panel is
   too far — e.g. the dahouet example's 0.62 m > the 0.50 m sharing τ), the corner genuinely
   is NOT that panel's vertex → **slide-only (DOF-1) is the correct answer**. Tagging a crease
   to a non-member panel from proximity would re-introduce the very thing we deleted. The
   system is self-consistent without it. (The only *legitimate* adjacent question is whether
   the 0.50 m make-shared τ is the right cutoff — a make-shared-threshold question, not a
   DOF/tagging one.)
4. **Coplanar-import `plane_ids` looseness (was L3) — RESOLVED 2026-06-24: NOT A BUG.**
   Full audit in [[2026-06-24-coplanar-import-plane-ids-audit]]. The coplanar-import is a
   deliberate sparse-plane signal-recovery feature (a corner born on a well-sampled plane is
   tagged onto an adjacent sparse plane so `boundary-polygons` can borrow it — 61% of
   coplanar labels are used, on genuine sparse facets). Its loose import tag (perp ≤0.12 m +
   balance ≥0.30, no footprint check — there's no polygon yet) is **backstopped by a 0.75 m
   anchor-to-contour gate in the sole consumer `boundary-polygons-2`** (`V7_BP2_CORNER_TO_
   FRAGMENT_RANGE_M`): a corner labelled 1–2 m from the real face fails that gate and is
   never used in a polygon (it stays an inert unused label). Loose-tag-at-import +
   tight-gate-at-consumer is the right shape; the only residual is threshold tuning.
5. **M4(b) meet-solve** — implement for real if a ≥3-plane split target ever arises
   (currently stub-and-refuse, unreachable from T7's single-plane precondition).
6. **Minor naming debt** — legacy `r4_*` stat names persist alongside the new `n_*`.
