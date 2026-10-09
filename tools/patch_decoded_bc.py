#!/usr/bin/env python3
"""patch_decoded_bc.py — rebuild a loadable Luau bytecode file from:
  * the raw wire bytes (wire.bin)
  * the native's post-remap instruction dumps (proto_N_sM.hex, in remap.log order)

Uses version>=12 protoSize semantics (Luau upstream loader) for parsing the wire,
so unknown proto-tail bytes are skipped correctly.

Usage: patch_decoded_bc.py <wire.bin> <logdir> <out.luac>
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


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


def main():
    wire_path, logdir, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    wire = bytearray(open(wire_path, "rb").read())

    # collect dump sizes in order (after the challenge marker)
    dumps = []
    started = False
    for line in open(os.path.join(logdir, "remap.log")):
        if "LOADER =challenge" in line:
            started = True
            continue
        if not started:
            continue
        m = re.match(r"PROTO#(\d+) sizecode=(\d+)", line)
        if m:
            dumps.append((int(m.group(1)), int(m.group(2))))
    print(f"dumps collected: {len(dumps)}")

    # parse wire structure with protoSize skipping
    off = 0
    version = wire[off]; off += 1
    tv = wire[off]; off += 1
    n_strings, off = varint(wire, off)
    for _ in range(n_strings):
        ln, off = varint(wire, off)
        off += ln
    if tv == 3:
        idx = wire[off]; off += 1
        while idx != 0:
            _, off = varint(wire, off)
            idx = wire[off]; off += 1
    proto_count, off = varint(wire, off)
    print(f"version={version} tv={tv} protos={proto_count}")

    patched = 0
    for i in range(proto_count):
        psize, off = varint(wire, off)
        proto_start = off
        off += 4 + 1
        if tv in (1, 2, 3):
            ts, off = varint(wire, off)
            off += ts
        sizecode, off = varint(wire, off)
        code_off = off
        if i < len(dumps) and dumps[i][1] == sizecode:
            fp = os.path.join(logdir, f"proto_{i}_s{sizecode}.hex")
            if os.path.exists(fp):
                words = bytes.fromhex(open(fp).read().strip())
                if len(words) == 4 * sizecode:
                    wire[code_off:code_off + 4 * sizecode] = words
                    patched += 1
                else:
                    print(f"  proto{i}: dump length mismatch {len(words)} != {4*sizecode}")
            else:
                print(f"  proto{i}: no dump file {fp}")
        else:
            print(f"  proto{i}: sizecode={sizecode} dumps={dumps[i] if i < len(dumps) else None} (not patched)")
        off = proto_start + psize  # skip tail (version >= 12)

    mainid, off = varint(wire, off)
    print(f"main proto id: {mainid}")
    print(f"patched {patched}/{proto_count}")
    open(out_path, "wb").write(wire)
    print("wrote", out_path)


if __name__ == "__main__":
    main()
