// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Minimal horizontal splitters for the 3-row center pane.
//
// The grid template uses `minmax(0, <flex>fr)` rows separated by 4px row
// splitters. Dragging a splitter rewrites the `fr` weights of the row above
// and below in proportion to the vertical delta, so rows resize smoothly
// without jitter and without leaving the grid overall.
//
// Order of rows is: dag, splitter, viewer, splitter, overlay. We persist
// no state across reloads — the default weights are sensible and the user
// can re-drag.

export function installSplitters(paneEl) {
  let fr = [1, 0.7, 0.55]; // dag, viewer, overlay
  const apply = () => {
    paneEl.style.gridTemplateRows =
      `minmax(0, ${fr[0]}fr) 4px minmax(0, ${fr[1]}fr) 4px minmax(0, ${fr[2]}fr)`;
  };
  apply();

  const splitters = paneEl.querySelectorAll('.pane-splitter');
  splitters.forEach((sp, idx) => {
    // idx 0 = between fr[0] and fr[1]; idx 1 = between fr[1] and fr[2].
    const above = idx, below = idx + 1;

    let dragging = false;
    let startY = 0;
    let paneH = 0;
    let startAbove = 0;
    let startBelow = 0;

    sp.addEventListener('pointerdown', (ev) => {
      dragging = true;
      startY = ev.clientY;
      const rect = paneEl.getBoundingClientRect();
      paneH = rect.height;
      startAbove = fr[above];
      startBelow = fr[below];
      sp.classList.add('active');
      sp.setPointerCapture(ev.pointerId);
    });
    sp.addEventListener('pointermove', (ev) => {
      if (!dragging) return;
      const dy = ev.clientY - startY;
      // Convert pixel delta into fr weight delta. Total fr sum stays the same;
      // we redistribute between the two adjacent rows.
      const total = startAbove + startBelow;
      // dy > 0 means dragging the splitter down → the above row grows.
      const px_per_fr = paneH / (fr.reduce((a, b) => a + b, 0) || 1);
      const dfr = Math.max(-startAbove + 0.1, Math.min(startBelow - 0.1, dy / px_per_fr));
      fr[above] = startAbove + dfr;
      fr[below] = startBelow - dfr;
      apply();
    });
    sp.addEventListener('pointerup', (ev) => {
      dragging = false;
      sp.classList.remove('active');
      sp.releasePointerCapture(ev.pointerId);
    });
    sp.addEventListener('pointercancel', () => {
      dragging = false;
      sp.classList.remove('active');
    });
  });
}
