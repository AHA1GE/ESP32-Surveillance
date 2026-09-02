// Cloudflare Realtime TURN credential minting and normalization.
//
// POST https://rtc.live.cloudflare.com/v1/turn/keys/{id}/credentials/generate-ice-servers
// (Bearer = the TURN key API token, body {"ttl": seconds}) returns:
//
//   { "iceServers": [
//       { "urls": ["stun:stun.cloudflare.com:3478", "stun:stun.cloudflare.com:53"] },
//       { "urls": ["turn:turn.cloudflare.com:3478?transport=udp", ...alt ports...,
//                  "turns:turn.cloudflare.com:5349?transport=tcp", ...],
//         "username": "...", "credential": "..." } ] }
//
// We normalize into the protocol shape of the Go backend's TurnCreds:
// primary ports only (the alt ports 53/80/443 are for captive-portal
// networks; 53 is known blocked in browsers), UDP first - the firmware picks
// the transport=udp entry.

export interface TurnCreds {
  urls: string[];
  username: string;
  credential: string;
}

export interface IceServers {
  stun: string[];
  turn: TurnCreds | null;
}

const CF_STUN = "stun:stun.cloudflare.com:3478";
const TTL_SECONDS = 7200; // 2 h, matching the old TURN_CRED_HOURS

// Primary-port URLs, kept in canonical order (UDP first).
const KEEP_TURN_URLS = [
  "turn:turn.cloudflare.com:3478?transport=udp",
  "turn:turn.cloudflare.com:3478?transport=tcp",
  "turns:turn.cloudflare.com:5349?transport=tcp",
];

interface CfResponse {
  iceServers?: Array<{ urls?: string[]; username?: string; credential?: string }>;
}

// mintIceServers returns normalized ICE servers, or stun-only when TURN is
// not configured or minting fails. Degrading keeps LAN-direct viewing (host
// candidates) working; the relay path is the only casualty.
export async function mintIceServers(env: Env): Promise<IceServers> {
  const keyId = env.TURN_SERVER_ID;
  const apiToken = env.TURN_SERVER_TOKEN;
  if (!keyId || !apiToken) {
    return { stun: [CF_STUN], turn: null };
  }

  try {
    const res = await fetch(
      `https://rtc.live.cloudflare.com/v1/turn/keys/${encodeURIComponent(keyId)}/credentials/generate-ice-servers`,
      {
        method: "POST",
        headers: {
          Authorization: `Bearer ${apiToken}`,
          "Content-Type": "application/json",
        },
        body: JSON.stringify({ ttl: TTL_SECONDS }),
      },
    );
    if (!res.ok) {
      console.error(`TURN credential minting failed: HTTP ${res.status}`);
      return { stun: [CF_STUN], turn: null };
    }

    const data = (await res.json()) as CfResponse;
    for (const entry of data.iceServers ?? []) {
      if (!entry.username || !entry.credential || !Array.isArray(entry.urls)) continue;
      const primary = KEEP_TURN_URLS.filter((u) => entry.urls!.includes(u));
      return {
        stun: [CF_STUN],
        turn: {
          // Prefer the canonical list; fall back to whatever CF sent if the
          // response shape changes.
          urls: primary.length > 0 ? primary : entry.urls,
          username: entry.username,
          credential: entry.credential,
        },
      };
    }

    console.error("TURN credential minting: no TURN entry in response");
    return { stun: [CF_STUN], turn: null };
  } catch (e) {
    console.error("TURN credential minting error:", e);
    return { stun: [CF_STUN], turn: null };
  }
}
