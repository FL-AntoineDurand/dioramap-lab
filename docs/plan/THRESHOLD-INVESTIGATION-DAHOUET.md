# V7 Threshold Derivation Audit + Dahouet Shared-Edge Investigation

**Status:** in progress, paused. Temporary investigation document — not a
spec, not a finalized plan. Designed so anyone (human or agent) can pick up
where it left off.

## Original observation

Lab UI (v3 prod) on the `real-22186-dahouet-502` test case produces
**7 shared edges** at `v7-boundary-merge`. v2's v7 pipeline on the same
test case produces **14 shared edges** when the full 45696-point tile is
loaded, **11 shared edges** when only the 16092-point building extract is
loaded as a separate bldgonly TCBD.

This document collects the facts measured while investigating where in the
pipeline v3 and v2 diverge, and what architectural cleanup was done along
the way. **It records measurements, not conclusions.**

---

## 1. Test cases used

- **v2 full**: `data/test-cases/real-22186-dahouet-502.json`
  + `.bin` sidecar. 45696 points, F32 positions in v2's y-up local frame.
  `metadata.l93_center = (289554.800200622, 6845192.01803264)`.

- **v2 bldgonly**: `data/test-cases/real-22186-dahouet-502-bldgonly.{json,bin}`.
  16092 points. Constructed by subsetting `v2 full` to the building +5m-buffer
  indices that v3 prod's `extract-buildings` selects. Same physical points as
  v3 prod operates on; v2's y-up local frame uses the same
  `metadata.l93_center`.

- **v3 prod input**: `data/test-cases/real-22186-dahouet-502.json`
  (via `v7-load-real-test-case`, which reads the same TCBD as v2 full but
  rotates positions to z-up Lambert93 world frame). After `extract-buildings`
  v3 operates per-bldg on 16092 points.

---

## 2. Build & run commands (workflow)

### v3 build

```bash
# Working build dir
cd build-dev
ninja          # incremental
ninja clean    # full rebuild if needed
```

CMake source tree is `core` (`add_library(dioramap_steps_v7 ...)` at line 836).
New step `.c` files must be added to that library list.

### v3 prod pipeline run

```bash
mkdir -p /tmp/v3-out
rm -rf /tmp/v3-cache && mkdir /tmp/v3-cache  # always use a fresh cache while
                                              # iterating on code

build-dev/dioramap-processor batch \
  --pipeline pipelines/smoke/research-v7-tile-signals-real.json \
  --input-json '{}' \
  --produce boundary-merge-roof-graph,grm-owner,grm-plane-equations,bldg-thresholds,boundary-signals-roof-graph \
  --output-dir /tmp/v3-out \
  --cache-dir /tmp/v3-cache
```

`--produce` accepts a comma-separated list of artifact names; output files
are written as `<artifactName>.bin`. The pipeline JSON pins the dahouet
test case.

### v2 dump (full pipeline + artifact serialization)

```bash
cd lidar-algos-bench-v2

node scripts/dump-v7-refs.js \
  --case test-cases-v2/real-22186-dahouet-502.json \
  --pipeline v7 \
  --out /tmp/v7-refs-fresh

# bldgonly equivalent
node scripts/dump-v7-refs.js \
  --case test-cases-v2/real-22186-dahouet-502-bldgonly.json \
  --pipeline v7 \
  --out /tmp/v7-refs-bldgonly
```

`/tmp/v7-refs-bldgonly/` is the reference dump used for byte comparisons
against v3 prod (same 16092 points). Runtime ~12 minutes (full pipeline).

### v3 bench harnesses (run v3 algo on v2 inputs)

Each `v7-<step>-bench` binary takes a v2 refs dir and writes outputs to
`<out-dir>` for byte comparison against v2's dumps.

```bash
build-dev/v7-grow-refit-merge-bench /tmp/v7-refs-bldgonly /tmp/v3-grm-bench
build-dev/v7-boundary-merge-bench   /tmp/v7-refs-bldgonly /tmp/v3-bm-bench
# ... etc for tile-signals, hemisphere-segment, normal-cascade-seed,
#                       density-voids-2, boundary-signals, boundary-corners,
#                       boundary-polygons
```

Bench harnesses respect `UP_AXIS` env var (default 1 = y-up bench frame).

### DERIV instrumentation

```bash
# v2 — env-gated console.error("[DERIV] ...") at 9 derivation sites
DERIV_LOG=1 node scripts/dump-v7-refs.js --case ... --out ... 2>deriv-v2.log

# v3 — env-gated fprintf(stderr, "[DERIV] ...") at every derivation site
DERIV_LOG=1 build-dev/dioramap-processor batch ... 2>deriv-v3.log

# Extract
grep '\[DERIV\]' deriv-v2.log
grep '\[DERIV\]' deriv-v3.log
```

The DERIV lines are logging-only — they have no effect on outputs.

---

## 3. Architectural changes shipped in working tree

These are clean wiring improvements. They were validated to not change
output beyond what they claim (see section 5 measurements).

### v7-derive-thresholds (new step)

- `v3/steps/v7/v7-derive-thresholds.{c,h}` — new.
- Reads `per-building-records`, `building-local-frames`.
- Writes `bldg-thresholds` (per-bldg ARRAY_INLINE, stride=48):
  - `density` = `n_pts / horizontal-bbox-area` (per-bldg, formula mirrors
    v2 `tile-signals.js#L242-L248`)
  - `spacing` = `1 / sqrt(density)`
  - `z_noise` = 0.02 (v2 `tile-signals.js#L249` constant)
  - `dist_threshold_seed` = 0.09 (= `max(0.05, 4.5 · z_noise)`)
  - `dist_threshold_grm`  = 0.12 (v2 `grow-refit-merge.js:72` hardcode)
- Registered in `v3/steps/v7/v7-steps.{c,h}` and added to
  `v3/core/CMakeLists.txt:838`.
- Inserted in `v3/pipelines/smoke/research-v7-tile-signals-real.json` after
  `build-building-kd-trees`.

### Consumers migrated

- `v3/steps/v7/v7-grow-refit-merge.c`: reads `bldg-thresholds.density` and
  `dist_threshold_grm` per-bldg when sentinel `params.density <= 0` /
  `params.dist_threshold <= 0`. Version bumped to 3.
- `v3/steps/v7/v7-density-voids-2.c`: reads `bldg-thresholds.density` and
  `dist_threshold_seed` per-bldg. Version bumped to 2.
- `v3/steps/v7/v7-boundary-topology.c`: reads `bldg-thresholds.spacing`
  per-bldg. Version bumped to 2.
- `v3/steps/v7/v7-tile-signals.c`: emits `ts-lidar-density` (was missing
  entirely before). Version bumped to 4. **Note:** the artifact is emitted
  but no v3 step reads it — `bldg-thresholds` superseded it. Kept for
  potential debug/viz consumers.

### Why per-bldg

v2's pipeline operates tile-wide; its `lidar-density` is the tile-wide
density. v3 prod's `extract-buildings` reduces the working set to per-bldg
(16092 pts on dahouet) before downstream steps. Per-bldg density values
calibrate search radii to the actual operating-set point density.

For dahouet, per-bldg density = 19.872 pts/m² — identical to v2's
tile-wide density on the bldgonly TCBD (same 16092 points = same bbox =
same density).

---

## 4. Diagnostic instrumentation in working tree

### v2 — logging-only

DERIV instrumentation added to 9 sites in v2:
- `lib/pipeline/steps/tile-signals.js` (after density compute)
- `lib/pipeline/steps/normal-cascade-seed.js` (after distThreshold derive)
- `lib/pipeline/steps/density-voids-2.js` (after spacing+distThreshold)
- `lib/pipeline/steps/grow-refit-merge.js` (after distThreshold hardcode)
- `lib/pipeline/steps/boundary-signals.js` (after spacing derive)
- `lib/pipeline/steps/planes-from-owner.js`
- `lib/pipeline/steps/adjacency-edges.js`
- `lib/pipeline/steps/snap-edges.js`
- `lib/pipeline/steps/compute-wall-edges.js`
- `lib/pipeline/steps/boundary-regularize.js`

Each adds exactly one `if (process.env.DERIV_LOG) console.error("[DERIV] ...")`
line, after the existing computation. No semantic change.

### v3 — logging-only

DERIV instrumentation added in v3 step wrappers, all gated on
`getenv("DERIV_LOG")`. Each adds one `fprintf(stderr, "[DERIV] ...")` at
step entry. No semantic change.

---

## 5. Measured facts

Each block records what was measured and how to reproduce. **No causal
claims here** — just measurements.

### 5.1 Threshold-derivation values per step on dahouet

| Step | v2 bldgonly | v3 prod | Source for v3 |
|---|---|---|---|
| `tile-signals` density | 19.872 | 30.475 (tile, 45696 pts) | `n / (Δx·Δy)` over 45696 |
| `normal-cascade-seed` density | 19.872 (read) | — (not read) | — |
| `normal-cascade-seed` distThreshold | 0.09 (=4.5·0.02) | 0.09 (const) | `v7_seed_params_default` |
| `density-voids-2` density | 19.872 | 19.872 | `bldg-thresholds` |
| `density-voids-2` spacing | 0.224 | — (computed inside impl) | — |
| `density-voids-2` distThreshold | 0.09 | 0.09 | `bldg-thresholds` |
| `grow-refit-merge` density | 19.872 | 19.872 | `bldg-thresholds` |
| `grow-refit-merge` distThreshold | 0.12 (hardcoded in v2) | 0.12 | `bldg-thresholds` |
| `boundary-topology` spacing | (inlined in v2 boundary-signals) | 0.224 | `bldg-thresholds` |
| `boundary-signals` spacing | 0.224 | — (no spacing usage in v3 signals) | — |
| `boundary-signals` plane_dist_filter | 0.20 (`PLANE_DIST_FILTER`) | 0.20 | `v7_bs_params_default` |
| `boundary-signals` corner_dist_threshold | 1.0 | 1.0 | `v7_bs_params_default` |
| `boundary-signals` corner_merge_threshold | 0.25 | 0.25 | `v7_bs_params_default` |
| `boundary-signals` anchor_dedup_radius | 0.40 (`DEDUP_RADIUS`) | 0.40 | `v7_bs_params_default` |
| `boundary-signals` grid_cell_size | 0.10 | 0.10 | `v7_bs_params_default` |
| `boundary-signals` nearby_radius | 2.0 | 2.0 | `v7_bs_params_default` |
| `boundary-corners` snap_radius | 0.30 | 0.30 | `v7_bc_params_default` |
| `boundary-corners` wall_tilt_min_deg | 72.0 | 72.0 | `v7_bc_params_default` |
| `boundary-corners` wall_side_margin | 0.30 | 0.30 | `v7_bc_params_default` |
| `boundary-corners` wall_min_h_extent | 0.50 | (n/a in v2 boundary-corners.js — not exposed as named const) | `v7_bc_params_default` |
| `boundary-corners` pool_merge_dist | 0.20 | 0.20 | `v7_bc_params_default` |
| `boundary-corners` coplanar_dist_threshold | 0.12 | 0.12 | `v7_bc_params_default` |
| `boundary-corners` ridge_side_max_dist | 1.5 | 1.5 | `v7_bc_params_default` |
| `boundary-polygons` anchor_dedup_radius | 0.40 (`ANCHOR_DEDUP`) | 0.40 | `v7_bp_params_default` |
| `boundary-polygons` graph_score_min | 0.15 | 0.15 | `v7_bp_params_default` |
| `boundary-polygons` delete_wsd_max | 0.50 | 0.50 | `v7_bp_params_default` |
| `boundary-polygons` max_edge_length | 15.0 | (not found in v2 boundary-polygons.js as named const) | `v7_bp_params_default` |
| `boundary-merge` edge_coplanar_thresh | 0.30 (`EDGE_COPLANAR_THRESH`) | 0.30 | `v7_bm_params_default` |
| `boundary-merge` edge_collinear_min_deg | 7.5 | 7.5 | `v7_bm_params_default` |
| `boundary-merge` eave_dist_3d | 0.30 (`EAVE_DIST_3D`) | 0.30 | `v7_bm_params_default` |
| `boundary-merge` pool_merge_dist | 0.20 | 0.20 | `v7_bm_params_default` |

All threshold values present in BOTH v2 and v3 match. v3's defaults are
faithful ports of v2's named constants in the v2 v7-pipeline modules
(`boundary-{signals,corners,polygons,merge}.js`, NOT the disabled
`boundary-edges/index.js` or `boundary-3d.js` modules).

Reproduce: run `DERIV_LOG=1` against either pipeline and grep `[DERIV]`.

### 5.2 Bench validation of v3 algorithms

v3 standalone bench harnesses on `/tmp/v7-refs-fresh/` and
`/tmp/v7-refs-bldgonly/` reference dumps:

- `v7-tile-signals-bench`: not run in this investigation.
- `v7-hemisphere-segment-bench`: not run in this investigation.
- `v7-normal-cascade-seed-bench`: not run in this investigation.
- `v7-density-voids-2-bench`: not run in this investigation.
- `v7-grow-refit-merge-bench` on `/tmp/v7-refs-fresh/` → `owner.bin`
  **byte-identical** to v2's `grow-refit-merge_0.owner.bin` (91392 bytes,
  0 diff). Algorithm port verified faithful on tile-wide input.
- `v7-grow-refit-merge-bench` on `/tmp/v7-refs-bldgonly/` → `owner.bin`
  byte-identical to v2's bldgonly owner.bin.
- `v7-boundary-{signals,corners,polygons,merge}-bench`: not run as
  full-chain in this investigation. The chain validation that does exist
  is the v3-bench-on-bldgonly inline harness, which produces 11 shared
  edges matching v2 bldgonly (`/tmp/v3-bldgonly/` outputs).

### 5.3 v3 prod step outputs on dahouet — counts only

After all threshold-wiring fixes shipped:

| Step | Input | n_planes (total / active) | n_ridges | n_corners | n_edges | n_shared |
|---|---|---|---|---|---|---|
| extract-buildings | tile 45696 | — | — | — | — | — |
| build-building-kd-trees | bldg 16092 | — | — | — | — | — |
| v7-derive-thresholds | bldg 16092 | — | — | — | — | — |
| v7-tile-signals | tile 45696 | — | — | — | — | — |
| v7-beam-incidence | tile 45696 | — | — | — | — | — |
| v7-hemisphere-segment | bldg 16092 | — | — | — | — | — |
| v7-normal-cascade-seed | bldg 16092 | 43 seeds | — | — | — | — |
| v7-density-voids-2 | bldg 16092 | 6 ghosts | — | — | — | — |
| v7-grow-refit-merge | bldg 16092 | 49 / 31 | — | — | — | — |
| v7-boundary-topology | bldg 16092 | 49 / 28 | 34 | 0 | 0 | — |
| v7-boundary-signals | bldg 16092 | 49 / 28 | 34 | 326 | 0 | — |
| v7-boundary-corners | bldg 16092 | 49 / 28 | 34 | 330 | 0 | — |
| v7-boundary-polygons | bldg 16092 | 49 / 28 | 34 | 330 | 174 | — |
| v7-boundary-merge | bldg 16092 | 49 / 28 | 34 | 330 | 168 | **7** |

Reproduce: run the v3 prod batch command (section 2), then
`node /tmp/count-shared.mjs <bin>` on each `boundary-*-roof-graph.bin`.

### 5.4 v2 bldgonly step outputs on dahouet — counts only

| Step | n_planes (total / active) | n_ridges | n_corners | n_edges | n_shared |
|---|---|---|---|---|---|
| tile-signals | — | — | — | — | — |
| hemisphere-normals-density-aware-segment | 73 segments | — | — | — | — |
| normal-cascade-seed | 43 seeds | — | — | — | — |
| density-voids-2 | 6 ghosts | — | — | — | — |
| grow-refit-merge | 49 planes | — | — | — | — |
| boundary-signals | 40 / 28 | 34 | 344 | — | — |
| boundary-corners | 40 / 28 | 34 | 316 | — | — |
| boundary-polygons | 40 / 28 | 34 | 316 | 176 | — |
| boundary-merge | 40 / 28 | 34 | 315 | 162 | **11** |

v2 silently drops planes with zero inliers (49 → 40); v3 keeps them in the
list with a "rejected" flag. Active plane count is identical (28 each).

### 5.5 Cross-step divergence points (post-thresholds-wiring)

These are byte/count comparisons between v2 bldgonly and v3 prod outputs
on the same 16092 physical points.

#### Hemisphere-segment outputs

- v2: 73 segments, 14218 total points segmented
- v3: 73 segments, 14218 total points segmented
- Per-point segment-id mapping: **1 point out of 14218 differs**.
  Point index 1646 → v2 segment 57 (normalGroupId=16), v3 segment 51
  (normalGroupId=13).

#### Normal-cascade-seed output (`cs-planes` / v2 seed-planes)

- 43 seeds in both, identical seed indices order for the first 42 seeds.
- Seed 16 in v3 differs from v2: v3 has 38 input points (added point #1646
  from segment 51), v2 has 37 input points. After outlier rejection: v3 has
  33 inliers, v2 has 34 (different sets; 32 common, v2 has 2 unique, v3
  has 1 unique). Normal differs by 0.0674 (Euclidean distance between unit
  vectors after rotation alignment).
- All other 42 seeds: normal distance < 0.001.

#### Density-voids-2 ghost-planes

- 6 ghost planes in both. Content not byte-compared.

#### GRM owner.bin

- v3 owner.bin vs v2 owner.bin (both 16092 Int16) **before** plane-index
  remapping: 1292 of 16092 points differ (8.0%).
- After remapping by inlier-set overlap (build a v2→v3 plane-id map):
  **230 of 16092 points differ (1.4%)**.
- 36 of 40 active planes match same-index. 4 pairs of plane indices are
  swapped:
    - v2#43 ↔ v3#47 (inlier overlap 16/16, normal diff 0.030)
    - v2#44 ↔ v3#48 (894/906, normal diff 0.0003)
    - v2#47 ↔ v3#44 (90/101, normal diff 0.0117)
    - v2#48 ↔ v3#43 (38/146, normal diff 0.602) — significant inlier set
      divergence
- Control test: feeding the v3 GRM bench harness with `v7-refs-bldgonly`'s
  v2-derived cs-planes — owner.bin matches v2's byte-identically.
  When the v3-prod-derived cs-planes (rotated to y-up, with local indices)
  is substituted for v2's, owner.bin diverges by 563 bytes (281 points).

#### Boundary-topology output

- 49 planes total in v3 (28 active + 21 rejected: 11 gvw>50% + 9 tombstoned
  + 1 low-nPts).
- 40 planes total in v2 (28 active + 12 gvw>50%; tombstoned planes dropped
  rather than tagged).
- Active plane count matches (28 = 28).
- Both produce 34 ridges and 34 ridge-lines.
- Per-plane n_pts top 15:
  - v2: 4828 1444 1015 906 844 760 640 215 192 185 185 142 105 105 103
  - v3: 4825 1444 1016 921 846 761 645 215 189 185 183 154 105 105 94

#### Boundary-signals output

- v2: 344 corners (11 ridge-intersection + 223 candidate-concave + 110
  candidate-convex).
- v3: 326 corners (11 ridge-intersection + 204 concave + 111 convex).
- Net diff: −19 concave, +1 convex, 0 RI corners. Total −18 corners.

##### Per-plane concave-anchor counts

Planes where v2 has ≥1 concave anchor:

| Plane | v2 concave | v3 concave | diff |
|---|---|---|---|
| 0 | 28 | 21 | -7 |
| 2 | 25 | 24 | -1 |
| 3 | 18 | 17 | -1 |
| 4 | 23 | 23 | 0 |
| 5 | 19 | 18 | -1 |
| 6 | 4 | 4 | 0 |
| 7 | 11 | 11 | 0 |
| 8 | 17 | 19 | +2 |
| 9 | 4 | 4 | 0 |
| 10 | 9 | 10 | +1 |
| 11 | 4 | 4 | 0 |
| 12 | 3 | 2 | -1 |
| 13 | 4 | 4 | 0 |
| 14 | 6 | 5 | -1 |
| 15 | 3 | 3 | 0 |
| 16 | 9 | 9 | 0 |
| 17 | 6 | 5 | -1 |
| 18 | 1 | 2 | +1 |
| 22 | 2 | 2 | 0 |
| 24 | 4 | 2 | -2 |
| 25 | 4 | 2 | -2 |
| 30 | 3 | 3 | 0 |
| 37 | 2 | 1 | -1 |
| 38 | 1 | 2 | +1 |
| 42 | 2 | 1 | -1 |
| 44 (v3 only) | 0 | 6 | +6 |
| 47 (v2 only) | 11 | 0 | -11 |

Planes 44↔47 are part of the GRM index-swap; counting them as one
physical plane the diff there is −5.

#### Plane-grid signal values

Comparing v2's plane-grids typed-array data (dumped via
`/tmp/dump-v2-plane-grids.mjs`) with v3's PLANE_GRIDS section
(`/tmp/v3-deriv-trace4/boundary-signals-roof-graph.bin`).

Grid origins differ between v2 and v3 by a non-integer multiple of
cellSize:
- v2 plane 0: gMinU=−11.448, gMinV=−18.041
- v3 plane 0: gMinU=−12.230, gMinV=−19.011
- Cell-index shift: dCol=−7.812, dRow=−9.708 → sub-cell offset
  0.1875 / 0.2923 cells

Per-cell ptCount diff (without origin alignment, integer-cell shift only):
- 41681 of 78240 compared cells differ (53%)
- 30323 cells have different oa (max diff 6.24, mean 1.61)

After grid-origin alignment via the `V3_FORCE_V2_CENTROID=1` A/B override
(see section 6), grids align byte-for-byte at the origin level
(gMinU = −11.448, gMinV = −18.041 in both):

| Plane | Cells compared | ptCount diff | oa cells differ | oa max diff | oa mean diff |
|---|---|---|---|---|---|
| 0 | 83328 | 1282 (1.5%) | 2547 (3.1%) | 2.94 | 0.18 |
| 2 | 14760 | 474 (3.2%) | 7274 (varies) | 1.30 | 0.016 |
| 3 | 11571 | 108 (0.9%) | 6146 (varies) | 0.74 | 0.005 |
| 16 | 6006 | 2710 (45%) | 2603 | 2.94 | 0.68 |

Pattern of ptCount diffs (plane 0, after alignment): v2 systematically has
slightly more points per cell than v3 (top diffs are 2→0, 3→0, 1→0, 6→2,
30→26, 31→27, etc. — small absolute differences spread across many cells).

Reproduce: `node /tmp/compare-plane-grids.mjs <plane-id>` (requires
`/tmp/v2-pg/` and `/tmp/v3-deriv-ab/` dumped first; see tooling section).

#### Boundary-corners output

- v2: 316 corners
- v3: 330 corners
- v2 reduces 344→316 (net -28: dedup + wall-bbox filtering); v3 increases
  326→330 (net +4: additions from coplanar import / ridge-end / wall-bbox).
- Net step-level behavior differs (v2 net negative, v3 net positive); the
  difference between the two final corner pools was not byte-compared.

#### Boundary-polygons output

- v2: 316 corners, 176 edges (polygon-paths field not populated in v2 dump
  format)
- v3: 330 corners, 174 edges, 27 polygon-paths

#### Boundary-merge output

- v2: 315 corners, 162 edges, **11 shared edges**
- v3: 330 corners, 168 edges, **7 shared edges**

### 5.6 A/B test: force v3 centroid to v2's metadata.l93_center

Patched `v3/steps/buildings/extract-buildings.c` to override the computed
building centroid with v2's metadata l93 center
`(289554.800200622, 6845192.01803264, 0)` when env var
`V3_FORCE_V2_CENTROID=1` is set.

After A/B run:
- v3 plane 0 grid origin: gMinU=−11.448, gMinV=−18.041 (matches v2
  byte-for-byte)
- v3 boundary-signals corners: 326 → 327 (+1)
- v3 boundary-merge corners: 330 → 327 (-3)
- v3 boundary-merge edges: 168 → 166 (-2)
- v3 boundary-merge **shared edges: 7 → 7 (unchanged)**

---

## 6. Tooling created (still in /tmp)

| Path | Purpose |
|---|---|
| `/tmp/dump-v2-plane-grids.mjs` | Runs v2 v7 pipeline programmatically and dumps `plane-grids` typed-array contents per plane as `.bin` files in the output dir + a `meta.json` manifest. Standard `dump-v7-refs.js` drops typed arrays with `__typed:dropped`. Usage: `node dump-v2-plane-grids.mjs <test-case.json> <out-dir>`. Runtime ~12min. |
| `/tmp/compare-plane-grids.mjs` | Loads v2 dump (from above) + v3 PLANE_GRIDS, normalizes cell-index shift between origins, compares per-cell ptCount and oa signals. Usage: `node compare-plane-grids.mjs <plane-id>`. Hardcoded paths inside; edit to point at desired v3 output dir. |
| `/tmp/count-shared.mjs` | Decodes a v3 `*-roof-graph.bin` (TLV format) and prints corner / edge / polygon counts + shared-edge count. Usage: `node count-shared.mjs <bin-path>`. |
| `/tmp/decode-rg.mjs` | Generic TLV summary for any v3 roof-graph. |

`/tmp/v7-refs-fresh/` and `/tmp/v7-refs-bldgonly/` are the v2 dumps from
`dump-v7-refs.js`. `/tmp/v2-pg/` is the plane-grids typed-array dump from
`dump-v2-plane-grids.mjs` on bldgonly.

---

## 7. What was checked

- v3 step constants in `v7-boundary-{signals,corners,polygons,merge}` —
  all 17 named constants match the corresponding v2 module hardcoded values.
- `v7-derive-thresholds` artifact correctness — `bldg-thresholds` record
  values verified by `DERIV_LOG` on a dahouet run.
- Threshold values consumed by GRM, DV2, topology — verified via
  `DERIV_LOG`.
- v3 GRM algorithm faithfulness — bench produces v2 byte-identical
  owner.bin on both `v7-refs-fresh/` and `v7-refs-bldgonly/`.
- v3 plane basis (e1, e2) derivation — matches v2's `planeBasis` formula
  in `geometry.js` after frame rotation (z-up basis is v2 y-up basis
  rotated by 90° about x).
- v3 plane equation frame conversion — `v7-boundary-signals.c:122`
  explicitly converts upstream world-frame `d` to local-frame `d_local =
  d_world + n · centroid` before compute. No frame mixing detected in
  boundary-signals exec path.
- Beam-direction parity — v3 vs v2 beam-directions max diff 0.013 over all
  16092 points. Reproduced earlier in this investigation.
- Position parity — v3 prod's per-bldg F32-cast local positions match
  v2 bldgonly bin positions byte-for-byte when mapped via per-building-records'
  `gidx[]` (after frame rotation z-up → y-up).
- PSIDs, point-weights, ts-* arrays — byte-identical between v2 bldgonly
  and v3 prod on the 16092 common points.
- Grid alignment via centroid override — A/B confirmed grid origins can
  be aligned exactly, but per-cell signal divergence persists.

## 8. What was NOT checked

- Per-cell signal values on tilted planes (other than 2, 3, 16) — only
  spot-checked. Comparison tool supports any plane-id; not run
  exhaustively.
- v2 boundary-corners step's internal sub-stages (dedup, pool-merge,
  coplanar import) — not traced.
- v3 boundary-corners step's per-sub-stage corner counts (D.0 wall-bbox,
  D.1 ridge-end, D.2 ridge-snap, D.2b ridge-side filter, pool-merge,
  coplanar import). Existing v7-boundary-corners-bench has logging; not
  run during this investigation.
- Whether plane-id swap (v2#47↔v3#44, v2#48↔v3#43) is deterministic across
  v3 runs — single run only.
- Cs-planes seed 16 root cause in upstream `v7-hemisphere-segment` —
  flagged (point 1646 is assigned to a different segment in v3 vs v2) but
  not traced.
- Why per-cell ptCount differs on plane 0 even with origin alignment —
  candidates include F32 vs F64 projection precision, point-in-cell
  boundary handling (`floor` semantics for negative coords), or order of
  point-walk in the signal aggregation loop. Not isolated.
- Whether the difference in step-level corner-count trajectories
  (v2: 344→316→315 vs v3: 326→330→330) is byte-meaningful or just
  bookkeeping (e.g. v2 dedups corners that v3 keeps separate but with same
  positions).

## 9. Where to pick up

The threshold-derivation audit is complete; v3 matches v2 byte-for-byte at
that layer. Wiring shipped. Remaining open questions for the dahouet 7 vs
11 shared-edge gap:

1. **Determinism / robustness of v3 boundary-signals plane-grid construction**
   to sub-cellSize input position drift — measured to be sensitive
   (planes 0, 16 show meaningful per-cell ptCount divergence even with
   origin alignment). Whether this is acceptable behavior or a bug needs
   judgment; reproducing v2's exact cell-aggregation may require matching
   v2's JS double semantics in projection (v3 F32-casts positions before
   projection).

2. **Step-level corner-count trajectory mismatch in boundary-corners.**
   v2 net-reduces (344→316), v3 net-increases (326→330). The internal
   sub-stages of v7-boundary-corners can be instrumented (existing bench
   harness has logging) to find which sub-stage diverges most.

3. **Hemisphere-segment point #1646** — the upstream-most known divergence.
   Could be a normal-vector hemisphere bin assignment FP tie-break.

4. **Plane-id swap determinism in GRM** — the swap between v2#47↔v3#44 is
   suggestive of a merge-order or tie-break difference. Worth examining
   `v7-grow-refit-merge-impl.c` plane-merge code for deterministic order.

Direct entry point: build v3, run the prod pipeline + grep DERIV, then
compare specific stages with the tools listed in section 6.

---

## 10. Plane-3 deep dive — D.7 deletion divergence (2026-05-16)

After narrowing the divergence to `boundary-polygons` D.7 deletion, the user
asked us to focus on one missing shared edge (v3 planes 3 ↔ 8) and trace
both v2 and v3's polygon construction for plane 3.

### Instrumentation

Added env-gated `POLY_TRACE` to both pipelines.

- v3: `steps/v7/v7-boundary-polygons-impl.c`
  emits stderr `[POLY] v3 plane=N stage=... ...` at D.3 input/output/kept,
  D.5 edges, D.6 prune, D.6b contour/arc/cycle, pre-D7, D.7 every candidate
  eval, D.7 winner delete, D.7 swap.
  `POLY_TRACE=*` enables for all planes; `POLY_TRACE=3,8` filters.
- v2: `lidar-algos-bench-v2/lib/pipeline/steps/boundary-polygons.js` adds the
  same env-gated tracing in `runDeletionPass` (D7-eval + D7-delete) plus a
  `pre-D7` line. v2 trace is logging-only — no version bump (per the bench
  CLAUDE.md `When NOT to bump` rule).

Run:

```bash
# v3 (~15s, plane=3 only)
rm -rf /tmp/v3-cache-poly && mkdir /tmp/v3-cache-poly
POLY_TRACE=3 build-dev/dioramap-processor batch \
  --pipeline pipelines/smoke/research-v7-tile-signals-real.json \
  --input-json '{}' --produce boundary-merge-roof-graph \
  --output-dir /tmp/v3-out-poly --cache-dir /tmp/v3-cache-poly 2>/tmp/poly-v3.log

# v2 (~12min — full pipeline)
cd lidar-algos-bench-v2
POLY_TRACE=3 node scripts/dump-v7-refs.js \
  --case test-cases-v2/real-22186-dahouet-502-bldgonly.json \
  --pipeline v7 --out /tmp/v7-refs-poly 2>/tmp/poly-v2.log
```

### Observed D.7 sequence for plane=3

| iter | v2 winner pool_id (src)    | v2 gain   | v2 wsd  | v3 winner pool_id (src)    | v3 gain  | v3 wsd  |
|------|----------------------------|-----------|---------|----------------------------|----------|---------|
| 1    | 112 (ridge)                | +0.1205   | 4.45→4.37 | 109 (ridge-end)          | +0.1195  | 4.08→4.00 |
| 2    | 99 (concave)               | +0.0260   | 4.37→4.23 | 96 (concave)             | +0.0730  | 4.00→3.81 |
| 3    | 104 (concave)              | +0.0234   |         | 95 (concave)             | +0.0664  | 3.81→3.66 |
| 4    | 96 (concave)               | +0.0217   |         | 92 (concave)             | +0.0641  | 3.66→3.43 |
| **5**| 103 (concave)              | +0.0029   | 4.25→4.23 | **4 (RI, high-x)**     | **+0.0599** | **3.43→3.21** ⚠️ |
| 6    | 101 (concave)              | +0.0011   |         | 197 (concave)            | +0.0622  |         |
| 7    | **196 (coplanar)**         | +0.0008   |         | 94 (concave)             | +0.0386  |         |
| 8    | **198 (coplanar)**         | -0.0004   |         | 99 (concave)             | +0.0220  |         |
| ...  | mostly coplanars + concaves|           |         |                          |          |         |
| 15   | **152 (coplanar)**         | -0.0225   |         | 104 (convex)             | +0.0124  |         |
| 16   | **143 (coplanar)**         | -0.0223   |         | 127 (concave)            | -0.0107  |         |
| **17**| **4 (ridge, low-x)**      | **-0.0367** | 4.11→4.14 ⚠ | 124 (concave)        | -0.0146  |         |
| 18   | 108 (concave)              | -0.0446   |         | **6 (RI, low-x)**      | **-0.0232** ⚠️ |     |
| 19   | 107 (concave)              | -0.0843   | 4.19→4.41 | 145 (concave)          | -0.0247  |         |
| end  | 19 dels; wsd plateau ~4.4  |           |         | 21 dels; wsd drops to 2.79 |        |         |

**v2 NEVER deletes pool_id=3** (the high-x RI between planes 3 ↔ 8 —
the one that holds the missing shared edge). Across all 19 iters, gain
for deleting pool_id=3 ranges from −0.69 to −17.78 (wsd would jump from
~4.2 to ≥19). It is structurally locked.

v2 *does* delete pool_id=4 (a ridge-family corner at the low-x end) at
iter 17 with gain=−0.0367. v2's other ridge-family deletion is pool_id=112
at iter 1 (gain=+0.12). The high-x RI pool_id=3 — the one we care about
for shared-edge preservation — is never touched.

**v3 deletes BOTH ridge-family corners** (pool_id=4 high-x at iter 5,
pool_id=6 low-x at iter 18). The high-x deletion is the one that breaks
the planes-3↔8 shared edge. At iter 5 the wsd actually *drops* (3.43→3.21)
— the geometry-scorer sees an improvement.

### Path composition for plane=3

v2 pre-D7 path: 30 corners
- 6 ridge family (RI + ridge-end): pool_ids 112, 111, 93, 4, 3, 98
- 14 concave
- 2 convex (109, 110)
- 7 **coplanar imports**: 143, 152, 191, 198, 196, 126, 131
- 1 misc

v3 pre-D7 path: 33 corners
- 2 RI (pool_id 4, 6) + 4 ridge-end (88, 93, 108, 109) = 6 ridge family
- 21 concave (src=4)
- 6 convex (src=5)
- **0 coplanar (src=8)** ⚠️

Verified across all v3 planes (`POLY_TRACE=*` run): 455 D3-in records,
zero `src=8`. v3 boundary-corners NEVER emits a corner with
`SRC_COPLANAR` reaching boundary-polygons.

### Root cause (architectural port bug)

**v2** `boundary-corners.js:817–824` — coplanar foreign-corner import:
```js
planeAnchors[g].push({
  x: sx, y: sy, z: sz,
  source: 'coplanar',                        // ← explicit per-plane source
  poolId: poolEntry.id,                      // ← references existing pool
  ...
});
```
v2 emits a per-plane anchor list (`planeAnchors[g]`) where the SAME pool
entry can appear with `source='concave'` in its origin plane and
`source='coplanar'` in an imported plane.

`SOURCE_PRIORITY = { ridge: 3, concave: 2, convex: 1, coplanar: 0, ... }`

In D.3 NMS (`boundary-polygons.js:222`), coplanar entries sort to the
bottom → dropped first if a higher-priority corner is within 40 cm.
What survives is what v2 calls the "real" boundary.

**v3** `v7-boundary-corners-impl.c:1396–1417` — coplanar import:
```c
anchor_t a = {0};
a.source = SRC_COPLANAR;                     // ← intent to mark coplanar
a.has_pool_id = true; a.pool_id = pe->id;
register_anchor_in_pool(&pool, &a, P->id, ...);  // ← reuses existing entry
```

`register_anchor_in_pool` at line 841–856:
```c
if (a->has_pool_id) {
    pentry_t* e = pool_find_by_id(pool, a->pool_id);
    if (e) {
        push_uint_unique(&e->plane_ids, ..., plane_id);
        // ← e->source is NEVER updated. a.source = SRC_COPLANAR is lost.
        return e;
    }
}
```

Then `out->corners[i].source = pe->source` (line 1441) — one source field
per pool entry, used across all planes via `plane_corner_ids[g]`.

`v7-boundary-polygons-impl.c:1530` consumes this:
```c
a->source = C->source;  // SAME source label for every plane this corner appears in
```

**Net result**: v3 has no way to express "this corner is `concave` in plane
A but `coplanar` in plane B." The coplanar-import semantic is structurally
preserved (plane_ids grows) but the per-plane source label is lost.

### Why this produces shorter chords near the ridge

In v3 plane=3, the foreign-imported corners from plane 8 (the other side
of the ridge) appear with their ORIGIN-plane source (concave/convex/RI).
They get D.3 NMS priority 1–3 instead of 0, survive dedup more
aggressively, and stay in the polygon path.

These imports physically sit very close to the ridge (within
`coplanar_dist_threshold = 0.12` m of plane 3). Their presence shortens
the chord that would replace the RI if it were deleted. The geometry
scorer sees a small wsd cost for deleting RI — sometimes even a
positive gain (iter 5: wsd 3.43→3.21).

v2 — where these same anchors are labelled `source='coplanar'` —
deletes them in early D.7 iters (tiny gain ~0.001), the polygon snaps
back to a shape where the RI is unambiguously load-bearing, and wsd
plateaus at ~4.13. RI is unreachable.

### Confirmed sanity checks

- v3's coplanar-import LOOP body is executed: `coplanar_imported` counter
  in `v7_bc_compute` is non-zero on dahouet (not verified numerically
  here but the loop is reached because the corner pool has corners
  passing the distance + balance checks; if zero, there would be no
  meaningful difference between v2 and v3 boundary-corners output, but
  v3 still has 5 extra D.3-in corners vs v2 in plane=3 — they're
  imports, just not labelled).
- Both v2 and v3 use the SAME `SOURCE_PRIORITY` table (verified in
  `v7-boundary-polygons-impl.c:129` `source_priority` switch).
- Both v2 and v3 use the same `ANCHOR_DEDUP_SQ = 0.40² = 0.16` (D.3
  radius). Verified constants match.
- Per-iter D.7-eval log of v2 pool_id=3 across iters 1–15 shows gain
  monotonically worse as the polygon shrinks. RI is structurally
  protected by the path shape.

### Suggested fix (NOT applied — deferred per user instruction)

User stated:
> ridge intersection are the most reliable and sure corners ever. we
> never reach an execution that show an improvement by removing a ridge
> intersection corner ! we must even refuse this deletion. but not
> now ! when we will have fully understand why it end up willing to
> remove such a corner. maybe a bug in geometry scorer/metrics... we
> will see.

The cause is now understood (above). The fix has two layers:

1. **Architectural**: emit a per-plane source override from v3
   `boundary-corners`. Either:
   - reintroduce a per-plane anchor list (v2-style), with the imported
     copy carrying `source = SRC_COPLANAR`. This was removed in
     Task #237 ("drop PLANE_ANCHORS, re-enable dirsByPlane"); revisit
     that decision.
   - OR add a `source_per_plane[]` field to `v7_bc_corner_t`,
     parallel to `plane_ids[]`. `boundary-polygons` reads
     `source_per_plane[k]` when iterating `plane_ids[k]`.
2. **Defensive (orthogonal)**: in `boundary-polygons-impl.c`
   `acceptDeletion`, hard-refuse deletion of any anchor with
   `source == SRC_RIDGE_INTERSECTION`. Same change applies to v2.
   Per user: defer this until after the architectural fix is in,
   then we can compare effects independently.

Reproduction is permanent: `POLY_TRACE` env var stays in both
codebases, gated to no-op when unset. Re-run with `POLY_TRACE=3`
to reproduce the traces above.

### Architectural fix shipped (2026-05-16) — partial

Shipped:
- `source_per_plane[]` field added to v3 corner pool entries
  (`v7p_entry_t`) and corner outputs (`v7_bc_corner_t`,
  `v7_bm_corner_t`) parallel to `plane_ids[]`. Single primitive
  `v7p_entry_register_plane(e, plane_id, src)` enforces the
  per-plane source override + the cached-global invariant on every
  plane-attach; `assert` in debug builds.
- All four CORNERS encoders updated (`v7-boundary-signals`,
  `v7-boundary-corners`, `v7-boundary-merge`) to emit
  `source_per_plane[]` after `plane_ids[]` in the CORNERS section
  record (no flag bit, no separate section). All decoders updated
  in lockstep (boundary-corners discards the field on read since
  signals never produces overrides; boundary-polygons consumes it
  at the D.3 input stage; boundary-merge forwards it).
- JS decoder reads `sourcePerPlane` on each corner.
- Step versions bumped: boundary-signals v2→v3, boundary-corners
  v2→v3, boundary-polygons v1→v2, boundary-merge v1→v2.
- `ROOF-GRAPH-FORMAT.md` §4.6 updated; section 0x08 stays reserved.
- `PHASE-5-V7-PORT.md` annotated explaining the broken assumption
  in Task #237.

Verified on dahouet:
- 63 `src=8` (coplanar) entries reach D.3 input across all planes
  (was 0 pre-fix).
- For plane=3, the D.3-kept set has 2 RI + 4 ridge-end + 14 concave
  + 5 convex + **8 coplanar** = 33 corners. Pre-fix the same 8
  corners appeared as 7 concave + 1 convex; positions unchanged.
- Invariant ASSERT does not fire.

**Result on shared-edge count**: still 7 on dahouet (unchanged
from before the fix). The reason: D.3 NMS only DROPS a corner if
a higher-priority corner is within 40 cm. Of the 8 coplanars in
plane=3, none sit within 40 cm of any non-coplanar corner — they
all survive D.3 even with the (now correct) priority-0 label. D.5
candidate-edge weights and D.7 deletion scoring use only
geometric metrics (path balance, proximity, ridge bonus, wsd,
edge length, compactness) — source is not in any of them.

So the fix is architecturally correct (closes the real port bug
introduced silently between Task #237 and Task #243), but in this
specific dahouet case the per-plane label was not what made v3's
polygon optimizer delete RI corners. The polygon SHAPE divergence
(v3 has 33 corners vs v2's 30 for plane=3, at slightly different
positions) is upstream of the source labels and is what actually
drives the D.7 wsd-improving-RI-deletion behavior.

### Next investigation (not yet done)

Where do the 3 extra corners come from in v3 plane=3? Candidates:
1. v3 boundary-corners produces more raw corners — extra concave/
   convex from signal-driven detection that v2 drops.
2. v3 boundary-corners produces more coplanar imports — v3 has 8,
   v2 has 7.
3. Position drift on a few corners pushes them outside the 40 cm
   NMS radius of a neighbour that would have absorbed them in v2.

Comparing the 33 v3 corner positions to v2's 30 (matched by 3D
proximity, not pool_id) would localize which corners are "extra"
in v3 and why. That's the next step.

## 11. CORNERS unification + per-plane anchor positions (2026-05-19)

Architectural cleanup that **does not move the polygon-shape
needle** on this dataset, but does close one of the known v3↔v2
semantic gaps and unblocks future work.

### What changed

- Wire format: section 0x07 `CORNER_MEMBERS` folded into 0x06
  `CORNERS` as an inline `members[]` tail block (flag bit 0x40).
  Each member's `plane_id` field is now populated (was reserved
  zeros). See `ROOF-GRAPH-FORMAT.md` §4.6 and commit `201540e`.
- D.3 NMS in `v7-boundary-polygons` now picks the **per-plane**
  anchor position (member with matching `plane_id`) instead of
  the pool centroid. Matches v2's `planeAnchors[g]` semantics.
  See commit `ae673bb`.

### Behavior-preservation check

Ran all 14 bench tools (chain + iso) on `/tmp/v7-refs-bldgonly-
exp2` pre and post the wire-format change. 54 / 55 outputs
byte-identical; the only diff is the additive `planeId` field
in the corner-members.json dump (purely informational, not
algorithmic).

### v2↔v3 polygon-shape diff: not closed

Comparing v3's per-plane `polygons.json` to v2's
`planes[g].polygonPath` (10 cm spatial tol, with rotation +
reverse), still **0 / 27 match in the chain bench, 6 / 27 in
the iso bench** — same as before the per-plane-position change.

Root cause: per-plane positions deviate from the pool centroid
by **3–13 cm typically**, but D.3 NMS dedupes within **40 cm**.
The shifted anchors fall inside the same dedup neighbourhood,
so NMS picks the same survivor set. The polygon-shape divergence
must come from later phases — D.5 candidate-edge scoring, D.6b
cycle building, D.7 optimisation, or D.8 collinear strip — or
from upstream corner-count drift (§5.6 + §10).

### What this work DID unlock

- `members[].plane_id` is now an actionable per-corner per-plane
  hook for future work (regularize phases, viz overlays, audit
  tooling).
- The TLV layout is one section simpler — every consumer that
  was walking 0x07 records keyed by corner_id now finds the
  same data inline as it walks 0x06 records.
- `v7-bench-v2-loaders.c` parses v2's `corners[].members[]`
  array into `v7_bc_member_t`, so polygons-only / merge-only
  benches see equivalent inputs to the chain.

### What still needs investigation

The 16-of-27 length-divergent polygons (v3 has a different
vertex count from v2 on the same plane) are the higher-leverage
target — same-length-different-shape divergences (10 / 27 chain,
13 / 27 iso) are likely downstream effects of length drift. The
next step is to instrument D.5 / D.6b / D.7 with POLY_TRACE on a
length-diverging plane and walk the cycle decisions side-by-side
with the v2 logs.

## 12. D.6b / D.7 deep dive — 2026-05-20

Iso-bench POLY_TRACE comparison v2 vs v3-iso-on-v2-input on plane 2
surfaced three contour-pipeline divergences. All fixed in commit
`115f158`. Iso match stays at 6/27 — there is residual FP-tie-break
drift in anchor segment-projection that needs follow-up.

### Fix 1 — Snap insertion order (D.6b snap_and_extract_loops)

**Symptom:** v3 contour's last 3 points (and total arc length) were
v3=27.34 vs v2=31.58 on plane 2. n_contour=13 in both.

**Root cause:** v2 computes BOTH `snapEnd` and `snapStart` projections
on the **original (pre-insertion) chain**, then applies both inserts
in order. v3 was doing snap-end FIRST (mutating the chain), then
querying snap-start on the modified chain — which shifted the
returned `insertAfter` index by +1 and produced a different chain
layout. The downstream `loops.push(chain.slice(endSnapIdx))` then
sliced a different subset of points, dropping one vertex and gaining
another.

**Fix:** Capture both queries first, then apply both inserts.
Contour now bit-identical to v2 on plane 2.

### Fix 2 — Multi-entry planeAnchors collapse (loader)

**Symptom:** v3 D.3-in trace for plane 2 reported `pool_id=61 src=5
(convex)` and `pool_id=40 src=5 (convex)`. v2's planeAnchors[2] for
these corners is `src=concave`.

**Root cause:** v2's boundary-corners emits MULTIPLE
`planeAnchors[g]` entries for the same pool corner when it carries
multiple member anchors on the plane (e.g. concave + convex for the
same pool entry). v2's D.3 NMS then dedupes by position + priority,
keeping the higher-priority one (concave > convex). v3's iso loader
was always overwriting `source_per_plane[k]` on each pass, so the
convex entry (encountered second in the JSON) won.

**Fix:** Initialise `source_per_plane` to 0 (sentinel) at the start of
the planeAnchors overlay; first entry sets it unconditionally;
subsequent entries apply max-priority. After overlay, fill any
remaining 0s with the canonical corner source.

### Fix 3 — Contour FP precision (uv2_t)

**Symptom:** Even after fixes 1 + 2 made the contour points and the
anchor positions byte-identical between v3 and v2, the anchor →
contour-segment projection still picked different segments for
anchors sitting near a chain vertex (where two adjacent segments
project to nearly the same point). Pre-D7 cycle differed at positions
28-30 by swapping 218/41/61 ordering.

**Root cause:** v2 carries chain positions as plain JS Numbers (IEEE
754 double). v3 was using `float` for `uv2_t.{u,v}` + the supporting
routines (`smooth3`, `dp_simplify`, `find_closest_on_chain`, the snap
constants, the `(float)` cast in `walk_from_seed`'s position
storage). After two smoothing passes + DP + snap-projection, the
contour vertices and snap-inserted points diverged from v2's at the
~6th decimal — enough to flip the tie-break on segment projection
for some anchors.

**Fix:** Promote `uv2_t.{u,v}` to `double` end-to-end. Smoothing /
DP / find_closest_on_chain / snap radii / walk_from_seed storage
all use double now. Match v2's effective precision.

### Residual divergence — what's still left

Even with all three fixes, iso-bench plane 2 produces pre-D7 cycle
`[…,39, 41, 61, 218, 40]` while v2 produces `[…,39, 218, 41, 61,
40]`. Same n_path=32, same 32 pool ids, but the cyclic order of the
last 3 anchors before the closing point is reversed.

This means anchor segment-projection is STILL placing anchors 218,
41, 61 onto different contour segments in v3 vs v2 — despite
identical contour vertices and identical anchor UV. The remaining
divergence is likely in a downstream operation (smooth3 output that
slipped past my earlier diff check, or arc-length accumulation FP
order). To pin down: dump each contour vertex with all 17 decimals
and look for the first byte-level mismatch.

### Iso bench match progression

| Phase | Iso match |
|-------|-----------|
| Pre any fix (Phase 0) | 6 / 27 |
| After RG-cleanup + per-plane positions (Phases 1+2) | 6 / 27 |
| After snap-order + source-collapse + uv2-double fixes | 6 / 27 |

Numerical impact on per-plane composition:
  - Plane 2 pre-D7 cycle is now numerically much closer to v2 (32
    identical pool ids in identical positions for indices 0..27;
    only positions 28-30 differ); the final 9-vertex polygon now
    contains 8 of v2's 9 vertices (was 7 of 9 before fix).
  - Other planes still show length and shape divergences that need
    their own POLY_TRACE walks.

## 13. D.6b cycle-source gate divergence root cause — 2026-05-20

Sub-agent investigation of the 4 planes (10, 14, 16, 38) where v2 and v3
picked different D.6b branches (contour-walk vs centroid-angle fallback)
on the iso bench.

### Root cause

`v7_bc_input_grid_field_t.{cell_size, g_min_u, g_min_v}` were stored as
`float` (32-bit) in v3. v2 keeps the equivalents as JS Number (IEEE-754
f64). The iso-bench loader's `(float)json_real_value(...)` cast in
`v7-bench-v2-loaders.c` truncated v2's f64 grid metadata to f32, so a
value like `cell_size = 0.10` became `0.10000000149011612` instead of
v2's `0.10000000000000000555...`.

The downstream effect was in `tangent_at_ref_cell`'s cell-index lookup:

```
gc = floor((u - g_min_u) / cell_size)
```

When the exact quotient sat within ~1e-7 of an integer (the boundary
between two cells), the float32 quantization of `cell_size` and
`g_min_u` slipped `floor()` onto the adjacent cell. Direct evidence
on plane 38 fwd-walk step 22 (ref-cell `(18, 43)`):

- **v2**: `g_min_u = -6.4924779206` (double), `cell_size = 0.10000000000`
  → `gc = floor(17.99...) = 17`. Index 1613 →
  `(outU, outV) = (-0.92, 0.39)` → tangent `(-0.39, -0.92)`.
- **v3 (before fix)**: `g_min_u = -6.4924779` (float), `cell_size = 0.10000001`
  (float) → `gc = floor(18.00...) = 18`. Index 1652 →
  `(outU, outV) = (-0.85, 0.52)` → tangent `(-0.52, -0.85)`.

One cell off in `gc` → different tangent → different `nextC/nextR` →
different best cell in the cross-search → entire chain diverges from
step 22 onward → DP/snap/loop selection all change → which side of the
contour-vs-centroid gate fires.

### Fix

Promote `{cell_size, g_min_u, g_min_v}` from `float` to `double` in
the input struct (`v7-boundary-corners-impl.h`) and drop the `(float)`
cast in the iso-bench loader (`v7-bench-v2-loaders.c`). Commit
`cffc454`. The TLV wire format §4.5 still stores these as f32; the
`memcpy(&local_float, rec+16, 4)` pattern then widens losslessly into
the new double struct field.

### Empirical impact (iso bench)

| Metric | Pre-fix | Post-fix |
|--------|---------|----------|
| Method-mismatched planes | 4 / 27 | **0 / 27** |
| Polygons full-match (10 cm tol) | 6 / 27 | 7 / 27 |
| Vertex-level (v2 verts within 10 cm of any v3 vert) | 120 / 167 (71.9%) | 125 / 167 (74.9%) |

The gate is now bit-identical across all 27 planes. The remaining
SHAPE-DIFF (14) and LEN-DIFF (6) classes are downstream effects:
contour-switchback ambiguity in arc-length parametrisation (§12) plus
D.7 deletion drift on small (3–5 vertex) polygons where one wrong cycle
slot rotates the entire polygon by 120°.

### What this doesn't fix (chain bench)

Chain bench (v3 producing its own boundary-corners output) reads grid
metadata from the TLV at f32 precision — the float→double widening at
decode time recovers no precision that wasn't already lost at encode
time. Closing the chain-bench gap requires either (a) bumping the TLV
wire format to f64 for these three fields, or (b) emulating the float
truncation in v3's boundary-signals to match the wire-read precision
of downstream consumers. Wire-format change is the cleaner option but
needs a TLV version bump.
