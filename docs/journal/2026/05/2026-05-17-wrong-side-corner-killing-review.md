---
id: "2026-05-17-wrong-side-corner-killing-review"
date: 2026-05-17
title: "Wrong-Side Corner Killing (D.2b) — Two Failure Modes, A Patch on a Symptom"
tags: [boundary-corners, D.2b, ridge-side, anchors, design, open-question, v7]
category: review
status: concluded
refs:
  - "2026-04-14-roof-graph-prepass-boundary-3d"
  - "2026-03-31-boundary-next-gen-plan"
  - "2026-03-29-plane-geometry-approaches"
pipeline: v7
---

## Context

`v7-boundary-corners` (formerly `boundary-3d`) ingests convex/concave anchor
candidates from `v7-boundary-signals` and merges them with ridge-intersection
corners from `v7-boundary-topology`. Phase **D.2b** is a defensive filter:
after D.2 has tried to snap anchors onto nearby ridges, D.2b sweeps remaining
unsnapped **convex** anchors against the ridges of their plane and kills any
that sit on the "wrong side" of a ridge — i.e. opposite the plane's interior
relative to that ridge.

Current implementation (v3 + v2, unchanged after this review):

```text
for each plane g:
  for each unsnapped convex anchor A on plane g:
    for each ridge R of plane g:
      perp_to_infinite_line = perpendicular dist from A to the infinite line of R
      if perp_to_infinite_line > RIDGE_SIDE_MAX_DIST (1.5 m):  continue
      side_d = (A - R.point) · R.side_vec
      if sign(side_d) != +1:  KILL A
```

`side_vec` is R's outward-from-plane-interior unit vector (computed in
`v7-boundary-topology`). A correctly-placed convex on plane g should satisfy
`side_d > 0` (it lies on the plane-interior side of R).

## Recent fixes that landed before this review

These were real bugs, not design issues — fixing them unblocked the analysis:

1. **`has_side` flag bit silently zero** (commit d0a634a) — topology was emitting
   `RIDGE_LINES.flags = has_line ? 0x01 : 0`, never setting bit 0x02 (`has_side`),
   so the D.2b reader's gate `if (rl->flags & 0x02)` was always false. Every
   ridge's side test was silently skipped. Root cause of dahouet's `7 shared
   edges` instead of v2's `11`/`14`.
2. **D.2b snap-target FP coin flip** (commit 1ea4570) — when an anchor was
   snapped onto a ridge in D.2, it sits *on* the ridge by construction, so
   `side_d ≈ 0` and FP rounding chooses the sign. Fix: skip the side test if
   the ridge being evaluated is the anchor's snap target.
3. **Flag-condition asserts** (commit e692c24) — added
   `assert(rl->has_line)` at 6 consumer sites where logic depends on a flag,
   to prevent future "silent skip when flag missing" bugs of the same shape.

Discussion below is about the *design* of D.2b, independent of these bugs.

## Two real failure modes of the current filter

**Failure mode 1 — segment range is ignored.**
`perp_to_infinite_line` measures distance to R's INFINITE line, not its
segment `[t_min, t_max]`. A ridge running along axis A is perpendicular-close
to anchors that lie *along the same axis* but far past the ridge's actual
extent. Example: a long roof spine ends 5 m before a small ell-feature
notches into the plane; an anchor at the notch is perpendicular-close to the
spine's infinite line, even though the spine segment doesn't reach the notch.
Currently this anchor can be killed by the spine's side test, which is
geometrically meaningless because the spine isn't part of the boundary at
that location.

**Failure mode 2 — courtyard-scale parallel ridges in a single plane.**
A single plane may have a U-shaped or frame-shaped (closed-ring) boundary —
this happens whenever roofs surround a courtyard or a notch. Such a plane
has parallel ridges on *both* sides of the void:

```text
    ┌──── ridge N (north arm)
    │
    │           (plane interior — courtyard)
    │
    └──── ridge S (south arm, same plane!)
```

A convex anchor near ridge N is, in plane-interior terms, on the **wrong
side** of ridge S (the south ridge's `side_vec` points southward away from
plane interior; the north anchor is far north of ridge S, i.e. opposite
that vector). Without a distance cap, ridge S would kill the north anchor —
yet ridge S has no structural relationship to anchors on the north arm.

These two failure modes are independent: (1) is about distance ALONG the
ridge direction, (2) is about wide planes where opposite sides of the
interior have legitimate but unrelated boundary edges.

## Three gates we considered

| Gate | What it catches | Catches mode 1? | Catches mode 2? |
|------|-----------------|-----------------|-----------------|
| Max distance cap (`RIDGE_SIDE_MAX_DIST = 1.5 m`) | Far-away ridges of any geometry | partial (only if perp is also large) | yes (courtyard width usually ≫ 1.5 m) |
| Segment distance (clamp `t` to `[t_min, t_max]`) | Ridges whose SEGMENT doesn't reach the anchor's locality | yes | no (parallel ridges overlap in `t` range) |
| Outward-direction relevance (use anchor's `com_bisector_by_plane`) | Ridges that aren't in the anchor's "where is the plane interior" direction | partial | **no** — fails on U-shape interior corners (see below) |

**Outward-direction gate — pros and cons** (open question, not decided):

- *Pro:* it's a structural test, not a magic distance. A ridge that's not
  in the anchor's outward direction is provably not part of the boundary
  this anchor was emitted to describe.
- *Pro:* defends against pathological cases the distance cap can't see —
  e.g., an anchor very near a ridge that bounds a DIFFERENT face of the
  same plane (across a thin neck).
- *Con (and this is the killer):* fails on U-shape interior corners. The
  inside of one arm has an outward direction pointing toward the OTHER arm
  of the U — so the other arm's ridge passes the outward gate, even though
  it's not the relevant boundary edge.
- *Con:* relies on `com_bisector` quality, which is itself a heuristic
  derived from the plane's CoM and the anchor's edge tangents; noisy on
  small or skinny plane fragments.
- *Verdict TBD:* outward-direction *might* be a useful third gate stacked
  on top of segment-distance + max-distance, but it's not a *replacement*
  for either, and the U-shape failure mode makes it dangerous as a sole
  gate.

## What the "fix" would look like (NOT applied — see "Why we're not fixing this")

```c
// Phase D.2b — relevance + side test
for each ridge R of plane g:
    // 1. Project anchor onto R's parametric axis, CLAMP to segment range
    double t = (A - R.line_point) · R.line_direction
    double tc = clamp(t, R.t_min, R.t_max)
    double closest_pt_on_segment = R.line_point + tc * R.line_direction
    double dist_to_segment = |A - closest_pt_on_segment|

    // 2. Local-neighborhood gate (defends against courtyard mode 2)
    if dist_to_segment > RIDGE_SIDE_MAX_DIST:  continue

    // 3. (Optional, TBD) outward-direction gate — stacked, NOT replacing the above
    // if (-A.com_bisector_by_plane[g]) · (closest_pt_on_segment - A) < 0:  continue

    // 4. Side test (unchanged)
    if sign((A - R.line_point) · R.side_vec) != +1:  KILL
```

Distinguishing rename: `RIDGE_SIDE_MAX_DIST` would be reframed in comments as
*"local-neighborhood radius beyond which ridge-side relationships within a
single plane are not meaningful constraints on each other"* — a guard
against courtyard-scale parallel ridges of the same plane, not a magic
distance threshold for "near a ridge".

## Why we're not fixing this — and why we should think bigger anyway

**Why we're not fixing it (yet):** v3 (C port) is mid-migration from v2 (JS
reference). The dahouet `7 → 13 shared edges` recovery this week came from
making v3 *match* v2's behavior (the topology has_side bug), not from
diverging. Until v3's v7 pipeline is byte-identical to v2 across the full
test corpus, every "local improvement" in v3 introduces a regression risk
and breaks our only ground-truth comparison. **Both v2 and v3 will keep the
current `perp-to-infinite-line + 1.5 m cap` formulation.**

**The bigger concern, worth stating clearly:** D.2b is a corner-*killing*
step. It exists because upstream geometry extraction (boundary-signals'
gradient-peak detection, hemisphere-segment plane partitioning,
grow-refit-merge) sometimes emits spurious convex anchors on the wrong side
of a ridge. Killing them after the fact is treating a symptom.

A better architecture would have upstream steps prefer-or-produce the right
corners directly:
- **boundary-signals** could weight convex candidates by their consistency
  with the plane's CoM/interior (anchors on the wrong side score low and
  never reach D.2b).
- **boundary-topology** could emit ridge-intersection corners with priority
  over signal-derived anchors in their neighborhood (so even when a signal
  anchor lands wrong, the ridge-intersection corner is preferred).
- **boundary-corners D.2** could be more aggressive about snapping (larger
  `SNAP_RADIUS`, or anisotropic snap that respects ridge direction), so the
  "remaining unsnapped convex" set that D.2b operates on is much smaller and
  more obviously wrong.

In other words: the real fix may not be a smarter D.2b at all — it may be
making D.2b obsolete by emitting fewer false anchors upstream. Worth a
proper experiment after v3 reaches v2 parity, before pouring more effort
into the relevance gate.

## Open questions to revisit post-v2-parity

1. Should `RIDGE_SIDE_MAX_DIST` use segment distance instead of
   infinite-line distance? (Failure mode 1.)
2. Should there be an outward-direction relevance gate stacked on top, and
   how do we handle the U-shape false-positive? (Failure mode 2 + open
   question.)
3. Should D.2b exist at all, or should we redesign upstream so wrong-side
   anchors never reach it? (Strategic — the meta-critique above.)
4. Quantify: on the bench corpus, how many anchors does D.2b currently
   kill, and what's the false-positive (legitimate corner killed) rate?
   Without this we're optimizing blind.

## Decision

- v3 D.2b code stays aligned with v2 D.2b code. No segment-distance change,
  no outward-direction gate.
- The discussion captured here is referenced from
  `v7-boundary-corners.doc.json` (planned per the .doc.json schema work in
  task #278) via a `related: [{type: "journal", id: "2026-05-17-..."}]`
  cross-reference, so the lab UI surfaces the design context next to the
  step's parameter list.
- Revisit after v3 byte-parity with v2 is achieved.
