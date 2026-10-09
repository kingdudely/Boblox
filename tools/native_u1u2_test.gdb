set pagination off
set confirm off

python
import gdb, os, time

import os as _os; ROOT = _os.environ.get("RBX_ROOT", _os.getcwd())
LOG = open(os.path.join(ROOT, "run/u1u2test.log"), "a", buffering=1)

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

STATE = {"patched": False}

class MsgBp(gdb.Breakpoint):
    """Patch native challenge u1/u2 with garbage at dispatcher entry."""
    def stop(self):
        try:
            rsi = int(gdb.parse_and_eval("$rsi"))
            data_ptr = u64(rsi + 8)
            if not (0x1000 < data_ptr < 0x800000000000):
                return False
            if u8(data_ptr) != 0x9b:
                return False
            u1 = u32(data_ptr + 1)
            u2 = u32(data_ptr + 5)
            LOG.write(f"native challenge: u1=0x{u1:08x} u2=0x{u2:08x}\n")
            LOG.flush()
            np1 = u1 ^ 0xDEADBEEF
            np2 = u2 ^ 0xBEEFDEAD
            gdb.selected_inferior().write_memory(data_ptr + 1, np1.to_bytes(4, "little"))
            gdb.selected_inferior().write_memory(data_ptr + 5, np2.to_bytes(4, "little"))
            LOG.write(f"patched to u1=0x{np1:08x} u2=0x{np2:08x}\n")
            LOG.flush()
            STATE["patched"] = True
        except Exception as e:
            LOG.write(f"MSG ERR {e}\n")
            LOG.flush()
        return False

class SendBp(gdb.Breakpoint):
    """Capture the answer the native computes for the patched challenge; DO NOT kill."""
    def stop(self):
        try:
            if not STATE["patched"]:
                return False
            rdx, rcx, r8 = (int(gdb.parse_and_eval(r)) for r in ("$rdx", "$rcx", "$r8"))
            if (rdx & 0xFF) != 4 or (rcx & 0xFFFFFFFF) != 1:
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
            if size != 9:
                return False
            payload = gdb.selected_inferior().read_memory(begin, size).tobytes()
            if payload[:1] != b"\x9b":
                return False
            LOG.write(f"native answer for patched challenge: {payload.hex()}\n(letting it send; watch acceptance)\n")
            LOG.flush()
            STATE["patched"] = False
        except Exception as e:
            LOG.write(f"SEND ERR {e}\n")
            LOG.flush()
        return False

MsgBp(f"*{BASE + 0x32AC2D6}", internal=False)
SendBp(f"*{BASE + 0x631BD58}", internal=False)
print("u1u2 test bp set")
end

continue
