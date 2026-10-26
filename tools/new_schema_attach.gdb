set pagination off
set confirm off

python
import gdb, os, struct, time

import os as _os; ROOT = _os.environ.get("RBX_ROOT", _os.getcwd())
RUN = os.path.join(ROOT, "run", "schema_cap")
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "index.txt"), "a", buffering=1)

def u64(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 8).tobytes(), "little")
def rd(a, n):
    try: return gdb.selected_inferior().read_memory(a, n).tobytes()
    except Exception: return b""

STATE = {"recv": 0, "prop_done": False}

class RecvBp(gdb.Breakpoint):
    """RbxTransport processPayload(conn, stream, data=rdx, size=rcx)."""
    def stop(self):
        try:
            data = int(gdb.parse_and_eval("$rdx"))
            size = int(gdb.parse_and_eval("$rcx"))
            if not (0 < size <= 2000000) or not (0x1000 < data < 0x800000000000):
                return False
            blob = rd(data, size)
            if len(blob) != size:
                return False
            n = STATE["recv"]; STATE["recv"] = n + 1
            open(os.path.join(RUN, f"r{n:04d}_s{size}.bin"), "wb").write(blob)
            LOG.write(f"recv {n:04d} size={size} head={blob[:16].hex()}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"ERR recv {e}\n"); LOG.flush()
        return False

class PropBp(gdb.Breakpoint):
    """sub_64CC280 (0x03 prop item): ctx=rsi, schema=*(ctx+12480)."""
    def stop(self):
        if STATE["prop_done"]:
            return False
        try:
            ctx = int(gdb.parse_and_eval("$rsi"))
            obj = u64(ctx + 12480)
            pbase = u64(obj + 208)
            pend = u64(obj + 216)
            pcount = (pend - pbase) // 32
            cbase = u64(obj + 184)
            cend = u64(obj + 192)
            stride = 224
            ccount = (cend - cbase) // stride
            LOG.write(f"prop ctx={hex(ctx)} obj={hex(obj)} pcount={pcount} "
                      f"cbase={hex(cbase)} cend={hex(cend)} ccount={ccount}\n")
            LOG.flush()
            if not (1000 < pcount < 200000 and 100 < ccount < 10000):
                return False
            raw = rd(pbase, pcount * 32)
            open(os.path.join(RUN, "proptable_raw.bin"), "wb").write(raw)
            craw = rd(cbase, ccount * stride)
            open(os.path.join(RUN, "classtable_raw.bin"), "wb").write(craw)
            open(os.path.join(RUN, "schema_obj.bin"), "wb").write(rd(obj, 512))
            ptrs = bytearray()
            for i in range(pcount):
                p = struct.unpack_from("<Q", raw, i * 32 + 24)[0]
                if 0x1000 < p < 0x800000000000:
                    ptrs += rd(p, 64)
                else:
                    ptrs += b"\0" * 64
            open(os.path.join(RUN, "prop_names.bin"), "wb").write(bytes(ptrs))
            # Retained serialized schema? (old prefix: v3/flags15/602 enums)
            needle = bytes.fromhex("030f00000000000000da04124163636573734d6f64696669657254797065")
            found = []
            pid = gdb.selected_inferior().pid
            for line in open(f"/proc/{pid}/maps"):
                parts = line.split()
                if len(parts) < 2 or "r" not in parts[1]:
                    continue
                a, b = (int(x, 16) for x in parts[0].split("-"))
                if b - a > 256 * 1024 * 1024:
                    continue
                try:
                    mem = gdb.selected_inferior().read_memory(a, b - a).tobytes()
                except Exception:
                    continue
                off = mem.find(needle)
                if off >= 0:
                    found.append(a + off)
                    LOG.write(f"schema-buffer candidate {hex(a+off)}\n")
                    LOG.flush()
            for j, a in enumerate(found[:4]):
                open(os.path.join(RUN, f"schema_buffer_{j}_{hex(a)}.bin"), "wb").write(rd(a, 600000))
            LOG.write(f"schema-buffer candidates={len(found)}\n")
            LOG.flush()
            STATE["prop_done"] = True
        except Exception as e:
            LOG.write(f"ERR prop {e}\n"); LOG.flush()
        if STATE["prop_done"]:
            try: gdb.execute("detach")
            except Exception: pass
            try: gdb.execute("quit")
            except Exception: pass
            return True
        return False

pid = gdb.selected_inferior().pid
BASE = None
for _ in range(400):
    try:
        lines = open(f"/proc/{pid}/maps").read().splitlines()
    except Exception:
        lines = []
    for line in lines:
        if "libroblox.so" not in line:
            continue
        parts = line.split()
        if len(parts) > 2 and parts[2] == "00000000":
            BASE = int(parts[0].split("-")[0], 16)
            break
    if BASE is not None:
        break
    time.sleep(0.1)
print(f"BASE={hex(BASE) if BASE is not None else None}")
LOG.write(f"# BASE={hex(BASE) if BASE is not None else None} pid={pid}\n"); LOG.flush()
if BASE is None:
    raise RuntimeError("libroblox base mapping not found")
RecvBp(f"*{BASE + 0x340C96C}", internal=False)
PropBp(f"*{BASE + 0x64CC280}", internal=False)
print("breakpoints set")
end

continue
