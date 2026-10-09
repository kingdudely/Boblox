// Node test harness for the protocol codecs (no browser APIs used).
import {
  Reader, Writer, varintSize, encodeStreamHeader, decodeStreamHeader,
  frameMessage, decodeDatagramHeader, encodeOpenReliable, encodeOpenUnreliable,
  encodeCloseUnreliable, decodeCtrlMessage, toHex, CtrlMsg,
} from '../src/protocol.js';

let pass = 0, fail = 0;
function check(name, got, want) {
  const ok = got === want;
  if (ok) pass++;
  else { fail++; console.log(`FAIL ${name}: got ${got} want ${want}`); }
}

// QUIC varint round trips at every boundary
for (const v of [0n, 1n, 62n, 63n, 64n, 16383n, 16384n, 1073741823n, 1073741824n, 4611686018427387903n]) {
  const w = new Writer().varint(v).toBytes();
  check(`varint size ${v}`, w.length, varintSize(v));
  check(`varint value ${v}`, new Reader(w).varint().value, v);
}
check('varint 2-byte prefix', new Writer().varint(64n).toBytes()[0] & 0xc0, 0x40);
check('varint 4-byte prefix', new Writer().varint(16384n).toBytes()[0] & 0xc0, 0x80);
check('varint 8-byte prefix', new Writer().varint(1073741824n).toBytes()[0] & 0xc0, 0xc0);

// zigzag signed varints
for (const v of [0n, 1n, -1n, 2n, -2n, 1000000n, -1000000n]) {
  const w = new Writer().svarint(v).toBytes();
  check(`svarint ${v}`, new Reader(w).svarint(), v);
}

// stream header
{
  const b = encodeStreamHeader(4, 0xdeadbeef);
  const d = decodeStreamHeader(b);
  check('stream magic', d.magic, 6);
  check('stream kind', d.kind, 1);
  check('stream app', d.appIndex, 4);
  check('stream chan', d.channelId, 0xdeadbeef);
}

// framing
{
  const payload = new Uint8Array([9, 8, 7]);
  const f = frameMessage(payload);
  const r = new Reader(f);
  check('frame len', Number(r.varint().value), 3);
  check('frame body', toHex(r.bytesN(3)), '09 08 07');
}

// control messages
{
  const d = decodeCtrlMessage(encodeOpenReliable(1, 2));
  check('openReliable type', d.type, CtrlMsg.OpenReliable);
  check('openReliable app', d.appIndex, 1);
  check('openReliable chan', d.channelId, 2);
}
{
  const d = decodeCtrlMessage(encodeOpenUnreliable(3, 4, 77));
  check('openUnreliable type', d.type, CtrlMsg.OpenUnreliable);
  check('openUnreliable wireId', d.wireId, 77);
}
{
  const d = decodeCtrlMessage(encodeCloseUnreliable(5, 1234n, 9n));
  check('close type', d.type, CtrlMsg.CloseUnreliable);
  check('close reason', d.reason, 5);
  check('close wireId', d.wireId, 1234n);
  check('close chan', d.channelId, 9n);
}

// datagram header
{
  const w = new Writer().svarint(-3).varint(7).varint(1000).varint(2).toBytes();
  const d = decodeDatagramHeader(w);
  check('dg channel', d.channelIdSigned, -3n);
  check('dg sendId', d.sendId, 7n);
  check('dg fragSize', d.fragSize, 1000n);
  check('dg frag', d.frag, 2n);
}

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
