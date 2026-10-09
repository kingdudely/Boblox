#!/usr/bin/env python3
"""standardize_wire.py — turn a wire bytecode program (the 7226-byte decoded
challenge program, i.e. what luau_load receives) into STANDARD Luau bytecode.

Steps:
  1. parse wire.bin structure (v13, protoSize skipping)
  2. optionally cross-check against post-remap proto dumps:
         dump_op == REMAP[wire_op]         (loader transform, must hold)
  3. rewrite each instruction-start opcode byte:
         wire_op -> internal = REMAP[wire_op] -> std = OPLEN[internal]
     using loader length semantics on the internal op
  4. verify the walk consumes exactly sizecode words per proto.

Usage: standardize_wire.py <dir> <out.luac>   (dir has wire.bin + proto_*.hex)
       (library use: standardize_bytes(buf) -> (buf, stats))
"""
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
T = json.load(open(os.path.join(HERE, "roblox_bc_tables.json")))
REMAP = T["remap"]
OPLEN = T["oplen"]

MASK1 = 0x829400003F02323
# sub_27509DA: "mov ecx, offset byte_17E7B; bt ecx, edi" — the tested value is the
# address constant 0x17E7B itself, not the memory at that address.
MASK2 = 0x00017E7B


def op_length(std_op):
    v2 = std_op - 7
    if 0 <= v2 <= 0x3B and (MASK1 >> v2) & 1:
        return 2
    v4 = std_op - 74
    if 0 <= v4 <= 0x10 and (MASK2 >> v4) & 1:
        return 2
    return 1


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


def parse_protos(buf):
    """Return list of (pstart, psize, code_off, sizecode) for each proto."""
    off = 0
    off += 1  # version
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
    for _ in range(proto_count):
        psize, off = varint(buf, off)
        pstart = off
        off += 4 + 1
        if tv in (1, 2, 3):
            ts, off = varint(buf, off)
            off += ts
        sizecode, off = varint(buf, off)
        protos.append((pstart, psize, off, sizecode))
        off = pstart + psize
    return protos


def standardize_bytes(buf, dumps=None):
    """Rewrite wire opcodes into standard Luau opcodes, in place.

    buf   — bytearray of the wire program
    dumps — optional dict {proto_index: list[int] words} with native post-remap
            code (for validation); mismatch counts are reported in stats.
    Returns (buf, stats dict).
    """
    buf = bytearray(buf)
    protos = parse_protos(buf)
    total_bad = 0
    total_starts = 0
    bad_walks = []
    for i, (pstart, psize, code_off, sizecode) in enumerate(protos):
        dump = (dumps or {}).get(i)
        pc = 0
        mism = 0
        while pc < sizecode:
            wire_word = struct.unpack_from("<I", buf, code_off + 4 * pc)[0]
            wop = wire_word & 0xFF
            internal = REMAP[wop]
            std = OPLEN[internal]
            if dump is not None:
                d_op = dump[pc] & 0xFF
                if d_op != internal:
                    mism += 1
            struct.pack_into("<I", buf, code_off + 4 * pc,
                             (wire_word & 0xFFFFFF00) | (std & 0xFF))
            pc += op_length(std)
            total_starts += 1
        if pc != sizecode:
            bad_walks.append((i, pc, sizecode))
        total_bad += mism
    return buf, {
        "starts": total_starts,
        "mismatches": total_bad,
        "protos": len(protos),
        "bad_walks": bad_walks,
    }


def main():
    d = sys.argv[1]
    out_path = sys.argv[2]
    buf = bytearray(open(os.path.join(d, "wire.bin"), "rb").read())

    dumps = {}
    for i in range(64):
        # dumps named proto_<i>_s<sizecode>.hex
        for fn in os.listdir(d):
            if fn.startswith(f"proto_{i}_s") and fn.endswith(".hex"):
                raw = bytes.fromhex(open(os.path.join(d, fn)).read().strip())
                words = struct.unpack("<%dI" % (len(raw) // 4), raw)
                dumps[i] = words

    buf, stats = standardize_bytes(buf, dumps)
    for b in stats["bad_walks"]:
        print(f"  BAD WALK proto{b[0]}: pc={b[1]} sizecode={b[2]}")
    print(f"total starts={stats['starts']}, transform mismatches={stats['mismatches']}")
    open(out_path, "wb").write(buf)
    print("wrote", out_path)


if __name__ == "__main__":
    main()
