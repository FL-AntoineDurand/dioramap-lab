// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Radius circle cursor — follows mouse, locks on click.
// Confirm button is positioned next to the picked point on the map (as a MapLibre marker).
// Confirm is only enabled when zoom >= minConfirmZoom.

import * as maplibregl from 'maplibre-gl';

const CIRCLE_SEGMENTS = 64;

/**
 * Generate a GeoJSON Polygon circle (no turf dependency).
 */
function geoCircle(centerLon, centerLat, radiusMeters) {
  const coords = [];
  const earthRadius = 6378137;
  const latRad = (centerLat * Math.PI) / 180;

  for (let i = 0; i <= CIRCLE_SEGMENTS; i++) {
    const angle = (i / CIRCLE_SEGMENTS) * 2 * Math.PI;
    const dLat = (radiusMeters * Math.cos(angle)) / earthRadius;
    const dLon = (radiusMeters * Math.sin(angle)) / (earthRadius * Math.cos(latRad));
    coords.push([
      centerLon + (dLon * 180) / Math.PI,
      centerLat + (dLat * 180) / Math.PI,
    ]);
  }

  return {
    type: 'Feature',
    geometry: { type: 'Polygon', coordinates: [coords] },
  };
}

/**
 * Attach circle cursor + click-to-pick behaviour to the map.
 * Radius is fixed (set by SDK config, not user-adjustable).
 * Confirm button appears as a map marker next to the picked point,
 * but is disabled until zoom >= minConfirmZoom.
 *
 * Returns { setCenter, unlock, getState, onConfirm, dispose }.
 */
export function createCircleCursor(map, containerEl, { radius, minConfirmZoom, onPick }) {
  const currentRadius = radius;
  let locked = false;
  let pickedCenter = null;
  let onConfirmCb = null;

  const emptyGeo = { type: 'FeatureCollection', features: [] };

  // ── Map layers ────────────────────────────────────────────────────
  function addLayers() {
    if (map.getSource('picker-circle')) return;

    map.addSource('picker-circle', { type: 'geojson', data: emptyGeo });
    map.addLayer({
      id: 'picker-circle-fill',
      type: 'fill',
      source: 'picker-circle',
      paint: { 'fill-color': '#3b82f6', 'fill-opacity': 0.15 },
    });
    map.addLayer({
      id: 'picker-circle-line',
      type: 'line',
      source: 'picker-circle',
      paint: {
        'line-color': '#3b82f6',
        'line-width': 2,
        'line-dasharray': [4, 3],
      },
    });

    map.addSource('picker-center', { type: 'geojson', data: emptyGeo });
    map.addLayer({
      id: 'picker-center-dot',
      type: 'circle',
      source: 'picker-center',
      paint: {
        'circle-radius': 5,
        'circle-color': '#3b82f6',
        'circle-stroke-width': 2,
        'circle-stroke-color': '#ffffff',
      },
    });
  }

  function updateCircle(lon, lat) {
    const src = map.getSource('picker-circle');
    const centerSrc = map.getSource('picker-center');
    if (!src) return;

    src.setData(geoCircle(lon, lat, currentRadius));
    centerSrc.setData({
      type: 'FeatureCollection',
      features: [{ type: 'Feature', geometry: { type: 'Point', coordinates: [lon, lat] } }],
    });
  }

  // ── Mouse / click handlers ────────────────────────────────────────
  function onMouseMove(e) {
    if (locked) return;
    updateCircle(e.lngLat.lng, e.lngLat.lat);
  }

  function onClick(e) {
    const { lng, lat } = e.lngLat;
    locked = true;
    pickedCenter = { lon: lng, lat };
    updateCircle(lng, lat);
    showConfirmMarker(lng, lat);
    if (onPick) onPick({ lat, lon: lng, radius: currentRadius });
  }

  const canvas = map.getCanvas();
  const origCursor = canvas.style.cursor;
  canvas.style.cursor = 'crosshair';

  map.on('mousemove', onMouseMove);
  map.on('click', onClick);

  if (map.isStyleLoaded()) addLayers();
  else map.on('load', addLayers);

  // ── Confirm button as a MapLibre marker ───────────────────────────
  let confirmMarker = null;

  // Build the marker DOM element. Visuals (hover/disabled colours) are
  // owned by the bundled CSS (.dioramap-picker-confirm-btn rules) so
  // embedders can theme via --dioramap-picker-confirm-bg etc.
  const confirmEl = document.createElement('div');
  confirmEl.className = 'dioramap-picker-confirm-wrap';

  const confirmBtn = document.createElement('button');
  confirmBtn.textContent = 'Confirm location';
  confirmBtn.className = 'dioramap-picker-confirm-btn';

  const zoomHint = document.createElement('div');
  zoomHint.className = 'dioramap-picker-zoom-hint';

  confirmEl.appendChild(confirmBtn);
  confirmEl.appendChild(zoomHint);

  confirmBtn.addEventListener('click', (e) => {
    e.stopPropagation();
    if (confirmBtn.disabled) return;
    if (pickedCenter && onConfirmCb) {
      onConfirmCb({ lat: pickedCenter.lat, lon: pickedCenter.lon, radius: currentRadius });
    }
  });

  function showConfirmMarker(lon, lat) {
    if (confirmMarker) confirmMarker.remove();
    confirmMarker = new maplibregl.Marker({ element: confirmEl, anchor: 'top', offset: [0, 12] })
      .setLngLat([lon, lat])
      .addTo(map);
    updateConfirmState();
  }

  function updateConfirmState() {
    const zoomOk = map.getZoom() >= minConfirmZoom;
    // The :disabled CSS rules (.dioramap-picker-confirm-btn:disabled)
    // own the disabled/hover backgrounds + cursor; just toggle the
    // boolean.
    confirmBtn.disabled = !zoomOk;
    zoomHint.textContent = zoomOk ? '' : '↑ Zoom in to confirm this area';
    zoomHint.style.display = zoomOk ? 'none' : 'block';
  }

  // Re-evaluate on every zoom change.
  map.on('zoom', updateConfirmState);

  // ── Zoom hint banner (shown at bottom when no point picked yet) ───
  const banner = document.createElement('div');
  banner.textContent = 'Click on the map to pick a location';
  banner.className = 'dioramap-picker-banner';
  containerEl.appendChild(banner);

  // Hide banner once a point is picked.
  const origOnPick = onPick;
  onPick = (data) => {
    banner.style.display = 'none';
    if (origOnPick) origOnPick(data);
  };

  // Re-bind onClick with updated onPick reference.
  map.off('click', onClick);
  onClick = (e) => {
    const { lng, lat } = e.lngLat;
    locked = true;
    pickedCenter = { lon: lng, lat };
    updateCircle(lng, lat);
    showConfirmMarker(lng, lat);
    if (onPick) onPick({ lat, lon: lng, radius: currentRadius });
  };
  map.on('click', onClick);

  return {
    setCenter(lon, lat) {
      locked = true;
      pickedCenter = { lon, lat };
      updateCircle(lon, lat);
      showConfirmMarker(lon, lat);
      banner.style.display = 'none';
    },
    unlock() {
      locked = false;
      pickedCenter = null;
      if (confirmMarker) { confirmMarker.remove(); confirmMarker = null; }
      banner.style.display = 'block';
    },
    getState() {
      return pickedCenter ? { lat: pickedCenter.lat, lon: pickedCenter.lon, radius: currentRadius } : null;
    },
    onConfirm(cb) { onConfirmCb = cb; },
    dispose() {
      map.off('mousemove', onMouseMove);
      map.off('click', onClick);
      map.off('zoom', updateConfirmState);
      canvas.style.cursor = origCursor;
      if (confirmMarker) confirmMarker.remove();
      banner.remove();
    },
  };
}
