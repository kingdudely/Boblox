# doc_dump3.gdb — dump the exact plaintext the native seals into its 0x90/0x8a.
#
# sub_648C61A is the seal wrapper:
#     sub_664D9B0(state, data_ptr, len, ..., &outlen, key)
# so at sub_664D9B0 entry rsi = plaintext, rdx = len. That is the body *before*
# any envelope, i.e. exactly what the server parses. Dumping it removes every
# assumption about std::string layout.
#
# Also probes sub_3917851 (the length-prefixed string writer) so the join
# document's in-memory shape can be read off directly if the body is built from
# a string.
#
# Env: RKC_DIR, RKC_COOKIE, RKC_PLACE, RKC_LIB, RKC_ASSETS.
set pagination off
set confirm off
set breakpoint pending on
handle SIGSEGV nostop noprint pass
python
import gdb, os

RUN = os.environ.get("RKC_DIR", "/tmp/docdump3")
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "log.txt"), "a", buffering=1)

def note(m):
    LOG.write(m + "\n"); LOG.flush()

def rd(a, n):
    try:
        return gdb.selected_inferior().read_memory(a, n).tobytes()
    except Exception:
        return b""

def reg(n):
    try:
        return int(gdb.parse_and_eval("$" + n)) & 0xFFFFFFFFFFFFFFFF
    except Exception:
        return 0

def find_base():
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
        try:
            lo = int(parts[0].split("-")[0], 16); off = int(parts[2], 16)
        except Exception:
            continue
        base = lo - off
        if base not in cands:
            cands.append(base)
    for b in cands:
        if rd(b, 4) == b"\x7fELF":
            return b
    return None

SEAL = [0]
STR = [0]


class SealBp(gdb.Breakpoint):
    """sub_664D9B0: rsi = plaintext, rdx = length."""
    def __init__(self, addr):
        super(SealBp, self).__init__("*0x%x" % addr, internal=False)
        self.silent = True
    def stop(self):
        try:
            ptr, ln = reg("rsi"), reg("rdx")
            if 32 <= ln <= 200000:
                body = rd(ptr, ln)
                path = os.path.join(RUN, "seal_%d.bin" % SEAL[0])
                open(path, "wb").write(body)
                braces = body.find(b'{')
                note("seal[%d] len=%d head=%s brace=%d json=%d"
                     % (SEAL[0], len(body), body[:24].hex(), braces,
                        body.find(b'"UserId"')))
                SEAL[0] += 1
        except Exception as e:
            note("seal bp err %r" % e)
        return False


class StrBp(gdb.Breakpoint):
    """sub_3917851: rdi = string object."""
    def __init__(self, addr):
        super(StrBp, self).__init__("*0x%x" % addr, internal=False)
        self.silent = True
    def stop(self):
        try:
            if STR[0] >= 24:
                return False
            p = reg("rdi")
            raw = rd(p, 32)
            note("str[%d] %s" % (STR[0], raw.hex()))
            # log a short preview from each plausible pointer
            for off in (0, 8, 16, 24):
                q = int.from_bytes(raw[off:off + 8], "little")
                if 0x10000 < q < (1 << 47):
                    prev = rd(q, 24)
                    note("   +%d -> %s" % (off, prev))
            STR[0] += 1
        except Exception as e:
            note("str bp err %r" % e)
        return False


class Probe(gdb.Breakpoint):
    def __init__(self):
        super(Probe, self).__init__("clock_gettime", internal=False)
        self.silent = True
        self.hits = 0
    def stop(self):
        try:
            self.hits += 1
            if self.hits % 32:
                return False
            b = find_base()
            if b:
                note("base=%x" % b)
                SealBp(b + 0x664D9B0)
                StrBp(b + 0x3917851)
                self.enabled = False
        except Exception as e:
            note("probe err %r" % e)
        return False

try:
    Probe()
except Exception as e:
    note("probe arm err %r" % e)

note("=== doc_dump3 launch place=%s ===" % os.environ.get("RKC_PLACE", "1818"))
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
note("done seals=%d strings=%d" % (SEAL[0], STR[0]))
end
