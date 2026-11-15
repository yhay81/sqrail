import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { createServer } from "node:http";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { test as base, expect } from "@playwright/test";

const siteRoot = fileURLToPath(new URL("../site/", import.meta.url));
const { version } = JSON.parse(
  await readFile(new URL("../package.json", import.meta.url), "utf8"),
);
const test = base.extend({
  siteURL: [
    async ({}, use) => {
      const types = {
        ".html": "text/html",
        ".js": "text/javascript",
        ".css": "text/css",
        ".svg": "image/svg+xml",
        ".woff2": "font/woff2",
      };
      const server = createServer(async (request, response) => {
        try {
          let route = new URL(request.url, "http://localhost").pathname;
          if (route.endsWith("/")) route += "index.html";
          const file = path.resolve(siteRoot, `.${decodeURIComponent(route)}`);
          assert.ok(file.startsWith(siteRoot));
          const body = await readFile(file);
          response.writeHead(200, {
            "Content-Type":
              types[path.extname(file)] ?? "application/octet-stream",
          });
          response.end(body);
        } catch {
          response.writeHead(404).end();
        }
      });
      await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
      try {
        await use(`http://127.0.0.1:${server.address().port}`);
      } finally {
        await new Promise((resolve) => server.close(resolve));
      }
    },
    { scope: "worker" },
  ],
  baseURL: async ({ siteURL }, use) => use(siteURL),
  page: async ({ page }, use) => {
    // These tests exercise local files and never rely on GitHub or web fonts.
    await page.route("https://**/*", (route) => route.abort());
    await use(page);
  },
});

const readClipboard = (page) =>
  page.evaluate(() => navigator.clipboard.readText());
const tabTo = async (page, target) => {
  for (let index = 0; index < 40; index += 1) {
    await page.keyboard.press("Tab");
    if (await target.evaluate((node) => node === document.activeElement))
      return;
  }
  throw new Error("Copy control was not reachable with Tab");
};
const noPageOverflow = async (page) => {
  expect(
    await page.evaluate(() => document.documentElement.scrollWidth),
  ).toBeLessThanOrEqual(page.viewportSize().width);
};
const layout = (page) =>
  page.locator(".code-block").evaluateAll((blocks) =>
    blocks.map((block) => {
      const { x, y, width, height } = block.getBoundingClientRect();
      return { x, y: y + window.scrollY, width, height };
    }),
  );

for (const [label, platform, userAgent, clientPlatform, selected] of [
  ["Windows x64", "Win32", "Windows NT 10.0; Win64; x64", null, "windows"],
  ["Windows Arm64", "Win32", "Windows NT 10.0; ARM64", null, "windows-arm64"],
  ["Linux x64", "Linux x86_64", "Linux x86_64", null, "linux"],
  ["Linux Arm64", "Linux aarch64", "Linux aarch64", null, "linux-arm64"],
  ["client hints", "MacIntel", "Windows NT 10.0", "Windows", "windows"],
  ["macOS", "MacIntel", "Macintosh", null, "homebrew"],
  ["Android", "Linux arm64", "Android", null, "homebrew"],
  ["unknown", "", "", null, "homebrew"],
]) {
  test(`automatically selects the platform for ${label}`, async ({ page }) => {
    await page.addInitScript(
      ({ platform, userAgent, clientPlatform }) => {
        Object.defineProperties(navigator, {
          platform: { value: platform },
          userAgent: { value: userAgent },
          userAgentData: {
            value: clientPlatform ? { platform: clientPlatform } : undefined,
          },
        });
      },
      { platform, userAgent, clientPlatform },
    );
    await page.goto("/");
    await expect(page.locator(`#install-tab-${selected}`)).toHaveAttribute(
      "aria-selected",
      "true",
    );
    await expect(
      page.locator(`#install-panel-${selected}`),
    ).not.toHaveAttribute("hidden");
  });
}

test("installer tabs wrap, support keyboard navigation, and copy matching archives", async ({
  page,
}, testInfo) => {
  await page.goto("/");
  const tabs = page.locator("[data-install-tab]");
  await expect(tabs).toHaveCount(6);
  const tabList = page.getByRole("tablist", {
    name: "Install or download method",
  });
  expect(
    await tabList.evaluate((node) => node.scrollWidth <= node.clientWidth),
  ).toBe(true);
  await noPageOverflow(page);

  await page.locator("#install-tab-homebrew").focus();
  await page.keyboard.press("End");
  await expect(page.locator("#install-tab-agent")).toBeFocused();
  await page.keyboard.press("ArrowRight");
  await expect(page.locator("#install-tab-homebrew")).toBeFocused();
  await page.keyboard.press("ArrowLeft");
  await expect(page.locator("#install-tab-agent")).toBeFocused();
  await page.keyboard.press("Home");

  for (const [tab, target, command] of [
    ["linux", "linux-x86_64.tar.gz", "curl"],
    ["linux-arm64", "linux-arm64.tar.gz", "curl"],
    ["windows", "windows-x86_64.zip", "curl.exe"],
    ["windows-arm64", "windows-arm64.zip", "curl.exe"],
  ]) {
    await page.keyboard.press("ArrowRight");
    const selected = page.locator(`#install-tab-${tab}`);
    const panel = page.locator(`#install-panel-${tab}`);
    await expect(selected).toBeFocused();
    await expect(selected).toHaveAttribute("aria-selected", "true");
    await expect(selected).toHaveAttribute(
      "aria-controls",
      `install-panel-${tab}`,
    );
    await expect(
      page.locator('[data-install-tab][aria-selected="true"]'),
    ).toHaveCount(1);
    expect(
      await tabs.evaluateAll(
        (nodes) => nodes.filter((node) => node.tabIndex === 0).length,
      ),
    ).toBe(1);
    const expected = `${command} -LO https://github.com/yhay81/sqrail/releases/download/v${version}/sqrail-v${version}-${target}`;
    await page.keyboard.press("Tab");
    await expect(panel.getByRole("button", { name: /Copy/ })).toBeFocused();
    await page.keyboard.press("Enter");
    await expect.poll(() => readClipboard(page)).toBe(expected);
    await expect(panel.getByRole("status")).toHaveText("Copied");
    await expect(panel.getByRole("status")).toBeInViewport({ ratio: 1 });
    await expect(page.locator("#install-code")).toHaveAttribute(
      "data-copy-value",
      expected,
    );
    await page.keyboard.press("Shift+Tab");
  }
  const installerScreenshot = testInfo.outputPath(
    "installer-windows-arm64.png",
  );
  await page
    .locator(".quick-install")
    .screenshot({ path: installerScreenshot });
  await testInfo.attach("installer-windows-arm64", {
    path: installerScreenshot,
    contentType: "image/png",
  });
  await page.locator('button[commandfor="install-code"]').press("Space");
  await expect.poll(() => readClipboard(page)).toContain(`windows-arm64.zip`);
});

for (const fallback of [false, true]) {
  test(`all seven guide examples copy exactly with ${fallback ? "click fallback" : "native commands"}`, async ({
    page,
  }, testInfo) => {
    if (fallback) await page.addInitScript(() => delete window.CommandEvent);
    await page.goto("/docs/");
    const blocks = page.locator(".code-block");
    await expect(blocks).toHaveCount(7);
    await expect(page.locator("#qs-code-3")).toHaveText(
      "sqrail run \\\n  -t sales=sales.csv \\\n  -t drugs=drugs.parquet \\\n  -o result.parquet \\\n  - <<'SQL'\nSELECT d.name, sum(s.amount) AS total\nFROM sales AS s\nJOIN drugs AS d USING (drug_id)\nGROUP BY d.name\nORDER BY total DESC\nSQL",
      { useInnerText: false },
    );
    const before = await layout(page);
    for (let index = 0; index < 7; index += 1) {
      const block = blocks.nth(index);
      const expected = await block.locator("pre > code").textContent();
      const button = block.getByRole("button", { name: /^Copy / });
      if (index === 0) await button.focus();
      else await tabTo(page, button);
      await expect(button).toBeFocused();
      await page.keyboard.press(index % 2 ? "Space" : "Enter");
      await expect.poll(() => readClipboard(page)).toBe(expected);
      await expect(block.getByRole("status")).toHaveText("Copied");
      await expect(block.getByRole("status")).toBeInViewport({ ratio: 1 });
      await expect(button).toBeFocused();
      expect(await layout(page)).toEqual(before);
    }
    await noPageOverflow(page);
    const guideScreenshot = testInfo.outputPath("guide-code.png");
    await blocks.nth(2).screenshot({ path: guideScreenshot });
    await testInfo.attach(`guide-copy-${fallback ? "fallback" : "native"}`, {
      path: guideScreenshot,
      contentType: "image/png",
    });
  });
}

test("copy preserves whitespace, shell variables, and redirection", async ({
  page,
}) => {
  await page.goto("/docs/");
  for (const text of [
    "  SELECT 1;\n\n",
    "> result.txt printf '%s\\n' \"$HOME\"\n",
    "$env:SQRAIL = 'value'\n",
  ]) {
    await page.locator("#qs-code-1").evaluate((code, value) => {
      code.textContent = value;
    }, text);
    await page.locator('button[commandfor="qs-code-1"]').press("Enter");
    await expect.poll(() => readClipboard(page)).toBe(text);
  }
});

test("copy failure, retry, and repeated success update the live status without shifting content", async ({
  page,
}) => {
  await page.goto("/docs/");
  await page.evaluate(() => {
    const write = navigator.clipboard.writeText.bind(navigator.clipboard);
    navigator.clipboard.writeText = async (text) => {
      navigator.clipboard.writeText = write;
      throw new DOMException("Permission denied", "NotAllowedError");
    };
  });
  const block = page.locator(".code-block").first();
  const button = block.getByRole("button");
  const status = block.getByRole("status");
  await button.focus();
  const before = await layout(page);
  await page.keyboard.press("Enter");
  await expect(status).toHaveText("Copy failed");
  await expect(status).toBeInViewport({ ratio: 1 });
  await expect(button).toHaveAttribute("data-failed");
  expect(await layout(page)).toEqual(before);
  await page.keyboard.press("Space");
  await expect(status).toHaveText("Copied");
  await expect(button).not.toHaveAttribute("data-failed");
  const message = await status.textContent();
  await page.keyboard.press("Enter");
  await expect.poll(() => status.textContent()).not.toBe(message);
  await expect(status).toHaveText("Copied");
  expect(await layout(page)).toEqual(before);
  await expect(status).toBeEmpty();
  await expect(button).toBeFocused();
  expect(await layout(page)).toEqual(before);
});

for (const unavailable of ["javascript", "clipboard", "writeText"]) {
  test(`code stays selectable without ${unavailable}`, async ({
    browser,
    siteURL,
  }, testInfo) => {
    const context = await browser.newContext({
      ...testInfo.project.use,
      javaScriptEnabled: unavailable !== "javascript",
    });
    try {
      const page = await context.newPage();
      await page.route("https://**/*", (route) => route.abort());
      if (unavailable !== "javascript") {
        await page.addInitScript((mode) => {
          Object.defineProperty(navigator, "clipboard", {
            value: mode === "clipboard" ? undefined : {},
          });
        }, unavailable);
      }
      await page.goto(`${siteURL}/docs/`);
      await expect(page.getByRole("button", { name: /^Copy / })).toHaveCount(0);
      const code = page.locator("#qs-code-3");
      await expect(code).toBeVisible();
      const expected = await code.textContent();
      const selected = await code.evaluate((node) => {
        const range = document.createRange();
        range.selectNodeContents(node);
        const selection = window.getSelection();
        selection.removeAllRanges();
        selection.addRange(range);
        return selection.toString();
      });
      expect(selected).toBe(expected);
      await noPageOverflow(page);
    } finally {
      await context.close();
    }
  });
}
