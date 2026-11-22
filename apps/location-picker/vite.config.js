// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Antoine Durand
// Build config for @dioramap/location-picker.
//
// Ships as a self-contained module: one minified ES file + one CSS file.
// MapLibre-GL is BUNDLED IN (not external) so consumers can drop the
// output into any page or app without loading MapLibre separately. Also
// produces a global-style IIFE build for <script type="module"> embeds
// on customer pages that don't use a bundler.

import { defineConfig } from 'vite';
import { resolve } from 'path';

export default defineConfig({
  server: {
    port: 5182,
    host: '127.0.0.1',
    strictPort: true,
  },
  build: {
    target: 'es2022',
    sourcemap: true,
    outDir: 'dist',
    emptyOutDir: true,
    minify: 'esbuild',
    cssCodeSplit: false, // one CSS file only
    lib: {
      entry: resolve(__dirname, 'src/sdk.js'),
      name: 'DioramapLocationPicker',
      // Standardised on ['es', 'umd'] to match viewer-sdk — embedders see
      // one filename pattern across both packages: .umd.js for <script>
      // tags + window globals, .es.js for `import { init } from '…'`.
      // Was ['es', 'iife'] historically; functionally identical for our
      // browser script-tag consumers, just inconsistent with the viewer.
      formats: ['es', 'umd'],
      fileName: (format) => `dioramap-location-picker.${format}.js`,
      cssFileName: 'dioramap-location-picker',
    },
    // NO external deps — MapLibre is bundled in. This was previously
    // externalised in the legacy viewer repo's config; flipped to bundled
    // for the v3 standalone story.
    rollupOptions: {
      output: {
        inlineDynamicImports: true,
        // sdk.js exports both a named `init` and a convenience default
        // object; prefer named-only in the bundle so consumers don't have
        // to do `DioramapLocationPicker.default.init(...)` when using UMD.
        exports: 'named',
      },
    },
  },
});
