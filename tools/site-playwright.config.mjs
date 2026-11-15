import { defineConfig } from "@playwright/test";

export default defineConfig({
  testDir: ".",
  testMatch: "site.spec.mjs",
  // Clipboard contents are shared by pages, so copy tests run serially.
  workers: 1,
  use: {
    browserName: "chromium",
    permissions: ["clipboard-read", "clipboard-write"],
    reducedMotion: "reduce",
    screenshot: "only-on-failure",
  },
  projects: [
    { name: "desktop", use: { viewport: { width: 1280, height: 900 } } },
    {
      name: "mobile",
      use: {
        viewport: { width: 390, height: 844 },
        isMobile: true,
        hasTouch: true,
      },
    },
  ],
});
