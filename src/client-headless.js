// Headless entry point: no UI. Load this page and the client auto-runs the
// join -> connect flow for DEFAULT_PLACE_ID, logging everything to the console
// and to window.__rbx for programmatic inspection.
//
// Overrides via query string, e.g.
//   client-headless.html?placeId=1818&universeId=13058&jobId=...&ctrlAppIndex=0
//
// Gamejoin is gated server-side on the client User-Agent; the extension sets
// "Roblox/WinInet" for *.roblox.com API calls via a declarativeNetRequest rule
// (rules.json), so no manual UA spoofing is needed here.

import { getAuthenticatedUser, joinGame, extractTransportInfo, normalizeCaps, hexToBytes } from './join.js';
import { RbxTransportClient } from './transport.js';

const params = new URLSearchParams(location.search);
const DEFAULT_PLACE_ID = 1818;

const cfg = {
  placeId: params.get('placeId') || String(DEFAULT_PLACE_ID),
  universeId: params.get('universeId') || undefined,
  jobId: params.get('jobId') || undefined,
  ctrlAppIndex: Number(params.get('ctrlAppIndex') ?? '0'),
  capsOverride: params.get('caps') || undefined,
  hostOverride: params.get('host') || undefined,
  portOverride: params.get('port') ? Number(params.get('port')) : undefined,
  certOverride: params.get('certHashes') ? params.get('certHashes').split(/[\s,]+/).filter(Boolean) : undefined,
};

const state = {
  phase: 'init',
  user: null,
  joinApi: null,
  joinReply: null,
  transportInfo: null,
  session: null,
  events: [],
  errors: [],
  packets: [],
};
window.__rbx = state;

function log(...args) {
  const line = args.map(a => (typeof a === 'string' ? a : JSON.stringify(a, (k, v) => typeof v === 'bigint' ? v.toString() : v))).join(' ');
  console.log('[rbx]', line);
  state.events.push({ t: Date.now(), line });
}

function fail(e, phase) {
  state.phase = phase + ':error';
  state.errors.push({ phase, message: String(e?.message || e), stack: String(e?.stack || '') });
  console.error('[rbx] ERROR in', phase, e);
}

function recordPacket(dir, kind, detail) {
  state.packets.push({ t: Date.now(), dir, kind, detail });
  if (state.packets.length > 2000) state.packets.splice(0, 500);
}

async function main() {
  try {
    state.phase = 'auth';
    const user = await getAuthenticatedUser(log);
    state.user = user;

    state.phase = 'join';
    const join = await joinGame({
      placeId: cfg.placeId,
      universeId: cfg.universeId,
      jobId: cfg.jobId,
    }, log);
    state.joinApi = join.api;
    state.joinReply = join.reply;
    log(`join reply: ${JSON.stringify(join.reply).slice(0, 1500)}`);

    state.phase = 'parse';
    const info = await extractTransportInfo(join.reply, log);
    state.transportInfo = {
      fqdn: info.fqdn,
      netStackPort: info.netStackPort,
      capsHex: info.capsHex,
      certHashes: info.certHashes.map(h => Array.from(h).map(b => b.toString(16).padStart(2, '0')).join('')),
      serverPort: info.serverPort,
      clientPort: info.clientPort,
      directServerReturn: info.directServerReturn,
    };
    if (cfg.hostOverride) info.fqdn = cfg.hostOverride;
    if (cfg.portOverride) info.netStackPort = cfg.portOverride;
    if (cfg.capsOverride) info.capsHex = normalizeCaps(cfg.capsOverride, log);
    if (cfg.certOverride) info.certHashes = cfg.certOverride.map(hexToBytes);

    state.phase = 'connect';
    const client = new RbxTransportClient({
      certHashes: info.certHashes,
      ctrlAppIndex: cfg.ctrlAppIndex,
      log,
    });
    state.session = client;

    client.addEventListener('ready', () => {
      recordPacket('in', 'WT_SESSION', `${info.fqdn}:${info.netStackPort}`);
      state.phase = 'session-open';
    });
    client.addEventListener('ctrlready', () => {
      recordPacket('out', 'CTRL_OPEN', `appIndex=${cfg.ctrlAppIndex} channel=0`);
      state.phase = 'ctrl-open';
    });
    client.addEventListener('ctrl', (ev) => {
      const d = ev.detail;
      recordPacket('in', 'CTRL:' + d.name, JSON.stringify(d, (k, v) => typeof v === 'bigint' ? v.toString() : v));
    });
    client.addEventListener('datagram', (ev) => {
      recordPacket('in', 'DATAGRAM', `ch=${ev.detail.header.channelIdSigned} send=${ev.detail.header.sendId} bytes=${ev.detail.payload.length}`);
    });
    client.addEventListener('message', (ev) => {
      recordPacket('in', 'STREAM_MSG', `app=${ev.detail.header?.appIndex} ch=${ev.detail.header?.channelId} len=${ev.detail.payload.length}`);
    });
    client.addEventListener('closed', (ev) => {
      recordPacket('in', 'WT_CLOSED', JSON.stringify(ev.detail ?? {}));
      state.phase = 'closed';
    });

    await client.connect(info.fqdn, info.netStackPort, info.capsHex);
    await client.openCtrlChannel();
  } catch (e) {
    fail(e, state.phase);
  }
}

main();

// Expose helpers for manual driving from the console.
window.__rbxConnectOnly = async ({ host, port, caps, certHashes, ctrlAppIndex = 0 }) => {
  const client = new RbxTransportClient({
    certHashes: (certHashes || []).map(hexToBytes),
    ctrlAppIndex,
    log,
  });
  state.session = client;
  client.addEventListener('ready', () => recordPacket('in', 'WT_SESSION', `${host}:${port}`));
  client.addEventListener('ctrl', (ev) => recordPacket('in', 'CTRL:' + ev.detail.name, JSON.stringify(ev.detail, (k, v) => typeof v === 'bigint' ? v.toString() : v)));
  client.addEventListener('datagram', (ev) => recordPacket('in', 'DATAGRAM', `ch=${ev.detail.header.channelIdSigned} send=${ev.detail.header.sendId}`));
  await client.connect(host, port, caps);
  await client.openCtrlChannel();
  return client;
};
