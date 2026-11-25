# ERRORS.md — how a failure travels from a step to the user's screen

**Status**: shipped in processor 0.3.0 / viewer-sdk 0.10.0 / location-picker 0.9.0.

The goal: when a scene fails to build, the person looking at it sees a plain
sentence ("No data for this place yet"), and a developer reading the bug report
sees the root cause ("HTTP 400 from data.geopf.fr: Unknown namespace
[IGNF_NUAGES-DE-POINTS-LIDAR-HD]") without having to open the server logs.
Error reports are allowed to be verbose: step names, upstream URLs, HTTP status
and response excerpts are not secrets.

```
 step_fail()            fetch_get() failure
 (the step's reason)    (URL, HTTP status, body excerpt — automatic)
        │                     │
        ▼                     ▼
   thread-local error sink, bound by the runner around each step
        │
        ▼
 runner: combine → step_failed log event + on_error ("[CODE] msg")
                 → on_run_failed (first failure of the run, terminal)
        │
        ▼
 server: ERROR frame (compat) + RUN_FAILED frame (terminal, full detail)
        │
        ▼
 @dioramap/protocol: 'run-failed' event → DioramapError
        │
        ▼
 viewer-sdk: error card (title + hint, technical details, Copy,
             Report on GitHub, Try again) + onError(DioramapError)
```

## 1. Reporting a failure from a step

```c
#include "v3/step.h"

if (nf == 0)
    return step_fail(ctx, V3_ERR_NO_DATA,
                     "no IGN LiDAR HD tile is published at lat=%.5f lon=%.5f yet", lat, lon);

// With structured evidence (a JSON object as text):
return step_fail_detail(ctx, V3_ERR_UPSTREAM_FORMAT, detail_json,
                        "IGN WFS answered with non-JSON content");
```

`step_fail*` always return -1. Write the message for a developer who has
only this line: what was expected, what was found, the key input.

A step that returns a bare `-1` still produces a report: the upstream
failure if `fetch_get` recorded one, otherwise
`INTERNAL: <step> failed without reporting a reason`. Converting a step is
therefore incremental: start with the steps that talk to outside services
or validate user input (done: `canonicalize-location`,
`compute-scene-origin`, `find-copc-tiles`, `fetch-copc-tiles`,
`laz-extract-disc`, every `steps/fetchers/*`).

### Codes

| Code | Meaning | Retry helps? |
|---|---|---|
| `NO_DATA` | The place has no data for this input. A normal outcome (no LiDAR HD tile yet, sea). | no |
| `INVALID_INPUT` | The request is wrong: lat/lon out of range, bad params. | no |
| `UPSTREAM_HTTP` | An external service answered with an HTTP error. | maybe |
| `UPSTREAM_UNREACHABLE` | DNS / TLS / connection failure to an external service. | maybe |
| `UPSTREAM_FORMAT` | The service answered 2xx with unusable content (e.g. WMS ServiceException instead of a JPEG). | maybe |
| `TIMEOUT` | An external service was too slow. | yes |
| `CANCELLED` | Client cancel or disconnect. | — |
| `INTERNAL` | Engine bug or unexpected state. | rarely |

Client-side codes added by `@dioramap/protocol`: `CONNECTION_FAILED` (the
WebSocket never opened), `CONNECTION_LOST` (closed mid-run), `STALLED` (no
frame for 5 min during a run), plus the server's request-level ERROR codes
(`AUTH_REQUIRED`, `AUTH_FAILED`, `RUN_BUSY`, `BAD_RUN`, `UNKNOWN_PIPELINE`, …).

## 2. Upstream evidence (fetch layer)

Every failed `fetch_get` (and so `fetch_and_cache`):

- logs a `fetch_failed` event with URL, status, curl error, attempts and a
  printable body excerpt;
- records upstream evidence in the current step's sink: `detail.upstream =
  {url, host, http_status, fetch_error, curl_error?, attempts, duration_s,
  body_excerpt, body_bytes, code, message}`. For OGC services the exception
  text (`ows:ExceptionText` / `ServiceException`) is pulled into the
  one-line message.

A step that tolerates a fetch failure and succeeds leaves no trace: the
runner clears the sink on success.

## 3. Wire format

`ERROR` keeps its old shape for compatibility; step failures now carry
`msg: "[CODE] message"` instead of the constant `"step failed"`.

`RUN_FAILED` is new and terminal: it replaces `RUN_COMPLETE` (or
`SUBSCRIBE_ACK` for a late subscribe) for a run that did not complete.
Exactly one per failed run; it describes the **first** failure (peers
cancelled after it are not reported).

```json
{ "t": "RUN_FAILED", "run_id": "r_1",
  "code": "UPSTREAM_HTTP",
  "step": "find-copc-tiles", "step_token": "tt_c5e060a1",
  "msg": "IGN LiDAR HD tile index query (WFS IGNF_LIDAR-HD_METADONNEE:metadata) failed: HTTP 400 from data.geopf.fr: Unknown namespace [IGNF_NUAGES-DE-POINTS-LIDAR-HD]",
  "detail": { "step_version": 2, "step_duration_ms": 182.3,
              "upstream": { "url": "https://data.geopf.fr/wfs/ows?…", "http_status": 400,
                            "body_excerpt": "<ows:ExceptionReport …", "attempts": 1, … } },
  "pipeline": "viewer-full", "input": { "lat": 48.11, "lon": -1.68, "radius_m": 75 },
  "processor_version": "0.3.0", "duration_us": 412000 }
```

`step` is the step name. `step_token` is the name the client saw in
`STEP_STARTED`; the two differ only in builds that rename identifiers, where
the processor maps the token back so reports stay readable.

The run is also recorded in the run journal with `success=false`,
`error_kind=<code>`, `error_message=<msg>`, and `SESSION_STARTED` now
carries `processor_version`.

## 4. Clients

`@dioramap/protocol` exports `DioramapError` and builders for each source
(`errorFromRunFailed`, `errorFromServerError`, `errorFromDisconnect`,
`errorFromWatchdog`), `formatReport(err)` (markdown + JSON) and
`issueUrl(err)` (pre-filled GitHub issue on the `run-failure` form of
`FL-AntoineDurand/dioramap-lab`). API keys and access tokens (`key=`, `token=`) are redacted from reports.

**viewer-sdk** (`init({ onError, locale, issueRepo, reportIssues })`):

- the loading card turns into an error card instead of freezing; texts in
  French when the page is French, English otherwise;
- `onError(err)` fires once per failed run with a `DioramapError`
  (`err.code`, `err.step`, `err.detail`, `err.toReport()`);
- against processors < 0.3.0 (no `RUN_FAILED`) it falls back to the
  step's `ERROR` after 1.5 s.

**lab-ui**: `RUN_FAILED` appears in the log panel with its full `detail`,
in the timeline, and marks the failing DAG node.

**location-picker**: address search failures show a message in the
dropdown and call `init({ onError })` with `code: 'GEOCODER_FAILED'`.

## 5. Checking it by hand

```bash
# NO_DATA: a place IGN has not published yet (temp cache, never the real one)
dioramap-processor batch --pipeline pipelines/smoke/viewer-full.json \
  --input-json '{"lat":47.6582,"lon":-2.7603}' --produce copc-tile-urls \
  --output-dir /tmp/out --cache-dir "$(mktemp -d)"

# UPSTREAM_HTTP with the OGC exception text: copy viewer-full.json and set
# "layer": "ORTHOIMAGERY.DOES-NOT-EXIST" in fetch-ortho-rgb's params.
```

Tests: `runner_test` (`test_step_failure_report`), `fetch_test`
(`test_ogc_exception_text`), `serve_full.py` (case 5: `RUN_FAILED` frame,
no `RUN_BUSY` on immediate retry).
