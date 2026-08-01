import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import path from "node:path";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const read = (relative) => readFile(path.join(root, relative), "utf8");

const [
  skill,
  metadata,
  pluginText,
  packageText,
  bootstrap,
  homepage,
  readme,
  discovery,
] = await Promise.all([
  read("skills/sqrail/SKILL.md"),
  read("skills/sqrail/agents/openai.yaml"),
  read("plugin.json"),
  read("package.json"),
  read("site/install-agent.md"),
  read("site/index.html"),
  read("README.md"),
  read("site/llms.txt"),
]);

const frontmatter = skill.match(/^---\n([\s\S]*?)\n---\n/);
assert(frontmatter, "skills/sqrail/SKILL.md must start with YAML frontmatter");

const fields = frontmatter[1]
  .split("\n")
  .filter((line) => /^[a-z][a-z-]*:/.test(line))
  .map((line) => line.slice(0, line.indexOf(":")));
assert.deepEqual(
  fields,
  ["name", "description"],
  "Skill frontmatter must contain only name and description",
);

const name = frontmatter[1].match(/^name:\s*(.+)$/m)?.[1];
const description = frontmatter[1].match(/^description:\s*(.+)$/m)?.[1];
assert.equal(name, "sqrail", "Skill name must match its directory");
assert(
  /^[a-z0-9]+(?:-[a-z0-9]+)*$/.test(name),
  "Skill name must follow the Agent Skills naming rules",
);
assert(
  description && description.length <= 1024,
  "Skill description must contain 1–1024 characters",
);
assert(
  skill.split("\n").length <= 500,
  "Skill instructions must remain under 500 lines",
);

assert.match(metadata, /default_prompt: "Use \$sqrail /);
assert.match(metadata, /short_description: ".{25,64}"/);

const plugin = JSON.parse(pluginText);
const packageManifest = JSON.parse(packageText);
assert.equal(plugin.name, "sqrail");
assert.equal(
  plugin.version,
  packageManifest.version,
  "AGY plugin and project versions must match",
);
assert.equal(plugin.repository, "https://github.com/yhay81/sqrail");

const homepageVersions = [
  ...homepage.matchAll(
    /(?:sqrail-v|\/releases\/(?:tag|download)\/v|"softwareVersion": "|"sqrail_version":"|>v)(\d+\.\d+\.\d+)/g,
  ),
].map((match) => match[1]);
assert(
  homepageVersions.length > 0,
  "site/index.html must state the release version",
);
for (const found of new Set(homepageVersions)) {
  assert.equal(
    found,
    plugin.version,
    `site/index.html still references version ${found}`,
  );
}

for (const required of [
  "brew install yhay81/tap/sqrail",
  "winget show --id yhay81.sqrail --exact",
  "agy plugin install https://github.com/yhay81/sqrail",
  "gh skill preview yhay81/sqrail sqrail",
  "gh skill install yhay81/sqrail sqrail --agent AGENT --scope user",
  "npx skills add yhay81/sqrail --skill sqrail --agent AGENT --global --yes",
  "https://github.com/yhay81/sqrail/tree/main/skills/sqrail",
]) {
  assert(
    bootstrap.includes(required),
    `install-agent.md is missing: ${required}`,
  );
}
assert(
  !/gemini\s+(?:cli|skills\s+install)/i.test(bootstrap),
  "install-agent.md must use the current AGY/Antigravity route",
);
assert(
  !/(?:curl|wget)[^\n]*\|\s*(?:ba)?sh/.test(bootstrap),
  "install-agent.md must not pipe remote content into a shell",
);

for (const [document, label] of [
  [homepage, "site/index.html"],
  [readme, "README.md"],
  [discovery, "site/llms.txt"],
]) {
  assert(
    document.includes("https://sqrails.yhay81.com/install-agent.md"),
    `${label} must link to the stable agent installer`,
  );
}

console.log("public Agent Skill and installation routes are consistent");
