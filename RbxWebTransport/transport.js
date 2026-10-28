// WebTransport connection to a Roblox game server (RtcIo/RbxTransport over WT).
//
// Establishes:  https://<GameFqdn>:<NetStackPort>/wt?X-Rbx-Capabilities=<16hex>
// Per the binary, the server's capability extractor checks the
// X-Rbx-Capabilities *header* first and then falls back to the query string.
// Browsers cannot set custom headers on the extended CONNECT request, so the
// query form is used exclusively here.
//
// After the session is up, the client opens channel 0 of the "ctrl"
// application on a reliable bidi stream:
//   [0x06][0x01][appIndex:u8][channelId:u32 BE]
// and exchanges control messages.

import {
  CtrlMsg, CtrlMsgName, Reader, Writer,
  decodeCtrlMessage, decodeDatagramHeader, decodeStreamHeader,
  encodeOpenReliable, encodeStreamHeader, frameMessage, toHex,
} from './protocol.js';

export class RbxTransportClient extends EventTarget {
  constructor({ certHashes = [], ctrlAppIndex = 0, log }) {
    super();
    this.certHashes = certHashes;
    this.ctrlAppIndex = ctrlAppIndex;
    this.log = log || (() => {});
    this.transport = null;
    this.closed = false;

    this.ctrlStream = null;
    this.ctrlWriter = null;
    this.streams = new Map();     // streamId(hex) -> { stream, reader, remote, appIndex, channelId, buffer }
    this.datagramChannels = new Map(); // channelIdSigned(string) -> meta
    this.sendId = 0n;
    this.wireChannelId = 0;
    this.stats = { txBytes: 0, rxBytes: 0, txPackets: 0, rxPackets: 0, txDatagrams: 0, rxDatagrams: 0, streams: 0, messages: 0 };
    this._streamCounter = 0;
  }

  get connected() { return !!this.transport && !this.closed; }

  buildUrl(host, port, capsHex) {
    return `https://${host}:${port}/wt?X-Rbx-Capabilities=${capsHex}`;
  }

  async connect(host, port, capsHex) {
    const url = this.buildUrl(host, port, capsHex);
    this.log(`WebTransport -> ${url}`);
    if (this.certHashes.length) {
      this.log(`pinning ${this.certHashes.length} server certificate hash(es)`);
    } else {
      this.log('no certificate hashes; relying on WebPKI validation');
    }

    const options = {};
    if (this.certHashes.length) {
      options.serverCertificateHashes = this.certHashes.map((value) => ({ algorithm: 'sha-256', value }));
    }

    this.transport = new WebTransport(url, options);
    this.transport.closed
      .then((info) => {
        this.closed = true;
        this.log(`WebTransport session closed (code=${info?.closeCode ?? 'n/a'}, reason=${JSON.stringify(info?.reason ?? '')})`);
        this.dispatchEvent(new CustomEvent('closed', { detail: info }));
      })
      .catch((e) => {
        this.closed = true;
        this.log('WebTransport session closed with error: ' + (e?.message || e));
        this.dispatchEvent(new CustomEvent('closed', { detail: { error: String(e) } }));
      });

    this.log('awaiting ready (QUIC + HTTP/3 CONNECT)...');
    await this.transport.ready;
    this.log('WebTransport session established');
    this.dispatchEvent(new CustomEvent('ready'));

    this._acceptIncomingStreams();
    this._acceptIncomingDatagrams();
    this._acceptBidirectionalStreams();

    return this;
  }

  _acceptBidirectionalStreams() {
    const incoming = this.transport.incomingBidirectionalStreams;
    if (!incoming) return;
    this._bidiReader = incoming.getReader();
    this._pumpBidi();
  }

  async _pumpBidi() {
    try {
      while (true) {
        const { value: stream, done } = await this._bidiReader.read();
        if (done) break;
        this._trackStream(stream, true);
      }
    } catch (e) {
      if (!this.closed) this.log('bidi stream accept loop ended: ' + e.message);
    }
  }

  _acceptIncomingStreams() {
    const incoming = this.transport.incomingUnidirectionalStreams;
    if (!incoming) return;
    this._uniReader = incoming.getReader();
    this._pumpUni();
  }

  async _pumpUni() {
    try {
      while (true) {
        const { value: stream, done } = await this._uniReader.read();
        if (done) break;
        this._trackStream(stream, false);
      }
    } catch (e) {
      if (!this.closed) this.log('uni stream accept loop ended: ' + e.message);
    }
  }

  _trackStream(stream, bidi) {
    this._streamCounter++;
    if (bidi && typeof stream.readable === 'undefined') {
      // Incoming bidi streams in some Chrome versions are plain streams.
      this._pumpStream(stream, null, bidi);
      return;
    }
    this.stats.streams++;
    this.stats.rxPackets++;
    this.log(`<- new ${bidi ? 'bidi' : 'uni'} stream #${this._streamCounter}`);
    this._pumpStream(stream, stream.readable ? stream.readable : stream, bidi);
  }

  async _pumpStream(stream, readable, bidi) {
    const reader = (readable || stream).getReader();
    let buffer = new Uint8Array(0);
    let header = null;
    try {
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        this.stats.rxBytes += value.length;
        buffer = concat(buffer, value);
        // Parse the stream-open header first.
        if (!header) {
          if (buffer.length < 7) continue;
          try {
            header = decodeStreamHeader(buffer);
          } catch (e) {
            this.log('failed to parse stream header: ' + e.message);
            header = { invalid: true };
          }
          buffer = buffer.subarray(header.headerLen ?? 7);
          this.log(`<- stream header magic=0x${header.magic.toString(16)} kind=${header.kind} appIndex=${header.appIndex} channelId=${header.channelId}`);
        }
        // Then decode varint-length framed messages.
        let progressed = true;
        while (progressed && buffer.length) {
          progressed = false;
          try {
            const r = new Reader(buffer);
            const { value: len } = r.varint();
            const length = Number(len);
            if (buffer.length - r.offset >= length) {
              const msg = buffer.subarray(r.offset, r.offset + length);
              this._onStreamMessage(header, msg, stream, bidi);
              buffer = buffer.subarray(r.offset + length);
              progressed = true;
            }
          } catch { /* need more bytes */ }
        }
      }
    } catch (e) {
      if (!this.closed) this.log('stream read error: ' + e.message);
    }
    this.log(`<- stream #${this._streamCounter} closed`);
  }

  _onStreamMessage(header, payload, stream, bidi) {
    this.stats.messages++;
    this.stats.rxPackets++;
    const hex = toHex(payload.subarray(0, 64));
    this.log(`<- stream msg (app=${header?.appIndex} ch=${header?.channelId}) len=${payload.length}\n     ${hex}${payload.length > 64 ? ' ...' : ''}`);
    this.dispatchEvent(new CustomEvent('message', { detail: { header, payload, stream, bidi } }));

    if (header?.appIndex === this.ctrlAppIndex && header?.channelId === 0) {
      const decoded = decodeCtrlMessage(payload);
      this.log(`     ctrl: ${decoded.name} ${JSON.stringify(Object.fromEntries(Object.entries(decoded).filter(([k]) => !['name', 'raw'].includes(k)).map(([k, v]) => [k, typeof v === 'bigint' ? v.toString() : v])))}`);
      this.dispatchEvent(new CustomEvent('ctrl', { detail: decoded }));
    }
  }

  _acceptIncomingDatagrams() {
    if (!this.transport.datagrams) return;
    this._datagramReader = this.transport.datagrams.readable.getReader();
    this._datagramWriter = this.transport.datagrams.writable.getWriter();
    this._pumpDatagrams();
  }

  async _pumpDatagrams() {
    try {
      while (true) {
        const { value, done } = await this._datagramReader.read();
        if (done) break;
        this.stats.rxDatagrams++;
        this.stats.rxPackets++;
        this.stats.rxBytes += value.length;
        try {
          const hdr = decodeDatagramHeader(value);
          const payload = value.subarray(hdr.headerLen);
          this.log(`<- datagram len=${value.length} channelId=${hdr.channelIdSigned} sendId=${hdr.sendId} fragSize=${hdr.fragSize}${hdr.fragSize ? ' frag=' + hdr.frag : ''}`);
          this.dispatchEvent(new CustomEvent('datagram', { detail: { header: hdr, payload } }));
        } catch (e) {
          this.log(`<- datagram (undecodable header) len=${value.length} hex=${toHex(value.subarray(0, 32))}`);
        }
      }
    } catch (e) {
      if (!this.closed) this.log('datagram loop ended: ' + e.message);
    }
  }

  /**
   * Opens a reliable channel on a brand-new bidi stream:
   * sends the stream-open header then the given payload framed.
   */
  async openReliableStream(appIndex, channelId) {
    this.log(`-> open reliable stream app=${appIndex} channel=${channelId}`);
    const stream = await this.transport.createBidirectionalStream();
    const writer = stream.writable.getWriter();
    const header = encodeStreamHeader(appIndex, channelId);
    await writer.write(header);
    this.stats.txBytes += header.length;
    this.stats.txPackets++;
    // Read side of our own stream (server control replies can come here).
    this._pumpStream(stream, stream.readable, true);
    return { stream, writer };
  }

  async openCtrlChannel() {
    const { stream, writer } = await this.openReliableStream(this.ctrlAppIndex, 0);
    this.ctrlStream = stream;
    this.ctrlWriter = writer;
    this.log('ctrl channel opened (appIndex=' + this.ctrlAppIndex + ', channel 0)');
    this.dispatchEvent(new CustomEvent('ctrlready'));
    return { stream, writer };
  }

  async sendCtrl(payload) {
    if (!this.ctrlWriter) throw new Error('ctrl channel not open');
    const framed = frameMessage(payload);
    await this.ctrlWriter.write(framed);
    this.stats.txBytes += framed.length;
    this.stats.txPackets++;
    this.log(`-> ctrl msg len=${payload.length} hex=${toHex(payload.subarray(0, 64))}`);
  }

  async openUnreliableChannel(appIndex, channelId) {
    const wireId = this.wireChannelId;
    this.wireChannelId++;
    const payload = new Writer()
      .u8(CtrlMsg.OpenUnreliable)
      .u8(appIndex)
      .u8(channelId)
      .u32be(0)
      .u32be(wireId)
      .toBytes();
    await this.sendCtrl(payload);
    this.log(`-> requested unreliable channel app=${appIndex} channel=${channelId} wireId=${wireId}`);
    return wireId;
  }

  async sendDatagram(channelIdSigned, payload) {
    if (!this.transport?.datagrams) throw new Error('datagrams not supported');
    if (!this._datagramWriter) this._datagramWriter = this.transport.datagrams.writable.getWriter();
    const header = new Writer().svarint(channelIdSigned).varint(this.sendId++).varint(0).toBytes();
    const packet = concat(header, payload);
    await this._datagramWriter.write(packet);
    this.stats.txDatagrams++;
    this.stats.txPackets++;
    this.stats.txBytes += packet.length;
    this.log(`-> datagram len=${packet.length} channelId=${channelIdSigned} sendId=${this.sendId - 1n}`);
  }

  async close() {
    if (this.closed) return;
    this.closed = true;
    try { this.transport?.close({ closeCode: 0, reason: 'client closing' }); } catch { /* ignore */ }
    this.log('connection closed by client');
  }
}

function concat(a, b) {
  const out = new Uint8Array(a.length + b.length);
  out.set(a, 0);
  out.set(b, a.length);
  return out;
}

export { CtrlMsg, CtrlMsgName };
