// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// attribution.js — always-visible bottom-right credit overlay (D5a).
//
// Renders two halves separated by `·`:
//   1. Hardcoded "Powered by [dioramap.com](https://dioramap.com)" — never
//      changes. The SDK's own credit, present on every embedder.
//   2. Pipeline-declared "Data: <label>, <label>, …" — populated from
//      CATALOG_RESPONSE.attributions[] (D0). Each label is rendered as a
//      link when its url is non-null, plain text otherwise. The whole
//      "Data:" half is omitted when the pipeline declares no attributions.
//
// Cannot be disabled via SDK options. See PLAN-DIORAMAP-DEPLOY.md §3 D5a:
// the "Powered by Dioramap" + the pipeline data attribution are contractual,
// not feature-flagged. CSS classes (`dioramap-attr-…`) live in the bundled
// dioramap-viewer.css that embedders <link> alongside the JS bundle —
// no runtime <style> injection (CSP-friendly).
//
// Mobile-aware: at container widths < 480px, the overlay collapses to a
// compact one-liner ("dioramap.com · IGN · …") via a CSS class toggled
// from a ResizeObserver.

// Equality check on the attributions array. Cheap (single pass) and avoids
// re-rendering on every catalog event when nothing changed.
function attributionsEqual(a, b) {
  if (!a && !b) return true;
  if (!a || !b) return false;
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) {
    if (a[i].label !== b[i].label) return false;
    const aUrl = a[i].url ?? null;
    const bUrl = b[i].url ?? null;
    if (aUrl !== bUrl) return false;
  }
  return true;
}

/**
 * Mount the attribution overlay into `container`.
 * @param {HTMLElement} container — the SDK's root container element
 * @returns {{ update(catalog: {attributions?: Array<{label:string,url:string|null}>}): void, dispose(): void }}
 */
export function mountAttribution(container) {
  const el = document.createElement('div');
  el.className = 'dioramap-attr-overlay';
  el.setAttribute('aria-label', 'Attributions');
  container.appendChild(el);

  // Initial render: powered-by only. The data half lights up when the
  // first CATALOG_RESPONSE arrives (typically <1 s after SDK init).
  let lastAttrs = null;
  render(/* attributions */ null);

  // Compact-mode toggle by container width. ResizeObserver keeps the
  // class in sync as the embedder resizes — fullscreen, sidebar collapse,
  // mobile rotation all flow through here.
  const ro = (typeof ResizeObserver !== 'undefined') ? new ResizeObserver((entries) => {
    for (const entry of entries) {
      const w = entry.contentRect?.width ?? container.clientWidth;
      el.classList.toggle('dioramap-attr-compact', w < 480);
    }
  }) : null;
  ro?.observe(container);
  // Run the compact check once now in case ResizeObserver fires only on change.
  el.classList.toggle('dioramap-attr-compact', container.clientWidth < 480);

  function render(attributions) {
    el.innerHTML = '';

    const powered = document.createElement('span');
    const prefix = document.createElement('span');
    prefix.className = 'dioramap-attr-prefix';
    prefix.textContent = 'Powered by ';
    powered.appendChild(prefix);
    const dioramapLink = document.createElement('a');
    dioramapLink.href = 'https://dioramap.com';
    dioramapLink.target = '_blank';
    dioramapLink.rel = 'noopener noreferrer';
    dioramapLink.textContent = 'dioramap.com';
    powered.appendChild(dioramapLink);
    el.appendChild(powered);

    if (!attributions || attributions.length === 0) return;

    const sep = document.createElement('span');
    sep.className = 'dioramap-attr-sep';
    sep.textContent = '·';
    el.appendChild(sep);

    const dataWrap = document.createElement('span');
    const dataPrefix = document.createElement('span');
    dataPrefix.className = 'dioramap-attr-prefix';
    dataPrefix.textContent = 'Data: ';
    dataWrap.appendChild(dataPrefix);

    attributions.forEach((a, i) => {
      if (i > 0) {
        const comma = document.createTextNode(', ');
        dataWrap.appendChild(comma);
      }
      const label = document.createElement('span');
      label.className = 'dioramap-attr-data-label';
      if (a.url) {
        const link = document.createElement('a');
        link.href = a.url;
        link.target = '_blank';
        link.rel = 'noopener noreferrer';
        link.textContent = a.label;
        label.appendChild(link);
      } else {
        label.textContent = a.label;
      }
      dataWrap.appendChild(label);
    });
    el.appendChild(dataWrap);
  }

  return {
    /**
     * Update the data half from a CATALOG_RESPONSE payload.
     * Idempotent: a no-change call is cheap (no DOM rewrite).
     */
    update(catalog) {
      const attrs = (catalog && Array.isArray(catalog.attributions))
        ? catalog.attributions : null;
      if (attributionsEqual(attrs, lastAttrs)) return;
      lastAttrs = attrs;
      render(attrs);
    },
    dispose() {
      ro?.disconnect();
      el.remove();
    },
  };
}
