"""ida_dec.py — decompile a list of addresses.

The code runs inside IDA's own interpreter, so it cannot see this process's
argv. Addresses are read from ADDR_FILE (default tools/ida_addrs.txt), one hex
address per line, '#' comments allowed.

    printf '0x33AE7D6\\n' > tools/ida_addrs.txt
    uv run --offline --with ida-nexus==0.14.2 python tools/ida_x.py tools/ida_dec.py
"""
import os

import ida_funcs
import ida_hexrays
import idc

ida_hexrays.init_hexrays_plugin()

ADDR_FILE = os.environ.get("IDA_ADDR_FILE", "/home/john/Boblox/tools/ida_addrs.txt")
addrs = []
if os.path.exists(ADDR_FILE):
    for line in open(ADDR_FILE):
        line = line.split("#")[0].strip()
        if line:
            addrs.append(int(line, 16))

for ea in addrs:
    f = ida_funcs.get_func(ea)
    if not f:
        print("no function at %#x" % ea)
        continue
    print("\n==== %s @ %#x (%d bytes)" % (idc.get_func_name(f.start_ea),
                                         f.start_ea, f.end_ea - f.start_ea))
    try:
        print(ida_hexrays.decompile(f.start_ea))
    except Exception as e:
        print("decompile failed: %r" % e)
