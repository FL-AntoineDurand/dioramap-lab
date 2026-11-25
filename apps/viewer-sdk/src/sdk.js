// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// sdk.js — public DioramapViewer.init API.
//
// Frozen signature per PLAN-VIEWER-SDK-PORT §5.7 — agri-map.fr can flip
// one URL and stop.
//
//   V2 working:  init, dispose, setSunTime, setSunEnabled
//   V5 wired:    sun-direction artifact init (slider seeds at capture time)
//   V6 wired:    addModel / removeModel / getModels / getModel /
//                selectModel / deselectModel / getSelectedModel /
//                setGizmoMode / setModelTransform
//   Stubbed:     setParcelsVisible / setCompassVisible / setPointsVisible /
//                setColumnsVisible / setTerrainVisible / setSurroundVisible /
//                setEDLEnabled — wait for the SDK to add the corresponding
//                node ids to EXPOSED_VISIBILITY_TOGGLES (per V8 polish).
//
// Stubs return sensible no-ops/null and warn once on call so a seller
// site that pre-wires its calls doesn't crash, but lab-ui devs see the
// reminder that those features aren't shipped yet.

// Bundled stylesheet — Vite extracts this to dist/style.css (one file
// per cssCodeSplit:false). Embedders must <link> the bundled CSS so the
// SDK works without 'unsafe-inline' in their CSP. See SDK-THEMING.md.
import './styles.css';
import { mountViewer } from './viewer.js';

// Local processor started with `dioramap-processor serve` (default port 7801).
const DEFAULT_BACKEND = 'ws://127.0.0.1:7801/';
const _warned = new Set();
function warnOnce(name) {
  if (_warned.has(name)) return;
  _warned.add(name);
  console.warn(`[viewer-sdk V2] ${name}() is not yet implemented (planned for a later milestone)`);
}

/**
 * Mount the embedded viewer in `config.container` and return an SDK handle.
 * @param {Object} config — see PLAN-VIEWER-SDK-PORT §5.7 for the frozen surface.
 * @returns {Promise<Object>}
 */
export async function init(config) {
  const {
    container,
    token = '',
    auth,
    backendUrl = DEFAULT_BACKEND,
    lat, lon,
    radius = 75,
    features: _features = {},
    onReady,
    onModelSelected,
    onModelMoved,
    onModelDeselected,
    onError,
    locale, issueRepo, reportIssues,
    plugins, pluginConfig,
  } = config;

  if (lat == null || lon == null) {
    throw new Error('DioramapViewer.init: lat and lon are required');
  }

  const el = typeof container === 'string' ? document.querySelector(container) : container;
  if (!el) throw new Error(`DioramapViewer.init: container "${container}" not found`);
  if (getComputedStyle(el).position === 'static') el.style.position = 'relative';

  // Credential appended to the WebSocket URL. `token` is the processor's
  // own --token; `auth: { param, value }` is for a processor extension that
  // reads another query parameter (docs/EXTENSIONS.md), e.g. { param: 'key' }.
  const cred = auth?.value ? auth : (token ? { param: 'token', value: token } : null);
  const fullBackend = cred
    ? backendUrl + (backendUrl.includes('?') ? '&' : '?') +
      `${encodeURIComponent(cred.param || 'token')}=${encodeURIComponent(cred.value)}`
    : backendUrl;

  const viewer = await mountViewer({
    container: el,
    backendUrl: fullBackend,
    lat, lon, radius,
    onError,
    onModelSelected, onModelMoved, onModelDeselected,
    locale, issueRepo, reportIssues,
    plugins, pluginConfig,
  });

  const sdk = {
    // Working at V2.
    setSunTime: (t) => viewer.sceneState.sun.setFromDate(t),
    dispose: () => viewer.dispose(),

    // V6 — models system.
    addModel:         (opts)  => viewer.models.addModel(opts),
    startGhostPlace:  (opts)  => viewer.models.startGhostPlace(opts),
    cancelGhost:      ()      => viewer.models.cancelGhost(),
    removeModel:      (uuid)  => viewer.models.removeModel(uuid),
    getModels:        ()      => viewer.models.getModels(),
    getModel:         (uuid)  => viewer.models.getModel(uuid),
    selectModel:      (uuid)  => viewer.models.selectModel(uuid),
    deselectModel:    ()      => viewer.models.deselectModel(),
    getSelectedModel: ()      => viewer.models.getSelectedModel(),
    setGizmoMode:     (mode)  => viewer.models.setGizmoMode(mode),
    setModelTransform:(uuid, t) => viewer.models.setModelTransform(uuid, t),

    // Visibility wrappers — they are no-ops until the corresponding
    // composer ships AND the SDK adds the node to EXPOSED_VISIBILITY_TOGGLES.
    setParcelsVisible:  (_v) => { warnOnce('setParcelsVisible'); },
    setCompassVisible:  (_v) => { warnOnce('setCompassVisible'); },
    setPointsVisible:   (_v) => { warnOnce('setPointsVisible'); },
    setColumnsVisible:  (_v) => { warnOnce('setColumnsVisible'); },
    setTerrainVisible:  (_v) => { warnOnce('setTerrainVisible'); },
    setSurroundVisible: (_v) => { warnOnce('setSurroundVisible'); },
    setEDLEnabled:      (_v) => { warnOnce('setEDLEnabled'); },
    setSunEnabled:      (v)  => viewer.sceneState.sun.setEnabled(v),

    // Internal handles for V3+ wiring; not part of the public API.
    _viewer: viewer,
  };

  if (onReady) onReady(sdk);
  return sdk;
}

export default { init };

if (typeof window !== 'undefined') {
  window.DioramapViewer = { init };
}
