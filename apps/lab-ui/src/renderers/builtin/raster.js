// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Built-in raster-F32 renderer.
//
// L2 scope: renders each channel as a grayscale/false-color canvas into the
// overlay panel, with a channel selector if n_channels > 1. A future L3
// polish adds the 3D ground-plane overlay (plan §4.3). The overlay view
// already makes the raster's shape + data ranges obvious, which is the
// primary L2 use case.

export function render(ctx, data) {
  const { cols, rows, n_channels, channels } = data;
  if (!channels || !channels.length) {
    ctx.renderHTML('<div class="render-placeholder">empty raster</div>');
    return;
  }

  const wrapper = document.createElement('div');
  wrapper.className = 'raster-render';

  // --- 3D ground-plane path -------------------------------------------- //
  // If this raster has a canonical-location sibling, drop a colored ground
  // plane into the 3D scene aligned with the rest of the cloud. The channel
  // picker below re-draws both the canvas and the plane's texture.
  const canonical = ctx.getSiblingDecoded('canonical-location');
  let threeDHandle = null;
  if (canonical?.json?.bbox_l93) {
    threeDHandle = setup3DGroundPlane(ctx, cols, rows, canonical.json);
  }
  // ---------------------------------------------------------------------- //

  const header = document.createElement('div');
  header.className = 'raster-header';
  header.innerHTML = `
    <span>${escape(ctx.label)}</span>
    <span class="mono">${cols}×${rows} · ${n_channels} ch</span>
  `;
  wrapper.appendChild(header);

  const picker = document.createElement('select');
  picker.className = 'raster-channel-picker';
  for (let i = 0; i < n_channels; i++) {
    const opt = document.createElement('option');
    opt.value = String(i);
    opt.textContent = channelLabel(ctx.label, i);
    picker.appendChild(opt);
  }

  const canvas = document.createElement('canvas');
  canvas.width = cols;
  canvas.height = rows;
  canvas.style.width = '100%';
  canvas.style.imageRendering = 'pixelated';

  const stats = document.createElement('div');
  stats.className = 'raster-stats mono';

  const draw = () => {
    const idx = +picker.value;
    const ch = channels[idx];
    const s = drawChannel(canvas, ch, cols, rows);
    stats.textContent = `min=${formatNum(s.min)} max=${formatNum(s.max)} mean=${formatNum(s.mean)} NaN=${s.nan}`;
    threeDHandle?.refresh(ch, s);
  };
  picker.addEventListener('change', draw);

  wrapper.appendChild(picker);
  wrapper.appendChild(canvas);
  wrapper.appendChild(stats);
  ctx.renderHTML(wrapper);
  draw();
}

// Drops a textured quad into the 3D scene, aligned with the L93 bbox that
// canonical-location carries. Returns a handle with `refresh(channel, stats)`
// that re-uploads the texture pixels when the overlay's channel picker fires.
function setup3DGroundPlane(ctx, cols, rows, canonical) {
  const [xmin, ymin, xmax, ymax] = canonical.bbox_l93;
  const sceneOrigin = ctx.viewer.getSceneOrigin();
  if (!sceneOrigin) {
    // No shared origin yet — the user subscribed to a raster without a
    // point cloud. Drop a plane-less render; canvas in the overlay still works.
    return null;
  }
  const sizeX = xmax - xmin;
  const sizeY = ymax - ymin;
  const centerLocal = {
    x: (xmin + xmax) / 2 - sceneOrigin.x,
    y: (ymin + ymax) / 2 - sceneOrigin.y,
    // Drop the ground plane ~10m below the scene origin so it doesn't
    // Z-fight the point cloud; the user can still orbit around it. A
    // future scene-origin artifact + elevation integration can pin this
    // to the real ground z.
    z: -10,
  };

  const pixels = new Uint8ClampedArray(cols * rows * 4);
  // Stable visibility-tree leaf id keyed off the artifact name (this.label),
  // grouped under a 'Rasters' subcategory so all the AK_RASTER_F32
  // ground planes (lidar-rasters, lidar-ndsm, ortho-rgb-rasters,
  // deshadowed-ortho-rgb-rasters, …) collapse under one tri-state
  // group node in the gear-popover Layers tree. Without this they
  // were always-on and untoggleable — see the L10/raster.js drift
  // discussion: ctx.viewer.addLayer was called directly, bypassing
  // sceneState.visibility.attach.
  const leafId = `raster::${ctx.label}`;
  const buildSpec = () => ({
    kind: 'ground-raster',
    cols, rows,
    pixels,
    originLocal: centerLocal,
    sizeXY: { w: sizeX, h: sizeY },
    opacity: 0.85,
    layer: {
      id: leafId,
      label: ctx.label,
      path: ['Rasters'],
      defaultVisible: true,
    },
  });
  ctx.render3D(buildSpec());

  return {
    refresh(channel, stats) {
      // Rebuild the ground plane's texture canvas pixels from the currently-
      // selected channel.
      const range = (stats.max - stats.min) || 1;
      // Flip Y so the raster's south/north matches the overlay canvas.
      for (let y = 0; y < rows; y++) {
        const srcRow = rows - 1 - y;
        for (let x = 0; x < cols; x++) {
          const v = channel[srcRow * cols + x];
          const p = (y * cols + x) * 4;
          if (Number.isNaN(v)) {
            pixels[p + 0] = 40; pixels[p + 1] = 20; pixels[p + 2] = 60; pixels[p + 3] = 160;
          } else {
            const t = (v - stats.min) / range;
            const c = Math.round(t * 255);
            pixels[p + 0] = c; pixels[p + 1] = c; pixels[p + 2] = c; pixels[p + 3] = 255;
          }
        }
      }
      // Re-render through ctx.render3D again with the SAME layer.id so
      // the visibility-tree leaf gets the new mesh attached and
      // user-toggled state still applies. (The previous mesh is
      // disposed inside viewer.addLayer's removeLayer-first path.)
      ctx.render3D(buildSpec());
    },
  };
}

function drawChannel(canvas, ch, cols, rows) {
  let min = Infinity, max = -Infinity, sum = 0, count = 0, nanCount = 0;
  for (let i = 0; i < ch.length; i++) {
    const v = ch[i];
    if (Number.isNaN(v)) { nanCount++; continue; }
    if (v < min) min = v;
    if (v > max) max = v;
    sum += v;
    count++;
  }
  const range = max - min || 1;
  const mean = count ? sum / count : NaN;

  const ctx2 = canvas.getContext('2d');
  const img = ctx2.createImageData(cols, rows);
  // Raster rows: row 0 is at ymin (south). Canvas rows: 0 is at top.
  // Flip vertically so north is up visually.
  for (let y = 0; y < rows; y++) {
    const srcRow = rows - 1 - y;
    for (let x = 0; x < cols; x++) {
      const v = ch[srcRow * cols + x];
      const p = (y * cols + x) * 4;
      if (Number.isNaN(v)) {
        img.data[p + 0] = 40;
        img.data[p + 1] = 20;
        img.data[p + 2] = 60;
        img.data[p + 3] = 255;
      } else {
        const t = (v - min) / range;
        const c = Math.round(t * 255);
        img.data[p + 0] = c;
        img.data[p + 1] = c;
        img.data[p + 2] = c;
        img.data[p + 3] = 255;
      }
    }
  }
  ctx2.putImageData(img, 0, 0);
  return { min, max, mean, nan: nanCount };
}

// Minimal per-artifact channel naming. Matches the C enums in
// steps/registration/registration-steps.h and steps/ortho/ortho-steps.h.
// An unknown artifact just uses "ch N".
function channelLabel(name, i) {
  const schemas = {
    'lidar-rasters': ['maxZ', 'minZ', 'meanZ', 'density', 'intensity', 'roughness', 'multiReturn'],
    'lidar-ndsm': ['ndsm'],
    'lidar-building-mask': ['mask'],
    'ortho-rgb-rasters': ['R', 'G', 'B', 'luminance', 'saturation', 'greenness', 'lumStddev'],
    'ortho-irc-rasters': ['nir', 'ndvi'],
  };
  const labels = schemas[name];
  return labels?.[i] ?? `ch ${i}`;
}

function formatNum(v) {
  if (Number.isNaN(v)) return 'NaN';
  if (!Number.isFinite(v)) return String(v);
  return Math.abs(v) >= 1000 || Math.abs(v) < 0.01
    ? v.toExponential(2)
    : v.toFixed(3);
}

function escape(s) {
  return String(s ?? '').replace(/[&<>"']/g, (c) => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[c]));
}
