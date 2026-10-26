set pagination off
set confirm off

python
import gdb, os, struct

import os as _os; ROOT = _os.environ.get("RBX_ROOT", _os.getcwd())
RUN = os.path.join(ROOT, "run", "newcap")
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "index.txt"), "a", buffering=1)
PT_TSV = os.path.join(ROOT, "run", "proptable_new.tsv")
PT_RAW = os.path.join(ROOT, "run", "proptable_new_raw.bin")

def u8(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 1).tobytes(), "little")
def u16(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 2).tobytes(), "little")
def u64(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 8).tobytes(), "little")

pid = gdb.selected_inferior().pid
BASE = None
starts = []
for line in open(f"/proc/{pid}/maps"):
    if "libroblox.so" not in line:
        continue
    parts = line.split()
    start = int(parts[0].split("-")[0], 16)
    starts.append(start)
    if len(parts) > 2 and parts[2] == "00000000":
        BASE = start
        break
if BASE is None and starts:
    BASE = min(starts) & ~0xFFF
print(f"BASE={hex(BASE) if BASE is not None else None}")
LOG.write(f"# BASE={hex(BASE) if BASE is not None else None} pid={pid}\n")
if BASE is None:
    raise RuntimeError("libroblox base mapping not found")

COUNTER = {"n": 0}

class SendBp(gdb.Breakpoint):
    """new-build send framer sub_3407364: app=a3, chan=a4, ns=a5."""
    def stop(self):
        try:
            rdx, rcx, r8 = (int(gdb.parse_and_eval(r)) for r in ("$rdx", "$rcx", "$r8"))
            app = rdx & 0xFF
            chan = rcx & 0xFFFFFFFF
            if app not in (4, 6) or chan not in (1, 3):
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
            if not (0 < size < 66000):
                return False
            payload = gdb.selected_inferior().read_memory(begin, size).tobytes()
            t = payload[:1].hex()
            idx = COUNTER["n"]; COUNTER["n"] += 1
            fn = os.path.join(RUN, f"m{idx:02d}_a{app}_c{chan}_t{t}_s{size}.bin")
            open(fn, "wb").write(payload)
            LOG.write(f"{idx:02d} app={app} chan={chan} type={t} size={size} file={os.path.basename(fn)}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"ERR send {e}\n")
        return False

class PropBp(gdb.Breakpoint):
    """sub_64CC280 (0x03 prop item): ctx=rsi, prop table=*(ctx+12480)."""
    def __init__(self, spec):
        super().__init__(spec, internal=False)
        self.done = False
    def stop(self):
        if self.done:
            return False
        try:
            ctx = int(gdb.parse_and_eval("$rsi"))
            obj = u64(ctx + 12480)
            base = u64(obj + 208)
            end = u64(obj + 216)
            count = (end - base) // 32
            LOG.write(f"proptable hit ctx={hex(ctx)} obj={hex(obj)} count={count}\n")
            LOG.flush()
            if count < 1000 or count > 200000:
                return False
            raw = gdb.selected_inferior().read_memory(base, count * 32).tobytes()
            open(PT_RAW, "wb").write(raw)
            lines = [
                "# proptable_new.tsv - native replicator prop table, updated client.",
                "# libroblox.so md5 30ecaff9ec5b77a0a0faa7cf17573a3a.",
                f"# ctx+12480 -> obj+208, stride 32, count={count}, tag=entry+9.",
                "# columns: prop_id native_tag",
            ]
            for i in range(count):
                prop = struct.unpack_from("<H", raw, i * 32)[0]
                tag = raw[i * 32 + 9]
                lines.append(f"{prop} {tag}")
            open(PT_TSV, "w").write("\n".join(lines) + "\n")
            LOG.write(f"proptable wrote {PT_TSV} count={count}\n")
            LOG.flush()
            print(f"prop table dumped: count={count}")
            self.done = True
        except Exception as e:
            LOG.write(f"ERR proptable {e}\n")
            LOG.flush()
        return False

SendBp(f"*{BASE + 0x3407364}", internal=False)
PropBp(f"*{BASE + 0x64CC280}")
print("new-build TX + proptable bps set")
end

continue
