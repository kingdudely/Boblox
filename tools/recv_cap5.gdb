set pagination off
set confirm off

python
import gdb, os, time

os.makedirs(ROOT + "/run/recv5", exist_ok=True)
LOG = open(ROOT + "/run/recv5/index.txt", "a", buffering=1)

def u8(a):
    return int.from_bytes(gdb.selected_inferior().read_memory(a, 1).tobytes(), "little")
def u64(a):
    return int.from_bytes(gdb.selected_inferior().read_memory(a, 8).tobytes(), "little")

pid = gdb.selected_inferior().pid
BASE = None
for line in open(f"/proc/{pid}/maps"):
    if "libroblox.so" in line and line.split()[2] == "00000000":
        BASE = int(line.split("-")[0], 16)
        break
print(f"BASE={hex(BASE)}")

COUNTER = {"n": 0}

class RecvBp(gdb.Breakpoint):
    """sub_632127A(conn, stream, netstream_or_data, size, ...)
       Payload likely at rdx (3rd arg) or r8."""
    def stop(self):
        try:
            rdx, rcx, r8, r9 = (int(gdb.parse_and_eval(r)) for r in ("$rdx", "$rcx", "$r8", "$r9"))
            size = rcx & 0xFFFFFFFF
            if not (0 < size <= 300000):
                return False
            idx = COUNTER["n"]
            COUNTER["n"] += 1
            saved = []
            # try rdx and r8 as direct payload pointers
            for lbl, ptr in (("rdx", rdx), ("r8", r8)):
                if not (0x1000 < ptr < 0x800000000000):
                    continue
                try:
                    blob = gdb.selected_inferior().read_memory(ptr, size).tobytes()
                except Exception:
                    continue
                # validate framing: 4-byte varint == size-4, or 1-byte varint == size-1
                v4 = int.from_bytes(blob[0:4], "big")
                ok4 = (v4 & 0xC0000000) == 0x80000000 and (v4 & 0x3FFFFFFF) == size - 4
                ok1 = (blob[0] & 0x3F) == size - 1 and blob[0] < 0x40
                v2 = int.from_bytes(blob[0:2], "big")
                ok2 = (v2 & 0xC000) == 0x4000 and (v2 & 0x3FFF) == size - 2
                if ok4 or ok2 or ok1:
                    fn = f"{ROOT}/run/recv5/msg_{idx:04d}_sz{size}_{lbl}.bin"
                    with open(fn, "wb") as f:
                        f.write(blob)
                    saved.append((lbl, blob[:16].hex()))
                elif size <= 200:  # small message, keep unconditionally for analysis
                    fn = f"{ROOT}/run/recv5/raw_{idx:04d}_sz{size}_{lbl}.bin"
                    with open(fn, "wb") as f:
                        f.write(blob)
                    saved.append((lbl + "_raw", blob[:16].hex()))
            LOG.write(f"{idx:04d} size={size} rdx={hex(rdx)} r8={hex(r8)} saved={saved}\n")
            LOG.flush()
            if COUNTER["n"] >= 120:
                gdb.execute("detach")
                gdb.execute("quit")
        except gdb.error as e:
            LOG.write(f"ERR {e}\n")
        return False

RecvBp(f"*{BASE + 0x632127A}", internal=False)
print("recv bp set")
end

continue
