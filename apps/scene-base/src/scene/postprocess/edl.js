// SPDX-License-Identifier: Apache-2.0 AND BSD-2-Clause
// Copyright 2026 Antoine Durand
// Eye-Dome Lighting (EDL) post-process.
//
// EDL enhances depth perception of point clouds (and any geometry) by
// darkening pixels at depth discontinuities — i.e. silhouette edges of
// foreground objects against more distant background. The result reads as
// a faint dark "halo" around every closer surface, which dramatically
// improves the sense of solidity in sparse / disc-based point clouds.
//
// References:
//   - Boucheny et al., "Eye-Dome Lighting" (Kitware, 2009)
//   - Potree's reference shader (BSD 2-Clause, see LICENSES/BSD-2-Clause-potree.txt):
//     https://github.com/potree/potree/blob/develop/src/materials/shaders/edl.fs
//
// Implementation notes:
//   - We sample 8 neighbours arranged on a circle of radius `uEdlRadius` px.
//   - Depth is read from a DepthTexture attached to the EffectComposer's
//     render target. The caller is responsible for setting that up — see
//     scene.js for the wiring.
//   - We use the perspective camera near/far planes to linearise depth.
//     Without this the response is dominated by the depth-buffer non-linearity
//     and large objects in the foreground produce no shading at all.
//
// Plan reference: 3D-VIEWER.md §F7 / §D7.

import * as THREE from 'three';
import { ShaderPass } from 'three/addons/postprocessing/ShaderPass.js';

const EDL_SHADER = {
  uniforms: {
    tDiffuse: { value: null },
    tDepth: { value: null },
    tOptInMask: { value: null },  // RGBA8 — .r = 1.0 where a setEdlOptIn() mesh is visible
    uResolution: { value: new THREE.Vector2(1, 1) },
    uNear: { value: 1 },
    uFar: { value: 1000 },
    uEdlStrength: { value: 1.0 },
    uEdlRadius: { value: 1.4 },
    uEdlFadeNear: { value: 0 },  // linear depth where EDL starts fading (0 = disabled)
    uEdlFadeFar: { value: 0 },   // linear depth where EDL reaches zero
    uEnabled: { value: 1 },
    // Debug: when 1, ignore tOptInMask and apply EDL to every pixel.
    // Used to localise mask-wiring issues vs EDL-shader issues.
    // Toggle from JS: viewer._edlPass.uniforms.uForceMask.value = 1.
    uForceMask: { value: 0 },
  },
  vertexShader: /* glsl */ `
    varying vec2 vUv;
    void main() {
      vUv = uv;
      gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
    }
  `,
  fragmentShader: /* glsl */ `
    uniform sampler2D tDiffuse;
    uniform sampler2D tDepth;
    uniform sampler2D tOptInMask;
    uniform vec2 uResolution;
    uniform float uNear;
    uniform float uFar;
    uniform float uEdlStrength;
    uniform float uEdlRadius;
    uniform float uEdlFadeNear;
    uniform float uEdlFadeFar;
    uniform int uEnabled;
    uniform int uForceMask;

    varying vec2 vUv;

    // Convert non-linear depth-buffer value [0..1] to linear eye-space depth.
    float linearizeDepth(float d) {
      float z = d * 2.0 - 1.0; // NDC
      return (2.0 * uNear * uFar) / (uFar + uNear - z * (uFar - uNear));
    }

    // EDL response: how much darker this pixel should be due to nearby
    // farther neighbours. Sums log-depth differences across 8 directions.
    float responseEDL(vec2 uv) {
      float depth = linearizeDepth(texture2D(tDepth, uv).x);
      // Background pixels (sky / nothing rendered) → no shading.
      if (depth >= uFar - 1.0) return 0.0;

      vec2 px = uEdlRadius / uResolution;
      float sum = 0.0;

      for (int i = 0; i < 8; i++) {
        float angle = float(i) * 0.7853981633; // π / 4
        vec2 offset = vec2(cos(angle), sin(angle)) * px;
        float nDepth = linearizeDepth(texture2D(tDepth, uv + offset).x);
        if (nDepth < uFar - 1.0) {
          // Closer pixels (this) → larger log diff → more darkening.
          // log() over linear eye-space depth gives a perceptually uniform
          // response across orders of magnitude in scene depth.
          sum += max(0.0, log(nDepth) - log(depth));
        }
      }

      return sum / 8.0;
    }

    void main() {
      vec4 col = texture2D(tDiffuse, vUv);
      if (uEnabled == 0) {
        gl_FragColor = col;
        return;
      }
      // Per-pixel EDL gating via the opt-in mask. MaskRenderPass
      // rendered the EDL_LAYER subset of the scene with a flat-red
      // overrideMaterial, so tOptInMask.r is ~1.0 where a setEdlOptIn()
      // mesh is visible and 0.0 elsewhere. Non-opt-in pixels return
      // the raw scene colour unchanged. uForceMask=1 bypasses the gate
      // (debug — see uniform decl).
      if (uForceMask == 0) {
        float mask = texture2D(tOptInMask, vUv).r;
        if (mask < 0.5) {
          gl_FragColor = col;
          return;
        }
      }
      float r = responseEDL(vUv);
      // Fade EDL to zero for distant pixels (surround terrain) so the
      // low-res environment doesn't get dark halos at depth edges.
      if (uEdlFadeFar > uEdlFadeNear && uEdlFadeNear > 0.0) {
        float depth = linearizeDepth(texture2D(tDepth, vUv).x);
        float fade = 1.0 - smoothstep(uEdlFadeNear, uEdlFadeFar, depth);
        r *= fade;
      }
      // Map response → multiplicative shade in [0..1].
      float shade = exp(-r * 80.0 * uEdlStrength);
      gl_FragColor = vec4(col.rgb * shade, col.a);
    }
  `,
};

/**
 * Build an EDL post-processing pass.
 *
 * @returns {ShaderPass}
 */
export function createEDLPass() {
  const pass = new ShaderPass(EDL_SHADER);
  pass.renderToScreen = false; // composer decides; flag set on the last pass
  return pass;
}
