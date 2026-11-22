# Logging Subsystem Plan

*Companion to [UNIFIED-ARCHITECTURE.md](../UNIFIED-ARCHITECTURE.md) §17. Structured, auditable, machine-parseable logging.*

## Scope

- **Structured JSON-lines** event emission.
- **Subsystem routing** via a fixed enum of known subsystems.
- **Stable event kinds** per subsystem (grep/analysis-friendly).
- **Levels**: debug / info / warn / error / fatal.
- **Output sinks**: stdout (default), optional file with rotation.
- **Backpressure**: never block callers; drop on queue overflow with accounting.
- **Runtime level control**: signal-driven debug toggle with auto-revert.

## Key decisions

1. **JSON lines as the primary format.** Ingestion into ELK/Loki/jq all work natively. No non-JSON log lines in production.
2. **Field names are short and stable.** `t`, `level`, `subsystem`, `event`, `run_id`, `session`. Not `timestamp_iso_utc`.
3. **Lock-free fast path.** Log emission from hot paths must not block. Ring buffer + dedicated drain thread.
4. **No string formatting in the caller.** Caller passes structured key-value pairs; serialization happens in the drain thread. Keeps hot-path cost minimal.
5. **Event kinds are an enum**, not free-form strings. Each subsystem owns its enum; adding a new kind is a code change, not a runtime one.

## Data structures

### Log event struct

```c
typedef enum subsystem {
    SUB_RUNNER,
    SUB_STORE,
    SUB_SESSION,
    SUB_TRANSPORT,
    SUB_FETCH,
    SUB_CONTROLPLANE,
    SUB_CUDA,
    SUB_LOG,
    SUB_STEP,           // generic per-step events
    SUB__COUNT,
} subsystem_t;

typedef enum log_level {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL,
} log_level_t;

typedef struct log_event {
    uint64_t ts_mono_ns;
    uint64_t ts_wall_us;
    subsystem_t subsystem;
    log_level_t level;
    uint32_t event_kind;         // subsystem-specific enum
    uint32_t n_fields;
    log_field_t fields[MAX_FIELDS];  // inline small array; bigger → heap alloc
} log_event_t;

typedef struct log_field {
    const char* key;             // must be compile-time-constant or equivalent
    log_value_t value;           // tagged union: int, double, str, hash, bool
} log_field_t;
```

### API

```c
// Emit an event. Returns immediately (never blocks).
#define LOG_EVENT(SUBSYS, LEVEL, EVENT_KIND, ...) \
    log_emit_(SUBSYS, LEVEL, EVENT_KIND, \
              (log_field_t[]){__VA_ARGS__}, \
              sizeof((log_field_t[]){__VA_ARGS__}) / sizeof(log_field_t))

// Helper builders for common field types
#define LF_INT(K, V)    ((log_field_t){.key=K, .value={.type=LV_INT, .i=V}})
#define LF_STR(K, V)    ((log_field_t){.key=K, .value={.type=LV_STR, .s=V}})
#define LF_HASH(K, V)   ((log_field_t){.key=K, .value={.type=LV_HASH, .h=V}})
#define LF_RUN_ID(V)    LF_STR("run_id", V)
#define LF_SESSION(V)   LF_STR("session", V)

// Usage:
LOG_EVENT(SUB_RUNNER, LOG_INFO, EV_STEP_COMPLETE,
          LF_RUN_ID(run->id),
          LF_STR("step", step->name),
          LF_INT("duration_ms", elapsed_ms),
          LF_INT("cache_hit", 0));
```

### Event-kind enums per subsystem

Each subsystem defines its own enum in a header that both the subsystem and the drain thread include:

```c
typedef enum runner_event_kind {
    EV_RUN_STARTED,
    EV_RUN_COMPLETE,
    EV_RUN_FAILED,
    EV_RUN_CANCELLED,
    EV_STEP_STARTED,
    EV_STEP_COMPLETE,
    EV_STEP_FAILED,
    EV_DAG_WALK,
    // ...
} runner_event_kind_t;
```

Drain thread uses a compile-time table `[subsystem][kind] → "event_name_string"` to emit readable names in JSON.

### Ring buffer

Lock-free SPMC (single producer-consumer pattern is too restrictive; use MPSC ring with CAS-based tail). Bounded capacity (default 16k events). On overflow: increment a `log_dropped_count` atomic; drain thread periodically emits a `log_dropped` event with the count.

## Status (2026-04-28)

| Task | State | Notes |
|---|---|---|
| L1 Core API + stub sink | ✅ done (A2, `017485b`) | LOG_EVENT macro + field helpers; stdout JSON-lines sink. |
| L2 Ring buffer | ✅ done (L2/L3, `db28085`) | MPSC bounded ring, mutex+condvar (not lock-free — simpler, sufficient). |
| L3 Drain thread + stdout sink | ✅ done (L2/L3, `db28085`) | Dedicated pthread consumer, batches 16 slots per wake. |
| L4 Event-kind tables | ✅ done | Subsystem + event enums + lookup tables per-subsystem. |
| L5 File sink with rotation | ❌ not started | Current: stdout only. |
| L6 Level filtering | ✅ done | Runtime: `DIORAMAP_LOG_LEVEL` env var. Compile-time stripping not implemented. |
| L7 Runtime debug toggle | ❌ not started | No SIGUSR1 handler yet. |
| L8 Backpressure + drop accounting | ✅ done | `log.dropped` synthetic event emitted when counter advances. |
| L9 Log assertion harness for tests | ✅ done (A2, `017485b`) | log_sink_swap_for_testing + log_flush. |
| L10 Correlation integration | ✅ done | run_id / step / thread_id fields present in runner + store events. |

## Implementation tasks (ordered)

### L1. Core API + stub sink
- `LOG_EVENT` macro + `log_emit_` function.
- Inline serialization to `stderr` for now (replaced in L3).
- Unit test: event fires, fields present.

### L2. Ring buffer
- MPSC lock-free queue.
- Size configurable.
- Benchmarks to confirm sub-microsecond emit cost.

### L3. Drain thread + stdout sink
- Dedicated thread pops events, serializes to JSON, writes to stdout.
- Non-blocking write path with fflush batching.
- Shutdown drains queue before exit.

### L4. Event-kind tables
- Per-subsystem enum + name table.
- Macro-generated or hand-maintained; choose based on simplicity.
- Unit test: every enum value has a name string.

### L5. File sink with rotation
- Optional `--log-file path` config.
- Size-based rotation: N files of M bytes each.
- Age-based deletion optional.
- Test under disk pressure.

### L6. Level filtering
- Compile-time minimum level (strip debug in release).
- Runtime level per subsystem (override via config).

### L7. Runtime debug toggle
- SIGUSR1 → enable debug for M minutes → auto-revert.
- Alternative: admin HTTP endpoint (if HTTP layer is ready).

### L8. Backpressure + dropped-count accounting
- Producer increments atomic on ring-full.
- Drain thread emits `log_dropped` event when count changes.
- Test under overload.

### L9. Log assertion harness for tests
- Test helper that captures events into an in-memory buffer.
- Assertions: "smoke run emits exactly these events in this order."
- Crucial for golden trace fixtures (§17.6 of arch doc).

### L10. Correlation integration (ongoing)
- Every run/session-scoped callsite includes run_id / session in fields.
- Code review: no anonymous events in these scopes.

## Dependencies

- **Before**: runtime primitives (ring buffer uses atomics + one thread).
- **After**: every other subsystem calls `LOG_EVENT`.

## Definition of done

- Emit cost on hot path: < 500 ns measured (no syscall, no malloc).
- Ring buffer handles 100k events/sec without drops in benchmark.
- Log file rotation works without data loss.
- Test harness captures events; smoke pipeline golden event sequence established.
- Runtime debug toggle works (SIGUSR1 test).
- Drain-thread shutdown is clean (no dropped final events).
- **Integration matrix coverage** (per [docs/quality/README.md](docs/quality/README.md)): the log subsystem is exercised end-to-end by every integration scenario (`tests/integration/scenarios/*.sh` set `DIORAMAP_LOG_LEVEL` then redirect processor stderr; assertions parse the structured `run_complete` log lines via `jq`).  TSan-clean on prod 2026-05-04 across all scenarios — validates concurrent emit + drain-thread interaction under load.  Dedicated log-correctness scenarios (e.g. forced ring-buffer drop, runtime debug toggle while events flow) would land under docs/quality/README.md if a regression appears.

## Open questions

- **Tracing (spans) vs flat events.** We're going with flat events for Phase A. Later: can add span IDs (parent/child) if distributed tracing is needed. Not now.
- **Metrics (counters, gauges) vs events.** Events are sufficient; a downstream tool can aggregate them to metrics. If pressure mounts for live counters, add a Prometheus endpoint separately.
- **Sensitive data.** API keys, IP addresses — what gets logged? Policy: no API keys (log key_id instead, tied to cp_client). IP addresses only on WARN+ events. Document; code-review enforces.

## Risk / unknowns

- **Queue full under runaway event rate.** A misbehaving step could flood the queue. Mitigation: per-subsystem rate limiting on WARN+ events if a kind fires > N/sec, sample. Not Phase A.
- **Clock skew between mono and wall.** mono_ns is monotonic (no NTP adjustments); wall_us can jump. Both recorded; consumers pick.
