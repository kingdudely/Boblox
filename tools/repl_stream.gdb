# repl_stream.gdb — log streaming-spawn (0x02) + Type-35 update wire bytes.
#
# Breaks at sub_34956D2 (streaming spawn, inner 0x02) entry: logs the
# flag at state+15562 (conditional pad byte) + first 96 stream bytes.
# Breaks at sub_3511D26 (inner 0x23/Type-35) entry: logs 16 stream bytes.
# Continues always; outer timeout ends the session. Output:
#   run/repl_stream.log  (SPAWN ... / T35 ... lines)
set pagination off
set confirm off

python
import gdb, os

ROOT = os.environ.get("RBX_ROOT", os.getcwd())
RUN = os.environ.get("CCAP_DIR", os.path.join(ROOT, "run"))
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "repl_stream.log"), "a", buffering=1)

def rd(a, n):
    try:
        return gdb.selected_inferior().read_memory(a, n).tobytes()
    except Exception:
        return b""

def u64(a):
    return int.from_bytes(rd(a, 8), "little")

def u32(a):
    return int.from_bytes(rd(a, 4), "little")

pid = gdb.selected_inferior().pid
BASE = None
for line in open(f"/proc/{pid}/maps"):
    if "libroblox.so" in line and line.split()[2] == "00000000":
        BASE = int(line.split("-")[0], 16)
        break
LOG.write(f"BASE={hex(BASE)}\n")
LOG.flush()

class SpawnBp(gdb.Breakpoint):
    def stop(self):
        try:
            rsi = int(gdb.parse_and_eval("$rsi"))
            rdx = int(gdb.parse_and_eval("$rdx"))
            flag = rd(rsi + 15562, 1).hex()
            base = u64(rdx + 16)
            pos = u32(rdx + 56)
            blob = rd(base + pos, 96).hex(" ")
            LOG.write(f"SPAWN flag15562={flag} pos={pos} bytes={blob}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"SERR {e}\n")
        return False

class T35Bp(gdb.Breakpoint):
    def stop(self):
        try:
            rdx = int(gdb.parse_and_eval("$rdx"))
            base = u64(rdx + 16)
            pos = u32(rdx + 56)
            blob = rd(base + pos, 24).hex(" ")
            LOG.write(f"T35 pos={pos} bytes={blob}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"TERR {e}\n")
        return False

SpawnBp(f"*{BASE + 0x34956D2}", internal=False)
T35Bp(f"*{BASE + 0x3511D26}", internal=False)
print("stream bps set")
end

continue
