set pagination off
set confirm off

python
import gdb, os

import os as _os; ROOT = _os.environ.get("RBX_ROOT", _os.getcwd())
RUN = os.path.join(ROOT, "run", "remap")
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "remap.log"), "a", buffering=1)

def u8(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 1).tobytes(), "little")
def u32(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 4).tobytes(), "little")
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

STATE = {"n": 0, "data_ptr": 0, "size": 0}

class LoaderBp(gdb.Breakpoint):
    def stop(self):
        try:
            rsi = int(gdb.parse_and_eval("$rsi"))
            name_ptr = u64(rsi + 48)
            data_ptr = u64(rsi + 56)
            size = u64(rsi + 64)
            name = b""
            if 0x1000 < name_ptr < 0x800000000000:
                name = rd(name_ptr, 64).split(b"\x00")[0]
            if name != b"=challenge" or not (0x1000 < data_ptr < 0x800000000000):
                return False
            STATE["data_ptr"] = data_ptr
            STATE["size"] = size
            open(os.path.join(RUN, "wire.bin"), "wb").write(rd(data_ptr, size))
            LOG.write(f"LOADER name={name} size={size} data=0x{data_ptr:x}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"LERR {e}\n")
        return False

class LenBp(gdb.Breakpoint):
    """sub_27509DA called once per instruction in the remap loop. Capture all
    registers on the first calls to locate the code-buffer pointer (v112)."""
    def stop(self):
        try:
            if STATE["n"] >= 12:
                return False
            regs = {}
            for r in ("rax","rbx","rcx","rdx","rsi","rdi","rbp","rsp","r8","r9","r10","r11","r12","r13","r14","r15"):
                try:
                    regs[r] = int(gdb.parse_and_eval("$" + r))
                except Exception:
                    pass
            n = STATE["n"]; STATE["n"] += 1
            LOG.write(f"CALL#{n}\n")
            for r, v in regs.items():
                if 0x1000 < v < 0x800000000000:
                    b4 = rd(v, 8)
                    LOG.write(f"  {r}=0x{v:x} bytes={b4.hex()}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"CERR {e}\n")
        return False

class RetBp(gdb.FinishBreakpoint):
    pass

LoaderBp(f"*{BASE + 0x679992E}", internal=False)
LenBp(f"*{BASE + 0x27509DA}", internal=False)
print("remap hooks set")
end

continue
