#!/usr/bin/env node
// One-shot end-to-end smoke test against a running worker (`wrangler dev` or
// the deployed one): exercises the auth gates, the login flow, D1 presence,
// and the full signaling protocol including busy/offline/supersede paths.
// Exits 0 on success, 1 on any failure.
//
// Usage:
//   node scripts/smoke-test.mjs [--base http://127.0.0.1:8787] [--token <SHARED_AUTH_TOKEN>]
// The token may also come from the SIM_TOKEN env var.
import WebSocket from "ws";

const args = new Map();
for (const arg of process.argv.slice(2)) {
  const m = /^--([^=]+)(?:=(.*))?$/.exec(arg);
  if (m) args.set(m[1], m[2] ?? "true");
}
const HTTP = (args.get("base") ?? "http://127.0.0.1:8787").replace(/\/+$/, "");
const WS = HTTP.replace(/^http/, "ws");
const TOKEN = args.get("token") ?? process.env.SIM_TOKEN ?? "local-test-token-1234";
// Unique per-run IDs: local D1 state persists across wrangler dev restarts,
// so a fixed ID would hit stale online rows and break the "unknown device"
// and empty-DB assertions.
const RUN = Date.now();
const ID = args.get("id") ?? `esp32cam-SMOKE-${RUN}`;
const NEVERSEEN = `esp32cam-NEVERSEEN-${RUN}`;
const COOKIE = `esp32cam_session=${TOKEN}`;

const SDP = [
  "v=0",
  "o=- 0 0 IN IP4 127.0.0.1",
  "s=-",
  "t=0 0",
  "m=application 9 UDP/DTLS/SCTP webrtc-datachannel",
  "a=ice-ufrag:",
  "a=ice-pwd:x",
  "",
].join("\r\n");

let failures = 0;
function pass(name, ok, detail) {
  console.log(`${ok ? "PASS" : "FAIL"}  ${name}${detail ? `  (${detail})` : ""}`);
  if (!ok) failures++;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Opens a WS; resolves with ws.status = 101 on open, or the HTTP status on
// unexpected-response (401/404 gates). Incoming JSON frames accumulate in
// ws.messages, the close frame in ws.closedInfo.
function openWs(path, headers) {
  return new Promise((resolve) => {
    const ws = new WebSocket(WS + path, { headers });
    ws.messages = [];
    ws.closedInfo = null;
    ws.on("message", (d) => {
      try {
        ws.messages.push(JSON.parse(d.toString()));
      } catch {
        ws.messages.push(d.toString());
      }
    });
    ws.on("open", () => {
      ws.status = 101;
      resolve(ws);
    });
    ws.on("unexpected-response", (_req, res) => {
      ws.status = res.statusCode;
      resolve(ws);
    });
    ws.on("close", (code, reason) => {
      ws.closedInfo = { code, reason: reason.toString() };
    });
    ws.on("error", () => {}); // surfaced via unexpected-response / close
  });
}

async function waitFor(ws, pred, ms = 8000) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    const hit = ws.messages.find(pred);
    if (hit) return hit;
    if (ws.closedInfo) return null;
    await sleep(50);
  }
  return null;
}

// Server-issued closes from fetch/waitUntil/alarm contexts reach the client
// after a ~10s workerd flush delay (see CLOUDFLARE_REVISION_PLAN.md), so
// close windows are generous.
async function waitClose(ws, ms = 15000) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) {
    if (ws.closedInfo) return ws.closedInfo;
    await sleep(50);
  }
  return null;
}

(async () => {
  // ---- HTTP auth gates ----
  let r = await fetch(HTTP + "/api/devices");
  pass("GET /api/devices without cookie -> 401", r.status === 401);

  // Browsers navigate with Accept: text/html; without it the worker answers
  // JSON 401, which is the intended API behavior.
  r = await fetch(HTTP + "/", { headers: { Accept: "text/html" } });
  const body = await r.text();
  pass(
    "GET / without cookie -> 401 login page",
    r.status === 401 &&
      (r.headers.get("content-type") ?? "").includes("text/html") &&
      body.includes("Access token"),
  );

  r = await fetch(HTTP + "/api/login", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ token: "wrong-token" }),
  });
  pass("POST /api/login wrong token -> 401", r.status === 401 && !(await r.json()).ok);

  r = await fetch(HTTP + "/api/login", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ token: TOKEN }),
  });
  pass("POST /api/login right token -> ok", r.status === 200 && (await r.json()).ok);
  const setCookie = r.headers.get("set-cookie") ?? "";
  pass(
    "login sets HttpOnly session cookie",
    /esp32cam_session=/.test(setCookie) && /HttpOnly/.test(setCookie),
  );

  r = await fetch(HTTP + "/api/devices", { headers: { Cookie: COOKIE } });
  const initialDevices = await r.json();
  pass(
    "GET /api/devices before run -> fresh ID not listed",
    r.status === 200 && Array.isArray(initialDevices) && !initialDevices.some((d) => d.id === ID),
    JSON.stringify(initialDevices),
  );

  r = await fetch(HTTP + "/", { headers: { Cookie: COOKIE } });
  pass(
    "GET / with cookie -> device list page",
    r.status === 200 && (await r.text()).includes("html"),
  );

  r = await fetch(HTTP + `/view/${NEVERSEEN}`, { headers: { Cookie: COOKIE } });
  pass("GET /view/unknown -> 404", r.status === 404);

  // ---- WS gates ----
  let ws = await openWs(`/view-signaling/${NEVERSEEN}`, { Cookie: COOKIE });
  pass("viewer WS to never-seen device -> 404", ws.status === 404);

  ws = await openWs(`/signaling/${ID}`, { Authorization: "Bearer wrong-token" });
  pass("device WS with wrong token -> 401", ws.status === 401);

  // ---- device online + full negotiation ----
  const device = await openWs(`/signaling/${ID}`, { Authorization: `Bearer ${TOKEN}` });
  pass("device WS connects", device.status === 101);

  await sleep(500); // let the DO write the D1 presence row
  r = await fetch(HTTP + "/api/devices", { headers: { Cookie: COOKIE } });
  const devices = await r.json();
  pass(
    "device shows online in D1",
    Array.isArray(devices) && devices.some((d) => d.id === ID && d.online),
    JSON.stringify(devices),
  );

  const viewer = await openWs(`/view-signaling/${ID}`, { Cookie: COOKIE });
  pass("viewer WS connects", viewer.status === 101);

  const ice = await waitFor(viewer, (m) => m && m.type === "ice_servers");
  pass(
    "viewer receives ice_servers",
    !!ice && Array.isArray(ice.stun) && ice.stun.length > 0,
    ice ? `stun=${JSON.stringify(ice.stun)} turn=${ice.turn ? "minted" : "stun-only"}` : "",
  );

  viewer.send(JSON.stringify({ type: "offer", sdp: SDP + "view" }));
  viewer.send(JSON.stringify({ type: "ice", candidate: "candidate:1 1 udp 2122260223 192.0.2.1 50000 typ host" }));

  const offerAtDevice = await waitFor(device, (m) => m && m.type === "offer");
  pass("device receives offer", !!offerAtDevice && !!offerAtDevice.sdp);
  pass(
    "offer carries stun",
    !!offerAtDevice && Array.isArray(offerAtDevice.stun) && offerAtDevice.stun.length > 0,
  );
  pass("device receives relayed ice", !!(await waitFor(device, (m) => m && m.type === "ice")));

  device.send(JSON.stringify({ type: "answer", sdp: SDP + "device" }));
  device.send(JSON.stringify({ type: "ice", candidate: "candidate:1 1 udp 2122260223 192.0.2.2 50001 typ host" }));
  pass("viewer receives answer", !!(await waitFor(viewer, (m) => m && m.type === "answer")));
  pass("viewer receives relayed ice", !!(await waitFor(viewer, (m) => m && m.type === "ice")));

  // ---- busy rejection ----
  const viewer2 = await openWs(`/view-signaling/${ID}`, { Cookie: COOKIE });
  const busy = await waitFor(viewer2, (m) => m && m.type === "error" && String(m.reason).includes("busy"));
  pass("second viewer gets busy error", !!busy, busy && busy.reason);
  const busyClose = await waitClose(viewer2);
  pass("second viewer closed with 1008", !!busyClose && busyClose.code === 1008);

  // ---- device disconnect -> peer_gone + offline ----
  device.close();
  const gone = await waitFor(viewer, (m) => m && m.type === "peer_gone");
  pass(
    "viewer gets peer_gone on device disconnect",
    !!gone && gone.reason === "device disconnected",
    gone && gone.reason,
  );
  const viewerClose = await waitClose(viewer);
  pass("viewer closed with 1000 after device disconnect", !!viewerClose && viewerClose.code === 1000);

  const viewer3 = await openWs(`/view-signaling/${ID}`, { Cookie: COOKIE });
  const offline = await waitFor(viewer3, (m) => m && m.type === "error" && String(m.reason).includes("offline"));
  pass("viewer gets offline error", !!offline, offline && offline.reason);

  // ---- reconnect supersedes + evicts the live viewer ----
  const device1 = await openWs(`/signaling/${ID}`, { Authorization: `Bearer ${TOKEN}` });
  pass("device reconnects", device1.status === 101);
  const viewer4 = await openWs(`/view-signaling/${ID}`, { Cookie: COOKIE });
  pass("viewer connects to reconnected device", viewer4.status === 101);
  pass("new viewer gets ice_servers", !!(await waitFor(viewer4, (m) => m && m.type === "ice_servers")));

  const device2 = await openWs(`/signaling/${ID}`, { Authorization: `Bearer ${TOKEN}` });
  pass("second device connects while first is live", device2.status === 101);
  const superseded = await waitClose(device1);
  pass(
    "first device superseded with close 1000",
    !!superseded && superseded.code === 1000,
    superseded && superseded.reason,
  );
  const evicted = await waitFor(viewer4, (m) => m && m.type === "peer_gone");
  pass("viewer evicted on device reconnect", !!evicted);

  device2.close();

  console.log(failures === 0 ? "\nSMOKE TEST PASSED" : `\nSMOKE TEST FAILED: ${failures} failure(s)`);
  process.exit(failures === 0 ? 0 : 1);
})().catch((e) => {
  console.error("smoke test crashed:", e);
  process.exit(1);
});
