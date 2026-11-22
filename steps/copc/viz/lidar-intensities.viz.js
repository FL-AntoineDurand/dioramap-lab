// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// step-visualizer: lidar-intensities (artifact written by laz-extract-disc).
//
// 32-bin equal-width histogram of point return intensities. Log-Y because
// the distribution is heavy at low intensities and thins out fast.

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

const N_BINS = 32;

export function render(ctx, data) {
  const { name, dtype, n_items, data: arr } = data;

  const stats = computeScalarStats(arr);
  const { labels, counts } = bin(arr, stats);

  ctx.renderChart((host) => buildChart(host, name, labels, counts));
  ctx.renderJSON({
    artifact: name,
    dtype,
    n_items,
    stats,
  });
}

function bin(arr, stats) {
  const min = stats.min;
  const max = stats.max;
  const width = (max - min) / N_BINS || 1;
  const counts = new Array(N_BINS).fill(0);
  for (let i = 0; i < arr.length; i++) {
    const bin = Math.min(N_BINS - 1, Math.max(0, Math.floor((arr[i] - min) / width)));
    counts[bin]++;
  }
  const labels = counts.map((_, i) => (min + i * width).toFixed(0));
  return { labels, counts };
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
          title: { display: true, text: name, color: '#6e7681' },
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

function computeScalarStats(arr) {
  const n = arr.length;
  if (n === 0) return { n: 0 };
  let min = Infinity, max = -Infinity, sum = 0;
  for (let i = 0; i < n; i++) {
    const v = arr[i];
    if (v < min) min = v;
    if (v > max) max = v;
    sum += v;
  }
  return { n, min, max, mean: sum / n };
}
