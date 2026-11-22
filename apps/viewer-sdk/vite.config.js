// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand

import { defineConfig } from 'vite';
import { resolve } from 'node:path';

// Two entry points:
//   - `npm run dev`   → dev server on 5183 for the standalone index.html harness.
//   - `npm run build` → library bundle (ES + UMD) + one CSS file in `dist/`.
export default defineConfig(({ command }) => {
  return ({
  server: {
    port: 5183,
    host: '127.0.0.1',
    strictPort: true,
  },
  resolve: {
    // Same dedupe contract as lab-ui: scene-base ships its own three
    // dep, so without dedupe a consumer would end up with two copies and
    // `instanceof THREE.Object3D` checks across module boundaries break.
    dedupe: ['three'],
  },
  ...(command === 'build' && {
    build: {
      // One CSS file extracted from `import './styles.css'` (CSP fix —
      // embedders <link> dist/dioramap-viewer.css instead of the SDK
      // injecting <style> at runtime).
      cssCodeSplit: false,
      outDir: 'dist',
      lib: {
        entry: resolve(__dirname, 'src/sdk.js'),
        name: 'DioramapViewer',
        cssFileName: 'dioramap-viewer',
        // Format suffix in filename so embedders see at a glance which
        // they're loading. UMD = drop in via <script src> + use the
        // window global. ES = `import { init } from '…dioramap-viewer.es.js'`.
        // Without the format placeholder Vite would write both to the
        // same filename and the second overwrites the first (the bug
        // sdk v0.4.0 shipped: a "dioramap-viewer.js" that was UMD-only).
        fileName: (format) => `dioramap-viewer.${format}.js`,
        formats: ['es', 'umd'],
      },
      rollupOptions: {
        // three is bundled in (one canonical copy in the dist) so embedding
        // pages can drop a single <script> tag with no peer-dep dance.
        external: [],
        output: {
          // We intentionally export both named (init) and default. The UMD
          // global is assigned manually inside sdk.js (`window.DioramapViewer`),
          // so the wrapper itself just needs to re-export named bindings —
          // 'named' silences the default+named ambiguity warning.
          exports: 'named',
        },
      },
    },
  }),
  });
});
