# tx_frombirth.gdb — TX capture from process birth (catches the join burst).
#
# Unlike tx_capture.gdb (attach to a live PID — always too late for the
# ~3s join burst), this runs the game itself under gdb with
# follow-fork-mode child + catch-load, so the framer breakpoint lands
# before the first send. Passive only (never modifies memory).
#
# Driven by env (export before invoking gdb):
#   TX_LOG   payload log path (required)
#   TX_APP   only log this app id (default: all)
#   TX_CHAN  only log this channel (default: all)
#   TX_MAX   detach after this many payloads (default 800)
#   TX_IDLE  1 keeps idle A6 count=0 frames (default 0 = skip)
#
# Usage (via a wrapper that keeps secrets out of cmdlines):
#   gdb --batch -x tools/tx_frombirth.gdb --args ./roblox --lib ... --place-id N
set pagination off
set confirm off
set follow-fork-mode child
set python print-stack full

python
import gdb, os

log_path = os.environ.get("TX_LOG")
assert log_path, "TX_LOG required"
max_n = int(os.environ.get("TX_MAX", "800"))
only_chan = os.environ.get("TX_CHAN", "")
only_chan = int(only_chan) if only_chan != "" else None
only_app = os.environ.get("TX_APP", "")
only_app = int(only_app) if only_app != "" else None
skip_idle = os.environ.get("TX_IDLE", "0") != "1"

FRAMER_OFF = 0x631BD58

def u64(a):
    return int.from_bytes(gdb.selected_inferior().read_memory(a, 8).tobytes(), "little")

def u8(a):
    return int.from_bytes(gdb.selected_inferior().read_memory(a, 1).tobytes(), "little")

state = {"n": 0, "log": open(log_path, "a", buffering=1), "armed": set()}

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
                return False
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

def on_load():
    pid = gdb.selected_inferior().pid
    if pid in state["armed"]:
        return
    base = None
    for line in open(f"/proc/{pid}/maps"):
        parts = line.split()
        if len(parts) >= 6 and "libroblox.so" in parts[5] and parts[2] == "00000000":
            base = int(parts[0].split("-")[0], 16)
            break
    if base is None:
        print(f"catch-load fired in {pid} but no libroblox base yet", flush=True)
        return
    addr = base + FRAMER_OFF
    TxBp(f"*{addr}", internal=False)
    state["armed"].add(pid)
    print(f"armed framer={addr:#x} in {pid}", flush=True)

gdb.execute("catch load libroblox.so", to_string=True)
gdb.execute("commands\nsilent\npython on_load()\ncontinue\nend", to_string=True)
print("from-birth armed, running", flush=True)
gdb.execute("run", to_string=True)
gdb.execute("continue", to_string=True)
