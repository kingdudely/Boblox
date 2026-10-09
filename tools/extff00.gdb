set pagination off
set confirm off

python
import gdb, os, time

import os as _os; ROOT = _os.environ.get("RBX_ROOT", _os.getcwd())
RUN = os.environ.get("QD_DIR", os.path.join(ROOT, "run", "extff00"))
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "index.txt"), "a", buffering=1)
T0 = time.monotonic()
N = {"p": 0, "parse": 0}

def u64(a):
    try:
        return int.from_bytes(gdb.selected_inferior().read_memory(a, 8).tobytes(), "little")
    except Exception:
        return 0

def rd(a, n):
    try:
        return gdb.selected_inferior().read_memory(a, n).tobytes()
    except Exception:
        return b""

def reg(n):
    return int(gdb.parse_and_eval("$" + n))

pid = gdb.selected_inferior().pid
BASE = None
for line in open(f"/proc/{pid}/maps"):
    if "libroblox.so" in line and line.split()[2] == "00000000":
        BASE = int(line.split("-")[0], 16)
        break
LOG.write(f"# BASE={hex(BASE)} pid={pid}\n")
LOG.flush()
print(f"BASE={hex(BASE)}")


class GetterBp(gdb.Breakpoint):
    """0x63810A4: call r13 inside add_cb (sub_638104F).
    r13 = getter fn (add_arg), rdi = ctx (ret of sub_6380586)."""
    def stop(self):
        try:
            r13 = reg("r13"); rdi = reg("rdi")
            vt = u64(rdi) if rdi else 0
            LOG.write(f"{time.monotonic()-T0:9.3f} GETTER={hex(r13)} ctx={hex(rdi)} ctx.vtable={hex(vt)}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"GERR {e}\n"); LOG.flush()
        return False


class PayloadBp(gdb.Breakpoint):
    """0x63810A7: right after getter returns: rax=ptr rdx=len."""
    def stop(self):
        try:
            rax = reg("rax"); rdx = reg("rdx")
            if 0 < rdx < 4096 and 0x1000 < rax < 0x800000000000:
                data = rd(rax, min(int(rdx), 512))
                n = N["p"]; N["p"] += 1
                open(os.path.join(RUN, f"payload{n}.bin"), "wb").write(data)
                LOG.write(f"{time.monotonic()-T0:9.3f} PAYLOAD#{n} len={rdx} {data[:40].hex()}\n")
                LOG.flush()
            bt = gdb.execute("bt 10", to_string=True)
            LOG.write(bt + "\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"PERR {e}\n"); LOG.flush()
        return False


class ParseBp(gdb.Breakpoint):
    """sub_638120C = parse_cb (SSL_custom_ext_parse_cb_ex).
    rsi=ext_type rcx=in r8=inlen -> does the SERVER echo ext 0xFF00?"""
    def stop(self):
        try:
            rsi = reg("rsi") & 0xFFFFFFFF
            rcx = reg("rcx"); r8 = reg("r8") & 0xFFFFFFFF
            n = N["parse"]; N["parse"] += 1
            data = rd(rcx, min(int(r8), 512)) if 0 < r8 < 4096 else b""
            LOG.write(f"{time.monotonic()-T0:9.3f} PARSE#{n} type=0x{rsi:04x} inlen={r8} {data[:40].hex()}\n")
            LOG.flush()
            bt = gdb.execute("bt 8", to_string=True)
            LOG.write(bt + "\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"RERR {e}\n"); LOG.flush()
        return False


GetterBp(f"*{BASE + 0x63810A4}", internal=False)
PayloadBp(f"*{BASE + 0x63810A7}", internal=False)
ParseBp(f"*{BASE + 0x638120C}", internal=False)
end

continue
