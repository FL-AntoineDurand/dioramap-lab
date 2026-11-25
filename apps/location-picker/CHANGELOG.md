# @dioramap/location-picker changelog

Versioning: SDK semver, in lockstep with `@dioramap/viewer-sdk` for
embedder-visible breaking changes.

## 0.10.0 — 2026-11-25

Version aligned with `@dioramap/viewer-sdk` (both bundles are published
together under `sdk/v<version>/`).

- **MapLibre GL JS 4.7 → 6** (security: `DOM.sanitize` XSS bypass advisory,
  MapLibre ≤ 6.4.0). MapLibre ≥ 5 loads its web worker from a separate file,
  which cannot work for a single cross-origin bundle; the worker is now
  embedded and handed to MapLibre as a `blob:` URL (`src/maplibre-worker.js`),
  as MapLibre 4 did internally. Embedding pages still need `worker-src blob:`
  in a strict CSP. Bundle size: ~460 kB gzip (was ~220 kB).
- **Build:** Vite 6. The stylesheet is now emitted as
  `dist/dioramap-location-picker.css` (was `dist/style.css`).
- **Address search failures are visible.** A failed BAN geocoder request
  (HTTP error or network) used to be silently ignored; the dropdown now says
  the search is unavailable (French or English, from the page language) and
  "No address found" on empty results.
- **New `init({ onError })`**: called with an `Error` whose `code` is
  `GEOCODER_FAILED` and `detail` holds the request URL and query.

## 0.9.0 — not released (folded into 0.10.0)

**Additive.**

- **Address search failures are visible.** A failed BAN geocoder request
  (HTTP error or network) used to be silently ignored; the dropdown now says
  the search is unavailable (French or English, from the page language) and
  "No address found" on empty results.
- **New `init({ onError })`**: called with an `Error` whose `code` is
  `GEOCODER_FAILED` and `detail` holds the request URL and query.

## 0.8.0 — 2026-05-06

**Breaking** in lockstep with viewer-sdk 0.8.0 (CSP / theming).

- **Externalised CSS.** All 9 `el.style.cssText = '…'` strings (in `map.js`,
  `search.js`, `circle.js`) and the 5 JS-set hover/disabled colours (in
  `circle.js`, `search.js`) lifted into a single `dioramap-location-picker.css`
  Vite-extracted from `src/styles.css`. Embedders' `<link>` for the CSS was
  already required since 0.7.x, so no NEW `<link>` is needed — but the
  bundle is now loaded from `src/styles.css` instead of being implicit
  in the JS.
- **`'unsafe-inline'` no longer required** in embedders' Content Security
  Policy. Bundle has zero `el.style.cssText`, zero `setAttribute('style', …)`,
  zero `<style>` injection. CSS-property assignments (`el.style.display = …`,
  `el.style.cursor = …`) are CSP-safe per CSP3 (style property, not
  attribute) and are kept where dynamic toggling is needed.
- **`--dioramap-picker-*` theming variables** (public API, stable within
  0.8.x). 13 CSS custom properties exposed at `:root` — confirm-button
  bg/hover/disabled, surface bg/shadow, dropdown shadow, divider, hover
  bg, hint banner colours, banner bg/text, radius, font. Picker uses its
  own variable namespace so it can coexist with the viewer's
  `--dioramap-*` on the same page without collision.
- **`<button :disabled>` selector** replaces the JS background swap on
  the confirm button. The `.dioramap-picker-confirm-btn:disabled` and
  `:hover:not(:disabled)` rules in the bundled CSS own the visual state;
  JS only sets `confirmBtn.disabled = !zoomOk`. Embedders can theme the
  disabled state via `--dioramap-picker-disabled-bg` instead of
  monkey-patching JS.
- **`.dioramap-picker-*` class names** (public API). Scoped to avoid
  embedder-style clashes; classes from `maplibregl-*` are NOT part of the
  surface (MapLibre owns those). Documented in
  `landing/docs/api/location-picker/#styling`.
