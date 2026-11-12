import assert from "node:assert/strict";
import { access, readdir, readFile } from "node:fs/promises";
import path from "node:path";

const root = process.cwd();

const collectHtml = async (directory) => {
  const entries = await readdir(directory, { withFileTypes: true });
  const files = await Promise.all(
    entries.map(async (entry) => {
      const entryPath = path.join(directory, entry.name);
      if (entry.isDirectory()) {
        return collectHtml(entryPath);
      }
      return entry.name.endsWith(".html") ? [entryPath] : [];
    }),
  );
  return files.flat();
};

const fileExists = async (targetPath) => {
  try {
    await access(targetPath);
    return true;
  } catch {
    return false;
  }
};

const inOrder = (source, fragments, label) => {
  let offset = 0;
  for (const fragment of fragments) {
    const position = source.indexOf(fragment, offset);
    assert.notEqual(position, -1, `${label}: missing ${fragment}`);
    offset = position + fragment.length;
  }
};

const compact = (value) => value.replaceAll(/\s+/g, "");

const headerLinks = [
  'href="/docs/">Get started</a>',
  'href="/docs/agents/">Agent setup</a>',
  'href="/docs/reference/contract/">Reference</a>',
];

const footerLinks = [
  'href="/docs/">Docs</a>',
  'href="https://github.com/yhay81/sqrail">Source</a>',
  'href="https://github.com/yhay81/sqrail/discussions">Community</a>',
  'href="https://github.com/yhay81/sqrail/releases">Releases</a>',
];

const docsGroups = [
  ["Start", "Quick start", "Agent setup"],
  ["Use", "CLI contract", "Recipes", "Agent integration"],
  ["Operate", "Performance", "Benchmark policy", "Platforms", "Security"],
  ["Project", "Contributing", "Testing", "Support", "Code of Conduct"],
];

const docsSequence = [
  ["site/docs/index.html", "/docs/", "Quick start"],
  ["site/docs/agents/index.html", "/docs/agents/", "Agent setup"],
  [
    "site/docs/reference/contract/index.html",
    "/docs/reference/contract/",
    "CLI contract",
  ],
  [
    "site/docs/reference/recipes/index.html",
    "/docs/reference/recipes/",
    "Recipes",
  ],
  [
    "site/docs/reference/agents/index.html",
    "/docs/reference/agents/",
    "Agent integration",
  ],
  [
    "site/docs/reference/performance/index.html",
    "/docs/reference/performance/",
    "Performance baselines",
  ],
  [
    "site/docs/reference/benchmarks/index.html",
    "/docs/reference/benchmarks/",
    "Benchmark policy",
  ],
  [
    "site/docs/reference/platforms/index.html",
    "/docs/reference/platforms/",
    "Platform support",
  ],
  ["site/docs/security/index.html", "/docs/security/", "Security boundary"],
  ["site/docs/contributing/index.html", "/docs/contributing/", "Contributing"],
  [
    "site/docs/contributing/testing/index.html",
    "/docs/contributing/testing/",
    "Testing architecture",
  ],
  [
    "site/docs/contributing/support/index.html",
    "/docs/contributing/support/",
    "Support",
  ],
  [
    "site/docs/contributing/code-of-conduct/index.html",
    "/docs/contributing/code-of-conduct/",
    "Code of Conduct",
  ],
];

const allHtmlFiles = await collectHtml(path.join(root, "site"));
const htmlByPath = new Map();
const idsByPath = new Map();

for (const file of allHtmlFiles) {
  const source = await readFile(file, "utf8");
  const normalized = path.resolve(file);
  htmlByPath.set(normalized, source);
  idsByPath.set(
    normalized,
    new Set([...source.matchAll(/\bid="([^"]+)"/g)].map(([, id]) => id)),
  );
}

for (const [file, source] of htmlByPath.entries()) {
  const relative = path.relative(root, file);
  const withoutCurrentState = source.replaceAll(
    / aria-current="(?:page|location)"/g,
    "",
  );
  inOrder(
    compact(withoutCurrentState),
    headerLinks.map(compact),
    `${relative} header`,
  );
  inOrder(compact(source), footerLinks.map(compact), `${relative} footer`);
  assert.match(
    source,
    /<meta name="color-scheme" content="dark"\s*\/>/,
    `${relative}: missing color-scheme metadata`,
  );
  assert.match(
    source,
    /<meta name="text-scale" content="scale"\s*\/>/,
    `${relative}: missing text-scale metadata`,
  );
  assert.match(
    source,
    /popovertarget="site-menu"/,
    `${relative}: missing responsive navigation menu control`,
  );

  const copyButtons = [
    ...source.matchAll(/<button\b[^>]*\bcommand="--copy"[^>]*>/g),
  ].map(([tag]) => tag);
  const copyLabels = new Set();
  for (const buttonTag of copyButtons) {
    const label = buttonTag.match(/\baria-label="([^"]+)"/)?.[1];
    assert.ok(
      label && label.startsWith("Copy "),
      `${relative}: every copy button must have a disambiguating aria-label starting with "Copy "`,
    );
    assert.ok(
      !copyLabels.has(label),
      `${relative}: duplicate copy button aria-label "${label}"`,
    );
    copyLabels.add(label);
  }
}

const docsFiles = await collectHtml(path.join(root, "site/docs"));
assert.equal(
  docsFiles.length,
  docsSequence.length,
  "rendered documentation page count must match docsSequence",
);

const quickStart = await readFile(
  path.join(root, "site/docs/index.html"),
  "utf8",
);
const quickStartSections = [
  ...quickStart.matchAll(
    /<section id="([^"]+)"[^>]*>\s*<p class="section-label">(\d+) \/ ([^<]+)<\/p>/g,
  ),
].map(([, id]) => id);
const quickStartPageSections = [
  ...quickStart.matchAll(/<a href="#([^"]+)"(?: aria-current="location")?>/g),
].map(([, id]) => id);
assert.deepEqual(
  quickStartPageSections,
  quickStartSections,
  "quick-start table of contents must match every numbered section",
);

const routeByFile = new Map(
  docsSequence.map(([file, route]) => [path.join(root, file), route]),
);

for (const file of docsFiles) {
  const source = htmlByPath.get(path.resolve(file));
  const relative = path.relative(root, file);
  const expectedRoute = routeByFile.get(path.resolve(file));

  inOrder(
    compact(source),
    docsGroups.flatMap(([group, ...links]) => [
      compact(`<p>${group}</p>`),
      ...links.map((link) => compact(`>${link}</a>`)),
    ]),
    `${relative} documentation navigation`,
  );
  assert.match(
    source,
    /<a class="nav-install" href="\/docs\/">Install<\/a>/,
    `${relative}: Install must lead to the quick start`,
  );

  const sidebarMatch = source.match(
    /<aside class="docs-sidebar"[\s\S]*?<\/aside>/,
  );
  assert.ok(sidebarMatch, `${relative}: missing .docs-sidebar`);
  const sidebarCurrentPages = [
    ...sidebarMatch[0].matchAll(
      /<a\s+href="([^"]+)"\s+aria-current="page"\s*>/g,
    ),
  ].map(([, route]) => route);
  assert.deepEqual(
    sidebarCurrentPages,
    [expectedRoute],
    `${relative}: sidebar must highlight only ${expectedRoute}`,
  );
  assert.doesNotMatch(
    sidebarMatch[0],
    /aria-current="location"/,
    `${relative}: sidebar must not highlight a second link with aria-current="location"`,
  );

  inOrder(
    compact(source),
    [
      compact('<article class="docs-content docs-reference-content">'),
      compact('<div class="docs-page-meta">'),
      compact('<a class="docs-markdown-link"'),
      compact('<nav class="docs-pagination"'),
      compact('<aside class="docs-page-sidebar" aria-label="On this page">'),
      compact('<nav class="docs-section-nav" aria-label="Page sections">'),
    ],
    `${relative} document chrome`,
  );
}

for (const [index, [file, , title]] of docsSequence.entries()) {
  const source = htmlByPath.get(path.join(root, file));
  const previous = docsSequence[index - 1];
  const next = docsSequence[index + 1];

  if (previous) {
    assert.ok(
      compact(source).includes(
        compact(
          `class="docs-pagination-link docs-pagination-previous" href="${previous[1]}"`,
        ),
      ),
      `${file}: ${title} must link back to ${previous[2]}`,
    );
  }
  if (next) {
    assert.ok(
      compact(source).includes(
        compact(
          `class="docs-pagination-link docs-pagination-next" href="${next[1]}"`,
        ),
      ),
      `${file}: ${title} must link forward to ${next[2]}`,
    );
  }
}

for (const [file, source] of htmlByPath.entries()) {
  const relative = path.relative(root, file);
  for (const [, rawHref] of source.matchAll(/\bhref="([^"]+)"/g)) {
    if (
      rawHref.startsWith("https://") ||
      rawHref.startsWith("http://") ||
      rawHref.startsWith("mailto:")
    ) {
      if (
        rawHref.startsWith(
          "https://raw.githubusercontent.com/yhay81/sqrail/main/",
        )
      ) {
        const repoPath = rawHref.slice(
          "https://raw.githubusercontent.com/yhay81/sqrail/main/".length,
        );
        assert.ok(
          await fileExists(path.join(root, repoPath)),
          `${relative}: raw GitHub URL points to missing file ${repoPath}`,
        );
      }
      continue;
    }

    const hashIndex = rawHref.indexOf("#");
    const routePart = hashIndex === -1 ? rawHref : rawHref.slice(0, hashIndex);
    const fragment =
      hashIndex === -1 ? "" : decodeURIComponent(rawHref.slice(hashIndex + 1));

    let targetFile = file;
    if (routePart) {
      const cleanRoute = routePart.replace(/^\//, "");
      const candidate = routePart.endsWith("/")
        ? path.join(root, "site", cleanRoute, "index.html")
        : path.join(root, "site", cleanRoute);
      assert.ok(
        await fileExists(candidate),
        `${relative}: broken internal link ${rawHref}`,
      );
      targetFile = path.resolve(candidate);
    }

    if (fragment) {
      const targetIds = idsByPath.get(targetFile);
      assert.ok(
        targetIds?.has(fragment),
        `${relative}: missing fragment #${fragment} in ${path.relative(root, targetFile)}`,
      );
    }
  }
}

const [sitemap, llmsTxt] = await Promise.all([
  readFile(path.join(root, "site/sitemap.xml"), "utf8"),
  readFile(path.join(root, "site/llms.txt"), "utf8"),
]);

assert.ok(
  sitemap.includes("<loc>https://sqrail.yhay81.com/</loc>"),
  "site/sitemap.xml must include the homepage",
);
for (const [, route, title] of docsSequence) {
  const url = `https://sqrail.yhay81.com${route}`;
  assert.ok(
    sitemap.includes(`<loc>${url}</loc>`),
    `site/sitemap.xml is missing ${title} (${url})`,
  );
  assert.ok(
    llmsTxt.includes(url),
    `site/llms.txt is missing ${title} (${url})`,
  );
}
assert.doesNotMatch(
  llmsTxt,
  /https:\/\/github\.com\/yhay81\/sqrail\/blob\/main\//,
  "site/llms.txt should link to raw Markdown rather than GitHub blob HTML pages",
);
for (const [, repoPath] of llmsTxt.matchAll(
  /https:\/\/raw\.githubusercontent\.com\/yhay81\/sqrail\/main\/(\S+)/g,
)) {
  assert.ok(
    await fileExists(path.join(root, repoPath)),
    `site/llms.txt references missing raw source ${repoPath}`,
  );
}

console.log(
  `Navigation, internal links, sitemap, and llms.txt verified across ${allHtmlFiles.length} HTML files.`,
);
