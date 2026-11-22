---
id: "2026-06-14-in-loop-make-shared-design"
date: 2026-06-14
title: "In-loop make-shared: unifying merge, snap, DOF and simplification into one regularize primitive"
tags: [boundary-regularize, boundary-merge, boundary-corners, make-shared, ridge, dof, simplification, architecture, design, v7]
category: design
status: concluded
refs:
  - "2026-06-11-merge-regularize-convergence-pathb"
  - "2026-05-21-boundary-polygons-tsp-rethink"
  - "2026-05-17-wrong-side-corner-killing-review"
  - "2026-04-27-regularization-exploration"
  - "2026-05-10-regularization-cost-architecture"
  - "2026-06-11-d2b-side-vec-wrong-plane-frame"
pipeline: v7-bp2
supersedes: "2026-06-11-merge-regularize-convergence-pathb"
---

> **Pivotal design capture.** This entry supersedes
> [[2026-06-11-merge-regularize-convergence-pathb]]: it keeps that entry's
> symptom/history/baseline but **corrects two of its core framings** (the
> "cross-plane corner-pair merge" primitive and the "duplicated ridge property"
> claim) and unifies several separate ideas into one primitive. Status running;
> filled as we build + adversarially review.

## 0. How to read this

This doc serves three readers; jump to your lane:
- **Implementing it** → **§8.2 build order** (the 5 sequenced, testable steps) + **§4**
  (the operations) + **§4.5/§4.6** (helper signatures, P4/DOF) + **§6** (the pinned
  thresholds/params). The build steps point back to the relevant §4/§9 detail.
- **Understanding it** → **§2** (the geometric-vs-topological core) + **§4.0** (the
  five-op vocabulary) + **§4.4** (the worked example).
- **Auditing the reasoning** → **§9** (the resolved-concerns log — the full journey;
  uses the *old* vocabulary, see the §4.0 map) and **§1** (how we got here).

**Tag legend** (cross-reference codes used throughout):

| family | meaning |
|---|---|
| **M1–M5** | adversarial guards (void-bridge, edge-type, R.0b-collision, consistency-invariant, self-intersection) — all resolved/qualified, §9 |
| **I1** | scope decision (ridge-type allowlist), §9 |
| **B1 / B2** | the conservative collapse rule / termination argument, §9 |
| **MG1 / MG2** | SPLIT memory-growth items (the original review's "B1/B2", renamed to avoid collision), §4.3/§9 |
| **S1 / S2 / S3** | FP-determinism dropped / cost-non-increase guard / threshold-flap dissolved, §9 |
| **F1** | the `ridge_ids` "always-0-outside-RI" comment fix, §2/§9 |
| **A2 / A3** | code-review items: P4 in-loop maintenance (§4.6) / the `nA×nB`-line vs RIDGES-segment caveat (§2) |
| **P1–P7** | the property catalog (§2.4): P1 plane_ids, P2 planes_dyn, P3 polys[].ids, P4 ridge_ids, P5 on_ridge_ids, P6 dof, P7 RIDGES |
| **PF1–PF3** | performance findings (§9): WSD is O(points) / cap=200 is deliberate / stop on `!any_move` |

**Status (2026-06-17):** **design** content-complete — all guards resolved/qualified,
thresholds pinned, every op + helper has a signature. The frontmatter `status:
running` refers to **implementation** (not started/in progress), *not* the design —
the design below is settled; build per §8.2.

## 1. How the understanding evolved (brief)

1. `boundary-merge` removed from the pipeline; regularize runs before classify
   (committed `0ca5a27`). Ridges no longer unified → the gap to close.
2. First cut **R.4-v1** (uncommitted): a *cross-plane proximity corner-merge*
   inside the chase. It fires correctly but recovers almost nothing on dahouet
   (2 merges) — and we found it has no real detector, just 0.30/0.50 m proximity
   gates (no edge-context, inherits merge's rough-geometry brittleness).
3. Realised static merge did something different — the **run-interval splice**:
   unify polygon *edges*, synthesising shared corners, even with no split pair.
4. Realised the corner→ridge "tag" already exists (`anchor_on_ridge` +
   `on_ridge_ids`, from the FP-accuracy work) and regularize already reads
   `corner.ridge_ids` — but it's **RI-only on output** ("always 0 outside RI",
   `v7-boundary-corners-impl.h:182`), so it misses the CC/CV run corners.
5. Found a **latent bug**: DOF is tagged from plane-count only
   (`-impl.c:540+`), so a single-plane corner sitting on a ridge gets **DOF 2
   (free)** and R.1 can drift it *off* the crease.
6. **Key clarification (this entry):** "corner belongs to plane B" has two
   meanings — *geometric* (position lies on B) vs *topological* (vertex of B's
   polygon). Conflating them caused several of the muddles above. Untangling
   them collapses snap + DOF-fix + merge into **one primitive: make-shared.**
7. **The twist (2026-06-15):** the splice is **not a separate op** — it *emerges*
   from (in-loop promote) + (in-loop **collapse**). And it's the **collapse**, not
   the merge, that forms the shared *edge*; the right collapse is **R.0b's
   existing tag-based logic** run in-loop (kink-safe, threshold-free). See §4.
8. **Vocabulary refactor + review closure (2026-06-16/17):** the muddled R.x /
   promote / collapse / splice language was replaced by **five named ops on a
   `settle()` cascade** (§4.0); a 4-agent code-vs-doc review then drove every
   adversarial guard to **resolved/qualified** (I1, M1, M2, M3, M5, M4), pinned the
   thresholds (§6), spec'd the helper signatures (§4.5) and the P4-maintenance/DOF
   coupling (§4.6, A2), and fixed an output-emission gap + SPLIT atomicity. The doc
   below is the post-review end-state; §9 logs the journey.

## 2. The conceptual core — geometric vs topological membership

Two genuinely **distinct** properties (previously conflated):

- **Geometric on-ridge** — the corner's position lies on the ridge crease.
  (Today: `ridge_ids` / `on_ridge_ids`.) It is a **maintained property**: set at
  every event that puts a corner on a ridge — birth (RI), upstream snap, and
  in-loop snap/promote — and **held by DOF** (a DOF-1 corner slides *along* its
  ridge and *cannot leave it*). So it changes only by a **controlled snap-in**,
  never by uncontrolled drift. *Precision caveat:* DOF-1's `axis_a` is
  `normalize(nA×nB)` (`:554-560`, from plane **normals**) — the plane-intersection
  *line*, which can differ ~1e-3 from the RIDGES *segment* direction (the
  two-normal-sources divergence). The corner is pinned to the `nA×nB` line, not
  literally to `rg.from→to`; promote should pin `axis_a := rg.rd` (or accept the
  ~1e-3 slide) and snap-tag against the **same** geometry DOF will then enforce.
  DOF also doesn't stop a slide *off the segment ends* — only the `[t0,t1]` clamp
  does, and only at promote time.
- **Topological shared** — the corner is a *vertex of both planes' polygons*.
  A property of the graph. (Today: `corner.plane_ids`, which must equal the set
  of planes whose polygon paths reference the corner.)

These usually coincide but **legitimately differ**: a single-plane corner
sitting on a ridge is geometric-on-ridge = true, topological-shared = false. It
should be **DOF-1** (slide along the ridge); today DOF is tagged from plane-count
only, so it wrongly gets **DOF-2 (free) and can drift** — *that* is the bug the
make-shared + DOF-coupling fix removes.

### What this does to the "property duplication" worry (correcting §PathB)

The earlier entry called corner→ridge membership a *same-concept-N-encodings
duplication that drifts* (`ridge_ids` vs `plane_ids ∩ ridge.planes`). **That was
partly wrong** — those two are not two encodings of one fact:

- `ridge_ids` encodes the **geometric** fact (on the crease).
- `plane_ids ∩ ridge.planes` encodes the **topological** fact (vertex of both
  polygons).

So they *should* be allowed to differ. The real situation is:

| relationship | verdict |
|---|---|
| `ridge_ids` (geometric) vs `plane_ids ∩ ridge.planes` (topological) | **NOT duplication** — two different concepts. Comparing them was the bug. |
| `plane_ids` (list) vs polygon-path membership (which paths contain the corner) | **genuine maintained-redundancy** — both encode "vertex of which polygons." The merge idiom keeps them consistent. This is *the invariant to protect.* |
| `ridge_ids` (on-ridge tag) vs DOF | **coupled, must be consistent** — on-ridge ⟹ DOF-constrained, and DOF then *enforces* on-ridge (the corner can't leave the crease). Set them together; don't re-derive defensively. |

**Resolution / data-model principle going forward:**
- *Geometric on-ridge* → a **maintained property**: set `ridge_ids` + DOF
  *together* at every snap event (birth, upstream snap, in-loop snap/promote);
  DOF keeps it true between events — **no drift, no flap** (this dissolves the
  S3 concern, §9). The input tag (RI ∪ snapped CC/CV — F1, §9) is the **seed**;
  the loop only *adds* tags on a controlled snap-in, it never re-checks for drift.
- *Topological shared* → `plane_ids`, **kept equal to polygon-path membership by
  every mutation** (the consistency invariant).
- *Ridge membership* is *also* derivable from `plane_ids ∩ ridge.planes` (for
  crease types only — see I1, §9), but the tag is the **unique** record for a
  *single-plane* on-ridge corner (the make-shared target), where
  `plane_ids = {A}` cannot derive it.

**make-shared is the bridge** that converts geometric-on-ridge → topological-
shared (insert the corner into the other polygon, update `plane_ids`), keeping
the invariant. DOF-1 then *derives* from `plane_ids` — no separate DOF tag, no
separate ridge tag.

> NB (from the review): "derive ridge membership from `plane_ids ∩
> ridge.planes`" is **only valid for crease ridge types {ridge,hip,valley}** —
> it INVERTS on step/flashing ridges (topological-shared yet not on a crease).
> See I1, §9. The catalog below is the authoritative per-property spec.

### 2.4 Property catalog (verified against code — root cause: none of these are commented at their declaration, which is why they were conflated)

**One-line key (to put atop §2 *and* as the comment at each declaration site):**
- **Topological** = vertex of a polygon (P3 canonical; P1/P2 are its index). Changes only by graph edits.
- **Geometric on-ridge** = position on a crease (P4/P5). *Target:* maintained at every snap event + held by DOF (no drift); the input tag is the seed. (In-loop maintenance is **to-build** — `corner_state_t` has no `ridge_ids` field today; see §2.4 P4.)
- **make-shared** converts geometric-on-ridge → topological-shared (insert into P3, update P2, derive P6) — and sets P4+P6 at the snap event.

**A. Topological membership — "vertex of which polygons"**

| facet | **P3 `polys[].ids`** (CANONICAL) | **P1 `corner.plane_ids`** | **P2 `planes_dyn`** |
|---|---|---|---|
| Declared | `regularize-impl.c:631` (`poly_t`) | `corners-impl.h:176`; copied `merge-impl.h:106/123` | `regularize-impl.c:876` |
| Semantic | per plane, the cyclic list of corner ids — *ground truth of topological membership* | per-corner index of which planes' polygons hold this corner (static copy) | the **live, mutable** per-corner version during the loop |
| Set up | cloned from input paths `:631-638` | upstream (boundary-corners emit) | `memcpy` from `plane_ids` at init `:892-894` |
| Updated | R.0b splice `:675`; MERGE_CORNERS rewrite+dedup `:1390`; R.2/R.3/R.4 | static (never) | **union** in MERGE_CORNERS `:1364-1375`; **NOT** by R.0b (bug) |
| Consumed | WSD re-walk `:937+`; REBUILD_CTXS/PW derive caches | R.0 DOF `:543-545` (count only) | R.2 share-test `:281`, R.3 gate `:1746`, RETAG_DOF |
| Proposed | stays canonical; **SPLIT must grow it** (`cap` dead — MG1, §4.3) | comment; derive on demand vs trust static copy | **comment as the maintained index of P3**; invariant `P2 == {planes whose P3 holds c}` |

**B. Geometric on-ridge — "position lies on a crease"**

| facet | **P5 `anchor.on_ridge_ids`** (source) | **P4 `corner.ridge_ids`** (persisted output) |
|---|---|---|
| Declared | `corners-impl.c:131` | `corners-impl.h:182` — comment **corrected** (was wrongly "always 0 outside RI" — F1, §9) |
| Semantic | FP-safe property "anchor is on ridge R" | the maintained on-ridge tag, carried downstream |
| Set up | `anchor_push_on_ridge`: RI `:1346`, RE `:1403`, **D.2 snap `:1447`** (≤0.30 m) | from pool entry; RI **∪ snapped CC/CV** (`:1445`,`:874-876`,emit `:1702-1707`) — the seed, richer than RI-only (F1, §9) |
| Updated | within boundary-corners only | **TODAY: not touched by regularize** — `corner_state_t` (`:220-229`) has **no `ridge_ids` field**, DOF is plane-count-only. **Spec'd as A2, §4.6**: add the field + seed from input + additive write at snap-in/merge + DOF coupling (on-ridge ⟹ DOF-1). Maintained at every snap event, DOF holds it between events — no drift |
| Consumed | `anchor_on_ridge` D.2b gate `:157/1485` | regularize **R.0b** redundant-midpoint `:666-670` |
| Proposed | (unchanged upstream) | **maintain-not-re-derive: set P4 *with* DOF (P6) at every snap event; input tag = seed; the *unique* record for single-plane on-ridge corners** |

**C. Derived & reference**

| facet | **P6 `cs[].dof`** | **P7 RIDGES record** |
|---|---|---|
| Declared | `regularize-impl.c:222` | topology `rg_ridge_record_t`; regularize `v7_br_input_ridge_t` |
| Semantic | DOF class 0 locked / 1 ridge-slide / 2 surface | ridge: `plane_a/plane_b`, segment `from→to`, **`type`** (ridge/hip/valley/step/flashing) |
| Set up | R.0 from `n_plane_ids` count only `:545-587` (the mis-tag) | topology emit |
| Updated | RETAG_DOF (R.4-v1 only) on merge; **not** R.2/R.3 | static |
| Consumed | R.1 candidate axes | regularize parses `plane_a/b`+`from/to` into `rg[]` `:1419-1427` |
| Proposed | **derive from ridge-membership (P4) + plane-membership (P2); set *together with* the on-ridge tag at every snap event; DOF then enforces on-ridge** (fixes the plane-count-only mis-tag; on-ridge single-plane → DOF-1) | **plumb `type` into `rg[]` (currently DROPPED)** — needed for I1, §9 allowlist |

**Redundancy summary:** P1/P2 are a *maintained index* of P3 (genuine redundancy → the §5 invariant polices `P2 ⇔ P3`). **P4 is a maintained on-ridge property, coupled to P6** — set together at every snap event, and DOF *enforces* it (no drift); it is the *unique* record for single-plane on-ridge corners and redundant with topology (`plane_ids ∩ ridge.planes`) only for already-shared ones. **P6 derives from P4 + P2** (set at every snap event), not from plane-count. P3 and P7 are the two ground-truth stores; P5 is P4's upstream source.

> **DECISION UPDATE — 2026-06-19 (semantic analysis; supersedes the P1/P2 rows above).**
> A read-only semantic audit (measured on dahouet) settled what P1 `plane_ids` and P2
> `planes_dyn` actually mean, and changed the plan:
> - **P1 `plane_ids` is GEOMETRIC, not a topological index.** It means *"this corner lies
>   on this plane's **surface**"* (set upstream by proximity to the plane fit, ≤0.12 m,
>   before any polygon exists). Measured: all 160 dropped-at-seed (corner,plane) claims on
>   dahouet are ≤0.12 m from the plane — i.e. the "over-labelling" is **real geometry, not a
>   bug**. P1 is therefore a *superset* of P3-membership and the row above ("static copy
>   index of P3") was wrong. **P1 is not used anywhere in regularize** (the high-level
>   polygon structures already carry everything this stage needs); do **not** "fix" it
>   upstream — that would destroy true geometry.
> - **P3 (paths) is the Single Source of Truth for membership; P2 is a pure function of it.**
>   `planes_of(corner) = { p : corner ∈ polys[p] }`. No seed from P1, no stored copy, no
>   union-up / reconcile-down. The seed-from-`plane_ids`-then-intersect was a redundant
>   detour that lands on "derive from P3" anyway (P1 ⊇ P3-membership). The MERGE union and
>   `RECONCILE_PLANES_DOWN` machinery **dissolve into "read the paths."** A stored reverse
>   index is allowed only as a perf *view* (rebuilt from P3 at one coarse chokepoint, never
>   independently written) — and only if a measurement demands it (perf is ~neutral: the
>   reconcile already pays the derive cost; WSD dominates runtime).
> - **The §5 `P2⇔P3` check stays, but its membership half becomes true-by-construction** (P2
>   *is* P3). Keep the check for the invariants that can still break (dead-id-in-path,
>   edges⇔paths). This reverses the T0b "keep the cache so the check catches desyncs" stance —
>   correct now because the premise flipped: no independent cache ⇒ no desync to catch.
> - **P4 `ridge_ids` is unaffected** — it is the geometric on-ridge record and **cannot** be
>   derived from P3 (24 single-plane on-ridge corners on dahouet are on-ridge-true /
>   in-one-polygon). It stays a separate field.
> - **R.0 DOF tag is now TOPOLOGICAL** (resolves the "mis-tag" this §2.4 flagged for P6). It
>   reads path-membership, not the geometric `n_plane_ids` count, which over-counted junctions
>   (a corner coplanar with a FAR plane, ≤0.12 m, was tagged a junction and frozen). Measured
>   phenomenon: the switch moves ~111 corners on dahouet — but that's optimizer-**basin
>   amplification of only ~4 genuine coupled-DOF changes** (A2 ridge-coupling MIN-re-locks 32 of
>   36 raw differences). The ~4 residual = 2 correct frees (coplanar-with-far-plane), 1
>   near-junction whose ridge to the 3rd plane wasn't tagged (a ridge-tag GAP — working-plan L2),
>   1 dead/phantom. Shipped without per-output metric eval (judged correct by the DOF analysis).
>   Working plan + task list: `v3/docs/plan/MEMBERSHIP-SSOT-AND-MAKE-SHARED-PLAN.md` (Phase B; L2/L3).

### 2.5 Code-comment task (step 0 of the build)

Add a definitional comment at each of P1–P6's declaration site stating (a) the concept it encodes (topological / geometric / derived), (b) the canonical store it indexes or derives from, (c) the invariant — incl. the **P4↔P6 coupling** (on-ridge ⟺ DOF-constrained). This is what was missing and caused the conflation. **Done now:** the wrong `corners-impl.h:182` comment is fixed (was "always 0 outside RI"); the rest land as build step 0.

## 3. Current state — code, flaws, results

**Pipeline (committed `0ca5a27`):** `… polygons-2 → regularize → classify →
wall-detect → score`. merge gone.

**regularize loop** (`v7-boundary-regularize-impl.c`): R.0 DOF-tag → R.0b
cleanup → R.1 greedy coordinate-descent (`while iter<max_iterations`, L1540;
per-iter `for ci<n_corners` sweep, GLOBAL_BEST single move + chase). In-loop
topology ops live in the **chase**: R.2 proximity corner-merge, R.3 collinear
removal. Cost = WSD (anchor) + angle/nbr-angle/horiz + R.7 pairwise. cap=200.

**Confirmed flaws:**
- **DOF mis-tag** (L540+): plane-count only → single-plane on-ridge corner = DOF
  2, free to drift off the crease. R.1 actively un-snaps ridge corners.
- **R.3 is single-plane-only** (`planes_dyn_n[oci] != 1 continue`, L1746) → it
  **cannot collapse shared collinear corners**. A unified ridge of N shared
  corners stays N.
- **No edge unification** without merge: R.4-v1 recovers ~2; classify
  `interior-shared` survives (22) only via the RI-corner ridge path, **not**
  edge `n_plane_ids` (6).
- **R.4-v1 detector is pure proximity** (0.30/0.50 m), no edge-context, inherits
  merge's rough-geometry brittleness; lives in the chase (wrong cadence).
- **EDGES were stale** (regularize mutated corners, passed EDGES through with
  dead-corner refs). R.4-v1 added a faithful rebuild from post-regularize paths.

**Results now (dahouet bldg0, R.4-v1 on):** pipeline clean to
`model-score-postwall`; corners 118 (54 shared); EDGES 182 (6 shared,
n_plane_ids≥2); interior-shared tag 22; convergence ≈ unchanged (+1.4%).

## 4. The plan — five local operations on a `settle()` cascade

Replace R.4-v1 (and absorb R.0b/R.2/R.3) with **one optimizer move plus four local
topology events**, all driven by a single **`settle()` cascade**: the optimizer
slides a corner, then we drain the topology events the change makes possible —
**only at the changed corner and its path-neighbours, never a global rescan**, and
gated by the geometric-vs-topological split (§2).

### 4.0 The operation vocabulary (canonical — supersedes the old `R.x` names)

| op | what it does | replaces |
|---|---|---|
| **SLIDE** | move one corner within its DOF to lower cost — *the optimizer* | R.1 |
| **MERGE_CORNER** | fuse two coincident corners into one — *same-plane duplicates* **and** *cross-plane ridge-partners* (the partner-weld half of sharing) | R.2 + R.4 |
| **DROP_COLINEAR** | remove a **non-shared** (single-plane) corner that is collinear in its one polygon — adds no outline | R.3 |
| **SPLIT_TO_SHARE_CORNER** | the only **growing** op: insert a corner into the other plane's *straight edge* when **no** partner corner exists there | (new) |
| **DROP_REDUNDANT_ON_RIDGE** | remove a redundant **shared** on-ridge corner — tag-based, kink-safe, **B1-conservative** | R.0b |

- **"Make a corner shared"** (the old *promote*) is **not a distinct op**: it is
  either a **MERGE_CORNER** with a cross-plane partner, or a
  **SPLIT_TO_SHARE_CORNER** when no partner exists there.
- **The "splice"** (old promote+collapse) is **not an op either** — it **emerges**
  from the cascade: a share event re-queues the survivor, and
  `DROP_REDUNDANT_ON_RIDGE` then fires on it naturally (§4.4).
- **Corner kinds** (by DOF): **FREE** (2-DOF, moves in its plane), **RIDGE**
  (1-DOF, slides along one crease), **PINNED** (0-DOF, a junction). Sharing is the
  FREE→RIDGE transition.

**Old→new term map** (the journey/§9 log still uses the old words; this is the
dictionary): *promote / make-shared* → **SHARE** (= `MERGE_CORNER` or
`SPLIT_TO_SHARE_CORNER`); *collapse / R.0b-in-loop / collapse_on_ridge_run* →
**`DROP_REDUNDANT_ON_RIDGE`**; *splice* → the **emergent** share+drop (§4.4);
*the R.1 chase* → the **`settle()` cascade**; *R.1* → **SLIDE**; *R.2* →
**`MERGE_CORNER`** (same-plane); *R.3 / collinear-removal* → **`DROP_COLINEAR`**;
*R.4 / R4_RIDGE_MERGE* → **`MERGE_CORNER`** (cross-plane). The five ops above are
the canonical names from here on. (Note: **"make-shared" survives as the *name of
this design effort*** — the doc id/title/tags — and as the informal umbrella for
"the SHARE machinery"; it is retired only as an *operation* name. `SHARE` itself is
a **concept** — the FREE→RIDGE transition — not a sixth callable op; it is always
*realized* as a `MERGE_CORNER` or a `SPLIT_TO_SHARE_CORNER`.)

### Why each operation (rationale + impact)

- **MERGE_CORNER / SPLIT_TO_SHARE_CORNER (sharing)** = correctness: convert a
  geometric-on-ridge corner into a topological-shared vertex → ridge unified (no
  duplicate edges), DOF-1 derived (corner stays on the crease for the rest of
  descent).
- **DROP_REDUNDANT_ON_RIDGE in the same cascade** = **correctness *and*
  performance** — it is what actually **creates the shared edge.** Sharing alone
  gives shared *corners* whose *neighbours still differ* between the two paths → no
  shared edge. Dropping the redundant on-ridge corners **between** two shared
  corners **in both paths** makes them **adjacent in both** → the edge between them
  is owned by both planes (`n_plane_ids=2`). Tag-based (3 consecutive sharing a
  `ridge_id` → drop the middle) — **kink-safe by construction, no angle threshold**
  (a real junction carries *two* ridge_ids, so the all-three-share-one test fails
  and it is kept). Bonus: shrinking N→2 along a ridge trims the corner-sensitive
  ~57% cost (PF1, §9).
- **In-loop snap-in (the FREE→RIDGE trigger)** = completeness: the upstream
  one-shot snap ran on the roughest geometry and missed corners regularize later
  straightens onto ridges. A controlled snap-in (a FREE corner reaching the crease
  → tag + RIDGE/DOF-1) catches them as they converge. One-way: once on, DOF keeps
  it there (§2).

### 4.1 The `settle()` cascade (replaces the R.1/R.2/R.3/R.4 "chase")

After the optimizer applies the single best `SLIDE` of a sweep, `settle()` drains
the topology events the move enabled — **local** to the changed corner, cascading
on survivors. There are two entry points (§8.0): a **seed pass** at iter 0 over
pre-tagged corners, and the **per-move** path below. Event-driven — never a
periodic full sweep, never a global scan.

```
LOOP until a full sweep changes nothing (cap = backstop):
    c, newpos = best_SLIDE_over_all_corners()       # the optimizer
    if none: break
    move c to newpos
    settle(c)

settle(x):                          # drain LOCAL topology events around one changed corner
    work = {x}
    while work not empty:
        c = work.pop();  if not alive(c): continue
        # each event is local to c + its path-neighbours; re-queue survivors/affected.
        # PRIORITY (load-bearing): MERGE_CORNER → SPLIT_TO_SHARE_CORNER →
        #   DROP_REDUNDANT_ON_RIDGE → DROP_COLINEAR.  The tag-drop MUST precede the
        #   angle-drop: that order + the single-plane gate is what stops DROP_COLINEAR
        #   from eating a freshly-shared corner (§4.2).
        if MERGE_CORNER(c):                 work += survivor;  continue   # same-plane dedup, OR cross-plane ridge-partner (guarded)
        if SPLIT_TO_SHARE_CORNER(c):        work += survivor;  continue   # on R, guard passes, NO partner → insert into B's edge
        if DROP_REDUNDANT_ON_RIDGE(near c): work += affected;  continue   # tag test, B1-conservative
        if DROP_COLINEAR(near c):           work += affected;  continue   # SINGLE-PLANE only (the invariant, §4.2)
        # nothing fired → c is settled

# Sharing a corner (the FREE→RIDGE transition; "SHARE" is a CONCEPT, not a 6th op):
#   fires when c is FREE, has reached ridge R, and the two-condition guard passes
#   (M1, §9): (B) a B corner within τ_partner of c → cross-plane MERGE_CORNER; else (A) c
#   falls in a B path-edge whose both endpoints carry R → SPLIT_TO_SHARE_CORNER
#   (insert c into that edge).  Either way: set c RIDGE, axis := R's crease dir (not
#   nA×nB — A3); reject/revert the whole op if it self-intersects (M5) or raises cost (S2).
# INVARIANT after every mutation: plane_ids[c] == {planes whose path holds c}  (P2⇔P3)
```

`MERGE_CORNER` reuses the existing redirect idiom (planes_dyn union + path
id-rewrite + dedup + alive + retag_dof) — it grows nothing, mints nothing, only
shrinks via dedup; **same safety class as today's R.2** (so it needs no new safety
analysis). **Survivor selection — reuse R.2/R.4's existing tie-break** (`impl.c`
MERGE: `dof` ascending, then plane-count descending, then `id` ascending — lower
wins): the **more-constrained** corner survives (a DOF-0 junction beats a DOF-1
ridge beats a DOF-2 free — this is the M4(a) junction-dominance), and `id` breaks a
true tie deterministically. The loser's `id` is rewritten to the survivor's
throughout the paths. **Pre-commit, like SPLIT:** run the M5 local edge-cross test
*before* the path id-rewrite; on a cross, reject (no state was mutated — there is
no post-commit unwind, so the test must gate first). `SPLIT_TO_SHARE_CORNER` is the
only genuinely new op and carries all the memory + atomicity burden — see §4.3.

> **MERGE_CORNER position is constraint-aware — reuse the corner-pool solver, do
> not midpoint.** The emission-time pool merge (`v7p_recompute_entry`,
> corner-pool.c:205) does *not* average naively: it ranks members by **authority**
> (FULL pin > AXIAL/on-ridge > FREE) and, for axial members, **solves the
> constrained position** (least-squares onto the intersection of the axis
> constraints). MERGE_CORNER should place a survivor by the same rule — two
> on-ridge corners merge to the **constrained meet of their creases**, not a
> midpoint. That same solver **is the M4(b) meet-solve** (a 3-plane junction = three
> axial constraints) — so M4(b)'s **position math is reuse** of the pool solver, not
> new geometry; only the **plumbing** (invoking it inside the regularize loop, where
> DOF-0 today never relocates a corner — §9 M4) is new. Caveat from the pool's own
> history: recompute any derived/cached field (e.g. `source`) from the **canonical**
> store, never per-member — that drift was the BM `v7p_entry_validate` bug
> (`same-concept-N-encodings`).

### 4.2 The two DROP operations — why both, and the invariant that keeps SPLIT safe

`DROP_COLINEAR` and `DROP_REDUNDANT_ON_RIDGE` are the two halves of "simplify,"
partitioned by **whether the corner is on a ridge**:

| | **DROP_COLINEAR** | **DROP_REDUNDANT_ON_RIDGE** |
|---|---|---|
| removes a corner adding no bend to… | one polygon's **outline** | a **crease** shared by two polygons |
| test | geometric **angle ≈180°** | topological **tag**: 3-in-a-row share one `ridge_id` |
| threshold | yes (angle tol — *flaps* near 180°) | **none** (exact tag — FP-robust) |
| applies to | **non-shared** corners (1 plane) | **shared / on-ridge** corners |
| kink safety | n/a | built-in: a junction has ≥2 `ridge_ids` → test fails → **kept** |
| decision scope | the corner's angle in its one polygon | **B1-conservative**: redundant in **every** path it's in |

**Why both are needed:** a shared on-ridge corner usually **isn't exactly
collinear in any single plane's 2-D projection** — the crease is a 3-D line each
plane sees foreshortened, and FP noise wobbles the angle right where the ≈180° test
is most sensitive. So the angle test is unreliable *and* asks the wrong question
(per-plane) for a shared corner; the tag test is exact and asks the right one
(is this on a straight crease run). Conversely a plain single-plane corner has no
ridge tag, so only the angle test can judge it.

**The invariant that makes `SPLIT_TO_SHARE_CORNER` safe:** SPLIT inserts cA *into*
B's straight edge, so cA is — by construction — **collinear in B's polygon**. A
naive DROP_COLINEAR would delete it next sweep and undo the split. Two guards stop
that:
1. **`DROP_COLINEAR` removes single-plane corners only** (the explicit design
   invariant — *all shared / on-ridge corners are simplified exclusively by the
   tag-based, B1-conservative `DROP_REDUNDANT_ON_RIDGE`*). After SPLIT, cA is shared
   `{A,B}` → out of DROP_COLINEAR's domain entirely.
2. **`DROP_REDUNDANT_ON_RIDGE` is B1-conservative** — it removes cA only if the
   tag test holds in **every** path containing it. cA was split into B because A's
   boundary *needs* a vertex there, so the test fails in A's path → cA is kept. The
   *only* time it's dropped is when cA is mid-straight-crease in **both** A and B —
   exactly when it *should* go (the shared edge is straight; the interior vertex is
   genuinely redundant). (Bonus: inserting cA between B's corners p,q leaves p's and
   q's own angles unchanged — cA lies *on* p→q — so no new collinearity is minted.)

**Why "redundant in *every* path" — the deeper reason (not bookkeeping).** It is
tempting to think a shared corner `X` could be dropped from just the polygon where it
looks straight, and kept in the one where it bends. It cannot — and the reason isn't
that the membership record would go stale (that's fixable). It's that **a shared edge
is a topological fact that needs the corner as an endpoint.** An edge is "shared by A
and B" only because *both* polygons list the *same* two corners as that edge's ends.
Take an `X` on the ridge that is dead-straight in A's outline but is where B's outline
*turns off the ridge*: `X` being straight in A tells you nothing useful, because `X`'s
job in A is not to mark a bend — it marks **the end of the shared edge** (the last
shared segment runs …W→X, then B departs and past X only A is on the ridge). Drop `X`
from A "because A is straight there" and A's edge now jumps W→Z *over* X, while B still
has W→X — so W→X is no longer an edge both polygons hold: **you have destroyed a real
shared edge.** The crease is still physically there; you've just stopped representing it
as shared. So a shared corner is only as removable as its *most demanding* polygon
allows — "geometrically redundant for shape" and "redundant for sharing" are different
questions, and a straight-in-one-polygon corner can be **topologically load-bearing** as
a shared edge's endpoint. The all-paths-agree rule is exactly what keeps us from
confusing the two; it removes `X` only when removing it from *all* its polygons leaves
every one of them spanning the same straight segment — i.e. when no sharing relies on it.

### 4.3 SPLIT_TO_SHARE_CORNER mechanics — memory growth + atomicity

SPLIT inserts an **existing** corner id `cA` into plane B's path, so B's path AND
the total (corner,polygon) membership each grow by one. That hits two stores
sized to *initial totals*, and needs an atomic multi-store update. (Resolves the
original review's memory items **MG1/MG2** — §9; *not* the B1/B2 of §9, which are
the collapse-rule and termination.)

**(a) Two fixed pools must become growable — pure realloc, copy the existing
pattern.** `pw_edges` (`impl.c:1266`) and every per-corner list (`corner_ctx_cap`
`:927`, `corner_to_pw_cap` `:1277`) ALREADY grow via `if(n==cap){cap=cap?cap*2:K;
realloc}`. Two stores do **not**, and would overflow on a SPLIT:
- `polys[pi].ids` — `cap` is set once (`:637`) and never used; the path only ever
  shrinks today.
- the global `ctxs[]` pool — sized once to `total_ctx` = initial Σ path-lengths
  (`:850`); REBUILD_CTXS writes `ctxs[n_ctx++]` with **no growth** → a SPLIT's +1
  membership overflows it. (`pw_edges` already grows, so it's fine.)

```
poly_reserve(p, need):               # CAPACITY only — never touches p.n or content (PREPARE-phase safe)
    if need > p.cap:
        newcap = max(need, p.cap*2)
        nb = realloc(p.ids, newcap*4)
        if !nb: return false          # original p.ids intact → caller aborts cleanly
        p.ids = nb; p.cap = newcap
    return true
# likewise planes_reserve(cA,+1) and ctx_reserve(): give ctxs[] a `cap_ctx` + the
# same reserve-guard; the actual length bumps happen only in COMMIT (§4.3c).
```

**(b) NEVER mint a new corner index (a constraint, not a task).** All ~8
per-corner arrays (`cs`, `planes_dyn(_n)`, `corner_ctx_*`, `corner_to_pw_*`,
`traj`, `out->dof`) + `corner_map` are sized to `n_corners` **once**. We avoid
growing them by only ever reusing existing ids: SPLIT inserts existing `cA`;
MERGE keeps an existing survivor. (Unlike static merge's pool, we do **not**
`upsert` synthetic corners — that path is unavailable here.)

**(c) The atomic unit (preserves the P2⇔P3 invariant) — compute-then-commit.**
`planes_dyn` (P2) is the **one** store not rebuilt from paths; everything else
regenerates from `polys[]`. The naïve "grow then insert then update" order is **not
rollback-safe**: the realloc that grows `B_path` runs *first*, so a later OOM (in
the `planes_dyn` union or a REBUILD) leaves the path already mutated. Fix: split
SPLIT into a **PREPARE** phase (every fallible step — the geometry guard and all
*capacity reservations* — touching **no** canonical store) and a **COMMIT** phase
(pure in-place writes into already-reserved space, so it **cannot fail**). Then the
multi-store update is genuinely atomic — abort in PREPARE leaves P2/P3 untouched.
```
SPLIT(cA, B, insert_pos):
  # ── PREPARE — fallible; mutates NO canonical store; any failure aborts cleanly ──
  if !guard_ok(cA, B, insert_pos):      return REJECT   # M1 coverage + M5 self-int, pre-commit (UV of B, §8.1)
  if !poly_reserve(B_path, B_path.n+1): return OOM       # grow capacity only — .n / content unchanged
  if !planes_reserve(cA, +1):           return OOM       # ensure planes_dyn[cA] has a free slot
  if !ctx_reserve(total_ctx + 1):       return OOM       # ensure ctxs[] cap (cap_ctx) for the +1 membership
  # ── COMMIT — allocation-free, infallible: pure in-place writes into reserved space ──
  insert cA.id into B_path.ids at insert_pos     # P3  insert_pos = the B path-edge whose both
                                                 #     endpoints carry ridge_id R (M1 cond. A, §9)
  planes_dyn[cA] ∪= {B}                           # P2  (the only hand-maintained, non-derived store)
  RETAG_DOF(cA)                                   # P6
  REBUILD_CTXS(); REBUILD_PW_EDGES()              # regenerate path-derived caches (capacity pre-reserved)
  assert invariant(cA)                            # P2 == {planes whose path holds cA}
  return OK
```

**(d) The burden is SPLIT-ONLY.** `MERGE_CORNER` (build step 3) reuses a survivor
and only shrinks (dedup) → grows no pool, mints no id. `DROP_REDUNDANT_ON_RIDGE`
(build step 4) only removes. So **steps 1–4 carry none of (a)/(b)/(c)** — the whole
memory+atomicity cost is isolated to step 5 (`SPLIT_TO_SHARE_CORNER`), which is why
it is last and optional. If MERGE+drop recover enough, (a)/(b)/(c) are never paid.

### 4.4 The shared edge EMERGES from the cascade (worked example)

The run-interval "splice" is **not a separate operation** — it falls out of the
`settle()` cascade: share the endpoints (`MERGE_CORNER`), then
`DROP_REDUNDANT_ON_RIDGE` fires on each plane's interior. Concrete trace. Ridge R,
`t = 0…10`; two **separate-corner** polygons along it:

```
A's run:   A0 ── A4 ── A7 ── A10     (A's corners at t = 0,4,7,10)
R:         ════════════════════
B's run:   B0 ────── B6 ────── B10   (B's corners at t = 0,6,10)
```
7 distinct corners; nothing shared; A's and B's edges are all single-plane.

1. **share the endpoints** (`MERGE_CORNER`, cross-plane partners): `A0≈B0 → S0`,
   `A10≈B10 → S10`; the interiors `A4,A7,B6` have **no near partner** → left
   single-plane. `A:[S0,A4,A7,S10]  B:[S0,B6,S10]` → shared *corners* S0,S10, but
   **no shared edge yet** (middles still differ).
2. **`DROP_REDUNDANT_ON_RIDGE`** (tag test): A `(S0,A4,A7)→drop A4`,
   `(S0,A7,S10)→drop A7`; B `(S0,B6,S10)→drop B6`. → `A:[S0,S10]  B:[S0,S10]`.

Now `{S0,S10}` is consecutive in **both** paths → **one shared edge** spanning the
ridge. **Sharing welded the endpoints; the drop swept each plane's interior so
both runs land on the same two corners.** (R.4-v1's proximity merge was the
degenerate 1-corner case — now just a `MERGE_CORNER`, no separate op.)

Caveats (the emergence's preconditions): (a) the endpoints must coincide and merge
— usually true (often already-shared RI junction corners); a run whose endpoint has
no partner (extents differ) needs `SPLIT_TO_SHARE_CORNER` for that *tip* only;
(b) a *shared* interior corner is dropped only by the **B1-conservative** rule
(removed from **all** its paths in one transaction, and only when every path agrees
— else a one-sided half-edge, M3). **Proof obligation:** the 2-polygons→1-edge
fixture (§5), once steps 1–4 land.

### 4.5 New helper interfaces (the signatures #4)

Three helpers are genuinely new; everything else reuses an existing routine
(`ridge_proj`, the redirect/dedup idiom, `poly_reserve`+the §4.3c atomic unit, the
M1 condition-A bracket). The SPLIT `insert_pos` is **not** here — it's the M1
condition-A path-edge (§9). Two of the three are *reuse-don't-duplicate* moves.

**(1) Constrained-meet position (`MERGE_CORNER`, M4(b)) — REUSE the authority
solver, do not reimplement.** Position a merge survivor by the corner-pool's
**authority-ranked** rule (`v7p_recompute_entry`, corner-pool.c:205): FULL > AXIAL
> NONE, then constrained least-squares. The loop's corners map onto it directly —
DOF-0 junction → **FULL** (dominates: this *is* M4(a)), DOF-1 ridge → **AXIAL**
(one constraint per plane/ridge), DOF-2 free → loose/target. **Do not reimplement
the constrained-meet math in the loop** (the `same-concept-N-encodings` trap).
*Mechanism is the implementer's choice* — either build transient pool members from
the merging corners and call `v7p_recompute_entry`, or extract a shared core:
```c
// recommended-but-not-mandated: lift the position branch (pool.c:236-289) into a
// shared pure function over a member array; the loop's corners ARE members
// (position + DOF→authority + plane→axis), so no fake structs.
void v7p_solve_member_position(const v7p_member_t *members, int n, double out[3]);
//   pool's recompute_entry calls it (after deriving `source` separately, which
//   stays out of the loop's path — that derivation was the old BM assertion bug).
```
Caveat: extraction is a **shared-module refactor** — extract, confirm the pool
tests still pass (no behaviour change), then add the second caller.

**(2) Local edge-cross test (`SPLIT`/snap geometry guard, M5 tier-1) — NEW caller
of the EXISTING primitive.** The whole-ring `v7_bp2_polygon_self_intersects_uv` is
O(n²); we need an O(n) test of just the changed edges, reusing the same crossing
primitive (`seg_seg_strict_intersect`, bp2-impl.c:115):
```c
// do the two edges incident to vertex `at_idx` cross any non-adjacent ring edge?
bool v7_local_edges_cross_uv(const double *U, const double *V, int n, int at_idx);
```
Run **pre-commit** (on the would-be ring), in the **UV of each ring `cA` touches**
(both A's and B's for a SPLIT — §8.1). Reuses the crossing math; only the
which-edges-to-test loop is new.

**(3) `ridges_incident_to(C)` — genuinely new (no existing equivalent).** Powers
the event-driven locality ("check only the moved corner's ridges"):
```c
int ridges_incident_to(uint32_t C, uint32_t *out);   // count, fills out[]
//   = C's ridge_ids tags (P4 — ridges C is ALREADY on)
//   ∪ { ridge r touching one of C's planes : perp(ridge_proj(C,r)) ≤ τ_in
//                                              && t ∈ [t0,t1] }   (newly REACHED)
```
Both relevances are needed: tags alone miss a FREE corner *reaching* a new ridge
(the snap trigger — it isn't tagged yet); proximity alone redoes work for already-on
corners. The newly-reached scan is bounded by a **per-plane→ridges index** built
once at setup (each ridge touches 2 planes), so per-move cost is O(C's 1–2 planes ×
ridges-per-plane).

### 4.6 P4 (`ridge_ids`) in-loop maintenance + DOF coupling (A2 — foundational)

The §2 model treats `ridge_ids` (P4) as a **maintained, DOF-coupled** property; today
it's a static input tag (`corner_state_t` has **no** field, `:220-229`; DOF is
**plane-count-only**, `:540-588`). M1's share-guard (cond. A reads tags),
`DROP_REDUNDANT_ON_RIDGE` (reads tags), and `ridges_incident_to` (tags half) **all
depend on this**, so it's foundational — it lands with build step 2, *before* any
SHARE fires. Build it mirroring `planes_dyn` (P2), the existing precedent for an
in-loop-maintained per-corner set:

- **Storage:** add `uint32_t *ridge_ids; uint32_t n_ridge_ids;` to `corner_state_t`
  (growable, same realloc idiom as `planes_dyn`).
- **Seed:** `memcpy` from `corners_in[].ridge_ids` at setup (as `planes_dyn` seeds
  from `plane_ids`).
- **Write sites — all *additive*, one-way** (no removal: a DOF-held corner can't
  drift off its ridge, so a tag never needs clearing — the maintained-property model,
  the same thing that dissolved S3):
  - **snap-in** (the FREE→RIDGE trigger, step 2): a corner reaching within `τ_in`
    perp of ridge R → `push_unique(ridge_ids, R)`. This is what tags on-ridge corners
    as the geometry converges, so the guard / DROP / `ridges_incident_to` have correct
    tags to read.
  - **`MERGE_CORNER`** (step 3): survivor `ridge_ids` = **union** of both (same idiom
    as the `planes_dyn` union).
  - **`SPLIT_TO_SHARE_CORNER`** (step 5): the inserted corner already carries R
    (that's why it's being shared onto R) — preserve, no extra write.
  - **`DROP_*`**: removes the corner; nothing to fix up.
- **DOF coupling** (extend `RETAG_DOF`): **take the MORE-CONSTRAINED of the
  plane-count rule and the ridge rule** — `dof = min(plane_dof, ridge_dof)`, where
  `ridge_dof = {n_ridge_ids ≥ 2 → 0, == 1 → 1, == 0 → 2}` (using the *reality-
  intersected* ridge set — a tag counts only if `rg[R].tmax>0` and the corner's
  `proj_t` lies in `[0,tmax]` within `τ_in` perp). For `== 1` set axis = that
  ridge's direction **`rg.rd`** (A3 — *not* `nA×nB`; guard `tmax>0`) **and
  `has_axis_a=true`** (else a DOF-1 corner with no axis generates zero SLIDE
  candidates and **freezes**). **This fixes the plane-count-only mis-tag** (a
  single-plane on-ridge corner: `plane_dof=2`, `ridge_dof=1` → **DOF-1**,
  slides-on-crease). *Why min, not ridge-overrides-plane (scout 2026-06-18):* the
  naïve "`n_ridge_ids==1 → DOF-1`" would **demote a real junction** — a 3-plane
  corner (`plane_dof=0`) whose other two ridge tags fall outside their clipped
  segments keeps only 1 surviving tag (`ridge_dof=1`); `min(0,1)=0` keeps it
  **pinned**, the override would wrongly un-pin it. Measured on dahouet: ~17 corners
  flip 2→1, ~1 flips 1→0, **0** wrong 0→1 demotions. **Prereq:** hoist the `rg[]`
  build above the R.0 DOF-tag/seed pass (it's built post-seed today; pure
  code-motion).

> **DOF follows TOPOLOGY, never proximity (decision 2026-06-23).** A crease (`ridge_id`)
> is tagged onto a corner only because the corner geometrically lies *on that crease* — but
> a crease exists between two panels, and we deliberately do **NOT** add a tag for a third
> panel merely because the corner is *near* it. Rationale: locking a corner to DOF-0 means
> "this is a genuine ≥3-panel junction," and junction-ness is **topological** (the corner is
> a vertex of those panels' polygons), not geometric proximity — the same principle as the
> Phase B membership SSoT (§2.4). If a near panel *should* join the junction, **make-shared
> wires its polygon onto the corner** → it becomes a 3-panel vertex and locks automatically.
> If make-shared declines (panel beyond τ), the corner genuinely isn't that panel's vertex →
> **DOF-1 (slide) is correct**. So a near-junction corner stuck at DOF-1 with one untagged
> proximate panel is **not a bug** — proximity-tagging it would re-introduce the geometric
> over-labelling Phase B removed. (Closed follow-up "ridge-tag near-junctions"; the only open
> adjacent question is whether the make-shared τ itself is the right cutoff.)

**The one behavior change with teeth.** The bookkeeping (field/seed/union) is
mechanical, but the DOF coupling changes which moves `SLIDE` explores for
single-plane on-ridge corners (a ridge-line search instead of free in-plane) — the
*intended* fix, but a real change. **Validate** (the §6 "DOF-1 help or fight WSD?"
A/B): expected to **help** (fewer DOF, on the true crease); confirm empirically, and
if it fights WSD on some corner class, that's the signal to revisit. Expected
positive.

## 5. Validation strategy (prove each step)

Every step ships with unit tests in `core/tests/v7_boundary_regularize_test.c`
(synthetic two-plane ridge fixtures) + the dahouet regression.

| step | unit test (synthetic) | invariant / regression |
|---|---|---|
| on-ridge detection (snap-in, one-way) | tagged corner → on; a FREE corner crossing τ_in → snaps on + becomes RIDGE/DOF-1; once on, a `SLIDE` nudge does **not** snap it off | DOF-1 holds it on the crease (no snap-out) |
| SHARE by MERGE_CORNER | single-plane cA + cB on an A\|B ridge → one survivor, `plane_ids={A,B}`, in **both** paths, DOF=1; position = **constrained meet** (pool solver), not midpoint | **consistency invariant** (see below) |
| SHARE by SPLIT_TO_SHARE_CORNER | cA on the ridge, B has only an edge there → cA inserted into B's path at the right place; B's path length +1; cA `plane_ids={A,B}` | invariant; cA **not** eaten by DROP_COLINEAR (single-plane gate) |
| SPLIT pool-growth (§4.3) | fixture forcing a `SPLIT_TO_SHARE_CORNER` (B edge, no B corner) → `polys[].ids` and `ctxs[]` grow without overflow; caches consistent; **run under ASan** | no heap overflow (ASan-clean); OOM path returns cleanly |
| **shared edge end-to-end — THE proof** | two **separate-corner** polygons sharing a ridge (the §4.4 worked example) → share endpoints + drop interior → **exactly one shared edge** (`n_plane_ids=2`) spanning the ridge, both paths on the same 2 corners | shared edge formed; corner count drops; invariant holds |
| DROP_REDUNDANT_ON_RIDGE (tag) | run of N on-ridge corners (`ridge_id` R) → reduced to its 2 endpoints; a **junction** corner (2 ridge_ids) is **kept**; shared corners removed from **both** paths | corner count drops; no kink erased; invariant holds |
| DROP_REDUNDANT_ON_RIDGE — asymmetric run (B1) | A's interiors share `R`, B has an interior that does **not** → the shared corner is **KEPT** (test fails in B's path), nothing erased; symmetric case → collapses to the clean shared edge | `P2⇔P3` holds in both cases; **no real corner erased**; conservative rule |
| DROP_COLINEAR vs SPLIT corner | a `SPLIT_TO_SHARE_CORNER` corner (collinear in B's outline, shared) → DROP_COLINEAR **skips it** (single-plane gate); a genuinely-collinear single-plane corner → removed | shared corners only ever simplified by the tag op |
| share-guard (M1) | cA near a ridge but B's boundary absent there (step ridge / past segment) → **NOT** shared; **U-shape void:** cA slides into the courtyard region of a one-record ridge → condition A finds B's path-edge there has an **off-ridge endpoint** → **NOT** shared (no cross-arm bridge); cA on a real arm with a both-`R` B edge → **shared** | no false unification across the void |
| termination | repeated sweeps on a fixture reach a fixpoint (no SHARE/DROP oscillation) | bounded sweeps; share/split events drain (B2, §9) |
| DOF derive (A2 coupling, **min-DOF**) | single-plane corner on ridge R → **DOF-1**, axis := `rg.rd` + `has_axis_a=true` (not DOF-2/free, not frozen); a **3-plane junction with only 1 surviving ridge tag → stays DOF-0** (`min(0,1)=0`, NOT demoted); degenerate `rg.tmax≤0` → tag ignored, no axis; near-parallel → no NaN | `dof=min(plane_dof,ridge_dof)`; mis-tag fixed, junctions preserved |
| convergence A/B (A2) | dahouet with DOF coupling on vs off → coupling **does not worsen** WSD/convergence (expected: improves — corners slide on the true crease) | §6 "DOF-1 help or fight WSD?" — measured, not assumed |
| 3-plane junction (M4) | **(a)** corner SHARE'd toward a vertex where an RI junction corner exists → **merges into it** (DOF-0, at the meet); **(b)** no RI corner there → synthesize DOF-0 **at the solved meet**, order-independent | result independent of ridge processing order; (b) raises the upstream-RI-gap flag |
| self-intersection guard (M5) | a snap/insert that would fold the ring (corner jumps past a neighbour edge; SPLIT at a wrong bracket, tested in **B's** UV) → **rejected** by the local edge-cross test; a fold forced → caught by full `self_intersects`; a thin **spike/sliver** (crosses nothing) → caught by `spike_penalty`, not by self-int | self-intersections **== 0**; spike_penalty bounded; signed-area sign unchanged; **bbox alone would miss it** |

**The consistency invariant (the key safety net), assert in a debug build after
every mutation:** for every live corner `c`, `plane_ids[c]` equals exactly the
set of planes whose polygon path contains `c`; and no path references a dead id.
A single `assert`/checker function run in the unit tests and (gated) in-loop.

**ASan note:** the regularize unit tests use **no CGAL**, so running them under
`build-dev` (ASan) is cheap and is the catch-net for the §4.3 pool-growth /
overflow class — do one ASan unit-test pass after the SPLIT step.

**Regression gates (dahouet bldg0):** pipeline clean to `model-score-postwall`;
corner count **drops** along ridges; shared edges (`n_plane_ids≥2`) **rise** vs
R.4-v1's 6; convergence wall-time **improves** (BR_PROFILE `[br-profile]`);
polygon count stable (no polygon loss); building bbox stable (no geometry blow-up);
20/20 existing v7 unit tests still pass.

**Performance hypothesis to measure (not assume):** the speedup is **cheaper
per-iteration** from earlier corner reduction on the corner-sensitive ~57%
(pairwise/collinear), **not** fewer iterations (cap is fixed, P2). Measure with
the 3-factor decomposition (`score_calls/r1_iter`, `cpu_ms/score_calls`) + the
new `br_p_MS` counter, at a non-cap-saturated cap, on the matrix harness
(`.tmp-br-matrices/`), pre/post.

## 6. Design parameters & scope to pin during implementation

**Guard status (after the 2026-06-17 review pass):** I1, M1, M2, M3, M5 **all
resolved** (§9); **M4 qualified** (§9). **No open adversarial guards remain** —
what's left here is design parameters to pin during implementation.

### Design parameters to pin
- **Share-guard = the two-condition tag-driven test** (M1 resolved §9): SHARE fires
  iff **(B)** a B corner is within τ_partner of cA in 3D (→ cross-plane `MERGE_CORNER`, no
  tag needed — proximity to an on-R point implies on-crease), **or (A)** cA falls
  within a B **path-edge** whose *both* consecutive endpoints carry `ridge_id == R`
  (→ `SPLIT_TO_SHARE_CORNER`, and that edge is the insert position). Void-safe by
  path-adjacency; no gap-tolerance to tune; rests on P4 tag integrity (conservative
  on tag gaps). Ties to the resolved I1 allowlist (§9).
- **Snap/merge thresholds — PINNED (2026-06-17): inherit R.4's tuned values, fixed
  metric (no density scaling).** The share machinery needs two distances, which
  *already exist* as R.4's parameters (R.4 was the crude make-shared, tuned on
  dahouet):
  - **τ_in** = on-ridge **perp** distance (corner→ridge-line) that triggers the
    one-way FREE→RIDGE snap. **Inherit `ridge_merge_coplanar_m` = 0.30 m**
    ("lies on the crease line"). Constraint: `τ_in ≥ nudge_step_m` (0.03 m) so a
    sliding corner sits in the band for several moves and can't step over it —
    trivially met (0.30 ≫ 0.03).
  - **τ_partner** = cross-plane **3D** distance (corner→corner) for M1 condition
    (B), the `MERGE_CORNER` weld. **Inherit `ridge_merge_dist_m` = 0.50 m** (tuned:
    dahouet's on-crease pairs sit ~0.40 m apart; 0.50 catches them without >0.8 m
    over-merges — see R.4 audit comment, `impl.c:56-60`).
  - These are **different geometric quantities** (perp-to-line vs corner-to-corner);
    keep them separate, as R.4 did. M1 condition (A) (the SPLIT bracket) needs
    **no** threshold — pure tag test.
  - **No density scaling** — these are **geometry tolerances** (how far apart two
    plane-polygon corners sit on a shared crease), a property of the plane-fit /
    polygon geometry, not of point sampling; regularize operates on corner/ridge
    geometry, not raw LIDAR points. Fixed metres is correct.
- ~~Collapse aggressiveness (cos threshold)~~ → **dissolved** (§9):
  `DROP_REDUNDANT_ON_RIDGE` is the tag-based test, kink-safe with **no angle
  threshold** to tune.
- ~~DOF-1 help or fight WSD?~~ → **ANSWERED (T3 A/B, `ae6430e`): it helps.** On
  dahouet, coupling ON vs OFF — composite **flat** (72.6105→72.6081, noise),
  regularize **wall −4.5%**, TOTAL_COST calls −24k, corners 104→103, no blow-up.
  Doesn't fight WSD; faster. Kept ON by default (`BR_DOF_COUPLE`).

### Scope — declared out (2026-06-16 review), with rationale
- **Holes / inner rings.** `polys[]` is a flat per-plane list; nothing
  distinguishes outer boundary from a courtyard/light-well ring. make-shared's
  t-bracketing says nothing about *which* ring — a SPLIT into the wrong ring is
  silent topology corruption. **Out of scope for now** (dahouet roofs are
  ring-simple); if a holed building appears, the per-ring self-int gate (M5) is
  the backstop, but the bracket logic would need ring-awareness. Flagged, not
  built.
- **A plane with multiple disjoint polygons.** `WSD_EVAL` takes the **first**
  matching path per plane and `break`s (`regularize-impl.c:980-984`) — a
  multi-component plane is already only partially scored *today*, independent of
  make-shared. make-shared's "B has an edge at `proj_t`" must pick the right
  component (ties to the M1 contiguous-coverage fix). **Out of scope**; noted so
  it isn't mistaken for covered.

## 7. Supersession & references

- **Supersedes** [[2026-06-11-merge-regularize-convergence-pathb]] — keeps its
  symptom/history/baseline; corrects its "cross-plane corner-pair merge"
  primitive (→ make-shared) and its "duplicated ridge property" claim (→ the
  geometric/topological split in §2).
- Builds on [[2026-05-21-boundary-polygons-tsp-rethink]] (why simplification was
  pushed downstream), [[2026-04-27-regularization-exploration]] +
  [[2026-05-10-regularization-cost-architecture]] (the regularize loop + cost),
  [[2026-06-11-d2b-side-vec-wrong-plane-frame]] (FP *sign* tests are the hazard,
  *magnitude* tests are fine — the basis for re-deriving geometric on-ridge).

## 8. Revised plan & forced decisions

The conceptual core survives (geometric vs topological; make-shared as the
unifying primitive). But the review forces concrete changes and one strategic
fork.

### 8.0 DECISION — in-loop, event-driven (reversed from an earlier "phased" lean)

**Chosen: in-loop, event-driven** — sharing is checked **per corner move**,
localized to that corner, inside the `settle()` cascade (§4.1) — plus one initial
seed pass over the input tags (RI ∪ snapped CC/CV, F1) for corners already
on-ridge at iter 0.

```
# this is the SHARE branch of settle(C) (§4.1), fired after C moved:
share_step(C):
    for R in ridges_incident_to(C):        # only C's ridges — O(C's edges), not a sweep
        if C is on R (tag, or snapped-in this move) and guards(C,R) pass:
            # "share" = whichever realizes it (SHARE is a concept, not a callable):
            MERGE_CORNER(C, partner) if a B partner is within τ_partner on R
            else SPLIT_TO_SHARE_CORNER(C, the both-R B path-edge)
    # the follow-on DROP_REDUNDANT_ON_RIDGE is a SEPARATE cascade step on the
    # survivor — it is what forms the shared edge (§4.4), not part of SHARE.
```

**Rationale (why in-loop beats phased here):**
- **A merge can only *arise* after a corner reaches a ridge** — a move event. So
  we check **only the moved corner's edges**, O(C's edges) per move, *not*
  O(ridges×corners) per sweep. The "sweep is too expensive to run often" plank
  of the phased case **does not apply** to event-driven.
- **Reduces corner count earlier** (than phased): the corner-sensitive cost is the
  **pairwise/collinear ~57%** (PF1), so a removed corner cheapens every later iter.
  *Honest caveat (2026-06-16):* τ_in waits for a corner to **converge** before
  snapping, so SHAREs fire **post-settle (~iter 16+)** — the saving lands on the
  already-cheap tail (the same tail cap=200 trims as <2%). So expect the **net**
  integral speedup to be **small**; "compounds over ~180 iters" overstated it. The
  per-iter saving is real; whether it dominates the *added* per-move cost (SHARE
  + the new O(n) self-int test) is exactly what the measurement must decide — don't
  pre-claim it. Phased keeps the full corner set through each SLIDE phase.
- **True co-convergence** (the Path-B vision): a merged DOF-1 corner immediately
  optimizes as a constrained corner.
- **The two reasons we'd have picked phased are gone:** the expensive-sweep one
  (N/A for event-driven, above) and FP-determinism (dropped, FP-reframe, §9).

**The one risk phased would have dodged — and why we judge it negligible.**
Promoting on *current* (mid-cascade) geometry could in principle pin a corner
that is only *transiently* near a ridge (a feature edge skimming past), since the
position-guards pass at that instant and the snap is one-way (sticky). **But if
the cost metrics are valid, corners are driven toward *real* topology and stay
*near their start* (moving only along their ridge/edge) — they do not roam the
building and cross ridges in passing.** So transient-near-ridge corners
essentially don't occur, and we **do not build a debounce now.** Kept *in
reserve*: if validation ever shows a transient pin, the fix is a per-corner
**settle gate** (SHARE only after C has been on-ridge for K iters / its last
move < ε) — a direct, earlier substitute for phased's settled-geometry, added
only if needed.

**S2 (cost-non-increase) becomes per-SHARE, not per-sweep:** check the
*localized* cost delta (only C's planes) at commit and revert the SHARE if it
worsens the total — cheaper and tighter than a whole-sweep check.

### 8.1 Corrections folded into the plan (traceability index)

The 2026-06-16/17 review's corrections — each now lives in its **canonical
section**; this is the index, not a restatement. (Underlying stance: the guards
keep every committed state **consistent + non-degenerate** and monitor quality
**directionally** — they do *not* prove the whole feasible set "good"; the
cost-check is a localized per-SHARE delta. FP-reframe, §9.)

| correction | canonical home | tag |
|---|---|---|
| Cost-non-increase per SHARE (revert if the localized delta of C's planes rises) | §8.0 | S2 |
| Ridge-type allowlist {ridge,hip,valley}; plumb `ridges[].type` into `rg[]` — **do first** | §9 | I1 |
| Two-condition tag-driven share-guard (B: partner within τ_partner; A: both-`R` B path-edge) | §6 + §9 | M1 |
| 3-plane: (a) merge-into-junction rule / (b) solve-the-meet guarded fallback | §9 | M4 |
| `DROP_REDUNDANT_ON_RIDGE` = tag test, kink-safe, **B1-conservative**, the edge-former | §4.2 + §4.4 + §9 | B1, M2 |
| Two-tier self-intersection gate (local pre-commit reject + sweep-boundary `self_intersects`+`spike_penalty`+area-sign) | §5 + §8.3 + §9 | M5 |
| Restate `P2⇔P3` + ship `br_check_consistency()` (must pass on `0ca5a27` first) | §8.2 step 1 + §9 | M3 |
| Termination via the one-way membership monovariant (analysis only, no new code) | §9 | B2 |
| Output re-emit: `polygon_paths` from final state (**mandatory**), `dof` (diagnostic) | §8.2 step 1 + §9 | — |
| Perf: the drop helps pairwise/collinear ~57%, not WSD; measure with the 3-factor decomposition (add `br_p_MS`) | §9 | PF1–3 |

### 8.2 Build order (each independently testable + revertible)

*New helper signatures referenced below are spec'd in §4.5: (1) the authority
position solver for step 3/5 — **DONE `6bf9958`** (`v7p_solve_member_position`,
pure refactor, byte-identical); (2) the local edge-cross test for step 5 — **DONE
`41c784b`** (`v7_local_edges_cross_uv`); (3) `ridges_incident_to` for step 2.*

1. **Consistency checker + stats + output re-emit.** *(Scouted 2026-06-17 — the
   gate was run; findings folded in. This is an **intentional behaviour-change**
   step, not a no-op — see (iii).)*
   (i) **Re-emit `polygon_paths` from final `polys[]` post-loop — MANDATORY.** Today
   it's a **pre-loop snapshot** (impl `:712-732`, before the loop at `:1540`), so it
   misses every in-loop R.2/R.3/R.4 merge — empirically, on dahouet `0ca5a27` the
   emitted paths carry **39 dead-id refs** (corners merged away in-loop). Mechanics:
   move the emission block next to the post-loop `corner_positions`/`corner_planes`
   re-derivation (`:2207-2231`), emit **all** final paths with `n≥3` of live ids
   (not just `changed`); the wrapper's `reemit_polygon_paths`/`reemit_edges`
   consume it unchanged. *Caveat (RISK 5):* don't disturb `out->dof[].alive` — it's
   the **CORNERS emission filter** (`reemit_corners :456`), not diagnostic; only the
   `.dof` *class* re-derivation is the nice-to-have.
   (ii) **`br_check_consistency` + per-op counters** (`n_merge_corner` /
   `n_split_share` / `n_drop_ridge` / `n_drop_colinear`, `br_p_MS`). Checker domain
   is **`alive && emitted` corners only**: for each, `corner_planes[c]` (deduped)
   == {plane_id of each emitted path containing `c`}; plus a **dead-id-in-path**
   assertion; plus **EDGES⇔POLYGON_PATHS** corner-id consistency. **Read the right
   sections: POLYGON_PATHS = `RG_SECT_POLYGON_PATHS = 0x0B`** (the doc's old "0x07"
   was the retired CORNER_MEMBERS section — stale). Corners in *no* path are out of
   domain (they're the legitimately-dead set, impl `:684-707`).
   (iii) **Gate — IMPLEMENTED + MEASURED (T0, commit `4b22de0`).** Run on the
   **working tree** (`reemit_edges`/R.4 don't exist at the bare commit, so
   "baseline" = current HEAD's pipeline). **Result on dahouet:** the re-emit (i)
   fixed **dead-id refs 41→0** and **EDGES⇔paths orphans 64→0** — but **P2⊋P3 only
   dropped 36→35: it does NOT go to 0.** So the scout's "re-emit is the whole fix"
   was wrong, and the **gate is still RED.** The residual is **not** R.0b
   asymmetry and **not** the stale emission — it's the **M3 reconciliation gap**:
   `planes_dyn` (P2) is unioned *up* on R.2/R.4 merges but never reconciled *down*
   when a corner is dropped from a plane's path (or a path collapses to `n<3`), so
   it stale-claims an orphaned plane.
   **T0b — DONE (commit `6d73595`): gate GREEN.** The 35 split into **26 in-loop-drop
   + 9 seed-origin over-claims** (upstream `corner.plane_ids` claiming a plane that
   never had a path — these *pre-exist* the loop, so an in-loop-only reconcile misses
   them). Fix: (1) **seed reconcile** — intersect the seeded `planes_dyn` with actual
   initial path-membership; (2) **`RECONCILE_PLANES_DOWN(ci)`** = rebuild
   `planes_dyn[ci]` from the canonical paths, called + `RETAG_DOF` after every
   path-shrinking op (R.2/R.4 MERGE, R.3). **`corner_planes` is emitted FROM the
   corrected `planes_dyn`, NOT re-derived from paths** — a rejected first attempt
   re-derived it, which makes the checker trivially green *by construction* and
   destroys its power to catch future make-shared desyncs. Gate now (independently
   confirmed): `dead_id=0 P2sup=0 P3sup=0 edge_orphan=0`. **The checker is now a
   clean, meaningful instrument — steps 2+ unblocked.**
   *Correction (de-dup, `6ae2def`):* T0b's R.2/R.3 reconcile was actually patched
   into the **GREEDY** chase copy only; the **DEFAULT GLOBAL_BEST** chase is a second
   hand-inlined copy that lacked it (the gate was green via the seed/R.4 reconcile +
   dahouet not tripping it). The two chase copies were **de-duplicated** into one
   `CHASE_AFTER_MOVE` macro (the correct body), so the reconcile/retag now reaches
   the default path — gate-green is honest, not luck. Verified byte-identical on
   **both** modes on dahouet (the added hygiene is inert here). Surfaced by the T4 scout.
   (iv) **Re-baseline — MEASURED through T0+T0b (the new §3/§5 baseline = post-T0b,
   gate-green).** T0's re-emit: EDGES 182→142, INTERIOR_SHARED 22→23, composite
   72.571→72.610. **T0b's planes_dyn reconcile then corrected R.4-v1's inconsistency
   further** (vs `4b22de0`): corners **118→104** (R.3 now removes corners correctly
   reclassified single-plane/collinear), **stale shared corners 54→29**, **real
   shared EDGES 7→12 (+5 — the design's "shared edges rise" goal already surfacing
   from a consistency fix)**, INTERIOR_SHARED 23→14, **composite 72.610→72.6105
   (flat — collinear removals are shape-preserving)**, building bbox at building
   scale (no blow-up; *watch-item:* corner-bbox z-extent 18.9→14.9 m — redundant
   high-z corners removed, composite-flat says benign). **These post-T0b numbers are
   the live baseline** every later step diffs against.
2. **P4 store + on-ridge detection + share-guard — DONE.** A2 (`ae6430e`):
   `ridge_ids` field + seed + merge-union + min-DOF coupling. T4 (`3a8f1a7`): ridge
   **type** plumbed (allowlist {ridge,hip,valley}; step/flashing excluded), the
   `plane→ridges` index, `ridges_incident_to`, the **snap-in** write (FREE→RIDGE in
   the unified `CHASE_AFTER_MOVE`, gated `BR_SNAP_IN`), and the **read-only
   two-condition M1 guard census** (counts only, share nothing). Gate green;
   byte-identical with snap-in off (scaffolding inert); snap-in on → wall −3.6%,
   composite flat. **Key finding:** on the default pipeline the guard reports **0
   would-share — because the legacy R.4 already merges the ~14 cross-plane
   candidates** (with `BR_NO_R4=1` the guard fires condB=3/condA=2). **R.4 is the
   crude make-shared that step 3 replaces** → the guard becomes the live yield then.
3. **`MERGE_CORNER`** (cross-plane) — **DONE `5843336`** (replaced R.4 in place, not
   a new op — R.4's scan *was* cond-B). Position via the **pool solver** (T1,
   constrained meet not midpoint); **M4(a) junction-merge for free** (DOF-0→FULL
   authority — trace shows rid=4/44 junctions merge DOF-0 at the meet); **type-gate**
   {ridge,hip,valley} (fixed an R.4 bug: it welded 2 step eaves on dahouet → fake
   shared edges); pre-commit self-int (T2); reordered above DROP (MERGE-first).
   Gate GREEN; xplane_merges=5 (R.4 was 8: −2 correct step exclusions, −1 marginal;
   **self_int_rejects=0 — no real welds lost**); shared corners flat (28), shared
   EDGES 13→11 (the 2 fake step edges removed; the real **rise is step 4's job**);
   composite flat. *Caveat:* the self-int *integration* reject path is unexercised
   on dahouet (0 rejects) — the primitive is unit-proven (T2, 4000×) but the
   end-to-end reject isn't; revisit if a building triggers it. Same-plane MERGE
   (old R.2) left as-is (it's already the same-plane MERGE_CORNER).
4. **`DROP_REDUNDANT_ON_RIDGE`** — tag-based, run in the cascade (kink-safe, no
   threshold). **This is the edge-forming step** (§4.4); MERGE+drop = the shared
   edge. **Conservative atomic rule (B1, §9):** a *shared* corner is dropped only
   when the 3-consecutive-share-`R` test holds in **every** path that contains it;
   then removed from **all** those paths in **one transaction** + `planes_dyn`
   reconciled (M3). Some-but-not-all → **keep it**. (Also: assert `DROP_COLINEAR`
   stays single-plane-only, so it never eats a shared corner — §4.2.)
5. **`SPLIT_TO_SHARE_CORNER`** — RISKIEST, last: insert position = the M1
   condition-A bracket (B path-edge with both endpoints carrying `R`, §9); needs the
   `poly_reserve`/compute-then-commit atomic unit (§4.3c). Ships with the
   self-intersection gate + the **M4-(b) meet-solve fallback** (synthesize a DOF-0
   corner at a real ≥3-plane vertex when no RI corner is there, + flag the gap) +
   invariant-after-insert.

Steps 1–4 = **the shared edge** (MERGE_CORNER the endpoints + DROP_REDUNDANT_ON_RIDGE
the interior, §4.4). `SPLIT_TO_SHARE_CORNER` (step 5) is only needed for a
**non-overlapping run-tip** — where one plane's run extends past the other's, so
the tip has a corner on one side and a straight edge on the other. The overlapping
span unifies without it.

### 8.3 Validation additions (from the review)

**The output is non-deterministic *by design* (FP-reframe, §9) — so tests assert
PROPERTIES, never exact output.** No byte-identical / v2-equiv regression. The
gates instead prove *every* run lands in a good state:
- **Hard invariants (the feasible-set-is-good guards):** self-intersection count
  `== 0`; per-polygon signed-area sign unchanged; consistency invariant `P2⇔P3`;
  no dead-id refs; **cost-non-increase per promote** (S2).
- **Quality deltas (direction, not exact value):** corner count drops, shared
  edges (`n_plane_ids≥2`) rise, DOF-histogram shifts free→ridge-slide/locked,
  `n_merge_corner + n_split_share > 0`, polygon count stable, building bbox sane — all from
  `v7-roof-graph-inspect` + a printed bbox.
- **Guard fixtures:** step-ridge-coincident-line, U-shape/past-segment, 3-plane
  promotion, collapse-kink-protection (these prove bad branches are *unreachable*,
  which is what replaces determinism).

Synthetic fixtures are necessary but **not sufficient** — perf + self-intersection
risks only surface on dahouet-bldgonly, where we assert the property gates above.

## 9. Resolved concerns (log)

Closed during the design + adversarial review (the §8 adversarial section was
dissolved into here + §6). Kept for the trail — each says how it resolved and
where it now lives.

**Feasibility**
- **MG1 / MG2** (the original review's *memory-growth* items, renamed from its
  "B1/B2" to free the B-series for the collapse-rule/termination above — paths
  can't grow / no new corner slot + atomicity) → **§4.3**: `poly_grow` + `ctxs[]`
  growth (copy the `pw_edges` idiom), the never-mint constraint, the atomic SPLIT
  unit; the burden is SPLIT-only. (Also caught the `ctxs[]` overflow MG1 first
  missed.)

**Data model**
- **F1** (`ridge_ids` not "always 0 outside RI") → **§2**: the input tag is **RI ∪
  snapped CC/CV** — a *richer* seed, not a liability; folded into P4 (maintained +
  DOF-coupled). Stale `corners-impl.h:182` comment **fixed in code**.

**Geometry scope**
- **I1** (step/flashing ridges) → resolved as a **scope decision, not a corruption
  guard** (the adversarial "step flattens a height jump between two roofs" was
  *wrong*). Verified from `classify_edge_type` (topology-impl.c:140-148): **step ⟹
  one plane's up-normal < 0.3 ⟺ tilt > 72.5° ⟺ a WALL**, so step = roof–wall (an
  eave); flashing = two near-coplanar planes (dihedral < 10°, unstable line);
  ridge/hip/valley = roof–roof creases. **Decision:** make-shared scopes to
  **{ridge, hip, valley}**; **skip step** (roof–wall — defer to wall-detect) and
  **flashing** (degenerate). Implementation (§8.1): plumb `ridges[].type` into
  `rg[]` (currently dropped) + the allowlist. *Caveat (2026-06-16):* "step ⟺ wall"
  is *necessary-not-sufficient* — a **steep roof facet (73–80°)** also trips
  `up<0.3` and gets typed `step`, so a genuine roof–roof crease on a mansard/steep
  dormer is **excluded** → make-shared **under-unifies** there. This is a
  conservative **false-negative, not corruption**; accepted. (A tighter gate would
  test actual wall-set membership, not the type label.)
- **M1** (U-shape / courtyard void-bridge) → **reopened 2026-06-16, RESOLVED
  2026-06-17 — the two-condition tag-driven share-guard.**
  *The problem (code-verified):* the code emits **ONE ridge record per plane pair**
  (`topology-impl.c:419-421` "one record per adj pair"; adjacency keyed `(a<<32)|b`
  :167/:198), `[from,to]` = a **single line clipped over ALL contact points** (:517;
  t-extents :303-315) with **no gap/run awareness**. So a plane-pair meeting on two
  disjoint arms (a courtyard) yields **one segment whose `[t0,t1]` spans the void**
  — the segment-clamp does **not** exclude a void-region corner. The earlier "two
  records at two `t`" premise was false. (The old static merge avoided this with
  `find_single_run`, merge-impl.c:285 — count contiguous on-ridge runs, refuse if
  ≠1 — which we dropped with boundary-merge.)
  *The resolution — SHARE fires only under one of two conditions, both
  expressible off the maintained tags / polygon structure (no geometric coverage
  scan, no gap-tolerance to tune):*
  - **(B) proximity / cross-plane `MERGE_CORNER`** (try first): ∃ a B corner within
    τ of cA in 3D → weld. **No tag needed** — cA is *on* R, so within-τ of cA ⟹
    within-τ of R ⟹ cB is on the crease; a B corner "not near any ridge" *cannot*
    be within τ_partner of an on-R point. Self-protecting. Auto-handles junctions: if the
    near corner is an RI junction (DOF-0), the survivor tie-break (dof asc) makes it
    win → cA merges *into* it = **M4(a) for free**.
  - **(A) bracket / `SPLIT_TO_SHARE_CORNER`** (fallback, no near partner): cA falls
    within a B **path-edge** — two *consecutive* corners in B's cyclic boundary —
    whose **both endpoints carry `ridge_id == R`** (the specific ridge). Two points
    on line R ⟹ the segment between them is on R, so this *is* "the edge runs along
    R," answered purely by P4 tags. That edge is the **SPLIT insert position** (so
    this also pins topic #4's t-bracket). **Disambiguation when >1 both-`R` B
    path-edge covers `t_A`** (a folded ridge — two arms project to the same t):
    pick the edge whose **segment is nearest cA in 3D** (cA is on R, so the correct
    arm's edge is the close one; the far arm is a void's-width away); **if still tied
    within ε, refuse** (conservative — don't guess which arm). The tag is the real
    gate; 3D-nearest only resolves the rare fold.
  - **else refuse.**
  *Why void-safe — path-adjacency is load-bearing:* in the void, the bracketing
  corners (north-run end, south-run start) **are** both on R but are **not
  path-adjacent**; B's actual path-edge over the void has an **off-ridge endpoint**
  (where B turns around the courtyard) → condition A rejects it. Condition B can't
  bridge a *wide* void (no near B corner); a *thin* void (arms nearly meet) merging
  is the benign near-junction case. *Why no gap-tolerance `g`:* the polygon boundary
  is a gap-free polyline, so point-sparsity creates no spurious gaps — coverage is
  the discrete tag question, not a geometric one.
  *Dependency:* M1's correctness now rests on **P4 (`ridge_ids`) tag integrity** —
  the in-loop maintenance still to build (A2: `corner_state_t` has no `ridge_ids`
  field yet). Error direction is **conservative**: a *missing* tag → refuse a
  legitimate share (under-unify, safe); a *false* bridge would need a corner wrongly
  tagged R off-crease, which the snap-only tag discipline prevents. Validation: the
  U-shape fixture (§5 guard row) — confirm no cross-arm unify.

**The splice twist (2026-06-15)** — the splice **emerges** from (in-loop promote)
+ (in-loop collapse); see §4 worked example.
- **M2** (R.0b "collision") → **resolved: R.0b is *half the splice*, not a
  collision.** Its tag-based collapse (3 consecutive sharing a `ridge_id` → drop
  middle) is exactly the edge-forming collapse — run it pre-R.1 **and** in-loop.
  And the agent's premise ("§2 deprecates `ridge_ids`") was wrong: the tag is
  *maintained*, so R.0b reads it correctly.
- **M5-kink + the "collapse aggressiveness" param** → **dissolved.** Tag-based
  collapse is **kink-safe by construction** (a junction carries 2 ridge_ids → the
  all-three-share-one test fails → kept), so there's **no angle threshold** to
  tune and no kink-erasure risk. (The *self-intersection* half of M5 is resolved
  by the two-tier gate in the **M5** entry below.)
- Consequence: **collapse is the edge-former** (not merge), **SPLIT shrinks to the
  non-overlapping run-*tip* only**, and **the atomic-collapse-of-shared-corners
  becomes the linchpin** of a *correct* shared edge — its rule now pinned
  conservative (B1, §9).

**M3 reclassified (2026-06-15)** — M3 ("consistency invariant `P2⇔P3`") was never
an open *question*; it's a known implementation requirement. It has **no decision
to make** — only work to do, already threaded into the plan:
- a **pre-task gate** — §8.2 step 1 ships `br_check_consistency()` and runs it on
  baseline `0ca5a27` *before* any make-shared code;
- a **standing assert** — §5 (debug build, every mutation) + §8.3 (hard
  invariant);
- a **validation test** — the invariant column on every step's row in §5.

The one genuine unknown — *does the invariant already hold on `0ca5a27`, or does
R.0b's per-path one-sided removal break it?* — is **resolved by running that gate,
not by debate**: pass → proceed; fail → fix R.0b to atomic-multi-path first. Hence
moved out of §6's open-guard list — nothing there is undecided.

**Why the invariant *can* already be false (the R.0b mechanism).** Three facts
about the committed code combine into a latent `P2≠P3`:
1. **R.0b edits one path at a time** (`impl.c:648–683`) — it splices a redundant
   ridge-midpoint out of a *single* polygon's corner list.
2. **It marks a corner dead only if it ends up in *no* path** (`impl.c:688–707`).
   A corner removed from one path but still present in another stays **alive**.
3. **`planes_dyn` (P2) is initialized *after* R.0b** (`impl.c:876–894`), straight
   from the static `corner.plane_ids` (P1), and is **never reconciled** with the
   path edits R.0b just made.

So if R.0b removes a *shared* corner from plane A's path **asymmetrically** — the
"three consecutive share a `ridge_id`" test holds in A's traversal but not B's —
the corner survives in B's path with `planes_dyn = {A,B}`, yet is a vertex of
only B. That is exactly a one-sided half-edge: `P2 = {A,B}` ≠ `P3 = {B}`. Whether
this *actually* occurs on dahouet is what the step-1 gate measures (symmetric
removals — the clean-ridge common case — delete from both sides and stay
consistent; it's the asymmetric case that bites).

**B1 — the atomic-collapse rule — resolved (2026-06-16: conservative).** The
in-loop collapse *is* R.0b reused, so it inherits R.0b's per-path asymmetry: for
a corner `X` that's a vertex of **both** A's and B's polygons, the 3-consecutive-
share-`R` test is evaluated separately in each path against **different
neighbours**, and the verdicts can disagree. The doc named "remove from all paths
atomically" but never said *on what condition* — the hole. Two rules, opposite
failures:
- **remove-if-ANY path flags** → drops `X` the moment one traversal says collinear
  → **erases a real kink** (X is a genuine corner of the other polygon);
- **remove-if-ALL paths agree** → keeps `X` unless every path says collinear →
  may **leave a half-formed edge** (consistent, just not fully simplified).
**Decision: the conservative remove-if-ALL rule.** Collapse a shared corner only
when the test holds in **every** path containing it, then remove it from all in
one transaction + reconcile `planes_dyn`; otherwise **keep it**. Rationale:
**kink-safe by construction stays literally true** (never delete a corner any
polygon needs); the cost — an un-collapsed corner on a genuinely-asymmetric span —
is a **consistent** state (`P2⇔P3` holds), self-correcting as neighbours converge,
worst case one extra vertex (a quality nit, not corruption). This keeps collapse
**monotone + `P2⇔P3`-preserving**, which M3's checker and B2's termination both
need. Trade accepted: *never corrupt, occasionally under-simplify* over
*occasionally erase a real corner*. **Revisitable** — if behaviour shows too many
un-collapsed survivors, reconsider the aggressive rule + a separate kink guard.
Validation: §5 "collapse — asymmetric shared run" + "splice end-to-end" rows.

**B2 — termination — resolved (2026-06-16: analysis only, no new code/guard).**
The *old* proof was "R.2/R.3 only ever remove corners → the loop drains and stops
on `!any_move`." Make-shared's SPLIT **grows** a polygon, which superficially
breaks that proof — so termination must be re-argued. It still holds, via a
**different monovariant**:
- **Membership is one-way** — a single-plane corner only ever becomes shared
  (sticky snap, no snap-out; the same property that dissolved S3), and **nothing
  mints a new single-plane on-ridge corner mid-loop** (SPLIT inserts an
  *existing* corner; MERGE/collapse only remove; M4(b) mints a *DOF-0* corner,
  which is not a promote candidate). So the set of possible **promote/split events
  is finite and strictly drains** — bounded by the initial single-plane count.
- **Collapse only removes** corners (monotone).
- **Geometric slides** are bounded by R.1's existing cost-descent + the `cap=200`
  backstop (which thus stays a *true* backstop, not a band-aid).
The only obligation is a **design invariant to preserve** (not code): *no op mints
a new single-plane on-ridge corner, and no corner un-shares.* Keep it true as the
ops are written and termination follows. (A corner the guard *rejects* stays a
candidate and is re-checked each sweep — a **no-op** that doesn't set `any_move`,
so a minor **perf** cost only; optionally latch rejected-at-converged corners
later — not a correctness issue.)

**M4 (T-junction / 3-plane vertex) — qualified (2026-06-15; wording corrected
2026-06-16): (a) merge-into-existing as the rule, (b) solve-the-meet as a guarded
fallback.** The question is about a corner that, via successive promotions,
reaches a point that is a *real* 3-or-more-plane intersection. Two cases:
- **(a) a corner already sits near the meet** — `boundary-corners` materializes RI
  junction corners as the **closest-approach midpoint of two ridge lines**
  (`signals-impl.c:149-151`), and for a 3-plane vertex an **incremental pairwise
  average** of those midpoints (`:282-284`) — **not** an analytic 3-plane solve.
  So the corner lands **within `merge_thresh` of the meet**, not exactly on it
  (averaging error: tiny for a clean junction, larger for skew/noise). The moving
  corner is **promoted-by-merge into it**, inheriting that small placement error —
  acceptable, and it reuses the proven MERGE idiom. **This is the rule.** It
  normally arrives DOF-0 (all 3 pairwise midpoints merge → 3 planes).
- **(b) no corner is at the meet** — reachable, not vanishingly rare: if a pairwise
  midpoint exceeds `merge_thresh` the vertex **splits** into separate DOF-1 corners,
  or only one ridge is detected. A **guarded fallback**: synthesize a DOF-0 corner
  *at the solved 3-plane intersection*. Today's DOF-0 only **freezes a corner in
  place** (`regularize-impl.c:1551` skips DOF-0 in R.1; :545-547 sets DOF-0 with no
  axis, no relocation) — it never *computes* a meet — so (b)'s **plumbing is new
  code** (relocating a corner inside the loop), though its **position math reuses the
  corner-pool solver** (`v7p_recompute_entry`, §4.1), not new geometry. (b)
  firing is *also* a hint to check for an upstream RI gap, but treat it as an
  expected branch, not an exception.

Mechanism that makes this safe-by-default: DOF is set by **plane count** (≥3 → 0,
2 → 1, 1 → 2), and existing junctions are **never disturbed** — collapse keeps
them (kink-safe: ≥2 `ridge_ids`), promote skips them (already multi-plane). M4's
only real bite is a *newly synthesized* 3-plane corner, which the (a)/(b) rule
now defines: merge if a junction is there, solve-and-flag if not.

**M5 (self-intersection / winding) — resolved (2026-06-15; corrected 2026-06-16):
two-tier gate, partly reusing existing helpers.** A promote's snap (a discrete position *jump*) or
a SPLIT at a wrong bracket can cross a neighbour edge → a non-simple polygon
(fold / figure-8). **The danger is that the bbox regression gate is blind to it**
— a figure-8 has the same bbox as the clean ring — so a fold passes undetected
and corrupts downstream consumers. No extent check can catch it; an explicit
geometric test is mandatory. Verified state: regularize has **no** such check
today, but `v7_bp2_polygon_self_intersects_uv` already exists, is tested
(`v7-bp2-test.c`), and is a hard guard in sibling boundary-polygons-2; a
signed-area helper exists in classify. **Pinned design:**
- **(1) per-mutation local reject — NEW code, not a reuse.** The existing
  `v7_bp2_polygon_self_intersects_uv(U,V,n)` is **whole-ring O(n²) only**
  (`:128-136`) — no "2 edges vs ring" entry point — so tier-1 is a small new
  helper: test the corner's **2 changed edges** against the rest of the ring
  (O(n)). **Run it in the UV frame of the polygon being mutated** — for SPLIT the
  fold is in **B's** ring (t-order ≠ B's winding order), and a snap that also
  moves cA in A's ring must be tested in **both** A's and B's UV. If it crosses,
  **reject the op** — the test **must run pre-commit**, before any P2/P3/DOF/cache
  mutation, so revert is "nothing happened" (MERGE/SPLIT have no post-commit unwind).
  This *prevents* the bad state rather than detecting it after.
- **(2) sweep-boundary hard invariant** — full `v7_bp2_polygon_self_intersects_uv
  == 0` (reuse) **+ `v7_bp2_spike_penalty_uv`** (exists, `:145`, default 30°) —
  self-int alone **misses spikes/slivers** (bp2 runs *both*, `:265-269`; a thin
  outward spike crosses nothing and doesn't flip the area sign) — **+ signed-area
  sign unchanged** per touched polygon. Cheap: plane polygons are small (~10–30
  corners), so even the O(n²) full test is hundreds of ops. In §8.3's invariants.
- **Scope:** the make-shared ops where positions *jump* (snap, SPLIT-bracket).
  R.1's pre-existing incremental DOF moves are out of scope (gradient-local; would
  already show if they folded routinely).

**Output-emission gap (2026-06-16 review) — NEW, must be fixed before the design
ships its result.** Code-verified emission order in `regularize-impl.c`:
`out->dof` is filled at **:593-603 (R.0, pre-loop)** from the *initial* plane-count
DOF; post-loop only `.alive` is patched (**:2234-2238**) — `.dof`/axes are **never
re-derived**, so in-loop `RETAG_DOF` (and make-shared's DOF changes) **don't reach
`dof.json`**. `out->polygon_paths` is emitted at **:710-733 (post-R.0b, pre-loop)**
and **never re-emitted** — so R.1's MERGE/R.4 path edits (and make-shared's
shared-edge formation, *the whole point*) **don't reach `polygon_paths`**. Only
`out->corner_planes` (**:2215-2231**, final `planes_dyn`) and `out->corner_positions`
(**:2206-2213**, final `cs[].point`) are re-derived from end-state.
- **Implication:** make-shared mutates membership/topology in-loop; its result is
  faithfully exported only via `corner_planes`/`corner_positions`. The *edges* and
  *DOF* it produces are lost unless we **re-emit `polygon_paths` + `dof` from final
  loop state** (build step 1's territory).
- **Consumer check — DONE (2026-06-16):**
  - **`POLYGON_PATHS` is LOAD-BEARING.** Both downstream steps read it as truth —
    `classify` (`v7-boundary-classify.c:263`) and `wall-detect`
    (`v7-wall-detect.c:431`). The wrapper re-emits it from `out->polygon_paths`
    (`reemit_polygon_paths`, `v7-boundary-regularize.c:539`), which the impl fills
    **pre-loop** (`:714`, before the loop at `:1540`) capturing **R.0b only**. So
    make-shared's in-loop path edits **never reach classify/wall-detect** unless we
    re-fill `out->polygon_paths` from the **final `polys[]`** post-loop. → step-1
    re-emit is **mandatory**.
  - **`dof.json` is DIAGNOSTIC.** It is **not** a roof-graph section — it's dumped
    only by the `regularize-only-bench` tool (`dump_dof_json`,
    `v7-boundary-regularize-only-bench.c:77`), not consumed by classify/wall-detect/
    score. Its stale `.dof` is low-severity; re-emit is nice-to-have (so the
    diagnostic reflects make-shared's DOF), not load-bearing.
  - **Pre-existing bug — CONFIRMED EMPIRICALLY (T0 scout, 2026-06-17, on `0ca5a27`
    dahouet):** the three in-loop path mutators are **all `true` by default**
    (`impl.c:51/53/55`), so R.2/R.3/R.4 rewrite `polys[]` in-loop while
    `out->polygon_paths` stays the pre-loop snapshot. Result on the emitted graph:
    **39 dead-id refs** (every one a corner merged away in-loop) and **38/120
    corners with P2⊋P3** — so the §8.2 step-1 gate **fails today**, and it's this
    stale emission (not R.0b asymmetry) that dominates. The post-loop re-emit
    (step 1.i) fixes all of it; R.0b only if a residual remains. **Caveat:** the
    re-emit is therefore an *intentional behaviour change* (EDGES rebuilds from the
    corrected paths → classify INTERIOR_SHARED + wall-detect move) — re-baseline §5,
    don't assume (§8.2 step 1.iv).
- **Checker corollary:** `br_check_consistency` (M3, §8.2 step 1) must assert
  `P2⇔P3` on the **emitted** records, not just in-memory `polys`/`planes_dyn` —
  the gap above is exactly an in-memory-vs-emitted divergence.

**Cadence / FP**
- **Cadence (phased vs in-loop)** → **§8.0: in-loop, event-driven** (check only the
  moved corner; reduces corner count earlier).
- **S1 / determinism** → **dropped by design**: non-deterministic convergence is
  accepted; correctness = **guards keep every state consistent + non-degenerate,
  quality monitored directionally** (the §6 guards), not reproducibility. No
  v2≡v3, no byte-identical test. *Caveat (2026-06-16):* the guards are
  local/instantaneous and cost-non-increase is a per-promote *localized* delta —
  they do **not** prove the *whole feasible set* is good (local monotonicity ≠
  global descent). To back the descent claim, add a **global** cost check at
  sweep boundary (the in-loop topology edits are exactly what a localized check
  can't see). Don't assert "feasible set all-good" as proven.
- **S2** → kept as a **guard** (not an FP issue): per-promote cost-non-increase
  (§8.1).
- **S3** (threshold flap) → **dissolved** by the §2 one-way snap (no snap-out → no
  flap; a single `τ_in`, not a band).

**Performance understanding** (PF-series — distinct from the §2.4 property catalog
P1–P7)
- **PF1** (WSD is O(points), not corners) → **§8.1 perf-reframe**: drop helps the
  pairwise/collinear **~57%**, not WSD; the measurement is a §5 task.
- **PF2** (always hits cap) → **not a failure**: cap=200 is the deliberate trim of
  the meaningless tail (geometry settles by ~16). Speedup = cheaper-per-iter, not
  fewer iters.
- **PF3** → stopping criterion is `!any_move` (`L2129`), not `bestImprovement<1e-9`.

**Open-questions closed**
- hysteresis → one-way `τ_in` (§2); cadence → event-driven (§8.0); FP/determinism
  → accepted (above); walls/step → folded into the **I1** type-allowlist (the
  *answer* is settled; I1 the *guard* is still in §6 to implement).
- Carried-forward from the superseded Path-B entry: edge-merge candidate
  generator → became make-shared; EDGES↔CORNERS → the rebuild + invariant;
  shared-edge tally → in `v7-roof-graph-inspect`; RIDGE_LINES → use RIDGES
  `from/to` + normals; D.2b "simplification belongs downstream" → that's this loop.
