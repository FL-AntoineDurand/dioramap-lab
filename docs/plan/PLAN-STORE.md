# Store + Cache Subsystem Plan

*Companion to [UNIFIED-ARCHITECTURE.md](../UNIFIED-ARCHITECTURE.md). Content-addressed artifact store with on-disk persistence.*

## Scope

- **Content-addressed lookup**: hash → artifact handle.
- **In-memory index**: hash → { disk path, size, pin count, last-access }.
- **On-disk persistence**: atomic write, prefix-sharded directory layout, startup scan, corruption handling.
- **mmap-based reads**: zero-copy access to cached bytes.
- **LRU eviction**: budget-bounded, session-aware pinning.
- **Janitor thread**: background eviction + stats emission.

## Key decisions

1. **Two tiers: hot (heap) + warm (disk).** Hot is the zero-copy buffer pool producers write into and consumers read from in-process; warm is the mmap-backed persistent cache. Demotion hot→warm is LRU-driven under a size×age score. See HASH-MODEL.md §5 for the unified model and S12 / S13 below.
2. **Content-addressing via xxh3-128.** Non-cryptographic; adequate for our non-adversarial content-addressing. Via `libxxhash` through the `dioramap_hash` wrapper. `hash_t` is sized at 32 bytes so a future blake3-256 swap remains a header-only change.
3. **Prefix-sharded directory layout.** `/<root>/<hash[0:2]>/<hash[2:4]>/<full-hash>.bin`. Keeps any directory under a few thousand entries.
4. **Atomic writes via temp-file + rename**: write to `<hash>.bin.tmp`, fsync, rename. Prevents partial files on crash.
5. **Startup scan rebuilds the in-memory index.** On first start (cold disk): empty. On subsequent starts: scan directories, parse each file's header, populate the index. Corrupted files (bad magic/version) are deleted. The intent-map log is replayed on the same startup path (see S14).
6. **Session refs pin against eviction, nothing else.** The refcount is purely an eviction guard; it does not govern memory lifetime.
7. **Persistent intent map alongside the store.** `step_identity → content_hash` append-only log + in-memory hashmap. Handles cache-skip on rerun; see S14 and HASH-MODEL.md §6.

## Data structures

### Hash

```c
typedef struct hash {
    uint8_t bytes[32];  // xxh3-128 fills first 16, remaining zeroed; sized to allow a blake3-256 swap later.
} hash_t;

bool hash_eq(const hash_t*, const hash_t*);
uint64_t hash_fingerprint(const hash_t*);  // for hashmap
void hash_format_hex(const hash_t*, char out[65]);
```

### Index entry

```c
typedef struct store_entry {
    hash_t hash;
    uint64_t size_bytes;
    uint64_t last_access_us;    // monotonic microseconds
    uint32_t pin_count;         // session + active-use pins
    uint8_t  kind;              // from artifact header
    uint32_t version;           // from artifact header
    // file path is derived from hash; no need to store
} store_entry_t;
```

### Store API (C)

```c
typedef struct store store_t;

store_t* store_open(store_config_t);  // scan disk, build index
void store_close(store_t*);

// Lookup. Returns non-NULL if in store; NULL if miss.
store_entry_t* store_lookup(store_t*, const hash_t*);

// Read path. Returns an mmap'd handle.
typedef struct store_read {
    const uint8_t* bytes;
    size_t len;
    // internal fd, mmap region, refcount
} store_read_t;

store_read_t* store_open_read(store_t*, const hash_t*);
void store_close_read(store_read_t*);

// Write path. Allocates a temp file and returns a handle.
typedef struct store_write {
    int fd;
    char tmp_path[PATH_MAX];
    hash_t hash;  // filled on commit
    // internal state
} store_write_t;

store_write_t* store_open_write(store_t*);
result_t store_write_bytes(store_write_t*, const void* data, size_t n);
result_t store_commit_write(store_write_t*, const hash_t* hash, uint8_t kind, uint32_t version);
void store_abort_write(store_write_t*);

// Pin management
void store_pin(store_t*, const hash_t*, ref_owner_t);
void store_unpin(store_t*, const hash_t*, ref_owner_t);

// Stats
store_stats_t store_stats(store_t*);  // size, count, hit rate, evictions
```

### Internal index

`hashmap<hash_t, store_entry_t*>` protected by a reader-writer lock. Reads (lookups) acquire shared; writes (inserts, evictions, pin updates) acquire exclusive. Contention expected to be low; if it isn't, shard by hash prefix.

## On-disk layout

```
/var/cache/dioramap/              (configurable)
├── index.lock                    file-lock; only one process can open this store
├── sha/                          (or bl3/ per hash algorithm)
│   ├── ab/
│   │   ├── 00/
│   │   │   ├── ab00f3d2...0a.bin
│   │   │   └── ab00f3d2...0b.bin
│   │   └── 01/
│   └── cd/
│       └── ...
└── tmp/                          for in-progress writes before rename
```

- Each `.bin` file is a single artifact in the §5 binary layout.
- No external index file. The filesystem is the index; startup scan rebuilds in-memory state.
- Directory permissions: binary-user-only (`0700`).

## Status (2026-04-28)

| Task | State | Notes |
|---|---|---|
| S1 Hash + path derivation | ✅ done (A4, `22aae7f`) | xxh3-128 in `core/src/hash/`. |
| S2 Basic in-memory index | ✅ done (A5, `ff40c53`) | Hashmap + reader-writer lock. |
| S3 Write path | ✅ done (A5, `ff40c53`) | Temp file + fsync + atomic rename. |
| S4 Read path via mmap | ✅ done (A5, `ff40c53`) | `store_open_read` mmap-backed; refcounted. |
| S5 Startup scan | ✅ done (A5+ audit, `bd00bc5`) | Scan + skip malformed; intent-map log replay shipped with S14. |
| S6 Pin / unpin | ✅ done (A5, `ff40c53`) | Owner-tagged pin tracking. |
| S7 LRU eviction | ✅ done (A5, `ff40c53`) | Janitor + budget eviction; pinned entries skipped. |
| S8 Janitor + stats | ✅ done (A5, `ff40c53`) | Background thread + `cache_evict` log events. |
| S9 Crash recovery integration | ✅ done (A5+) | Tmp cleanup at startup; partial files removed. |
| S10 Corruption handling | ✅ done | Bad-magic files removed during scan (audited 2026 — `bd00bc5`). |
| S11 Append writes (streaming) | ❌ deferred | B9 shipped streaming via in-process accumulator (PLAN-RUNNER T14); no store-side append helper added. |
| S12 Hot tier (RAM-resident, zero-copy) | ✅ done (B3.5, `f359b50`) | See HASH-MODEL.md §5. |
| S13 Demotion (hot → warm) | ✅ done (B3.5, `f359b50`) | LRU-driven + janitor + shutdown-drain. |
| S14 Intent map | ✅ done (B3.5, `f359b50`) | `core/src/intent_map/` — append-only log + in-memory hashmap. |
| S15 Shutdown drain (hot → warm) | ✅ done (B3.5, `f359b50`) | Drains hot entries to warm before exit. |

## Implementation tasks (ordered)

### S1. Hash + path derivation ✅ done (A4, commit `22aae7f`)
- `hash_format_hex`, path-from-hash computation.
- Unit tests.

### S2. Basic in-memory index ✅ done (A5, commit `ff40c53`)
- Hashmap with shared lock.
- Insert, lookup, remove.
- No disk yet.

### S3. Write path ✅ done (A5, commit `ff40c53`)
- `store_open_write` creates temp file.
- `store_write_bytes` writes incrementally.
- `store_commit_write` fsyncs, renames, inserts in index.
- `store_abort_write` unlinks temp.
- Unit tests including concurrent writers of the same content hash (only one commits; others abort).

### S4. Read path via mmap ✅ done (A5, commit `ff40c53`)
- `store_open_read` opens + mmaps.
- Returns typed pointer + length.
- Refcount for open reads; file isn't unlinked while refs exist.
- Unit tests: concurrent readers, large files.

### S5. Startup scan ✅ done (A5 + audit `bd00bc5`)

*Post-S12 note: the warm-tier scan below populates the index with `tier = TIER_WARM` entries with `warm_fd = -1` (lazy open on first read). Hot tier starts empty — first `store_put` after startup populates it. Intent-map log scan (S14) happens in parallel on a separate file.*

- Walk directory tree.
- Parse each file's header (validate magic/version).
- Populate index.
- Delete corrupt files.
- Stats logged: entries found, entries deleted, total bytes.

### S6. Pin / unpin ✅ done (A5, commit `ff40c53`)
- `store_pin` / `store_unpin` update pin count.
- Pin tied to `(owner_type, owner_id)` so multi-pin by same owner is idempotent.

### S7. LRU eviction ✅ done (A5, commit `ff40c53`)
- Evict unpinned entries by `last_access_us`.
- Trigger: disk usage > budget.
- Janitor thread wakes every N seconds, checks, evicts.
- Unit tests: forced budget pressure, verify pinned entries survive.

### S8. Janitor + stats ✅ done (A5, commit `ff40c53`)
- Background thread.
- Emits log events (`cache_evict`, `cache_scan_complete`).
- Exposes stats via `store_stats`.

### S9. Crash recovery integration ✅ done
- Tmp file cleanup on startup (S5 extension).
- Partial file detection (header parse fail → delete).
- Integration test: kill process mid-write, restart, verify clean state.

### S10. Corruption handling ✅ done (audit `bd00bc5`)
- mmap read validates header magic; mismatch logs error, marks entry bad, removes from index.
- Corrupt files moved to `quarantine/` for post-mortem rather than deleted.

### S11. Append writes (for streaming artifacts — DEFERRED, not needed for B9)
- **Status**: deferred. B9 landed streaming without any store-side change. No `store_put_append` was added. The runner accumulates chunks in-process (inside `step_context`, geometric-realloc heap buffer) and calls the existing `store_put` once at `final=true` — see PLAN-RUNNER T14 and PLAN-VIEWER-PORT §7.2.
- **Why deferred**: the streams we currently produce fit trivially in RAM (≤2.4 MB for a 100k-point `laz-extract-disc` run). An in-process accumulator avoids introducing fsync-per-chunk syscall churn and keeps the store API surface stable. Atomicity still holds by construction — `store_put` runs exactly once at finalisation, so there is never a partial entry visible to readers.
- **When to revisit**: if/when a streaming consumer emits more than a small-single-digit fraction of available RAM (plausible for future large-area point-cloud streams, high-res intermediate rasters, or long-running algorithm runs on multi-GB inputs). At that point, re-activate the design below:
  - Helper `store_put_append(accum_handle*, bytes, len, bool final)` used by a runtime-flagged streaming mode in the runner.
  - First call allocates a temp file under `tmp/<run-id>-<write-name>`; subsequent calls append. `final=true` hashes the accumulated bytes, renames the temp file under `<root>/xx/yy/<content-hash>.bin` atomically, and registers the entry in the in-memory index.
  - Mid-stream cancel: the accumulator is torn down, the temp file is `unlink`'d. Partial bytes never reach the content-addressed tree.
  - Integration constraint: streaming writes pin the accumulator out-of-hot-tier until finalized; on `final=true` they land in the warm tier directly (hot-tier promotion would require an extra copy).
  - Unit test: write in 5 chunks; cancel after chunk 3 leaves no tmp artifact; complete write produces a store entry under `hash(concat(chunks))`.

### S12. Hot tier (RAM-resident, zero-copy producer→consumer) ✅ done (B3.5)
*Shipped in commit `f359b50` alongside PLAN-RUNNER T15/T16; see [`HASH-MODEL.md`](../HASH-MODEL.md) §5.*

- New in-memory hashmap `content_hash → store_entry_t`. Shared with warm-tier index; an entry's `tier` field discriminates.
- `store_entry_t`: content_hash, size, refcount (atomic), last_access_us, tier (`HOT` / `WARM` / `DEMOTING`), `hot_bytes` pointer (owned when tier == HOT), `warm_fd` (lazy open when tier == WARM), LRU prev/next.
- Single LRU list spanning both tiers scored by `size × (now - last_access)` (or plain LRU; pick the simpler form first).
- `store_put(key, bytes, len)` **takes ownership** of the heap buffer. Callers that need to preserve their bytes do `malloc + memcpy` at the call site. One write function; no `_owned` / `_copy` split.
- Put-path budget management: while `hot_bytes_total + len > hot_budget`, pick LRU hot entry with `refcount == 0`, call `demote_to_warm` (S13). Fall back to direct warm write if all hot entries are pinned or the new entry is larger than the hot budget.
- Read path: `store_lookup` returns a handle discriminated by tier:
  - HOT → `handle.bytes = entry.hot_bytes` (direct pointer, zero copy).
  - WARM → mmap the file on first read (cached in `entry.warm_fd` for reuse), `handle.bytes = mmap_region`.
- Refcount increment on lookup, decrement on release. Pinned (refcount > 0) entries skipped by demotion and eviction.
- Unit tests: ref-counted read/write, concurrent lookup + demotion, LRU selection under budget pressure, fall-through to warm when hot full, idempotent put of identical-content bytes.

### S13. Demotion (hot → warm) ✅ done (B3.5)
*Shipped with S12.*

- Triggered:
  - Synchronously inside `store_put` when hot-tier hard cap would be exceeded.
  - By the janitor when `hot_bytes_total > hot_soft_cap` (default 80% of hard cap).
  - On `store_close` (shutdown drain).
- Algorithm: set entry tier to `DEMOTING` (protects against lookup race), release store lock, `write + fsync + rename` to `<root>/xx/yy/<content-hash>.bin`, reacquire store lock, flip tier to `WARM`, null `hot_bytes`, set `warm_fd = -1` (lazy open on next read), free the heap buffer, update `hot_bytes_total` and `warm_bytes_total`.
- Never demotes a pinned entry. Janitor skips; synchronous put-path demotion falls back to warm if no candidate is available.
- Unit tests: demotion during concurrent reads (reader pin blocks demotion), demotion preserves bytes (post-demotion read returns same content), multiple demotions under burst write.

### S14. Intent map (persistent step-identity → content-hash map) ✅ done (B3.5)
*Shipped in commit `f359b50`; see [`HASH-MODEL.md`](../HASH-MODEL.md) §6. Lives at `core/src/intent_map/`.*

- New subsystem at `core/src/intent_map/`. Independent of the store but coresident in the cache directory.
- On-disk format: `<root>/intent.log`. Append-only. Fixed 65-byte records: `[32 B identity][32 B content_hash][1 B flags]`. Flag bit 0 = tombstone (invalidation).
- In-memory hashmap `identity → content_hash` mirrors the log's current state. All reads served from memory (O(1)). All writes append to the log AND update the hashmap.
- Public API:
  ```c
  int intent_map_open(const char* path);
  void intent_map_close(void);
  int intent_map_get(const hash_t* identity, hash_t* out_content_hash);
  int intent_map_put(const hash_t* identity, const hash_t* content_hash);
  int intent_map_invalidate(const hash_t* identity);
  ```
- Startup: scan log in order, rebuild hashmap (later records supersede earlier for same identity, tombstones delete). Partial trailing record (crash mid-append) detected by `file_size % 65 != 0`; truncate and continue.
- Compaction: on startup if log size > 2× live-entries × 65 B, rewrite from in-memory hashmap to `intent.log.compact`, atomic rename. Prunes dangling entries whose content_hash is no longer in the store (soft orphans from eviction).
- Concurrency: single `pthread_mutex_t` covering appends + hashmap updates. O_APPEND + record size under PIPE_BUF makes the `write()` atomic vs concurrent writers; the mutex just keeps the hashmap consistent with the log.
- Unit tests: round-trip, tombstones, partial-tail recovery, concurrent put/get, compaction.

### S15. Shutdown drain (hot → warm) ✅ done (B3.5)
*Shipped with S12–S14.*

- `store_close` ordering:
  1. Signal + join the janitor thread.
  2. Walk LRU from tail; for every `TIER_HOT` entry with `refcount == 0`, call the demotion path (S13). Any entry with refcount > 0 at shutdown is an unclean-shutdown warning (should not happen — all runs and sessions should be done).
  3. Close any cached warm-tier fds.
  4. `fsync` the intent-map log.
  5. Free data structures.
- Graceful shutdown preserves the cache: all hot-tier content that was produced during the session lands on disk. Next startup rehydrates warm-tier entries via the existing startup scan; hot tier is empty, repopulates as pipelines run.
- Unclean shutdown (SIGKILL, crash): hot tier lost, warm tier intact. Lost content self-heals via idempotent re-execution (content-hash + intent map makes the model resilient — see `HASH-MODEL.md` §5.9).

## Dependencies

- **Before**: logging subsystem (emits events), hash subsystem (xxh3-128 via `dioramap_hash`), runtime primitives (locks).
- **After**: runner subsystem (store_lookup during DAG walk), protocol subsystem (store_open_read during artifact send).

## Definition of done

### Warm-tier only (Phase A baseline, shipped)
- Concurrent reads + writes stable under load (stress test: 16 threads, 1 min, mixed reads/writes).
- Startup scan handles 10k entries in <5 seconds.
- LRU eviction respects pins.
- Crash-kill in any phase leaves the store in a consistent state.
- TSan clean on all access patterns.
- Integration with smoke pipeline: second run is a full cache hit.

### With S12–S15 (hash-model refactor, B3.5)
- Zero-copy producer→consumer within a pipeline run (verified by pointer equality on hot-tier handles).
- Hot-tier demotion under hard-cap pressure is TSan-clean under stress.
- `intent_map` round-trip: put → close → reopen → get returns the same content_hash.
- `intent_map` partial-tail recovery: append a record, kill -9 before the last byte, restart, entry is absent and file is consistent.
- `intent_map` compaction: startup rewrite prunes superseded and tombstoned entries; atomic via rename.
- Validate-on-use recovery: force-evict a leaf, rerun pipeline, verify the producing step re-executes and the downstream completes successfully.
- Graceful shutdown drain: all hot entries demoted to warm before process exit; restart preserves the cache.
- **Integration matrix coverage** (per [docs/quality/README.md](docs/quality/README.md), 2026-05-04): store hot/warm interaction exercised end-to-end by `batch_demo_chain_warm` (cold/warm dedup), `serve_all_cache_hit_burst` (100 RUNs hammer `store_lookup` hot path), `serve_rehydrate_run` (REHYDRATE walks `store_lookup` per artifact + pins survive the grace window), `serve_invalidate_then_rerun` (`store_put` dedup proves deterministic re-execution produces identical content hashes → downstream cache-hits).  All scenarios use per-scenario tmp `store_root`; no collision with dev/prod stores. TSan-clean on prod 2026-05-04.

## Open questions

- **Compression.** Worth it for large opaque-json artifacts? Probably zstd for kind-7 only. Deferred.
- **Deduplication.** If two content-identical artifacts exist (shouldn't, by content addressing, unless hash collision or unaligned parameters), tolerance is nil — overwrites are safe because content is byte-identical.
- **Cross-filesystem atomicity.** The cache root and temp dir must be on the same filesystem (rename is cross-device-unsafe). Startup validates.
- **GC during write-heavy phases.** If the janitor can't keep up under sustained write pressure, LRU eviction lags budget. Config: hard cap that rejects writes with `STORE_FULL` error; soft cap that accelerates janitor. Document the two thresholds.

## Risk / unknowns

- **NFS compatibility.** For future multi-node: rename semantics on NFS are subtly different. Not a Phase A concern but the write path should use `O_TMPFILE` + `linkat` on Linux for atomicity (works on NFS v4.2).
- **mmap limits.** Default open file limit matters if we have many concurrent reads. Bump `RLIMIT_NOFILE` at startup.
