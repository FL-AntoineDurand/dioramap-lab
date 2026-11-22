---
id: "2026-04-02-flight-heading-reconstruction"
date: 2026-04-02
title: "Flight Heading & Beam Direction Reconstruction — Methods, Validation, Pitfalls"
tags: [beam-angle, flight-heading, cross-track, scan-angle, PSID, COPC, coordinate-systems]
category: finding
status: concluded
refs: ["2026-04-01-lidar-point-density-physics-model", "2026-03-31-boundary-next-gen-plan"]
---

# Flight Heading & Beam Direction Reconstruction

## Goal

Reconstruct the 3D beam direction for each LiDAR point from the available per-point
metadata (scan angle, PSID). This requires knowing the flight heading per strip.

## Sensor

Riegl VQ-780 II (single channel, rotating polygon mirror, nadir-pointing).
- Scan pattern: parallel scan lines, perpendicular to flight
- FOV: ±30° (60° total)
- Scan plane: vertical, perpendicular to flight direction
- No forward/aft tilt (unlike the dual-channel VQ-1560 II which has ±14° tilt)
- `ScannerChannel` field = 0 for all points (confirmed single channel)

## Available per-point metadata

| Field | Type | Meaning |
|-------|------|---------|
| ScanAngle | Float32 (degrees) | Angle from nadir within the scan plane |
| PointSourceId (PSID) | Uint16 | Flight strip identifier |
| ScanDirectionFlag | bit | 0=negative scan direction, 1=positive |
| GpsTime | Float64 | Adjusted GPS time (seconds) |
| Intensity | Uint16 | Return amplitude |

## Coordinate systems

**Lambert93 (L93)** — the COPC native system:
- X = easting (increases east)
- Y = northing (increases north)

**Local** — our test case coordinate system:
- X = L93_X - origin_X (east, same as L93)
- Z = -(L93_Y - origin_Y) (south! negated northing)
- Y = elevation (up)

**Critical**: the Y→Z negation flips north-south. Azimuth formulas must account
for this. In L93: `azimuth = atan2(dx, dy)` (from north). In local:
`azimuth = atan2(dx, -dz)` (dz points south, so -dz points north).

## Scan angle sign convention

The LAS 1.4 specification is **ambiguous** about scan angle sign (acknowledged by
ASPRS GitHub issue #41). The older spec says "–90° to the left side of the aircraft
in the direction of flight", implying positive = right side. But the field is
described elsewhere as "rotational position with respect to the vertical."

**Empirical determination** (see below): on the dahouet tile, positive scan angles
correspond to points south of nadir for both PSIDs. Since flight goes east,
positive = right side = south. This is consistent with the older spec definition.

## Beam direction formula

For a nadir-pointing scanner with the scan plane perpendicular to flight:

```
beam = sin(θ) × cross_track + (-cos(θ)) × vertical
     = (sin(θ)·ctX, -cos(θ), sin(θ)·ctZ)
```

Where `θ` = scan angle (degrees, from nadir), `cross_track = (ctX, 0, ctZ)` is the
unit vector pointing in the positive-angle direction (away from nadir, toward the
side of the swath where angle increases).

The sign of cross_track is **critical** — getting it wrong flips the beam 180° in
the horizontal plane, causing below-plane points to project inside roof boundaries
instead of outside.

---

## Five methods for flight heading estimation

All methods tested on the dahouet building (Pléneuf-Val-André, tile 0289_6846).

### Method 1: Gradient fit

Fit `scan_angle = g_x·(x-mx) + g_z·(z-mz) + mean_angle` on all points of a strip.
The gradient direction `normalize(g_x, g_z)` = cross-track. Flight = perpendicular.

```javascript
let sxx=0, szz=0, sxz=0, sxa=0, sza=0;
for (const p of pts) {
  const dx=p.x-mx, dz=p.z-mz, da=p.a-ma;
  sxx+=dx*dx; szz+=dz*dz; sxz+=dx*dz;
  sxa+=dx*da; sza+=dz*da;
}
const det = sxx*szz - sxz*sxz;
const gx = (szz*sxa - sxz*sza) / det;
const gz = (sxx*sza - sxz*sxa) / det;
const crossTrack = normalize(gx, gz);
const flightHeading = perpendicular(crossTrack);
```

**Pros**: uses all points, gives cross-track sign directly.
**Cons**: degenerate when the area is small (building ~35m) and angle range is narrow
(~1.4°). The gradient in one axis can vanish into noise, causing the direction to
collapse to a coordinate axis. On the building, this gave 23-49° error vs true heading.

**Verdict**: reliable only on large areas (>500m). On the full 1km² tile, agrees with
other methods to <1°.

### Method 2: Min/max angle positions

Find the points with minimum and maximum scan angle. The vector from min to max
= cross-track direction.

**Pros**: simple.
**Cons**: unreliable. Uses only 2 points which may be outliers or at diagonal
positions on a rectangular footprint. Consistently gave wrong answers (20-49° error)
on both tile and building scales.

**Verdict**: do not use.

### Method 3: PCA on iso-angle band

Take all points at approximately the same scan angle (e.g. median ± 0.5°). These
points lie on a scan line. PCA gives the line direction = flight heading.

```javascript
const band = pts.filter(p => Math.abs(p.angle - targetAngle) < 0.5);
// PCA on (x, y) positions of band → eigenvector of largest eigenvalue = flight direction
```

**Pros**: directly measures the scan line direction. Very robust on large areas
(elongation 700:1 on 1km² tile).
**Cons**: fails when the iso-angle band is small or when the area shape dominates
(elongation ~2 on the building, meaning building shape = scan line shape by
coincidence).

**Verdict**: best method on tile-scale data. Use nadir (angle≈0) if available,
otherwise median angle.

### Method 4: PCA on swath edges

Take the 3-5% of points with the smallest (or largest) scan angles. These form the
swath edge, which is a line parallel to the flight. PCA = flight heading.

**Pros**: works even at building scale (elongation remains high because swath edge
is always a narrow line). Agrees with tile-level PCA within 2°.
**Cons**: slight bias possible if the swath edge is curved (aircraft turning).

**Verdict**: most robust method across all scales. Works on both 1km² tile and 35m
building.

### Method 5: GPS time scan line extraction

Sort points by GPS time within a strip. Consecutive points with monotonically
changing scan angle form a scan line. Detect reversals (angle direction change) to
segment into individual scan lines. PCA on each scan line = flight heading.

Additionally, within each scan line the angle progression relative to position
gives the **signed** cross-track direction: if angle increases in the direction of
the PCA eigenvector, that's the positive-angle side.

```javascript
pts.sort((a,b) => a.gpsTime - b.gpsTime);
// Detect reversals in scan angle → segment into scan lines
// PCA on each line → flight heading
// Check: angle increases toward which direction → cross-track sign
```

**Pros**: gives individual scan line directions + cross-track sign. Very high
elongation (5K-72K per scan line). Independent confirmation of all other methods.
**Cons**: requires GPS time per point (not always stored). COPC octree shuffles
storage order, so point indices cannot be used — must sort by GPS time.

**Verdict**: gold standard for validation. Confirmed flight heading and cross-track
sign independently.

---

## Results on dahouet

### Building: tile 0289_6846

Tile cube: X=289000-290000, Y=6845000-6846000 (L93).
Building at L93 (289555, 6845192) — within this tile despite the tile name
suggesting 6846000-6847000 (the name is the *upper* Y bound in km, not lower).

**Important**: the building was initially analyzed on the WRONG tile (0290_6845,
which is to the east). The correct tile was found by querying the WFS with the
building's exact coordinates. The COPC octree from the adjacent tile included
building points because octree nodes extend beyond tile boundaries.

### Per-strip results (all methods)

| | P15 | P16 | P17 |
|---|---|---|---|
| **Flight heading** | **~90° (E)** | **~90° (E)** | **~91° (E)** |
| **+angle direction (L93)** | **North (0°)** | **South (180°)** | **North (1°)** |
| **+angle direction (local)** | **-Z (north)** | **+Z (south)** | **-Z (north)** |
| Scan angle range (tile) | +8.6° to +17.2° | -14.1° to +16.9° | -17.1° to -13.9° |
| Scan angle range (building) | +14.3° to +15.7° | +11.7° to +13.1° | not present |
| GPS time span | 12.6 s | 12.3 s | — |
| Scan lines found | 107 | 2281 | — |

### Method agreement

| Method | P15 flight | P16 flight | Scale |
|--------|:---:|:---:|---|
| Gradient fit | 90.0° | 270° (=90° flipped) | tile |
| PCA iso-angle | 90.0° | 89.8° | tile |
| PCA swath edge | 90.0° | 90.0° | tile |
| GPS scan lines | 91.0° | 90.5° | tile |
| PCA swath edge | 89.7° / 91.6° | 88.8° / 90.1° | building |
| Gradient fit | 89.9° | 269.9° (=89.9° flipped) | building |
| Min/max | 66.4° ✗ | 319.2° ✗ | building |

All methods agree on ~90° (East) except min/max which is unreliable.

### Cross-track sign confirmation

Three independent confirmations:

1. **GPS scan lines**: within each scan line, angle increases toward L93 az=0°
   (north) for P15 and toward 180° (south) for P16.

2. **Spatial distribution**: for P16 (which spans negative to positive angles),
   positive angles are at lower L93 Y = further south. Confirmed:
   "positive angles are SOUTH of negative angles."

3. **Gradient sign on tile**: P15 gradient points north (increasing angle toward
   north), P16 gradient points south.

### Why P15 and P16 have opposite cross-track signs

The two strips scan in **opposite cross-track directions**. This is normal — the
rotating polygon mirror always spins the same way, but the aircraft flies in
alternating directions on adjacent strips:
- P15: likely flying east, scanning north-to-south (positive angles = north)
- P16: likely flying west, scanning south-to-north (positive angles = south)

Wait — both fly east (~90°). The difference is in the **scan mirror rotation phase**.
Actually, the PSID includes both strip number and possibly scan direction. With a
rotating polygon mirror, alternate facets sweep in opposite directions.

Or more likely: the COPC processing chain may have normalized scan angles differently
for the two strips, or the strips genuinely have different cross-track conventions
because they come from different flights/days.

---

## Pitfalls discovered

### 1. Wrong tile

The building at L93 (289555, 6845192) was analyzed on tile 0290_6845 (X=290000+)
because the WFS query returned it. But the building is at X=289555 — outside that
tile. The COPC octree extends beyond tile boundaries, so points were found, but
the tile-level analysis (root node PCA) gave results for the wrong area.

**Fix**: always verify the building coordinates fall within the tile's actual cube
bounds, not just the WFS result.

### 2. Gradient fit on small areas

On a 35m building with 1.4° angle range, the gradient fit is numerically
degenerate. The coefficient in one axis can vanish into noise, causing the
cross-track direction to collapse to a coordinate axis. Error: 23-49°.

**Fix**: use tile-level data (1km², >5000 pts per PSID) for gradient fit, or use
PCA on swath edges which is robust even at building scale.

### 3. Sign ambiguity

The gradient direction has a 180° ambiguity — it points "uphill" in angle space,
but doesn't tell you which way is positive vs negative. On the building, where all
angles are positive, this ambiguity can't be resolved from the gradient alone.

**Fix**: use GPS time scan lines to determine the signed cross-track direction
independently. Or use the full tile where angles span both positive and negative.

### 4. Coordinate system confusion

L93 (X=east, Y=north) vs local (X=east, Z=south) creates sign flips in the
north-south axis. Azimuth formulas must use `atan2(dx, dy)` in L93 but
`atan2(dx, -dz)` in local coords. Getting this wrong flips the cross-track 180°.

### 5. COPC point ordering is shuffled

Points in COPC storage order do NOT follow scan line order. The octree spatial
partitioning destroys temporal ordering. Must sort by GPS time to recover scan lines.

---

## Recommended implementation

### At import time

When importing a real building from COPC, compute the flight heading per PSID
using the tile's root node (large area, many points):

```javascript
// 1. Load root node of the COPC tile
// 2. For each PSID, collect points near the nadir angle (or median angle ±0.5°)
// 3. PCA on (L93_X, L93_Y) → eigenvector of largest eigenvalue = flight heading
// 4. Determine cross-track sign:
//    - If angle range spans zero: positive angles are on one side, check which
//    - If all positive: gradient direction = away from nadir = cross-track
// 5. Store in test case JSON: per-PSID { flightHeadingDeg, crossTrackAz }
```

### At runtime (beam-incidence step)

Use the stored flight heading + cross-track to compute beam direction:

```javascript
const ctX = cos(crossTrackAz) // NOT sin — because az is from north
const ctZ = sin(crossTrackAz) // in local coords where Z=south
// Actually: crossTrack in local = (sin(ctAz_L93), 0, -cos(ctAz_L93))

beam_x = sin(scanAngle) * ctLocalX
beam_y = -cos(scanAngle)
beam_z = sin(scanAngle) * ctLocalZ
```

### Validation

The beam direction can be validated by projecting below-plane points along the
beam and checking they land **outside** the roof boundary (at the eave edge),
not inside. If they land inside, the cross-track sign is wrong.
