#!/usr/bin/env python3
"""join_wire_cmp.py — side-by-side view of the RUPP join ladder from two
RBX_RAKNET_WIRE / RC_HOOKS=wire captures (native vs ours).

Both sides use the same 31-byte RUPP client header:
    [0]=01 [1]=00 [2:4]=hdrlen BE [4]=01 [5]=11 [6]=subtype
    [7:23]=16B token [23]=02 [24]=06 [25:29]=ip [29:31]=port BE
so the app payload starts at offset 31.

    tools/join_wire_cmp.py /tmp/rc_dhw/wire_tx_*.bin -- /tmp/ourwire/*.bin
"""
import sys
import glob
import os


def frames(pattern, limit):
    names = glob.glob(pattern)
    if not names:
        return []
    # wire_tx_N / wNNNNNN -> numeric order
    def idx(p):
        b = os.path.basename(p)
        digits = "".join(c for c in b if c.isdigit())
        return int(digits or 0)

    names.sort(key=idx)
    return names[:limit]


def describe(path):
    d = open(path, "rb").read()
    sub = d[6] if len(d) > 6 else -1
    tlv_port = (d[29] << 8 | d[30]) if len(d) >= 31 else -1
    body = d[31:]
    if not body:
        return (len(d), sub, tlv_port, "-")
    aid = body[0]
    subid = body[1] if len(body) > 1 else 0
    tag = f"{aid:02x}"
    if aid in (0x00, 0x03):
        tag = f"pad{aid:02x}"
    elif aid == 0x83:
        tag = f"83/{subid:02x}"
    elif 0x81 <= aid <= 0xA9:
        tag = f"{aid:02x}/{subid:02x}"
    return (len(d), sub, tlv_port, tag + f" ({len(body)}B) {body[:10].hex()}")


def main():
    if "--" not in sys.argv:
        print(__doc__)
        return 1
    cut = sys.argv.index("--")
    left = frames(sys.argv[1], 40)
    right = frames(" ".join(sys.argv[cut + 1:]) if len(sys.argv) > cut + 1 else "", 40)
    right = frames(sys.argv[cut + 1], 40)
    for i in range(max(len(left), len(right))):
        a = describe(left[i]) if i < len(left) else ("",) * 4
        b = describe(right[i]) if i < len(right) else ("",) * 4
        print(f"{i:3d}  N {a[0]:>5} sub={a[1]:<3} tlv={a[2]:<6} {a[3]}")
        print(f"     O {b[0]:>5} sub={b[1]:<3} tlv={b[2]:<6} {b[3]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
