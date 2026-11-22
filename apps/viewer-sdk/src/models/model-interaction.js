// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Model selection + TransformControls gizmo.
//
// Click on a model → highlight with a wireframe bbox + show the gizmo
// + show floating action buttons (move / rotate / delete). Click on
// empty space → deselect. Gizmo drag fires onMoved.
//
// Z-up adaptation from the legacy v2 implementation: gizmo's allowed
// rotation axes default to ['z'] (heading), action-button positioning
// uses (cx, cy, max.z + 0.3) instead of (cx, max.y + 0.3, cz), and
// ground-follow during a horizontal drag is detected via gizmo.axis ∉
// {'Z'} rather than ∉ {'Y'}.

import * as THREE from 'three';
import { TransformControls } from 'three/addons/controls/TransformControls.js';
import { CSS2DRenderer, CSS2DObject } from 'three/addons/renderers/CSS2DRenderer.js';

/**
 * @param {Object} opts
 * @param {THREE.Scene} opts.scene
 * @param {THREE.Camera} opts.camera
 * @param {THREE.WebGLRenderer} opts.renderer
 * @param {import('three/addons/controls/OrbitControls.js').OrbitControls} opts.orbitControls
 * @param {ReturnType<import('./model-manager.js').createModelManager>} opts.modelManager
 * @param {(uuid: string, info: Object) => void} [opts.onSelected]
 * @param {(uuid: string, transform: Object) => void} [opts.onMoved]
 * @param {() => void} [opts.onDeselected]
 */
export function createModelInteraction({
  scene, camera, renderer, orbitControls, modelManager,
  onSelected, onMoved, onDeselected,
}) {
  const raycaster = new THREE.Raycaster();
  const mouse = new THREE.Vector2();

  let selectedId = null;
  let bboxHelper = null;

  // ── CSS2D overlay renderer (separate DOM layer over the canvas) ────
  const css2dRenderer = new CSS2DRenderer();
  css2dRenderer.setSize(renderer.domElement.clientWidth, renderer.domElement.clientHeight);
  css2dRenderer.domElement.style.position = 'absolute';
  css2dRenderer.domElement.style.top = '0';
  css2dRenderer.domElement.style.left = '0';
  css2dRenderer.domElement.style.pointerEvents = 'none';
  renderer.domElement.parentElement.appendChild(css2dRenderer.domElement);
  const ro = new ResizeObserver(() => {
    css2dRenderer.setSize(renderer.domElement.clientWidth, renderer.domElement.clientHeight);
  });
  ro.observe(renderer.domElement);

  // ── Action buttons as a CSS2DObject ────────────────────────────────
  let _suppressClick = false;
  function suppressNextClick() {
    _suppressClick = true;
    setTimeout(() => { _suppressClick = false; }, 200);
  }

  function createActionButtons() {
    const div = document.createElement('div');
    div.className = 'dioramap-action-buttons';
    const mkBtn = (icon, title, btnAction) => {
      const btn = document.createElement('button');
      btn.textContent = icon;
      btn.title = title;
      btn.className = 'dioramap-action-btn';
      btn.addEventListener('pointerdown', (e) => e.stopPropagation());
      btn.addEventListener('click', (e) => {
        e.stopPropagation();
        suppressNextClick();
        btnAction();
      });
      return btn;
    };
    div.appendChild(mkBtn('↔', 'Move', () => {
      if (!selectedId) return;
      const obj = modelManager.getObject(selectedId);
      gizmo.detach();
      gizmo.setMode('translate');
      gizmo.showX = true; gizmo.showY = true; gizmo.showZ = true;
      gizmo.attach(obj);
      gizmoRoot.visible = true;
    }));
    div.appendChild(mkBtn('↻', 'Rotate', () => {
      if (!selectedId) return;
      const obj = modelManager.getObject(selectedId);
      const cfg = modelManager.getConfig(selectedId);
      const axes = cfg?.allowedRotationAxes || ['x', 'y', 'z'];
      gizmo.detach();
      gizmo.setMode('rotate');
      gizmo.showX = axes.includes('x');
      gizmo.showY = axes.includes('y');
      gizmo.showZ = axes.includes('z');
      gizmo.attach(obj);
      gizmoRoot.visible = true;
    }));
    div.appendChild(mkBtn('✕', 'Delete', () => {
      if (!selectedId) return;
      const id = selectedId;
      deselect();
      modelManager.removeModel(id);
      if (onDeselected) onDeselected();
    }));
    const obj2d = new CSS2DObject(div);
    obj2d.visible = false;
    scene.add(obj2d);
    return obj2d;
  }

  const actionButtons = createActionButtons();

  function updateActionButtons() {
    if (!selectedId) {
      actionButtons.visible = false;
      actionButtons.element.style.display = 'none';
      css2dRenderer.render(scene, camera);
      return;
    }
    const obj = modelManager.getObject(selectedId);
    if (!obj) {
      actionButtons.visible = false;
      actionButtons.element.style.display = 'none';
      return;
    }
    const box = new THREE.Box3().setFromObject(obj);
    // Z-up: park buttons above the model's top (max.z), centred in XY.
    actionButtons.position.set(
      (box.min.x + box.max.x) / 2,
      (box.min.y + box.max.y) / 2,
      box.max.z + 0.3,
    );
    actionButtons.visible = true;
    actionButtons.element.style.display = '';
    css2dRenderer.render(scene, camera);
    // CSS2DRenderer can reset the overlay's pointer-events on render;
    // re-pin it so clicks pass through to the canvas everywhere except
    // the buttons themselves (which set pointer-events:auto inline).
    css2dRenderer.domElement.style.pointerEvents = 'none';
    for (const child of css2dRenderer.domElement.children) {
      child.style.pointerEvents = 'none';
    }
  }

  // ── TransformControls ─────────────────────────────────────────────
  const gizmo = new TransformControls(camera, renderer.domElement);
  gizmo.setSize(0.8);
  gizmo.setMode('translate');
  gizmo.addEventListener('dragging-changed', (ev) => {
    orbitControls.enabled = !ev.value;
  });
  gizmo.addEventListener('objectChange', () => {
    if (!selectedId) return;
    // Per-frame ground follow on X/Y/XY/XZ/YZ drags (matches the legacy
    // v2 SDK behaviour). Pure 'Z' drag = manual height tuning, leave it
    // alone. The earlier bug where this teleported the model to weird
    // Z values came from sampleGroundZ accidentally hitting
    // TransformControls' invisible drag-plane mesh — fixed in
    // model-manager.sampleGroundZ via isTransformControls* exclusion.
    const axis = gizmo.axis;
    const hasHorizontal = axis && axis !== 'Z';
    modelManager.syncFromObject(selectedId, { groundSnap: hasHorizontal });
    if (onMoved) {
      const info = modelManager.getModel(selectedId);
      if (info) onMoved(selectedId, { position: info.position, rotation: info.rotation });
    }
  });
  gizmo.addEventListener('mouseUp', suppressNextClick);
  // r169 splits TransformControls: getHelper() exposes the visible part.
  const gizmoRoot = typeof gizmo.getHelper === 'function' ? gizmo.getHelper() : gizmo;
  gizmoRoot.visible = false;
  scene.add(gizmoRoot);

  // ── Selection ─────────────────────────────────────────────────────

  function select(uuid) {
    deselect();
    const obj = modelManager.getObject(uuid);
    if (!obj) return;
    selectedId = uuid;

    // Highlight bbox — cyan wireframe.
    const box = new THREE.Box3().setFromObject(obj);
    bboxHelper = new THREE.Box3Helper(box, 0x00ddff);
    bboxHelper.material.linewidth = 2;
    bboxHelper.material.transparent = true;
    bboxHelper.material.opacity = 0.8;
    bboxHelper.name = 'model-selection-bbox';
    bboxHelper.renderOrder = 9998;
    scene.add(bboxHelper);

    gizmo.attach(obj);
    gizmoRoot.visible = true;
    gizmo.setMode('translate');
    gizmo.showX = true;
    gizmo.showY = true;
    gizmo.showZ = true;
    actionButtons.visible = true;
    actionButtons.element.style.display = '';

    if (onSelected) onSelected(uuid, modelManager.getModel(uuid));
  }

  function deselect() {
    if (bboxHelper) {
      scene.remove(bboxHelper);
      bboxHelper.geometry?.dispose();
      bboxHelper = null;
    }
    gizmo.detach();
    gizmoRoot.visible = false;
    actionButtons.visible = false;
    actionButtons.element.style.display = 'none';
    if (selectedId) {
      selectedId = null;
      if (onDeselected) onDeselected();
    }
  }

  function getSelectedId() { return selectedId; }

  // ── Click handler ─────────────────────────────────────────────────

  function onClick(event) {
    if (gizmo.dragging || _suppressClick) return;

    const rect = renderer.domElement.getBoundingClientRect();
    mouse.x = ((event.clientX - rect.left) / rect.width) * 2 - 1;
    mouse.y = -((event.clientY - rect.top) / rect.height) * 2 + 1;

    raycaster.setFromCamera(mouse, camera);

    // Build the candidate list from every model's mesh descendants.
    const targets = [];
    for (const inst of modelManager) {
      inst.object.traverse((child) => {
        if (child.isMesh) targets.push(child);
      });
    }

    const hits = raycaster.intersectObjects(targets, false);
    if (hits.length > 0) {
      let cur = hits[0].object;
      while (cur && !cur.userData.modelUUID) cur = cur.parent;
      if (cur?.userData.modelUUID) {
        select(cur.userData.modelUUID);
        return;
      }
    }
    deselect();
  }

  renderer.domElement.addEventListener('click', onClick);

  function dispose() {
    renderer.domElement.removeEventListener('click', onClick);
    try { deselect(); } catch (e) { console.warn('models: deselect during dispose failed', e); }
    // Three.js r169 restructured TransformControls — `this` in dispose()
    // is no longer an Object3D, so the upstream `this.traverse(...)` call
    // throws TypeError. Removing the gizmo from the scene + dropping our
    // reference is enough; WebGL resources GC normally. Wrap so a single
    // dispose failure doesn't tank the rest of the cleanup chain.
    try { gizmo.dispose(); } catch (e) { console.warn('models: gizmo.dispose failed (likely Three.js r169 TransformControls); continuing', e?.message || e); }
    try { scene.remove(gizmoRoot); } catch (e) { console.warn('models: scene.remove gizmoRoot failed', e); }
    try { scene.remove(actionButtons); } catch (e) { console.warn('models: scene.remove actionButtons failed', e); }
    try {
      if (css2dRenderer.domElement.parentElement) {
        css2dRenderer.domElement.parentElement.removeChild(css2dRenderer.domElement);
      }
    } catch (e) { console.warn('models: css2dRenderer detach failed', e); }
    try { ro.disconnect(); } catch (e) { /* already disconnected */ }
  }

  return {
    select, deselect, getSelectedId, dispose, gizmo,
    setMode(mode) { gizmo.setMode(mode); },
    /** Call each frame to keep action buttons in screen-space sync. */
    update: updateActionButtons,
  };
}
