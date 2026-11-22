// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Renderer loader — resolves an artifact to its visualizer module per
// the three-subtype model documented in v3/docs/VISUALIZERS.md §5:
//
//   1. step-visualizer at  v3/steps/<step>/viz/<artifact>.viz.js
//      (auto-discovered via Vite glob; eager so streaming dispatch can
//      resolve sync without racing through an await).
//   2. builtin-visualizer keyed on artifact kind
//      (apps/lab-ui/src/renderers/builtin/<kind>.js).
//
// composite-visualizers (apps/scene-base/src/viz/) are the third
// subtype; they don't go through this loader — the composite-views
// panel wires them directly via vizCatalog.

import { AK } from '@dioramap/protocol';
import * as pointColumn from './builtin/point-column.js';
import * as raster from './builtin/raster.js';
import * as imageJpeg from './builtin/image-jpeg.js';
import * as metadataJson from './builtin/metadata-json.js';
import * as lineSegments from './builtin/line-segments.js';

const BUILTIN_BY_KIND = {
  [AK.POINT_COLUMN]: pointColumn,
  [AK.RASTER_F32]: raster,
  [AK.IMAGE_JPEG]: imageJpeg,
  [AK.OPAQUE_JSON]: metadataJson,
  [AK.ARRAY_INLINE]: metadataJson,
  [AK.ARRAY_BY_HASH]: metadataJson,
  [AK.GEOMETRY_PRIMITIVES]: lineSegments,
};

// L11: Vite-glob discovery of step-visualizers. eager:true so the
// streaming dispatch path (resolveRendererSync) can return the module
// immediately. Each `.viz.js` is small (tens to a few hundred LOC),
// total bundle impact is minor and only grows as the step library does.
//
// Path pattern uses an absolute-from-vite-root path so we don't have
// to count `..` segments from this file. The vite root is
// apps/lab-ui/, but server.fs.allow extends to the v3 root (see
// vite.config.js); the glob resolves relative to this file with `..`
// hops to reach v3/steps/.
const STEP_VIZ_MODULES = import.meta.glob(
  '../../../../steps/*/viz/*.viz.js',
  { eager: true },
);

// Build artifact-name → module map. Filename (minus `.viz.js`) is the
// artifact name. The producing step is the directory name, kept in a
// side map for diagnostics (we can't attach it to the module object
// because Vite hands eager-glob modules out as frozen Proxies in dev).
const STEP_VIZ_BY_ARTIFACT = new Map();
const STEP_VIZ_PRODUCER = new Map();
for (const [path, mod] of Object.entries(STEP_VIZ_MODULES)) {
  const m = path.match(/\/steps\/([^/]+)\/viz\/(.+)\.viz\.js$/);
  if (!m) continue;
  const [, step, artifact] = m;
  if (STEP_VIZ_BY_ARTIFACT.has(artifact)) {
    console.warn(
      `[loader] duplicate step-visualizer for artifact '${artifact}': ` +
      `existing=${STEP_VIZ_PRODUCER.get(artifact)}, new=${step}. Keeping first.`,
    );
    continue;
  }
  STEP_VIZ_BY_ARTIFACT.set(artifact, mod);
  STEP_VIZ_PRODUCER.set(artifact, step);
}

/**
 * Async — returns a promise resolving to a module with `render(ctx, data)`.
 * Step-visualizer wins over builtin when present.
 * @param {{kind: number, name: string}} args
 */
export function resolveRenderer({ kind, name }) {
  const stepViz = STEP_VIZ_BY_ARTIFACT.get(name);
  if (stepViz) return Promise.resolve(stepViz);
  const mod = BUILTIN_BY_KIND[kind];
  if (!mod) return Promise.resolve(FALLBACK_UNKNOWN);
  return Promise.resolve(mod);
}

/**
 * Sync — used by the streaming inspector path so back-to-back chunks
 * don't race through an `await`. Step-visualizers are eager-imported
 * (see STEP_VIZ_MODULES above) so they resolve sync just like
 * builtins.
 */
export function resolveRendererSync({ kind, name }) {
  const stepViz = STEP_VIZ_BY_ARTIFACT.get(name);
  if (stepViz) return stepViz;
  return BUILTIN_BY_KIND[kind] ?? null;
}

const FALLBACK_UNKNOWN = {
  render(ctx, data) {
    ctx.renderHTML(
      `<div class="render-placeholder">no renderer for kind ${data.kind ?? '?'}</div>`,
    );
  },
};
