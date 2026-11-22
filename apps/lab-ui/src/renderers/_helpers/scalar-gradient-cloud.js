// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Generic helper: render an F32 per-point scalar field as an HSL gradient
// 3D point cloud + Chart.js histogram + min/mid/max legend + JSON stats.
//
// Used by step-visualizers whose artifact is one F32 value per point. Shape
// discrimination is automatic:
//   - AK_POINT_COLUMN  (kind 10):     payload.data is a Float32Array length N
//                                      points at sibling lidar-positions
//   - AK_ARRAY_INLINE  (kind 8):      payload.items is Uint8Array[] — one
//                                      Float32Array record per building, with
//                                      global indices coming from sibling
//                                      per-building-records[bi] header.
//
// Colour mapping matches bench-v2's `colorMode: 'gradient'`: blue → red via
// HSL hue interpolation (hue 0.66 → 0.0, S=0.9, L=0.5).
//
// Config knobs:
//   { valueRange:    [min,max] | 'auto'   — clamp domain (default auto)
//     layerPath:     string[]              — visibility-tree group path
//     layerLabel:    string                — leaf label
//     layerId:       string                — unique leaf ID
//     histogramScale:'linear' | 'log'     — y-axis scale (default linear)
//     histogramBins: number                — bin count (default 64)
//     pointSize:     number                — THREE.PointsMaterial size
//   }

import * as THREE from 'three';
import {
  Chart, BarController, BarElement,
  CategoryScale, LinearScale, LogarithmicScale,
  Tooltip, Title,
} from 'chart.js';
import { parseBuildingHeader } from './per-building-records.js';

Chart.register(
  BarController, BarElement,
  CategoryScale, LinearScale, LogarithmicScale,
  Tooltip, Title,
);

const AK_POINT_COLUMN = 10;
const AK_ARRAY_INLINE = 8;

export function renderScalarGradientCloud(ctx, payload, config) {
  const cfg = {
    valueRange: 'auto',
    layerPath: undefined,
    layerLabel: payload?.name ?? 'scalar',
    layerId: payload?.name ?? 'scalar-cloud',
    histogramScale: 'linear',
    histogramBins: 64,
    pointSize: 0.1,
    ...config,
  };

  const positionsArt = ctx.getSiblingDecoded?.('lidar-positions');
  if (!positionsArt || positionsArt.kind !== AK_POINT_COLUMN || !positionsArt.data) {
    ctx.renderHTML(
      `<div class="render-placeholder">${cfg.layerLabel}: subscribe lidar-positions to render the cloud.</div>`,
    );
    return;
  }
  const xyz = positionsArt.data;

  // Gather (position, value) pairs into flat buffers, then build a single
  // THREE.Points layer. Range scan + render run as two passes so we can
  // compute the histogram domain before colouring.
  const collected = collectFromPayload(ctx, payload, xyz);
  if (!collected || collected.n === 0) {
    ctx.renderJSON({ artifact: payload?.name, n: 0, note: 'no points' });
    return;
  }
  const { positions, values, n } = collected;

  // Range
  let vmin, vmax;
  if (Array.isArray(cfg.valueRange) && cfg.valueRange.length === 2) {
    [vmin, vmax] = cfg.valueRange;
  } else {
    vmin = Infinity;
    vmax = -Infinity;
    for (let i = 0; i < n; i++) {
      const v = values[i];
      if (Number.isFinite(v)) {
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
      }
    }
    if (!Number.isFinite(vmin)) { vmin = 0; vmax = 1; }
    if (vmin === vmax) { vmax = vmin + 1e-9; }
  }
  const span = vmax - vmin;

  // Recenter to scene origin
  let origin = ctx.viewer.getSceneOrigin?.();
  if (!origin) {
    origin = computeOrigin(positions, Math.min(n, 1000));
    ctx.viewer.setSceneOrigin?.(origin);
  }
  const centred = new Float32Array(n * 3);
  const colors  = new Float32Array(n * 3);
  const tmp = new THREE.Color();
  for (let i = 0; i < n; i++) {
    centred[i * 3 + 0] = positions[i * 3 + 0] - origin.x;
    centred[i * 3 + 1] = positions[i * 3 + 1] - origin.y;
    centred[i * 3 + 2] = positions[i * 3 + 2] - origin.z;
    const t = (values[i] - vmin) / span;
    const hue = 0.66 * (1.0 - clamp01(t)); // 0.66 (blue) → 0.0 (red)
    tmp.setHSL(hue, 0.9, 0.5);
    colors[i * 3 + 0] = tmp.r;
    colors[i * 3 + 1] = tmp.g;
    colors[i * 3 + 2] = tmp.b;
  }

  const geom = new THREE.BufferGeometry();
  geom.setAttribute('position', new THREE.BufferAttribute(centred, 3));
  geom.setAttribute('color',    new THREE.BufferAttribute(colors,  3));
  const mat = new THREE.PointsMaterial({
    size: cfg.pointSize,
    sizeAttenuation: true,
    vertexColors: true,
  });
  const points = new THREE.Points(geom, mat);
  points.name = cfg.layerId;

  const layerSpec = {
    id: cfg.layerId,
    label: cfg.layerLabel,
    defaultVisible: false,
  };
  if (cfg.layerPath && cfg.layerPath.length) layerSpec.path = cfg.layerPath;

  ctx.render3D({ kind: 'points', object: points, layer: layerSpec });
  ctx.onCleanup(() => { geom.dispose(); mat.dispose(); });

  // Histogram (in the inspector overlay).
  ctx.renderChart((host) =>
    buildHistogram(host, payload?.name ?? cfg.layerLabel, values, n, vmin, vmax,
                   cfg.histogramBins, cfg.histogramScale));

  // Legend (in the 3D viewer overlay) — multi-stop gradient bar that
  // traces the same HSL hue 0.66→0.0 sweep as the cloud (blue → cyan
  // → green → yellow → red). A 2-colour CSS gradient would interpolate
  // RGB linearly and skip the cyan/green/yellow band.
  const HUE_STOPS = 9;
  const gradientStops = [];
  for (let s = 0; s < HUE_STOPS; s++) {
    const t = s / (HUE_STOPS - 1);
    const hueDeg = 0.66 * (1 - t) * 360;
    gradientStops.push(`hsl(${hueDeg.toFixed(0)},90%,50%)`);
  }
  ctx.renderLegend?.({
    title: cfg.layerLabel,
    gradient: {
      min: vmin,
      max: vmax,
      stops: gradientStops,
    },
    note: cfg.histogramScale === 'log' ? 'log-y histogram in inspector' : undefined,
  });

  // JSON stats (in the inspector overlay).
  ctx.renderJSON({
    artifact: payload?.name,
    n,
    valueRange: [vmin, vmax],
    coloredBy: 'HSL gradient (blue → red)',
    layer: cfg.layerId,
  });
}

// ── Shape adapters ────────────────────────────────────────────────────

function collectFromPayload(ctx, payload, xyz) {
  if (!payload) return null;
  const kind = payload.kind;
  if (kind === AK_POINT_COLUMN) {
    return collectFromPointColumn(payload, xyz);
  }
  if (kind === AK_ARRAY_INLINE) {
    return collectFromArrayInline(ctx, payload, xyz);
  }
  return null;
}

function collectFromPointColumn(payload, xyz) {
  const arr = payload.data;
  const n = Math.min(payload.n_items ?? arr.length, xyz.length / 3 | 0);
  const positions = new Float64Array(n * 3);
  const values = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    positions[i * 3 + 0] = xyz[i * 3 + 0];
    positions[i * 3 + 1] = xyz[i * 3 + 1];
    positions[i * 3 + 2] = xyz[i * 3 + 2];
    values[i] = arr[i];
  }
  return { positions, values, n };
}

function collectFromArrayInline(ctx, payload, xyz) {
  const items = payload.items || [];
  const buildings = ctx.getSiblingDecoded?.('per-building-records');
  if (!buildings?.items) {
    ctx.renderHTML(
      `<div class="render-placeholder">${payload.name}: subscribe per-building-records to map records to points.</div>`,
    );
    return null;
  }
  // Pre-count.
  let total = 0;
  for (let bi = 0; bi < items.length; bi++) {
    const u8 = items[bi];
    if (!u8 || u8.byteLength === 0) continue;
    total += u8.byteLength / 4 | 0;
  }
  if (total === 0) return { positions: new Float64Array(0), values: new Float32Array(0), n: 0 };

  const positions = new Float64Array(total * 3);
  const values = new Float32Array(total);
  let cursor = 0;
  for (let bi = 0; bi < items.length; bi++) {
    const u8 = items[bi];
    if (!u8 || u8.byteLength === 0) continue;
    const f32 = new Float32Array(u8.buffer, u8.byteOffset, u8.byteLength / 4 | 0);
    const bldgU8 = buildings.items[bi];
    const hdr = parseBuildingHeader(bldgU8);
    if (!hdr || hdr.n_pts !== f32.length) continue;
    const gidx = hdr.globalIndices;
    for (let i = 0; i < hdr.n_pts; i++) {
      const g = gidx[i];
      positions[cursor * 3 + 0] = xyz[g * 3 + 0];
      positions[cursor * 3 + 1] = xyz[g * 3 + 1];
      positions[cursor * 3 + 2] = xyz[g * 3 + 2];
      values[cursor] = f32[i];
      cursor++;
    }
  }
  return { positions: positions.subarray(0, cursor * 3), values: values.subarray(0, cursor), n: cursor };
}

function computeOrigin(positions, sample) {
  let sx = 0, sy = 0, sz = 0;
  const n = sample;
  for (let i = 0; i < n; i++) {
    sx += positions[i * 3 + 0];
    sy += positions[i * 3 + 1];
    sz += positions[i * 3 + 2];
  }
  return { x: sx / n, y: sy / n, z: sz / n };
}

function clamp01(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

function buildHistogram(host, name, values, n, vmin, vmax, bins, scale) {
  const counts = new Uint32Array(bins);
  const span = vmax - vmin || 1;
  for (let i = 0; i < n; i++) {
    const t = (values[i] - vmin) / span;
    let b = Math.floor(t * bins);
    if (b < 0) b = 0;
    if (b >= bins) b = bins - 1;
    counts[b]++;
  }
  // Hue-matched bar colours
  const labels = new Array(bins);
  const colors = new Array(bins);
  for (let b = 0; b < bins; b++) {
    const t = (b + 0.5) / bins;
    const hue = 0.66 * (1.0 - t);
    labels[b] = (vmin + t * span).toFixed(3);
    colors[b] = `hsl(${(hue * 360).toFixed(0)},90%,50%)`;
  }

  const canvas = document.createElement('canvas');
  canvas.style.width = '100%';
  canvas.style.height = '160px';
  host.appendChild(canvas);

  new Chart(canvas, {
    type: 'bar',
    data: {
      labels,
      datasets: [{
        label: 'count',
        data: Array.from(counts),
        backgroundColor: colors,
        borderWidth: 0,
        barPercentage: 1.0,
        categoryPercentage: 1.0,
      }],
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      plugins: {
        legend: { display: false },
        title: { display: true, text: `${name}  (${vmin.toFixed(3)} → ${vmax.toFixed(3)})`,
                 color: '#8b949e', font: { size: 11, weight: 'normal' } },
        tooltip: { enabled: true },
      },
      scales: {
        x: {
          ticks: { color: '#8b949e', maxTicksLimit: 6 },
          grid: { color: '#21262d' },
        },
        y: {
          type: scale === 'log' ? 'logarithmic' : 'linear',
          ticks: { color: '#8b949e' },
          grid: { color: '#21262d' },
        },
      },
    },
  });
}
