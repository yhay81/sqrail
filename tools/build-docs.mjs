import { mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";

import MarkdownIt from "markdown-it";
import * as prettier from "prettier";

const root = process.cwd();
const checkOnly = process.argv.includes("--check");

const pages = [
  {
    source: "site/install-agent.md",
    output: "site/docs/agents/index.html",
    route: "/docs/agents/",
    title: "Agent setup",
    category: "Guide",
    description:
      "Install sqrail and its optional Agent Skill with a review-first path for the agent you already use.",
    machineUrl: "https://sqrails.yhay81.com/install-agent.md",
    navRoute: "/docs/agents/",
  },
  {
    source: "docs/CONTRACT.md",
    output: "site/docs/reference/contract/index.html",
    route: "/docs/reference/contract/",
    title: "CLI contract",
    category: "Reference",
    description:
      "Normative commands, query behavior, streams, resource controls, schemas, and diagnostics.",
    navRoute: "/docs/reference/contract/",
  },
  {
    source: "docs/AGENT_INTEGRATION.md",
    output: "site/docs/reference/agents/index.html",
    route: "/docs/reference/agents/",
    title: "Agent integration",
    category: "Reference",
    description:
      "Minimal context, install discovery, invocation rules, limits, stop conditions, and error handling for coding agents.",
    navRoute: "/docs/reference/agents/",
  },
  {
    source: "docs/BASELINE.md",
    output: "site/docs/reference/performance/index.html",
    route: "/docs/reference/performance/",
    title: "Performance baselines",
    category: "Evidence",
    description:
      "Reproducible size, startup, memory, and throughput measurements against the pinned DuckDB CLI.",
    navRoute: "/docs/reference/performance/",
  },
  {
    source: "docs/BENCHMARKS.md",
    output: "site/docs/reference/benchmarks/index.html",
    route: "/docs/reference/benchmarks/",
    title: "Benchmark policy",
    category: "Evidence",
    description:
      "The rules and workload matrix used to keep every published performance claim reproducible.",
    navRoute: "/docs/reference/performance/",
  },
  {
    source: "docs/PLATFORMS.md",
    output: "site/docs/reference/platforms/index.html",
    route: "/docs/reference/platforms/",
    title: "Platform support",
    category: "Reference",
    description:
      "Release targets, support tiers, filesystem semantics, and the exact meaning of tested support.",
    navRoute: "/docs/reference/platforms/",
  },
  {
    source: "SECURITY.md",
    output: "site/docs/security/index.html",
    route: "/docs/security/",
    title: "Security boundary",
    category: "Project",
    description:
      "Supported versions, private reporting, guarantees, threat boundaries, and explicit non-goals.",
    navRoute: "/docs/security/",
  },
  {
    source: "CONTRIBUTING.md",
    output: "site/docs/contributing/index.html",
    route: "/docs/contributing/",
    title: "Contributing",
    category: "Project",
    description:
      "A focused path from a first issue through tests, performance evidence, and a reviewable pull request.",
    navRoute: "/docs/contributing/",
  },
  {
    source: "docs/TESTING.md",
    output: "site/docs/contributing/testing/index.html",
    route: "/docs/contributing/testing/",
    title: "Testing architecture",
    category: "Project",
    description:
      "Test layers, workflow placement, sanitizers, fuzzing, and checks for the website and Agent Skill.",
    navRoute: "/docs/contributing/",
  },
  {
    source: "SUPPORT.md",
    output: "site/docs/contributing/support/index.html",
    route: "/docs/contributing/support/",
    title: "Support",
    category: "Project",
    description:
      "Choose the right public or private channel for usage questions, defects, security reports, and proposals.",
    navRoute: "/docs/contributing/",
  },
  {
    source: "CODE_OF_CONDUCT.md",
    output: "site/docs/contributing/code-of-conduct/index.html",
    route: "/docs/contributing/code-of-conduct/",
    title: "Code of Conduct",
    category: "Project",
    description:
      "The conduct, reporting, and repair expectations that keep the sqrail community safe and productive.",
    navRoute: "/docs/contributing/",
  },
];

const documentationNavigation = [
  {
    label: "Start",
    links: [
      ["Quick start", "/docs/"],
      ["Agent setup", "/docs/agents/"],
    ],
  },
  {
    label: "Use",
    links: [
      ["CLI contract", "/docs/reference/contract/"],
      ["Agent integration", "/docs/reference/agents/"],
    ],
  },
  {
    label: "Operate",
    links: [
      ["Performance", "/docs/reference/performance/"],
      ["Benchmark policy", "/docs/reference/benchmarks/"],
      ["Platforms", "/docs/reference/platforms/"],
      ["Security", "/docs/security/"],
    ],
  },
  {
    label: "Project",
    links: [
      ["Contributing", "/docs/contributing/"],
      ["Support", "/docs/contributing/support/"],
    ],
  },
];

const publicRoutes = new Map([
  ["docs/README.md", "/docs/"],
  ...pages.map((page) => [page.source, page.route]),
]);

const escapeHtml = (value) =>
  value
    .replaceAll("&", "&amp;")
    .replaceAll('"', "&quot;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;");

const slugify = (value) =>
  value
    .normalize("NFKD")
    .toLowerCase()
    .replaceAll(/[^a-z0-9]+/g, "-")
    .replaceAll(/^-|-$/g, "") || "section";

const githubBlobUrl = (repoPath) =>
  `https://github.com/yhay81/sqrail/blob/main/${repoPath}`;

const githubRawUrl = (repoPath) =>
  `https://raw.githubusercontent.com/yhay81/sqrail/main/${repoPath}`;

const splitFragment = (href) => {
  const marker = href.indexOf("#");
  return marker === -1
    ? [href, ""]
    : [href.slice(0, marker), href.slice(marker)];
};

const rewriteHref = (page, href) => {
  if (
    href.startsWith("#") ||
    href.startsWith("mailto:") ||
    href.startsWith("https://") ||
    href.startsWith("http://")
  ) {
    if (href === "https://sqrails.yhay81.com/install-agent.md") {
      return "/docs/agents/";
    }
    return href;
  }

  const [relativePath, fragment] = splitFragment(href);
  const resolved = path
    .normalize(path.join(path.dirname(page.source), relativePath))
    .replaceAll(path.sep, "/");
  return `${publicRoutes.get(resolved) ?? githubBlobUrl(resolved)}${fragment}`;
};

const markdown = new MarkdownIt({
  html: false,
  linkify: true,
  typographer: true,
});

const renderMarkdown = (page, source) => {
  const body = source.replace(/^# .+\n+/, "");
  const tokens = markdown.parse(body, {});
  const headings = [];
  const slugCounts = new Map();

  const visit = (tokenList) =>
    tokenList.forEach((token) => {
      if (token.children) visit(token.children);

      if (token.type === "link_open") {
        const href = token.attrGet("href");
        if (href) token.attrSet("href", rewriteHref(page, href));
      }

      if (["th_open", "td_open"].includes(token.type)) {
        const style = token.attrGet("style");
        const alignment = style?.match(
          /text-align:\s*(left|center|right)/,
        )?.[1];
        if (alignment) token.attrJoin("class", `align-${alignment}`);
        const styleIndex = token.attrIndex("style");
        if (styleIndex >= 0) token.attrs.splice(styleIndex, 1);
      }
    });
  visit(tokens);

  tokens.forEach((token, index) => {
    if (token.type === "heading_open") {
      const label = tokens[index + 1]?.content ?? "Section";
      const base = slugify(label);
      const count = slugCounts.get(base) ?? 0;
      slugCounts.set(base, count + 1);
      const id = count ? `${base}-${count + 1}` : base;
      token.attrSet("id", id);
      if (token.tag === "h2") headings.push({ id, label });
    }
  });

  return {
    headings,
    html: markdown.renderer.render(tokens, markdown.options, {}),
  };
};

const renderRepositoryLink = () => `
  <a
    class="github-repo"
    data-github-repo
    href="https://github.com/yhay81/sqrail"
    aria-label="View yhay81/sqrail on GitHub — 1 star"
  >
    <svg class="github-icon" viewBox="0 0 16 16" aria-hidden="true" focusable="false">
      <path d="M8 0C3.58 0 0 3.64 0 8.13c0 3.59 2.29 6.64 5.47 7.71.4.08.55-.18.55-.39 0-.19-.01-.83-.01-1.51-2.01.38-2.53-.5-2.69-.96-.09-.23-.48-.96-.82-1.15-.28-.15-.68-.54-.01-.55.63-.01 1.08.59 1.23.83.72 1.23 1.87.88 2.33.67.07-.53.28-.88.51-1.08-1.78-.2-3.64-.9-3.64-4.01 0-.89.31-1.62.82-2.19-.08-.2-.36-1.04.08-2.16 0 0 .67-.22 2.2.84A7.48 7.48 0 0 1 8 3.91c.68 0 1.36.09 2 .27 1.53-1.06 2.2-.84 2.2-.84.44 1.12.16 1.96.08 2.16.51.57.82 1.3.82 2.19 0 3.12-1.87 3.81-3.65 4.01.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.47.55.39A8.13 8.13 0 0 0 16 8.13C16 3.64 12.42 0 8 0Z" />
    </svg>
    <span class="github-label">GitHub</span>
    <span class="github-stars">
      <svg class="github-star-icon" viewBox="0 0 16 16" aria-hidden="true" focusable="false">
        <path d="M8 0.25a0.75 0.75 0 0 1 0.673 0.418l1.882 3.815 4.21 0.612a0.75 0.75 0 0 1 0.416 1.279l-3.046 2.97 0.719 4.193a0.75 0.75 0 0 1-1.088 0.79L8 12.347l-3.766 1.98a0.75 0.75 0 0 1-1.088-0.79l0.719-4.194L0.819 6.374a0.75 0.75 0 0 1 0.416-1.28l4.21-0.611 1.882-3.815A0.75 0.75 0 0 1 8 0.25Z" />
      </svg>
      <span class="github-star-count" data-github-stars aria-live="polite">1</span>
    </span>
  </a>`;

const renderPageNavigation = (page) =>
  documentationNavigation
    .map(
      ({ label, links }) => `
        <div class="docs-nav-group">
          <p>${label}</p>
          <div>
            ${links
              .map(([linkLabel, route]) => {
                const current =
                  route === page.route
                    ? "page"
                    : route === page.navRoute
                      ? "location"
                      : null;
                return `<a href="${route}"${current ? ` aria-current="${current}"` : ""}>${linkLabel}</a>`;
              })
              .join("")}
          </div>
        </div>`,
    )
    .join("");

const renderSectionNavigation = (headings) =>
  headings
    .map(
      ({ id, label }, index) => `
        <a href="#${id}"${index === 0 ? ' aria-current="location"' : ""}>${escapeHtml(label)}</a>`,
    )
    .join("");

const renderPage = async (page, source) => {
  const rendered = renderMarkdown(page, source);
  const sourceUrl = githubRawUrl(page.source);
  const machineUrl = page.machineUrl ?? sourceUrl;
  const canonical = `https://sqrails.yhay81.com${page.route}`;
  const title = escapeHtml(page.title);
  const description = escapeHtml(page.description);
  const sourceNote =
    page.source === "site/install-agent.md"
      ? "This page and the agent-readable installer share one source."
      : "The rendered page and repository Markdown share one source.";

  const html = `<!doctype html>
<html lang="en">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <meta name="description" content="${description}" />
    <meta name="theme-color" content="#090a09" />
    <meta property="og:title" content="${title} — sqrail" />
    <meta property="og:description" content="${description}" />
    <meta property="og:type" content="article" />
    <meta property="og:url" content="${canonical}" />
    <meta property="og:image" content="https://sqrails.yhay81.com/social-card.png" />
    <meta name="twitter:card" content="summary_large_image" />
    <link rel="canonical" href="${canonical}" />
    <link rel="alternate" type="text/markdown" href="${machineUrl}" />
    <title>${title} — sqrail documentation</title>
    <link rel="icon" href="/icon.svg" type="image/svg+xml" />
    <link rel="stylesheet" href="/styles.css" />
    <script src="/app.js" defer></script>
  </head>
  <body class="docs-body">
    <a class="skip-link" href="#main">Skip to content</a>

    <header class="site-header" data-site-header>
      <div class="nav">
        <div class="nav-brand">
          <a class="wordmark" href="/" aria-label="sqrail home">
            <span class="wordmark-mark" aria-hidden="true"></span>
            sqrail
          </a>
          <span class="nav-tagline">Agent-safe SQL on files</span>
        </div>
        <nav class="nav-links" aria-label="Primary">
          <a href="/docs/"${page.route === "/docs/" ? ' aria-current="page"' : ""}>Get started</a>
          <a href="/docs/agents/"${page.route === "/docs/agents/" ? ' aria-current="page"' : ""}>Agent setup</a>
          <a href="/docs/reference/contract/"${
            page.route === "/docs/reference/contract/"
              ? ' aria-current="page"'
              : page.route !== "/docs/agents/"
                ? ' aria-current="location"'
                : ""
          }>Reference</a>
        </nav>
        <div class="nav-actions">
          ${renderRepositoryLink()}
          <a class="nav-install" href="/docs/">Install</a>
        </div>
      </div>
    </header>

    <main class="docs-shell docs-shell-reference" id="main">
      <aside class="docs-sidebar" aria-label="Documentation navigation">
        <p class="docs-sidebar-title">sqrail docs</p>
        <nav class="docs-global-nav" aria-label="Documentation pages">
          ${renderPageNavigation(page)}
        </nav>
      </aside>

      <article class="docs-content docs-reference-content">
        <p class="eyebrow"><span>${escapeHtml(page.category)}</span> sqrail docs</p>
        <h1>${title}</h1>
        <p class="docs-deck">${description}</p>
        <div class="docs-markdown">
          ${rendered.html}
        </div>
        <aside class="docs-source-note" aria-label="Machine-readable source">
          <div>
            <strong>Prefer plain text?</strong>
            <span>${sourceNote}</span>
          </div>
          <a href="${machineUrl}">Markdown source <span aria-hidden="true">→</span></a>
        </aside>
      </article>

      <aside class="docs-page-sidebar" aria-label="On this page">
        <p>On this page</p>
        <nav class="docs-section-nav" aria-label="Page sections">
          ${renderSectionNavigation(rendered.headings)}
        </nav>
      </aside>
    </main>

    <footer>
      <a class="wordmark" href="/">
        <span class="wordmark-mark" aria-hidden="true"></span>
        sqrail
      </a>
      <p>One query at a time.</p>
      <div>
        <a href="/docs/">Docs</a>
        <a href="https://github.com/yhay81/sqrail">Source</a>
        <a href="https://github.com/yhay81/sqrail/discussions">Community</a>
        <a href="https://github.com/yhay81/sqrail/releases">Releases</a>
      </div>
    </footer>
  </body>
</html>`;

  return prettier.format(html, { parser: "html" });
};

const stale = [];

for (const page of pages) {
  const source = await readFile(path.join(root, page.source), "utf8");
  const expected = await renderPage(page, source);
  const outputPath = path.join(root, page.output);

  if (checkOnly) {
    let current = "";
    try {
      current = await readFile(outputPath, "utf8");
    } catch {
      // Report missing generated pages with the same stale-file message.
    }
    if (current !== expected) stale.push(page.output);
    continue;
  }

  await mkdir(path.dirname(outputPath), { recursive: true });
  await writeFile(outputPath, expected);
  process.stdout.write(`generated ${page.output}\n`);
}

if (stale.length) {
  process.stderr.write(
    `Generated documentation is stale:\n${stale.map((file) => `  ${file}`).join("\n")}\nRun npm run docs:build.\n`,
  );
  process.exitCode = 1;
}
