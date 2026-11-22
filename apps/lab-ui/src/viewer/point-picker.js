// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Scene feature-picker (debug). Click anywhere in the 3D viewer and the
// FULL stack of features under the cursor is logged near→far — one row per
// object — each tagged visible/HIDDEN, with per-layer info. Stashed on
// `window.__lastPick` (the stack) for inspection.
//
// ── Why a stack, and why the visible/HIDDEN tag ──────────────────────
// THREE's raycaster returns EVERY intersection along the ray, sorted by
// distance, and — crucially — it ignores `object.visible` (see
// three.module.js intersect(): it gates only on `object.layers`, never on
// `.visible`) and does no depth/occlusion test. So a click legitimately
// passes through occluded geometry AND through layers you toggled off.
// Rather than hide that, we surface it: every row says whether it would
// actually be drawn. (That's why an early click reported the hidden
// `lidar-positions-preview` cloud — it's still raycastable while invisible.)
//
// ── How a layer contributes info ─────────────────────────────────────
// Resolution ladder, per hit:
//   1. nearest ancestor with `userData.pick` (function) → call it(intersect)
//   2. else static own-fields on `userData` (e.g. corner {id,name,source})
//   3. else a bare THREE.Points → {pointId: index}
//   4. else a Mesh → {faceIndex}
// Most layers need nothing (the fallbacks cover them). A layer only adds a
// `userData.pick` closure when it can say something better — e.g.
// categorical-cloud maps its local vertex index back to a global point id
// + owner plane.
import * as THREE from 'three';

export function attachPointPicker(viewer) {
  const raycaster = new THREE.Raycaster();
  // Points/Lines have no area — a pick radius (world units) is required.
  // 0.15 m matches bench-v2 (ui/public/app.js). Tune via
  // window.__pointPicker.threshold.
  raycaster.params.Points.threshold = 0.15;
  raycaster.params.Line.threshold = 0.10;
  const mouse = new THREE.Vector2();
  const canvas = viewer.renderer.domElement;

  // Distinguish a click from an orbit/pan drag (OrbitControls also uses the
  // canvas): only treat near-stationary pointer up/down as a pick.
  let downX = 0, downY = 0;
  canvas.addEventListener('pointerdown', (e) => { downX = e.clientX; downY = e.clientY; });

  canvas.addEventListener('click', (e) => {
    if (Math.hypot(e.clientX - downX, e.clientY - downY) > 4) return;  // was a drag
    const r = canvas.getBoundingClientRect();
    mouse.x = ((e.clientX - r.left) / r.width) * 2 - 1;
    mouse.y = -((e.clientY - r.top) / r.height) * 2 + 1;
    raycaster.setFromCamera(mouse, viewer.activeCamera);

    // All intersections, near→far. Collapse to the nearest hit per object
    // (a point cloud returns many points within the radius; one row each).
    const hits = raycaster.intersectObjects(viewer.scene.children, true);
    const seen = new Set();
    const rows = [];
    for (const h of hits) {
      if (seen.has(h.object)) continue;
      seen.add(h.object);
      rows.push({
        distance: h.distance,
        visible: isVisible(h.object),
        layer: layerNameOf(h.object),
        info: featureInfo(h),
        world: h.point,
      });
    }
    if (rows.length === 0) return;

    const near = rows[0].world;
    const result = {
      xyz: [round(near.x), round(near.y), round(near.z)],
      features: rows.map((r) => ({
        distance: round(r.distance, 3),
        visible: r.visible,
        layer: r.layer,
        ...r.info,
      })),
    };
    window.__lastPick = result;

    // One JSON blob — single console line (one file:line suffix) and
    // auto-copied to the clipboard so it's ready to paste. The click is a
    // user gesture, so navigator.clipboard is permitted (localhost is a
    // secure context).
    const json = JSON.stringify(result, null, 2);
    let copyNote = '';
    try {
      navigator.clipboard?.writeText(json).catch(() => {});
      copyNote = ' (copied to clipboard)';
    } catch { /* clipboard unavailable — JSON is still logged below */ }
    // eslint-disable-next-line no-console
    console.log(`[pick]${copyNote}\n${json}`);
  });

  // Console handle for tuning the pick radius without a rebuild.
  window.__pointPicker = raycaster.params.Points;
}

function round(v, dp = 2) {
  const m = 10 ** dp;
  return Math.round(v * m) / m;
}

// The raycaster ignores visibility, so we recompute it: an object is
// visible only if it and every ancestor has `.visible !== false`.
function isVisible(object) {
  let o = object;
  while (o) {
    if (o.visible === false) return false;
    o = o.parent;
  }
  return true;
}

// Nearest named ancestor — the human-facing layer label.
function layerNameOf(object) {
  let o = object;
  while (o) {
    if (o.name) return o.name;
    o = o.parent;
  }
  return '(unnamed)';
}

// Per-feature info for a hit, via the resolution ladder (see header).
function featureInfo(hit) {
  // 1. nearest ancestor with a pick() resolver.
  let o = hit.object;
  while (o) {
    if (typeof o.userData?.pick === 'function') {
      const out = o.userData.pick(hit);
      if (out) return out;
    }
    o = o.parent;
  }
  // 2. static own-fields on the hit object (e.g. {id,name,source}).
  const ud = hit.object?.userData;
  if (ud) {
    const staticFields = {};
    for (const k of Object.keys(ud)) {
      if (typeof ud[k] !== 'function') staticFields[k] = ud[k];
    }
    if (Object.keys(staticFields).length) return staticFields;
  }
  // 3/4. intrinsic fallbacks.
  if (hit.object?.isPoints && hit.index != null) return { pointId: hit.index };
  if (hit.faceIndex != null) return { faceIndex: hit.faceIndex };
  return {};
}
