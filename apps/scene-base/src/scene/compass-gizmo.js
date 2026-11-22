// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Marine compass HUD — full-degree graduation, bottom-right of the 3D
// viewport. Replaces the four-cardinal rose from the legacy bench UI (§4.4
// of PLAN-LAB-UI-PORT.md).
//
// Two halves:
//   (a) ticks — draw once per pipeline, rotate with camera yaw.
//       Cardinals (N/E/S/W) are emphasized; N is a red arrow.
//   (b) labels — drawn once, repositioned every frame via a CSS translate
//       to follow the rotated ring while keeping the text upright.
//
// The disc tilts with camera pitch (CSS `rotateX`): when the camera is
// looking straight down, the compass lies flat; when the camera is level,
// the compass stands on edge. Angles are clamped so it remains readable.
//
// PERF NOTE (2026-05-12): per-frame updates use CSS `transform` on SVG
// elements rather than SVG `transform` / `x` / `y` attributes. SVG
// attribute writes invalidate Firefox WebRender's picture cache on every
// change → forces full re-rasterisation of the entire page each frame
// (profiled 86% picture-cache invalidation rate, 54 ms average composite).
// CSS transforms on elements with `will-change: transform` stay on the
// GPU compositor layer and don't touch the picture cache.

const SVG_NS = 'http://www.w3.org/2000/svg';
const SIZE = 120; // 25% larger than the original L3 compass (was 96).

// Class names: `.dioramap-compass-*`. Rules live in:
//   - viewer-sdk/src/styles.css → bundled into dist/dioramap-viewer.css for
//     embedders' <link>;
//   - lab-ui/src/layout/style.css → in-repo lab-ui import.
// No runtime <style> injection (CSP-friendly, no `unsafe-inline` needed).

const CX = SIZE / 2;
const CY = SIZE / 2;
const R_OUTER = SIZE / 2 - 2;
const R_MINOR = R_OUTER - 3;
const R_MAJOR = R_OUTER - 6;
const R_LABEL = R_OUTER - 14;
const MAX_TILT_DEG = 70;

export class MarineCompass {
  constructor(host, { viewer, enabled = false }) {
    this.host = host;
    this.viewer = viewer;
    this.enabled = enabled;
    this._labels = []; // { el, deg }
    this._build();
    this._tick = this._tick.bind(this);
    this._raf = null;
    this.setEnabled(enabled);
  }

  setEnabled(on) {
    this.enabled = !!on;
    this.root.hidden = !this.enabled;
    if (this.enabled && this._raf == null) {
      this._raf = requestAnimationFrame(this._tick);
    } else if (!this.enabled && this._raf != null) {
      cancelAnimationFrame(this._raf);
      this._raf = null;
    }
  }

  _build() {
    const root = document.createElement('div');
    root.className = 'dioramap-compass';

    // The outer .dioramap-compass element handles CSS 3D perspective + pitch tilt.
    // An inner wrapper holds the SVG; we rotateX that wrapper.
    const tilt = document.createElement('div');
    tilt.className = 'dioramap-compass-tilt';
    const svg = document.createElementNS(SVG_NS, 'svg');
    svg.setAttribute('viewBox', `0 0 ${SIZE} ${SIZE}`);
    svg.setAttribute('width', String(SIZE));
    svg.setAttribute('height', String(SIZE));

    // Bg disc.
    const bg = document.createElementNS(SVG_NS, 'circle');
    bg.setAttribute('cx', String(CX));
    bg.setAttribute('cy', String(CY));
    bg.setAttribute('r', String(SIZE / 2 - 1));
    bg.setAttribute('class', 'dioramap-compass-bg');
    svg.appendChild(bg);

    // --- Ring (ticks only) — rotates with camera yaw ---
    const ring = document.createElementNS(SVG_NS, 'g');
    ring.setAttribute('class', 'dioramap-compass-ring');
    svg.appendChild(ring);
    for (let deg = 0; deg < 360; deg++) {
      const isCardinal = deg % 90 === 0;
      const isMajor = deg % 10 === 0;
      const theta = (deg - 90) * Math.PI / 180;
      const rIn = isCardinal ? R_MAJOR - 2 : (isMajor ? R_MAJOR : R_MINOR);
      const x1 = CX + R_OUTER * Math.cos(theta);
      const y1 = CY + R_OUTER * Math.sin(theta);
      const x2 = CX + rIn * Math.cos(theta);
      const y2 = CY + rIn * Math.sin(theta);
      const tick = document.createElementNS(SVG_NS, 'line');
      tick.setAttribute('x1', x1.toFixed(2));
      tick.setAttribute('y1', y1.toFixed(2));
      tick.setAttribute('x2', x2.toFixed(2));
      tick.setAttribute('y2', y2.toFixed(2));
      tick.setAttribute('class',
        isCardinal ? 'dioramap-compass-cardinal' : (isMajor ? 'dioramap-compass-major' : 'dioramap-compass-minor'));
      ring.appendChild(tick);
    }
    // Red North arrow — triangle pointing from just inside R_OUTER toward
    // the center at 0° (up). Sits inside the rotating ring so it always
    // points to world-North once the ring rotates.
    const nArrow = document.createElementNS(SVG_NS, 'path');
    nArrow.setAttribute('d',
      `M ${CX} ${CY - R_OUTER + 1} L ${CX - 5} ${CY - R_OUTER + 11} ` +
      `L ${CX + 5} ${CY - R_OUTER + 11} Z`);
    nArrow.setAttribute('class', 'dioramap-compass-n-arrow');
    ring.appendChild(nArrow);

    // --- Labels — separate group so we counter-rotate each one every frame.
    // Keep only the four cardinals and four intercardinals (every 45°); at
    // this compass diameter (96px) every-10° numerics overlap. The ticks
    // themselves are still drawn every degree so the ring reads as a full
    // graduation.
    //
    // Each label is placed STATICALLY at its heading=0 position (computed
    // once below). Per frame, _tick applies a CSS `transform: translate(...)`
    // to move it to the current-heading position. The text never rotates,
    // so it stays upright. Why static SVG positions + CSS translate (not
    // SVG x/y attributes per frame): see PERF NOTE in the header comment.
    const labels = document.createElementNS(SVG_NS, 'g');
    labels.setAttribute('class', 'dioramap-compass-labels');
    svg.appendChild(labels);
    const CARDINAL = { 0: 'N', 90: 'E', 180: 'S', 270: 'W' };
    const LABEL_DEGS = [0, 45, 90, 135, 180, 225, 270, 315];
    for (const deg of LABEL_DEGS) {
      // Heading=0 position on the label ring.
      const theta0 = (deg - 90) * Math.PI / 180;
      const lx0 = CX + R_LABEL * Math.cos(theta0);
      const ly0 = CY + R_LABEL * Math.sin(theta0);
      const el = document.createElementNS(SVG_NS, 'text');
      el.textContent = CARDINAL[deg] ?? String(deg);
      el.setAttribute('class',
        CARDINAL[deg]
          ? (deg === 0 ? 'dioramap-compass-north-label' : 'dioramap-compass-cardinal-label')
          : 'dioramap-compass-label');
      el.setAttribute('text-anchor', 'middle');
      el.setAttribute('dominant-baseline', 'middle');
      // STATIC position via SVG x/y — set once, never updated again.
      el.setAttribute('x', lx0.toFixed(2));
      el.setAttribute('y', ly0.toFixed(2));
      el.style.willChange = 'transform';
      labels.appendChild(el);
      this._labels.push({ el, deg, lx0, ly0 });
    }

    tilt.appendChild(svg);
    root.appendChild(tilt);
    // Readout sits OUTSIDE the tilt wrapper so it always faces the user
    // flat, regardless of camera pitch. It's also the one element sized
    // 25% larger than the original compass text per L3.1 feedback.
    const readout = document.createElement('div');
    readout.className = 'dioramap-compass-readout';
    root.appendChild(readout);
    this.host.appendChild(root);
    this.root = root;
    this.$tilt = tilt;
    this.$ring = ring;
    this.$readout = readout;
  }

  _tick() {
    this._raf = requestAnimationFrame(this._tick);
    const heading = this._computeHeading();       // 0..360
    const pitchDeg = this._computePitchDeg();     // 0 (down) .. 90 (horizon)

    // Ring rotates via CSS transform (NOT SVG `transform` attribute).
    // transform-origin set once on the element via inline style at first
    // tick so we don't depend on CSS-class plumbing here.
    if (this._ringOriginSet !== true) {
      this.$ring.style.transformOrigin = `${CX}px ${CY}px`;
      this.$ring.style.willChange = 'transform';
      this._ringOriginSet = true;
    }
    this.$ring.style.transform = `rotate(${-heading.toFixed(2)}deg)`;

    // Labels: move each label from its static (lx0, ly0) heading=0
    // position to the current-heading position via CSS translate. No
    // SVG attribute writes; no rotation applied to the text → still upright.
    for (const { el, deg, lx0, ly0 } of this._labels) {
      const effectiveDeg = deg - heading;
      const theta = (effectiveDeg - 90) * Math.PI / 180;
      const lx = CX + R_LABEL * Math.cos(theta);
      const ly = CY + R_LABEL * Math.sin(theta);
      el.style.transform = `translate(${(lx - lx0).toFixed(2)}px, ${(ly - ly0).toFixed(2)}px)`;
    }

    // Readout: only update DOM when the integer value changes — avoids a
    // text-node mutation on every frame when the value's stable.
    const h = Math.round(((heading % 360) + 360) % 360);
    if (h !== this._lastReadout) {
      this._lastReadout = h;
      this.$readout.textContent = `${h}°`;
    }

    // Pitch: CSS transform on a `will-change: transform` element — cheap.
    const tilt = Math.min(MAX_TILT_DEG, Math.max(0, pitchDeg));
    this.$tilt.style.transform = `rotateX(${tilt.toFixed(1)}deg)`;
  }

  // Compass heading: angle from +Y (North) clockwise, in degrees.
  // Derived from the XY projection of (target - camera).
  _computeHeading() {
    const cam = this.viewer.activeCamera;
    const target = this.viewer.controls?.target;
    if (!cam || !target) return 0;
    const dx = target.x - cam.position.x;
    const dy = target.y - cam.position.y;
    if (Math.abs(dx) < 1e-6 && Math.abs(dy) < 1e-6) return 0;
    return Math.atan2(dx, dy) * 180 / Math.PI;
  }

  // Compass tilt derived from the camera's angle below the horizon.
  // Returns 0 when looking straight down (compass flat, viewed from above),
  // 90 when looking along the horizon (edge-on; clamped by MAX_TILT_DEG).
  _computePitchDeg() {
    const cam = this.viewer.activeCamera;
    const target = this.viewer.controls?.target;
    if (!cam || !target) return 0;
    const dx = target.x - cam.position.x;
    const dy = target.y - cam.position.y;
    const dz = target.z - cam.position.z;
    const horiz = Math.hypot(dx, dy);
    // Angle from the horizontal plane (+ up), in degrees. `dz` is negative
    // when the camera looks down at the scene (its z is above the target's).
    // We want 0° when straight down, 90° horizontal.
    if (horiz < 1e-6 && Math.abs(dz) < 1e-6) return 0;
    const depression = Math.atan2(-dz, horiz) * 180 / Math.PI; // 0 horizontal, 90 down
    return Math.max(0, 90 - depression);
  }

  dispose() {
    this.setEnabled(false);
    this.root?.remove();
    this.root = null;
    this.$tilt = null;
    this.$ring = null;
    this.$readout = null;
    this._labels = [];
    this.viewer = null;
  }
}
