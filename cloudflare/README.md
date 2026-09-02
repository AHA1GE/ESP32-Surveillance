# Cloudflare side of ESP32-Surveillance

Wrangler project for the `esp32-surveillance` Worker at `espcam.dofor.fun`:
auth, the device list + viewer pages, WebSocket signaling via one
`DeviceHub` Durable Object per device, D1 presence, and Cloudflare Realtime
TURN credential minting. The protocol and design live in
[CLOUDFLARE_REVISION_PLAN.md](../CLOUDFLARE_REVISION_PLAN.md).

Layout:

- `src/index.ts` - Worker entrypoint: auth, routes, login page, WS proxying
- `src/devicehub.ts` - the DeviceHub Durable Object (WebSocket hibernation)
- `src/turn.ts` - Cloudflare TURN credential minting + normalization
- `src/db.ts` - D1 presence helpers + in-code schema init
- `src/env.d.ts` - the shared `Env` binding types
- `assets/` - static pages (`index.html` device list, `view.html` viewer)
- `scripts/sim-device.mjs` - protocol smoke-test client (no hardware needed)
- `schema.sql` - DDL mirror of `src/db.ts` (reference; see below)

## Local development

```bash
npm install
cp .dev.vars.example .dev.vars   # fill in the two tokens
npx wrangler dev                 # http://localhost:8787
```

Then, in two terminals:

```bash
# terminal 1: impersonate a device
node scripts/sim-device.mjs --token <token>

# terminal 2: impersonate a browser viewer (run twice to see the busy 1008)
node scripts/sim-device.mjs --view --token <token>
```

Without `TURN_SERVER_TOKEN` in `.dev.vars`, minting degrades to
stun-only ICE servers (still enough to exercise the protocol locally).

## Deploying

```bash
npx wrangler deploy
```

Notes for the first deploy (existing worker was created in the dashboard):

- `wrangler deploy` replaces the worker's bindings with `wrangler.jsonc`.
  The old D1 binding `esp32-surveillance-db` becomes `esp32_surveillance_db`
  (same `database_id`, data untouched) - answer yes if asked to remove the
  old binding.
- If asked about secret bindings not in the config (`SHARED_AUTH_TOKEN`,
  `TURN_SERVER_TOKEN`), answer **no** - they stay on the worker.

## Schema

The D1 schema initializes itself in code (`src/db.ts`, `_meta`
`schema_version` guard + `CREATE TABLE IF NOT EXISTS`), so deploys need no
migration step. Bump `SCHEMA_VERSION` there (and mirror the DDL in
`schema.sql`) for schema changes.
