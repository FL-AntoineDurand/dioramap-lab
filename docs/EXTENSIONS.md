# EXTENSIONS.md — running Dioramap behind your own accounts

The lab build trusts whoever can reach the port (optionally gated by
`--token`). A hosted service usually needs more: its own API keys and
allowed origins, a public-only view for customer sites, usage metering, and
add-ons in the embedded viewer. None of that belongs in the engine, so the
engine exposes two small seams instead:

| Side | Seam | Selected |
|---|---|---|
| Processor (C) | `v3_extension_t` in [`core/include/v3/extension.h`](../core/include/v3/extension.h) | at link time: `-DDIORAMAP_EXTENSION_DIR=<dir>` |
| Viewer SDK (JS) | `init({ auth, plugins, pluginConfig })` | at bundle time: a wrapper entry that imports the SDK |

The service code lives in its own repository and builds this one as a
dependency (e.g. a git submodule pinned to a release). Nothing here is
patched.

## 1. Processor extension

Configure with a directory whose `CMakeLists.txt` defines a static library
target named `dioramap_extension` that implements `v3_extension()`:

```cmake
# my-service/processor-ext/CMakeLists.txt
add_library(dioramap_extension STATIC my_auth.c my_usage.c)
target_link_libraries(dioramap_extension PUBLIC dioramap_log jansson)
target_include_directories(dioramap_extension PUBLIC ${CMAKE_SOURCE_DIR}/include)
```

```bash
cmake -S dioramap-lab/core -B build -DDIORAMAP_EXTENSION_DIR=$PWD/processor-ext
```

Without the option the build links `src/extension/none.c` (no extension).

```c
#include "v3/extension.h"

static const v3_extension_t k_ext = {
    .name             = "my-service",
    .credential_param = "key",            // client connects with ?key=…
    .init             = my_init,          // read env, open clients; NULL return = refuse to start
    .authenticate     = my_authenticate,  // per connection
    .on_run_finished  = my_on_run,        // per finished run, success or failure
    .shutdown         = my_shutdown,      // flush
};
const v3_extension_t* v3_extension(void) { return &k_ext; }
```

**authenticate** receives the credential (`?<credential_param>=`) and the
`Origin` header, and fills `v3_ext_auth_result_t`:

- refuse: `ok = false`, optional `error_code` (default `AUTH_FAILED`) and
  `reason`; the client gets `ERROR {code, msg}` and the connection closes.
  Nothing it sends is dispatched.
- accept: `ok = true`, a `principal` (e.g. the API key) and `account` kept on
  the session and returned in run reports, an `access` level, and an
  optional `tier_label` echoed as `SESSION_STARTED.tier`.

`access = V3_ACCESS_PUBLIC_ONLY` limits the connection to artifacts whose
writer is declared `STEP_VIS_PUBLIC`: the others are left out of
`CATALOG_RESPONSE`, and `RUN` / `SUBSCRIBE` naming one gets
`ERROR FORBIDDEN_ARTIFACT`. This is how a customer site sees the finished
scene but not the debug intermediates the lab UI shows.

If `--token` is also set, the token check runs first.

**on_run_finished** gets a `v3_run_report_t` after the terminal frame is
queued: principal, account, `event` (`scene_load` / `late_subscribe`),
run id, pipeline, input hash (never the raw input), lat/lon when present,
duration, step counts, and for failures the [error code and
message](ERRORS.md). It runs on the run's worker thread: buffer and flush
in the background.

**Threading.** `authenticate` runs on the WebSocket service thread while the
connection is established; a slow call stalls other connections, so cache
verdicts.

The test double [`core/tests/fake_extension.c`](../core/tests/fake_extension.c)
and [`serve_extension.py`](../core/tests/serve_extension.py) exercise every
path (refusals, public-only access, success and failure reports, init
refusal, shutdown) and are a working example.

## 2. Viewer SDK

```js
import { init } from '@dioramap/viewer-sdk';   // or the UMD global DioramapViewer

init({
  container: '#scene', lat, lon,
  backendUrl: 'wss://example.com/ws',
  auth: { param: 'key', value: apiKey },        // appended as ?key=…
  plugins: [feedbackPlugin],
  pluginConfig: { endpoint: 'https://example.com/api/feedback' },
});
```

`auth` is for a processor extension that reads another query parameter;
`token: '…'` remains the shorthand for the processor's own `--token`.

A **plugin** is a function called once when the viewer mounts:

```js
function feedbackPlugin({ container, controlsPanel, config, locale }) {
  // build DOM inside `container`
  return {
    onRunStarted(detail) {},   // RUN_STARTED frame
    onRunComplete() {},
    onRunFailed(err) {},       // DioramapError (docs/ERRORS.md)
    dispose() {},
  };
}
```

All hooks are optional. A plugin that throws is logged and skipped; it never
breaks the viewer.

A hosted service typically ships its own bundle: a small entry file that
imports `init` from this SDK, fills in `backendUrl`, `auth` and `plugins`
from its embed parameters, and is built with Vite like
`apps/viewer-sdk/vite.config.js`.
