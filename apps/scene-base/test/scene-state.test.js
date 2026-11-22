// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// SceneState unit tests — pure JS, no DOM, no Three.js needed.
//
// Run with `node test/scene-state.test.js` from apps/scene-base/.
//
// PLAN-VIEWER-SDK-PORT §7 S2 validation: tests for (a) flat leaf
// setVisible, (b) cascade on group node, (c) tri-state visibleDerived,
// (d) leaf removal. Plus a few sanity checks that fell out naturally
// while writing.

import { createSceneState } from '../src/scene/scene-state.js';

let passed = 0;
let failed = 0;

function check(label, cond) {
  if (cond) { passed++; console.log('  ✓', label); }
  else      { failed++; console.error('  ✗', label); }
}

function checkEq(label, actual, expected) {
  const ok = JSON.stringify(actual) === JSON.stringify(expected);
  if (ok) { passed++; console.log('  ✓', label); }
  else    { failed++; console.error('  ✗', label, '\n    actual:', actual, '\n    expected:', expected); }
}

// Mock Three.js Object3D — only `visible` is touched by SceneState.
function mockObj() { return { visible: true }; }

// Mock sceneCtx — SceneState reads `sun` from it; null is fine for our
// non-light tests.
function mockSceneCtx() { return { sun: null }; }

// ─────────────────────────────────────────────────────────────────────────
// (a) Flat leaf setVisible
// ─────────────────────────────────────────────────────────────────────────

console.log('\n[test] (a) flat leaf setVisible');
{
  const s = createSceneState(mockSceneCtx(), { lat: 48.8566, lon: 2.3522 });
  const obj = mockObj();
  s.visibility.attach('canopy', obj, { label: 'Canopy', defaultVisible: true });

  check('object initially visible', obj.visible === true);
  check('node lookup by id works', s.visibility.node('canopy')?.id === 'canopy');

  let evt = null;
  s.addEventListener('visibility-changed', (e) => { evt = e.detail; });

  s.visibility.setVisible('canopy', false);
  check('object visibility flipped on setVisible(false)', obj.visible === false);
  checkEq('event payload nodeId', evt?.nodeId, 'canopy');
  checkEq('event payload visible', evt?.visible, false);

  s.visibility.setVisible('canopy', true);
  check('object visibility flipped on setVisible(true)', obj.visible === true);

  // Idempotent: setting same value emits nothing.
  let count = 0;
  s.addEventListener('visibility-changed', () => count++);
  s.visibility.setVisible('canopy', true);
  check('idempotent set emits no event', count === 0);
}

// ─────────────────────────────────────────────────────────────────────────
// (b) Cascade on group node
// ─────────────────────────────────────────────────────────────────────────

console.log('\n[test] (b) cascade on group node');
{
  const s = createSceneState(mockSceneCtx(), { lat: 48.8566, lon: 2.3522 });

  // Build a tree:
  //   Roof Graph / 2-plane-geometry / edges
  //   Roof Graph / 2-plane-geometry / shared-edges
  //   Roof Graph / 3-merged       / edges
  const o1 = mockObj(), o2 = mockObj(), o3 = mockObj();
  s.visibility.attach('rg-2-pg-edges',     o1, { label: 'Edges',        path: ['Roof Graph', '2-plane-geometry'] });
  s.visibility.attach('rg-2-pg-shared',    o2, { label: 'Shared Edges', path: ['Roof Graph', '2-plane-geometry'] });
  s.visibility.attach('rg-3-merged-edges', o3, { label: 'Edges',        path: ['Roof Graph', '3-merged'] });

  check('three leaves attached', s.visibility.flatList().length === 3);
  check('all initially visible', o1.visible && o2.visible && o3.visible);

  // Cascade hide of the entire 'Roof Graph' subtree.
  let subtreeEvt = null;
  s.addEventListener('subtree-changed', (e) => { subtreeEvt = e.detail; });

  s.visibility.setVisible(['Roof Graph'], false);
  check('all three leaves hidden after cascade', !o1.visible && !o2.visible && !o3.visible);
  check('subtree event fired', subtreeEvt !== null);
  checkEq('subtree pathRoot', subtreeEvt?.pathRoot, ['Roof Graph']);
  check('subtree affected count', subtreeEvt?.affectedLeaves?.length === 3);

  // Cascade-show only the '2-plane-geometry' subtree.
  s.visibility.setVisible(['Roof Graph', '2-plane-geometry'], true);
  check('two leaves under 2-plane-geometry visible', o1.visible && o2.visible);
  check('one leaf under 3-merged still hidden', !o3.visible);
}

// ─────────────────────────────────────────────────────────────────────────
// (c) Tri-state visibleDerived
// ─────────────────────────────────────────────────────────────────────────

console.log('\n[test] (c) tri-state visibleDerived');
{
  const s = createSceneState(mockSceneCtx(), { lat: 48.8566, lon: 2.3522 });
  s.visibility.attach('a', mockObj(), { path: ['G'] });
  s.visibility.attach('b', mockObj(), { path: ['G'] });

  checkEq('all visible → "all"', s.visibility.node(['G'])?.visibleDerived, 'all');

  s.visibility.setVisible('a', false);
  checkEq('one of two off → "some"', s.visibility.node(['G'])?.visibleDerived, 'some');

  s.visibility.setVisible('b', false);
  checkEq('all off → "none"', s.visibility.node(['G'])?.visibleDerived, 'none');

  s.visibility.setVisible('a', true);
  s.visibility.setVisible('b', true);
  checkEq('all on again → "all"', s.visibility.node(['G'])?.visibleDerived, 'all');
}

// Nested tri-state.
{
  const s = createSceneState(mockSceneCtx(), { lat: 48.8566, lon: 2.3522 });
  s.visibility.attach('x1', mockObj(), { path: ['outer', 'inner1'] });
  s.visibility.attach('x2', mockObj(), { path: ['outer', 'inner1'] });
  s.visibility.attach('y1', mockObj(), { path: ['outer', 'inner2'] });

  checkEq('outer derived starts "all"', s.visibility.node(['outer'])?.visibleDerived, 'all');

  s.visibility.setVisible('x1', false);
  // inner1 = some, inner2 = all → outer should be "some"
  checkEq('mixed inner1 + full inner2 → outer "some"', s.visibility.node(['outer'])?.visibleDerived, 'some');

  s.visibility.setVisible('x2', false);
  s.visibility.setVisible('y1', false);
  // inner1 = none, inner2 = none → outer should be "none"
  checkEq('all leaves off → outer "none"', s.visibility.node(['outer'])?.visibleDerived, 'none');
}

// ─────────────────────────────────────────────────────────────────────────
// (d) Leaf / subtree removal
// ─────────────────────────────────────────────────────────────────────────

console.log('\n[test] (d) leaf removal');
{
  const s = createSceneState(mockSceneCtx(), { lat: 48.8566, lon: 2.3522 });
  s.visibility.attach('alpha', mockObj());
  s.visibility.attach('beta',  mockObj(), { path: ['group'] });
  s.visibility.attach('gamma', mockObj(), { path: ['group'] });

  check('three leaves before removal', s.visibility.flatList().length === 3);

  let clearedEvt = null;
  s.addEventListener('subtree-cleared', (e) => { clearedEvt = e.detail; });

  s.visibility.clearSubtree('alpha');
  check('alpha removed', s.visibility.node('alpha') === null);
  checkEq('cleared event reports alpha', clearedEvt?.removedLeafIds, ['alpha']);

  // Clearing the group node removes all descendant leaves AND the
  // (now-empty) group itself.
  clearedEvt = null;
  s.visibility.clearSubtree(['group']);
  check('group leaves removed', s.visibility.flatList().length === 0);
  check('beta gone', s.visibility.node('beta') === null);
  check('gamma gone', s.visibility.node('gamma') === null);
  checkEq('cleared event reports both leaves',
    new Set(clearedEvt?.removedLeafIds), new Set(['beta', 'gamma']));
}

// ─────────────────────────────────────────────────────────────────────────
// Sun sanity
// ─────────────────────────────────────────────────────────────────────────

console.log('\n[test] sun.setFromSunDirection sanity');
{
  // Mock DirectionalLight — has position.set + a target with position.set.
  const lightTarget = { position: { set(x, y, z) { this.x = x; this.y = y; this.z = z; }, updateMatrixWorld: () => {} } };
  const light = {
    position: { set(x, y, z) { this.x = x; this.y = y; this.z = z; } },
    target: lightTarget,
    visible: true,
  };
  const sceneCtx = { sun: light };

  const s = createSceneState(sceneCtx, { lat: 48.8566, lon: 2.3522 });

  let sunEvt = null;
  s.addEventListener('sun-changed', (e) => { sunEvt = e.detail; });

  s.sun.setFromSunDirection({ sun_dx: 0.5, sun_dy: 0.0, sun_dz: 0.866 });
  check('sun position updated', light.position.x !== undefined);
  check('sun event fired', sunEvt !== null);
  checkEq('sun state.dir', sunEvt?.dir, { dx: 0.5, dy: 0.0, dz: 0.866 });

  s.sun.setEnabled(false);
  check('sun disabled toggles light visibility', light.visible === false);
}

// ─────────────────────────────────────────────────────────────────────────

console.log(`\n[summary] ${passed}/${passed + failed} checks passed`);
if (failed > 0) process.exit(1);
