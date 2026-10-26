set pagination off
set confirm off

python
import gdb, os, struct, hashlib

import os as _os; ROOT = _os.environ.get("RBX_ROOT", _os.getcwd())
RUN = os.path.join(ROOT, "run")
os.makedirs(RUN, exist_ok=True)
TSV = os.path.join(RUN, "proptable_new.tsv")
RAW = os.path.join(RUN, "proptable_new_raw.bin")
LOG = open(os.path.join(RUN, "proptable_new.log"), "a", buffering=1)

def u64(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 8).tobytes(), "little")
def u16(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 2).tobytes(), "little")

pid = gdb.selected_inferior().pid
BASE = None
for line in open(f"/proc/{pid}/maps"):
    if "libroblox.so" in line and line.split()[2] == "00000000":
        BASE = int(line.split("-")[0], 16)
        break
print(f"BASE={hex(BASE)}")
LOG.write(f"# BASE={hex(BASE)} pid={pid}\n")

class PropBp(gdb.Breakpoint):
    """sub_64CC280 (0x03 prop item): ctx=rsi, prop table=*(ctx+12480)."""
    def stop(self):
        try:
            ctx = int(gdb.parse_and_eval("$rsi"))
            obj = u64(ctx + 12480)
            base = u64(obj + 208)
            end = u64(obj + 216)
            count = (end - base) // 32
            LOG.write(f"hit ctx={hex(ctx)} obj={hex(obj)} base={hex(base)} end={hex(end)} count={count}\n")
            LOG.flush()
            if count < 1000 or count > 200000:
                return False
            raw = gdb.selected_inferior().read_memory(base, count * 32).tobytes()
            open(RAW, "wb").write(raw)
            lines = [
                "# proptable_new.tsv - native replicator prop table, updated client.",
                "# libroblox.so md5 30ecaff9ec5b77a0a0faa7cf17573a3a.",
                f"# ctx+12480 -> obj+208, stride 32, count={count}, tag=entry+9.",
                "# columns: prop_id native_tag",
            ]
            for i in range(count):
                pid_ = struct.unpack_from("<H", raw, i * 32)[0]
                tag = raw[i * 32 + 9]
                lines.append(f"{pid_} {tag}")
            open(TSV, "w").write("\n".join(lines) + "\n")
            LOG.write(f"wrote {TSV} count={count}\n")
            LOG.flush()
            print(f"prop table dumped: count={count}")
        except Exception as e:
            LOG.write(f"ERR {e}\n")
            LOG.flush()
        try:
            gdb.execute("detach")
        except Exception:
            pass
        try:
            gdb.execute("quit")
        except Exception:
            pass
        return True

PropBp(f"*{BASE + 0x64CC280}", internal=False)
print("prop table bp set")
end

continue
