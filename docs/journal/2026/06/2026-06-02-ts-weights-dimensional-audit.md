---
id: "2026-06-02-ts-weights-dimensional-audit"
date: 2026-06-02
title: "ts-weights dimensional audit: are we using a [L²] area as area, length, and ratio interchangeably?"
tags: [ts-weights, density-compensation, dimensional-analysis, audit, hemisphere-segment, grow-refit-merge, v7]
category: review
status: running
refs:
  - "2026-06-02-seed-curvature-median-gate"
  - "2026-06-01-grm-rms-weighting-and-density"
  - "2026-03-15-v7-compare-segmentation-arena"
pipeline: v7-bp2
---

## What `w` is — plain English (read this first)

`w = π · d_8² / 8`, where `d_8` is the distance to a point's 8th-nearest
neighbour. `π·d_8²` is the disk reaching those 8 points; `÷8` makes it **area
per point**. So **`w` is the patch of roof (m²) one point "owns"** — its
Voronoi territory / inverse local density. Dense → small `w`; sparse → big
`w`. It is a **real area, units m²**.

Theory: for density ρ pts/m² a point owns ≈ `1/ρ ≈ (local spacing)²`. On
dahouet bldg0 (bbox density 19.9): expect `1/ρ = 0.050 m²`; measured **mean
`w` = 0.047 m²** — the formula behaves exactly as theory predicts.

Distribution (16 092 pts), heavy-tailed (CV 1.24, max/min ≈ 94×):

| | min | p25 | median | mean | p90 | max |
|---|---|---|---|---|---|---|
| `w` (m²) | 0.011 | 0.019 | **0.028** | **0.047** | 0.084 | 1.02 |
| `√w` (m) | — | — | 0.168 | **0.197** | — | — |

**The single most important fact: `w` is centred on ~0.047 m², NOT on 1.** It
is not a dimensionless factor near unity — it's a small physical area.
Average value vs a naïve uniform `1`:

| form used | mean | vs uniform `1` |
|---|---|---|
| `w` (area) | 0.047 | **21× smaller than 1** |
| `√w` (length) | 0.197 m | **5× smaller than 1** |
| `w/Σw` (ratio) | — | magnitude cancels; only spread matters |

So anywhere `w` is plugged in expecting an ≈1 factor it is ~21× off (~5× for
`√w`). It is only correct where the magnitude is *meant* to be an area, or is
normalised away. **Three honest uses** (the dimensional classes):
1. **as area [m²]** — keep `w` raw (size gates, density raster, sort);
2. **as length [m]** — use `√w` (≈ local spacing) × a *pure number* only;
3. **as ratio [1]** — `w/Σw`; magnitude irrelevant, only the tail matters.

### The honest "uniform": `1/density`, not `1`

When we say "unweight / give every point equal representation", the correct
constant is **NOT `w = 1`** (dimensionless, 21× too big) — it is **`w = 1/density`
= the mean Voronoi area** (per-building, straight from `v7-derive-thresholds`:
`1/density = spacing²`). That gives every point the *average area* — equal
representation **in the right units (m²)**:

- **area uses:** `Σ(1/density) = n/density` = the building's footprint area
  (correct), vs `Σ1 = n` (a meaningless count).
- **length uses:** `√(1/density) = 1/√density` = the **global** spacing
  (correct), vs `√1 = 1` (a bare, wrong "metre").
- **ratio / fit uses:** any constant cancels, so `1` and `1/density` are
  identical here — which is why the de-weighted fits this session are fine.

Consequence for remediation: the dimensionally-correct radius is
`scale · √(w_eff)`, where `w_eff` is either the per-point area `w` (adaptive)
**or** the mean area `1/density` (uniform → `scale/√density`). The C9 bug is
using `√w` **and** `1/√density` together — that's `√w · √(1/density)`, i.e. the
geometric mean of local and global spacing, dimension [L²]. Pick one `w_eff`,
take its square root once.

## Why this audit (rationale)

The per-point weight `w = π·d_k²/k` (k=8), emitted by `v7-tile-signals` as
`point_weights`, has a **real physical dimension: area, [L²]** (m²) — it is
the Voronoi cell a point represents (inverse local density). It was wired
through the v7 segmentation as a general "density-compensation factor"
([[2026-03-15-v7-compare-segmentation-arena]], commit `0c3e28a`) on a
**"this is semantically correct, let's see what happens"** basis — *correct
in spirit, but never studied dimensionally or by order of magnitude*.

This entry exists so we don't lose the thread: it records the **diagnosis**,
the **rubric**, the **expectations**, then (appended) the **per-consumer
findings**, and later the **remediation + results**.

### The trigger

Chasing why a junk valley segment (seg 68) became its own roof plane, we
bisected it to `0c3e28a` and pinned the lever to the **GRM search radius**:
`r = scale·√w·(1/√density)`. Working the numbers (dahouet bldg0,
density 19.872, scale 3.0): the intended/uniform radius is **0.673 m**, but
the ts-weighted radius has **median 0.113 m** (0.17×, ~6× smaller), because
raw `√w ≈ local point spacing ≈ 0.17 m` — *not* an O(1) factor. The formula
is dimensionally `[L]·[L] = [L²]`, a length-squared wearing a length's
clothes. It "works" only because the two spacings multiply to a plausible
number. That is the smell that motivates the whole audit.

## The diagnosis: a polymorphic-dimension (category) error

The same number `w` ([L²]) is fed into consumers that mathematically want
**three different kinds of quantity**:

| role | dimension wanted | correct transform of `w` |
|------|------------------|--------------------------|
| **area estimate** (size gates, density raster) | [L²] | identity (`w`) |
| **length scale** (connectivity / search radius) | [L] | `√w` *normalised* — see below |
| **relative importance** (weighted means, KDE) | dimensionless ratio | only `w/Σw` matters — magnitude irrelevant, **tail shape** is everything |

So "weighting good or bad" is the wrong question. The right questions are
**dimension** and **transform**, per consumer.

## The rubric (how each consumer gets judged)

For every use-site, fill one row:

1. **Current treatment** — raw `w`, `√w`, `Σw`, `w/Σw`, …
2. **Dimension required** — area [L²] / length [L] / ratio [1].
3. **Invariance test** — *"if I doubled the point density everywhere
   (halved the area per point), should this quantity change, and how?"*
   - area gate → should stay the same number of **m²** (so summing raw `w` is right; summing a **count** would double — wrong).
   - length scale → should **shrink like spacing** (`∝ 1/√density`).
   - ratio/KDE → should be **invariant** (ratios don't move).
4. **Required transform** — identity / `√` / normalise-to-mean-1 / clamp-winsorise / drop.
5. **Magnitude- or ratio-sensitive** — does the absolute value matter (gates, radius) or only the relative spread (means/KDE)?
6. **Order-of-magnitude anchor** — a worked number on dahouet, so
   "semantically correct" is replaced by an actual value.
7. **Verdict** — correct / dimensionally-wrong / needs-normalisation /
   needs-tail-robustness / drop.

### The trap (why there is no single global fix)

Tempting "fix": normalise `w` to mean=1 at the **source** (tile-signals).
But that **helps** the ratio uses (invariant anyway) and the radius (makes
`√w` ≈ O(1)), while it **breaks** the area gates (they stop being m² and
their thresholds — `min_segment_area = 1.0 m²` etc. — become meaningless).
So remediation must be **per-consumer**, not one global knob.

## Expectations (hypotheses, to be confirmed/refuted by the audit)

- **Area class** (hemisphere group/cluster/component gates, lambert basin,
  seed sort, boundary density raster): dimensionally **fine** as raw `w`; the
  live risk is **threshold calibration**, not the weight itself.
- **Length class** (hemisphere connectivity radius, GRM search radius):
  **dimensionally broken** — `√w` is a raw local spacing, not a normalised
  factor, so the extra `1/√density` double-counts. This is where the real
  magnitude bug lives (already proven for the GRM radius).
- **Ratio class** (hemisphere weighted-mean normal, offset KDE): magnitude
  irrelevant; the real question is **tail robustness** (a ~180× area spread
  lets a few sparse swath-edge points dominate). For the seed curvature gate
  we already concluded median (no weight) wins
  ([[2026-06-02-seed-curvature-median-gate]]).
- **Removed this session**: all plane **fits/refits** (seed/dv2/grm) were
  pulled off `w` via `v7_fit_weights() → NULL` — a perpendicular fit should
  not be swayed by a tangential area. Out of scope here (already decided).

## Consumer inventory (the sites to audit)

Source: `v7-tile-signals-impl.c:230` — `point_weights[i] = π·d_8²/8` [m²].

| # | consumer | site | current treatment |
|---|----------|------|-------------------|
| C1 | hemisphere — lambert basin accumulate | `v7-hemisphere-segment-impl.c:227,241`; gate `lambert_min_basin_area=0.5` | raw `w` |
| C2 | hemisphere — group area gate | `:440,442`; `min_segment_area=1.0` | `Σw` |
| C3 | hemisphere — weighted mean normal | `:457,464` | `Σ(n·w)/Σw` |
| C4 | hemisphere — offset KDE + min-density | `:488,510,514` | `w` as KDE weight; `min_segment_area/(Σw·bw)` |
| C5 | hemisphere — cluster area gate | `:604,606` | `Σw` |
| C6 | hemisphere — connectivity radius | `:663,664` | `√w · connect_scale` |
| C7 | hemisphere — component area gate | `:743,745` | `Σw` |
| C8 | seed — segment sort priority | `v7-normal-cascade-seed-impl.c:119,121` | `Σw` (DESC) |
| C9 | GRM — search radius | `v7-grow-refit-merge-impl.c:206,207` | `scale·√w·inv_sqrt_density` |
| C10 | boundary-signals — density raster + nearby calib | `v7-boundary-signals-impl.c:1331` | `w` per cell; nearby calibrated to mean own-`w` |

Out of scope: `v7-tile-signals` `weighted_pca` for per-point **normals**
uses an angular/spherical-KDE weight, **not** `point_weights`. The plane
fits were already de-weighted this session.

## Audit findings (2026-06-02)

Anchors used throughout (dahouet bldg0): `w` distribution min 0.011 /
median **0.028** / p90 0.084 / max 1.018 m²; `√w` median **0.167 m** (≈ local
point spacing); density 19.872 → global spacing `1/√density` = **0.224 m**;
`scale = 3.0` for both radii.

### Verdict table

| # | consumer | dim required | current | invariance test | verdict |
|---|----------|--------------|---------|-----------------|---------|
| C1 | lambert basin accumulate | area [L²] | raw `w` | double density → same m² ✓ | **CORRECT** (calibration risk) |
| C2 | group area gate | area [L²] | `Σw` vs 1.0 m² | ✓ stays m² | **CORRECT** (calibration risk) |
| C3 | weighted mean normal | ratio [1] | `Σ(n·w)/Σw` | invariant ✓ | **OK — ratio** (watch tail) |
| C4 | offset KDE + min-density | ratio [1] + area | `w` weight; `A/(Σw·bw)` | invariant / consistent ✓ | **OK** |
| C5 | cluster area gate | area [L²] | `Σw` vs 1.0 m² | ✓ | **CORRECT** (calibration risk) |
| C6 | hemisphere connect radius | length [L] | `√w · 3.0` | shrinks like spacing ✓ | **CORRECT** ✅ |
| C7 | component area gate | area [L²] | `Σw` vs 1.0 m² | ✓ | **CORRECT** (calibration risk) |
| C8 | seed sort priority | area [L²] (ordering) | `Σw` DESC | order by m² ✓ | **OK** (count vs area: no seg-68 effect) |
| C9 | **GRM search radius** | length [L] | `3.0·√w·(1/√d)` | **double-counts spacing** | **DIMENSIONALLY BROKEN** ❌ |
| C10 | boundary density raster | area [L²] | `w`; nearby = mean-own-`w` | ✓; nearby self-normalised | **CORRECT** |

### The one real break: C9 (GRM search radius)

Side-by-side with its correct twin C6:

```
C6 hemisphere :  r = √w · scale            = (local spacing)·3      = [L]   ✓
C9 GRM        :  r = √w · scale · (1/√d)   = (local spacing)·3·(global spacing) = [L²]  ✗
```

C9 carries an **extra `1/√density` factor that C6 does not.** It multiplies
*local* spacing (`√w`) by *global* spacing (`1/√d`), yielding a
length-squared. Numerically that makes it **~5× too small**:

| form | radius (median) |
|------|-----------------|
| C9 as-shipped `3·√w·(1/√d)` | **0.113 m** (the bug) |
| fix A — match C6 `3·√w` (adaptive) | ~0.50 m |
| fix B — uniform `3·(1/√d)` (≈ pre-`0c3e28a`) | 0.673 m |

Both dimensionally-valid forms are **4–6× larger** than the shipped value.
This is the same anomaly that keeps junk valley plane seg 68 alive: the radius
is so small that a neighbour plane can't reach across to absorb it. **Fixing
the dimension (either A or B) would enlarge the radius and let the neighbour
swallow seg 68 — i.e. the principled fix and the seg-68 cleanup are the same
change.** (Confirmed empirically that the uniform form B absorbs seg 68; form
A is intermediate and untested.)

### The rest

- **Area class (C1, C2, C5, C7, C8, C10): dimensionally sound.** Raw `w`/`Σw`
  is the *correct* thing — the invariance test passes (a count would have been
  wrong; area is right). The live risk is **threshold calibration** (the
  1.0 m² / 0.5 m² gates were set when these summed counts-as-1.0, i.e. before
  `0c3e28a` wired real ts-weights — see C8's history; worth a separate pass).
- **Ratio class (C3, C4): fine.** Magnitude is irrelevant (only `w/Σw`
  matters), so no dimension problem. The open question is **tail robustness**
  — a ~180× area spread lets a few sparse points dominate a weighted mean/KDE.
  Low priority unless a specific artifact looks skewed.
- **C10 already self-normalises** the nearby-point weight to the mean own-`w`
  — a good pattern the radius consumers could borrow.

### Audit conclusion

The ts-weight system is **mostly dimensionally correct**: area gates use it as
area (right), ratio consumers don't care about scale (fine), and even one of
the two length consumers (hemisphere C6) builds a proper adaptive radius.
**There is exactly one true dimensional bug — C9, the GRM search radius —
which double-counts spacing and runs ~5× small.** It is also the mechanical
cause of the seg-68 junk plane. Secondary, non-dimensional concern: the
area-gate thresholds may be mis-calibrated post-`0c3e28a` (count→area), but
that's tuning, not a category error.

## Remediation — next-session plan (2 tasks)

**Task 1 — correct the "uniform" generator (`1` → `1/density`).**
The canonical unweighted / equal-representation weight is the **mean Voronoi
area `1/density`** (per-building, from `v7-derive-thresholds`), not the
dimensionless `1`. Apply where magnitude matters:
- `v7-tile-signals-impl.c` degenerate fallback `point_weights = 1.0f` → `1/density`.
- Any uniform-weight injection / A/B path → emit `1/density`, not `1`.
- No change needed for the de-weighted **fits** (ratio use — the constant
  cancels, so `1` is already equivalent to `1/density` there).

**Task 2 — fix C9 (GRM search radius), dimensionally.**
`v7-grow-refit-merge-impl.c` `search_radius()`: replace `scale·√w·(1/√d)`
([L²]) with `scale·√(w_eff)` ([L]) — pick ONE `w_eff`, sqrt once:
- form A (adaptive): `w_eff = w` → `scale·√w` (≈ 0.5 m median; like hemisphere C6),
- form B (uniform): `w_eff = 1/density` → `scale/√density` (= 0.673 m; ties into task 1).
A/B **both** against the hand-labelled good/bad segment set (the 5× larger
radius moves *all* plane growth — confirm labelled GOOD segments don't
over-merge while seg 68 gets absorbed). Bump GRM `.version`, journal results.

_Deferred (task 3, lower priority): re-calibrate the area-gate thresholds
(`min_segment_area=1.0`, `lambert_min_basin_area=0.5`) — set when those summed
counts (pre-`0c3e28a`), now summing real m²; tuning, not a dimensional bug._

_(Results to be appended after the A/B.)_

## Remediation

_(to be appended after we decide.)_
