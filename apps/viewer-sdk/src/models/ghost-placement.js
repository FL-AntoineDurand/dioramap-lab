// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Ghost placement mode.
//
// Loads a GLB, applies the same Y-up→Z-up rotation + bottom-on-ground
// centring as the real model, makes it semi-transparent, and follows
// the cursor over the scene. Click to confirm (calls modelManager.addModel
// with the ghost's final position); Escape to cancel.
//
// Z-up adaptation from legacy v2: rotation.x = +PI/2 (was -PI/2 in
// legacy Y-up — same sign for the same intent), the fallback ground
// plane normal is +Z (was +Y), and the picked world position uses .z
// as the height component (was .y).

import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import { loadGltf } from './gltf-source.js';

const TARGET_WIDTH_M = 4;

const loader = new GLTFLoader();

/**
 * @param {Object} opts
 * @param {string} [opts.url]    URL to fetch via GLTFLoader
 * @param {ArrayBuffer | Blob | ArrayBufferView} [opts.data]
 *        Raw GLB bytes; parsed via GLTFLoader.parse, no network call.
 *        Mutually exclusive with `url` — exactly one must be provided.
 *        On click-confirm the same source is forwarded to
 *        modelManager.addModel so the placed model reuses the same
 *        bytes (parsed twice, no re-fetch).
 * @param {THREE.Scene} opts.scene
 * @param {THREE.Camera} opts.camera
 * @param {THREE.WebGLRenderer} opts.renderer
 * @param {(sceneX:number, sceneY:number) => number|null} opts.sampleGroundZ
 *        — top-down ground sampler (from model-manager).
 * @param {ReturnType<import('./model-manager.js').createModelManager>} opts.modelManager
 * @param {Object} [opts.modelOpts]
 * @returns {{cancel: () => void, promise: Promise<string|null>}}
 */
export function startGhostPlacement({
  url, data, scene, camera, renderer, sampleGroundZ,
  modelManager, modelOpts = {},
}) {
  let cancel = () => {};
  const promise = new Promise((resolve) => {
    const raycaster = new THREE.Raycaster();
    const mouse = new THREE.Vector2();
    // Z-up fallback ground plane at z=0 (sky/no-disc fallback).
    const groundPlane = new THREE.Plane(new THREE.Vector3(0, 0, 1), 0);
    const hitPoint = new THREE.Vector3();

    let ghost = null;
    let disposed = false;

    loadGltf(loader, { url, data }).then((gltf) => {
      if (disposed) return;

      const gltfScene = gltf.scene;
      gltfScene.rotation.x = Math.PI / 2;
      gltfScene.updateMatrixWorld(true);

      const bbox = new THREE.Box3().setFromObject(gltfScene);
      const size = new THREE.Vector3();
      bbox.getSize(size);
      const center = new THREE.Vector3();
      bbox.getCenter(center);

      let scale = modelOpts.scale || 1;
      if (size.x > 0.01 && size.x < 100) {
        scale *= TARGET_WIDTH_M / size.x;
      }
      gltfScene.scale.setScalar(scale);
      gltfScene.position.set(
        -center.x * scale,
        -center.y * scale,
        -bbox.min.z * scale,
      );

      // Semi-transparent ghost — clone every material so we don't pollute
      // the cached GLTF for subsequent real loads.
      gltfScene.traverse((child) => {
        if (child.isMesh) {
          child.material = child.material.clone();
          child.material.transparent = true;
          child.material.opacity = 0.5;
          child.material.depthWrite = false;
          child.castShadow = false;
          child.receiveShadow = false;
        }
      });

      ghost = new THREE.Group();
      ghost.add(gltfScene);
      ghost.name = 'ghost-placement';
      scene.add(ghost);
    }).catch((err) => {
      console.error('[ghost-placement] load failed:', err);
      cleanup();
      resolve(null);
    });

    function onPointerMove(event) {
      if (!ghost) return;
      const rect = renderer.domElement.getBoundingClientRect();
      mouse.x = ((event.clientX - rect.left) / rect.width) * 2 - 1;
      mouse.y = -((event.clientY - rect.top) / rect.height) * 2 + 1;

      raycaster.setFromCamera(mouse, camera);

      // Prefer top-down ground Z at the cursor's XY (consistent with
      // the final placed-model snap). Use a small ray-against-floor as
      // a fallback when the cursor is over empty sky.
      // First, project the camera ray onto the world to find (x, y).
      // For the XY we use the actual ray hit on the disc cloud / surround
      // (more accurate on slopes than a flat-plane projection); for Z we
      // re-sample via sampleGroundZ at that XY to match the gizmo's
      // ground-follow behaviour exactly.
      const candidates = scene.children.filter((c) =>
        !c.userData?.modelUUID &&
        !c.isTransformControlsRoot &&
        c.name !== 'ghost-placement' &&
        c.name !== '__axes' &&
        c.name !== '__grid' &&
        c.name !== 'model-selection-bbox'
      );
      const hits = raycaster.intersectObjects(candidates, true);
      if (hits.length > 0) {
        const pt = hits[0].point;
        ghost.position.x = pt.x;
        ghost.position.y = pt.y;
        const gz = sampleGroundZ(pt.x, pt.y);
        ghost.position.z = gz != null ? gz : pt.z;
      } else {
        // Fallback to flat plane through current Z.
        groundPlane.constant = -(ghost.position.z || 0);
        if (raycaster.ray.intersectPlane(groundPlane, hitPoint)) {
          ghost.position.x = hitPoint.x;
          ghost.position.y = hitPoint.y;
        }
      }
    }

    function onPointerDown(event) {
      if (!ghost || event.button !== 0) return;
      const pos = {
        x: ghost.position.x, y: ghost.position.y, z: ghost.position.z,
      };
      cleanup();
      // Forward whichever source the caller provided. Parsing the same
      // bytes twice (once for ghost, once for the placed model) is far
      // cheaper than re-fetching, and cloning a parsed glTF scene is
      // tricky (materials, textures, animations).
      const source = data ? { data } : { url };
      modelManager.addModel({ ...source, ...modelOpts, position: pos, groundSnap: true })
        .then((uuid) => resolve(uuid));
    }

    function onKeyDown(event) {
      if (event.key === 'Escape') { cleanup(); resolve(null); }
    }

    function cleanup() {
      if (disposed) return;
      disposed = true;
      renderer.domElement.removeEventListener('pointermove', onPointerMove);
      renderer.domElement.removeEventListener('pointerdown', onPointerDown);
      window.removeEventListener('keydown', onKeyDown);
      if (ghost) {
        scene.remove(ghost);
        ghost.traverse((child) => {
          if (child.geometry) child.geometry.dispose();
          if (child.material) child.material.dispose();
        });
        ghost = null;
      }
    }

    cancel = () => { cleanup(); resolve(null); };

    renderer.domElement.addEventListener('pointermove', onPointerMove);
    renderer.domElement.addEventListener('pointerdown', onPointerDown);
    window.addEventListener('keydown', onKeyDown);
  });

  return { cancel, promise };
}
