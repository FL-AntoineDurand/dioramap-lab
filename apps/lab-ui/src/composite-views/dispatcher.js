// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// composite-views/dispatcher.js — runs composite-visualizers in lab-ui.
//
// Per VISUALIZERS.md §1, composite-visualizers live in @dioramap/scene-base/viz/
// and are consumed by both the SDK (closed allow-list) and lab-ui (Vite-glob
// vizCatalog + this dispatcher). This is the lab-ui side.
//
// Lifecycle (per VISUALIZERS.md §5):
//   - panel toggles a composite ON  → enable(id):  send SUBSCRIBE for any
//     meta.requires that aren't already in our decoded buffer; instantiate
//     a BaseRenderContext keyed on the composite id; render() once all
//     requirements have arrived.
//   - panel toggles a composite OFF → disable(id): ctx.dispose() removes
//     the visualizer's contributions from the 3D scene + visibility tree.
//   - artifact arrives                → handleArtifact(...): decode, store
//     in shared decoded buffer, attempt-render every enabled composite.
//   - stream chunk arrives            → handleStreamChunk(...): route to
//     the composite that has a streaming triplet AND lists the artifact in
//     its meta.requires. Suppress the trailing ARTIFACT_HEADER on final.
//
// Why a separate decoded buffer (vs reusing inspector's)? The inspector
// only decodes artifacts the user explicitly subscribed to via DAG tile
// clicks. Composite-visualizers need their requires regardless of what
// the user clicked, so the dispatcher subscribes + decodes independently.
// Both can coexist — SUBSCRIBE on the same artifact twice is a no-op
// server-side, and decoding the same bytes twice on arrival is cheap.

import { decodeArtifact } from '@dioramap/protocol';
import { BaseRenderContext } from '@dioramap/scene-base';

export class CompositeViewsDispatcher {
  /**
   * @param {Object} args
   * @param {WSClient}        args.client
   * @param {PipelineCatalog} args.catalog
   * @param {Viewer}          args.viewer       lab-ui's Three.js viewer
   * @param {SceneState}      args.sceneState   lab-ui's scene-state
   */
  constructor({ client, catalog, viewer, sceneState }) {
    this.client = client;
    this.catalog = catalog;
    this.viewer = viewer;
    this.sceneState = sceneState;

    // Per-composite state: id → { mod, ctx, rendered, streaming, finalised }.
    // mod is the imported viz module (with meta + render + optional streaming
    // triplet). Created on enable(id), destroyed on disable(id).
    this._composers = new Map();

    // Shared decoded buffer: artifact name → decoded payload (last seen).
    // Built up as artifacts arrive; queried by composers when checking
    // whether all their meta.requires are present. The trailing
    // ARTIFACT_HEADER on a streamed artifact (dual-delivery contract,
    // PLAN-VIEWER-PORT §7.3) is decoded into this map like any other
    // ARTIFACT — load-bearing for multi-consumer cases, e.g. when both
    // a streaming preview and a non-streaming discs composer require
    // `lidar-positions`. Streaming composers are gated against a
    // duplicate render by `state.streaming` in `_tryRender`.
    this._decoded = new Map();

    this._currentPipeline = null;
  }

  setCurrentPipeline(pipeline) {
    if (pipeline === this._currentPipeline) return;
    // Pipeline changed — drop everything. Each composite's ctx.dispose()
    // tears down its scene contributions + visibility leaves.
    for (const id of Array.from(this._composers.keys())) this.disable(id);
    this._decoded.clear();
    this._currentPipeline = pipeline;
  }

  /** Returns true if `id` is currently enabled. */
  isEnabled(id) {
    return this._composers.has(id);
  }

  /**
   * Enable a composite-visualizer.
   * @param {string} id    — vizCatalog entry id (filename minus .viz.js)
   * @param {Object} mod   — the imported viz module
   */
  enable(id, mod) {
    if (this._composers.has(id)) return;
    const ctx = new BaseRenderContext({
      viewer: this.viewer,
      sceneState: this.sceneState,
      layerId: `composite::${id}`,
      label: mod.meta?.name ?? id,
    });
    const state = {
      mod,
      ctx,
      rendered: false,
      streaming: false,
      finalised: false,
    };
    this._composers.set(id, state);

    // Don't subscribe here. Lab-ui's run-panel defaults to subscribe-all,
    // and SUBSCRIBE before the first RUN raises NO_RUN errors that
    // surface in the log panel. If the user has narrowed the run-panel
    // subscribe checklist below what a composite needs, that composite
    // simply won't fire — acceptable trade-off for L10. A future tweak:
    // subscribe lazily only after the first RUN_COMPLETE on this session.

    // If everything required is already buffered (e.g. composite was
    // toggled on after a run), render immediately.
    this._tryRender(id);
  }

  /**
   * Reset every enabled composite for a REHYDRATE_RUN replay.
   *
   * After REHYDRATE_RUN, the server emits the same artifacts again as
   * a normal run. Enabled composites stay enabled (the user wants to
   * see them paint), but their per-render state (`rendered`,
   * `streaming`, `finalised` + the BaseRenderContext that owns scene
   * contributions) needs to be wiped so the next artifact arrival
   * re-fires `_tryRender` instead of being ignored as already-rendered.
   *
   * Effect: same as toggling each composite OFF then ON, but in one
   * pass and without losing the user's enabled set.
   */
  resetForRehydrate() {
    for (const [id, state] of this._composers) {
      try { state.ctx?.dispose?.(); }
      catch (err) { console.warn('[composite-views] reset dispose', id, err); }
      state.ctx = new BaseRenderContext({
        viewer: this.viewer,
        sceneState: this.sceneState,
        layerId: `composite::${id}`,
        label: state.mod.meta?.name ?? id,
      });
      state.rendered = false;
      state.streaming = false;
      state.finalised = false;
    }
    // Forget any artifacts decoded under the previous run — REHYDRATE_RUN
    // will re-emit them with fresh bytes. Without this, a composite
    // checking `_decoded.has(req)` against a stale entry could fire
    // before the rehydrated bytes arrive and read the wrong payload.
    this._decoded.clear();
  }

  /** Disable a composite-visualizer — tear down its scene contributions. */
  disable(id) {
    const state = this._composers.get(id);
    if (!state) return;
    try { state.ctx?.dispose?.(); }
    catch (err) { console.warn('[composite-views] dispose', id, err); }
    this._composers.delete(id);
  }

  // ─── WS event handlers ────────────────────────────────────────────────

  handleArtifact({ header, bytes }) {
    const { artifact } = header;
    const kind = this._kindFor(artifact);
    if (kind == null) return;
    let decoded;
    try { decoded = decodeArtifact(kind, artifact, bytes); }
    catch (err) {
      console.warn('[composite-views] decode failed', artifact, err);
      return;
    }
    this._decoded.set(artifact, decoded);
    this._tryRenderAllReady();
  }

  handleStreamChunk({ header, bytes }) {
    const { artifact, seq, final, n_items_total } = header;
    const target = this._findStreamingComposerFor(artifact);
    if (!target) return;
    const { id, state } = target;
    if (state.finalised) return;
    const kind = this._kindFor(artifact);
    if (kind == null) return;

    let decoded;
    try { decoded = decodeArtifact(kind, artifact, bytes); }
    catch (err) {
      console.warn('[composite-views] chunk decode failed', artifact, err);
      return;
    }

    if (!state.streaming) {
      // eslint-disable-next-line no-console
      console.log('[composite-views] beginStream', id, 'on artifact', artifact);
      try { state.mod.beginStream(state.ctx, { artifact, seq, final, n_items_total }); }
      catch (err) { console.warn('[composite-views] beginStream', id, err); return; }
      state.streaming = true;
    }
    try { state.mod.appendChunk(state.ctx, decoded, { artifact, seq, final, n_items_total }); }
    catch (err) { console.warn('[composite-views] appendChunk', id, err); return; }

    if (final) {
      try { state.mod.finalizeStream(state.ctx, { artifact, n_items: n_items_total }); }
      catch (err) { console.warn('[composite-views] finalizeStream', id, err); }
      state.finalised = true;
      // The trailing ARTIFACT_HEADER for this artifact will follow per
      // the dual-delivery contract; we let it land in handleArtifact
      // normally so non-streaming consumers of the same artifact can
      // find it in `_decoded`. The streaming composer itself is gated
      // against duplicate render by `state.streaming` in `_tryRender`.
    }
  }

  // ─── Internals ────────────────────────────────────────────────────────

  _kindFor(artifactName) {
    if (!this._currentPipeline) return null;
    const list = this.catalog.artifactsByPipeline.get(this._currentPipeline);
    if (!list) return null;
    const entry = list.find((a) => a.name === artifactName);
    return entry?.kind ?? null;
  }

  _findStreamingComposerFor(artifact) {
    for (const [id, state] of this._composers) {
      if (typeof state.mod.beginStream !== 'function') continue;
      if ((state.mod.meta?.requires ?? []).includes(artifact)) {
        return { id, state };
      }
    }
    return null;
  }

  _tryRenderAllReady() {
    for (const id of this._composers.keys()) this._tryRender(id);
  }

  _tryRender(id) {
    const state = this._composers.get(id);
    if (!state || state.rendered || state.streaming) return;
    const reqs = state.mod.meta?.requires ?? [];
    const missing = reqs.filter((r) => !this._decoded.has(r));
    if (missing.length > 0) {
      // Log once per (composite, missing-set) combination so the console
      // doesn't flood — but only on the FIRST attempt that has at least
      // one decoded require, so we can tell "not yet" from "stalled".
      const someDecoded = reqs.some((r) => this._decoded.has(r));
      if (someDecoded && !state._lastMissingLog) {
        // eslint-disable-next-line no-console
        console.log('[composite-views]', id, 'waiting on:', missing.join(', '),
                    '— have:', reqs.filter((r) => this._decoded.has(r)).join(', '));
        state._lastMissingLog = missing.join(',');
      }
      return;
    }
    const decodedView = {};
    for (const r of reqs) decodedView[r] = this._decoded.get(r);
    // eslint-disable-next-line no-console
    console.log('[composite-views] render', id, '— all reqs satisfied:', reqs.join(', '));
    try { state.mod.render(state.ctx, decodedView); }
    catch (err) {
      console.warn('[composite-views] render', id, err);
      return;
    }
    state.rendered = true;
    // Honour meta.supersedes (same semantics as the SDK orchestration).
    const supersedes = state.mod.meta?.supersedes;
    if (Array.isArray(supersedes)) {
      for (const sid of supersedes) {
        try { this.sceneState?.visibility?.setVisible(sid, false); }
        catch (err) { console.warn('[composite-views] supersedes', sid, err); }
      }
    }
  }
}
