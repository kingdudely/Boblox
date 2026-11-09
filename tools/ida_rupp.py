"""ida_rupp.py — locate the Rupp token subsystem and the 0x83/21 sender.

Runs inside the already-analysed IDA database (see tools/ida_x.py). It reports,
for every "Rupp"/"TokenGen"/"Pepper" log string, the string address, the
functions that xref it, and those functions' sizes. The token generator and the
60 s attestation sender are the two unknowns left for the 304, and both are
reachable from these channel names.
"""
import idautils
import ida_funcs
import ida_hexrays
import ida_lines
import idc


def xref_funcs(ea):
    out = []
    for x in idautils.XrefsTo(ea, 0):
        f = ida_funcs.get_func(x.frm)
        out.append((x.frm, f.start_ea if f else 0,
                    idc.get_func_name(f.start_ea) if f else "?"))
    return out


def main():
    want = ("Rupp", "TokenGen", "Pepper", "RuppToken", "Stemma")
    seen = {}
    for s in idautils.Strings():
        try:
            text = str(s)
        except Exception:
            continue
        if not any(w in text for w in want):
            continue
        if "[DFLog::" not in text and "Rupp" not in text and "Stemma" not in text:
            continue
        for xa, fstart, fname in xref_funcs(s.ea):
            seen.setdefault(fstart, []).append((text[:90], xa))
    print("functions referencing Rupp/token strings: %d" % len(seen))
    for fstart, hits in sorted(seen.items()):
        f = ida_funcs.get_func(fstart)
        size = (f.end_ea - f.start_ea) if f else 0
        print("\n%s @ %#x size=%d" % (idc.get_func_name(fstart), fstart, size))
        for text, xa in hits[:4]:
            print("    %#x  %s" % (xa, text))


main()
