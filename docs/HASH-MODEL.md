# Hash model — identity, content, and the intent map

*Reference doc for the v3 content-addressing design. Central to the whole system: every cache hit, every cross-pipeline share, every reproducibility guarantee rests on what's in this doc.*

> **Status: ✅ shipped.** Implemented in commit `f359b50` (B3.5 hash-model refactor) before B4. Phase A + B1-B3 ran an earlier version with a known architectural conflation (see Appendix B); the refactor replaced it with the model described here. Subsequent milestones (B4-B10) all run on this model. No regressions; cache-hit class expanded as predicted.

---

## Table of contents

1. [The model in 60 seconds](#1-the-model-in-60-seconds)
2. [Three invariants](#2-three-invariants)
3. [The single primitive: `cached_compute`](#3-the-single-primitive-cached_compute)
4. [Identity computation](#4-identity-computation)
5. [The content-addressed store](#5-the-content-addressed-store)
6. [The intent map](#6-the-intent-map)
7. [Worked examples](#7-worked-examples)
8. [Patterns for step authors](#8-patterns-for-step-authors)
9. [The non-determinism escape hatch](#9-the-non-determinism-escape-hatch)
10. [Hash function choice](#10-hash-function-choice)
11. [Validity — does it cover everything?](#11-validity--does-it-cover-everything)
12. [Implementation plan](#12-implementation-plan)
13. [Consequences for existing docs](#13-consequences-for-existing-docs)
14. [Open questions](#14-open-questions)
15. [Appendix A — Conversation chronology](#15-appendix-a--conversation-chronology)
16. [Appendix B — Earlier models we explored and why we rejected them](#16-appendix-b--earlier-models-we-explored-and-why-we-rejected-them)

---

## 1. The model in 60 seconds

- Every artifact in the system lives in a content-addressed **store**, keyed by `hash(bytes)`.
- A persistent **intent map** records, for any hashable "identity" we care about, what content hash that identity's computation produced. `identity → content_hash`.
- A single primitive — `cached_compute(identity, compute_fn)` — queries the intent map, runs `compute_fn` only on miss, content-hashes the result, stores, records the intent-map entry, and returns the content hash.
- The same primitive is used at **any scope** the caller chooses: step level (identity = step_name + version + params + reads), sub-step level (identity = e.g. a URL), future finer grain (per-tile, per-chunk, per-row) if the need ever arises.
- A step with multiple writes returns a small **step-output manifest** listing `{write_name → content_hash}` entries. The manifest itself is content-addressed. Downstream reads the per-write hashes from it.

That's the whole architecture. Three data structures (store, intent map, manifest) and one primitive. Everything else is convention and tooling.

---

## 2. Three invariants

Memorize these. Every correct use of the model preserves all three; any bug that shows up here is almost certainly a violation of one.

### I1. Identity is a pure function of declared inputs

For **any** identity computed anywhere in the system:

```
identity = hash(canonical_serialization_of_inputs)
```

No hidden state. No wall-clock. No RNG. No environment variable. If it's not in the canonical serialization, it cannot affect the identity.

At step level: identity depends on step name, version, params, and the content hashes of every declared read. Nothing else.

At sub-step level: identity depends on whatever the step author decides to hash, as long as that "whatever" is itself deterministic.

### I2. Store keys are always content hashes

`hash(bytes) → bytes`. Every entry. No per-chain-keyed entries, no derivation-keyed entries, no identity-keyed entries. The store is purely a content-addressable blob database.

Corollary: two different chains that produce byte-identical outputs converge automatically. `store_put` is idempotent — if the key already exists with the same bytes, it's a noop.

### I3. Intent map is the only mapping from identity to content hash

The store has no idea what produced any of its entries. The intent map is the sole directory telling you "if you want the artifact produced by identity `X`, its content hash is `Y`."

The intent map is persistent, append-only in the common case, and compactable by pruning entries whose `content_hash` is no longer in the store.

---

## 3. The single primitive: `cached_compute`

```c
// Callback: produce the bytes for an identity that missed the intent map.
// The callee allocates; the caller frees. Return 0 on success, -1 on error.
typedef int (*compute_fn_t)(void* ctx, uint8_t** out_bytes, size_t* out_len);

// Content-addressed cache-or-compute.
// On success: *out_content_hash is the content hash of the bytes (whether
// they came from cache or were freshly produced). Returns 0.
// On failure (compute_fn errored, OOM, store I/O): returns -1, no intent
// entry recorded. Retries are idempotent.
int cached_compute(const hash_t* identity,
                   compute_fn_t compute_fn,
                   void* ctx,
                   hash_t* out_content_hash);
```

Reference implementation:

```c
int cached_compute(const hash_t* identity, compute_fn_t compute_fn, void* ctx,
                   hash_t* out_content_hash) {
    // Intent-map hit path.
    hash_t hit;
    if (intent_map_get(identity, &hit) == 0) {
        *out_content_hash = hit;
        return 0;
    }

    // Miss: run the computation.
    uint8_t* bytes = NULL;
    size_t len = 0;
    if (compute_fn(ctx, &bytes, &len) != 0) {
        return -1;   // compute_fn failed; no intent entry.
    }

    // Content-address the result. store_put takes ownership of `bytes`;
    // it frees the buffer later when the entry is demoted or evicted.
    hash_t content_hash = hash_bytes(bytes, len);
    int rc = store_put(&content_hash, bytes, len);    // ← buffer ownership transfers
    if (rc != 0) {
        free(bytes);
        return -1;
    }

    intent_map_put(identity, &content_hash);                   // append
    *out_content_hash = content_hash;
    return 0;
}
```

Roughly 30 LOC of real code. This is the **entire** cache mechanism of the system. Every other caching feature is a specific application of this primitive to a specific identity construction.

### Write API — one function, it takes the buffer

There is **one write function**: `store_put(key, bytes, len)`. It takes the heap buffer — after a successful return, the store frees it when the entry is demoted or evicted. Callers must not touch the buffer afterwards.

Callers that need to preserve their bytes (stack buffers, static data, bytes they want to keep using) do `malloc + memcpy` explicitly at the call site before calling `store_put`:

```c
uint8_t* dup = malloc(len);
memcpy(dup, original, len);
int rc = store_put(&key, dup, len);
```

The same pattern applies to `step_write_output` — one signature, takes the buffer.

"Ownership" in this doc is not a first-class concept. It's shorthand for the `free()` convention: after a successful `store_put`, the store calls `free()`. Before that, or on error, the caller does. There is no `_owned` vs `_copy` API split — the distinction collapsed because making the convention explicit at every call site is clearer than making it implicit in a second function name.

---

## 4. Identity computation

### 4.1 Step-level identity

The runner computes a step invocation's identity:

```
step_identity = hash(
    step_name_utf8,
    step_version_u32,
    canonical_json(params),
    sorted_read_hashes_joined
)
```

- **`step_name`**: registered step name.
- **`step_version`**: the `version` field from `step_def_t`. Bumps only on algorithmic change.
- **`canonical_json(params)`**: the step's pipeline-supplied params, serialized in a canonical (key-sorted, whitespace-stripped) form so two equivalent param JSONs produce the same bytes.
- **`sorted_read_hashes_joined`**: for each declared read, look up its content hash in the current run's artifact map, concatenate the hashes in ascending byte order, feed into the hash. Sort order is on hash bytes, not on read names — this means the step author can reorder `reads[]` in `step_def_t` without invalidating cache.

No other field enters. The runner computes this just before invoking `cached_compute`.

### 4.2 Sub-scope identity

Inside a step, the step author can compute any identity they want, as long as it's deterministic. The convention for namespacing: prefix a short ASCII tag.

```c
// Per-URL identity for a fetch sub-operation.
hash_t url_identity = hash_namespaced("url", url_bytes, url_len);

// Per-tile identity for a per-tile compute.
hash_t tile_identity = hash_namespaced("tile", tile_key_bytes, tile_key_len);
```

The namespace prefix prevents accidental collisions between a `url:"/foo"` identity and a `tile:"/foo"` identity. Both feed into the same intent map but live in disjoint identity subspaces.

Identity computation at sub-scope is the step author's responsibility. It's a design choice when authoring a step: "what's the right granularity of caching inside this step's body?" Typical answer: no sub-scope caching at all — the step-level intent map suffices. Rare answer: URL-level (for batch-fetching steps like `fetch-copc-tiles`), per-tile (for per-tile compute steps if they ever share tiles across pipelines).

---

## 5. The content-addressed store

Content-addressed blob database. Two-tier zero-copy architecture. Detailed implementation plan lives in [PLAN-STORE.md](plan/PLAN-STORE.md) tasks S12–S15; this section is the architectural specification.

### 5.1 Interface

Public API, from the caller's POV:

```c
// Write. Transfers the heap buffer to the store; caller must not touch
// `bytes` after a successful return. Store frees the buffer on demotion
// or eviction. Idempotent — a put with a matching existing key discards
// the caller's buffer and reports success.
int store_put(store_t*, const hash_t* key, uint8_t* bytes, size_t len);

// Read. Returns a handle with a zero-copy pointer. Caller must release.
// Ref-counted; the entry cannot be demoted or evicted while any handle is live.
int store_lookup(store_t*, const hash_t* key, store_handle_t* out);
void store_release(store_t*, store_handle_t*);

// Presence check without opening the entry.
bool store_contains(store_t*, const hash_t* key);

// Session-level pin to survive LRU (separate from per-handle ref-count).
void store_pin(store_t*, const hash_t*);
void store_unpin(store_t*, const hash_t*);
```

What the store does **not** do:
- Doesn't know what produced any entry (that's the intent map's job).
- Doesn't have a concept of "identity."
- Doesn't know whether two entries are byte-equivalent (they can't be — same content hash = same bytes by construction).

### 5.2 Two tiers

| Tier | Location | Read access | Write path |
|---|---|---|---|
| **Hot** | Heap-resident buffer (the same pointer the producer wrote) | Direct pointer via handle — true zero copy | `store_put` takes the buffer and records it |
| **Warm** | Disk file `<root>/xx/yy/<full-hex>.bin` | mmap on first read — zero-copy from OS page cache | Produced by demotion from hot, or by direct writes when hot tier can't accept |

An entry lives in exactly one tier at a time. Demotion (hot → warm) flips the tier in place; the index entry's hash key doesn't change. Eviction (warm → deleted) removes the entry entirely.

### 5.3 Ownership and the write path

There is one write function: `store_put(key, bytes, len)`. It takes the heap buffer — the store calls `free()` on it later (on demotion, on eviction, or on shutdown). Callers who need to retain their buffer do `malloc + memcpy` explicitly at the call site. No `_owned` vs `_copy` API — the convention is uniform and visible.

Write algorithm:

```
store_put(h, bytes, len):
    lock store
    if entry h exists (hot or warm):
        free(bytes)                   # idempotent: bytes were already stored
        unlock; return 0
    # Try to fit in hot tier.
    while hot_bytes_total + len > hot_budget:
        victim = pick LRU hot entry with ref_count == 0
        if !victim: break             # can't free more
        demote_to_warm(victim)        # synchronous disk write, free heap
    if hot_bytes_total + len <= hot_budget:
        insert hot entry, take `bytes` pointer
    else:
        # Can't fit (entry larger than budget, or all hot pinned).
        # Skip hot; write directly to warm.
        unlock
        write_to_warm_disk(h, bytes, len)  # temp + fsync + rename
        free(bytes)
        lock
        insert warm entry
    unlock
```

A janitor thread maintains a soft cap below the hard budget, so synchronous demotion on the put path is rare. Puts larger than the hot-tier budget skip hot entirely.

### 5.4 Zero-copy read path

```
store_lookup(h) →
    hashmap.get(h) → entry E
    bump E.ref_count
    if E.tier == HOT:
        handle.bytes = E.hot_bytes      # direct heap pointer
    else: # WARM
        handle.bytes = mmap(E.fd)        # page-cache-backed, zero-copy
    return 0
```

Between a step A's `store_put` and step B's `store_lookup`, if both happen in the same pipeline and the store hasn't demoted the entry, the bytes live at the same heap address the whole time. **Zero physical copies between producer and consumer.**

If the entry was demoted before B reads: B's mmap reads from OS page cache (data still hot in RAM if demotion was recent) or from disk (one I/O if cold). Still zero copies from user code's POV.

### 5.5 Demotion (hot → warm)

Triggered by:
- **Hard cap breach** in `store_put`: synchronous, blocks the put until the LRU entry's bytes are flushed to disk.
- **Soft-cap pressure** via the janitor: wakes periodically; walks LRU; demotes bottom-N entries with `ref_count == 0`.
- **Graceful shutdown**: demote all hot entries so the cache survives restart.

Demotion algorithm:

```
demote_to_warm(entry):
    assert entry.tier == HOT and entry.ref_count == 0
    entry.tier = DEMOTING                    # transient; lookups wait
    write entry.hot_bytes to `<root>/xx/yy/H.bin.tmp`
    fsync
    rename → `<root>/xx/yy/H.bin`
    entry.tier = WARM
    entry.warm_fd = -1                       # opened lazily on first read
    free(entry.hot_bytes); entry.hot_bytes = NULL
    hot_bytes_total -= entry.size
    warm_bytes_total += entry.size
```

The `DEMOTING` transient state protects concurrent lookups — they either wait on a condvar or retry. In practice we just hold the store mutex during the write: ~30 ms for a 500 MB blob on SSD. Short enough that lock contention doesn't matter at our scale; simpler than the condvar dance.

### 5.6 Eviction (warm → deleted)

Triggered when `warm_bytes_total > warm_budget`. Disk janitor picks LRU warm entries with `ref_count == 0` and `pin_count == 0`, `unlink()`s the file, removes from index.

Kernel semantics: `unlink` removes the directory entry; the inode and its pages stay alive until the last `munmap` releases the file. Active readers with live handles are unaffected. When they eventually `store_release` (which munmaps), the inode is freed.

**Manifest cascade handling**: when an evicted content hash is referenced by a still-live manifest elsewhere in the store, the system detects the missing reference via the validate-on-use policy (§11.2) and re-executes the producing step. No reverse index maintained in Phase B; re-compute handles it transparently.

### 5.7 Startup

`store_open(cfg)`:

1. `mkdir -p` the root dir; empty `xx/yy/` subdirs created lazily.
2. Walk `<root>/xx/yy/*.bin`, parse filenames as hex hashes, `stat` for size. Each file becomes a warm-tier index entry with `warm_fd = -1` (lazy open) and `ref_count = 0`.
3. Unlink any `.tmp` files (orphans from a crashed writer) and zero-byte files.
4. Hot tier initialized empty. First `store_put` of the new process populates it.
5. Start janitor thread.

Timing: roughly 10 ms per 10k warm entries on SSD (directory walk + stat). Single-threaded; one-time per process.

### 5.8 Shutdown

`store_close(s)`:

1. Stop janitor (signal condvar, `pthread_join`).
2. Drain hot tier: walk LRU from tail, demote every `TIER_HOT` entry with `ref_count == 0`. This preserves the cache across restart.
3. Close any cached warm file descriptors.
4. Flush intent-map log (`fsync`).
5. Free data structures.

Time to drain: ~2 s for a 2 GB hot tier on SSD (dominated by `fsync`). Well within systemd's default SIGTERM grace window.

### 5.9 Crash recovery

`SIGKILL` / OOM / power loss:
- **Warm tier survives** — files on disk, untouched.
- **Hot tier is lost** — heap bytes never hit disk. Entries produced since last demotion or process start are gone.
- **Last in-flight write** may leave a `.tmp` file; startup cleanup unlinks it. No dangling index entry because the entry wasn't inserted in-memory until after rename.
- **Intent map** survives up to the last `fsync`; we don't `fsync` every append, so expect to lose the last few seconds of recent intent-map activity.

Consistency: every index entry on restart maps to a complete, fsync'd file. No half-written artifacts. Lost hot entries self-heal via content-addressed idempotent re-execution.

---

## 6. The intent map

### 6.1 Purpose

Answers exactly one question: *given this identity, what content hash did its computation produce (if ever)?*

```
intent_map: hash_t identity → hash_t content_hash
```

### 6.2 Persistence

Append-only file in the cache directory: `<root>/intent.log`.

Each record is a fixed 65-byte tuple:

```
[32 bytes: identity]  [32 bytes: content_hash]  [1 byte: flags/reserved]
```

On process start, the file is read into an in-memory hashmap (`identity → content_hash`). On `intent_map_put`, we both update the in-memory map and append to the log. On process shutdown, the log is fsync'd.

### 6.3 Why append-only

- Writes are single-record appends — no seek, no rewrite, minimal contention across threads.
- Crash safety: truncated last record on an unclean shutdown is discarded on next scan.
- Reads are O(1) in-memory after startup.

### 6.4 Compaction

Periodic (or shutdown-driven): rewrite the log keeping only entries whose `content_hash` is still in the store. Pruning is necessary because LRU-evicted store entries can leave dangling intent-map entries — harmless (they'd just miss on next lookup when `store_contains` returns false), but eventually wasteful.

Cheapest correct policy: "on startup, drop entries whose `content_hash` isn't in the store." This runs in one pass over the log during load. The log gets rewritten atomically. Cost: linear in log size, which is small (tens of MB even at millions of entries).

### 6.5 Concurrency

Append-only plus an in-memory mutex on the hashmap. The intent map is a LEAF in the lock hierarchy — it is acquired last, held briefly, and never while holding the store's lock.

(See [CONCURRENCY.md §5.1](CONCURRENCY.md) for the shipped threading details.)

### 6.6 Sizing

One entry is 65 bytes. A million entries = 65 MB. A hundred million = 6.5 GB. The store itself will dominate disk use long before the intent map does.

---

## 7. Worked examples

Abbreviations: `H_foo` is a 32-byte hash (abbreviated in examples).

### 7.1 Example 1 — cold first run of a simple step

Step: `canonicalize-location` with `pipeline-input = {"lat":48.8566, "lon":2.3522}`.

```
runner:
    # Compute step identity.
    hash(pipeline-input bytes) = H_pin
    step_identity = hash("canonicalize-location", 1, {}, [H_pin])
                  = H_cl_id

    # Outer cached_compute at step scope.
    cached_compute(H_cl_id, run_step_body):
        intent_map.get(H_cl_id) → MISS
        run_step_body():
            exec_fn runs; produces canonical_bytes
            hash(canonical_bytes) = H_canon
            store_put(H_canon, canonical_bytes)
            # One write → trivially small manifest.
            manifest_bytes = serialize({"canonical-location": H_canon})
            return manifest_bytes
        hash(manifest_bytes) = H_cl_manif
        store_put(H_cl_manif, manifest_bytes)
        intent_map.put(H_cl_id, H_cl_manif)
        return H_cl_manif

    # Parse manifest, populate run-local artifact map.
    artifact_map["canonical-location"] = H_canon

State after:
    store:       H_pin → ..., H_canon → canonical_bytes, H_cl_manif → manifest
    intent_map:  H_cl_id → H_cl_manif
    artifact_map: canonical-location → H_canon
```

### 7.2 Example 2 — second run, identical input

```
runner:
    step_identity = H_cl_id (same inputs)

    cached_compute(H_cl_id, run_step_body):
        intent_map.get(H_cl_id) → HIT H_cl_manif
        return H_cl_manif

    parse store[H_cl_manif] → artifact_map["canonical-location"] = H_canon
```

Zero execution. Zero hashing of output bytes. One intent-map lookup (µs) + one mmap'd manifest parse (µs). That's it.

### 7.3 Example 3 — different input that canonicalizes to the same cell

Input: a user clicks 1 m east — same 20 m cell, different raw `pipeline-input` bytes.

```
runner:
    hash(pipeline-input' bytes) = H_pin'    ← different
    step_identity' = hash(..., [H_pin'])  = H_cl_id'  ← different

    cached_compute(H_cl_id', run_step_body):
        intent_map.get(H_cl_id') → MISS
        run_step_body():
            exec produces canonical_bytes   ← SAME bytes as 7.1 (same cell)
            hash(canonical_bytes) = H_canon  ← same content hash
            store_put(H_canon, canonical_bytes) ← idempotent noop
            manifest_bytes identical to 7.1
            return manifest_bytes
        hash(manifest_bytes) = H_cl_manif   ← same as 7.1
        store_put(H_cl_manif, ...) ← idempotent noop
        intent_map.put(H_cl_id', H_cl_manif) ← NEW intent entry, same target
        return H_cl_manif

    artifact_map["canonical-location"] = H_canon (same as 7.1)

State after:
    store:       unchanged from 7.2
    intent_map:  H_cl_id  → H_cl_manif
                 H_cl_id' → H_cl_manif     ← two identities, one artifact
```

Both chains converge. Downstream steps see the same `H_canon`; their identities match; they cache-hit. The runner executed `canonicalize-location` twice because the identity was new, but nothing downstream of `canonicalize-location` runs twice.

Cost of the convergence: one extra run of `canonicalize-location` (~10 ms). Benefit: all downstream steps share the cached results.

### 7.4 Example 4 — step body uses `cached_compute` internally (fetch-copc-tiles)

Setup: `fetch-copc-tiles` has read `copc-tile-urls` = an inline array of two URLs. Two pipelines run at adjacent canonical-locations that pick the same tile set.

**Pipeline A, first time for this tile set:**

```
runner:
    step_identity_A = hash("fetch-copc-tiles", 1, {}, [H_urls])
                    = H_fct_idA

    cached_compute(H_fct_idA, body_A):
        intent_map.get(H_fct_idA) → MISS
        body_A():
            for url_0:
                url_id_0 = hash_namespaced("url", url_0)
                cached_compute(url_id_0, http_fetch_0):
                    intent_map.get(url_id_0) → MISS
                    http_fetch_0() fetches ~200 MB LAZ
                    hash(laz0_bytes) = H_laz0
                    store_put(H_laz0, laz0_bytes)
                    intent_map.put(url_id_0, H_laz0)
                    return H_laz0
            for url_1: (same pattern) → H_laz1
            manifest = [H_laz0, H_laz1]  (by-hash manifest bytes)
            step_write_output("copc-raw-tiles", manifest)
            step_output_manifest = {"copc-raw-tiles": hash(manifest_bytes)}
            return serialize(step_output_manifest)
        ...cache the step-level result
        intent_map.put(H_fct_idA, H_fct_manifA)
```

**Pipeline B, adjacent canonical-location, same tiles:**

Assume upstream `find-copc-tiles` emitted byte-identical `copc-tile-urls` manifests for A and B (same tiles sorted the same way). So `H_urls` is the same for both pipelines.

```
runner:
    step_identity_B = hash("fetch-copc-tiles", 1, {}, [H_urls]) = H_fct_idA
                                                                  ↑
                                                     SAME as Pipeline A
    cached_compute(H_fct_idA, body_B):
        intent_map.get(H_fct_idA) → HIT H_fct_manifA
        return H_fct_manifA           ← body_B never called

    artifact_map["copc-raw-tiles"] = ... (parsed from manifest)
```

Pipeline B pays zero cost. The entire sub-tree (fetch + decompress + anything downstream of copc-raw-tiles that's deterministic from it) cache-hits.

**What if upstream manifests differ byte-wise but URL sets match?**

E.g., pipeline B's `copc-tile-urls` manifest has extra whitespace or different sort order. Then `H_urls_B ≠ H_urls_A`, so `step_identity_B ≠ step_identity_A`. Intent-map miss at step level. But:

```
body_B():
    for url_0: url_id_0 same → cached_compute HIT → H_laz0 (no HTTP)
    for url_1: url_id_1 same → HIT → H_laz1 (no HTTP)
    manifest = [H_laz0, H_laz1]       ← byte-identical to A
    step_write_output(..., manifest)
    step_output_manifest = {...: hash(manifest)} same bytes, same hash
    return serialize(step_output_manifest)   ← same bytes as A
```

The body emits the same manifest bytes as pipeline A. `hash(manifest) = H_fct_manifA`. `store_put` is idempotent. `intent_map.put(H_fct_idB, H_fct_manifA)` records a new intent entry pointing at the same result. Downstream cache-hits.

Summary: two identities, one byte-level result. **Convergence is automatic regardless of which scope causes it.**

### 7.5 Example 5 — partial failure and retry

Step: a batch fetcher with 3 URLs. The second URL's server is down.

```
body():
    for url_0: cached_compute(url_id_0, http_0)
        → succeeds → intent_map.put(url_id_0, H_blob0)
    for url_1: cached_compute(url_id_1, http_1)
        → http_1 returns -1 → cached_compute returns -1
        → body returns -1
step-level cached_compute returns -1 → step_identity NOT recorded
```

State: the intent map has the succeeded URL's entry (url_0), but not the step-level entry.

On retry (same inputs):

```
body():
    for url_0: cached_compute → HIT → no HTTP
    for url_1: cached_compute → MISS → http_1 retries
        → succeeds this time → record intent
    for url_2: fresh → succeed → record intent
    ...
step_identity → now recorded.
```

Partial progress is preserved. Retries pick up where they left off without the step author writing any retry logic.

### 7.6 Example 6 — single-write step vs multi-write step

**Single-write** (most steps): the "manifest" is essentially a single `{write_name → content_hash}` record. A few dozen bytes. The extra layer of indirection (step identity → manifest → write hash) is cheap and uniform.

**Multi-write** (e.g. `laz-extract-disc` with 4 outputs): manifest has 4 entries. Still a few hundred bytes max. Same uniform pattern.

We could optimize the single-write case by collapsing the manifest — `intent_map[step_identity] = content_hash_of_the_one_write` directly. Nice but not necessary. Uniformity wins for simplicity.

---

## 8. Patterns for step authors

### 8.1 Default: don't think about sub-scope caching

The step-level intent map handles everything. Write your step as a normal function that reads inputs, computes, writes outputs. The runner wraps it in `cached_compute` with `step_identity` and the whole thing is automatically cached.

If your step reruns are cheap (tens of milliseconds), this is the only caching you need.

### 8.2 When to add sub-scope `cached_compute` calls

Only when:

1. Your step does a batch of costly sub-operations (fetch many URLs, decompress many tiles, run a kernel per region, etc.).
2. AND the sub-operations' inputs can be hashed deterministically.
3. AND you have a plausible reason to believe the same sub-operation will be invoked from a **different step invocation** (different step identity) that wouldn't step-level cache-hit.

`fetch-copc-tiles` satisfies all three: different canonical-locations can legitimately share tile URLs. Adding `cached_compute` at URL scope is a real win.

Most steps do **not** satisfy #3 — if the sub-operations' results are fully determined by the step's own inputs, sub-scope caching buys nothing beyond step-level caching. Don't add it.

### 8.3 Good and bad patterns

**Good — step with no sub-scope caching:**

```c
int compute_normals_exec(step_context_t* ctx) {
    // Read positions from declared input.
    const double* positions = step_read(ctx, "lidar-positions", &len);

    // Compute.
    float* normals = malloc(...);
    compute_pca_normals(positions, len, normals);

    // Write output — the runner's outer cached_compute handles caching.
    step_write_output(ctx, "lidar-normals", normals, ...);
    return 0;
}
```

**Good — step with sub-scope caching:**

```c
int fetch_copc_tiles_exec(step_context_t* ctx) {
    hash_t element_hashes[MAX];
    for each url in step_array iter of "copc-tile-urls":
        hash_t url_id = hash_namespaced("url", url);
        int rc = cached_compute(&url_id, http_fetch_trampoline, url,
                                 &element_hashes[i]);
        if (rc != 0) return -1;
    build and emit by-hash manifest from element_hashes;
    step_write_output(ctx, "copc-raw-tiles", manifest_bytes, ...);
    return 0;
}
```

**Bad — using `cached_compute` at sub-scope where step-level would suffice:**

A `compute-normals` step that caches each block of points independently. The per-block identity is derived from the step's own inputs — step-level cache-hit would skip the whole thing anyway. The per-block caching adds overhead without adding hits.

**Bad — sub-scope identity uses non-deterministic input:**

Hashing `time(NULL)` into an identity. Immediate cache poisoning: every invocation gets a fresh identity, every entry accumulates forever. (Invariant I1 violation.)

**Bad — forgetting to namespace sub-scope identities:**

Hashing `hash(url_bytes)` without the `"url:"` prefix. Collides with any other identity that happens to produce the same 32 bytes. Vanishingly unlikely at 128-bit hashes, but disciplined namespacing costs nothing and eliminates the class of bug.

---

## 9. The non-determinism escape hatch

A step whose output depends on something the system cannot hash into its identity (wall clock, external process state, random seed picked at runtime) is **non-deterministic**. Its step identity doesn't truly pin down its output — running it twice with "the same" identity can produce different bytes.

For these cases, the step declaration opts out:

```c
step_write_t writes_arr[] = {
    { .name = "my-output", ..., .skip_on_identity = false },
};
```

Runtime effect: the runner bypasses the intent map and runs the step every time. The content hash of each run's output still gets stored (idempotent), but the intent map is not consulted or updated for this step.

**This is rarely used.** v3 treats it as a smell. The vast majority of steps should be declarative functions of their declared inputs; if yours isn't, the first question is whether you can redesign to make it deterministic (e.g. hoist the RNG seed into params). Only after that fails is `skip_on_identity: false` the right answer.

Default is `skip_on_identity: true`. The field is an escape hatch, not a design axis — if you're choosing per-step, you're probably designing the step wrong.

---

## 10. Hash function choice

### 10.1 Requirement

Fast, deterministic, hardware-accelerated where available, collision-resistant under normal (non-adversarial) inputs. Not required: cryptographic strength, pre-image resistance, collision resistance under adversarial attack.

### 10.2 Choice: **xxh3-128**

[xxHash](https://xxhash.com/) `xxh3` 128-bit variant. Already in use (`libxxhash` linked via `dioramap_hash`).

**Why:**

- **Speed**: 20-30 GB/s on modern x86 with AVX2, 10+ GB/s on Apple Silicon (NEON). For a 500 MB LAZ tile, hashing takes ~30 ms — essentially free compared to decompression.
- **Hardware acceleration** built-in: xxh3 uses SIMD intrinsics (SSE2 minimum, AVX2 preferred, NEON on ARM) automatically at compile time.
- **Quality**: passes SMHasher, no known pathological inputs in practice.
- **Collision probability at 128 bits**: birthday bound is 2⁶⁴ ≈ 1.8 × 10¹⁹ distinct inputs. At 1 million cache entries per day for 1000 years we'd be 4 orders of magnitude from a meaningful collision probability.

**Why not blake3:**

- Cryptographic strength isn't needed — v3 doesn't ingest adversarial bytes, doesn't share a CAS across trust boundaries, and doesn't need pre-image resistance.
- blake3 is slower (1–3 GB/s single-threaded; multi-threaded faster but still below xxh3 for our bulk-hash pattern).
- Cryptographic hashes are a pessimization when the threat model doesn't need them.

### 10.3 On-disk stability

`hash_t` is 32 bytes. xxh3-128 fills 16 bytes; the remaining 16 are zeroed. This leaves room to swap to a larger hash later (blake3-256, sha3-256) without changing the on-disk layout of any artifact or intent-map file. If we ever need cryptographic hashing (e.g. to verify integrity across untrusted transfers), that swap is a single-file change in `hash.c`.

### 10.4 Canonical rules

- Same input bytes → same hash, always. Endianness, padding, alignment all normalized in the hash implementation.
- Strings are hashed as UTF-8 bytes, no NUL terminator included unless explicitly noted.
- Structured inputs (step identity, manifest contents) are serialized into a canonical byte stream **before** hashing — never a struct-memcpy. Fields in defined order, u32 / u64 little-endian, JSON canonicalization key-sorted and whitespace-stripped.

These rules are enforced in the `hash_*` helpers in `core/src/hash/`.

---

## 11. Validity — does it cover everything?

A model is only worth shipping if it handles every scenario we care about. Walk through each.

### 11.1 Requirements matrix

| Requirement | How the model handles it |
|---|---|
| Same inputs → same output on rerun | Step identity is deterministic from declared inputs → intent_map hit → same content hash returned. |
| Different-chain convergence (byte-identical outputs share cache) | Content-hashed store + intent-map-points-to-content. Different identities can legitimately point at the same content hash; `store_put` is idempotent. |
| Non-deterministic steps (wall clock, RNG) | `skip_on_identity: false` escape hatch. Runner bypasses intent map; each run content-hashes independently. |
| Partial failure inside a batch step | Sub-scope `cached_compute` calls that succeeded record their intent entries. Step-level intent entry is recorded only on full success. Retries pick up where they left off. |
| Multi-writer step | Step body emits a manifest `{write_name → content_hash}`. Manifest is itself content-hashed and stored. Runner parses manifest to populate the run-local artifact map. |
| Streaming writes (PLAN-RUNNER T14) | Step body writes incrementally; final `content_hash` computed on `final=true`. Fits `cached_compute`: the compute_fn returns the accumulated bytes at final. Intent entry recorded at stream completion. |
| Sub-step caching (URL sharing across pipelines) | Inner `cached_compute` call with a URL-derived identity. Same mechanism as step-level. |
| Multiple pipelines sharing a cache dir | Intent map is append-only; atomic appends + mutex on in-memory map. Store is already multi-writer safe (atomic temp+rename). |
| LRU eviction of large store entries | Self-healing via §11.2. |
| Evicted element referenced by a live manifest | Handled by validate-on-use policy; see §11.2. |
| Reproducibility across machines | Identity computation is fully specified (deterministic serialization + hash function + endianness). Two machines with the same inputs produce the same identity, the same content hash, the same on-disk layout. |
| Garbage collection | LRU in the store is independent; intent-map pruning happens on startup or periodically. |
| Tests / CI | Offline tests use a fresh cache dir per test → always cold runs. Cache behavior is tested separately with seeded caches. |

No requirement on the list is unaddressed. No requirement forces an architectural special case outside the three invariants.

### 11.2 Manifest eviction cascade — the validate-on-use policy

The one correctness hazard worth spelling out: a manifest (a step-output manifest, or an `ARRAY_BY_HASH` by-hash manifest from a fan-out step) references other content by hash. If the referenced content is evicted while the manifest is still live, the manifest's pointers dangle.

Concrete example: `fetch-copc-tiles` emits `copc-raw-tiles` — a by-hash manifest listing two LAZ blobs by their content hashes. LRU eviction removes one of the blobs from disk. On a later run, `laz-extract-disc` tries to open that blob via `step_array_open` and misses.

#### The policy: validate on use

The runner already parses every step-output manifest to populate the run-local `artifact_map`. Extending that parse to also verify each referenced content hash is present in the store is free on the happy path (one `store_contains` per entry; microseconds). Two validation points:

1. **After every `cached_compute` hit** (step-scope). Runner parses the returned manifest, calls `store_contains(write_hash)` for each entry. On any miss: invalidate the intent-map entry and re-run the step.

2. **Before dispatching a step with array-typed reads**. For each read where `step_read_t.array == true`, parse the upstream manifest and `store_contains` each element. On any miss: invalidate the intent-map entry of the producing step (found via the pipeline's `writes[]` declaration) and re-run that step; then retry the current dispatch.

Pseudocode:

```c
int resolve_manifest_from_hit(run_state_t* rs, step_def_t* step, hash_t manifest_hash) {
    store_handle_t h;
    if (store_lookup(&manifest_hash, &h) != 0) return STALE;
    for each entry in parse_manifest(h.bytes):
        if (!store_contains(&entry.write_hash)) { store_release(&h); return STALE; }
    // all present; populate artifact map from entry list
    store_release(&h);
    return OK;
}

int run_step(step_def_t* step, run_state_t* rs) {
    for (int retry = 0; retry <= 1; retry++) {
        identity = compute_step_identity(step, rs);
        manifest_hash = cached_compute(identity, body_trampoline, step);
        if (resolve_manifest_from_hit(rs, step, manifest_hash) == OK) return OK;
        intent_map_invalidate(&identity);   // force re-exec on next attempt
    }
    return ERR_STALE_CACHE;   // shouldn't happen twice in a row
}
```

#### Recovery properties

- **Deterministic steps produce the same bytes** on re-run → content-hashed writes are idempotent → no explicit delete needed. The re-run just re-populates the evicted bytes under the same key.
- **Intent-map overwrite is automatic** via append-and-supersede. The old entry stays in the log until compaction; the in-memory hashmap reflects the newest entry.
- **Bounded retry of 1 per level**. If validation fails twice in a row on the same step, something is badly wrong (disk errors, eviction outpacing puts, a non-deterministic step mis-declared as deterministic). Surface as a hard error.
- **Transparent to clients**. Observable only as "the step took longer than usual on the first post-eviction invocation."

#### Nested-manifest case (fetch-copc-tiles → laz-extract-disc)

`fetch-copc-tiles` emits a step-output manifest `{"copc-raw-tiles": H_inner}` where `H_inner` is itself an `ARRAY_BY_HASH` manifest listing LAZ blobs. Runner's step-scope validation (point 1 above) checks only that `H_inner` is in the store, not that the blobs it points at are still present.

That's where point 2 kicks in — when `laz-extract-disc` is dispatched, the runner parses its array-read (`copc-raw-tiles`, the inner manifest) and validates each blob's presence. A missing blob triggers invalidation of the producer (`fetch-copc-tiles`) and a re-run of that step. `fetch-copc-tiles` re-runs, `fetch_and_cache` sees URL-keyed cache hits for intact blobs and URL-keyed cache misses for the evicted one (which it re-fetches), re-assembles the by-hash manifest (byte-identical to the original), and the retry succeeds.

Recursion is bounded because each invalidation is scoped to one step; the DAG walk doesn't loop.

#### Why no reverse index in Phase B

A theoretically complete solution tracks `leaf → manifests_referencing_it`, so the eviction janitor skips leaves that are referenced. Requires a second persistent data structure and maintenance on every manifest write. Deferred to Phase C if evidence shows the cascade fires often enough to matter.

In practice: prod store budget is TB-scale, evictions are rare. Dev/CI runs in fresh cache dirs, so cascades can't occur within one session. The validate-on-use recovery adds a per-step-dispatch `store_contains` check (microseconds) and handles the rare cascade transparently. Adequate for Phase B; upgrade path preserved.

---

## 12. Implementation plan

### 12.1 New code

| File | LOC | Purpose |
|---|---|---|
| `core/include/v3/intent_map.h` | ~40 | Public API: `intent_map_get`, `intent_map_put`, `intent_map_invalidate`, lifecycle (open/close/flush). |
| `core/src/intent_map/intent_map.c` | ~200 | Append-only log + in-memory hashmap; startup scan with partial-tail discard; compaction on startup; mutex for concurrent appends. |
| `core/include/v3/cached_compute.h` | ~20 | Public API: `cached_compute(identity, fn, ctx, out)`. |
| `core/src/runner/cached_compute.c` | ~50 | Intent-map lookup → compute_fn on miss → `store_put` + `intent_map_put`. |
| `core/src/store/hot_tier.c` | ~400 | Hot-tier hashmap, LRU list, refcount, sync demotion on put, janitor soft-cap pass. See PLAN-STORE S12-S13. |
| `core/include/v3/store.h` additions | ~30 | New handle discriminated union (hot_ptr vs warm mmap); updated `store_put` signature (takes buffer). |

Roughly **640 LOC new**.

### 12.2 Changed code

| File | LOC touched | Change |
|---|---|---|
| `core/src/runner/runner.c` | ~120 | Replace derivation/content branching with `cached_compute` at step scope; manifest generation wrapper around `exec_fn`; validate-on-use at cached_compute hit + array-read dispatch. |
| `core/include/v3/step.h` | ~10 | Remove `output_hash` field. Add `skip_on_identity` (default `true`). |
| Every step file (demo, gpu-smoke, fetchers, coords, copc) | ~30 | One-line diff per step_write_t declaration to remove `output_hash` field initializer. |
| `core/src/fetch/fetch.c` | ~40 | `fetch_and_cache` becomes thin wrapper over `cached_compute`. |
| `core/src/store/store.c` | ~100 | `store_put_with_hash` → `store_put` (now takes buffer ownership); hot-tier integration. |

Roughly **300 LOC touched**. Total project scope: ~940 LOC churn for a meaningful architectural improvement.

### 12.3 Migration order

Landing-order matters because each step preserves green tests.

1. **Wave A — infrastructure, no callers yet.**
   - Add `intent_map` + `cached_compute` libraries with full unit tests.
   - Add hot-tier scaffolding to `store.c` but keep the old `store_put_with_hash` as a deprecated alias that internally does `malloc + memcpy + store_put`.
   - Tests: intent_map round-trip, cached_compute hit/miss/idempotency, store_put ownership-transfer + handle.

2. **Wave B — runner migration.**
   - Refactor `execute_step` to call `cached_compute` at step scope.
   - Emit the step-output manifest. Parse on return; populate artifact_map.
   - Add validate-on-use (both cached_compute hit and array-read dispatch).
   - Remove `output_hash` field; all existing step files compile after the one-line diff.
   - `skip_on_identity` defaults to `true`; no existing step declares `false`.
   - Tests: all 21 existing ctests green; add one that asserts second run of `batch_array_demo` produces zero executions.

3. **Wave C — fetcher consolidation.**
   - Rewrite `fetch_and_cache` as a wrapper. External API unchanged.
   - Delete `step_output_hash_t` enum.

4. **Wave D — store cleanup.**
   - Remove the `store_put_with_hash` alias.
   - Remove derivation-key computation in runner.c.
   - Remove reference to `STEP_OH_DERIVATION` / `STEP_OH_CONTENT` anywhere left.

Each wave is an independent commit that leaves the repo green.

### 12.4 Testing strategy

**Unit level.**
- `intent_map`: concurrent writers, crash mid-write (truncate partial tail), startup scan, compaction, tombstones.
- `cached_compute`: hit/miss, idempotent re-put, compute_fn failure propagates, concurrent calls with same identity serialize correctly.
- Hot tier: ref-count pinning protects demotion, LRU eviction order, demotion during concurrent lookup, put with budget pressure triggers demotion.

**Integration level.**
- `batch_array_demo` rerun: all 3 steps cache-hit, zero `body_fn` executions.
- `batch_viewer_pointcloud` rerun (DIORAMAP_TEST_IGN=1): LAZ blobs fetched once, re-extract disc cache-hits; tile sets partially overlapping produce URL-level hits.
- Simulated eviction cascade: force-evict a leaf, rerun, verify validate-on-use re-executes only the producing step.

**Sanitizers.**
- ASan + TSan on every unit and integration test.
- Stress test: 100 concurrent step invocations in a single process, mixed hit/miss, bounded memory usage.

---

## 13. Consequences for existing docs

- **`UNIFIED-ARCHITECTURE.md` §9**: replace current §9.1 (derivation vs content hash formulas) with a short summary pointing at this doc. §9.3 (content-hash rationale for normalization steps) is subsumed — every step is content-hashed now; convergence is automatic where it applies.
- **`PLAN-RUNNER.md`**: new task T15 ("intent map + cached_compute"). Supersedes any mention of `STEP_OH_DERIVATION` / `STEP_OH_CONTENT`.
- **`PLAN-STORE.md`**: new tasks S12 (hot tier), S13 (demotion), S14 (intent map on-disk format), S15 (shutdown drain).
- **`PLAN-VIEWER-PORT.md` §10**: delete the output-hash decision table. All steps are content-hashed; there's nothing to choose per-step. Mention the rare `skip_on_identity: false` case if any specific step needs it (none do today).
- **`PLAN-FETCHERS.md`**: `fetch_and_cache` reframed as a thin helper over `cached_compute`. Still useful but not an architectural primitive.
- **`CONCURRENCY.md`**: add the intent-map mutex to the lock inventory in §2; layer 0 leaf.

---

## 14. Open questions

1. **Compaction policy.** Startup-only, or periodic (every N inserts / every T seconds)? Lean toward startup-only to start; revisit if the log grows faster than expected.
2. **Multi-process writers to the same cache dir.** The store already handles this (atomic temp+rename). The intent log needs the same discipline — `O_APPEND` writes are atomic on POSIX up to `PIPE_BUF` (4 KB+), and each record is 65 bytes. Safe by construction, but worth stating explicitly.
3. **Should sub-scope identities use a different prefix convention than `"url:"`, `"tile:"`?** Consider a well-defined subset: `"v3:fetch:url:"`, `"v3:tile:"`, etc., to leave room for future namespace categories. Low-cost discipline, zero runtime cost.
4. **Is there a case for forcing a particular step to always use content-hashed output but NOT cache-hit (i.e. `skip_on_identity: false` but intent-map entry still recorded)?** Probably not — either you trust your identity or you don't. But worth stating: when `skip_on_identity: false`, the intent map is not queried AND not updated. The store still gets every write's bytes via `store_put`.
5. **Cross-machine intent map portability.** Intent-map files are not portable across machines in the general case (they reference content hashes whose corresponding entries may not be in the destination machine's store). Should this be documented as "copy the whole cache dir together"? Yes.

---

## 15. Appendix A — Conversation chronology

For future readers: how we arrived at this model matters, because the simplifications were earned, not assumed. Preserving the false turns gives future readers the logic and makes later architectural changes easier to evaluate.

### Round 1 — the audit found two different problems

During the Phase-A-audit of the shipped steps (`docs/audit/STEPS-AUDIT-2026-04-21.md`), we noticed:

- `find-copc-tiles` called `fetch_get` directly instead of routing through `fetch_and_cache`. The audit fix-up flipped it to `fetch_and_cache` as a mechanical compliance fix.
- But that triggered the deeper question: **why** do we have two fetcher functions? What are they really for?

Independently, the audit missed a related bug: `fetch-ortho-rgb` and `fetch-elevation` used `output_hash: CONTENT` + `fetch_get`, which meant they **always re-ran and always re-HTTPed** on repeat invocations. This was hiding a real performance cost.

### Round 2 — "are we not double-caching?"

The user pushed on the audit fix: if `find-copc-tiles` now uses `fetch_and_cache` **and** its output is stored, are we storing the same thing twice?

Traced it:
- `fetch_and_cache(url)` stores the raw WFS response under `hash(url)`.
- `step_write_output(..., manifest)` stores the parsed-and-sorted manifest under (at the time) `hash(manifest)`.
- Two different entries, different bytes. Not a duplicate.

**But** — this was the first crack. The two caching layers had different purposes that were never clearly articulated.

### Round 3 — the ortho/elevation re-HTTP bug

Working through the double-caching question surfaced: `fetch-ortho-rgb` and `fetch-elevation` with `output_hash: CONTENT` **always re-run the step**, therefore **always re-HTTP**. The audit had flagged visibility and kind mismatches but missed this.

Two proposed fixes:

- **Solution A**: `CONTENT → DERIVATION`. Runner can cache-skip the whole step on rerun.
- **Solution B**: keep `CONTENT`, but use `fetch_and_cache` so the step's internal HTTP is cached even though the step itself re-runs.

### Round 4 — walking through both solutions

Step-by-step pseudo-execution (preserved in earlier revisions of this doc) showed:

- Solution A: one store entry per input tuple, zero HTTP on rerun. Clean.
- Solution B: two store entries (URL-keyed + content-keyed with identical bytes), step runs on every invocation but no HTTP. Wasteful for ortho/elevation.

Verdict: A for plain fetchers. B is the right pattern only for **manifest-style** fetchers (`fetch-copc-tiles`) where the step's output is a pointer list to URL-keyed blobs — there's no byte duplication because the URL-keyed entries ARE the real data.

### Round 5 — "when is CONTENT ever useful?"

The user pushed deeper: what does CONTENT actually buy us? We named it: **funnel convergence**. CONTENT makes downstream steps share cache across different provenance chains that happen to produce the same bytes. Examples:

- `canonicalize-location` — different raw inputs that quantize to the same cell.
- `find-copc-tiles` — different canonical-locations picking the same tile set.
- `fetch-copc-tiles` — different upstream manifest representations encoding the same URL set.
- `laz-extract-disc` — different tile sets yielding the same disc points.

All four are legitimate funnels. All four should use CONTENT. The other ("plain") fetchers have no funnel use and should use DERIVATION.

### Round 6 — the user's central insight

The user: *"I feel like derivation is to know if step must run, and content is to know if next steps must run. I feel like they are treated as one concept when there are two?"*

This was the key moment. We had been treating two orthogonal concerns as one:

- **Concern A**: can the runner skip executing this step on a rerun?
- **Concern B**: can downstream steps share cache across different upstream chains that produce the same bytes?

The `output_hash` flag baked the answer to both into one field, with two of the four cells arbitrarily excluded. DERIVATION gave A but not B. CONTENT gave B but not A.

### Round 7 — first proposal: split the flag + add an intent map

Proposed two fields: `output_hash` (controlling store-key style, concern B) and `skip_on_identity` (controlling runner-skip, concern A). The intent map (`derivation_key → artifact_hash`) would let the runner skip `CONTENT`-output steps on identity match — unlocking the missing cell.

This looked like progress. The 2×2 matrix had four useful cells now.

### Round 8 — the user's second insight: "why not content-only?"

The user: *"what happen if we only use content hash for artifacts actually? It always works?"*

Thought about it. Yes, content-only works for every case, because:

- Cache-skip is provided by the intent map regardless of what the store key is.
- Convergence is automatic under content-hash.
- `skip_on_identity: false` still works as an escape hatch.
- Nothing useful is lost by dropping DERIVATION.

Content-only is strictly more general. We kept DERIVATION around only out of inertia.

### Round 9 — "fetch_and_cache feels like a special case"

The user: *"I feel like fetch_and_cache is a special case... the smell is we need a special handling because our model is not generalized enough."*

Thought about it harder. `fetch_and_cache` is actually **the same pattern as step-level caching, just with a different identity**. URL is just another hashable identifier, and the URL-keyed store entry + the intent-map-like retrieval is identical to what step-level caching does.

Unified primitive: `cached_compute(identity, compute_fn)`. Callable at any scope. Step-level uses step_identity. Sub-scope uses whatever identity the step author wants to hash (e.g., URL). Same mechanism.

At this point the model collapsed from "store + intent map + fetch_and_cache + output_hash enum + skip_on_identity" to "store + intent map + one primitive + one escape hatch."

### Round 10 — the runner's job

The user: *"the runner must calculate step_identity on all its inputs (reads)."*

Reaffirmation of invariant I1. Step identity is always computed from all declared inputs. The runner's step-level `cached_compute` uses step_identity. Sub-scope `cached_compute` calls use whatever identity the step author constructs — but those are independent of step_identity, not derived from it.

### Round 11 — the pseudo-code

The user sketched the runner's execution flow: compute step_identity, call `cached_compute`, on hit return a manifest of the step's writes, on miss execute the body (which can internally use `cached_compute` for sub-ops) and return a manifest. Manifest itself is content-hashed and stored; intent map records step_identity → manifest_hash.

Confirmed — that IS the model. Documented here as §3, §4, and §7.

### Round 12 — this doc

Written to capture the full final model, including the chronology so future readers can retrace the logic.

---

## 16. Appendix B — Earlier models we explored and why we rejected them

### B.1 Shipped today: single `output_hash` enum

What shipped: `step_write_t.output_hash: DERIVATION | CONTENT`. Runner computes derivation key from step identity; if DERIVATION, stores under derivation key and can skip on `store_contains`; if CONTENT, always runs and stores under content hash.

**Rejected because**: conflates two orthogonal concerns. Forces the step author to trade cache-skippability against convergence, when the two are independent properties that every funnel step wants.

### B.2 First proposal: split-flag + intent map

Two fields: `output_hash` (DERIVATION | CONTENT, controlling store key) and `skip_on_identity` (bool, controlling runner-skip). Intent map maps `derivation_key → artifact_hash` to enable cache-skip for CONTENT steps.

**Rejected because**: DERIVATION becomes redundant once the intent map exists. For DERIVATION steps, `intent_map.put(derivation_key, derivation_key)` is a tautology — the derivation key IS the store key. The `output_hash` flag degenerates to a vestigial choice.

Also, the 2×2 matrix I drew suggested `skip_on_identity` was an architectural axis peer to `output_hash`. It's not — it's an escape hatch for non-deterministic steps.

### B.3 Content-only with step-level intent map

Dropped `output_hash` entirely; every output is content-hashed. Intent map handles step-level skip. `fetch_and_cache` remains a separate primitive.

**Partially accepted, then superseded**. Content-only was the right simplification for the output-keying side. But `fetch_and_cache` sitting alongside the step-level intent map felt unclean — two caching mechanisms doing structurally similar work.

### B.4 Final: content-only + unified `cached_compute` at any scope

The model documented in §1-§8 above. One primitive, one intent map, any scope. `fetch_and_cache` becomes an instance of `cached_compute` applied to URL-keyed identities — a library convenience, not an architectural primitive.

**Accepted**. Minimal, general, handles every case on our requirements list without special cases.

---

*Last updated 2026-04-21. Companion docs: `UNIFIED-ARCHITECTURE.md` §9 (to be updated), `docs/audit/STEPS-AUDIT-2026-04-21.md` (which surfaced the issues).*
