---
id: "2026-06-24-coplanar-import-plane-ids-audit"
date: 2026-06-24
title: "Coplanar-import plane_ids: a deliberate sparse-plane signal-recovery feature, backstopped by the 0.75 m contour gate (F4 closed)"
tags: [boundary-corners, boundary-polygons, plane_ids, coplanar-import, membership, dof, finding, v7]
category: finding
status: concluded
refs:
  - "2026-06-20-in-loop-make-shared-cascade-complete"
  - "2026-06-14-in-loop-make-shared-design"
pipeline: v7-bp2
---

## Summary

F4 ("coplanar-import `plane_ids` looseness") — a follow-up logged at the make-shared
cascade close-out — is **resolved as NOT a bug**. The coplanar-import is a deliberate
**sparse-plane signal-recovery** feature, and its deliberately-loose label is
**backstopped by a 0.75 m anchor-to-contour gate in the consumer** (`boundary-polygons-2`).
A corner labelled 1–2 m from a plane's real face never reaches that plane's polygon — it
fails the 0.75 m gate and stays an inert, unused label. This entry records the full audit
(writes / reads / statistics), what the `balance` gate actually measures, and the
two-stage design that makes the looseness harmless.

## Background: F4's original framing, and why it was suspect

The concern (from the Phase B analysis): in `boundary-corners`, the coplanar-import can
tag a corner as belonging to a plane even when it sits **1.29 m / 2.47 m** from that
plane's real roof face (corners 153/163). The worry was a too-loose lateral gate writing
spurious membership.

Two membership concepts (the Phase B distinction this rests on, see
[[2026-06-14-in-loop-make-shared-design]] §2.4):
- **Topological** — "is a *vertex* of this plane's polygon." What `boundary-regularize`
  now uses (single source of truth).
- **Geometric `plane_ids`** — "*lies on* this plane's surface." Upstream, in
  `boundary-corners`. **Coplanar-import writes this one.**

## The intent (what coplanar-import is *for*)

LiDAR coverage is non-homogeneous — some roof planes are well-sampled, others sparse or
obstructed. Coplanar-import lets a **sparse plane borrow a well-sampled neighbour's
corner** when that corner genuinely fits both planes, so `boundary-polygons` can build the
sparse plane's outline using the borrowed corner. It is a **signal-recovery feature**, not
sloppy labelling.

## The audit

**Writes.** The coplanar-import write is `v7-boundary-corners-impl.c:892` (driven by the
second per-plane sweep `:1611-1667`), via the canonical `v7p_entry_register_plane`
(`v7-corner-pool.c:115`). Gates: perpendicular `|sd| ≤ coplanar_dist_threshold` (0.12 m,
`:1632`) **and** the projected (u,v) cell is in-bounds with `bal_field[cell] ≥
coplanar_balance_min` (0.30, `:1643`). The corner is **projected onto the plane** before
registration, and tagged with a distinct source byte `SRC_COPLANAR = 8` (so borrows are
cleanly separable from native membership). **There is no footprint check at import** — and
there *cannot* be one: the polygon does not exist yet at `boundary-corners` time.

**Reads — the decisive finding.** The geometric corner `plane_ids` has **exactly ONE
functional consumer** in the live `v7-bp2` pipeline: `boundary-polygons-2` (per-plane
anchor selection). `boundary-regularize` reads it **nowhere** (Phase B — it derives
membership from polygon paths). `classify` and `wall-detect` only *parse-and-free* the
corner field to walk the record layout; their semantic reads are all of the **edge**
`plane_ids`, never the corner field. So after `boundary-polygons-2`, everything is
topological.

**Statistics (dahouet, v14 build).** The borrowing does real work:
- **59 coplanar labels → 36 used (61 %)** as actual polygon vertices; 23 unused (39 %).
- Used borrows concentrate on **genuine sparse facets**: plane 4 (746 pts, +8 vertices),
  plane 24 (183 pts, **8 of its 18** vertices borrowed), planes 2/3/5, walls 6/11. Several
  polygons would be **materially worse — or fail the ≥3-vertex minimum — without it.**
- The false-ground planes are mostly rejected (0 pts); the payoff is on real facets, not
  the ground pathology.
- The **39 % unused labels are inert** — the sole consumer already had its pass, and
  nothing downstream reads the geometric corner field.

## What `balance` actually measures

Not a naïve proximity check. `balance` is a **boundary-confidence signal** from
`boundary-signals` (`v7-boundary-signals-impl.h:32-33`): it blends the plane's own
group-CoM with the foreign nearby-CoM, weighted by point count, with an
**opposing-direction reward** — high where own-points sit inward and foreign-points sit
outward (the signature of a real plane edge, with support). It is a **coarse 0.10 m raster
proxy, computed at import time before any polygon exists** — the best lateral signal
available then, but fuzzy and grid-limited, and it can read ≥0.30 in cells the eventual
polygon excludes.

## The resolution: the consumer already applies the lateral gate

`boundary-polygons-2` does **not** blindly use every `plane_ids`-tagged corner. In
`v7_bp2_compute` (`v7-boundary-polygons-2-impl.c:866-877`), for each of a plane's
contours it keeps an anchor **only if it is within `V7_BP2_CORNER_TO_FRAGMENT_RANGE_M =
0.75 m` of the contour** (`v7_pt_edge_dist ≤ range`, `:31`/`:873-875`); the surviving
subset becomes the polygon. That 0.75 m anchor-to-contour distance **is the lateral
footprint gate** — it already exists.

So the design is two-stage and sound:
- **Import (boundary-corners):** can't see a polygon yet → casts a deliberately **loose
  wide net** with cheap signals (perpendicular + balance).
- **Use (boundary-polygons-2):** has the real boundary contour → applies the **tight
  lateral gate** (0.75 m).

This directly disposes of the original worry: a corner labelled 1–2 m from the face
**fails the 0.75 m gate → it is never used in a polygon**, ending up as one of the inert
unused labels. The over-labelling cannot distort a polygon because the consumer already
filters it out. (A lateral-distance measurement of the *used* borrows would be moot — they
are ≤0.75 m from the contour by construction, that being the gate that admitted them.)

## Verdict & what remains

**F4 = not a bug.** Loose-tag-at-import + tight-gate-at-consumer is the right shape; the
looseness produces only inert labels. The single remaining question is **tuning** — are
0.12 m (perp) / 0.30 (balance) / **0.75 m** (contour) the right knobs — the same flavour
as the 10 °/coplanar threshold (a knob, not a correctness gap), and not worth touching
without a demonstrated failure.

Process note: an earlier step in this analysis nearly proposed *adding* a lateral check to
`boundary-polygons-2` — it already exists (the 0.75 m gate). The miss came from reading the
anchor-*selection* (`plane_ids == Y`) without the contour-*subset* filter that runs right
after. Recorded so the next reader doesn't repeat it.

**Key anchors:** write `v7-boundary-corners-impl.c:892,1611-1667` (+ thresholds `:99`);
sole read `v7-boundary-polygons-2-impl.c:866-877` (gate const `:31`); regularize non-read
confirmed (Phase B); `balance` def `v7-boundary-signals-impl.h:32`. Related:
[[finding_v7_plane_ids_geometric_not_membership]].
