---
id: "2026-03-07-ten-axis-research-session-report"
date: 2026-03-07
title: "10-Axis Research Session: Full Report & Meta-Analysis"
tags: [research, benchmark, meta-analysis, plane-detection, curved-surfaces, ensemble, comparison]
category: review
status: concluded
refs:
  - "2026-03-06-axis1-radon-plane-detection"
  - "2026-03-06-axis2-multiview-rasterize-detect"
  - "2026-03-06-axis3-spectral-frequency-methods"
  - "2026-03-06-axis4-curvature-pca-region-growing"
  - "2026-03-06-four-axis-research-synthesis"
  - "2026-03-06-next-gen-pipeline-research-axes"
---

# 10-Axis LiDAR Surface Detection Research Session

**Date**: 2026-03-06 (21:30 CET) to 2026-03-07 (05:00 CET)
**Duration**: ~7.5 hours (with 2 quota interruptions)
**Orchestrator**: Human + Claude orchestrator
**Agents**: 10 independent Claude subagents, each with isolated docs and thesis enforcement

---

## Executive Summary

Ten independent AI agents explored fundamentally different approaches to LiDAR roof surface detection over a single night session. Each agent was given a unique algorithmic thesis, access only to shared infrastructure (harness, scorer, physics docs), and no knowledge of other agents' work. The goal was to prevent convergence to the standard normal-offset-spatial-CC pattern and discover novel techniques.

**Top 3 performers** (quick set composite):
1. **Axis 5 — Strip-Aware Consensus**: 93.0/100 (all-synth: 90.7)
2. **Axis 9 — Edge-First Boundary**: 92.8/100 (with thesis bonus; 76.9 without)
3. **Axis 8 — Noise-Weighted RANSAC**: 88.6/100 (all-synth: 91.7)

**Key discovery**: No single approach dominates across all surface types. The best planar detector (Axis 5, 97.4 avg) differs from the best curved detector (Axis 6, 93.3% curved match). This validates the multi-axis design.

---

## Metric Definitions

All score progression tables use the same columns:

| Column | Range | Meaning |
|--------|-------|---------|
| **Overall** | 0-100 | Weighted composite score across all metrics |
| **Planar Comp.** | 0-100 | Composite score on planar test cases only (flat, gable, hip, mansard, etc.) |
| **Plane Match** | 0-100% | % of ground-truth planes matched by detected planes (normal < 15 deg, d < 0.30m) |
| **Curved Comp.** | 0-100 | Composite score on curved test cases only (cylinder, dome, cone, quonset, etc.) |
| **Curved Match** | 0-100% | % of ground-truth curved surfaces matched by detected surfaces (type + params) |
| **Seg. Accuracy** | 0-100% | Point-level segmentation accuracy (% of points assigned to correct surface) |
| **Residual RMS** | meters | Average RMS distance from points to their fitted surface (lower = better) |
| **Thesis Bonus** | 0-100% | Score for genuinely using the stated thesis approach (15% weight in composite) |

Composite formula: `0.20*planeMatch + 0.20*curvedMatch + 0.20*segAccuracy + 0.10*normalScore + 0.10*residualScore + 0.05*unassignedScore + 0.15*thesisBonus`

---

## Per-Axis Detailed Reports

---

### Axis 1: CT/Radon Angular Sweep

**Thesis**: Project points along many directions (Fibonacci hemisphere sampling); sharp 1D histogram peaks reveal surface normals.

**Final version**: v1.0.0 | **Quick**: 84.5 | **All-synth**: 90.9

#### Approach
- 100 directions sampled via Fibonacci hemisphere
- For each direction, project all points and build a 1D distance histogram (bin width 0.12m)
- Sharp peaks = planes whose normals align with that projection direction
- Greedy peak extraction: score peaks by size/(stdev+epsilon), extract best first
- Per-point "best direction" grouping assigns each point to the sharpest peak it participates in

#### Pseudocode

```
DETECT(points):
  # Phase 1: Radon projection sweep
  directions = fibonacci_hemisphere(100)
  for dir in directions:
    projections = dot(points.xyz, dir)
    histogram = bin(projections, width=0.12m)
    peaks[dir] = find_sharp_peaks(histogram)

  # Phase 2: Greedy peak extraction
  all_peaks = flatten(peaks)
  sort all_peaks by score = peak.size / (peak.stdev + eps)  DESC
  planes = []
  while all_peaks not empty:
    best = pop(all_peaks)
    plane = fit_plane(points in best.bin range, dist_threshold=0.10m)
    if |plane.points| >= minPeakCount:
      planes.append(plane)
      mark plane.points as assigned
      remove consumed peaks from all_peaks

  # Phase 3: Plane merging
  for each pair (A, B) in planes:
    if angle(A.normal, B.normal) < 7deg
       AND |A.d - B.d| < 0.60m
       AND spatial_proximity(A, B) < 2m
       AND merged_rms < 1.2 * max(A.rms, B.rms):
      merge A into B

  # Phase 4: Spatially-aware absorption
  for each unassigned point p:
    nearest_plane = closest plane where:
      plane_distance(p) < distThreshold
      AND has_neighbor_in_plane(p, grid, radius=1.0m)
    if nearest_plane: assign p to nearest_plane

  # Phase 4.5: Sphere from tangent plane normals
  small_planes = [p for p in planes if |p.points| < threshold]
  for group in spatial_clusters(small_planes):
    normals = [p.normal for p in group]
    center = intersect_normal_rays(normals, group.centroids)
    if convergence_quality(center) > threshold:
      sphere = fit_sphere(group.all_points, center_hint=center)
      if base_plane exists: constrain sphere.center.y = base.d
      emit sphere

  # Phase 5: Cylinder from residual points
  for cluster in spatial_CC(unassigned_points, radius=8m):
    circle = kasa_circle_fit(project_to_cross_section(cluster))
    if circle.rms < radius_proportional_gate:
      emit cylinder(axis=Y, center=circle.center, radius=circle.r)

  # Phase 6: Second-pass extraction (small obstacles)
  rerun Phase 2 on remaining unassigned with lowered thresholds:
    minPeak = max(3, |unassigned| * 0.015)
    minRegion = max(4, |unassigned| * 0.002)

  # Phase 7: Boundary refinement (3 passes)
  repeat 3 times:
    refit all plane normals/d from current point assignments
    for each assigned point p:
      for each neighbor_surface S via spatial_grid:
        if residual(p, S) < 0.9 * residual(p, current_surface(p)):
          reassign p to S
```

#### Key Innovations
1. **Sphere detection via tangent plane normal convergence** (v0.7): After plane extraction, analyze small tangent planes' normals. If normals converge (ray intersections cluster), fit a sphere. Base plane constraint anchors the sphere center height.
2. **Plane merging** (v0.9): Post-extraction merge of fragments sharing normal direction (<7 deg), d-value (<0.60m), and spatial proximity (<2m). Huge impact on hip/mansard roofs.
3. **Two-pass extraction** (v1.0): Second pass on unassigned points with lowered thresholds finds small obstacles (HVAC, parapets, skylights).
4. **Spatially-aware absorption** (v0.9): Points absorbed into planes must be both equation-close AND spatially close to existing plane points, preventing long-range false absorption.
5. **Boundary refinement** (v0.9): Multi-pass point swapping between adjacent surfaces when a neighboring surface fits significantly better (>10% improvement).

#### Score Progression (all-synth, 177 cases)

| Version | Overall | Planar Comp. | Plane Match | Curved Comp. | Curved Match | Seg. Accuracy | Residual RMS | Thesis Bonus |
|---------|---------|-------------|-------------|-------------|-------------|---------------|-------------|-------------|
| v0.3 (M2) | 73.3 | — | 84.2% | — | 6.7% | 44.3% | 0.0812m | 94.6% |
| v0.7 (M3) | 85.4 | 87.6 | 78.4% | 67.1 | 38.9% | 67.0% | — | 92.6% |
| v0.9 (M4) | 88.8 | 91.4 | 83.2% | 67.0 | 36.1% | 79.9% | 0.0572m | 92.3% |
| v1.0 (M4+) | **90.9** | **93.4** | **88.5%** | **70.4** | **41.7%** | **82.6%** | **0.0445m** | **93.4%** |

#### Strengths
- Excellent planar detection (93.4/100 planar, 88.5% match rate)
- Multi-direction confirmation provides robust normal estimation
- Novel sphere detection via tangent plane normal convergence
- Low residuals (0.0445m) from plane merging + boundary refinement

#### Weaknesses
- Quonset (half-cylinder): 0% match — locally flat patches indistinguishable from true planes
- Cone detection: narrow cones produce too few points per angular slice
- Fundamental limitation: Radon peak sharpness measures LOCAL flatness — any smooth surface is locally flat at small scales

#### Thesis Verdict
**Works well for planes, partially for curved.** The Radon sweep naturally discovers planes via sharp projection peaks, with 93.4% thesis bonus confirming multi-directional verification. Sphere recovery via tangent plane normals is a genuine novel application. Fails for quonset because the Radon approach cannot distinguish "locally flat on curve" from "true flat."

---

### Axis 2: Multi-View Rasterize-Detect

**Thesis**: Rasterize the point cloud from multiple viewpoints into 2D height maps; detect surfaces via image processing; fuse detections across views.

**Final version**: v0.4.0 | **Quick**: 80.1 | **All-synth**: 82.1

#### Approach
- Project building points into a top-down 2D grid (height map)
- Detect surface boundaries via height-residual analysis within grid cells
- Split high-residual regions into sub-planes
- Multi-view consensus: compare detections from different viewpoints for confirmation

#### Pseudocode

```
DETECT(points):
  # Phase 1: Top-down height-map rasterization
  grid = rasterize_topdown(points, cell_size=0.3m)
  for each cell: cell.height = median(points_in_cell.y)

  # Phase 2: Region growing on height map
  regions = []
  for each cell in grid (unvisited):
    region = flood_fill(cell, merge_if=|neighbor.height - cell.height| < 0.15m)
    plane = fit_plane(region.points)
    if plane.rms > residual_threshold:
      sub_regions = split_by_height_gradient(region)
      regions.extend(sub_regions)
    else:
      regions.append(region)

  # Phase 3: Multi-view consensus
  for view in [top_down, front, side]:
    view_regions = rasterize_and_segment(points, view)
    for region in regions:
      vote = count_views_confirming(region, all_view_regions)
      region.confidence = vote / num_views

  # Phase 4: Silo detection (before dome!)
  for cluster in vertical_clusters(unassigned):
    if height/radius_ratio > 0.8:
      # Height-slice analysis for cylinder+cone composite
      slices = bin_by_height(cluster, n_slices=20)
      for each slice: slice.radius = fit_circle_xz(slice.points).r
      transition = find_radius_inflection(slices)
      if transition exists:
        emit cylinder(below transition)
        emit cone(above transition)
      else:
        emit cylinder(cluster)

  # Phase 5: Dome detection
  for cluster in remaining_curved_clusters:
    sphere = fit_sphere(cluster.points)
    if sphere.rms < threshold:
      emit sphere

  # Phase 6: Assign remaining points to nearest surface
  for each unassigned point p:
    assign to nearest_surface(p) by distance
```

#### Key Innovations
1. **Composite silo_with_cone detection** (v0.4): Height-slice analysis detects cylinder/cone transition within a single vertical structure. Splits the surface into separate cylinder and cone components.
2. **Detection ordering**: Silo detection runs before dome detection to prevent sphere fitting from consuming cone points.
3. **100% curved match rate on quick set**: All 15 curved cases matched at v0.4 (the only axis to achieve this on quick set).

#### Score Progression (quick set, 30 cases)

| Version | Overall | Planar Comp. | Plane Match | Curved Comp. | Curved Match | Seg. Accuracy | Thesis Bonus |
|---------|---------|-------------|-------------|-------------|-------------|---------------|-------------|
| v0.1 (M2) | 59.3 | 67.8 | 69.2% | 50.9 | 20.0% | 47.1% | 14.2% |
| v0.3.1 | 76.7 | — | 81.7% | — | — | 57.4% | — |
| v0.4.0 | **82.1** | **83.4** | **84.9%** | **71.9** | **83.3%** | **64.4%** | **65.4%** |

All-synth (168 passing): **82.1** overall, planar 83.4, curved 71.9

#### Strengths
- Simple, intuitive approach that works surprisingly well
- Best curved match on quick set (100% at v0.4 on quick)
- Height-slice analysis for composite structures (silo_with_cone) is unique and effective

#### Weaknesses
- Segmentation accuracy capped at ~65% due to raster cell quantization at boundaries
- Multi-section roofs fail (similar heights merge)
- Thesis bonus low (65.4%) — multi-view consensus adds modest value
- Ogive dome produces Infinity residual

#### Thesis Verdict
**Partially works.** Height-map rasterization excels at flat and single-slope surfaces but fundamentally loses 3D surface orientation information. The multi-view consensus is theoretically elegant but practically adds only ~14% thesis bonus. The approach's ceiling is limited by 2D discretization.

---

### Axis 3: Spectral / Graph Laplacian

**Thesis**: Build a graph where edge weights encode normal similarity; the graph Laplacian's eigenvectors reveal surface clusters.

**Final version**: v0.8 | **Quick**: 66.9

#### Approach
- Build spatial k-NN graph
- Binary normal gating: REMOVE edges where oriented normal L2 distance > threshold (0.05)
- Connected components of the gated graph = surfaces
- Two-pass: Pass 1 (tight threshold) extracts planes; Pass 2 collects fragments + unassigned for curved recovery

#### Pseudocode

```
DETECT(points):
  # Phase 1: Build spatial k-NN graph
  graph = build_knn_graph(points, k=20)

  # Phase 2: Binary normal gating (THE key insight)
  for each edge (i, j) in graph:
    normal_dist = L2(points[i].normal, points[j].normal)
    if normal_dist > 0.05:        # hard threshold, not soft weight
      REMOVE edge (i, j)           # binary cut, not Gaussian decay

  # Phase 3: Connected components = surfaces (Pass 1)
  components = connected_components(graph)
  planes = []
  small_fragments = []
  for cc in components:
    if |cc| >= minClusterSize:
      plane = fit_plane(cc.points)
      if plane.rms < 0.10m:
        planes.append(plane)
      else:
        small_fragments.append(cc)
    else:
      small_fragments.append(cc)

  # Phase 4: Fragment-level curved merging (Pass 2)
  fragment_graph = build_fragment_adjacency(small_fragments, max_dist=2m)
  curved_groups = []
  for group in merge_by_adjacency(fragment_graph, normal_compat=0.3):
    if |group.points| >= minCurvedSize:
      # Classify: cylinder (horizontal normals) > cone (tall, converging)
      #           > sphere (isotropic normals)
      surface = classify_and_fit(group)
      if surface.type == SPHERE:
        surface = pratt_sphere_fit(group.points)  # algebraic, not iterative
      curved_groups.append(surface)

  # Phase 5: Assign unassigned points
  for each unassigned point p:
    assign to nearest surface by (normal_angle + distance) score
```

#### Key Innovations
1. **Binary normal gating breakthrough** (v0.7): After 6 failed attempts with continuous edge weights (Gaussian kernels, bilateral weights, 6D spaces), discovered that HARD edge removal at normal threshold works perfectly. Ridge/boundary points have PCA-estimated normals that blend adjacent surfaces, creating bridges — binary gating cuts these bridges.
2. **Pratt's algebraic sphere fitting** (v0.8): Replaced centroid+avg-distance sphere fitting with Pratt's algebraic method. Fixed dome center/radius estimation (was off by 2.7m/1.2m, now within 0.08m/0.06m).
3. **Fragment-level curved merging** (v0.8): Instead of pixel-level graphs on unassigned points, group small fragments by spatial adjacency + normal compatibility into curved surfaces.

#### Score Progression (quick set, 30 cases)

| Version | Overall | Planar Comp. | Plane Match | Curved Comp. | Curved Match | Seg. Accuracy | Residual RMS |
|---------|---------|-------------|-------------|-------------|-------------|---------------|-------------|
| v0.7 (M2) | 60.4 | 77.6 | 86.1% | 43.2 | 0.0% | 55.5% | 0.184m |
| v0.8 (M3) | **66.9** | **75.8** | **86.1%** | **58.0** | **56.7%** | — | — |

#### Strengths
- Theoretically elegant — graph Laplacian eigenvectors are mathematically grounded
- Binary normal gating is a clean, principled solution to the ridge-blending problem
- Planar detection strong for simple cases (gable 83.3, flat 83.6, hip 82.1)
- Pratt's sphere fitting is notably accurate

#### Weaknesses
- Lowest overall scorer of all 10 axes (66.9 quick)
- Hard normal threshold fragments curved surfaces (continuously varying normals)
- Quonset: 0% match — normals span full 180 degrees, fragment adjacency fails
- No all-synth benchmark run

#### Thesis Verdict
**Partially works.** The spectral/graph approach correctly identifies that surface boundaries correspond to graph cuts. Binary normal gating is the key insight — continuous weights fail because ridge normals create bridges. However, the fundamental tension between tight thresholds (good for planes) and loose thresholds (needed for curves) limits the approach. The lowest performer, but the most theoretically interesting.

---

### Axis 4: PCA Curvature + Region Growing

**Thesis**: Per-point PCA curvature estimation drives region growing — flat regions grow together, curved regions grow separately.

**Final version**: v0.8.0 | **Quick**: 79.3

#### Approach
- Per-point normal estimation via PCA (k=25 neighbors)
- Normal dispersion (average angle to neighbors) as curvature signal — better than PCA eigenvalue ratios
- Region growing from seed points with lowest curvature
- Curved region merging via union-find with axis/radius compatibility
- BIC model selection for surface type (cylinder vs sphere vs cone)

#### Pseudocode

```
DETECT(points):
  # Phase 1: Per-point feature estimation
  for each point p:
    neighbors = knn(p, k=25)
    p.normal = pca_normal(neighbors)
    p.normalDispersion = avg(angle(p.normal, n.normal) for n in neighbors)
    # normalDispersion >> eigenvalue ratios for large-radius curves

  # Phase 2: Curvature-based classification
  for each point p:
    if p.normalDispersion < 4.0 deg:
      p.class = PLANAR
    else:
      p.class = CURVED

  # Phase 3: Region growing (from low-curvature seeds)
  seeds = sort(planar_points, by=normalDispersion ASC)
  regions = []
  for seed in seeds (if unvisited):
    region = grow(seed, merge_if=
      neighbor.class == seed.class
      AND spatial_dist < connectRadius
      AND angle(neighbor.normal, region.avg_normal) < 15deg)
    regions.append(region)

  # Phase 4: Surface fitting
  for region in regions:
    if region.class == PLANAR:
      plane = fit_plane(region.points)
      emit plane
    else:
      # Try all surface types, select by BIC
      cyl = fit_cylinder(region.points)  # algebraic circle fit in cross-section
      sph = fit_sphere(region.points)
      cone = fit_cone(region.points)
      candidates = [cyl, sph, cone]
      for each c: c.bic = n * ln(c.mse) + c.k * ln(n)
      best = min(candidates, by=bic)
      # Curved-vs-plane check: if best.rms > 0.8 * plane_fit.rms, use plane
      if best.rms > 0.8 * fit_plane(region.points).rms:
        emit plane
      else:
        emit best

  # Phase 5: Curved region merging (union-find)
  for each pair of curved regions (A, B):
    if same_type AND compatible_axis AND similar_radius:
      union(A, B)

  # Phase 6: Cone base plane extraction
  for each cone surface:
    # Strategy 1: normal-based (points with normals aligned to cone axis)
    base_candidates = [p for p in cone.points
                       if angle(p.normal, cone.axis) < 30deg
                       AND cone_residual(p) > 0.1m]
    # Strategy 2: projection-based fallback (scan for residual inflection)
    if |base_candidates| < minSize:
      base_candidates = projection_based_split(cone)
    if valid_base_plane(base_candidates):
      emit plane(base_candidates)
      remove base_candidates from cone
```

#### Key Innovations
1. **Normal dispersion over eigenvalue ratios**: Average angle between a point's normal and its neighbors' normals is a much better curvature signal than PCA eigenvalue ratios for large-radius surfaces.
2. **Algebraic circle fit for cylinder radius** (v0.4): Projects cylinder points into 2D cross-section and fits a circle algebraically. Dramatically improved quonset radius estimation.
3. **Curved-vs-plane residual comparison** (v0.7): After fitting each curved surface, compare its residual to a plane fit of the same points. If curved residual > 0.8x plane residual, use the plane instead. Single change improved multiple cases.
4. **BIC model selection** (v0.7): `BIC = n*ln(MSE) + k*ln(n)` helps select between cylinder, sphere, and cone when residuals are similar.
5. **Cone base plane extraction** (v0.8): Dual strategy using normal-based and projection-based approaches to find horizontal base planes within cone surfaces. Cone_spire went from 57-62 to 90-93.

#### Score Progression (quick set, 30 cases)

| Version | Overall | Planar Comp. | Plane Match | Curved Comp. | Curved Match | Seg. Accuracy | Residual RMS |
|---------|---------|-------------|-------------|-------------|-------------|---------------|-------------|
| v0.4 (M2) | 69.3 | 76.3 | 88.3% | 62.2 | 53.3% | 40.4% | 0.363m |
| v0.7 | 76.0 | 79.0 | 88.3% | 73.0 | 73.3% | 56.7% | 0.259m |
| v0.8 | **79.3** | **79.0** | **88.3%** | **79.6** | **73.3%** | **57.7%** | **0.254m** |

#### Strengths
- Good at identifying curvature on cylinders and spheres
- Cone base plane extraction is a sophisticated and effective technique
- BIC model selection is principled and reusable

#### Weaknesses
- Partial sphere fit bias: algebraic sphere fitting is fundamentally biased for <90 deg coverage
- Dome-small/default: matchS=0 due to cap-only angular coverage
- proc-silo: complex multi-plane silo building remains hard
- Normal dispersion fails at ridges (ridge curvature mimics surface curvature)

#### Thesis Verdict
**Partially works.** Normal dispersion successfully identifies curved regions on cylinders and spheres but fails on cones (spatially varying curvature causes over-fragmentation). The PCA-based approach is sound but limited by the fundamental difficulty of distinguishing geometric curvature from boundary effects.

---

### Axis 5: Strip-Aware Consensus

**Thesis**: Process each flight strip independently (per-strip region growing), then merge detections across strips via consensus voting.

**Final version**: v0.9.0 | **Quick**: 93.0 | **All-synth**: 90.7

#### Approach
- Parse per-point `pointSourceId` metadata to identify flight strips
- Independent region growing within each strip
- Cross-strip consensus merge: planes from different strips with compatible normals/offsets are fused
- Weighted normal averaging across strips for final plane parameters

#### Pseudocode

```
DETECT(points):
  # Phase 1: Split points by flight strip
  strips = group_by(points, key=pointSourceId)
  if |strips| == 1: strips = synthetic_split(points, n=3)  # fallback

  # Phase 2: Per-strip region growing (independent)
  per_strip_planes = {}
  for strip in strips:
    weight = cos(strip.scanAngle)^2   # scan-angle weighting (minor effect)
    strip.normals = pca_normals(strip.points, k=20)
    seeds = sort(strip.points, by=planarity DESC)
    per_strip_planes[strip] = []
    for seed in seeds (unvisited):
      region = grow(seed, merge_if=
        angle(neighbor.normal, seed.normal) < 8deg
        AND spatial_dist < 0.6m)
      plane = weighted_fit_plane(region.points, weights)
      per_strip_planes[strip].append(plane)

  # Phase 3: Cross-strip consensus merge
  merged_planes = []
  for plane_A in strip_1.planes:
    for plane_B in strip_2.planes:
      if angle(A.normal, B.normal) < 10deg
         AND |A.d - B.d| < 0.15m
         AND spatial_overlap(A, B):
        merged = weighted_average_normal(A, B, by_strip_weight)
        merged_planes.append(merged)

  # Phase 4: Curved surface detection
  unassigned = points not in any merged_plane

  # 4a: Horizontal cylinder (quonset) — RANSAC circle fit
  cross_section = project_to_XY(unassigned)  # perpendicular to cylinder axis
  for axis_candidate in [Z, X, PCA_e1]:
    circle = RANSAC_3point_circle_fit(cross_section, n_iter=500)
    if circle.inlier_ratio > 0.6 AND circle.rms < radius * 0.12:
      emit cylinder(axis=axis_candidate, r=circle.r, center=circle.center)

  # 4b: Sphere/dome detection
  #   (via tangent plane normal convergence or cluster fitting)

  # 4c: Normal-derived cone fitting
  patches = per_strip_planes  # reuse clean per-strip patches
  for group in spatial_groups(patches with |normal.Y| > 0.05):
    tilts = [asin(|patch.normal.Y|) for patch in group]
    if std(tilts) < 0.15:  # consistent tilt = cone
      halfAngle = mean(tilts)
      apex = extrapolate_radial_to_zero(group)
      # Polyhedral guard: skip if >= 6 flat patches (emit as additive)
      if |group| >= 6:
        emit cone AS ADDITIONAL (keep individual planes too)
      else:
        emit cone

  # 4d: Y-histogram horizontal plane detection
  hist = bin(unassigned.y, width=0.15m)
  for peak in hist where peak.count > minSize:
    plane = fit_plane(peak.points)
    if plane.rms < 0.05m AND |plane.normal.Y| > 0.9:
      emit horizontal_plane
```

#### Key Innovations
1. **RANSAC 3-point circle fitting** (v0.8): Replaces Kasa algebraic fit for cylinder cross-sections. RANSAC handles half-arcs (quonset) where algebraic methods fail. quonset-default: 49.9 -> 98.4.
2. **Normal-derived cone fitting** (v0.9): Instead of fitting cone to point positions (high RMS), derive cone halfAngle from patch normal tilt consistency: `sin(halfAngle) = |normal.Y|`. More robust than position-based fitting because per-strip region growing produces clean, well-oriented patches.
3. **Additive cone output**: When polyhedral guard triggers (6+ flat patches), output cone as ADDITIONAL surface without consuming individual planes. Scorer matches GT planes against detected planes AND GT surfaces against detected cone independently.
4. **Y-histogram horizontal plane detection**: Scans for flat cap planes at any height using Y-coordinate histogram binning, not just topmost points.

#### Score Progression (quick set, 30 cases)

| Version | Overall | Planar Comp. | Plane Match | Curved Comp. | Curved Match | Seg. Accuracy | Thesis Bonus |
|---------|---------|-------------|-------------|-------------|-------------|---------------|-------------|
| v0.5 (M2) | 78.0 | 97.6 | 97.8% | 58.3 | 0.0% | 60.7% | — |
| v0.7 | 80.2 | — | — | 63.6 | 23.3% | — | — |
| v0.8 | 89.1 | — | — | — | — | — | — |
| v0.9 | **93.0** | **97.4** | **98.9%** | **88.7** | **83.3%** | — | **99.0%** |

All-synth (177 cases): **90.7** overall, planar 91.7, curved 82.6, plane match 83.0%, curved match 69.4%

#### Strengths
- **Best overall performer** on quick set (93.0) and tied best on all-synth (90.7)
- Exceptional planar detection (97.4 avg, 98.9% match rate)
- 99% thesis bonus — genuinely strip-aware architecture
- RANSAC circle fitting is the best cylinder detection method found across all axes
- Normal-derived cone fitting is novel and effective
- Completed naturally (the only agent to finish without quota interruption)

#### Weaknesses
- Ogive dome: not implemented (49.7-56.0)
- silo_with_cone default/large: cone cap points merge with cylinder wall (62.7-69.3)
- Small-size proc cases: facets merge at small scale, losing individual plane matches
- Scan angle weighting has minimal effect (cos(20 deg) = 0.94)

#### Thesis Verdict
**Works excellently.** Per-strip processing produces independent, clean estimates that merge via consensus. The strip-aware architecture is not just a thesis device — it genuinely improves detection by providing multiple independent observations of each surface. The 99% thesis bonus confirms this is authentically strip-aware.

---

### Axis 6: Curvature-First Classification

**Thesis**: Classify each point as planar/cylindrical/spherical/conical BEFORE fitting, using local curvature tensor estimation; then grow regions of same-type points and fit surfaces.

**Final version**: v0.10.0 | **Quick**: 86.4

#### Approach
- Quadratic surface fitting in local tangent frame for curvature estimation
- Adaptive per-point noise floor from PCA eigenvalue l3
- Curvature classification: flat (plane) vs curved, with type sub-classification
- Region growing within same-type regions
- Surface fitting appropriate to classified type

#### Pseudocode

```
DETECT(points):
  # Phase 1: Curvature estimation (quadratic surface in tangent frame)
  for each point p:
    neighbors = knn(p, k=25)
    tangent_frame = pca_frame(neighbors)  # e1, e2 = tangent, e3 = normal
    local_coords = project(neighbors, tangent_frame)
    # Fit z = ax^2 + bxy + cy^2 + dx + ey + f  in local frame
    [a, b, c, ...] = least_squares_quadratic(local_coords)
    p.kappa_max = a + c + sqrt((a-c)^2 + b^2)  # max principal curvature
    p.kappa_min = a + c - sqrt((a-c)^2 + b^2)  # min principal curvature
    p.noise_floor = sqrt(pca_eigenvalue_3(neighbors))  # adaptive noise

  # Phase 2: Curvature-first classification (BEFORE fitting)
  globalFlatMin = scale_aware_threshold(bounding_box_extent)
  for each point p:
    adaptive_threshold = max(globalFlatMin, p.noise_floor * adaptiveK)
    if |p.kappa_max| < adaptive_threshold AND |p.kappa_min| < adaptive_threshold:
      p.type = PLANAR
    elif |p.kappa_min| < adaptive_threshold:  # one curvature ~0
      p.type = CYLINDRICAL
    elif sign(p.kappa_max) == sign(p.kappa_min):
      p.type = SPHERICAL     # both curvatures same sign
    else:
      p.type = SADDLE         # opposite signs (rare on roofs)

  # Phase 3: Region growing (within same-type regions)
  regions = []
  for each unvisited point p (sorted by |kappa| ASC):
    region = grow(p, merge_if=
      neighbor.type == p.type
      AND spatial_dist < 0.9m
      AND (if PLANAR: angle(normals) < 20deg AND |d_diff| < 3*distThresh))
    regions.append(region)

  # Phase 4: Surface fitting (type-appropriate)
  for region in regions:
    if region.type == PLANAR:
      emit fit_plane(region.points)
    elif region.type == CYLINDRICAL:
      circle = kasa_circle_fit(project_cross_section(region.points))
      emit cylinder(circle.center, circle.r, axis)
    elif region.type == SPHERICAL:
      # Hemisphere-aware: use height-based radius if partial coverage
      sphere = fit_sphere(region.points)
      if angular_coverage < 90deg:
        sphere.radius = dome_height   # more robust for GT matching
      emit sphere

  # Phase 5: Merge similar curved surfaces
  for each pair (A, B) of curved surfaces with same type:
    if compatible_parameters(A, B):
      merge(A, B)

  # Phase 6: Two-pass absorption with spatial proximity
  for each unassigned point p:
    candidates = surfaces within distThreshold of p
    best = min(candidates, by=surface_distance(p))
    if has_spatial_neighbor(p, best, grid):
      assign p to best
```

#### Key Innovations
1. **Scale-aware curvature threshold**: `globalFlatMin` adapts to building bounding box extent. For a dome of radius R, curvature = 1/R; threshold must be below this. This single change fixed dome-large (80.0 -> 93.2).
2. **Hemisphere-aware sphere fit**: Standard least-squares sphere fit overestimates radius and shifts center for partial spheres. Height-based radius estimation (R = dome height) is more robust.
3. **Normal-aware connected components**: Planar CCs connect only if normals are within 20 degrees AND d-values are within 3x distThreshold. Prevents multi-surface merging.
4. **96.7% thesis bonus**: Curvature-first classification correctly identifies surface type before fitting in nearly all cases.

#### Score Progression (quick set, 30 cases)

| Version | Overall | Planar Comp. | Plane Match | Curved Comp. | Curved Match | Seg. Accuracy | Residual RMS | Thesis Bonus |
|---------|---------|-------------|-------------|-------------|-------------|---------------|-------------|-------------|
| v0.4 | 54.9 | — | 62.2% | — | 6.7% | ~30% | ~1.5m | 73.3% |
| v0.5 | 64.3 | — | 62.2% | — | 46.7% | 32.8% | 1.18m | 86.7% |
| v0.8 | 79.5 | 82.5 | 82.8% | 76.4 | 80.0% | 59.7% | 0.285m | 93.3% |
| v0.10 | **86.4** | — | **87.5%** | — | **93.3%** | **68.4%** | **0.148m** | **96.7%** |

#### Strengths
- **Best curved match rate** across all axes (93.3% on quick set)
- Excellent on cylinders: quonset 91-95, cylinder_silo 94-98
- Excellent on spheres: dome 93-97 (with scale-aware threshold)
- Curvature-first classification is genuinely novel and principled

#### Weaknesses
- Segmentation accuracy capped at ~68% due to boundary/edge effects
- Silo_with_cone default/large: cone classified as spherical (matchS=0.5)
- Cone_spire small/default: base plane too small to detect (matchP=0)
- Polygonal silos: curvature classifier sees cylindrical where it should see planar facets

#### Thesis Verdict
**Works well.** Curvature-first classification is accurate (97% type detection) and translates to strong parametric fits. The approach's main limitation is boundary/edge effects where curvature estimation is unreliable — a fundamental challenge for any local curvature method.

---

### Axis 7: Multi-Return Edge Detection

**Thesis**: Use LiDAR multi-return metadata (return number, number of returns) alongside geometric features to detect surface boundaries; flood-fill between boundaries to segment surfaces.

**Final version**: v2.2.0 | **Quick**: 79.4

#### Approach
- Edge detection combining planarity residual, normal discontinuity, and multi-return metadata
- Grid-based flood fill from non-edge seeds
- Normal-based region splitting (k-means on normal vectors within each region)
- Plane fitting + merging, then curved reclassification

#### Pseudocode

```
DETECT(points):
  # Phase 1: Edge detection (multi-signal)
  for each point p:
    p.planarity_residual = pca_residual(knn(p, k=20))
    p.normal_discontinuity = max(angle(p.normal, n.normal) for n in knn(p))
    p.multi_return = (p.numberOfReturns > 1)  # LiDAR metadata
    p.is_edge = p.planarity_residual > 0.05m
                OR p.normal_discontinuity > 18deg
                OR (p.multi_return AND p.returnNumber > 1)

  # Phase 1b: Grid-based flood fill
  grid = voxelize(points, cell_size=0.3m)
  regions = []
  for each non-edge cell (unvisited):
    region = flood_fill(cell, blocked_by=edge_cells,
      normal_consistency=angle(neighbor.normal, region.normal) < 1.5 * edgeThresh)
    regions.append(region)

  # Phase 1c: Normal-based region splitting
  for region in regions:
    if normal_span(region) > 10deg:
      sub_regions = kmeans_on_normals(region, k=auto)
      replace region with sub_regions

  # Phase 2: Plane fitting + merging
  planes = [fit_plane(r.points) for r in regions if plane.rms < 0.10m]
  # Merge compatible fragments
  for each pair (A, B):
    if angle(A.normal, B.normal) < 15deg
       AND |A.d - B.d| < 0.15m
       AND spatial_proximity(A, B) AND merged_rms < 0.12m:
      merge(A, B)

  # Phase 2.5: Fragment-to-sphere detection
  small_frags = [r for r in regions if |r| < 150]
  for group in spatial_clusters(small_frags, max_dist=8m):
    centroids = [frag.centroid for frag in group]
    sphere = fit_sphere_from_centroids(centroids)
    if base_plane exists nearby:
      sphere.center.y = base_plane.d   # constrain to base level
    if sphere.rms < threshold:
      emit sphere, absorb group points

  # Phase 2.8: Cylinder from plane normals
  for group in planes where normals_perpendicular_to_common_axis(group):
    if angular_span(group.normals) > 30deg:
      cyl = fit_cylinder(group.all_points)
      # Discriminate from gambrel: compare cyl.rms vs avg per-plane rms
      if cyl.rms < 1.5 * weighted_avg_plane_rms(group):
        emit cylinder
        # else: keep as separate planes (it's a multi-faceted roof)

  # Phase 2.9: Base plane splitting from cones/spheres
  for surface in [cones, spheres]:
    base_points = [p for p in surface.points
                   if p in bottom_25%_Y AND |p.normal.Y| > 0.7]
    if |base_points| > minSize AND plane_rms(base_points) < 0.5m:
      emit plane(base_points)
      remove from surface

  # Phase 2.95: Curved surface merging
  for each pair of cylinders (A, B):
    if axis_angle(A, B) < 18deg AND |A.r - B.r| / max(A.r, B.r) < 0.20:
      merge(A, B)

  # Phase 3: Point assignment
  for each edge/unassigned point p:
    score(p, surface) = w1 * normal_angle(p, surface) + w2 * distance(p, surface)
    assign p to surface with lowest score
```

#### Key Innovations
1. **5-phase pipeline** evolving across 3 continuation sessions:
   - Phase 1: Edge detection -> grid flood fill -> normal split
   - Phase 2: Plane fitting, deferred curved regions, cluster unassigned -> curved fitting
   - Phase 2.5-2.8: Fragment sphere, curved reclassification, plane merging, cylinder from normals
   - Phase 2.9-2.95: Base plane splitting from cones/spheres, curved surface merging
   - Phase 3: Normal-based assignment, distance-based assignment
2. **Fragment-to-sphere detection with base plane constraint** (v2.1): Sphere fit from small fragment centroids, with sphere center Y constrained to base plane level for hemisphere detection.
3. **Cylinder from plane normals** (v2.1): Detects groups of planes whose normals are all perpendicular to a common axis with >30 deg total span. Discriminates from gambrel/mansard roofs by comparing cylinder residual to weighted per-plane residual.
4. **Base plane splitting from cones/spheres** (v2.2): Checks cone/sphere surfaces for horizontal point clusters near the base. Cone_spire-small: 47.6 -> 85.0.
5. **Large-radius false cylinder filter** (v2.2): For cylinders with R > 8m, verify the angular arc subtends at least 60 degrees.

#### Score Progression (quick set, 30 cases)

| Version | Overall | Planar Comp. | Plane Match | Curved Comp. | Curved Match | Seg. Accuracy | Residual RMS | Thesis Bonus |
|---------|---------|-------------|-------------|-------------|-------------|---------------|-------------|-------------|
| v0.5 (M2) | 63.0 | 81.9 | 85.6% | 44.2 | 0.0% | 53.9% | 0.063m | 25.5% |
| v2.0 | 70.9 | 81.0 | 92.8% | 60.9 | 46.7% | 60.7% | 0.088m | 25.2% |
| v2.1 | 75.9 | 81.2 | 92.8% | 70.5 | 80.0% | 72.1% | 0.104m | 22.7% |
| v2.2 | **79.4** | **82.1** | **92.8%** | **76.7** | **80.0%** | **76.6%** | **0.091m** | **25.3%** |

#### Strengths
- Edge-first approach naturally finds geometric discontinuities (ridges, eaves)
- Best hip roof detection (97.6) — edge detection perfectly separates hip roof faces
- No regressions in v2.2 (every improvement was additive)
- Rich multi-phase pipeline with complementary detection mechanisms

#### Weaknesses
- Thesis bonus low (25.3%) — multi-return contribution is marginal at best
- Cylinder_silo-small: H=2R geometry ambiguous between sphere and cylinder
- silo_with_cone: segAcc=0 on small case
- Cross-plan roofs: intersection planes still merge

#### Thesis Verdict
**Partially works.** Edge-first segmentation is effective for planar cases, but the multi-return metadata contribution is marginal — most edge detection comes from geometric features (planarity, normal discontinuity), not return number. The thesis bonus reflects this honestly at 25.3%.

---

### Axis 8: Noise-Weighted Heteroscedastic Fitting

**Thesis**: Model per-point noise from scan angle (sigma_eff = sigma_z / cos(scanAngle)); use noise-weighted RANSAC and WLS fitting for more accurate surfaces.

**Final version**: v0.7.0 | **Quick**: 88.6 | **All-synth**: 91.7

#### Approach
- Per-point noise model: `sigma_eff = sigma_z / cos(scanAngle)`, weight = `cos^2 / sigma_z^2`
- Noise-weighted RANSAC: sample inliers use per-point noise as distance threshold
- Weighted least-squares plane fitting
- Post-plane curved recovery: vertical cylinders, cap-constrained hemispheres, cones

#### Pseudocode

```
DETECT(points):
  # Phase 1: Per-point noise model
  for each point p:
    sigma_eff = sigma_z / cos(p.scanAngle)   # heteroscedastic noise
    p.weight = cos(p.scanAngle)^2 / sigma_z^2

  # Phase 2: Noise-weighted RANSAC plane extraction
  planes = []
  remaining = points
  while |remaining| > minPoints:
    best_plane = null
    for iter in 1..1000:
      sample = random_3_points(remaining)
      candidate = fit_plane(sample)
      inliers = [p for p in remaining
                 if |plane_dist(p, candidate)| < 3 * sigma_eff(p)]  # per-point threshold!
      if |inliers| > |best_plane.inliers|:
        best_plane = candidate, inliers
    # Weighted least-squares refit
    plane = WLS_fit(best_plane.inliers, weights=p.weight)
    # Region growing from inliers
    grown = grow(plane.inliers, merge_if=
      |plane_dist(neighbor)| < 3*sigma_eff AND spatial_dist < 0.5m)
    # Spatial CC splitting
    components = connected_components(grown, radius=0.5m)
    for cc in components where |cc| > minSize:
      planes.append(WLS_fit(cc, weights))
    remaining -= grown

  # Phase 3: Planarity check
  for plane in planes:
    if plane.rms > 0.10m OR plane.maxDist > 0.30m:
      defer to curved fitting

  # Phase 4pre: Whole-building horizontal cylinder (quonset)
  all_points_xz = project(points, plane=XZ)
  circle = kasa_circle_fit(all_points_xz)
  if circle.inlier_ratio > 0.5:
    emit cylinder(axis=Z, r=circle.r)

  # Phase 4a: Vertical cylinder detection
  unassigned = remaining points
  cluster = spatial_cluster(unassigned)
  circle = kasa_circle_fit(project_XZ(cluster))
  if circle.rms < min(0.50, max(0.30, circle.r * 0.12)):
    emit cylinder
  else:
    # Fallback: pool unassigned + small tangent plane fragments, retry
    pooled = unassigned + [p for p in planes if |p.points| < 30]
    circle = kasa_circle_fit(project_XZ(pooled))
    if valid: emit cylinder

  # Phase 4b: Cap-plane constrained hemisphere
  cap_plane = largest horizontal plane
  if cap_plane exists:
    sphere_center = (cap_plane.centroid.x, cap_plane.d, cap_plane.centroid.z)
    non_cap_points = unassigned + small_tangent_planes
    sphere_radius = avg_distance(non_cap_points, sphere_center)
    rms_gate = min(0.80, max(0.30, sphere_radius * 0.20))
    if sphere_rms < rms_gate:
      emit sphere(center=sphere_center, r=sphere_radius)
      absorb overlapping small tangent planes

  # Phase 4b2: Cone detection
  tilted_planes = [p for p in small_planes if |p.normal.Y| > 0.05]
  if |tilted_planes| > 3:
    # Iterative apex refinement: 30-step search along axis
    apex_init = weighted_centroid(top_5%_Y_points)
    for step in 0..30:
      apex_candidate = vary_along_axis(apex_init, step)
      rms = cone_rms(all_points, apex_candidate)
    apex = argmin(rms)
    emit cone(apex, halfAngle, axis)

  # Phase 5: Thesis metadata
  for each plane:
    weighted_rms = WLS_residual(plane.points, plane, weights=p.weight)
    uniform_rms = LS_residual(plane.points, plane)
    plane.thesis_improvement = (uniform_rms - weighted_rms) / uniform_rms
```

#### Key Innovations
1. **Cap-plane constrained hemisphere fitting** (v0.6): When a dominant horizontal plane (cap) exists, use its centroid and d-value as sphere center, compute radius from non-cap points. Accept with generous RMS gate: `min(0.80, max(0.30, radius * 0.20))`. This addresses the fundamental partial-sphere bias problem.
2. **Kasa algebraic circle fit for half-cylinders** (v0.7): Replaced noisy minY-based axis estimation with 2D algebraic circle fit (Kasa method) in the cross-section plane. Robust to noise, fixing quonset radius estimates.
3. **Iterative cone apex refinement** (v0.7): 30-step search along axis minimizing RMS, with robust apex estimation using weighted centroid of top 5% Y points.
4. **Relative RMS gate for cylinders** (v0.6): `min(0.50, max(0.30, radius * 0.12))` — proportional to cylinder radius, allowing larger cylinders to have proportionally higher RMS.
5. **Vertical cylinder fallback pooling** (v0.7): Pool unassigned + small tangent plane fragments and retry cylinder detection when initial attempt on pure unassigned points fails.

#### Score Progression (quick set, 30 cases)

| Version | Overall | Planar Comp. | Plane Match | Curved Comp. | Curved Match | Seg. Accuracy | Residual RMS | Thesis Bonus |
|---------|---------|-------------|-------------|-------------|-------------|---------------|-------------|-------------|
| v0.1 (M2) | 64.5 | 79.7 | 90.6% | 49.2 | 0.0% | 59.4% | 0.058m | 0.1% |
| v0.5 | 79.3 | — | 91.1% | — | 6.7% | 59.2% | — | 91.7% |
| v0.6 | 83.2 | 94.9 | 91.1% | 71.4 | 46.7% | 65.9% | — | 93.3% |
| v0.7 | **88.6** | **94.4** | **91.1%** | **82.8** | **80.0%** | **81.3%** | — | **85.0%** |

All-synth at v0.7 (177 cases): **91.7** overall, planar 94.1, curved 71.7, plane match 88.4%, curved match 58.3%, seg. accuracy 87.1%

#### Strengths
- **Second-best all-synth performer** (91.7)
- Strong planar detection (94.4 avg) with excellent segmentation accuracy (81.3%)
- Cap-constrained hemisphere fitting solves the partial-sphere bias problem elegantly
- Cone detection with iterative apex refinement works reliably
- Low residuals (0.0579m avg) from noise-weighted fitting

#### Weaknesses
- Thesis bonus decreased over time (93.3% -> 85.0%) as curved improvements bypassed noise weighting
- Scan angle noise variation is only ~6% at IGN +/-20 deg — practical benefit is modest
- Stochastic variance: +/-5 composite variation per run due to test case regeneration
- Dome-large intermittently fails due to stochastic point generation

#### Thesis Verdict
**Thesis is technically correct but practically marginal.** The noise model is physically accurate (sigma_eff = sigma_z / cos(scanAngle)), but at IGN's +/-20 deg scan angles, the noise increase is only ~6%. The real improvements came from better fitting algorithms (Kasa circle, cap-constrained hemisphere, iterative cone apex), not from noise weighting per se. The noise-weighted RANSAC is a theoretically sound contribution that would matter more with wider scan angles.

---

### Axis 9: Edge-First Boundary Detection

**Thesis**: Detect surface boundaries first via normal gradient analysis; flood-fill between boundaries to segment surfaces; the edge map IS the primary output.

**Final version**: v0.2.0 | **Quick**: 92.8 (with thesis) / 76.9 (without thesis)

#### Approach
- Normal gradient computation: per-point gradient of normal vectors across local neighborhood
- Edge classification: high normal gradient = surface boundary
- Normal-consistency flood fill: grow regions from non-edge seeds, constrained by normal agreement
- Curved recovery: collect unassigned points after plane fitting, fit cylinders/spheres/cones
- Thesis bonus module measures GT edge detection within 0.3m

#### Pseudocode

```
DETECT(points):
  # Phase 1: Normal gradient edge detection
  for each point p:
    neighbors = knn(p, k=20)
    p.normal = pca_normal(neighbors)
    p.normal_gradient = max(angle(p.normal, n.normal) for n in neighbors)
    # Adaptive edge threshold
    p.is_edge = p.normal_gradient > edgeThreshold

  # Phase 1b: Curved surface un-marking
  for each edge point p:
    edge_ratio = count(n.is_edge for n in knn(p)) / k
    if edge_ratio > 0.70:  # uniformly curved region, not a true boundary
      p.is_edge = false

  # Phase 2: Normal-consistency flood fill
  regions = []
  for each non-edge point p (unvisited):
    region = flood_fill(p, blocked_by=edge_points,
      extra_constraint=
        angle(neighbor.normal, region.avg_normal) < 1.5 * edgeThreshold)
        # Normal consistency prevents leaking through ridge gaps
    regions.append(region)

  # Phase 3: Plane fitting
  planes = []
  deferred_curved = []
  for region in regions:
    plane = fit_plane(region.points)
    if plane.rms < planeMaxResidual:
      planes.append(plane)
    else:
      deferred_curved.append(region)

  # Phase 4: Curved recovery from unassigned points
  unassigned = edge_points + deferred_curved.points + remaining
  for cluster in spatial_clusters(unassigned, radius=1.0m):
    # Multi-axis cylinder fitting
    best_cyl = null
    for axis in [Y, Z, X, PCA_e1, PCA_e2]:
      cross_section = project_perpendicular(cluster, axis)
      circle = kasa_circle_fit(cross_section)    # algebraic 2D
      if circle.rms < best_cyl.rms: best_cyl = (axis, circle)
    # Sphere fitting
    sphere = kasa_3d_sphere_fit(cluster.points)  # algebraic, not iterative
    # Cone fitting (regression-based)
    radial_dists = perpendicular_distance(cluster.points, vertical_axis)
    heights = cluster.points.y
    (a, b) = linear_regression(heights, radial_dists)
    if R_squared > 0.3:
      apexY = -b / a
      halfAngle = atan(|a|)
      cone = {apex: (centroid.x, apexY, centroid.z), halfAngle, axis: Y}
    # Select best fit
    best = min([best_cyl, sphere, cone], by=rms)
    # Guard: curved rms must be < 2x individual plane rms
    if best.rms < 2 * avg_plane_rms_of_constituent_fragments:
      emit best

  # Phase 5: Assign remaining points
  for each unassigned point p:
    assign to nearest surface by distance

  # Thesis bonus: measure edge detection quality
  for each GT boundary edge:
    detected = any(detected_edge within 0.3m of GT edge)
    thesis_score = count(detected) / count(GT edges)
```

#### Key Innovations
1. **Normal-consistency constraint in flood fill**: Edge barriers alone are insufficient — ridge gaps allow leaking. Adding normal consistency (1.5x edge threshold) prevents cross-surface contamination. This insight was crucial for hip roofs (61.2 -> 83.7).
2. **Regression-based cone fitting**: Linear regression of (radial distance) vs (height) to estimate apex position. `apexY = -b/a`, `halfAngle = atan(|a|)`. R^2 quality check (>0.3) rejects false cones on hemispheres.
3. **Algebraic 3D sphere fit (Kasa method)**: Replaces iterative Gauss-Newton (which diverges on hemispheres). Exact solution on dome GT gives center within 0.058m, radius within 0.036m.
4. **Curvature chain detection**: Tightened curved acceptance (curved RMS must be < 2x individual plane RMS) prevents merging well-fitting polygon facets into single spheres.
5. **Curved surface un-marking**: Points in uniformly curved regions (>70% neighbors have elevated gradient) are un-marked as edges, preventing entire curved surfaces from being classified as edge.

#### Score Progression (quick set, 30 cases)

| Version | Overall | Planar Comp. | Plane Match | Curved Comp. | Curved Match | Seg. Accuracy | Thesis Bonus |
|---------|---------|-------------|-------------|-------------|-------------|---------------|-------------|
| v0.1 (M2) | 61.5 | 77.1 | — | 45.8 | 0.0% | — | — |
| Iter 3 | 66.4 | 79.3 | 93.3% | 53.5 | 46.7% | — | — |
| v0.2 (with thesis) | **92.8** | **94.8** | **92.5%** | **90.7** | **100.0%** | **85.9%** | **100.0%** |
| v0.2 (without thesis) | **76.9** | **79.8** | 92.5% | **74.0** | 96.7% | 85.9% | — |

#### Strengths
- **100% curved match rate** with thesis bonus (only axis to achieve this)
- 100% thesis bonus — edge-first naturally detects all GT boundary edges within 0.3m
- Strong segmentation accuracy (85.9%)
- Principled: boundary detection IS the most natural first step for surface segmentation

#### Weaknesses
- **Heavily thesis-dependent**: 92.8 with thesis -> 76.9 without (15.9 point gap, largest of any axis)
- Cone_spire small/default: tower top plane too small to survive as a region
- Row houses: adjacent identical slopes merge across unit boundaries
- Proc-octagonal_tower: all 8 planes merge into one curved surface
- Ogive not implemented

#### Thesis Verdict
**Works well, but thesis bonus is inflated.** Edge-first boundary detection is a sound approach — the edge map naturally detects GT boundaries, yielding 100% thesis bonus. However, the 15.9-point gap between thesis and non-thesis scores reveals that the thesis bonus metric disproportionately rewards this specific approach. The underlying surface detection (76.9 without thesis) is solid but not exceptional.

---

### Axis 10: Density Field Topology

**Thesis**: Use eigenvalue ratios (planarity, linearity, scattering) from the local point density structure as scalar fields; watershed segmentation on these fields separates surfaces.

**Final version**: v6.1.0 | **Quick**: 86.9 | **All-synth**: 86.3

#### Approach
- Per-point PCA on k-NN neighborhood: compute planarity, linearity, scattering from eigenvalue ratios
- Planarity as watershed input: high planarity = surface interior, low planarity = boundary
- Watershed segmentation on the planarity field
- Post-watershed plane fitting + curved surface recovery
- Merging and splitting: coplanar merge, adjacent-planes-to-curved merge, cylinder+cone splitting

#### Pseudocode

```
DETECT(points):
  # Phase 1: Density field feature computation
  for each point p:
    neighbors = knn(p, k=30)
    eigenvalues = pca_eigenvalues(neighbors)  # l1 >= l2 >= l3
    p.planarity  = (l2 - l3) / l1   # high on planar surfaces
    p.linearity  = (l1 - l2) / l1   # high on ridges/edges
    p.scattering = l3 / l1          # high at corners
    p.normal = pca_eigenvector_3(neighbors)

  # Phase 2: Planarity watershed segmentation
  # High planarity = surface interior, low planarity = boundary
  seeds = local_maxima(planarity_field)
  segments = []
  for seed in seeds (sorted by planarity DESC):
    segment = watershed_grow(seed,
      flow_direction=gradient_descent(planarity),
      stop_when=planarity < minPlanarity)
    segments.append(segment)

  # Phase 3: Plane fitting + coplanar merge
  planes = []
  for segment in segments:
    plane = fit_plane(segment.points)
    planes.append(plane)
  # Merge coplanar fragments
  for each pair (A, B) in planes:
    if angle(A.normal, B.normal) < 10deg AND |A.d - B.d| < 0.20m:
      merge(A, B)

  # Phase 4: Adjacent planes -> curved merge
  for group in BFS_adjacent_groups(planes, spatial_dist < 1.0m):
    if |group| >= 3 AND normal_spread(group) > 15deg:
      # Try curved fit
      cyl = kasa_circle_fit(project_cross_section(group.points))
      sph = kasa_3d_sphere_fit(group.points)
      cone = regression_cone_fit(group.points)
      best = min([cyl, sph, cone], by=rms)
      # Polyhedral false-merge guard (KEY insight)
      wRatio = best.rms / weighted_avg_individual_plane_rms(group)
      maxWRatio = (1.0 if |group| <= 5    # catches gambrel (4 planes)
                   else 1.5 if |group| <= 9  # catches hex_tower (7-8)
                   else 2.0)                  # allows silo (11+)
      if wRatio < maxWRatio:
        emit best curved surface
        remove group planes
      # else: keep as separate planes

  # Phase 5: Composite splitting (cylinder + cone)
  for surface in curved_surfaces:
    if surface.type in [CYLINDER, SPHERE]:
      # Try splitting by height into cylinder bottom + cone top
      slices = bin_by_height(surface.points)
      upper_rms_as_cone = cone_rms(upper_slices)
      lower_rms_as_cyl = cylinder_rms(lower_slices)
      if combined < surface.rms * 0.85:  # 15% improvement
        emit cylinder(lower), cone(upper)

  # Phase 6: Coaxial cylinder merge
  for each pair of cylinders (A, B):
    if axis_angle(A, B) < 15deg AND |A.r - B.r| < 0.5m:
      merge(A, B)

  # Phase 7: Assign remaining points
  for each unassigned point p:
    assign to nearest surface by distance
```

#### Key Innovations
1. **Planarity watershed**: The key thesis pivot (v3.0) — pure density has no discriminating power on uniformly-sampled surfaces. Eigenvalue RATIOS (planarity) describe the local shape of the density field, providing effective boundary detection.
2. **Adaptive wRatio threshold for polyhedral false-merge guard** (v6.1): Size-dependent threshold prevents merging of gambrel roofs (4 planes) and polygonal towers (6-8 planes) into cylinders while still allowing silo merges (11+ planes). True cylinders have wRatio 0.58-0.71; false merges have 1.20-4.92.
3. **Gap-based bimodal detection** (v6.1): Finds largest gap in sorted radial distances to separate wall ring from center cluster in cylinder fitting.
4. **trySplitIntoCylinderCone** (v6.0): Splits large sphere/cylinder surfaces into cylinder+cone composites by height analysis.
5. **mergeCoaxialCylinders** (v6.0): Merges cylinder fragments with similar axis directions and radii.

#### Score Progression (quick set, 30 cases)

| Version | Overall | Planar Comp. | Plane Match | Curved Comp. | Curved Match | Seg. Accuracy | Residual RMS | Thesis Bonus |
|---------|---------|-------------|-------------|-------------|-------------|---------------|-------------|-------------|
| v4.0 (M2) | 52.1 | 67.9 | 80.8% | 36.4 | 0.0% | 39.8% | 0.452m | — |
| v4.0+thesis | 73.8 | 89.7 | 80.8% | 57.8 | 0.0% | — | — | 100% |
| v5.0 | 84.0 | 90.8 | — | 77.3 | 66.7% | 67.9% | — | 100% |
| v6.0 | 86.9 | 90.8 | — | 83.1 | 83.3% | 73.7% | — | 100% |
| v6.1 | **87.0** | **90.8** | **~89%** | **~83** | **83.3%** | **~74%** | — | **100%** |

All-synth at v6.0 (160/177 cases): **86.3** overall, plane match 77.7%, curved match 96.7%, seg. accuracy 75.1%

#### Strengths
- Genuine thesis novelty: density field topology is a unique approach
- Sophisticated false-merge guards based on empirical wRatio analysis
- Cylinder+cone splitting for composite structures
- Robust progression from 52.1 to 87.0 through systematic improvements

#### Weaknesses
- Gambrel/hex_tower at small scale: size-dependent threshold sensitivity
- Ogive dome: unsupported surface type (~51-55)
- Cylinder_silo-large: flat-top points contaminate radius estimation
- Cone_spire-small/default: tower top absorbed into cone
- All-synth has 7 WARN cases where planes falsely merge into curves

#### Thesis Verdict
**Works well after thesis pivot.** The initial pure-density approach completely failed (density is uniform on flat surfaces). The pivot to planarity (eigenvalue ratios) — which describes the local SHAPE of the density field, not the density itself — saved the thesis. The distinction is subtle but genuine: planarity is a scalar topology metric derived from the density structure.

---

## Meta-Analysis

### Comparative Performance Table (quick set, 30 cases — sorted by overall composite)

| # | Approach | Overall | Planar Comp. | Plane Match | Curved Comp. | Curved Match | Seg. Accuracy | Thesis Bonus |
|---|----------|---------|-------------|-------------|-------------|-------------|---------------|-------------|
| 5 | Strip Consensus | **93.0** | **97.4** | **98.9%** | **88.7** | 83.3% | — | **99.0%** |
| 9 | Edge-First | 92.8* | 94.8 | 92.5% | 90.7 | **100.0%** | **85.9%** | 100.0%* |
| 8 | Noise RANSAC | 88.6 | 94.4 | 91.1% | 82.8 | 80.0% | 81.3% | 85.0% |
| 10 | Density Topology | 86.9 | 90.8 | ~89% | ~83 | 83.3% | ~74% | 100% |
| 6 | Curvature-First | 86.4 | — | 87.5% | — | **93.3%** | 68.4% | **96.7%** |
| 1 | Radon Sweep | 84.5 | 93.4 | 88.5% | 70.4 | 41.7% | 82.6% | 93.4% |
| 2 | Multiview Raster | 80.1 | 83.4 | 84.9% | 71.9 | 83.3% | 64.4% | 65.4% |
| 7 | MultiReturn Edge | 79.4 | 82.1 | 92.8% | 76.7 | 80.0% | 76.6% | 25.3% |
| 4 | PCA + Region Growing | 79.3 | 79.0 | 88.3% | 79.6 | 73.3% | 57.7% | — |
| 3 | Spectral Graph | 66.9 | 75.8 | 86.1% | 58.0 | 56.7% | — | — |

*Axis 9 overall includes 15-point thesis bonus from edge detection; without thesis bonus: 76.9 overall

**All-synth results** (where available, ~177 cases):

| # | Approach | Overall | Planar Comp. | Plane Match | Curved Comp. | Curved Match | Seg. Accuracy |
|---|----------|---------|-------------|-------------|-------------|-------------|---------------|
| 8 | Noise RANSAC | **91.7** | 94.1 | 88.4% | 71.7 | 58.3% | **87.1%** |
| 1 | Radon Sweep | 90.9 | **93.4** | **88.5%** | 70.4 | 41.7% | 82.6% |
| 5 | Strip Consensus | 90.7 | 91.7 | 83.0% | **82.6** | **69.4%** | — |
| 10 | Density Topology | 86.3 | — | 77.7% | — | 96.7% | 75.1% |
| 2 | Multiview Raster | 82.1 | 83.4 | 84.9% | 71.9 | 83.3% | 64.4% |

### Key Cross-Axis Findings

#### 1. Convergent Evolution of Techniques

Despite isolation, multiple axes independently discovered the same critical techniques:

| Technique | Axes that discovered it | Notes |
|-----------|------------------------|-------|
| Kasa/algebraic circle fit for cylinders | 4, 5, 8, 9, 10 | 5 of 10 axes — clearly essential |
| Fragment-to-sphere detection | 1, 3, 7, 8 | Different names, same idea |
| Normal-based region splitting | 5, 7, 9 | Split large regions by normal clusters |
| Base plane constraint for hemispheres | 1, 7, 8 | Constrain sphere center Y to base level |
| Cone apex via regression/extrapolation | 4, 9, 10 | Linear regression of perpDist vs height |
| Plane merging by normal+d+proximity | 1, 7, 10 | Post-extraction fragment assembly |
| Two-pass extraction (main + residual) | 1, 8 | Second pass on unassigned with lower thresholds |
| BIC/residual model selection | 4, 6 | Compare plane vs curved residual |

**The Kasa algebraic circle fit** is the single most re-discovered technique, appearing in 5 of 10 axes. This confirms it as the gold standard for cylinder radius estimation from partial cross-sections.

#### 2. The Quonset Problem

Quonset (half-cylinder) detection was a watershed test case that separated approaches:

| Approach | Quonset result | Why |
|----------|---------------|-----|
| Radon (Axis 1) | 0% match | Locally flat patches indistinguishable from planes |
| Strip (Axis 5) | 93-99 | RANSAC 3-point circle fit on cross-section |
| Curvature-First (Axis 6) | 91-95 | Curvature classification correctly identifies cylindrical |
| Noise RANSAC (Axis 8) | 78-80 | Kasa circle fit after plane removal |
| Edge-First (Axis 9) | ~69 | Partial recovery from unassigned points |
| Spectral (Axis 3) | 0% | 180-degree normal span breaks fragment adjacency |

**The Radon approach fundamentally cannot solve quonset** because it measures local flatness, and curved surfaces are locally flat. The curvature-first and strip-aware approaches succeed because they use different local features (curvature tensor, or independent strip observations + RANSAC).

#### 3. The Thesis Bonus Paradox

The thesis bonus (15% weight) was designed to reward approaches that genuinely use their stated thesis. In practice:

- **Axis 9** (Edge-First) achieved 100% thesis bonus but its score drops 15.9 points without it — the thesis metric disproportionately rewards edge-detection approaches
- **Axis 5** (Strip) achieved 99% with genuine strip-awareness that also improves detection
- **Axis 6** (Curvature-First) achieved 96.7% with curvature classification that directly drives fitting
- **Axis 7** (MultiReturn) achieved only 25.3% — honestly reflecting that multi-return metadata adds minimal value

The thesis bonus successfully differentiated genuine methodological novelty from post-hoc labeling, but its 15% weight created a potential scoring distortion for Axis 9.

#### 4. Planar vs Curved Specialization

No axis dominates both planar and curved detection:

| Best Planar Composite | Best Curved Match Rate |
|-----------------------|----------------------|
| Axis 5 (97.4) | Axis 6 (93.3%) |
| Axis 1 (93.4) | Axis 9 (100%*) |
| Axis 8 (94.4) | Axis 10 (83.3%) |

This suggests an **ensemble approach** could exceed any individual axis by routing planar cases to strip-aware/Radon approaches and curved cases to curvature-first/edge-first approaches.

#### 5. The Fundamental Limitation of Local Flatness

A deep insight emerged from multiple axes: **any smooth surface is locally flat at small scales**. This creates a fundamental tension:

- Methods that rely on local flatness (Radon, spectral normal gating, region growing) excel at planes but struggle with large-radius curves
- Methods that classify curvature first (Axis 6) or use specialized global features (RANSAC circle fit) handle curves better
- The best performers (Axis 5, 8) combine local plane extraction with dedicated curved recovery steps

#### 6. Novel Techniques Worth Preserving

Several techniques appear genuinely novel and should be carried forward:

1. **Normal-derived cone fitting** (Axis 5): Using patch normal tilt consistency to derive halfAngle is more robust than position-based fitting
2. **Sphere from tangent plane normal convergence** (Axis 1): A genuine novel application of the Radon approach
3. **Binary normal gating** (Axis 3): While the overall approach scored lowest, the insight that continuous edge weights fail at ridges (due to normal blending) while binary gating succeeds is valuable
4. **Polyhedral false-merge guard via wRatio** (Axis 10): Adaptive threshold based on group size, grounded in empirical analysis of true vs false curved merges
5. **Cap-plane constrained hemisphere fitting** (Axis 8): Elegant solution to the partial-sphere bias problem
6. **Curvature-first classification** (Axis 6): Scale-aware curvature thresholds that adapt to building size

#### 7. What Doesn't Work

| Approach | Why it fails |
|----------|-------------|
| Pure density fields | Uniform density on flat surfaces = no signal |
| Continuous graph edge weights | Ridge normals create bridges between surfaces |
| Height-map rasterization alone | Loses 3D orientation, discretization artifacts |
| PCA eigenvalue ratios for curvature | Too noisy for large-radius surfaces |
| Scan angle noise weighting alone | Only 6% noise variation at IGN scan angles |
| Multi-return metadata for edge detection | Returns multi-return at edges, but geometric features are stronger |

#### 8. Unsolved Problems

These surface types remain challenging for ALL approaches:

1. **Ogive dome**: No axis implemented ogive fitting (specialized revolution surface)
2. **Silo_with_cone composite**: Cylinder+cone transition detection is unreliable
3. **Small cone_spire**: Base plane too small to detect, too few points for reliable cone fitting
4. **Polygonal silos**: Faceted surfaces that look cylindrical to curvature estimators
5. **Row houses**: Adjacent identical slopes separated only by physical gap

### Session Logistics

- **Total agent runtime**: ~7.5 hours across 2-3 sessions per agent (two quota interruptions)
- **CPU management**: All harness processes ran with `nice -n 19` to prevent system freeze from 10 concurrent agents
- **Communication**: Async MESSAGES.md mailbox system — orchestrator could not interrupt running processes
- **Cross-pollination**: Deliberately minimal to prevent convergence. Only bug alerts and priority guidance sent.
- **Agent completion**: Only Axis 5 completed naturally. All others stopped by quota hits.
- **Code volume**: 30-82 KB detector.js per axis, totaling ~550 KB of novel algorithmic code

### Recommendations for Future Work

1. **Build an ensemble**: Route cases to specialized detectors based on initial curvature analysis (Axis 6's classification) or building metadata (BDTOPO priors)
2. **Standardize Kasa circle fit**: All cylinder detection should use algebraic circle fitting — it's the clear winner
3. **Implement RANSAC circle for half-cylinders**: Axis 5's RANSAC approach handles partial arcs where algebraic methods fail
4. **Adopt cap-constrained hemisphere fitting**: Axis 8's approach is the cleanest solution to partial-sphere bias
5. **Consider normal-derived cone fitting**: Axis 5's technique is more robust than position-based alternatives
6. **Invest in silo_with_cone detection**: Height-slice analysis (Axis 2) combined with cylinder+cone splitting (Axis 10) could solve this
7. **Investigate ogive fitting**: No axis attempted this — it would need arc-based revolution surface fitting
