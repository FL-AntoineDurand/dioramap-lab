// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: hs-offset-kde — per-Lambert-group weighted
// offset KDE curves with peak / valley markers. One Chart.js multi-line
// plot per building.
//
// Per-group line colour matches the hs-segments palette by
// normal_group_id (golden-angle palette), so a peak in the Lambert disc,
// a curve here, and the corresponding 3D segments all share a hue.
//
// Per-record schema (matches v7-hemisphere-segment-impl.h
// v7_hs_offset_kde_record_t + the C step's pack-as-blob):
//   u32     normal_group_id
//   u32     n_eval
//   u32     n_peaks
//   u32     n_valleys
//   double  bandwidth                    (struct field is `double`)
//   double  eval_pts[n_eval]
//   double  densities[n_eval]
//   double  peaks[n_peaks][2]            (center, density)
//   double  valleys[n_valleys][2]
// Header is 24 B (4×u32 + f64). Doubles inside the inner blob start at
// non-8-aligned u8 offsets (because the inner blob has a 4-byte length
// prefix), so the parser uses DataView reads exclusively — typed-array
// views over the underlying ArrayBuffer would throw on misalignment.
//
// Outer artifact is ARRAY_INLINE — one OUTER record per building. The
// OUTER record is itself a u32-length-prefixed packed blob of GROUP
// records (the C step builds kde_blob via blob_push and emits its
// entire bytes as a single ARRAY_INLINE record per building, since
// outer-records-per-building keeps building-N alignment with the
// other artifacts).

import {
  Chart,
  LineController, LineElement, PointElement,
  CategoryScale, LinearScale,
  Tooltip, Title, Legend,
  Filler,
} from 'chart.js';

Chart.register(
  LineController, LineElement, PointElement,
  CategoryScale, LinearScale,
  Tooltip, Title, Legend,
  Filler,
);

const AK_ARRAY_INLINE = 8;
const GOLDEN_ANGLE_DEG = 137.508;

function paletteColor(id, alpha = 1.0) {
  const hue = (id * GOLDEN_ANGLE_DEG) % 360;
  return `hsla(${hue.toFixed(0)},78%,55%,${alpha})`;
}

// Walk the per-building blob: 8-aligned records (see <v3/blob.h>),
// each holding the 24-byte header above + n_eval × 16 + n_peaks × 16 +
// n_valleys × 16 bytes.
function parseGroupRecords(u8) {
  if (!u8 || u8.byteLength < 8) return [];
  const groups = [];
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  let off = 0;
  while (off + 8 <= u8.byteLength) {
    const recLen = dv.getUint32(off, true);
    off += 8;
    if (recLen < 24 || off + recLen > u8.byteLength) break;
    const headBase = off;
    const normalGroupId = dv.getUint32(headBase + 0, true);
    const nEval = dv.getUint32(headBase + 4, true);
    const nPeaks = dv.getUint32(headBase + 8, true);
    const nValleys = dv.getUint32(headBase + 12, true);
    const bandwidth = dv.getFloat64(headBase + 16, true);
    let p = headBase + 24; // header is 24 bytes
    if (p + nEval * 8 + nEval * 8 + nPeaks * 16 + nValleys * 16 > headBase + recLen) {
      // Truncated record — skip.
      off += (recLen + 7) & ~7;
      continue;
    }
    // DataView.getFloat64 has no alignment requirement; typed-array
    // views do, so we copy into a fresh Float64Array. nEval is small
    // (default 200), so the copy is cheap.
    const evalPts = new Float64Array(nEval);
    for (let k = 0; k < nEval; k++) evalPts[k] = dv.getFloat64(p + k * 8, true);
    p += nEval * 8;
    const densities = new Float64Array(nEval);
    for (let k = 0; k < nEval; k++) densities[k] = dv.getFloat64(p + k * 8, true);
    p += nEval * 8;
    const peaks = [];
    for (let i = 0; i < nPeaks; i++) {
      peaks.push({
        center: dv.getFloat64(p + 0, true),
        density: dv.getFloat64(p + 8, true),
      });
      p += 16;
    }
    const valleys = [];
    for (let i = 0; i < nValleys; i++) {
      valleys.push({
        center: dv.getFloat64(p + 0, true),
        density: dv.getFloat64(p + 8, true),
      });
      p += 16;
    }
    groups.push({ normalGroupId, nEval, nPeaks, nValleys, bandwidth, evalPts, densities, peaks, valleys });
    off += recLen;
  }
  return groups;
}

function buildBuildingChart(host, bi, groups) {
  if (groups.length === 0) {
    const note = document.createElement('div');
    note.className = 'render-placeholder';
    note.textContent = `building #${bi}: no offset KDE groups (no Lambert peak survived basin filter).`;
    host.appendChild(note);
    return;
  }

  // Compute a shared x-axis range across groups (offsets are scene-units;
  // groups don't share a scale otherwise). Use 200 sample points across
  // the min/max of all eval_pts.
  let xMin = Infinity, xMax = -Infinity;
  for (const g of groups) {
    if (g.nEval === 0) continue;
    if (g.evalPts[0] < xMin) xMin = g.evalPts[0];
    if (g.evalPts[g.nEval - 1] > xMax) xMax = g.evalPts[g.nEval - 1];
  }
  if (!Number.isFinite(xMin)) { xMin = 0; xMax = 1; }
  const N = 200;
  const labels = new Array(N);
  for (let i = 0; i < N; i++) {
    labels[i] = (xMin + (xMax - xMin) * (i / (N - 1))).toFixed(3);
  }

  // Resample each group's KDE onto the shared grid (linear interp).
  const datasets = [];
  for (const g of groups) {
    if (g.nEval < 2) continue;
    const data = new Array(N);
    const lo = g.evalPts[0];
    const hi = g.evalPts[g.nEval - 1];
    const step = (hi - lo) / (g.nEval - 1);
    for (let i = 0; i < N; i++) {
      const x = xMin + (xMax - xMin) * (i / (N - 1));
      if (x <= lo || x >= hi) {
        data[i] = 0;
        continue;
      }
      const t = (x - lo) / step;
      const idx = Math.max(0, Math.min(g.nEval - 2, Math.floor(t)));
      const f = t - idx;
      data[i] = g.densities[idx] * (1 - f) + g.densities[idx + 1] * f;
    }
    datasets.push({
      label: `grp ${g.normalGroupId}  (bw ${g.bandwidth.toFixed(3)}, ${g.nPeaks}p ${g.nValleys}v)`,
      data,
      borderColor: paletteColor(g.normalGroupId, 0.9),
      backgroundColor: paletteColor(g.normalGroupId, 0.1),
      borderWidth: 1.5,
      pointRadius: 0,
      tension: 0,
      fill: false,
    });
  }

  // Peak / valley markers — drawn as separate scatter datasets so the
  // shapes pop out without cluttering the line legend. One marker
  // dataset per group.
  for (const g of groups) {
    if (g.peaks.length > 0) {
      datasets.push({
        type: 'line',
        label: `pk ${g.normalGroupId}`,
        data: g.peaks.map(p => ({
          x: ((p.center - xMin) / (xMax - xMin) * (N - 1)),
          y: p.density,
        })),
        showLine: false,
        pointRadius: 5,
        pointStyle: 'triangle',
        backgroundColor: paletteColor(g.normalGroupId, 0.95),
        borderColor: '#0d1117',
        borderWidth: 1,
        order: -1, // draw on top
      });
    }
    if (g.valleys.length > 0) {
      datasets.push({
        type: 'line',
        label: `vl ${g.normalGroupId}`,
        data: g.valleys.map(v => ({
          x: ((v.center - xMin) / (xMax - xMin) * (N - 1)),
          y: v.density,
        })),
        showLine: false,
        pointRadius: 4,
        pointStyle: 'crossRot',
        backgroundColor: paletteColor(g.normalGroupId, 0.6),
        borderColor: '#0d1117',
        borderWidth: 1,
        order: -1,
      });
    }
  }

  const card = document.createElement('div');
  card.style.cssText = 'display:flex;flex-direction:column;gap:4px;margin-bottom:8px';
  const title = document.createElement('div');
  title.style.cssText = 'font:11px monospace;color:#8b949e';
  title.textContent = `building #${bi}  •  ${groups.length} Lambert group(s)  •  offset (m) on x-axis`;
  card.appendChild(title);

  // Chart.js with responsive:true + maintainAspectRatio:false reads
  // size from the canvas's parent. If that parent's height is content-
  // driven (flex-column auto height), the canvas grows the parent which
  // triggers another resize — runaway feedback. Wrap in a sized,
  // position:relative box to break the loop.
  const canvasWrap = document.createElement('div');
  canvasWrap.style.cssText = 'position:relative;width:100%;height:240px';
  const canvas = document.createElement('canvas');
  canvasWrap.appendChild(canvas);
  card.appendChild(canvasWrap);
  host.appendChild(card);

  new Chart(canvas, {
    type: 'line',
    data: { labels, datasets },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: false,
      interaction: { mode: 'nearest', intersect: false },
      plugins: {
        legend: {
          display: true,
          labels: {
            color: '#8b949e',
            font: { size: 10, family: 'monospace' },
            filter: (item) => !item.text.startsWith('pk ') && !item.text.startsWith('vl '),
            usePointStyle: false,
          },
        },
        tooltip: { enabled: true, callbacks: {
          title: (items) => items[0]?.label ? `offset = ${items[0].label} m` : '',
        }},
        title: { display: false },
      },
      scales: {
        x: {
          ticks: { color: '#8b949e', maxTicksLimit: 8, autoSkip: true },
          grid: { color: '#21262d' },
          title: { display: true, text: 'plane offset along group mean-normal (m)',
                   color: '#6e7681', font: { size: 10 } },
        },
        y: {
          ticks: { color: '#8b949e' },
          grid: { color: '#21262d' },
          title: { display: true, text: 'weighted density',
                   color: '#6e7681', font: { size: 10 } },
        },
      },
    },
  });
}

export function render(ctx, data) {
  if (data?.kind !== AK_ARRAY_INLINE || !Array.isArray(data.items)) {
    ctx.renderHTML(`<div class="render-placeholder">hs-offset-kde expected ARRAY_INLINE.</div>`);
    return;
  }

  ctx.renderChart((host) => {
    const root = document.createElement('div');
    root.style.cssText = 'display:flex;flex-direction:column;gap:6px';
    let totalGroups = 0;
    for (let bi = 0; bi < data.items.length; bi++) {
      const groups = parseGroupRecords(data.items[bi]);
      totalGroups += groups.length;
      buildBuildingChart(root, bi, groups);
    }
    if (totalGroups === 0) {
      const note = document.createElement('div');
      note.className = 'render-placeholder';
      note.textContent = 'No offset KDE groups produced (every Lambert peak fell below the basin-area threshold, OR emitVizArtifacts=false).';
      root.appendChild(note);
    }
    host.appendChild(root);
  });

  let totalGroups = 0;
  for (let bi = 0; bi < data.items.length; bi++) {
    totalGroups += parseGroupRecords(data.items[bi]).length;
  }
  ctx.renderJSON({
    artifact: 'hs-offset-kde',
    n_buildings: data.items.length,
    n_groups: totalGroups,
    note: 'colours match hs-segments palette by normal_group_id; ▲ = KDE peak, ✕ = valley',
  });
}
