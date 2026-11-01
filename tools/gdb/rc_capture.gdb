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
#
# RC_HOOKS selects which inferior hooks arm (comma list: bits,copy,grow,dgram,
# kdf,seal,wire,rx).  Unset = all.  A driven-input capture only needs
# "kdf,seal,wire": the bitstream/copy/dgram hooks fire hundreds of times per
# second and slow the client enough that it misses the 0x9B window.
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

def _libroblox_segments():
    """(base, offset, size, perms) for every libroblox mapping."""
    pid = gdb.selected_inferior().pid
    out = []
    try:
        lines = open("/proc/%d/maps" % pid).read().splitlines()
    except Exception:
        return out
    for line in lines:
        if "libroblox.so" not in line or "audio" in line:
            continue
        parts = line.split()
        try:
            rng = parts[0].split("-")
            lo, hi = int(rng[0], 16), int(rng[1], 16)
            off = int(parts[2], 16)
        except Exception:
            continue
        out.append((lo - off, off, hi - lo, parts[1]))
    return out

def stable_base():
    """The *final* load base.

    The launcher first maps the whole file at base-0x100000 (one segment, offset
    0, full file size) and only later replaces it with the real per-segment
    layout.  Hooks armed against the first mapping are silently wiped when the
    loader remaps at the same addresses, which is why /tmp/nat_now had zero
    KDF#/SEAL#/DGRAM# hits.  A layout is final when the offset-0 segment is
    smaller than the file and a nonzero-offset sibling exists.
    """
    segs = _libroblox_segments()
    groups = {}
    for base, off, size, perms in segs:
        groups.setdefault(base, []).append((off, size, perms))
    for base, g in groups.items():
        if rd(base, 4) != b"\x7fELF":
            continue
        first = [x for x in g if x[0] == 0]
        others = [x for x in g if x[0] != 0]
        if first and others and first[0][1] < 0x4000000:
            return base
    return None

def base_from_addr(ra):
    """Load base of the libroblox mapping that contains an executing address.

    The launcher can map the file more than once (a temporary whole-file copy
    at base+0x100000 and the real per-segment mapping at base), so the maps
    alone cannot say which copy is running.  A return address captured inside
    the client is unambiguous: the mapping that contains it is the executing
    one.  For the executable LOAD segment vaddr == file offset, so
    base = mapping.lo - mapping.file_offset.
    """
    for base, off, size, perms in _libroblox_segments():
        lo = base + off
        if lo <= ra < lo + size:
            return base
    return None

def find_base(final_only=True):
    global BASE
    if final_only:
        base = stable_base()
        if base:
            BASE = base
            return base
        return None
    cands = []
    for base, off, size, perms in _libroblox_segments():
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

TAG_HITS = {}

def h_bits():
    """sub_4486E48(bs, src, bits, align) — message-id and item-tag writes."""
    JOINING[0] = True
    bs, src, bits = rg("rdi"), rg("rsi"), rg("rdx")
    if bits == 0 or bits > 8 or not ok(bs):
        return False
    val = rd(src, 1)
    if not val:
        return False
    # Chain item tag: the write lands immediately after a 0x83 header byte in
    # the same stream.  Messages are batched into one datagram-level stream, so
    # the header is not at bit 0 — test the preceding byte instead of the
    # absolute bit offset.  The backtrace names the serializer.
    pre = ru32(bs + 0xC)
    if val[0] in (0x1c, 0x0e) and bits <= 64:
        buf = ru64(bs + 0x18)
        prev83 = (pre % 8 == 0 and pre >= 8 and ok(buf)
                  and rd(buf + pre // 8 - 1, 1) == b"\x83")
        key = val[0] if prev83 else ("any", val[0])
        cap = 6 if prev83 else 2
        n = TAG_HITS.get(key, 0)
        if n < cap:
            TAG_HITS[key] = n + 1
            note("TAG%s#%d val=%02x bits=%d pre=%d buf=%#x" %
                 ("" if prev83 else "ANY", n, val[0], bits, pre, buf))
            note("   C " + " ".join(bt(24)))
    if pre != 0:                     # only the first byte of a fresh stream
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

# Chain-item anchors: `83 1c` / `83 0e` are client->server replication items
# (see FINDINGS.md).  The seal hook sees the plaintext, so a scan here tags the
# exact datagram and backtraces whoever produced it.  Capped hard: those byte
# pairs also occur inside payload data, and a hit is a lead, not a proof.
CHAIN_PATS = ((b"\x83\x1c", "1c"), (b"\x83\x0e", "0e"))
CHAIN_N = {}
CHAIN_CAP = 6
CHAIN_WP = {}

class ChainWpBp(gdb.Breakpoint):
    """Watch byte 1 of a chain message buffer: the next write is the tag writer.

    Armed at seal time on a buffer whose plaintext began 83 1c / 83 0e.  The
    buffer is reused for the next message, so the first write to +1 after the
    seal is whichever code writes the next message's first item tag; logging
    the value and a backtrace names that writer without guessing.
    """
    def __init__(self, addr, tag):
        super(ChainWpBp, self).__init__("*0x%x" % addr, gdb.BP_WATCHPOINT, gdb.WP_WRITE)
        self.addr, self.tag = addr, tag
        self.silent = True
        self.hits = 0
    def stop(self):
        try:
            self.hits += 1
            m = bump("chainwp", 48)
            if m is not None:
                note("CHAINWP#%d tag=%02x val=%s" % (m, self.tag, rd(self.addr, 1).hex()))
                note("   C " + " ".join(bt(26)))
            if self.hits >= 8:
                self.enabled = False
        except Exception as e:
            note("chainwp[%s] err %r" % (self.tag, e))
        return False

def arm_chain_wp(buf, plain):
    # Experimental: hardware watchpoints on a hot buffer can perturb timing, so
    # this stays off unless asked for.  RKC_CHAINWP=1 turns it on.
    if not os.environ.get("RKC_CHAINWP"):
        return
    if len(plain) < 2 or plain[0] != 0x83 or plain[1] not in (0x1c, 0x0e):
        return
    tag = plain[1]
    if tag in CHAIN_WP:
        return
    try:
        CHAIN_WP[tag] = ChainWpBp(buf + 1, tag)
        note("CHAINWP armed tag=%02x at %#x" % (tag, buf + 1))
    except Exception as e:
        note("chainwp arm err %r" % e)

def scan_chain(plain):
    for pat, name in CHAIN_PATS:
        off = plain.find(pat)
        if off < 0:
            continue
        seen = CHAIN_N.get(name, 0)
        if seen >= CHAIN_CAP:
            continue
        CHAIN_N[name] = seen + 1
        note("CHAIN#%s-%d off=%d len=%d win=%s" %
             (name, seen, off, len(plain), plain[off:off + 64].hex()))
        note("   C " + " ".join(bt(30)))

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
    scan_chain(plain)
    arm_chain_wp(buf, plain)
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
    # The loader can leave a stale copy of the ELF at BASE while the executing
    # segments live elsewhere.  Verify the code bytes at every hook offset
    # against the file before arming; a mismatch means this BASE is not the
    # mapping that runs.
    try:
        _f = open(os.environ.get("RKC_LIB", "/home/john/rr/libroblox.so"), "rb")
        _f.seek(0)
        _file = _f.read()
    except Exception:
        _file = None
    bad = []
    if _file is not None:
        for tag, off in (("bits", 0x4486E48), ("copy", 0x4486D2E),
                         ("grow", 0x4486DA0), ("dgram", 0x449C354),
                         ("kdf", 0x44AA839), ("seal", 0x648C61A)):
            mem = rd(BASE + off, 16)
            if mem != _file[off:off + 16]:
                bad.append(tag)
    if bad:
        note("== ARM REFUSED base=%#x bad=%s ==" % (BASE, ",".join(bad)))
        return False
    sel = os.environ.get("RC_HOOKS")
    only = set(x.strip() for x in sel.split(",") if x.strip()) if sel else None
    def want(tag):
        return only is None or tag in only
    for tag, off, fn in (("bits", 0x4486E48, h_bits), ("copy", 0x4486D2E, h_copy),
                         ("grow", 0x4486DA0, h_grow), ("dgram", 0x449C354, h_dgram),
                         ("kdf", 0x44AA839, h_kdf), ("seal", 0x648C61A, h_seal)):
        if not want(tag):
            continue
        try:
            Bp(tag, off, fn)
        except Exception as e:
            note("arm %s err %r" % (tag, e))
    if MAL_BP is not None:
        MAL_BP.enabled = True          # join-band allocations only, post-preflight
    if want("rx"):
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
    # First wire frame is proof that libroblox is mapped and executing.  The
    # return address of this sendto call identifies *which* mapping runs (the
    # launcher sometimes keeps a stale whole-file copy at another base), so arm
    # against that mapping.  Hooks armed earlier can be wiped by the loader's
    # remap; this is the last moment at which arming is both safe and useful.
    if not ARMED:
        global BASE
        ra = ru64(rg("rsp"))
        base = base_from_addr(ra) if ok(ra) else None
        if base is None:
            base = find_base(final_only=False)
        if base is not None:
            BASE = base
            arm()
        else:
            note("SEND before libroblox mapping known: ra=%#x BASE=%s" % (ra, BASE))
    n = bump("wire", 400)
    if n is None:
        return False
    save("wire_tx", n, rd(buf, ln))
    # sendto(fd, buf, len, flags, dest, addrlen): the destination sockaddr is
    # r8. Recording it is what tells a rehomed direct-to-RCC path apart from
    # the Udmux handshake endpoint: if the native moves its subtype-2 datagrams
    # to a different peer, its payloads alone can never show it.
    dst = rg("r8")
    if ok(dst):
        sa = rd(dst, 16)
        if len(sa) == 16 and sa[0] == 2:  # AF_INET
            port = (sa[2] << 8) | sa[3]
            note("SEND#%d dst=%d.%d.%d.%d:%d" %
                 (n, sa[4], sa[5], sa[6], sa[7], port))
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
# sendto is the only source of wire_tx_*.bin, so it arms unless the caller
# explicitly selected hooks that exclude it.
_HOOKSEL = os.environ.get("RC_HOOKS")
_WANT_WIRE = (not _HOOKSEL) or ("wire" in [x.strip() for x in _HOOKSEL.split(",")])
try:
    if _WANT_WIRE:
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
    """Poll for libroblox.so (the client's own loader bypasses `catch load`).

    Arming happens from the sendto hook instead: the return address of the
    first outgoing RakNet frame names the mapping that actually executes, which
    the /proc maps alone cannot do when the launcher keeps a second copy.
    """
    def __init__(self):
        super(ProbeBp, self).__init__("clock_gettime", internal=False)
        self.silent = True
    def stop(self):
        try:
            PROBE_HITS[0] += 1
            if PROBE_HITS[0] % 32:
                return False
            segs = _libroblox_segments()
            if segs:
                note("PROBE libroblox mapped: " +
                     " ".join("%#x/%#x" % (b, o) for b, o, s, p in segs[:6]))
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
