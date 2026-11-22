// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Map-picker wrapper (L7).
//
// Thin glue around @dioramap/location-picker: creates a modal overlay, mounts
// the picker, and resolves a Promise with {lat, lon, radius} when the user
// confirms (or null if they dismiss via Esc / click outside). Each open()
// call disposes the picker after resolving so the modal can be reused.

import { init as initPicker } from '@dioramap/location-picker/src/sdk.js';

export class MapPicker {
  constructor() {
    this._openPromise = null;
    this._resolve = null;
    this._picker = null;
    this._modal = null;
  }

  // Show the picker. Returns a Promise resolving to {lat, lon, radius} on
  // confirm, or null if dismissed. `defaults` seeds the initial map center
  // (lat/lon in degrees) + radius in metres.
  open(defaults = {}) {
    if (this._openPromise) return this._openPromise;
    this._openPromise = new Promise((resolve) => { this._resolve = resolve; });
    this._buildModal(defaults);
    return this._openPromise;
  }

  _buildModal({ lat, lon, radius }) {
    const modal = document.createElement('div');
    modal.className = 'map-picker-modal';
    modal.innerHTML = `
      <div class="map-picker-card">
        <header class="map-picker-header">
          <span class="map-picker-title">Pick a location</span>
          <button type="button" class="map-picker-close" title="cancel (Esc)">×</button>
        </header>
        <div class="map-picker-host"></div>
        <footer class="map-picker-footer mono">
          click a point · zoom in (≥17) to enable confirm
        </footer>
      </div>
    `;
    document.body.appendChild(modal);
    this._modal = modal;

    const host = modal.querySelector('.map-picker-host');
    const closeBtn = modal.querySelector('.map-picker-close');
    const readout = modal.querySelector('.map-picker-footer');

    const cancel = () => this._close(null);
    closeBtn.addEventListener('click', cancel);
    modal.addEventListener('click', (ev) => {
      if (ev.target === modal) cancel();
    });
    const keyHandler = (ev) => {
      if (ev.key === 'Escape') { ev.preventDefault(); cancel(); }
    };
    document.addEventListener('keydown', keyHandler);
    this._keyHandler = keyHandler;

    // Seed center: Paris default if no valid seed.
    const seedLat = Number.isFinite(lat) ? lat : 48.8566;
    const seedLon = Number.isFinite(lon) ? lon : 2.3522;
    const seedRadius = Number.isFinite(radius) ? radius : 75;

    initPicker({
      container: host,
      center: [seedLon, seedLat],
      zoom: Number.isFinite(lat) ? 17 : 6,
      radius: seedRadius,
      basemap: 'both',
      onPick: ({ lat, lon, radius }) => {
        readout.textContent =
          `pick: ${lat.toFixed(5)}, ${lon.toFixed(5)} · r=${radius}m — zoom ≥17 to confirm`;
      },
      onConfirm: (payload) => this._close(payload),
    }).then((sdk) => {
      this._picker = sdk;
    });
  }

  _close(result) {
    document.removeEventListener('keydown', this._keyHandler);
    if (this._picker) {
      try { this._picker.dispose(); } catch { /* noop */ }
      this._picker = null;
    }
    if (this._modal) {
      this._modal.remove();
      this._modal = null;
    }
    const resolve = this._resolve;
    this._resolve = null;
    this._openPromise = null;
    resolve?.(result);
  }
}
