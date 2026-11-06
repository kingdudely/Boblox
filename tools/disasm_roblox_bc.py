#!/usr/bin/env python3
"""disasm_roblox_bc.py — bytecode disassembler for the Roblox =challenge program.

Format follows upstream Luau lvmload.cpp for version 13 / typeversion 3.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bc_common import varint  # noqa: E402


def read_varint64(buf, off):
    result = 0
    shift = 0
    while True:
        b = buf[off]; off += 1
        result |= (b & 127) << shift
        shift += 7
        if not (b & 128):
            return result, off


def read_string(strings):
    def f(buf, off):
        idx, off2 = varint(buf, off)
        return strings[idx - 1] if idx else None, off2
    return f


class Proto:
    def __init__(self):
        self.maxstacksize = 0
        self.numparams = 0
        self.nups = 0
        self.is_vararg = 0
        self.flags = 0
        self.code = []
        self.k = []
        self.p = []
        self.linedefined = 0
        self.debugname = None
        self.linegaplog2 = 0
        self.lineinfo = []
        self.locvars = []
        self.upvalues = []


def parse(buf, verbose=False):
    off = 0
    version = buf[off]; off += 1
    typesversion = 0
    if version >= 4:
        typesversion = buf[off]; off += 1
    # string table
    n_strings, off = varint(buf, off)
    strings = []
    for _ in range(n_strings):
        ln, off = varint(buf, off)
        strings.append(buf[off:off+ln].decode("latin1"))
        off += ln
    # userdata remapping
    if typesversion == 3:
        idx = buf[off]; off += 1
        while idx != 0:
            # readString
            sidx, off = varint(buf, off)
            idx = buf[off]; off += 1
    proto_count, off = varint(buf, off)
    if verbose:
        print(f"; version={version} tv={typesversion} strings={n_strings} protos={proto_count}")
    protos = []
    for i in range(proto_count):
        p = Proto()
        proto_size = 0
        if version >= 12:
            proto_size, off = varint(buf, off)
        proto_start = off
        p.maxstacksize = buf[off]; off += 1
        p.numparams = buf[off]; off += 1
        p.nups = buf[off]; off += 1
        p.is_vararg = buf[off]; off += 1
        if version >= 4:
            p.flags = buf[off]; off += 1
            if typesversion in (2, 3):
                typesize, off = varint(buf, off)
                off += typesize
            elif typesversion == 1:
                typesize, off = varint(buf, off)
                off += typesize
        sizecode, off = varint(buf, off)
        p.code = list(struct.unpack("<%dI" % sizecode, buf[off:off+4*sizecode]))
        off += 4 * sizecode
        sizek, off = varint(buf, off)
        if verbose:
            print(f"; proto {i} start={proto_start} size={proto_size} flags={p.flags} code={sizecode} k={sizek} off={off}")
        for ki in range(sizek):
            t = buf[off]; off += 1
            if verbose and i == proto_count - 1:
                print(f";   k[{ki}] tag={t} off={off-1}")
            if t == 0:
                p.k.append(("nil",))
            elif t == 1:
                v = buf[off]; off += 1
                p.k.append(("bool", v))
            elif t == 2:
                v = struct.unpack("<d", buf[off:off+8])[0]; off += 8
                p.k.append(("num", v))
            elif t == 3:
                sidx, off = varint(buf, off)
                p.k.append(("str", strings[sidx - 1]))  # string ids are 1-based
            elif t == 4:
                v = struct.unpack("<i", buf[off:off+4])[0]; off += 4
                p.k.append(("import", v))
            elif t == 5:
                nkeys, off = varint(buf, off)
                keys = []
                for _ in range(nkeys):
                    kk, off = varint(buf, off)
                    keys.append(kk)
                p.k.append(("table", keys))
            elif t == 6:
                v, off = varint(buf, off)
                p.k.append(("closure", v))
            elif t == 7:
                vals = struct.unpack("<4f", buf[off:off+16]); off += 16
                p.k.append(("vecf", vals))
            elif t == 8:
                # table with prefilled constants (v7+)
                nkeys, off = varint(buf, off)
                keys = []
                for _ in range(nkeys):
                    kk, off = varint(buf, off)
                    ci = struct.unpack("<i", buf[off:off+4])[0]; off += 4
                    keys.append((kk, ci))
                p.k.append(("tablec", keys))
            elif t == 9:
                # signed 64-bit integer (v8+)
                neg = buf[off]; off += 1
                mag, off = varint(buf, off)
                v = -mag if neg else mag
                p.k.append(("int", v))
            elif t == 10:
                # class shape (v10+)
                cnid, off = varint(buf, off)
                nprop, off = varint(buf, off)
                nmeth, off = varint(buf, off)
                members = []
                for _ in range(nprop + nmeth):
                    mid, off = varint(buf, off)
                    members.append(mid)
                p.k.append(("class", cnid, nprop, nmeth, members))
            elif t == 11:
                vals = struct.unpack("<4d", buf[off:off+32]); off += 32
                p.k.append(("vecd", vals))
            else:
                raise ValueError(f"bad const type {t} at off {off-1}")
        sizep, off = varint(buf, off)
        for _ in range(sizep):
            fid, off = varint(buf, off)
            p.p.append(fid)
        p.linedefined, off = varint(buf, off)
        dnameidx, off = varint(buf, off)
        p.debugname = strings[dnameidx - 1] if dnameidx else None
        lineinfo = buf[off]; off += 1
        if lineinfo:
            p.linegaplog2 = buf[off]; off += 1
            intervals = ((sizecode - 1) >> p.linegaplog2) + 1
            lastoffset = 0
            p.lineinfo = []
            for _ in range(sizecode):
                lastoffset = (lastoffset + buf[off]) & 0xFF
                p.lineinfo.append(lastoffset)
                off += 1
            lastline = 0
            p.abslineinfo = []
            for _ in range(intervals):
                lastline += struct.unpack("<i", buf[off:off+4])[0]
                p.abslineinfo.append(lastline)
                off += 4
        debuginfo = buf[off]; off += 1
        if debuginfo:
            nlocvars, off = varint(buf, off)
            for _ in range(nlocvars):
                nm, off = varint(buf, off)
                startpc, off = varint(buf, off)
                endpc, off = varint(buf, off)
                reg = buf[off]; off += 1
                p.locvars.append((strings[nm - 1], startpc, endpc, reg))
            nup, off = varint(buf, off)
            assert nup == p.nups, (nup, p.nups)
            for _ in range(nup):
                nm, off = varint(buf, off)
                p.upvalues.append(strings[nm - 1])
        # v11+ feedback vector; v12+ inlining cost. These live after debug
        # info and must be skipped or every following proto is misaligned.
        if version >= 11:
            nfeedback, off = varint(buf, off)
            for _ in range(nfeedback):
                off += 1  # slot type
                _, off = varint(buf, off)  # call target pc
        if version >= 12 and (p.flags & 0x08):  # LPF_INLINABLE
            _, off = read_varint64(buf, off)
        protos.append(p)
    return version, typesversion, strings, protos, off


# ---------------------------------------------------------------------------
# opcode table (from upstream Common/include/Luau/Bytecode.h, LuauOpcode order)
# ---------------------------------------------------------------------------
OPNAMES = [
    "NOP", "BREAK", "LOADNIL", "LOADB", "LOADN", "LOADK", "MOVE",
    "GETGLOBAL", "SETGLOBAL", "GETUPVAL", "SETUPVAL", "CLOSEUPVALS",
    "GETIMPORT", "GETTABLE", "SETTABLE", "GETTABLEKS", "SETTABLEKS",
    "GETTABLEN", "SETTABLEN", "NEWCLOSURE", "NAMECALL", "CALL",
    "RETURN", "JUMP", "JUMPBACK", "JUMPIF", "JUMPIFNOT",
    "JUMPIFEQ", "JUMPIFLE", "JUMPIFLT", "JUMPIFNOTEQ", "JUMPIFNOTLE", "JUMPIFNOTLT",
    "ADD", "SUB", "MUL", "DIV", "MOD", "POW", "DIVK", "IDIV", "IDIVK",
    "ADDK", "SUBK", "MULK", "MODK", "POWK", "AND", "OR", "ANDK", "ORK",
    "CONCAT", "NEWTABLE", "DUPTABLE", "SETLIST", "FORNPREP", "FORNLOOP", "FORGLOOP",
    "FORGPREP_INEXT", "FASTCALL3",  # note: names/order below verified against Luau source
]
# Build exact order from Luau source instead:
import os
import re
_luau = os.environ.get(
    "LUAU_DIR",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "third_party", "luau"),
)
hdr = open(os.path.join(_luau, "Common", "include", "Luau", "Bytecode.h")).read()
m = re.search(r"enum LuauOpcode\s*\{(.*?)\};", hdr, re.S)
body = re.sub(r"//[^\n]*", "", m.group(1))
OPNAMES = []
cur = 0
for item in [x.strip() for x in body.split(",") if x.strip()]:
    if "=" in item:
        name, val = [s.strip() for s in item.split("=", 1)]
        cur = int(val, 0)
    else:
        name = item
    OPNAMES.append((cur, name))
    cur += 1
OPMAP = dict(OPNAMES)

# op lengths (mirror Luau getOpLength)
LEN2 = {
    "GETGLOBAL", "SETGLOBAL", "GETIMPORT", "GETTABLEKS", "SETTABLEKS", "NAMECALL",
    "JUMPIFEQ", "JUMPIFLE", "JUMPIFLT", "JUMPIFNOTEQ", "JUMPIFNOTLE", "JUMPIFNOTLT",
    "NEWTABLE", "SETLIST", "FORGLOOP", "LOADKX", "FASTCALL2", "FASTCALL2K", "FASTCALL3",
    "JUMPXEQKNIL", "JUMPXEQKB", "JUMPXEQKN", "JUMPXEQKS", "GETUDATAKS", "SETUDATAKS",
    "NAMECALLUDATA", "NEWCLASSMEMBER", "CALLFB", "CMPPROTO", "NEWCLASS",
}


def A(w): return (w >> 8) & 0xFF
def B(w): return (w >> 16) & 0xFF
def C(w): return (w >> 24) & 0xFF
def D(w): return (w >> 16) & 0xFFFF
def E(w): return (w >> 8) & 0xFFFF


def line_at(p, pc):
    if not p.lineinfo or pc >= len(p.lineinfo):
        return None
    return p.abslineinfo[pc >> p.linegaplog2] + p.lineinfo[pc]


def main():
    buf = open(sys.argv[1], "rb").read()
    version, tv, strings, protos, endoff = parse(buf, verbose=True)
    print(f"; parsed OK, end offset {endoff}/{len(buf)}")
    for pi, p in enumerate(protos):
        print(f"\n; ===== proto {pi} name={p.debugname!r} params={p.numparams} nups={p.nups} "
              f"vararg={p.is_vararg} stack={p.maxstacksize} flags={p.flags:#x} code={len(p.code)} consts={len(p.k)}")
        for i, k in enumerate(p.k):
            if k[0] == "str":
                print(f";   K{i} = str {k[1]!r}")
            elif k[0] == "import":
                imp = k[1]
                if imp & (1 << 30):
                    idx = (imp >> 20) & 1023
                else:
                    idx = imp & 0xFFFF
                print(f";   K{i} = import aux={imp:#x}")
            else:
                print(f";   K{i} = {k}")
        if p.upvalues:
            print(f";   upvals: {p.upvalues}")
        if p.locvars:
            print(f";   locals: {[(l[0], l[3]) for l in p.locvars]}")
        pc = 0
        while pc < len(p.code):
            w = p.code[pc]
            opcode = w & 0xFF
            name = OPMAP.get(opcode, f"OP_{opcode}")
            short = name[4:] if name.startswith("LOP_") else name
            if short in LEN2:
                aux = p.code[pc + 1] if pc + 1 < len(p.code) else 0
                extra = ""
                if short in ("GETGLOBAL", "SETGLOBAL", "GETIMPORT", "GETTABLEKS", "SETTABLEKS", "NAMECALL"):
                    kk = aux & 0xFFFFFF
                    if kk < len(p.k):
                        extra = f"  ; K{kk}={p.k[kk]}"
                if short == "LOADK":
                    extra = f"  ; K{D(w)}={p.k[D(w)] if D(w) < len(p.k) else '?'}"
                ln = line_at(p, pc)
                print(f"{pc:5d} L{ln if ln is not None else '?':>4}: {name:16s} A={A(w)} B={B(w)} C={C(w)} aux={aux}{extra}")
                pc += 2
            else:
                extra = ""
                if short == "LOADK":
                    extra = f"  ; K{D(w)}={p.k[D(w)] if D(w)<len(p.k) else '?'}"
                if short in ("GETUPVAL", "SETUPVAL"):
                    extra = f"  ; U{B(w)}"
                if short in ("NEWCLOSURE", "DUPCLOSURE"):
                    extra = f"  ; P{D(w)}"
                ln = line_at(p, pc)
                print(f"{pc:5d} L{ln if ln is not None else '?':>4}: {name:16s} A={A(w)} B={B(w)} C={C(w)} D={D(w)}{extra}")
                pc += 1


if __name__ == "__main__":
    main()
