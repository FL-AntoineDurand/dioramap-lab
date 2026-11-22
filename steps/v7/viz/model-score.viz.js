// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: model-score — per-building intrinsic (no-GT) model-
// quality scorecard. One 72-byte fixed record per building, decoded via
// DataView and rendered as a dark-themed HTML grid of cards (one per
// building) plus a top-of-pane aggregate summary.
//
// Record layout (lockstep with v7_score_intrinsic_pack in
// steps/v7/v7-score-intrinsic-impl.c):
//   u32 version
//   u32 n_planes
//   u32 n_points_total
//   u32 n_points_assigned
//   f64 completeness
//   f64 residual_rms
//   f64 residual_score
//   f64 footprint_coverage
//   f64 footprint_overshoot
//   f64 plane_overlap
//   f64 composite
//                       = 72 B per building
//
// NaN-valued fields render as "n/a" (the rasterisation metrics today are
// all NaN until that follow-up lands). The composite formula in the C
// impl drops NaN inputs and redistributes weight, so the displayed
// composite is meaningful as soon as ANY metric is finite.

const AK_ARRAY_INLINE = 8;
const RECORD_BYTES = 72;
const RMS_REFERENCE = 0.15;  // V7_SCORE_RMS_REFERENCE — for the residual-bar gradient
const V7_SCORE_NA = -1;      // sentinel: impl writes this for "not computed"
                             // (was NaN; switched for v3 -warn-special-float audit
                             // discipline). isOk() = "computed and non-negative".

// All metrics in the score record live in [0, +∞) when computed; -1 is
// the "not computed" sentinel. Treat anything < 0 (or non-finite, for
// defensiveness against bit-flips in future schemas) as n/a.
function isOk(v) {
  return Number.isFinite(v) && v >= 0;
}

function parseRecord(u8) {
  if (!u8 || u8.byteLength < RECORD_BYTES) return null;
  const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  return {
    version:            dv.getUint32 (  0, true),
    n_planes:           dv.getUint32 (  4, true),
    n_points_total:     dv.getUint32 (  8, true),
    n_points_assigned:  dv.getUint32 ( 12, true),
    completeness:       dv.getFloat64( 16, true),
    residual_rms:       dv.getFloat64( 24, true),
    residual_score:     dv.getFloat64( 32, true),
    footprint_coverage: dv.getFloat64( 40, true),
    footprint_overshoot:dv.getFloat64( 48, true),
    plane_overlap:      dv.getFloat64( 56, true),
    composite:          dv.getFloat64( 64, true),
  };
}

// Colour scale: red (0) → amber (50) → green (100). Composite is 0-100;
// per-metric bars convert their [0,1] value × 100 first.
function ramp(v100) {
  if (!isOk(v100)) return '#30363d';  // neutral grey for n/a
  const t = Math.max(0, Math.min(1, v100 / 100));
  // HSL hue 0 (red) → 120 (green); keep saturation/lightness tasteful.
  const hue = (t * 120).toFixed(0);
  return `hsl(${hue},65%,48%)`;
}

const fmtPct  = (v) => isOk(v) ? `${(v * 100).toFixed(1)}%` : 'n/a';
const fmtMet  = (v) => isOk(v) ? `${v.toFixed(3)} m`        : 'n/a';
const fmtComp = (v) => isOk(v) ? v.toFixed(1)               : '—';

// One labelled bar row. `value01` is 0-1 (or NaN); we display `text`
// verbatim alongside.
function metricBar(label, value01, text) {
  const pct100 = isOk(value01) ? Math.max(0, Math.min(1, value01)) * 100 : 0;
  const colour = ramp(isOk(value01) ? pct100 : NaN);
  return `
    <div style="display:grid;grid-template-columns:110px 60px 1fr;gap:6px;align-items:center;margin:2px 0">
      <span style="color:#8b949e;font-size:11px">${label}</span>
      <span style="color:${colour};font-size:11px;text-align:right;font-variant-numeric:tabular-nums">${text}</span>
      <div style="height:6px;background:#21262d;border-radius:3px;overflow:hidden">
        <div style="height:100%;width:${pct100.toFixed(1)}%;background:${colour}"></div>
      </div>
    </div>`;
}

function buildingCard(bi, r) {
  if (!r) {
    return `<div class="render-placeholder" style="padding:6px">building #${bi}: empty score record</div>`;
  }

  // Residual rendered both as a metric (lower-is-better, scaled to
  // RMS_REFERENCE) AND as its companion residual_score bar. We show the
  // raw RMS as the text and use the residual_score for the colour.
  const residualPctBar = isOk(r.residual_score) ? r.residual_score : NaN;

  // Composite header: big number with colour + a slim bar underneath.
  const compColour = ramp(r.composite);

  return `
    <div style="
      background:#0d1117;border:1px solid #21262d;border-radius:6px;
      padding:8px 10px;font-family:system-ui,sans-serif">
      <div style="display:flex;justify-content:space-between;align-items:baseline;margin-bottom:4px">
        <span style="color:#c9d1d9;font-size:12px;font-weight:600">Building #${bi}</span>
        <span style="color:${compColour};font-size:22px;font-weight:700;font-variant-numeric:tabular-nums">
          ${fmtComp(r.composite)}<span style="color:#6e7681;font-size:10px;font-weight:400"> / 100</span>
        </span>
      </div>
      <div style="height:4px;background:#21262d;border-radius:2px;overflow:hidden;margin-bottom:8px">
        <div style="height:100%;width:${isOk(r.composite) ? r.composite.toFixed(1) : 0}%;background:${compColour}"></div>
      </div>

      ${metricBar('completeness',  r.completeness,        fmtPct(r.completeness))}
      ${metricBar('residual RMS',  residualPctBar,        fmtMet(r.residual_rms))}
      ${metricBar('residual sc.',  r.residual_score,      fmtPct(r.residual_score))}
      ${metricBar('fp coverage',   r.footprint_coverage,  fmtPct(r.footprint_coverage))}
      ${metricBar('fp overshoot',  r.footprint_overshoot, fmtPct(r.footprint_overshoot))}
      ${metricBar('plane overlap', r.plane_overlap,       fmtPct(r.plane_overlap))}

      <div style="
        margin-top:6px;padding-top:6px;border-top:1px solid #21262d;
        display:flex;justify-content:space-between;color:#8b949e;font-size:10px;font-family:monospace">
        <span>${r.n_planes} planes</span>
        <span>${r.n_points_assigned}/${r.n_points_total} pts</span>
        <span>v${r.version}</span>
      </div>
    </div>`;
}

function summaryHeader(records) {
  const valid = records.filter(r => r != null);
  if (valid.length === 0) {
    return `<div class="render-placeholder">model-score: no records.</div>`;
  }
  const finite = (arr) => arr.filter(isOk);
  const mean = (xs) => xs.length === 0 ? NaN : xs.reduce((a, b) => a + b, 0) / xs.length;
  const avgComp = mean(finite(valid.map(r => r.composite)));
  const avgCompl = mean(finite(valid.map(r => r.completeness)));
  const avgRes = mean(finite(valid.map(r => r.residual_rms)));
  const totalPts = valid.reduce((s, r) => s + r.n_points_total, 0);
  const totalAss = valid.reduce((s, r) => s + r.n_points_assigned, 0);

  return `
    <div style="
      background:#161b22;border:1px solid #30363d;border-radius:6px;
      padding:8px 12px;margin-bottom:10px;font-family:system-ui,sans-serif;
      display:flex;flex-wrap:wrap;gap:14px 22px;align-items:baseline">
      <span style="color:#c9d1d9;font-size:13px;font-weight:600">model-score · ${valid.length} building${valid.length === 1 ? '' : 's'}</span>
      <span style="color:#8b949e;font-size:11px">avg composite: <strong style="color:${ramp(avgComp)}">${fmtComp(avgComp)}</strong></span>
      <span style="color:#8b949e;font-size:11px">avg completeness: <strong style="color:#c9d1d9">${fmtPct(avgCompl)}</strong></span>
      <span style="color:#8b949e;font-size:11px">avg residual RMS: <strong style="color:#c9d1d9">${fmtMet(avgRes)}</strong></span>
      <span style="color:#8b949e;font-size:11px">points: <strong style="color:#c9d1d9">${totalAss.toLocaleString()}/${totalPts.toLocaleString()}</strong></span>
    </div>`;
}

export function render(ctx, data) {
  if (data?.kind !== AK_ARRAY_INLINE || !Array.isArray(data.items)) {
    ctx.renderHTML(`<div class="render-placeholder">model-score expected ARRAY_INLINE.</div>`);
    return;
  }

  const records = data.items.map(parseRecord);

  // Cards in a responsive grid — minmax(220, 1fr) gives 2-4 columns
  // depending on inspector width, never less than ~220 px wide.
  const cards = records
    .map((r, bi) => buildingCard(bi, r))
    .join('');

  const html = `
    ${summaryHeader(records)}
    <div style="
      display:grid;grid-template-columns:repeat(auto-fill,minmax(220px,1fr));gap:8px">
      ${cards}
    </div>`;

  ctx.renderHTML(html);

  // Also expose the raw decoded values via the JSON pane — handy for
  // copy-paste into a notebook or diffing two runs without screenshots.
  ctx.renderJSON({
    artifact: 'model-score',
    n_buildings: records.length,
    record_bytes: RECORD_BYTES,
    rms_reference: RMS_REFERENCE,
    per_building: records,
  });
}
