#!/usr/bin/env node
// Smoke-test client for the signaling worker: impersonates an ESP32 device
// (default) or a browser viewer (--view) against `wrangler dev` or the
// deployed worker, so the DeviceHub DO can be exercised without hardware.
//
// Device mode: connects /signaling/{id} with the Authorization header (like
// the firmware), answers offers with canned SDP/ICE, pings every 30 s, and
// reconnects after 3 s like the firmware does.
//
// Viewer mode: connects /view-signaling/{id} with the session cookie (like
// the browser), waits for ice_servers, sends a canned offer, and logs
// answer/ice/error/close. Run a second viewer while the first is connected
// to see the busy (1008) rejection.
//
// Usage:
//   node scripts/sim-device.mjs --token <SHARED_AUTH_TOKEN> [--url ws://127.0.0.1:8787] [--id esp32cam-TESTSIM]
//   node scripts/sim-device.mjs --view --token <SHARED_AUTH_TOKEN>
// The token may also come from the SIM_TOKEN env var.
import WebSocket from "ws";

const args = new Map();
for (const arg of process.argv.slice(2)) {
  const m = /^--([^=]+)(?:=(.*))?$/.exec(arg);
  if (m) args.set(m[1], m[2] ?? "true");
}
const mode = args.has("view") ? "viewer" : "device";
const base = args.get("url") ?? "ws://127.0.0.1:8787";
const id = args.get("id") ?? "esp32cam-TESTSIM";
const token = args.get("token") ?? process.env.SIM_TOKEN ?? "";
if (!token) {
  console.error("no token: pass --token=... or set SIM_TOKEN");
  process.exit(1);
}

// Canned signaling payloads: the worker only relays these strings, so
// placeholder SDP/candidates are enough to exercise the protocol.
const OFFER_SDP = [
  "v=0",
  "o=- 0 0 IN IP4 127.0.0.1",
  "s=-",
  "t=0 0",
  "m=application 9 UDP/DTLS/SCTP webrtc-datachannel",
  "a=ice-ufrag:view",
  "a=ice-pwd:view",
  "",
].join("\r\n");
const ANSWER_SDP = OFFER_SDP.replace(/view/g, "device");
const VIEWER_CANDIDATE = "candidate:1 1 udp 2122260223 192.0.2.1 50000 typ host";
const DEVICE_CANDIDATE = "candidate:1 1 udp 2122260223 192.0.2.2 50001 typ host";

const url = `${base}/${mode === "device" ? "signaling" : "view-signaling"}/${id}`;
const headers =
  mode === "device"
    ? { Authorization: `Bearer ${token}` }
    : { Cookie: `esp32cam_session=${token}` };

let pingTimer = null;
let reconnectTimer = null;

function log(...parts) {
  console.log(`[${mode} ${id}]`, ...parts);
}

function connect() {
  log("connecting", url);
  const ws = new WebSocket(url, { headers });

  ws.on("open", () => {
    log("connected");
    // App-level keepalives, like the firmware (30 s) / browser (20 s) do.
    pingTimer = setInterval(
      () => { if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({ type: "ping" })); },
      mode === "device" ? 30000 : 20000,
    );
  });

  ws.on("message", (data) => {
    let msg;
    try {
      msg = JSON.parse(data.toString());
    } catch {
      log("bad JSON from server:", data.toString());
      return;
    }
    switch (msg.type) {
      case "ice_servers":
        log(
          "ice_servers: stun =",
          JSON.stringify(msg.stun ?? []),
          "turn =",
          msg.turn ? `${msg.turn.urls.length} urls, username ${msg.turn.username.length} chars` : "none",
        );
        if (mode === "viewer") {
          setTimeout(() => {
            ws.send(JSON.stringify({ type: "offer", sdp: OFFER_SDP }));
            ws.send(JSON.stringify({ type: "ice", candidate: VIEWER_CANDIDATE }));
          }, 300);
        }
        break;
      case "offer":
        log(
          "offer received:",
          msg.sdp ? `sdp ${msg.sdp.length} B` : "no sdp",
          "stun =", JSON.stringify(msg.stun ?? []),
          "turn =", msg.turn ? `${msg.turn.urls.join(", ")} (user ${msg.turn.username.length} chars)` : "none",
        );
        setTimeout(() => {
          ws.send(JSON.stringify({ type: "answer", sdp: ANSWER_SDP }));
          ws.send(JSON.stringify({ type: "ice", candidate: DEVICE_CANDIDATE }));
        }, 300);
        break;
      case "answer":
        log("answer received:", msg.sdp ? `${msg.sdp.length} B` : "no sdp");
        break;
      case "ice":
        log("ice candidate received:", msg.candidate);
        break;
      case "viewer_gone":
        log("viewer_gone - PeerConnection torn down");
        break;
      case "peer_gone":
        log("peer_gone:", msg.reason);
        break;
      case "error":
        log("error:", msg.reason);
        break;
      default:
        log("unknown message type:", msg.type);
    }
  });

  ws.on("close", (code, reason) => {
    log("closed:", code, reason.toString());
    clearInterval(pingTimer);
    // Firmware-style 3 s reconnect. Viewer mode does not retry - a busy
    // rejection (1008) should terminate with a clear log.
    if (mode === "device") {
      reconnectTimer = setTimeout(connect, 3000);
    } else if (code === 1008) {
      console.log("[viewer] second viewer rejected as expected (1008)");
    }
  });

  ws.on("error", (err) => {
    log("socket error:", err.message);
  });
}

process.on("SIGINT", () => {
  clearInterval(pingTimer);
  clearTimeout(reconnectTimer);
  process.exit(0);
});

connect();
