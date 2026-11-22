// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Artifact inspector — L2 slice.
//
// Responsibilities:
//   - Own the active layers map (layerId → RenderContext).
//   - Listen for `artifact` events from the WS client, decode, dispatch to
//     a renderer, and tell the viewer + overlay panel to render.
//   - Expose `removeArtifact(layerId)` / `clearAll()` for the caller.
//
// L3 adds the dagre DAG view above this module; L2 keeps it flat — one
// arriving artifact → one overlay entry, one or more 3D layers.

import { decodeArtifact } from '@dioramap/protocol';
import { resolveRenderer, resolveRendererSync } from '../renderers/loader.js';
import { RenderContext } from '../renderers/context.js';

export class ArtifactInspector {
  constructor({ viewer, sceneState, overlay, catalog, legendOverlay }) {
    this.viewer = viewer;
    // Optional — when provided, RenderContext bridges 3D layers into
    // sceneState.visibility (auto-register + tri-state cascade). Lab-ui
    // wires this from app.js as of L8.
    this.sceneState = sceneState ?? null;
    this.overlay = overlay;
    this.catalog = catalog;
    this.legendOverlay = legendOverlay ?? null;
    this.contexts = new Map();        // layerId → RenderContext
    this.decoded = new Map();         // artifact name → decoded payload
    this.layerIdByArtifact = new Map(); // artifact name → layerId
    this._currentPipeline = null;
    // Auto-rerun bookkeeping (Phase 5.2-followup-E). Each step-visualizer
    // fires once when its primary artifact arrives, but may request
    // sibling artifacts via getSiblingDecoded(). If the sibling hasn't
    // arrived/decoded yet, we register the requesting context here; when
    // the sibling later lands in handleArtifact() we destroy + re-fire
    // those contexts so they re-render with the now-available sibling.
    // Also handles the cache-hit case where artifacts arrive in
    // arbitrary order.
    this._pendingSiblings = new Map();   // siblingName → Set<layerId>
    this._contextInfo = new Map();       // layerId → {artifact,kind,decoded,mod,hash}
    // Streaming accumulator (PLAN-VIEWER-PORT §7, B9). One entry per
    // in-flight streaming artifact. Entries are created lazily on the
    // first chunk, freed on `final=true` (or dropped on error/clearAll).
    //   _streams: artifact-name → {
    //     layerId, kind, mod, ctx,
    //     chunks: Uint8Array[],       // raw chunk bytes in seq order
    //     totalSize: number,          // sum of chunk sizes
    //     expectedSeq: number,        // next chunk's expected seq
    //     n_items: number,            // running element count (for renderers that want it)
    //     progressive: boolean,       // true if the renderer has beginStream()
    //   }
    this._streams = new Map();
    // After a streaming artifact finalises, its trailing ARTIFACT_HEADER
    // (dual delivery) must be ignored — we already rendered from chunks.
    // Keyed by layerId so re-selecting the pipeline clears with clearAll.
    this._renderedViaStream = new Set();

    overlay.setRemoveHandler((layerId) => this.removeArtifact(layerId));
  }

  setCurrentPipeline(pipeline) {
    this._currentPipeline = pipeline;
  }

  // Exposed to RenderContext: renderers look up siblings (e.g. lidar-normals
  // fetching lidar-positions to color those points by normal direction).
  // The `requestingLayerId` form registers a pending request when the
  // sibling isn't decoded yet — handleArtifact() later finds the entry
  // and re-fires the requesting context's render.
  getSiblingDecoded(name) { return this.decoded.get(name); }
  getSiblingDecodedFor(requestingLayerId, name) {
    const v = this.decoded.get(name);
    if (v === undefined && requestingLayerId) {
      let s = this._pendingSiblings.get(name);
      if (!s) { s = new Set(); this._pendingSiblings.set(name, s); }
      s.add(requestingLayerId);
    }
    return v;
  }
  getLayerIdByArtifact(name) { return this.layerIdByArtifact.get(name); }

  // Drop any pending-sibling registrations a layer holds. Called before
  // re-rendering the same layer (so the next render's getSiblingDecoded
  // calls re-register fresh) and on destroy.
  _clearPendingFor(layerId) {
    for (const [name, set] of this._pendingSiblings) {
      set.delete(layerId);
      if (set.size === 0) this._pendingSiblings.delete(name);
    }
  }

  // Re-render the contexts that asked for `arrivedArtifact` but found it
  // missing earlier. Called from handleArtifact after decoded.set.
  _refireWaiters(arrivedArtifact) {
    const waiters = this._pendingSiblings.get(arrivedArtifact);
    if (!waiters || waiters.size === 0) return;
    // Snapshot then clear (a re-render may register fresh waiters).
    const snapshot = Array.from(waiters);
    this._pendingSiblings.delete(arrivedArtifact);
    for (const layerId of snapshot) {
      const info = this._contextInfo.get(layerId);
      if (!info) continue;
      this._renderInto(layerId, info.artifact, info.kind, info.decoded,
                       info.hash, info.mod);
    }
  }

  // Build a RenderContext, store it, drive `mod.render()`. Re-usable
  // across first-arrival and re-fire paths.
  _renderInto(layerId, artifact, kind, decoded, hash, mod) {
    const existing = this.contexts.get(layerId);
    if (existing) {
      existing.destroy();
      this.contexts.delete(layerId);
    }
    this._clearPendingFor(layerId);

    const label = `${artifact}  ${hash ? hash.slice(0, 8) : ''}`;
    this.overlay.setLabel(layerId, label);
    const ctx = new RenderContext({
      viewer: this.viewer,
      sceneState: this.sceneState,
      overlay: this.overlay,
      layerId,
      label: artifact,
      inspector: this,
      legendOverlay: this.legendOverlay,
    });
    this.contexts.set(layerId, ctx);
    this._contextInfo.set(layerId, { artifact, kind, decoded, hash, mod });

    try {
      mod.render(ctx, decoded);
    } catch (e) {
      console.error('[inspector] render failed', artifact, e);
      ctx.renderHTML(`<div class="render-error">render error: ${e.message}</div>`);
    }
    if (this._pendingFocus === artifact) {
      this._pendingFocus = null;
      this.overlay.focus(layerId);
    }
  }

  // Focus the overlay row for an artifact. If the artifact hasn't arrived
  // yet (user clicked a DAG tile that triggers a SUBSCRIBE), remember the
  // request and apply it when handleArtifact delivers the row.
  focusArtifact(name) {
    const layerId = this.layerIdByArtifact.get(name);
    if (layerId) {
      this.overlay.focus(layerId);
      this._pendingFocus = null;
    } else {
      this._pendingFocus = name;
    }
  }

  // Handle one `artifact` event from the WS client.
  handleArtifact({ header, bytes }) {
    const { artifact, hash } = header;
    // Layer id stays stable across re-runs of the same pipeline + artifact.
    // When the user re-runs, we replace the existing layer in place.
    const layerId = `${this._currentPipeline ?? '?'}::${artifact}`;

    // Dual-delivery dedup: if this artifact was already rendered via a
    // completed STREAM_CHUNK sequence, the trailing ARTIFACT_HEADER is a
    // server-side resend for non-streaming clients. Drop it silently.
    if (this._renderedViaStream.has(layerId)) {
      this._renderedViaStream.delete(layerId);
      return;
    }

    const kind = this._kindFor(artifact);
    if (kind == null) {
      console.warn('[inspector] no kind for artifact', artifact);
      return;
    }

    // Decode.
    let decoded;
    try {
      decoded = decodeArtifact(kind, artifact, bytes);
    } catch (e) {
      console.error('[inspector] decode failed', artifact, e);
      return;
    }

    this.decoded.set(artifact, decoded);
    this.layerIdByArtifact.set(artifact, layerId);

    resolveRenderer({ kind, name: artifact }).then((mod) => {
      this._renderInto(layerId, artifact, kind, decoded, hash, mod);
      // Wake up any earlier-rendered context that asked for this
      // artifact via getSiblingDecoded() and got undefined. Re-fires
      // their render now that the sibling is available. This is what
      // makes cache-hit runs (where artifacts arrive in arbitrary
      // order) work without relying on emit-order in the C step.
      this._refireWaiters(artifact);
    });
  }

  // Handle one `stream-chunk` event from the WS client.
  //
  // First chunk: decode kind → resolve renderer → create context. If the
  // renderer exports beginStream(), enter progressive mode: its appendChunk
  // is called with the decoded just-arrived bytes on every call; finalize-
  // Stream() is called on final=true.
  //
  // If the renderer only exposes the plain render() entry, fall back to
  // concat-and-render-once: buffer every chunk's bytes, on final concatenate
  // into a single Uint8Array, decode, call render() as usual. Behavior
  // matches the non-streaming code path; user gets no early-pixel benefit
  // but nothing breaks.
  handleStreamChunk({ header, bytes }) {
    const { artifact, seq, final, hash } = header;
    const layerId = `${this._currentPipeline ?? '?'}::${artifact}`;

    const kind = this._kindFor(artifact);
    if (kind == null) {
      console.warn('[inspector] no kind for streaming artifact', artifact);
      return;
    }

    let state = this._streams.get(artifact);
    if (!state) {
      // First chunk: clean up any previous layer under this id, set up
      // a fresh RenderContext + resolve the renderer module. Every step
      // here is synchronous so back-to-back chunks can't race past the
      // `_streams.set()` guard.
      const existing = this.contexts.get(layerId);
      if (existing) { existing.destroy(); this.contexts.delete(layerId); }

      const mod = resolveRendererSync({ kind });
      if (!mod) {
        console.warn('[inspector] streaming requires a sync-resolvable renderer, falling back to concat', artifact);
        // No sync renderer today means a future lazy .viz.js — we buffer
        // chunks and render on final, same as the non-progressive fallback.
      }

      const label = `${artifact}  streaming…`;
      this.overlay.setLabel(layerId, label);
      const ctx = new RenderContext({
        viewer: this.viewer,
        sceneState: this.sceneState,
        overlay: this.overlay,
        layerId,
        label: artifact,
        inspector: this,
      });
      this.contexts.set(layerId, ctx);
      this.layerIdByArtifact.set(artifact, layerId);

      const progressive = !!(mod && typeof mod.beginStream === 'function');
      state = {
        layerId, kind, mod, ctx,
        chunks: [],
        totalSize: 0,
        expectedSeq: 0,
        n_items: 0,
        progressive,
      };
      this._streams.set(artifact, state);
      if (progressive) {
        try { mod.beginStream(ctx, { artifact, kind }); }
        catch (e) { console.error('[inspector] beginStream failed', artifact, e); }
      }
    }

    if (seq !== state.expectedSeq) {
      console.warn('[inspector] stream seq gap', artifact, 'expected', state.expectedSeq, 'got', seq);
    }
    state.expectedSeq = seq + 1;

    const chunkBytes = bytes instanceof ArrayBuffer ? new Uint8Array(bytes) : bytes;
    state.chunks.push(chunkBytes);
    state.totalSize += chunkBytes.byteLength;

    if (state.progressive && state.mod) {
      // Decode JUST this chunk and hand it off. The decoder reads the
      // wire layout for point columns as raw typed-array bytes, which
      // works per-chunk provided the chunk size is a multiple of the
      // element stride — the producer (step_write_output_stream) honors
      // this today (see laz-extract-disc: CHUNK_POINTS * 3 * sizeof(F64)).
      let decoded;
      try {
        decoded = decodeArtifact(kind, artifact, chunkBytes.buffer.slice(
          chunkBytes.byteOffset,
          chunkBytes.byteOffset + chunkBytes.byteLength,
        ));
      } catch (e) {
        console.error('[inspector] chunk decode failed', artifact, e);
        return;
      }
      state.n_items += decoded.n_items ?? 0;
      try {
        state.mod.appendChunk(state.ctx, decoded, { seq, final, n_items_total: state.n_items });
      } catch (e) {
        console.error('[inspector] appendChunk failed', artifact, e);
      }
    }

    if (final) {
      this._renderedViaStream.add(layerId);
      this._streams.delete(artifact);
      this.overlay.setLabel(layerId, `${artifact}  ${hash ? hash.slice(0, 8) : ''}`);

      if (state.progressive) {
        try { state.mod.finalizeStream?.(state.ctx, { n_items: state.n_items, hash }); }
        catch (e) { console.error('[inspector] finalizeStream failed', artifact, e); }
      } else {
        // Fallback: concat every chunk's bytes into one buffer, decode,
        // call plain render(). Matches non-streaming behavior. If the
        // renderer didn't resolve sync (future lazy .viz.js), async-load
        // it now.
        const full = new Uint8Array(state.totalSize);
        let off = 0;
        for (const c of state.chunks) { full.set(c, off); off += c.byteLength; }
        let decoded;
        try { decoded = decodeArtifact(kind, artifact, full.buffer); }
        catch (e) {
          console.error('[inspector] final decode failed', artifact, e);
          return;
        }
        this.decoded.set(artifact, decoded);
        const modPromise = state.mod
          ? Promise.resolve(state.mod)
          : resolveRenderer({ kind, name: artifact });
        modPromise.then((mod) => {
          try { mod.render(state.ctx, decoded); }
          catch (e) {
            console.error('[inspector] render failed', artifact, e);
            state.ctx.renderHTML(`<div class="render-error">render error: ${e.message}</div>`);
          }
        });
      }
    }
  }

  // Drop any in-flight streams — called on RUN (new run replaces state) and
  // on ERROR. The renderer's begun layers go with their RenderContext via
  // clearAll's ctx.destroy().
  _dropAllStreams() {
    this._streams.clear();
    this._renderedViaStream.clear();
  }

  removeArtifact(layerId) {
    const ctx = this.contexts.get(layerId);
    if (!ctx) return;
    ctx.destroy();
    this.contexts.delete(layerId);
    this._contextInfo.delete(layerId);
    this._clearPendingFor(layerId);
    this.legendOverlay?.unregisterByContext(layerId);
  }

  clearAll() {
    for (const layerId of Array.from(this.contexts.keys())) {
      this.removeArtifact(layerId);
    }
    this.viewer.clearAll();
    this.overlay.clearAll();
    this.decoded.clear();
    this.layerIdByArtifact.clear();
    this._contextInfo.clear();
    this._pendingSiblings.clear();
    this.legendOverlay?.clearAll();
    this._dropAllStreams();
  }

  // Resolve kind from the cached pipeline catalog. The WS protocol puts
  // `kind` in CATALOG_RESPONSE.artifacts (not in ARTIFACT_HEADER), so we
  // require a CATALOG per-pipeline query before runs fire. app.js pre-warms
  // the catalog on pipeline selection.
  _kindFor(artifactName) {
    if (!this._currentPipeline) return null;
    const list = this.catalog.artifactsByPipeline.get(this._currentPipeline);
    if (!list) return null;
    const entry = list.find((a) => a.name === artifactName);
    return entry?.kind ?? null;
  }
}
