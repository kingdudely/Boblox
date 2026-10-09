#!/usr/bin/env python3
"""opcode_map.py — derive Roblox-internal opcode -> standard Luau opcode mapping
by aligning the native's post-remap code dumps (run/remap/proto_*.hex) against a
reference compile of the decompiled source (made with the v13-era Luau).

Alignment works on operand bytes: for each candidate mapping we match each
instruction word's non-opcode bytes. Because the reference program is the same
program, the streams line up 1:1 (both compiled at -O2 by same-era compilers).

Output: tools/roblox_opcode_map.json  {"internal_op": std_op, ...}

Usage: opcode_map.py <reference.luac> <remap_log_dir>
"""
import json
import os
import re
import struct
import sys


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


def parse_code(buf):
    """Extract per-proto code words from a v13+ .luac (protoSize skipping)."""
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
    for _ in range(proto_count):
        psize = None
        if version >= 12:
            psize, off = varint(buf, off)
        pstart = off
        off += 4 + 1
        if tv in (1, 2, 3):
            ts, off = varint(buf, off)
            off += ts
        sizecode, off = varint(buf, off)
        words = struct.unpack_from("<%dI" % sizecode, buf, off)
        protos.append(list(words))
        if psize is not None:
            off = pstart + psize
        else:
            raise RuntimeError("pre-v12 ref not supported")
    return version, tv, protos


def load_native(logdir, max_protos=64):
    sizes = []
    started = False
    for line in open(os.path.join(logdir, "remap.log")):
        if "LOADER =challenge" in line:
            started = True
            continue
        if not started:
            continue
        m = re.match(r"PROTO#(\d+) sizecode=(\d+)", line)
        if m:
            sizes.append(int(m.group(2)))
            if len(sizes) >= max_protos:
                break
    protos = []
    for i, s in enumerate(sizes):
        fp = os.path.join(logdir, f"proto_{i}_s{s}.hex")
        if not os.path.exists(fp):
            break
        data = bytes.fromhex(open(fp).read().strip())
        protos.append(list(struct.unpack_from("<%dI" % s, data)))
    return protos


def align_lcs(a, b, key):
    """LCS alignment of two word lists keyed by operand bytes. Returns list of
    (i, j) matched index pairs."""
    # simple dynamic programming LCS on key equality (lists are <= ~600)
    n, m = len(a), len(b)
    ka = [key(w) for w in a]
    kb = [key(w) for w in b]
    # trim to keep DP small
    dp = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(n - 1, -1, -1):
        row = dp[i]
        nxt = dp[i + 1]
        for j in range(m - 1, -1, -1):
            if ka[i] == kb[j]:
                row[j] = nxt[j + 1] + 1
            else:
                row[j] = max(nxt[j], row[j + 1])
    out = []
    i = j = 0
    while i < n and j < m:
        if ka[i] == kb[j]:
            out.append((i, j))
            i += 1
            j += 1
        elif dp[i + 1][j] >= dp[i][j + 1]:
            i += 1
        else:
            j += 1
    return out


def main():
    ref = sys.argv[1]
    logdir = sys.argv[2]
    buf = open(ref, "rb").read()
    version, tv, ref_protos = parse_code(buf)
    nat = load_native(logdir)
    print(f"reference: {len(ref_protos)} protos; native: {len(nat)} protos")

    # match protos by code size and operand alignment score
    votes = {}
    matches = 0
    for ni, nwords in enumerate(nat):
        best = None
        for ri, rwords in enumerate(ref_protos):
            if abs(len(rwords) - len(nwords)) > 6:
                continue
            pairs = align_lcs(nwords, rwords, key=lambda w: w >> 8)
            score = len(pairs)
            if best is None or score > best[0]:
                best = (score, ri, pairs)
        if best is None or best[0] < len(nwords) // 2:
            continue
        score, ri, pairs = best
        matches += 1
        for (i, j) in pairs:
            iop = nwords[i] & 0xFF
            sop = ref_protos[ri][j] & 0xFF
            votes.setdefault(iop, {})
            votes[iop][sop] = votes[iop].get(sop, 0) + 1
        print(f"native proto{ni} ({len(nwords)}w) ~ ref proto{ri} ({len(ref_protos[ri])}w): matched {score}")

    opmap = {}
    conflicts = 0
    for iop, counts in sorted(votes.items()):
        total = sum(counts.values())
        sop, c = max(counts.items(), key=lambda kv: kv[1])
        opmap[iop] = sop
        if c < total * 0.6:
            conflicts += 1
        print(f"  internal {iop:#04x} -> std {sop:#04x}  ({c}/{total})")
    print(f"mapped {len(opmap)} ops, low-confidence {conflicts}")

    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "roblox_opcode_map.json")
    json.dump({str(k): v for k, v in opmap.items()}, open(out, "w"))
    print("wrote", out)


if __name__ == "__main__":
    main()
