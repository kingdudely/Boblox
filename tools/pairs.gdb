set pagination off
set confirm off

python
import gdb, os, struct

os.makedirs("/home/john/RobloxInBrowser/run/pairs", exist_ok=True)
LOG = open("/home/john/RobloxInBrowser/run/pairs/index.txt", "a", buffering=1)

def u8(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 1).tobytes(), "little")
def u32(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 4).tobytes(), "little")
def u64(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 8).tobytes(), "little")

pid = gdb.selected_inferior().pid
BASE = None
for line in open(f"/proc/{pid}/maps"):
    if "libroblox.so" in line and line.split()[2] == "00000000":
        BASE = int(line.split("-")[0], 16)
        break
print(f"BASE={hex(BASE)}")

COUNTER = {"ch": 0, "ans": 0, "gen": 0}

class ChalBp(gdb.Breakpoint):
    def stop(self):
        try:
            rsi = int(gdb.parse_and_eval("$rsi"))
            data_ptr = u64(rsi + 8)
            size = u32(rsi + 16)
            if not (0x1000 < data_ptr < 0x800000000000) or not (9 < size < 300000):
                return False
            if u8(data_ptr) != 0x9b:
                return False
            blob = gdb.selected_inferior().read_memory(data_ptr, size).tobytes()
            n = COUNTER["ch"]; COUNTER["ch"] += 1
            fn = f"/home/john/RobloxInBrowser/run/pairs/chal_{n:03d}.bin"
            open(fn, "wb").write(blob)
            u1 = int.from_bytes(blob[1:5], "little")
            u2 = int.from_bytes(blob[5:9], "little")
            blen = int.from_bytes(blob[9:13], "little")
            LOG.write(f"CHAL {n} size={size} u1=0x{u1:08x} u2=0x{u2:08x} blen={blen}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"CHAL ERR {e}\n")
        return False

class AnsBp(gdb.Breakpoint):
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
            if not (0 < size < 64):
                return False
            payload = gdb.selected_inferior().read_memory(begin, size).tobytes()
            if payload[:1] != b"\x9b":
                return False
            n = COUNTER["ans"]; COUNTER["ans"] += 1
            if len(payload) >= 9:
                a1 = int.from_bytes(payload[1:5], "little")
                a2 = int.from_bytes(payload[5:9], "little")
                LOG.write(f"ANS  {n} size={size} a1=0x{a1:08x} a2=0x{a2:08x} raw={payload.hex()}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"ANS ERR {e}\n")
        return False

class GenEntry(gdb.Breakpoint):
    """sub_32AECE2 — the 0x9B sender: dump args + the challenge answer blob state."""
    def stop(self):
        try:
            a1, a2, a3, a4 = (int(gdb.parse_and_eval(r)) for r in ("$rdi", "$rsi", "$rdx", "$rcx"))
            n = COUNTER["gen"]; COUNTER["gen"] += 1
            LOG.write(f"GEN  {n} a1={hex(a1)} u1=0x{a2 & 0xFFFFFFFF:08x} u2=0x{a3 & 0xFFFFFFFF:08x} a4={hex(a4)}\n")
            # a4 points to the challenge result object (v176); dump 64 bytes
            try:
                dump = gdb.selected_inferior().read_memory(a4, 64).tobytes()
                LOG.write(f"     a4[0:64]={dump.hex()}\n")
            except Exception:
                pass
            LOG.flush()
        except Exception as e:
            LOG.write(f"GEN ERR {e}\n")
        return False

ChalBp(f"*{BASE + 0x32AC2D6}", internal=False)
AnsBp(f"*{BASE + 0x631BD58}", internal=False)
GenEntry(f"*{BASE + 0x32AECE2}", internal=False)
print("pair+gen bp set")
end

continue
