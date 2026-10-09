set pagination off
set confirm off

python
import gdb, os

os.makedirs(ROOT + "/run/msgs", exist_ok=True)
LOG = open(ROOT + "/run/msgs/index.txt", "a", buffering=1)

def u64(a):
    return int.from_bytes(gdb.selected_inferior().read_memory(a, 8).tobytes(), "little")
def u32(a):
    return int.from_bytes(gdb.selected_inferior().read_memory(a, 4).tobytes(), "little")

pid = gdb.selected_inferior().pid
BASE = None
for line in open(f"/proc/{pid}/maps"):
    if "libroblox.so" in line and line.split()[2] == "00000000":
        BASE = int(line.split("-")[0], 16)
        break
print(f"BASE={hex(BASE)}")

COUNTER = {"n": 0}

class MsgBp(gdb.Breakpoint):
    """sub_32AC2D6(a1, msg_struct) — msg bytes at *(a2+8), length probe."""
    def stop(self):
        try:
            rsi = int(gdb.parse_and_eval("$rsi"))
            data_ptr = u64(rsi + 8)
            if not (0x1000 < data_ptr < 0x800000000000):
                return False
            # try to find length: dump struct
            try:
                struct_head = gdb.selected_inferior().read_memory(rsi, 64).tobytes()
            except Exception:
                return False
            # guess length from a2+16..24 (docs: size fields often at +16/u32 or +20)
            size1 = int.from_bytes(struct_head[16:20], "little")
            size2 = int.from_bytes(struct_head[20:24], "little")
            size3 = int.from_bytes(struct_head[24:32], "little")
            idx = COUNTER["n"]
            COUNTER["n"] += 1
            # dump generous 65536 bytes from data_ptr
            try:
                blob = gdb.selected_inferior().read_memory(data_ptr, 65536).tobytes()
            except Exception:
                blob = b""
            with open(f"{ROOT}/run/msgs/msg_{idx:04d}.bin", "wb") as f:
                f.write(blob)
            LOG.write(f"{idx:04d} type={blob[:1].hex()} s1={size1} s2={size2} s3={size3} ptr={hex(data_ptr)} sh={struct_head.hex()}\n")
            LOG.flush()
            if COUNTER["n"] >= 150:
                gdb.execute("detach")
                gdb.execute("quit")
        except gdb.error as e:
            LOG.write(f"ERR {e}\n")
        return False

MsgBp(f"*{BASE + 0x32AC2D6}", internal=False)
print("msg bp set")
end

continue
