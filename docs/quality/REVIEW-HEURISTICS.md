# Review heuristics

Cross-cutting patterns worth checking in review, distilled from bugs we've
actually shipped + caught. Each is a *smell* (not a hard rule) plus how to
spot it and how to fix it. Add entries as new recurring shapes surface.

---

## 1. One concept, N encodings (cache-vs-canonical drift)

**Smell.** The same conceptual property is stored in more than one place,
kept in sync by hand-written code at each mutation site. The moment the
data model evolves, *some* of those sites get updated and others don't —
and the copies silently disagree.

**Symptoms when it bites:**
- *Silent wrong answer* — a reader picks the stale copy and nobody errors.
- *Invariant violation* — a `validate()` that only runs in Debug fires (or
  worse, doesn't exist).
- *FP "coin-flip"* — two code paths compute "the same" value slightly
  differently and a downstream `==`/threshold flips on the difference.

### Case study A — corner `source` (v7-corner-pool, audit #254-263)

A pool corner's "source" classification lived in **two** places:
- `members[i].source` — per-contributing-anchor source, and
- `source_per_plane[k]` — per-plane source (the later-added canonical store),

with a cached `e->source` derived from one or the other depending on which
mutator last touched it. A Stage 2.7 eave-merge that reused an existing
convex corner therefore reported `POLYGON_MERGE` under the old derivation
(`members[]`) but `CONVEX` under the new one (`source_per_plane[]`).

This is exactly what diverged `v7_boundary_merge_test` during the 2026-05-25
merge: the same input produced ≥2 `POLYGON_MERGE` corners pre-merge and 0
post-merge — not a regression, but the cache being re-derived from a
different axis. The fix (#254-263) made `source_per_plane[]` the **single
canonical store** and `e->source = max-priority(source_per_plane[])` a
derived value recomputed by one writer (`v7p_recompute_entry`).

> Test consequence: assert the *effect*, not the cached label. The merge
> test now checks "≥2 corners span both planes" (the real eave-merge
> outcome) instead of the `source` enum, so it survives canonical-store
> changes. See `docs/quality/FRAMA-C-FALSE-POSITIVES.md` #5–#7 and the
> proposed ACSL on `v7-corner-pool.h`.

### Case study B — per-step `blob_t` copies

Each v7 step that emitted an `ARRAY_INLINE` artifact carried its own local
`blob_t` accumulator with its own framing + overflow guard. They drifted:
the HS-2 / V7-1 audit found the `blob_reserve` overflow guard present in
some copies and missing in others. The merge unified them into one shared
`core/include/v3/blob.h` so every emitter inherits the same (guarded)
framing.

### How to spot it in review

- Grep for the field name. If a value is *assigned* in ≥2 places that
  aren't a single setter, treat it as suspect.
- Ask "if I add a new way to construct/mutate this object tomorrow, how
  many places must I remember to update?" If the answer is >1, it's this.
- Look for `validate()`/assert functions that re-check consistency between
  two fields — their existence is a confession that the two can disagree.

**Automated radar.** This heuristic is now partly mechanised. The Full tier
runs two **report-only** code-duplication detectors that surface the
copy-paste form of "N encodings" before review even starts:

- `scripts/ci/steps/run-cpd.sh` — PMD CPD over the C surface (`steps/` +
  `core/src/`, excluding `third_party/`). It is what flags the
  `parse_corners` / `parse_edges` / `parse_polygon_paths` decode loops
  copy-pasted across the v7 boundary step wrappers (the prototypical "same
  concept, hand-synced at N sites" shape).
- `scripts/ci/steps/run-jscpd.sh` — jscpd over `apps/lab-ui/` JS.

Both are **non-gating** — they produce a clone inventory (JUnit + raw report
under the CI artifact dir) for a human to triage, not a merge blocker. A
cluster they flag is a *candidate* for the "pick one canonical store / derive
the rest" fix below; a cluster they *don't* flag (sub-threshold, or logically
duplicated without being lexically duplicated) still needs the human grep.
See `README.md` → "Code-duplication detection".

### How to fix it

1. **Pick one canonical store.** The richest / most-authoritative encoding.
2. **Derive everything else from it** — ideally via an accessor (compute on
   read, no cache), otherwise via a single writer + explicit `recompute`
   that every mutator calls before returning.
3. **Encode the derivation as an invariant** the analyzer or a test can
   check (ACSL postcondition, or a property test on the public API).
4. **Test the effect, not the cache** — assertions should target the
   observable outcome, which survives a change of canonical store.

---
