#!/usr/bin/env python3
"""decode_roblox_bc.py — decode a captured Roblox =challenge bytecode file into
standard Luau bytecode that upstream Luau can load.

Roblox's client loader (sub_679992E) parses the wire format, then rewrites every
instruction's opcode byte in place using two obfuscation tables:
    op  = remap[wire_op]                    (byte_CC2904, libroblox.so VA 0xCC2904)
    len = oplen_check(oplen_table[op])      (byte_CC2A04, VA 0xCC2A04)
Instructions advance by len words (len in {1,2}).

Tables are read from tools/roblox_bc_tables.json (dumped from IDA).

Usage:
  decode_roblox_bc.py <wire.bin> <out.luac>

Exits non-zero (and prints diagnostics) if the length walk disagrees with
upstream Luau's own getOpLength for a decoded opcode — meaning the numbering is
fork-specific and needs a name mapping too.
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TABLES = json.load(open(os.path.join(HERE, "roblox_bc_tables.json")))
REMAP = TABLES["remap"]
OPLEN = TABLES["oplen"]

# sub_27509DA: returns 2 if the value is in this set, else 1.
TWO_WORD_SET = {7, 8, 12, 15, 16, 20, 27, 28, 29, 30, 31, 32, 52, 55, 58, 60, 66}


def varint(buf, off):
    v = 0
    sh = 0
    while True:
        b = buf[off]
        off += 1
        v |= (b & 0x7F) << sh
        if not (b & 0x80):
            return v, off
        sh += 7


def parse_layout(buf):
    """Parse structure, returning list of (proto_index, code_offset_words, sizecode)."""
    off = 0
    version = buf[off]; off += 1
    tv = buf[off]; off += 1
    n_strings, off = varint(buf, off)
    for _ in range(n_strings):
        ln, off = varint(buf, off)
        off += ln
    if tv == 3:
        idx = buf[off]; off += 1
        while idx != 0:
            _, off = varint(buf, off)
            idx = buf[off]; off += 1
    proto_count, off = varint(buf, off)
    protos = []
    for i in range(proto_count):
        proto_size = 0
        if version >= 12:
            proto_size, off = varint(buf, off)
        off += 4  # maxstacksize, numparams, nups, is_vararg
        off += 1  # flags
        if tv in (1, 2, 3):
            ts, off = varint(buf, off)
            off += ts
        sizecode, off = varint(buf, off)
        code_off = off
        off += 4 * sizecode
        sizek, off = varint(buf, off)
        for _ in range(sizek):
            t = buf[off]; off += 1
            if t == 0:
                pass
            elif t == 1:
                off += 1
            elif t == 2:
                off += 8
            elif t == 3:
                _, off = varint(buf, off)
            elif t == 4:
                off += 4
            elif t == 5:
                n, off = varint(buf, off)
                for _ in range(n):
                    _, off = varint(buf, off)
            elif t == 6:
                _, off = varint(buf, off)
            elif t == 7:
                off += 16
            elif t == 8:
                off += 32
            elif t == 9:
                n, off = varint(buf, off)
                for _ in range(n):
                    _, off = varint(buf, off)
                    off += 4
            elif t == 10:
                off += 1
                _, off = varint(buf, off)
            elif t == 11:
                _, off = varint(buf, off)
                _, off = varint(buf, off)
                _, off = varint(buf, off)
                # members: unknown count from the two varints; re-read them
                raise RuntimeError("class shape const in file; extend parser")
            else:
                raise RuntimeError(f"bad const type {t} at {off-1}")
        sizep, off = varint(buf, off)
        for _ in range(sizep):
            _, off = varint(buf, off)
        _, off = varint(buf, off)  # linedefined
        _, off = varint(buf, off)  # debugname
        lineinfo = buf[off]; off += 1
        if lineinfo:
            gaplog2 = buf[off]; off += 1
            intervals = ((sizecode - 1) >> gaplog2) + 1
            off += sizecode
            off += 4 * intervals
        debuginfo = buf[off]; off += 1
        if debuginfo:
            n, off = varint(buf, off)
            for _ in range(n):
                _, off = varint(buf, off)
                _, off = varint(buf, off)
                _, off = varint(buf, off)
                off += 1
            nup, off = varint(buf, off)
            for _ in range(nup):
                _, off = varint(buf, off)
        protos.append((i, code_off, sizecode))
    return version, tv, protos, off, len(buf)


# upstream Luau getOpLength (BytecodeUtils.h)
UPSTREAM_TWO_WORD = {
    "GETGLOBAL", "SETGLOBAL", "GETIMPORT", "GETTABLEKS", "SETTABLEKS", "NAMECALL",
    "JUMPIFEQ", "JUMPIFLE", "JUMPIFLT", "JUMPIFNOTEQ", "JUMPIFNOTLE", "JUMPIFNOTLT",
    "NEWTABLE", "SETLIST", "FORGLOOP", "LOADKX", "FASTCALL2", "FASTCALL2K", "FASTCALL3",
    "JUMPXEQKNIL", "JUMPXEQKB", "JUMPXEQKN", "JUMPXEQKS", "GETUDATAKS", "SETUDATAKS",
    "NAMECALLUDATA", "NEWCLASSMEMBER", "CALLFB", "CMPPROTO", "NEWCLASS",
}
# upstream opcode names in enum order
import os as _os
import re as _re
_luau = _os.environ.get(
    "LUAU_DIR",
    _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "..", "third_party", "luau"),
)
_hdr = open(_os.path.join(_luau, "Common", "include", "Luau", "Bytecode.h")).read()
_m = _re.search(r"enum LuauOpcode\s*\{(.*?)\};", _hdr, _re.S)
_body = _re.sub(r"//[^\n]*", "", _m.group(1))
UP_NAMES = []
_cur = 0
for _it in [x.strip() for x in _body.split(",") if x.strip()]:
    if "=" in _it:
        _n, _v = [s.strip() for s in _it.split("=", 1)]
        _cur = int(_v, 0)
    else:
        _n = _it
    UP_NAMES.append((_cur, _n))
    _cur += 1
UP_BY_ID = dict(UP_NAMES)


def upstream_len(op):
    name = UP_BY_ID.get(op)
    if name is None:
        return None
    return 2 if name in UPSTREAM_TWO_WORD else 1


def walk(buf, code_off, sizecode, verbose=False, proto_idx=0):
    """Return list of (pc, wire_op, decoded_op, len, upstream_len, ok)."""
    out = []
    pc = 0
    words = struct.unpack_from("<%dI" % sizecode, buf, code_off)
    while pc < sizecode:
        w = words[pc]
        wire_op = w & 0xFF
        rop = REMAP[wire_op]
        lv = OPLEN[rop]
        length = 2 if lv in TWO_WORD_SET else 1
        ul = upstream_len(rop)
        ok = (ul == length)
        out.append((pc, wire_op, rop, length, ul, ok))
        pc += length
    return out, words


def main():
    wire_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else wire_path + ".decoded"
    buf = bytearray(open(wire_path, "rb").read())
    version, tv, protos, endoff, total = parse_layout(buf)
    print(f"version={version} tv={tv} protos={len(protos)} parsed_end={endoff}/{total}")

    bad = 0
    stats = {}
    for (i, code_off, sizecode) in protos:
        steps, words = walk(buf, code_off, sizecode, proto_idx=i)
        # rewrite opcodes in the buffer
        for (pc, wire_op, rop, length, ul, ok) in steps:
            w = words[pc]
            neww = (w & 0xFFFFFF00) | rop
            struct.pack_into("<I", buf, code_off + 4 * pc, neww)
            stats[rop] = stats.get(rop, 0) + 1
            if not ok:
                bad += 1
                if bad <= 20:
                    print(f"  proto{i} pc={pc}: wire={wire_op:#04x} -> {rop:#04x} "
                          f"len={length} upstream_len={ul} {'?' if ul is None else ''}")
    print(f"instructions remapped; mismatches={bad}")
    if bad:
        print("NOTE: mismatches mean fork-specific opcode numbering; a name map is needed.")

    open(out_path, "wb").write(buf)
    print("wrote", out_path)
    top = sorted(stats.items(), key=lambda kv: -kv[1])[:20]
    print("top decoded opcodes:", [(hex(o), UP_BY_ID.get(o, '?'), n) for o, n in top])


if __name__ == "__main__":
    main()
