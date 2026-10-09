set pagination off
set confirm off

python
import gdb, os

import os as _os; ROOT = _os.environ.get("RBX_ROOT", _os.getcwd())
RUN = os.path.join(ROOT, "run", "trace")
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "trace.log"), "a", buffering=1)

def u8(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 1).tobytes(), "little")
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

# resolve the Lua bytecode PC at each NextInteger call by walking the Lua stack?
# Simpler: hook sub_28119AA (the Lua-facing NextInteger) and capture the Lua
# caller function via the lua_State's ci (call info). For now capture return addr.
LOG.write(f"BASE={hex(BASE)}\n")
LOG.flush()

STATE = {"armed": False, "n": 0}

class ChalBp(gdb.Breakpoint):
    def stop(self):
        try:
            rsi = int(gdb.parse_and_eval("$rsi"))
            data_ptr = u64(rsi + 8)
            if 0x1000 < data_ptr < 0x800000000000 and u8(data_ptr) == 0x9b:
                STATE["armed"] = True
                STATE["n"] = 0
                msg = rd(data_ptr, 13)
                u1 = int.from_bytes(msg[1:5], "little"); u2 = int.from_bytes(msg[5:9], "little")
                LOG.write(f"CHALLENGE u1=0x{u1:08x} u2=0x{u2:08x}\n")
                LOG.flush()
        except Exception:
            pass
        return False

class NextBp(gdb.Breakpoint):
    def stop(self):
        try:
            if not STATE["armed"]:
                return False
            rsp = int(gdb.parse_and_eval("$rsp"))
            retaddr = int.from_bytes(rd(rsp, 8), "little")
            off = retaddr - BASE
            n = STATE["n"]; STATE["n"] += 1
            LOG.write(f"NEXT#{n} ret=0x{retaddr:x} (lib+0x{off:x})\n")
            LOG.flush()
            if n >= 300:
                gdb.execute("detach")
                gdb.execute("quit")
        except Exception as e:
            LOG.write(f"ERR {e}\n")
        return False

ChalBp(f"*{BASE + 0x32AC2D6}", internal=False)
NextBp(f"*{BASE + 0x28119AA}", internal=False)
print("trace bp set")
end

continue
