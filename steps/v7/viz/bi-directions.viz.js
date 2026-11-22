// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: bi-directions — F32×3 unit vectors from aircraft
// toward ground, per tile point.
//
// The beam is mostly nadir (vertical component ≈ -0.97 for IGN LiDAR HD),
// so a naive |bx|·|by|·|bz| → R·G·B mapping makes every point look the
// same washed-blue. The interesting signal lives in the TWO HORIZONTAL
// components (always within ±sin(20°) ≈ ±0.34 for IGN), which encode:
//   - WHICH FLIGHT STRIP the point belongs to → cross-track direction
//     (each strip's `ct` vector points roughly N/S or E/W, distinct hue)
//   - WHERE WITHIN THE STRIP the point is → scan-angle magnitude
//     (centre of swath = small horizontal ≈ 0; swath edge = max horizontal
//     ≈ sin(scan_angle_max))
//
// Encoding:
//   - hue       = atan2 of horizontal vector (0..360° around the wheel)
//   - lightness = 0.30 + 0.50 × clamp01(magnitude / 0.35)
//   - saturation = 0.85
// Result: each strip gets a distinct hue, with darker centre / brighter
// edges within the strip. Points without a strip fit (default beam =
// straight down) have zero horizontal magnitude → black.
//
// Up axis: production v3 emits beams in z-up (LAZ) frame so the
// horizontal pair is (bx, by). For a bench-frame y-up dump the pair
// is (bx, bz). We auto-detect by which component carries the dominant
// vertical magnitude across the cloud.

import * as THREE from 'three';
import { parseBuildingHeader } from '../../../apps/lab-ui/src/renderers/_helpers/per-building-records.js';

const AK_POINT_COLUMN = 10;

function clamp01(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

// Pick the (h0, h1) horizontal-component indices by finding which axis
// dominates the |·| sum across the cloud — the up-axis is the one where
// the average absolute component is closest to 1.
function detectUpAxis(beamData, n) {
  let s0 = 0, s1 = 0, s2 = 0;
  let count = 0;
  const stride = Math.max(1, (n / 4096) | 0);
  for (let i = 0; i < n; i += stride) {
    const o = i * 3;
    s0 += Math.abs(beamData[o]);
    s1 += Math.abs(beamData[o + 1]);
    s2 += Math.abs(beamData[o + 2]);
    count++;
  }
  if (count === 0) return 1;
  s0 /= count; s1 /= count; s2 /= count;
  if (s2 >= s0 && s2 >= s1) return 2;  // z-up
  if (s1 >= s0 && s1 >= s2) return 1;  // y-up
  return 0;
}

export function render(ctx, data) {
  if (data?.kind !== AK_POINT_COLUMN || !data.data) {
    ctx.renderHTML(`<div class="render-placeholder">bi-directions expected POINT_COLUMN F32×3.</div>`);
    return;
  }
  const beam = data.data;          // Float32Array(N×3)
  const N = (data.n_items ?? (beam.length / 3)) | 0;

  const positionsArt = ctx.getSiblingDecoded?.('lidar-positions');
  if (!positionsArt?.data) {
    ctx.renderHTML(`<div class="render-placeholder">bi-directions: subscribe lidar-positions to render.</div>`);
    return;
  }
  const xyz = positionsArt.data;

  const upAxis = detectUpAxis(beam, N);
  const h0 = (upAxis === 0) ? 1 : 0;
  const h1 = (upAxis === 2) ? 1 : 2;
  const upLabel = (upAxis === 1) ? 'y-up (bench)' : 'z-up (LAZ)';
  const horizLabel = (upAxis === 1) ? '(bx, bz)' : '(bx, by)';

  let origin = ctx.viewer.getSceneOrigin?.();
  if (!origin) {
    origin = { x: xyz[0], y: xyz[1], z: xyz[2] };
    ctx.viewer.setSceneOrigin?.(origin);
  }

  const positions = new Float32Array(N * 3);
  const colors = new Float32Array(N * 3);
  const tmp = new THREE.Color();

  // Magnitude scale: actual horizontal mags top out around sin(scan_angle_max)
  // ~ 0.30–0.35 for IGN. Scale 0..0.35 → 0..1 lightness band.
  const MAG_MAX = 0.35;

  let nNonZero = 0;
  let maxMag = 0;
  for (let i = 0; i < N; i++) {
    const o = i * 3;
    positions[o]     = xyz[o]     - origin.x;
    positions[o + 1] = xyz[o + 1] - origin.y;
    positions[o + 2] = xyz[o + 2] - origin.z;

    const a = beam[o + h0];
    const b = beam[o + h1];
    const mag = Math.sqrt(a * a + b * b);
    if (mag > 1e-4) {
      nNonZero++;
      if (mag > maxMag) maxMag = mag;
      const angle = Math.atan2(b, a);                  // -π..π
      const hue = (angle + Math.PI) / (2 * Math.PI);   // 0..1
      const light = 0.30 + 0.50 * clamp01(mag / MAG_MAX);
      tmp.setHSL(hue, 0.85, light);
      colors[o]     = tmp.r;
      colors[o + 1] = tmp.g;
      colors[o + 2] = tmp.b;
    } else {
      // Default-down (no strip fit) → near-black so unstripped points
      // recede visually.
      colors[o] = 0.05; colors[o + 1] = 0.05; colors[o + 2] = 0.05;
    }
  }

  const geom = new THREE.BufferGeometry();
  geom.setAttribute('position', new THREE.BufferAttribute(positions, 3));
  geom.setAttribute('color',    new THREE.BufferAttribute(colors,    3));
  const mat = new THREE.PointsMaterial({
    size: 2.0, sizeAttenuation: false, vertexColors: true,
  });
  const points = new THREE.Points(geom, mat);
  points.name = 'beam/directions';

  ctx.render3D({
    kind: 'points', object: points,
    layer: { id: 'beam/directions',
             label: 'bi-directions (horizontal hue)',
             path: ['Beam Incidence'],
             defaultVisible: false },
  });
  ctx.onCleanup(() => { geom.dispose(); mat.dispose(); });

  // Legend: 8-stop hue wheel showing the horizontal-direction colour map.
  const stops = [];
  for (let s = 0; s < 8; s++) {
    const t = s / 7;
    stops.push(`hsl(${(t * 360).toFixed(0)},85%,55%)`);
  }
  ctx.renderLegend?.({
    title: 'bi-directions',
    gradient: {
      min: 0, max: 360, stops,
      format: (v) => `${v.toFixed(0)}°`,
    },
    note: `horizontal ${horizLabel} dir (hue) × mag (lightness)  •  ${upLabel}`,
  });

  ctx.renderJSON({
    artifact: 'bi-directions',
    n_points: N,
    n_with_horizontal: nNonZero,
    max_horizontal_mag: maxMag,
    up_axis: upAxis,
    horizontal_components: horizLabel,
    note: 'hue = atan2(h1, h0) of horizontal beam vector; lightness = magnitude',
  });

  void parseBuildingHeader;
}
