// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: lidar-num-returns (artifact written by laz-extract-disc).
//
// Per-value bar chart over the small integer set of total returns
// (typically 1..7). Linear-Y; the distribution isn't extreme.

import {
  Chart,
  BarController, BarElement,
  CategoryScale, LinearScale,
  Tooltip, Title, Legend,
} from 'chart.js';

Chart.register(
  BarController, BarElement,
  CategoryScale, LinearScale,
  Tooltip, Title, Legend,
);

export function render(ctx, data) {
  const { name, dtype, n_items, data: arr } = data;

  const counter = new Map();
  for (let i = 0; i < arr.length; i++) {
    counter.set(arr[i], (counter.get(arr[i]) ?? 0) + 1);
  }
  const keys = Array.from(counter.keys()).sort((a, b) => a - b);
  const labels = keys.map((k) => String(k));
  const counts = keys.map((k) => counter.get(k));

  ctx.renderChart((host) => buildChart(host, name, labels, counts));
  ctx.renderJSON({
    artifact: name,
    dtype,
    n_items,
    counts: Object.fromEntries(keys.map((k, i) => [String(k), counts[i]])),
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
          title: { display: true, text: '# returns', color: '#6e7681' },
          ticks: { color: '#8b949e' },
          grid: { color: '#21262d' },
        },
        y: {
          ticks: { color: '#8b949e' },
          grid: { color: '#21262d' },
        },
      },
    },
  });
}
