# rc_capture.gdb — run the native Roblox client under gdb and capture everything
# the RakNet/session layer does, *including the join phase*, which happens only
# ~3 s into the process (so attaching after the fact always misses it).
#
# Why it works this way:
#   * the client maps libroblox.so with its own loader (the mapping is rwxp and
#     `catch load` never fires), so we poll a hot libc entry point
#     (clock_gettime) until the ELF header of libroblox.so shows up, then arm.
#   * every hook is a plain `Bp(offset)` on BASE+offset (BASE verified by ELF
#     magic), so nothing needs symbols.
#
# Hooks (all log to $RKC_DIR/rkc.log):
#   MB#   an 8-bit write into a *fresh* bitstream  -> message-id writes,
#         deduped by (caller, byte); the caller chain names the builder
#   COPY# a >=64-byte copy into a bitstream        -> fragment/blob copies;
#         src dumps land in copy_src_*.bin (raw pre-fragmentation bodies)
#   GROW# bitstream growth (>=2048 bits)           -> who allocates big streams
#   DGRAM# datagram plaintext (pre-encryption)     -> dgram_*.bin + header bytes
#   KDF#  session key derivation (pub/sec/peer)    -> lets you decode captures
#   MALLOC# libc malloc of 4 KiB..128 KiB          -> who allocates the join
#           buffers (backtrace names the builder)
#   wire_tx_*.bin: outgoing UDP frames (sendto), header 01 00 00 1f 01 11
#   RX#   one line per received UDP frame (recvfrom/recv), len + first bytes
#         -> wire_rx_*.bin. The frame is still sealed; pair it with the KDF#
#         keys and tools/raknet_rx_decode.py for the plaintext app messages.
#
# Env: RKC_DIR (output dir), RKC_COOKIE (.ROBLOSECURITY value),
#      RKC_PLACE (place id, default 1818), RKC_LIB, RKC_ASSETS, RKC_CLIENT.
set pagination off
set confirm off
set breakpoint pending on
handle SIGSEGV nostop noprint pass
python
import gdb, os, re

RUN = os.environ.get("RKC_DIR", "/tmp/rc_capture")
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "rkc.log"), "a", buffering=1)

def note(m):
    LOG.write(m + "\n"); LOG.flush()

def rd(a, n):
    try:
        return gdb.selected_inferior().read_memory(a, n).tobytes()
    except Exception:
        return b""

def ru32(a):
    b = rd(a, 4)
    return int.from_bytes(b, "little") if len(b) == 4 else None

def ru64(a):
    b = rd(a, 8)
    return int.from_bytes(b, "little") if len(b) == 8 else 0

def rg(n):
    try:
        return int(gdb.parse_and_eval("$" + n)) & 0xFFFFFFFFFFFFFFFF
    except Exception:
        return 0

def ok(a):
    return 0x1000 < a < 0x800000000000

def save(tag, n, blob):
    try:
        open(os.path.join(RUN, "%s_%d.bin" % (tag, n)), "wb").write(blob)
    except Exception as e:
        note("save err %r" % e)

HITS = {}
def bump(k, mx=10**9):
    n = HITS.get(k, 0)
    if n >= mx:
        return None
    HITS[k] = n + 1
    return n

BASE = None
ARMED = False

def find_base():
    global BASE
    pid = gdb.selected_inferior().pid
    cands = []
    try:
        lines = open("/proc/%d/maps" % pid).read().splitlines()
    except Exception:
        return None
    for line in lines:
        if "libroblox.so" not in line or "audio" in line:
            continue
        parts = line.split()
        lo = int(parts[0].split("-")[0], 16)
        base = lo - int(parts[2], 16)
        if base not in cands:
            cands.append(base)
    for base in cands:
        if rd(base, 4) == b"\x7fELF":
            BASE = base
            return base
    return None

def bt(depth=26):
    try:
        lines = gdb.execute("bt %d" % depth, to_string=True).split("\n")
    except Exception:
        return []
    out = []
    for l in lines:
        m = re.search(r"#(\d+)\s+(?:0x)?([0-9a-f]{6,})", l)
        if m:
            a = int(m.group(2), 16)
            if BASE and a >= BASE:
                out.append("+%X" % (a - BASE))
            else:
                out.append("0x%x" % a)
    return out

class Bp(gdb.Breakpoint):
    def __init__(self, tag, off, fn):
        super(Bp, self).__init__("*0x%x" % (BASE + off))
        self.tag, self.fn = tag, fn
        self.silent = True
    def stop(self):
        try:
            return self.fn()
        except Exception as e:
            note("bp[%s] err %r" % (self.tag, e))
            return False

# ---------------------------------------------------------------- hooks ----
# BitStream layout from sub_4486E48/sub_4486D2E/sub_4486DA0:
#   +0xC current bit length, +0x18 data pointer, inline 33-byte header before it.
# (JOINING flips on the first hook hit: from then on the join payload buffers
#  are being allocated, so that is when the malloc watchpoints get armed.)
INTERESTING = {0x09, 0x13, 0x1c, 0x83, 0x8a, 0x8f, 0x90, 0x92, 0x9b, 0xa6}
SEEN = {}

def h_bits():
    """sub_4486E48(bs, src, bits, align) — message-id writes."""
    JOINING[0] = True
    bs, src, bits = rg("rdi"), rg("rsi"), rg("rdx")
    if bits == 0 or bits > 8 or not ok(bs):
        return False
    if ru32(bs + 0xC) != 0:          # only the first byte of a fresh stream
        return False
    val = rd(src, 1)
    if not val:
        return False
    raw = bt(4)
    direct = raw[1] if len(raw) > 1 else "?"
    key = (direct, val[0])
    if key in SEEN:
        SEEN[key] += 1
        return False
    SEEN[key] = 1
    m = bump("mb", 1500)
    if m is None:
        return False
    note("MB#%d val=%02x bs=%#x buf=%#x direct=%s" % (m, val[0], bs, ru64(bs + 0x18), direct))
    note("   C " + " ".join(bt(26)))
    return False

def h_copy():
    """sub_4486D2E(bs, src, bytes) — fragment copies and blob copies."""
    JOINING[0] = True
    bs, src, nbytes = rg("rdi"), rg("rsi"), rg("rdx")
    if nbytes < 64 or not ok(bs):
        return False
    m = bump("copy", 400)
    if m is None:
        return False
    note("COPY#%d bytes=%d bs=%#x src=%#x pre=%s" % (m, nbytes, bs, src, ru32(bs + 0xC)))
    note("   C " + " ".join(bt(26)))
    body = rd(src, min(nbytes, 65536)) if ok(src) and nbytes >= 256 else b""
    if body:
        save("copy_src", m, body)
    # A message body starts with its id byte.  Arm a write watchpoint on the
    # start of any join/replication body: the implementation reuses these
    # buffers, so the next write there names the serializer that fills them.
    if body and body[0] in (0x90, 0x8a, 0x83, 0x92) and src not in FIRSTBODY[0] and JOINING[0]:
        FIRSTBODY[0][src] = body[0]
        note("WP armed on id=%02x body buffer %#x" % (body[0], src))
        try:
            WPS.append(WpBp(src))
            if len(WPS) > 6:
                old = WPS.pop(0)
                try:
                    old.enabled = False
                except Exception:
                    pass
        except Exception as e:
            note("body wp err %r" % e)
    return False

def h_grow():
    """sub_4486DA0(bs, bits) — bitstream growth."""
    bs, bits = rg("rdi"), rg("rsi")
    if bits < 2048 or not ok(bs):
        return False
    m = bump("grow", 300)
    if m is None:
        return False
    note("GROW#%d bs=%#x bits=%d now=%s buf=%#x" % (m, bs, bits, ru32(bs + 0xC), ru64(bs + 0x18)))
    note("   C " + " ".join(bt(28)))
    return False

def h_dgram():
    """sub_449C354(crypto, buf, len) — datagram seal; plaintext = buf[0:len-18]."""
    JOINING[0] = True
    obj, buf, ln = rg("rdi"), rg("rsi"), rg("rdx")
    if not ok(buf) or ln < 0x12 or ln > 4096:
        return False
    m = bump("dgram", 600)
    if m is None:
        return False
    plain = rd(buf, ln - 18)
    note("DGRAM#%d len=%d hdr=%s" % (m, ln, plain[:32].hex()))
    save("dgram", m, plain)
    return False

def h_kdf():
    """0x44AA839(pub, sec, peer) — BLAKE2b session KDF (one call per DH)."""
    n = bump("kdf", 8)
    if n is None:
        return False
    pub, sec, peer = rg("rdx"), rg("rcx"), rg("r8")
    note("KDF#%d pub=%s sec=%s peer=%s" % (n, rd(pub, 32).hex(), rd(sec, 32).hex(), rd(peer, 32).hex()))
    return False

def h_seal():
    """sub_648C61A(client, stream, key16) — the join-message AES envelope.

    Dumps the 16-byte envelope key, the PCG state at client+0x160 (feeds the
    padding bytes) and the plaintext stream *before* sealing.  The sealed body
    is already in the wire_tx_*.bin captures, so these three together are a
    complete reference vector for reimplementation.
    """
    cl, st, key = rg("rdi"), rg("rsi"), rg("rdx")
    n = bump("seal", 12)
    if n is None:
        return False
    data, ln = ru64(st + 0x10), ru32(st + 0x30)
    note("SEAL#%d key=%s pcg=%#x data=%#x len=%s id=%s" % (
        n, rd(key, 16).hex(), ru64(cl + 0x160), data, ln,
        rd(data, 1).hex() if ok(data) else "?"))
    note("   C " + " ".join(bt(20)))
    if ok(data) and ln and ln < 65536:
        save("seal_plain", n, rd(data, ln))
    return False

def arm():
    global ARMED
    if ARMED or not BASE:
        return False
    for tag, off, fn in (("bits", 0x4486E48, h_bits), ("copy", 0x4486D2E, h_copy),
                         ("grow", 0x4486DA0, h_grow), ("dgram", 0x449C354, h_dgram),
                         ("kdf", 0x44AA839, h_kdf), ("seal", 0x648C61A, h_seal)):
        try:
            Bp(tag, off, fn)
        except Exception as e:
            note("arm %s err %r" % (tag, e))
    if MAL_BP is not None:
        MAL_BP.enabled = True          # join-band allocations only, post-preflight
    for bp in RX_BPS:
        bp.enabled = True
    ARMED = True
    note("== ARMED BASE=%#x ==" % BASE)
    return True

# ------------------------------------------------------- process-wide BPs --
def h_sendto():
    """Outgoing UDP frame (Roblox RUPP header first)."""
    fd, buf, ln = rg("rdi"), rg("rsi"), rg("rdx")
    if not ok(buf) or ln < 40 or ln > 4096:
        return False
    data = rd(buf, min(ln, 64))
    if not (len(data) > 40 and data[:6] == b"\x01\x00\x00\x1f\x01\x11"):
        return False
    n = bump("wire", 400)
    if n is None:
        return False
    save("wire_tx", n, rd(buf, ln))
    return False

# ------------------------------------------------------- receive-side BPs --
# recvfrom/recv give the *sealed* frame. A breakpoint body cannot use
# gdb.FinishBreakpoint under gdb 9.4 (same limitation the malloc hook works
# around), so read the return address from the stack at entry and stop there to
# pick up $rax. One temporary breakpoint per pending call, dropped as soon as
# its return is consumed.
RXPEND = {}          # return address -> [(buf, len)]
RXRET = {}           # return address -> RxRetBp
RXN = [0]
# Created disabled: recv/recvfrom fire on every socket in the process, and the
# launcher's auth preflight is sensitive to being stopped (same reason the
# malloc hook starts disabled). arm() turns them on once libroblox is mapped.
RX_BPS = []


def h_recv(buf, ln):
    if not ok(buf) or ln < 40 or ln > 4096:
        return False
    ra = ru64(rg("rsp"))
    if not ok(ra):
        return False
    RXPEND.setdefault(ra, []).append((buf, ln))
    if ra not in RXRET:
        try:
            RXRET[ra] = RxRetBp(ra)
        except Exception as e:
            note("rx retbp arm err %r" % e)
    return False


class RxRetBp(gdb.Breakpoint):
    def __init__(self, retaddr):
        super(RxRetBp, self).__init__("*0x%x" % retaddr, internal=False)
        self.retaddr = retaddr
        self.silent = True

    def stop(self):
        try:
            pend = RXPEND.get(self.retaddr) or []
            buf, ln = pend.pop(0) if pend else (0, 0)
            if not pend:
                self.enabled = False
                RXRET.pop(self.retaddr, None)
            n = rg("rax")
            # n is the byte count from recv/recvfrom; negative means an error.
            if n and n <= ln and n >= 40:
                data = rd(buf, n)
                if len(data) == n:
                    m = bump("rx", 1200)
                    if m is not None:
                        save("wire_rx", m, data)
                        note("RX#%d len=%d hdr=%s" % (m, n, data[:32].hex()))
                        RXN[0] = m + 1
        except Exception as e:
            note("rxret err %r" % e)
        return False


def h_recvfrom():
    return h_recv(rg("rsi"), rg("rdx"))


def h_recv_plain():
    return h_recv(rg("rsi"), rg("rdx"))


WATCHED = [0]

class WpBp(gdb.Breakpoint):
    """First write into a join-sized buffer: names the serializer."""
    def __init__(self, ptr):
        super(WpBp, self).__init__("*0x%x" % ptr, gdb.BP_WATCHPOINT, gdb.WP_WRITE)
        self.addr = ptr
        self.silent = True
    def stop(self):
        try:
            m = bump("wp", 24)
            note("WP#%d first write %#x" % (m, self.addr))
            note("   C " + " ".join(bt(24)))
            self.enabled = False
        except Exception as e:
            note("wp err %r" % e)
        return False

# Capture malloc's return value by hand: gdb.FinishBreakpoint inside a
# breakpoint handler makes gdb 9.4 fall over, so we break at the return address
# read from the stack at malloc entry.
JOINING = [False]                     # set once the RakNet session is live
PENDING = {}                          # return address -> [sizes]
RETBP = {}                            # return address -> RetBp
WPS = []                              # armed write watchpoints (keep <= 3)
FIRSTBODY = [{}]                      # body buffer -> id (watchpoints armed)

class RetBp(gdb.Breakpoint):
    def __init__(self, retaddr):
        super(RetBp, self).__init__("*0x%x" % retaddr, internal=False)
        self.retaddr = retaddr
        self.silent = True
    def stop(self):
        try:
            pend = PENDING.get(self.retaddr) or []
            size = pend.pop(0) if pend else None
            if not pend:
                self.enabled = False
                RETBP.pop(self.retaddr, None)
            ptr = rg("rax")
            if (size is not None and JOINING[0] and MALLOC_LO <= size <= MALLOC_HI
                    and ok(ptr)):
                note("MALLOCRET size=%d ptr=%#x (watchpoint armed)" % (size, ptr))
                try:
                    wp = WpBp(ptr)
                    WPS.append(wp)
                    if len(WPS) > 3:
                        old = WPS.pop(0)
                        try:
                            old.enabled = False
                        except Exception:
                            pass
                except Exception as e:
                    note("wp arm err %r" % e)
        except Exception as e:
            note("retbp err %r" % e)
        return False

def malloc_finish(size):
    ra = ru64(rg("rsp"))
    if not ok(ra):
        return
    PENDING.setdefault(ra, []).append(size)
    if ra not in RETBP:
        try:
            RETBP[ra] = RetBp(ra)
        except Exception as e:
            note("retbp arm err %r" % e)

# NOTE: keep this hook narrow. Firing on every 4-128 KiB allocation (with a
# backtrace) slows startup enough that the launcher's auth preflight times out
# ("authentication service unavailable") and the client never reaches the join.
MALLOC_LO, MALLOC_HI = 8000, 24576

def h_malloc():
    """Join-sized allocations: names the code that allocates the payload buffer."""
    size = rg("rdi")
    if size < MALLOC_LO or size > MALLOC_HI:
        return False
    m = bump("malloc", 200)
    if m is None:
        return False
    if bump("mallog", 40) is not None:
        note("MALLOC#%d size=%d" % (m, size))
    if size <= MALLOC_HI:
        malloc_finish(size)
    return False

class SymBp(gdb.Breakpoint):
    def __init__(self, tag, spec, fn):
        super(SymBp, self).__init__(spec, internal=False)
        self.tag, self.fn = tag, fn
        self.silent = True
    def stop(self):
        try:
            return self.fn()
        except Exception:
            return False

MAL_BP = None
try:
    SymBp("sendto", "sendto", h_sendto)
except Exception as e:
    note("sendto bp err %r" % e)
for _spec, _fn in (("recvfrom", h_recvfrom), ("recv", h_recv_plain)):
    try:
        _bp = SymBp(_spec, _spec, _fn)
        _bp.enabled = False
        RX_BPS.append(_bp)
    except Exception as e:
        note("%s bp err %r" % (_spec, e))
if os.environ.get("RC_MALLOC"):
    try:
        # created DISABLED: the launcher's auth preflight runs before libroblox is
        # mapped and is sensitive to being stopped (any hook overhead there makes
        # it fail with "authentication service unavailable").  arm() enables it.
        MAL_BP = SymBp("malloc", "malloc", h_malloc)
        MAL_BP.enabled = False
    except Exception as e:
        note("malloc bp err %r" % e)
else:
    note("malloc hook off (set RC_MALLOC=1 to enable)")

PROBE_HITS = [0]

class ProbeBp(gdb.Breakpoint):
    """Poll for libroblox.so (the client's own loader bypasses `catch load`)."""
    def __init__(self):
        super(ProbeBp, self).__init__("clock_gettime", internal=False)
        self.silent = True
    def stop(self):
        try:
            PROBE_HITS[0] += 1
            if ARMED or PROBE_HITS[0] % 32:
                return False
            if find_base():
                arm()
                self.enabled = False
        except Exception as e:
            note("probe err %r" % e)
        return False

try:
    ProbeBp()
except Exception as e:
    note("probe arm err %r" % e)

# ------------------------------------------------------------ launch ------
note("=== rc_capture launch (place=%s) ===" % os.environ.get("RKC_PLACE", "1818"))
args = ["--lib", os.environ.get("RKC_LIB", "/home/john/rr/libroblox.so"),
        "--assets", os.environ.get("RKC_ASSETS", "/home/john/rr/assets"),
        "--launch-uri", "roblox://placeId=%s" % os.environ.get("RKC_PLACE", "1818"),
        "--roblosecurity", os.environ.get("RKC_COOKIE", ""),
        "--graphics", "opengl"]
gdb.execute("set args " + " ".join("'%s'" % a for a in args))
try:
    gdb.execute("run")
except Exception as e:
    note("run err %r" % e)
note("inferior exited: base=%s probe=%d" % (hex(find_base() or 0), PROBE_HITS[0]))
note("== script end hits=%r ==" % HITS)
end
