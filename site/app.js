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

  const navInner = siteHeader.querySelector(".nav");
  const siteMenu = document.getElementById("site-menu");
  const menuButton = siteHeader.querySelector(".nav-menu-button");
  const supportsPopover = Object.hasOwn(HTMLElement.prototype, "popover");

  if (navInner && siteMenu && menuButton) {
    if (!supportsPopover) {
      menuButton.addEventListener("click", () => {
        if (siteHeader.classList.contains("is-wide")) return;
        siteMenu.classList.toggle(":popover-open");
      });
      document.addEventListener("keydown", (event) => {
        if (event.key === "Escape") {
          siteMenu.classList.remove(":popover-open");
        }
      });
      document.addEventListener("click", (event) => {
        if (
          !siteHeader.classList.contains("is-wide") &&
          !siteMenu.contains(event.target) &&
          !menuButton.contains(event.target)
        ) {
          siteMenu.classList.remove(":popover-open");
        }
      });
    }

    let lastHeaderWidth = 0;
    let isCurrentlyWide = false;

    const syncHeaderLayout = (force = false) => {
      const currentWidth = navInner.clientWidth;
      if (!force && !isCurrentlyWide && currentWidth <= lastHeaderWidth) {
        lastHeaderWidth = currentWidth;
        return;
      }
      lastHeaderWidth = currentWidth;

      const wasOpen = supportsPopover
        ? siteMenu.matches(":popover-open")
        : siteMenu.classList.contains(":popover-open");

      siteHeader.classList.add("is-measuring", "is-wide");
      siteMenu.removeAttribute("popover");
      siteMenu.classList.remove(":popover-open");

      const fits = navInner.scrollWidth <= navInner.clientWidth;
      isCurrentlyWide = fits;
      siteHeader.classList.toggle("is-wide", fits);

      if (!fits) {
        siteMenu.setAttribute("popover", "auto");
        if (wasOpen) {
          if (supportsPopover) siteMenu.showPopover();
          else siteMenu.classList.add(":popover-open");
        }
      }

      window.requestAnimationFrame(() => {
        siteHeader.classList.remove("is-measuring");
      });
    };

    syncHeaderLayout(true);

    if ("ResizeObserver" in window) {
      const observer = new ResizeObserver(() => syncHeaderLayout());
      observer.observe(siteHeader);
      observer.observe(siteMenu);
    } else {
      window.addEventListener("resize", () => syncHeaderLayout(true));
    }

    siteMenu.addEventListener("click", (event) => {
      if (!event.target.closest("a")) return;
      if (!isCurrentlyWide) {
        if (supportsPopover && siteMenu.matches(":popover-open")) {
          siteMenu.hidePopover();
        } else {
          siteMenu.classList.remove(":popover-open");
        }
      }
    });
  }
}

const sidebarToggle = document.querySelector("[data-docs-sidebar-toggle]");
const docsGlobalNav = document.getElementById("docs-global-nav");
if (sidebarToggle && docsGlobalNav) {
  const compactDocsMedia = window.matchMedia("(max-width: 42.5em)");
  const syncDocsSidebar = (collapseIfCompact = false) => {
    if (!compactDocsMedia.matches) {
      docsGlobalNav.hidden = false;
      sidebarToggle.setAttribute("aria-expanded", "true");
    } else if (collapseIfCompact) {
      docsGlobalNav.hidden = true;
      sidebarToggle.setAttribute("aria-expanded", "false");
    }
  };

  syncDocsSidebar(true);
  compactDocsMedia.addEventListener("change", () => syncDocsSidebar(true));
  sidebarToggle.addEventListener("click", () => {
    const expanded = sidebarToggle.getAttribute("aria-expanded") === "true";
    docsGlobalNav.hidden = expanded;
    sidebarToggle.setAttribute("aria-expanded", String(!expanded));
  });
}

const docsSectionNav = document.querySelector(".docs-section-nav");
const docsSectionLinks = [
  ...document.querySelectorAll('.docs-section-nav a[href^="#"]'),
];
if (docsSectionNav && docsSectionLinks.length) {
  const docsSections = docsSectionLinks
    .map((link) => ({
      link,
      section: document.getElementById(decodeURIComponent(link.hash.slice(1))),
    }))
    .filter(({ section }) => section);

  docsSectionNav.dataset.scrollspyReady = "";
  const supportsScrollTargetGroup =
    typeof CSS !== "undefined" && CSS.supports("scroll-target-group: auto");

  if (supportsScrollTargetGroup) {
    const syncNativeAriaCurrent = () => {
      const currentLink =
        docsSectionNav.querySelector("a:target-current") ?? docsSectionLinks[0];
      docsSectionLinks.forEach((link) => {
        if (link === currentLink) {
          link.setAttribute("aria-current", "location");
        } else {
          link.removeAttribute("aria-current");
        }
      });
    };
    syncNativeAriaCurrent();
    document.addEventListener("scrollend", syncNativeAriaCurrent, {
      passive: true,
    });
    window.addEventListener("hashchange", syncNativeAriaCurrent);
  } else if (docsSections.length) {
    let docsFrame = 0;

    const syncDocsNavigation = () => {
      const headerHeight =
        Number.parseFloat(
          getComputedStyle(document.documentElement).getPropertyValue(
            "--header-height",
          ),
        ) || 78;
      const readingLine =
        window.scrollY + headerHeight + window.innerHeight * 0.28;
      const atBottom =
        window.innerHeight + window.scrollY >=
        document.documentElement.scrollHeight - 8;
      let current = docsSections[0];

      docsSections.forEach((candidate) => {
        const top =
          window.scrollY + candidate.section.getBoundingClientRect().top;
        if (top <= readingLine) current = candidate;
      });

      if (atBottom) {
        current = docsSections[docsSections.length - 1];
      }

      if (window.location.hash) {
        const hashed = docsSections.find(
          ({ link }) => link.hash === window.location.hash,
        );
        if (hashed) {
          const rect = hashed.section.getBoundingClientRect();
          if (rect.top >= 0 && rect.top <= window.innerHeight * 0.65) {
            current = hashed;
          }
        }
      }

      docsSections.forEach((candidate) => {
        const active = candidate === current;
        candidate.link.classList.toggle("is-current", active);
        if (active) candidate.link.setAttribute("aria-current", "location");
        else candidate.link.removeAttribute("aria-current");
      });
      docsFrame = 0;
    };

    const scheduleDocsNavigation = () => {
      if (!docsFrame) {
        docsFrame = window.requestAnimationFrame(syncDocsNavigation);
      }
    };

    syncDocsNavigation();
    window.addEventListener("scroll", scheduleDocsNavigation, {
      passive: true,
    });
    window.addEventListener("resize", scheduleDocsNavigation);
    window.addEventListener("hashchange", scheduleDocsNavigation);
  }
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

const hasFocusableChild = (element) =>
  Boolean(
    element.querySelector(
      'a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])',
    ),
  );

const setupTabs = (
  buttonSelector,
  panelSelector,
  tabKey,
  panelKey,
  onActivate,
) => {
  const buttons = [...document.querySelectorAll(buttonSelector)];
  const panels = [...document.querySelectorAll(panelSelector)];
  if (!buttons.length || !panels.length) return null;

  const activate = (value, { moveFocus = false, scrollTab = false } = {}) => {
    buttons.forEach((button) => {
      const selected = button.dataset[tabKey] === value;
      button.setAttribute("aria-selected", String(selected));
      button.tabIndex = selected ? 0 : -1;
      if (selected && moveFocus) button.focus();
      if (selected && (moveFocus || scrollTab)) {
        button.scrollIntoView({
          block: "nearest",
          inline: "nearest",
          container: "nearest",
        });
      }
    });

    let activePanel = null;
    panels.forEach((panel) => {
      const selected = panel.dataset[panelKey] === value;
      if (selected) {
        activePanel = panel;
        panel.removeAttribute("hidden");
        if (!hasFocusableChild(panel)) panel.tabIndex = 0;
        else panel.removeAttribute("tabindex");
      } else {
        panel.setAttribute("hidden", "until-found");
        panel.removeAttribute("tabindex");
      }
    });

    onActivate?.(value, activePanel);
  };

  buttons.forEach((button, index) => {
    button.addEventListener("click", () =>
      activate(button.dataset[tabKey], { scrollTab: true }),
    );
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
      activate(buttons[nextIndex].dataset[tabKey], { moveFocus: true });
    });
  });

  panels.forEach((panel) => {
    panel.addEventListener("beforematch", () => {
      activate(panel.dataset[panelKey], { scrollTab: true });
    });
  });

  return activate;
};

const bottomInstallCode = document.getElementById("install-code");
const syncBottomInstall = (_value, activePanel) => {
  if (!bottomInstallCode || !activePanel) return;
  const sourceCode = activePanel.querySelector("code");
  if (!sourceCode) return;
  const copyValue = sourceCode.dataset.copyValue;
  const promptSymbol = sourceCode.dataset.prompt ?? "$";
  if (!copyValue) return;
  bottomInstallCode.dataset.copyValue = copyValue;
  const displayText = sourceCode.textContent
    .replace(/^[$>]\s*/, "")
    .replaceAll(/\s+/g, " ")
    .trim();
  const promptSpan = document.createElement("span");
  promptSpan.textContent = promptSymbol;
  bottomInstallCode.replaceChildren(
    promptSpan,
    document.createTextNode(` ${displayText}`),
  );
};

const activateInstall = setupTabs(
  "[data-install-tab]",
  "[data-install-panel]",
  "installTab",
  "installPanel",
  syncBottomInstall,
);
setupTabs(
  "[data-terminal-tab]",
  "[data-terminal-panel]",
  "terminalTab",
  "terminalPanel",
);

if (activateInstall) {
  const platform =
    navigator.userAgentData?.platform ?? navigator.platform ?? "";
  const ua = navigator.userAgent ?? "";
  if (/win/i.test(platform)) {
    activateInstall("windows");
  } else if (/linux/i.test(platform) && !/android/i.test(ua)) {
    const archHint = `${platform} ${ua}`;
    activateInstall(
      /\b(?:aarch64|arm64)\b/i.test(archHint) ? "linux-arm64" : "linux",
    );
  }
}

stagedLines.forEach((line, index) => {
  const delay = reduceMotion ? 0 : 180 + index * 210;
  window.setTimeout(() => {
    line.classList.add("is-staged");
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

const copyTimers = new WeakMap();
let lastCopyMessage = "";

const reportCopy = (button, ok) => {
  const status = button.nextElementSibling;
  window.clearTimeout(copyTimers.get(button));
  button.removeAttribute("data-copied");
  button.removeAttribute("data-failed");

  let message = ok ? "Copied" : "Copy failed";
  if (message === lastCopyMessage) message += "\u00A0";
  lastCopyMessage = message;

  if (status) status.textContent = "";
  void button.offsetWidth;
  button.setAttribute(ok ? "data-copied" : "data-failed", "");
  if (status) {
    window.requestAnimationFrame(() => {
      status.textContent = message;
    });
  }

  copyTimers.set(
    button,
    window.setTimeout(() => {
      button.removeAttribute("data-copied");
      button.removeAttribute("data-failed");
      if (status) status.textContent = "";
    }, 1800),
  );
};

const copyTargetText = async (button, target) => {
  if (!button || !target) return;
  const raw =
    target.dataset.copyValue ??
    (target.matches("input, textarea")
      ? target.value
      : target.innerText.replace(/^[$>]\s*/, ""));
  const text = raw.trim();
  if (!text) return;

  try {
    await navigator.clipboard.writeText(text);
    reportCopy(button, true);
  } catch {
    reportCopy(button, false);
  }
};

if ("CommandEvent" in window) {
  document.addEventListener(
    "command",
    (event) => {
      if (event.command === "--copy") {
        copyTargetText(event.source, event.target);
      }
    },
    true,
  );
} else {
  document.addEventListener("click", (event) => {
    const button = event.target.closest('button[command="--copy"]');
    if (!button) return;
    const targetId = button.getAttribute("commandfor");
    const target = targetId ? document.getElementById(targetId) : null;
    if (target) copyTargetText(button, target);
  });
}
