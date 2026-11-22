// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: hs-segment-list — variable-length per-segment
// records (one per cascade segment). Renders a JSON summary table:
// per-segment area (sum of ts-weights), point count, mean normal,
// (normal_group_id, offset_cluster_id, spatial_cluster_id) breakdown.
//
// Replaces v2's separate `cascade-segment-stats` artifact — those stats
// are derivable from the segment list alone.
//
// Per-record schema (matches v7-hemisphere-segment.c pack_segment_record):
//   u32 schema = 1
//   u32 normal_group_id
//   u32 offset_cluster_id
//   u32 spatial_cluster_id
//   f64 peak_offset
//   f64 mean_normal[3]
//   u32 n_pts
//   u32 reserved
//   u32 pt_idx[n_pts]

const AK_ARRAY_INLINE = 8;

function parseSegment(u8) {
  if (u8.byteLength < 56) return null;
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  const schema = dv.getUint32(0, true);
  if (schema !== 1) return null;
  const normalGroupId = dv.getUint32(4, true);
  const offsetClusterId = dv.getUint32(8, true);
  const spatialClusterId = dv.getUint32(12, true);
  const peakOffset = dv.getFloat64(16, true);
  const meanNormal = [dv.getFloat64(24, true), dv.getFloat64(32, true), dv.getFloat64(40, true)];
  const nPts = dv.getUint32(48, true);
  const idxOff = 56;
  if (idxOff + nPts * 4 > u8.byteLength) return null;
  return { normalGroupId, offsetClusterId, spatialClusterId, peakOffset, meanNormal, nPts };
}

// Each per-building record in hs-segment-list is itself a blob of
// 8-aligned segment-records (framed via <v3/blob.h>'s blob_push).
function parsePerBuildingSegments(u8) {
  if (!u8 || u8.byteLength < 8) return [];
  const segs = [];
  let off = 0;
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  while (off + 8 <= u8.byteLength) {
    const len = dv.getUint32(off, true);
    off += 8;
    if (off + len > u8.byteLength) break;
    const seg = parseSegment(new Uint8Array(u8.buffer, u8.byteOffset + off, len));
    if (seg) segs.push(seg);
    off += (len + 7) & ~7;
  }
  return segs;
}

export function render(ctx, data) {
  if (data?.kind !== AK_ARRAY_INLINE || !Array.isArray(data.items)) {
    ctx.renderHTML(
      `<div class="render-placeholder">hs-segment-list expected ARRAY_INLINE.</div>`,
    );
    return;
  }

  const allSegments = [];
  for (let bi = 0; bi < data.items.length; bi++) {
    const segs = parsePerBuildingSegments(data.items[bi]);
    for (const s of segs) {
      allSegments.push({ buildingIdx: bi, ...s });
    }
  }

  if (allSegments.length === 0) {
    ctx.renderHTML(`<div class="render-placeholder">No cascade segments produced.</div>`);
    return;
  }

  // Build a small HTML table — quick scan of segment shapes.
  const rows = allSegments.map((s, i) => {
    const n = s.meanNormal;
    return `<tr>
      <td>${i}</td>
      <td>${s.buildingIdx}</td>
      <td>${s.normalGroupId}</td>
      <td>${s.offsetClusterId}</td>
      <td>${s.spatialClusterId}</td>
      <td>${s.nPts}</td>
      <td>${s.peakOffset.toFixed(3)}</td>
      <td>(${n[0].toFixed(3)}, ${n[1].toFixed(3)}, ${n[2].toFixed(3)})</td>
    </tr>`;
  }).join('');

  const html = `
    <div style="max-height:280px;overflow:auto;font:11px monospace">
      <table style="width:100%;border-collapse:collapse">
        <thead style="position:sticky;top:0;background:#0d1117;color:#8b949e">
          <tr>
            <th>seg</th><th>bldg</th><th>n.grp</th><th>off.cl</th><th>sp.cl</th>
            <th>n_pts</th><th>peak_off (m)</th><th>mean_normal</th>
          </tr>
        </thead>
        <tbody>${rows}</tbody>
      </table>
    </div>`;
  ctx.renderHTML(html);

  ctx.renderJSON({
    artifact: 'hs-segment-list',
    n_segments: allSegments.length,
    n_buildings: data.items.length,
    distinct_normal_groups: new Set(allSegments.map(s => s.normalGroupId)).size,
    largest_segment_pts: allSegments.reduce((m, s) => Math.max(m, s.nPts), 0),
  });
}
