# ADR-001 — Hash model: content-addressing + intent map

**Status**: accepted (2026-04-21); implemented in commit `f359b50` (B3.5 hash-model refactor).  See [`HASH-MODEL.md`](../HASH-MODEL.md) for the full design and shipped state.

## Context

The shipped v3 code (Phase A + B1–B3) used a single `output_hash: DERIVATION | CONTENT` flag per step write to decide both:

- **Concern A — cache-skippability on rerun.** Only DERIVATION was skippable, because its key could be pre-computed from the step's identity before running the step. CONTENT required running the step to discover its output hash.
- **Concern B — convergence across upstream chains producing identical bytes.** Only CONTENT converged, because its key was derived from the output bytes. DERIVATION forked: different chains with the same result got different store keys.

A step author had to choose one benefit or the other per write. The steps audit (`docs/audit/STEPS-AUDIT-2026-04-21.md`) surfaced symptoms of this conflation:

- `fetch-ortho-rgb` / `fetch-elevation` used `CONTENT + fetch_get` → always re-HTTP on rerun.
- Funnel steps (`canonicalize-location`, `find-copc-tiles`, `fetch-copc-tiles`, `laz-extract-disc`) used `CONTENT` → could share downstream cache but always re-executed the funnel step itself.

Additionally, the fetcher library had a parallel caching primitive (`fetch_and_cache`) that duplicated the logic of step-level caching at URL scope, making the architectural story fragmented.

## Decision

Retire the `output_hash` flag. Replace with:

1. **All step outputs are content-hashed.** Every entry in the store is keyed by `hash(bytes)`. Convergence across chains is automatic (`store_put` is idempotent on matching keys).

2. **A persistent intent map** maps `step_identity → content_hash`. Step identity = `hash(step_name, version, canonical_params, sorted_read_hashes)`. A step rerun with identical identity reads the content hash from the intent map and skips the step's exec entirely.

3. **One runtime primitive, `cached_compute(identity, fn)`**, orchestrates the whole flow: lookup in intent map, run `fn` on miss, hash the output, store, record the intent-map entry. Called at step scope by the runner and at sub-scope by step bodies (e.g., per-URL caching inside `fetch-copc-tiles`).

4. **`fetch_and_cache` becomes a thin wrapper** over `cached_compute` with a URL-derived identity. No longer an architectural primitive; a convenience helper for fetcher steps.

5. **Store gains a hot tier** alongside the existing warm (disk) tier. `store_put(key, bytes, len)` takes ownership of the heap buffer; downstream `store_lookup` returns a direct pointer (hot) or mmap (warm). **Zero copies** between producer and consumer in the same pipeline. Demotion and eviction governed by LRU + refcount/pin semantics.

6. **`skip_on_identity: bool`** (default `true`) stays as a per-write escape hatch for genuinely non-deterministic steps. Rarely used.

## What's retired

- The `step_output_hash_t` enum and the `output_hash` field on `step_write_t`.
- The separate derivation-key path in `runner.c`'s `execute_step`.
- `fetch_and_cache` as a standalone architectural primitive (it remains as a helper).
- The `_owned` vs `_copy` store-write API split we briefly considered — superseded by "one function, it takes the buffer; caller that needs to preserve bytes does `malloc + memcpy`."
- The "reverse index for leaf-eviction cascade prevention" we considered — deferred to Phase C behind the simpler validate-on-use recovery path.
- The "two options per step" 2×2 matrix framing (skip × converge) — the matrix collapsed once we recognized skip is handled universally by the intent map.

## Consequences

**Positive:**
- Single cache mechanism across step-level and sub-scope identities. One concept, one primitive, one data structure.
- Funnel steps gain cache-skip automatically — previously paid the re-execute cost every invocation.
- Plain fetchers (`fetch-ortho-rgb`, etc.) gain cache-skip — previously re-HTTPed on every rerun.
- Zero-copy between steps in the same pipeline — no unnecessary disk round-trip.

**Costs:**
- ~940 LOC of code churn (~640 new + ~300 touched). Mostly new intent-map library + hot-tier in store + runner.c rewrite of `execute_step`. See `HASH-MODEL.md` §12.
- Ongoing storage overhead: intent-map log grows with each new step-identity + content-hash pair (65 bytes per entry, periodically compacted).
- Soft-miss recovery path (validate-on-use in §11.2) adds one `store_contains` check per manifest entry on every dispatch. Microseconds; dominated by the parse that was already happening.

**Risks:**
- The hot-tier lock is held during synchronous demotion (write + fsync + rename), which can take ~30 ms for a 500 MB blob on SSD. Acceptable at our scale; revisit if profiling shows contention.
- Evicted leaves referenced by live manifests trigger recompute (see `HASH-MODEL.md` §11.2). Rare given TB-scale prod budgets; managed by the validate-on-use policy.

## Alternatives considered and rejected

- **Keep the split flag, add a two-level cache (intent map for skip, output_hash for store key).** Progress, but `DERIVATION` became redundant once the intent map existed — the derivation key is just a tautological `intent_map.put(deriv_key, deriv_key)`. Simplified to content-only.
- **Content-only outputs, but keep `fetch_and_cache` as a distinct primitive.** Rejected once we noticed URL-keyed caching is structurally identical to step-level caching — just a different identity scope. Unified under `cached_compute`.
- **`_owned` / `_copy` store-write API split.** Rejected because "ownership" in C is a free() convention, not enforceable. One function that takes the buffer + explicit `malloc + memcpy` at sites that need to preserve is simpler and more visible.
- **Reverse-index cascade tracking.** Deferred: the validate-on-use recovery handles the rare case adequately without the extra data structure.

## References

- [`HASH-MODEL.md`](../HASH-MODEL.md) — the full model specification.
- [`UNIFIED-ARCHITECTURE.md`](../UNIFIED-ARCHITECTURE.md) §9, §15 N9 — architecture-level framing.
- [`PLAN-RUNNER.md`](../plan/PLAN-RUNNER.md) T15, T16 — runner implementation tasks.
- [`PLAN-STORE.md`](../plan/PLAN-STORE.md) S12–S15 — store implementation tasks.
- [`PLAN-VIEWER-PORT.md`](../plan/PLAN-VIEWER-PORT.md) §10, §11 (B3.5 milestone) — Phase B sequencing.
- [`CONCURRENCY.md`](../CONCURRENCY.md) §5.1, §5.2, §12.6 — threading invariants.
- [`docs/audit/STEPS-AUDIT-2026-04-21.md`](../audit/STEPS-AUDIT-2026-04-21.md) — the audit that surfaced the conflation.

## Superseded

- UNIFIED §15 decision **N8** ("Content-hashed outputs for normalization steps at DAG funnel points", 2026-04-20). The optional opt-in mechanism it introduced is no longer the interface — content-hashing is now universal, not opt-in.
