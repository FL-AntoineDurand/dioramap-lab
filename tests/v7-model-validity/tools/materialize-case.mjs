#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// tests/v7-model-validity/tools/materialize-case.mjs
//
// Given a bench-v2 synthetic test case id (e.g. "flat-default"):
//   - materialize the point cloud (positions + classifications + footprint)
//     via bench-v2's loadTestCase
//   - write it as v2 TCBD .bin sidecar + v2-shaped .json spec, so v3's
//     `v7-load-real-test-case` step can consume it
//   - extract the analytic ground truth from the roof-types.js geometry
//     (plane count, plane normals + areas, edge count by type, footprint
//     polygon) into a ground-truth.json file next to the spec
//
// Usage:
//   node materialize-case.mjs <case-id> <out-dir>
//
// Produces (relative to <out-dir>):
//   <case-id>.bin         — TCBD binary sidecar
//   <case-id>.json        — v2-shaped spec, with footprint + l93_center
//                           pointing at the .bin (so v7-load-real-test-case
//                           can locate the data file)
//   ground-truth.json     — analytic GT extracted from the generator

import { readFileSync, writeFileSync } from 'node:fs';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

// Locate bench-v2 — sibling repo.  Allow override via $BENCH_V2_DIR.
const SELF_DIR = dirname(fileURLToPath(import.meta.url));
const DEFAULT_V2 = resolve(SELF_DIR, '../../../../lidar-algos-bench-v2');
const BENCH_V2 = process.env.BENCH_V2_DIR
    ? resolve(process.env.BENCH_V2_DIR)
    : DEFAULT_V2;

const { loadTestCase } = await import(`${BENCH_V2}/lib/test-case-loader.js`);
const { writeTestCaseBinary } = await import(`${BENCH_V2}/lib/test-case-binary.js`);

const [caseId, outDir] = process.argv.slice(2);
if (!caseId || !outDir) {
    console.error('usage: materialize-case.mjs <case-id> <out-dir>');
    process.exit(2);
}

const specPath = join(BENCH_V2, 'test-cases-v2', `${caseId}.json`);
const spec = JSON.parse(readFileSync(specPath, 'utf8'));

if (spec.type !== 'synthetic') {
    console.error(`case ${caseId} is not synthetic (type=${spec.type}); B.1 only handles synthetic`);
    process.exit(2);
}

// Materialize the point cloud + the analytic roof geometry.
const inputs = await loadTestCase(specPath, { baseDir: dirname(specPath) });
// `inputs.footprint` is the 2D XZ footprint polygon.
// `inputs.groundTruth` is { planes[], surfaces?, edges[], total_roof_area_m2, ground_level }.
// `inputs.positions` / `.classifications` are the materialized cloud
// (Float32 / Uint8 typed arrays).

// Write the TCBD .bin in v2's expected format (so v7-load-real-test-case reads it).
// v3's v7-load-real-test-case declares scan-angles / point-source-ids /
// return-numbers / number-of-returns as REQUIRED writes (the writes_arr
// doesn't mark them optional), so we must supply them.  bench-v2 generates
// these for flight-strip clouds via flight-strip-generator; for plain
// grid clouds they're missing — synthesise zero-filled stubs in that case
// so the bin has the required flag bits set.
const binPath = join(outDir, `${caseId}.bin`);
const nPts = inputs.positions.length / 3;
const f32zeros = () => new Float32Array(nPts);   // scan_angles (deg)
const u16zeros = () => new Uint16Array(nPts);    // point_source_ids
const u8zeros  = () => new Uint8Array(nPts);     // return_numbers, number_of_returns
writeTestCaseBinary(binPath, {
    positions: inputs.positions,
    classifications: inputs.classifications,
    scanAngles:      inputs.scanAngles      ? inputs.scanAngles      : f32zeros(),
    pointSourceIds:  inputs.pointSourceIds  ? inputs.pointSourceIds  : u16zeros(),
    returnNumbers:   inputs.returnNumbers   ? inputs.returnNumbers   : u8zeros(),
    numberOfReturns: inputs.numberOfReturns ? inputs.numberOfReturns : u8zeros(),
});

// Write a v2-shaped spec that points at the .bin we just made.  v7-load-real-test-case
// reads metadata.l93_center to invert the local-frame translation; for synthetic
// data we have no L93 frame, so pick (0,0) and document.
const v2SpecOut = {
    type: 'real',                                        // pretend-real so v3 reads .bin
    id: caseId,
    dataFile: `${caseId}.bin`,
    footprint: inputs.footprint.map(p => ({ x: p.x, z: p.z })),
    metadata: {
        l93_center: { x: 0, y: 0 },                      // synthetic origin
        synthetic_origin: spec,                          // keep original spec for trace
    },
};
const specOutPath = join(outDir, `${caseId}.json`);
writeFileSync(specOutPath, JSON.stringify(v2SpecOut, null, 2));

// Extract analytic ground truth from the roof geometry.
// bench-v2 includes wall planes (id="wall_*", tilt_deg=90) in the GT
// even though v7 is a ROOF-recon pipeline — walls are context, not target.
// We expose both `plane_count_total` (everything, including walls) and
// `roof_plane_count` (filters out walls).  The plane_count_match metric
// uses roof_plane_count.
const groundTruth = inputs.groundTruth;
const isRoofPlane = (p) => {
    const id = (p.id || '').toString();
    if (id.startsWith('wall_')) return false;
    // Tilt 90° → vertical → wall.  Synthetic uses exact 90.0; cap at 89.5
    // to be safe.
    if (typeof p.tilt_deg === 'number' && p.tilt_deg >= 89.5) return false;
    return true;
};
const roofPlanes = groundTruth.planes.filter(isRoofPlane);
const gt = {
    case_id: caseId,
    roof_type: spec.roofType,
    plane_count_total: groundTruth.planes.length,
    roof_plane_count: roofPlanes.length,
    plane_count: roofPlanes.length,    // backward-compat alias for the metric
    planes: groundTruth.planes.map(p => ({
        id: p.id,
        normal: { x: p.normal.x, y: p.normal.y, z: p.normal.z },
        d: p.d,
        tilt_deg: p.tilt_deg,
        azimuth_deg: p.azimuth_deg,
        area_m2: p.area_m2,
        n_vertices: p.vertices ? p.vertices.length : null,
    })),
    edges_by_type: {},
    edge_count: groundTruth.edges.length,
    footprint_vertex_count: inputs.footprint.length,
    total_roof_area_m2: groundTruth.total_roof_area_m2,
};
for (const e of groundTruth.edges) {
    gt.edges_by_type[e.type] = (gt.edges_by_type[e.type] || 0) + 1;
}
const gtPath = join(outDir, 'ground-truth.json');
writeFileSync(gtPath, JSON.stringify(gt, null, 2));

// Print a one-line summary the run.sh harness consumes.
console.log(JSON.stringify({
    case_id: caseId,
    out_dir: outDir,
    bin: binPath,
    spec: specOutPath,
    ground_truth: gtPath,
    n_points: inputs.positions.length / 3,
    n_planes_gt: gt.plane_count,
}));
