// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: cs-planes — per-plane stats card per building.
//
// Three small Chart.js histograms side-by-side per building:
//   1. Tilt distribution (degrees from horizontal, 0–90).
//   2. Inlier-count distribution (log-y, long-tail dominated by tiny
//      segments — log scale keeps the head readable).
//   3. RMS distribution (fitted-plane residual, m). Should cluster well
//      below distThreshold (0.09 m for V7); anything near or above is
//      a warning sign worth chasing.
//
// Per-record schema (matches v7_seed_plane_record_t in
// v7-normal-cascade-seed-impl.h — 64-byte fixed-stride header + per-
// record variable seed_indices[]):
//   f64×3  normal
//   f64    d
//   f64    tilt
//   f64    azimuth
//   f64    rms
//   u32    inlier_count
//   u32    source_segment_id
//   u32×inlier_count  seed_indices  (LOCAL to building)
//
// The outer artifact is ARRAY_INLINE — one record per building, each
// containing a flat sequence of [u32 record_len][record_bytes] entries
// (matches the hs-segment-list packing convention).

import {
  Chart,
  BarController, BarElement,
  CategoryScale, LinearScale, LogarithmicScale,
  Tooltip, Title, Legend,
} from 'chart.js';

Chart.register(
  BarController, BarElement,
  CategoryScale, LinearScale, LogarithmicScale,
  Tooltip, Title, Legend,
);

const AK_ARRAY_INLINE = 8;
const HEADER_BYTES = 64;

function parsePlanesBlob(u8) {
  // Per-bldg blob uses the 8-aligned framing produced by blob_push in
  // <v3/blob.h>: `{u32 rec_len, u32 zero_pad, payload, pad_to_8}` × N.
  if (!u8 || u8.byteLength < 8) return [];
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  const planes = [];
  let off = 0;
  while (off + 8 <= u8.byteLength) {
    const recLen = dv.getUint32(off, true);
    off += 8;
    if (recLen < HEADER_BYTES || off + recLen > u8.byteLength) break;
    const tilt = dv.getFloat64(off + 32, true);
    const azimuth = dv.getFloat64(off + 40, true);
    const rms = dv.getFloat64(off + 48, true);
    const inlierCount = dv.getUint32(off + 56, true);
    const sourceSegmentId = dv.getUint32(off + 60, true);
    planes.push({ tilt, azimuth, rms, inlierCount, sourceSegmentId });
    off += (recLen + 7) & ~7;
  }
  return planes;
}

function makeHistogram(host, title, values, { bins, range, scale = 'linear', formatX = (v) => v.toFixed(2) }) {
  const wrap = document.createElement('div');
  wrap.style.cssText = 'position:relative;width:100%;height:160px';
  const canvas = document.createElement('canvas');
  wrap.appendChild(canvas);
  host.appendChild(wrap);

  const [vMin, vMax] = range;
  const span = vMax - vMin || 1;
  const counts = new Uint32Array(bins);
  for (const v of values) {
    let b = Math.floor((v - vMin) / span * bins);
    if (b < 0) b = 0;
    if (b >= bins) b = bins - 1;
    counts[b]++;
  }
  const labels = new Array(bins);
  for (let b = 0; b < bins; b++) {
    labels[b] = formatX(vMin + (b + 0.5) / bins * span);
  }

  new Chart(canvas, {
    type: 'bar',
    data: {
      labels,
      datasets: [{
        label: 'count',
        data: Array.from(counts),
        backgroundColor: 'rgba(105, 145, 230, 0.78)',
        borderWidth: 0,
        barPercentage: 1.0,
        categoryPercentage: 1.0,
      }],
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      plugins: {
        legend: { display: false },
        title: {
          display: true,
          text: title,
          color: '#8b949e',
          font: { size: 11, family: 'monospace', weight: 'normal' },
        },
        tooltip: { enabled: true },
      },
      scales: {
        x: {
          ticks: { color: '#8b949e', maxTicksLimit: 6, font: { family: 'monospace', size: 10 } },
          grid: { color: '#21262d' },
        },
        y: {
          type: scale === 'log' ? 'logarithmic' : 'linear',
          ticks: { color: '#8b949e', font: { family: 'monospace', size: 10 } },
          grid: { color: '#21262d' },
        },
      },
    },
  });
}

function buildBuildingCard(host, bi, planes) {
  const card = document.createElement('div');
  card.style.cssText = 'display:flex;flex-direction:column;gap:4px;margin-bottom:8px';
  const title = document.createElement('div');
  title.style.cssText = 'font:11px monospace;color:#8b949e';
  title.textContent = `building #${bi}  •  ${planes.length} seed plane(s)`;
  card.appendChild(title);

  if (planes.length === 0) {
    const note = document.createElement('div');
    note.className = 'render-placeholder';
    note.textContent = 'no seed planes (every cascade segment was rejected by the curvature gate or the LSQ fit failed).';
    card.appendChild(note);
    host.appendChild(card);
    return;
  }

  const row = document.createElement('div');
  row.style.cssText = 'display:flex;flex-direction:row;gap:6px';

  const tiltCol = document.createElement('div');
  tiltCol.style.cssText = 'flex:1 1 0;min-width:0';
  makeHistogram(tiltCol, 'tilt (°)', planes.map(p => p.tilt),
                { bins: 18, range: [0, 90], formatX: (v) => v.toFixed(0) });

  const inlCol = document.createElement('div');
  inlCol.style.cssText = 'flex:1 1 0;min-width:0';
  const maxInl = Math.max(1, ...planes.map(p => p.inlierCount));
  makeHistogram(inlCol, 'inliers (log-y)', planes.map(p => p.inlierCount),
                { bins: 24, range: [0, maxInl], scale: 'log',
                  formatX: (v) => Math.round(v).toString() });

  const rmsCol = document.createElement('div');
  rmsCol.style.cssText = 'flex:1 1 0;min-width:0';
  const maxRms = Math.max(0.01, ...planes.map(p => p.rms));
  makeHistogram(rmsCol, 'rms (m)', planes.map(p => p.rms),
                { bins: 20, range: [0, maxRms],
                  formatX: (v) => v.toFixed(3) });

  row.append(tiltCol, inlCol, rmsCol);
  card.appendChild(row);
  host.appendChild(card);
}

export function render(ctx, data) {
  if (data?.kind !== AK_ARRAY_INLINE || !Array.isArray(data.items)) {
    ctx.renderHTML(`<div class="render-placeholder">cs-planes expected ARRAY_INLINE.</div>`);
    return;
  }

  ctx.renderChart((host) => {
    const root = document.createElement('div');
    root.style.cssText = 'display:flex;flex-direction:column;gap:6px';
    let total = 0;
    for (let bi = 0; bi < data.items.length; bi++) {
      const planes = parsePlanesBlob(data.items[bi]);
      total += planes.length;
      buildBuildingCard(root, bi, planes);
    }
    if (total === 0) {
      const note = document.createElement('div');
      note.className = 'render-placeholder';
      note.textContent = 'No seed planes produced. Check curvature gate threshold + sourceSegmentId of each cascade segment.';
      root.appendChild(note);
    }
    host.appendChild(root);
  });

  // JSON summary in the inspector overlay.
  let totalPlanes = 0;
  let nWalls = 0;
  let nFlat = 0;
  let rmsSum = 0;
  let rmsN = 0;
  for (let bi = 0; bi < data.items.length; bi++) {
    for (const p of parsePlanesBlob(data.items[bi])) {
      totalPlanes++;
      if (p.tilt > 80) nWalls++;
      else if (p.tilt < 10) nFlat++;
      rmsSum += p.rms; rmsN++;
    }
  }
  ctx.renderJSON({
    artifact: 'cs-planes',
    n_buildings: data.items.length,
    n_planes: totalPlanes,
    n_walls_tilt_gt_80: nWalls,
    n_flat_tilt_lt_10: nFlat,
    mean_rms_m: rmsN ? rmsSum / rmsN : null,
    note: 'tilt + inlier-count + rms histograms per building; ▭ y-axis log on inliers.',
  });
}
