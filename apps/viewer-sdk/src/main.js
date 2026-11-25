// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// main.js — standalone dev-page entry. Boots the SDK against the local
// dioramap-processor; not shipped in the library bundle (the lib entry
// is sdk.js per vite.config.js).
//
// Query string overrides:
//   ?lat=<float>&lon=<float>  — pick mount location (default = Notre-Dame)
//   ?backend=<wss-url>        — override WS URL
//   ?radius=<m>               — override mount radius
//
// Default backend is ws://<host>:7801/ — matches lab-ui's pin
// (apps/lab-ui/src/app.js, processor's --port 7801 launch). Override
// with ?backend=... if running a different processor.

import { init } from './sdk.js';

const DEFAULT_LAT = 48.576633764094396;
const DEFAULT_LON = -2.5686196133455326;
const DEFAULT_BACKEND = `ws://${location.hostname}:7801/`;

const params = new URLSearchParams(window.location.search);
const lat = parseFloat(params.get('lat') ?? String(DEFAULT_LAT));
const lon = parseFloat(params.get('lon') ?? String(DEFAULT_LON));
const radius = Number(params.get('radius') ?? 75);
const backendUrl = params.get('backend') ?? DEFAULT_BACKEND;
// Optional credential: ?token=… or VITE_DIORAMAP_TOKEN (processor --token).
// With a processor extension that reads another query parameter, set
// VITE_DIORAMAP_AUTH_PARAM (e.g. "key"); docs/EXTENSIONS.md.
const token = params.get('token') ?? import.meta.env.VITE_DIORAMAP_TOKEN ?? '';
const authParam = import.meta.env.VITE_DIORAMAP_AUTH_PARAM || 'token';
const auth = token ? { param: authParam, value: token } : undefined;

// Forward-declared so the model callbacks can poke the dev panel before
// it is wired (onModelMoved fires from the SDK, not from our button code).
let _refreshList = () => {};

init({
  container: '#app',
  backendUrl,
  auth,
  lat,
  lon,
  radius,
  onReady: (sdk) => {
    window.__viewer = sdk;
    console.log('[viewer-sdk dev page] mounted; sdk on window.__viewer');
    _refreshList = wireModelsPanel(sdk);
  },
  onModelMoved: () => _refreshList(),
  onModelDeselected: () => _refreshList(),
  onError: (err) => {
    console.error('[viewer-sdk dev page] error', err);
  },
}).catch((err) => {
  console.error('[viewer-sdk dev page] init failed', err);
});

// ── V6 dev panel — exercises addModel / removeModel / selectModel
// against the three GLBs in apps/viewer-sdk/public/models/. Lives
// outside the SDK bundle. ───────────────────────────────────────
function wireModelsPanel(sdk) {
  const panel = document.querySelector('#models-panel');
  const list = panel?.querySelector('#placed-list');
  if (!panel || !list) return () => {};

  function refreshList() {
    list.innerHTML = '';
    for (const m of sdk.getModels()) {
      const row = document.createElement('div');
      row.className = 'row';
      const span = document.createElement('span');
      const file = m.url.split('/').pop().replace('.glb', '');
      // (x, y, z) in scene-local metres. x = east, y = north, z = up.
      span.textContent = `${file}  x=${m.position.x.toFixed(1)} y=${m.position.y.toFixed(1)} z=${m.position.z.toFixed(1)}`;
      span.style.cursor = 'pointer';
      span.addEventListener('click', () => sdk.selectModel(m.uuid));
      const del = document.createElement('button');
      del.textContent = '✕';
      del.addEventListener('click', () => { sdk.removeModel(m.uuid); refreshList(); });
      row.appendChild(span);
      row.appendChild(del);
      list.appendChild(row);
    }
  }

  panel.querySelectorAll('button[data-glb]').forEach((btn) => {
    btn.addEventListener('click', async () => {
      btn.disabled = true;
      btn.textContent = btn.textContent.replace('+ ', '… ');
      try {
        // Ghost-placement: a translucent preview follows the cursor;
        // click in the scene to confirm, Esc to cancel.
        const uuid = await sdk.startGhostPlace({ url: btn.dataset.glb });
        if (uuid) refreshList();
      } catch (err) {
        console.error('[dev panel] startGhostPlace failed', err);
      } finally {
        btn.disabled = false;
        btn.textContent = btn.textContent.replace('… ', '+ ');
      }
    });
  });

  return refreshList;
}
