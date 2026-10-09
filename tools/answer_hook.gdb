set pagination off
set confirm off

python
import gdb, os

LOG = open(ROOT + "/run/pairs/hook.txt", "a", buffering=1)

def u32(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 4).tobytes(), "little")
def u64(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 8).tobytes(), "little")

pid = gdb.selected_inferior().pid
BASE = None
for line in open(f"/proc/{pid}/maps"):
    if "libroblox.so" in line and line.split()[2] == "00000000":
        BASE = int(line.split("-")[0], 16)
        break
print(f"BASE={hex(BASE)}")

class RetBp(gdb.Breakpoint):
    """catch returns from sub_435D53A by breaking at its return site.
       Instead: break at sub_32AECE2+offset where v17[2] is read (find dynamically)."""
    def stop(self):
        return False

class OutBp(gdb.Breakpoint):
    """sub_435D53A returns a1 (rdi). On finish, a1 = challenge result vector.
       The vector: *a1 = begin, *(a1+8) = end. Entries are 80 bytes each?
       From decompile: v30 = (end-begin)>>4; then reads v17[2] of entry."""
    def stop(self):
        try:
            rdi = int(gdb.parse_and_eval("$rdi"))
            begin = u64(rdi)
            end = u64(rdi + 8)
            n = (end - begin) // 16 if end > begin else 0
            LOG.write(f"OUT begin={hex(begin)} end={hex(end)} n16={n}\n")
            if 0x1000 < begin < 0x800000000000:
                dump = gdb.selected_inferior().read_memory(begin, min(80*4, 320)).tobytes()
                LOG.write(f"  dump: {dump.hex()}\n")
                for i in range(0, min(80*4, 320), 4):
                    LOG.write(f"  +{i}: 0x{u32(begin+i):08x}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"OUT err {e}\n")
        return False

# sub_435D53A return: use finish breakpoint via FinishBreakpoint API
class FinishRet(gdb.FinishBreakpoint):
    def stop(self):
        try:
            rv = int(self.return_value)
            LOG.write(f"FINISH 435D53A rv={hex(rv)}\n")
            begin = u64(rv); end = u64(rv+8)
            LOG.write(f"  begin={hex(begin)} end={hex(end)}\n")
            if 0x1000 < begin < 0x800000000000:
                for i in range(0, 80):
                    v = u32(begin + 4*i)
                    LOG.write(f"  [{i}] 0x{v:08x}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"FINISH err {e}\n")
        return False

class CallBp(gdb.Breakpoint):
    def stop(self):
        try:
            FinishRet()
        except Exception:
            pass
        return False

CallBp(f"*{BASE + 0x435D53A}", internal=False)
OutBp(f"*{BASE + 0x435D892}", internal=False)
print("answer hook set")
end

continue
