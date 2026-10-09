set pagination off
set confirm off

python
import gdb, os

ROOT = "/home/john/RobloxInBrowser"
RUN = os.path.join(ROOT, "run", "btcap")
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "bt.log"), "a", buffering=1)

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

STATE = {"hit": 0}

class LoaderBp(gdb.Breakpoint):
    """At =challenge load: dump data ptr + backtrace + caller frames."""
    def stop(self):
        try:
            rsi = int(gdb.parse_and_eval("$rsi"))
            name_ptr = u64(rsi + 48)
            data_ptr = u64(rsi + 56)
            size = u64(rsi + 64)
            name = b""
            if 0x1000 < name_ptr < 0x800000000000:
                name = rd(name_ptr, 64).split(b"\x00")[0]
            if name != b"=challenge":
                return False
            STATE["hit"] += 1
            n = STATE["hit"]
            LOG.write(f"LOADER HIT#{n} data=0x{data_ptr:x} size={size}\n")
            # dump a page-aligned region around the bytecode + preceding 4KB (decode buffers!)
            start = (data_ptr - 8192) & ~0xFFF
            region = rd(start, 16384 - (data_ptr - start) + 4096)
            open(os.path.join(RUN, f"region_{n}.bin"), "wb").write(region)
            LOG.write(f"  region dumped: start=0x{start:x} len={len(region)}\n")
            # backtrace
            gdb.execute("set pagination off")
            bt = gdb.execute("bt 40", to_string=True)
            LOG.write("BACKTRACE:\n" + bt + "\n")
            # Also dump caller return addresses on stack (raw)
            rsp = int(gdb.parse_and_eval("$rsp"))
            words = rd(rsp, 8*64)
            ws = [int.from_bytes(words[i*8:(i+1)*8], "little") for i in range(64)]
            LOG.write("stack words: " + ", ".join(hex(w) for w in ws) + "\n")
            LOG.flush()
            if n >= 1:
                gdb.execute("detach")
                gdb.execute("quit")
        except gdb.error as e:
            LOG.write(f"ERR {e}\n")
            LOG.flush()
        return False

LoaderBp(f"*{BASE + 0x679992E}", internal=False)
print("bt bp set")
end

continue
