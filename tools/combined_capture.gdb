set pagination off
set confirm off

python
import gdb, os

import os as _os; ROOT = _os.environ.get("RBX_ROOT", _os.getcwd())
RUN = os.environ.get("CCAP_DIR", os.path.join(ROOT, "run", "comb"))
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "comb.log"), "a", buffering=1)

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
LOG.write(f"BASE={hex(BASE)}\n")
LOG.flush()

STATE = {"want": False, "seen": 0, "rng": 0, "armed": False}

class LoaderBp(gdb.Breakpoint):
    def stop(self):
        try:
            rsi = int(gdb.parse_and_eval("$rsi"))
            name_ptr = u64(rsi + 48)
            data_ptr = u64(rsi + 56)
            size = u64(rsi + 64)
            name = b""
            if 0x1000 < name_ptr < 0x800000000000:
                name = rd(name_ptr, 64).split(b"\x00")[0]
            if name == b"=challenge" and 0x1000 < data_ptr < 0x800000000000:
                STATE["want"] = True
                STATE["seen"] = 0
                open(os.path.join(RUN, "wire.bin"), "wb").write(rd(data_ptr, size))
                LOG.write(f"LOADER =challenge size={size}\n")
                LOG.flush()
        except Exception as e:
            LOG.write(f"LERR {e}\n")
        return False

class RemapEndBp(gdb.Breakpoint):
    def stop(self):
        try:
            if not STATE["want"]:
                return False
            r12 = int(gdb.parse_and_eval("$r12"))
            if not (0x1000 < r12 < 0x800000000000):
                return False
            code_ptr = u64(r12 + 0x68)
            sizecode = u32(r12 + 0xA0)
            if not (0x1000 < code_ptr < 0x800000000000) or not (0 < sizecode < 100000):
                return False
            n = STATE["seen"]; STATE["seen"] += 1
            code = rd(code_ptr, 4 * sizecode)
            open(os.path.join(RUN, f"proto_{n}_s{sizecode}.hex"), "w").write(code.hex())
            LOG.write(f"PROTO#{n} sizecode={sizecode}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"RERR {e}\n")
        return False

class MsgBp(gdb.Breakpoint):
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
            u1 = int.from_bytes(msg[1:5], "little"); u2 = int.from_bytes(msg[5:9], "little")
            LOG.write(f"WIRECHAL u1=0x{u1:08x} u2=0x{u2:08x} msgsize={size}\n")
            LOG.flush()
            STATE["armed"] = True
        except Exception as e:
            LOG.write(f"MERR {e}\n")
        return False

class RngBp(gdb.Breakpoint):
    def stop(self):
        try:
            if not STATE["armed"]:
                return False
            rdi = int(gdb.parse_and_eval("$rdi"))
            state_in = u64(rdi) if 0x1000 < rdi < 0x800000000000 else 0
            rsi = int(gdb.parse_and_eval("$rsi"))
            rdx = int(gdb.parse_and_eval("$rdx"))
            n = STATE["rng"]; STATE["rng"] += 1
            LOG.write(f"NEXTINT#{n} state_in=0x{state_in:016x} min={rsi & 0xffffffff} max={rdx & 0xffffffff}\n")
            LOG.flush()
        except Exception:
            pass
        return False

class SendBp(gdb.Breakpoint):
    def stop(self):
        try:
            rdx, rcx, r8 = (int(gdb.parse_and_eval(r)) for r in ("$rdx", "$rcx", "$r8"))
            if (rdx & 0xFF) != 4 or (rcx & 0xFFFFFFFF) != 1:
                return False
            ns = r8
            flag = u8(ns + 64)
            if flag:
                data_ptr = u64(ns + 32); length = u64(ns + 40)
            else:
                data_ptr = u64(ns + 8); length = u64(ns + 16) - data_ptr
            consumed = u64(ns + 72)
            begin = data_ptr + consumed
            size = length - consumed if length >= consumed else 0
            if size != 9:
                return False
            payload = rd(begin, size)
            if payload[:1] != b"\x9b":
                return False
            open(os.path.join(RUN, "wire_ans.bin"), "wb").write(payload)
            u2 = int.from_bytes(payload[1:5], "little"); ans = int.from_bytes(payload[5:9], "little")
            LOG.write(f"WIREANS u2=0x{u2:08x} ans=0x{ans:08x}\n")
            LOG.flush()
            gdb.execute("detach")
            gdb.execute("quit")
        except Exception as e:
            LOG.write(f"SERR {e}\n")
        return False

LoaderBp(f"*{BASE + 0x679992E}", internal=False)
RemapEndBp(f"*{BASE + 0x679A22D}", internal=False)
MsgBp(f"*{BASE + 0x32AC2D6}", internal=False)
RngBp(f"*{BASE + 0x2307554}", internal=False)
SendBp(f"*{BASE + 0x631BD58}", internal=False)
print("comb bp set")
end

continue
