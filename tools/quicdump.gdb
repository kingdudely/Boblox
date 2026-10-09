set pagination off
set confirm off

python
import gdb, os, time, struct

import os as _os; ROOT = _os.environ.get("RBX_ROOT", _os.getcwd())
RUN = os.environ.get("QD_DIR", os.path.join(ROOT, "run", "quicdump"))
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "index.txt"), "a", buffering=1)
T0 = time.monotonic()

def u8(a):  return int.from_bytes(gdb.selected_inferior().read_memory(a, 1).tobytes(), "little")
def u32(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 4).tobytes(), "little")
def u64(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 8).tobytes(), "little")
def rd(a, n):
    try:
        return gdb.selected_inferior().read_memory(a, n).tobytes()
    except Exception:
        return b""

pid = gdb.selected_inferior().pid
BASE = None
for line in open(f"/proc/{pid}/maps"):
    if "libroblox.so" in line and line.split()[2] == "00000000":
        BASE = int(line.split("-")[0], 16)
        break
LOG.write(f"# BASE={hex(BASE)} pid={pid}\n")
LOG.flush()
print(f"BASE={hex(BASE)}")

# known offsets (tools/ngtcp2_off3.txt)
TP_OFF = [
    ("initial_max_stream_data_bidi_local", 192), ("initial_max_stream_data_bidi_remote", 200),
    ("initial_max_stream_data_uni", 208), ("initial_max_data", 216),
    ("initial_max_streams_bidi", 224), ("initial_max_streams_uni", 232),
    ("max_idle_timeout", 240), ("max_udp_payload_size", 248),
    ("active_connection_id_limit", 256), ("ack_delay_exponent", 264),
    ("max_ack_delay", 272), ("max_datagram_frame_size", 280),
    ("version_info", 312), ("version_info_present", 336),
]
TP_BYTES = [("stateless_reset_token_present", 288), ("disable_active_migration", 289),
            ("original_dcid_present", 290), ("initial_scid_present", 291),
            ("retry_scid_present", 292), ("preferred_addr_present", 293),
            ("grease_quic_bit", 310)]
ST_OFF = [
    ("qlog_write", 0), ("initial_ts", 16), ("initial_rtt", 24), ("log_printf", 32),
    ("max_tx_udp_payload_size", 40), ("tokenlen", 56), ("rand_ctx", 72),
    ("max_window", 80), ("max_stream_window", 88), ("ack_thresh", 96),
    ("handshake_timeout", 112), ("glitch_ratelim_burst", 184), ("glitch_ratelim_rate", 192),
    # native writes durations at +200/+208 (diverges from our header >=200)
    ("nat_cfg2_ms", 200), ("nat_cfg3_ms", 208),
]
ST_U32 = [("cc_algo", 8), ("token_type", 64), ("original_version", 152), ("initial_pkt_num", 160)]
ST_BOOL = [("no_tx_udp_payload_size_shaping", 104), ("no_pmtud", 156)]

N = {"conn": 0, "ext": 0}

class ConnBp(gdb.Breakpoint):
    """sub_69BAE01 = ngtcp2_conn_client_new_versioned.
    At entry: [rsp+8]=callbacks, [+16]=settings_version, [+24]=settings,
              [+32]=tp_version, [+40]=params, [+48]=mem, [+56]=user_data.
    r8=client_chosen_version, r9=callbacks_version."""
    def stop(self):
        try:
            rsp = int(gdb.parse_and_eval("$rsp"))
            r8 = int(gdb.parse_and_eval("$r8")) & 0xFFFFFFFF
            r9 = int(gdb.parse_and_eval("$r9")) & 0xFFFFFFFF
            sv = u64(rsp + 16)
            settings = u64(rsp + 24)
            tv = u64(rsp + 32)
            params = u64(rsp + 40)
            mem = u64(rsp + 48)
            n = N["conn"]; N["conn"] += 1
            LOG.write(f"{time.monotonic() - T0:9.3f} CONN#{n} vers={r8} cbsver={r9} "
                      f"settings_ver={sv} tp_ver={tv} settings={hex(settings)} params={hex(params)} mem={hex(mem)}\n")
            pbin = rd(params, 344)
            sbin = rd(settings, 256)
            if len(pbin) == 344:
                open(os.path.join(RUN, f"params{n}.bin"), "wb").write(pbin)
                for name, off in TP_OFF:
                    LOG.write(f"    TP {name:36s} {struct.unpack_from('<Q', pbin, off)[0]}\n")
                for name, off in TP_BYTES:
                    LOG.write(f"    TP {name:36s} {pbin[off]}\n")
            if len(sbin) == 256:
                open(os.path.join(RUN, f"settings{n}.bin"), "wb").write(sbin)
                for name, off in ST_OFF:
                    LOG.write(f"    ST {name:36s} {struct.unpack_from('<Q', sbin, off)[0]}\n")
                for name, off in ST_U32:
                    LOG.write(f"    ST {name:36s} {struct.unpack_from('<I', sbin, off)[0]}\n")
                for name, off in ST_BOOL:
                    LOG.write(f"    ST {name:36s} {sbin[off]}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"CERR {e}\n"); LOG.flush()
        return False


class ExtFin(gdb.FinishBreakpoint):
    """After add_cb returns: dump *out[0..*outlen]."""
    def __init__(self, outp, outlenp, ext_type):
        super().__init__(internal=True)
        self.outp = outp
        self.outlenp = outlenp
        self.ext_type = ext_type
    def stop(self):
        try:
            n = u64(self.outlenp)
            ptr = u64(self.outp)
            if 0 < n < 4096 and 0x1000 < ptr < 0x800000000000:
                data = rd(ptr, int(n))
                fn = os.path.join(RUN, f"ext_{self.ext_type}_{N['ext']}.bin")
                open(fn, "wb").write(data)
                LOG.write(f"    EXT payload type=0x{self.ext_type:04x} len={len(data)} {data[:64].hex()}\n")
                LOG.flush()
        except Exception as e:
            LOG.write(f"XERR {e}\n"); LOG.flush()
        return False


class ExtBp(gdb.Breakpoint):
    """sub_638104F = TLS custom-ext add_cb. sig guess: (ssl, ext_type, ctx, out, outlen, arg)."""
    def stop(self):
        try:
            rsi = int(gdb.parse_and_eval("$rsi")) & 0xFFFFFFFF
            rdx = int(gdb.parse_and_eval("$rdx")) & 0xFFFFFFFF
            rcx = int(gdb.parse_and_eval("$rcx"))
            r8 = int(gdb.parse_and_eval("$r8"))
            n = N["ext"]; N["ext"] += 1
            LOG.write(f"{time.monotonic() - T0:9.3f} EXT#{n} type=0x{rsi:04x} ctx={rdx} "
                      f"out={hex(rcx)} outlen={hex(r8)}\n")
            LOG.flush()
            if 0x1000 < rcx < 0x800000000000 and 0x1000 < r8 < 0x800000000000 and n < 40:
                ExtFin(rcx, r8, rsi)
        except Exception as e:
            LOG.write(f"EERR {e}\n"); LOG.flush()
        return False


# ---- raw UDP datagram capture (QUIC Initial decryptable offline) ----
UDPDIR = os.path.join(RUN, "udp")
os.makedirs(UDPDIR, exist_ok=True)
UDPN = {"tx": 0, "rx": 0, "quic_tx": 0, "quic_rx": 0, "other_tx": 0, "other_rx": 0,
        "rupp_tx": 0, "rupp_rx": 0}

def is_quic_v1(b):
    return len(b) >= 20 and (b[0] & 0x80) != 0 and b[1:5] == b"\x00\x00\x00\x01"

def rupp_inner(b):
    """If b is a RUPP-wrapped datagram ([01][00][u16be hdr_len][TLVs][QUIC]),
    return (full_buffer, inner_quic). header_len semantics are ambiguous
    (TLVs-only vs incl. 4-byte prefix) so try both and require QUIC magic."""
    if len(b) >= 26 and b[0] == 0x01 and b[1] == 0x00:
        hlen = (b[2] << 8) | b[3]
        for start in (4 + hlen, hlen):
            if (start < len(b) - 20 and (b[start] & 0x80) != 0
                    and b[start+1:start+5] == b"\x00\x00\x00\x01"):
                return b[start:], hlen
    return None, None

def fmt_addr(ptr):
    try:
        if ptr == 0:
            return "-"
        fam = u32(ptr) & 0xFFFF
        if fam == 2:
            port = (u8(ptr + 2) << 8) | u8(ptr + 3)
            ip = ".".join(str(u8(ptr + 4 + i)) for i in range(4))
            return f"{ip}:{port}"
        return f"fam{fam}"
    except Exception:
        return "?"

def read_iov(iovptr, iovlen, cap=65535):
    out = b""
    for i in range(int(iovlen)):
        try:
            base = u64(iovptr + 16 * i)
            ln = u64(iovptr + 16 * i + 8)
            if base == 0 or ln == 0:
                continue
            out += rd(base, min(ln, cap - len(out)))
        except Exception:
            break
        if len(out) >= cap:
            break
    return out[:cap]

def dump_udp(tag, buf, addr, hook=""):
    nkey = "tx" if tag == "TX" else "rx"
    qkey = "quic_" + nkey
    rupp_hlen = None
    if buf and buf[0] == 0x01 and buf[1] == 0x00:
        inner, hlen = rupp_inner(buf)
        if inner is not None:
            UDPN["rupp_" + nkey] += 1
            rn = UDPN["rupp_" + nkey]
            rupp_hlen = hlen
            if rn <= 20:
                rfn = os.path.join(UDPDIR, f"rupp_{nkey}{rn:03d}.bin")
                open(rfn, "wb").write(buf)
                LOG.write(f"{time.monotonic() - T0:9.3f} {tag}RUPP n={rn} size={len(buf)} "
                          f"hdr_len={hlen} inner={len(inner)} hook={hook} "
                          f"-> {os.path.basename(rfn)}\n")
                LOG.flush()
            buf = inner  # continue as the inner QUIC datagram
    if is_quic_v1(buf):
        UDPN[qkey] += 1
        n = UDPN[qkey]
        if n <= 20:
            fn = os.path.join(UDPDIR, f"{nkey}{n:03d}.bin")
            open(fn, "wb").write(buf)
            dcid_len = buf[5] if len(buf) > 5 else 0
            LOG.write(f"{time.monotonic() - T0:9.3f} {tag}QUIC n={n} size={len(buf)} "
                      f"b0=0x{buf[0]:02x} dcid_len={dcid_len} dst={addr} hook={hook}"
                      f"{' rupp' if rupp_hlen is not None else ''} -> {os.path.basename(fn)}\n")
            LOG.flush()
    else:
        UDPN["other_" + nkey] += 1
        if UDPN["other_" + nkey] <= 10 and buf:
            ofn = os.path.join(UDPDIR, f"other_{nkey}{UDPN['other_' + nkey]:03d}.bin")
            open(ofn, "wb").write(buf[:1500])
            LOG.write(f"{time.monotonic() - T0:9.3f} {tag}OTHER n={UDPN['other_' + nkey]} "
                      f"size={len(buf)} head={buf[:8].hex()} hook={hook} "
                      f"-> {os.path.basename(ofn)}\n")
            LOG.flush()

class SendToBp(gdb.Breakpoint):
    def stop(self):
        try:
            buf = rd(int(gdb.parse_and_eval("$rsi")), min(int(gdb.parse_and_eval("$rdx")), 65535))
            dump_udp("TX", buf, fmt_addr(int(gdb.parse_and_eval("$r8"))), "sendto")
        except Exception as e:
            LOG.write(f"STERR {e}\n"); LOG.flush()
        return False

class SendMsgBp(gdb.Breakpoint):
    def stop(self):
        try:
            mh = int(gdb.parse_and_eval("$rsi"))
            iov = u64(mh + 16)
            iovlen = u64(mh + 24)
            buf = read_iov(iov, iovlen)
            dump_udp("TX", buf, fmt_addr(u64(mh)), "sendmsg")
        except Exception as e:
            LOG.write(f"SMERR {e}\n"); LOG.flush()
        return False

class SendMmsgBp(gdb.Breakpoint):
    def stop(self):
        try:
            mv = int(gdb.parse_and_eval("$rsi"))
            vlen = min(int(gdb.parse_and_eval("$rdx")), 8)
            for i in range(vlen):
                mh = mv + 64 * i
                iov = u64(mh + 16)
                iovlen = u64(mh + 24)
                dump_udp("TX", read_iov(iov, iovlen), fmt_addr(u64(mh)), "sendmmsg")
        except Exception as e:
            LOG.write(f"SMMERR {e}\n"); LOG.flush()
        return False

def _quicish(head):
    """Cheap pre-filter for hot paths: RUPP(01 00), QUIC long(0x80) or short(0x40)."""
    return bool(head) and (head[0] == 0x01 or (head[0] & 0xC0) != 0)

class WriteBp(gdb.Breakpoint):
    """write(fd, buf, n) — connected-UDP send path. QUIC/RUPP-magic filtered."""
    def stop(self):
        try:
            p = int(gdb.parse_and_eval("$rsi"))
            n = min(int(gdb.parse_and_eval("$rdx")), 65535)
            if not _quicish(rd(p, 8)):
                UDPN["skipped_write"] = UDPN.get("skipped_write") + 1
                return False
            dump_udp("TX", rd(p, n), "-", "write")
        except Exception as e:
            LOG.write(f"WERR {e}\n"); LOG.flush()
        return False

class WritevBp(gdb.Breakpoint):
    """writev(fd, iov, cnt)."""
    def stop(self):
        try:
            iov = int(gdb.parse_and_eval("$rsi"))
            cnt = min(int(gdb.parse_and_eval("$rdx")), 8)
            if cnt > 0 and not _quicish(rd(u64(iov), 8)):
                UDPN["skipped_writev"] = UDPN.get("skipped_writev") + 1
                return False
            dump_udp("TX", read_iov(iov, cnt), "-", "writev")
        except Exception as e:
            LOG.write(f"WVERR {e}\n"); LOG.flush()
        return False

class RecvMsgBp(gdb.Breakpoint):
    """disabled: dynamic FinishBreakpoint creation crashes this gdb build
    (find_program_space_for_breakpoint assertion during thread churn)."""
    def stop(self):
        return False

class RecvFromBp(gdb.Breakpoint):
    def stop(self):
        return False

class RecvMmsgBp(gdb.Breakpoint):
    def stop(self):
        return False

try:
    gdb.execute("set breakpoint pending on")
except Exception:
    pass
SendToBp("sendto", internal=False)
SendMsgBp("sendmsg", internal=False)
SendMmsgBp("sendmmsg", internal=False)
WriteBp("write", internal=False)
WritevBp("writev", internal=False)
RecvFromBp("recvfrom", internal=False)
RecvMsgBp("recvmsg", internal=False)
RecvMmsgBp("recvmmsg", internal=False)

ConnBp(f"*{BASE + 0x69BAE01}", internal=False)
ExtBp(f"*{BASE + 0x638104F}", internal=False)
print("quicdump bps set")
end

continue
