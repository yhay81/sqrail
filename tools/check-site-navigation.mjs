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
  ["Project", "Contributing", "Support"],
];

const homepage = await readFile(path.join(root, "site/index.html"), "utf8");
inOrder(compact(homepage), headerLinks.map(compact), "site/index.html header");

const docsFiles = await collectHtml(path.join(root, "site/docs"));
assert.ok(docsFiles.length > 1, "expected rendered documentation pages");

const quickStart = await readFile(
  path.join(root, "site/docs/index.html"),
  "utf8",
);
const quickStartSteps = [
  ...quickStart.matchAll(/<a href="#([^"]+)"><span>(\d+)<\/span>([^<]+)<\/a>/g),
].map(([, id, number, label]) => ({ id, number, label }));
const quickStartSections = [
  ...quickStart.matchAll(
    /<section id="([^"]+)">\s*<p class="section-label">(\d+) \/ ([^<]+)<\/p>/g,
  ),
].map(([, id, number, label]) => ({ id, number, label }));
assert.deepEqual(
  quickStartSteps,
  quickStartSections,
  "quick-start index must match every numbered section",
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
}

console.log(
  `Navigation contract verified across the homepage and ${docsFiles.length} documentation pages.`,
);
