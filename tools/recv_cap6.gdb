set pagination off
set confirm off

python
import gdb, os

os.makedirs("/home/john/RobloxInBrowser/run/recv6", exist_ok=True)
LOG = open("/home/john/RobloxInBrowser/run/recv6/index.txt", "a", buffering=1)

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
    """sub_632127A(conn, stream, data/r8...) — payload at r8, size at rcx."""
    def stop(self):
        try:
            r8, rcx = int(gdb.parse_and_eval("$r8")), int(gdb.parse_and_eval("$rcx"))
            size = rcx & 0xFFFFFFFF
            if not (0 < size <= 300000):
                return False
            if not (0x1000 < r8 < 0x800000000000):
                return False
            idx = COUNTER["n"]
            COUNTER["n"] += 1
            try:
                blob = gdb.selected_inferior().read_memory(r8, size).tobytes()
            except Exception:
                LOG.write(f"{idx:04d} size={size} r8={hex(r8)} READFAIL\n")
                return False
            fn = f"/home/john/RobloxInBrowser/run/recv6/msg_{idx:04d}_sz{size}.bin"
            with open(fn, "wb") as f:
                f.write(blob)
            LOG.write(f"{idx:04d} size={size} first={blob[:1].hex()} head={blob[:32].hex()}\n")
            LOG.flush()
            if COUNTER["n"] >= 60:
                gdb.execute("detach")
                gdb.execute("quit")
        except gdb.error as e:
            LOG.write(f"ERR {e}\n")
        return False

RecvBp(f"*{BASE + 0x632127A}", internal=False)
print("recv bp set")
end

continue
