# Concurrency reference

*How threads, locks, and ownership work across the v3 runtime. Keep this in sync
with the code; this is the primary reference when reviewing any PR that touches
the runner, store, pool, or log subsystems.*

---

## 1. Scope

As of this writing the v3 binary has four concurrency-relevant layers:

- **Runner**: dispatches DAG steps across a thread pool; intra-pipeline parallelism.
- **Store**: content-addressed disk cache; concurrent writers and readers.
- **Thread pool**: bounded-queue pthread pool used by the runner and (later) others.
- **Log**: ring-buffered producer / single drain thread.

Each subsystem has its own mutex / condvar set and its own invariants. They're
designed so *no cross-layer lock ordering is ever required*: you never hold the
runner's lock across a store call, never hold the store's lock across a pool
call, etc. That property is load-bearing — if it's ever broken, the system can
deadlock.

---

## 2. Lock inventory

| Layer | Object | Scope | Protects |
|---|---|---|---|
| 0 | `session_t::lock` | Per WS session | out-queue, wsi, run flags, disconnect state, pins |
| 0 | `server_t::sessions_lock` | Per server | `sessions[]` array |
| 0 | `server_t::janitor_lock` + `janitor_cv` | Per server | Grace-sweep wake-up signal |
| 1 | `runner::active_lock` | Per runner | `active_runs[]` registry (for `runner_cancel`) |
| 1 | `rs->lock` (`pthread_mutex_t`) | Per `runner_run` invocation | artifact_map, in_degree[], n_pending, cancel, cancel_external, run_rc, n_executed, n_cache_hits |
| 1 | `rs->done_cv` (`pthread_cond_t`) | Same | Main thread waits; workers broadcast on progress |
| 2 | `store->lock` | Per store | Content-addressed index (hashmap spanning hot + warm tiers), LRU list, hot/warm byte totals, per-entry `tier`, `hot_bytes`, `warm_fd`, `ref_count`, `pin_count`. Single mutex for the whole index after the hash-model refactor (was warm-only pre-refactor). |
| 2 | `store->janitor_lock` + `janitor_wake` | Per store | Signals the janitor thread; separate from the main store lock to avoid shutdown ordering issues. Janitor now handles both hot-tier demotion (hot → warm) and warm-tier eviction (warm → deleted). |
| 2 | `intent_map::lock` | Per intent map (one per store) | Append to `intent.log` (via `O_APPEND` + fixed 65-byte record) + in-memory hashmap update. Leaf; never held across I/O beyond the append `write()` syscall. Separate from `store->lock`. |
| 3 | `pool->lock` + `not_empty` + `not_full` | Per pool | Work queue ring buffer |
| 4 | Log ring-buffer lock + `not_empty` + `drained` | Global (one log system) | Producer-consumer queue |

Layer 0 (session/server) is always leaf. No code takes `session_t::lock` and
then acquires a lower-layer lock (runner, store, pool, log producer is OK
since logging is lock-free at the producer side). See §12 for the full
server-layer contract.

**Rule**: every shared-mutable data structure is guarded by exactly one of the
above. There is no "protected by convention" state in the v3 codebase.

---

## 3. Core invariants

Memorize these. Every change to concurrent code should preserve them.

1. **No lock held across long ops.** Never hold any lock across: step exec,
   `pool_submit`, a callback, `store_lookup`/`store_put`, mmap, a
   syscall with unbounded latency. Locks are for brief structural updates only.

2. **No cross-layer lock acquisition.** If you're holding `rs->lock`, do not
   call into store, pool, or log (beyond `LOG_EVENT` macro which is lock-free
   at the producer side). Release first.

3. **Hashes returned by value, not by pointer.** `artifact_map_get_locked`
   copies the hash into an out-parameter. Returning pointers into the
   artifact list would create torn-read races when another thread's put
   triggers a realloc.

4. **Submit before decrementing own `n_pending`.** A worker that completed
   a step must submit all newly-ready successors (each of which bumps
   `n_pending`) *before* decrementing its own contribution. Otherwise
   `n_pending` can transiently hit 0, main wakes, `rs` is freed, and
   pending `pool_submit` calls crash on use-after-free.

5. **Broadcast under lock.** Every `pthread_cond_broadcast` on `rs->done_cv`
   happens while holding `rs->lock`. Main's `pthread_cond_wait` atomically
   releases-and-sleeps while holding the same lock. That guarantees no
   lost wakeups.

6. **Predicate in a while loop.** `pthread_cond_wait` can return spuriously.
   Always re-check the predicate: `while (rs.n_pending > 0)`, never `if`.

7. **Callbacks may fire from any worker thread.** Caller's responsibility to
   synchronize. The runner explicitly does not serialize callbacks. See §11
   for the precise per-callback origin contract — `on_run_started`,
   `on_run_complete`, `on_error` (cancel path) currently fire from the
   caller's thread, but the contract permits any worker. Don't bake the
   "fires from caller" assumption into call sites.

8. **Per-session callback state is per-session-mutex-guarded.** Any
   `session_t::*` field that a runner callback writes (journal accumulator,
   run_id, pin list) must be touched under `session_t::lock`. The
   2026-05-01 journal-acc bug was a missed application of this rule:
   `cb_on_artifact_ready` appended to `journal_artifact_names[]` without
   the lock, racing parallel workers on cache-hit runs. When a callback
   touches session state, lock — even if the field "looks single-writer"
   today. **Recovery, 2026-05-04**: `session_t` is now opaque; all
   access goes through `session_*` accessors that take the lock
   internally. The compiler enforces the rule.

9. **Lock-drop-relock requires re-validation.** When you drop a lock
   for an unbounded operation (network I/O, syscall, callback, foreign
   subsystem call) and re-acquire it, you must:
     (a) take values across the gap, never pointers into shared state
         (cache slots, list nodes, hash entries can be freed/repurposed
         in the unlocked window),
     (b) re-validate every predicate you assumed under the first lock
         (re-look up the entry, re-check the flag, re-fetch the size),
     (c) never carry a derived invariant across the gap ("X was true
         when I unlocked, X must still be true now" — wrong).
   Legitimate uses of the pattern: long I/O under lock would serialize
   callers (cp_verify_key HTTP, store demotion fsync, store warm-tier
   mmap), calling out to user code (runner→cb_on_artifact_ready
   §4.7), atomic wait (`pthread_cond_wait` is lock-drop-relock at the
   kernel boundary), crossing a global lock-order boundary. The
   `cp_verify_key` bug (2026-05-01) was a violation of (a)+(b): a
   stale slot pointer was reused after the HTTP round-trip.

10. **Encapsulate shared mutable state.** Define the struct private to
    the .c file that owns it. Expose only opaque-pointer accessors
    that take the internal mutex. Never hand out a struct field; never
    let a caller take the internal mutex. The compiler then enforces
    the locking discipline at every call site instead of relying on
    convention. Exemplars: `store_t`, `runner_t`, `intent_map_t`,
    `log`, `session_t` (post 2026-05-04), `session_mgr_t`,
    `batch_ctx_t`. Anti-exemplar (pre 2026-05-04): the old struct-
    bag `session_t` in server.c — drift between the lock convention
    and new field additions caused the journal-acc race.

---

## 4. Runner phase-by-phase walkthrough

### 4.1 Main: initial submission + wait

```c
pthread_mutex_lock(&rs.lock);
for (size_t i = 0; i < plan_len; i++) {
    if (rs.in_degree[i] == 0) {
        submit_step_task_locked(r, &rs, i);
    }
}
while (rs.n_pending > 0) {
    pthread_cond_wait(&rs.done_cv, &rs.lock);
}
int final_rc = rs.run_rc;
pthread_mutex_unlock(&rs.lock);
```

**Why under lock the whole time**: if main released between submissions,
workers could complete and broadcast before all initials are queued. Main
could then see `n_pending == 0` prematurely and exit. Holding the lock
means any worker wanting to decrement waits for us to enter `cond_wait`
(which releases) or finish the submit loop.

**`cond_wait` semantics**: atomic release of `rs.lock` + sleep, atomic
reacquire on wake. Main wakes either on genuine broadcast or spuriously;
the `while` re-check handles both uniformly.

### 4.2 `submit_step_task_locked`

Assumes caller holds `rs->lock` on entry; guarantees same on exit.

```c
static int submit_step_task_locked(runner_t* r, run_state_t* rs, size_t plan_pos) {
    step_task_t* t = malloc(sizeof(step_task_t));
    if (!t) return -1;
    t->r = r; t->rs = rs; t->plan_pos = plan_pos;

    rs->n_pending++;                           // count the task as in-flight
    pthread_mutex_unlock(&rs->lock);           // <-- must release here
    int rc = pool_submit(r->pool, step_task_fn, t);
    pthread_mutex_lock(&rs->lock);

    if (rc != 0) {
        rs->n_pending--;
        free(t);
        return -1;
    }
    return 0;
}
```

**Why release before `pool_submit`**: pool_submit blocks if the queue is
full (the pool's `not_full` condvar). While blocked, it holds the pool's
lock waiting for a worker to free a slot. That worker, when it finishes
its step and tries to decrement `rs->n_pending`, would need `rs->lock`.
If we held `rs->lock` across `pool_submit`, that worker would be stuck
waiting for us, and we'd be stuck waiting for a slot the worker is trying
to free. Deadlock.

**Why bump `n_pending` before release**: we need "the task is in flight"
to be visible before the unlock. Otherwise there's a window where
`n_pending == 0` even though we're about to submit — main could wake,
free rs, and we'd crash.

### 4.3 Worker: cancel short-circuit

```c
pthread_mutex_lock(&rs->lock);
bool cancelled = rs->cancel;
pthread_mutex_unlock(&rs->lock);

if (cancelled) {
    pthread_mutex_lock(&rs->lock);
    rs->n_pending--;
    pthread_cond_broadcast(&rs->done_cv);
    pthread_mutex_unlock(&rs->lock);
    free(t);
    return;
}
```

**Why still decrement**: even if we skip the step, our task counted against
`n_pending` when submitted. Main won't exit until we release that count.

**Broadcast here too**: we may be the last task, in which case main needs to
wake. Always broadcast after any `n_pending` change.

### 4.4 Worker: gather read hashes

```c
hash_t* read_hashes = calloc(def->n_reads, sizeof(hash_t));
pthread_mutex_lock(&rs->lock);
for (size_t i = 0; i < def->n_reads; i++) {
    if (!artifact_map_get_locked(rs, def->reads[i], &read_hashes[i])) {
        pthread_mutex_unlock(&rs->lock);
        free(read_hashes);
        return -1;
    }
}
pthread_mutex_unlock(&rs->lock);
```

**Why copy by value**: `read_hashes[i]` stores the hash bytes, not a pointer
into `rs->artifacts[]`. A concurrent `artifact_map_put_locked` on another
thread may grow the artifacts array via realloc; any pointer into the old
buffer is now dangling. By copying at read time, we're immune.

**Why release before compute**: the step identity hash calculation plus the
cache-check plus the step exec itself can take milliseconds to seconds.
Holding the lock would serialize every worker on the run. We have what we
need (the hash values); release.

### 4.5 Worker: `step_read` / `step_write_output`

Called from inside the step's own `exec` function.

```c
// step_read:
pthread_mutex_lock(&ctx->rs->lock);
bool found = artifact_map_get_locked(ctx->rs, name, &h);
pthread_mutex_unlock(&ctx->rs->lock);
if (!found) return NULL;
store_lookup(ctx->runner->store, &h, &handle);          // separate lock in store
```

```c
// step_write_output:
store_put(ctx->runner->store, &key, bytes, len);   // takes ownership; store-internal lock
pthread_mutex_lock(&ctx->rs->lock);
artifact_map_put_locked(ctx->rs, name, &key);
pthread_mutex_unlock(&ctx->rs->lock);
```

**Contract**: step implementations can call these freely. Return value from
`step_read` is a pointer into the store's mmap region; it stays valid until
`step_read`/`step_write_output` are called on conflicting names, or until
the step's `exec` returns (whichever comes first; in practice, until exec
returns — we don't invalidate mid-step).

**Anti-pattern warning**: the step must not stash a pointer from `step_read`
past its own exec. Pointers are backed by `store_handle_t`s in
`ctx->read_handles`, which the runner releases after exec returns.

### 4.6 Worker: the bookkeeping critical section

After a successful exec, under lock, we decrement successors' in_degree and
collect newly-ready.

```c
pthread_mutex_lock(&rs->lock);
if (cache_hit) rs->n_cache_hits++;
else           rs->n_executed++;

size_t ready_stack[32];
size_t ready_n = 0;
size_t* ready_heap = NULL;

for (size_t i = 0; i < rs->n_successors[plan_pos]; i++) {
    size_t succ = rs->successors[plan_pos][i];
    if (rs->in_degree[succ] == 0) continue;        // defensive
    if (--rs->in_degree[succ] == 0) {
        if (ready_n < 32) ready_stack[ready_n++] = succ;
        else { /* heap fallback */ }
    }
}
pthread_mutex_unlock(&rs->lock);
```

**The race that requires this lock**: say steps A and B both feed step X;
`in_degree[X] == 2`. Without the lock, both workers might read `2`,
decrement locally, and write `1` — losing one decrement. Or both might
read `1` after the first decrement, both see "I'm the one who brought it
to 0", and double-submit X.

The `--in_degree[succ] == 0` idiom under the lock makes the "who saw the
zero" decision single-writer-per-successor.

**Why collect in a local array**: we want the dispatch to happen *outside*
the lock (dispatch involves `pool_submit` which can block). So stash the
ready list in function-local memory, release the lock, then submit.

### 4.7 Worker: callbacks outside the lock

```c
// rs->lock NOT held here
LOG_EVENT(SUB_RUNNER, LOG_INFO, EV_RUNNER_STEP_COMPLETE, ...);
if (cb->on_step_complete) cb->on_step_complete(cb->ctx, rs->run_id, ...);
if (cb->on_artifact_ready) {
    for (size_t w = 0; w < def->n_writes; w++) {
        hash_t h;
        pthread_mutex_lock(&rs->lock);
        bool found = artifact_map_get_locked(rs, def->writes[w].name, &h);
        pthread_mutex_unlock(&rs->lock);
        if (found) cb->on_artifact_ready(cb->ctx, rs->run_id, ...);
    }
}
```

**Why outside the lock**: callbacks may be slow. The batch-mode callback
does a `store_lookup` + file write. The (future) WS callback will copy
bytes onto a network buffer. Both are tens of microseconds to milliseconds.
Under the lock, they'd block every other worker on the same run.

**Why `rs` is still alive**: `rs->n_pending` has not yet been decremented
for this task. Main's `cond_wait` predicate is therefore still non-zero,
so main hasn't returned and hasn't destroyed `rs`. The worker can freely
read `rs->run_id`, `def->name`, etc. across the unlocked window.

**Thread-safety of callbacks themselves**: explicitly the caller's problem.
The contract is documented in `v3/core/include/v3/runner.h`. The batch
implementation uses its own `pthread_mutex_t` in `batch_ctx_t`. The
runner_test uses `atomic_size_t` for counters.

### 4.8 Worker: submit successors BEFORE decrementing own `n_pending`

**This is the most important ordering invariant in the runner.**

```c
size_t* ready = ready_heap ? ready_heap : ready_stack;
for (size_t i = 0; i < ready_n; i++) {
    pthread_mutex_lock(&rs->lock);
    if (!rs->cancel) {
        if (submit_step_task_locked(r, rs, ready[i]) != 0) {
            rs->cancel = true;
            rs->run_rc = -1;
        }
    }
    pthread_mutex_unlock(&rs->lock);
}
free(ready_heap);

pthread_mutex_lock(&rs->lock);
rs->n_pending--;                         // <-- LAST
pthread_cond_broadcast(&rs->done_cv);
pthread_mutex_unlock(&rs->lock);
```

**Why the ordering matters**: imagine we decremented our own `n_pending`
first and left successor submission for after. Scenario: a step with
two successors, both independent terminals. After completing:

```
BUG order:
  rs->n_pending--               // count goes from 1 to 0
  pthread_cond_broadcast(...)
  // lock released
  // ... main wakes, exits loop, destroys rs ...
  pool_submit(... ready[0] ...) // rs is freed — use-after-free
```

Correct order: successors submitted first (each bumps `n_pending` under
lock), own decrement last. `n_pending` goes from 1 → 2 → 2 → 1 for the
first successor, then → 2 → 2 → 1 for the second, then our own decrement
makes it → 0. Main wakes exactly once, after everything is in flight.

This was the first bug I hit. It's non-obvious if you aren't watching for it.

### 4.9 Main: post-wait cleanup

After `cond_wait` returns with `n_pending == 0`:

```c
int final_rc = rs.run_rc;
size_t executed = rs.n_executed;
size_t cache_hits = rs.n_cache_hits;
pthread_mutex_unlock(&rs.lock);

// fire on_run_complete / on_error outside the lock — no workers left to contend
...

// free rs state, destroy lock + cv
for (size_t i = 0; i < rs.n_artifacts; i++) free(rs.artifacts[i].name);
free(rs.artifacts);
free(rs.plan);
free(rs.in_degree);
for (size_t i = 0; i < rs.plan_len; i++) free(rs.successors[i]);
free(rs.successors);
free(rs.n_successors);
pthread_cond_destroy(&rs.done_cv);
pthread_mutex_destroy(&rs.lock);
```

**Why safe to destroy lock/cv**: no worker is inside the critical section
when main holds the lock (main just did `cond_wait` → reacquired lock,
and `n_pending == 0` means every worker either never ran or has already
returned from `step_task_fn`). After releasing main's lock-hold, there is
no worker that will ever try to lock rs again.

**Edge case: failed initial submit**. If `submit_step_task_locked` fails in
Phase 4.1, we set `rs.cancel` and `rs.run_rc = -1`. Any in-flight workers
spawned so far will eventually decrement `n_pending` and exit. Main's
while loop waits for them, then cleans up. Same code path.

---

## 5. Store-internal synchronization

The runner never touches the store's lock directly, but it's worth knowing
the store's rules.

- **`store_put`** (post-refactor; was `store_put_with_hash`): fast path
  under lock if hash already present — caller's buffer is freed
  (ownership transferred + noop'd); access time bumped. Miss path: try to
  fit in hot tier, demoting LRU hot entries as needed (each demotion is
  write+fsync+rename executed with the store lock held; ~30 ms for 500 MB
  on SSD). If hot can't accommodate, write directly to warm via
  temp+fsync+rename with the store lock released during I/O, re-acquired
  to insert the entry. The temp+rename pattern makes writes atomic and
  tolerant of concurrent writers of the same content.

- **`store_lookup`**: locks, finds entry, bumps ref_count + access time,
  unlocks. Hot-tier entries: handle carries a direct heap pointer (zero
  copy). Warm-tier entries: open the file (lazy; cached on the entry) and
  mmap; handle carries the mmap'd pointer. File ops run outside the store
  lock. If the file vanished (concurrent eviction — shouldn't happen with
  ref_count/pin semantics) undo and report miss.

- **`store_release`**: decrements ref_count atomically. If the handle was a
  warm-tier mmap, unmaps.

- **`store_contains`**: locks briefly to check presence; returns bool.

- **Janitor thread**: wakes on `janitor_wake` condvar (signaled on shutdown
  or by a periodic soft-cap tick). Two passes per wake: (1) hot-tier
  demotion when `hot_bytes_total > hot_soft_cap` — pick LRU unpinned hot
  entries, demote; (2) warm-tier eviction when `warm_bytes_total >
  warm_budget` — pick LRU unpinned warm entries, unlink files, remove
  index entry. Both under the store lock; file I/O released/reacquired as
  needed.

**Key cross-layer rule**: the store's lock is completely independent of the
runner's `rs->lock`. The runner never holds `rs->lock` across a store call.
Conversely, the store never holds its lock across a callback into user code
(it doesn't accept callbacks at all).

### 5.1 Intent map synchronization (post-refactor T15)

The intent map is a peer of the store, not a layer of it. Two operations,
both leaf-level under their own mutex:

- **`intent_map_put(identity, content_hash)`**: format the 65-byte record,
  acquire the intent-map mutex, `write(log_fd, rec, 65)` (O_APPEND +
  fixed record size under `PIPE_BUF` — atomic against concurrent writers),
  update the in-memory hashmap, release.
- **`intent_map_get(identity, &out)`**: acquire the mutex, hashmap lookup,
  release. No file I/O at read time.

Lock order: `intent_map::lock` is a leaf. It is never acquired while
holding `store->lock` or `rs->lock`. `cached_compute` (§5.2 below)
orchestrates both intent-map and store calls but acquires neither lock
simultaneously — puts and lookups are separate brief critical sections.

### 5.2 `cached_compute` concurrency (post-refactor T15)

The unified cache primitive from `HASH-MODEL.md`. Called at step-scope by
the runner and at sub-scope by step bodies.

- **Hit path**: one `intent_map_get` under the intent-map lock (µs),
  followed by `store_lookup` on the returned content hash under the
  store lock (µs). No compute, no heavy I/O. Two sequential short
  critical sections on different locks.

- **Miss path**:
  1. `intent_map_get` (miss) — brief, intent-map lock.
  2. `compute_fn(ctx)` — runs outside any lock. May take arbitrary time
     (HTTP call, heavy compute, a whole step body).
  3. `hash_bytes(output)` — no lock.
  4. `store_put(content_hash, bytes, len)` — brief, store lock.
  5. `intent_map_put(identity, content_hash)` — brief, intent-map lock.

  Steps 4 and 5 are independent puts on different locks; either may
  succeed without the other. Crash between 4 and 5 leaves bytes in the
  store without an intent-map entry — next lookup misses, recomputes,
  `store_put` is idempotent, `intent_map_put` lands. Self-healing.

- **Concurrent calls with the same identity**: two threads both miss in
  the intent map, both call compute_fn, both produce the same bytes (by
  determinism), both `store_put`s are idempotent, both `intent_map_put`s
  write records with the same (key, value) — log-level duplicate,
  in-memory hashmap update is a no-op. Wasted compute is the cost; no
  correctness issue. Could be avoided with a per-identity mutex or a
  "pending" table but not worth the complexity at our scale.

- **Sub-scope calls inside a step body**: the runner's outer
  `cached_compute` at step scope is in-flight on the worker thread. The
  inner sub-scope call (e.g. per-URL fetch) acquires the intent-map +
  store locks transiently, releases before returning. No lock held
  across the outer/inner boundary.

- **Validate-on-use (T16)**: the runner's step dispatch calls
  `cached_compute`, then parses the returned manifest, then calls
  `store_contains` per entry. All of this happens on the runner's
  worker thread without holding any rs-level or session-level lock.
  Intent-map invalidation on stale-manifest detection calls
  `intent_map_invalidate`, which appends a tombstone record under the
  intent-map lock. Simple leaf-level operation.

---

## 6. Thread pool internal synchronization

Also not the runner's direct concern, but:

- **`pool_submit`**: acquires pool lock, waits on `not_full` if queue is
  full, copies `(fn, arg)` into ring slot, signals `not_empty`, releases
  lock. **Returns -1 only on shutdown** — a full queue never fails, it
  blocks.

- **Worker loop**: acquires pool lock, waits on `not_empty`, pops one
  slot into a local, signals `not_full`, releases lock, invokes `fn(arg)`
  with no pool lock held. Repeats until shutdown.

- **`pool_shutdown`**: sets shutdown flag under pool lock, broadcasts both
  condvars, joins all worker threads. Queued work drains normally (workers
  keep running as long as queue has items).

The workers run our `step_task_fn` with **no pool lock held**. This is
what makes it safe to `pool_submit` from inside `step_task_fn`.

---

## 7. Log subsystem: producer / drain

The log subsystem has its own ring buffer and one dedicated drain thread.

- **`LOG_EVENT` producers**: briefly acquire the ring's mutex, memcpy the
  formatted line into a free slot, signal `not_empty`, release. If the ring
  is full, the event is **dropped** (atomic counter tick) and producer
  returns immediately. Producers never block on slow sinks.

- **Drain thread**: waits on `not_empty`, pops a batch under lock, releases,
  writes the batch to stdout (or the current sink), loops. Writes to
  stdout happen outside the ring lock.

- **Testing**: `log_sink_swap_for_testing` flushes the ring to the current
  sink before installing a new one. Tests call `log_flush()` after emitting
  to ensure all events are on the sink before asserting.

**Crucial property for callers**: `LOG_EVENT` is safe from any thread,
including the runner's workers. It never takes rs->lock, store->lock, or
pool->lock — orthogonal subsystem.

---

## 8. Ownership and lifetime

- **`runner_t`** owns its pool. Created in `runner_new`, destroyed in
  `runner_free`. Multiple concurrent `runner_run` calls on the same
  runner share the pool (inter-run concurrency, needed for A7 WS sessions).

- **`run_state_t`** is stack-local to a single `runner_run` call. Its
  lifetime is bounded by the function. Workers spawned during that run
  must not outlive it — enforced by the "submit before decrementing"
  invariant (§4.8).

- **`step_task_t`** is heap-allocated per submission, freed by the worker
  after its final broadcast in `step_task_fn`.

- **`step_context_t`** is stack-local to one step exec invocation.
  Read handles tracked inside are released immediately after `def->exec`
  returns. The step must not stash pointers from `step_read` past its
  own exec.

- **`store_handle_t`** lifetime is from `store_lookup` until `store_release`.
  While a handle is alive, the underlying file is mmap'd and the pin
  count is bumped; the janitor cannot evict it.

- **Artifacts** in the cache live until evicted. Eviction is blocked while
  any handle is open or session ref is held. No session layer exists yet;
  pins today come only from in-flight reads.

---

## 9. Anti-patterns (do not do)

1. **Never hold rs->lock across** pool_submit, store_*, mmap, open, write,
   fsync, a syscall, a callback, step->exec, or a malloc of unknown size.

2. **Never return a pointer into the artifact map.** Always copy the hash
   by value into an out-param.

3. **Never decrement `n_pending` before submitting successors.** This was
   the first bug.

4. **Never `cond_signal` outside the lock.** Broadcast/signal must happen
   under the matching mutex so cond_wait's atomic release-and-sleep
   serializes correctly.

5. **Never check a condition variable's predicate with `if`.** Use `while`
   to handle spurious wakes.

6. **Never assume callbacks fire sequentially.** The runner dispatches
   from multiple worker threads; callbacks race. Caller must synchronize.
   Bug 2026-05-01: `session_journal_acc_append` violated this invariant
   for ~3 weeks. Crash repro is "all-cache-hit run" (steps finish in µs,
   workers fire in parallel). Doc: `BUG-cache-hit-runner-cleanup.md`.
   Canonical positive example post-fix: `session_journal_append_artifact`
   in `core/src/server/session.c` takes the session lock for its
   realloc + slot-write critical section. After the 2026-05-04
   encapsulation refactor, the structural recovery is that direct
   field access is impossible — callers see only the accessor API.

7. **Never access `rs->cancel` or `rs->run_rc` without the lock.** They're
   not declared atomic; consistency is mutex-provided.

8. **Never call `runner_free` while a `runner_run` is in flight.** Higher
   layer (session manager, to land in A7) is responsible.

9. **Never call `pool_submit` from inside the pool's worker loop WHILE
   holding the pool's lock.** The pool's API doesn't even expose this
   path — our `step_task_fn` submits successors and the pool's own queue
   access is separate. Just don't do anything weird.

10. **Never signal `rs->done_cv` from outside `rs->lock`-holding code.**
    If you're adding a new reason for main to wake, gate it through the
    same lock.

11. **Never read a shared mutable bool/scalar without lock or `_Atomic`.**
    Plain `bool`/`int`/pointer reads of fields written by another thread
    are UB even on x86 (compiler is free to fuse reloads, hoist out of
    loops, or assume single-writer). Known offenders surveyed
    2026-05-01: `session_t::run_active`, `late_subscribe_active`,
    `run_thread_joinable`, `batch_ctx_t::error_count`. Each one is
    "probably benign" today because of an implicit barrier (a join, a
    `pthread_create`, or a state-machine invariant), but each one is
    one refactor away from being an actual race. New shared scalars:
    pick `_Atomic` (cheap, no callsite changes) or guard with the
    nearest mutex.

12. **Never re-use a pointer into a shared cache after dropping its
    lock.** If you locked a cache, took a slot pointer, dropped the lock
    for a long op (network, syscall), and then re-acquired — re-look up
    the slot. A concurrent caller may have evicted/repurposed it. The
    `cp_verify_key` race (2026-05-01) was exactly this: pointer captured
    pre-HTTP, slot evicted by a different `(api_key, origin)` during
    the unlocked window, post-HTTP write corrupted the other thread's
    cache entry.

13. **Never lock-handoff: `lock(A); lock(B); unlock(A); unlock(B)`.**
    The only legitimate two-lock pattern is strict LIFO:
    `lock(A); lock(B); ...; unlock(B); unlock(A)`. The handoff variant
    breaks lock-pairing discipline, makes the protected invariant
    ambiguous (what does B guard once A is released?), and is hard to
    RAII-wrap. If you find yourself wanting it, the right answer is
    almost always to either (a) make B a leaf and finish your work
    under A then re-take B alone, or (b) carry the handoff out of band
    via a snapshot. Equally, lock acquisition order across the codebase
    must follow the global order in §12.3 — strict LIFO without a
    consistent global order is still deadlock-prone.

14. **Never expose a struct field protected by an internal mutex.**
    If `s->FIELD` is touched under `s->lock`, then `s` must be opaque
    and `FIELD` must be reached only through `s_get_FIELD` /
    `s_set_FIELD` accessors that take the lock. Direct field access
    means every call site has to remember the convention; the
    convention always drifts; drift always becomes a bug. (The
    journal-acc race made this invariant explicit.)

---

## 10. Known non-minimal locked sections (acknowledged debt)

These are not bugs. They're places where the locked critical section is
doing *slightly* more than strictly required. Listed so reviewers know
they're known and intentional-for-now.

### 10.1 `submit_step_task_locked` mallocs under lock

```c
step_task_t* t = malloc(sizeof(step_task_t));   // 24 bytes, under rs->lock
```

Measured cost: 10–50 ns from a thread-local arena. Could be moved before
the lock with a small contract change (caller pre-allocates). No current
contention signal to motivate the refactor.

### 10.2 Phase-6 `ready_heap` fallback allocation under lock

```c
ready_heap = malloc(rs->n_successors[plan_pos] * sizeof(size_t));
```

Only triggers if a single plan entry has >32 direct successors — hasn't
happened in any real pipeline. If it ever does, move the alloc outside
with a pre-size-check pass.

### 10.3 Artifact map is a linear list

Every `artifact_map_get_locked` / `_put_locked` scans with strcmp. O(N)
per call. For pipelines with hundreds of named artifacts, total lock-held
time per step grows to tens of microseconds. Fine for our current scale
(< 50 artifacts per run). The store already has a proper open-addressing
hashmap; we'd lift that pattern into the runner when profiling warrants.

### 10.4 Main holds rs->lock across the initial-submit loop

The loop body releases via `submit_step_task_locked`'s internal unlock,
so actual lock-held chunks are small. But wall time for the loop scales
with plan_len. At plan_len in the tens, negligible. At plan_len in the
hundreds, could stall the first workers a few hundred microseconds.
Would fix by chunking submissions + briefly releasing between chunks if
ever observed.

---

## 11. Thread-safety contract for external callers

This is what a new subsystem integrating with the runner needs to know.

### runner_callbacks_t

- Fields may be NULL individually; the runner no-ops on null.
- **Every callback may fire from any compute-thread-pool worker thread.**
- **Multiple callbacks may be in flight concurrently.** For example:
  `on_artifact_ready` for a write of step A can overlap with
  `on_step_started` for step B.
- The pointers passed (`run_id`, `step_name`, `artifact_name`) are stable
  for the duration of the callback; do not stash them unless you copy.
- The `hash_t*` passed to `on_artifact_ready` is a local pointer valid
  only for the duration of the callback. Copy the value if you need it
  later.

#### Per-callback origin (current implementation)

The contract above is the *worst case*. As of 2026-05-01 the runner
actually fires callbacks from these threads:

| Callback | Origin today | Notes |
|---|---|---|
| `on_run_started` | runner_run's caller thread | Fires once, before any task is dispatched (runner.c:1499). |
| `on_step_started` | worker pool thread | One per executed step. May overlap with siblings. |
| `on_step_complete` | worker pool thread | Same. May overlap with `on_artifact_ready` for the same step. |
| `on_artifact_ready` | worker pool thread | Fires per artifact write. Multiple concurrent writes (different steps) → multiple concurrent callbacks. |
| `on_stream_chunk` | worker pool thread | Same step's chunks are serial; cross-step interleaves. |
| `on_run_complete` | runner_run's caller thread | Fires once, after all workers have decremented n_pending to 0 and joined back to main. |
| `on_error` | runner_run's caller thread (cancel) OR worker (step failure) | Two origins. Don't assume. |

**Do not** bake the "fires from caller" assumption into call sites. The
contract is "any worker thread", and the implementation is one
refactor away from honoring it more literally. The 2026-05-01 audit
flagged several sites (`cb_on_run_started`, `batch_on_run_started`)
that touch shared state assuming serial execution — they were locked
defensively to honor the contract.

### store_t

- `store_put` (post-refactor; takes buffer ownership), `store_lookup`, `store_contains`, `store_release`,
  `store_pin`, `store_unpin`, `store_get_stats` are all safe to call from
  any thread concurrently with each other, including on the same hash.
- `store_open` and `store_close` are NOT thread-safe against other store
  ops. Called once at program start / end by main.

### thread_pool_t

- `pool_submit` is thread-safe (can be called from any thread, including
  from inside a worker running a task on the same pool).
- `pool_shutdown` and `pool_destroy` are NOT thread-safe against
  `pool_submit` — stop submitting before shutting down.

### log

- `LOG_EVENT` / `LOG_EVENT0` / `log_flush` are thread-safe from any thread.
- `log_init` / `log_shutdown` are idempotent and single-owner.
- `log_sink_swap_for_testing` is thread-safe (internally flushes then swaps
  under lock) but is a test-only helper.

---

## 12. Server: WS service thread + sessions (A7.1)

The WebSocket server (`core/src/server/server.c`) sits on top of the runner
and adds three new threads/roles. This section records the synchronization
contract introduced in A7.1 so future additions (reconnect, late subscribe,
cancellation from disconnect) land on an already-documented foundation.

### 12.0 Terminology

Used consistently below — and worth referencing from other docs to
avoid the four-way drift across "session manager" / `session_mgr_t` /
"sessions table" / "sessions_lock" that crept into earlier sketches:

- **session manager** — the abstract concept (the thing that owns
  the sessions table + janitor). Use in prose only.
- **`session_mgr_t`** — the concrete C type implementing the
  manager.  Defined in `core/src/server/session_mgr.c`; opaque
  pointer outside that file.
- **`sessions[]`** — the field inside `session_mgr_t` holding
  `session_t*` pointers. Mutated only via `session_mgr_*` accessors.
- **`sessions_lock`** — the specific mutex guarding `sessions[]`.
- **`session_t`** — the per-connection state struct (one per WS
  client). Defined in `core/src/server/session.c`; opaque pointer
  outside that file. Each one carries its own internal mutex
  (`session_t::lock`).
- **`session_t::lock`** — the per-session mutex. Distinct from
  `sessions_lock`; lock-ordering is `sessions_lock → session_t::lock`
  (see §12.3).

### 12.1 Thread cast

| Thread | Role |
|---|---|
| **Service thread** | The caller of `server_run()`. Drives `lws_service()`. Owns all WS I/O — inbound frame parsing, outbound `lws_write()`, connection accept/close, RECONNECT swap. Never blocks on compute work. |
| **Run thread** (per active RUN or SUBSCRIBE) | Detached `pthread_create()` from `handle_run_message()` or `handle_subscribe_message()`. Calls `runner_run()` synchronously. Destroyed when the run completes. Joined by the next RUN on the same session or by `session_destroy()`. |
| **Runner worker pool** | Same thread pool as §4–6. Fires the runner callbacks (on_run_started, on_step_complete, on_artifact_ready, …) for the in-flight run. |
| **Janitor thread** (per server, if grace_sec > 0) | Wakes every ~1s; walks `sessions[]` and destroys any session whose `disconnected` timestamp has exceeded `grace_sec`. |
| **Session objects** | Not threads, but shared state. Each connection has one `session_t` with a mutex, out-queue, run thread slot, subscription mirror, pin list, and grace state. |

### 12.2 Mutexes

| Mutex | Guards | Held by |
|---|---|---|
| `session_t::lock` | All `session_t` fields (queue, run state, subscribe[], pins[], journal accumulator, last-run state, auth, grace state). **Held only internally by `session_*` accessor functions** (post 2026-05-04 encapsulation). Outside callers do not see this lock. | session.c accessors only |
| `session_mgr_t::sessions_lock` | `sessions[]` table | session_mgr.c accessors (register / unregister / reattach / arm_pending_writes / janitor expiry sweep) — never held across network I/O |
| `session_mgr_t::janitor_lock` + `janitor_cv` | Janitor wake-up + shutdown signal | session_mgr_disconnect / session_mgr_free (signal), janitor_thread_main (wait) |
| `batch_ctx_t::lock` | `produced[]`, `error_count`, `run_id`. Held only internally by `batch_*` accessor functions. | batch_ctx.c accessors only |
| `runner::active_lock` | `active_runs[]` pointer array | `runner_run` on register / unregister, `runner_cancel` on scan. Nests with the per-run `rs->lock` (active_lock taken first). |

### 12.3 Cross-thread wakeup invariant

The critical primitive is `lws_cancel_service()`. `lws_callback_on_writable()`
is **not** thread-safe in libwebsockets 4.3; calling it from a foreign thread
races the service-thread event loop. The indirection is:

```
producer (foreign thread, e.g. runner worker)
    ├── lock session->lock
    ├── append out_msg to queue
    ├── unlock
    └── lws_cancel_service(ctx)          // wakes service thread's poll()

service thread (wakes from poll via EVENT_WAIT_CANCELLED)
    ├── server_arm_pending_writes()      // iterate sessions
    ├── for each session with out_head != NULL and wsi != NULL:
    │       lws_callback_on_writable(wsi)
    └── return to poll

service thread (subsequent SERVER_WRITEABLE callback)
    ├── lock session->lock, pop one out_msg, note has_more, unlock
    ├── lws_write(wsi, msg->buf + LWS_PRE, len, mode)
    ├── free(msg)
    └── if has_more: lws_callback_on_writable(wsi)   // same thread; safe
```

### Global lock acquisition order

Every two-lock site in the codebase MUST follow this order. New code
that needs to nest locks differently MUST update this list.

```
sessions_lock        →  session_t::lock          (manager iteration only)
runner::active_lock  →  rs->lock                  (runner_cancel scan)
cp_client::cache_lock → cp_client::ring_lock      (cp_client_get_stats only)
```

Leaves (never hold another lock while taking these):

```
session_t::lock
batch_ctx_t::lock
store_t::lock
intent_map_t::lock
cp_client_t::ring_lock       (when not nested under cache_lock)
runner::rs->lock             (when not nested under active_lock)
log internal mutexes
thread_pool internal mutexes
```

Nesting MUST be strict-LIFO: `lock(A); lock(B); ...; unlock(B); unlock(A)`.
Lock hand-off (`lock(A); lock(B); unlock(A); unlock(B)`) is forbidden;
see §9 anti-pattern 13.

Today the only nesting site in the server subsystem is the
`session_mgr_*` iteration code (`reattach`, `arm_pending_writes`,
janitor expiry sweep) which holds `sessions_lock` while calling
`session_*` accessors that briefly take `session_t::lock`. No I/O, no
malloc; the inner critical sections are O(1). Safe, documented in
session_mgr.c.

### 12.4 Invariants

- **Out-queue FIFO is preserved**: only the service thread dequeues. Producers
  append at the tail; the single consumer pops from the head. Linearization
  of messages is the order of the enqueue → `lws_cancel_service()` calls.

- **Binary artifact frame follows its header**: `cb_on_artifact_ready` enqueues
  `ARTIFACT_HEADER` (JSON) *then* the binary payload in the same critical
  section sequence. The runner worker that fires this callback is
  single-threaded for one artifact (artifact production is serial per-write
  in the runner's commit phase), so two artifacts cannot interleave
  header/payload pairs on the wire.

- **`session->wsi` is nulled under lock on close**: `LWS_CALLBACK_CLOSED`
  locks, sets `wsi = NULL`, unlocks. Any producer that re-acquires the lock
  afterwards sees `wsi == NULL` and drops the message on the floor instead
  of enqueueing it. This avoids use-after-free on the `lws_write()` path.

- **Run thread outlives the WS connection it was started for**: if the client
  disconnects mid-run, the runner keeps running. Its callbacks try to
  enqueue, `session_enqueue` returns -1 (wsi is NULL), the message is
  discarded. `session_destroy()` pthread_joins the run thread before freeing
  the session so the runner can't touch freed memory. Cancellation of the
  run is a future item (T11) — for now, the run completes and the output
  is absorbed by the store anyway, which is harmless.

- **Only one run per session at a time**: `handle_run_message` rejects a
  second RUN with `ERROR { code: "RUN_BUSY" }` while `run_active == true`.
  On the next RUN, if the previous run's thread is still joinable, it is
  joined first. This keeps the "ownership over sharing" discipline intact:
  at any moment exactly one run thread writes to `session->run_id` and
  `session->subscribe[]`.

### 12.5 Grace period, reconnect, cancel, late SUBSCRIBE (A7)

The A7-complete set of behaviors extend §12.1–§12.4 without changing their
invariants. Summary:

**Grace period + janitor thread.**
- A fifth thread role joins the cast: the `janitor_thread` (one per server)
  wakes every ~1s and destroys any session that has been in `disconnected`
  state for longer than `server_config.grace_sec` (default 600s).
- `server_t` adds a `janitor_lock` / `janitor_cv` pair that's independent of
  `sessions_lock` — the janitor holds `janitor_lock` only for its own wait;
  it re-acquires `sessions_lock` to iterate the table.
- Lock order on expiry sweep: `sessions_lock` → per-session `session_t::lock`
  (scan only; no nested I/O). Destroys happen outside both locks.

**Disconnect = detach, not destroy (when grace_sec > 0).**
- `LWS_CALLBACK_CLOSED` calls `server_disconnect_session`, which cancels the
  active run (`runner_cancel`), nulls `wsi`, drains the out-queue, stamps
  `disconnect_us_monotonic`, and signals the janitor.
- The session remains in `server_t::sessions[]` with its pins + last_pipeline
  + last_input intact so a future RECONNECT can resume.

**Reconnect.**
- `handle_reconnect_message` runs on the service thread with the fresh
  ephemeral session (created on ESTABLISHED) already attached to the new
  wsi. It walks `sessions[]` looking for a disconnected match, under
  `sessions_lock` with per-session `lock` nested only for flag reads /
  flips. On success it swaps `*user` to the preserved session, destroys
  the ephemeral, and enqueues `RECONNECTED`.

**Late SUBSCRIBE.**
- Re-enters the run-thread pattern from §12.1 with one new per-session flag:
  `late_subscribe_active`. The runner callbacks read the flag and suppress
  RUN_STARTED / RUN_COMPLETE / STEP_* events, emitting `SUBSCRIBE_ACK`
  instead at the end. No new locks, no new lifetime shape.

**Store pins.**
- Sessions own a list of `hash_t` pins guarded by `session_t::lock`.
- `cb_on_artifact_ready` calls `session_pin_artifact` **before** reading the
  bytes — this both guarantees the entry can't be LRU-evicted mid-write and
  retains it for reconnect-then-SUBSCRIBE service-from-cache.
- `session_destroy` unpins all after `pthread_join` of the run thread — so
  no concurrent enqueue can race the unpin.

**Cancel invariant.**
- `runner_cancel()` takes `runner::active_lock`, finds the run_state_t by
  run_id string, acquires `rs->lock`, flips `cancel` + `cancel_external` +
  `run_rc = -1`, broadcasts `done_cv`, releases both locks.
- Lock order added: `runner::active_lock` → `rs->lock`. No other code
  nests these two.
- The waiting main thread wakes on the broadcast; in-flight workers
  short-circuit via the `cancel` check at the top of `step_task_fn` and
  drain `n_pending` to 0.

### 12.6 Known deferred (post-A7)

- **Visibility tier enforcement**: catalog entries already carry
  `visibility: "public" | "debug"`, but the connection tier is always
  `debug` today. Gating requires auth (control-plane, A10).
- **HTTP endpoints** (`/healthz`, static file serving): libwebsockets supports
  it on the same port — pure protocol-callback work. Not concurrency-relevant.
- **libwebsockets log → `log` subsystem**: currently direct to stderr at
  `LLL_ERR | LLL_WARN`.
- ~~**Dedicated session_mgr_t file**~~: **DONE 2026-05-04.** Split
  into `core/src/server/session.c` (opaque session_t + accessors) and
  `core/src/server/session_mgr.c` (sessions[] table + janitor +
  reattach). Public headers `v3/session.h` and `v3/session_mgr.h`. The
  refactor was driven by the journal-acc race (§3 invariant 8) — the
  old struct-bag design required every callsite to remember the
  locking convention; encapsulation closes that drift surface.
  `batch_ctx_t` got the same treatment (`v3/batch_ctx.h`,
  `core/src/batch/batch_ctx.c`).
- **Array artifacts (PLAN-RUNNER T13)**: no threading implications —
  `ARRAY_INLINE` / `ARRAY_BY_HASH` are just new artifact kinds. A step with
  an array read is still a single task; no fan-out instancing. See
  UNIFIED-ARCHITECTURE §5.3 and PLAN-VIEWER-PORT §6.
- **Streaming writes (PLAN-RUNNER T14)**: introduces multi-call writes per
  step. Invariants to preserve when it lands:
  - Chunks are appended to a temp file under `tmp/<run-id>-<write-name>`;
    no lock held across the write syscall.
  - The accumulator handle is owned by the step (step-local state), not by
    the runner's `rs`. No cross-thread aliasing.
  - `final=true` hashes the accumulated bytes, renames the temp file
    under `<root>/xx/yy/<content-hash>.bin` atomically, inserts the warm-
    tier entry in the store index.
  - Mid-stream cancel: step_task_fn's cancel-check fires between chunks;
    the accumulator's destructor `unlink`'s the temp file before the task
    returns. No partial bytes ever reach the content-addressed tree.
  - WS `STREAM_CHUNK` emission uses the existing session out-queue — each
    chunk enqueued independently, service thread drains as today. No new
    lock, no new wakeup primitive.

- **Hash-model refactor (T15 + T16 + PLAN-STORE S12-S15)**: lands between
  B3 and B4. Architectural implications covered in §5 (store), §5.1
  (intent map), §5.2 (cached_compute). This section lists only the
  threading-relevant changes:
  - Store lock spans hot + warm tiers; a single mutex for the whole index.
  - New `intent_map::lock` leaf. Appends under mutex (single 65-byte
    `write()` per record). Never held across I/O.
  - `cached_compute` uses intent-map and store locks in separate short
    critical sections; never simultaneously.
  - Hot-tier demotion holds the store lock during the `write + fsync +
    rename`; ~30 ms for 500 MB on SSD. Acceptable at our scale; revisit
    if profiling shows contention.
  - Concurrent `cached_compute` calls with the same identity can both
    miss and both execute. Wasted compute, no correctness issue, no
    deadlock.
  - Shutdown drains hot-tier entries to warm under the store lock; stops
    the janitor first, walks LRU, demotes each unpinned entry.
  - Crash-safety inherited from: content addressing (idempotent puts) +
    atomic temp+rename + intent-map partial-tail truncation on startup.

- **Reverse-index cascade tracking (deferred Phase C)**: if the
  validate-on-use recovery path (§ `HASH-MODEL.md` §11.2) fires too
  often in operational reality, we could preemptively pin leaves
  referenced by live manifests by maintaining a `leaf → manifests`
  reverse index. New persistent data structure + maintenance on every
  manifest write. Not in Phase B scope.

---

## 13. When this doc needs updating

- New mutex introduced anywhere → add it to §2.
- New invariant discovered or bug fixed → add it to §3 and §9 if it was
  an anti-pattern.
- Lock-held time grows in a section → update §10.
- New subsystem with its own threading → add a §6-style section.
- New callback surface → update §11.
- **New shared-mutable session/server field** (added during normal
  feature work — accumulators, counters, caches, flags) → list it
  under the appropriate mutex's "Guards" column in §2 and §12.2.
  This is the doc gap that caused the 2026-05-01 journal-acc race —
  the fields were added without the doc being updated, so the next
  implementer adding a callback handler had no signal that they
  needed to lock. Drift between code and doc here = real bug.

*End of concurrency reference.*
