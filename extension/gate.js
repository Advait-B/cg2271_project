// Doomscroll Breaker Lock - runs on tiktok.com and instagram.com.
// The page starts covered (fail closed). Every 1.5 s it asks the background worker whether the
// device is plugged in, open and has videos left; only then is the cover removed.

(() => {
  const POLL_MS = 1500;
  let allowed = false;

  const cover = document.createElement("div");
  cover.id = "doomscroll-breaker-lock";
  cover.innerHTML = `
    <div class="dsb-card">
      <div class="dsb-icon">
        <svg viewBox="0 0 24 24" width="26" height="26" fill="none" stroke="currentColor"
             stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
          <rect x="4" y="11" width="16" height="10" rx="2.5"></rect>
          <path d="M8 11V7.5a4 4 0 0 1 8 0V11"></path>
        </svg>
      </div>
      <div class="dsb-title">Locked</div>
      <div class="dsb-detail">Checking your device...</div>
      <div class="dsb-left"></div>
      <div class="dsb-brand">Doomscroll Breaker</div>
    </div>`;
  const detail = cover.querySelector(".dsb-detail");
  const leftLine = cover.querySelector(".dsb-left");

  function attachCover() {
    const root = document.documentElement;
    if (root && !cover.isConnected) {
      root.appendChild(cover);
    }
  }

  function silenceMedia() {
    document.querySelectorAll("video, audio").forEach((m) => {
      if (!m.paused) m.pause();
      m.muted = true;
    });
  }

  function lock(message, left) {
    allowed = false;
    detail.textContent = message;
    leftLine.textContent = typeof left === "number" ? `${left} video${left === 1 ? "" : "s"} left today` : "";
    leftLine.style.display = typeof left === "number" ? "" : "none";
    attachCover();
    silenceMedia();
  }

  function unlock() {
    if (!allowed) {
      // undo the mute we forced while locked
      document.querySelectorAll("video, audio").forEach((m) => { m.muted = false; });
    }
    allowed = true;
    cover.remove();
  }

  // any video that tries to start while locked gets stopped straight away
  document.addEventListener("play", (e) => {
    if (!allowed && e.target instanceof HTMLMediaElement) {
      e.target.pause();
      e.target.muted = true;
    }
  }, true);

  // keyboard / wheel / touch scrolling is swallowed while locked
  for (const type of ["wheel", "touchmove", "keydown"]) {
    window.addEventListener(type, (e) => {
      if (!allowed) {
        e.stopImmediatePropagation();
        e.preventDefault();
      }
    }, { capture: true, passive: false });
  }

  // the site's own scripts sometimes rebuild <html>/<body>; put the cover back if it disappears
  new MutationObserver(() => { if (!allowed) attachCover(); })
    .observe(document, { childList: true, subtree: true });

  function check() {
    try {
      chrome.runtime.sendMessage({ type: "getState" }, (s) => {
        if (chrome.runtime.lastError || !s) {
          lock("Lock extension not responding - reload the page.");
          return;
        }
        if (s.allowed) unlock();
        else lock(s.detail, s.left);
      });
    } catch (e) {
      // extension was reloaded or removed; stay locked until the page is refreshed
      lock("Lock extension was reloaded - refresh the page.");
    }
  }

  lock("Checking your device...");
  check();
  setInterval(() => {
    check();
    if (!allowed) silenceMedia();
  }, POLL_MS);
})();
