// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Built-in JSON / metadata renderer.
//
// Handles three kinds:
//   - AK_OPAQUE_JSON  — decoded `{ text, json }`; show the parsed tree if
//                        available, otherwise the raw text.
//   - AK_ARRAY_INLINE — decoded `{ n, stride, items_json, items }`; show
//                        a compact summary + items_json if every item parsed.
//   - AK_ARRAY_BY_HASH — decoded `{ n, hashes_hex }`; show the hash list.
//
// The kind is dispatched by renderers/loader.js; `data` here already has the
// decoder's shape.

import { AK } from '@dioramap/protocol';


export function render(ctx, data) {
  switch (data.kind) {
    case AK['OPAQUE_JSON']:
      if (data.json !== null) ctx.renderJSON(data.json);
      else ctx.renderHTML(`<pre class="json-raw">${escape(data.text)}</pre>`);
      return;
    case AK['ARRAY_INLINE']: {
      const allJson = data.items_json?.length > 0
        && data.items_json.every((it) => it != null);
      let items;
      if (allJson) {
        items = data.items_json;
      } else {
        // Raw fixed-stride binary records (e.g. bldg-thresholds, plane
        // equations) aren't JSON — decode them NUMERICALLY so the values are
        // readable instead of a "<N raw items>" placeholder. F64 lanes when
        // the stride is 8-aligned (covers density/thresholds/normals/d), plus
        // a hex preview. Capped so the inspector stays light.
        const MAX_ITEMS = 64;
        items = data.items.slice(0, MAX_ITEMS).map((u8) => decodeRawItem(u8, data.stride));
        if (data.items.length > MAX_ITEMS) {
          items.push(`… ${data.items.length - MAX_ITEMS} more items`);
        }
      }
      ctx.renderJSON({ kind: 'array-inline', n: data.n, stride: data.stride, items });
      return;
    }
    case AK['ARRAY_BY_HASH']:
      ctx.renderJSON({
        kind: 'array-by-hash',
        n: data.n,
        hashes: data.hashes_hex,
      });
      return;
    default:
      ctx.renderJSON({ kind: data.kind, note: 'no specific renderer' });
  }
}

// Decode one raw fixed-stride record into a readable object: F64 lanes (when
// the stride is 8-aligned) + a hex preview. Most v3 ARRAY_INLINE records are
// F64-based (bldg-thresholds, grm-plane-equations, grm-distance-threshold…),
// so the F64 lanes give the actual values; hex is the always-correct fallback
// (and the only useful view for u32/mixed records).
function decodeRawItem(u8, stride) {
  const out = {};
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  if (stride > 0 && stride % 8 === 0) {
    const MAX_LANES = 64;
    const lanes = Math.min(stride / 8, MAX_LANES);
    const f64 = [];
    for (let i = 0; i < lanes; i++) f64.push(dv.getFloat64(i * 8, true));
    if (stride / 8 > MAX_LANES) f64.push('…');
    out.f64 = f64;
  }
  const HEX_MAX = 64;
  const m = Math.min(u8.byteLength, HEX_MAX);
  let hex = '';
  for (let i = 0; i < m; i++) hex += u8[i].toString(16).padStart(2, '0');
  if (u8.byteLength > HEX_MAX) hex += '…';
  out.hex = hex;
  return out;
}

function escape(s) {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[c]));
}
