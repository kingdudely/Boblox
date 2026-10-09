set pagination off
set confirm off

python
import gdb, os

import os as _os; ROOT = _os.environ.get("RBX_ROOT", _os.getcwd())
RUN = os.path.join(ROOT, "run", "bc")
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "bc.log"), "a", buffering=1)

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

CNT = {"loader": 0, "exec": 0, "chal": 0}

class LoaderBp(gdb.Breakpoint):
    """Luau bytecode loader sub_679992E(luaState, LoadS* a2):
       a2+48 = name (char*), a2+56 = bytecode ptr, a2+64 = size."""
    def stop(self):
        try:
            rsi = int(gdb.parse_and_eval("$rsi"))
            name_ptr = u64(rsi + 48)
            data_ptr = u64(rsi + 56)
            size = u64(rsi + 64)
            name = rd(name_ptr, 64).split(b"\x00")[0] if 0x1000 < name_ptr < 0x800000000000 else b""
            if not (0x1000 < data_ptr < 0x800000000000):
                return False
            n = CNT["loader"]; CNT["loader"] += 1
            blob = rd(data_ptr, min(size, 400000)) if size and size < 400000 else b""
            open(os.path.join(RUN, f"bc_{n}_s{size}.bin"), "wb").write(blob)
            LOG.write(f"LOADER#{n} name={name!r} size={size} first16={blob[:16].hex()}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"LOADER ERR {e}\n")
            LOG.flush()
        return False

class ExecBp(gdb.Breakpoint):
    """sub_274DF88(a1, a2, a3, a4): challenge VM context setup."""
    def stop(self):
        try:
            rdi, rsi, rdx, rcx = (int(gdb.parse_and_eval(r)) for r in ("$rdi", "$rsi", "$rdx", "$rcx"))
            n = CNT["exec"]; CNT["exec"] += 1
            a2 = rd(rsi, 16) if 0x1000 < rsi < 0x800000000000 else b""
            a3 = rd(rdx, 16) if 0x1000 < rdx < 0x800000000000 else b""
            a4 = rd(rcx, 32) if 0x1000 < rcx < 0x800000000000 else b""
            LOG.write(f"EXEC#{n} a1=0x{rdi:x} a2={a2.hex()} a3={a3.hex()} a4={a4.hex()}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"EXEC ERR {e}\n")
            LOG.flush()
        return False

class ChalBp(gdb.Breakpoint):
    """sub_435E556(a1, a2, a3, a4, a5, a6): challenge run entry; a3 = 64-bit key."""
    def stop(self):
        try:
            rdx = int(gdb.parse_and_eval("$rdx"))
            n = CNT["chal"]; CNT["chal"] += 1
            LOG.write(f"CHALRUN#{n} a3=0x{rdx:x} (u32={rdx & 0xffffffff} u32hi={rdx >> 32})\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"CHAL ERR {e}\n")
            LOG.flush()
        return False

LoaderBp(f"*{BASE + 0x679992E}", internal=False)
ExecBp(f"*{BASE + 0x274DF88}", internal=False)
ChalBp(f"*{BASE + 0x435E556}", internal=False)
print("bc hooks set")
end

continue
