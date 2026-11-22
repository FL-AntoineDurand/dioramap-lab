// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// 3D model instance manager.
//
// Maintains a Map<uuid, ModelInstance> keyed by stable UUID. Each model
// is a GLB loaded via GLTFLoader, rotated from Y-up to v3's Z-up
// convention, normalised to ~4 m wide, ground-snapped, and added to the
// scene as a wrapped Group. Public surface (addModel/removeModel/...)
// is exposed by the SDK in sdk.js.
//
// Coordinate convention: v3 is Z-up Lambert93 (x=east, y=north, z=up).
// GLBs are typically authored Y-up (height = +Y, forward = +Z). We
// rotate gltfScene by +90° around X so the model's height axis (+Y)
// lands on world +Z and its forward axis (+Z) lands on world -Y (south).
// rotation.x = -PI/2 would flip the model upside-down (Y → -Z).

import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import { loadGltf } from './gltf-source.js';

let _nextId = 0;
function uuid() { return `model-${++_nextId}-${Date.now().toString(36)}`; }

const TARGET_WIDTH_M = 4;        // auto-scale target — a garden shed
const RAYCAST_FROM_Z = 1000;     // height we cast the ground-sample ray from
const RAYCAST_TO_Z = -100;       // depth we cast it to (well below any surface)

/**
 * @param {Object} opts
 * @param {THREE.Scene}  opts.scene
 */
export function createModelManager({ scene }) {
  /** @type {Map<string, ModelInstance>} */
  const models = new Map();
  const loader = new GLTFLoader();

  // Ground-sample ray reused across queries.
  const _groundRay = new THREE.Raycaster();
  const _groundDir = new THREE.Vector3(0, 0, -1);

  /**
   * Top-down raycast at (x, y) against scene meshes. Returns the world Z
   * of the highest hit, or null if nothing was hit (sky / outside disc
   * coverage). Excludes models themselves and dev helpers / gizmo
   * decorations so we don't return the gizmo's own drag-plane Z (which
   * is huge and invisible) or our selection bbox helper.
   *
   * Walks up the parent chain so a hit on a deep mesh inside e.g. a
   * placed model is correctly attributed to the model wrap (which has
   * userData.modelUUID).
   */
  function sampleGroundZ(x, y) {
    _groundRay.set(new THREE.Vector3(x, y, RAYCAST_FROM_Z), _groundDir);
    _groundRay.far = RAYCAST_FROM_Z - RAYCAST_TO_Z;

    const hits = _groundRay.intersectObjects(scene.children, true);
    for (const h of hits) {
      let cur = h.object;
      let skip = false;
      while (cur) {
        if (cur.userData?.modelUUID) { skip = true; break; }
        if (cur.isTransformControlsRoot) { skip = true; break; }
        if (cur.isTransformControlsGizmo) { skip = true; break; }
        if (cur.isTransformControlsPlane) { skip = true; break; }
        if (cur.name === '__axes' || cur.name === '__grid') { skip = true; break; }
        if (cur.name === 'ghost-placement') { skip = true; break; }
        if (cur.name === 'model-selection-bbox') { skip = true; break; }
        cur = cur.parent;
      }
      if (!skip) return h.point.z;
    }
    return null;
  }

  /**
   * @param {Object} opts
   * @param {string} [opts.url]                              URL to fetch via GLTFLoader
   * @param {ArrayBuffer | Blob | ArrayBufferView} [opts.data]
   *        Raw GLB bytes; parsed via GLTFLoader.parse, no network call.
   *        Mutually exclusive with `url` — exactly one must be provided.
   *        Use `data` for procedural / pre-fetched / configurator-built
   *        models, or to keep the embedder's CSP `connect-src` tight
   *        (the SDK never fetches the bytes).
   * @param {string} [opts.label='']
   * @param {{x:number,y:number,z:number}} [opts.position]   scene-local
   * @param {number} [opts.rotationZ=0]                       degrees, heading
   * @param {number} [opts.scale=1]
   * @param {boolean} [opts.groundSnap=true]                  snap z on placement
   * @param {boolean} [opts.groundFollow=true]                follow ground when dragged
   * @param {string[]} [opts.allowedRotationAxes=['z']]       which gizmo axes ('x','y','z')
   * @returns {Promise<string>} uuid of placed model
   */
  async function addModel({
    url, data, label = '',
    position = { x: 0, y: 0, z: 0 },
    rotationZ = 0,
    scale = 1,
    groundSnap = true,
    groundFollow = true,
    allowedRotationAxes = ['z'],
  }) {
    const gltf = await loadGltf(loader, { url, data });

    const gltfScene = gltf.scene;

    // Reorient Y-up GLB → Z-up. Rotate around world X by +90° so the
    // model's original +Y axis (height) lands on world +Z. The wrong
    // sign here flips the model upside-down and makes -bbox.min.z * s
    // a large positive ground offset (model floats high above ground).
    gltfScene.rotation.x = Math.PI / 2;
    gltfScene.updateMatrixWorld(true);

    // Compute bbox in the new (Z-up) orientation.
    const bbox = new THREE.Box3().setFromObject(gltfScene);
    const size = new THREE.Vector3();
    bbox.getSize(size);
    const bboxCenter = new THREE.Vector3();
    bbox.getCenter(bboxCenter);

    // Auto-scale based on horizontal extent (X). Models authored at very
    // different scales (cm-units, m-units) all land at ~4 m wide. Skip
    // if size.x is degenerate.
    let effectiveScale = scale;
    if (size.x > 0.01 && size.x < 100) {
      effectiveScale = scale * (TARGET_WIDTH_M / size.x);
    }
    gltfScene.scale.setScalar(effectiveScale);

    // Centre horizontally + sit on ground (bbox.min.z → wrap-local 0).
    // Offsets are pre-multiplied by effectiveScale because position and
    // scale combine as (position + scale * p); using unscaled offsets
    // would leave a residual of bbox.min.z * (scale - 1).
    gltfScene.position.set(
      -bboxCenter.x * effectiveScale,
      -bboxCenter.y * effectiveScale,
      -bbox.min.z * effectiveScale,
    );

    // Enable shadows on every mesh in the GLB.
    gltfScene.traverse((child) => {
      if (child.isMesh) {
        child.castShadow = true;
        child.receiveShadow = true;
      }
    });

    // Wrap in a Group whose position/rotation drive placement.
    const wrap = new THREE.Group();
    wrap.add(gltfScene);
    wrap.position.set(position.x, position.y, position.z);
    wrap.rotation.z = rotationZ * Math.PI / 180;

    if (groundSnap) {
      const gz = sampleGroundZ(wrap.position.x, wrap.position.y);
      if (gz != null) wrap.position.z = gz;
    }

    const id = uuid();
    wrap.name = `model-${id}`;
    wrap.userData.modelUUID = id;
    scene.add(wrap);

    const instance = {
      uuid: id, url, label,
      object: wrap,
      bbox: new THREE.Box3().setFromObject(wrap),
      position: { x: wrap.position.x, y: wrap.position.y, z: wrap.position.z },
      rotation: { x: 0, y: 0, z: rotationZ },
      scale: effectiveScale,
      config: { groundFollow, allowedRotationAxes },
    };
    models.set(id, instance);
    return id;
  }

  function removeModel(id) {
    const inst = models.get(id);
    if (!inst) return false;
    scene.remove(inst.object);
    inst.object.traverse((child) => {
      if (child.geometry) child.geometry.dispose();
      if (child.material) {
        if (Array.isArray(child.material)) child.material.forEach((m) => m.dispose());
        else child.material.dispose();
      }
    });
    models.delete(id);
    return true;
  }

  function getModels() {
    return [...models.values()].map((m) => ({
      uuid: m.uuid, url: m.url, label: m.label,
      position: { ...m.position },
      rotation: { ...m.rotation },
      scale: m.scale,
    }));
  }

  function getModel(id) {
    const m = models.get(id);
    if (!m) return null;
    return {
      uuid: m.uuid, url: m.url, label: m.label,
      position: { ...m.position },
      rotation: { ...m.rotation },
      scale: m.scale,
    };
  }

  function getObject(id) { return models.get(id)?.object || null; }
  function getConfig(id) { return models.get(id)?.config || null; }

  function setTransform(id, { position, rotationZ, scale: s }) {
    const inst = models.get(id);
    if (!inst) return;
    if (position) {
      inst.object.position.set(position.x, position.y, position.z);
      inst.position = { ...position };
    }
    if (rotationZ != null) {
      inst.object.rotation.z = rotationZ * Math.PI / 180;
      inst.rotation.z = rotationZ;
    }
    if (s != null) {
      inst.object.children[0]?.scale.setScalar(s);
      inst.scale = s;
    }
    inst.bbox = new THREE.Box3().setFromObject(inst.object);
  }

  /**
   * Sync the instance's stored transform from the live wrap Group (after
   * a gizmo drag). When ground-follow is on AND the drag had a
   * horizontal component, snap z back to the ground; pure-Z drags leave
   * the user's manual height tuning alone.
   */
  function syncFromObject(id, { groundSnap = true } = {}) {
    const inst = models.get(id);
    if (!inst) return;
    if (groundSnap && inst.config.groundFollow) {
      const gz = sampleGroundZ(inst.object.position.x, inst.object.position.y);
      if (gz != null) inst.object.position.z = gz;
    }
    inst.position = {
      x: inst.object.position.x,
      y: inst.object.position.y,
      z: inst.object.position.z,
    };
    inst.rotation.z = inst.object.rotation.z * 180 / Math.PI;
    inst.bbox = new THREE.Box3().setFromObject(inst.object);
  }

  return {
    addModel, removeModel, getModels, getModel, getObject, getConfig,
    setTransform, syncFromObject,
    sampleGroundZ,
    get count() { return models.size; },
    [Symbol.iterator]() { return models.values(); },
  };
}
