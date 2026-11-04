#!/usr/bin/env node
// probe.js — drive a real Google Chrome over CDP with no npm dependencies.
//
// Why this exists: the Freebuff preview browser sends a `Freebuff/... Electron`
// User-Agent, and setup.rbxcdn.com answers 403 AccessDenied to it, so the
// official web client can never load its engine there. This launches the
// system Chrome with a stock UA, injects the .ROBLOSECURITY cookie the user
// keeps in ~/mizg.txt or ~/king.txt, navigates, and records what actually
// happened on the wire.
//
// usage:
//   node --experimental-websocket tools/cdp/probe.js \
//     --url 'https://play.roblox.com/games/start?placeId=1818' \
//     --cookie ~/mizg.txt --seconds 75 --out /tmp/cdp_1818.json \
//     [--shot /tmp/1818.png] [--headed]
//
// Everything is captured as JSON: console/log lines in arrival order, every
// request to a Roblox API host with its postData and response body, every
// WebTransport create/established/close with its session id, plus failures.
const fs = require("fs");
const os = require("os");
const path = require("path");
const { spawn } = require("child_process");

const UA =
  "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) " +
  "Chrome/155.0.0.0 Safari/537.36";

function arg(name, def) {
  const i = process.argv.indexOf("--" + name);
  if (i === -1) return def;
  const v = process.argv[i + 1];
  return v && !v.startsWith("--") ? v : true;
}

const URL_ARG = arg("url", "https://play.roblox.com/games/start?placeId=1818");
const SECONDS = Number(arg("seconds", 75));
const OUT = arg("out", "/tmp/cdp_probe.json");
const SHOT = arg("shot", null);
const HEADED = Boolean(arg("headed", false));
const PORT = Number(arg("port", 9222));
const COOKIE_FILE = arg("cookie", path.join(os.homedir(), "mizg.txt"));

const PROFILE = fs.mkdtempSync(path.join(os.tmpdir(), "cdp-prof-"));

const chromeArgs = [
  HEADED ? "--new-window" : "--headless=new",
  `--remote-debugging-port=${PORT}`,
  `--user-data-dir=${PROFILE}`,
  "--no-first-run",
  "--no-default-browser-check",
  "--disable-dev-shm-usage",
  "--no-sandbox",
  "--enable-unsafe-swiftshader",
  "--use-angle=swiftshader",
  "--autoplay-policy=no-user-gesture-required",
  `--user-agent=${UA}`,
  "--window-size=1280,800",
  ...(process.env.CHROME_FLAGS ? process.env.CHROME_FLAGS.split(/\s+/).filter(Boolean) : []),
  "about:blank",
];

const chrome = spawn("google-chrome", chromeArgs, { stdio: ["ignore", "ignore", "pipe"] });
chrome.stderr.on("data", (d) => process.stderr.write("[chrome] " + d));
const cleanup = () => {
  try { chrome.kill("SIGTERM"); } catch (_) {}
};
process.on("exit", cleanup);

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function versionInfo() {
  for (let i = 0; i < 100; i++) {
    try {
      const r = await fetch(`http://127.0.0.1:${PORT}/json/version`);
      if (r.ok) return r.json();
    } catch (_) {}
    await sleep(100);
  }
  throw new Error("chrome did not open the debugging port");
}

// ---- minimal CDP client ----
class Cdp {
  constructor(ws) {
    this.ws = ws;
    this.id = 0;
    this.pending = new Map();
    this.handlers = new Map();
    ws.addEventListener("message", (ev) => {
      const msg = JSON.parse(ev.data);
      if (msg.id && this.pending.has(msg.id)) {
        const { res, rej } = this.pending.get(msg.id);
        this.pending.delete(msg.id);
        msg.error ? rej(new Error(JSON.stringify(msg.error))) : res(msg.result);
      } else if (msg.method) {
        // Pass the sessionId so callers can issue follow-ups (e.g. read a body)
        // against the worker that owns the request, not just the page.
        (this.handlers.get(msg.method) || []).forEach((h) => h(msg.params, msg.sessionId));
      }
    });
  }
  send(method, params = {}, sessionId) {
    const id = ++this.id;
    const payload = { id, method, params };
    if (sessionId) payload.sessionId = sessionId;
    this.ws.send(JSON.stringify(payload));
    return new Promise((res, rej) => {
      this.pending.set(id, { res, rej });
      // A destroyed target never answers; never let one call stall the run.
      setTimeout(() => {
        if (this.pending.delete(id)) rej(new Error("timeout " + method));
      }, 8000).unref();
    });
  }
  on(method, fn) {
    if (!this.handlers.has(method)) this.handlers.set(method, []);
    this.handlers.get(method).push(fn);
  }
}

const INTERESTING =
  /(gamejoin|gamejoin\.roblox|qdmux|setup\.rbxcdn|clientsettings|apis\.roblox|games\.roblox|users\.roblox|assetdelivery|data\.rbxcdn|ecsv2|metrics)/i;

(async () => {
  const info = await versionInfo();
  const ws = new WebSocket(info.webSocketDebuggerUrl);
  await new Promise((res, rej) => {
    ws.addEventListener("open", res, { once: true });
    ws.addEventListener("error", rej, { once: true });
  });
  const cdp = new Cdp(ws);
  await cdp.send("Browser.setDownloadBehavior", { behavior: "deny" }).catch(() => {});

  const { targetId } = await cdp.send("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await cdp.send("Target.attachToTarget", { targetId, flatten: true });
  const send = (m, p) => cdp.send(m, p, sessionId);

  const events = [];
  const net = new Map(); // requestId -> request record
  // Append-only JSONL alongside the final dump, so a hung run still has evidence.
  const JL = OUT.replace(/\.json$/, "") + ".jsonl";
  try { fs.unlinkSync(JL); } catch (_) {}
  const push = (kind, data) => {
    const e = { t: Date.now(), kind, ...data };
    events.push(e);
    try { fs.appendFileSync(JL, JSON.stringify(e) + "\n"); } catch (_) {}
  };
  const flush = () => {
    try { fs.writeFileSync(OUT, JSON.stringify(events, null, 1)); } catch (_) {}
  };
  const watchdog = setTimeout(() => { flush(); console.error("watchdog: flushing partial capture"); process.exit(3); }, (SECONDS + 25) * 1000);
  watchdog.unref();

  cdp.on("Target.attachedToTarget", () => {});
  // Session-scoped events arrive with sessionId; the flat handler still gets them.
  const onSession = (method, fn) => {
    if (!cdp.handlers.has(method)) cdp.handlers.set(method, []);
    cdp.handlers.get(method).push((p) => {
      // Only react to our own page target (workers get their own sessions).
      fn(p);
    });
  };

  onSession("Runtime.consoleAPICalled", (p) => {
    const text = (p.args || [])
      .map((a) => (a.value !== undefined ? a.value : a.description || a.unserializableValue || ""))
      .join(" ");
    push("console", { level: p.type, text });
  });
  onSession("Log.entryAdded", (p) => {
    push("log", { level: p.entry.level, text: p.entry.text, url: p.entry.url });
  });
  onSession("Runtime.exceptionThrown", (p) => {
    const d = p.exceptionDetails || {};
    push("exception", { text: d.text, desc: d.exception && d.exception.description });
  });
  onSession("Network.requestWillBeSent", (p) => {
    const url = p.request.url;
    net.set(p.requestId, {
      url,
      method: p.request.method,
      postData: p.request.postData,
      headers: p.request.headers,
      type: p.type,
      startedAt: Date.now(),
    });
    if (INTERESTING.test(url)) {
      push("request", {
        url,
        method: p.request.method,
        type: p.type,
        postData: p.request.postData ? p.request.postData.slice(0, 4000) : undefined,
        headers: p.request.headers,
      });
    }
  });
  onSession("Network.responseReceived", (p) => {
    const rec = net.get(p.requestId);
    if (!rec) return;
    rec.status = p.response.status;
    rec.mime = p.response.mimeType;
    if (INTERESTING.test(rec.url)) {
      push("response", { url: rec.url, status: p.response.status, mime: p.response.mimeType });
    }
  });
  onSession("Network.loadingFinished", async (p, eventSessionId) => {
    const rec = net.get(p.requestId);
    if (!rec) return;
    if (!/(gamejoin|\/join|join-game|joinattempt)/i.test(rec.url)) return;
    try {
      const body = await cdp.send(
        "Network.getResponseBody",
        { requestId: p.requestId },
        eventSessionId || sessionId
      );
      push("body", { url: rec.url, status: rec.status, body: (body.body || "").slice(0, 20000) });
    } catch (_) {}
  });
  onSession("Network.loadingFailed", (p) => {
    const rec = net.get(p.requestId);
    push("failed", {
      url: rec ? rec.url : "?",
      error: p.errorText,
      blocked: p.blockedReason,
      cors: p.corsErrorStatus && p.corsErrorStatus.corsError,
    });
  });
  onSession("Network.webTransportCreated", (p) => {
    push("webtransport", { event: "created", url: p.url, id: p.transportId });
  });
  onSession("Network.webTransportConnectionEstablished", (p) => {
    push("webtransport", { event: "established", id: p.transportId, ts: p.timestamp });
  });
  onSession("Network.webTransportClosed", (p) => {
    push("webtransport", { event: "closed", id: p.transportId });
  });
  onSession("Network.webSocketCreated", (p) => push("websocket", { event: "created", url: p.url }));
  onSession("Page.javascriptDialogOpening", (p) => {
    push("dialog", { type: p.type, message: p.message });
    send("Page.handleJavaScriptDialog", { accept: true }).catch(() => {});
  });

  // The engine runs its join HTTP inside workers, so a page-scoped
  // Network.enable never sees it. Auto-attach to every target and enable the
  // same domains per session.
  cdp.on("Target.attachedToTarget", (p) => {
    const sid = p.sessionId;
    cdp.send("Network.enable", { maxTotalBufferSize: 64 * 1024 * 1024 }, sid).catch(() => {});
    cdp.send("Runtime.enable", {}, sid).catch(() => {});
    cdp.send("Network.setUserAgentOverride", { userAgent: UA }, sid).catch(() => {});
    push("target", { type: p.targetInfo && p.targetInfo.type, url: p.targetInfo && p.targetInfo.url });
  });

  await send("Page.enable");
  await send("Runtime.enable");
  await send("Log.enable");
  // Page-scoped: the engine lives in a `stub.js` worker, and only this
  // session's auto-attach covers its children.
  await send("Target.setAutoAttach", { autoAttach: true, waitForDebuggerOnStart: false, flatten: true }).catch(() => {});
  await send("Network.enable", {
    maxTotalBufferSize: 200 * 1024 * 1024,
    maxResourceBufferSize: 32 * 1024 * 1024,
  });
  await send("Network.setUserAgentOverride", { userAgent: UA, acceptLanguage: "en-US,en;q=0.9" });
  await send("Emulation.setDeviceMetricsOverride", {
    width: 1280,
    height: 800,
    deviceScaleFactor: 1,
    mobile: false,
  });

  // Log in before the first navigation so the web client boots authenticated.
  if (COOKIE_FILE && fs.existsSync(COOKIE_FILE)) {
    const value = fs.readFileSync(COOKIE_FILE, "utf8").trim();
    const r = await send("Network.setCookie", {
      name: ".ROBLOSECURITY",
      value,
      domain: ".roblox.com",
      path: "/",
      secure: true,
      httpOnly: true,
      sameSite: "Lax",
      expires: Math.floor(Date.now() / 1000) + 30 * 86400,
    });
    push("cookie", { ok: Boolean(r && r.success), bytes: value.length });
  } else {
    push("cookie", { ok: false, reason: "cookie file missing" });
  }

  push("navigate", { url: URL_ARG });
  await send("Page.navigate", { url: URL_ARG });

  // The engine refuses to join when it cannot get a GPU context, so record
  // what this Chrome actually exposes instead of guessing from a screenshot.
  const GPU_PROBE = `(async () => {
    const out = {};
    try { const a = await navigator.gpu.requestAdapter(); out.webgpu = a ? (a.info ? (a.info.vendor + '/' + a.info.architecture) : 'adapter') : 'no adapter'; } catch (e) { out.webgpu = 'err ' + e; }
    try { const c = document.createElement('canvas'); const g = c.getContext('webgl2'); out.webgl2 = g ? g.getParameter(g.RENDERER) : 'none'; } catch (e) { out.webgl2 = 'err ' + e; }
    out.gpuProcess = navigator.userAgent.includes('Headless') ? 'headless' : 'headed';
    return JSON.stringify(out);
  })()`;
  await sleep(5000);
  const gpu = await send("Runtime.evaluate", { expression: GPU_PROBE, awaitPromise: true, returnByValue: true }).catch(() => null);
  if (gpu && gpu.result && gpu.result.value) push("gpu", JSON.parse(gpu.result.value));

  const deadline = Date.now() + SECONDS * 1000;
  let lastShot = 0;
  while (Date.now() < deadline) {
    await sleep(1000);
    if (SHOT && Date.now() - lastShot > 15000) {
      lastShot = Date.now();
      const shot = await send("Page.captureScreenshot", { format: "png" }).catch(() => null);
      if (shot) fs.writeFileSync(SHOT.replace(/\.png$/, "") + "_" + Math.floor((Date.now() % 1e6) / 1000) + ".png", Buffer.from(shot.data, "base64"));
    }
    const state = await send("Runtime.evaluate", {
      expression: "JSON.stringify({u:location.href,t:document.title,b:(document.body&&document.body.innerText||'').slice(0,400)})",
      returnByValue: true,
    }).catch(() => null);
    if (state && state.result && state.result.value) {
      push("page", JSON.parse(state.result.value));
    }
  }

  const final = await send("Page.captureScreenshot", { format: "png" }).catch(() => null);
  if (SHOT && final) fs.writeFileSync(SHOT, Buffer.from(final.data, "base64"));

  flush();
  const summary = events.filter((e) =>
    ["cookie", "failed", "webtransport", "exception", "body", "navigate", "gpu"].includes(e.kind) ||
    (e.kind === "console" && /wasm|WebTransport|gameFqdn|join|error|fail/i.test(e.text || "")) ||
    (e.kind === "log" && (e.level === "error" || /wasm|WebTransport|gameFqdn/i.test(e.text || "")))
  );
  console.log(JSON.stringify({ out: OUT, totalEvents: events.length, interesting: summary }, null, 1));
  ws.close();
  cleanup();
  await sleep(200);
  process.exit(0);
})().catch((e) => {
  console.error("probe failed:", e && e.stack || e);
  cleanup();
  process.exit(1);
});
