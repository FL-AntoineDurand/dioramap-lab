// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// MapLibre map initialization with IGN Plan + IGN ortho basemaps (Géoplateforme WMTS).

import * as maplibregl from 'maplibre-gl';
import { installMaplibreWorker } from './maplibre-worker.js';

// Plan IGN v2 street map. Replaces direct use of tile.openstreetmap.org, whose
// tile usage policy does not allow heavy or embedded use. The `basemap`
// option keeps accepting 'osm' as an alias of 'plan' for compatibility.
const PLAN_SOURCE = {
  type: 'raster',
  tiles: [
    'https://data.geopf.fr/wmts?SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0&LAYER=GEOGRAPHICALGRIDSYSTEMS.PLANIGNV2&STYLE=normal&FORMAT=image/png&TILEMATRIXSET=PM&TILEMATRIX={z}&TILEROW={y}&TILECOL={x}',
  ],
  tileSize: 256,
  maxzoom: 19,
  attribution: '&copy; IGN &mdash; Plan IGN',
};

const ORTHO_SOURCE = {
  type: 'raster',
  tiles: [
    'https://data.geopf.fr/wmts?SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0&LAYER=ORTHOIMAGERY.ORTHOPHOTOS&STYLE=normal&FORMAT=image/jpeg&TILEMATRIXSET=PM&TILEMATRIX={z}&TILEROW={y}&TILECOL={x}',
  ],
  tileSize: 256,
  maxzoom: 18,
  attribution: '&copy; IGN &mdash; BD ORTHO',
};

/**
 * Create the MapLibre map inside the given container div.
 * Returns { map, mapDiv, setBasemap }.
 */
export function createMap(containerEl, { center, zoom, basemap }) {
  const mapDiv = document.createElement('div');
  mapDiv.className = 'dioramap-picker-map';
  containerEl.style.position = 'relative';
  containerEl.appendChild(mapDiv);

  const { sources, layers } = buildStyle(basemap);

  installMaplibreWorker();
  const map = new maplibregl.Map({
    container: mapDiv,
    style: {
      version: 8,
      sources,
      layers,
    },
    center,
    zoom,
  });

  map.addControl(new maplibregl.NavigationControl({ showCompass: true }), 'top-right');
  map.addControl(new maplibregl.ScaleControl(), 'bottom-left');

  function setBasemap(mode) {
    const { sources: s, layers: l } = buildStyle(mode);
    map.setStyle({
      version: 8,
      sources: s,
      layers: l,
    });
  }

  return { map, mapDiv, setBasemap };
}

function buildStyle(basemap) {
  const sources = {};
  const layers = [];

  if (basemap === 'plan' || basemap === 'osm' || basemap === 'both') {
    sources.plan = PLAN_SOURCE;
    layers.push({ id: 'plan-tiles', type: 'raster', source: 'plan', minzoom: 0, maxzoom: 22 });
  }
  if (basemap === 'ortho' || basemap === 'both') {
    sources.ortho = ORTHO_SOURCE;
    layers.push({ id: 'ortho-tiles', type: 'raster', source: 'ortho', minzoom: 0, maxzoom: 20 });
  }

  return { sources, layers };
}
