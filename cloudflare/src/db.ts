// D1 presence helpers. The schema is initialized in code (the same pattern as
// ztmdl-room-reserve-system): a _meta schema_version row plus CREATE TABLE IF
// NOT EXISTS, so deploys need no separate `wrangler d1 migrations apply`
// step. schema.sql mirrors this DDL for reference.
//
// devices is written by the DeviceHub DO (online/last_seen on connect,
// disconnect, and the liveness alarm) and read by the Worker (/api/devices,
// the never-seen-404 check).
const SCHEMA_VERSION = 1;

const CREATE_DEVICES = `
  CREATE TABLE IF NOT EXISTS devices (
    id TEXT PRIMARY KEY,
    online INTEGER NOT NULL DEFAULT 0,
    first_seen TEXT,
    last_seen TEXT
  )
`;

export interface DeviceInfo {
  id: string;
  online: boolean;
  lastSeen: string; // RFC3339 UTC; "" if never connected
}

export async function ensureDatabase(env: Env): Promise<void> {
  const db = env.esp32_surveillance_db;
  await db
    .prepare(`CREATE TABLE IF NOT EXISTS _meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)`)
    .run();

  const row = await db
    .prepare(`SELECT value FROM _meta WHERE key = 'schema_version'`)
    .first<{ value: string }>();
  const version = row ? parseInt(row.value, 10) : 0;
  if (version !== SCHEMA_VERSION) {
    // First create (or a future bump). Nothing to drop at v1; later versions
    // drop/recreate their tables here before re-creating below.
    console.log(
      `D1 schema version mismatch (db: ${version}, code: ${SCHEMA_VERSION}) - ensuring tables`,
    );
  }

  await db.prepare(CREATE_DEVICES).run();
  await db
    .prepare(`INSERT OR REPLACE INTO _meta (key, value) VALUES ('schema_version', ?1)`)
    .bind(String(SCHEMA_VERSION))
    .run();
}

export async function listDevices(env: Env): Promise<DeviceInfo[]> {
  await ensureDatabase(env);
  const res = await env.esp32_surveillance_db
    .prepare(`SELECT id, online, last_seen FROM devices ORDER BY id`)
    .all<{ id: string; online: number; last_seen: string | null }>();
  return res.results.map((r) => ({
    id: r.id,
    online: r.online !== 0,
    lastSeen: r.last_seen ?? "",
  }));
}

export async function deviceExists(env: Env, id: string): Promise<boolean> {
  await ensureDatabase(env);
  const row = await env.esp32_surveillance_db
    .prepare(`SELECT id FROM devices WHERE id = ?1 LIMIT 1`)
    .bind(id)
    .first();
  return row !== null;
}

// upsertDeviceOnline marks a device online, preserving first_seen across
// reconnects (ON CONFLICT only touches online and last_seen).
export async function upsertDeviceOnline(env: Env, id: string, now: string): Promise<void> {
  await env.esp32_surveillance_db
    .prepare(
      `INSERT INTO devices (id, online, first_seen, last_seen) VALUES (?1, 1, ?2, ?2)
       ON CONFLICT(id) DO UPDATE SET online = 1, last_seen = excluded.last_seen`,
    )
    .bind(id, now)
    .run();
}

export async function setDeviceOffline(env: Env, id: string, now: string): Promise<void> {
  await env.esp32_surveillance_db
    .prepare(`UPDATE devices SET online = 0, last_seen = ?2 WHERE id = ?1`)
    .bind(id, now)
    .run();
}

// touchDeviceLastSeen refreshes presence for a connected device (the DO's
// 30 s alarm calls this, so "last seen" stays current without an HTTP
// heartbeat from the firmware).
export async function touchDeviceLastSeen(env: Env, id: string, now: string): Promise<void> {
  await env.esp32_surveillance_db
    .prepare(`UPDATE devices SET online = 1, last_seen = ?2 WHERE id = ?1`)
    .bind(id, now)
    .run();
}
