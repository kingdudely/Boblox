#!/usr/bin/env python3
"""a1_attr_probe.py — fit the 0xA1 attribute-section grammar from dumped tails.

tools/a1_tail_probe (A1_TAIL_DIR=<dir>) writes the bytes parseAppStateBody
leaves after a record's selected members. Those bytes are the per-instance
attribute section. This script walks them with a candidate entry shape and
reports which shapes close to the exact end, so the grammar is fitted from
thousands of real records instead of a handful of hexdumps.

    a1_attr_probe.py <tails-dir> [--show N]

Candidate shape per entry (the one the samples suggest):

    u8  prefix (0 on every sample so far)
    u8  type   (the property-value tag the value uses)
    u32 name length
    ... name bytes
    ... value, sized by `type` through the snapshot tag widths

An entry that does not close is printed with its offset and bytes so the
offending tag can be identified.
"""
import os
import struct
import sys
from collections import Counter

# Value widths for the snapshot/property tag grammar (read_tagged_value).
# None = not modelled yet; those entries are reported rather than skipped.
def value_end(buf, p, tag):
    def need(n):
        return p + n <= len(buf)
    if tag in (0, 1):              # nil / bool
        return p + 1 if need(1) else None
    if tag == 2:                   # float / int32
        return p + 4 if need(4) else None
    if tag == 3:
        return p + 8 if need(8) else None
    if tag == 9:                   # u8
        return p + 1 if need(1) else None
    if tag == 10:                  # u64
        return p + 8 if need(8) else None
    if tag == 12:                  # string
        if not need(4):
            return None
        n = struct.unpack_from("<I", buf, p)[0]
        return p + 4 + n if p + 4 + n <= len(buf) else None
    if tag == 13:                  # vector3
        return p + 12 if need(12) else None
    if tag in (14, 15):            # cframe / vector2-ish
        return p + 48 if tag == 14 and need(48) else (p + 8 if tag == 15 and need(8) else None)
    return None


def walk(buf, prefix_len=1, has_value=True):
    """Walk [u8 prefix?][u8 tag][u32 len][name][value?] entries.

    Returns (ok, entries, fail_at, fail_tag). `prefix_len` 0 means the entry
    starts with the tag; `has_value` False means the entry is a bare name,
    which is what a name-only dictionary would look like.
    """
    p = 0
    entries = []
    hdr = prefix_len + 1 + 4
    while p < len(buf):
        if p + hdr > len(buf):
            return False, entries, p, None
        prefix = buf[p] if prefix_len else 0
        tag = buf[p + prefix_len]
        n = struct.unpack_from("<I", buf, p + prefix_len + 1)[0]
        if n > 128 or p + hdr + n > len(buf):
            return False, entries, p, None
        name = buf[p + hdr:p + hdr + n]
        if any(b < 9 or (13 < b < 32) or b > 126 for b in name):
            return False, entries, p, None
        vp = p + hdr + n
        if has_value:
            end = value_end(buf, vp, tag)
            if end is None:
                return False, entries, p, tag
        else:
            end = vp
        entries.append((prefix, tag, name.decode("ascii", "replace"), end - vp))
        p = end
    return p == len(buf), entries, p, None


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    d = argv[1]
    show = 0
    if "--show" in argv:
        show = int(argv[argv.index("--show") + 1])
    files = sorted(os.listdir(d))
    # Grammar fit first: which (prefix_len, has_value) closes the most tails
    # exactly? A grammar that only closes a few is not the wire's.
    variants = [(1, True), (1, False), (0, True), (0, False)]
    print("grammar fit (offset 0, exact closure):")
    for pl, hv in variants:
        closed = 0
        for f in files:
            buf = open(os.path.join(d, f), "rb").read()
            if walk(buf, pl, hv)[0]:
                closed += 1
        print(f"  prefix_len={pl} value={hv}: {closed}/{len(files)}")
    ok = 0
    tags = Counter()
    prefix = Counter()
    fails = Counter()
    shown = 0
    for f in files:
        buf = open(os.path.join(d, f), "rb").read()
        good, entries, at, tag = walk(buf, 1, False)
        if good:
            ok += 1
            for pr, tg, nm, vl in entries:
                tags[tg] += 1
                prefix[pr] += 1
        else:
            fails[tag] += 1
            if shown < show:
                shown += 1
                print(f"FAIL {f} len={len(buf)} at={at} tag={tag}")
                print("     " + buf[max(0, at - 8):at + 32].hex(" "))
                print("     " + repr(buf[max(0, at - 8):at + 32]))
    print(f"tails={len(files)} closed={ok} failed={len(files) - ok}")
    print("prefix:", dict(prefix))
    print("tags:", dict(tags))
    print("fail tags:", dict(fails))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
