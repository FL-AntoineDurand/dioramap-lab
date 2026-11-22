// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand

import { defineConfig } from 'vite';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const __dirname = dirname(fileURLToPath(import.meta.url));

// Single dev-server port dedicated to this agent so it doesn't collide with
// other agents' instances. 5181 = Vite default (5173) + 8.
export default defineConfig({
  server: {
    port: 5181,
    host: '127.0.0.1',
    strictPort: true,
    fs: {
      // L11: step-visualizer glob lives outside lab-ui's root. Allow
      // serving from the v3 root so `/v3/steps/<step>/viz/*.viz.js`
      // resolves over the dev server.
      allow: [resolve(__dirname, '..', '..')],
    },
    // Proxy `/ws` to the dioramap-processor's WebSocket port. Lets the
    // browser connect to ws://<vite-host>:5181/ws → Vite forwards to
    // ws://127.0.0.1:7801/ inside the dev container. Bypasses VS Code's
    // separate port-forwarder for 7801, which sometimes mangles the
    // WebSocket Upgrade handshake.
    //
    // Override the processor port via the PROCESSOR_PORT env var when
    // launching `npm run dev` (default 7801).
    proxy: {
      '/ws': {
        target: `ws://127.0.0.1:${process.env.PROCESSOR_PORT || 7801}`,
        ws: true,
        // The processor listens at `/`, not `/ws`. Strip the prefix so
        // the upgrade reaches the right path.
        rewrite: (path) => path.replace(/^\/ws/, ''),
      },
    },
  },
  resolve: {
    // @dioramap/scene-base depends on three too. Without dedupe, the bundle
    // would ship two copies (one resolved from lab-ui, one from scene-base).
    // dedupe ensures one canonical Three.js — needed for instanceof checks
    // (Mesh, Points, Object3D) to work across module boundaries.
    //
    // chart.js is in dedupe so step-visualizers under v3/steps/ (outside
    // lab-ui's node_modules tree) can `import 'chart.js'` and have Vite
    // resolve it from lab-ui/node_modules instead of failing the walk-up.
    dedupe: ['three', 'chart.js'],
  },
});
