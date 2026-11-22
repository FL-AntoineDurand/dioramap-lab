---
id: "2026-06-11-d2b-side-vec-wrong-plane-frame"
date: 2026-06-11
title: "D.2b wrong-side test was blind on steep dihedrals: side_vec lived in only one plane's frame"
tags: [boundary-corners, boundary-topology, D.2b, ridge-side, side_vec, bugfix, wire-format, v7]
category: bugfix
status: concluded
refs:
  - "2026-05-17-wrong-side-corner-killing-review"
  - "2026-05-21-boundary-polygons-tsp-rethink"
pipeline: v7-bp2
---

## The bug, in one sentence

The D.2b "wrong-side convex corner" kill test judges a corner of plane **B**
using a `side_vec` that lives in plane **A's** surface — so on a steep
dihedral the test axis is nearly orthogonal to where the corner actually
sits, `side_d` collapses to FP noise, and clearly-wrong-side corners are
kept (or, symmetrically, right-side ones could be killed).

## How we found it

Visual symptom on `v7-bp2` / dahouet: plane **24** keeps four convex corners
(`P24X3..P24X6`) floating ~0.34–0.41 m on the **wrong side** of the ridge it
shares with plane **2**. They are not snapped (just past `snap_radius` 0.30 m)
and not killed.

`BC_TRACE=24` on the D.2b filter showed the side test *running* on the right
ridge and voting "keep":

```
ridge=1 (pA=2 pB=24) perp=0.34m side_d=+0.005 anchor_side=+1 expected=+1 → keep
```

`perp = 0.34 m` but `side_d = +0.005`: the corner is a third of a metre off
the ridge, yet the side projection reads ~zero. That is geometrically
impossible for a correct side axis — a 0.34 m offset must project to ±0.34 on
a unit perpendicular-in-plane vector. So the axis was wrong.

We added a one-line diagnostic to the trace: recompute `side_d` using the
corner's **own** plane tangent, `line_dir × P->normal` (plane 24), instead of
the stored `side_vec`:

```
corner | perp  | side_d (stored, plane-2 frame) | side_d_OWN (plane-24 frame)
P24X3  | 0.34  | +0.005  → keep                 | -0.339
P24X4  | 0.41  | +0.006  → keep                 | -0.413
P24X5  | 0.37  | +0.006  → keep                 | -0.373
P24X6  | 0.41  | +0.006  → keep                 | -0.408
```

In plane 24's own frame `|side_d_OWN| ≈ perp` to the millimetre and the sign
is **negative** — the opposite of expected. The corners are genuinely
wrong-side; the stored test simply could not see it.

## Root cause

`v7-boundary-topology-impl.c` computes, per ridge line between planes A and B:

```c
side_vec = normalize(line_dir × normal_A);     // plane A's normal ONLY
side_sign_a = sample_side(A's points, side_vec);
side_sign_b = sample_side(B's points, side_vec);   // ← still A's frame
```

`side_vec = line_dir × normal_A` is perpendicular to the ridge **and lying in
plane A's surface**. For a corner of plane A that is exactly the right axis.
For a corner of plane **B**, its real offset lives in plane B's surface, and
the angle between "perp-to-ridge in A" and "perp-to-ridge in B" **is the
dihedral angle** between the two planes. Plane 2 and plane 24 meet steeply, so
`side_vec` is ~89° off plane 24's offset direction and the projection
collapses to the noise floor. Both `side_sign_b` *and* the corner's measured
side are sampled against the same wrong-frame axis, so the test is
self-consistent but **blind** — not flipped, just unable to resolve the sign.

The single `side_vec` + two `side_sign` fields are an artifact: a raw cross
product gives the right *axis* but an arbitrary *orientation*, so a sign was
stored to say "which end is the interior". Deriving that one axis from A's
normal forced both planes to be described relative to A's frame.

## History — this is original sin, dormant then unmasked

Archaeology (git `-S`/`-L` across both repos):

| date | commit | event |
|------|--------|-------|
| 2026-04-21 | v2 `feb553e` | side-sign wrong-side test **born**: `sideVec = cross(direction, eqA.normal)`, `sideSignA/B` both sampled against it. Single-vector-two-signs from day one. |
| 2026-04-25 | v2 `0f35b9f`, `c6ee72d` | extracted from `boundary-3d` into `boundary-edges/ridges.js` + `boundary-corners.js` (refactor, logic unchanged) |
| 2026-05-11 | v3 `c29e35c`, `e185354` | **ported to v3 C unchanged** — `side_vec = line_dir × seeds[a].normal` is a line-for-line port; D.2b ported too |
| 2026-05-16 | v3 `d0a634a` | `has_side` flag-bit fix — *before this, D.2b silently skipped every ridge* (wire `has_side` was always 0), so the frame bug was **invisible** in v3 from the port until here |
| 2026-05-17 | journal `2026-05-17-wrong-side-corner-killing-review` | deep review of D.2b found **two** failure modes (segment-range; courtyard parallel ridges) but **missed this one** — it explicitly assumed `side_vec` was "R's outward-from-plane-interior unit vector" valid for both planes |

Key findings:

- **The per-plane (`eqB`/`normal_b`) form never existed in any revision** of
  either codebase (`git log -S "eqB.normal"` → empty). This is original sin,
  not a regression.
- **v2 has the bug too** and *live*: v2's D.2b reads JS object fields directly
  (no `has_side` wire bit), so its wrong-side test has been running — and
  mis-judging steep-dihedral corners — since 2026-04-21.
- In v3 the bug was **double-masked**: dormant until `d0a634a` made D.2b run
  (2026-05-16), and then mis-attributed — the 2026-05-17 review caught the
  *symptom class* ("wrong-side corners survive") but pinned it on distance
  gates, not the frame.

## Why it is safe to fix now

The 2026-05-17 review deferred *all* D.2b changes "until v3 reaches v2 byte
parity", to preserve v2 as the comparison ground truth. That constraint was
**lifted on 2026-05-21** when v2-byte-parity was abandoned (see
[[2026-05-21-boundary-polygons-tsp-rethink]] — stability-under-perturbation
replaced byte-parity as the success criterion). v3 is now free to be *correct*
rather than bug-compatible with v2.

## The fix — per-plane oriented vectors, drop the signs

Replace `side_vec[3] + side_sign_a + side_sign_b` with two **interior-oriented**
unit vectors and two validity bits:

```c
side_vec_a = normalize(line_dir × normal_a);
if (mean over A's near-ridge pts of side_vec_a·(p−line_point) < 0) side_vec_a = −side_vec_a;
// side_vec_a now points from the ridge INTO plane A's body; same for side_vec_b with normal_b.
```

The kill test for a corner of plane P collapses to one sign — no `expected`,
no two-sign comparison:

```c
d = side_vec_P · (corner − line_point);   //  d < 0  → wrong side → kill
```

`has_side_{a,b}` (cross-product non-degenerate AND a definite interior sign
found) replaces the `side_sign == 0` state — no information lost.

Wire record `rg_ridge_line_record_t` goes 88 → **96 B** (drop 12 B vec + 2
signs + 2 pad; add two 12 B vecs; two validity bits move into the existing
`flags` word → zero padding).

### Change set / version bumps

| file | change | version |
|------|--------|---------|
| `v7-roof-graph-format.h` | record: `side_vec_a/b`, flag bits 1/2 | wire format |
| `v7-boundary-topology-impl.{h,c}` | struct + compute two oriented vecs (sample-and-flip per plane) | topology `.version` 4→5 |
| `v7-boundary-topology.c` | encoder packs two vecs + flags | (same step) |
| `v7-boundary-corners-impl.{h,c}` | input struct; D.2b picks `side_vec_P`, kill iff `dot < 0` | corners `.version` 8→9 |
| `v7-boundary-{corners,merge,polygons}.c` | three decoders unpack two vecs | merge 3→4, polygons 3→4 (record bytes changed) |
| `*-bench.c` (corners/merge/polygons) | copy two vecs through | — |
| `v7-bench-v2-loaders.c` | synthesize `side_vec_a/b` from `line_dir × normal_{a,b}`, orient via v2's `side_sign_a/b` | — |

This also closes (or reframes) two of the 2026-05-17 open questions: the side
test is no longer a magic-distance heuristic but a correct per-plane geometric
test; the `ridge_side_max_dist` cap remains only as the courtyard guard
(failure mode 2), not as the thing standing between "blind" and "correct".

## Results

Implemented the per-plane model end to end:

- **Wire record** `rg_ridge_line_record_t` 88 → **96 B** (`_Static_assert` updated);
  `side_vec[3]` + `side_sign_a/b` + 2 pad → `side_vec_a[3]` + `side_vec_b[3]`,
  validity in `flags` bits 1 (`has_side_a`) and 2 (`has_side_b`).
- **Topology** computes two interior-oriented axes via a new `compute_side_axis`
  helper (cross product → normalize → sample-and-flip toward the plane's body;
  returns false if degenerate or no definite interior). `.version` 4 → 5.
- **D.2b** picks `side_vec_{a|b}` by whether the corner's plane is `plane_a` or
  `plane_b`, gates on the matching `has_side_*`, and kills iff
  `side_vec_P · (corner − line_point) < 0`. `expected`/two-sign comparison and
  the temporary `side_d_OWN` diagnostic are gone. `.version` 8 → 9.
- Decoders (corners/merge/polygons + their `-bench` variants), `-bench` copies,
  the D.2b unit test, and the v2 loader all migrated. Merge/polygons `.version`
  3 → 4 (record bytes changed). The v2 loader folds v2's A-frame `sideVec` and
  `sideSignA/B` into `side_vec_a = sideVec·signA`, `side_vec_b = sideVec·signB`
  — which makes the new `dot < 0` test **reproduce v2's exact (wrong-frame)
  behavior**, keeping the v2-comparison harness faithful while production gets
  the corrected axes.

**Validation on v7-bp2 / dahouet, plane 24** (`BC_TRACE=24`):

| corner | perp | OLD side_d (plane-2 frame) | OLD fate | NEW side_d (plane-24 frame) | NEW fate |
|--------|------|----------------------------|----------|-----------------------------|----------|
| P24X3  | 0.34 | +0.005 | kept | −0.339 | **KILLED** |
| P24X4  | 0.41 | +0.006 | kept | −0.413 | **KILLED** |
| P24X5  | 0.37 | +0.006 | kept | −0.373 | **KILLED** |
| P24X6  | 0.41 | +0.006 | kept | −0.408 | **KILLED** |

The plane-24 axis on that ridge went `(0.54,−0.47,+0.70)` (plane-2 frame) →
`(0.51,−0.46,−0.73)` (plane-24 frame) — the z-component flips because the axis
now lies in plane 24's surface, not plane 2's.

- **No marginal flips:** the smallest `|side_d|` across *all* plane-24 kills is
  **0.339 m** — ~5 orders above the on-ridge FP gate (1e-6 m). Every kill is a
  decisively wrong-side corner, not a coin-flip.
- **Not a slaughter:** distinct anchors killed on plane 24 went 8 → 15 (the
  whole plane sits on steep dihedrals, so correcting the frame re-decides all
  its corners), but the corners output grew only +480 B ≈ the RIDGE_LINES
  record growth (8 B × ridge count) — net corner count essentially unchanged.
  The fix *corrects which corners die*, it doesn't gut the set.
- **20/20** v7 unit tests pass; the full v7-bp2 pipeline runs clean to
  `boundary-polygons-roof-graph` (14 steps, 0 errors).

## Follow-ups (not done here)

- `v7-boundary-topology.doc.json` (§4.3 wire spec says 88 B / single side_vec)
  and `v7-boundary-corners.doc.json` (D.2b description) are now stale — update
  to the per-plane record. Same class as the ts-weights doc.json drift.
- The 2026-05-17 open questions on the `ridge_side_max_dist` cap (failure modes
  1 & 2: segment-range; courtyard parallel ridges) are still open — the cap now
  guards only those, no longer the line between "blind" and "correct".
- v2 JS still has the live bug; not fixing (v2 is the frozen, parity-abandoned
  reference).
