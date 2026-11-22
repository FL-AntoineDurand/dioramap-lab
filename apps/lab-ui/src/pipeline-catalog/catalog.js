// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Pipeline catalog — L1 slice.
//
// Owns:
//   - the list of pipeline names (bare CATALOG response)
//   - per-pipeline artifact lists (CATALOG with a `pipeline` arg)
//   - a tiny request queue so the callsite can `await` per-pipeline lookups
//
// L2+ will extend this with test-case fixtures and step manifests.

export class PipelineCatalog extends EventTarget {
  constructor(client) {
    super();
    this.client = client;
    this.pipelines = [];
    this.artifactsByPipeline = new Map(); // pipeline → [{name, kind, producer, visibility}]
    this.stepsByPipeline = new Map();     // pipeline → [{name, version, reads, writes, params}]
    this.pendingPipelineRequests = new Map(); // pipeline → {resolve, reject}

    client.addEventListener('catalog', (ev) => this._onCatalogResponse(ev.detail));
    client.addEventListener('error-msg', (ev) => this._onProtocolError(ev.detail));
  }

  // Fetch the pipeline name list. Emits `pipelines-loaded` when ready.
  requestPipelines() {
    this.client.catalog();
  }

  // Returns a promise that resolves with `{ artifacts, steps }` for `pipeline`.
  // Caches on success.
  requestArtifacts(pipeline) {
    if (this.artifactsByPipeline.has(pipeline)) {
      return Promise.resolve({
        artifacts: this.artifactsByPipeline.get(pipeline),
        steps: this.stepsByPipeline.get(pipeline) ?? [],
      });
    }
    const existing = this.pendingPipelineRequests.get(pipeline);
    if (existing) return existing.promise;

    let resolve, reject;
    const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
    this.pendingPipelineRequests.set(pipeline, { promise, resolve, reject });
    this.client.catalog(pipeline);
    return promise;
  }

  _onCatalogResponse(msg) {
    if (msg.pipeline) {
      const pending = this.pendingPipelineRequests.get(msg.pipeline);
      this.pendingPipelineRequests.delete(msg.pipeline);
      const artifacts = msg.artifacts ?? [];
      const steps = msg.steps ?? [];
      this.artifactsByPipeline.set(msg.pipeline, artifacts);
      this.stepsByPipeline.set(msg.pipeline, steps);
      this.dispatchEvent(new CustomEvent('artifacts-loaded', {
        detail: { pipeline: msg.pipeline, artifacts, steps },
      }));
      pending?.resolve({ artifacts, steps });
    } else {
      this.pipelines = msg.pipelines ?? [];
      this.dispatchEvent(new CustomEvent('pipelines-loaded', {
        detail: { pipelines: this.pipelines },
      }));
    }
  }

  _onProtocolError(msg) {
    // UNKNOWN_PIPELINE errors should surface back to whoever is awaiting the
    // specific pipeline's artifact list. Without a request-id field on the
    // wire, we can only match by the error's `msg` field (which carries the
    // pipeline name in UNKNOWN_PIPELINE).
    if (msg.code === 'UNKNOWN_PIPELINE' && msg.msg) {
      const pending = this.pendingPipelineRequests.get(msg.msg);
      if (pending) {
        this.pendingPipelineRequests.delete(msg.msg);
        pending.reject(new Error(`UNKNOWN_PIPELINE: ${msg.msg}`));
      }
    }
  }
}
