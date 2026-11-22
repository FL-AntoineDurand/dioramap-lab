---
id: "2026-04-02-beam-direction-multireturn"
date: 2026-04-02
title: "Beam Direction Reconstruction: Multi-Return Pairs Solve the Along-Track Tilt"
tags: [beam-angle, multi-return, projection, nearby-points, boundary-edges]
category: finding
status: concluded
refs: ["2026-04-02-flight-heading-reconstruction", "2026-03-31-boundary-next-gen-plan"]
---

# Beam Direction Reconstruction via Multi-Return Pairs

## Problem

Nearby points projected onto roof planes along the reconstructed beam direction
were landing **inside** the roof boundary instead of at the eave edge. Below-plane
ground points should project to where the beam crossed the plane — at the eave —
not to the interior.

## Root cause

The scan angle model reconstructs the beam in the cross-track × vertical plane:

```
beam_model = (sin(θ)·ctX, -cos(θ), sin(θ)·ctZ)
```

This assumes zero along-track component — the beam swings only perpendicular to
flight. In reality, the scanner (or aircraft pitch) adds a ~7° tilt along the
flight axis. This **along-track component** was completely missing from our model.

## Discovery: multi-return pairs give the true beam

When a LiDAR pulse splits at an edge (roof eave, tree canopy), it produces
multiple returns from the same beam. Return 1 hits the first surface, return 2
hits below. The vector from return 1 to return 2 IS the beam direction — measured
directly from the data, no model needed.

### Key finding: consecutive storage

In the COPC-extracted point arrays, multi-return points from the same pulse are
stored **consecutively** with sequential return numbers:

```
point[i]:   returnNumber=1, numberOfReturns=2, PSID=16, angle=12.50°
point[i+1]: returnNumber=2, numberOfReturns=2, PSID=16, angle=12.51°
```

Detection criteria:
- Consecutive indices
- Same PSID
- Same numberOfReturns
- returnNumber 1 then 2
- Scan angle within ±0.1°

### The measured beam has a significant X component

| PSID | Model beam X | Measured beam X | Correction |
|------|:---:|:---:|:---:|
| P15 | 0.000 | +0.128 | **+0.128** |
| P16 | 0.000 | -0.131 | **-0.131** |

The model said zero, reality says ±0.13. This corresponds to a **7-8° tilt**
along the flight axis (east-west on dahouet).

### The correction is constant per PSID

Tested across the full building footprint (~35m × 33m):

| Split | P15 beam X | P16 beam X |
|-------|:---:|:---:|
| West half | +0.125 | -0.131 |
| East half | +0.124 | -0.127 |
| North half | +0.124 | -0.125 |
| South half | +0.126 | -0.134 |
| Low angle | +0.124 | -0.126 |
| High angle | +0.125 | -0.133 |

Standard deviation: 0.02 (P15), 0.05 (P16). No spatial or angular variation.
A single constant per PSID is sufficient.

### Y and Z residuals are noise, not signal

The Y and Z components show large std (0.16-0.28) but small means (0.04-0.06).
This is surface geometry noise: multi-return points hit different surfaces
(roof edge vs ground) with ~0.3m footprint offset over ~2m separation.
Not real beam direction variation — no interpolation needed.

## Statistics

| Metric | Value |
|--------|-------|
| Total points | 45,696 |
| Multi-return points | 10,548 (23%) |
| 2-return points | 7,599 (16.6%) |
| 3-return points | 2,452 (5.4%) |
| 4-return points | 466 (1.0%) |
| Consecutive ret-1→2 pairs found | 1,151 |
| P15 pairs | 735 |
| P16 pairs | 416 |
| Median pair separation | 2.05m |

## Implementation

### Phase 2a: Flight direction (PCA on swath edges + iso-angle bands)

For each PSID, identify points at extreme scan angles (swath edges) and at
intermediate angles (iso-angle bands). PCA on each band gives the flight direction.
Average across ~9 bands (2 edges + 7 intermediate) for robustness.

```
sorted = points.sortBy(scanAngle)
bands = [sorted[0..3%], sorted[97..100%]]  // edges
       + [iso-angle bands at 7 evenly-spaced angles]  // intermediate

for each band:
  PCA on (x, z) → eigenvector = flight direction
  ensure consistent sign across bands
  
flightDir = mean(all band eigenvectors), normalized
crossTrack = perpendicular(flightDir)
```

Sign disambiguation: check whether higher scan angles are on the positive or
negative cross-track side. Flip cross-track if needed so it points toward
positive angles.

### Phase 2b: Multi-return beam correction

For each consecutive return-1→2 pair:
```
beam_measured = normalize(position[ret2] - position[ret1])
beam_model = (sin(θ)·ctX, -cos(θ), sin(θ)·ctZ)
delta = beam_measured - beam_model
```

Average delta per PSID. Keep only horizontal components (Δx, Δz).
Y delta is noise — set to 0.

### Phase 3b: Per-point beam direction

```
beam = normalize(sin(θ)·ctX + Δx, -cos(θ), sin(θ)·ctZ + Δz)
```

The scan angle model provides the per-point angle-dependent component.
The multi-return correction adds the fixed along-track tilt.

## Physical interpretation

The ~7° along-track tilt likely comes from:
- **Aircraft pitch**: ~2-3° nose-down attitude at cruise speed
- **Scanner mount offset**: deliberate tilt for overlap optimization
- **Or both combined**

Both PSIDs show the same magnitude (~0.13) but opposite sign — consistent with
alternating flight directions: one strip flies east (beam tilts east), the other
flies west (beam tilts west).

## Impact on nearby-point projection

Before the correction, below-plane points projected along the beam would land
~0.7m inside the roof (at 5m height difference). After correction, they land
at the eave edge where the beam physically crossed the roof plane.

This enables Phase B of the boundary plan: using nearby points (ground, walls,
adjacent planes) as boundary signals for edge detection on sparse planes.

## Generalization

The correction vector `(Δx, Δz)` is in local coordinates. For buildings with
different flight headings, the vector naturally rotates with the heading.
No decomposition into heading + magnitude needed — store and apply the raw
`(Δx, Δz)` per PSID.

For sensors without consecutive multi-return storage, the correction cannot be
computed this way. Fallback: use the scan angle model without correction (the
cross-track component is still correct, only the along-track is missing).
