set pagination off
set confirm off

python
import gdb, os

import os as _os; ROOT = _os.environ.get("RBX_ROOT", _os.getcwd())
RUN = os.path.join(ROOT, "run")
LOG = open(os.path.join(RUN, "a7cap.log"), "a", buffering=1)

def u8(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 1).tobytes(), "little")
def u64(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 8).tobytes(), "little")

pid = gdb.selected_inferior().pid
BASE = None
for line in open(f"/proc/{pid}/maps"):
    if "libroblox.so" in line and line.split()[2] == "00000000":
        BASE = int(line.split("-")[0], 16)
        break
print(f"BASE={hex(BASE)}")

COUNTER = {"n": 0}

class SendBp(gdb.Breakpoint):
    """capture app=4 chan=1 sends; save A7 (0xa7) and 92 (0x92) immediately."""
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
            t = payload[:1].hex()
            if t == "a7":
                open(os.path.join(RUN, "fresh_a7.bin"), "wb").write(payload)
                LOG.write(f"A7 saved len={size} hex={payload.hex()[:80]}...\n")
                LOG.flush()
            elif t == "92":
                open(os.path.join(RUN, "fresh_92.bin"), "wb").write(payload)
                LOG.write(f"92 saved len={size} hex={payload.hex()}\n")
                LOG.flush()
            elif t == "90":
                open(os.path.join(RUN, "fresh_90.bin"), "wb").write(payload)
                LOG.write(f"90 saved len={size}\n")
                LOG.flush()
            elif t == "9b":
                open(os.path.join(RUN, "fresh_9b.bin"), "wb").write(payload)
                LOG.write(f"9b saved len={size}\n")
                LOG.flush()
        except Exception as e:
            LOG.write(f"ERR {e}\n")
        return False

SendBp(f"*{BASE + 0x631BD58}", internal=False)
print("a7 cap bp set")
end

continue
