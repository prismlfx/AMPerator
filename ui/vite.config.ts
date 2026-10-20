import { defineConfig } from 'vitest/config';

// The app embeds ui/dist with juce_add_binary_data, whose file list is fixed at CMake configure time
// (app/CMakeLists.txt, AMP_UI_FILES). So the build emits stable, unhashed names, inlines every asset
// (images, fonts) into the JS/CSS, and produces exactly: index.html, assets/index.js, assets/index.css.
// app/CheckUiDist.cmake fails the app build if anything else appears.
export default defineConfig({
  base: './',
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    assetsInlineLimit: () => true,
    cssCodeSplit: false,
    modulePreload: { polyfill: false },
    rolldownOptions: {
      output: {
        codeSplitting: false,
        entryFileNames: 'assets/index.js',
        assetFileNames: (asset) => (asset.names.some((n) => n.endsWith('.css')) ? 'assets/index.css' : 'assets/[name][extname]'),
      },
    },
  },
  server: { port: 5173, strictPort: true },
  test: { environment: 'jsdom' },
});
