// Roblox GameJoin flow, reconstructed from GameJoinUtil / GameJoinV2.
//
// The desktop client's reactive path (GameJoinV2, EnableReactiveGameJoin3=True
// in production) posts to:
//
//   POST https://gamejoin.roblox.com/v2/join-game   (Accept: text/event-stream)
//
// and consumes a Server-Sent-Events stream:
//
//   event: Acknowledged
//   data:
//
//   event: ResponseReady
//   data: {"status":2,"jobId":"...","joinScript":{...},"queuePosition":0,...}
//
// The legacy path is POST /v1/join-game returning a single JSON document.
// Both are implemented here; v2 is attempted first to match the production
// client, with v1 as fallback.
//
// All requests use credentials:"include" so the browser attaches the
// .ROBLOSECURITY session cookie. Roblox POSTs are CSRF-protected; a 403
// response carries x-csrf-token which must be echoed back.

const GAMEJOIN = 'https://gamejoin.roblox.com';
const AUTHED_USER = 'https://users.roblox.com/v1/users/authenticated';

export class RobloxApiError extends Error {
  constructor(message, { status, body, url } = {}) {
    super(message);
    this.name = 'RobloxApiError';
    this.status = status;
    this.body = body;
    this.url = url;
  }
}

const uuid = () => crypto.randomUUID();

export async function getAuthenticatedUser(log = () => {}) {
  log('GET ' + AUTHED_USER);
  const res = await fetch(AUTHED_USER, { credentials: 'include' });
  if (res.status === 401 || res.status === 403) {
    throw new RobloxApiError('Not logged in to Roblox (no valid .ROBLOSECURITY cookie).', {
      status: res.status, url: AUTHED_USER,
    });
  }
  if (!res.ok) {
    throw new RobloxApiError(`users API returned HTTP ${res.status}`, { status: res.status, url: AUTHED_USER });
  }
  const user = await res.json();
  log(`authenticated: ${user.name} (id ${user.id})`);
  return user;
}

export function buildJoinBody({ placeId, universeId, jobId, launchData, joinAttemptOrigin, browserTrackerId }) {
  const body = {
    placeId: Number(placeId),
    isTeleport: false,
    gameJoinAttemptId: uuid(),
    browserTrackerId: browserTrackerId ?? 0,
    playSessionId: uuid(),
    eventId: uuid(),
    launchData: launchData || '',
    joinAttemptOrigin: joinAttemptOrigin || 'PlayButton',
  };
  if (universeId) body.gameId = Number(universeId);
  if (jobId) body.jobId = jobId;
  return body;
}

async function postWithCsrf(url, body, extraHeaders, log) {
  const headers = {
    'Content-Type': 'application/json; charset=utf-8',
    ...extraHeaders,
  };
  log(`POST ${url}`);
  let res = await fetch(url, {
    method: 'POST', credentials: 'include', headers,
    body: JSON.stringify(body),
  });
  if (res.status === 403) {
    const csrf = res.headers.get('x-csrf-token');
    if (csrf) {
      log('  CSRF challenge -> retrying with x-csrf-token');
      res = await fetch(url, {
        method: 'POST', credentials: 'include',
        headers: { ...headers, 'X-CSRF-TOKEN': csrf },
        body: JSON.stringify(body),
      });
    }
  }
  return res;
}

/** Parses an SSE body into a list of {event, data} records. */
export function parseSse(text) {
  const records = [];
  let cur = { event: 'message', data: [] };
  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine;
    if (line === '') {
      if (cur.data.length || cur.event !== 'message') {
        records.push({ event: cur.event, data: cur.data.join('\n') });
      }
      cur = { event: 'message', data: [] };
      continue;
    }
    if (line.startsWith(':')) continue;
    const idx = line.indexOf(':');
    const field = idx === -1 ? line : line.slice(0, idx);
    let value = idx === -1 ? '' : line.slice(idx + 1);
    if (value.startsWith(' ')) value = value.slice(1);
    if (field === 'event') cur.event = value;
    else if (field === 'data') cur.data.push(value);
    else if (field === 'id' || field === 'retry') { /* bookkeeping */ }
  }
  if (cur.data.length || cur.event !== 'message') records.push({ event: cur.event, data: cur.data.join('\n') });
  return records;
}

/**
 * v2 join: the server streams events. `ResponseReady` carries the joinScript.
 * Returns { api:'v2', status, joinScript, raw }.
 */
export async function joinGameV2(params, log = () => {}) {
  const body = buildJoinBody(params);
  const url = GAMEJOIN + '/v2/join-game';
  const res = await postWithCsrf(url, body, { Accept: 'text/event-stream' }, log);
  const text = await res.text();
  log(`  <- HTTP ${res.status}, ${text.length} bytes`);
  if (!res.ok) {
    throw new RobloxApiError(`join-game v2 failed with HTTP ${res.status}`, { status: res.status, body: text.slice(0, 1000), url });
  }
  // Some deployments answer v2 with plain JSON rather than SSE.
  const trimmed = text.trim();
  if (trimmed.startsWith('{')) {
    let json;
    try { json = JSON.parse(trimmed); } catch (e) {
      throw new RobloxApiError('v2 join reply was neither JSON nor SSE', { status: res.status, body: text.slice(0, 500), url });
    }
    return { api: 'v2', status: res.status, reply: json, raw: text };
  }
  const events = parseSse(text);
  for (const ev of events) log(`  SSE event=${ev.event} data=${ev.data.slice(0, 200)}`);
  const ready = events.find(e => e.event === 'ResponseReady');
  let reply = null;
  if (ready && ready.data) {
    try { reply = JSON.parse(ready.data); } catch { /* tolerate */ }
  }
  if (!reply) {
    // Fall back to any event carrying JSON with a joinScript.
    for (const ev of events) {
      if (!ev.data) continue;
      try {
        const j = JSON.parse(ev.data);
        if (j && (j.joinScript || j.status !== undefined)) { reply = j; break; }
      } catch { /* keep scanning */ }
    }
  }
  if (!reply) {
    throw new RobloxApiError('v2 join stream produced no ResponseReady payload', { status: res.status, body: text.slice(0, 1000), url });
  }
  return { api: 'v2', status: res.status, reply, raw: text };
}

/** v1 join: single JSON reply. */
export async function joinGameV1(params, log = () => {}) {
  const body = buildJoinBody(params);
  const url = GAMEJOIN + '/v1/join-game';
  const res = await postWithCsrf(url, body, {}, log);
  const text = await res.text();
  log(`  <- HTTP ${res.status}, ${text.length} bytes`);
  let reply = null;
  try { reply = text ? JSON.parse(text) : null; } catch { /* tolerate text */ }
  if (!res.ok) {
    throw new RobloxApiError(`join-game v1 failed with HTTP ${res.status}`, { status: res.status, body: text.slice(0, 1000), url });
  }
  if (!reply) throw new RobloxApiError('v1 join reply was not JSON', { status: res.status, body: text.slice(0, 500), url });
  return { api: 'v1', status: res.status, reply, raw: text };
}

/**
 * Join orchestration. Order matches the production client as closely as
 * possible: reactive v2 first (EnableReactiveGameJoin3 is on in prod), then v1.
 */
export async function joinGame(params, log = () => {}) {
  const errors = [];
  for (const [name, fn] of [['v2', joinGameV2], ['v1', joinGameV1]]) {
    try {
      log(`join attempt (${name})`);
      return await fn(params, log);
    } catch (e) {
      log(`join ${name} failed: ${e.message}`);
      errors.push(e);
    }
  }
  throw errors[0] ?? new RobloxApiError('all join attempts failed');
}

// ---------------------------------------------------------------------------
// Join reply parsing
// ---------------------------------------------------------------------------

export function extractJoinScript(joinResponse) {
  if (!joinResponse) return null;
  if (joinResponse.joinScript && typeof joinResponse.joinScript === 'object') return joinResponse.joinScript;
  if (joinResponse.JoinScript && typeof joinResponse.JoinScript === 'object') return joinResponse.JoinScript;
  if (typeof joinResponse.joinScript === 'string') {
    try { return JSON.parse(joinResponse.joinScript); } catch { /* fall through */ }
  }
  return joinResponse;
}

function tryParseJson(value) {
  if (typeof value !== 'string') return value;
  let t = value.trim();
  // NetStackConfig / RbxTransportConfig arrive percent-encoded on the wire
  // (e.g. %7B%22pubKey%22%3A...%7D), so decode before parsing.
  if (t.includes('%')) { try { t = decodeURIComponent(t); } catch { /* keep raw */ } }
  if (!t.startsWith('{') && !t.startsWith('[')) return value;
  try { return JSON.parse(t); } catch { return value; }
}

function pick(obj, ...names) {
  if (!obj || typeof obj !== 'object') return undefined;
  for (const n of names) if (obj[n] !== undefined && obj[n] !== null) return obj[n];
  const lower = new Map(Object.keys(obj).map(k => [k.toLowerCase(), k]));
  for (const n of names) {
    const k = lower.get(n.toLowerCase());
    if (k !== undefined && obj[k] !== undefined && obj[k] !== null) return obj[k];
  }
  return undefined;
}

/**
 * Normalizes a join reply into { fqdn, netStackPort, capsHex, certHashes, ... }.
 * Handles both inline joinScript objects and the joinScriptUrl indirection
 * (fetching the URL returns the same JSON document).
 */
export async function extractTransportInfo(joinResponse, log = () => {}) {
  // joinScriptUrl indirection: the reply may carry only a URL.
  if (joinReplyHasUrlOnly(joinResponse)) {
    const url = pick(joinResponse, 'joinScriptUrl');
    log(`join reply carries joinScriptUrl; fetching ${url}`);
    const res = await fetch(url, { credentials: 'include' });
    const text = await res.text();
    log(`  <- HTTP ${res.status}, ${text.length} bytes`);
    try { joinResponse = JSON.parse(text); } catch { throw new Error('joinScriptUrl did not return JSON'); }
  }
  const js = extractJoinScript(joinResponse);
  if (!js || typeof js !== 'object') throw new Error('join reply contained no joinScript object');
  log('joinScript keys: ' + Object.keys(js).join(', '));

  const machineAddress = pick(js, 'MachineAddress', 'machineAddress');
  const gameFqdnRaw = pick(js, 'GameFqdn', 'gameFqdn', 'GameFQDN');
  let gameFqdn = typeof gameFqdnRaw === 'string' && gameFqdnRaw.trim() ? gameFqdnRaw.trim() : null;
  let fqdn = gameFqdn;
  let netStackPort = pick(js, 'NetStackPort', 'netStackPort');
  let serverPort = pick(js, 'ServerPort', 'serverPort');
  const clientPort = pick(js, 'ClientPort', 'clientPort');
  let serverConnections = pick(js, 'ServerConnections', 'serverConnections');
  const udmuxEndpoints = pick(js, 'UdmuxEndpoints', 'udmuxEndpoints');
  const dsr = pick(js, 'DirectServerReturn', 'directServerReturn');

  if (!fqdn && typeof machineAddress === 'string') {
    const m = machineAddress.match(/^\[?([^\]\s]+?)\]?:(\d+)$/);
    if (m) { fqdn = m[1]; netStackPort ??= Number(m[2]); } else fqdn = machineAddress;
  }

  const rbxConfigRaw = pick(js, 'RbxTransportConfig', 'rbxTransportConfig');
  const rbxConfig = tryParseJson(rbxConfigRaw) || {};
  const netStackRaw = pick(js, 'NetStackConfig', 'netStackConfig');
  const netStack = tryParseJson(netStackRaw) || {};
  if (rbxConfigRaw !== undefined) log('RbxTransportConfig: ' + JSON.stringify(rbxConfig).slice(0, 400));
  if (netStackRaw !== undefined) log('NetStackConfig: ' + JSON.stringify(netStack).slice(0, 400));

  // NetStackPort / NetStackConfig.port is the RbxTransport (WebTransport) port.
  // The join reply's ServerPort belongs to the RakNet path; use it only as a
  // last-resort fallback when no NetStack port was provided.
  if (netStackPort == null && netStack.port != null) netStackPort = netStack.port;
  fqdn = gameFqdn ?? pick(rbxConfig, 'fqdn', 'host') ?? pick(netStack, 'host');

  // Prefer a public endpoint: GameFqdn (direct WT host), then Udmux address.
  // MachineAddress/ServerConnections point into the datacenter (10.x) and are
  // unreachable from outside; they're only useful for same-LAN cases.
  const endpointFromList = (list) => {
    if (!Array.isArray(list) || !list.length) return null;
    const first = list[0];
    if (typeof first === 'string') {
      const m = first.match(/^(.*?):(\d+)$/);
      return m ? { host: m[1], port: Number(m[2]) } : { host: first, port: null };
    }
    if (first && typeof first === 'object') {
      const host = pick(first, 'Address', 'address', 'Ip', 'ip', 'Host', 'host', 'Endpoint', 'endpoint');
      const port = pick(first, 'Port', 'port');
      if (host) {
        const m = String(host).match(/^(.*?):(\d+)$/);
        if (m) return { host: m[1], port: port != null ? Number(port) : Number(m[2]) };
        return { host, port: port != null ? Number(port) : null };
      }
    }
    return null;
  };
  let endpointNote = '';
  if (!fqdn) {
    const udmux = Array.isArray(udmuxEndpoints) && udmuxEndpoints.length ? udmuxEndpoints[0] : null;
    const udHost = udmux ? pick(udmux, 'Address', 'address', 'Ip', 'ip', 'Host', 'host') : null;
    if (udHost) {
      const m = String(udHost).match(/^\[?(.+?)\]?:(\d+)$/);
      fqdn = m ? m[1] : String(udHost);
      // Udmux announces the RakNet ServerPort here; the RbxTransport/QUIC
      // listener is on NetStackPort, so do not take the udmux port for WT.
      endpointNote = `using udmux endpoint ${fqdn} (udmux port ${m ? m[2] : pick(udmux, 'Port', 'port')})`;
    }
  }
  if (!fqdn) {
    const e1 = endpointFromList(serverConnections);
    if (e1) { fqdn = e1.host; netStackPort ??= e1.port; endpointNote = 'using ServerConnections (datacenter)'; }
  }
  if (endpointNote) log(endpointNote);
  if (!fqdn) throw new Error('join reply has no GameFqdn / server address');
  if (!netStackPort) {
    if (serverPort) { log('warning: NetStack port missing; falling back to ServerPort'); netStackPort = serverPort; }
    else throw new Error('join reply has no NetStackPort');
  }

  let capsRaw = pick(rbxConfig, 'serverCapabilities', 'ServerCapabilities', 'capabilities', 'caps')
    ?? pick(netStack, 'caps', 'capabilities')
    ?? pick(js, 'ServerCapabilities', 'serverCapabilities');
  let capsHex;
  if (capsRaw !== undefined) capsHex = normalizeCaps(capsRaw, log);
  else { log('warning: no capabilities in join reply; using zero mask'); capsHex = '0000000000000000'; }

  let certHashesRaw = pick(netStack, 'certHashes', 'CertHashes', 'certificateHashes')
    ?? pick(rbxConfig, 'certHashes', 'CertHashes');
  const certHashes = normalizeCertHashes(certHashesRaw, log);

  return {
    joinScript: js,
    fqdn,
    netStackPort: Number(netStackPort),
    serverPort: serverPort != null ? Number(serverPort) : null,
    clientPort: clientPort != null ? Number(clientPort) : null,
    serverConnections,
    udmuxEndpoints,
    directServerReturn: !!dsr,
    capsHex,
    capsRaw,
    certHashes,
    rbxTransportConfig: typeof rbxConfig === 'object' ? rbxConfig : null,
    netStackConfig: typeof netStack === 'object' ? netStack : null,
  };
}

function joinReplyHasUrlOnly(reply) {
  if (!reply || typeof reply !== 'object') return false;
  const hasUrl = typeof pick(reply, 'joinScriptUrl') === 'string';
  if (!hasUrl) return false;
  const js = pick(reply, 'joinScript');
  return !js; // no inline script
}

export function normalizeCaps(value, log = () => {}) {
  try {
    let big;
    if (typeof value === 'number') big = BigInt(Math.trunc(value));
    else if (typeof value === 'bigint') big = value;
    else if (typeof value === 'string') {
      const t = value.trim();
      if (t === '') return '0000000000000000';
      if (/^0x[0-9a-f]+$/i.test(t)) big = BigInt(t);
      else if (/^[0-9a-f]{16}$/i.test(t)) big = BigInt('0x' + t);
      else if (/^\d+$/.test(t)) big = BigInt(t);
      else if (/^[0-9a-f]+$/i.test(t)) big = BigInt('0x' + t);
      else throw new Error(`unrecognized caps format: ${t}`);
    } else throw new Error('unrecognized caps type: ' + typeof value);
    const hex = (big & 0xffffffffffffffffn).toString(16).padStart(16, '0');
    log(`capabilities = 0x${hex} (from ${JSON.stringify(value)})`);
    return hex;
  } catch (e) {
    log('warning: failed to parse capabilities: ' + e.message);
    return '0000000000000000';
  }
}

export function normalizeCertHashes(value, log = () => {}) {
  if (value === undefined || value === null) return [];
  const list = Array.isArray(value) ? value : [value];
  const out = [];
  for (const item of list) {
    if (item == null) continue;
    if (item instanceof ArrayBuffer) { out.push(new Uint8Array(item)); continue; }
    if (ArrayBuffer.isView(item)) { out.push(new Uint8Array(item.buffer, item.byteOffset, item.byteLength)); continue; }
    if (typeof item !== 'string') continue;
    const t = item.trim();
    let bytes = null;
    if (/^[0-9a-f]{64}$/i.test(t)) bytes = hexToBytes(t);
    else if (/^[A-Za-z0-9+/]{43}=?$/.test(t)) bytes = base64ToBytes(t);
    if (bytes && bytes.length === 32) out.push(bytes);
  }
  log(`certHashes: ${out.length} SHA-256 digest(s)` + (out.length ? '' : ' (none usable; TLS pinning disabled)'));
  return out;
}

export function hexToBytes(hex) {
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  return out;
}

export function base64ToBytes(b64) {
  const bin = atob(b64);
  const out = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
  return out;
}
