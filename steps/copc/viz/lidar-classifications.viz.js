// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: lidar-classifications (artifact written by laz-extract-disc).
//
// Per-class bar chart with ASPRS class names, log-Y because the
// ground/non-ground split is extreme on most tiles.

import {
  Chart,
  BarController, BarElement,
  CategoryScale, LinearScale, LogarithmicScale,
  Tooltip, Title, Legend,
} from 'chart.js';

Chart.register(
  BarController, BarElement,
  CategoryScale, LinearScale, LogarithmicScale,
  Tooltip, Title, Legend,
);

const LAS_CLASS_NAMES = {
  0: 'created/never',
  1: 'unclassified',
  2: 'ground',
  3: 'low veg',
  4: 'med veg',
  5: 'high veg',
  6: 'building',
  7: 'low point',
  8: 'reserved',
  9: 'water',
  10: 'rail',
  11: 'road',
  12: 'overlap',
  17: 'bridge',
  64: 'virtual',
};

export function render(ctx, data) {
  const { name, dtype, n_items, data: arr } = data;

  const counter = new Map();
  for (let i = 0; i < arr.length; i++) {
    counter.set(arr[i], (counter.get(arr[i]) ?? 0) + 1);
  }
  const keys = Array.from(counter.keys()).sort((a, b) => a - b);
  const labels = keys.map((k) =>
    LAS_CLASS_NAMES[k] ? `${k} · ${LAS_CLASS_NAMES[k]}` : `class ${k}`,
  );
  const counts = keys.map((k) => counter.get(k));

  ctx.renderChart((host) => buildChart(host, name, labels, counts));
  ctx.renderJSON({
    artifact: name,
    dtype,
    n_items,
    n_classes: keys.length,
    counts: Object.fromEntries(keys.map((k, i) => [
      LAS_CLASS_NAMES[k] ? `${k} · ${LAS_CLASS_NAMES[k]}` : `class ${k}`,
      counts[i],
    ])),
  });
}

function buildChart(host, name, labels, counts) {
  const canvas = document.createElement('canvas');
  canvas.style.width = '100%';
  canvas.style.height = '220px';
  host.appendChild(canvas);

  new Chart(canvas, {
    type: 'bar',
    data: {
      labels,
      datasets: [{
        label: 'count',
        data: counts,
        backgroundColor: '#2f81f7',
        borderWidth: 0,
      }],
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      plugins: {
        legend: { display: false },
        title: { display: true, text: name, color: '#8b949e', font: { size: 11, weight: 'normal' } },
        tooltip: { enabled: true },
      },
      scales: {
        x: {
          title: { display: true, text: 'class', color: '#6e7681' },
          ticks: { color: '#8b949e', maxRotation: 40, minRotation: 0, autoSkip: true },
          grid: { color: '#21262d' },
        },
        y: {
          type: 'logarithmic',
          ticks: { color: '#8b949e' },
          grid: { color: '#21262d' },
        },
      },
    },
  });
}
