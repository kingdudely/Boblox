set pagination off
set confirm off

python
import gdb, os, time

ROOT = "/home/john/RobloxInBrowser"
CHAL_PATH = os.path.join(ROOT, "run/oracle_challenge.bin")
ANS_PATH = os.path.join(ROOT, "run/oracle_answer.txt")
NAT_PATH = os.path.join(ROOT, "run/oracle_native_challenge.bin")
LOG = open(os.path.join(ROOT, "run/oracle_gdb.log"), "a", buffering=1)

def u8(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 1).tobytes(), "little")
def u32(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 4).tobytes(), "little")
def u64(a): return int.from_bytes(gdb.selected_inferior().read_memory(a, 8).tobytes(), "little")

pid = gdb.selected_inferior().pid
BASE = None
for line in open(f"/proc/{pid}/maps"):
    if "libroblox.so" in line and line.split()[2] == "00000000":
        BASE = int(line.split("-")[0], 16)
        break
print(f"BASE={hex(BASE)}")

STATE = {"patched": False}

class MsgBp(gdb.Breakpoint):
    """sub_32AC2D6 dispatcher: native 0x9B challenge arrives here.

    Loop: wait for a challenge file from the Python client whose BLOB matches
    the native's (same server). Discard mismatching files and keep waiting.
    On match: patch u1/u2, let the native compute the answer (SendBp captures).
    """
    def stop(self):
        try:
            rsi = int(gdb.parse_and_eval("$rsi"))
            data_ptr = u64(rsi + 8)
            if not (0x1000 < data_ptr < 0x800000000000):
                return False
            if u8(data_ptr) != 0x9b:
                return False
            LOG.write(f"native challenge msg at {hex(data_ptr)}\n")
            LOG.flush()
            # dump the native's own challenge for reference
            try:
                size = u32(rsi + 16)
                nat_msg = gdb.selected_inferior().read_memory(data_ptr, min(size, 300000)).tobytes()
                open(NAT_PATH, "wb").write(nat_msg)
                nat_blob = nat_msg[13:13+int.from_bytes(nat_msg[9:13], "little")]
            except Exception as e:
                LOG.write(f"native dump err {e}\n")
                nat_blob = None

            deadline = time.time() + 1800
            while time.time() < deadline:
                if os.path.exists(CHAL_PATH):
                    try:
                        our = open(CHAL_PATH, "rb").read()
                    except Exception:
                        our = b""
                    if len(our) >= 13:
                        our_blob = our[13:13+int.from_bytes(our[9:13], "little")]
                        equal = (nat_blob is not None and our_blob == nat_blob)
                        LOG.write(f"blobcheck: ours={len(our_blob)} native={len(nat_blob) if nat_blob else -1} equal={equal}\n")
                        LOG.flush()
                        if not equal:
                            # wrong server; discard and keep waiting for a matching one
                            try:
                                os.remove(CHAL_PATH)
                            except OSError:
                                pass
                            continue
                        u1p = int.from_bytes(our[1:5], "little")
                        u2p = int.from_bytes(our[5:9], "little")
                        nat_u1 = u32(data_ptr + 1)
                        nat_u2 = u32(data_ptr + 5)
                        LOG.write(f"patching native challenge u1 0x{nat_u1:08x}->0x{u1p:08x}  u2 0x{nat_u2:08x}->0x{u2p:08x}\n")
                        LOG.flush()
                        gdb.selected_inferior().write_memory(data_ptr + 1, u1p.to_bytes(4, "little"))
                        gdb.selected_inferior().write_memory(data_ptr + 5, u2p.to_bytes(4, "little"))
                        STATE["patched"] = True
                        return False
                    else:
                        try:
                            os.remove(CHAL_PATH)
                        except OSError:
                            pass
                time.sleep(0.001)
            LOG.write("gave up waiting for matching challenge\n")
            LOG.flush()
        except gdb.error as e:
            LOG.write(f"MSG ERR {e}\n")
        return False

class SendBp(gdb.Breakpoint):
    """sub_631BD58 send: capture the native's 0x9B answer (app=4 chan=1)."""
    def stop(self):
        try:
            if not STATE["patched"]:
                return False
            rdx, rcx, r8 = (int(gdb.parse_and_eval(r)) for r in ("$rdx", "$rcx", "$r8"))
            app = rdx & 0xFF
            chan = rcx & 0xFFFFFFFF
            if app != 4 or chan != 1:
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
            if not (0 < size < 64):
                return False
            payload = gdb.selected_inferior().read_memory(begin, size).tobytes()
            if payload[:1] != b"\x9b" or len(payload) != 9:
                return False
            with open(ANS_PATH, "w") as f:
                f.write(payload.hex())
            LOG.write(f"captured answer {payload.hex()} -> {ANS_PATH}\n")
            LOG.flush()
            STATE["patched"] = False
            # Kill the native BEFORE its (now wrong-for-its-own-session) answer
            # reaches its server; avoids the kick dialog.
            LOG.write("killing native before wire send\n")
            LOG.flush()
            try:
                gdb.execute("kill")
            except Exception as e:
                LOG.write(f"kill err {e}\n")
            return True
        except gdb.error as e:
            LOG.write(f"SEND ERR {e}\n")
        return False

MsgBp(f"*{BASE + 0x32AC2D6}", internal=False)
SendBp(f"*{BASE + 0x631BD58}", internal=False)
print("oracle bp set")
end

continue
