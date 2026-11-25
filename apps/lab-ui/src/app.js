// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Lab UI entry — L3 slice.
//
// Composition root. Wires:
//   - WSClient (protocol)
//   - PipelineCatalog (bare CATALOG + per-pipeline CATALOG with steps[])
//   - Viewer (Three.js) + ViewerToolbar (floating gear)
//   - DagView (center default surface)
//   - OverlayPanel (layer-list + render stack)
//   - ArtifactInspector (decode + dispatch to renderer modules)
//   - RunPanel + StepTimeline
//   - Left pane: filterable pipeline + artifact lists

import { WSClient } from '@dioramap/protocol';
import { PipelineCatalog } from './pipeline-catalog/catalog.js';
import { Viewer, MarineCompass, createSceneState } from '@dioramap/scene-base';
import Stats from 'three/addons/libs/stats.module.js';
import { ViewerToolbar } from './viewer/viewer-toolbar.js';
import { attachPointPicker } from './viewer/point-picker.js';
import { OverlayPanel } from './artifact-inspector/overlay-panel.js';
import { LegendOverlay } from './viewer/legend-overlay.js';
import { ArtifactInspector } from './artifact-inspector/inspector.js';
import { DagView } from './artifact-inspector/dag-view.js';
import { RunPanel } from './run-panel/run-panel.js';
import { StepTimeline } from './timeline/step-timeline.js';
import { LogPanel } from './log/log-panel.js';
import { Overrides } from './params-editor/overrides.js';
import { MapPicker } from './map-picker/wrapper.js';
import { CompositeViewsDispatcher } from './composite-views/dispatcher.js';
import { CompositeViewsPanel } from './composite-views/panel.js';
import { HistoryOverlay } from './history-panel/overlay.js';
import './history-panel/overlay.css';
// Splitter module retired at L3.4 — the center pane is now mode-switched
// (DAG vs Inspect) rather than a vertical split. The file stays in-tree
// in case we want to bring it back under Blender-style floating panels,
// but it's no longer imported.
import {
  loadViewerPrefs,
  loadSelectedPipeline,
  saveSelectedPipeline,
} from './layout/preferences.js';

// Connect to Vite's proxied `/ws` endpoint on the same origin as the
// lab-ui page itself. The proxy is configured in vite.config.js to
// forward to ws://127.0.0.1:7801/ inside the dev container, so the
// browser only ever sees the Vite origin's port (5181 by default) and
// VS Code's port-forwarder doesn't have to handle a separate WS port.
//
// Optional shared token for a processor started with --token
// (VITE_DIORAMAP_TOKEN in a local .env file).
const _TOKEN = import.meta.env.VITE_DIORAMAP_TOKEN ?? '';
const WS_URL = `ws://${location.host}/ws${_TOKEN ? `?token=${encodeURIComponent(_TOKEN)}` : ''}`;

const dom = {
  connStatus: document.getElementById('conn-status'),
  sessionToken: document.getElementById('session-token'),
  pipelineList: document.getElementById('pipeline-list'),
  filterPipelines: document.getElementById('filter-pipelines'),
  dagHost: document.getElementById('dag-host'),
  viewerHost: document.getElementById('viewer-host'),
  layerListHost: document.getElementById('layer-list-host'),
  stackHost: document.getElementById('stack-host'),
  runHost: document.getElementById('run-host'),
  compositeViewsHost: document.getElementById('composite-views-host'),
  timelineHost: document.getElementById('timeline-host'),
  logHost: document.getElementById('log-host'),
  paneCenter: document.getElementById('pane-center'),
  paneLeft: document.getElementById('pane-left'),
  paneLeftHeader: document.getElementById('pane-left-header'),
  paneLeftSpine: document.getElementById('pane-left-spine'),
  sceneControlsHost: document.getElementById('scene-controls-host'),
  viewerSettingsHost: document.getElementById('viewer-settings-host'),
  viewerLayersHost: document.getElementById('viewer-layers-host'),
  // backToDag button removed — Esc and Tab still switch modes (handled
  // in the keydown listener below).
  openHistory: document.getElementById('open-history'),
};

const client = new WSClient(WS_URL);
const catalog = new PipelineCatalog(client);
const viewer = new Viewer(dom.viewerHost, {
  // Lab-ui keeps the axes/grid debug helpers (orange/red/green cross +
  // grey grid plane) for orientation while inspecting artifacts.
  devHelpers: true,
});

// Debug: click a point in the 3D scene → console.log its point id.
attachPointPicker(viewer);

// Stats.js (Mr. Doob's perf gizmo) — FPS / MS / MB cycle (click the
// strip to switch panels). We add two custom panels driven by
// Three.js renderer.info: per-frame draw calls and per-frame
// triangles-in-thousands. A small mono readout below the chart shows
// the values that don't graph well (line/point counts, geometry +
// texture + program counts).
//
// Hooked into Viewer.setFrameCallback so values reflect the *actual*
// render rate; idle frames under render-on-demand are skipped, and
// a rAF-driven Stats would show a misleading 60 fps when nothing is
// drawing.
//
// Mount happens after viewerToolbar is constructed (it owns the host
// element). See the `viewerToolbar.perfHost.appendChild(...)` below.
const stats = new Stats();
const panelCalls = stats.addPanel(new Stats.Panel('Calls', '#0ff', '#022'));
const panelTris  = stats.addPanel(new Stats.Panel('kTri',  '#ff0', '#220'));
const perfMax = { calls: 100, tris: 100 };
let lastPerfInfoT = 0;
viewer.setFrameCallback(() => {
  stats.update();
  const ri = viewer.renderer.info;
  perfMax.calls = Math.max(perfMax.calls, ri.render.calls);
  perfMax.tris  = Math.max(perfMax.tris,  Math.ceil(ri.render.triangles / 1000));
  panelCalls.update(ri.render.calls, perfMax.calls);
  panelTris.update(ri.render.triangles / 1000, perfMax.tris);
  // Throttle the static readout — its values change slowly relative
  // to frame rate, and text mutations are expensive: textContent
  // writes trigger Firefox layout + a small WebRender invalidation
  // around the element. 4 Hz is plenty for a "current state" gauge.
  const now = performance.now();
  if (viewerToolbar?.perfInfo && (now - lastPerfInfoT) > 250) {
    lastPerfInfoT = now;
    viewerToolbar.perfInfo.textContent =
      `calls ${ri.render.calls}  tris ${ri.render.triangles.toLocaleString()}  ` +
      `lines ${ri.render.lines}  points ${ri.render.points}\n` +
      `geometries ${ri.memory.geometries}  textures ${ri.memory.textures}  ` +
      `programs ${ri.programs?.length ?? 0}`;
  }
});
// SceneState — sun + visibility tree + cascade events. Currently used by
// lab-ui only via the auto-registration that happens inside RenderContext;
// the dedicated 3D-scene-parameters panel arrives at L9. Lat/lon defaults
// to Paris (Notre-Dame) for SunCalc; lab-ui doesn't yet expose a sun
// slider, so the value is unused but must be valid (SunCalc requires
// finite numbers for getPosition).
const sceneState = createSceneState(viewer, { lat: 48.8566, lon: 2.3522 });
// Load persisted viewer preferences once, pass them into the compass +
// toolbar so reloads visibly restore the last configured state instead of
// flashing the defaults first. Individual toolbar setters re-save on change.
const viewerPrefs = loadViewerPrefs();
const compass = new MarineCompass(dom.viewerHost, { viewer, enabled: viewerPrefs.compass });
// Always-visible Scene-Controls column (inspect mode). Splits across
// TWO hosts in the column so the column-level pane-headers ("3D Scene
// Controls" / "Composite Views" / "Layers") read at the same visual
// weight as every other section title in the lab:
//   #viewer-settings-host → projection / point size / compass / sky /
//                           fog / sun (the "3D Scene Controls" section)
//   #viewer-layers-host   → visibility tree (the "Layers" section)
// Composite Views sits between them via its own host (set up below).
const viewerToolbar = new ViewerToolbar(dom.viewerSettingsHost, {
  viewer, compass, sceneState, initial: viewerPrefs,
  inline: true,
  layersHost: dom.viewerLayersHost,
});
// Stats.dom is owned by Stats.js (sets its own absolute positioning).
// We just inject it into the Performance section's host; the CSS
// override (style.css `.vw-perf-host > div`) flattens stats.js's
// inline `position:fixed/absolute` so it flows inside the column.
viewerToolbar.perfHost?.appendChild(stats.dom);
const overlay = new OverlayPanel({
  listHost: dom.layerListHost,
  stackHost: dom.stackHost,
});
const overrides = new Overrides();
const dagView = new DagView(dom.dagHost, { overrides });
// Phase 5.2-followup-A — legend overlay anchored inside the 3D viewer
// pane (top-left). Per-visualizer legends register here via
// ctx.renderLegend(); the overlay auto-shows/hides each based on the
// 3D layer's visibility-tree state.
const legendOverlay = new LegendOverlay(dom.viewerHost, { sceneState });
const inspector = new ArtifactInspector({
  viewer, sceneState, overlay, catalog, legendOverlay,
});
const runPanel = new RunPanel(dom.runHost, { overrides });
const compositeViews = new CompositeViewsDispatcher({
  client, catalog, viewer, sceneState,
});
const compositeViewsPanel = new CompositeViewsPanel(dom.compositeViewsHost, {
  dispatcher: compositeViews,
});
// L10 debug handle — paste in the browser console:
//   __cv._composers          — Map of enabled composites + their state
//   __cv._decoded.keys()     — what artifacts have been decoded
//   __viewer.scene.children  — list of every Three.js node in the scene
//   __sceneState.visibility.flatList()  — every visibility leaf
window.__cv = compositeViews;
window.__viewer = viewer;
window.__sceneState = sceneState;
window.__catalog = catalog;
window.__client = client;
const mapPicker = new MapPicker();
const timeline = new StepTimeline(dom.timelineHost);
const logPanel = new LogPanel(dom.logHost, { client });

// --- center-pane mode switch ------------------------------------------ //
function setCenterMode(mode) {
  dom.paneCenter.dataset.mode = mode;
  // The Layers visibility tree is only shown in 'inspect' (results) mode
  // (#scene-controls-host is display:none in 'dag'). Tell the toolbar so it
  // defers/flushes its tree rebuilds accordingly (avoids O(N²) rebuilds while
  // hidden during a run). See ViewerToolbar.setActive / _scheduleRenderTree.
  viewerToolbar?.setActive(mode === 'inspect');
}
// Match the toolbar's initial active-state to the starting center mode
// (index.html opens in 'dag', so the tree starts deferred).
viewerToolbar.setActive(dom.paneCenter.dataset.mode === 'inspect');
// (back-to-DAG button removed; mode switch via Esc / Tab key handlers below.)

// --- pipeline column collapse toggle ---------------------------------- //
// Click the column's `Pipelines` header to collapse → narrow spine; click
// the spine to expand back. State persists in localStorage. Enter/Space
// also work (role=button + tabindex on both elements).
{
  const KEY = 'dioramap.lab.paneLeftCollapsed';
  const stored = localStorage.getItem(KEY);
  if (stored === 'true') dom.paneLeft.dataset.collapsed = 'true';
  const setCollapsed = (next) => {
    dom.paneLeft.dataset.collapsed = String(next);
    localStorage.setItem(KEY, String(next));
  };
  const toggle = () => setCollapsed(dom.paneLeft.dataset.collapsed !== 'true');
  const handleKey = (ev) => {
    if (ev.key === 'Enter' || ev.key === ' ') {
      ev.preventDefault();
      toggle();
    }
  };
  dom.paneLeftHeader?.addEventListener('click', () => setCollapsed(true));
  dom.paneLeftHeader?.addEventListener('keydown', handleKey);
  dom.paneLeftSpine?.addEventListener('click', () => setCollapsed(false));
  dom.paneLeftSpine?.addEventListener('keydown', handleKey);
}

// --- L6 history overlay ---------------------------------------------- //
// Full-page modal above everything. Opens via topbar button + `H` key;
// clicking a row sends REHYDRATE_RUN, closes the overlay, and switches
// to inspect mode so the rehydrated artifacts paint into the inspector.
const historyOverlay = new HistoryOverlay({ client });
window.__history = historyOverlay;
dom.openHistory?.addEventListener('click', () => historyOverlay.open());
historyOverlay.addEventListener('rehydrate-requested', () => {
  // Clear inspector + composite-views state so the rehydrated artifacts
  // re-render instead of being treated as duplicates of an already-
  // rendered run. The composite-views dispatcher caches `rendered=true`
  // per composite once its requirements are met; without an explicit
  // reset, REHYDRATE_RUN's fresh artifact deliveries are ignored and
  // the user has to manually toggle each composite off→on. Keeps the
  // user's enabled set intact.
  inspector.clearAll?.();
  compositeViews.resetForRehydrate?.();
  setCenterMode('inspect');
});

// --- two-way focus link: DAG tile ↔ layer-list row -------------------- //
// Clicking a layer-list row comes from inside OverlayPanel; we relay its
// artifact name back to the DAG so the matching tile also lights up.
overlay.addEventListener('focus-requested', (ev) => {
  const name = ev.detail.artifactName;
  dagView.setFocused(name);
});

const state = {
  pipelineFilter: '',
  selectedPipeline: null,
  runActive: false,
  hasRunForCurrentPipeline: false,
};

// ---------------------------------------------------------------------- //
// WS + catalog wiring
// ---------------------------------------------------------------------- //
client.addEventListener('state', (ev) => {
  dom.connStatus.dataset.state = ev.detail.state;
  dom.connStatus.textContent = ev.detail.state;
  if (ev.detail.state === 'connected') catalog.requestPipelines();
});

client.addEventListener('session', (ev) => {
  dom.sessionToken.textContent = ev.detail.session_token;
});

client.addEventListener('error-msg', (ev) => {
  console.warn('[ws error]', ev.detail);
  // Step failures are reported in full by the RUN_FAILED that follows
  // (processor ≥ 0.3.0); only show request-level errors here, plus step
  // errors from older processors.
  if (!(ev.detail.step && client.processorVersion)) timeline.onError(ev.detail);
  if (ev.detail.step) dagView.setStepState(ev.detail.step, 'failed');
  state.runActive = false;
  runPanel.setRunning(false);
});

client.addEventListener('run-failed', (ev) => {
  const d = ev.detail;
  console.error('[run failed]', d);
  timeline.onError({ step: d.step, msg: `[${d.code}] ${d.msg}` });
  if (d.step_token || d.step) dagView.setStepState(d.step_token ?? d.step, 'failed');
  state.runActive = false;
  runPanel.setRunning(false);
});

client.addEventListener('run-started', (ev) => {
  timeline.reset(ev.detail.step_plan);
  // Fresh run — reset every step's run-state so previous run's
  // cached/fresh badges don't linger on the DAG nodes.
  dagView.clearAllStepStates();
});
client.addEventListener('step-started', (ev) => {
  timeline.onStepStarted(ev.detail.step);
  // Highlight the running step's DAG node.
  dagView.setStepState(ev.detail.step, 'running');
});
client.addEventListener('step-complete', (ev) => {
  timeline.onStepComplete(ev.detail);
  // Reflect on DAG tiles for each write of this step + on the step
  // node itself (cached/fresh distinguishes which steps actually ran
  // vs were short-circuited from the intent map).
  const stepName = ev.detail.step;
  const steps = catalog.stepsByPipeline.get(state.selectedPipeline) ?? [];
  const step = steps.find((s) => s.name === stepName);
  for (const w of step?.writes ?? []) {
    const dagState = ev.detail.cache_hit ? 'cached' : 'loaded';
    dagView.setArtifactState(w.name, dagState);
  }
  dagView.setStepState(stepName, ev.detail.cache_hit ? 'cached' : 'fresh');
});
client.addEventListener('run-complete', (ev) => {
  timeline.onRunComplete(ev.detail.summary);
  state.runActive = false;
  runPanel.setRunning(false);
  state.hasRunForCurrentPipeline = true;
  // Auto-switch to inspect mode at run-end so the user lands on the
  // 3D scene + artifact panes without having to click out of the DAG.
  // Tab / Esc still toggle back any time.
  setCenterMode('inspect');
});
client.addEventListener('subscribe-ack', (ev) => {
  state.runActive = false;
  runPanel.setRunning(false);
  timeline.onSubscribeAck(ev.detail);
  // DAG tile state was set correctly by the preceding step_complete events
  // (cache_hit true → cached; false → loaded). Nothing to do here.
});

client.addEventListener('artifact', (ev) => {
  inspector.handleArtifact(ev.detail);
  // Composite-views dispatcher (L10) listens to the same artifact stream
  // and decodes independently — two consumers on one event is cheap and
  // keeps their decoded buffers decoupled.
  compositeViews.handleArtifact(ev.detail);
  // Intentionally do NOT set the DAG tile state here — step_complete has
  // already set it based on cache_hit, and the artifact frame arrives for
  // both cache-hit and fresh executions alike, so overriding would mask
  // the cached vs fresh distinction the user asked for.
});

client.addEventListener('stream-chunk', (ev) => {
  inspector.handleStreamChunk(ev.detail);
  compositeViews.handleStreamChunk(ev.detail);
});

client.addEventListener('invalidated', (ev) => {
  // Reset the DAG tile's state — next run will recompute and repopulate.
  dagView.setArtifactState(ev.detail.artifact, 'idle');
});

catalog.addEventListener('pipelines-loaded', () => {
  renderPipelineList();
  // Restore the last-selected pipeline from session prefs, once the list is
  // known. Fires before first artifacts-loaded and sets up the DAG + input
  // JSON as if the user had clicked that pipeline themselves.
  const saved = loadSelectedPipeline();
  if (saved && catalog.pipelines.includes(saved) && state.selectedPipeline !== saved) {
    selectPipeline(saved);
  }
});
catalog.addEventListener('artifacts-loaded', (ev) => {
  if (ev.detail.pipeline === state.selectedPipeline) {
    runPanel.setPipeline(state.selectedPipeline, ev.detail.artifacts);
    dagView.setPipeline(state.selectedPipeline, {
      steps: ev.detail.steps,
      artifacts: ev.detail.artifacts,
    });
    // L10: tell the dispatcher which pipeline is active (so it knows
    // which artifact-name → kind map to use), then refresh the panel
    // with the eligible composite-visualizers + auto-enable defaults.
    compositeViews.setCurrentPipeline(state.selectedPipeline);
    compositeViewsPanel.setAvailableArtifacts(ev.detail.artifacts.map((a) => a.name));
  }
});

// ---------------------------------------------------------------------- //
// DAG view → click-to-subscribe
// ---------------------------------------------------------------------- //
dagView.addEventListener('artifact-invalidate-requested', (ev) => {
  const name = ev.detail.name;
  if (!state.hasRunForCurrentPipeline) {
    timeline.onError({
      msg: `run the pipeline at least once before invalidating — server needs the input to resolve the step`,
    });
    return;
  }
  try {
    client.invalidate(name);
  } catch (e) {
    console.error('[invalidate]', e);
  }
});

dagView.addEventListener('artifact-clicked', (ev) => {
  const artifactName = ev.detail.name;
  if (!state.hasRunForCurrentPipeline) {
    // Friendly hint: the server's SUBSCRIBE requires a prior RUN on the
    // session so it knows which pipeline + input to replay.
    timeline.onError({
      msg: `click "Run" first — SUBSCRIBE needs a prior RUN for input context`,
    });
    return;
  }
  // Clicking a tile always puts us in inspect mode. This matches the user
  // expectation from L3.4 — DAG is for navigation, the 3-column layout
  // (layers | 3D | renders) is for looking at an output.
  setCenterMode('inspect');

  // Highlight the tile + the matching layer-list row. If the row doesn't
  // exist yet (SUBSCRIBE is about to fire), inspector.focusArtifact remembers
  // the request and applies it once the row is created.
  dagView.setFocused(artifactName);
  inspector.focusArtifact(artifactName);

  // If the artifact was already delivered on the current run (because it
  // was in the RUN's subscribe list), skip the redundant SUBSCRIBE — the
  // block is already rendered, mode switch + focus are enough.
  const tileState = dagView.state.artifactState.get(artifactName);
  if (tileState === 'loaded' || tileState === 'cached') return;

  if (state.runActive) {
    timeline.onError({ msg: 'a run is in flight — wait for it to finish' });
    return;
  }
  dagView.setArtifactState(artifactName, 'loading');
  state.runActive = true;
  runPanel.setRunning(true);
  timeline.noteLateSubscribe(artifactName);
  try {
    client.subscribe(artifactName);
  } catch (e) {
    console.error('[subscribe]', e);
    state.runActive = false;
    runPanel.setRunning(false);
    dagView.setArtifactState(artifactName, 'idle');
  }
});

// ---------------------------------------------------------------------- //
// Run panel wiring
// ---------------------------------------------------------------------- //
runPanel.addEventListener('run-requested', (ev) => {
  const { pipeline, input, subscribe, params } = ev.detail;
  inspector.clearAll();
  timeline.reset([]);
  dagView.clearArtifactStates();
  state.runActive = true;
  state.hasRunForCurrentPipeline = false;
  runPanel.setRunning(true);
  inspector.setCurrentPipeline(pipeline);
  // L6 — synthesise a human-friendly test_case_label from the input so
  // the history list shows something readable per row. Falls through to
  // null when no lat/lon is present (then the journal stores NULL and
  // the overlay falls back to the input hash).
  let label = null;
  if (Number.isFinite(input?.lat) && Number.isFinite(input?.lon)) {
    const r = Number.isFinite(input.radius_m) ? input.radius_m
            : Number.isFinite(input.radius)   ? input.radius : null;
    label = `${input.lat.toFixed(4)},${input.lon.toFixed(4)}` + (r ? ` r=${r}` : '');
  }
  try {
    client.run(pipeline, input, subscribe, params, label);
  } catch (e) {
    console.error('[run]', e);
    state.runActive = false;
    runPanel.setRunning(false);
  }
});

runPanel.addEventListener('cancel-requested', () => {
  if (state.runActive) client.cancel();
});

runPanel.addEventListener('run-error', (ev) => {
  timeline.onError({ msg: ev.detail });
});

runPanel.addEventListener('pick-requested', async (ev) => {
  const pick = await mapPicker.open(ev.detail);
  if (pick) runPanel.applyPick(pick);
});

// ---------------------------------------------------------------------- //
// Left-pane rendering
// ---------------------------------------------------------------------- //
function renderPipelineList() {
  const q = state.pipelineFilter.trim().toLowerCase();
  const visible = catalog.pipelines.filter(
    (name) => !q || name.toLowerCase().includes(q),
  );
  dom.pipelineList.innerHTML = '';
  if (visible.length === 0) {
    dom.pipelineList.innerHTML = '<li class="empty">no pipelines</li>';
    return;
  }
  for (const name of visible) {
    const li = document.createElement('li');
    li.textContent = name;
    li.dataset.name = name;
    if (name === state.selectedPipeline) li.classList.add('selected');
    li.addEventListener('click', () => selectPipeline(name));
    dom.pipelineList.appendChild(li);
  }
}

function selectPipeline(name) {
  state.selectedPipeline = name;
  state.hasRunForCurrentPipeline = false;
  saveSelectedPipeline(name);
  renderPipelineList();
  inspector.setCurrentPipeline(name);
  inspector.clearAll();
  setCenterMode('dag');
  catalog.requestArtifacts(name)
    .then(({ artifacts, steps }) => {
      runPanel.setPipeline(name, artifacts);
      dagView.setPipeline(name, { artifacts, steps });
    })
    .catch((err) => {
      console.warn('[catalog]', err);
    });
}

function escape(s) {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[c]));
}

// ---------------------------------------------------------------------- //
// Wire up + boot
// ---------------------------------------------------------------------- //
dom.filterPipelines.addEventListener('input', (ev) => {
  state.pipelineFilter = ev.target.value;
  renderPipelineList();
});

document.addEventListener('keydown', (ev) => {
  const inField = ev.target.matches('input, textarea, select');
  if ((ev.metaKey || ev.ctrlKey) && ev.key === 'Enter') {
    ev.preventDefault();
    runPanel._fireRun();
  }
  if (ev.key === 'Escape' && !inField) {
    // History overlay takes priority — Esc closes it first.
    if (historyOverlay.isOpen()) {
      historyOverlay.close();
      return;
    }
    // Esc returns to DAG mode (L3.4 contract). Layers persist so
    // re-entering inspect re-shows whatever was loaded.
    if (dom.paneCenter.dataset.mode === 'inspect') {
      setCenterMode('dag');
    }
  }
  if (ev.key === 'Tab' && !inField && !ev.metaKey && !ev.ctrlKey && !ev.altKey) {
    // Tab toggles DAG ↔ inspect. Defer to inputs (Tab still focuses
    // the next field when the user is editing run-panel JSON or
    // params) — we only catch the no-modifier, no-input case.
    ev.preventDefault();
    setCenterMode(dom.paneCenter.dataset.mode === 'inspect' ? 'dag' : 'inspect');
  }
  if ((ev.key === 'h' || ev.key === 'H') && !inField &&
      !ev.metaKey && !ev.ctrlKey && !ev.altKey) {
    // L6 — H toggles the history overlay.
    ev.preventDefault();
    historyOverlay.toggle();
  }
});

renderPipelineList();
runPanel.setRunning(false);
client.connect();
