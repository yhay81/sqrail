const terminal = document.querySelector(".terminal");
const stagedLines = [...document.querySelectorAll("[data-stage]")];
const reduceMotion = window.matchMedia(
  "(prefers-reduced-motion: reduce)",
).matches;

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
