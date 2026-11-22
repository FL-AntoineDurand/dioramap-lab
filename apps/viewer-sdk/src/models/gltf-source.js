// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Shared GLB loading dispatch for model-manager.js + ghost-placement.js.
//
// `addModel` and `startGhostPlace` accept either a `url` (legacy) or
// `data` (ArrayBuffer | Blob, since 0.9.0). The url path goes through
// `loader.load()` (browser fetch → CSP `connect-src` applies). The data
// path goes through `loader.parse()` (no network — embedder controls
// bytes, useful for procedural / pre-fetched / configurator-generated
// models, and lets strict-CSP embedders avoid allowlisting their model
// CDN). See PLAN-VIEWER-SDK-PORT §5.7 (`data:` migration note).

/**
 * Convert any accepted `data` value to an ArrayBuffer.
 * @param {ArrayBuffer | Blob} data
 * @returns {Promise<ArrayBuffer>}
 */
async function toArrayBuffer(data) {
  if (data instanceof ArrayBuffer) return data;
  if (typeof Blob !== 'undefined' && data instanceof Blob) return await data.arrayBuffer();
  // Typed-array views are tolerated (.buffer is the underlying ArrayBuffer).
  if (ArrayBuffer.isView(data)) {
    return data.byteOffset === 0 && data.byteLength === data.buffer.byteLength
      ? data.buffer
      : data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
  }
  throw new TypeError(
    "addModel/startGhostPlace: `data` must be an ArrayBuffer, Blob, or typed-array view",
  );
}

/**
 * Load a glTF/GLB from either a URL (HTTP fetch via the browser) or
 * raw bytes (no network). Exactly one of `url` / `data` must be set.
 * Returns the parsed gltf object as Three's GLTFLoader produces it.
 *
 * @param {import('three/addons/loaders/GLTFLoader.js').GLTFLoader} loader
 * @param {{ url?: string, data?: ArrayBuffer | Blob | ArrayBufferView }} source
 * @returns {Promise<{ scene: import('three').Group, [k: string]: unknown }>}
 */
export async function loadGltf(loader, { url, data } = {}) {
  if (url && data) {
    throw new Error("addModel/startGhostPlace: pass either `url` or `data`, not both");
  }
  if (data) {
    const buf = await toArrayBuffer(data);
    return new Promise((resolve, reject) => {
      // path='' — GLB embeds all resources; we never resolve external URIs.
      // Embedders who want loose .gltf with sibling files should host them
      // and use the `url` path so the browser handles relative-URI resolution
      // (and so connect-src/img-src apply to texture fetches).
      loader.parse(buf, '', resolve, reject);
    });
  }
  if (url) {
    return new Promise((resolve, reject) => {
      loader.load(url, resolve, undefined, reject);
    });
  }
  throw new Error("addModel/startGhostPlace: must provide `url` or `data`");
}
