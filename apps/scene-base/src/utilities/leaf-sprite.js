// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Procedural leaf mosaic texture for canopy hex discs.
//
// Generates a 128×128 RGBA OffscreenCanvas with ~40 small oval leaves
// scattered across the hex face. Gaps between leaves are transparent
// (alpha = 0). Used as `map` + `alphaTest` on the canopy
// MeshStandardMaterial and its customDepthMaterial so both the visible
// surface and the shadow silhouette show individual leaf shapes.
//
// Ported verbatim from legacy SDK textures/leaf-sprite.js. Consumed by
// canopy.viz.js (S5).

import * as THREE from 'three';

/**
 * Generate a procedural leaf mosaic texture.
 *
 * @param {number} [size=128]      Texture side in pixels
 * @param {number} [leafCount=40]  Number of leaves to scatter
 * @returns {THREE.CanvasTexture}
 */
export function generateLeafTexture(size = 128, leafCount = 40) {
  const canvas = new OffscreenCanvas(size, size);
  const ctx = canvas.getContext('2d');

  ctx.clearRect(0, 0, size, size);

  const cx = size / 2;
  const cy = size / 2;
  // Hex inscribed radius — keep leaves inside this circle so they don't
  // bleed outside the CircleGeometry(1, 6) UV range.
  const hexR = size * 0.45;

  // Deterministic LCG so the texture is reproducible across runs.
  let seed = 12345;
  function rand() {
    seed = (seed * 16807 + 0) % 2147483647;
    return (seed - 1) / 2147483646;
  }

  for (let i = 0; i < leafCount; i++) {
    const angle = rand() * Math.PI * 2;
    const dist = Math.sqrt(rand()) * hexR * 0.85;
    const lx = cx + Math.cos(angle) * dist;
    const ly = cy + Math.sin(angle) * dist;

    // Leaf size: 2-5 px wide on a 128 px canvas. Each leaf covers ~2-4%
    // of the hex face → ~40-50% total fill → plenty of transparent gaps
    // for the silhouette effect.
    const leafW = 2 + rand() * 3;
    const leafH = leafW * (2.0 + rand() * 1.5); // elongated oval
    const rot = rand() * Math.PI * 2;

    // Hue 90-140° (yellow-green to green), varied saturation/lightness.
    const hue = 90 + rand() * 50;
    const sat = 40 + rand() * 30;
    const lit = 25 + rand() * 30;
    const fill = `hsl(${hue}, ${sat}%, ${lit}%)`;

    ctx.save();
    ctx.translate(lx, ly);
    ctx.rotate(rot);

    ctx.beginPath();
    ctx.ellipse(0, 0, leafW, leafH, 0, 0, Math.PI * 2);
    ctx.fillStyle = fill;
    ctx.fill();

    // Subtle vein.
    ctx.beginPath();
    ctx.moveTo(0, -leafH * 0.8);
    ctx.lineTo(0, leafH * 0.8);
    ctx.strokeStyle = `hsl(${hue}, ${sat - 10}%, ${lit + 15}%)`;
    ctx.lineWidth = 0.5;
    ctx.stroke();

    ctx.restore();
  }

  const texture = new THREE.CanvasTexture(canvas);
  texture.flipY = false;
  texture.magFilter = THREE.LinearFilter;
  texture.minFilter = THREE.LinearMipmapLinearFilter;
  texture.needsUpdate = true;
  return texture;
}
