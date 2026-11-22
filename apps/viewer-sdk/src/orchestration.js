// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// orchestration.js — CATALOG-driven artifact dispatch + composer routing.
//
// The SDK is a closed-allow-list orchestrator (PLAN-VIEWER-SDK-PORT §5.2 +
// §5.3). For each composer in `composers`:
//   - Track decoded artifacts the composer requires.
//   - When all `meta.requires` are satisfied AND the composer hasn't yet
//     rendered, build a fresh BaseRenderContext and call composer.render(ctx, decoded).
//   - For composers with a streaming triplet (beginStream/appendChunk/
//     finalizeStream), route every STREAM_CHUNK whose artifact is in the
//     composer's `requires` list. The composer's own ctx is reused across
//     chunks; the trailing dual-delivery ARTIFACT_HEADER is suppressed
//     (the composer already consumed all bytes via chunks).
//
// Single-pass: this keeps each composer alive until `dispose()`. Re-runs
// of the same SDK session would need explicit reset (V8 polish).
//
// Trailing ARTIFACT_HEADER on streamed artifacts: per the protocol's
// dual-delivery contract (PLAN-VIEWER-PORT §7.3, ws-client.js comment),
// every STREAM_CHUNK sequence is followed by a full ARTIFACT_HEADER
// carrying the assembled bytes. The orchestrator decodes that header
// like any other ARTIFACT, populating `_decoded[artifact]`. This is
// load-bearing for multi-consumer streamed artifacts (today: the
// `lidar-positions` produced by `laz-extract-disc` is consumed by the
// streaming preview composer AND by the non-streaming discs + canopy
// composers). The streaming composer itself is gated against re-render
// by `st.streaming === true` in `_maybeRenderReadyComposers`, so the
// extra decode is purely for the benefit of consumers that aren't on
// the streaming path. Skipping it left fresh runs (no cache hit) stuck
// on the blue preview cloud — composers like discs/canopy never saw
// `lidar-positions` in `_decoded` and never fired their render.

import { decodeArtifact } from '@dioramap/protocol';
import { BaseRenderContext } from '@dioramap/scene-base';

export class Orchestration {
  /**
   * @param {Object} args
   * @param {Object} args.viewer            Viewer instance from scene-base
   * @param {Object} args.sceneState        SceneState from scene-base
   * @param {Array}  args.composers         [{ id, label, mod }]; mod = imported viz module
   */
  constructor({ viewer, sceneState, composers }) {
    this.viewer = viewer;
    this.sceneState = sceneState;
    this.composers = composers;
    // Cached kind lookup, seeded by setKinds() once CATALOG arrives.
    this._kindByName = new Map();
    // Decoded artifact payload buffer, name → decoded.
    this._decoded = new Map();
    // Per-composer state.
    this._composerState = new Map();
    for (const c of composers) {
      this._composerState.set(c.id, {
        ctx: null,             // built lazily on first render call
        rendered: false,       // one-shot composer fired
        streaming: false,      // begin/append/finalize triplet active
        finalised: false,      // streaming sequence completed
      });
    }
  }

  /** Seed the kind map from a CATALOG_RESPONSE. */
  setKinds(artifacts) {
    for (const a of (artifacts ?? [])) {
      if (a.name && Number.isInteger(a.kind)) {
        this._kindByName.set(a.name, a.kind);
      }
    }
  }

  /** Handle one ARTIFACT (full payload) from the WS client. */
  handleArtifact({ header, bytes }) {
    const { artifact } = header;
    const kind = this._kindByName.get(artifact);
    if (kind == null) {
      // CATALOG hasn't arrived yet, or the artifact isn't in the catalog
      // (server-side bug). Drop silently — composers will simply never fire.
      console.warn('[viewer-sdk] no kind for', artifact);
      return;
    }
    let decoded;
    try {
      decoded = decodeArtifact(kind, artifact, bytes);
    } catch (err) {
      console.warn('[viewer-sdk] decode failed for', artifact, err);
      return;
    }
    this._decoded.set(artifact, decoded);
    this._maybeRenderReadyComposers();
  }

  /** Handle one STREAM_CHUNK. */
  handleStreamChunk({ header, bytes }) {
    const { artifact, seq, final, n_items_total } = header;
    const kind = this._kindByName.get(artifact);
    if (kind == null) return;

    // Find the streaming composer (if any) that requires this artifact.
    const target = this._findStreamingComposerFor(artifact);
    if (!target) return;

    const { id, mod } = target;
    const st = this._composerState.get(id);
    if (st.finalised) return;

    let decoded;
    try {
      // Each chunk decodes as a standalone payload (e.g. one batch of points).
      decoded = decodeArtifact(kind, artifact, bytes);
    } catch (err) {
      console.warn('[viewer-sdk] chunk decode failed for', artifact, err);
      return;
    }

    if (!st.streaming) {
      st.ctx = this._buildCtx(target);
      try { mod.beginStream(st.ctx, { artifact, seq, final, n_items_total }); }
      catch (err) { console.warn('[viewer-sdk] beginStream', id, err); return; }
      st.streaming = true;
    }
    try { mod.appendChunk(st.ctx, decoded, { artifact, seq, final, n_items_total }); }
    catch (err) { console.warn('[viewer-sdk] appendChunk', id, err); return; }

    if (final) {
      try { mod.finalizeStream(st.ctx, { artifact, n_items: n_items_total }); }
      catch (err) { console.warn('[viewer-sdk] finalizeStream', id, err); }
      st.finalised = true;
      // The trailing ARTIFACT_HEADER for this artifact will follow per
      // the protocol's dual-delivery contract; we let it land in
      // handleArtifact normally so non-streaming consumers of the same
      // artifact (e.g. discs/canopy composers also requiring
      // `lidar-positions`) can find it in `_decoded`. The streaming
      // composer itself is gated against a duplicate render by
      // `st.streaming` in `_maybeRenderReadyComposers`.
    }
  }

  dispose() {
    for (const st of this._composerState.values()) {
      try { st.ctx?.dispose?.(); } catch { /* best-effort */ }
    }
    this._composerState.clear();
    this._decoded.clear();
  }

  // ─── Internals ──────────────────────────────────────────────────────

  _maybeRenderReadyComposers() {
    for (const c of this.composers) {
      const st = this._composerState.get(c.id);
      if (st.rendered || st.streaming) continue;
      const reqs = c.mod.meta?.requires ?? [];
      const ready = reqs.every((r) => this._decoded.has(r));
      if (!ready) continue;
      // If the composer also has a streaming triplet AND its key artifact is
      // in flight (chunked), let the streaming path handle it. We detect
      // "key artifact" as the first entry in `requires` by convention.
      if (typeof c.mod.beginStream === 'function') {
        // For streaming-capable composers, prefer the chunk path. If a full
        // ARTIFACT_HEADER landed first (cached re-run delivers cached bytes
        // as one frame, not chunks), fall through to the one-shot render
        // below. The streaming path won't re-fire because st.rendered/st.streaming
        // gates it.
      }
      const decodedView = this._gather(reqs);
      const ctx = this._buildCtx(c);
      try { c.mod.render(ctx, decodedView); }
      catch (err) { console.warn('[viewer-sdk] render', c.id, err); continue; }
      st.ctx = ctx;
      st.rendered = true;
      // Honour meta.supersedes — hide every leaf id this composite
      // declares as visually redundant once it's painted (e.g. discs
      // hides 'lidar-positions-preview' so the early streaming blue
      // preview doesn't show through under the high-quality discs).
      this._applySupersedes(c.mod);
    }
  }

  _applySupersedes(mod) {
    const ids = mod?.meta?.supersedes;
    if (!Array.isArray(ids) || ids.length === 0) return;
    for (const id of ids) {
      try { this.sceneState?.visibility?.setVisible(id, false); }
      catch (err) { console.warn('[viewer-sdk] supersedes', id, err); }
    }
  }

  _gather(names) {
    const out = {};
    for (const n of names) out[n] = this._decoded.get(n);
    return out;
  }

  _findStreamingComposerFor(artifact) {
    for (const c of this.composers) {
      if (typeof c.mod.beginStream !== 'function') continue;
      if ((c.mod.meta?.requires ?? []).includes(artifact)) return c;
    }
    return null;
  }

  _buildCtx(composer) {
    return new BaseRenderContext({
      viewer: this.viewer,
      sceneState: this.sceneState,
      layerId: `sdk::${composer.id}`,
      label: composer.label ?? composer.mod.meta?.name ?? composer.id,
    });
  }
}
