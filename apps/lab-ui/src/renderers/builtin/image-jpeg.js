// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Built-in image-JPEG renderer.
//
// L2: renders an <img> into the overlay panel. The plan also mentions a
// ground-plane 3D overlay (plan §4.3); that lands in L3 alongside the raster
// 3D overlay — both use the same ground-plane primitive.

export function render(ctx, data) {
  const blob = new Blob([data.bytes], { type: 'image/jpeg' });
  const url = URL.createObjectURL(blob);

  const wrapper = document.createElement('div');
  wrapper.className = 'image-render';

  const img = document.createElement('img');
  img.src = url;
  img.alt = ctx.label;
  img.className = 'image-render-img';

  const meta = document.createElement('div');
  meta.className = 'image-render-meta mono';
  img.addEventListener('load', () => {
    meta.textContent = `${img.naturalWidth}×${img.naturalHeight} · ${(data.bytes.byteLength / 1024).toFixed(1)} KB`;
  });

  wrapper.appendChild(img);
  wrapper.appendChild(meta);
  ctx.renderHTML(wrapper);

  ctx.onCleanup(() => URL.revokeObjectURL(url));
}
