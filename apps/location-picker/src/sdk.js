// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// DioramapLocationPicker SDK — lightweight map coordinate picker.
//
// Self-contained: MapLibre-GL is bundled into the output; callers load
// one JS file and one CSS file and they're done. Works inside the
// lab UI, inside the customer-facing viewer SDK, or as a plain <script>
// on a static page.
//
// Usage — ES module:
//   import { init } from '@dioramap/location-picker';
//   import '@dioramap/location-picker/dist/style.css';
//   const picker = await init({
//     container: '#picker',
//     radius: 75,
//     onConfirm: ({ lat, lon, radius }) => console.log(lat, lon, radius),
//   });
//
// Usage — global <script> on a customer page:
//   <link rel="stylesheet" href=".../style.css">
//   <script type="module">
//     import { init } from '.../dioramap-location-picker.es.js';
//     init({ container: '#picker', … });
//   </script>

import { createMap } from './map.js';
import { createSearch } from './search.js';
import { createCircleCursor } from './circle.js';
// Pull MapLibre's CSS into the build output so consumers only need to
// load ONE stylesheet (dioramap-location-picker.css) instead of MapLibre's
// separately. Own picker styles live in ./styles.css and are extracted
// to the same bundled CSS by Vite — no runtime <style> injection or
// .style.cssText assignment, so embedders can drop 'unsafe-inline' from
// their CSP. See SDK-THEMING.md for the --dioramap-picker-* surface.
import 'maplibre-gl/dist/maplibre-gl.css';
import './styles.css';

/**
 * @param {Object} config
 * @param {string|HTMLElement} config.container      CSS selector or HTMLElement (required)
 * @param {[number,number]} [config.center]          [lon, lat], default France center
 * @param {number} [config.zoom]                     default 6
 * @param {number} [config.radius]                   circle radius in meters, default 75 (not user-adjustable)
 * @param {'osm'|'ortho'|'both'} [config.basemap]    default 'both'
 * @param {number} [config.minConfirmZoom]            min zoom level to allow confirm, default 17
 * @param {({lat,lon,radius}) => void} [config.onPick]     fired on each map click
 * @param {({lat,lon,radius}) => void} [config.onConfirm]  fired on confirm button
 * @param {(picker) => void} [config.onReady]
 * @returns {Promise<Object>}
 */
async function init(config) {
  const {
    container,
    center = [2.2, 46.6],
    zoom = 6,
    radius = 75,
    basemap = 'both',
    minConfirmZoom = 17,
    onPick,
    onConfirm,
    onReady,
  } = config;

  const el = typeof container === 'string' ? document.querySelector(container) : container;
  if (!el) throw new Error(`DioramapLocationPicker: container "${container}" not found`);

  // Mount map.
  const { map, setBasemap } = createMap(el, { center, zoom, basemap });

  // Address search — mount immediately, no need to wait for tiles.
  const search = createSearch(el, {
    onSelect: ({ lat, lon }) => {
      map.flyTo({ center: [lon, lat], zoom: 17, duration: 1200 });
      map.once('moveend', () => {
        circle.setCenter(lon, lat);
        if (onPick) onPick({ lat, lon, radius });
      });
    },
  });

  // Circle cursor (radius is fixed, set by config).
  const circle = createCircleCursor(map, el, {
    radius,
    minConfirmZoom,
    onPick,
  });

  // Wire confirm callback.
  circle.onConfirm((data) => {
    if (onConfirm) onConfirm(data);
  });

  // ── Public API ──────────────────────────────────────────────────────

  const sdk = {
    getSelection: () => circle.getState(),
    setCenter: ([lon, lat]) => {
      map.flyTo({ center: [lon, lat], zoom: Math.max(map.getZoom(), 15), duration: 800 });
      circle.setCenter(lon, lat);
    },
    flyTo: ([lon, lat], z) => {
      map.flyTo({ center: [lon, lat], zoom: z ?? zoom, duration: 800 });
    },
    setBasemap,
    dispose: () => {
      circle.dispose();
      map.remove();
      el.innerHTML = '';
    },
  };

  if (onReady) onReady(sdk);
  return sdk;
}

export { init };
export default { init };
if (typeof window !== 'undefined') {
  window.DioramapLocationPicker = { init };
}
