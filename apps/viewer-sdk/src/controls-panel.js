// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// controls-panel.js — V2 minimal DOM panel.
//
// Per PLAN-VIEWER-SDK-PORT §5.5, the SDK ships an explicit allow-list of
// visibility toggles. V2 is `[]` (no composers yet); subsequent V-milestones
// extend it as composers land.
//
// Sun slider matches the legacy SDK's 3-slider UX (Month / Day / Time):
//   - Month: 1..12
//   - Day:   1..31 (the slider doesn't clamp to month length; SunCalc
//            handles overflow by rolling into the next month, which is
//            the legacy behaviour)
//   - Time:  packed slot 0..143 → hour*6 + minute/10
//
// The panel binds to sceneState.sun. Slider input → sceneState.sun.setFromDate;
// 'sun-changed' event → reflect state in the slider readouts.

// CSS for .dioramap-controls* lives in the bundled dioramap-viewer.css
// (CSP-friendly — no runtime <style> injection).

// Curated allow-list (PLAN-VIEWER-SDK-PORT §5.5). Each entry is one
// user-visible checkbox that maps to a leaf in sceneState.visibility. The
// composer that owns each leaf must declare it (via ctx.render3D's
// layer.id) — IDs here MUST match what the composer registers.
//
// Adds per release require a CHANGELOG bump (CHANGELOG.md / SDK semver).
//
// V4 curated set — intentionally minimal: only cadastre outlines is a
// user-facing toggle. Point cloud, canopy, surround terrain are part of
// the production scene and shouldn't be hidden in the embed. The
// streaming preview hides itself when the disc composite finishes
// (point-cloud-discs.viz declares it via meta.supersedes), so users
// don't see the blue preview lingering once the high-quality discs
// arrive. Per §5.5 — adding/removing entries here is a documented
// per-release product decision (CHANGELOG bump).
const EXPOSED_VISIBILITY_TOGGLES = [
  { nodeId: 'parcels', label: 'Cadastre outlines' },
];

function pad2(n) {
  return String(n).padStart(2, '0');
}

function buildSunSection(panel) {
  const heading = document.createElement('h4');
  heading.textContent = 'Sun';
  panel.appendChild(heading);

  function makeRow(label, min, max, value, fmt) {
    const row = document.createElement('div');
    row.className = 'row';
    const lab = document.createElement('label');
    lab.textContent = label;
    const input = document.createElement('input');
    input.type = 'range';
    input.min = String(min);
    input.max = String(max);
    input.value = String(value);
    const val = document.createElement('span');
    val.className = 'val';
    val.textContent = fmt(value);
    row.appendChild(lab);
    row.appendChild(input);
    row.appendChild(val);
    panel.appendChild(row);
    return { input, val };
  }

  // Initial state — match viewer.js's mount default.
  const initial = { year: 2024, month: 6, day: 15, slot: 12 * 6 };

  const month = makeRow('Month', 1, 12, initial.month, (v) => String(v));
  const day = makeRow('Day', 1, 31, initial.day, (v) => String(v));
  const time = makeRow('Time', 0, 143, initial.slot, (v) => {
    const h = Math.floor(v / 6);
    const m = (v % 6) * 10;
    return `${pad2(h)}:${pad2(m)}`;
  });

  return { month, day, time, initial };
}

/**
 * Mount the SDK controls panel.
 * @param {HTMLElement} container  — viewer container (panel is appended inside)
 * @param {SceneState}  sceneState — from createSceneState
 * @returns {{ dispose: () => void }}
 */
export function mountControlsPanel(container, sceneState) {
  const panel = document.createElement('div');
  panel.className = 'dioramap-controls';

  // Collapse header — full-width clickable bar with caret. Default-
  // collapsed under 768px (mobile + small tablet) so the panel doesn't
  // eat half the viewport on phone-sized embeds. Desktop stays open.
  const toggle = document.createElement('button');
  toggle.type = 'button';
  toggle.className = 'dioramap-controls-toggle';
  toggle.setAttribute('aria-expanded', 'true');
  const toggleLabel = document.createElement('span');
  toggleLabel.className = 'label';
  toggleLabel.textContent = 'Contrôles';
  const toggleCaret = document.createElement('span');
  toggleCaret.className = 'caret';
  toggleCaret.textContent = '▾';
  toggleCaret.setAttribute('aria-hidden', 'true');
  toggle.appendChild(toggleLabel);
  toggle.appendChild(toggleCaret);
  panel.appendChild(toggle);

  const body = document.createElement('div');
  body.className = 'dioramap-controls-body';
  panel.appendChild(body);

  toggle.addEventListener('click', () => {
    const willCollapse = !panel.classList.contains('is-collapsed');
    panel.classList.toggle('is-collapsed', willCollapse);
    toggle.setAttribute('aria-expanded', willCollapse ? 'false' : 'true');
  });

  // Default-collapsed under 768px. Done after-mount so the resize doesn't
  // toggle existing user preference; once they expand on mobile, it stays.
  if (typeof window !== 'undefined' && window.matchMedia &&
      window.matchMedia('(max-width: 768px)').matches) {
    panel.classList.add('is-collapsed');
    toggle.setAttribute('aria-expanded', 'false');
  }

  const sunUI = buildSunSection(body);

  // ── Visibility toggles section (V2: placeholder) ───────────────
  const togglesHeading = document.createElement('h4');
  togglesHeading.textContent = 'Layers';
  body.appendChild(togglesHeading);

  const togglesContainer = document.createElement('div');
  togglesContainer.className = 'toggles';
  body.appendChild(togglesContainer);

  const rowEls = new Map();
  if (EXPOSED_VISIBILITY_TOGGLES.length === 0) {
    const empty = document.createElement('div');
    empty.className = 'toggle-empty';
    empty.textContent = '(no layers configured)';
    togglesContainer.appendChild(empty);
  } else {
    for (const t of EXPOSED_VISIBILITY_TOGGLES) {
      const row = document.createElement('label');
      row.style.display = 'block';
      const cb = document.createElement('input');
      cb.type = 'checkbox';
      cb.checked = true;
      cb.addEventListener('change', () => {
        sceneState.visibility.setVisible(t.nodeId, cb.checked);
      });
      row.appendChild(cb);
      row.appendChild(document.createTextNode(' ' + t.label));
      togglesContainer.appendChild(row);
      rowEls.set(t.nodeId, cb);
    }
  }

  container.appendChild(panel);

  // ── Wire sun sliders → sceneState.sun ──────────────────────────
  function readDate() {
    const month = Number(sunUI.month.input.value);
    const day = Number(sunUI.day.input.value);
    const slot = Number(sunUI.time.input.value);
    const hour = Math.floor(slot / 6);
    const minute = (slot % 6) * 10;
    return { year: sunUI.initial.year, month, day, hour, minute };
  }

  function pushSliderToState() {
    sceneState.sun.setFromDate(readDate());
  }

  const onMonth = () => {
    sunUI.month.val.textContent = sunUI.month.input.value;
    pushSliderToState();
  };
  const onDay = () => {
    sunUI.day.val.textContent = sunUI.day.input.value;
    pushSliderToState();
  };
  const onTime = () => {
    const slot = Number(sunUI.time.input.value);
    const h = Math.floor(slot / 6);
    const m = (slot % 6) * 10;
    sunUI.time.val.textContent = `${pad2(h)}:${pad2(m)}`;
    pushSliderToState();
  };
  sunUI.month.input.addEventListener('input', onMonth);
  sunUI.day.input.addEventListener('input', onDay);
  sunUI.time.input.addEventListener('input', onTime);

  // ── Reflect sceneState.sun back into the sliders ───────────────
  // sun-direction artifact (V5) and any external setSunTime() callers
  // mutate state without touching the DOM; mirror it here.
  const onSunChanged = (ev) => {
    const d = ev.detail.date;
    if (!d) return;
    sunUI.month.input.value = String(d.month);
    sunUI.month.val.textContent = String(d.month);
    sunUI.day.input.value = String(d.day);
    sunUI.day.val.textContent = String(d.day);
    const slot = d.hour * 6 + Math.floor(d.minute / 10);
    sunUI.time.input.value = String(slot);
    sunUI.time.val.textContent = `${pad2(d.hour)}:${pad2(d.minute)}`;
  };
  sceneState.addEventListener('sun-changed', onSunChanged);

  // ── Reflect visibility flips in the (currently empty) toggles ──
  const onVisChanged = (ev) => {
    const cb = rowEls.get(ev.detail.nodeId);
    if (cb) cb.checked = ev.detail.visible;
  };
  sceneState.addEventListener('visibility-changed', onVisChanged);

  function setCollapsed(collapse) {
    panel.classList.toggle('is-collapsed', collapse);
    toggle.setAttribute('aria-expanded', collapse ? 'false' : 'true');
  }

  return {
    isCollapsed() { return panel.classList.contains('is-collapsed'); },
    collapse()    { setCollapsed(true); },
    expand()      { setCollapsed(false); },
    dispose() {
      sceneState.removeEventListener('sun-changed', onSunChanged);
      sceneState.removeEventListener('visibility-changed', onVisChanged);
      panel.remove();
    },
  };
}
