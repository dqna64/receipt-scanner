import { defineConfig } from 'vite';

// Dev: the Drogon server runs natively on :8080 and Vite proxies /api to it, so the session
// cookie is same-origin. `host: true` serves on the LAN for testing from a phone.
export default defineConfig({
  // The only big chunk is heic-to's libheif WASM (~3 MB), which is imported lazily, only when a
  // photo is HEIC and the browser can't decode it natively.
  build: { chunkSizeWarningLimit: 3500 },
  server: {
    host: true,
    port: 5173,
    proxy: {
      '/api': 'http://localhost:8080',
    },
  },
});
