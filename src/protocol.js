// RbxTransport wire-format codecs, reconstructed from the Roblox client binaries.
//
// Layers (outermost first):
//   QUIC / HTTP3 + WebTransport (handled by the browser)
//     -> application channels (reliable = bidi stream, unreliable = datagram)
//          reliable stream header:  [0x06][0x01][appIndex:u8][channelId:u32 BE]
//          reliable message frame:  QUIC varint(length) + payload
//          unreliable datagram:     varint(zigzag channelId) varint(sendId) varint(fragSize) [varint(frag)] payload
//          control messages (app="ctrl", channel 0):
//             OpenReliableChannelControl:   [type=1][appIndex:u8][channelId:u8][streamId:u32 BE]
//             OpenUnreliableChannelControl: [type=2][appIndex:u8][channelId:u8][streamId:u32 BE][wireId:u32 BE]
//             CloseUnreliableChannelControl:[type=3][reason:u8][wireId:varint][channelId:varint]
//
// The RUPP prefix layer sits below QUIC (it is prepended to UDP packets) and is
// intentionally NOT implemented here: production runs with
// RbxTransportPureQuicSupport=true, and the server auto-detects raw QUIC by the
// fixed bit 0x40. A browser can only emit raw QUIC, which is exactly that case.

export const CTRL_APP_ID = 0x6374726c; // "ctrl"

export class Reader {
  constructor(bytes, offset = 0) {
    this.bytes = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    this.offset = offset;
  }
  get remaining() { return this.bytes.length - this.offset; }
  u8() {
    if (this.remaining < 1) throw new RangeError('reader underflow (u8)');
    return this.bytes[this.offset++];
  }
  u16be() {
    if (this.remaining < 2) throw new RangeError('reader underflow (u16)');
    const v = (this.bytes[this.offset] << 8) | this.bytes[this.offset + 1];
    this.offset += 2;
    return v;
  }
  u32be() {
    if (this.remaining < 4) throw new RangeError('reader underflow (u32)');
    const b = this.bytes, o = this.offset;
    const v = ((b[o] * 0x1000000) + ((b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3])) >>> 0;
    this.offset += 4;
    return v;
  }
  u64be() {
    if (this.remaining < 8) throw new RangeError('reader underflow (u64)');
    const hi = this.u32be(), lo = this.u32be();
    return (BigInt(hi) << 32n) | BigInt(lo);
  }
  bytesN(n) {
    if (this.remaining < n) throw new RangeError('reader underflow (bytes)');
    const out = this.bytes.subarray(this.offset, this.offset + n);
    this.offset += n;
    return out;
  }
  // RFC 9000 QUIC variable-length integer.
  varint() {
    if (this.remaining < 1) throw new RangeError('reader underflow (varint)');
    const first = this.bytes[this.offset];
    const len = 1 << (first >> 6);
    if (this.remaining < len) throw new RangeError('reader underflow (varint body)');
    return { value: this.readVarintRaw(), len };
  }
  readVarintRaw() {
    const first = this.bytes[this.offset];
    const len = 1 << (first >> 6);
    let v = BigInt(first & 0x3f);
    for (let i = 1; i < len; i++) v = (v << 8n) | BigInt(this.bytes[this.offset + i]);
    this.offset += len;
    return v;
  }
  // Zigzag-decoded varint (signed): (n >> 1) ^ -(n & 1)
  svarint() {
    const { value } = this.varint();
    const mag = value >> 1n;
    return (value & 1n) === 1n ? -mag - 1n : mag;
  }
}

export class Writer {
  constructor() { this.buf = new Uint8Array(256); this.len = 0; }
  ensure(extra) {
    if (this.len + extra <= this.buf.length) return;
    let cap = this.buf.length * 2;
    while (cap < this.len + extra) cap *= 2;
    const nb = new Uint8Array(cap);
    nb.set(this.buf.subarray(0, this.len));
    this.buf = nb;
  }
  u8(v) { this.ensure(1); this.buf[this.len++] = v & 0xff; return this; }
  u16be(v) { this.ensure(2); this.buf[this.len++] = (v >> 8) & 0xff; this.buf[this.len++] = v & 0xff; return this; }
  u32be(v) {
    this.ensure(4);
    this.buf[this.len++] = (v >>> 24) & 0xff;
    this.buf[this.len++] = (v >>> 16) & 0xff;
    this.buf[this.len++] = (v >>> 8) & 0xff;
    this.buf[this.len++] = v & 0xff;
    return this;
  }
  bytes(arr) { this.ensure(arr.length); this.buf.set(arr, this.len); this.len += arr.length; return this; }
  // RFC 9000 QUIC varint.
  varint(v) {
    v = BigInt(v);
    if (v < 0n) throw new RangeError('varint must be non-negative');
    let len;
    if (v <= 0x3fn) len = 1;
    else if (v <= 0x3fffn) len = 2;
    else if (v <= 0x3fffffffn) len = 4;
    else if (v <= 0x3fffffffffffffffn) len = 8;
    else throw new RangeError('varint too large');
    this.ensure(len);
    const o = this.len;
    for (let i = len - 1; i >= 0; i--) { this.buf[o + i] = Number(v & 0xffn); v >>= 8n; }
    this.buf[o] |= (len === 1 ? 0 : len === 2 ? 0x40 : len === 4 ? 0x80 : 0xc0);
    this.len += len;
    return this;
  }
  svarint(v) {
    v = BigInt(v);
    const zz = v >= 0n ? (v << 1n) : ((-v << 1n) - 1n);
    return this.varint(zz);
  }
  toBytes() { return this.buf.slice(0, this.len); }
}

export function varintSize(v) {
  v = BigInt(v);
  if (v <= 0x3fn) return 1;
  if (v <= 0x3fffn) return 2;
  if (v <= 0x3fffffffn) return 4;
  return 8;
}

export function concatBytes(...chunks) {
  let total = 0;
  for (const c of chunks) total += c.length;
  const out = new Uint8Array(total);
  let o = 0;
  for (const c of chunks) { out.set(c, o); o += c.length; }
  return out;
}

// ---------------------------------------------------------------------------
// Reliable stream header
// ---------------------------------------------------------------------------
export const STREAM_HEADER_MAGIC = 0x06;

export function encodeStreamHeader(appIndex, channelId) {
  return new Writer().u8(STREAM_HEADER_MAGIC).u8(0x01).u8(appIndex).u32be(channelId).toBytes();
}

export function decodeStreamHeader(bytes) {
  const r = new Reader(bytes);
  const magic = r.u8();
  const kind = r.u8();
  const appIndex = r.u8();
  const channelId = r.u32be();
  return { magic, kind, appIndex, channelId, headerLen: r.offset };
}

// ---------------------------------------------------------------------------
// Reliable message framing (QUIC varint length prefix)
// ---------------------------------------------------------------------------
export function frameMessage(payload) {
  return concatBytes(new Writer().varint(payload.length).toBytes(), payload);
}

// ---------------------------------------------------------------------------
// Unreliable datagram header
// ---------------------------------------------------------------------------
export function encodeDatagramHeader(channelIdSigned, sendId, fragSize = 0, frag = 0) {
  const w = new Writer().svarint(channelIdSigned).varint(sendId).varint(fragSize);
  if (fragSize !== 0) w.varint(frag);
  return w.toBytes();
}

export function decodeDatagramHeader(bytes) {
  const r = new Reader(bytes);
  const channelIdSigned = r.svarint();
  const sendId = r.varint().value;
  const fragSize = r.varint().value;
  let frag = 0n;
  if (fragSize !== 0n) frag = r.varint().value;
  return { channelIdSigned, sendId: BigInt(sendId), fragSize, frag, headerLen: r.offset };
}

// ---------------------------------------------------------------------------
// Control-channel messages
// ---------------------------------------------------------------------------
export const CtrlMsg = Object.freeze({
  OpenReliable: 1,
  OpenUnreliable: 2,
  CloseUnreliable: 3,
  Handshake: 4,
  AppControl: 5,
  FlowControl: 6,
  Ack: 7,
  Loss: 8,
  AppFin: 9,
  ConnClose: 10,
  StreamClose: 11,
  StreamAccepted: 12,
  NewAppConnection: 13,
  AcceptConnection: 14,
  AcceptH3: 15,
  AcceptWtSession: 16,
});

export const CtrlMsgName = Object.fromEntries(Object.entries(CtrlMsg).map(([k, v]) => [v, k]));

export function encodeOpenReliable(appIndex, channelId) {
  return new Writer().u8(CtrlMsg.OpenReliable).u8(appIndex).u8(channelId).u32be(0).toBytes();
}

export function encodeOpenUnreliable(appIndex, channelId, wireId = 0) {
  return new Writer().u8(CtrlMsg.OpenUnreliable).u8(appIndex).u8(channelId).u32be(0).u32be(wireId).toBytes();
}

export function encodeCloseUnreliable(reason, wireId, channelId) {
  return new Writer().u8(CtrlMsg.CloseUnreliable).u8(reason).varint(wireId).varint(channelId).toBytes();
}

export function decodeCtrlMessage(bytes) {
  const r = new Reader(bytes);
  const type = r.u8();
  const out = { type, name: CtrlMsgName[type] ?? `Unknown(${type})` };
  try {
    switch (type) {
      case CtrlMsg.OpenReliable: {
        out.appIndex = r.u8();
        out.channelId = r.u8();
        out.streamId = r.u32be();
        break;
      }
      case CtrlMsg.OpenUnreliable: {
        out.appIndex = r.u8();
        out.channelId = r.u8();
        out.streamId = r.u32be();
        out.wireId = r.u32be();
        break;
      }
      case CtrlMsg.CloseUnreliable: {
        out.reason = r.u8();
        out.wireId = r.varint().value;
        out.channelId = r.varint().value;
        break;
      }
      default:
        out.raw = bytes;
        break;
    }
  } catch (e) {
    out.error = String(e);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------
export function toHex(bytes, sep = ' ') {
  return Array.from(bytes).map(b => b.toString(16).padStart(2, '0')).join(sep);
}
