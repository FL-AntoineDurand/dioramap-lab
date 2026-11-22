// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Sun control — drives a Three.js DirectionalLight.
//
// Two input paths feed the same DirectionalLight:
//
//   - setFromSunDirection({sun_dx, sun_dy, sun_dz})
//       One-shot at mount, fed by the v3 `sun-direction` artifact (output
//       of `find-best-sun-time`). Used to align the scene's sun with the
//       direction the deshadowed ortho was baked against. After init, the
//       deshadowed texture has no baked shadows so the sun moves freely.
//
//   - setFromDate({year, month, day, hour, minute}, lat, lon)
//       Per slider movement. Computes sun position client-side via SunCalc
//       at the mount's lat/lon. Pure browser math, no WS traffic.
//
// Both paths converge on _applyDirection(dx, dy, dz) which moves the
// DirectionalLight (and its target) so the scene's sun shines from that
// direction. The vector convention matches the v3 convention: x=east,
// y=north, z=up, vector points TOWARD the sun.

import SunCalc from 'suncalc';

// Distance from scene origin where we park the DirectionalLight. Doesn't
// affect lighting math (DirectionalLight is parallel rays); just needs to
// be far enough that the light's helper visualization (if ever shown) is
// visibly outside the scene. Inherited from legacy SDK conventions.
const SUN_DISTANCE = 500;

export class SunControl extends EventTarget {
  /**
   * @param {THREE.DirectionalLight} light       — the scene's sun light
   * @param {{lat:number, lon:number}} location  — scene's geographic anchor
   */
  constructor(light, { lat, lon }) {
    super();
    this.light = light;
    this.lat = lat;
    this.lon = lon;
    this.state = {
      // Date the slider currently represents. Defaults to a sensible noon.
      date: { year: 2024, month: 6, day: 15, hour: 12, minute: 0 },
      // Direction unit vector (x=east, y=north, z=up; toward the sun).
      dir: { dx: 0, dy: 0, dz: 1 },
      enabled: true,
    };
  }

  // Initialize from the v3 sun-direction artifact (one-shot at mount).
  setFromSunDirection({ sun_dx, sun_dy, sun_dz }) {
    this.state.dir = { dx: sun_dx, dy: sun_dy, dz: sun_dz };
    this._applyDirection();
    this._emit('sun-changed');
  }

  // Move the sun via SunCalc client-side (slider path).
  setFromDate({ year, month, day, hour, minute }) {
    this.state.date = { year, month, day, hour, minute };
    const utc = new Date(Date.UTC(year, month - 1, day, hour, minute, 0));
    const pos = SunCalc.getPosition(utc, this.lat, this.lon);
    // SunCalc azimuth: 0 = south, +ve toward west. Convert to L93 axes:
    //   east  = -sin(azimuth) * cos(altitude)
    //   north = -cos(azimuth) * cos(altitude)   (south=0 → -1)
    //   up    =  sin(altitude)
    // Same convention as the C port at v3/steps/shadow/sun-position.c.
    const cosA = Math.cos(pos.altitude);
    this.state.dir = {
      dx: -Math.sin(pos.azimuth) * cosA,
      dy: -Math.cos(pos.azimuth) * cosA,
      dz:  Math.sin(pos.altitude),
    };
    this._applyDirection();
    this._emit('sun-changed');
  }

  setEnabled(enabled) {
    this.state.enabled = !!enabled;
    if (this.light) this.light.visible = this.state.enabled;
    this._emit('sun-changed');
  }

  // Apply the current direction to the Three.js light.
  _applyDirection() {
    if (!this.light) return;
    const { dx, dy, dz } = this.state.dir;
    // DirectionalLight shines from light.position toward light.target.position.
    // Place the light along the +dir vector so it shines back toward origin.
    this.light.position.set(dx * SUN_DISTANCE, dy * SUN_DISTANCE, dz * SUN_DISTANCE);
    if (this.light.target) {
      this.light.target.position.set(0, 0, 0);
      this.light.target.updateMatrixWorld?.();
    }
  }

  _emit(type) {
    this.dispatchEvent(new CustomEvent(type, { detail: { ...this.state } }));
  }
}
