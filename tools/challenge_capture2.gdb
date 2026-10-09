set pagination off
set confirm off

python
import gdb, os, hashlib

ROOT = "/home/john/RobloxInBrowser"
RUN = os.environ.get("CCAP_DIR", os.path.join(ROOT, "run", "ccap"))
os.makedirs(RUN, exist_ok=True)
LOG = open(os.path.join(RUN, "ccap.log"), "a", buffering=1)

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
print(f"BASE={hex(BASE)}")

CNT = {"chal": 0, "msg": 0, "ans": 0}

class LoaderBp(gdb.Breakpoint):
    """Luau bytecode loader: dump only the exact '=challenge' program."""
    def stop(self):
        try:
            rsi = int(gdb.parse_and_eval("$rsi"))
            name_ptr = u64(rsi + 48)
            data_ptr = u64(rsi + 56)
            size = u64(rsi + 64)
            name = b""
            if 0x1000 < name_ptr < 0x800000000000:
                name = rd(name_ptr, 64).split(b"\x00")[0]
            if name != b"=challenge":
                return False
            if not (0x1000 < data_ptr < 0x800000000000 and 0 < size < 200000):
                return False
            n = CNT["chal"]; CNT["chal"] += 1
            blob = rd(data_ptr, size)
            fn = os.path.join(RUN, f"chal_{n}_s{size}.bin")
            open(fn, "wb").write(blob)
            h = hashlib.md5(blob).hexdigest()
            LOG.write(f"CHALPROG#{n} size={size} md5={h} file={fn}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"LOADER ERR {e}\n")
            LOG.flush()
        return False

class MsgBp(gdb.Breakpoint):
    """Dispatcher: dump raw 0x9B challenge + u1/u2 + blob material."""
    def stop(self):
        try:
            rsi = int(gdb.parse_and_eval("$rsi"))
            data_ptr = u64(rsi + 8)
            if not (0x1000 < data_ptr < 0x800000000000):
                return False
            if u8(data_ptr) != 0x9b:
                return False
            size = u32(rsi + 16)
            if not (0 < size < 300000):
                size = 1962
            msg = rd(data_ptr, size)
            n = CNT["msg"]; CNT["msg"] += 1
            open(os.path.join(RUN, f"wire_chal_{n}.bin"), "wb").write(msg)
            if len(msg) >= 13:
                u1 = int.from_bytes(msg[1:5], "little")
                u2 = int.from_bytes(msg[5:9], "little")
                blen = int.from_bytes(msg[9:13], "little")
                LOG.write(f"WIRECHAL#{n} msgsize={size} u1=0x{u1:08x} u2=0x{u2:08x} bloblen={blen}\n")
            else:
                LOG.write(f"WIRECHAL#{n} msgsize={size}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"MSG ERR {e}\n")
            LOG.flush()
        return False

class SendBp(gdb.Breakpoint):
    """send: dump the 9-byte 0x9B answer."""
    def stop(self):
        try:
            rdx, rcx, r8 = (int(gdb.parse_and_eval(r)) for r in ("$rdx", "$rcx", "$r8"))
            if (rdx & 0xFF) != 4 or (rcx & 0xFFFFFFFF) != 1:
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
            if size != 9:
                return False
            payload = rd(begin, size)
            if payload[:1] != b"\x9b":
                return False
            n = CNT["ans"]; CNT["ans"] += 1
            open(os.path.join(RUN, f"wire_ans_{n}.bin"), "wb").write(payload)
            u2 = int.from_bytes(payload[1:5], "little")
            ans = int.from_bytes(payload[5:9], "little")
            LOG.write(f"WIREANS#{n} u2=0x{u2:08x} ans=0x{ans:08x} hex={payload.hex()}\n")
            LOG.flush()
        except Exception as e:
            LOG.write(f"SEND ERR {e}\n")
            LOG.flush()
        return False

LoaderBp(f"*{BASE + 0x679992E}", internal=False)
MsgBp(f"*{BASE + 0x32AC2D6}", internal=False)
SendBp(f"*{BASE + 0x631BD58}", internal=False)
print("ccap bp set")
end

continue
