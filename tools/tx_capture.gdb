# tx_capture.gdb — passive upstream (client->server) TX capture on a live
# mocktail session. Breaks at the reliable send framer (sub_631BD58:
# app=rdx&0xFF, chan=rcx, ns=r8) and logs every upstream payload by
# channel. NEVER modifies memory (passive only — anti-cheat safety rule).
#
# Usage:
#   gdb -p <roblox-pid> -x tools/tx_capture.gdb -batch
# Env:
#   TX_LOG   log path (default run/tx_capture.log)
#   TX_MAX   max payloads to log before detaching (default 500)
#   TX_CHAN  only log this channel decimal (default: all)
#   TX_IDLE  if 1, log heartbeats too; else skip empty 0xA6 count=0 (default 0)
set pagination off
set confirm off
set python print-stack full

python
import gdb, os

pid = gdb.selected_inferior().pid
base = None
for line in open(f"/proc/{pid}/maps"):
    parts = line.split()
    if len(parts) >= 6 and "libroblox.so" in parts[5] and parts[2] == "00000000":
        base = int(parts[0].split("-")[0], 16)
        break
assert base is not None, "libroblox base not found"
addr = base + 0x631BD58
print(f"framer={addr:#x}", flush=True)

root = os.environ.get("RBX_ROOT", os.getcwd())
log_path = os.environ.get("TX_LOG", os.path.join(root, "run", "tx_capture.log"))
max_n = int(os.environ.get("TX_MAX", "500"))
only_chan = os.environ.get("TX_CHAN", "")
only_chan = int(only_chan) if only_chan != "" else None
only_app = os.environ.get("TX_APP", "")
only_app = int(only_app) if only_app != "" else None
skip_idle = os.environ.get("TX_IDLE", "0") != "1"

def u64(a):
    return int.from_bytes(gdb.selected_inferior().read_memory(a, 8).tobytes(), "little")

def u8(a):
    return int.from_bytes(gdb.selected_inferior().read_memory(a, 1).tobytes(), "little")

state = {"n": 0, "log": open(log_path, "a", buffering=1)}

class TxBp(gdb.Breakpoint):
    def stop(self):
        try:
            f = gdb.selected_frame()
            rdx = int(f.read_register("rdx"))
            rcx = int(f.read_register("rcx")) & 0xFFFFFFFF
            ns = int(f.read_register("r8"))
            app = rdx & 0xFF
            if only_app is not None and app != only_app:
                return False
            if only_chan is not None and rcx != only_chan:
                return False
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
            if not (0 < size < 66000):
                return False
            payload = gdb.selected_inferior().read_memory(begin, size).tobytes()
            if skip_idle and len(payload) == 2 and payload[0] == 0xA6 and payload[1] == 0:
                return False  # idle heartbeat, skip by default
            state["n"] += 1
            state["log"].write(
                f"HIT#{state['n']} app={app} chan={rcx} size={size} head={payload[:32].hex()} full={payload.hex()}\n")
            if state["n"] >= max_n:
                state["log"].write(f"cap {max_n} payloads, detaching\n")
                state["log"].close()
                print(f"cap {max_n}, detaching", flush=True)
                gdb.execute("detach", to_string=True)
                gdb.execute("quit", to_string=True)
        except Exception as e:
            print(f"TxBp error: {e}", flush=True)
        return False

TxBp(f"*{addr}", internal=False)
print(f"armed, logging to {log_path}", flush=True)
gdb.execute("continue", to_string=True)
