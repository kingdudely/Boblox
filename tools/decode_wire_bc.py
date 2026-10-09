#!/usr/bin/env python3
"""decode_wire_bc.py — convert a captured Roblox wire bytecode file (v13) into
standard Luau bytecode, using the loader's own tables from libroblox.so:

  wire_op  --byte_CC2904--> internal_op  --byte_CC2A04--> standard Luau opcode
  instruction length = sub_27509DA(standard_op) words (1 or 2)

Evidence (IDA, sub_679992E):
    v113 = byte_CC2904[wire_op]; *v112 = v113;
    v112 += 4 * sub_27509DA(byte_CC2A04[v113]);

and sub_27509DA's 2-word set uses standard Luau numbering, i.e.
byte_CC2A04 is exactly the internal→standard opcode table.

Validation: every proto's instruction walk must consume exactly sizecode words.

Usage: decode_wire_bc.py <wire.bin> <out.luac> [tables.json]
"""
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TABLES = json.load(open(os.path.join(HERE, "roblox_bc_tables.json")))
REMAP = TABLES["remap"]   # byte_CC2904: wire -> internal
OPLEN = TABLES["oplen"]   # byte_CC2A04: internal -> standard opcode number


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


# --- exact sub_27509DA -------------------------------------------------------
# return 2 if (op-7) in [0,0x3B] and bit set in 0x829400003F02323, OR
#           (op-74) in [0,0x10] and bit set in byte_17E7B (observed 0xFE);
# else 1.
MASK1 = 0x829400003F02323
MASK2 = 0x000000FE  # byte_17E7B value; bits 1..7 => ops 75..81


def op_length(std_op):
    v2 = std_op - 7
    if 0 <= v2 <= 0x3B and (MASK1 >> v2) & 1:
        return 2
    v4 = std_op - 74
    if 0 <= v4 <= 0x10 and (MASK2 >> v4) & 1:
        return 2
    return 1


def decode(buf):
    buf = bytearray(buf)
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
    report = []
    for i in range(proto_count):
        psize, off = varint(buf, off)
        pstart = off
        off += 4 + 1
        if tv in (1, 2, 3):
            ts, off = varint(buf, off)
            off += ts
        sizecode, off = varint(buf, off)
        code_off = off

        # walk + rewrite opcode bytes
        pc = 0
        std_ops = []
        while pc < sizecode:
            word = struct.unpack_from("<I", buf, code_off + 4 * pc)[0]
            wire_op = word & 0xFF
            internal = REMAP[wire_op]
            std = OPLEN[internal]
            ln = op_length(std)
            # standardise the opcode byte
            neww = (word & 0xFFFFFF00) | (std & 0xFF)
            struct.pack_into("<I", buf, code_off + 4 * pc, neww)
            std_ops.append(std)
            pc += ln
        ok = (pc == sizecode)
        report.append((i, sizecode, pc, ok, len(set(std_ops))))
        off = pstart + psize  # version>=12 tail skip

    mainid, off = varint(buf, off)
    return buf, proto_count, mainid, report, off


def main():
    wire_path, out_path = sys.argv[1], sys.argv[2]
    buf = bytearray(open(wire_path, "rb").read())
    _, proto_count, mainid, report, endoff = decode(buf)
    print(f"protos={proto_count} main={mainid} file_end={endoff}/{len(buf)}")
    bad = 0
    for (i, sc, pc, ok, nops) in report:
        flag = "OK" if ok else f"DESYNC({pc}!={sc})"
        if not ok:
            bad += 1
        print(f"  proto{i:2d}: sizecode={sc:4d} walked={pc:4d} {flag}")
    if bad:
        print(f"\n{bad} protos desynced — mapping hypothesis wrong or table mismatch")
        return 1
    open(out_path, "wb").write(buf)
    print("all protos walked cleanly; wrote", out_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
