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
//
// On failure (onRunFailed, docs/ERRORS.md) the same card turns into an
// error card: a plain-language title + hint, a collapsible "technical
// details" block with the full report (code, step, upstream URL / HTTP
// status / response excerpt, versions), and Copy / Report on GitHub /
// Try again buttons. The bar never just freezes.

import { formatReport, issueUrl } from '@dioramap/protocol';
import { describeError } from './error-messages.js';

/**
 * @param {HTMLElement} container
 * @returns {{
 *   onRunStarted: (msg: {step_plan?: string[]}) => void,
 *   onStepStarted: (msg: {step?: string}) => void,
 *   onStepComplete: (msg: {step?: string}) => void,
 *   onRunComplete: () => void,
 *   onRunFailed: (err: import('@dioramap/protocol').DioramapError,
 *                 opts?: {onRetry?: () => void}) => void,
 *   dispose: () => void,
 * }}
 */
export function mountLoadingBar(container, { locale = 'en', issueRepo, reportIssues = true } = {}) {
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
  // Error panel — built on demand by onRunFailed, removed on next run.
  let errorPanel = null;
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

  function clearError() {
    if (errorPanel) { errorPanel.remove(); errorPanel = null; }
    overlay.classList.remove('is-error');
    trackBg.hidden = false;
    pctLabel.hidden = false;
  }

  function onRunStarted(msg) {
    clearError();
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

  function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }

  function onRunFailed(err, { onRetry } = {}) {
    clearError();
    show();
    overlay.classList.add('is-error');
    trackBg.hidden = true;
    pctLabel.hidden = true;
    const d = describeError(err, locale);
    label.textContent = `⚠ ${d.title}`;

    errorPanel = el('div', 'dioramap-error');
    if (d.body) errorPanel.appendChild(el('div', 'dioramap-error-hint', d.body));
    // The technical one-liner is always visible: it is the most useful
    // thing to quote, and it's what an embedder's support sees first.
    errorPanel.appendChild(el('div', 'dioramap-error-msg', `${err.code}: ${err.message}`));

    const details = el('details', 'dioramap-error-details');
    details.appendChild(el('summary', null, d.ui.details));
    const report = formatReport(err);
    details.appendChild(el('pre', 'dioramap-error-pre', JSON.stringify(err.toReport?.() ?? err, null, 2)));
    errorPanel.appendChild(details);

    const actions = el('div', 'dioramap-error-actions');
    const copyBtn = el('button', 'dioramap-error-btn', d.ui.copy);
    copyBtn.type = 'button';
    copyBtn.addEventListener('click', async () => {
      try {
        await navigator.clipboard.writeText(report);
        copyBtn.textContent = d.ui.copied;
        setTimeout(() => { copyBtn.textContent = d.ui.copy; }, 1500);
      } catch {
        // Clipboard blocked (insecure context / permissions): open the
        // details so the user can select the text by hand.
        details.open = true;
      }
    });
    actions.appendChild(copyBtn);
    if (reportIssues) {
      const a = el('a', 'dioramap-error-btn', d.ui.report);
      a.href = issueUrl(err, issueRepo ? { repo: issueRepo } : undefined);
      a.target = '_blank';
      a.rel = 'noopener noreferrer';
      actions.appendChild(a);
    }
    if (onRetry && d.retryable) {
      const retry = el('button', 'dioramap-error-btn is-primary', d.ui.retry);
      retry.type = 'button';
      retry.addEventListener('click', () => { clearError(); onRetry(); });
      actions.appendChild(retry);
    }
    errorPanel.appendChild(actions);
    card.appendChild(errorPanel);
  }

  function dispose() {
    if (_hideTimeout) clearTimeout(_hideTimeout);
    if (_removeTimeout) clearTimeout(_removeTimeout);
    if (overlay.parentNode) overlay.parentNode.removeChild(overlay);
  }

  return { onRunStarted, onStepStarted, onStepComplete, onRunComplete, onRunFailed, dispose };
}
