const terminal = document.querySelector(".terminal");
const stagedLines = [...document.querySelectorAll("[data-stage]")];

stagedLines.forEach((line, index) => {
  window.setTimeout(() => {
    line.style.opacity = "1";
    if (index === stagedLines.length - 1) terminal.classList.add("is-live");
  }, 180 + index * 210);
});

const reveal = new IntersectionObserver(
  (entries) => {
    entries.forEach((entry) => {
      if (entry.isIntersecting) {
        entry.target.classList.add("is-visible");
        reveal.unobserve(entry.target);
      }
    });
  },
  { threshold: 0.12 }
);

document.querySelectorAll(".reveal").forEach((element) => reveal.observe(element));

document.querySelectorAll("[data-copy-target]").forEach((button) => {
  button.addEventListener("click", async () => {
    const target = document.getElementById(button.dataset.copyTarget);
    if (!target) return;
    const text = target.textContent.replace(/^\$\s*/, "").trim();
    try {
      await navigator.clipboard.writeText(text);
      const previous = button.textContent;
      button.textContent = "Copied";
      window.setTimeout(() => {
        button.textContent = previous;
      }, 1400);
    } catch {
      button.textContent = "Select";
    }
  });
});
