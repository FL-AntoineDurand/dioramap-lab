// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Loading overlay — centred card with a progress bar.
// Driven by RUN_STARTED / STEP_COMPLETE / RUN_COMPLETE from
// @dioramap/protocol's WSClient.
//
// RUN_STARTED carries `step_plan` (array of step names) — used as the
// progress denominator. STEP_COMPLETE per step ticks the numerator.
// The label stays "Loading…" throughout: the SDK is the production
// embed, end users shouldn't see internal pipeline step names. The
// lab-ui has its own timeline panel for that.
//
// On RUN_COMPLETE we fade the overlay out; the next RUN_STARTED brings
// it back. Lives inside the SDK's container so it sits above the
// WebGL canvas + controls panel + compass.

/**
 * @param {HTMLElement} container
 * @returns {{
 *   onRunStarted: (msg: {step_plan?: string[]}) => void,
 *   onStepStarted: (msg: {step?: string}) => void,
 *   onStepComplete: (msg: {step?: string}) => void,
 *   onRunComplete: () => void,
 *   dispose: () => void,
 * }}
 */
export function mountLoadingBar(container) {
  const overlay = document.createElement('div');
  overlay.className = 'dioramap-loading-overlay';

  const card = document.createElement('div');
  card.className = 'dioramap-loading-card';

  const label = document.createElement('div');
  label.className = 'dioramap-loading-label';
  label.textContent = 'Loading…';

  const trackBg = document.createElement('div');
  trackBg.className = 'dioramap-loading-track';

  const fill = document.createElement('div');
  fill.className = 'dioramap-loading-fill';

  const pctLabel = document.createElement('div');
  pctLabel.className = 'dioramap-loading-pct';
  pctLabel.textContent = '0%';

  trackBg.appendChild(fill);
  card.appendChild(label);
  card.appendChild(trackBg);
  card.appendChild(pctLabel);
  overlay.appendChild(card);
  container.appendChild(overlay);

  let total = 0;
  let completed = 0;
  let _hideTimeout = null;
  let _removeTimeout = null;

  function show() {
    if (_hideTimeout) { clearTimeout(_hideTimeout); _hideTimeout = null; }
    if (_removeTimeout) { clearTimeout(_removeTimeout); _removeTimeout = null; }
    overlay.classList.add('is-visible');
    overlay.style.opacity = '1';
  }

  function refresh() {
    if (total <= 0) return;
    const pct = Math.min(99, Math.max(0, (completed / total) * 100));
    fill.style.width = `${pct}%`;
    pctLabel.textContent = `${pct.toFixed(0)}%`;
  }

  function onRunStarted(msg) {
    total = (msg?.step_plan?.length ?? 0);
    completed = 0;
    fill.style.width = '0%';
    pctLabel.textContent = '0%';
    label.textContent = 'Loading…';
    show();
  }

  // Step-started is currently a no-op for the SDK overlay — we used to
  // surface the active step name here, but exposing internal pipeline
  // step names to end-users isn't appropriate for the production embed.
  function onStepStarted() { /* intentionally empty */ }

  function onStepComplete() {
    completed += 1;
    refresh();
  }

  function onRunComplete() {
    fill.style.width = '100%';
    pctLabel.textContent = '100%';
    label.textContent = 'Ready';
    _hideTimeout = setTimeout(() => { overlay.style.opacity = '0'; }, 400);
    _removeTimeout = setTimeout(() => { overlay.classList.remove('is-visible'); }, 1000);
  }

  function dispose() {
    if (_hideTimeout) clearTimeout(_hideTimeout);
    if (_removeTimeout) clearTimeout(_removeTimeout);
    if (overlay.parentNode) overlay.parentNode.removeChild(overlay);
  }

  return { onRunStarted, onStepStarted, onStepComplete, onRunComplete, dispose };
}
