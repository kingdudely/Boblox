"""ida_xref.py — list callers of one function and decompile them.

Reads target addresses from tools/ida_addrs.txt. For each target, prints every
call site (from the caller's body) and then decompiles the caller, which is how
the per-second RUPP token updater gets named from its two log sites.
"""
import os

import ida_funcs
import ida_hexrays
import idautils
import idc

ida_hexrays.init_hexrays_plugin()
ADDR_FILE = os.environ.get("IDA_ADDR_FILE", "/home/john/Boblox/tools/ida_addrs.txt")
targets = []
if os.path.exists(ADDR_FILE):
    for line in open(ADDR_FILE):
        line = line.split("#")[0].strip()
        if line:
            targets.append(int(line.lstrip("+"), 16))

for tgt in targets:
    print("\n######## callers of %s @ %#x" % (idc.get_func_name(tgt), tgt))
    callers = []
    for x in idautils.XrefsTo(tgt, 0):
        f = ida_funcs.get_func(x.frm)
        if not f:
            continue
        if f.start_ea not in [c for c, _ in callers]:
            callers.append((f.start_ea, x.frm))
    for fstart, site in callers:
        print("   call at %#x from %s @ %#x" % (site, idc.get_func_name(fstart), fstart))
    for fstart, _ in callers:
        f = ida_funcs.get_func(fstart)
        print("\n==== %s @ %#x (%d bytes)" % (idc.get_func_name(fstart), fstart,
                                             f.end_ea - f.start_ea))
        try:
            print(ida_hexrays.decompile(fstart))
        except Exception as e:
            print("decompile failed: %r" % e)
