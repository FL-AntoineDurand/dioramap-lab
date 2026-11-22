# Frama-C — known false positives

Eva-analyzer alarms we've classified as **not real bugs**, with the
reasoning recorded so future readers can recognize the shape and avoid
re-triaging them.

A Frama-C audit target is considered **failing** when its `eva.log`
contains `sure alarm (invalid status)`. Eva also produces many
**Unknown** properties — those are imprecisions, not failures, and are
not listed here. The targets below have a sure-alarm we've decided to
accept.

If you see a NEW sure-alarm, **don't add it here blindly**. First
classify it: does it fit one of the patterns below, or is it the kind
of thing that's normally a real bug (null deref, OOB, divide-by-zero
arising from actual arithmetic)? Real bugs go in the source fix; only
analyzer/harness/stub-limitation alarms belong here.

## Current accepted FPs

### 1. `module-pipeline` — `free(slot->label)` after `strdup`

- **Where**: `core/src/pipeline/pipeline.c:238`.
- **Symptom**: Eva reports `function free: precondition 'freeable' got status invalid`.
- **Root cause**: Eva's libc stub for `strdup` declares `allocates \result` (so the result is heap-allocated). But Eva can't reconcile that with the struct-field addressing pattern (`slot->label = strdup(...)` where `slot = &p->attrs[p->n_attrs]`). The result is that, by the time `free(slot->label)` runs, Eva has lost track of which allocation `slot->label` points at, and treats the `freeable` precondition as Unknown — but it stops propagating because it might also be invalid.
- **Why we accept it**: fixing structurally would require either (a) tightening Eva's strdup stub to track field-of-struct provenance, which would bleed across every module, or (b) adding a Frama-C-specific `//@ assert \valid(slot->label);` annotation at the call site, which pollutes production code with analyzer-specific noise. Code is correct; analyzer can't prove it.
- **Pattern of a real bug of this shape**: a `free` whose pointer chain you can't trace back to a definite `malloc`/`strdup` family call. If a NEW alarm of this shape appears at a site where the allocation isn't obviously from strdup/malloc/calloc, it's worth investigating.

### 2. `module-v7_grm_impl` — `rte,mem_access` invalid at iterative-refinement boundary

- **Where**: `steps/v7/v7-grow-refit-merge-impl.c:376`.
- **Symptom**: `assertion 'rte,mem_access' got status invalid (stopping propagation)` followed later in the trace by `got final status valid` for the same address class. Eva's iterative refinement converged on `valid` but its first-pass conservative read flagged a sure alarm.
- **Root cause**: Eva's bounded-iteration value analysis sometimes can't reconcile its own intermediate states. The plane-fit + kd-tree pattern in `v7-grow-refit-merge` is well-known to trigger this — the pool indices indirect through `seeds[pi].seed_indices[i]`, and Eva's pointer-arithmetic analysis loses precision after a few iterations.
- **Why we accept it**: writing a full ACSL contract on the kd-tree internals would be a multi-week project. The dynamic tests + ASan integration sweep cover this code path with high confidence already.
- **Pattern of a real bug of this shape**: an alarm at a `[...]` indexing where the indices come from unmodified user input. The seeds here come from our own seed-selection step, which itself is audited.

### 3. `bench-v7-tile-signals-bench` — recursive `build_node` without spec

- **Where**: `steps/normals/kd_tree.c:74` (the recursive call site).
- **Symptom**: `Recursive call to build_node without a specification. Generating probably incomplete assigns to interpret the call.` followed by Eva aborting via the deferred-error mechanism.
- **Root cause**: Eva can't analyze unbounded recursion without an explicit ACSL specification on the recursive function. `build_node` builds the kd-tree by halving the input set; the depth depends on the input size, which Eva must conservatively treat as unbounded.
- **Why we accept it**: writing a sound `requires/assigns/ensures` contract on `build_node` (and its mutually-recursive split helpers) would take days and add ACSL clutter to a performance-sensitive hot path. The kd-tree is exhaustively unit-tested (`core/tests/normals_test.c`) and exercised by every v7 bench end-to-end.
- **Pattern of a real bug of this shape**: a recursive function that we DO want Eva to fully analyze, where adding the ACSL spec would be inexpensive. None today.

### 4. `bench-v7-hemisphere-segment-bench` — `rte,mem_access` invalid (same class as #2)

- **Where**: `steps/v7/v7-hemisphere-segment-impl.c:337`.
- **Symptom**: same as #2 — Eva's iterative-refinement boundary on a `mem_access` assertion, "invalid" first then "valid" finally, with one sure alarm counted at the end.
- **Root cause / why we accept**: identical class to #2. The hemisphere-segment code shares the kd-tree query pattern that trips Eva.

### 5. `module-polygon_primitives` — `is_nan` on the ray-cast edge division

- **Where**: `steps/v7/v7-polygon-primitives.c:16` (the `v7_pt_in_poly` crossing test).
- **Symptom**: 4 `is_nan` properties (status Unknown) on the sub-expression `(polyU[j]-polyU[i])*(pv-polyV[i]) / (polyV[j]-polyV[i])` and its `+ polyU[i]`.
- **Root cause**: the division is reached only inside `if ((polyV[i] > pv) != (polyV[j] > pv) && …)`. That crossing guard makes `polyV[i]` and `polyV[j]` straddle `pv`, so `polyV[j] - polyV[i] ≠ 0` — but Eva can't carry that relational fact to the divide, and with finite-but-unbounded operands it can't bound the quotient below ∞ either, so it leaves `¬\is_NaN` Unknown.
- **Why we accept it**: the harness already constrains inputs to a finite ±1e6 box (the real precondition — UV coords are projected plane geometry, always finite), which discharges every is_nan obligation EXCEPT this guarded division. Proving it would need a relational `//@ assert polyV[j] != polyV[i];` annotation in the hot inner loop. Code is correct (standard even-odd ray cast); analyzer can't prove the guard. Exhaustively unit-tested in `core/tests/v7_polygon_primitives_test.c` (incl. concave polygons and degenerate edges).
- **Pattern of a real bug of this shape**: a `divide_by_zero`/`is_nan` where the divisor's range genuinely includes 0 with no guarding predicate → real (see the divide_by_zero note below).

### 6. `module-grid_contour` — hashmap/dynamic-buffer invariants Eva can't track

- **Where**: `steps/v7/v7-grid-contour.c` — `gc_chain_segments`, `gc_intern`, `gc_add_neighbour`, `v7_gc_result_free`. 90 unproven, **0 sure alarms**.
- **Symptom & classes** (all status Unknown, none proven invalid):
  - **51 `mem_access` + 17 `initialization`** — the segment-chaining open-addressing hashmap. `gc_intern` probes `h[slot]` with `slot = hash & (hsize-1)` (so `slot < hsize`, the calloc'd size) and writes `verts[id]` where `id` is bounded by "≤ 2 distinct vertices per segment ⇒ id < 2·n_segs" (the calloc'd size). Both are real invariants Eva can't relate to the dynamic alloc sizes — same family as the kd-tree iterative-algorithm FPs (#2, #4).
  - **9 `dangling_pointer` + 5 `precondition of free` + init** in `v7_gc_result_free` — freeing `out->outer.pts` / `out->holes[i].pts` that are conditionally allocated; free-after-conditional-alloc, same family as #1.
  - **3 `signed_overflow`** on the `±1` bias in `gc_intern`/`gc_add_neighbour` (`packed_key + 1`, `- 1`, `n_nb + 1`). The packed key is `round(u·1e4)` interleaved with `round(v·1e4)`; for finite *local* UV coordinates (meters, always ≪ 2·10⁵ m in a tile) the high bit stays clear so the value is a moderate positive int64 and `+1` cannot overflow. Latent only for adversarial coords that never occur.
- **Why we accept it**: the code is correct for its real input domain (finite, tile-local UV); the harness already constrains to a finite box. Proving the hashmap/counting invariants would need heavyweight loop+relational ACSL on a hot path. Exhaustively unit-tested in `core/tests/v7_grid_contour_test.c`.
- **Pattern of a real bug of this shape**: a `mem_access` where the index has NO masking/counting bound (genuinely unbounded) → real. The `±1` overflow would become real if UV coordinates could exceed ~2·10⁵ m; switching the key bias to unsigned arithmetic would make it provable (noted as optional hardening).

### 7. `module-bp2` — guarded intersection division + harness float artifacts

- **Where**: `steps/v7/v7-boundary-polygons-2-impl.c` (`seg_seg_strict_intersect`) and the linked `v7-polygon-primitives.c` (`v7_pt_in_poly`). 14 unproven, **0 sure alarms**.
- **Symptom & classes** (all status Unknown):
  - **2 `is_nan` in `seg_seg_strict_intersect`** — the `t`/`u` parameters `… / denom` where `denom` is the segment cross-product. The function early-returns when `fabs(denom)` is below an epsilon (parallel/degenerate), so the division is reached only with `denom ≠ 0` — but Eva can't carry that guard relationally (exact same shape as the PIP division in FP #5).
  - **2 `is_nan` in `v7_pt_in_poly`** — identical to FP #5 (polygon-primitives is a bp2 dependency).
  - **10 `is_nan`/`bool_value` in `main`** — obligations on the harness's own `volatile` result locals (`area`, `per`, `cmp`, `wsd`, the `bool` returns …). Eva can't prove a multi-step float result is finite or that an abstracted `bool` is exactly 0/1. These are harness-read artifacts, not properties of bp2's own statements.
- **Why we accept it**: the geometry kernels are correct for finite inputs (exhaustively unit-tested via main's `v7-bp2-test` + `core/tests/v7_polygon_primitives_test.c`); the guarded divisions are safe by the same predicate-guard reasoning as #5. No allocation/index/overflow obligation in bp2's own code went unproven.
- **Pattern of a real bug of this shape**: an intersection/area division reached with NO `fabs(denom) > eps` guard → real divide-producing-NaN.

### 8. `module-v7_corner_pool` — canonical-source invariant + dynamic-array families

- **Where**: `steps/v7/v7-corner-pool.c` driven by `corner_pool_harness.c`. 56 unproven, **0 sure alarms**.
- **The point of this module**: it actively exercises the #254-263 canonical-source invariant. The harness drives `v7p_upsert` / `v7p_entry_register_plane` / `v7p_recompute_entry` with mixed-priority sources, then calls `v7p_entry_validate`, whose live `assert(v7p_src_priority(e->source) == v7p_src_priority(max source_per_plane[]))` becomes an Eva-checked property. **Eva reaches the assert (the `\valid_read(e)` precondition is `Valid` at all three call sites, not `Dead` as in the shallow bench driver) and finds no counterexample** — i.e. the cache never drifts from the canonical store on the exercised paths. Eva can't *prove* the relational max-priority equality (status `Unknown`); discharging it fully is a WP target, captured in the prose contract on `v7p_entry_register_plane`.
- **The 56 unproven** are the established families, on the pool's dynamic arrays (`plane_ids[]`, `source_per_plane[]`, `members[]`):
  - **23 `mem_access` + 10 `initialization`** — Eva can't relate the parallel-array indices to the realloc'd capacities (same as the grid-contour hashmap, FP #6).
  - **10 `dangling_pointer` + free preconditions** in `v7p_entry_free` / `v7p_pool_free` — free-after-conditional-alloc (FP #1 family).
  - **6 `is_nan`** on member-centroid averages (`v7p_avg_members`) — finite-input float FPs (FP #5 family).
  - **4 unprovable `source_per_plane` sizing** preconditions on `v7p_entry_validate` — the same parallel-array-sizing fact Eva can't carry.
- **Why we accept it**: the invariant is correct and now machine-exercised (no Eva counterexample); the residue is all dynamic-structure sizing / free / float that Eva structurally can't discharge without WP. Regression-tested via `core/tests/v7_boundary_*_test.c`.
- **Pattern of a real bug of this shape**: a `v7p_entry_validate` assert turning `invalid` (a *sure* alarm) would mean the cache genuinely drifted — the bug we fixed. That's the property this module guards.

### 9. `module-v7_bclass_impl` — dynamic-output init/aliasing + finite-float

- **Where**: `steps/v7/v7-boundary-classify-impl.c` driven by `boundary_classify_harness.c`. 32 unproven, **0 sure alarms**, 85% statement coverage (the harness reaches `v7_bclass_compute` — its preconditions check `Valid`, not `Dead`).
- **Classes** (all status Unknown):
  - **15 `initialization` + 7 `mem_access`** in `v7_bclass_compute` — the per-edge output records and coverage-interval arrays are built incrementally (allocate, then populate field-by-field across phases); Eva can't prove every field is initialised before a later read, nor relate the parallel output-edge indices to the input-edge count. Same family as the grid-contour dynamic-buffer FPs (#6).
  - **9 `is_nan`** — the coverage t-axis projections and PIP-probe geometry; finite inputs (the harness constrains coords to ±100) leave only the multi-step float chains Eva can't bound (FP #5 family).
  - **1 `postcondition`** — the `assigns *out` clause: the function also writes into the heap it allocates for `out->edges` / `out->new_adjacencies`, which Eva can't fold into a single `*out` frame. Approximate-but-safe.
- **Why we accept it**: the geometry is correct for finite post-merge graphs (exhaustively unit-tested in `core/tests/v7_boundary_classify_test.c`); the residue is dynamic-output init/aliasing + float that Eva structurally can't discharge without WP.
- **Pattern of a real bug of this shape**: an `initialization` alarm on a field read on a path where it is genuinely never written (not merely unprovable) → real uninit read.

### 10. `module-v7_wd_impl` — sqrt-of-sum-of-squares + id-map sizing

- **Where**: `steps/v7/v7-wall-detect-impl.c` driven by `wall_detect_harness.c`. Just **3 unproven, 0 sure alarms**, and all 20 `v7_wd_compute` preconditions check **Valid** at both (NULL-beam + explicit-beam) call sites — the cleanest of the new-step audits.
- **The 3 unproven** (all Unknown):
  - **2 × `sqrt` `not_negative`** — `sqrt(x)` where `x` is a sum of squared deltas (a length / distance). Always ≥ 0 mathematically, but Eva can't prove the FP sum stays non-negative, so the libc `sqrt` precondition is left Unknown. Standard sum-of-squares FP.
  - **1 × `id_map_set` `mem_access`** — `\valid(m->idx + id)` on the corner-id → index map; `id` is bounded by the map's grown capacity, a sizing invariant Eva can't relate (same family as the corner-pool / grid-contour dynamic-buffer FPs, #6 / #8).
- **Why we accept it**: correct for finite inputs (exhaustively unit-tested in `core/tests/v7_wall_detect_test.c`); the residue is the non-negative-sqrt and dynamic-map-sizing facts Eva structurally can't discharge without WP.
- **Pattern of a real bug of this shape**: a `sqrt` whose argument can be negative through an un-clamped subtraction (not a sum of squares) → real domain error.

### 11. `module-v7_br_impl` — monolithic iterative optimiser, all known families

- **Where**: `steps/v7/v7-boundary-regularize-impl.c` driven by `boundary_regularize_harness.c`. **295 unproven, 0 sure alarms**, 91% function / 86% statement coverage, all 22 `v7_br_compute` preconditions **Valid**. Highest unproven count in the audit — because `v7_br_compute` is a single ~1250-line function (R.0 DOF tagging → R.1 seeded-PRNG nudge/joint-move loop → R.2/R.3) operating on many parallel work-buffers.
- **Classes** (all Unknown; nothing proven invalid):
  - **120 `mem_access` + 74 `initialization`** — the per-corner state, polygon-path, and nudge work arrays; Eva can't relate their indices/partial-init to the realloc'd capacities (family #6 / #9, just more of them).
  - **64 `dangling_pointer`** + 2 `precondition of memcpy` — the cleanup-path frees of the many work buffers and the R.0b geometry memcpy (the one already UBSan-fixed upstream); free-after-conditional-alloc at scale (family #1).
  - **27 `bool_value`** — DOF / move-accept flags Eva keeps as `[0..1]`-unprovable after abstract arithmetic.
  - **3 `is_nan`** — the angle-snap / cost float chains (family #5).
- **Why we accept it**: 0 sure alarms — every obligation is "can't prove", none "proven false"; the harness drives R.0 + a real R.1 PRNG iteration (seed-deterministic, no global state) and exercises 86% of statements without a counterexample. The residue is the dynamic-buffer/free/float families Eva structurally can't discharge without WP.
- **Pattern of a real bug of this shape**: any of these turning `invalid` (sure alarm) — especially a `dangling_pointer` at a free on a path where the buffer was already freed (double-free) — is real. The high *Unknown* count also argues for the same refactor the CPD baseline flags: this monolith is a prime candidate for splitting into per-phase helpers (smaller frames Eva can actually discharge).

## How to verify a NEW alarm fits one of these patterns

1. Locate the source line: `grep "got status invalid" .frama-c-out/<target>/eva.log | head`.
2. Read the line. Ask:
   - Is the operation a `free`, `malloc`, `strdup` chain through a struct field? → Pattern 1.
   - Is the operation a `[]` indexing inside an iterative algorithm (kd-tree, plane fit, RANSAC, etc.)? → Pattern 2.
   - Is the call site recursive without an ACSL contract? → Pattern 3.
3. If yes to any: add an entry above with the specific source location.
4. If no: triage as a potential real bug. The patterns of real Frama-C-caught bugs we've seen during the audit:
   - `null deref` on a public symbol where the caller didn't establish non-null (real).
   - `divide_by_zero` where a divisor's range includes 0 (real).
   - `is_nan_or_infinite` from a non-sentinel FP op (real; sentinel-style INFINITY writes are silenced project-wide via `-warn-special-float nan`).
   - `signed_overflow` on un-cast arithmetic with bounded inputs (often real).
   - `index_bound` on an array access where the index isn't proven bounded (usually real).

## How the audit script renders these

`scripts/ci/steps/run-frama-c.sh` reports each target's testcase as **fail** when the eva.log contains `sure alarm (invalid status)`. The four entries above will show up as **expected failures** in every Full-tier run.  They're not "broken" — they're documented limits of the analyzer + harness.

(Optional future polish: tag these specific testcases as `<skipped/>` or with an Allure category so they show in a separate "documented-FP" bucket. Not done today; not blocking.)

## Suppression policy

We **don't** add `Frama_C_assume(...)` or `//@ assert ...` annotations in production code purely to silence an Eva FP, unless the assertion would also document a real invariant a reader benefits from seeing.

We **do** improve the harness / contract / stub when the fix is local
and small (example: `coords_harness.c` initializes `x, y` so Eva can
prove `\initialized` — that's a harness-only fix, no production-code
change).

When neither path is cheap, the alarm gets an entry here.

### 6. `module-v7_score_intrinsic` — `is_nan` on `signed_distance` arithmetic
- **Module**: `v7_score_intrinsic` (`v7_score_intrinsic_compute` → `signed_distance` inline).
- **Symptom**: 12 `is_nan` properties (status **Unknown**, 0 sure alarms) on the FP expression `pl->normal[0]*p[0] + pl->normal[1]*p[1] + pl->normal[2]*p[2] + pl->d`. No sure alarms; 102/102 ACSL preconditions valid; 100% function coverage; 90% statement reach.
- **Root cause**: same shape as #5 — Eva tracks interval bounds on FP but cannot prove a finite-but-large product/sum stays under `±DBL_MAX`. The harness already constrains `positions` to ±1000 m, normals to [-1, 1] (unit), and `d` to ±1000 (matching real-world building geometry), which caps the absolute value of the sum at 4000 — far below `DBL_MAX` — yet Eva can't carry that relational fact across the inline call and leaves `¬\is_NaN` Unknown on each term.
- **Why we accept it**: the bounds in the harness *are* the real precondition (LiDAR positions and plane equations live in a finite metre-scale box; the impl never sees genuine NaN/Inf inputs in practice — the wrapper reads serialized records produced by upstream `v7-grow-refit-merge` which already constrains plane equations to unit normals). The impl additionally has `if (!isfinite(sd)) { any_distance_blew_up = 1; continue; }` defending against NaN propagation in case an upstream invariant violator slips through. Exhaustively unit-tested in `core/tests/v7_score_intrinsic_test.c` (38 assertions across perfect-fit, outlier, all-unassigned, out-of-range owner, pack/unpack roundtrip, n_planes > INT16_MAX rejection).
- **Pattern of a real bug of this shape**: a sure alarm on this expression (status invalid, not Unknown) — that would mean Eva proved a NaN/Inf is unavoidable on some path, e.g. an unguarded division by `||normal||²` that can be zero. Sure alarms here would mean the upstream invariant (`|normal| = 1`) has been silently relaxed.

### 12. `module-v7_br_impl` BR_PROFILE — `clock_gettime` *tp init

- **Module**: `v7_br_impl` (`v7_br_compute` → BR_NOW macro + two inline timing blocks at v7-boundary-regularize-impl.c:970, :978, :2997).
- **Symptom (pre-fix)**: a sure alarm at line 979 — `\initialized(&_ts.tv_sec)` `Invalid or unreachable` after `clock_gettime(CLOCK_MONOTONIC_RAW, &_ts)`.
- **Root cause**: Frama-C's libc stub for `clock_gettime` doesn't declare that `*tp` is written on success — `tp`'s `tv_sec` / `tv_nsec` stay uninitialized in Eva's abstract state. Reading them then trips `Eva,initialization`.
- **Fix landed**: zero-init `struct timespec _ts = {0};` at all three call sites (task #87). 8 bytes of stack that clock_gettime immediately clobbers — costless at runtime, removes the FP. We prefer this over `#ifndef DIORAMAP_FRAMA_C_AUDIT`-gating the whole BR_PROFILE block because the gate would obscure the macro from a future reader and the FP is genuinely the stub's fault, not the code's.
- **Pattern of a real bug of this shape**: a sure alarm on `_ts.tv_sec` / `tv_nsec` AFTER the zero-init lands — that would mean Eva proved a write-after-init was overwritten with garbage by some intervening call.

### 13. `bench-v7-boundary-polygons-2-only-bench` — slurp/json_loads bench-harness gap

- **Module**: `v7-bench-v7-boundary-polygons-2-only-bench` (the bench-as-harness audit over `steps/v7/v7-boundary-polygons-2-only-bench.c`).
- **Symptom**: 1 sure alarm — `\valid_read(f32p + i)` at line 242 of the renamed bench source. Eva also flags `json_decref` precondition Invalid at line 261 (same family, same root cause).
- **Root cause**: `audit-v7-bench.sh` feeds the bench's own `main()` to Eva as the entry point. The bench loads a binary blob via local `slurp()` returning `uint8_t*`, then casts to `const float*` and indexes `f32p[i]`. Eva's libc `fopen/fread/ftell` stubs return abstract bounded values (NULL or valid FILE*) but don't propagate the "alloced buffer is at least size_bytes" relation through `malloc(size_bytes)` + `fread(buf, size_bytes)` into the post-cast `float*` indexing. The `regularize-only-bench` doesn't trip this because its post-slurp code path doesn't index into the cast pointer before bailing.
- **Why we don't gate it**: this is bench-IO-layer, not bp2 algorithm. The actual bp2 algorithm audit is covered by `module-bp2` (the per-impl harness `scripts/frama-c/harnesses/v7/bp2_harness.c`) which feeds Eva a bounded abstract input directly and passes cleanly. The `*-only-bench` audit's value would be in catching bench-driver bugs, but the harness pattern needs a libc-stub upgrade we're not undertaking now.
- **Decision**: kept OUT of `scripts/frama-c/v7-benches.tsv` (was added in #91 then reverted in the same commit chain when the FP surfaced). The bench BUILD still gets the asan/release/clang/cov coverage as before — only the bench-as-harness Frama-C audit is skipped. If the slurp+cast pattern gets a proper stub upgrade later, register the bench and revisit.
- **Pattern of a real bug of this shape**: a sure alarm AFTER the slurp+cast lines, in the bp2 ALGORITHM itself — e.g. `module-bp2` (the per-impl harness) growing a sure alarm. That would mean a real bp2 invariant broke; the bench wouldn't be the right place to find it.
