// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// MapLibre ≥ 5 loads its web worker as a separate module file next to
// maplibre-gl.mjs. This picker ships as ONE script (UMD / ES) that embedders
// load cross-origin, where a separate worker file cannot work (module
// workers must be same-origin) — tiles render but GeoJSON layers such as
// the radius circle never appear. So the worker source (self-contained,
// no imports) is embedded at build time and handed to MapLibre as a
// same-origin blob: URL, which is what MapLibre 4 did internally. CSP: the
// embedding page must allow `worker-src blob:` (unchanged from MapLibre 4).
import * as maplibregl from 'maplibre-gl';
import workerSource from 'maplibre-gl/dist/maplibre-gl-worker.mjs?raw';

let installed = false;

export function installMaplibreWorker() {
  if (installed) return;
  installed = true;
  const url = URL.createObjectURL(new Blob([workerSource], { type: 'text/javascript' }));
  maplibregl.setWorkerUrl(url);
}
