const terminal = document.querySelector(".terminal");
const stagedLines = [...document.querySelectorAll("[data-stage]")];
const reduceMotion = window.matchMedia(
  "(prefers-reduced-motion: reduce)",
).matches;

const siteHeader = document.querySelector("[data-site-header]");
if (siteHeader) {
  let headerFrame = 0;
  const syncHeader = () => {
    siteHeader.classList.toggle("is-scrolled", window.scrollY > 12);
    headerFrame = 0;
  };
  syncHeader();
  window.addEventListener(
    "scroll",
    () => {
      if (!headerFrame) headerFrame = window.requestAnimationFrame(syncHeader);
    },
    { passive: true },
  );
}

const docsSidebarLinks = [
  ...document.querySelectorAll('.docs-sidebar a[href^="#"]'),
];
if (docsSidebarLinks.length) {
  const docsSections = docsSidebarLinks
    .map((link) => ({ link, section: document.querySelector(link.hash) }))
    .filter(({ section }) => section);
  let docsFrame = 0;

  const syncDocsNavigation = () => {
    const headerHeight = Number.parseFloat(
      getComputedStyle(document.documentElement).getPropertyValue(
        "--header-height",
      ),
    );
    const readingLine =
      window.scrollY + headerHeight + window.innerHeight * 0.35;
    let current = docsSections[0];

    docsSections.forEach((candidate) => {
      if (candidate.section.offsetTop <= readingLine) current = candidate;
    });
    docsSidebarLinks.forEach((link) => link.removeAttribute("aria-current"));
    current?.link.setAttribute("aria-current", "location");
    docsFrame = 0;
  };

  const scheduleDocsNavigation = () => {
    if (!docsFrame) {
      docsFrame = window.requestAnimationFrame(syncDocsNavigation);
    }
  };

  syncDocsNavigation();
  window.addEventListener("scroll", scheduleDocsNavigation, { passive: true });
  window.addEventListener("resize", scheduleDocsNavigation);
}

const githubStarNodes = [...document.querySelectorAll("[data-github-stars]")];
const githubRepoLinks = [...document.querySelectorAll("[data-github-repo]")];
if (githubStarNodes.length) {
  const renderGithubStars = (count) => {
    const formatted =
      count >= 1000
        ? new Intl.NumberFormat("en", {
            notation: "compact",
            maximumFractionDigits: 1,
          }).format(count)
        : new Intl.NumberFormat("en").format(count);
    githubStarNodes.forEach((node) => {
      node.textContent = formatted;
    });
    githubRepoLinks.forEach((link) => {
      link.setAttribute(
        "aria-label",
        `View yhay81/sqrail on GitHub — ${new Intl.NumberFormat("en").format(count)} ${count === 1 ? "star" : "stars"}`,
      );
      link.classList.remove("is-live");
      window.requestAnimationFrame(() => link.classList.add("is-live"));
    });
  };

  const controller = new AbortController();
  const githubTimeout = window.setTimeout(() => controller.abort(), 4000);
  fetch("https://api.github.com/repos/yhay81/sqrail", {
    headers: { Accept: "application/vnd.github+json" },
    signal: controller.signal,
  })
    .then((response) => {
      if (!response.ok) throw new Error(`GitHub API ${response.status}`);
      return response.json();
    })
    .then((repository) => {
      if (Number.isSafeInteger(repository.stargazers_count)) {
        renderGithubStars(repository.stargazers_count);
      }
    })
    .catch(() => {})
    .finally(() => window.clearTimeout(githubTimeout));
}

const setupTabs = (buttonSelector, panelSelector, tabKey, panelKey) => {
  const buttons = [...document.querySelectorAll(buttonSelector)];
  const panels = [...document.querySelectorAll(panelSelector)];
  if (!buttons.length || !panels.length) return null;

  const activate = (value, moveFocus = false) => {
    buttons.forEach((button) => {
      const selected = button.dataset[tabKey] === value;
      button.setAttribute("aria-selected", String(selected));
      button.tabIndex = selected ? 0 : -1;
      if (selected && moveFocus) button.focus();
    });
    panels.forEach((panel) => {
      panel.hidden = panel.dataset[panelKey] !== value;
    });
  };

  buttons.forEach((button, index) => {
    button.addEventListener("click", () => activate(button.dataset[tabKey]));
    button.addEventListener("keydown", (event) => {
      const direction = event.key === "ArrowRight" ? 1 : -1;
      if (!["ArrowLeft", "ArrowRight", "Home", "End"].includes(event.key)) {
        return;
      }
      event.preventDefault();
      let nextIndex = index + direction;
      if (event.key === "Home") nextIndex = 0;
      if (event.key === "End") nextIndex = buttons.length - 1;
      nextIndex = (nextIndex + buttons.length) % buttons.length;
      activate(buttons[nextIndex].dataset[tabKey], true);
    });
  });

  return activate;
};

const activateInstall = setupTabs(
  "[data-install-tab]",
  "[data-install-panel]",
  "installTab",
  "installPanel",
);
setupTabs(
  "[data-terminal-tab]",
  "[data-terminal-panel]",
  "terminalTab",
  "terminalPanel",
);

if (activateInstall) {
  const platform = navigator.userAgentData?.platform ?? navigator.platform;
  if (/win/i.test(platform)) activateInstall("windows");
  if (/linux/i.test(platform)) activateInstall("linux");
}

stagedLines.forEach((line, index) => {
  const delay = reduceMotion ? 0 : 180 + index * 210;
  window.setTimeout(() => {
    line.style.opacity = "1";
    if (index === stagedLines.length - 1 && terminal) {
      terminal.classList.add("is-live");
    }
  }, delay);
});

const revealElements = document.querySelectorAll(".reveal");
if ("IntersectionObserver" in window && !reduceMotion) {
  const reveal = new IntersectionObserver(
    (entries) => {
      entries.forEach((entry) => {
        if (entry.isIntersecting) {
          entry.target.classList.add("is-visible");
          reveal.unobserve(entry.target);
        }
      });
    },
    { threshold: 0.12 },
  );
  revealElements.forEach((element) => reveal.observe(element));
} else {
  revealElements.forEach((element) => element.classList.add("is-visible"));
}

document.querySelectorAll("[data-copy-target]").forEach((button) => {
  button.addEventListener("click", async () => {
    const target = document.getElementById(button.dataset.copyTarget);
    if (!target) return;
    const text = (
      target.dataset.copyValue ?? target.textContent.replace(/^[$>]\s*/, "")
    ).trim();
    try {
      await navigator.clipboard.writeText(text);
      const previous = button.textContent;
      button.textContent = "Copied";
      button.setAttribute("aria-live", "polite");
      window.setTimeout(() => {
        button.textContent = previous;
      }, 1400);
    } catch {
      button.textContent = "Select";
    }
  });
});
