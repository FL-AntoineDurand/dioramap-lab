// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// SceneState — the 3D-scene state surface (sun + visibility tree + events).
//
// Both the SDK's controls panel and lab-ui's "3D scene parameters" panel
// bind to this surface. State + effect + cascading logic live here once;
// each consumer ships only its own DOM chrome that listens to events and
// calls setters.
//
// Design reference: PLAN-VIEWER-SDK-PORT §4.3.
//
// Two subsurfaces:
//   sceneState.sun         → SunControl (sun.js)
//   sceneState.visibility  → VisibilityTree (this file)
//
// Events emitted on the SceneState (EventTarget):
//   'sun-changed'             — sun.state mutated
//   'visibility-registered'   — first ctx.add(obj, layerId) for a new node
//   'visibility-changed'      — leaf or group toggled
//   'subtree-changed'         — group cascade flipped multiple leaves
//   'subtree-cleared'         — a composer disposed (subtree removed)
//
// The visibility tree is initialized empty. Composers populate it via
// `ctx.add(obj, layerId)` (host-impl in render-context.js delegates to
// `visibility.attach`, which auto-declares the layer if not yet seen).

import * as THREE from 'three';

import { SunControl } from './sun.js';

// ─────────────────────────────────────────────────────────────────────────
// Visibility tree
// ─────────────────────────────────────────────────────────────────────────

/**
 * Internal node shape:
 *   { id, label, parent, children: Map<id, Node>, objects: THREE.Object3D[],
 *     visible, defaultVisible, isLeaf }
 *
 * Group nodes (isLeaf=false): `children` Map populated, `objects` empty.
 * Leaf nodes  (isLeaf=true):  `children` empty,        `objects` populated.
 *
 * `visible` is the authoritative state for leaves; for groups it's a
 * derived value computed from children at query time (see _deriveGroup).
 */

class VisibilityTree {
  constructor(emit) {
    this._emit = emit;
    // Synthetic root node. Real nodes either sit at root (path=[]) or
    // descend through path components.
    this._root = this._makeGroup({ id: '__root', label: '', parent: null });
    // Flat index: layerId → leaf node, for O(1) setVisible(id) lookup.
    this._byId = new Map();
  }

  _makeGroup({ id, label, parent }) {
    return {
      id, label, parent,
      children: new Map(),
      objects: [],
      visible: true,
      defaultVisible: true,
      isLeaf: false,
    };
  }

  _makeLeaf({ id, label, parent, defaultVisible = true, source = null }) {
    return {
      id, label, parent,
      children: new Map(),
      objects: [],
      visible: defaultVisible,
      defaultVisible,
      isLeaf: true,
      // Provenance — which subsystem attached this leaf:
      //   'composer' → CompositeViewsDispatcher (composite visualizer)
      //   'inspector' → ArtifactInspector (per-artifact step visualizer)
      //   null → unknown / legacy
      // Surfaced in the lab-ui visibility tree as a small text badge.
      source,
    };
  }

  // Walk path from root, creating intermediate group nodes on demand.
  // Returns the parent group node where a leaf or group with `terminalId`
  // would attach.
  _ensurePath(path) {
    let cur = this._root;
    for (const segment of path) {
      if (!cur.children.has(segment)) {
        const node = this._makeGroup({ id: segment, label: segment, parent: cur });
        cur.children.set(segment, node);
      }
      cur = cur.children.get(segment);
    }
    return cur;
  }

  // ─── Declaration / attachment ──────────────────────────────────────────

  /**
   * Declare a layer (leaf node). Idempotent: re-declare with the same id is
   * a no-op (returns the existing leaf). Path must NOT include the leaf's
   * own id; e.g. for a leaf 'canopy' under ['Roof Graph', 'Plane 4'], pass
   * `{ id: 'canopy', path: ['Roof Graph', 'Plane 4'] }`.
   *
   * @returns {Object} the leaf node (internal — read-only from outside).
   */
  declare({ id, label, path = [], defaultVisible = true, source = null }) {
    const existing = this._byId.get(id);
    if (existing) return existing;
    const parent = this._ensurePath(path);
    if (parent.children.has(id)) {
      // A group with this id was implicitly created earlier (when a deeper
      // descendant was declared). Convert it to a leaf — this is unusual
      // but legitimate (composer order independence).
      const orphan = parent.children.get(id);
      if (orphan.children.size > 0) {
        // Genuine collision: declaring a leaf where a populated group lives.
        // Reject — this is a composer bug.
        throw new Error(
          `VisibilityTree.declare: layerId "${id}" collides with a group node containing ${orphan.children.size} children`,
        );
      }
    }
    const leaf = this._makeLeaf({ id, label: label ?? id, parent, defaultVisible, source });
    parent.children.set(id, leaf);
    this._byId.set(id, leaf);
    // Emit only the id — NOT a full _snapshot(leaf). The snapshot calls
    // _pathOf (walks to root) per leaf, and a pipeline run registers
    // hundreds of leaves, but no consumer reads detail.node (the toolbar and
    // legend both re-derive from state on their coalesced re-render). A
    // consumer that needs the snapshot can call visibility.node(id) lazily.
    this._emit('visibility-registered', { id });
    return leaf;
  }

  /**
   * Attach a Three.js object to a leaf. Auto-declares the leaf with default
   * settings if not yet seen — typical path for `ctx.add(obj, layerId)`.
   * Sets the object's `.visible` to match the leaf's current visibility.
   */
  attach(layerId, object, { label, path, defaultVisible, source } = {}) {
    let leaf = this._byId.get(layerId);
    if (!leaf) {
      leaf = this.declare({ id: layerId, label, path, defaultVisible, source });
    } else if (source && !leaf.source) {
      // Late-arriving source tag — fill it in if the leaf was declared
      // without one (defensive; declare normally captures source).
      leaf.source = source;
    }
    leaf.objects.push(object);
    if (object) object.visible = leaf.visible;
  }

  // ─── Queries ───────────────────────────────────────────────────────────

  /**
   * Snapshot the entire tree. Use for UI rendering. Returns a plain JS
   * object with `children` arrays (not Maps) for easier iteration.
   */
  tree() {
    return this._snapshotTree(this._root, true);
  }

  /**
   * Flat list of every leaf node. Each entry includes `path` (array of
   * group ids leading to it).
   */
  flatList() {
    const out = [];
    this._collectLeaves(this._root, [], out);
    return out;
  }

  /**
   * Look up a node by leaf id (string) or path-from-root (array of ids).
   * Returns a snapshot (with `visibleDerived` for groups) or null.
   */
  node(idOrPath) {
    let n;
    if (typeof idOrPath === 'string') {
      n = this._byId.get(idOrPath);
      // Could also be a group named exactly idOrPath at root level.
      if (!n) n = this._root.children.get(idOrPath);
    } else if (Array.isArray(idOrPath)) {
      n = this._root;
      for (const seg of idOrPath) {
        n = n?.children.get(seg);
        if (!n) return null;
      }
    }
    if (!n) return null;
    return this._snapshot(n);
  }

  // ─── Mutations ─────────────────────────────────────────────────────────

  /**
   * Set visibility for a leaf or group. On a group, cascades to every
   * descendant leaf and emits one `subtree-changed` event with the
   * affected leaves.
   */
  setVisible(idOrPath, bool) {
    const target = this._lookupRaw(idOrPath);
    if (!target) return;
    const value = !!bool;
    if (target.isLeaf) {
      if (target.visible === value) return;
      target.visible = value;
      for (const obj of target.objects) {
        if (obj) obj.visible = value;
      }
      this._emit('visibility-changed', {
        nodeId: target.id,
        path: this._pathOf(target),
        visible: value,
      });
    } else {
      const affected = [];
      this._cascade(target, value, affected);
      if (affected.length > 0) {
        // pathRoot = full path TO the cascade root, including its own id.
        // _pathOf returns the parent-chain path; append target.id so
        // subscribers can identify which subtree was toggled.
        this._emit('subtree-changed', {
          pathRoot: [...this._pathOf(target), target.id],
          affectedLeaves: affected,
        });
      }
    }
  }

  hideAll()    { this._setAllLeaves(false); }
  showAll()    { this._setAllLeaves(true); }

  /** Restore each leaf to its `defaultVisible`. */
  setDefaults() {
    const affected = [];
    for (const leaf of this._byId.values()) {
      if (leaf.visible !== leaf.defaultVisible) {
        leaf.visible = leaf.defaultVisible;
        for (const obj of leaf.objects) {
          if (obj) obj.visible = leaf.visible;
        }
        affected.push({ nodeId: leaf.id, visible: leaf.visible });
      }
    }
    if (affected.length > 0) {
      this._emit('subtree-changed', {
        pathRoot: [],
        affectedLeaves: affected,
      });
    }
  }

  /**
   * Remove a leaf or entire subtree (for composer dispose). Detaches all
   * Three.js objects from the scene graph (caller's responsibility — we
   * just drop our references) and emits `subtree-cleared`.
   */
  clearSubtree(idOrPath) {
    const target = this._lookupRaw(idOrPath);
    if (!target || target === this._root) return;
    const removedLeaves = [];
    this._collectLeavesRaw(target, removedLeaves);
    // Detach from parent.
    target.parent.children.delete(target.id);
    // Remove from byId index.
    for (const leaf of removedLeaves) {
      this._byId.delete(leaf.id);
    }
    // Garbage-collect now-empty group ancestors so the tree doesn't fill
    // up with empty group skeletons after multiple disposes.
    let p = target.parent;
    while (p && p !== this._root && p.children.size === 0) {
      const grandparent = p.parent;
      grandparent.children.delete(p.id);
      p = grandparent;
    }
    this._emit('subtree-cleared', {
      pathRoot: [...this._pathOf(target), target.id],
      removedLeafIds: removedLeaves.map(l => l.id),
    });
  }

  // ─── Internals ─────────────────────────────────────────────────────────

  _lookupRaw(idOrPath) {
    if (typeof idOrPath === 'string') {
      return this._byId.get(idOrPath) ?? this._root.children.get(idOrPath) ?? null;
    }
    if (Array.isArray(idOrPath)) {
      let n = this._root;
      for (const seg of idOrPath) {
        n = n?.children.get(seg);
        if (!n) return null;
      }
      return n;
    }
    return null;
  }

  _pathOf(node) {
    const path = [];
    let n = node;
    while (n && n.parent && n.parent !== this._root) {
      path.unshift(n.parent.id);
      n = n.parent;
    }
    return path;
  }

  _cascade(group, value, affected) {
    for (const child of group.children.values()) {
      if (child.isLeaf) {
        if (child.visible !== value) {
          child.visible = value;
          for (const obj of child.objects) {
            if (obj) obj.visible = value;
          }
          // Field name `nodeId` mirrors the visibility-changed event so
          // listeners can destructure the same shape from both events
          // (render-context.js's lazy-build trampoline relies on it —
          // a prior `{id}` shape silently dropped group cascades).
          affected.push({ nodeId: child.id, visible: value });
        }
      } else {
        this._cascade(child, value, affected);
      }
    }
  }

  _setAllLeaves(value) {
    const affected = [];
    for (const leaf of this._byId.values()) {
      if (leaf.visible !== value) {
        leaf.visible = value;
        for (const obj of leaf.objects) {
          if (obj) obj.visible = value;
        }
        affected.push({ nodeId: leaf.id, visible: value });
      }
    }
    if (affected.length > 0) {
      this._emit('subtree-changed', { pathRoot: [], affectedLeaves: affected });
    }
  }

  _deriveGroup(group) {
    let on = 0, off = 0;
    for (const child of group.children.values()) {
      if (child.isLeaf) {
        if (child.visible) on++; else off++;
      } else {
        const sub = this._deriveGroup(child);
        if (sub === 'all') on++;
        else if (sub === 'none') off++;
        else return 'some';
      }
    }
    if (on === 0 && off === 0) return 'all';   // empty group → vacuously visible
    if (off === 0) return 'all';
    if (on === 0) return 'none';
    return 'some';
  }

  _snapshot(node) {
    if (node.isLeaf) {
      return {
        id: node.id,
        label: node.label,
        path: this._pathOf(node),
        isLeaf: true,
        visible: node.visible,
        defaultVisible: node.defaultVisible,
        objectCount: node.objects.length,
        source: node.source,
      };
    }
    return {
      id: node.id,
      label: node.label,
      path: this._pathOf(node),
      isLeaf: false,
      visibleDerived: this._deriveGroup(node),
    };
  }

  _snapshotTree(node, isRoot) {
    const out = {
      id: isRoot ? null : node.id,
      label: isRoot ? '' : node.label,
      isLeaf: node.isLeaf,
      children: [],
    };
    if (node.isLeaf) {
      out.visible = node.visible;
      out.defaultVisible = node.defaultVisible;
      out.objectCount = node.objects.length;
    } else if (!isRoot) {
      out.visibleDerived = this._deriveGroup(node);
    }
    for (const child of node.children.values()) {
      out.children.push(this._snapshotTree(child, false));
    }
    return out;
  }

  _collectLeaves(node, path, out) {
    if (node.isLeaf) {
      out.push({
        id: node.id,
        label: node.label,
        path: [...path],
        visible: node.visible,
        defaultVisible: node.defaultVisible,
        objectCount: node.objects.length,
      });
      return;
    }
    const subPath = node === this._root ? [] : [...path, node.id];
    for (const child of node.children.values()) {
      this._collectLeaves(child, subPath, out);
    }
  }

  _collectLeavesRaw(node, out) {
    if (node.isLeaf) { out.push(node); return; }
    for (const child of node.children.values()) {
      this._collectLeavesRaw(child, out);
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────
// SceneState — top-level surface
// ─────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────
// Selection — cross-visualizer point-set highlight slot
// ─────────────────────────────────────────────────────────────────────────
//
// Lets one visualizer (e.g. the Lambert disc click handler) tell another
// (e.g. the cascade-segments 3D point cloud) to highlight a specific
// point set. Independent of visibility — visible layers render their
// usual content but apply the selection on top (highlight in / dim out).
//
// Generic enough for any future "click X → highlight Y" use case
// (segment-list row click, KDE peak hover, etc.). One active selection
// at a time; subsequent setSelection calls replace it.

class Selection extends EventTarget {
  constructor() {
    super();
    this._state = null;
  }

  // spec = { source, segmentIds?, pointIndices? }
  // Either segmentIds (Int16Array of cascade-segment ids to highlight)
  // OR pointIndices (Uint32Array of global lidar-positions indices) — at
  // least one must be present. `source` is the visualizer's layerId
  // string, so the system can clear stale selections when that
  // visualizer disposes.
  setSelection(spec) {
    if (!spec || (!spec.segmentIds && !spec.pointIndices)) {
      this.clearSelection();
      return;
    }
    this._state = {
      source: spec.source ?? null,
      segmentIds: spec.segmentIds ?? null,
      pointIndices: spec.pointIndices ?? null,
    };
    this.dispatchEvent(new CustomEvent('selection-changed', { detail: this._state }));
  }

  clearSelection() {
    if (!this._state) return;
    this._state = null;
    this.dispatchEvent(new CustomEvent('selection-changed', { detail: null }));
  }

  // Returns null when no selection is active.
  getSelection() {
    return this._state;
  }

  // Drop the selection iff it was set by `source`. Use when a visualizer
  // is about to dispose so it doesn't leave a stale highlight that
  // points at indices nobody can see anymore.
  clearIfSource(source) {
    if (this._state && this._state.source === source) this.clearSelection();
  }
}

export class SceneState extends EventTarget {
  /**
   * @param {Object} sceneCtx                    — the scene context (Viewer instance)
   * @param {{lat:number, lon:number}} location  — geographic anchor for SunCalc
   */
  constructor(sceneCtx, { lat, lon }) {
    super();
    this.sceneCtx = sceneCtx;
    // Every event dispatched through the SceneState surface also marks
    // the viewer as needing a re-render. This is what makes render-on-
    // demand work: visibility toggles, sun-direction changes, selection
    // updates, etc. all originate as events on this EventTarget; by
    // calling viewer.invalidate() in the shared emit() the rAF loop
    // picks up the change and renders one frame.
    const emit = (type, detail) => {
      this.dispatchEvent(new CustomEvent(type, { detail }));
      sceneCtx?.invalidate?.();
    };

    this.sun = new SunControl(sceneCtx?.sun ?? null, { lat, lon });
    // Forward sun events up to the SceneState surface so consumers only
    // need to bind to one EventTarget.
    this.sun.addEventListener('sun-changed', (ev) => emit('sun-changed', ev.detail));

    this.visibility = new VisibilityTree(emit);
    this.selection = new Selection();
    this.selection.addEventListener('selection-changed', (ev) =>
      emit('selection-changed', ev.detail));
  }
}

/**
 * Factory — preferred over `new SceneState(...)` so future versions can
 * change construction without breaking consumers.
 */
export function createSceneState(sceneCtx, { lat, lon }) {
  return new SceneState(sceneCtx, { lat, lon });
}
