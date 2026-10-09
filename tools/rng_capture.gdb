set pagination off
set confirm off

python
import gdb, os

ROOT = "/home/john/RobloxInBrowser"
RUN = os.path.join(ROOT, "run", "rng2")
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "rng.log"), "a", buffering=1)

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
print(f"BASE={hex(BASE)}")

STATE = {"armed": False, "n": 0, "in_call": False}

class ChalBp(gdb.Breakpoint):
    def stop(self):
        try:
            rsi = int(gdb.parse_and_eval("$rsi"))
            data_ptr = u64(rsi + 8)
            if 0x1000 < data_ptr < 0x800000000000 and u8(data_ptr) == 0x9b:
                STATE["armed"] = True
                STATE["n"] = 0
                msg = rd(data_ptr, 13)
                if len(msg) >= 13:
                    u1 = int.from_bytes(msg[1:5], "little"); u2 = int.from_bytes(msg[5:9], "little")
                    LOG.write(f"CHALLENGE u1=0x{u1:08x} u2=0x{u2:08x}\n")
                    LOG.flush()
        except Exception:
            pass
        return False

class NextIntBp(gdb.Breakpoint):
    def stop(self):
        try:
            if not STATE["armed"]:
                return False
            rdi, rsi, rdx = (int(gdb.parse_and_eval(r)) for r in ("$rdi", "$rsi", "$rdx"))
            state_in = u64(rdi) if 0x1000 < rdi < 0x800000000000 else 0
            n = STATE["n"]; STATE["n"] += 1
            LOG.write(f"NEXTINT#{n} state_in=0x{state_in:016x} min={rsi & 0xffffffff} max={rdx & 0xffffffff}\n")
            LOG.flush()
            if n >= 400:
                gdb.execute("detach")
                gdb.execute("quit")
        except Exception as e:
            LOG.write(f"ERR {e}\n")
            LOG.flush()
        return False

ChalBp(f"*{BASE + 0x32AC2D6}", internal=False)
NextIntBp(f"*{BASE + 0x2307554}", internal=False)
print("rng bp3 set")
end

continue
