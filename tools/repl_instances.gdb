# repl_instances.gdb — log every deserialized join instance from a live
# mocktail engine (libroblox.so) to pin the JoinData record grammar.
#
# Breakpoint at the "Done reading join instance %s offset=%X bytes with
# numOfChildren=%d, indexInParent=%d" call site (0x348d72a): logs
#   INST <index_id> <stream-offset> <numChildren> <indexInParent>
# per instance. No detach logic — the outer timeout ends the session.
# Verifies/falsifies: record boundaries, linkage section, counts.
set pagination off
set confirm off

python
import gdb, os

ROOT = os.environ.get("RBX_ROOT", os.getcwd())
RUN = os.environ.get("CCAP_DIR", os.path.join(ROOT, "run"))
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "repl_instances.log"), "a", buffering=1)

def rd(a, n):
    try:
        return gdb.selected_inferior().read_memory(a, n).tobytes()
    except Exception:
        return b""

def cstr(a, limit=48):
    try:
        raw = gdb.selected_inferior().read_memory(a, limit).tobytes()
        return raw.split(b"\x00")[0].decode("ascii", "replace")
    except Exception:
        return "?"

pid = gdb.selected_inferior().pid
BASE = None
for line in open(f"/proc/{pid}/maps"):
    if "libroblox.so" in line and line.split()[2] == "00000000":
        BASE = int(line.split("-")[0], 16)
        break
LOG.write(f"BASE={hex(BASE)}\n")
LOG.flush()

STATE = {"n": 0}

class InstBp(gdb.Breakpoint):
    def stop(self):
        try:
            rcx = int(gdb.parse_and_eval("$rcx"))
            r8 = int(gdb.parse_and_eval("$r8d"))
            r9 = int(gdb.parse_and_eval("$r9d"))
            rsp = int(gdb.parse_and_eval("$rsp"))
            idx_in_parent = int.from_bytes(rd(rsp, 4), "little")
            name = cstr(rcx)
            LOG.write(f"INST {name} off={r8} nkids={r9} idx={idx_in_parent}\n")
            LOG.flush()
            STATE["n"] += 1
        except Exception as e:
            LOG.write(f"IERR {e}\n")
        return False

InstBp(f"*{BASE + 0x348d72a}", internal=False)
print("repl instances bp set")
end

continue
