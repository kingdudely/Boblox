import idc
import idautils
import ida_funcs
import ida_nalt


def fname(ea):
    f = ida_funcs.get_func(ea)
    return ida_funcs.get_func_name(f.start_ea) if f else "?"


print("ROOT", ida_nalt.get_root_filename())
terms = (
    "raknet", "network", "camera", "replicat", "packet", "send", "recv",
    "receive", "dispatch", "appdata", "app_data", "client", "player",
)
seen = set()
count = 0
for ea, name in idautils.Names():
    low = name.lower()
    if any(t in low for t in terms):
        print("NAME", hex(ea), name)
        seen.add(ea)
        count += 1
        if count >= 700:
            break
print("NAMES", count)

s_count = 0
for s in idautils.Strings():
    text = str(s)
    low = text.lower()
    if not any(t in low for t in terms):
        continue
    print("STR", hex(s.ea), repr(text)[:220])
    for x in idautils.XrefsTo(s.ea, 0):
        print("  XREF", hex(x.frm), fname(x.frm))
    s_count += 1
    if s_count >= 250:
        break
print("STRINGS", s_count)
idc.qexit(0)
