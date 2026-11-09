"""ida_sites.py — resolve stack-scan offsets to functions and call sites.

Reads one module offset per line from tools/ida_addrs.txt (plain hex, no 0x
needed) and, for each, prints the containing function, its size, and the
instruction at the offset with a few lines of context. A frame recovered by
tools/gdb/rc_capture.gdb's sbt() is a *return address*: the interesting
instruction is the call immediately before it.
"""
import os

import ida_funcs
import ida_lines
import ida_ua
import idc

ADDR_FILE = os.environ.get("IDA_ADDR_FILE", "/home/john/Boblox/tools/ida_addrs.txt")
BASE = 0x0
addrs = []
if os.path.exists(ADDR_FILE):
    for line in open(ADDR_FILE):
        line = line.split("#")[0].strip()
        if not line:
            continue
        line = line.lstrip("+")
        addrs.append(int(line, 16))

for ea in addrs:
    f = ida_funcs.get_func(ea)
    if not f:
        print("\n%#x: no function" % ea)
        continue
    print("\n%#x -> %s @ %#x (%d bytes)  %s" % (
        ea, idc.get_func_name(f.start_ea), f.start_ea,
        f.end_ea - f.start_ea,
        "CALLSITE" if idc.print_insn_mnem(ea) else "inside"))
    # walk back a little so a return address shows the call it came from
    cur = ea - 16 if ea - f.start_ea > 16 else f.start_ea
    for _ in range(8):
        if cur < f.start_ea or cur >= f.end_ea:
            break
        dis = idc.generate_disasm_line(cur, 0)
        if dis:
            print("   %#x  %s" % (cur, ida_lines.tag_remove(dis)))
        nxt = idc.next_head(cur, f.end_ea)
        if nxt <= cur:
            break
        cur = nxt
