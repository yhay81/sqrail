import assert from "node:assert/strict";
import { readdir, readFile } from "node:fs/promises";
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

const docsGroups = [
  ["Start", "Quick start", "Agent setup"],
  ["Use", "CLI contract", "Agent integration"],
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

const homepage = await readFile(path.join(root, "site/index.html"), "utf8");
inOrder(compact(homepage), headerLinks.map(compact), "site/index.html header");

const docsFiles = await collectHtml(path.join(root, "site/docs"));
assert.ok(docsFiles.length > 1, "expected rendered documentation pages");

const quickStart = await readFile(
  path.join(root, "site/docs/index.html"),
  "utf8",
);
const quickStartSections = [
  ...quickStart.matchAll(
    /<section id="([^"]+)">\s*<p class="section-label">(\d+) \/ ([^<]+)<\/p>/g,
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

for (const file of docsFiles) {
  const source = await readFile(file, "utf8");
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
  const source = await readFile(path.join(root, file), "utf8");
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

console.log(
  `Navigation contract verified across the homepage and ${docsFiles.length} documentation pages.`,
);
