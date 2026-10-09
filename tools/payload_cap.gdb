set pagination off
set confirm off

python
import gdb, os

os.makedirs(ROOT + "/run/cap2", exist_ok=True)
LOG = open(ROOT + "/run/cap2/index.txt", "a", buffering=1)
COUNTER = {"n": 0}
LIMIT = 400

def u8(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 1).tobytes(), "little")
def u64(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 8).tobytes(), "little")

class SendBp(gdb.Breakpoint):
    def stop(self):
        try:
            rdx, rcx, r8 = (int(gdb.parse_and_eval(r)) for r in ("$rdx", "$rcx", "$r8"))
            app = rdx & 0xFF
            chan = rcx & 0xFFFFFFFF
            if app not in (4, 6):
                return False
            ns = r8
            flag = u8(ns + 64)
            if flag:
                data_ptr = u64(ns + 32)
                length = u64(ns + 40)
            else:
                data_ptr = u64(ns + 8)
                length = u64(ns + 16) - data_ptr
            consumed = u64(ns + 72)
            begin = data_ptr + consumed
            size = length - consumed if length >= consumed else 0
            if 0 < size <= 66000 and 0x1000 < begin < 0x800000000000:
                try:
                    payload = gdb.selected_inferior().read_memory(begin, size).tobytes()
                except Exception:
                    payload = b""
                n = COUNTER["n"]
                COUNTER["n"] += 1
                fn = f"{ROOT}/run/cap2/msg_{n:04d}_a{app}_c{chan}.bin"
                with open(fn, "wb") as f:
                    f.write(payload)
                if app == 4:
                    first = payload[:1].hex() if payload else ""
                    LOG.write(f"{n:04d} app={app} chan={chan} len={size} consumed={consumed} first={first} file={fn}\n")
            if COUNTER["n"] >= LIMIT:
                LOG.write("=== limit reached, detaching ===\n")
                LOG.flush()
                gdb.execute("detach")
                gdb.execute("quit")
        except gdb.error as e:
            LOG.write(f"ERR {e}\n")
        return False

SendBp("*0x7f39dc300000 + 0x631BD58", internal=False)
print("breakpoint set")
end

continue
