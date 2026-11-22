# In-loop corner insertion (#25) — implementation plan (TEMPORARY, uncommitted)

Working doc for the build. Not a journal; delete when the op is landed + journaled.
Validated by: sim (`BR_SIM_INSERT`) + per-edge misfit metric (`BR_DUMP_EDGEMISFIT`),
2026-06-30. See journals `2026-06-29-regularize-corner-insertion-edge-fit-metric`
(design) + `2026-06-30-wsd-normalization-deadend-curriculum` (why this, not normalization).

## Goal

Let `boundary-regularize` ADD corners on polygon edges that locally fit points badly,
so an under-covered plane (terrain ring plane 0: 4807 pts, 67% outside a 6-vertex
polygon) can actually be covered → the un-normalized WSD curriculum completes instead
of producing a spike. This is the principled fix for the runaway class; the cost
function stays as-is (un-normalized simple sum — correct, see curriculum journal).

## Validated algorithm (done)

- **Metric** (free decomposition of WSD): per polygon edge, signed
  `misfit = Σ(owner OUTSIDE charged to edge: +w·d) − Σ(foreign INSIDE: −w·d)`, via
  `pt_nearest_edge_uv` (returns nearest edge index + foot t). Rank by misfit/length.
  `mass_uv` = weighted centroid of the edge's outside points = insertion target.
- **Placement**: insert ONE corner at `mass_uv` (displaced, real kink) — NOT the
  geometric midpoint (a midpoint is 180° collinear → collinear-removal kills it; line
  2761 threshold 170°, single-plane gated). By construction we only insert where
  misfit is high ⇒ mass far off edge ⇒ kink ≫ 10° ⇒ survives.
- **Guards**: self-int reject (`poly_self_int_uv`) + strict total-cost drop.
- **Loop**: insert one → optimizer refines it → re-measure → repeat. Static stacking
  saturates + self-intersects (sim proved this) ⇒ MUST be in-loop, interleaved with
  the coordinate descent. Collinear-removal is the GC for inserts that prove useless.

## Architecture — 3 layers (T7-scale; harder than T7, which only re-used existing ids)

### Layer 1 — optimizer array growth (in `v7-boundary-regularize-impl.c`)
`corners_in` is `const`; ~10 arrays sized `n_corners`. Reads of `corners_in[ci]`:
`.id` (36×), `.name` (4×), `.ridge_ids/.n_ridge_ids` (1×) — minimal.

- Define `N_INSERT_MAX` (start 64), `n_cap = n_corners + N_INSERT_MAX` after
  `max_corner` (line 1084).
- `corner_map` (line 1086): size `max_corner + 1 + N_INSERT_MAX`.
- **Rebind**: `corners_owned = malloc(n_cap); memcpy(corners_in); corners_in =
  corners_owned;` — all 84 reads unchanged; writes go to `corners_owned[ci]`.
  Declare `corners_owned=NULL` before first `goto cleanup`; `free(corners_owned)` at
  the single `cleanup:` label (3856) — covers all paths (free(NULL) safe).
- **Over-size** these allocations `n_corners → n_cap`: cs(1233), out->dof(1319),
  used(1337), corner_ctx_indices/n/cap(1539-41), traj(1796),
  corner_to_pw_idxs/n/cap(2057-59), mc_*_scratch(2286-88).
- `n_corners` is a value param → `n_corners++` on insert; all `for(ci<n_corners)`
  loops include new corners automatically.
- ctx/pw pools: T7's `poly_reserve`(268) + `ctx_reserve`(339) + `cap_ctx`(1526)
  already grow on demand — call `ctx_reserve` before REBUILD_CTXS in the insert.

### Layer 2 — emit (mostly free)
`corner_positions`/`corner_planes` (3718/3729) size to grown `n_corners` → include new
corners. `out->n_dof`/`n_corner_positions` (1329 etc.) set to grown `n_corners`.

### Layer 3 — wrapper serialization (`v7-boundary-regularize.c`, the real catch)
`reemit_corners` (line 444) rewrites the CORNERS section **1:1 from upstream records**
(`up_idx`). New corners (no upstream record) would be DROPPED → dangling id in the
polygon path → downstream classify/edges break. Fix: after the 1:1 loop, **append a
fresh RG_SECT_CORNERS record** for each inserted corner (those with output index ≥
upstream count): build id, position (yup→zup, +world shift), plane_ids (single plane),
empty/ synthetic ridge block, name ("INS<n>"), source tag. Mirror `parse_corners`
layout (offsets: [32] n_plane_ids, [36] pad, [40] plane_ids[pc], u8 source[pc],
align8, ridge block).

## Failure philosophy: crash, don't degrade
Exceeding the corner-buffer cap (`n_corners >= n_cap`) means our sizing assumption is
WRONG. A graceful "stop inserting" would silently ship an under-covered polygon — the
failure must NOT go unnoticed. So: **FATAL fprintf + `abort()`** (the `v3` convention,
e.g. `runner.c:469`). NOT `assert()` — the noasan build is Release/NDEBUG, so `assert`
compiles out → silent overflow. `abort()` crashes in every build. Soft per-plane/total
insert budgets (below) keep us well under the cap in normal operation; the `abort()` is
the invariant backstop that tells us loudly if that ever fails.

## `insert_corner(plane_idx, edge_k, mass_uv)` (Layer 1 helper)
1. `ci = n_corners++`. If `n_corners > n_cap`: `fprintf(stderr, "[v7-boundary-regularize]
   FATAL: corner-insertion exceeded N_INSERT_MAX ...\n"); abort();` (crash — see above).
2. UV→3D: need inverse of `project_uv` (basis origin + u/v axes at line 136/153).
   `pos3d = origin + mass_u·U_axis + mass_v·V_axis`. Add the inverse helper.
3. Fill `corners_owned[ci]` = { id = max_corner+1+counter, name = "INS<n>",
   n_ridge_ids=0, ridge_ids=NULL }. Fill `cs[ci]` = { point=pos3d, dof=2, alive,
   axes = plane in-plane e1/e2, n_planes=1 }.
4. `id_set(&corner_map, new_id, ci)`.
5. `poly_reserve(&polys[plane], polys[plane].n+1)`; memmove insert `new_id` at
   `edge_k+1`; `polys[plane].n++`.
6. `ctx_reserve` + `REBUILD_CTXS()`; `RETAG_DOF(ci)`; `REBUILD_PW_EDGES()`.

## In-loop sweep (Layer 4)
Periodic (e.g. every K=20 iters, and once near the end): per plane, measure per-edge
misfit; for the worst edge above `perlen` threshold, PREPARE insert at `mass_uv`,
self-int check, compute Δcost (insert iff strict drop), commit; cap inserts/plane and
total. Optimizer refines on subsequent iters. Gate `BR_CORNER_INSERT` (default OFF
until validated). Re-evaluate the slide-clamp (#26) once this covers plane 0.

## Staging (test each before next)
1. Foundation (Layer 1 growth scaffold, NO insert) → byte-identical output. ← START
2. `insert_corner` + emit (one post-loop insert) → gate green, ASan no-leak, the
   corner appears in `corner_positions`.
3. Wrapper serialization → renders in lab UI; classify/wall-detect don't choke.
4. In-loop sweep + tune → plane 0 `own_out` drops, gate green, broader sanity.

## Risks / notes
- Delicate optimizer + manual memory mgmt (15 error paths) — single-point free via
  `cleanup:` keeps it safe; over-sizing is behaviour-neutral (extra slots zero/untouched).
- Downstream impact: new corners flow to classify/wall-detect/score — watch those.
- Keep instrumentation (`BR_SIM_INSERT`, `BR_DUMP_EDGEMISFIT`, `BR_DUMP_COVERAGE`) — it's
  the A/B harness for this work.
