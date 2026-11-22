# BUG — `session_journal_acc_append` race on parallel cache-hit runs

**Status**: ✅ **RESOLVED 2026-05-04**. Lock added to the journal-acc
path (commit `d91f0ab` — included in the same change as the broader
encapsulation refactor that closes the structural drift surface this
bug exposed).  Validated under ASan + helgrind locally and under TSan
on the dioramap.com prod box (10/10 scenarios pass, zero TSan
warnings).  See "Resolution" section at the bottom for the postmortem.

The original investigation, repro, and fix-proposal text below is
preserved as a record of the diagnosis path.

---

**Original status** (kept for the historical record): root cause
identified, fix not yet applied. Caught by ASan in dev session
2026-05-01. Real bug, not a transient. Should be fixed before
agri-map prod traffic warms the cache enough to hit it regularly.

## Root cause (one-line)

> **Note for current readers**: the file paths and line numbers below
> reference the **pre-2026-05-04 server.c** (where session_t was a
> struct-bag).  After the encapsulation refactor (commit `d91f0ab`,
> see "Resolution" at the bottom), the equivalent code lives in
> `core/src/server/session.c`; `session_journal_acc_append` is now
> `session_journal_append_artifact`.  Greps against the current tree
> won't find the old paths.

`session_journal_acc_append` (`v3/core/src/server/server.c:663`) is
called from worker-pool threads via `cb->on_artifact_ready`
(`v3/core/src/runner/runner.c:1304`) **with no synchronization**. On
all-cache-hit runs every step finishes in microseconds → workers fire
the callback simultaneously → the unguarded `realloc + strdup +
slot[n]= + n++` sequence races, leaving wild pointers in
`s->journal_artifact_names[i]` that crash later in `cb_on_run_complete`
when the journal walks the array.

## Symptom — what users see

Processor crashes mid-session. Local dev: the WS server dies and the
viewer-sdk reports `Error: viewer-sdk WS error`. Prod equivalent:
`dioramap-processor` segfaults / aborts; the container restarts; any
in-flight WS connections break. The next run after restart always
succeeds (data is cache-hit again), but after another N runs the same
crash recurs.

## Trigger

A pipeline run where **every step is a cache hit** (no actual algorithm
work). Repro:

1. Run any pipeline once with new inputs → all steps execute + cache.
2. Run the SAME pipeline a second time → all steps cache-hit, runner
   produces `run_complete` with `steps_executed:0, cache_hits:N`.
3. The crash happens during the post-run cleanup that fires immediately
   after that `run_complete` event is logged.

A run with even ONE executed step doesn't crash. The bug is on the
"skip-execute" branch of the runner's per-step bookkeeping.

## What we have so far

### Crash 1 — `double free or corruption (out)` (libc)

First observed under release build (no ASan). Glibc heap-checker
detected a double-free, killed the process. No stack trace beyond the
libc message. Last log line before the abort:

```
{"event":"run_complete","run_id":"r_59","pipeline":"viewer-full",
 "steps_executed":0,"cache_hits":29}
double free or corruption (out)
```

### Crash 2 — `SEGV in sb_put_json_str` (caught by ASan)

After rebuilding with `DIORAMAP_ASAN=ON` (now the dev-up.sh default),
we caught the same bug class with full diagnostics:

```
==14661==ERROR: AddressSanitizer: SEGV on unknown address (pc … T17)
==14661==The signal is caused by a READ memory access.
==14661==Hint: this fault was caused by a dereference of a high value
              address (see register values below).
    #0 sb_put_json_str           v3/core/src/runner/run_journal.c:78
    #1 build_produced_json       v3/core/src/runner/run_journal.c:229
    #2 run_journal_record        v3/core/src/runner/run_journal.c:244
    #3 cb_on_run_complete        v3/core/src/server/server.c:1000
    #4 runner_run                v3/core/src/runner/runner.c:1536
    #5 run_thread_main           v3/core/src/server/server.c:1117

Thread T17 created by T0 here:
    #1 handle_run_message        v3/core/src/server/server.c:1798
    #2 protocol_cb               v3/core/src/server/server.c:2006
    …
```

### Diagnosis (from the ASan trace)

`build_produced_json` at line 229 calls `sb_put_json_str(&s, arts[i].name)`.
Inside that function, a for-loop dereferences each character of the
string at line 78 — and SEGVs because `arts[i].name` is **non-NULL but
pointing at freed/garbage memory** ("dereference of a high value
address" matches the typical pattern of poisoned heap memory ASan paints
with sentinel bytes like `0xbe`).

So `produced[].name` is a **dangling pointer**: someone in the
all-cache-hit completion path is freeing the backing storage for the
artifact-name string before `cb_on_run_complete` fires
`run_journal_record`. The journal then walks the array and dereferences
the freed strings.

The two crashes are two faces of the same lifecycle bug:
- Double-free = the SAME pointer freed twice (race + ordering)
- Use-after-free of the dangling string = same pointer read after free

Both fire from the **runner's post-run cleanup on the cache-hit path**.

### Where the race lives — annotated trace

**Producer side** (worker thread firing `on_artifact_ready`):

```
v3/core/src/runner/runner.c:1294-1307  step_complete_cb (worker thread)
  → cb->on_artifact_ready(cb->ctx, run_id, write[w].name, &h)
        ↓
v3/core/src/server/server.c:861-872    cb_on_artifact_ready
  → session_journal_acc_append(s, name, hex)      ← UNGUARDED
        ↓
v3/core/src/server/server.c:663-687    session_journal_acc_append
   if (n == cap) {
     names = realloc(names, ...);                  ← race window 1
     hashes = realloc(hashes, ...);
     cap *= 2;
   }
   names[n]  = strdup(name);                       ← race window 2
   hashes[n] = strdup(hex);                        ← race window 3
   n++;                                            ← race window 4
```

**Consumer side** (also runner thread, AFTER `n_pending == 0`):

```
v3/core/src/runner/runner.c:1536       runner_run final block
  → cb->on_run_complete(...)
        ↓
v3/core/src/server/server.c:953-1000   cb_on_run_complete
   for (i = 0; i < n_arts; i++) {
     arts[i].name     = s->journal_artifact_names[i];   ← reads racy data
     arts[i].hash_hex = s->journal_artifact_hashes[i];
   }
   run_journal_record(... &rec ...)                     ← derefs wild ptr
                                                          → crash
```

The execute path doesn't trip it because steps take milliseconds vs
microseconds for cache hits — workers serialize naturally on real work
and `on_artifact_ready` calls space out enough to avoid the race.

### Three crash flavours from the same race

| Race outcome | Symptom |
|---|---|
| Two threads both `realloc()` at `n==cap` | One alloc leaks; libc detects later via guard pages → `double free or corruption (out)` (the original libc-only crash) |
| Thread A realloc'd new buffer; Thread B mid-write to the freed-by-realloc old buffer | UAF — `names[i]` ends up a ghost pointer; `build_produced_json` SEGVs (the ASan crash we caught) |
| Two threads write the same slot index, then `n++` twice | One strdup wins, the other leaks; `n` overshoots → journal walks past last valid entry into `calloc(0)` zeros → null deref or garbage |

All three from the same single missing mutex.

### Fix

Add a dedicated mutex. Don't reuse `s->lock` (the out-queue mutex) —
that would create lock-ordering risk with the dozens of out-queue call
sites.

```c
// Diff vs server.c

@@ struct session_t {
    pthread_mutex_t lock;                      // out-queue
+   pthread_mutex_t journal_acc_lock;          // journal_artifact_{names,hashes,n,cap}
    ...
}

@@ session init (around line 547)
    pthread_mutex_init(&s->lock, NULL);
+   pthread_mutex_init(&s->journal_acc_lock, NULL);

@@ session destroy (around line 638)
    pthread_mutex_destroy(&s->lock);
+   pthread_mutex_destroy(&s->journal_acc_lock);

@@ session_journal_acc_append (line 663)
 static void session_journal_acc_append(session_t* s, const char* name, const char* hash_hex) {
     if (!name || !hash_hex) return;
+    pthread_mutex_lock(&s->journal_acc_lock);
     if (s->journal_artifact_n == s->journal_artifact_cap) { … }
     …
     s->journal_artifact_n++;
+    pthread_mutex_unlock(&s->journal_acc_lock);
 }

@@ session_reset_journal_acc (line 645)
+   pthread_mutex_lock(&s->journal_acc_lock);
    for (i = 0 …) free(names[i]); free(hashes[i]);
    free(names); names = NULL;
    free(hashes); hashes = NULL;
    n = 0; cap = 0;
+   pthread_mutex_unlock(&s->journal_acc_lock);

@@ cb_on_run_complete (line 970-980 area, the read loop)
+   pthread_mutex_lock(&s->journal_acc_lock);
    for (i = 0; i < n_arts; i++) {
        arts[i].name     = s->journal_artifact_names[i];
        arts[i].hash_hex = s->journal_artifact_hashes[i];
    }
+   pthread_mutex_unlock(&s->journal_acc_lock);
    // arts[] now self-contained; safe to release the lock before calling
    // run_journal_record (which only reads from arts, not from s->journal_*).
```

The `cb_on_run_complete` lock acquisition is conservative — the runner
guarantees `n_pending == 0` before firing it, so all
`cb_on_artifact_ready` calls from worker threads have returned. But
adding the lock costs nothing and protects against future race shapes
(e.g. a late-subscribe path that re-uses the accumulator while the
journal write is pending).

### Strdup-safety of `arts[]` after lock release

`arts[i].name`/`hash_hex` point at strings owned by
`s->journal_artifact_names[]`/`hashes[]`. Those won't be freed until
the next `session_reset_journal_acc` (called when the next RUN message
arrives). Between cb_on_run_complete and that next reset, no other
thread mutates the arrays (worker threads have all exited; the run
thread itself is the one in cb_on_run_complete). So holding the lock
only across the read-into-arts loop is sufficient — the journal write
that follows can't race because nothing else writes those entries
until the next RUN.

### Regression test shape

Add `v3/core/tests/runner_cache_hit_repeat_test.c`:

1. Build a small pipeline (e.g. 4 steps, all using cheap deterministic
   work), prime the cache by running once.
2. Loop 100 times: re-issue the same RUN message → expect every run
   to cache-hit cleanly with no crash.
3. Build under ASan + UBSan (the existing dev profile from
   `dev-up.sh`) — any race manifests as a sanitizer report rather than
   a flaky pass/fail.
4. Mirror the existing `surround_gating_test.c` style for setup +
   teardown.

## Repro / debug setup

The dev container's `dev-up.sh` already does this — but documenting in
case someone bypasses it:

1. **CMake config** (in `v3/`):
   ```bash
   cmake -S core -B build-dev -G Ninja \
     -DCMAKE_BUILD_TYPE=Debug \
     -DDIORAMAP_OBFUSCATE=OFF \
     -DDIORAMAP_ASAN=ON \
     -DDIORAMAP_BUILD_TESTS=OFF
   cmake --build build-dev --target dioramap_processor
   ```
   The dev-up `ensure_processor_built` enforces these flags via a
   `CMakeCache.txt` check; reconfigures + rebuilds whenever any drift.

2. **Runtime env**:
   ```bash
   ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:disable_coredump=0:detect_leaks=0:print_stacktrace=1"
   UBSAN_OPTIONS="halt_on_error=1:abort_on_error=1:print_stacktrace=1"
   ulimit -c unlimited
   ```
   `dev-up.sh start_processor` sets these automatically.

3. **Repro recipe**:
   - Start the processor under dev-up.
   - Open `http://127.0.0.1:5183/` (or `http://127.0.0.1:3099/3d.html`)
     and let the viewer-sdk render the default location once. Wait for
     `run_complete`.
   - Refresh / re-open the page. The same pipeline runs again with all
     cache hits → crash on the `run_complete` cleanup.
   - The ASan stack trace lands in `/tmp/dioramap-processor.log`.

4. **Post-mortem** (if the in-process trace isn't enough):
   ```bash
   ls -la core* /tmp/core* 2>/dev/null
   gdb build-dev/dioramap-processor <core>
   (gdb) bt full
   ```
   On WSL2 the kernel pipes cores to `wsl-capture-crash`; check the
   host filesystem if the in-tree `core.<pid>` file isn't there.

## Estimate

- 1–2 hours focused C work to read the runner state machine, identify
  who owns what string when the cache-hit path runs, and fix.
- Add a regression test under `v3/core/tests/` that runs a small
  pipeline 100 times in a loop under ASan — catches both this bug AND
  any future ownership regression in the same area.
- Suggest: `runner_cache_hit_repeat_test.c` mirroring the existing
  `surround_gating_test.c` style.

## Why we haven't seen this in prod

- Before D6 ship, the prod processor was running v0.1.0 / v0.0.0 with
  no usage and no warmed cache.
- After D6 (v0.2.0 ship 2026-05-01), cache is mostly cold for new
  customers — every run executes most steps.
- The bug fires when a cached pipeline is RE-RUN with identical inputs.
  This becomes common when many users hit the same locations
  (popular cities, repeated agri-map clicks).
- Risk window: when agri-map gets steady usage on a small set of
  popular tiles, the same input_hash will cache-hit repeatedly →
  crash → container restart → orphaned WS connections → poor UX.

## Workaround until fixed

Container restart-on-failure already covers it (the systemd unit /
docker run has `--restart=always`). Visible impact is one or two
broken WS sessions per crash; users refresh and reconnect to the
freshly-restarted processor. Not catastrophic but visibly poor.

If the crash rate becomes high before the fix lands, a short-term
mitigation could be: in `cb_on_run_complete`, `strdup` every string in
`rec->produced[]` before passing to `run_journal_record`. Frees the
copies after the SQL insert. Costs ~20 small allocations per run in
exchange for not depending on the runner's lifetime contract. Easy
30-line patch + a commented `// TODO: remove once runner ownership
fixed (BUG-cache-hit-runner-cleanup.md)`.

---

## Resolution (2026-05-04)

The fix was bigger than the originally-proposed patch.  In tracing
the bug we found this wasn't just a missing lock — it was the
*structural* problem of `session_t` being a struct-bag of fields
where every callsite had to remember the locking convention.  The
convention had drifted because the journal-acc fields were added
weeks after the original session_t design, and nobody updated the
"every callback that touches session_t fields takes s->lock" rule
to cover them.  The same drift surface had already produced two
other latent races caught by the follow-up audit (cp_verify_key
cache-slot reuse, parse_produced_json realloc UAF).

### What shipped

**Encapsulation refactor** (commit `d91f0ab`, 2026-05-04).
`session_t`, `session_mgr_t`, `batch_ctx_t` became opaque types
defined in dedicated `.c` files.  All field access goes through
accessor/mutator functions that take the internal mutex; outside
callers never see the lock.  The compiler now enforces what was
previously convention-only.  See `core/include/v3/session.h`,
`core/src/server/{session,session_mgr}.c`, `core/src/batch/batch_ctx.c`.

**clang `-Wthread-safety` annotations** wired through
`core/include/v3/thread_safety.h` and applied to all three opaque
types.  Auto-enabled on any clang build via CMakeLists conditional;
catches forgot-to-unlock, double-acquire, lock-order inversions at
compile time.

**Audit-driven fixes** to the same code family:
- `parse_produced_json` realloc-per-slot pattern (UAF on partial OOM)
- `cp_verify_key` re-look-up cache slot after re-acquire (slot reuse
  across the unlocked HTTP-roundtrip window)
- `store_put` ownership-contract dead-code removed + commented

**CONCURRENCY.md** updates:
- §3 invariants 8–10 (per-session callback state mutex-guarded;
  lock-drop-relock contract; encapsulate shared mutable state).
- §9 anti-patterns 11–14 (bare-bool reads; cache-slot pointer reuse;
  no lock-handoff; no exposed protected fields).
- §12.3 global lock acquisition order documented.
- §12.6 marks "Dedicated session_mgr_t file" DONE.

### Validation

- **ASan + UBSan** locally: 10 integration scenarios pass
  (`tests/integration/scenarios/`), including
  `serve_all_cache_hit_burst` which generalises this bug's repro
  (100 identical RUNs on one socket → all-cache-hit after the first).
- **Helgrind** on the same scenario locally: zero race warnings.
- **TSan + UBSan** on prod (dioramap.com, real Linux 6.8 — TSan won't
  run on the WSL2 dev box): all 10 scenarios pass with zero TSan
  warnings.  See `docs/quality/README.md` §4 for the bundle
  build / scp / cleanup procedure used.
- **clang `-Wthread-safety -Werror`**: clean across the three
  opaque-type files.

### Regression net

`tests/integration/scenarios/serve_all_cache_hit_burst.sh`
generalises the original repro (100 identical RUNs through one WS
socket; default tunable via `IT_BURST_N`).  Lives in the per-PR
matrix tier; runs under every variant.

`tests/journal_acc_race_smoke.sh` — earlier standalone smoke (kept
as a quick-test bypass when the orchestrator isn't appropriate).

`tests/fuzz/fuzz_run_journal_parse.c` — libFuzzer target for
`parse_produced_json` (the function the audit found a UAF in).
~5M random inputs through it during initial validation, zero
crashes.  Cron-runnable for hours/days.

### Why the simple "add a dedicated journal_acc_lock" fix wasn't taken

The proposed patch in the "Fix" section above adds a *new* mutex
specifically for the journal accumulator, separate from `s->lock`.
That would have closed *this* bug in isolation.  We rejected it for
three reasons:

1. **It doesn't address the structural cause.**  Adding more
   per-field mutexes increases the convention surface area; the
   *next* developer adding a session_t field would face the same
   "which mutex guards this?" question and could pick wrong.
2. **Same-callback-multiple-locks risk.**  `cb_on_artifact_ready`
   would have had to take both `s->lock` (for `session_pin_artifact`)
   *and* `journal_acc_lock` (for the new path), introducing a lock
   ordering question for zero benefit.
3. **The audit had found two more bugs in the same family.**
   Patching them one-by-one was bounded work; a structural fix was
   bounded work that *also* prevented the next instance.

The encapsulation refactor was ~3000 lines of code change but
demonstrably won the cost-benefit: no new bugs since, every audit-
flagged issue closed in one pass, all 10 integration scenarios pass
on every variant we can run.
