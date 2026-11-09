"""ida_find.py — find a byte pattern in the analysed binary and name what references it.

The capability vector the native sends in its join-time 0x83/21 (12 34 5 0 0 102 32)
is a fixed immediate in the code that builds that message; the 60 s 0x83/21
type-4 message is built by the same layer, so its builder is what this reaches.
Pattern is read from PAT (default the kind-3 body).
"""
import os

import ida_bytes
import ida_funcs
import idautils
import idc

PAT = os.environ.get("IDA_PAT", "39 30 00 00 00 00 00 00 66 00 00 00 20 00 00 00")
print("pattern:", PAT)
ea = ida_bytes.find_bytes(bytes(int(x, 16) for x in PAT.split()), 0, 0, 0xFFFFFFFFF)
n = 0
while ea != idc.BADADDR and n < 12:
    seg = idc.get_segm_name(ea)
    print("\nhit at %#x  seg=%s  name=%s" % (ea, seg, idc.get_name(ea) or ""))
    for x in idautils.XrefsTo(ea, 0):
        f = ida_funcs.get_func(x.frm)
        print("   xref from %#x  func=%s @ %#x" %
              (x.frm, idc.get_func_name(f.start_ea) if f else "?", f.start_ea if f else 0))
    n += 1
    ea = ida_bytes.find_bytes(bytes(int(x, 16) for x in PAT.split()), ea + 1, 0, 0xFFFFFFFFF)
print("\ntotal shown:", n)
