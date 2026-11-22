// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: dv2-viz-2d — top-down inspector SVG
// showing the rasterized density grid + detected voids + rim points.
//
// Per-bldg blob layout (matches v7-density-voids-2.c::pack_viz_2d):
//   header (56 B):
//     f64×4 bounds (h0_min, h0_max, h1_min, h1_max),
//     f64   cell_size,
//     i32   cols, rows, rim_dilation,
//     u32   n_points
//   counts:    [u32 len][i32×len]
//   void_ids:  [u32 len][i16×len]
//   rim_mask:  [u32 len][u8×len]
//   voids:     [u32 n][record×n]   (120 B per record — see wrapper)
//   rim_xy:    [u32 n][f64×2·n]
//   rim_aux:   [u32 n][6 B per pt: i16 void_id, u8 inlier, u8 surv,
//                                   u8 dist, u8 filtered]

const AK_ARRAY_INLINE = 8;
const VOID_SUMMARY_BYTES = 120;
const RIM_AUX_BYTES = 6;
const GOLDEN_ANGLE_DEG = 137.508;

function paletteColor(id, alpha = 0.55) {
  const hue = (id * GOLDEN_ANGLE_DEG) % 360;
  return `hsla(${hue.toFixed(0)},78%,55%,${alpha})`;
}

function parseBlob(u8) {
  if (!u8 || u8.byteLength < 60) return null;
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  // `u8` retains the blob_push framing: u32 record_len + u32 pad = 8 bytes,
  // then the record's first field (bounds). The original code skipped only 4
  // (record_len) and missed the 4-byte pad, shifting every field by 4 and
  // tripping the "bad blob" length check. Skip the full 8.
  let off = 8;
  const bounds = {
    h0Min: dv.getFloat64(off + 0, true),
    h0Max: dv.getFloat64(off + 8, true),
    h1Min: dv.getFloat64(off + 16, true),
    h1Max: dv.getFloat64(off + 24, true),
  };
  off += 32;
  const cellSize = dv.getFloat64(off, true); off += 8;
  const cols = dv.getInt32(off, true); off += 4;
  const rows = dv.getInt32(off, true); off += 4;
  const rimDilation = dv.getInt32(off, true); off += 4;
  const nPoints = dv.getUint32(off, true); off += 4;
  const nCells = cols * rows;

  const rdLen = (n) => { const L = dv.getUint32(off, true); off += 4; if (L !== n) throw new Error('bad blob'); };

  rdLen(nCells);
  const counts = new Int32Array(nCells);
  for (let k = 0; k < nCells; k++) counts[k] = dv.getInt32(off + k * 4, true);
  off += nCells * 4;

  rdLen(nCells);
  const voidIds = new Int16Array(nCells);
  for (let k = 0; k < nCells; k++) voidIds[k] = dv.getInt16(off + k * 2, true);
  off += nCells * 2;

  rdLen(nCells);
  const rimMask = new Uint8Array(u8.buffer, u8.byteOffset + off, nCells);
  off += nCells;

  const nVoids = dv.getUint32(off, true); off += 4;
  const voids = [];
  for (let i = 0; i < nVoids; i++) {
    const b = off;
    voids.push({
      cellCount:                dv.getUint32(b +   0, true),
      rimPointCount:            dv.getUint32(b +   4, true),
      unownedRimPointCount:     dv.getUint32(b +   8, true),
      interiorSurvivorCount:    dv.getUint32(b +  12, true),
      area:                     dv.getFloat64(b +  16, true),
      cx:                       dv.getFloat64(b +  24, true),
      cz:                       dv.getFloat64(b +  32, true),
      hasFit:                   u8[b + 40] !== 0,
      fitNormal: [dv.getFloat64(b + 48, true), dv.getFloat64(b + 56, true), dv.getFloat64(b + 64, true)],
      fitD:      dv.getFloat64(b +  72, true),
      fitRms:    dv.getFloat64(b +  80, true),
      fitTilt:   dv.getFloat64(b +  88, true),
      fitAzimuth:dv.getFloat64(b +  96, true),
      fitNCands: dv.getUint32(b + 104, true),
      fitNInliers: dv.getUint32(b + 108, true),
    });
    off += VOID_SUMMARY_BYTES;
  }

  const nRim = dv.getUint32(off, true); off += 4;
  const rimXY = new Float64Array(nRim * 2);
  for (let k = 0; k < nRim * 2; k++) rimXY[k] = dv.getFloat64(off + k * 8, true);
  off += nRim * 16;
  if (dv.getUint32(off, true) !== nRim) throw new Error('rim aux count mismatch');
  off += 4;
  const rimVoid    = new Int16Array(nRim);
  const rimInlier  = new Uint8Array(nRim);
  const rimSurv    = new Uint8Array(nRim);
  const rimDist    = new Uint8Array(nRim);
  const rimFiltered = new Uint8Array(nRim);
  for (let k = 0; k < nRim; k++) {
    const b = off + k * RIM_AUX_BYTES;
    rimVoid[k] = dv.getInt16(b, true);
    rimInlier[k] = u8[b + 2];
    rimSurv[k] = u8[b + 3];
    rimDist[k] = u8[b + 4];
    rimFiltered[k] = u8[b + 5];
  }

  return {
    bounds, cellSize, cols, rows, rimDilation, nPoints,
    counts, voidIds, rimMask: new Uint8Array(rimMask), voids,
    rimXY, rimVoid, rimInlier, rimSurv, rimDist, rimFiltered, nRim,
  };
}

function clamp01(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

function renderSvg(host, bi, blob) {
  const { bounds, cellSize, cols, rows, counts, voidIds, rimMask,
          voids, rimXY, rimVoid, rimInlier, rimSurv, rimFiltered, nRim } = blob;
  // Render at a fixed internal coord space; CSS scales to the column.
  // Aspect ratio comes from the world XZ extent so cells stay square.
  const W = 720;
  const H = Math.max(200, Math.round(W * (bounds.h1Max - bounds.h1Min) / (bounds.h0Max - bounds.h0Min)));

  const ns = 'http://www.w3.org/2000/svg';
  const svg = document.createElementNS(ns, 'svg');
  svg.setAttribute('viewBox', `0 0 ${W} ${H}`);
  svg.setAttribute('preserveAspectRatio', 'xMidYMid meet');
  svg.style.cssText = 'background:#0d1117;display:block;width:100%;height:auto;max-width:100%';

  // Project (h0, h1) → SVG (x flips? we keep h0 along x, h1 along inverted y).
  const w0 = bounds.h0Max - bounds.h0Min;
  const w1 = bounds.h1Max - bounds.h1Min;
  const sx = W / w0;
  const sy = H / w1;
  const px = (a) => (a - bounds.h0Min) * sx;
  const py = (b) => H - (b - bounds.h1Min) * sy;
  const cellPx = sx * cellSize;
  const cellPy = sy * cellSize;

  // Density heatmap: count → log-scale heat.
  let maxC = 0;
  for (const c of counts) if (c > maxC) maxC = c;
  if (maxC === 0) maxC = 1;
  const logMax = Math.log1p(maxC);
  const heat = document.createElementNS(ns, 'g');
  for (let r = 0; r < rows; r++) {
    for (let c = 0; c < cols; c++) {
      const k = r * cols + c;
      const v = counts[k];
      if (v === 0) continue;
      const t = clamp01(Math.log1p(v) / logMax);
      // Cool→warm gradient, low alpha.
      const hue = 220 - 220 * t;
      const rect = document.createElementNS(ns, 'rect');
      rect.setAttribute('x', (px(bounds.h0Min + c * cellSize)).toFixed(2));
      rect.setAttribute('y', (py(bounds.h1Min + (r + 1) * cellSize)).toFixed(2));
      rect.setAttribute('width',  cellPx.toFixed(2));
      rect.setAttribute('height', cellPy.toFixed(2));
      rect.setAttribute('fill', `hsl(${hue.toFixed(0)},65%,${(20 + 35 * t).toFixed(0)}%)`);
      heat.appendChild(rect);
    }
  }
  svg.appendChild(heat);

  // Void overlay: translucent fill per void id; rim cells get a brighter outline.
  const voidLayer = document.createElementNS(ns, 'g');
  for (let r = 0; r < rows; r++) {
    for (let c = 0; c < cols; c++) {
      const k = r * cols + c;
      const vid = voidIds[k];
      if (vid >= 0) {
        const rect = document.createElementNS(ns, 'rect');
        rect.setAttribute('x', (px(bounds.h0Min + c * cellSize)).toFixed(2));
        rect.setAttribute('y', (py(bounds.h1Min + (r + 1) * cellSize)).toFixed(2));
        rect.setAttribute('width',  cellPx.toFixed(2));
        rect.setAttribute('height', cellPy.toFixed(2));
        rect.setAttribute('fill', paletteColor(vid, 0.42));
        voidLayer.appendChild(rect);
      } else if (rimMask[k]) {
        const rect = document.createElementNS(ns, 'rect');
        rect.setAttribute('x', (px(bounds.h0Min + c * cellSize)).toFixed(2));
        rect.setAttribute('y', (py(bounds.h1Min + (r + 1) * cellSize)).toFixed(2));
        rect.setAttribute('width',  cellPx.toFixed(2));
        rect.setAttribute('height', cellPy.toFixed(2));
        rect.setAttribute('fill', 'none');
        rect.setAttribute('stroke', '#ffffff44');
        rect.setAttribute('stroke-width', '0.5');
        voidLayer.appendChild(rect);
      }
    }
  }
  svg.appendChild(voidLayer);

  // Rim points — color by status.
  //   inlier=1 → bright yellow
  //   filtered=1 (dropped by normal filter) → grey x
  //   survivor=1 (interior point in low-density void cell) → cyan
  //   default rim → white
  const ptLayer = document.createElementNS(ns, 'g');
  for (let i = 0; i < nRim; i++) {
    const x = px(rimXY[2 * i + 0]);
    const y = py(rimXY[2 * i + 1]);
    const dot = document.createElementNS(ns, 'circle');
    dot.setAttribute('cx', x.toFixed(2));
    dot.setAttribute('cy', y.toFixed(2));
    dot.setAttribute('r', '1.2');
    let fill = '#ffffff';
    if (rimFiltered[i])      fill = '#666';
    else if (rimInlier[i])   fill = '#fde047';
    else if (rimSurv[i])     fill = '#22d3ee';
    dot.setAttribute('fill', fill);
    dot.setAttribute('opacity', rimFiltered[i] ? '0.4' : '0.95');
    ptLayer.appendChild(dot);
  }
  svg.appendChild(ptLayer);

  // Void labels (id + cell count).
  const labels = document.createElementNS(ns, 'g');
  for (let i = 0; i < voids.length; i++) {
    const v = voids[i];
    const t = document.createElementNS(ns, 'text');
    t.setAttribute('x', px(v.cx).toFixed(2));
    t.setAttribute('y', py(v.cz).toFixed(2));
    t.setAttribute('text-anchor', 'middle');
    t.setAttribute('font-family', 'monospace');
    t.setAttribute('font-size', '11');
    t.setAttribute('fill', '#fff');
    t.setAttribute('stroke', '#000');
    t.setAttribute('stroke-width', '0.5');
    t.textContent = `#${i} (${v.cellCount}, ${v.fitNInliers}/${v.fitNCands})`;
    labels.appendChild(t);
  }
  svg.appendChild(labels);

  const wrap = document.createElement('div');
  wrap.style.cssText = 'display:flex;flex-direction:column;gap:4px;margin-bottom:8px';
  const title = document.createElement('div');
  title.style.cssText = 'font:11px monospace;color:#8b949e';
  title.textContent = `bldg ${bi}  •  ${cols}×${rows} cells (${cellSize.toFixed(3)} m)  •  ${voids.length} void(s), ${nRim} rim pts`;
  wrap.appendChild(title);
  wrap.appendChild(svg);

  // Per-void table.
  const table = document.createElement('table');
  table.style.cssText = 'font:10px monospace;color:#c9d1d9;border-collapse:collapse;width:100%';
  const thead = document.createElement('thead');
  thead.innerHTML = '<tr><th>id</th><th>cells</th><th>area m²</th><th>rim</th><th>cands</th><th>inliers</th><th>tilt</th><th>az</th><th>rms</th></tr>';
  for (const c of thead.querySelectorAll('th')) {
    c.style.cssText = 'text-align:right;padding:1px 4px;border-bottom:1px solid #21262d;color:#8b949e;font-weight:normal';
  }
  table.appendChild(thead);
  const tbody = document.createElement('tbody');
  for (let i = 0; i < voids.length; i++) {
    const v = voids[i];
    const row = document.createElement('tr');
    const cells = [
      `#${i}`,
      String(v.cellCount),
      v.area.toFixed(2),
      String(v.rimPointCount),
      String(v.fitNCands),
      String(v.fitNInliers),
      v.hasFit ? `${v.fitTilt.toFixed(0)}°` : '—',
      v.hasFit ? `${v.fitAzimuth.toFixed(0)}°` : '—',
      v.hasFit ? v.fitRms.toFixed(3) : '—',
    ];
    for (const c of cells) {
      const td = document.createElement('td');
      td.textContent = c;
      td.style.cssText = 'text-align:right;padding:1px 4px';
      row.appendChild(td);
    }
    // Colour the id cell to match the void overlay.
    row.firstChild.style.background = paletteColor(i, 0.55);
    tbody.appendChild(row);
  }
  table.appendChild(tbody);
  wrap.appendChild(table);

  host.appendChild(wrap);
}

export function render(ctx, data) {
  if (data?.kind !== AK_ARRAY_INLINE || !Array.isArray(data.items)) {
    ctx.renderHTML(`<div class="render-placeholder">dv2-viz-2d expected ARRAY_INLINE.</div>`);
    return;
  }
  const root = document.createElement('div');
  root.style.cssText = 'display:flex;flex-direction:column;gap:8px';
  let total = 0;
  for (let bi = 0; bi < data.items.length; bi++) {
    const u8 = data.items[bi];
    if (!u8 || u8.byteLength === 0) continue;
    let blob;
    try { blob = parseBlob(u8); } catch (e) {
      const note = document.createElement('div');
      note.className = 'render-error';
      note.textContent = `bldg ${bi}: ${e.message}`;
      root.appendChild(note);
      continue;
    }
    if (!blob) continue;
    renderSvg(root, bi, blob);
    total += blob.voids.length;
  }
  if (total === 0) {
    const note = document.createElement('div');
    note.className = 'render-placeholder';
    note.textContent = 'no voids detected (every region passed minVoidCells).';
    root.appendChild(note);
  }
  ctx.renderHTML(root);
}
