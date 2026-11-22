// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand

const t = (s) => s;

// elevation-surround.viz.js — surround terrain mesh, textured.
//
// Reads:
//   elevation-surround-f32  AK_RASTER_F32 (cols, rows, 1 channel — heights in m)
//   ortho-surround-jpeg     AK_IMAGE_JPEG  (the surround aerial photo)
//   canonical-location      OPAQUE_JSON {bbox_l93}
//   scene-origin            OPAQUE_JSON {x, y, z}
//
// Builds a vertex-displaced PlaneGeometry covering bbox_l93 expanded by
// `SURROUND_PADDING_M` on each side. Vertex z = elevation - origin.z.
// The JPEG is decoded asynchronously (createImageBitmap) and applied
// as the diffuse texture once it lands; until then the mesh shows with
// a neutral grey base colour so the geometry is visible immediately.
//
// This was two separate composites in S4 (elevation-surround.viz +
// ortho-surround-jpeg.viz) — they merged here at V4-polish so the
// surround actually looks like the legacy SDK's textured surround
// rather than two overlapping flat planes.

import * as THREE from 'three';

export const meta = {
  name: 'Surround terrain (heightmap + ortho)',
  requires: [
    'elevation-surround-f32',
    'ortho-surround-jpeg',
    'canonical-location',
    'scene-origin',
  ],
  pipelines: '*',
  default: true,
};

// Half-side of the surround tile in metres, centred on canonical-location's
// center_l93_{x,y}. Must stay in sync with the `radius_m` default in
// v3/steps/fetchers/fetch-ortho-surround.c and fetch-elevation-surround.c
// (both default to 600 m). Without this match the mesh gets sized to a
// different bbox than the data covers, squishing the texture + heightmap
// into the wrong footprint.
const SURROUND_RADIUS_M = 600;

export function render(ctx, decoded) {
  const raster = decoded['elevation-surround-f32'];
  const jpeg = decoded['ortho-surround-jpeg'];
  const loc = decoded['canonical-location'];
  const origin = decoded['scene-origin']?.json;
  if (!raster?.channels?.length || !loc?.json?.bbox_l93 || !origin) return;

  const { cols, rows, channels } = raster;
  const elev = channels[0];

  // Surround extent: ±SURROUND_RADIUS_M around canonical-location's
  // center, NOT bbox_l93 (bbox_l93 is the lidar disc bbox at 75 m;
  // the surround tile is 8× wider).
  const cxL93 = loc.json.center_l93_x;
  const cyL93 = loc.json.center_l93_y;
  if (!Number.isFinite(cxL93) || !Number.isFinite(cyL93)) return;
  const sizeX = SURROUND_RADIUS_M * 2;
  const sizeY = SURROUND_RADIUS_M * 2;
  const cx = cxL93 - origin.x;
  const cy = cyL93 - origin.y;

  // PlaneGeometry vertex order: row 0 at +Y/2 (top of plane → +north
  // in our Z-up scene after mesh.position translation), going down to
  // row=heightSegments at -Y/2 (-south).
  //
  // Source data orientation — IGN WMS-BIL is delivered north-up: image
  // pixel (0,0) is at the top-left, which corresponds to the bbox's
  // (xmin, ymax) corner. So raster row 0 = NORTH. (This contradicts the
  // raster_format.h docstring's "row 0 at ymin" but matches what the
  // wrapped artifact actually contains — the C step preserves BIL row
  // order when wrapping in the header.)
  //
  // Mapping: vertex y=0 (top of plane = north) → raster row 0 (north
  // of BIL). No flip needed.
  //
  // No-data: IGN's BIL returns very large magnitude sentinels (-99999
  // or similar) for cells outside the dataset / sea / etc. We range-
  // check against [NODATA_MIN, NODATA_MAX] then run a MAD-based
  // outlier pass to catch single bogus values that pass the range
  // check. Invalid cells get z=0 (sea-level) instead of being culled
  // — the dominant cause of no-data is sea offshore, where a flat
  // surface with the ortho mapped on it is the right look.
  const NODATA_MIN = -2000;  // below the lowest land point on Earth
  const NODATA_MAX = 9500;   // above the highest peak
  const geom = new THREE.PlaneGeometry(sizeX, sizeY, cols - 1, rows - 1);
  const pos = geom.attributes.position;

  // Two-pass validity. First pass: range-check sentinel values (-99999
  // etc.). Second pass: outlier rejection — find median + MAD of the
  // range-clean cells, then mark anything more than 6×MAD from the
  // median as invalid too. This catches single bogus 5000 m peaks in
  // 30 m terrain that the simple range check lets through.
  const validFlag = new Uint8Array(pos.count);
  const tmpZ = new Float32Array(pos.count);
  for (let i = 0; i < pos.count; i++) {
    const x = i % cols;
    const y = Math.floor(i / cols);
    const v = elev[y * cols + x];
    const ok = Number.isFinite(v) && v > NODATA_MIN && v < NODATA_MAX;
    validFlag[i] = ok ? 1 : 0;
    tmpZ[i] = ok ? v - origin.z : 0;
  }
  // Median + MAD of the range-clean cells.
  const validZs = [];
  for (let i = 0; i < pos.count; i++) if (validFlag[i]) validZs.push(tmpZ[i]);
  if (validZs.length > 0) {
    validZs.sort((a, b) => a - b);
    const median = validZs[validZs.length >> 1];
    const dev = validZs.map((v) => Math.abs(v - median));
    dev.sort((a, b) => a - b);
    const mad = dev[dev.length >> 1] || 1;
    const reject = mad * 6;
    for (let i = 0; i < pos.count; i++) {
      if (validFlag[i] && Math.abs(tmpZ[i] - median) > reject) {
        validFlag[i] = 0;
      }
    }
  }
  // ── Fill no-data via nearest-neighbour dilation ──────────────────
  // IGN's RGE ALTI HIGHRES layer is land-only — sea cells return a
  // -99999 sentinel and we have no real bathymetry. Earlier choices
  // (z=0 in scene; -origin.z = MSL; global minZ) all looked wrong
  // somewhere — either tsunami above land, deep cliff into the void,
  // or a flat patch at a far-away valley's elevation.
  //
  // Right approach: for each invalid cell, propagate the elevation
  // of the nearest valid neighbour outward via iterative dilation.
  //   pass N: every still-invalid cell with ≥1 valid 4-neighbour
  //           adopts the AVERAGE Z of those neighbours, marks itself
  //           valid for pass N+1.
  //   stops when no progress (or after maxPasses iterations).
  //
  // Use cases this handles correctly:
  //   - SEA OFFSHORE: invalid cells touch the coast. After 1 pass
  //     the coast-adjacent sea ring takes coast Z. After 2 passes,
  //     2-deep ring takes the (newly-filled) Z. Converges to a flat
  //     sea at coast elevation — looks like the coast continues
  //     outward.
  //   - INLAND GAP (rare): smoothly interpolates across the hole
  //     using surrounding terrain, no abrupt step.
  //   - WHOLE-TILE no-data (defensive — shouldn't happen with a
  //     valid IGN bbox but guard against it): leaves cells at z=0,
  //     ortho still mapped flat.
  //
  // Cost: 256×256 ≈ 65k cells × ~256 worst-case passes = sub-50ms.
  const filledThisPass = new Uint8Array(pos.count);
  const maxPasses = Math.max(rows, cols);
  let dilateChanged = true;
  let dilatePasses = 0;
  while (dilateChanged && dilatePasses < maxPasses) {
    dilateChanged = false;
    filledThisPass.fill(0);
    for (let i = 0; i < pos.count; i++) {
      if (validFlag[i]) continue;
      const x = i % cols;
      const y = Math.floor(i / cols);
      let sum = 0, count = 0;
      if (x > 0        && validFlag[i - 1])    { sum += tmpZ[i - 1];    count++; }
      if (x < cols - 1 && validFlag[i + 1])    { sum += tmpZ[i + 1];    count++; }
      if (y > 0        && validFlag[i - cols]) { sum += tmpZ[i - cols]; count++; }
      if (y < rows - 1 && validFlag[i + cols]) { sum += tmpZ[i + cols]; count++; }
      if (count > 0) {
        tmpZ[i] = sum / count;
        filledThisPass[i] = 1;
        dilateChanged = true;
      }
    }
    // Promote newly-filled cells to valid (only AFTER the full pass,
    // so within a pass we read pre-existing valid cells only — keeps
    // dilation deterministic, no left-to-right propagation bias).
    for (let i = 0; i < pos.count; i++) {
      if (filledThisPass[i]) validFlag[i] = 1;
    }
    dilatePasses++;
  }

  // Min/max for downstream consumers (currently unused; kept for
  // future bbox / fog computations).
  let minZ = Infinity, maxZ = -Infinity;
  for (let i = 0; i < pos.count; i++) {
    if (!validFlag[i]) continue;
    const z = tmpZ[i];
    if (z < minZ) minZ = z;
    if (z > maxZ) maxZ = z;
  }
  if (!Number.isFinite(minZ)) { minZ = 0; maxZ = 0; }

  // Write Z to every vertex. validFlag now true everywhere except
  // the pathological all-invalid case (which keeps tmpZ at the
  // initial 0).
  for (let i = 0; i < pos.count; i++) {
    pos.setZ(i, validFlag[i] ? tmpZ[i] : 0);
  }

  // Full index buffer — every triangle, including those touching former
  // no-data cells (now z=0). Boundary triangles between a valid ridge
  // and a sea cell will slope from ridge elevation down to z=0, which
  // is visually correct for a coastline. The previous approach culled
  // these triangles, producing invisible holes; the user-visible
  // alternative (per-fragment discard with vValid) left smooth dive
  // zones across boundary triangles. Flat-floor + slope is the right
  // compromise for the dominant use case (sea offshore).
  //
  // Triangle winding: counterclockwise as seen from +Z (so the
  // front face points up after computeVertexNormals). PlaneGeometry's
  // stock vertex labelling matches this winding.
  const indices = [];
  for (let row = 0; row < rows - 1; row++) {
    for (let col = 0; col < cols - 1; col++) {
      const a = row * cols + col;
      const b = a + 1;
      const c = a + cols;
      const d = c + 1;
      indices.push(a, c, b);
      indices.push(c, d, b);
    }
  }
  geom.setIndex(indices);
  pos.needsUpdate = true;
  // computeVertexNormals walks the index buffer, so it correctly skips
  // the culled triangles. Vertices with no remaining triangles get a
  // default (0,0,1) normal — fine since they never get rendered.
  geom.computeVertexNormals();

  // Material — start with a neutral colour so the mesh is visible
  // immediately. The JPEG texture replaces this once decoded (async).
  // onBeforeCompile injects: (1) inner-radius discard so the disc
  // cutout stays clear, (2) per-fragment colour grading on the ortho
  // sample (S-curve contrast + saturation + green boost + levels) to
  // compensate for the surround JPEG's washed-out source.
  const material = new THREE.MeshStandardMaterial({
    color: 0x6e7681,
    roughness: 1.0,
    metalness: 0.0,
    flatShading: false,
  });
  // Inner-radius clip — the lidar disc covers the centre with its own
  // composites (point cloud + canopy). Discarding fragments inside the
  // disc footprint stops the surround from peeking through with its
  // coarse 4.7 m / cell elevation while the discs paint at their true
  // (sub-decimeter) altitudes. The disc radius comes from canonical-
  // location (`radius_m`, default 75 m for the SDK pipelines).
  const innerRadius = Number.isFinite(loc.json.radius_m) ? loc.json.radius_m : 75;
  material.onBeforeCompile = (shader) => {
    shader.uniforms.uInnerR2 = { value: innerRadius * innerRadius };
    shader.vertexShader = shader.vertexShader
      .replace('#include <common>',
        `#include <common>
         varying vec2 vWorldXY;`)
      .replace('#include <project_vertex>',
        `#include <project_vertex>
         vec4 _wp = modelMatrix * vec4(transformed, 1.0);
         vWorldXY = _wp.xy;`);
    // Fragment shader injection:
    //   1. inner-radius discard (existing)
    //   2. colour-grade the sampled texture in `<map_fragment>` —
    //      ports the legacy SDK's terrain shader colour grading
    //      (S-curve contrast + saturation +40% + green-vegetation
    //      boost + levels remap [0.08, 0.92] → [0, 1]). Compensates
    //      for the surround JPEG's washed-out, low-contrast look from
    //      the WMS aerial source.
    shader.fragmentShader = shader.fragmentShader
      .replace('#include <common>',
        `#include <common>
         varying vec2 vWorldXY;
         uniform float uInnerR2;`)
      .replace('#include <clipping_planes_fragment>',
        `#include <clipping_planes_fragment>
         if (dot(vWorldXY, vWorldXY) < uInnerR2) discard;`)
      .replace('#include <map_fragment>',
        `#ifdef USE_MAP
           vec4 _sampled = texture2D(map, vMapUv);
           vec3 _col = _sampled.rgb;
           // S-curve contrast: smoothstep(0,1,lum) on perceptual luminance.
           float _lum = dot(_col, vec3(0.299, 0.587, 0.114));
           _lum = _lum * _lum * (3.0 - 2.0 * _lum);
           // Saturation +40%.
           _col = mix(vec3(_lum), _col, 1.4);
           // Green-vegetation boost: where green is dominant, lift G
           // and tame R so foliage reads as actual green instead of olive.
           float _greenDom = _col.g - max(_col.r, _col.b);
           if (_greenDom > 0.02) {
             _col.g *= 1.0 + _greenDom * 1.5;
             _col.r *= 1.0 - _greenDom * 0.3;
           }
           // Levels remap [0.08, 0.92] → [0, 1] for final contrast.
           _col = clamp((_col - 0.08) / 0.84, 0.0, 1.0);
           diffuseColor *= vec4(_col, _sampled.a);
         #endif`);
  };
  const mesh = new THREE.Mesh(geom, material);
  mesh.position.set(cx, cy, 0);
  mesh.name = 'surround';
  mesh.receiveShadow = true;

  ctx.render3D({
    kind: 'mesh',
    object: mesh,
    layer: { id: 'surround', label: 'Surround terrain', defaultVisible: true },
  });

  // ── Pit wall ──────────────────────────────────────────────────────
  // The surround mesh's fragment shader discards everything inside
  // `innerRadius` — that opens a disc-shaped hole. With the camera
  // tilted, the user can see THROUGH the hole to the sky background.
  // Build a black cylinder seated INSIDE the hole whose top rim
  // follows the elevation contour at radius=innerRadius (so the seam
  // with the surround is invisible) and whose bottom is well below
  // any visible geometry. Renders the inside black + writes depth so
  // it occludes the sky cleanly.
  function sampleSurroundZ(wx, wy) {
    // Same orientation contract as the surround mesh: row 0 = north
    // (cy + SURROUND_RADIUS_M world Y), col 0 = west (cx - SURROUND_RADIUS_M).
    // Reads the DILATED tmpZ rather than the raw elev raster — so over
    // sea-offshore samples we get the propagated coast elevation, not
    // null. Pit + skirt rim seals follow the dilated terrain.
    const normX = (wx - cx + SURROUND_RADIUS_M) / sizeX;
    const normY = (cy + SURROUND_RADIUS_M - wy) / sizeY;
    if (normX < 0 || normX > 1 || normY < 0 || normY > 1) return null;
    const col = Math.max(0, Math.min(cols - 1, Math.round(normX * (cols - 1))));
    const row = Math.max(0, Math.min(rows - 1, Math.round(normY * (rows - 1))));
    const idx = row * cols + col;
    return validFlag[idx] ? tmpZ[idx] : null;
  }
  const PIT_SEGMENTS = 96;          // smooth circle, low geom cost
  const pitTopZs = new Float32Array(PIT_SEGMENTS);
  let pitMinTop = Infinity;
  for (let i = 0; i < PIT_SEGMENTS; i++) {
    const θ = (i / PIT_SEGMENTS) * Math.PI * 2;
    const wx = innerRadius * Math.cos(θ);
    const wy = innerRadius * Math.sin(θ);
    const z = sampleSurroundZ(wx, wy);
    pitTopZs[i] = z !== null ? z : 0;
    if (z !== null && z < pitMinTop) pitMinTop = z;
  }
  if (!Number.isFinite(pitMinTop)) pitMinTop = 0;
  // 1000 m below the lowest seal point. Deep enough that a top-down view
  // through the pit doesn't see the bottom in the same shot as the
  // terrain (avoids a visible "floor" line at the rim's elevation).
  const pitBottomZ = pitMinTop - 1000;

  // Vertex layout: rim top/bottom pairs only — no cap. The skirt's
  // own bottom cap is wider and covers the same area below the pit,
  // so a pit floor would just z-fight with it. Side walls suffice.
  const pitPositions = new Float32Array(PIT_SEGMENTS * 2 * 3);
  const pitIndices = [];
  for (let i = 0; i < PIT_SEGMENTS; i++) {
    const θ = (i / PIT_SEGMENTS) * Math.PI * 2;
    const x = innerRadius * Math.cos(θ);
    const y = innerRadius * Math.sin(θ);
    pitPositions[(i * 2 + 0) * 3 + 0] = x;
    pitPositions[(i * 2 + 0) * 3 + 1] = y;
    pitPositions[(i * 2 + 0) * 3 + 2] = pitTopZs[i];
    pitPositions[(i * 2 + 1) * 3 + 0] = x;
    pitPositions[(i * 2 + 1) * 3 + 1] = y;
    pitPositions[(i * 2 + 1) * 3 + 2] = pitBottomZ;
  }
  for (let i = 0; i < PIT_SEGMENTS; i++) {
    const next = (i + 1) % PIT_SEGMENTS;
    const top_i = i * 2;
    const bot_i = i * 2 + 1;
    const top_n = next * 2;
    const bot_n = next * 2 + 1;
    // Side quad — DoubleSide material so winding doesn't matter.
    pitIndices.push(top_i, bot_i, top_n);
    pitIndices.push(bot_i, bot_n, top_n);
  }
  const pitGeom = new THREE.BufferGeometry();
  pitGeom.setAttribute('position', new THREE.BufferAttribute(pitPositions, 3));
  pitGeom.setIndex(pitIndices);
  pitGeom.computeBoundingSphere();
  const pitMaterial = new THREE.MeshBasicMaterial({
    color: 0x000000,
    side: THREE.DoubleSide,
    fog: false,                     // distance fog would tint it grey
  });
  const pitMesh = new THREE.Mesh(pitGeom, pitMaterial);
  pitMesh.position.set(0, 0, 0);    // disc rim is in scene-origin coords already
  pitMesh.name = 'pit-wall';

  ctx.render3D({
    kind: 'mesh',
    object: pitMesh,
    layer: { id: 'pit-wall', label: 'Tile pit wall', defaultVisible: true },
  });

  // ── Surround underside ────────────────────────────────────────────
  // The main surround mesh defaults to FrontSide rendering — looking at
  // it from below (orbit underground, or looking up at a slope's
  // underside) shows nothing, the camera sees through to the sky. Add
  // a sibling mesh that shares the same geometry but renders only
  // back-facing triangles with a flat black, no-lighting material. The
  // inner-radius discard is preserved so it doesn't fill the disc
  // cutout when viewed from below.
  const undersideMaterial = new THREE.MeshBasicMaterial({
    color: 0x000000,
    side: THREE.BackSide,
    fog: false,
  });
  undersideMaterial.onBeforeCompile = (shader) => {
    shader.uniforms.uInnerR2 = { value: innerRadius * innerRadius };
    shader.vertexShader = shader.vertexShader
      .replace('#include <common>',
        `#include <common>
         varying vec2 vWorldXY;`)
      .replace('#include <project_vertex>',
        `#include <project_vertex>
         vec4 _wp = modelMatrix * vec4(transformed, 1.0);
         vWorldXY = _wp.xy;`);
    shader.fragmentShader = shader.fragmentShader
      .replace('#include <common>',
        `#include <common>
         varying vec2 vWorldXY;
         uniform float uInnerR2;`)
      .replace('#include <clipping_planes_fragment>',
        `#include <clipping_planes_fragment>
         if (dot(vWorldXY, vWorldXY) < uInnerR2) discard;`);
  };
  const undersideMesh = new THREE.Mesh(geom, undersideMaterial);
  undersideMesh.position.set(cx, cy, 0);
  undersideMesh.name = 'surround-underside';
  ctx.render3D({
    kind: 'mesh',
    object: undersideMesh,
    layer: { id: 'surround-underside', label: 'Surround underside', defaultVisible: true },
  });

  // ── Outer skirt ───────────────────────────────────────────────────
  // Black wall hanging from the surround's outer perimeter (the 1200 m
  // square edge) down to a Z well below any geometry. Without it, the
  // camera can see through the side of the surround tile to the sky
  // background — visible whenever the orbit dips below the terrain
  // level near the tile edge, or the user pans far enough that the
  // surround edge enters the view frustum.
  const SKIRT_SAMPLES_PER_EDGE = 60;
  const xMin = cx - SURROUND_RADIUS_M;
  const xMax = cx + SURROUND_RADIUS_M;
  const yMin = cy - SURROUND_RADIUS_M;
  const yMax = cy + SURROUND_RADIUS_M;
  const perimPts = [];
  function pushPerim(p1, p2, n) {
    for (let i = 0; i < n; i++) {
      const t = i / n;
      const px = p1.x + (p2.x - p1.x) * t;
      const py = p1.y + (p2.y - p1.y) * t;
      const pz = sampleSurroundZ(px, py);
      perimPts.push({ x: px, y: py, z: pz !== null ? pz : 0 });
    }
  }
  // Counterclockwise from above (so default winding has the OUTSIDE face
  // pointing outward — matters less since we use DoubleSide, but keeps
  // backface-culled debug renders sane).
  pushPerim({ x: xMin, y: yMin }, { x: xMax, y: yMin }, SKIRT_SAMPLES_PER_EDGE);
  pushPerim({ x: xMax, y: yMin }, { x: xMax, y: yMax }, SKIRT_SAMPLES_PER_EDGE);
  pushPerim({ x: xMax, y: yMax }, { x: xMin, y: yMax }, SKIRT_SAMPLES_PER_EDGE);
  pushPerim({ x: xMin, y: yMax }, { x: xMin, y: yMin }, SKIRT_SAMPLES_PER_EDGE);

  let skirtMinTop = Infinity;
  for (const p of perimPts) if (p.z < skirtMinTop) skirtMinTop = p.z;
  if (!Number.isFinite(skirtMinTop)) skirtMinTop = 0;
  const skirtBottomZ = skirtMinTop - 1000;   // matches pit depth

  const N = perimPts.length;
  // Vertex layout: N×2 (rim top/bottom pairs) + 4 corner vertices for
  // the rectangular bottom cap. The cap closes the box so a vertical-
  // down view doesn't see sky through the open bottom.
  const skirtVertCount = N * 2 + 4;
  const cornerSWIdx = N * 2 + 0;             // (xMin, yMin, bottom)
  const cornerSEIdx = N * 2 + 1;             // (xMax, yMin, bottom)
  const cornerNEIdx = N * 2 + 2;             // (xMax, yMax, bottom)
  const cornerNWIdx = N * 2 + 3;             // (xMin, yMax, bottom)
  const skirtPositions = new Float32Array(skirtVertCount * 3);
  const skirtIndices = [];
  for (let i = 0; i < N; i++) {
    skirtPositions[(i * 2 + 0) * 3 + 0] = perimPts[i].x;
    skirtPositions[(i * 2 + 0) * 3 + 1] = perimPts[i].y;
    skirtPositions[(i * 2 + 0) * 3 + 2] = perimPts[i].z;
    skirtPositions[(i * 2 + 1) * 3 + 0] = perimPts[i].x;
    skirtPositions[(i * 2 + 1) * 3 + 1] = perimPts[i].y;
    skirtPositions[(i * 2 + 1) * 3 + 2] = skirtBottomZ;
  }
  // Bottom-cap corner vertices.
  skirtPositions[cornerSWIdx * 3 + 0] = xMin; skirtPositions[cornerSWIdx * 3 + 1] = yMin; skirtPositions[cornerSWIdx * 3 + 2] = skirtBottomZ;
  skirtPositions[cornerSEIdx * 3 + 0] = xMax; skirtPositions[cornerSEIdx * 3 + 1] = yMin; skirtPositions[cornerSEIdx * 3 + 2] = skirtBottomZ;
  skirtPositions[cornerNEIdx * 3 + 0] = xMax; skirtPositions[cornerNEIdx * 3 + 1] = yMax; skirtPositions[cornerNEIdx * 3 + 2] = skirtBottomZ;
  skirtPositions[cornerNWIdx * 3 + 0] = xMin; skirtPositions[cornerNWIdx * 3 + 1] = yMax; skirtPositions[cornerNWIdx * 3 + 2] = skirtBottomZ;
  // Side walls.
  for (let i = 0; i < N; i++) {
    const next = (i + 1) % N;
    const top_i = i * 2;
    const bot_i = i * 2 + 1;
    const top_n = next * 2;
    const bot_n = next * 2 + 1;
    skirtIndices.push(top_i, bot_i, top_n);
    skirtIndices.push(bot_i, bot_n, top_n);
  }
  // Bottom-cap rectangle, two triangles. DoubleSide on material so any
  // winding works — picked SW→SE→NE→NW for normal pointing +Z.
  skirtIndices.push(cornerSWIdx, cornerSEIdx, cornerNEIdx);
  skirtIndices.push(cornerSWIdx, cornerNEIdx, cornerNWIdx);
  const skirtGeom = new THREE.BufferGeometry();
  skirtGeom.setAttribute('position', new THREE.BufferAttribute(skirtPositions, 3));
  skirtGeom.setIndex(skirtIndices);
  skirtGeom.computeBoundingSphere();
  const skirtMesh = new THREE.Mesh(skirtGeom, new THREE.MeshBasicMaterial({
    color: 0x000000,
    side: THREE.DoubleSide,
    fog: false,
  }));
  skirtMesh.position.set(0, 0, 0);   // perimPts already in world (scene) coords
  skirtMesh.name = 'surround-skirt';
  ctx.render3D({
    kind: 'mesh',
    object: skirtMesh,
    layer: { id: 'surround-skirt', label: 'Surround skirt', defaultVisible: true },
  });

  // PlaneGeometry's default UV is (0,0) at the corner that ends up with
  // x = -sizeX/2, y = +sizeY/2 — i.e. the top-left of the plane (north-
  // west in our Z-up scene). The JPEG is delivered with row 0 at north
  // (top of the image). UV.v=0 at the top + image row 0 at top → no flip
  // needed beyond Three.js' CanvasTexture default.
  let bitmap = null;
  let texture = null;
  if (jpeg?.bytes) {
    const blob = new Blob([jpeg.bytes], { type: 'image/jpeg' });
    // imageOrientation:'flipY' so the resulting bitmap matches THREE's
    // default CanvasTexture flipY=true convention — top of plane = UV
    // v=1 = top of image = north of geo. Without this, browsers vary
    // on whether the bitmap is flipped, and the texture ends up
    // mirrored vs the heightmap.
    createImageBitmap(blob, { imageOrientation: 'flipY' }).then((bm) => {
      bitmap = bm;
      texture = new THREE.CanvasTexture(bm);
      texture.colorSpace = THREE.SRGBColorSpace;
      texture.minFilter = THREE.LinearFilter;
      texture.magFilter = THREE.LinearFilter;
      texture.needsUpdate = true;
      material.color.setHex(0xffffff);
      material.map = texture;
      material.needsUpdate = true;
    }).catch((err) => {
      console.warn('[surround] JPEG decode failed:', err);
    });
  }

  ctx.onCleanup(() => {
    geom.dispose();
    material.dispose();
    texture?.dispose?.();
    bitmap?.close?.();
  });
}
