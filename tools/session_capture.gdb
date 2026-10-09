set pagination off
set confirm off

python
import gdb, os, time

ROOT = "/home/john/RobloxInBrowser"
RUN = os.environ.get("SCAP_DIR", os.path.join(ROOT, "run", "sessioncap"))
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "index.txt"), "a", buffering=1)
T0 = time.monotonic()

def u8(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 1).tobytes(), "little")
def u32(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 4).tobytes(), "little")
def u64(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 8).tobytes(), "little")
def rd(a, n):
    try:
        return gdb.selected_inferior().read_memory(a, n).tobytes()
    except Exception:
        return b""

pid = gdb.selected_inferior().pid
BASE = None
for line in open(f"/proc/{pid}/maps"):
    if "libroblox.so" in line and line.split()[2] == "00000000":
        BASE = int(line.split("-")[0], 16)
        break
LOG.write(f"# BASE={hex(BASE)} pid={pid}\n")
LOG.flush()
print(f"BASE={hex(BASE)}")

CNT = {"s": 0, "r": 0}
DEADLINE = T0 + 150


def maybe_quit():
    if time.monotonic() > DEADLINE:
        LOG.write("# deadline reached, detaching\n"); LOG.flush()
        try:
            gdb.execute("detach")
            gdb.execute("quit")
        except Exception:
            pass


def dump(dirch, idx, app, chan, payload):
    fn = os.path.join(RUN, f"{dirch}{idx:04d}_a{app}_c{chan}_s{len(payload)}.bin")
    open(fn, "wb").write(payload)
    LOG.write(f"{time.monotonic() - T0:9.3f} {dirch}{idx:04d} app={app} chan={chan} "
              f"size={len(payload)} head={payload[:40].hex()}\n")
    LOG.flush()


class SendBp(gdb.Breakpoint):
    """sub_631BD58 — client send: app=rdx&0xFF, chan=rcx, netbuf=r8 (validated)."""
    def stop(self):
        try:
            rdx, rcx, r8 = (int(gdb.parse_and_eval(r)) for r in ("$rdx", "$rcx", "$r8"))
            app = rdx & 0xFF
            chan = rcx & 0xFFFFFFFF
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
            if 0 < size < 66000:
                payload = rd(begin, size)
                idx = CNT["s"]; CNT["s"] += 1
                dump("s", idx, app, chan, payload)
        except Exception as e:
            LOG.write(f"SENDERR {e}\n"); LOG.flush()
        maybe_quit()
        return False


class RecvBp(gdb.Breakpoint):
    """sub_632127A — generic receive path: payload=r8, size=rcx (unvalidated;
    identify streams by payload heads)."""
    def stop(self):
        try:
            r8, rcx = int(gdb.parse_and_eval("$r8")), int(gdb.parse_and_eval("$rcx"))
            size = rcx & 0xFFFFFFFF
            if 0 < size <= 300000 and 0x1000 < r8 < 0x800000000000:
                payload = rd(r8, size)
                idx = CNT["r"]; CNT["r"] += 1
                dump("r", idx, 0, 0, payload)
        except Exception as e:
            LOG.write(f"RECVERR {e}\n"); LOG.flush()
        maybe_quit()
        return False


class MsgBp(gdb.Breakpoint):
    """0x32AC2D6 — challenge message dispatch (rsi=struct: data=+8, size=+16)."""
    def stop(self):
        try:
            rsi = int(gdb.parse_and_eval("$rsi"))
            data_ptr = u64(rsi + 8)
            if not (0x1000 < data_ptr < 0x800000000000) or u8(data_ptr) != 0x9b:
                return False
            size = u32(rsi + 16)
            if not (0 < size < 300000):
                size = 1962
            msg = rd(data_ptr, size)
            open(os.path.join(RUN, "wire_chal.bin"), "wb").write(msg)
            u1 = int.from_bytes(msg[1:5], "little")
            u2 = int.from_bytes(msg[5:9], "little")
            LOG.write(f"{time.monotonic() - T0:9.3f} MSG9B u1=0x{u1:08x} u2=0x{u2:08x} msgsize={size}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"MERR {e}\n"); LOG.flush()
        return False


class LoaderBp(gdb.Breakpoint):
    """0x679992E — challenge program load (rsi+48=name, +56=data, +64=size)."""
    def stop(self):
        try:
            rsi = int(gdb.parse_and_eval("$rsi"))
            name_ptr = u64(rsi + 48)
            data_ptr = u64(rsi + 56)
            size = u64(rsi + 64)
            name = rd(name_ptr, 64).split(b"\x00")[0] if 0x1000 < name_ptr < 0x800000000000 else b""
            if name == b"=challenge" and 0x1000 < data_ptr < 0x800000000000:
                open(os.path.join(RUN, "wire.bin"), "wb").write(rd(data_ptr, size))
                LOG.write(f"{time.monotonic() - T0:9.3f} LOADER =challenge size={size}\n")
                LOG.flush()
        except Exception as e:
            LOG.write(f"LERR {e}\n"); LOG.flush()
        return False


SendBp(f"*{BASE + 0x631BD58}", internal=False)
RecvBp(f"*{BASE + 0x632127A}", internal=False)
MsgBp(f"*{BASE + 0x32AC2D6}", internal=False)
LoaderBp(f"*{BASE + 0x679992E}", internal=False)
print("session capture bps set")
end

continue
