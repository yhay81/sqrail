const terminal = document.querySelector(".terminal");
const stagedLines = [...document.querySelectorAll("[data-stage]")];
const reduceMotion = window.matchMedia(
  "(prefers-reduced-motion: reduce)",
).matches;

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
    const text = target.textContent.replace(/^\$\s*/, "").trim();
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
