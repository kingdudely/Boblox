set pagination off
set confirm off

python
import gdb, os, struct, time

ROOT = os.environ.get("RBX_ROOT", os.getcwd())
RUN = os.path.join(ROOT, "run", "cap97")
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "index.txt"), "a", buffering=1)

def u64(a):
    try: return int.from_bytes(gdb.selected_inferior().read_memory(a, 8).tobytes(), "little")
    except Exception: return 0
def rd(a, n):
    try: return gdb.selected_inferior().read_memory(a, n).tobytes()
    except Exception: return b""

STATE = {"done": False, "hits": 0}

def scan_words(tag, region, nwords=64):
    """Scan qwords in a region for pointers to a 0x97 message."""
    for i in range(nwords):
        p = u64(region + i * 8)
        if not (0x1000 < p < 0x800000000000):
            continue
        head = rd(p, 12)
        if len(head) >= 6 and head[0] == 0x97:
            size = struct.unpack_from("<I", head, 5)[0]
            take = size + 9 if 0 < size < 5000000 else 300000
            blob = rd(p, take)
            if blob[:1] == b"\x97":
                fn = os.path.join(RUN, f"new97_{tag}_{i}_{hex(p)}.bin")
                open(fn, "wb").write(blob)
                LOG.write(f"FOUND97 {tag} word{i} ptr={hex(p)} size={len(blob)} head={blob[:16].hex()}\n")
                LOG.flush()
                STATE["done"] = True
                return True
    return False

class EventBp(gdb.Breakpoint):
    """connection event handler sub_63DC94E(client, event=a2)."""
    def stop(self):
        try:
            a2 = int(gdb.parse_and_eval("$rsi"))
            STATE["hits"] += 1
            LOG.write(f"event hit {STATE['hits']} a2={hex(a2)} "
                      f"p56={hex(u64(a2+56))} q64={hex(u64(a2+64))} "
                      f"q72={hex(u64(a2+72))}\n")
            LOG.flush()
            if scan_words("ev", a2, 96):
                return True
            data = u64(a2 + 56)
            if 0x1000 < data < 0x800000000000:
                head = rd(data, 32)
                LOG.write(f"  data={head.hex()}\n"); LOG.flush()
            rsp = int(gdb.parse_and_eval("$rsp"))
            if scan_words("stack", rsp, 1024):
                return True
        except Exception as e:
            LOG.write(f"ERR ev {e}\n"); LOG.flush()
        return False

class HandlerBp(gdb.Breakpoint):
    """join broker sub_63D8F36(client, join=a2)."""
    def stop(self):
        try:
            a2 = int(gdb.parse_and_eval("$rsi"))
            LOG.write(f"handler hit a2={hex(a2)}\n"); LOG.flush()
            if scan_words("join", a2, 128):
                return True
            rsp = int(gdb.parse_and_eval("$rsp"))
            if scan_words("jstack", rsp, 2048):
                return True
        except Exception as e:
            LOG.write(f"ERR join {e}\n"); LOG.flush()
        return False

class DoneBp(gdb.Breakpoint):
    """prop item: if no 0x97 yet, give up and detach."""
    def stop(self):
        LOG.write("prop hit (no 0x97)\n"); LOG.flush()
        STATE["done"] = True
        return True

pid = gdb.selected_inferior().pid
BASE = None
for _ in range(500):
    try: lines = open(f"/proc/{pid}/maps").read().splitlines()
    except Exception: lines = []
    for line in lines:
        if "libroblox.so" in line:
            parts = line.split()
            if len(parts) > 2 and parts[2] == "00000000":
                BASE = int(parts[0].split("-")[0], 16); break
    if BASE is not None: break
    time.sleep(0.1)
print("BASE", hex(BASE) if BASE else None)
LOG.write(f"# BASE={hex(BASE) if BASE else None}\n"); LOG.flush()
if BASE is None:
    raise RuntimeError("no base")
EventBp(f"*{BASE + 0x63DC94E}", internal=False)
HandlerBp(f"*{BASE + 0x63D8F36}", internal=False)
DoneBp(f"*{BASE + 0x64CC280}", internal=False)
print("breakpoints set")
end

continue
