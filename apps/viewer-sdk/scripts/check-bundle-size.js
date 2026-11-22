// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Bundle-size guard for the SDK production build.
//
// Asserts:
//   1. dist/dioramap-viewer.js exists.
//   2. raw size <= RAW_LIMIT.
//   3. gzipped size <= GZ_LIMIT.
//   4. the closed-allow-list invariant (PLAN-VIEWER-SDK-PORT §5.8) — the
//      SDK should NOT pull in viz modules it doesn't explicitly import.
//      We check for source-level markers of the EXCLUDED composers
//      (terrain.viz, deshadowed-ortho.viz, ortho-surround-jpeg.viz) by
//      looking for distinctive strings each module sets (the leaf labels
//      that are unique to that composer's render call). If any is found
//      in the bundle, an unintended import path leaked.
//
// Run via `npm run check:bundle-size` (added to package.json by V8). Also
// callable in CI.
//
// Why a separate script and not a Vite plugin: keeps the production build
// configuration dumb (just emit files), and the guard runs as a discrete
// CI step that fails loudly with an actionable diff against the budget.

import { existsSync, readFileSync, statSync } from 'node:fs';
import { gzipSync } from 'node:zlib';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const BUNDLE = resolve(__dirname, '..', 'dist', 'dioramap-viewer.js');

// Budget — set comfortably above today's size, tightened at each release.
// Raw budget is generous because UMD wrapping is verbose. Gzip is the
// number that matters for what the seller actually downloads.
const RAW_LIMIT = 1_100_000;   // 1.1 MB
const GZ_LIMIT  =   220_000;   // 220 kB

// Strings that should NOT appear in the SDK bundle. Each leaf-label is
// distinctive enough to uniquely identify the composer that registered
// it. If you add a new excluded composer, append its label here.
const EXCLUDED_MARKERS = [
  'Local terrain (heightmap + deshadowed ortho)', // terrain.viz             — SDK uses discs (per-vertex ortho)
  'Deshadowed ortho — ground plane',              // deshadowed-ortho.viz    — same reasoning
  'Surround backdrop — flat JPEG',                // ortho-surround-jpeg.viz — SDK uses elevation-surround instead
];

let failed = false;

if (!existsSync(BUNDLE)) {
  console.error(`✗ ${BUNDLE} does not exist — run \`npm run build\` first.`);
  process.exit(1);
}

const raw = readFileSync(BUNDLE);
const gz = gzipSync(raw);

const rawSize = statSync(BUNDLE).size;
const gzSize = gz.length;

console.log(`bundle: ${BUNDLE}`);
console.log(`  raw   ${rawSize.toLocaleString()} bytes  / limit ${RAW_LIMIT.toLocaleString()}`);
console.log(`  gzip  ${gzSize.toLocaleString()} bytes  / limit ${GZ_LIMIT.toLocaleString()}`);

if (rawSize > RAW_LIMIT) {
  console.error(`✗ raw bundle size (${rawSize}) exceeds limit (${RAW_LIMIT}).`);
  failed = true;
}
if (gzSize > GZ_LIMIT) {
  console.error(`✗ gzipped bundle size (${gzSize}) exceeds limit (${GZ_LIMIT}).`);
  failed = true;
}

const text = raw.toString('utf8');
for (const marker of EXCLUDED_MARKERS) {
  if (text.includes(marker)) {
    console.error(
      `✗ excluded composer leaked into bundle — found marker '${marker}'.\n` +
      `  This means an import is transitively pulling in a viz module the SDK\n` +
      `  is supposed to skip (PLAN-VIEWER-SDK-PORT §5.8). Likely cause: someone\n` +
      `  imported '@dioramap/scene-base/viz' (the index) instead of a specific\n` +
      `  viz path, OR a new file imports the index transitively.`
    );
    failed = true;
  }
}

if (failed) process.exit(1);

console.log('✓ bundle-size + composer-isolation checks passed');
