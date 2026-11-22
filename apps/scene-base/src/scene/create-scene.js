// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Three.js 3D viewer — L2 slice.
//
// Scope shipped today:
//   - scene, renderer, perspective + orthographic cameras, OrbitControls
//   - axis helper (full marine compass gizmo deferred to L3 polish)
//   - declarative layer API: addLayer(id, spec) / removeLayer(id)
//   - primitive support: { kind: 'points', positions, colors?, properties? }
//
// Deferred to later L2/L3:
//   - click inspector (per-primitive `properties` dict display)
//   - plane / edges / cylinders / spheres / poly-surface / ground-raster primitives
//   - per-layer color legends + histograms
//   - point-size slider (exposed as API here; DOM control added in layout glue)

import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { EffectComposer } from 'three/addons/postprocessing/EffectComposer.js';
import { RenderPass } from 'three/addons/postprocessing/RenderPass.js';
import { createEDLPass } from './postprocess/edl.js';
import { MaskRenderPass, EDL_LAYER } from './postprocess/mask-render.js';

// Public helper: mark a mesh as opt-in for EDL. Without this call, the
// mesh's pixels are not in the EDL mask and the EDL post-process leaves
// them alone — i.e. EDL is OFF for that mesh by default. Inverts the
// historical semantics where EDL applied to everything and individual
// materials had to "noEDL"-stomp alpha to 0 to opt out.
//
// Usage:
//   import { setEdlOptIn } from '@dioramap/scene-base';
//   setEdlOptIn(myMesh);
//
// Safe to call on any Object3D (Mesh, InstancedMesh, Points, Group…).
// Effect is invisible if the Viewer was constructed with edl:false
// (composer never runs the mask pass) or if no mesh in the scene is
// opted in (composer is bypassed — direct renderer.render).
//
// Implementation: enable the EDL_LAYER bit on the mesh's layer mask.
// MaskRenderPass sets camera.layers.set(EDL_LAYER) and re-renders the
// scene with a flat-red overrideMaterial; only opt-in meshes pass the
// layer test, so the mask target ends up red at opt-in pixels and
// black elsewhere. EDL pass reads that mask and gates per-pixel.
export function setEdlOptIn(mesh) {
  if (!mesh || typeof mesh.layers?.enable !== 'function') {
    console.warn('[viewer] setEdlOptIn: expected Object3D, got', mesh);
    return;
  }
  mesh.layers.enable(EDL_LAYER);
}

// Realloc a BufferAttribute's underlying Float32Array to a larger capacity,
// preserving existing contents. Three.js doesn't expose a direct "resize"
// helper for BufferAttribute, so we replace the attribute in place.
function growAttr(geom, name, newCapacity, itemSize) {
  const old = geom.getAttribute(name);
  const bigger = new Float32Array(newCapacity * itemSize);
  bigger.set(old.array);
  const attr = new THREE.BufferAttribute(bigger, itemSize);
  attr.setUsage(THREE.DynamicDrawUsage);
  attr.needsUpdate = true;
  geom.setAttribute(name, attr);
}

export class Viewer {
  /**
   * @param {HTMLElement} container
   * @param {Object}  [opts]
   * @param {boolean} [opts.devHelpers=false]  Add the AxesHelper + GridHelper
   *   debug aids to the scene. Lab-ui passes true; the SDK leaves it false
   *   so production embeds don't show the orange/red/green axis cross or
   *   the grey grid plane.
   * @param {number}  [opts.maxFps=0]  Render-loop frame cap. Default 0
   *   = uncapped; render once per requestAnimationFrame tick (~display
   *   refresh rate). With render-on-demand (the default), idle frames
   *   are skipped automatically, so a cap is rarely useful — only set
   *   it if you want to throttle motion frames specifically (e.g. for
   *   low-power devices). Picking a cap close to but below the display
   *   refresh (e.g. 50 on a 60 Hz screen) causes rAF jitter to drop
   *   every-other frame and halves the effective rate; pick well below
   *   or leave at 0.
   * @param {boolean} [opts.edl=true]  Enable Eye-Dome Lighting post-process.
   *   Adds a faint dark halo around every closer surface — dramatically
   *   improves the sense of solidity in disc-based point clouds. Set to
   *   `false` to skip the EffectComposer wrapping and render plain.
   */
  constructor(container, { devHelpers = false, maxFps = 0, edl = true, targetRadius = 75 } = {}) {
    this.container = container;
    this.layers = new Map();        // id → { object, cleanup }
    // Default in WORLD UNITS (metres in L93). sizeAttenuation=true on
    // PointsMaterial scales screen size by 1/depth, so points get
    // larger close to the camera and smaller far — like any other 3D
    // geometry. 0.1 m ≈ typical lidar point spacing.
    this.pointSize = 0.1;
    this._devHelpers = !!devHelpers;
    this._minFrameMs = (maxFps && maxFps > 0) ? (1000 / maxFps) : 0;
    this._edlEnabled = !!edl;
    // Render-on-demand. `_dirty=true` means the next eligible rAF will
    // run a full composite; otherwise we skip the render (the rAF still
    // ticks so OrbitControls damping has a chance to advance, but no
    // GPU work happens). invalidate() flips the flag from anywhere.
    // We start dirty so the first frame renders.
    this._dirty = true;
    // Tile radius used to frame the camera at construction. SDK passes
    // its `cfg.radius` through; falls back to 75 matching the SDK default.
    this._targetRadius = targetRadius;
    // Shared Lambert93 origin for all 3D layers in this scene. The first
    // renderer that has a canonical L93 anchor (usually the point cloud's
    // centroid) sets it via setSceneOrigin(). Subsequent renderers that
    // want to place things in L93 (ground rasters, edges, planes…) read it
    // via getSceneOrigin() and offset their coordinates so everything lives
    // in the same local frame with WebGL-F32-safe magnitudes.
    this.sceneOrigin = null;
    this._initScene();
    this._installResizeObserver();
  }

  setSceneOrigin(o) { this.sceneOrigin = { x: o.x, y: o.y, z: o.z }; }
  getSceneOrigin() { return this.sceneOrigin; }
  clearSceneOrigin() { this.sceneOrigin = null; }

  // ---------------------------------------------------------------------- //
  // Scene
  // ---------------------------------------------------------------------- //

  _initScene() {
    const { clientWidth: w, clientHeight: h } = this.container;

    this.scene = new THREE.Scene();
    // Sky gradient: blue at zenith → near-white at horizon (ported from
    // legacy SDK's scene.js). Gives a real-feeling outdoor look once a
    // terrain mesh and point cloud are in the scene; for empty scenes
    // (e.g. lab-ui inspector with one artifact) it just looks like a
    // bright background. Set as scene.background to act as the equirect
    // env reflection too.
    const SKY_TOP = new THREE.Color(0x5588cc);
    const SKY_HORIZON = new THREE.Color(0xc8daea);
    const skyCanvas = new OffscreenCanvas(1, 256);
    const skyCtx = skyCanvas.getContext('2d');
    const grad = skyCtx.createLinearGradient(0, 0, 0, 256);
    grad.addColorStop(0, `#${SKY_TOP.getHexString()}`);
    grad.addColorStop(0.55, `#${SKY_HORIZON.getHexString()}`);
    grad.addColorStop(1, `#${SKY_HORIZON.getHexString()}`);
    skyCtx.fillStyle = grad;
    skyCtx.fillRect(0, 0, 1, 256);
    this._skyTexture = new THREE.CanvasTexture(skyCanvas);
    this._skyTexture.mapping = THREE.EquirectangularReflectionMapping;
    this._skyTexture.colorSpace = THREE.SRGBColorSpace;
    this._darkBackground = new THREE.Color(0x0d1117);
    this._skyEnabled = true;
    this.scene.background = this._skyTexture;
    // Three.js' EquirectangularReflectionMapping treats +Y as the pole
    // (Y-up convention). Our scene is Z-up (Lambert-93 / L93 metres), so
    // without rotation the bright zenith band lies along ±Y in world
    // space and visually appears as a horizontal stripe at one compass
    // direction. Rotate the background by 90° around X to put the
    // texture pole on +Z.
    this.scene.backgroundRotation = new THREE.Euler(Math.PI / 2, 0, 0);

    // Fog — subtle distance haze. Near=500 keeps everything inside
    // the surround tile crisp; far=1200 puts only the very far edge
    // (and beyond) into faint sky-coloured haze.
    this._fogColor = new THREE.Color(0xc8daea);  // matches the sky horizon
    this._fogNear = 500;
    this._fogFar = 1200;
    this._fogEnabled = true;
    this.scene.fog = new THREE.Fog(this._fogColor, this._fogNear, this._fogFar);

    this.perspCamera = new THREE.PerspectiveCamera(55, w / h || 1, 0.1, 50000);
    this.perspCamera.up.set(0, 0, 1);

    // ── Initial framing ─────────────────────────────────────────────
    // Spec: tilt 35° down toward the tile centre (origin); distance set
    // so the FULL disc (a circle on Z=0) is visible in the viewport on
    // whichever axis is the constraint.
    //
    // Geometry (camera at distance D, tilt below horizontal):
    //   - The disc's left/right edges (perpendicular to camera-origin
    //     bearing) sit at forward-distance = D, perpendicular offset = r.
    //     Horizontal half-angle on screen: atan(r / D).
    //   - The disc's far edge sits at forward-distance D+r·cos(tilt),
    //     perpendicular offset r·sin(tilt). Half-angle (above centre):
    //     atan(r·sin(tilt) / (D + r·cos(tilt)))
    //   - The disc's NEAR edge sits at forward-distance D−r·cos(tilt)
    //     (smaller!), perpendicular offset r·sin(tilt). Half-angle (below
    //     centre): atan(r·sin(tilt) / (D − r·cos(tilt))) — this is the
    //     LARGER vertical angle because the denominator shrinks. The near
    //     edge is the binding case for vertical fit.
    //
    // Solving θ_near = vFovHalf for D:
    //   D = r × (sin(tilt) / tan(vFovHalf) + cos(tilt))
    //
    // Solving |θ_horiz| = hFovHalf:
    //   D = r / tan(hFovHalf)
    //
    // Take max + apply margin. Crossover at aspect ≈ 1 for tilt 35°/vFOV 55°.
    //
    // Camera placed on the (1,1) horizontal bearing at the right tilt:
    //   horizDist = D × cos(tilt)
    //   z         = D × sin(tilt)
    //   x = y     = horizDist / √2
    const initialAspect = (w / h) || 1;
    const TILT_DEG = 35;            // fixed downward tilt
    const MARGIN = 1.10;            // 10 % padding around the tile
    const tiltRad = TILT_DEG * Math.PI / 180;
    const vFovHalfRad = (55 / 2) * Math.PI / 180;
    const hFovHalfRad = Math.atan(Math.tan(vFovHalfRad) * initialAspect);
    const dHoriz = (this._targetRadius * MARGIN) / Math.tan(hFovHalfRad);
    const dVert  = (this._targetRadius * MARGIN) * (
                     Math.sin(tiltRad) / Math.tan(vFovHalfRad) + Math.cos(tiltRad)
                   );
    const distance = Math.max(dHoriz, dVert);
    const constraint = dVert > dHoriz ? 'vertical' : 'horizontal';
    const horizDist = distance * Math.cos(tiltRad);
    const camZ = distance * Math.sin(tiltRad);
    const camX = horizDist / Math.SQRT2;
    const camY = horizDist / Math.SQRT2;
    this.perspCamera.position.set(camX, camY, camZ);
    this.perspCamera.lookAt(0, 0, 0);

    // Ortho mirrors perspective camera position; half-extent matches the
    // padded tile radius so it shows the same scene area at zoom-1.
    const orthoHalf = this._targetRadius * MARGIN;
    this.orthoCamera = new THREE.OrthographicCamera(-orthoHalf, orthoHalf, orthoHalf, -orthoHalf, 0.1, 50000);
    this.orthoCamera.position.set(camX, camY, camZ);
    this.orthoCamera.up.set(0, 0, 1);
    this.orthoCamera.lookAt(0, 0, 0);

    this.activeCamera = this.perspCamera;

    // No stencil attachment on the canvas — the EDL opt-in mechanism
    // is layer-based (see setEdlOptIn) and the mask comes from a
    // second scene render via MaskRenderPass, not a stencil blit.
    this.renderer = new THREE.WebGLRenderer({
      antialias: true, alpha: false,
    });
    // Cap pixel ratio at 2. High-DPR mobile (3-4×) on a 400×800 viewport
    // would otherwise allocate a 1600×3200 framebuffer ≈ 5 megapixel —
    // unnecessary for visual quality of point clouds + meshes at this
    // scale, and a real perf hit on integrated GPUs. Two is the sweet
    // spot every WebGL app converges on (Sketchfab, Mapbox GL, etc).
    this.renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
    // setSize(w, h, true) — third arg = updateStyle. Without it, three.js
    // sets the canvas WIDTH/HEIGHT attributes (= framebuffer size in
    // device pixels) but does NOT set canvas.style.{width,height}. The
    // browser then renders the canvas at the attribute size in CSS pixels
    // — i.e. twice the container width when DPR=2. The visible CSS canvas
    // overflows the container; only the left half is visible, the scene
    // origin lands at viewport-right.
    this.renderer.setSize(w, h, true);
    // Shadow map enabled by default. PCFSoft = nice soft edges, works on
    // mobile. Disc/canopy meshes already declare castShadow + receiveShadow;
    // they also ship custom 25%-scale depth materials so the inflated
    // visible hex doesn't cast a chunky over-sized shadow.
    this.renderer.shadowMap.enabled = true;
    this.renderer.shadowMap.type = THREE.PCFSoftShadowMap;
    this.container.appendChild(this.renderer.domElement);

    this.controls = new OrbitControls(this.activeCamera, this.renderer.domElement);
    // Damping (inertia) defaults OFF. With damping the camera continues
    // moving for several frames after mouse release; with render-on-demand
    // those coast frames keep the GPU busy too. Set explicitly via
    // setDamping(factor) if you want it back.
    this.controls.enableDamping = false;
    this.controls.dampingFactor = 0;
    // Tile centroid is at origin (lidar viz recenters to centroid via
    // recenter()), so origin = orbit point. Default OrbitControls target
    // is (0,0,0) but be explicit for clarity + survive future changes.
    this.controls.target.set(0, 0, 0);
    this.controls.update();
    // Render-on-demand dirty trigger from user interaction.
    this.controls.addEventListener('change', () => this.invalidate());

    // ── EffectComposer + EDL post-process ───────────────────────────
    //
    // Pass chain:
    //   1. RenderPass     scene → composer target A (color + depth).
    //                     All layers (camera.layers default mask covers
    //                     layer 0 ∪ EDL_LAYER, so opt-in meshes render
    //                     normally here alongside everything else).
    //   2. MaskRenderPass scene → mask target M, with
    //                     camera.layers.set(EDL_LAYER) +
    //                     scene.overrideMaterial = red. Only opt-in
    //                     meshes (those with EDL_LAYER enabled) pass
    //                     the layer test; M.r = 1.0 at opt-in pixels.
    //   3. EDLPass        renderToScreen. Samples color + depth from A
    //                     and mask from M. Per-pixel: mask<0.5 ⇒ raw
    //                     scene colour; else ⇒ EDL response applied.
    //
    // No `gl.blitFramebuffer` anywhere, no WebGL extensions required.
    // Cost vs single-render: one extra scene render of only the opt-in
    // subset (typically a single InstancedMesh — disc-other), with the
    // cheapest possible material (flat red MeshBasicMaterial).
    if (this._edlEnabled) {
      const dpr = this.renderer.getPixelRatio();
      const rtW = Math.floor(w * dpr);
      const rtH = Math.floor(h * dpr);
      // Depth texture — feeds EDL's neighbour-log-depth math. 24-bit
      // depth, no stencil (the opt-in mechanism is now layer-based, not
      // stencil-based). NEAREST sampling is required (LINEAR is
      // undefined for depth textures).
      const dt = new THREE.DepthTexture(rtW, rtH);
      dt.format = THREE.DepthFormat;
      dt.type   = THREE.UnsignedIntType;
      dt.minFilter = THREE.NearestFilter;
      dt.magFilter = THREE.NearestFilter;
      this._composerTarget = new THREE.WebGLRenderTarget(rtW, rtH, {
        minFilter: THREE.LinearFilter,
        magFilter: THREE.LinearFilter,
        format: THREE.RGBAFormat,
        depthBuffer: true,
        stencilBuffer: false,
        depthTexture: dt,
        samples: 0,
      });

      // Mask render target — MaskRenderPass clears to black and renders
      // only opt-in meshes (via camera.layers + scene.overrideMaterial)
      // as solid red. EDL pass reads .r to gate the effect per pixel.
      // Depth buffer attached so opt-in meshes z-fight correctly among
      // themselves (not shared with composer target — see mask-render.js
      // for the trade-off).
      this._maskTarget = new THREE.WebGLRenderTarget(rtW, rtH, {
        format: THREE.RGBAFormat,
        type: THREE.UnsignedByteType,
        depthBuffer: true,
        stencilBuffer: false,
        minFilter: THREE.NearestFilter,
        magFilter: THREE.NearestFilter,
      });

      this._composer = new EffectComposer(this.renderer, this._composerTarget);
      this._composer.setPixelRatio(dpr);
      this._composer.setSize(w, h);
      this._renderPass = new RenderPass(this.scene, this.activeCamera);
      this._maskPass = new MaskRenderPass(this.scene,
                                          () => this.activeCamera,
                                          this._maskTarget);
      this._edlPass = createEDLPass();
      this._edlPass.uniforms.tDepth.value = dt;
      this._edlPass.uniforms.tOptInMask.value = this._maskTarget.texture;
      this._edlPass.uniforms.uResolution.value.set(rtW, rtH);
      this._edlPass.uniforms.uNear.value = this.activeCamera.near;
      this._edlPass.uniforms.uFar.value = this.activeCamera.far;
      // Defaults from the legacy SDK — strength=1.0, radius=1.4 px,
      // distance fade 150..300 m. Tuned against the disc/canopy/
      // surround content; produces a clean silhouette halo without moire.
      this._edlPass.uniforms.uEdlFadeNear.value = 150;
      this._edlPass.uniforms.uEdlFadeFar.value = 300;
      this._edlPass.renderToScreen = true;
      this._composer.addPass(this._renderPass);
      this._composer.addPass(this._maskPass);
      this._composer.addPass(this._edlPass);
    }

    // Dev helpers — axes cross + grey grid. Always created (cheap), but
    // visibility defaults to the `devHelpers` constructor opt (lab-ui
    // true, SDK false). Togglable later via setAxesEnabled / setGridEnabled.
    this._axesHelper = new THREE.AxesHelper(20);
    this._axesHelper.name = '__axes';
    this._axesHelper.visible = !!this._devHelpers;
    this.scene.add(this._axesHelper);

    this._gridHelper = new THREE.GridHelper(200, 20, 0x30363d, 0x21262d);
    this._gridHelper.rotation.x = Math.PI / 2; // XY plane (Lambert93)
    this._gridHelper.name = '__grid';
    this._gridHelper.visible = !!this._devHelpers;
    this.scene.add(this._gridHelper);

    // Three-light rig ported from legacy SDK's scene.js. HemisphereLight
    // gives soft sky-vs-ground tint, AmbientLight ensures non-lit faces
    // aren't pure black, DirectionalLight is the sun (its direction is
    // mutable from outside via setSunDirection — used by the slider in
    // the future SDK and by the sun-direction artifact in the auto-fit
    // case). Defaults aim slightly off-overhead so meshes get visible
    // shading even before a sun-direction artifact lands.
    const hemi = new THREE.HemisphereLight(0xffffeb, 0x404060, 0.85);
    const amb = new THREE.AmbientLight(0xffffff, 0.7);
    this.sun = new THREE.DirectionalLight(0xfff4e0, 4.5);
    this.sun.position.set(200, 200, 300);
    // Shadow caster setup. The orthographic shadow camera covers a
    // ~600 m square centred at the scene origin — comfortably bigger
    // than the lidar disc (75 m) + surround padding (200 m), so cast
    // shadows reach across the surround terrain too. Bias values pulled
    // from the legacy SDK to suppress acne/peter-panning on the disc
    // shader's tilted instances.
    this.sun.castShadow = true;
    this.sun.shadow.mapSize.set(2048, 2048);
    this.sun.shadow.camera.left = -300;
    this.sun.shadow.camera.right = 300;
    this.sun.shadow.camera.top = 300;
    this.sun.shadow.camera.bottom = -300;
    this.sun.shadow.camera.near = 0.5;
    this.sun.shadow.camera.far = 1500;
    this.sun.shadow.bias = -0.0005;
    this.sun.shadow.normalBias = 0.02;
    // DirectionalLight aims at light.target by default — make sure the
    // target is in the scene graph so its world matrix updates as the
    // sun's position rotates with the SunControl slider.
    this.scene.add(this.sun.target);
    this.scene.add(hemi, amb, this.sun);

    this._lastRenderMs = 0;
    this._disposed = false;
    this._rafId = 0;
    this._animate = (t) => this._frame(t);
    this._rafId = requestAnimationFrame(this._animate);
  }

  // Scan the scene for any Object3D with the EDL_LAYER bit enabled
  // (set by setEdlOptIn). Cached — _edlNeededCache is cleared by
  // addLayer / removeLayer / ensureBuilt / disposeBuilt, so a stale
  // cached `false` can't survive a real opt-in landing.
  _sceneNeedsEdl() {
    if (this._edlNeededCache !== null && this._edlNeededCache !== undefined) {
      return this._edlNeededCache;
    }
    let needs = false;
    this.scene.traverse((o) => {
      if (needs) return;
      // layers.mask is a bitfield; test bit EDL_LAYER.
      if (o.layers && (o.layers.mask & (1 << EDL_LAYER)) !== 0) {
        needs = true;
      }
    });
    this._edlNeededCache = needs;
    return needs;
  }

  _invalidateEdlCache() { this._edlNeededCache = null; }

  // Mark the scene dirty — the next eligible rAF tick will render.
  // Callers: any code that mutates scene state (visibility toggles,
  // sun direction, layer add/remove, material edits, etc.). The
  // Viewer auto-calls this on its own mutating methods + on
  // OrbitControls 'change' + on resize; external code that mutates
  // object properties directly should call invalidate() afterwards.
  invalidate() {
    this._dirty = true;
  }

  _frame(t) {
    if (this._disposed) return;
    this._rafId = requestAnimationFrame(this._animate);
    // Always advance OrbitControls — damping needs every rAF tick to
    // interpolate smoothly. Returns true while still updating; we
    // treat that as a dirt source so motion frames render.
    const controlsActive = this.controls.update();
    if (controlsActive) this._dirty = true;
    if (!this._dirty) return;
    // renderer.info accumulates draw calls + triangles ACROSS all
    // passes in this frame. With autoReset=true (default), the next
    // renderer.render() inside the composer would zero it again and
    // we'd only see the last pass's stats. We disable autoReset and
    // reset manually here, so perf monitors read the whole-frame
    // totals after composer.render() returns.
    if (this.renderer.info.autoReset !== false) {
      this.renderer.info.autoReset = false;
    }
    this.renderer.info.reset();
    // Frame cap (`maxFps` constructor option). Skip the render until
    // enough wall time has passed; _dirty stays true so the next
    // eligible frame catches it.
    if (this._minFrameMs > 0 && (t - this._lastRenderMs) < this._minFrameMs) {
      return;
    }
    this._lastRenderMs = t;
    this._dirty = false;
    // Bypass the EDL composer entirely when no mesh in the scene has
    // opted in via setEdlOptIn. Saves both the MaskRenderPass second
    // scene render and the EDL fullscreen quad. Detection is a
    // scene.traverse looking for the EDL_LAYER bit on Object3D.layers;
    // cached + invalidated on layer add/remove and ensureBuilt /
    // disposeBuilt below.
    if (this._composer && this._sceneNeedsEdl()) {
      // Keep the EDL pass' camera-dependent uniforms in sync — these
      // change every time setProjection swaps perspective ↔ ortho or
      // the user zooms/pans (near/far doesn't move on a perspective
      // unless explicitly retuned, but cheap to update every frame).
      if (this._edlPass) {
        this._edlPass.uniforms.uNear.value = this.activeCamera.near;
        this._edlPass.uniforms.uFar.value = this.activeCamera.far;
      }
      this._composer.render();
    } else {
      this.renderer.render(this.scene, this.activeCamera);
    }
    // Notify subscribers that a real render just completed — used by
    // stats / perf-monitor overlays that want to track actual frame
    // rate (vs rAF rate). Skipped frames don't fire this.
    if (this._onFrameRendered) this._onFrameRendered();
  }

  /**
   * Register a callback fired once per *rendered* frame (skipped frames
   * don't trigger it). Use this for stats.js or other perf gauges that
   * should reflect the actual render rate. Pass `null` to clear.
   */
  setFrameCallback(fn) {
    this._onFrameRendered = (typeof fn === 'function') ? fn : null;
  }

  _installResizeObserver() {
    const ro = new ResizeObserver(() => this._resize());
    ro.observe(this.container);
    this._ro = ro;
  }

  _resize() {
    const w = this.container.clientWidth;
    const h = this.container.clientHeight;
    if (!w || !h) return;
    this.invalidate();
    this.renderer.setSize(w, h, true);
    const aspect = w / h;
    this.perspCamera.aspect = aspect;
    this.perspCamera.updateProjectionMatrix();
    // For the ortho camera, scale half-extents to preserve aspect.
    const half = Math.abs(this.orthoCamera.top);
    this.orthoCamera.left = -half * aspect;
    this.orthoCamera.right = half * aspect;
    this.orthoCamera.updateProjectionMatrix();
    // EffectComposer + EDL pass also need resizing. Use the same
    // pixel-ratio scaling as on construction so the depth texture
    // resolution matches the colour buffer.
    if (this._composer) {
      const dpr = this.renderer.getPixelRatio();
      const rtW = Math.floor(w * dpr);
      const rtH = Math.floor(h * dpr);
      this._composer.setSize(w, h);
      this._composerTarget?.setSize?.(rtW, rtH);
      this._maskTarget?.setSize?.(rtW, rtH);
      if (this._edlPass) this._edlPass.uniforms.uResolution.value.set(rtW, rtH);
    }
  }

  // ---------------------------------------------------------------------- //
  // Camera + view controls
  // ---------------------------------------------------------------------- //

  setProjection(mode) {
    const was = this.activeCamera;
    this.activeCamera = mode === 'ortho' ? this.orthoCamera : this.perspCamera;
    this.activeCamera.position.copy(was.position);
    this.activeCamera.quaternion.copy(was.quaternion);
    this.activeCamera.up.copy(was.up);
    this.controls.object = this.activeCamera;
    this.controls.update();
    this._resize();
    this.invalidate();
  }

  setPointSize(size) {
    this.pointSize = size;
    for (const { object } of this.layers.values()) {
      if (object && object.isPoints && object.material) {
        object.material.size = size;
        object.material.needsUpdate = true;
      }
    }
    this.invalidate();
  }

  setSkyEnabled(enabled) {
    this._skyEnabled = !!enabled;
    this.scene.background = this._skyEnabled ? this._skyTexture : this._darkBackground;
    this.invalidate();
  }

  setFogEnabled(enabled) {
    this._fogEnabled = !!enabled;
    this.scene.fog = this._fogEnabled
      ? new THREE.Fog(this._fogColor, this._fogNear, this._fogFar)
      : null;
    this.invalidate();
  }

  setAxesEnabled(enabled) {
    if (this._axesHelper) this._axesHelper.visible = !!enabled;
    this.invalidate();
  }

  setGridEnabled(enabled) {
    if (this._gridHelper) this._gridHelper.visible = !!enabled;
    this.invalidate();
  }

  /**
   * Configure OrbitControls inertia.
   * @param {number} factor 0 = damping off (motion stops on mouse release).
   *                        0.01–0.3 = enabled, smaller = longer coast,
   *                        larger = tighter stop. Three.js default 0.05;
   *                        our default 0.08.
   */
  setDamping(factor) {
    const f = Number(factor);
    if (!Number.isFinite(f) || f <= 0) {
      this.controls.enableDamping = false;
      this.controls.dampingFactor = 0;
    } else {
      this.controls.enableDamping = true;
      this.controls.dampingFactor = Math.min(1, f);
    }
    this.invalidate();
  }

  // Fit both cameras + controls target to a bounding sphere.
  frameBox(box) {
    const sphere = box.getBoundingSphere(new THREE.Sphere());
    if (!isFinite(sphere.radius) || sphere.radius <= 0) return;
    this.controls.target.copy(sphere.center);
    const dist = sphere.radius * 2.5;
    const dir = new THREE.Vector3(1, 1, 1).normalize();
    const pos = sphere.center.clone().addScaledVector(dir, dist);
    this.perspCamera.position.copy(pos);
    this.orthoCamera.position.copy(pos);
    // Scale ortho half-extent to sphere radius * 1.5 (gives margin on either side).
    const half = sphere.radius * 1.5;
    const aspect = this.container.clientWidth / this.container.clientHeight || 1;
    this.orthoCamera.top = half;
    this.orthoCamera.bottom = -half;
    this.orthoCamera.left = -half * aspect;
    this.orthoCamera.right = half * aspect;
    this.orthoCamera.updateProjectionMatrix();
    this.perspCamera.lookAt(sphere.center);
    this.orthoCamera.lookAt(sphere.center);
    this.controls.update();
  }

  // ---------------------------------------------------------------------- //
  // Layer API — consumed by renderer modules via ctx.render3D
  // ---------------------------------------------------------------------- //

  addLayer(id, spec) {
    this.removeLayer(id);
    // Lazy-build path: spec.build is a thunk returning { object, dispose? }.
    // Nothing is scene-added yet — the RenderContext wires this layer's
    // visibility toggle to ensureBuilt / disposeBuilt below. Toggling on
    // for the first time pays the construction cost; toggling off disposes.
    if (typeof spec.build === 'function') {
      this.layers.set(id, { builder: spec.build, object: null, cleanup: null });
      return () => this.removeLayer(id);
    }
    let object = null;
    let cleanup = null;
    // Composer pre-built path (PLAN-VIEWER-SDK-PORT §4.2): if the spec
    // already carries a THREE.Object3D, just scene-add it. The viz module
    // owns construction + disposal; we own scene insertion + visibility.
    // This is the path scene-base/viz/*.viz.js modules use.
    if (spec.object && (spec.object.isObject3D || spec.object.isMesh ||
                        spec.object.isPoints || spec.object.isLine ||
                        spec.object.isLineSegments || spec.object.isGroup)) {
      object = spec.object;
    } else {
      switch (spec.kind) {
        case 'points':
          [object, cleanup] = this._buildPoints(spec);
          break;
        case 'ground-raster':
          [object, cleanup] = this._buildGroundRaster(spec);
          break;
        default:
          console.warn('[viewer] unknown primitive kind:', spec.kind);
          return () => {};
      }
    }
    if (object) this.scene.add(object);
    this.layers.set(id, { object, cleanup });
    this._invalidateEdlCache();
    this.invalidate();
    return () => this.removeLayer(id);
  }

  // Construct a lazy layer's object on demand (idempotent — no-op if
  // already built or if this isn't a lazy layer). Returns the object
  // so the caller can attach it to sceneState.visibility for tri-state
  // cascades. Disposes traverse-deep to catch geom/mat/textures inside
  // groups; viz authors can supply an extra `dispose` callback in the
  // build return for non-traversable resources (e.g. sprite textures
  // not held on a material).
  ensureBuilt(id) {
    const e = this.layers.get(id);
    if (!e || !e.builder || e.object) return e?.object ?? null;
    const built = e.builder();
    if (!built) return null;
    e.object = built.object ?? null;
    e.cleanup = built.dispose ?? null;
    if (e.object) this.scene.add(e.object);
    this._invalidateEdlCache();
    this.invalidate();
    return e.object;
  }

  disposeBuilt(id) {
    const e = this.layers.get(id);
    if (!e || !e.object) return;
    if (e.object) {
      this.scene.remove(e.object);
      this._disposeDeep(e.object);
    }
    e.cleanup?.();
    e.object = null;
    e.cleanup = null;
    this._invalidateEdlCache();
    this.invalidate();
  }

  _disposeDeep(obj) {
    obj.traverse?.((o) => {
      o.geometry?.dispose?.();
      const m = o.material;
      if (!m) return;
      if (Array.isArray(m)) for (const sub of m) sub.dispose?.();
      else m.dispose?.();
    });
    // Top-level object may itself carry geom/material that the traverse
    // walks (Mesh etc.); the traverse calls dispose on those too.
  }

  removeLayer(id) {
    const entry = this.layers.get(id);
    if (!entry) return;
    if (entry.object) {
      this.scene.remove(entry.object);
      this._disposeDeep(entry.object);
    }
    entry.cleanup?.();
    this.layers.delete(id);
    this._invalidateEdlCache();
    this.invalidate();
  }

  clearAll() {
    for (const id of Array.from(this.layers.keys())) this.removeLayer(id);
    this.clearSceneOrigin();
    this.invalidate();
  }

  /**
   * Tear down everything this Viewer owns: the rAF render loop, the
   * ResizeObserver, registered layers (via clearAll), the post-process
   * composer + its render target, OrbitControls, the sky texture, the
   * WebGLRenderer + its DOM canvas. Idempotent — safe to call twice.
   *
   * What this DOES NOT clean up: lights / cameras (no GPU resources to
   * dispose on those), nor anything the consumer attached to `this.scene`
   * outside the layer system. Consumers are expected to clean their own
   * scene.add()'d objects via the `cleanup` returned by addLayer or
   * SceneState's clearSubtree.
   */
  dispose() {
    if (this._disposed) return;
    this._disposed = true;

    if (this._rafId) cancelAnimationFrame(this._rafId);
    this._rafId = 0;

    this._ro?.disconnect();
    this._ro = null;

    this.clearAll();

    if (this._composer) {
      // EffectComposer doesn't expose a dispose API; manually dispose its
      // passes' shader materials + the shared render targets.
      this._renderPass?.material?.dispose?.();
      this._maskPass?.dispose?.();
      this._edlPass?.material?.dispose?.();
      this._composerTarget?.dispose?.();
      this._maskTarget?.dispose?.();
      this._composer = null;
      this._composerTarget = null;
      this._maskTarget = null;
      this._renderPass = null;
      this._maskPass = null;
      this._edlPass = null;
    }

    this.controls?.dispose?.();
    this.controls = null;

    this._skyTexture?.dispose?.();
    this._skyTexture = null;
    this.scene.background = null;
    this.scene.environment = null;

    if (this.renderer) {
      this.renderer.dispose();
      this.renderer.forceContextLoss?.();
      const canvas = this.renderer.domElement;
      if (canvas?.parentElement) canvas.parentElement.removeChild(canvas);
    }
    this.renderer = null;
  }

  // ---------------------------------------------------------------------- //
  // Primitive builders
  // ---------------------------------------------------------------------- //

  // spec = {
  //   kind: 'points',
  //   positions: Float32Array (interleaved x,y,z — one vec3 per point),
  //   colors?: Float32Array (interleaved r,g,b, 0..1) OR a flat hex string,
  //   size?: number (pixels),
  //   // Growable mode (B9 streaming): if `capacity` is provided, the
  //   // BufferAttribute is preallocated to `capacity` points, usage is
  //   // DynamicDrawUsage, and the initial drawRange is set to the count
  //   // implied by `positions.length / 3` (0 for an empty stream start).
  //   // Call viewer.growPoints(layerId, addedPositions [, addedColors]) per
  //   // chunk to append points and bump the drawRange.
  //   capacity?: number,
  // }
  _buildPoints(spec) {
    const geom = new THREE.BufferGeometry();
    const hasColors = spec.colors instanceof Float32Array;
    const growable = Number.isFinite(spec.capacity);
    if (growable) {
      const capF32 = new Float32Array(spec.capacity * 3);
      capF32.set(spec.positions ?? new Float32Array(0));
      const posAttr = new THREE.BufferAttribute(capF32, 3);
      posAttr.setUsage(THREE.DynamicDrawUsage);
      geom.setAttribute('position', posAttr);
      if (hasColors) {
        const capColors = new Float32Array(spec.capacity * 3);
        capColors.set(spec.colors);
        const colorAttr = new THREE.BufferAttribute(capColors, 3);
        colorAttr.setUsage(THREE.DynamicDrawUsage);
        geom.setAttribute('color', colorAttr);
      }
      const initialCount = (spec.positions?.length ?? 0) / 3;
      geom.setDrawRange(0, initialCount);
      geom.__grownCount = initialCount;  // our own counter — drawRange.count survives dispose
      geom.__grownCapacity = spec.capacity;
      geom.__hasColors = hasColors;
    } else {
      geom.setAttribute('position', new THREE.BufferAttribute(spec.positions, 3));
      if (hasColors) {
        geom.setAttribute('color', new THREE.BufferAttribute(spec.colors, 3));
      }
    }
    const mat = new THREE.PointsMaterial({
      size: spec.size ?? this.pointSize,
      sizeAttenuation: true,
      vertexColors: hasColors,
      color: hasColors ? 0xffffff : (spec.color ?? 0xe6edf3),
    });
    const pts = new THREE.Points(geom, mat);
    return [pts, null];
  }

  // Append points into an existing growable `points` layer. addedPositions
  // is a Float32Array of length 3*N (interleaved x,y,z). addedColors, if
  // provided, must match the capacity mode the layer was created with.
  // Triggers a realloc of the underlying BufferAttribute if the capacity
  // is exceeded (rare under sane preallocation). Bumps drawRange.
  growPoints(id, addedPositions, addedColors) {
    const entry = this.layers.get(id);
    if (!entry || !entry.object || !entry.object.isPoints) return;
    const geom = entry.object.geometry;
    if (!Number.isFinite(geom.__grownCount)) {
      console.warn('[viewer] growPoints on non-growable layer', id);
      return;
    }
    const addN = addedPositions.length / 3;
    const oldCount = geom.__grownCount;
    const newCount = oldCount + addN;

    // Expand capacity geometrically if the new count exceeds it.
    if (newCount > geom.__grownCapacity) {
      const newCap = Math.max(newCount, Math.floor(geom.__grownCapacity * 1.6));
      growAttr(geom, 'position', newCap, 3);
      if (geom.__hasColors) growAttr(geom, 'color', newCap, 3);
      geom.__grownCapacity = newCap;
    }

    const posAttr = geom.getAttribute('position');
    posAttr.array.set(addedPositions, oldCount * 3);
    posAttr.addUpdateRange(oldCount * 3, addN * 3);
    posAttr.needsUpdate = true;

    if (addedColors && geom.__hasColors) {
      const colorAttr = geom.getAttribute('color');
      colorAttr.array.set(addedColors, oldCount * 3);
      colorAttr.addUpdateRange(oldCount * 3, addN * 3);
      colorAttr.needsUpdate = true;
    }

    geom.__grownCount = newCount;
    geom.setDrawRange(0, newCount);
    geom.computeBoundingSphere();
    this.invalidate();
  }

  // spec = {
  //   kind: 'ground-raster',
  //   cols, rows,           // raster dimensions
  //   pixels: Uint8ClampedArray,  // RGBA, length = cols*rows*4
  //   originLocal: { x, y, z },   // plane centered at (x,y,z) in viewer-local coords
  //   sizeXY: { w, h },     // plane size in viewer-local units (meters, post-recenter)
  //   opacity?: number,     // default 0.85
  // }
  _buildGroundRaster(spec) {
    const { cols, rows, pixels, originLocal, sizeXY, opacity = 0.85 } = spec;
    const canvas = document.createElement('canvas');
    canvas.width = cols;
    canvas.height = rows;
    const c2d = canvas.getContext('2d');
    const img = new ImageData(new Uint8ClampedArray(pixels), cols, rows);
    c2d.putImageData(img, 0, 0);

    const texture = new THREE.CanvasTexture(canvas);
    texture.colorSpace = THREE.SRGBColorSpace;
    texture.minFilter = THREE.LinearFilter;
    texture.magFilter = THREE.LinearFilter;
    texture.needsUpdate = true;

    const geom = new THREE.PlaneGeometry(sizeXY.w, sizeXY.h);
    const mat = new THREE.MeshBasicMaterial({
      map: texture,
      transparent: opacity < 1,
      opacity,
      side: THREE.DoubleSide,
      depthWrite: false,
    });
    const mesh = new THREE.Mesh(geom, mat);
    // Plane is XY-facing by default (normal = +Z). Place at the given origin.
    mesh.position.set(originLocal.x, originLocal.y, originLocal.z);

    return [mesh, () => { texture.dispose(); }];
  }
}
