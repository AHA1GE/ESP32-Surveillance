// Worker entrypoint for espcam.dofor.fun: auth, the device list API, the
// two static pages, and WebSocket proxying into one DeviceHub DO per device.
//
// Auth model (shared bearer token):
// - Devices send `Authorization: Bearer <token>` on /signaling/{id}
//   (browsers cannot set WS headers, so devices use the header path).
// - Browsers POST /api/login {token} once and get an HttpOnly cookie; every
//   page fetch, /api call, and viewer WS upgrade checks the cookie.
// - The token never appears in URLs (no query-param leaks into logs).
//
// Routes (see CLOUDFLARE_REVISION_PLAN.md §2.1):
//   GET  /                      device list page (assets/index.html)
//   GET  /view/{id}             viewer page (assets/view.html)
//   POST /api/login             token -> HttpOnly cookie
//   GET  /api/devices           [{id, online, lastSeen}] from D1
//   WS   /signaling/{id}        device socket, proxied to DeviceHub DO
//   WS   /view-signaling/{id}   viewer socket, proxied to DeviceHub DO

import { DeviceHub } from "./devicehub";
import { deviceExists, ensureDatabase, listDevices } from "./db";

// Wrangler needs the Durable Object class exported from the entrypoint to
// wire the DEVICE_HUB binding (both in `wrangler dev` and on deploy).
export { DeviceHub };

const DEVICE_ID_RE = /^[a-zA-Z0-9-]{1,64}$/;
const SESSION_COOKIE = "esp32cam_session";
const SESSION_MAX_AGE = 60 * 60 * 24 * 365; // 1 year

// Constant-time comparison of the shared token, so both the header and the
// cookie paths take the same amount of time for a wrong guess.
async function tokensEqual(a: string, b: string): Promise<boolean> {
  const enc = new TextEncoder();
  const [ha, hb] = await Promise.all([
    crypto.subtle.digest("SHA-256", enc.encode(a)),
    crypto.subtle.digest("SHA-256", enc.encode(b)),
  ]);
  return crypto.subtle.timingSafeEqual(ha, hb);
}

function getCookie(request: Request, name: string): string | null {
  const header = request.headers.get("Cookie");
  if (!header) return null;
  for (const part of header.split(";")) {
    const eq = part.indexOf("=");
    if (eq < 0) continue;
    if (part.slice(0, eq).trim() === name) return part.slice(eq + 1).trim();
  }
  return null;
}

async function deviceAuthorized(request: Request, env: Env): Promise<boolean> {
  const authz = request.headers.get("Authorization") ?? "";
  if (!authz.startsWith("Bearer ")) return false;
  return tokensEqual(authz.slice("Bearer ".length), env.SHARED_AUTH_TOKEN);
}

async function viewerAuthorized(request: Request, env: Env): Promise<boolean> {
  const session = getCookie(request, SESSION_COOKIE);
  if (!session) return false;
  return tokensEqual(session, env.SHARED_AUTH_TOKEN);
}

function unauthorized(request: Request, url: URL): Response {
  const wantsHtml = (request.headers.get("Accept") ?? "").includes("text/html");
  if (!wantsHtml) {
    return Response.json({ error: "unauthorized" }, { status: 401 });
  }
  return new Response(loginPage(nextFor(url)), {
    status: 401,
    headers: { "Content-Type": "text/html; charset=utf-8" },
  });
}

// Where the login page sends the browser after a successful login.
function nextFor(url: URL): string {
  const next = url.searchParams.get("next") ?? url.pathname + url.search;
  return next.startsWith("/") && !next.startsWith("//") ? next : "/";
}

// Minimal self-contained login page (returned on 401 for navigation
// requests); it POSTs the token and reloads the target page. next is
// JSON-escaped into a data attribute, which is safe inside an attribute
// value.
function loginPage(next: string): string {
  return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Sign in</title>
<style>
  :root { color-scheme: dark; }
  body { font-family: system-ui, sans-serif; background: #111; color: #eee; margin: 0; padding: 1rem; }
  form { max-width: 320px; margin: 15vh auto 0; background: #1b1b1b; border: 1px solid #2a2a2a; border-radius: 8px; padding: 1.25rem; }
  h1 { font-size: 1.2rem; margin-top: 0; }
  input { width: 100%; box-sizing: border-box; padding: .5rem; border: 1px solid #333; border-radius: 6px; background: #111; color: #eee; font-size: 1rem; }
  button { width: 100%; margin-top: .75rem; padding: .5rem; border: 0; border-radius: 6px; background: #357; color: #fff; font-size: 1rem; cursor: pointer; }
  #error { color: #c66; font-size: .85rem; margin-top: .5rem; min-height: 1em; }
</style>
</head>
<body>
<form id="form">
  <h1>Access token</h1>
  <input id="token" type="password" autocomplete="current-password" placeholder="token" required>
  <button type="submit">Sign in</button>
  <div id="error"></div>
</form>
<script>
var next = ${JSON.stringify(next)};
document.getElementById('form').addEventListener('submit', function (ev) {
  ev.preventDefault();
  var err = document.getElementById('error');
  err.textContent = '';
  fetch('/api/login', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ token: document.getElementById('token').value }),
  }).then(function (r) {
    if (r.ok) { location.href = next; return; }
    err.textContent = 'wrong token';
  }).catch(function () { err.textContent = 'network error'; });
});
</script>
</body>
</html>`;
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    const upgrade = (request.headers.get("Upgrade") ?? "").toLowerCase();

    // ---- WebSocket upgrades ----
    if (upgrade === "websocket") {
      if (request.method !== "GET") return new Response("not found", { status: 404 });

      let m = url.pathname.match(/^\/signaling\/([a-zA-Z0-9-]{1,64})$/);
      if (m) {
        if (!(await deviceAuthorized(request, env))) {
          return new Response("unauthorized", { status: 401 });
        }
        const stub = env.DEVICE_HUB.get(env.DEVICE_HUB.idFromName(m[1]));
        return stub.fetch(request);
      }

      m = url.pathname.match(/^\/view-signaling\/([a-zA-Z0-9-]{1,64})$/);
      if (m) {
        if (!(await viewerAuthorized(request, env))) {
          return new Response("unauthorized", { status: 401 });
        }
        // Never-seen device IDs 404 before the upgrade, like the Go hub.
        if (!(await deviceExists(env, m[1]))) {
          return new Response("device not found", { status: 404 });
        }
        const stub = env.DEVICE_HUB.get(env.DEVICE_HUB.idFromName(m[1]));
        return stub.fetch(request);
      }

      return new Response("not found", { status: 404 });
    }

    // ---- login (the one route open without a cookie) ----
    if (url.pathname === "/api/login") {
      if (request.method !== "POST") {
        return new Response("method not allowed", { status: 405 });
      }
      let body: { token?: unknown };
      try {
        body = await request.json();
      } catch {
        return Response.json({ ok: false, error: "bad request" }, { status: 400 });
      }
      if (typeof body.token !== "string" || body.token === "") {
        return Response.json({ ok: false, error: "bad request" }, { status: 400 });
      }
      if (!(await tokensEqual(body.token, env.SHARED_AUTH_TOKEN))) {
        return Response.json({ ok: false, error: "wrong token" }, { status: 401 });
      }
      const secure = url.protocol === "https:";
      return Response.json(
        { ok: true },
        {
          headers: {
            "Set-Cookie":
              `${SESSION_COOKIE}=${body.token}; HttpOnly; SameSite=Lax; Path=/; ` +
              `Max-Age=${SESSION_MAX_AGE}${secure ? "; Secure" : ""}`,
          },
        },
      );
    }

    // ---- everything below requires the session cookie ----
    if (!(await viewerAuthorized(request, env))) return unauthorized(request, url);

    if (url.pathname === "/api/devices") {
      if (request.method !== "GET") {
        return new Response("method not allowed", { status: 405 });
      }
      await ensureDatabase(env);
      return Response.json(await listDevices(env));
    }

    if (url.pathname === "/") {
      if (request.method !== "GET") {
        return new Response("method not allowed", { status: 405 });
      }
      // html_handling "none" (set to stop the /view.html -> /view redirect
      // from stripping the device id) also stops the root from resolving
      // index.html implicitly, so ask for the file by name.
      return env.ASSETS.fetch(new Request(new URL("/index.html", request.url), request));
    }

    if (url.pathname === "/view" || url.pathname === "/view.html") {
      // The view page needs a device id in the URL (/view/{id}); a bare
      // /view (or a stale /view.html bookmark) has nothing to connect to,
      // so land on the device list instead. (Response.redirect needs an
      // absolute URL - a bare "/" throws "Unable to parse URL" in workerd.)
      return Response.redirect(new URL("/", request.url).toString(), 302);
    }

    const vm = url.pathname.match(/^\/view\/([a-zA-Z0-9-]{1,64})$/);
    if (vm) {
      if (request.method !== "GET") {
        return new Response("method not allowed", { status: 405 });
      }
      // Unknown device -> 404, matching the Go ui handler.
      if (!(await deviceExists(env, vm[1]))) {
        return new Response("device not found", { status: 404 });
      }
      // Serve the viewer page asset (view.html lives in assets/).
      return env.ASSETS.fetch(new Request(new URL("/view.html", request.url), request));
    }

    return new Response("not found", { status: 404 });
  },
} satisfies ExportedHandler<Env>;
