// Client page controller: join -> WebTransport connect -> protocol session.
import { getAuthenticatedUser, joinGame, extractTransportInfo, normalizeCaps, hexToBytes } from './join.js';
import { RbxTransportClient } from './transport.js';
import {
  Reader, Writer, encodeStreamHeader, decodeStreamHeader, frameMessage,
  encodeOpenReliable, decodeCtrlMessage, decodeDatagramHeader, toHex,
} from './protocol.js';

const $ = (id) => document.getElementById(id);
const logEl = $('log');
const statusEl = $('status');
const statsEl = $('stats');
const packetsEl = $('packets');

let client = null;
let packetCount = 0;

function log(msg) {
  const ts = new Date().toISOString().slice(11, 23);
  logEl.textContent += `[${ts}] ${msg}\n`;
  logEl.scrollTop = logEl.scrollHeight;
}

function setStatus(text, cls = '') {
  statusEl.textContent = text;
  statusEl.className = 'status ' + cls;
}

function addPacket(dir, kind, detail) {
  packetCount++;
  if (packetCount === 1) packetsEl.innerHTML = '';
  const div = document.createElement('div');
  div.className = 'pkt';
  const dirCls = dir === 'in' ? 'dir-in' : 'dir-out';
  div.innerHTML = `<span class="t">${new Date().toISOString().slice(11, 23)}</span> <span class="${dirCls}">${dir === 'in' ? '&lt;-' : '-&gt;'}</span> <b>${kind}</b> ${escapeHtml(detail || '')}`;
  packetsEl.prepend(div);
  while (packetsEl.childElementCount > 400) packetsEl.lastElementChild.remove();
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}

function updateStats() {
  if (!client) { statsEl.textContent = ''; return; }
  const s = client.stats;
  statsEl.textContent =
    `tx: ${s.txPackets} packets / ${s.txBytes} B (${s.txDatagrams} datagrams)\n` +
    `rx: ${s.rxPackets} packets / ${s.rxBytes} B (${s.rxDatagrams} datagrams)\n` +
    `streams: ${s.streams} | messages: ${s.messages}`;
}
setInterval(updateStats, 500);

// ---------------------------------------------------------------------------
// Codec self-test: round-trips every message type we implemented.
// ---------------------------------------------------------------------------
function selfTest() {
  const results = [];
  const eq = (name, a, b) => results.push({ name, ok: a === b, a, b });

  // varint round trip
  for (const v of [0n, 63n, 64n, 16383n, 16384n, 1073741823n, 1073741824n, 4611686018427387903n]) {
    const w = new Writer().varint(v).toBytes();
    const r = new Reader(w).varint();
    eq(`varint(${v})`, r.value, v);
  }
  // zigzag
  for (const v of [0n, 1n, -1n, 12345n, -12345n]) {
    const w = new Writer().svarint(v).toBytes();
    eq(`svarint(${v})`, new Reader(w).svarint(), v);
  }
  // stream header
  {
    const b = encodeStreamHeader(3, 0x11223344);
    const d = decodeStreamHeader(b);
    eq('stream header magic', d.magic, 0x06);
    eq('stream header app', d.appIndex, 3);
    eq('stream header chan', d.channelId, 0x11223344);
  }
  // ctrl messages
  {
    const d = decodeCtrlMessage(encodeOpenReliable(2, 5));
    eq('ctrl OpenReliable type', d.type, 1);
    eq('ctrl OpenReliable app', d.appIndex, 2);
    eq('ctrl OpenReliable chan', d.channelId, 5);
  }
  // datagram header
  {
    const w = new Writer().svarint(-7).varint(42).varint(0).toBytes();
    const d = decodeDatagramHeader(w);
    eq('datagram channel', d.channelIdSigned, -7n);
    eq('datagram sendId', d.sendId, 42n);
  }
  // framing
  {
    const payload = new Uint8Array([1, 2, 3, 4, 5]);
    const framed = frameMessage(payload);
    const r = new Reader(framed);
    eq('frame len', Number(r.varint().value), 5);
    eq('frame body', r.u32be === undefined ? 0 : 1, 1);
  }

  const failed = results.filter(r => !r.ok);
  for (const r of results) {
    if (!r.ok) log(`SELFTEST FAIL ${r.name}: got ${r.a} expected ${r.b}`);
  }
  if (!failed.length) {
    log(`SELFTEST OK: ${results.length} checks passed`);
    setStatus('codec self-test passed', 'ok');
  } else {
    setStatus(`${failed.length} self-test failures (see log)`, 'err');
  }
}

// ---------------------------------------------------------------------------
// Main connection flow
// ---------------------------------------------------------------------------
async function doJoin() {
  const placeId = $('placeId').value.trim();
  if (!placeId) throw new Error('Place ID is required');
  const universeId = $('universeId').value.trim() || undefined;
  const jobId = $('jobId').value.trim() || undefined;

  setStatus('checking Roblox session...', 'busy');
  const user = await getAuthenticatedUser(log);

  setStatus('requesting join...', 'busy');
  const join = await joinGame({ placeId, universeId, jobId }, log);
  log(`join API responded via ${join.api}`);
  const info = extractTransportInfo(join.response, log);
  log(`endpoint: ${info.fqdn}:${info.netStackPort} (caps=0x${info.capsHex})`);
  return { user, join, info };
}

async function connectFlow({ connectOnly = false } = {}) {
  $('connectBtn').disabled = true;
  $('joinOnlyBtn').disabled = true;
  $('disconnectBtn').disabled = false;
  try {
    let info;
    if (connectOnly) {
      // reuse overrides only
      const host = $('hostOverride').value.trim();
      const port = Number($('portOverride').value.trim());
      if (!host || !port) throw new Error('host/port overrides are required for direct connect');
      const capsHex = normalizeCaps($('capsOverride').value.trim() || '0', log);
      const certHashes = ($('certOverride').value.trim() ? $('certOverride').value.trim().split(/[\s,]+/).filter(Boolean).map(hexToBytes) : []);
      info = { fqdn: host, netStackPort: port, capsHex, certHashes };
    } else {
      const { info: joined } = await doJoin();
      info = joined;
      if ($('capsOverride').value.trim()) info.capsHex = normalizeCaps($('capsOverride').value.trim(), log);
      if ($('hostOverride').value.trim()) info.fqdn = $('hostOverride').value.trim();
      if ($('portOverride').value.trim()) info.netStackPort = Number($('portOverride').value.trim());
      if ($('certOverride').value.trim()) {
        info.certHashes = $('certOverride').value.trim().split(/[\s,]+/).filter(Boolean).map(hexToBytes);
      }
    }

    const ctrlAppIndex = Number($('ctrlAppIndex').value) || 0;

    client = new RbxTransportClient({ certHashes: info.certHashes, ctrlAppIndex, log });

    client.addEventListener('ready', async () => {
      setStatus(`WebTransport connected to ${info.fqdn}:${info.netStackPort}`, 'ok');
      addPacket('in', 'WT_SESSION', `${info.fqdn}:${info.netStackPort}`);
      try {
        await client.openCtrlChannel();
        setStatus('ctrl channel open; session live', 'ok');
        addPacket('out', 'CTRL_OPEN', `appIndex=${ctrlAppIndex} channel=0`);
      } catch (e) {
        log('failed to open ctrl channel: ' + e.message);
        setStatus('ctrl channel failed: ' + e.message, 'err');
      }
    });

    client.addEventListener('ctrl', (ev) => {
      addPacket('in', 'CTRL:' + ev.detail.name, JSON.stringify(ev.detail, (k, v) => typeof v === 'bigint' ? v.toString() : v));
    });

    client.addEventListener('datagram', (ev) => {
      addPacket('in', 'DATAGRAM', `ch=${ev.detail.header.channelIdSigned} send=${ev.detail.header.sendId} bytes=${ev.detail.payload.length}`);
    });

    client.addEventListener('message', (ev) => {
      addPacket('in', 'STREAM_MSG', `app=${ev.detail.header?.appIndex} ch=${ev.detail.header?.channelId} len=${ev.detail.payload.length}`);
    });

    client.addEventListener('closed', () => {
      setStatus('session closed', 'err');
      $('connectBtn').disabled = false;
      $('disconnectBtn').disabled = true;
      updateStats();
    });

    await client.connect(info.fqdn, info.netStackPort, info.capsHex);
    // Give the ctrl-open a moment; then keep the page logging until closed.
  } catch (e) {
    log('ERROR: ' + (e?.stack || e));
    setStatus(String(e?.message || e), 'err');
    $('connectBtn').disabled = false;
    $('joinOnlyBtn').disabled = false;
    $('disconnectBtn').disabled = true;
  }
}

$('connectBtn').addEventListener('click', () => connectFlow({ connectOnly: false }));
$('joinOnlyBtn').addEventListener('click', async () => {
  try {
    $('joinOnlyBtn').disabled = true;
    const { info } = await doJoin();
    setStatus(`join ok: ${info.fqdn}:${info.netStackPort}`, 'ok');
  } catch (e) {
    log('ERROR: ' + (e?.stack || e));
    setStatus(String(e?.message || e), 'err');
  } finally {
    $('joinOnlyBtn').disabled = false;
  }
});
$('selftestBtn').addEventListener('click', selfTest);
$('disconnectBtn').addEventListener('click', async () => {
  await client?.close();
  $('disconnectBtn').disabled = true;
  $('connectBtn').disabled = false;
  $('joinOnlyBtn').disabled = false;
});
$('clearBtn').addEventListener('click', () => {
  logEl.textContent = '';
  packetsEl.innerHTML = '<em>No packets yet.</em>';
  packetCount = 0;
});

log('Roblox In Browser client ready.');
log('Extension origin: ' + location.origin);
