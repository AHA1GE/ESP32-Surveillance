// DeviceHub: one Durable Object per deviceID, replacing the Go backend's
// per-device hub (registry.Device + signaling.Hub). It holds the device's
// long-lived signaling socket and the single viewer slot, relays the
// SignalMsg JSON envelope byte-for-byte (see CLOUDFLARE_REVISION_PLAN.md §1),
// and keeps the D1 presence row current.
//
// WebSocket hibernation notes (https://developers.cloudflare.com/durable-objects/best-practices/websockets/):
// - Sockets are accepted with ctx.acceptWebSocket (never ws.accept()), so the
//   DO can sleep between events while both sockets stay open.
// - Instance fields do NOT survive hibernation; everything stateful lives in
//   ctx.storage or in per-socket attachments (serializeAttachment), which the
//   runtime persists.
// - Events (webSocketMessage/webSocketClose/alarm) run exclusively - the same
//   mutual exclusion the Go registry's mutex provided.
// - With compat date >= 2026-04-07 (web_socket_auto_reply_to_close) the
//   runtime completes the close handshake; calling ws.close() remains safe.

import { DurableObject } from "cloudflare:workers";
import { ensureDatabase, upsertDeviceOnline, setDeviceOffline, touchDeviceLastSeen } from "./db";
import { mintIceServers, type IceServers } from "./turn";

const ALARM_MS = 30_000;
const DEVICE_IDLE_MS = 90_000; // firmware auto-reconnects ~3 s after the kill
const VIEWER_IDLE_MS = 60_000; // browser pings every 20 s, so ~3 pings grace

const STORAGE = {
  deviceId: "deviceId",
  dbReady: "dbReady",
  deviceLastActivity: "deviceLastActivity",
  viewerSid: "viewerSid",
  viewerLastActivity: "viewerLastActivity",
  iceServers: "iceServers",
} as const;

// Per-socket metadata that survives hibernation.
interface SocketMeta {
  role: "device" | "viewer";
  sid?: string; // viewer session id; matches STORAGE.viewerSid while claimed
  stale?: boolean; // device socket superseded by a reconnect
}

function metaOf(ws: WebSocket): SocketMeta | null {
  return ws.deserializeAttachment() as SocketMeta | null;
}

export class DeviceHub extends DurableObject<Env> {
  async fetch(request: Request): Promise<Response> {
    if (request.headers.get("Upgrade") !== "websocket") {
      return new Response("Expected Upgrade: websocket", { status: 426 });
    }

    const url = new URL(request.url);
    let role: "device" | "viewer" | null = null;
    let id = "";
    let m = url.pathname.match(/^\/signaling\/([a-zA-Z0-9-]{1,64})$/);
    if (m) {
      role = "device";
      id = m[1];
    } else {
      m = url.pathname.match(/^\/view-signaling\/([a-zA-Z0-9-]{1,64})$/);
      if (m) {
        role = "viewer";
        id = m[1];
      }
    }
    if (!role) return new Response("not found", { status: 404 });

    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    server.serializeAttachment(
      role === "viewer" ? { role, sid: crypto.randomUUID() } : { role },
    );
    this.ctx.acceptWebSocket(server);

    // Claim semantics run in waitUntil, after the 101 response is flushed.
    // workerd quirk: ws.close() issued from fetch/waitUntil/alarm contexts on
    // a hibernatable socket reaches the client only after an internal ~10 s
    // flush (see CLOUDFLARE_REVISION_PLAN.md §7). The terminal JSON (error /
    // peer_gone) still arrives instantly, so the reject order matches the Go
    // hub (error JSON first, then close); clients act on the JSON and the
    // close follows ~10 s later.
    this.ctx.waitUntil(
      role === "device" ? this.onDeviceConnect(server, id) : this.onViewerConnect(server),
    );

    return new Response(null, { status: 101, webSocket: client });
  }

  // ---- connect / disconnect (mirrors registry.Attach/Detach) ----

  private async onDeviceConnect(ws: WebSocket, id: string): Promise<void> {
    // The DO is keyed idFromName(id), so this never changes; it is stored so
    // alarm/D1 writes know the id after waking from hibernation.
    await this.ctx.storage.put(STORAGE.deviceId, id);

    // Schema init exactly once per DO (a schema bump can force a re-run by
    // deleting the flag; _meta carries the version).
    if (!(await this.ctx.storage.get(STORAGE.dbReady))) {
      await ensureDatabase(this.env);
      await this.ctx.storage.put(STORAGE.dbReady, true);
    }

    // A reconnect taking over the slot: mark any still-open device socket
    // stale (its close event must not run detach semantics) and close it.
    // The firmware reconnects every ~3 s, so the old socket is usually
    // already dead and this loop finds nothing.
    for (const sock of this.ctx.getWebSockets()) {
      const meta = metaOf(sock);
      if (meta && meta.role === "device" && !meta.stale && sock !== ws) {
        sock.serializeAttachment({ ...meta, stale: true });
        try {
          sock.close(1000, "superseded");
        } catch {
          // already closing; fine
        }
      }
    }

    // A session cannot outlive its device (registry.Detach): evict the
    // current viewer and free the slot for the next attempt.
    const viewer = await this.viewerSocket();
    if (viewer) {
      this.safeSend(viewer, { type: "peer_gone", reason: "device disconnected" });
      try {
        viewer.close(1000, "device disconnected");
      } catch {
        // close handler still runs cleanup
      }
    }
    await this.clearViewerState();

    const now = Date.now();
    await this.ctx.storage.put(STORAGE.deviceLastActivity, now);
    await upsertDeviceOnline(this.env, id, new Date(now).toISOString());
    await this.ctx.storage.setAlarm(now + ALARM_MS);
    console.log(`device ${id} connected`);
  }

  private async onViewerConnect(ws: WebSocket): Promise<void> {
    const device = this.deviceSocket();
    if (!device) {
      this.safeSend(ws, { type: "error", reason: "device offline" });
      try {
        ws.close(1000, "device offline");
      } catch {
        // already gone
      }
      return;
    }

    if (await this.ctx.storage.get(STORAGE.viewerSid)) {
      this.safeSend(ws, { type: "error", reason: "busy: another viewer is already watching" });
      try {
        ws.close(1008, "viewer slot busy");
      } catch {
        // already gone
      }
      return;
    }

    const meta = metaOf(ws)!;
    const now = Date.now();
    await this.ctx.storage.put(STORAGE.viewerSid, meta.sid);
    await this.ctx.storage.put(STORAGE.viewerLastActivity, now);

    // Session ICE servers: minted once per viewer claim, cached in storage
    // so the offer forwarded later (possibly after a hibernation wake) carries
    // the same credentials the browser already configured its PC with.
    const ice = await mintIceServers(this.env);
    await this.ctx.storage.put(STORAGE.iceServers, ice);

    this.safeSend(ws, { type: "ice_servers", ...iceServersMsg(ice) });
    console.log(`viewer connected for ${await this.ctx.storage.get(STORAGE.deviceId)}`);
  }

  // ---- message relay (mirrors the hub.go switch tables) ----

  async webSocketMessage(ws: WebSocket, message: string | ArrayBuffer): Promise<void> {
    if (typeof message !== "string") return; // the envelope is always JSON text
    const meta = metaOf(ws);
    if (!meta || meta.stale) return;

    let msg: { type?: string; sdp?: string; candidate?: string };
    try {
      msg = JSON.parse(message);
    } catch {
      return; // bad JSON ignored, as in the Go hub
    }
    const type = msg.type ?? "";
    const now = Date.now();

    if (meta.role === "device") {
      const viewer = await this.viewerSocket();
      switch (type) {
        case "answer":
          this.safeSend(viewer, { type: "answer", sdp: msg.sdp ?? "" });
          break;
        case "ice":
          this.safeSend(viewer, { type: "ice", candidate: msg.candidate ?? "" });
          break;
        case "ping":
          await this.ctx.storage.put(STORAGE.deviceLastActivity, now);
          break;
        default:
          break; // unknown types ignored; the protocol grows forward
      }
      return;
    }

    // viewer
    const device = this.deviceSocket();
    switch (type) {
      case "offer": {
        // ICE servers ride the offer (the device configures its
        // PeerConnection before answering).
        const ice = await this.ctx.storage.get<IceServers>(STORAGE.iceServers);
        this.safeSend(device, { type: "offer", sdp: msg.sdp ?? "", ...iceServersMsg(ice) });
        break;
      }
      case "ice":
        this.safeSend(device, { type: "ice", candidate: msg.candidate ?? "" });
        break;
      case "ping":
        await this.ctx.storage.put(STORAGE.viewerLastActivity, now);
        break;
      default:
        break;
    }
  }

  async webSocketClose(ws: WebSocket, code: number, reason: string, wasClean: boolean): Promise<void> {
    const meta = metaOf(ws);
    if (!meta || meta.stale) return;

    if (meta.role === "device") {
      const id = await this.ctx.storage.get<string>(STORAGE.deviceId);
      // A session cannot outlive its device: evict the viewer (peer_gone
      // first, matching hub.go's teardown order).
      const viewer = await this.viewerSocket();
      if (viewer) {
        this.safeSend(viewer, { type: "peer_gone", reason: "device disconnected" });
        try {
          viewer.close(1000, "device disconnected");
        } catch {
          // already closing
        }
      }
      await this.clearViewerState();
      await this.ctx.storage.deleteAlarm();
      if (id) await setDeviceOffline(this.env, id, new Date().toISOString());
      console.log(`device ${id ?? "?"} disconnected (${code} ${reason})`);
      return;
    }

    // viewer: free the slot only if this socket still holds the claim (a
    // device disconnect already released it, and its close then finds no
    // viewerSid to match).
    const storedSid = await this.ctx.storage.get<string>(STORAGE.viewerSid);
    if (meta.sid && meta.sid === storedSid) {
      await this.clearViewerState();
      this.safeSend(this.deviceSocket(), { type: "viewer_gone" });
      console.log(
        `viewer disconnected from ${await this.ctx.storage.get(STORAGE.deviceId)} (${code} ${reason})`,
      );
    }
  }

  async webSocketError(ws: WebSocket, error: unknown): Promise<void> {
    // Errors are followed by webSocketClose, which runs the cleanup.
    console.error("websocket error:", error);
  }

  // ---- liveness (the Go hub's read deadlines, ported to an alarm) ----

  async alarm(): Promise<void> {
    const now = Date.now();
    const deviceId = await this.ctx.storage.get<string>(STORAGE.deviceId);
    const device = this.deviceSocket();
    const viewerSid = await this.ctx.storage.get<string>(STORAGE.viewerSid);

    if (device) {
      // Kill a half-open device socket only while nobody is watching: a live
      // view sends no device->server signaling traffic once ICE is up, and
      // today's firmware has no app-level ping yet (Phase 2 adds it). With a
      // viewer present, the viewer's pings keep the alarm running but the
      // device socket is left alone - same gap as the Go hub, closed once
      // firmware pings arrive.
      if (!viewerSid) {
        const lastActivity =
          (await this.ctx.storage.get<number>(STORAGE.deviceLastActivity)) ?? 0;
        if (now - lastActivity > DEVICE_IDLE_MS) {
          try {
            device.close(1000, "idle timeout");
          } catch {
            // close handler cleans up
          }
          return; // webSocketClose runs the detach + alarm delete
        }
      }

      // Presence refresh: keeps last_seen current without an HTTP heartbeat.
      if (deviceId) await touchDeviceLastSeen(this.env, deviceId, new Date(now).toISOString());
      await this.ctx.storage.setAlarm(now + ALARM_MS);
    } else {
      await this.ctx.storage.deleteAlarm();
    }

    // Viewer idle eviction (browser pings every 20 s; evict after 60 s).
    if (viewerSid) {
      const lastActivity =
        (await this.ctx.storage.get<number>(STORAGE.viewerLastActivity)) ?? 0;
      if (now - lastActivity > VIEWER_IDLE_MS) {
        const viewer = await this.viewerSocket();
        if (viewer) {
          try {
            viewer.close(1000, "viewer idle");
          } catch {
            // close handler releases the slot
          }
        }
      }
    }
  }

  // ---- helpers ----

  private deviceSocket(): WebSocket | null {
    for (const ws of this.ctx.getWebSockets()) {
      const meta = metaOf(ws);
      if (meta && meta.role === "device" && !meta.stale) return ws;
    }
    return null;
  }

  // The viewer socket that currently holds the claim. Rejected or evicted
  // sockets linger up to ~10 s before workerd flushes their close frame
  // (see CLOUDFLARE_REVISION_PLAN.md §7), so role alone is not enough: the
  // socket's sid must match the stored claim, otherwise answer/ice relays
  // could go to a dying socket instead of the live viewer.
  private async viewerSocket(): Promise<WebSocket | null> {
    const storedSid = await this.ctx.storage.get<string>(STORAGE.viewerSid);
    if (!storedSid) return null;
    for (const ws of this.ctx.getWebSockets()) {
      const meta = metaOf(ws);
      if (meta && meta.role === "viewer" && meta.sid === storedSid) return ws;
    }
    return null;
  }

  private async clearViewerState(): Promise<void> {
    await this.ctx.storage.delete(STORAGE.viewerSid);
    await this.ctx.storage.delete(STORAGE.viewerLastActivity);
    await this.ctx.storage.delete(STORAGE.iceServers);
  }

  private safeSend(ws: WebSocket | null, msg: unknown): void {
    if (!ws) return;
    try {
      ws.send(JSON.stringify(msg));
    } catch {
      // the close event runs the cleanup
    }
  }
}

// iceServersMsg renders the protocol's ice_servers/offer payload: stun[]
// always, turn{} only when credentials were minted (JSON.stringify drops
// undefined, matching the Go struct's omitempty).
function iceServersMsg(ice: IceServers | undefined): { stun?: string[]; turn?: IceServers["turn"] } {
  if (!ice) return {};
  return { stun: ice.stun, turn: ice.turn ?? undefined };
}
