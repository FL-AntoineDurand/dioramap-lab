# Glossary

Internal names used throughout the code, docs and journal.

## Engine

| Term | Meaning |
|---|---|
| **step** | A versioned processing unit with declared `reads` / `writes` (artifacts). C struct `step_def_t`. One directory per step family under `steps/`. |
| **artifact** | A named, typed output of a step (`lidar-positions`, `grm-owner`, …). Content-addressed in the store. |
| **pipeline** | JSON list of steps + params (`pipelines/smoke/*.json`). The runner resolves it into a DAG. |
| **store / intent map** | The content-addressed cache. The *store* holds artifact bytes by hash. The *intent map* maps "this step, these inputs, these params" to the output hash, so unchanged work is skipped. See `docs/HASH-MODEL.md`, `docs/adr/ADR-001-HASH-MODEL.md`. |
| **batch / serve** | The two processor modes: run a pipeline once to disk, or stream artifacts to WebSocket clients. |
| **visibility (public/debug)** | Per-artifact metadata. Viewer-facing artifacts are `public`; intermediate ones are `debug`. Informational only; the server does not filter by it. |
| **v3** | Codebase generation: this C engine. **v2** was the earlier JavaScript R&D bench (not published); the journal often compares the two. |

## Roof reconstruction (`steps/v7`)

| Term | Meaning |
|---|---|
| **v7** | Pipeline generation: the current roof-reconstruction chain. Earlier ones (v4–v6x) existed only in the JS prototype. |
| **tile signals** | Per-point local features (normals, curvature, spacing) with density compensation. |
| **beam incidence** | Recovering each laser beam's direction from scan angle and flight strip (point source ID, **PSID**), without the aircraft trajectory. |
| **ts-weight** | Per-point tangential area (m²) used as a sample weight. Roughly 1/density, not 1. |
| **hemisphere segment / normal cascade** | Plane segmentation by clustering normals on the hemisphere, then by offset, then by connectivity. |
| **seed** | Initial plane hypotheses from the cascade (`v7-normal-cascade-seed`). |
| **density voids / ghost planes** | Plane hypotheses inferred from regions where points are missing (dropout), `v7-density-voids-2`. |
| **GRM** | *Grow–refit–merge* (`v7-grow-refit-merge`): region growing, per-strip refit and plane merging. |
| **roof graph** | Planes + shared edges + corners + outline polygons; binary TLV format spec in `docs/plan/ROOF-GRAPH-FORMAT.md`. |
| **boundary topology / signals / corners** | Finding plane adjacency, per-point boundary evidence, and corner candidates (`v7-boundary-*`). |
| **bp1 / bp2 / bp3** | Successive designs of the outline-polygon step. `v7-boundary-polygons-2` currently implements the *bp3* design: contour arc-length walk + windowed or-opt under a compactness cost. |
| **WSD** | *Wrong-side distance*: polygon cost term, the distance of a plane's own points falling outside its outline. |
| **regularize** | Outline regularization (`v7-boundary-regularize`): snapping, corner insertion, compactness gate. |
| **D.0 … D.8** | Numbered sub-stages of the boundary/regularize chain, as named in the step docs (`*.doc.json`) and journal entries. |
| **wall detect** | Detecting walls and bounding eave overhang below roof outlines (`v7-wall-detect`). |
| **intrinsic score** | Ground-truth-free model-quality score (`v7-score-intrinsic*`). |
| **dahouet** | The most-studied real test building (`data/test-cases/real-22186-dahouet-502*`). *bldgonly* = the per-building extract subset. |

## Data

| Term | Meaning |
|---|---|
| **LiDAR HD** | IGN's national aerial LiDAR programme (~10+ pts/m², classified, COPC). |
| **COPC** | Cloud-Optimized Point Cloud: LAZ with an octree index for HTTP range reads. |
| **BD ORTHO / IRC** | IGN orthophotos (RGB and infrared). IRC is used for NDVI vegetation masks. |
| **nDSM** | Normalized digital surface model (surface minus terrain). |
| **L93** | Lambert-93 (EPSG:2154), the French projected CRS used internally. |
| **INSEE code** | 5-digit French commune code, used in test-case names (`real-<insee>-<place>-<id>`). |
