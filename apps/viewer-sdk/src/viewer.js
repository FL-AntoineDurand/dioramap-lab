// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// viewer.js — V3 orchestrator.
//
// What this owns:
//   - opens one WSClient (@dioramap/protocol)
//   - mounts sceneCtx (Viewer from @dioramap/scene-base) into the container
//   - mounts SceneState (sun + visibility tree)
//   - mounts the controls-panel DOM
//   - sends CATALOG `viewer-full`, then RUN with the §5.4 subscribe list
//   - hands every ARTIFACT / STREAM_CHUNK to Orchestration which decodes
//     and dispatches to the right composer
//
// Closed allow-list (§5.2): composer modules are imported by direct path
// here and listed in ORCHESTRATION. Adding a new composer = SDK code change
// + CHANGELOG bump + release. Lab-ui's Vite-glob path is irrelevant here.
//
// What's still deferred:
//   - terrain.viz multi-artifact composer (S5)
//   - point-cloud-discs.viz (S5)
//   - canopy.viz (S5)

import {
  WSClient, errorFromRunFailed, errorFromServerError, errorFromDisconnect, errorFromWatchdog,
} from '@dioramap/protocol';
import { Viewer, MarineCompass } from '@dioramap/scene-base';
import { createSceneState } from '@dioramap/scene-base/scene/scene-state';

// Closed allow-list — explicit imports per §5.2.
import * as previewMod from '@dioramap/scene-base/viz/lidar-positions';
import * as parcelsMod from '@dioramap/scene-base/viz/parcel-lines';
import * as discsMod from '@dioramap/scene-base/viz/point-cloud-discs';
import * as canopyMod from '@dioramap/scene-base/viz/canopy';
import * as surroundMod from '@dioramap/scene-base/viz/elevation-surround';

import { Orchestration } from './orchestration.js';
import { mountControlsPanel } from './controls-panel.js';
import { createModelsSystem } from './models/index.js';
import { mountLoadingBar } from './loading-bar.js';
import { mountAttribution } from './attribution.js';
import { resolveLocale } from './error-messages.js';

// Injected by vite.config.js (`define`); 'dev' when running from source.
// eslint-disable-next-line no-undef
const SDK_VERSION = typeof __DIORAMAP_SDK_VERSION__ !== 'undefined' ? __DIORAMAP_SDK_VERSION__ : 'dev';

// No frame at all for this long while a run is active = stalled. Generous:
// a cold run downloads ~100 MB LiDAR tiles inside one step, and the
// processor only reports at step boundaries.
const STALL_MS = 5 * 60 * 1000;
// Processors < 0.3.0 send only ERROR (no RUN_FAILED) on step failure.
// Wait this long for a RUN_FAILED before treating the ERROR as terminal.
const LEGACY_ERROR_GRACE_MS = 1500;

const PIPELINE = 'viewer-full';

// PLAN-VIEWER-SDK-PORT §5.4 — frozen subscription set.
//
// `lidar-ground-grid` was previously in this list but is only consumed
// by terrain.viz which is intentionally excluded from the SDK
// (per scene-base/src/viz/README.md — "the disc cloud already paints
// ortho per-vertex; a separate ground plane would z-fight"). Removing
// it from the subscribe set means no debug-only artifact is requested
// from a public-tier session, which would otherwise FORBIDDEN_ARTIFACT
// out under D6.5's tier-gated SUBSCRIBE.
const SUBSCRIBE_LIST = [
  'canonical-location', 'scene-origin', 'sun-direction',
  'lidar-positions', 'lidar-normals', 'lidar-spacing',
  'lidar-classifications',
  'canopy-flags', 'grass-flags',
  'deshadowed-ortho-rgb-rasters',
  'ortho-surround-jpeg', 'elevation-surround-f32',
  'parcel-lines',
];

// Closed composer allow-list (§5.2). The SDK is the production embed —
// terrain.viz + deshadowed-ortho.viz are intentionally excluded: the
// oriented-disc point cloud already paints the ortho per-vertex onto
// every disc, so a separate ground plane below would just z-fight and
// add a debug-feel grey patch under the actual point geometry. Lab-ui
// still exposes terrain + ortho via its composite-views panel because
// that's a debugging aid (compare ground-plane vs disc rendering).
//
// Order matters for visual stacking. preview renders first (~1 s after
// the first STREAM_CHUNK arrives) so users see something immediately;
// when discs.viz finishes, its meta.supersedes hides preview's leaf so
// the two don't overlap. discs/canopy require all the per-point reads
// (positions + normals + spacing + classifications + canopy-flags +
// deshadowed ortho), which arrive later in the run.
const ORCHESTRATION = [
  { id: 'preview',  label: 'LiDAR positions (preview)', mod: previewMod },
  { id: 'parcels',  label: 'Cadastre parcels',          mod: parcelsMod },
  { id: 'discs',    label: 'Point cloud (discs)',       mod: discsMod },
  { id: 'canopy',   label: 'Canopy (leaves)',           mod: canopyMod },
  { id: 'surround', label: 'Surround terrain',          mod: surroundMod },
];

/**
 * @param {Object} cfg
 * @param {HTMLElement} cfg.container        already-resolved container element
 * @param {string}      cfg.backendUrl       WS URL (key already appended by sdk.js)
 * @param {number}      cfg.lat
 * @param {number}      cfg.lon
 * @param {number}      [cfg.radius=75]
 * @param {Function}    [cfg.onError]          called once per failed run with a DioramapError
 *                                             ({code, message, step, detail, runId, …, toReport()})
 * @param {string}      [cfg.locale]           'fr' | 'en' (default: page / browser language)
 * @param {string}      [cfg.issueRepo]        GitHub "owner/repo" for the Report button
 * @param {boolean}     [cfg.reportIssues=true] show the Report on GitHub button
 * @param {Function[]}  [cfg.plugins]          viewer plugins (docs/EXTENSIONS.md):
 *                                             (ctx) => ({ onRunStarted, onRunComplete,
 *                                             onRunFailed, dispose }), all optional
 * @param {Object}      [cfg.pluginConfig]     passed to every plugin as ctx.config
 * @param {Function}    [cfg.onModelSelected]
 * @param {Function}    [cfg.onModelMoved]
 * @param {Function}    [cfg.onModelDeselected]
 * @returns {Promise<Object>}                viewer handle (sun, dispose, _internals)
 */
export async function mountViewer(cfg) {
  const {
    container, backendUrl, lat, lon, radius = 75,
    onError, onModelSelected, onModelMoved, onModelDeselected,
    locale: localeOpt, issueRepo, reportIssues = true,
    plugins: pluginFactories = [], pluginConfig = {},
  } = cfg;
  const locale = resolveLocale(localeOpt);

  // ── Three.js scene ──────────────────────────────────────────────
  const sceneCtx = new Viewer(container, { targetRadius: radius });
  const sceneState = createSceneState(sceneCtx, { lat, lon });
  sceneState.sun.setFromDate({
    year: 2024, month: 6, day: 15, hour: 12, minute: 0,
  });
  // Compass overlay — same widget lab-ui mounts. Sits as a DOM child of
  // the viewer container in the bottom-right by default; reads the camera
  // each frame to render the bearing.
  const compass = new MarineCompass(container, { viewer: sceneCtx, enabled: true });

  // ── Orchestration ──────────────────────────────────────────────
  const orchestration = new Orchestration({
    viewer: sceneCtx,
    sceneState,
    composers: ORCHESTRATION,
  });

  // ── DOM controls ───────────────────────────────────────────────
  const panel = mountControlsPanel(container, sceneState);

  // ── V7 loading bar — driven by RUN_STARTED / STEP_STARTED /
  //    STEP_COMPLETE / RUN_COMPLETE from the WSClient. ────────────
  const loadingBar = mountLoadingBar(container, { locale, issueRepo, reportIssues });

  // ── D5a attribution overlay — always-on bottom-right credit.
  // Initially shows just "Powered by dioramap.com"; the "Data: …" half
  // lights up when the first CATALOG_RESPONSE arrives (~1 s post-init).
  const attribution = mountAttribution(container);

  // ── Plugins (docs/EXTENSIONS.md): hosted-service add-ons such as a
  // feedback widget. A plugin that throws is logged and skipped; it never
  // breaks the viewer.
  const plugins = [];
  for (const factory of pluginFactories) {
    try {
      const p = factory({ container, controlsPanel: panel, config: pluginConfig, locale });
      if (p) plugins.push(p);
    } catch (e) {
      console.error('[viewer-sdk] plugin failed to mount', e);
    }
  }
  const notifyPlugins = (hook, arg) => {
    for (const p of plugins) {
      try { p[hook]?.(arg); } catch (e) { console.error(`[viewer-sdk] plugin ${hook} threw`, e); }
    }
  };

  // ── Models (V6: GLB placement + gizmo + click-select) ──────────
  const models = createModelsSystem({
    viewer: sceneCtx,
    onSelected: onModelSelected,
    onMoved: onModelMoved,
    onDeselected: onModelDeselected,
  });
  // Drive the action-button CSS2D overlay each frame. The Viewer runs
  // its own internal rAF for the WebGL render — we run a parallel rAF
  // here for the screen-space overlay positioning. Cheap (one Box3
  // setFromObject + a few DOM writes per frame); doing it inside the
  // Viewer's loop would need a hook the Viewer doesn't expose today.
  let raf = requestAnimationFrame(function tick() {
    models._tick();
    raf = requestAnimationFrame(tick);
  });
  models._cancelTick = () => cancelAnimationFrame(raf);

  // ── WebSocket ──────────────────────────────────────────────────
  const ws = new WSClient(backendUrl);
  const runInput = { lat, lon, radius_m: radius };

  // Run lifecycle, for failure reporting (docs/ERRORS.md). Exactly one
  // terminal outcome per run: RUN_COMPLETE, or fail() — whichever first.
  const run = {
    active: false, terminal: false, runId: null, everConnected: false,
    stepsTotal: 0, stepsCompleted: 0, lastStep: null,
    legacyTimer: null, kindsReady: false,
  };
  const errCtx = () => ({
    runId: run.runId, pipeline: PIPELINE, input: runInput,
    processorVersion: ws.processorVersion, clientVersion: `viewer-sdk ${SDK_VERSION}`,
    backendUrl, everConnected: run.everConnected, lastStep: run.lastStep,
    stepsCompleted: run.stepsCompleted, stepsTotal: run.stepsTotal,
  });
  const startRun = () => {
    run.active = true; run.terminal = false; run.runId = null;
    run.stepsTotal = 0; run.stepsCompleted = 0; run.lastStep = null;
    ws.lastMessageAt = Date.now();
    ws.run(PIPELINE, runInput, SUBSCRIBE_LIST);
  };
  const retry = () => {
    if (ws.ws && ws.ws.readyState === WebSocket.OPEN && run.kindsReady) startRun();
    else { run.active = true; run.terminal = false; ws.connect(); }
  };
  function fail(err) {
    if (run.terminal) return;
    run.terminal = true;
    run.active = false;
    clearTimeout(run.legacyTimer);
    console.error('[viewer-sdk] run failed:', err.code, err.message, err.toReport());
    loadingBar.onRunFailed(err, { onRetry: retry });
    notifyPlugins('onRunFailed', err);
    if (onError) {
      try { onError(err); } catch (e) { console.error('[viewer-sdk] onError handler threw', e); }
    }
  }

  ws.addEventListener('state', ({ detail }) => {
    if (detail.state === 'connected') run.everConnected = true;
    // 'error' is always followed by 'disconnected' (with the close code);
    // report once, from there.
    if (detail.state === 'disconnected' && (run.active || !run.everConnected) && !run.terminal) {
      fail(errorFromDisconnect(detail, errCtx()));
    }
  });

  // Stall watchdog.
  const watchdog = setInterval(() => {
    if (!run.active || run.terminal || !ws.lastMessageAt) return;
    const idle = Date.now() - ws.lastMessageAt;
    if (idle > STALL_MS) fail(errorFromWatchdog(idle, errCtx()));
  }, 10_000);

  // Two-step handshake: SESSION_STARTED → CATALOG → CATALOG_RESPONSE → RUN.
  // We need the CATALOG response to know each artifact's kind for decoding.
  ws.addEventListener('session', () => {
    run.active = true;
    ws.catalog(PIPELINE);
  });
  ws.addEventListener('catalog', ({ detail }) => {
    if (detail.pipeline !== PIPELINE) return;
    orchestration.setKinds(detail.artifacts ?? []);
    run.kindsReady = true;
    // D5a: refresh the bottom-right credit overlay from the pipeline's
    // declared attributions[]. Idempotent — no DOM rewrite if unchanged.
    attribution.update(detail);
    startRun();
  });

  ws.addEventListener('artifact', ({ detail }) => {
    // V5: sun-direction is the output of find-best-sun-time — an
    // OPAQUE_JSON {sun_dx, sun_dy, sun_dz, year, month, day, hour, minute, …}.
    // Use the date fields to seed the slider at the photo-capture time so
    // the scene starts roughly aligned with the deshadowed ortho's bake;
    // setFromDate also recomputes direction via SunCalc, which agrees with
    // (sun_dx, sun_dy, sun_dz) within numerical drift. No composer requires
    // the artifact, so we tap it here before handing off to the orchestrator.
    if (detail?.header?.artifact === 'sun-direction') {
      try {
        const text = new TextDecoder().decode(detail.bytes);
        const j = JSON.parse(text);
        if (Number.isInteger(j.year) && Number.isInteger(j.month) &&
            Number.isInteger(j.day) && Number.isInteger(j.hour) &&
            Number.isInteger(j.minute)) {
          sceneState.sun.setFromDate({
            year: j.year, month: j.month, day: j.day,
            hour: j.hour, minute: j.minute,
          });
        }
      } catch (err) {
        console.warn('[viewer-sdk] sun-direction parse failed', err);
      }
    }
    // scene-origin is the canonical {x, y, z} origin computed by
    // compute-scene-origin (canonical-location.center + midZ from BIL
    // elevation). Wire it into the Viewer so the streaming preview's
    // `getSceneOrigin()` returns the canonical value before the first
    // lidar-positions chunk lands — eliminating the first-chunk-centroid
    // bias that left the preview cloud offset on fresh runs. Discs and
    // canopy composers read scene-origin from their own decoded view
    // (via `meta.requires`) and have always been aligned correctly;
    // this just brings the preview into the same frame.
    if (detail?.header?.artifact === 'scene-origin') {
      try {
        const text = new TextDecoder().decode(detail.bytes);
        const j = JSON.parse(text);
        if (Number.isFinite(j.x) && Number.isFinite(j.y) && Number.isFinite(j.z)) {
          sceneCtx.setSceneOrigin({ x: j.x, y: j.y, z: j.z });
        }
      } catch (err) {
        console.warn('[viewer-sdk] scene-origin parse failed', err);
      }
    }
    orchestration.handleArtifact(detail);
  });
  ws.addEventListener('stream-chunk', ({ detail }) => {
    orchestration.handleStreamChunk(detail);
  });

  ws.addEventListener('run-failed', ({ detail }) => {
    fail(errorFromRunFailed(detail, errCtx()));
  });

  ws.addEventListener('error-msg', ({ detail }) => {
    console.warn('[viewer-sdk] server error:', detail);
    const err = errorFromServerError(detail, errCtx());
    if (detail.step) {
      // Step failure. Processors ≥ 0.3.0 follow up with RUN_FAILED (full
      // detail); older ones send nothing else — fall back to this ERROR.
      clearTimeout(run.legacyTimer);
      run.legacyTimer = setTimeout(() => fail(err), LEGACY_ERROR_GRACE_MS);
    } else if (detail.code === 'ARTIFACT_MISSING') {
      // One artifact could not be shipped; the run itself goes on.
    } else if (err.code === 'CANCELLED') {
      // We cancel on dispose(); processors ≥ 0.3.0 also send RUN_FAILED.
    } else if (run.active || /^AUTH_/.test(err.code)) {
      // Request-level rejection (AUTH_*, BAD_RUN, UNKNOWN_PIPELINE,
      // RUN_BUSY, FORBIDDEN_ARTIFACT…): the run will not happen. Auth
      // refusals arrive right after connecting, before any run has started,
      // and the server then closes the socket: terminal too, or the loading
      // card would wait forever (wrong / revoked key, origin not allowed).
      fail(err);
    }
  });

  // Loading-bar event hooks. Fire after the orchestration's own
  // listeners — order doesn't matter (no shared state); two parallel
  // observers of the same WS events.
  ws.addEventListener('run-started', ({ detail }) => {
    loadingBar.onRunStarted(detail);
    notifyPlugins('onRunStarted', detail);
  });
  ws.addEventListener('run-started', ({ detail }) => {
    run.runId = detail.run_id ?? null;
    run.stepsTotal = detail.step_plan?.length ?? 0;
  });
  ws.addEventListener('step-started', ({ detail }) => {
    run.lastStep = detail.step ?? null;
    loadingBar.onStepStarted(detail);
  });
  ws.addEventListener('step-complete', ({ detail }) => {
    run.stepsCompleted += 1;
    loadingBar.onStepComplete(detail);
  });
  ws.addEventListener('run-complete', () => {
    if (run.terminal) return;
    run.terminal = true;
    run.active = false;
    clearTimeout(run.legacyTimer);
    loadingBar.onRunComplete();
    notifyPlugins('onRunComplete');
  });

  ws.connect();

  return {
    sceneCtx,
    sceneState,
    ws,
    orchestration,
    models,
    dispose() {
      // Disposal is not a failure: silence the terminal-failure path.
      run.terminal = true;
      run.active = false;
      clearInterval(watchdog);
      clearTimeout(run.legacyTimer);
      try { ws.cancel(); } catch { /* WS may already be closed */ }
      try { ws.ws?.close(); } catch { /* idem */ }
      orchestration.dispose();
      models?._cancelTick?.();
      models?._dispose?.();
      loadingBar?.dispose?.();
      panel?.dispose?.();
      compass?.dispose?.();
      attribution?.dispose?.();
      notifyPlugins('dispose');
      sceneCtx.dispose?.();
    },
  };
}
