// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Section-aware roof-graph renderer.
//
// Probes which sections a `<step>-roof-graph` artifact contains and
// emits one viewer layer per visible primitive (planes / ridges /
// corners / edges / ridge-lines / anchors / mr-points). Each per-
// step viz file (`apps/lab-ui/src/.../<step>-roof-graph.viz.js`) is
// a 5-line delegate that calls `renderRoofGraph(ctx, rg, snapshotName)`.
//
// The benefit: viewer overlays group naturally under "Roof Graph /
// <snapshot>" with subcategory = snapshot name (matches bench-v2
// roofGraphToLayers convention), so a pipeline run that emits
// `boundary-signals-roof-graph`, `boundary-corners-roof-graph`,
// etc. shows up in the lab-ui as a stack of toggleable snapshots.
//
// Color choices intentionally match bench-v2's roofGraphToLayers
// (lib/pipeline/steps/boundary-edges/roof-graph.js) so v2/v3
// renders look identical side-by-side during the port.

import * as THREE from 'three';
import { decodeRoofGraph, pickRoofGraph, RG_SECT, RG_RIDGE_TYPE, RG_CORNER_SOURCE } from
  './roof-graph-decoder.js';

// ── Color palette (matches bench-v2 boundary-edges/roof-graph.js) ──

const RIDGE_TYPE_COLOR = {
  ridge:    0xffd600,
  hip:      0xff9100,
  valley:   0x00ccdd,
  eave:     0x3498db,
  step:     0xe040fb,
  flashing: 0x90a4ae,
  unknown:  0xaaaaaa,
};

const CORNER_SOURCE_COLOR = {
  'ridge-intersection': 0xff2222,
  'ridge-end':          0xffd600,
  'polygon-merge':      0xff88cc,
  'concave':            0x00ddff,
  'convex':             0xff6600,
  'mr-group':           0xff00ff,
  'mr-nearby':          0xff88cc,
  'coplanar':           0x66bb6a,
};

const PLANE_CENTROID_COLOR = 0xcccccc;
const ANCHOR_COLOR         = 0xfdd835;
const EDGE_COLOR           = 0x90caf9;
const EDGE_RIDGE_COLOR     = 0xff8800;   // shared between ≥2 planes
const EDGE_EAVE_COLOR      = 0x4caf50;   // single plane (eave / wall outline)
const MR_POINT_COLOR       = 0xff00ff;
const LINK_COLOR           = 0x555555;

// Perimeter-tag palette — mirrors v2's classify-stage colour intent
// (perimeter = red eave candidates, shared = green, overhang =
// blue/cyan, interior-shared = grey, wall = brown, level-transition =
// orange).
const PERIM_TAG_COLOR = {
  perimeter:         0xff2222,
  shared:            0x4caf50,
  'overhang-top':    0x29b6f6,
  'overhang-bottom': 0x00838f,
  'interior-shared': 0x9e9e9e,
  wall:              0x8d6e63,
  'level-transition':0xff9100,
  untagged:          0x444444,
};

// Outward XZ arrow colour-coded by tag (same palette as the edge).
const OUTWARD_ARROW_LEN_M = 1.0;

// Per-plane hue ramp. Matches bench-v2 app.js convention:
//   planeHue(g) = (g * 137) % 360  — golden-angle spread, distinct
//   hues even at high plane counts. saturation 75%, lightness 60%.
function planeHueHex(planeId) {
  const c = new THREE.Color();
  const h = ((planeId * 137) % 360) / 360;
  c.setHSL(h, 0.75, 0.60);
  return c.getHex();
}

// Color by plane_type (rg_plane_type_t) for classifier verification:
// roof=blue, wall=orange, ground=green, unclassified=grey.
function planeTypeColor(planeType) {
  switch (planeType) {
    case 1: return 0x4a90d9; // roof
    case 2: return 0xe8883a; // wall
    case 3: return 0x5ca85c; // ground
    default: return 0x888888; // unclassified
  }
}

// ── helpers ────────────────────────────────────────────────────────

function ensureOrigin(ctx, samplePoints) {
  let origin = ctx.viewer.getSceneOrigin?.();
  if (origin) return origin;
  // Compute centroid of sample (up to 1024 points).
  const n = Math.min(samplePoints.length, 1024);
  if (n === 0) return { x: 0, y: 0, z: 0 };
  let sx = 0, sy = 0, sz = 0;
  for (let i = 0; i < n; i++) {
    const p = samplePoints[i];
    sx += p[0]; sy += p[1]; sz += p[2];
  }
  origin = { x: sx / n, y: sy / n, z: sz / n };
  ctx.viewer.setSceneOrigin?.(origin);
  return origin;
}

function makeLineSegments(segments, origin, getColor) {
  // Mirrors bench-v2's pattern (lib's app.js#L4286-4290):
  //   LineSegments(geom with Float32 position + color attrs,
  //                LineBasicMaterial({ vertexColors: true, linewidth: 3 }))
  // Note: WebGL clamps linewidth to 1px in practice; the value is kept
  // for parity with v2 in case a future Line2 fallback honours it.
  // Explicit bounding sphere — auto-compute on first render can be late
  // enough that EDL's RenderPass culls the object before computing.
  const positions = new Float32Array(segments.length * 6);
  const colors    = new Float32Array(segments.length * 6);
  for (let i = 0; i < segments.length; i++) {
    const s = segments[i];
    positions[i*6 + 0] = s.from[0] - origin.x;
    positions[i*6 + 1] = s.from[1] - origin.y;
    positions[i*6 + 2] = s.from[2] - origin.z;
    positions[i*6 + 3] = s.to[0]   - origin.x;
    positions[i*6 + 4] = s.to[1]   - origin.y;
    positions[i*6 + 5] = s.to[2]   - origin.z;
    const hex = getColor ? getColor(s) : 0xffffff;
    const c = new THREE.Color(hex);
    colors[i*6 + 0] = c.r; colors[i*6 + 1] = c.g; colors[i*6 + 2] = c.b;
    colors[i*6 + 3] = c.r; colors[i*6 + 4] = c.g; colors[i*6 + 5] = c.b;
  }
  const geom = new THREE.BufferGeometry();
  geom.setAttribute('position', new THREE.BufferAttribute(positions, 3));
  geom.setAttribute('color',    new THREE.BufferAttribute(colors, 3));
  geom.computeBoundingSphere();
  const mat = new THREE.LineBasicMaterial({
    vertexColors: true,
    linewidth: 3,
    transparent: true,
    opacity: 0.95,
    depthWrite: false,
    depthTest: false,  // always on top — at certain camera headings,
                       // lidar points wrote depth values closer to camera
                       // than our line endpoints, which made lines fail
                       // LEQUAL and vanish. renderOrder=100 keeps the
                       // draw-order right; depthTest=false makes the
                       // visibility independent of what's behind.
  });
  const lines = new THREE.LineSegments(geom, mat);
  lines.renderOrder = 100;  // draw on top of points
  return { object: lines, geom, mat };
}

function makeSpheres(centers, radius, origin, getColor, getMeta) {
  // Mirrors bench-v2 (app.js#L4296-4310): individual Mesh per sphere,
  // grouped under a Group so the scene-add/remove API treats them as
  // one layer. InstancedMesh + EDL had subtle depth-test interactions
  // in v3; the per-Mesh path is a few KB of overhead per building but
  // bulletproof. `getMeta(i)` (optional) is stashed on each Mesh's
  // userData so future raycaster / inspector hooks can surface per-
  // sphere context (corner names "RI3", "P5C2", etc.).
  const group = new THREE.Group();
  const sphereGeom = new THREE.SphereGeometry(1, 10, 8);  // unit radius;
                                                          // scale per-instance
  const mats = [];
  for (let i = 0; i < centers.length; i++) {
    const c = centers[i];
    const hex = getColor ? getColor(i) : 0xffffff;
    const mat = new THREE.MeshBasicMaterial({ color: hex });
    mats.push(mat);
    const mesh = new THREE.Mesh(sphereGeom, mat);
    mesh.position.set(c[0] - origin.x, c[1] - origin.y, c[2] - origin.z);
    mesh.scale.setScalar(radius);
    if (getMeta) {
      const meta = getMeta(i);
      if (meta) {
        mesh.userData = meta;
        if (meta.name) mesh.name = meta.name;
      }
    }
    group.add(mesh);
  }
  group.renderOrder = 101;  // draw on top of points
  return {
    object: group,
    geom: sphereGeom,
    mat: { dispose: () => { for (const m of mats) m.dispose(); } },
  };
}

// ── per-section renderers ──────────────────────────────────────────

export function renderPlanes(ctx, rg, opts = {}) {
  if (!rg.has(RG_SECT.PLANES)) return;
  const planes = [...rg.planes()].filter(p => p.nPts > 0);
  if (planes.length === 0) return;
  const origin = ensureOrigin(ctx, planes.map(p => p.centroid));
  const radius = opts.centroidRadius ?? 0.20;
  const { object, geom, mat } = makeSpheres(
    planes.map(p => p.centroid),
    radius, origin,
    () => PLANE_CENTROID_COLOR,
    // Per-sphere pick info (surfaced via the picker's static-userData tier).
    (i) => ({
      planeId: planes[i].id,
      nPts: planes[i].nPts,
      tilt: `${planes[i].tiltDeg}°`,
      status: planes[i].rejected ? `rejected:${planes[i].rejectionReason}` : 'accepted',
    }),
  );
  object.name = `${opts.snapshotName}-plane-centroids`;
  ctx.render3D({
    kind: 'mesh', object,
    layer: {
      id: `${opts.layerIdPrefix}/plane-centroids`,
      label: `Plane centroids (${planes.length})`,
      path: opts.layerPath,
      defaultVisible: opts.defaultVisible ?? false,
    },
  });
  ctx.onCleanup(() => { geom.dispose(); mat.dispose(); });
}

export function renderRidges(ctx, rg, opts = {}) {
  if (!rg.has(RG_SECT.RIDGES)) return;
  const ridges = [...rg.ridges()].filter(r => r.length > 0);
  if (ridges.length === 0) return;
  const origin = ensureOrigin(ctx, ridges.flatMap(r => [r.from, r.to]));
  const segs = ridges.map(r => ({ from: r.from, to: r.to, type: r.type }));
  const { object, geom, mat } = makeLineSegments(segs, origin,
    s => RIDGE_TYPE_COLOR[s.type] ?? RIDGE_TYPE_COLOR.unknown);
  object.name = `${opts.snapshotName}-ridges`;
  ctx.render3D({
    kind: 'lines', object,
    layer: {
      id: `${opts.layerIdPrefix}/ridges`,
      label: `Ridges (${ridges.length})`,
      path: opts.layerPath,
      defaultVisible: opts.defaultVisible ?? false,
    },
  });
  ctx.onCleanup(() => { geom.dispose(); mat.dispose(); });
}

// Text sprite for one label — bench-v2 pattern (ui/public/app.js#L4359-4391):
// canvas sized to fit text; stroke + fill with the label colour; CanvasTexture
// in a SpriteMaterial with depthTest:false so labels never get occluded by
// the geometry they annotate. Caller owns the Sprite + cleanup.
function makeTextSprite(text, color, opts = {}) {
  const FONT_PX = opts.fontPx ?? 28;
  const PAD_X = 12, PAD_Y = 8;
  const measure = document.createElement('canvas').getContext('2d');
  measure.font = `bold ${FONT_PX}px monospace`;
  const textW = Math.max(1, Math.ceil(measure.measureText(text).width));
  const cw = textW + PAD_X * 2;
  const ch = FONT_PX + PAD_Y * 2;
  const c = document.createElement('canvas');
  c.width = cw; c.height = ch;
  const ctx2 = c.getContext('2d');
  ctx2.font = `bold ${FONT_PX}px monospace`;
  ctx2.textAlign = 'center';
  ctx2.textBaseline = 'middle';
  ctx2.strokeStyle = '#000000';
  ctx2.lineWidth = 4;
  ctx2.strokeText(text, cw / 2, ch / 2);
  ctx2.fillStyle = color || '#ffffff';
  ctx2.fillText(text, cw / 2, ch / 2);
  const tex = new THREE.CanvasTexture(c);
  const mat = new THREE.SpriteMaterial({ map: tex, depthTest: false, transparent: true });
  const sprite = new THREE.Sprite(mat);
  const s = opts.scale ?? 0.5;
  sprite.scale.set(s * (cw / ch), s, 1);
  return { sprite, tex, mat };
}

export function renderCornerLabels(ctx, rg, opts = {}) {
  if (!rg.has(RG_SECT.CORNERS)) return;
  // Only corners that actually have a name get a label sprite. RI corners
  // get "RI<n>", concave get "P<plane>C<n>", convex get "P<plane>X<n>"
  // (see v7-boundary-signals-impl.c).
  const named = [];
  for (const c of rg.corners()) {
    if (c.name) named.push(c);
  }
  if (named.length === 0) return;

  // Lazy build — corner labels are the biggest perf hit on the
  // roof-graph snapshots (Canvas2D fillText + GPU texture upload per
  // label). Pay the construction cost only when the user toggles the
  // layer on; dispose all sprite textures + materials on toggle-off.
  // z+0.3 up-offset — v3 scene is z-up (see scene-base/create-scene.js).
  // Sprites are billboard-quads so the offset just shifts them above the
  // sphere they annotate; matches v2 boundary-edges/roof-graph.js#L466.
  const LABEL_Z_OFFSET = 0.3;
  ctx.render3D({
    layer: {
      id: `${opts.layerIdPrefix}/corner-labels`,
      label: `Corner labels (${named.length})`,
      path: opts.layerPath,
      defaultVisible: opts.defaultVisible ?? false,
    },
    build: () => {
      const origin = ensureOrigin(ctx, named.map(c => c.point));
      const group = new THREE.Group();
      const disposers = [];
      for (const c of named) {
        const color = `#${(CORNER_SOURCE_COLOR[c.source] ?? 0xffffff).toString(16).padStart(6, '0')}`;
        const { sprite, tex, mat } = makeTextSprite(c.name, color, { scale: 0.5 });
        sprite.position.set(
          c.point[0] - origin.x,
          c.point[1] - origin.y,
          c.point[2] - origin.z + LABEL_Z_OFFSET,
        );
        sprite.renderOrder = 103;  // above corners (101) + nearby projections (102)
        sprite.userData = { id: c.id, name: c.name, source: c.source };
        group.add(sprite);
        disposers.push(() => { mat.dispose(); tex.dispose(); });
      }
      group.renderOrder = 103;
      return {
        object: group,
        dispose: () => { for (const d of disposers) d(); },
      };
    },
  });
}

// Plane id label at each plane centroid — mirrors renderCornerLabels.
// Lazy-built so the Canvas2D fillText + GPU texture upload cost is only
// paid when the user toggles the layer on. Coloured by plane hue so the
// label matches the plane's colour elsewhere in the scene.
export function renderPlaneLabels(ctx, rg, opts = {}) {
  if (!rg.has(RG_SECT.PLANES)) return;
  const planes = [...rg.planes()].filter(p => p.nPts > 0);
  if (planes.length === 0) return;

  const LABEL_Z_OFFSET = 0.3;  // z-up scene; lift the label above the centroid sphere
  ctx.render3D({
    layer: {
      id: `${opts.layerIdPrefix}/plane-labels`,
      label: `Plane labels (${planes.length})`,
      path: opts.layerPath,
      defaultVisible: opts.defaultVisible ?? false,
    },
    build: () => {
      const origin = ensureOrigin(ctx, planes.map(p => p.centroid));
      const group = new THREE.Group();
      const disposers = [];
      for (const p of planes) {
        // Label colored per-plane (matches the Polygons layer); the [type] text keeps
        // the classification handy without turning the Plane types layer on.
        const color = `#${planeHueHex(p.id).toString(16).padStart(6, '0')}`;
        const { sprite, tex, mat } = makeTextSprite(`${p.id} [${p.planeTypeLabel}]`, color, { scale: 0.6 });
        sprite.position.set(
          p.centroid[0] - origin.x,
          p.centroid[1] - origin.y,
          p.centroid[2] - origin.z + LABEL_Z_OFFSET,
        );
        sprite.renderOrder = 104;  // above corner labels (103)
        sprite.userData = { planeId: p.id };
        group.add(sprite);
        disposers.push(() => { mat.dispose(); tex.dispose(); });
      }
      group.renderOrder = 104;
      return {
        object: group,
        dispose: () => { for (const d of disposers) d(); },
      };
    },
  });
}

export function renderCorners(ctx, rg, opts = {}) {
  if (!rg.has(RG_SECT.CORNERS)) return;
  const corners = [...rg.corners()];
  if (corners.length === 0) return;
  const origin = ensureOrigin(ctx, corners.map(c => c.point));
  const radius = opts.cornerRadius ?? 0.14;

  // Lazy + instanced: one InstancedMesh draw call instead of N Meshes.
  // Per-instance Matrix4 (position + uniform scale) and per-instance
  // colour (via setColorAt → instanceColor attribute). Picking returns
  // intersect.instanceId; map back to corners[i] for inspector context.
  ctx.render3D({
    layer: {
      id: `${opts.layerIdPrefix}/corners`,
      label: `Corners (${corners.length})`,
      path: opts.layerPath,
      defaultVisible: opts.defaultVisible ?? false,
    },
    build: () => {
      const geom = new THREE.SphereGeometry(1, 10, 8);  // unit radius
      const mat  = new THREE.MeshBasicMaterial({ color: 0xffffff });
      const mesh = new THREE.InstancedMesh(geom, mat, corners.length);
      const tmpMat = new THREE.Matrix4();
      const tmpCol = new THREE.Color();
      const cornersMeta = [];
      for (let i = 0; i < corners.length; i++) {
        const c = corners[i];
        tmpMat.makeScale(radius, radius, radius);
        tmpMat.setPosition(
          c.point[0] - origin.x,
          c.point[1] - origin.y,
          c.point[2] - origin.z,
        );
        mesh.setMatrixAt(i, tmpMat);
        tmpCol.setHex(CORNER_SOURCE_COLOR[c.source] ?? 0xffffff);
        mesh.setColorAt(i, tmpCol);
        cornersMeta.push({
          id: c.id, name: c.name, source: c.source,
          planeIds: c.planeIds, ridgeIds: c.ridgeIds,
        });
      }
      mesh.instanceMatrix.needsUpdate = true;
      if (mesh.instanceColor) mesh.instanceColor.needsUpdate = true;
      mesh.name = `${opts.snapshotName}-corners`;
      mesh.renderOrder = 101;
      // Index for raycaster: intersect.instanceId → cornersMeta[i]
      mesh.userData.instances = cornersMeta;
      return {
        object: mesh,
        dispose: () => { geom.dispose(); mat.dispose(); },
      };
    },
  });
}

export function renderEdges(ctx, rg, opts = {}) {
  if (!rg.has(RG_SECT.EDGES) || !rg.has(RG_SECT.CORNERS)) return;
  // Edges are stored as cornerId pairs; resolve via corners section.
  const corners = new Map();
  for (const c of rg.corners()) corners.set(c.id, c);
  const eaveSegs  = [];   // planeIds.length == 1 — eave / wall outline
  const ridgeSegs = [];   // planeIds.length >= 2 — shared between planes
  for (const e of rg.edges()) {
    const a = corners.get(e.cornerA);
    const b = corners.get(e.cornerB);
    if (!a || !b) continue;
    const nPlanes = (e.planeIds?.length) ?? 0;
    const seg = { from: a.point, to: b.point };
    if (nPlanes >= 2) ridgeSegs.push(seg);
    else              eaveSegs.push(seg);
  }
  const totalSegs = eaveSegs.length + ridgeSegs.length;
  if (totalSegs === 0) return;

  const subPath = [...(opts.layerPath || []), 'Edges'];
  const allSamples = [];
  for (const s of eaveSegs)  allSamples.push(s.from, s.to);
  for (const s of ridgeSegs) allSamples.push(s.from, s.to);
  const origin = ensureOrigin(ctx, allSamples);

  // Two sub-layers under "Edges":
  //   * Eave (single-plane)  — most polygon edges
  //   * Ridge (≥2 planes)    — shared edges (true ridges + interior shared seams)
  // Toggled independently. Default both off (mirrors original edges default).
  if (eaveSegs.length > 0) {
    const { object, geom, mat } = makeLineSegments(eaveSegs, origin, () => EDGE_EAVE_COLOR);
    object.name = `${opts.snapshotName}-edges-single`;
    ctx.render3D({
      kind: 'lines', object,
      layer: {
        id: `${opts.layerIdPrefix}/edges-single`,
        label: `Single-plane edges (${eaveSegs.length})`,
        path: subPath,
        defaultVisible: opts.defaultVisible ?? false,
      },
    });
    ctx.onCleanup(() => { geom.dispose(); mat.dispose(); });
  }
  if (ridgeSegs.length > 0) {
    const { object, geom, mat } = makeLineSegments(ridgeSegs, origin, () => EDGE_RIDGE_COLOR);
    object.name = `${opts.snapshotName}-edges-shared`;
    ctx.render3D({
      kind: 'lines', object,
      layer: {
        id: `${opts.layerIdPrefix}/edges-shared`,
        label: `Shared edges (${ridgeSegs.length})`,
        path: subPath,
        defaultVisible: opts.defaultVisible ?? false,
      },
    });
    ctx.onCleanup(() => { geom.dispose(); mat.dispose(); });
  }
}

// Per-plane closed polygon ring from POLYGON_PATHS. One LineLoop per
// plane, plane-hued via the (g * 137) golden-angle ramp. Sub-layers
// under "Polygons" so the visibility tree lets the user toggle each
// plane individually; lazy build so the geometry is only constructed
// when first made visible.
export function renderPolygonPaths(ctx, rg, opts = {}) {
  if (!rg.has(RG_SECT.POLYGON_PATHS) || !rg.has(RG_SECT.CORNERS)) return;
  const corners = new Map();
  for (const c of rg.corners()) corners.set(c.id, c);
  const planeTypeById = new Map();  // for color-by-plane_type
  for (const pl of rg.planes()) planeTypeById.set(pl.id, pl.planeType);
  const paths = [...rg.polygonPaths()].filter(p => (p.corners?.length ?? 0) >= 3);
  if (paths.length === 0) return;

  const subPath = [...(opts.layerPath || []), 'Polygons'];

  // Sample points up front for scene-origin selection.
  const samples = [];
  for (const p of paths) {
    for (let i = 0; i < p.corners.length && samples.length < 1024; i++) {
      const c = corners.get(p.corners[i]);
      if (c) samples.push(c.point);
    }
  }
  const origin = ensureOrigin(ctx, samples);

  for (const p of paths) {
    const layerId = `${opts.layerIdPrefix}/polygons/${p.planeId}`;
    ctx.render3D({
      layer: {
        id: layerId,
        label: `Plane ${p.planeId} (${p.corners.length}v)`,
        path: subPath,
        defaultVisible: opts.defaultVisible ?? false,
      },
      build: () => {
        const n = p.corners.length;
        const positions = new Float32Array(n * 3);
        let ok = true;
        for (let i = 0; i < n; i++) {
          const c = corners.get(p.corners[i]);
          if (!c) { ok = false; break; }
          positions[i*3 + 0] = c.point[0] - origin.x;
          positions[i*3 + 1] = c.point[1] - origin.y;
          positions[i*3 + 2] = c.point[2] - origin.z;
        }
        if (!ok) return { object: new THREE.Group(), dispose: () => {} };
        const geom = new THREE.BufferGeometry();
        geom.setAttribute('position', new THREE.BufferAttribute(positions, 3));
        geom.computeBoundingSphere();
        const mat = new THREE.LineBasicMaterial({
          color: planeHueHex(p.planeId),  // per-plane color — to tell planes apart
          linewidth: 3,
          transparent: true,
          opacity: 0.95,
          depthWrite: false,
          depthTest: false,
        });
        const loop = new THREE.LineLoop(geom, mat);
        loop.renderOrder = 100;
        loop.name = `${opts.snapshotName}-polygon-${p.planeId}`;
        return {
          object: loop,
          dispose: () => { geom.dispose(); mat.dispose(); },
        };
      },
    });
  }

  // Dedicated "Plane types" layer — the same polygon outlines colored by plane_type
  // (roof=blue, wall=orange, ground=green, unclassified=grey). One toggle, sibling of
  // the per-plane-colored "Polygons" group, so you can flip between "tell planes apart"
  // and "check the classification".
  ctx.render3D({
    layer: {
      id: `${opts.layerIdPrefix}/plane-types`,
      label: 'Plane types',
      path: [...(opts.layerPath || [])],
      defaultVisible: false,
    },
    build: () => {
      const group = new THREE.Group();
      const disposers = [];
      for (const p of paths) {
        const n = p.corners.length;
        const positions = new Float32Array(n * 3);
        let ok = true;
        for (let i = 0; i < n; i++) {
          const c = corners.get(p.corners[i]);
          if (!c) { ok = false; break; }
          positions[i*3 + 0] = c.point[0] - origin.x;
          positions[i*3 + 1] = c.point[1] - origin.y;
          positions[i*3 + 2] = c.point[2] - origin.z;
        }
        if (!ok) continue;
        const geom = new THREE.BufferGeometry();
        geom.setAttribute('position', new THREE.BufferAttribute(positions, 3));
        const mat = new THREE.LineBasicMaterial({
          color: planeTypeColor(planeTypeById.get(p.planeId) ?? 0),
          linewidth: 3, transparent: true, opacity: 0.95,
          depthWrite: false, depthTest: false,
        });
        const loop = new THREE.LineLoop(geom, mat);
        loop.renderOrder = 100;
        group.add(loop);
        disposers.push(() => { geom.dispose(); mat.dispose(); });
      }
      return { object: group, dispose: () => disposers.forEach(f => f()) };
    },
  });
}

export function renderNearbyProjections(ctx, rg, opts = {}) {
  if (!rg.has(RG_SECT.NEARBY_PROJECTIONS)) return;
  const perPlane = [...rg.nearbyProjections()].filter(p => p.items.length > 0);
  if (perPlane.length === 0) return;
  // Set scene origin from a flat sample across all planes if not yet set.
  const samplePts = [];
  for (const { items } of perPlane) {
    for (let i = 0; i < items.length && samplePts.length < 1024; i++) {
      samplePts.push(items[i].position);
    }
  }
  const origin = ensureOrigin(ctx, samplePts);

  // One Points layer per plane — toggle them individually in the tree.
  // All land under a shared "Nearby projections" subgroup so the
  // visibility tree stays tidy.
  const subPath = [...(opts.layerPath || []), 'Nearby projections'];
  const tmpColor = new THREE.Color();
  let total = 0;
  for (const { items } of perPlane) total += items.length;

  // Shared colour-bar legend — one spec object, registered once per
  // per-plane layer with a common `groupKey` so the legend overlay
  // dedupes to a single card regardless of how many of the per-plane
  // layers are visible. The colour ramp is fixed (bench-v2 domain
  // (-5, +5) m) so per-plane bars look consistent across snapshots.
  const HUE_STOPS = 9;
  const stops = [];
  for (let s = 0; s < HUE_STOPS; s++) {
    const t = s / (HUE_STOPS - 1);
    const hueDeg = 0.66 * (1 - t) * 360;
    stops.push(`hsl(${hueDeg.toFixed(0)},90%,50%)`);
  }
  const sharedLegendSpec = {
    groupKey: `${opts.layerIdPrefix}/nearby-legend`,
    title: `Nearby projections — signed distance (${total} pts across ${perPlane.length} planes)`,
    gradient: { min: -5, max: 5, stops },
    note: 'red dot = |sd|<0.2 m (filter survivors); ramp domain fixed at (-5, +5) m',
  };

  for (const { planeId, items } of perPlane) {
    const layerId = `${opts.layerIdPrefix}/nearby/${planeId}`;
    ctx.render3D({
      layer: {
        id: layerId,
        label: `Plane ${planeId} (${items.length})`,
        path: subPath,
        defaultVisible: opts.defaultVisible ?? false,
      },
      build: () => {
        const positions = new Float32Array(items.length * 3);
        const colors    = new Float32Array(items.length * 3);
        for (let i = 0; i < items.length; i++) {
          const p = items[i].position;
          positions[i * 3 + 0] = p[0] - origin.x;
          positions[i * 3 + 1] = p[1] - origin.y;
          positions[i * 3 + 2] = p[2] - origin.z;
          const sd = items[i].signedDist;
          if (Math.abs(sd) < 0.2) {
            tmpColor.setHex(0xff0000);
          } else {
            // hslRampHex from v2: hue 0.66*(1 - t), s=0.9, l=0.5, t in [0,1].
            const t = Math.max(0, Math.min(1, (sd + 5) / 10));
            tmpColor.setHSL(0.66 * (1 - t), 0.9, 0.5);
          }
          colors[i * 3 + 0] = tmpColor.r;
          colors[i * 3 + 1] = tmpColor.g;
          colors[i * 3 + 2] = tmpColor.b;
        }
        const geom = new THREE.BufferGeometry();
        geom.setAttribute('position', new THREE.BufferAttribute(positions, 3));
        geom.setAttribute('color',    new THREE.BufferAttribute(colors, 3));
        geom.computeBoundingSphere();
        const mat = new THREE.PointsMaterial({
          size: 0.08,
          sizeAttenuation: true,
          vertexColors: true,
        });
        const points = new THREE.Points(geom, mat);
        points.name = `${opts.snapshotName}-nearby-${planeId}`;
        points.renderOrder = 102;
        return {
          object: points,
          dispose: () => { geom.dispose(); mat.dispose(); },
        };
      },
    });
    // Register the same shared legend spec under EVERY per-plane
    // layer id. The legend-overlay dedupes by `groupKey` so only one
    // card renders regardless of which combination of planes is
    // visible — and the card is hidden iff every per-plane layer is off.
    ctx.renderLegend?.(sharedLegendSpec, layerId);
  }
}

// Internal helper: render one scalar field per plane (heat map).
// cfg = {
//   pickValue(grid) -> Float32Array | Uint16Array | null,
//   nodePositions(grid) -> { nodeX, nodeY, nodeZ } typed arrays OR
//                          null to fall back to (col, row) → 3D derivation,
//   range: { min, max }       — domain for the colour ramp
//   label, sublabel,           — viz tree labels
//   groupKey, legendNote,
//   layerIdSlug,
// }
function _renderCellHeatMap(ctx, grids, opts, cfg) {
  if (grids.length === 0) return;

  // Scene origin from sampled cell positions. All coords come out of
  // the decoder in BUILDING-LOCAL; we shift to TILE-WORLD here in F64.
  const sample = [];
  for (const g of grids) {
    if (sample.length >= 1024) break;
    const np = cfg.nodePositions(g);
    const total = g.nCols * g.nRows;
    const stride = Math.max(1, Math.floor(total / 64));
    const [cx, cy, cz] = g.centroid;
    if (np) {
      for (let i = 0; i < total && sample.length < 1024; i += stride) {
        sample.push([np.nodeX[i] + cx, np.nodeY[i] + cy, np.nodeZ[i] + cz]);
      }
    } else {
      for (let i = 0; i < total && sample.length < 1024; i += stride) {
        const col = i % g.nCols, row = (i / g.nCols) | 0;
        const u = g.gMinU + col * g.cellSize;
        const v = g.gMinV + row * g.cellSize;
        // Building-local cell node, then F64 shift to world.
        const lx = u * g.basisE1[0] + v * g.basisE2[0] - g.d * g.normal[0];
        const ly = u * g.basisE1[1] + v * g.basisE2[1] - g.d * g.normal[1];
        const lz = u * g.basisE1[2] + v * g.basisE2[2] - g.d * g.normal[2];
        sample.push([lx + cx, ly + cy, lz + cz]);
      }
    }
  }
  const origin = ensureOrigin(ctx, sample);
  // Tree shape: ".../Plane grids/Plane <id>/<field>" — group BY PLANE
  // first, then by field type, so the user toggles all of one plane's
  // grids together instead of jumping between four sibling subtrees.
  const planeGridsBasePath = [...(opts.layerPath || []), 'Plane grids'];
  const tmpColor = new THREE.Color();

  const HUE_STOPS = 9;
  const stops = [];
  for (let s = 0; s < HUE_STOPS; s++) {
    const t = s / (HUE_STOPS - 1);
    const hueDeg = 0.66 * (1 - t) * 360;
    stops.push(`hsl(${hueDeg.toFixed(0)},90%,50%)`);
  }
  const legendSpec = {
    groupKey: `${opts.layerIdPrefix}/${cfg.groupKey}`,
    title: cfg.label,
    gradient: { min: cfg.range.min, max: cfg.range.max, stops },
    note: cfg.legendNote,
  };

  const span = cfg.range.max - cfg.range.min;

  // Per-plane sweep: register one lazy layer per (field × plane) cell.
  // Each pays its build cost only when toggled on (every plane × every
  // field is hidden by default; on dahouet bldg-0 that's 4 × 31 = 124
  // potential Points objects per snapshot, all skipped at artifact time).
  for (const g of grids) {
    const arr = cfg.pickValue(g);
    if (!arr) continue;
    // Quick nValid pre-count — cheap, avoids registering a leaf that
    // would build into an empty Points object anyway.
    let nValid = 0;
    for (let i = 0; i < arr.length; i++) {
      const v = arr[i];
      if (arr instanceof Uint16Array) { if (v !== 0) nValid++; }
      else if (!Number.isNaN(v))      { nValid++; }
    }
    if (nValid === 0) continue;

    const planeId = g.planeId;
    const layerId = `${opts.layerIdPrefix}/grid-${cfg.layerIdSlug}/${planeId}`;
    const planePath = [...planeGridsBasePath, `Plane ${planeId}`];

    ctx.render3D({
      layer: {
        id: layerId,
        label: `${cfg.fieldLabel} (${nValid})`,
        path: planePath,
        defaultVisible: false,
      },
      build: () => {
        const np = cfg.nodePositions(g);
        const [cx, cy, cz] = g.centroid;
        const positions = new Float32Array(nValid * 3);
        const colors    = new Float32Array(nValid * 3);
        let outI = 0;
        for (let row = 0; row < g.nRows; row++) {
          for (let col = 0; col < g.nCols; col++) {
            const idx = row * g.nCols + col;
            const val = arr[idx];
            if (Number.isNaN(val)) continue;
            if (arr instanceof Uint16Array && val === 0) continue;
            let lx, ly, lz;
            if (np) {
              lx = np.nodeX[idx]; ly = np.nodeY[idx]; lz = np.nodeZ[idx];
            } else {
              const u = g.gMinU + col * g.cellSize;
              const v_ = g.gMinV + row * g.cellSize;
              lx = u * g.basisE1[0] + v_ * g.basisE2[0] - g.d * g.normal[0];
              ly = u * g.basisE1[1] + v_ * g.basisE2[1] - g.d * g.normal[1];
              lz = u * g.basisE1[2] + v_ * g.basisE2[2] - g.d * g.normal[2];
            }
            positions[outI * 3 + 0] = (lx + cx) - origin.x;
            positions[outI * 3 + 1] = (ly + cy) - origin.y;
            positions[outI * 3 + 2] = (lz + cz) - origin.z;
            const t = Math.max(0, Math.min(1, (val - cfg.range.min) / span));
            tmpColor.setHSL(0.66 * (1 - t), 0.9, 0.5);
            colors[outI * 3 + 0] = tmpColor.r;
            colors[outI * 3 + 1] = tmpColor.g;
            colors[outI * 3 + 2] = tmpColor.b;
            outI++;
          }
        }
        const geom = new THREE.BufferGeometry();
        geom.setAttribute('position', new THREE.BufferAttribute(positions, 3));
        geom.setAttribute('color',    new THREE.BufferAttribute(colors, 3));
        geom.computeBoundingSphere();
        const mat = new THREE.PointsMaterial({
          size: 0.15, sizeAttenuation: true, vertexColors: true,
        });
        const points = new THREE.Points(geom, mat);
        points.name = `${opts.snapshotName}-grid-${cfg.layerIdSlug}-${planeId}`;
        points.renderOrder = 99;
        return {
          object: points,
          dispose: () => { geom.dispose(); mat.dispose(); },
        };
      },
    });
    ctx.renderLegend?.(legendSpec, layerId);
  }
}

export function renderPlaneGrids(ctx, rg, opts = {}) {
  if (!rg.has(RG_SECT.PLANE_GRIDS)) return;
  const grids = [...rg.planeGrids()];

  // Heat maps registered in field order; the tree groups by plane
  // ("Plane grids / Plane <id> / <field>"), so each plane's leaves
  // appear together regardless of how many fields are emitted.
  _renderCellHeatMap(ctx, grids.filter(g => g.balField), opts, {
    pickValue: (g) => g.balField,
    nodePositions: (g) => g.groupField ? g.groupField : null,
    range: { min: 0, max: 1 },
    label: 'Plane grids — balance (0=imbalanced → 1=symmetric)',
    fieldLabel: 'balance',
    groupKey: 'plane-grids-balance-legend',
    legendNote: 'group/nearby CoM blend; ≥0.25 gates anchor validation',
    layerIdSlug: 'bal',
  });
  _renderCellHeatMap(ctx, grids.filter(g => g.groupField), opts, {
    pickValue: (g) => g.groupField?.oa,
    nodePositions: (g) => g.groupField,
    range: { min: 0, max: 2 * Math.PI },
    label: 'Plane grids — open angle (0 rad → 2π rad)',
    fieldLabel: 'open angle',
    groupKey: 'plane-grids-oa-legend',
    legendNote: 'low (red) = sharp corner candidate, high (blue) = open',
    layerIdSlug: 'oa',
  });
  _renderCellHeatMap(ctx, grids.filter(g => g.groupField), opts, {
    pickValue: (g) => g.groupField?.comDist,
    nodePositions: (g) => g.groupField,
    range: { min: 0, max: 1.0 },
    label: 'Plane grids — CoM distance (m)',
    fieldLabel: 'CoM distance',
    groupKey: 'plane-grids-com-legend',
    legendNote: 'cell-to-local-centroid distance; high = edge-like',
    layerIdSlug: 'com',
  });
  _renderCellHeatMap(ctx, grids.filter(g => g.groupField), opts, {
    pickValue: (g) => g.groupField?.ptCount,
    nodePositions: (g) => g.groupField,
    range: { min: 0, max: 80 },
    label: 'Plane grids — point count per cell',
    fieldLabel: 'density',
    groupKey: 'plane-grids-count-legend',
    legendNote: 'cells with 0 points are skipped (not rendered)',
    layerIdSlug: 'count',
  });
  // OA gradient (oaSpan) — max abs diff across 4 opposing-cell-pair
  // directions per cell. C-computed in v7-boundary-signals-impl.c#L757-785;
  // the corner detector's local-maxima sweep runs on this exact field.
  // Visualizing it shows the cells the convex/concave detector considered
  // peaks of, which is the precursor of corner candidates.
  _renderCellHeatMap(ctx, grids.filter(g => g.groupField), opts, {
    pickValue: (g) => g.groupField?.oaSpan,
    nodePositions: (g) => g.groupField,
    range: { min: 0, max: Math.PI },
    label: 'Plane grids — groups OA gradient (oaSpan, rad)',
    fieldLabel: 'groups OA gradient',
    groupKey: 'plane-grids-oa-gradient-legend',
    legendNote: 'C-computed max abs OA diff across opposing cell pairs',
    layerIdSlug: 'oa-grad',
  });
  // Nearby field — same shape as group but on the projected-nearby
  // point set. Emitted by v7-boundary-signals as a SECOND PLANE_GRIDS
  // record per plane (flag 0x02). Used by D.4 corner detection (false
  // convex anchors beyond the ridge are typically peaks here, not in
  // the group field).
  _renderCellHeatMap(ctx, grids.filter(g => g.nearbyField), opts, {
    pickValue: (g) => g.nearbyField?.oa,
    nodePositions: (g) => g.nearbyField,
    range: { min: 0, max: 2 * Math.PI },
    label: 'Plane grids — nearby open angle (0 rad → 2π rad)',
    fieldLabel: 'nearby OA',
    groupKey: 'plane-grids-nearby-oa-legend',
    legendNote: 'low (red) = peak in nearby projections; corner candidate',
    layerIdSlug: 'nearby-oa',
  });
  _renderCellHeatMap(ctx, grids.filter(g => g.nearbyField), opts, {
    pickValue: (g) => g.nearbyField?.oaSpan,
    nodePositions: (g) => g.nearbyField,
    range: { min: 0, max: Math.PI },
    label: 'Plane grids — nearby OA gradient (oaSpan, rad)',
    fieldLabel: 'nearby OA gradient',
    groupKey: 'plane-grids-nearby-oa-gradient-legend',
    legendNote: 'C-computed gradient on nearby field — corner-detector input',
    layerIdSlug: 'nearby-oa-grad',
  });
}

// Per-plane grid-fill contour viz (PLANE_CONTOURS, 0x11).
//
// Five lazy layers per plane, all hidden by default — grouped under
// "Plane contours / Plane <id> / <field>":
//   density   — raw rasterised weight (f32)
//   smoothed  — post-Gaussian density (f32)
//   binary    — post-ICM binary mask (u8)
//   outer     — longest closed marching-squares loop (LineLoop)
//   holes     — additional loops with n_pts ≥ 4 (one LineLoop each)
//
// Contour vertices are stored in building-local plane (u,v) coords;
// this renderer needs the plane's basisE1/E2 + centroid (from the
// PLANES section in the same roof-graph) to project to world space.
export function renderPlaneContours(ctx, rg, opts = {}) {
  if (!rg.has(RG_SECT.PLANE_CONTOURS)) return;
  if (!rg.has(RG_SECT.PLANE_GRIDS)) return;   // need building-local basis/d/centroid

  // Build lookup from PLANE_GRIDS — its basis is rotation-only,
  // its d is BUILDING-LOCAL, and its `centroid` field is the BUILDING
  // centroid used to shift back to world. The PLANES section can't be
  // used here: topology emits it in TILE-WORLD coords, so projecting
  // the contour's building-local (u,v) against world d + world centroid
  // shifts the result by (n·c)·n along the normal — on dahouet (Lambert93
  // z~50m) tens of metres off-screen, silently invisible.
  //
  // The first PLANE_GRIDS record per plane carries the group field's
  // geometry; nearby's record carries the same basis/d/centroid (same
  // plane, same building) so we just take the first.
  const planeById = new Map();
  for (const g of rg.planeGrids()) {
    if (planeById.has(g.planeId)) continue;
    if (!g.normal || !g.basisE1 || !g.basisE2 || !g.centroid) continue;
    planeById.set(g.planeId, {
      planeId:  g.planeId,
      normal:   g.normal,
      d:        g.d,           // building-local
      basisE1:  g.basisE1,
      basisE2:  g.basisE2,
      centroid: g.centroid,    // building centroid (F64×3)
    });
  }

  const contours = [...rg.planeContours()].filter(c => planeById.has(c.planeId));
  if (contours.length === 0) return;

  const basePath = [...(opts.layerPath || []), 'Plane contours'];

  // Sample a handful of cell-centre world positions for scene-origin.
  const samples = [];
  for (const c of contours) {
    if (samples.length >= 1024) break;
    const p = planeById.get(c.planeId);
    const [e1x, e1y, e1z] = p.basisE1;
    const [e2x, e2y, e2z] = p.basisE2;
    const [cx, cy, cz] = p.centroid;
    const [nx, ny, nz] = p.normal;
    const u = c.gMinU + (c.nCols >>> 1) * c.cellSize;
    const v = c.gMinV + (c.nRows >>> 1) * c.cellSize;
    const lx = u * e1x + v * e2x - p.d * nx;
    const ly = u * e1y + v * e2y - p.d * ny;
    const lz = u * e1z + v * e2z - p.d * nz;
    samples.push([lx + cx, ly + cy, lz + cz]);
  }
  const origin = ensureOrigin(ctx, samples);

  const tmpColor = new THREE.Color();
  const HUE_STOPS = 9;
  const stops = [];
  for (let s = 0; s < HUE_STOPS; s++) {
    const t = s / (HUE_STOPS - 1);
    const hueDeg = 0.66 * (1 - t) * 360;
    stops.push(`hsl(${hueDeg.toFixed(0)},90%,50%)`);
  }

  // Project building-local (u, v) → world via the plane's basis + d_local
  // + building centroid. Same formula renderPlaneGrids uses (see L600-606).
  const project = (p, u, v) => {
    const [e1x, e1y, e1z] = p.basisE1;
    const [e2x, e2y, e2z] = p.basisE2;
    const [cx, cy, cz] = p.centroid;
    const [nx, ny, nz] = p.normal;
    return [
      u * e1x + v * e2x - p.d * nx + cx,
      u * e1y + v * e2y - p.d * ny + cy,
      u * e1z + v * e2z - p.d * nz + cz,
    ];
  };

  // Heat-map helper for grid layers (density / smoothed / binary).
  const registerHeat = (c, p, planePath, arr, isBinary, slug, label, range, fieldLabel, legendKey, legendNote) => {
    if (!arr) return;
    let nValid = 0;
    for (let i = 0; i < arr.length; i++) {
      const v = arr[i];
      if (isBinary) { if (v) nValid++; }
      else if (!Number.isNaN(v) && v > 0) { nValid++; }
    }
    if (nValid === 0) return;
    const layerId = `${opts.layerIdPrefix}/contour-${slug}/${c.planeId}`;
    const span = range.max - range.min;
    ctx.render3D({
      layer: {
        id: layerId,
        label: `${fieldLabel} (${nValid})`,
        path: planePath,
        defaultVisible: false,
      },
      build: () => {
        const positions = new Float32Array(nValid * 3);
        const colors    = new Float32Array(nValid * 3);
        let outI = 0;
        for (let row = 0; row < c.nRows; row++) {
          for (let col = 0; col < c.nCols; col++) {
            const idx = row * c.nCols + col;
            const val = arr[idx];
            if (isBinary ? !val : (Number.isNaN(val) || val <= 0)) continue;
            // Cell-centre, half-cell shift from g_min like the grid-fill itself.
            const u = c.gMinU + (col + 0.5) * c.cellSize;
            const v = c.gMinV + (row + 0.5) * c.cellSize;
            const [wx, wy, wz] = project(p, u, v);
            positions[outI * 3 + 0] = wx - origin.x;
            positions[outI * 3 + 1] = wy - origin.y;
            positions[outI * 3 + 2] = wz - origin.z;
            const t = isBinary ? 1 : Math.max(0, Math.min(1, (val - range.min) / span));
            tmpColor.setHSL(0.66 * (1 - t), 0.9, 0.5);
            colors[outI * 3 + 0] = tmpColor.r;
            colors[outI * 3 + 1] = tmpColor.g;
            colors[outI * 3 + 2] = tmpColor.b;
            outI++;
          }
        }
        const geom = new THREE.BufferGeometry();
        geom.setAttribute('position', new THREE.BufferAttribute(positions, 3));
        geom.setAttribute('color',    new THREE.BufferAttribute(colors, 3));
        geom.computeBoundingSphere();
        const mat = new THREE.PointsMaterial({
          size: 0.12, sizeAttenuation: true, vertexColors: true,
        });
        const points = new THREE.Points(geom, mat);
        points.name = `${opts.snapshotName}-contour-${slug}-${c.planeId}`;
        points.renderOrder = 99;
        return { object: points, dispose: () => { geom.dispose(); mat.dispose(); } };
      },
    });
    ctx.renderLegend?.({
      groupKey: `${opts.layerIdPrefix}/${legendKey}`,
      title: label,
      gradient: { min: range.min, max: range.max, stops },
      note: legendNote,
    }, layerId);
  };

  // Closed-loop helper (outer + each hole).
  const registerLoop = (c, p, planePath, uvFlat, slug, label, colorHex) => {
    if (!uvFlat || uvFlat.length < 6) return;     // need ≥3 verts
    const n = uvFlat.length >>> 1;
    const layerId = `${opts.layerIdPrefix}/contour-${slug}/${c.planeId}`;
    ctx.render3D({
      layer: {
        id: layerId,
        label: `${label} (${n}v)`,
        path: planePath,
        defaultVisible: false,
      },
      build: () => {
        const positions = new Float32Array(n * 3);
        for (let i = 0; i < n; i++) {
          const u = uvFlat[i * 2 + 0];
          const v = uvFlat[i * 2 + 1];
          const [wx, wy, wz] = project(p, u, v);
          positions[i * 3 + 0] = wx - origin.x;
          positions[i * 3 + 1] = wy - origin.y;
          positions[i * 3 + 2] = wz - origin.z;
        }
        const geom = new THREE.BufferGeometry();
        geom.setAttribute('position', new THREE.BufferAttribute(positions, 3));
        geom.computeBoundingSphere();
        const mat = new THREE.LineBasicMaterial({
          color: colorHex,
          linewidth: 3,
          transparent: true,
          opacity: 0.95,
          depthWrite: false,
          depthTest: false,
        });
        const loop = new THREE.LineLoop(geom, mat);
        loop.renderOrder = 101;
        loop.name = `${opts.snapshotName}-contour-${slug}-${c.planeId}`;
        return { object: loop, dispose: () => { geom.dispose(); mat.dispose(); } };
      },
    });
  };

  for (const c of contours) {
    const p = planeById.get(c.planeId);
    const planePath = [...basePath, `Plane ${c.planeId}`];

    // Heat maps — only emit if has_grids flag is set.
    if (c.flags & 0x01) {
      const dRange = { min: 0, max: Math.max(1e-6, c.maxDensity) };
      registerHeat(c, p, planePath, c.density,  false, 'density',  'Plane contours — density (raw weight, own points)',           dRange, 'density',  'plane-contours-density-legend',  'sum of Voronoi-area point weights per cell');
      registerHeat(c, p, planePath, c.smoothed, false, 'smoothed', 'Plane contours — smoothed (2× 3×3 Gaussian, own points)',     dRange, 'smoothed', 'plane-contours-smoothed-legend', 'driving signal for threshold + ICM');
      registerHeat(c, p, planePath, c.binary,   true,  'binary',   'Plane contours — binary mask (own>threshold AND own>nearby)', { min: 0, max: 1 }, 'binary', 'plane-contours-binary-legend', 'cells used by marching squares to extract loops');
      // Competing density from nearby (foreign-plane projections).
      // Bright cells = where other planes' points push back against
      // ours. The contour ends up at the contrast between own and nearby.
      if (c.densityNearby) {
        registerHeat(c, p, planePath, c.densityNearby,  false, 'density-nearby',  'Plane contours — density (nearby/foreign points)',          dRange, 'density (nearby)',  'plane-contours-density-nearby-legend',  'foreign-plane projections that push back at the boundary');
        registerHeat(c, p, planePath, c.smoothedNearby, false, 'smoothed-nearby', 'Plane contours — smoothed nearby (competing signal)',       dRange, 'smoothed (nearby)', 'plane-contours-smoothed-nearby-legend', 'binarise condition: cell is ours iff own>nearby here');
      }
    }

    // Outer + holes — closed line loops.
    if (c.flags & 0x02) {
      registerLoop(c, p, planePath, c.outer, 'outer', 'Outer loop', planeHueHex(c.planeId));
    }
    if (c.flags & 0x04) {
      for (let h = 0; h < c.holes.length; h++) {
        registerLoop(c, p, planePath, c.holes[h], `hole-${h}`, `Hole #${h}`, 0xff44aa);
      }
    }
  }
}

export function renderMrPointsByPlane(ctx, rg, opts = {}) {
  if (!rg.has(RG_SECT.MR_POINTS_BY_PLANE)) return;
  const all = [];
  for (const { points } of rg.mrPointsByPlane()) {
    for (const p of points) all.push(p);
  }
  if (all.length === 0) return;
  const origin = ensureOrigin(ctx, all);
  const radius = opts.mrRadius ?? 0.06;
  const { object, geom, mat } = makeSpheres(all, radius, origin, () => MR_POINT_COLOR);
  object.name = `${opts.snapshotName}-mr-points`;
  ctx.render3D({
    kind: 'mesh', object,
    layer: {
      id: `${opts.layerIdPrefix}/mr-points`,
      label: `MR points (${all.length})`,
      path: opts.layerPath,
      defaultVisible: false,
    },
  });
  ctx.onCleanup(() => { geom.dispose(); mat.dispose(); });
}

// ── high-level entry point ─────────────────────────────────────────

/**
 * Render every section present in the roof-graph artifact. Pass the
 * decoded artifact (output of decodeRoofGraph) and a snapshotName
 * (used as the layer subcategory + layer-id prefix). Each layer the
 * per-section renderers create lands under
 *     "Roof Graph" → snapshotName → primitive.
 *
 * @param {Context} ctx
 * @param {Uint8Array | RoofGraphReader} rgOrBytes
 * @param {string} snapshotName  e.g. 'boundary-signals'
 * @param {object} [opts]
 *   layerIdPrefix? string  (default: snapshotName)
 *   layerPath? string[]    (default: ['Roof Graph', snapshotName])
 */
// PERIMETER_TAGS overlay. Produces two sub-layers:
//   * Tagged edges    — every edge with a perimeter tag, coloured by tag.
//   * Outward normals — 1 m horizontal arrow from each tagged edge's
//                       midpoint along edge.outward (perimeter +
//                       level-transition edges carry one — 2-component
//                       horizontal vector in z-up; see PERIMETER_TAGS
//                       spec §4.12).
//
// Both default to off — toggled from the layer tree like all other
// roof-graph layers.
export function renderPerimeterTags(ctx, rg, opts = {}) {
  if (!rg.has(RG_SECT.PERIMETER_TAGS) || !rg.has(RG_SECT.EDGES) || !rg.has(RG_SECT.CORNERS)) {
    return;
  }
  const corners = new Map();
  for (const c of rg.corners()) corners.set(c.id, c);
  const edges = new Map();
  for (const e of rg.edges()) edges.set(e.id, e);

  // Two pre-allocated segment lists.
  const tagSegs = [];          // { from, to, color }
  const arrowSegs = [];        // { from, to, color }
  const tagCounts = {};        // tag → count, for the legend
  let nArrows = 0;
  for (const pt of rg.perimeterTags()) {
    const e = edges.get(pt.edgeId);
    if (!e) continue;
    const a = corners.get(e.cornerA);
    const b = corners.get(e.cornerB);
    if (!a || !b) continue;
    const color = PERIM_TAG_COLOR[pt.tag] ?? PERIM_TAG_COLOR.untagged;
    tagSegs.push({ from: a.point, to: b.point, color });
    tagCounts[pt.tag] = (tagCounts[pt.tag] || 0) + 1;
    if (pt.outward) {
      // v3 scene is z-up: arrow grows along X and Y (horizontal),
      // Z (vertical) stays at the edge-midpoint height.
      const mx = (a.point[0] + b.point[0]) * 0.5;
      const my = (a.point[1] + b.point[1]) * 0.5;
      const mz = (a.point[2] + b.point[2]) * 0.5;
      arrowSegs.push({
        from: [mx, my, mz],
        to:   [mx + pt.outward.x * OUTWARD_ARROW_LEN_M,
               my + pt.outward.y * OUTWARD_ARROW_LEN_M,
               mz],
        color,
      });
      nArrows++;
    }
  }
  if (tagSegs.length === 0) return;

  const subPath = [...(opts.layerPath || []), 'Classify'];
  const allSamples = [];
  for (const s of tagSegs) allSamples.push(s.from, s.to);
  for (const s of arrowSegs) allSamples.push(s.from, s.to);
  const origin = ensureOrigin(ctx, allSamples);

  // Legend spec for the tagged-edges layer (and outward-normals — they
  // share the same colour palette). Ordered so the most common tags
  // (perimeter, shared, overhang) lead, then secondary (interior-shared,
  // level-transition, wall). Tags with zero edges are dropped so the
  // legend stays compact.
  const TAG_DISPLAY_ORDER = [
    'perimeter', 'shared',
    'overhang-top', 'overhang-bottom',
    'level-transition', 'interior-shared', 'wall',
  ];
  const HEX = (n) => '#' + n.toString(16).padStart(6, '0');
  const swatches = TAG_DISPLAY_ORDER
    .filter((t) => (tagCounts[t] || 0) > 0)
    .map((t) => ({ label: t, color: HEX(PERIM_TAG_COLOR[t]), count: tagCounts[t] }));

  // Tagged-edges layer
  {
    const layerId = `${opts.layerIdPrefix}/perimeter-tags`;
    const { object, geom, mat } = makeLineSegments(tagSegs, origin, (s) => s.color);
    object.name = `${opts.snapshotName}-perimeter-tags`;
    ctx.render3D({
      kind: 'lines', object,
      layer: {
        id: layerId,
        label: `Perimeter tags (${tagSegs.length})`,
        path: subPath,
        defaultVisible: opts.defaultVisible ?? false,
      },
    });
    ctx.onCleanup(() => { geom.dispose(); mat.dispose(); });
    ctx.renderLegend?.({
      groupKey: `${opts.layerIdPrefix}/perimeter-tags-legend`,
      title: `${opts.snapshotName} — perimeter tags`,
      swatches,
      note: 'edge classification from v7-boundary-classify',
    }, layerId);
  }

  if (nArrows > 0) {
    const layerId = `${opts.layerIdPrefix}/outward-normals`;
    const { object, geom, mat } = makeLineSegments(arrowSegs, origin, (s) => s.color);
    object.name = `${opts.snapshotName}-outward-normals`;
    ctx.render3D({
      kind: 'lines', object,
      layer: {
        id: layerId,
        label: `Outward normals (${nArrows})`,
        path: subPath,
        defaultVisible: opts.defaultVisible ?? false,
      },
    });
    ctx.onCleanup(() => { geom.dispose(); mat.dispose(); });
    // Same palette + groupKey as perimeter-tags → one legend card shows
    // when either layer is visible.
    ctx.renderLegend?.({
      groupKey: `${opts.layerIdPrefix}/perimeter-tags-legend`,
      title: `${opts.snapshotName} — perimeter tags`,
      swatches,
      note: 'edge classification from v7-boundary-classify',
    }, layerId);
  }

  // NEW_ADJACENCIES — synthesized plane-pair links via classify. Draw
  // as a thin line connecting the midpoints of the two referenced edges
  // (visualises which plane-pair classify believes is shared).
  if (rg.has(RG_SECT.NEW_ADJACENCIES)) {
    const adjSegs = [];
    for (const adj of rg.newAdjacencies()) {
      const ea = edges.get(adj.edgeIdA);
      const eb = edges.get(adj.edgeIdB);
      if (!ea || !eb) continue;
      const a0 = corners.get(ea.cornerA), a1 = corners.get(ea.cornerB);
      const b0 = corners.get(eb.cornerA), b1 = corners.get(eb.cornerB);
      if (!a0 || !a1 || !b0 || !b1) continue;
      const ma = [(a0.point[0]+a1.point[0])*0.5, (a0.point[1]+a1.point[1])*0.5, (a0.point[2]+a1.point[2])*0.5];
      const mb = [(b0.point[0]+b1.point[0])*0.5, (b0.point[1]+b1.point[1])*0.5, (b0.point[2]+b1.point[2])*0.5];
      adjSegs.push({ from: ma, to: mb });
    }
    if (adjSegs.length > 0) {
      const { object, geom, mat } = makeLineSegments(adjSegs, origin, () => 0xffeb3b);
      object.name = `${opts.snapshotName}-new-adjacencies`;
      ctx.render3D({
        kind: 'lines', object,
        layer: {
          id: `${opts.layerIdPrefix}/new-adjacencies`,
          label: `New adjacencies (${adjSegs.length})`,
          path: subPath,
          defaultVisible: opts.defaultVisible ?? false,
        },
      });
      ctx.onCleanup(() => { geom.dispose(); mat.dispose(); });
    }
  }
}

export function renderRoofGraph(ctx, rgOrBytesOrArtifact, snapshotName, opts = {}) {
  let rg;
  const input = rgOrBytesOrArtifact;
  if (input && typeof input === 'object' && typeof input.has === 'function') {
    rg = input;
  } else if (input instanceof Uint8Array || input instanceof ArrayBuffer) {
    rg = decodeRoofGraph(input);
  } else if (input && (input.isRoofGraphArray || input.isRoofGraph)) {
    const bldgIdx = opts.buildingIdx ?? ctx.sceneState?.selection?.buildingIdx ?? 0;
    rg = pickRoofGraph(input, bldgIdx);
    if (!rg) {
      ctx.renderHTML(`<div class="render-placeholder">${snapshotName}: no graph for building ${bldgIdx} (n=${input.items?.length ?? '?'}).</div>`);
      return;
    }
  } else {
    ctx.renderHTML(`<div class="render-placeholder">${snapshotName}: no roof-graph bytes (got ${typeof input}).</div>`);
    return;
  }
  renderRoofGraphSync(ctx, rg, snapshotName, opts);
}

function renderRoofGraphSync(ctx, rg, snapshotName, opts) {

  const merged = {
    ...opts,
    snapshotName,
    layerIdPrefix: opts.layerIdPrefix ?? snapshotName,
    layerPath: opts.layerPath ?? ['Roof Graph', snapshotName],
  };

  // Render in dependency order — corners first so edges + polygon paths
  // can resolve them.
  renderRidges(ctx, rg, merged);
  renderCorners(ctx, rg, merged);
  renderCornerLabels(ctx, rg, merged);
  renderEdges(ctx, rg, merged);
  renderPolygonPaths(ctx, rg, merged);
  renderPlanes(ctx, rg, merged);
  renderPlaneLabels(ctx, rg, merged);
  renderNearbyProjections(ctx, rg, merged);
  renderPlaneGrids(ctx, rg, merged);
  renderPlaneContours(ctx, rg, merged);
  renderMrPointsByPlane(ctx, rg, merged);
  renderPerimeterTags(ctx, rg, merged);

  // Inspector pane: lightweight summary so users see what landed.
  const summary = {
    snapshot: snapshotName,
    sections: rg.sectionTypes().map(t => ({
      typeId: t,
      ...rg.sectionHeader(t),
    })),
  };
  ctx.renderJSON(summary);
}
