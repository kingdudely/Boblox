set pagination off
set confirm off

python
import gdb, os

os.makedirs(ROOT + "/run/corr", exist_ok=True)
LOG = open(ROOT + "/run/corr/log.txt", "a", buffering=1)

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

SENDS = []
COUNT = {"sent": 0}

class SendBp(gdb.Breakpoint):
    """capture app=4 chan=1 sends (0x92, 0x8a, 0x9b)"""
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
            first = payload[:1].hex()
            if first in ("92", "8a", "9b", "a8", "90"):
                n = COUNT["sent"]; COUNT["sent"] += 1
                fn = f"{ROOT}/run/corr/sent_{n:02d}_{first}.bin"
                open(fn, "wb").write(payload)
                LOG.write(f"SENT {n} type={first} len={size}\n")
                LOG.flush()
        except Exception as e:
            LOG.write(f"SENT ERR {e}\n")
        return False

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
            u1 = int.from_bytes(blob[1:5], "little")
            u2 = int.from_bytes(blob[5:9], "little")
            LOG.write(f"CHAL u1=0x{u1:08x} u2=0x{u2:08x} size={size}\n")
            LOG.flush()
        except Exception:
            pass
        return False

SendBp(f"*{BASE + 0x631BD58}", internal=False)
ChalBp(f"*{BASE + 0x32AC2D6}", internal=False)
print("corr bp set")
end

continue
