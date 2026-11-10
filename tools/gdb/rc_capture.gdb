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
import gdb, os, re, shlex, time

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

    This client's loader (mcpelauncher Bionic linker) maps each PT_LOAD as its own
    independent region, so `lo - off` is NOT a constant across segments — measured
    2026-11-05, three segments produced three different candidate bases 0x4000 apart
    and every hook was armed against nothing. The load base is the address of the
    mapping whose file offset is 0; the code hooks live inside that same first
    segment (all offsets < 0x6d88000), so BASE+off is valid for them.
    """
    best = None
    for base, off, size, perms in _libroblox_segments():
        if off != 0:
            continue
        if rd(base, 4) != b"\x7fELF":
            continue
        if best is None or size > best[1]:
            best = (base, size)
    if best:
        return best[0]
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
    """Load base of libroblox: the mapping whose file offset is 0.

    The launcher is not a plain mmap loader: it maps one huge rwx region for
    the file's first 0x6d88000 bytes and then additional per-segment regions,
    placing them so that lo == base + p_vaddr while p_vaddr != p_offset.  So
    `lo - offset` yields a *different* base per segment (observed: three bases
    0x4000 apart), which is why the old "final layout" heuristic never found a
    base and arm() silently returned early on every poll.  `lo` of the
    offset-0 mapping is the base; arm() then verifies the code bytes at every
    hook offset against the file, so a wrong candidate cannot arm.
    """
    global BASE
    segs = _libroblox_segments()
    cands = [b for b, off, size, perms in segs if off == 0]
    cands += [b for b, off, size, perms in segs if b not in cands]
    if final_only:
        # Prefer the region that actually holds the hooks: the file is mapped
        # contiguously from offset 0, so its start must expose the ELF header.
        cands = cands[:1]
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

def sbt(depth=32, span=0x8000):
    """Stack-scanning backtrace, innermost first.

    libroblox.so has no frame pointers and no usable unwind info, so gdb's own
    `bt` reports the first frame and then junk (`+7CEE5C01AA454EF8 ...`).
    Every real frame still has its return address on the stack, so scanning for
    8-byte-aligned values that land inside the module recovers the chain. It is
    a heuristic: expect padding/garbage entries, read the lowest offsets as the
    innermost frames, and verify a hit by disassembling the call site in IDA.
    Unlike bt(), this one is worth calling from a hook where the builder is
    still live.
    """
    global BASE
    if not BASE:
        return []
    try:
        sp = int(gdb.parse_and_eval("$rsp"))
        mem = gdb.selected_inferior().read_memory(sp, span).tobytes()
    except Exception:
        return []
    out, seen = [], set()
    for i in range(0, len(mem) - 8, 8):
        v = int.from_bytes(mem[i:i + 8], "little")
        if v < BASE or v >= BASE + 0x6D88000:
            continue
        off = v - BASE
        if off in seen:
            continue
        seen.add(off)
        out.append("+%X" % off)
        if len(out) >= depth:
            break
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

SPECA_BP = {}

class SpecAllocRetBp(gdb.Breakpoint):
    """Return from sub_44967AC: rax is the message spec just allocated.

    Logging caller+spec for every allocation is what makes a *one-shot* builder
    reachable after the fact: when the 0x83/21 kind-4 message is copied at ~61 s
    the spec pointer is in rbx, so the allocation of that exact pointer can be
    looked up in this log and its caller is the builder.
    """
    def __init__(self, retaddr):
        super(SpecAllocRetBp, self).__init__("*0x%x" % retaddr)
        self.retaddr = retaddr
        self.silent = True
    def stop(self):
        try:
            n = bump("spec", 60000)
            if n is not None:
                note("SPECALLOC#%s ret=%#x spec=%#x" %
                     (n, self.retaddr - BASE if BASE else self.retaddr, rg("rax")))
        except Exception as e:
            note("specalloc err %r" % e)
        return False

def h_specalloc():
    """sub_44967AC(pool) -> message spec."""
    ra = ru64(rg("rsp"))
    if not ok(ra):
        return False
    if ra not in SPECA_BP:
        try:
            SPECA_BP[ra] = SpecAllocRetBp(ra)
            note("SPECALLOC caller armed ret=%#x" % (ra - BASE if BASE else ra))
        except Exception as e:
            note("specalloc arm err %r" % e)
    return False

# Return sites of sub_44967AC (call site + 5, from the xrefs in IDA).  One stop
# per allocation instead of two: the entry hook below costs an extra stop per
# message and stalls replication badly (measured: 427 datagrams in 120 s
# against 13,626 in the same window without it).
SPECA2_RETS = (0x448C9B1, 0x448CDD7, 0x448EC80, 0x448F0DB, 0x449056E,
               0x4490E53, 0x4491368, 0x4493E36)

class SpecAllocRet2(gdb.Breakpoint):
    def __init__(self, off):
        super(SpecAllocRet2, self).__init__("*0x%x" % (BASE + off))
        self.off = off
        self.silent = True
    def stop(self):
        try:
            n = bump("spec", 60000)
            if n is not None:
                note("SPECALLOC#%s ret=%#x spec=%#x" % (n, self.off, rg("rax")))
        except Exception as e:
            note("specalloc2 err %r" % e)
        return False

SPECA2_ARMED = [False]
# Wall clock at arm(), so the allocation probes can be switched on only for the
# last seconds before the 60 s reply instead of costing a stop per message from
# the start of the session.
T0 = [time.time()]
SPECA2_AT = float(os.environ.get("RKC_SPECALLOC_AT") or 52)

def time_arm_specalloc2():
    if SPECA2_ARMED[0] or not os.environ.get("RKC_SPECALLOC2"):
        return
    if time.time() - T0[0] < SPECA2_AT:
        return
    SPECA2_ARMED[0] = True
    note("%.1fs elapsed -- arming spec-allocation probes on the clock" %
         (time.time() - T0[0]))
    arm_specalloc2()

def maybe_arm_specalloc2(blob):
    """Arm the spec-allocation probes the moment the 0x83/20 challenge lands.

    Arming them at start-up is unusable: one stop per message stalls the client
    (measured 1,193 datagrams in 130 s against 13,626 in the same window).  The
    reply's spec is allocated after the challenge is processed, so this is both
    the cheapest and the correct window.
    """
    if SPECA2_ARMED[0] or not os.environ.get("RKC_SPECALLOC2"):
        return
    if b"\x83\x20\x04\x00\x00\x00" not in blob:
        return
    SPECA2_ARMED[0] = True
    note("0x83/20 challenge seen -- arming spec-allocation probes")
    arm_specalloc2()

def arm_specalloc2():
    for off in SPECA2_RETS:
        try:
            SpecAllocRet2(off)
        except Exception as e:
            note("specalloc2 arm %#x err %r" % (off, e))
    note("SPECALLOC2 armed on %d return sites" % len(SPECA2_RETS))

SPEC_WP = {}

class SpecWpBp(gdb.Breakpoint):
    """Write to a message spec's payload pointer (spec+0x40).

    At the copy that emits a message, rbx still holds the caller's spec
    (callee-saved, and the callee has not run yet), and spec+0x40 is the
    payload pointer just copied.  Whoever writes that field next is attaching
    a payload — i.e. is the builder — and [rsp] at the write is an exact
    one-hop caller, the one frame this binary can give reliably.

    Arming it early is the whole point: the 0x83/21 kind-4 message is sent once
    per session at ~61 s, so anything armed after it has fired is useless.
    """
    def __init__(self, addr):
        super(SpecWpBp, self).__init__("*0x%x" % addr, gdb.BP_WATCHPOINT, gdb.WP_WRITE)
        self.addr = addr
        self.silent = True
        self.hits = 0
        self.k4 = 0
    def stop(self):
        try:
            self.hits += 1
            ptr = ru64(self.addr)
            head = rd(ptr, 10).hex() if ok(ptr) else "?"
            ra = ru64(rg("rsp"))
            is_k4 = head.startswith("832104000000")
            if is_k4:
                self.k4 += 1
            if is_k4 or self.hits <= 12:
                n = bump("specwp", 80)
                note("SPECWP#%s hit=%d ra=%#x ptr=%#x head=%s%s" %
                     (n, self.hits, ra, ptr, head, "  <== K4" if is_k4 else ""))
            if self.k4:
                self.enabled = False
        except Exception as e:
            note("specwp err %r" % e)
        return False

def arm_spec_wp(spec):
    if SPEC_WP.get("armed"):
        return
    SPEC_WP["armed"] = 1
    try:
        SPEC_WP["bp"] = SpecWpBp(spec + 0x40)
        note("SPECWP armed at %#x (spec %#x) payload=%s" %
             (spec + 0x40, spec, rd(ru64(spec + 0x40), 8).hex() if ok(ru64(spec + 0x40)) else "?"))
    except Exception as e:
        note("specwp arm err %r" % e)

K4_SRC_WP = {}

class K4SrcWp(gdb.Breakpoint):
    """Write into the scratch buffer a 0x83/21 type-4 body was copied out of.

    At the copy hook that buffer already holds the finished 589-byte message,
    so the assembler has returned and cannot be found on any stack.  The buffer
    is a reusable scratch area, so the next write at this offset is that same
    assembler filling the next copy — the only moment its frame is live.
    """
    def __init__(self, base, addr, label):
        super(K4SrcWp, self).__init__("*0x%x" % addr, gdb.BP_WATCHPOINT, gdb.WP_WRITE)
        self.base, self.addr, self.label = base, addr, label
        self.silent = True
        self.hits = 0
    def stop(self):
        try:
            self.hits += 1
            m = bump("k4src", 16)
            if m is not None:
                note("K4SRC#%d %s at=%#x head=%s" %
                     (m, self.label, self.addr, rd(self.base, 14).hex()))
                note("   S " + " ".join(sbt(30)))
            if self.hits >= 6:
                self.enabled = False
        except Exception as e:
            note("k4src err %r" % e)
        return False

def arm_k4_src(src):
    if K4_SRC_WP.get("armed"):
        return
    K4_SRC_WP["armed"] = 1
    # +0 is the 0x83 id byte, +6 the u32 body length (0x242 = 578).
    for label, off in (("head", 0), ("len", 6)):
        try:
            K4_SRC_WP[label] = K4SrcWp(src, src + off, label)
            note("K4SRC armed %s at %#x" % (label, src + off))
        except Exception as e:
            note("k4src arm[%s] err %r" % (label, e))

COPY_K4 = {}
COPY_K3 = {}
K3_WP = {}

class K3WpBp(gdb.Breakpoint):
    """Write into the node buffer a 0x83/21 kind-3 frame was copied from.

    The copier runs after the frame exists, so the builder has returned by the
    time the copy hook sees it.  The node buffer is reusable scratch: the next
    write at this address is a later message being assembled into the same
    buffer, and that writer's PC names the builder directly.
    """
    def __init__(self, addr):
        super(K3WpBp, self).__init__("*0x%x" % addr, gdb.BP_WATCHPOINT, gdb.WP_WRITE)
        self.addr = addr
        self.silent = True
        self.hits = 0
    def stop(self):
        try:
            self.hits += 1
            pc, ra = rg("pc"), ru64(rg("rsp"))
            m = bump("k3wp", 12)
            if m is not None:
                note("K3WP#%d at=%#x pc=%#x ra=%#x val=%s" %
                     (m, self.addr, pc, ra, rd(self.addr, 16).hex()))
                note("   S " + " ".join(sbt(30)))
                note("   B " + " ".join(bt(24)))
            if self.hits >= 6:
                self.enabled = False
        except Exception as e:
            note("k3wp err %r" % e)
        return False

def arm_k3_wp(src):
    if K3_WP.get("armed") or not os.environ.get("RKC_K3WP"):
        return
    K3_WP["armed"] = 1
    try:
        K3_WP["bp"] = K3WpBp(src)
        note("K3WP armed at %#x val=%s" % (src, rd(src, 16).hex()))
    except Exception as e:
        note("k3wp arm err %r" % e)

SEND83_HITS = [0]

class Send83Bp(gdb.Breakpoint):
    """Entry of sub_4471D74: rsi is the finished app-message payload.

    sub_4490DAA has exactly one caller, so every 0x83/21 frame goes through
    this entry with its payload already assembled.  A gdb-side condition on
    the 83 21 header keeps the Python handler for just the join kind-3 and the
    60 s kind-4, and [rsp] is the exact return address of the function that
    built and sent it.
    """
    def __init__(self, addr):
        super(Send83Bp, self).__init__("*0x%x" % addr)
        self.silent = True
    def stop(self):
        try:
            # The prototype is (peer, payload, bits, ...), but a wrong-register
            # guess would silently miss the message, so check every argument
            # register for the 83 21 header.  gdb-side conditions were observed
            # not to filter this breakpoint, hence the in-Python test.
            regs = ("rsi", "rdx", "rcx", "r8", "r9", "rdi")
            src = None
            for r in regs:
                cand = rg(r)
                if ok(cand) and rd(cand, 2) == b"\x83\x21":
                    src = cand
                    break
            if src is None:
                return False
            SEND83_HITS[0] += 1
            ra, bits = ru64(rg("rsp")), rg("rdx")
            n = min((bits + 7) // 8, 48) if bits >= 0 else 0
            body = rd(src, n) if ok(src) else b""
            note("SEND83#%d kind=%s ra=%#x src=%#x bits=%#x body=%s" %
                 (SEND83_HITS[0], body[2] if len(body) > 2 else "?",
                  ra, src, bits, body.hex()))
            note("   S " + " ".join(sbt(32)))
            if len(body) > 2 and body[2] == 3:
                arm_ring(rg("rdi"), rg("r13"), src, bits)
            if SEND83_HITS[0] >= 8:
                self.enabled = False
        except Exception as e:
            note("send83 err %r" % e)
        return False

def arm_send83():
    if not os.environ.get("RKC_SEND83"):
        return
    try:
        Send83Bp(BASE + 0x4471D74)
        note("SEND83 armed at %#x" % (BASE + 0x4471D74))
    except Exception as e:
        note("send83 arm err %r" % e)

RING = {}

class RingWp(gdb.Breakpoint):
    """Write to the network object's outgoing-ring count (a1+0x450).

    sub_447F46E drains 160-byte slots from this ring; the slot it forwards at
    the 60 s mark holds the 0x83/21 kind-4 payload.  The enqueue increments the
    count after filling the new slot, so at the count write the tail slot
    (read_index + count - 1) is the one just queued.  Checking its payload head
    for 83 21 04 names the enqueue writer, which is the code that built and
    queued the body.
    """
    def __init__(self, net, off, base, cap, rd_off, prev):
        super(RingWp, self).__init__("*0x%x" % (net + off), gdb.BP_WATCHPOINT, gdb.WP_WRITE)
        self.net, self.off = net, off
        self.base, self.cap, self.rd_off, self.prev = base, cap, rd_off, prev
        self.silent = True
        self.hits = 0
        self.enq = 0
        self.k4 = 0
    def stop(self):
        try:
            self.hits += 1
            after = ru64(self.net + self.off)
            delta = after - self.prev
            self.prev = after
            if delta == 1:
                self.enq += 1
                rpos = ru64(self.net + self.rd_off)
                idx = (rpos + after - 1) % self.cap
                slot = self.base + 160 * idx
                payload = ru64(slot + 0x68) if ok(slot) else 0
                head = rd(payload, 10) if ok(payload) else b""
                if head.startswith(b"\x83\x21\x04"):
                    self.k4 += 1
                    note("RINGK4 off=%#x hits=%d enq=%d pc=%#x ra=%#x after=%d rd=%d idx=%d slot=%#x payload=%#x bits=%#x head=%s" %
                         (self.off, self.hits, self.enq, rg("pc"), ru64(rg("rsp")), after, rpos,
                          idx, slot, payload, ru32(slot + 16), head.hex()))
                    note("   S " + " ".join(sbt(32)))
                    note("   B " + " ".join(bt(24)))
                    self.enabled = False
                elif self.enq <= 4:
                    note("RINGENQ#%d pc=%#x after=%d rd=%d idx=%d slot=%#x payload=%#x head=%s" %
                         (self.enq, rg("pc"), after, rpos, idx, slot, payload,
                          head.hex() if head else "?"))
            if self.hits % 20000 == 0:
                note("RINGSTAT hits=%d enq=%d" % (self.hits, self.enq))
            if self.hits >= 200000:
                note("RINGWP cap reached; disarming")
                self.enabled = False
        except Exception as e:
            note("ringwp err %r" % e)
        return False

def arm_ring(net, slot, src, bits):
    if RING.get("armed") or not ok(net):
        return
    if os.environ.get("RKC_K4CHAIN"):
        note("RING skipped (RKC_K4CHAIN)")
        return
    try:
        base, cap = ru64(net + 0x438), ru64(net + 0x440)
        rd, count = ru64(net + 0x448), ru64(net + 0x450)
        if not (0 < cap < 100000 and ok(base)):
            note("RING bad layout net=%#x base=%#x cap=%s" % (net, base, cap))
            return
        expect = base + 160 * (rd % cap)
        note("RING net=%#x base=%#x cap=%d rd=%d count=%d slot=%#x expect=%#x src=%#x bits=%#x" %
             (net, base, cap, rd, count, slot, expect, src, bits))
        if expect != slot or ru64(slot + 0x68) != src:
            note("RING layout mismatch slot=%#x payloadfield=%#x src=%#x" %
                 (slot, ru64(slot + 0x68) if ok(slot) else 0, src))
            return
        RING["armed"] = 1
        RING["wps"] = []
        note("RING pending base=%#x cap=%d rd=%d count=%d" %
             (ru64(net + 0x418), ru64(net + 0x420), ru64(net + 0x428), ru64(net + 0x430)))
        for off, boff, coff, roff in ((0x450, 0x438, 0x440, 0x448),
                                      (0x430, 0x418, 0x420, 0x428)):
            b, c, r = ru64(net + boff), ru64(net + coff), ru64(net + roff)
            if 0 < c < 100000 and ok(b):
                try:
                    RING["wps"].append(RingWp(net, off, b, c, roff, ru64(net + off)))
                    note("RINGWP armed off=%#x base=%#x cap=%d rd=%d count=%d" %
                         (off, b, c, r, ru64(net + off)))
                except Exception as e:
                    note("ringwp arm off=%#x err %r" % (off, e))
            else:
                note("RINGWP skip off=%#x base=%#x cap=%s rd=%d" % (off, b, c, r))
    except Exception as e:
        note("ring arm err %r" % e)
# RKC_COPY_MIN raises the floor above small fragment copies, which are hot and
# say nothing about how a body was assembled.
COPY_MIN = [int(os.environ.get("RKC_COPY_MIN") or 64)]

def h_copy():
    """sub_4486D2E(bs, src, bytes) — fragment copies and blob copies."""
    JOINING[0] = True
    bs, src, nbytes = rg("rdi"), rg("rsi"), rg("rdx")
    if nbytes < COPY_MIN[0] or not ok(bs):
        return False
    m = bump("copy", 20000)
    if m is None:
        return False
    note("COPY#%d bytes=%d bs=%#x src=%#x pre=%s" % (m, nbytes, bs, src, ru32(bs + 0xC)))
    # rbx is the caller's message spec (see SpecWpBp); confirm it by checking
    # that its payload pointer is the buffer being copied, then watch that field
    # so the one-shot 0x83/21 builder is still reachable 60 s later.
    if os.environ.get("RKC_SPECWP") and not SPEC_WP.get("armed") and ok(src):
        spec = rg("rbx")
        if ok(spec) and ru64(spec + 0x40) == src:
            arm_spec_wp(spec)
    # bt() is worthless in libroblox (no frame pointers — it prints one frame
    # and then junk), so a large copy gets the stack scan instead.  At this
    # instant whoever assembles that blob is still live, which is the whole
    # point of hooking the copy path rather than the seal.
    if nbytes >= 256:
        note("   S " + " ".join(sbt(30)))
    # Read small bodies too: the join-time 0x83/21 kind-3 frame is only 27 B,
    # and its builder is the same message layer that emits the 60 s kind-4
    # answer.  The floor above is for logging/stacks, not for the pattern test.
    body = rd(src, min(nbytes, 65536)) if ok(src) else b""
    if body:
        # 0x83/21 kind 3 -- the fixed capability vector at join.  Its copier is
        # the 0x83/21 writer, reachable here while its frame is still live.
        k3 = body.find(b"\x83\x21\x03")
        if k3 >= 0 and not COPY_K3.get("hit"):
            COPY_K3["hit"] = 1
            note("COPY#K3 at=%d copy=%d body=%s" % (k3, nbytes, body[k3:k3 + 40].hex()))
            note("   S " + " ".join(sbt(30)))
            note("   B " + " ".join(bt(30)))
            note("   rbx=%#x from=%#x" % (rg("rbx"), src))
            arm_k3_wp(src)
    if body and nbytes >= 256:
        save("copy_src", m, body)
        # The 0x83/21 type-4 answer to the RCC's one-shot challenge.  Seeing it
        # go past this hook in one piece is what names its builder.
        k = body.find(b"\x83\x21\x04\x00\x00\x00")
        if k >= 0 and not COPY_K4.get("hit"):
            COPY_K4["hit"] = 1
            note("COPY#K4 at=%d copy=%d body=%s" % (k, nbytes, body[k:k + 40].hex()))
            note("   S " + " ".join(sbt(30)))
            # sub_4492EA4 keeps a frame pointer (it addresses args as
            # [rbp+arg_0]), so unlike the seal-time stack gdb may unwind here.
            # If it does, this is a precise chain instead of the scan's guess.
            note("   B " + " ".join(bt(30)))
            note("   spec=%#x from=%#x" % (rg("rbx"), src))
            if os.environ.get("RKC_K4SRCWP"):
                arm_k4_src(src)
    # A message body starts with its id byte.  Arm a write watchpoint on the
    # start of any join/replication body: the implementation reuses these
    # buffers, so the next write there names the serializer that fills them.
    # The 0x83/21 probes keep the four hardware slots for themselves; these
    # per-body watchpoints would exhaust them (measured: "Could not insert
    # hardware watchpoint 33") before the kind-4 reply is ever reached.
    k4_probe = os.environ.get("RKC_K4WP") or os.environ.get("RKC_K4SRCWP")
    if (body and nbytes >= 256 and body[0] in (0x90, 0x8a, 0x83, 0x92)
            and src not in FIRSTBODY[0] and JOINING[0] and not k4_probe):
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
    # A grow this large is a >=500-byte message being serialized, which at the
    # 60 s mark is the 0x83/21 type-4 attestation (4624 bits). The builder is
    # still live here, so the stack scan can name it where bt() cannot.
    if bits >= 4000:
        note("   S " + " ".join(sbt(28)))
    return False

# Chain-item anchors: `83 1c` / `83 0e` are client->server replication items
# (see FINDINGS.md).  The seal hook sees the plaintext, so a scan here tags the
# exact datagram and backtraces whoever produced it.  Capped hard: those byte
# pairs also occur inside payload data, and a hit is a lead, not a proof.
# (name, first byte, second byte, offset in the message buffer to watch).
# The reuse watchpoint catches the writer of the NEXT message at the same
# offset, which for a periodic stream is the same builder: that is how a
# builder that never appears on the seal stack still gets named.
CHAIN_ARM = (("1c", 0x83, 0x1c, 1), ("0e", 0x83, 0x0e, 1),
             ("0a", 0x83, 0x0a, 2), ("a6", 0xa6, None, 1))
# scan_chain only does the cheap "does this datagram contain the pattern"
# one-shot backtrace, for the two tags whose buffers get reused in place.
CHAIN_PATS = tuple((bytes((f, s)), n) for n, f, s, _o in CHAIN_ARM
                   if s is not None)
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
                note("CHAINWP#%d tag=%s val=%s" % (m, self.tag, rd(self.addr, 1).hex()))
                note("   S " + " ".join(sbt(28)))
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
    o = payload_start(plain)
    if o is None or o + 1 >= len(plain):
        return
    for name, first, second, off in CHAIN_ARM:
        if plain[o] != first:
            continue
        if second is not None and (o + 1 >= len(plain) or plain[o + 1] != second):
            continue
        if name in CHAIN_WP:
            continue
        try:
            CHAIN_WP[name] = ChainWpBp(buf + o + off, name)
            note("CHAINWP armed tag=%s at %#x" % (name, buf + o + off))
        except Exception as e:
            note("chainwp arm[%s] err %r" % (name, e))
        return

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

BTFN = {}

# RKC_PROBE=<hex value>: arm a *conditional* breakpoint on the tag writer for
# one sub-opcode, late enough that the join snapshot is done. That is the only
# way to name a builder whose message is appended to a batched datagram and so
# never appears on the seal stack. The condition is evaluated by gdb itself,
# so the Python side only runs when the value matches.
PROBE = os.environ.get("RKC_PROBE")
PROBE_HITS = [0]
PROBE_ARMED = [False]

class SubTagBp(gdb.Breakpoint):
    # Not "ProbeBp": that name belongs to the mapping poller defined later in
    # this file, and a late-bound call would resolve to it instead.
    def __init__(self, addr, val):
        super(SubTagBp, self).__init__("*0x%x" % addr)
        self.silent = True
        self.condition = "*(unsigned char*)$rsi == %d" % val
        self.val = val
    def stop(self):
        try:
            PROBE_HITS[0] += 1
            note("PROBE#%d val=%02x" % (PROBE_HITS[0], self.val))
            note("   P " + " ".join(bt(22)))
            if PROBE_HITS[0] >= 5:
                self.enabled = False
        except Exception as e:
            note("probe err %r" % e)
        return False

def arm_probe():
    if not PROBE or PROBE_ARMED[0]:
        return
    PROBE_ARMED[0] = True
    try:
        SubTagBp(BASE + 0x4486E48, int(PROBE, 16))
        note("PROBE armed for value %s" % PROBE)
    except Exception as e:
        note("probe arm err %r" % e)

# RKC_PROBE21=1: catch the byte write of 0x21 (the 0x83/21 message sub-id).  At
# function entry [rsp] still holds the return address, which is an exact one-hop
# caller — the only frame gdb can give reliably in this binary.  That is enough
# to name the function that writes the kind-4 header, which is the builder the
# copy-path hook could not show (it runs after the payload already exists).
P21_HITS = [0]
P21_ARMED = [False]

class Sub21Bp(gdb.Breakpoint):
    def __init__(self, addr):
        super(Sub21Bp, self).__init__("*0x%x" % addr)
        self.silent = True
        # 0x21 is a common byte, so make gdb itself require that the preceding
        # byte in the same stream is 0x83 — i.e. this is the 0x83/21 header and
        # not a payload byte.  Evaluated before Python runs.
        self.condition = (
            "*(unsigned char*)$rsi == 0x21"
            " && *(unsigned int*)($rdi+0xC) >= 8"
            " && *(unsigned char*)(*(unsigned long*)($rdi+0x18)"
            " + (*(unsigned int*)($rdi+0xC))/8 - 1) == 0x83")
    def stop(self):
        try:
            P21_HITS[0] += 1
            ra = ru64(rg("rsp"))
            bs = rg("rdi")
            bits = ru32(bs + 0xC) if ok(bs) else -1
            buf = ru64(bs + 0x18) if ok(bs) else 0
            prev = "?"
            if ok(buf) and bits >= 8:
                prev = rd(buf + bits // 8 - 8, 10).hex()
            note("P21#%d ra=%#x bs=%#x bits=%s buf=%#x prev=%s" %
                 (P21_HITS[0], ra, bs, bits, buf, prev))
            if P21_HITS[0] >= 400:
                self.enabled = False
        except Exception as e:
            note("p21 err %r" % e)
        return False

def arm_p21():
    if not os.environ.get("RKC_PROBE21") or P21_ARMED[0]:
        return
    P21_ARMED[0] = True
    try:
        Sub21Bp(BASE + 0x4486E48)
        note("P21 armed on the 0x21 byte write")
    except Exception as e:
        note("p21 arm err %r" % e)

def payload_start(b):
    """Offset of the first internal packet's payload in a data datagram."""
    o = 4                                   # 0x81 + u24 datagram number
    if o + 3 > len(b):
        return None
    fl = b[o]
    o += 3                                  # flags + u16 BE bit length
    if fl & 0x08: o += 4                    # ACK receipt
    if fl & 0x40: o += 3                    # reliable message number
    if fl & 0x20: o += 4                    # ordering index + channel
    if fl & 0x10: o += 3                    # sequencing index
    if fl & 0x04: o += 10                   # split header
    return o if o < len(b) else None

K4_WP = {}

class K4WpBp(gdb.Breakpoint):
    """First write to the 0x83 of a 0x83/21 body in the datagram buffer.

    The seal sees the datagram after every message in it has been serialized,
    so the builder has already returned and a stack scan finds stale frames
    from an earlier message.  The buffer is reused for the next datagram, so
    the next write at this exact offset is the builder writing this same body
    again — the one moment its frame is still live.
    """
    def __init__(self, addr):
        super(K4WpBp, self).__init__("*0x%x" % addr, gdb.BP_WATCHPOINT, gdb.WP_WRITE)
        self.addr = addr
        self.silent = True
        self.hits = 0
    def stop(self):
        try:
            self.hits += 1
            m = bump("k4wp", 12)
            if m is not None:
                note("K4WP#%d at=%#x val=%s" % (m, self.addr, rd(self.addr, 8).hex()))
                note("   S " + " ".join(sbt(30)))
            if self.hits >= 4:
                self.enabled = False
        except Exception as e:
            note("k4wp err %r" % e)
        return False

def arm_k4_wp(buf, j):
    if K4_WP.get("armed"):
        return
    K4_WP["armed"] = 1
    try:
        K4_WP["bp"] = K4WpBp(buf + j)
        note("K4WP armed at %#x (buf %#x + %d)" % (buf + j, buf, j))
    except Exception as e:
        note("k4wp arm err %r" % e)

def h_dgram():
    """sub_449C354(crypto, buf, len) — datagram seal; plaintext = buf[0:len-18]."""
    JOINING[0] = True
    obj, buf, ln = rg("rdi"), rg("rsi"), rg("rdx")
    if not ok(buf) or ln < 0x12 or ln > 4096:
        return False
    # A parity round runs for the full 11-minute session window at ~30
    # datagrams/s, so the old 600-frame debug cap truncated it after 20 s.
    m = bump("dgram", 100000)
    if m is None:
        return False
    plain = rd(buf, ln - 18)
    note("DGRAM#%d len=%d hdr=%s" % (m, ln, plain[:32].hex()))
    save("dgram", m, plain)
    # One backtrace per application id (and per 0x83 subtype) names the exact
    # builder of that message, so its wire shape can be read out of decompiled
    # code instead of guessed from samples. The seal call is on the same stack
    # as the builder, which is what makes this worth doing here.
    # Arm the late probe once the join snapshot has clearly finished.
    if m == 250:
        arm_probe()
    time_arm_specalloc2()
    o = payload_start(plain)
    if o is not None and o < len(plain):
        key = plain[o]
        if key == 0x83 and o + 1 < len(plain):
            key = 0x8300 | plain[o + 1]
        if key not in BTFN:
            BTFN[key] = 1
            note("BTFN#%x len=%d body=%s" %
                 (key, len(plain) - o, plain[o:o + 48].hex()))
            note("   B " + " ".join(bt(30)))
    # 0x83/21 type 4 — the 60 s attestation. It rides in a datagram whose first
    # message is something else (measured: offset 54, after an 0xa6 tick), so
    # the per-id backtrace above never sees it. Scan the whole datagram and, on
    # the first hit of the session, record where the builder sits on the stack.
    j = plain.find(bytes((0x83, 0x21, 0x04, 0, 0, 0)))
    if j >= 0 and not BTFN.get("k4"):
        BTFN["k4"] = 1
        n4 = int.from_bytes(plain[j + 6:j + 10], "little") if j + 10 <= len(plain) else -1
        note("BTFN#k4 len=%d at=%d body=%s" % (n4, j, plain[j:j + 40].hex()))
        note("   S " + " ".join(sbt(30)))
        if os.environ.get("RKC_K4WP"):
            arm_k4_wp(buf, j)
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

def h_aead():
    """sub_44AD85C(out, tag_out, taglen_out, src, len, aad, aadlen, 0, nonce, key).

    The ChaCha20-Poly1305 primitive behind both sealers.  A datagram seal is
    filtered out by its 0x81/0xc0 RakNet header: what is left is a seal applied
    *inside* the datagram, which is where the opaque 0x83/21 kind-4 body has to
    come from.  Dumps the plaintext, the 12-byte nonce and the 32-byte key.
    """
    src, ln = rg("rcx"), rg("r8")
    if not ok(src) or ln < 8 or ln > 0x4000:
        return False
    head = rd(src, 4)
    if not head or head[0] in (0x81, 0xc0):
        return False
    sp = rg("rsp")
    nonce, key = ru64(sp + 0x18), ru64(sp + 0x20)
    n = bump("aead", 200)
    if n is None:
        return False
    note("AEAD#%d len=%d head=%s nonce=%s key=%s" %
         (n, ln, head.hex(), rd(nonce, 12).hex(), rd(key, 32).hex()))
    note("   C " + " ".join(bt(16)))
    save("aead_plain", n, rd(src, ln))
    return False

def h_msgseal():
    """sub_449BE6A(conn, out, total, src, len, taglen) — message sealer.

    28 bytes of overhead: [16 cleartext][ct][12-byte nonce][16-byte tag].
    """
    src, ln = rg("rcx"), rg("r8")
    if not ok(src) or ln < 8 or ln > 0x4000:
        return False
    n = bump("msgseal", 120)
    if n is None:
        return False
    note("MSGSEAL#%d len=%d head=%s" % (n, ln, rd(src, 32).hex()))
    note("   C " + " ".join(bt(16)))
    save("msgseal_plain", n, rd(src, ln))
    return False

def h_rxopen():
    """The receive-side twin of the datagram seal: decrypted app messages.

    Hooked at `+449C4F0`, the instruction after the AEAD open inside
    `sub_449C432(conn, buf, len)` (the decoder sits immediately after the
    datagram seal `sub_449C354`, and calls the same AEAD wrapper family):

        ctlen = len - 18
        key   = conn + 64 + 32*(conn[40] == 0)      // epoch ring
        nonce = LE64(counter) || "mbeR"             // sub_449BDFE
        ok    = opener(buf, buf, ctlen, tag, nonce, key) == 0   // in place

    At the hook point the plaintext is already in `r14` (same buffer it came
    in), the plaintext length is `[rbp-0x58]`, and `eax` is the opener's
    return value (0 = authenticated). This is the only place the engine itself
    shows what it accepts, so it answers "does the RCC challenge the native?"
    without needing offline key recovery or full-stream coverage.
    """
    try:
        if rg("eax") != 0:            # tag rejected: not plaintext, do not dump
            return False
        buf, ln = rg("r14"), ru64(rg("rbp") - 0x58)
        if not ok(buf) or ln == 0 or ln > 0x2000:
            return False
        n = bump("rxopen")
        if n is None:
            return False
        blob = rd(buf, ln)
        note("RXOPEN#%d len=%d %s" % (n, ln, blob[:32].hex()))
        save("rxopen", n, blob)
        maybe_arm_specalloc2(blob)
        time_arm_specalloc2()
    except Exception as e:
        note("rxopen err %r" % e)
    return False

# libroblox's own PLT stubs (IDA, this build: .plt entries that jump through
# .got.plt).  Socket calls are resolved to the APK's bundled bionic libc, not
# glibc, so a `break sendto` binds a symbol the client never calls and stays
# silent for a whole session.  Hooking the client's PLT entry fires no matter
# which libc wins the lookup, and the caller's argument registers are still
# intact at that point.
PLT_SENDMMSG = 0x6D86070
PLT_SENDMSG = 0x6D86340
PLT_SENDTO = 0x6D86080
PLT_RECVFROM = 0x6D86050
PLT_RECVMMSG = 0x6D86060
ARMED_BASE = [None]


# --------------------------------------------- 0x83/21 AC-response chain --
# RKC_K4CHAIN=1 traces the buffer chain that feeds sub_665A78A:
#   sub_665ADF0 builds the 0x242-byte body at scratch+0xC000 (length at
#   +0xC004, data at +0xC008); sub_6659D20 copies it into the captured lambda;
#   sub_665A78A invokes that lambda and wraps the captured bytes in an
#   AndroidACResponseItem; sub_665F40E serializes the item.

def h_k4lam():
    """Entry of sub_665A78A: a1 is the captured lambda closure."""
    a1, ra = rg("rdi"), ru64(rg("rsp"))
    if not ok(a1):
        return False
    typ, beg, end = ru32(a1 + 0x20), ru64(a1 + 8), ru64(a1 + 16)
    n = end - beg
    if not ok(beg) or n < 0 or n > 0x100000:
        return False
    k = bump("k4lam", 16)
    if k is None:
        return False
    note("K4LAM#%d a1=%#x typ=%s ra=%#x beg=%#x end=%#x n=%d head=%s" %
         (k, a1, typ, ra, beg, end, n, rd(beg, min(n, 24)).hex()))
    note("   S " + " ".join(sbt(40)))
    if n:
        save("k4lam", k, rd(beg, min(n, 65536)))
    return False

def h_k4copy():
    """0x665A188: the memcpy(dest, scratch+0xC008, n) has just completed."""
    rbp = rg("rbp")
    if not ok(rbp):
        return False
    scratch, dest, n = ru64(rbp - 0x148), ru64(rbp - 0x188), rg("r14")
    if not ok(scratch) or n < 0 or n > 0x10000:
        return False
    src = scratch + 0xC008
    k = bump("k4copy2", 16)
    if k is None:
        return False
    note("K4COPY#%d scratch=%#x dest=%#x n=%d src=%#x srchead=%s desthead=%s" %
         (k, scratch, dest, n, src,
          rd(src, min(n, 24)).hex() if n else "",
          rd(dest, min(n, 24)).hex() if n else ""))
    note("   S " + " ".join(sbt(40)))
    if n:
        save("k4copy2_src", k, rd(src, min(n, 65536)))
        save("k4copy2_dest", k, rd(dest, min(n, 65536)))
    return False

class K4ScratchWp(gdb.Breakpoint):
    """Write to the scratch packet's length or first data byte."""
    def __init__(self, addr, label):
        super(K4ScratchWp, self).__init__("*0x%x" % addr, gdb.BP_WATCHPOINT, gdb.WP_WRITE)
        self.addr, self.label = addr, label
        self.silent = True
        self.hits = 0
    def stop(self):
        try:
            self.hits += 1
            m = bump("k4swp", 60)
            if m is not None:
                note("K4SWP#%d %s at=%#x pc=%#x ra=%#x val=%s" %
                     (m, self.label, self.addr, rg("pc"), ru64(rg("rsp")),
                      rd(self.addr, 16).hex()))
                note("   S " + " ".join(sbt(40)))
            if self.hits >= 12:
                self.enabled = False
        except Exception as e:
            note("k4swp err %r" % e)
        return False

def h_k4scratch():
    """0x6659E26: sub_6659D20 has just allocated its 0x21800-byte scratch."""
    rbp = rg("rbp")
    if not ok(rbp):
        return False
    scratch = ru64(rbp - 0x148)
    if not ok(scratch):
        return False
    k = bump("k4scratch", 8)
    if k is None:
        return False
    note("K4SCRATCH#%d scratch=%#x len=%s head=%s" %
         (k, scratch, ru32(scratch + 0xC004),
          rd(scratch + 0xC008, 32).hex()))
    for label, off in (("len", 0xC004), ("head", 0xC008)):
        try:
            K4ScratchWp(scratch + off, label)
            note("K4SWP armed %s at %#x" % (label, scratch + off))
        except Exception as e:
            note("k4swp arm[%s] err %r" % (label, e))
    return False

def h_k4token():
    """sub_665ADF0: builds the 0x242-byte body; token lands at +0xC008."""
    a1, a2, a3, a4, a5 = rg("rdi"), rg("rsi"), rg("rdx"), rg("rcx"), rg("r8")
    k = bump("k4token", 16)
    if k is None:
        return False
    note("K4TOKEN#%d a1=%#x a2=%s a3=%s a4=%s a5=%#x ra=%#x" %
         (k, a1, a2, a3, a4, a5, ru64(rg("rsp"))))
    note("   S " + " ".join(sbt(40)))
    if ok(a5):
        note("   A5 " + rd(a5, 0x80).hex())
    return False

def h_k4ser():
    """sub_665F40E: AndroidACResponseItem serializer."""
    a1, ra = rg("rdi"), ru64(rg("rsp"))
    if not ok(a1):
        return False
    beg, end = ru64(a1 + 0x48), ru64(a1 + 0x50)
    n = end - beg
    k = bump("k4ser", 16)
    if k is None:
        return False
    note("K4SER#%d a1=%#x ra=%#x type=%s beg=%#x end=%#x n=%d head=%s" %
         (k, a1, ra, ru32(a1 + 0x40), beg, end, n,
          rd(beg, min(n, 32)).hex() if ok(beg) and n > 0 else ""))
    note("   S " + " ".join(sbt(40)))
    if ok(beg) and n > 0:
        save("k4ser", k, rd(beg, min(n, 65536)))
    return False

def arm_k4chain():
    if not os.environ.get("RKC_K4CHAIN"):
        return
    for tag, off, fn in (("k4lam", 0x665A78A, h_k4lam),
                         ("k4scratch", 0x6659E26, h_k4scratch),
                         ("k4copy", 0x665A188, h_k4copy),
                         ("k4token", 0x665ADF0, h_k4token),
                         ("k4ser", 0x665F40E, h_k4ser)):
        try:
            Bp(tag, off, fn)
        except Exception as e:
            note("k4chain arm %s err %r" % (tag, e))
    note("K4CHAIN armed lam/copy/token/ser")

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
                         ("kdf", 0x44AA839), ("seal", 0x648C61A),
                         ("aead", 0x44AD85C), ("aead2", 0x44AC0C7),
                         ("msgseal", 0x449BE6A), ("rxopen", 0x449C4F0),
                         ("send83", 0x4471D74),
                         ("k4lam", 0x665A78A), ("k4scratch", 0x6659E26),
                         ("k4copy", 0x665A188), ("k4token", 0x665ADF0),
                         ("k4ser", 0x665F40E)):
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
                         ("kdf", 0x44AA839, h_kdf), ("seal", 0x648C61A, h_seal),
                         ("aead", 0x44AD85C, h_aead), ("aead2", 0x44AC0C7, h_aead),
                         ("msgseal", 0x449BE6A, h_msgseal),
                         ("rxopen", 0x449C4F0, h_rxopen),
                         ("specalloc", 0x44967AC, h_specalloc)):
        if not want(tag):
            continue
        try:
            Bp(tag, off, fn)
        except Exception as e:
            note("arm %s err %r" % (tag, e))
    if want("wire"):
        for tag, off, fn in (("plt_sendmmsg", PLT_SENDMMSG, h_sendmmsg),
                             ("plt_sendmsg", PLT_SENDMSG, h_sendmsg),
                             ("plt_sendto", PLT_SENDTO, h_sendto)):
            try:
                Bp(tag, off, fn)
            except Exception as e:
                note("arm %s err %r" % (tag, e))
    if MAL_BP is not None:
        MAL_BP.enabled = True          # join-band allocations only, post-preflight
    if want("rx"):
        for bp in RX_BPS:
            bp.enabled = True
        try:
            Bp("plt_recvfrom", PLT_RECVFROM, h_recvfrom)
        except Exception as e:
            note("arm plt_recvfrom err %r" % e)
    arm_p21()
    arm_send83()
    arm_k4chain()
    ARMED = True
    ARMED_BASE[0] = BASE
    note("== ARMED BASE=%#x ==" % BASE)
    return True

# ------------------------------------------------------- process-wide BPs --
def wire_frame(buf, ln):
    """One outgoing frame: arm on the first RUPP one, keep the rest.

    RUPP header: 01 00 <u16 BE total length> 01 11 <sub>. The total length
    varies per frame, so matching it (it used to be pinned to 31) only armed
    when the *first* captured frame happened to be that size — which is how a
    live run reached a joined session with no hooks armed at all. Match only
    the fixed fields.

    The first wire frame is also proof that libroblox is mapped and executing.
    The caller's return address identifies *which* mapping runs (the launcher
    sometimes keeps a stale whole-file copy at another base), so arming happens
    here: hooks armed earlier can be wiped by the loader's remap, and this is
    the last moment at which arming is both safe and useful.

    -> (n, frame) on capture, None otherwise.
    """
    if not ok(buf) or ln < 40 or ln > 4096:
        return None
    data = rd(buf, min(ln, 64))
    if not data or not (len(data) >= 6 and data[:2] == b"\x01\x00" and
                        data[4:6] == b"\x01\x11"):
        return None
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
    n = bump("wire", 20000)
    if n is None:
        return None
    frame = rd(buf, ln)
    if not frame:
        return None
    save("wire_tx", n, frame)
    return (n, frame)


def note_peer(n, sa):
    """Record where a frame went: AF_INET sockaddr with the port in BE."""
    if sa and len(sa) == 16 and sa[0] == 2:
        note("SEND#%d dst=%d.%d.%d.%d:%d" %
             (n, sa[4], sa[5], sa[6], sa[7], (sa[2] << 8) | sa[3]))


def h_sendto():
    """sendto(fd, buf, len, flags, dest, addrlen) — dest is r8."""
    r = wire_frame(rg("rsi"), rg("rdx"))
    if r:
        dst = rg("r8")
        if ok(dst):
            note_peer(r[0], rd(dst, 16))
    return False


def h_sendmmsg():
    """sendmmsg(fd, msgvec, vlen, flags) — the path RakNet actually uses.

    Struct layout (x86-64 Linux): mmsghdr is 64B, msghdr puts msg_iov at +16
    and msg_iovlen at +24; each iovec is 16B. A batch is several datagrams, so
    every entry is examined, not just the first.
    """
    vec, vlen = rg("rsi"), rg("rdx")
    if not ok(vec) or vlen == 0 or vlen > 64:
        return False
    for i in range(min(vlen, 16)):
        mh = vec + i * 64
        iovp, iovn = ru64(mh + 16), ru64(mh + 24)
        if not ok(iovp) or iovn == 0 or iovn > 4:
            continue
        for k in range(iovn):
            base, blen = ru64(iovp + k * 16), ru64(iovp + k * 16 + 8)
            r = wire_frame(base, blen)
            if r:
                name = ru64(mh)
                if ok(name):
                    note_peer(r[0], rd(name, 16))
    return False

def h_sendmsg():
    """sendmsg(fd, msghdr*, flags) — one header, same iovec walk."""
    mh = rg("rsi")
    if not ok(mh):
        return False
    iovp, iovn = ru64(mh + 16), ru64(mh + 24)
    if not ok(iovp) or iovn == 0 or iovn > 4:
        return False
    for k in range(iovn):
        base, blen = ru64(iovp + k * 16), ru64(iovp + k * 16 + 8)
        r = wire_frame(base, blen)
        if r:
            name = ru64(mh)
            if ok(name):
                note_peer(r[0], rd(name, 16))
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
                    m = bump("rx", 20000)
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
# The RakNet transport batches datagrams through sendmmsg, so a sendto hook
# alone can stay silent for a whole session (observed: a joined run with no
# ARMED line and no wire frames). Hook both; whichever fires first arms.
try:
    if _WANT_WIRE:
        SymBp("sendto", "sendto", h_sendto)
        SymBp("sendmmsg", "sendmmsg", h_sendmmsg)
except Exception as e:
    note("send bp err %r" % e)
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
PROBE_SEEN = [False]


def dump_libroblox_maps():
    """Log the raw libroblox /proc/maps lines once.

    Segment offsets are the only trustworthy way to name a load base, and a
    garbled derivation (offsets read from the wrong column) silently disables
    every offset hook, so the raw lines belong in the log.
    """
    try:
        pid = gdb.selected_inferior().pid
        for line in open("/proc/%d/maps" % pid).read().splitlines():
            if "libroblox.so" in line and "audio" not in line:
                note("MAPS " + line.strip())
    except Exception as e:
        note("maps dump err %r" % e)

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
            if PROBE_HITS[0] % 1024:
                return False
            segs = _libroblox_segments()
            if not segs:
                return False
            if not PROBE_SEEN[0]:
                PROBE_SEEN[0] = True
                note("PROBE libroblox mapped: " +
                     " ".join("%#x/%#x" % (b, o) for b, o, s, p in segs[:6]))
                dump_libroblox_maps()
            # Arm here rather than waiting for the first outgoing frame: the
            # send hook now lives in the client's own PLT, which only exists
            # once the library is mapped, and a session can join without ever
            # reaching the send path (auth preflight failures).  arm() verifies
            # the code bytes at BASE, so a stale whole-file copy is refused and
            # the next poll retries.
            if not ARMED:
                base = find_base(final_only=True)
                if base is not None and arm():
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
extra_args = shlex.split(os.environ.get("RC_ARGS", "").strip())
uri = os.environ.get("RKC_LAUNCH_URI")
if not uri and "--launch-uri" not in extra_args:
    uri = "roblox://placeId=%s" % os.environ.get("RKC_PLACE", "1818")
args = ["--lib", os.environ.get("RKC_LIB", "/home/john/rr/libroblox.so"),
        "--assets", os.environ.get("RKC_ASSETS", "/home/john/rr/assets")]
if uri:
    args += ["--launch-uri", uri]
args += extra_args
args += ["--roblosecurity", os.environ.get("RKC_COOKIE", "")]
# No --graphics override: forcing opengl makes SDL_CreateWindow fail with
# "Could not create GLES window surface" under gdb, and the client exits 1 before
# the join (measured 2026-11-05). Default graphics selection works.
gdb.execute("set args " + " ".join("'%s'" % a for a in args))
try:
    gdb.execute("run")
except Exception as e:
    note("run err %r" % e)
note("inferior exited: base=%s probe=%d" % (hex(find_base() or 0), PROBE_HITS[0]))
note("== script end hits=%r ==" % HITS)
end
