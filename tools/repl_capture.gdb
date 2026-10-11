# repl_capture.gdb — native JoinData ground truth for the replication RE.
#
# Attaches to a live mocktail engine (libroblox.so), arms two breakpoints:
#   MsgBp  @ stream frame dispatcher (0x32AC2D6): saves the first native
#           0x97 frame (repl_native97.bin) + first big 0x83 (repl_native83.bin)
#   StatsBp @ JoinDataItemV2 stats (0x3489D46): dumps the parsed JoinData
#           struct (repl_native_joinstruct.bin) then detaches.
# Verifies our own capture structurally (same place 1818) and yields the
# native's parsed counts/names to pin the decoder against.
set pagination off
set confirm off

python
import gdb, os

ROOT = os.environ.get("RBX_ROOT", os.getcwd())
RUN = os.environ.get("CCAP_DIR", os.path.join(ROOT, "run"))
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "repl_native.log"), "a", buffering=1)

def u8(a):
    return int.from_bytes(gdb.selected_inferior().read_memory(a, 1).tobytes(), "little")

def u32(a):
    return int.from_bytes(gdb.selected_inferior().read_memory(a, 4).tobytes(), "little")

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
LOG.write(f"BASE={hex(BASE)}\n")
LOG.flush()

STATE = {"got97": False, "got83": False, "got81": False, "got06": False,
         "gotstats": False}

def u64(a):
    return int.from_bytes(gdb.selected_inferior().read_memory(a, 8).tobytes(), "little")

class FrameBp(gdb.Breakpoint):
    def stop(self):
        try:
            rsi = int(gdb.parse_and_eval("$rsi"))
            data_ptr = u64(rsi + 8)
            if not (0x1000 < data_ptr < 0x800000000000):
                return False
            first = u8(data_ptr)
            size = u32(rsi + 16)
            if first == 0x97 and not STATE["got97"] and 0 < size < 300000:
                msg = rd(data_ptr, size)
                if len(msg) == size and msg[:1] == b"\x97":
                    open(os.path.join(RUN, "repl_native97.bin"), "wb").write(msg)
                    LOG.write(f"NATIVE97 size={size} head={msg[:16].hex()}\n")
                    LOG.flush()
                    STATE["got97"] = True
            elif first == 0x81 and not STATE["got81"] and 0 < size < 10000:
                msg = rd(data_ptr, size)
                if len(msg) == size and msg[:1] == b"\x81":
                    open(os.path.join(RUN, "repl_native81.bin"), "wb").write(msg)
                    LOG.write(f"NATIVE81 size={size} head={msg[:24].hex()}\n")
                    LOG.flush()
                    STATE["got81"] = True
            elif first == 0x83 and not STATE["got83"] and size > 1000 and size < 300000:
                msg = rd(data_ptr, size)
                if len(msg) == size and msg[:1] == b"\x83":
                    open(os.path.join(RUN, "repl_native83.bin"), "wb").write(msg)
                    LOG.write(f"NATIVE83 size={size} head={msg[:16].hex()}\n")
                    LOG.flush()
                    STATE["got83"] = True
            elif first == 0x83 and not STATE["got06"] and size > 100 and size < 300000:
                msg = rd(data_ptr, size)
                if len(msg) == size and msg[:2] == b"\x83\x06":
                    open(os.path.join(RUN, "repl_native83_06.bin"), "wb").write(msg)
                    LOG.write(f"NATIVE83_06 size={size} head={msg[:16].hex()}\n")
                    LOG.flush()
                    STATE["got06"] = True
            if STATE["got97"] and STATE["got81"] and STATE["gotstats"]:
                gdb.execute("detach")
                gdb.execute("quit")
        except Exception as e:
            LOG.write(f"FERR {e}\n")
        return False

class StatsBp(gdb.Breakpoint):
    def stop(self):
        try:
            if STATE["gotstats"]:
                return False
            rdi = int(gdb.parse_and_eval("$rdi"))
            blob = rd(rdi, 512)
            open(os.path.join(RUN, "repl_native_joinstruct.bin"), "wb").write(blob)
            LOG.write(f"JOINSTRUCT rdi=0x{rdi:x} u32s={[int.from_bytes(blob[i:i+4],'little') for i in range(0,64,4)]}\n")
            LOG.flush()
            STATE["gotstats"] = True
            if STATE["got97"] and STATE["got81"] and STATE["gotstats"]:
                gdb.execute("detach")
                gdb.execute("quit")
        except Exception as e:
            LOG.write(f"SERR {e}\n")
        return False

FrameBp(f"*{BASE + 0x32AC2D6}", internal=False)
StatsBp(f"*{BASE + 0x3489D46}", internal=False)
print("repl bp set")
end

continue
