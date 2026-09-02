-- D1 schema for esp32-surveillance.
-- REFERENCE ONLY: the runtime source of truth is src/db.ts (ensureDatabase),
-- which creates these tables in code with the _meta schema_version guard, so
-- deploys need no `wrangler d1 migrations apply` step. Keep this file in sync
-- when the schema bumps.

CREATE TABLE IF NOT EXISTS devices (
  id TEXT PRIMARY KEY,          -- device ID, ^[a-zA-Z0-9-]{1,64}$ (esp32cam-XXXXXX)
  online INTEGER NOT NULL DEFAULT 0,
  first_seen TEXT,              -- RFC3339 UTC
  last_seen TEXT                -- RFC3339 UTC
);

-- Internal bookkeeping table.
CREATE TABLE IF NOT EXISTS _meta (
  key TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
-- _meta rows: ('schema_version', '1')
