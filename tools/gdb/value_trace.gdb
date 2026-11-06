# value_trace.gdb — log the native property-value reader's exact wire extent.
#
# Purpose: the 0x03 (property update) items in live 0x83 chains disagree with
# the snapshot value shape for some tags (a live f32 looks like
# [u8][f32]). The native is the only authority on how many bytes a tag
# actually consumes, so this script breakpoints sub_64E9D50 (the value
# reader) and logs, per call:
#
#     VT <tag> <bytes consumed>
#
# (tag = *(u8*)(descriptor+1), extent = stream->pos delta across the call).
# Grouping VT lines by tag gives the true per-tag wire length, including any
# format prefix the static reader hides behind a flag.
#
# Env: RKC_DIR (output dir), RKC_COOKIE, RKC_PLACE, RKC_LIB, RKC_ASSETS
# Run with: tools/gdb/value_trace.sh
set pagination off
set confirm off
set breakpoint pending on
handle SIGSEGV nostop noprint pass
python
import gdb, os, re

RUN = os.environ.get("RKC_DIR", "/tmp/value_trace")
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "vt.log"), "a", buffering=1)

def note(m):
    LOG.write(m + "\n")
    LOG.flush()

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

BASE = None

def _segs():
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
    groups = {}
    for base, off, size, perms in _segs():
        groups.setdefault(base, []).append((off, size, perms))
    for base, g in groups.items():
        if rd(base, 4) != b"\x7fELF":
            continue
        first = [x for x in g if x[0] == 0]
        others = [x for x in g if x[0] != 0]
        if first and others and first[0][1] < 0x4000000:
            return base
    return None

VT_HITS = [0]
SEEN = {}

def base_from_pc(pc):
    """Load base of the libroblox mapping that contains pc (or None).

    For every LOAD segment vaddr == file offset, so base = mapping.lo -
    mapping.file_offset for the mapping that contains the address.  The
    /proc maps alone cannot choose between the launcher's temporary
    whole-file copy and the real mapping; an executing pc can.
    """
    for base, off, size, perms in _segs():
        lo = base + off
        if lo <= pc < lo + size:
            return base
    return None

def base_from_bt():
    try:
        out = gdb.execute("bt 12", to_string=True)
    except Exception:
        return None
    for line in out.split("\n"):
        m = re.search(r"0x([0-9a-f]{6,})", line)
        if not m:
            continue
        b = base_from_pc(int(m.group(1), 16))
        if b and rd(b, 4) == b"\x7fELF":
            return b
    return None

class Fin(gdb.FinishBreakpoint):
    def __init__(self, tag, stream, pos):
        super(Fin, self).__init__(internal=True)
        self.tag = tag
        self.stream = stream
        self.pos = pos

    def stop(self):
        try:
            out = ru32(self.stream + 56)
            if out is not None:
                note("VT %d %d" % (self.tag, out - self.pos))
                VT_HITS[0] += 1
                if VT_HITS[0] % 200 == 0:
                    note("VT progress %d" % VT_HITS[0])
        except Exception as e:
            note("fin err %r" % e)
        return False

class ItemBp(gdb.Breakpoint):
    """0x03 item reader entry: log the stream cursor before the header."""

    def __init__(self):
        super(ItemBp, self).__init__("*0x%x" % (BASE + 0x64CC280))
        self.silent = True

    def stop(self):
        try:
            stream = rg("rdx")
            pos = ru32(stream + 56)
            if pos is not None:
                note("IT %d" % pos)
        except Exception as e:
            note("item bp err %r" % e)
        return False


class DispFin(gdb.FinishBreakpoint):
    def __init__(self, ty, stream, pos, buf):
        super(DispFin, self).__init__(internal=True)
        self.ty, self.stream, self.pos, self.buf = ty, stream, pos, buf

    def stop(self):
        try:
            out = ru32(self.stream + 56)
            if out is not None:
                blen = max(0, out - self.pos)
                blob = rd(self.buf + self.pos, min(64, blen + 16))
                note("D3 %d %d %s" % (self.ty, blen, blob.hex()))
        except Exception as e:
            note("disp fin err %r" % e)
        return False


class VtBp(gdb.Breakpoint):
    """sub_64CC280's virtual call at vtable+0x458: name the callee.

    That method consumes the one byte between the u16 property id and the
    value, so its target is the authority on what the byte means.
    """

    def __init__(self):
        super(VtBp, self).__init__("*0x%x" % (BASE + 0x64CC42D))
        self.silent = True
        self.hits = 0

    def stop(self):
        try:
            obj = rg("rax")
            tgt = ru64(obj + 0x458)
            self.hits += 1
            note("VCALL %#x" % ((tgt - BASE) if tgt > BASE else 0))
            stream = rg("rsi")
            note("VCALL args rdi=%#x rsi=%#x rdx=%#x pos=%s" %
                 (rg("rdi"), stream, rg("rdx"), ru32(stream + 56)))
            if self.hits >= 4:
                self.enabled = False
        except Exception as e:
            note("vt bp err %r" % e)
        return False


class DispBp(gdb.Breakpoint):
    """Chain item dispatcher: record the wire extent of every 0x03 item."""

    def __init__(self):
        super(DispBp, self).__init__("*0x%x" % (BASE + 0x63F224E))
        self.silent = True

    def stop(self):
        try:
            ty = rg("rcx") & 0xFF
            stream = rg("rdx")
            pos = ru32(stream + 56)
            buf = ru64(stream + 16)
            if ty == 3 and pos is not None:
                DispFin(ty, stream, pos, buf)
        except Exception as e:
            note("disp bp err %r" % e)
        return False


class ValBp(gdb.Breakpoint):
    def __init__(self):
        super(ValBp, self).__init__("*0x%x" % (BASE + 0x64E9D50))
        self.silent = True

    def stop(self):
        try:
            desc = rg("rdx")
            stream = rg("rsi")
            tag = rd(desc + 1, 1)
            pos = ru32(stream + 56)
            if tag and pos is not None:
                ra = ru64(rg("rsp"))
                off = (ra - BASE) if (ra and ra > BASE) else 0
                note("VS %d %d R%x" % (tag[0], pos, off))
                Fin(tag[0], stream, pos)
        except Exception as e:
            note("val bp err %r" % e)
        return False

SEND_HITS = [0]

class SendBp(gdb.Breakpoint):
    """Arm the value reader once the client is actually talking to a server."""

    def __init__(self):
        super(SendBp, self).__init__("sendto")
        self.silent = True

    def stop(self):
        global BASE
        SEND_HITS[0] += 1
        if SEND_HITS[0] % 8:
            return False
        if BASE is None:
            BASE = stable_base() or base_from_bt()
        note("sendto #%d: base=%s" % (SEND_HITS[0], hex(BASE) if BASE else None))
        if BASE:
            ItemBp()
            ValBp()
            DispBp()
            VtBp()
            note("item reader armed at %#x" % (BASE + 0x64CC280))
            note("value reader armed at %#x" % (BASE + 0x64E9D50))
            note("dispatcher armed at %#x" % (BASE + 0x63F224E))
            self.enabled = False
        return False

try:
    SendBp()
    note("=== value_trace launch (place=%s) ===" % os.environ.get("RKC_PLACE", "1818"))
except Exception as e:
    note("send bp err %r" % e)

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
note("inferior exited: vt_hits=%d" % VT_HITS[0])
end
