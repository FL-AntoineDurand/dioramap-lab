// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// GLTF / GLB loading helper.
//
// Used by the SDK's models system (V6) to drop arbitrary 3D models into
// the scene. Lab-ui can also use it for inspector previews. Asset URLs
// are caller-provided; CC0 / CC-BY models for the demo come from
// poly.pizza (mostly Quaternius).
//
// API: `loadGLBModel(url)` returns a Promise that resolves to a
// `{ scene, dimensions, bbox }` triple. The caller is responsible for
// positioning, scaling, and adding to the parent scene.
//
// Ported verbatim from legacy SDK objects/gltf-loader.js.

import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';

const _loader = new GLTFLoader();

/**
 * @param {string} url
 * @returns {Promise<{
 *   scene: THREE.Group,
 *   dimensions: { x: number, y: number, z: number },
 *   bbox: THREE.Box3,
 * }>}
 */
export function loadGLBModel(url) {
  return new Promise((resolve, reject) => {
    _loader.load(
      url,
      (gltf) => {
        const root = gltf.scene;
        // Recursively enable shadows on every mesh — we want models to
        // both cast shadows on the lidar discs and receive shadows from
        // themselves and nearby tall structures.
        root.traverse((obj) => {
          if (obj.isMesh) {
            obj.castShadow = true;
            obj.receiveShadow = true;
          }
        });

        const bbox = new THREE.Box3().setFromObject(root);
        const dimensions = {
          x: bbox.max.x - bbox.min.x,
          y: bbox.max.y - bbox.min.y,
          z: bbox.max.z - bbox.min.z,
        };
        resolve({ scene: root, dimensions, bbox });
      },
      undefined,
      (err) => reject(err),
    );
  });
}
