set pagination off
set confirm off

python
import gdb, os, time

import os as _os; ROOT = _os.environ.get("RBX_ROOT", _os.getcwd())
RUN = os.path.join(ROOT, "run", "luau")
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "dump.log"), "a", buffering=1)

def u8(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 1).tobytes(), "little")
def u32(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 4).tobytes(), "little")
def u64(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 8).tobytes(), "little")

def read_cxx_shortstr(addr, maxlen=4096):
    """libc++ std::string: byte0 flag; if flag&1: size=*(addr+8), ptr=*(addr+16) else size=flag>>1, ptr=addr+1."""
    b0 = u8(addr)
    if b0 & 1:
        size = u64(addr + 8)
        ptr = u64(addr + 16)
    else:
        size = b0 >> 1
        ptr = addr + 1
    if size <= 0 or size > maxlen:
        return b""
    try:
        return gdb.selected_inferior().read_memory(ptr, size).tobytes()
    except Exception:
        return b""

pid = gdb.selected_inferior().pid
BASE = None
for line in open(f"/proc/{pid}/maps"):
    if "libroblox.so" in line and line.split()[2] == "00000000":
        BASE = int(line.split("-")[0], 16)
        break
print(f"BASE={hex(BASE)}")

STATE = {"n": 0}

class TransformBp(gdb.Breakpoint):
    """sub_2706206(&out_pair, &blob_string): dump input blob, then dump outputs at return."""
    def stop(self):
        try:
            rdi = int(gdb.parse_and_eval("$rdi"))   # &s (output pair, 2 x cxx string?)
            rsi = int(gdb.parse_and_eval("$rsi"))   # &blob_string
            inp = read_cxx_shortstr(rsi, 200000)
            idx = STATE["n"]; STATE["n"] += 1
            LOG.write(f"call#{idx} blob_len={len(inp)}\n")
            open(os.path.join(RUN, f"in_{idx}.bin"), "wb").write(inp)
            LOG.flush()
            # set breakpoint at return to dump outputs
            ret = u64(int(gdb.parse_and_eval("$rsp")))
            STATE["ret_addr"] = ret
            STATE["out_addr"] = rdi
            STATE["idx"] = idx
            class RetBp(gdb.Breakpoint):
                def stop(self_inner):
                    try:
                        out = STATE["out_addr"]
                        # out pair: two cxx strings at out+0 and out+16 (guess); dump raw 64 bytes
                        raw = gdb.selected_inferior().read_memory(out, 64).tobytes()
                        open(os.path.join(RUN, f"out_{STATE['idx']}_raw.bin"), "wb").write(raw)
                        s1 = read_cxx_shortstr(out, 200000)
                        s2 = read_cxx_shortstr(out + 16, 200000)
                        open(os.path.join(RUN, f"out_{STATE['idx']}_0.bin"), "wb").write(s1)
                        open(os.path.join(RUN, f"out_{STATE['idx']}_1.bin"), "wb").write(s2)
                        LOG.write(f"  ret: out_raw={raw[:24].hex()} s1={len(s1)} s2={len(s2)}\n")
                        LOG.write(f"  s1 head={s1[:24].hex()}\n  s2 head={s2[:24].hex()}\n")
                        LOG.flush()
                    except Exception as e:
                        LOG.write(f"RET ERR {e}\n")
                    return True   # delete
            RetBp(f"*{ret}", internal=False)
        except Exception as e:
            LOG.write(f"ERR {e}\n")
            LOG.flush()
        return False

TransformBp(f"*{BASE + 0x2706206}", internal=False)
print("transform bp set")
end

continue
