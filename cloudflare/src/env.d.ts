// Ambient worker environment, shared by the Worker and the DeviceHub DO.
// Bindings are declared in wrangler.jsonc; secrets are set on the deployed
// worker (and .dev.vars for local dev).
interface Env {
  ASSETS: Fetcher;
  DEVICE_HUB: DurableObjectNamespace<import("./devicehub").DeviceHub>;
  esp32_surveillance_db: D1Database;
  SHARED_AUTH_TOKEN: string;
  TURN_SERVER_ID: string;
  TURN_SERVER_TOKEN: string;
}
