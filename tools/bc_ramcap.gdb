set pagination off
set confirm off

python
import gdb, os

ROOT = "/home/john/RobloxInBrowser"
RUN = os.path.join(ROOT, "run", "bcram")
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "bcram.log"), "a", buffering=1)

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

STATE = {"phase": 0, "code_ptr": 0, "code_count": 0, "code_off": 0}

class LoaderBp(gdb.Breakpoint):
    """sub_679992E: dump raw wire code words for =challenge at load time."""
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
            LOG.write(f"LOADER hit name={name} size={size}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"ERR {e}\n")
        return False

# The remap loop in the loader happens right after reading code words.
# We intercept sub_27509DA (the length helper) calls to capture the FIRST length
# decision — but the most reliable: break on writes to the code buffer.
# Simpler: after the loader returns (luau_load wrapper sub_274FFB2), hook the
# parsed Proto via the lua_State. Too complex; instead hook the remap table reads:
# break at 0x679992E+... no. Use a watchpoint on the wire bytecode when remapped?
#
# Practical approach: break at the instruction AFTER the code-words copy loop in
# sub_679992E (after it stored code into p->code=*(v328+104)), which is guarded by
# the remap loop itself. We set a breakpoint that triggers when *v112 reads happen.
# Easiest robust hook: break on sub_27509DA and capture its caller context to find
# p->code pointer for =challenge: the arg is byte_CC2A04[op], scalar only.
#
# So: hook the loop's helper sub_27509DA is useless. Instead, watch where p->code
# is finalized: line 759: *(v328+24) = v108. Set bp on the address right after the
# remap loop and dump [v108, v108+4*sizecode). As the loader is a single function
# with fixed code addresses, use the known offset of line 759 region:
# We'll brute force: set bp at BASE+0x679992E + delta where delta ~ near "759" line.
# From IDA, get the address of the instruction storing *(v328+24)=v108:

base_delta = 0
for ln in open(os.path.join(RUN, "offsets.txt")) if os.path.exists(os.path.join(RUN, "offsets.txt")) else []:
    pass

print("loader bp set; also installing nothing else")
LoaderBp(f"*{BASE + 0x679992E}", internal=False)
end

continue
