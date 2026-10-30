// Doomscroll Breaker Lock - background service worker.
// Asks the laptop helper (tools/doomscroll_keys.py) for the device state. The helper gets it from
// the ESP32 over the USB cable ("$STATE,<state>,<left>" once a second). Anything unexpected = locked.

const HELPER_URL = "http://127.0.0.1:47271/state";  // port must match LOCK_PORT in doomscroll_keys.py
const STALE_MS = 4000;  // no $STATE line for this long = device unplugged or reset

async function readState() {
  let res;
  try {
    res = await fetch(HELPER_URL, { cache: "no-store" });
  } catch (e) {
    return {
      allowed: false, reason: "helper",
      detail: "The Doomscroll helper isn't running."
    };
  }
  const data = await res.json().catch(() => null);
  if (!data || data.ageMs === null || data.ageMs === undefined) {
    return { allowed: false, reason: "never", detail: "Your Doomscroll Breaker isn't connected. Plug it in." };
  }
  const left = Number(data.left) || 0;
  if (data.ageMs > STALE_MS) {
    return { allowed: false, reason: "stale", left, detail: "Your Doomscroll Breaker isn't connected." };
  }
  if (data.state === "open" && left > 0) {
    return { allowed: true, reason: "open", left };
  }
  const why = {
    idle: "Press the button on your Doomscroll Breaker to open the app.",
    locked: "No videos left today. Hold the button to ask your friend for more.",
    waiting: "Waiting for your friend to approve more videos...",
    break: "Break in progress.",
    open: "No videos left today. Hold the button to ask your friend for more."
  };
  return { allowed: false, reason: data.state, left, detail: why[data.state] || "Locked." };
}

function setBadge(s) {
  const off = ["helper", "never", "stale"].includes(s.reason);
  chrome.action.setBadgeText({ text: off ? "OFF" : String(s.left ?? "") });
  chrome.action.setBadgeBackgroundColor({ color: s.allowed ? "#1f9d55" : "#d64545" });
}

chrome.runtime.onMessage.addListener((msg, _sender, reply) => {
  if (msg && msg.type === "getState") {
    readState().then((s) => {
      setBadge(s);
      reply(s);
    });
    return true;  // reply asynchronously
  }
});
