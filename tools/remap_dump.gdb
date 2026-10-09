set pagination off
set confirm off

python
import gdb, os

ROOT = "/home/john/RobloxInBrowser"
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

STATE = {"want_challenge": False, "dumped": 0, "seen": 0}

class LoaderBp(gdb.Breakpoint):
    """At =challenge load entry: mark that the next remap loop belongs to it."""
    def stop(self):
        try:
            rsi = int(gdb.parse_and_eval("$rsi"))
            name_ptr = u64(rsi + 48)
            data_ptr = u64(rsi + 56)
            size = u64(rsi + 64)
            name = b""
            if 0x1000 < name_ptr < 0x800000000000:
                name = rd(name_ptr, 64).split(b"\x00")[0]
            if name == b"=challenge" and 0x1000 < data_ptr < 0x800000000000:
                STATE["want_challenge"] = True
                STATE["data_ptr"] = data_ptr
                STATE["size"] = size
                open(os.path.join(RUN, "wire.bin"), "wb").write(rd(data_ptr, size))
                LOG.write(f"LOADER =challenge size={size}\n")
                LOG.flush()
        except Exception as e:
            LOG.write(f"LERR {e}\n")
        return False

class RemapEndBp(gdb.Breakpoint):
    """At the end of the per-proto remap loop (0x679A22D): if this proto belongs
    to the challenge load, dump p->code and p->sizecode.

    r12 = LuaProto* (set at 0x679A1F0 mov r12,[rbp+var_150] and restored after).
    By 0x679A22D r12 still holds the proto being processed."""
    def stop(self):
        try:
            if not STATE["want_challenge"]:
                return False
            r12 = int(gdb.parse_and_eval("$r12"))
            if not (0x1000 < r12 < 0x800000000000):
                return False
            code_ptr = u64(r12 + 0x68)
            sizecode = u32(r12 + 0xA0)
            if not (0x1000 < code_ptr < 0x800000000000) or not (0 < sizecode < 100000):
                return False
            n = STATE["seen"]; STATE["seen"] += 1
            code = rd(code_ptr, 4 * sizecode)
            open(os.path.join(RUN, f"proto_{n}_s{sizecode}.hex"), "w").write(code.hex())
            LOG.write(f"PROTO#{n} sizecode={sizecode} code=0x{code_ptr:x} first_words={code[:16].hex()}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"RERR {e}\n")
        return False

LoaderBp(f"*{BASE + 0x679992E}", internal=False)
RemapEndBp(f"*{BASE + 0x679A22D}", internal=False)
print("remap dump bp set")
end

continue
