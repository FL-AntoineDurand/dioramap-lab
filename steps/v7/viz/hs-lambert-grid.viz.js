// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: hs-lambert-grid — F64 N×N density surface
// (Lambert equal-area projection of unit normals). Renders as an SVG
// disc with the heatmap painted via a canvas-backed data URL, with
// peak markers overlaid (size ∝ basin weight, hue = golden-angle palette
// matching hs-segments so peaks correspond visually with segments).
//
// Reads sibling hs-lambert-peaks for the peak markers — if not
// subscribed, just draws the heatmap.
//
// Per-record format (matches v7-hemisphere-segment.c emit_blob):
// One ARRAY_INLINE record per building, payload = N×N F64 density grid.

const AK_ARRAY_INLINE = 8;
const GOLDEN_ANGLE_DEG = 137.508;

function hslPaletteColor(id, lightness = 0.55) {
  return `hsl(${(id * GOLDEN_ANGLE_DEG) % 360},78%,${lightness * 100}%)`;
}

// Heatmap colormap: blue → red via HSL (matches scalar-gradient-cloud
// + bench-v2 'gradient' mode).
function heatmapColor(t01) {
  const hue = 0.66 * (1.0 - Math.max(0, Math.min(1, t01))) * 360;
  return `hsl(${hue.toFixed(0)},90%,50%)`;
}

// Parse a hs-segment-list per-building blob (8-aligned segment records,
// framed via <v3/blob.h>'s blob_push) and build a
// normal_group_id → Int16Array<segmentIds> map.
function buildGroupToSegments(u8) {
  const map = new Map();
  if (!u8 || u8.byteLength < 8) return map;
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  let off = 0;
  while (off + 8 <= u8.byteLength) {
    const recLen = dv.getUint32(off, true);
    off += 8;
    if (recLen < 56 || off + recLen > u8.byteLength) break;
    const schema = dv.getUint32(off + 0, true);
    const groupId = dv.getUint32(off + 4, true);
    // Segment id = its position in the segments array (we don't carry
    // it in the record — global_seg_id is the natural index since
    // emit_segment_record is called in id order).
    if (schema === 1) {
      let lst = map.get(groupId);
      if (!lst) { lst = []; map.set(groupId, lst); }
      // Use the running count as the segment id.
      lst.push(0); // placeholder; we'll re-number below
    }
    off += (recLen + 7) & ~7;
  }
  // Renumber: walk records again, assign sequential ids.
  let idCursor = 0;
  let off2 = 0;
  const final = new Map();
  while (off2 + 8 <= u8.byteLength) {
    const recLen = dv.getUint32(off2, true);
    off2 += 8;
    if (recLen < 56 || off2 + recLen > u8.byteLength) break;
    const schema = dv.getUint32(off2 + 0, true);
    const groupId = dv.getUint32(off2 + 4, true);
    if (schema === 1) {
      let arr = final.get(groupId);
      if (!arr) { arr = []; final.set(groupId, arr); }
      arr.push(idCursor++);
    }
    off2 += (recLen + 7) & ~7;
  }
  return final;
}

function renderDisc(host, gridF64, resolution, peakRecords, ctx, groupToSegments, padding = 12) {
  const size = 240;
  const inner = size - 2 * padding;
  const cellPx = inner / resolution;

  // Find max for colormap.
  let maxV = 0;
  for (let i = 0; i < gridF64.length; i++) {
    if (gridF64[i] > maxV) maxV = gridF64[i];
  }
  if (maxV === 0) maxV = 1;

  // Paint heatmap on a hidden canvas, then embed as <image> in SVG.
  const canvas = document.createElement('canvas');
  canvas.width = resolution;
  canvas.height = resolution;
  const c2d = canvas.getContext('2d');
  const imgData = c2d.createImageData(resolution, resolution);
  const px = imgData.data;
  for (let r = 0; r < resolution; r++) {
    for (let c = 0; c < resolution; c++) {
      const v = gridF64[r * resolution + c] / maxV;
      const t = v;
      // Use the same heatmap as scalar-gradient-cloud.
      // Convert HSL → RGB inline (cheap):
      const h = 0.66 * (1.0 - Math.max(0, Math.min(1, t))) * 360;
      const [R, G, B] = hslToRgb(h, 0.9, 0.5);
      const o = (r * resolution + c) * 4;
      px[o + 0] = R;
      px[o + 1] = G;
      px[o + 2] = B;
      px[o + 3] = (v > 0) ? 255 : 0;
    }
  }
  c2d.putImageData(imgData, 0, 0);

  const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
  svg.setAttribute('width', String(size));
  svg.setAttribute('height', String(size));
  svg.setAttribute('viewBox', `0 0 ${size} ${size}`);
  svg.style.background = '#0d1117';

  // Disc clip (radius √2 mapped to inner / 2).
  const defs = document.createElementNS(svg.namespaceURI, 'defs');
  const clip = document.createElementNS(svg.namespaceURI, 'clipPath');
  clip.setAttribute('id', `disc-clip-${Math.floor(Math.random() * 1e9)}`);
  const clipCircle = document.createElementNS(svg.namespaceURI, 'circle');
  clipCircle.setAttribute('cx', String(size / 2));
  clipCircle.setAttribute('cy', String(size / 2));
  clipCircle.setAttribute('r', String(inner / 2 * (Math.SQRT2 / (Math.SQRT2 + 0.05))));
  clip.appendChild(clipCircle);
  defs.appendChild(clip);
  svg.appendChild(defs);

  const img = document.createElementNS(svg.namespaceURI, 'image');
  img.setAttribute('href', canvas.toDataURL());
  img.setAttribute('x', String(padding));
  img.setAttribute('y', String(padding));
  img.setAttribute('width', String(inner));
  img.setAttribute('height', String(inner));
  img.setAttribute('clip-path', `url(#${clip.getAttribute('id')})`);
  img.setAttribute('image-rendering', 'pixelated');
  svg.appendChild(img);

  // Disc outline.
  const outline = document.createElementNS(svg.namespaceURI, 'circle');
  outline.setAttribute('cx', String(size / 2));
  outline.setAttribute('cy', String(size / 2));
  outline.setAttribute('r', String(inner / 2 * (Math.SQRT2 / (Math.SQRT2 + 0.05))));
  outline.setAttribute('fill', 'none');
  outline.setAttribute('stroke', '#30363d');
  outline.setAttribute('stroke-width', '1');
  svg.appendChild(outline);

  // Peak markers.
  if (peakRecords && peakRecords.length > 0) {
    let maxBW = 0;
    for (const p of peakRecords) maxBW = Math.max(maxBW, p.basinWeight);
    if (maxBW === 0) maxBW = 1;

    for (let i = 0; i < peakRecords.length; i++) {
      const p = peakRecords[i];
      // Project peak normal to disc (u,v) — replicate lambertProjectTyped
      // logic for a single normal. up_axis is implicit; we emit grid-coord
      // peaks already? No, we emit (tilt, azimuth, peak_normal). Project
      // peak_normal here.
      const n = p.peakNormal;
      // Use up_axis = 2 (LAZ) for v3 pipeline output.
      const sign = n[2] < 0 ? -1 : 1;
      const nx = n[0] * sign, ny = n[1] * sign, nz_ = n[2] * sign;
      const denom = 1 + Math.max(0, nz_);
      let u = 0, v = 0;
      if (denom >= 1e-12) {
        const k = Math.sqrt(2 / denom);
        u = k * nx;
        v = k * ny;
      }
      // u, v are in [-√2, √2]; map to disc px (R = √2 + 0.05 in our grid frame).
      const R = Math.SQRT2 + 0.05;
      const cx = size / 2 + (u / R) * (inner / 2);
      const cy = size / 2 - (v / R) * (inner / 2); // y inverted in screen
      const r = 3 + 6 * Math.sqrt(p.basinWeight / maxBW);

      const dot = document.createElementNS(svg.namespaceURI, 'circle');
      dot.setAttribute('cx', cx.toFixed(2));
      dot.setAttribute('cy', cy.toFixed(2));
      dot.setAttribute('r', r.toFixed(2));
      dot.setAttribute('fill', hslPaletteColor(i));
      dot.setAttribute('stroke', '#0d1117');
      dot.setAttribute('stroke-width', '1.5');
      dot.setAttribute('opacity', '0.92');
      dot.style.cursor = 'pointer';
      const title = document.createElementNS(svg.namespaceURI, 'title');
      const segIds = groupToSegments?.get(i) ?? [];
      title.textContent =
        `peak #${i}  (click to highlight in 3D)\n` +
        `tilt ${p.tilt.toFixed(1)}°  azimuth ${p.azimuth.toFixed(1)}°\n` +
        `basin ${p.basinWeight.toFixed(2)} m²  ${p.nAssignedPts} pts\n` +
        `${segIds.length} cascade segment(s) under this peak`;
      dot.appendChild(title);

      // Click → cross-viz selection (highlights matching cascade-
      // segments points). Click again on the same peak → clear.
      dot.addEventListener('click', (evt) => {
        evt.stopPropagation();
        const sel = ctx.sceneState?.selection;
        // Diagnostic: surface state to the console so a user reporting
        // "click does nothing" can verify (a) handler fires, (b) the
        // SceneState exposes a selection slot, (c) the group→segments
        // lookup actually resolved.
        // eslint-disable-next-line no-console
        console.info('[hs-lambert-grid] peak click', {
          peakIndex: i,
          hasSelection: !!sel,
          segIdsCount: segIds.length,
          firstSegIds: segIds.slice(0, 8),
          source: ctx.layerId,
        });
        if (!sel) return;
        const cur = sel.getSelection();
        const isToggle = cur && cur.source === ctx.layerId
          && cur.segmentIds && cur.segmentIds.length === segIds.length
          && segIds.every((id, k) => cur.segmentIds[k] === id);
        if (isToggle || segIds.length === 0) {
          sel.clearSelection();
          dot.setAttribute('stroke', '#0d1117');
          dot.setAttribute('stroke-width', '1.5');
        } else {
          sel.setSelection({
            source: ctx.layerId,
            segmentIds: new Int16Array(segIds),
          });
          // Visual feedback: thicken stroke on the active peak.
          for (const sib of svg.querySelectorAll('circle[data-peak-marker]')) {
            sib.setAttribute('stroke', '#0d1117');
            sib.setAttribute('stroke-width', '1.5');
          }
          dot.setAttribute('stroke', '#ffffff');
          dot.setAttribute('stroke-width', '2.5');
        }
      });
      dot.setAttribute('data-peak-marker', String(i));
      svg.appendChild(dot);
    }
  }

  // Centre cross-hair (disc origin = horizontal-up normal).
  const ch = (axis) => {
    const line = document.createElementNS(svg.namespaceURI, 'line');
    line.setAttribute('x1', axis === 'x' ? String(size / 2 - 6) : String(size / 2));
    line.setAttribute('y1', axis === 'x' ? String(size / 2)     : String(size / 2 - 6));
    line.setAttribute('x2', axis === 'x' ? String(size / 2 + 6) : String(size / 2));
    line.setAttribute('y2', axis === 'x' ? String(size / 2)     : String(size / 2 + 6));
    line.setAttribute('stroke', '#8b949e');
    line.setAttribute('stroke-width', '0.7');
    return line;
  };
  svg.appendChild(ch('x'));
  svg.appendChild(ch('y'));

  host.appendChild(svg);
}

// Tiny HSL→RGB (so we can paint the canvas without relying on the
// browser's CSS HSL parser per pixel — much faster).
function hslToRgb(hDeg, s, l) {
  const c = (1 - Math.abs(2 * l - 1)) * s;
  const h6 = hDeg / 60;
  const x = c * (1 - Math.abs(h6 % 2 - 1));
  let r, g, b;
  if      (h6 < 1) { r = c; g = x; b = 0; }
  else if (h6 < 2) { r = x; g = c; b = 0; }
  else if (h6 < 3) { r = 0; g = c; b = x; }
  else if (h6 < 4) { r = 0; g = x; b = c; }
  else if (h6 < 5) { r = x; g = 0; b = c; }
  else             { r = c; g = 0; b = x; }
  const m = l - c / 2;
  return [Math.round((r + m) * 255), Math.round((g + m) * 255), Math.round((b + m) * 255)];
}

// Parse hs-lambert-peaks per-building record (fixed-stride 56 B —
// 6 doubles + 2 u32). The earlier "64 B" comment in -impl.h was wrong;
// the struct sizeof is 56 (no padding needed since u32 follows after
// six 8-byte doubles → 48 + 8 = 56, already 8-aligned).
// Schema (matches v7_hs_lambert_peak_record_t in -impl.h):
//   f64 tilt, azimuth, basin_weight, peak_normal[3]
//   u32 n_assigned_pts, _reserved
function parsePeakRecords(u8) {
  if (!u8 || u8.byteLength === 0) return [];
  const stride = 56;
  const n = (u8.byteLength / stride) | 0;
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  const out = [];
  for (let i = 0; i < n; i++) {
    const off = i * stride;
    out.push({
      tilt: dv.getFloat64(off + 0, true),
      azimuth: dv.getFloat64(off + 8, true),
      basinWeight: dv.getFloat64(off + 16, true),
      peakNormal: [
        dv.getFloat64(off + 24, true),
        dv.getFloat64(off + 32, true),
        dv.getFloat64(off + 40, true),
      ],
      nAssignedPts: dv.getUint32(off + 48, true),
    });
  }
  return out;
}

export function render(ctx, data) {
  if (data?.kind !== AK_ARRAY_INLINE || !Array.isArray(data.items)) {
    ctx.renderHTML(`<div class="render-placeholder">hs-lambert-grid expected ARRAY_INLINE.</div>`);
    return;
  }

  const peaksArt = ctx.getSiblingDecoded?.('hs-lambert-peaks');
  const segListArt = ctx.getSiblingDecoded?.('hs-segment-list');
  const allPeaks = peaksArt?.items ?? [];

  // Render one disc per building. Most cases = one building (dahouet).
  const root = document.createElement('div');
  root.style.cssText = 'display:flex;flex-wrap:wrap;gap:8px';

  for (let bi = 0; bi < data.items.length; bi++) {
    const u8 = data.items[bi];
    if (!u8 || u8.byteLength === 0) continue;

    // Resolution = sqrt(byteLength / 8). Square grid.
    const resolution = Math.round(Math.sqrt(u8.byteLength / 8));
    if (resolution * resolution * 8 !== u8.byteLength) continue;

    const grid = new Float64Array(u8.buffer, u8.byteOffset, resolution * resolution);
    const peaks = parsePeakRecords(allPeaks[bi]);
    const groupToSegments = segListArt?.items
      ? buildGroupToSegments(segListArt.items[bi])
      : new Map();

    const card = document.createElement('div');
    card.style.cssText = 'display:flex;flex-direction:column;align-items:center;gap:4px';
    const label = document.createElement('div');
    label.style.cssText = 'font:11px monospace;color:#8b949e';
    const peakLabel = peaksArt
      ? `${peaks.length} peaks`
      : 'subscribe hs-lambert-peaks for peak markers';
    const segLabel = segListArt
      ? `(click peaks to highlight segments in 3D)`
      : `(subscribe hs-segment-list for click-to-highlight)`;
    label.textContent = `building #${bi}  •  ${resolution}×${resolution} grid  •  ${peakLabel}  •  ${segLabel}`;
    card.appendChild(label);
    renderDisc(card, grid, resolution, peaks, ctx, groupToSegments);
    root.appendChild(card);
  }

  ctx.renderHTML(root);

  // If this viz is removed, drop any selection it may have set (avoids
  // a stale highlight pointing at indices the user can no longer
  // navigate from).
  ctx.onCleanup(() => {
    ctx.sceneState?.selection?.clearIfSource(ctx.layerId);
  });

  ctx.renderJSON({
    artifact: 'hs-lambert-grid',
    n_buildings: data.items.length,
    note: 'colours match hs-segments palette by Lambert peak index. Click a peak to highlight matching points in 3D.',
  });
}
