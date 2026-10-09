set pagination off
set confirm off

python
import gdb, os

ROOT = "/home/john/RobloxInBrowser"
RUN = os.path.join(ROOT, "run")
LOG = open(os.path.join(RUN, "anarchy.log"), "a", buffering=1)

MODE = os.environ.get("ANARCHY_MODE", "corrupt")   # corrupt | drop | log
CORRUPT = int(os.environ.get("ANARCHY_VALUE", "DEADBEEF"), 16)

def u8(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 1).tobytes(), "little")
def u64(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 8).tobytes(), "little")

pid = gdb.selected_inferior().pid
BASE = None
for line in open(f"/proc/{pid}/maps"):
    if "libroblox.so" in line and line.split()[2] == "00000000":
        BASE = int(line.split("-")[0], 16)
        break
print(f"BASE={hex(BASE)} mode={MODE}")

HITS = {"n": 0}

class SendBp(gdb.Breakpoint):
    """app=rdx&0xFF, chan=rcx, ns=r8 — intercept 0x9B answer send on app=4 chan=1."""
    def stop(self):
        try:
            rdx, rcx, r8 = (int(gdb.parse_and_eval(r)) for r in ("$rdx", "$rcx", "$r8"))
            app = rdx & 0xFF
            chan = rcx & 0xFFFFFFFF
            if app != 4 or chan != 1:
                return False
            ns = r8
            flag = u8(ns + 64)
            if flag:
                data_ptr = u64(ns + 32)
                length = u64(ns + 40)
            else:
                data_ptr = u64(ns + 8)
                length = u64(ns + 16) - data_ptr
            consumed = u64(ns + 72)
            begin = data_ptr + consumed
            size = length - consumed if length >= consumed else 0
            if not (0 < size < 66000):
                return False
            payload = gdb.selected_inferior().read_memory(begin, size).tobytes()
            if payload[:1] != b"\x9b" or size != 9:
                return False
            HITS["n"] += 1
            LOG.write(f"HIT#{HITS['n']} mode={MODE} app={app} chan={chan} size={size} orig={payload.hex()}\n")
            LOG.flush()
            if MODE == "corrupt":
                # keep [9b][u2], replace answer with garbage
                newans = CORRUPT.to_bytes(4, "little")
                gdb.selected_inferior().write_memory(begin + 5, newans)
                after = gdb.selected_inferior().read_memory(begin, size).tobytes()
                LOG.write(f"  corrupted answer -> {after.hex()}\n")
                LOG.flush()
            elif MODE == "drop":
                # skip the entire send call: return immediately
                rsp = int(gdb.parse_and_eval("$rsp"))
                ret = u64(rsp)
                gdb.execute(f"set $rip = {ret}")
                gdb.execute(f"set $rsp = {ret:#x} + 8")
                gdb.execute("set $rax = 0")
                LOG.write("  send skipped (dropped)\n")
                LOG.flush()
            # keep the breakpoint for subsequent hits
            return False
        except Exception as e:
            LOG.write(f"ERR {e}\n")
            LOG.flush()
        return False

SendBp(f"*{BASE + 0x631BD58}", internal=False)
print("anarchy bp set")
end

continue
