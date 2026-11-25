# Protocol Subsystem Plan

*Companion to [UNIFIED-ARCHITECTURE.md](../UNIFIED-ARCHITECTURE.md). WS + HTTP + batch mode + binary artifact codec.*

## Scope

- **Binary artifact layout codec** (§5 of arch doc): encoder, decoder, validation, round-trip.
- **WebSocket transport**: libwebsockets integration, session token issuance, RUN / SUBSCRIBE / CANCEL / RECONNECT handling, ARTIFACT frame delivery.
- **HTTP endpoints**: static file serving, catalog endpoint, schema endpoint, healthz.
- **Batch mode CLI**: args/stdin parsing, output-directory writer, manifest.json generation.

All three entry modes (serve/batch/cli) share the binary codec.

## Key decisions

1. **One wire format, three transports.** Cache files, WS binary frames, and batch-mode output files all use the §5 layout byte-for-byte. Codec is written once.
2. **Text WS frames for JSON control messages, binary WS frames for artifact bytes.** Standard WS fragmentation handles large artifacts transparently.
3. **HTTP via libwebsockets**, not a second HTTP library. libwebsockets serves HTTP on the same port as WS.
4. **Batch mode is first-class.** Shares the entire runner, not a stripped-down variant. Only the delivery-channel callbacks differ.
5. **Clean break from legacy viewer protocol.** No backcompat, no version negotiation.

## Data structures

### Artifact header (packed C struct)

```c
typedef struct __attribute__((packed)) artifact_header {
    char     magic[4];           // "DMAP"
    uint32_t version;            // layout version (1 initially)
    uint32_t artifact_kind;      // enum (see §5)
    uint64_t count_n;            // row count
    uint32_t n_columns;
    uint32_t flags;              // reserved
    uint64_t metadata_offset;
    uint64_t metadata_len;
    uint8_t  reserved[128 - /*current fields*/];
} artifact_header_t;

typedef struct __attribute__((packed)) column_desc {
    uint32_t name_hash;
    uint8_t  dtype;
    uint8_t  stride;
    uint16_t flags;
    uint64_t offset;
    uint64_t length;
    uint8_t  reserved[8];
} column_desc_t;
```

`static_assert` sizes match spec (128 B, 32 B).

### Codec API

```c
typedef struct artifact_builder artifact_builder_t;

artifact_builder_t* artifact_build_begin(uint32_t kind, uint64_t count_n);
void artifact_build_add_column(artifact_builder_t*, const char* name, dtype_t, const void* data, size_t len);
void artifact_build_set_metadata_json(artifact_builder_t*, const char* json, size_t len);
result_t artifact_build_finalize(artifact_builder_t*, store_write_t* out);  // or to buffer for WS
void artifact_build_free(artifact_builder_t*);

// Reader
typedef struct artifact_view {
    const artifact_header_t* header;
    const column_desc_t* columns;
    const uint8_t* base;
    size_t total_len;
} artifact_view_t;

result_t artifact_view_open(const uint8_t* bytes, size_t len, artifact_view_t* out);
const void* artifact_view_column(const artifact_view_t*, const char* name);
```

Column lookup is by name hash for speed; returns a typed pointer into the bytes.

### WS message types

Text frames (JSON, via yyjson):

```
Client → server:
  CATALOG       { }
  RUN           { pipeline, input, subscribe: [...], params, session_token? }
  SUBSCRIBE     { run_id, artifact }
  CANCEL        { run_id }
  RECONNECT     { session_token }

Server → client:
  SESSION_STARTED { session_token, tier, processor_version }
  CATALOG_RESPONSE { artifacts: [...] }
  RUN_STARTED    { run_id, step_plan }
  STEP_STARTED   { run_id, step }
  PROGRESS       { run_id, step, phase, pct }
  ARTIFACT_HEADER { run_id, artifact, kind, size, hash }
  // followed by one binary WS frame of `size` bytes in §5 layout
  STEP_COMPLETE  { run_id, step, writes }
  RUN_COMPLETE   { run_id, summary }
  RUN_FAILED     { run_id, code, msg, step?, step_token?, detail?, pipeline?, input?,
                   processor_version, duration_us, late_subscribe? }   // docs/ERRORS.md
  SUBSCRIBE_DENIED { artifact, reason }
  ERROR          { run_id?, step?, code?, msg }   // step failures: msg = "[CODE] message"
```

### Session token

Opaque random string, 32 bytes base64url-encoded → 43 chars. Generated via `getrandom()`. Server-side lookup table; no signing (Q6 resolution).

## Status (2026-04-28)

| Task | State | Notes |
|---|---|---|
| P1 Artifact codec writer | ✅ done (A4, `22aae7f`) | `artifact_build_*` in `core/src/artifact/`. |
| P2 Artifact codec reader | ✅ done (A4, `22aae7f`) | `artifact_view_open` + round-trip tests in `artifact_test.c`. |
| P3 libwebsockets WS stub | ✅ done (A7.1, `0b18787`) | See item below. |
| P4 JSON parser/serializer | ✅ done (A7, `0b18787`) | jansson; PROGRESS / SUBSCRIBE_DENIED still not emitted. |
| P5 Session management integration | ✅ done (A7 `0b18787`; encapsulated 2026-05-04 `d91f0ab`) | session_token issue + RECONNECT + grace + janitor. **2026-05-04**: session_t became opaque (`v3/session.h`); sessions[] table + janitor moved to session_mgr.c (`v3/session_mgr.h`). server.c is now pure lws-glue (~1100 lines, was ~2270). Drift surface that caused BUG-cache-hit-runner-cleanup.md is closed structurally; clang `-Wthread-safety` enforces lock pairing at compile time. See PLAN-RUNNER T9 for the full story. |
| P6 RUN handler | ✅ done (A7.1, `0b18787`) | Detached run thread, RUN_BUSY rejection on overlap. |
| P7 SUBSCRIBE / CANCEL / visibility filter | ✅ done (A7 + D6.5) | SUBSCRIBE + CANCEL shipped in A7; visibility filter shipped in D6.5 — `tier_allows_visibility` helper gates three sites in `server.c`: (1) RUN's `subscribe[]` validation rejects with `FORBIDDEN_ARTIFACT` before any runner thread spawns, (2) `handle_subscribe_message` (late-SUBSCRIBE) reuses the same gate, (3) `handle_catalog_message` strips debug-only writes from the payload sent to public-tier sessions so the algorithm graph doesn't leak through CATALOG_RESPONSE. Coverage: `core/tests/serve_visibility.py` ctest exercises debug-tier (full graph + RUN succeeds) and public-tier (filtered catalog + RUN forbidden). |
| P8 HTTP endpoints (catalog/schema/healthz/static) | ⚠️ partial — `/healthz` only (processor v0.2.2 `e6a742f`) | Shipped 2026-05-06 in response to a customer-facing 502 caused by UptimeRobot probing `/ws` (plain HTTP GET to a WS-only endpoint hung 15s waiting for an upgrade that never came → nginx 502). `LWS_CALLBACK_HTTP` handler in `protocol_cb` returns: `/healthz` → 200 OK + `"ok\n"`; `/ws` (plain GET) → 426 Upgrade Required + `"use WebSocket\n"`; everything else → 404. ~40 LOC. nginx routes `https://app.dioramap.com/healthz` → `127.0.0.1:7801/healthz` (note: `location = /healthz` block, no trailing slash on `proxy_pass`, in contrast to `/ws` which strips). Still deferred: `/api/catalog` (CATALOG over WS already serves the lab UI; no HTTP consumer), `/api/schema/*` (no consumer), static file serving (nginx in front handles this in prod). |
| P9 Batch mode CLI | ✅ done (A8, `707d616`) | `batch` subcommand + manifest.json + ctests `batch_demo_chain`, `batch_gpu_smoke`. |
| P10 Streaming artifact support | ✅ done (B9, `bd10425`) | `STREAM_CHUNK` + dual-delivery; `serve_stream` ctest. |
| P11 Error propagation | ✅ done | `cb_on_error` fires `ERROR` frame; runner propagates step-fail / dag-fail / cancel through callbacks. |
| P12 Cancellation + disconnect handling | ✅ done (A7, `0b18787`) | `runner_cancel` from WS CANCEL; mid-run disconnect triggers `runner_cancel` via session teardown. |

## Implementation tasks (ordered)

### P1. Artifact codec — writer ✅ done (A4, commit `22aae7f`)
- `artifact_build_*` functions building in-memory.
- Finalize flushes header + columns + metadata to a `store_write_t` or a buffer.
- Unit tests: various kinds, zero-column, large-column, metadata-only.

### P2. Artifact codec — reader ✅ done (A4, commit `22aae7f`)
- `artifact_view_open` validates header, maps columns.
- `artifact_view_column` resolves by name hash.
- Round-trip test: write then read, compare semantics.

### P3. libwebsockets integration — WS server stub ✅ (A7.1)
- Accept connections: done in `core/src/server/server.c`.
- Parse text frames (jansson): done.
- Event loop runs on the calling thread (main invokes `server_run`).
- Cross-thread wakeup pattern: `lws_cancel_service()` from runner callbacks;
  service thread arms writable per-session on `LWS_CALLBACK_EVENT_WAIT_CANCELLED`.

### P4. JSON message parser/serializer ✅ (A7)
- Using **jansson** (not yyjson) — same lib as the pipeline loader, one less dep.
- Inbound: `RUN`, `CATALOG`, `SUBSCRIBE`, `CANCEL`, `RECONNECT`.
- Outbound: `SESSION_STARTED`, `RECONNECTED`, `CATALOG_RESPONSE`, `RUN_STARTED`,
  `STEP_STARTED`, `STEP_COMPLETE`, `ARTIFACT_HEADER` (+ binary frame),
  `RUN_COMPLETE`, `SUBSCRIBE_ACK`, `ERROR`.
- Not yet emitted: `PROGRESS` (reserved for long-running step percentage
  updates), `SUBSCRIBE_DENIED` (depends on tier enforcement in A10).

### P5. Session management integration ✅ (A7)
- session_token issued on connect (`SESSION_STARTED`).
- `RECONNECT { session_token }` reattaches; returns `RECONNECTED` or
  `ERROR { code: "UNKNOWN_TOKEN" }` if the token is unknown / still attached.
- Grace period: default 600s; a janitor thread sweeps expired sessions.
- Disconnected sessions keep store pins so `SUBSCRIBE` after reconnect serves
  from cache without recompute.

### P6. RUN handler ✅ (A7.1)
- `handle_run_message()` parses RUN → spawns a detached run thread that calls
  `runner_run()`.
- Runner callbacks enqueue JSON + binary artifact frames on the originating
  session's out-queue; service thread drains via `LWS_CALLBACK_SERVER_WRITEABLE`.
- Second RUN on a session with an active run returns `ERROR { code: "RUN_BUSY" }`.

### P7. SUBSCRIBE / CANCEL / visibility filter ⚠️ partial (A7)
- SUBSCRIBE: ✅ session remembers `last_pipeline`/`last_input`; new run runs
  with `subscribe=[new_artifact]`. Terminator: `SUBSCRIBE_ACK`.
- CANCEL: ✅ `runner_cancel()` flips per-run cancel flag; client receives
  `ERROR { msg: "cancelled" }`. Also wired to disconnect-mid-run.
- Visibility filter: ⚠️ catalog entries carry `visibility: public|debug` per
  step metadata, but the server does not yet filter by connection tier —
  all clients currently see `debug`. Requires auth / control-plane (A10).

### P8. HTTP endpoints via libwebsockets ❌ not started
*Deferred per the A7 audit (see MAIN-IMPL-PLAN A7 row). No `LWS_CALLBACK_HTTP*` handler; nginx in front handles static serving in dev/prod for now. Lab-ui's dev-up health probe uses TCP-open.*

- `GET /` → static file (index.html for lab UI).
- `GET /dist/*` → static JS/CSS.
- `GET /api/catalog` → JSON response (same data as WS CATALOG).
- `GET /api/schema/<kind>` → schema metadata.
- `GET /healthz` → 200.

### P9. Batch mode CLI ✅ (A8)
- `dioramap-processor batch` subcommand (A8). Writes `manifest.json` +
  `<name>.bin` per subscribed artifact.
- Shares the runner and callback interface with serve mode.
- Covered by `batch_demo_chain` and `batch_gpu_smoke` ctests.

### P10. Streaming artifact support ✅ (B9)
- Message `STREAM_CHUNK { t, run_id, artifact, seq, final, size, hash? }` paired with one binary WS frame of `size` bytes per chunk. `seq` is zero-based per artifact. `hash` is `null` on non-final chunks; on `final: true` it carries the finalised artifact's content hash — same algorithm as every other `hash_t` on the wire (xxh3-128 zero-padded into 32 bytes, 64-hex formatted; see `core/src/hash/hash.c`). Schema finalised in UNIFIED-ARCHITECTURE.md §8.1.
- **Dual delivery** (see PLAN-VIEWER-PORT §7.3): after `STREAM_CHUNK final:true`, the server *also* emits a standard `ARTIFACT_HEADER` + binary frame carrying the full concatenated bytes. Streaming-aware clients ignore the trailing header; legacy clients (late subscribers, batch-mode callbacks, simple consumers) receive the artifact via the standard path with no special-casing.
- The runner atomically commits the complete artifact to the store when `final=true` arrives — partial chunks leave no residue on cancel or mid-stream error. Downstream steps block on `final=true`; streaming is a transport concern, not a DAG concern.
- Content-hashing the final concatenated bytes happens when the stream completes (the runner's in-process accumulator sees the whole buffer at `final=true` — see PLAN-RUNNER T14); intermediate chunks are not separately addressable. Second identical run is a full cache hit: step `exec` not called, no chunks fire; the cached artifact is re-delivered via the regular `ARTIFACT_HEADER` path.
- Test (`core/tests/serve_stream.py`): `stream-demo` step emits 5 chunks, client reassembles bytes and validates the deterministic payload pattern. Protocol-level hash invariant: the final chunk's `hash` must equal the trailing `ARTIFACT_HEADER`'s `hash` — both are produced by the same runner write from the concatenated accumulator, so equality proves the streamed bytes finalised to the same content hash the store committed. (We don't recompute xxh3-128 client-side — no Python stdlib equivalent.) First-chunk-within-500ms budget enforced (measured ~0.2 ms RUN_STARTED → first chunk on the bench).

### P11. Error propagation ✅ done
- Callback `on_error` triggers `ERROR` message.
- Runtime crash in a step → clean error propagation, not process death.
- Test: forced step failure, verify ERROR reaches client.

### P12. Cancellation + disconnect handling ✅ done (A7, commit `0b18787`)
- WS close mid-run → treat as cancellation; runner_cancel.
- Test: client closes, verify in-flight step aborts cleanly.
- See [CONCURRENCY.md §12.5](../CONCURRENCY.md) for the cancel invariant + lock order (`runner::active_lock → rs->lock`) and the documented interaction with worker-thread callbacks.  Integration scenarios `serve_cancel_during_run` + `serve_disconnect_during_run` exercise both paths.

## Dependencies

- **Before**: runtime primitives, logging, store (for reading artifacts to send), runner (provides callbacks).
- **After**: nothing; protocol is the outermost layer.

## Definition of done

- Smoke pipeline runs end-to-end via WS with artifact delivery verified on client side.
- Same pipeline runs in batch mode with identical outputs.
- RECONNECT within grace delivers cached artifacts correctly.
- ERROR paths tested: bad RUN, bad SUBSCRIBE, forced step failure, mid-run disconnect.
- All message schema validated by a round-trip test suite.

## Open questions

- **Backpressure.** If client can't consume artifact frames as fast as produced, what happens? libwebsockets buffers up to a limit then drops; we need: server-side flow control + client ACKs, or disconnect-on-backup. Phase A: accept the libwebsockets default (disconnect on buffer overflow), revisit if it bites.
- **Multiple concurrent RUNs per session.** Currently rejected. Will we ever need it? Lab UI probably not; viewer probably not. Leave rejected; reopen if needed.
- **Static file caching headers.** Production SDK may be CDN-hosted; HTTP caching is less critical. Dev server: `Cache-Control: no-cache`.

## Risk / unknowns

- **libwebsockets API learning curve.** Event-driven state machine. Plan: write a minimal echo server in a prototype sandbox before P3 to familiarize.
- **Binary frame size limits.** libwebsockets has configurable max frame / message size. Large artifacts (hundreds of MB) need fragmented messages. Verify library limits early.
- **TLS.** Not in scope for the processor — terminated upstream at nginx (D14). libwebsockets is therefore built without OpenSSL/mbedTLS for inbound. Outbound TLS (fetchers, control plane) is libcurl-owned and lives in those subsystems.
