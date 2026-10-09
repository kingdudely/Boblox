#!/usr/bin/env python3
"""extract_2016_api.py — inventory the DataModel scripting surface from the
ROBLOX 2016 source tree (used as REFERENCE ONLY, never committed).

Reads the reflection registrations out of the 2016 tree:
  headers  App/include/v8datamodel/*.h   — class list, superclass (the
           Described<Cls, Super, sName> base), typed prop_* declarations
  sources  App/v8datamodel/*.cpp         — PropDescriptor/EnumPropDescriptor
           (script name, C++ type, getter/setter, attributes),
           BoundFuncDesc/CustomBoundFuncDesc (script name, C++ signature),
           event_* registrations (script name), EnumDesc addPair tables,
           sName = "ScriptName" resolutions.

Emits api2016.json: {classes: [{name, super, header, props[], methods[],
events[], enums[]}]}. Property types are raw 2016 C++ types — mapping them
onto our Variant/PropType system is the registry's job (unmapped types are
reported, not guessed).

  ./tools/extract_2016_api.py [2016-tree] [out.json]

Only API FACTS (names, types, signatures) are extracted — no proprietary
code is copied. The tree itself is never committed (see .gitignore).
"""
import json
import os
import re
import sys

TREE = sys.argv[1] if len(sys.argv) > 1 else "/tmp/opencode/roblox2016"
OUT = sys.argv[2] if len(sys.argv) > 2 else "run/api2016.json"

INC = os.path.join(TREE, "App/include/v8datamodel")
SRC = os.path.join(TREE, "App/v8datamodel")

CLASS_RE = re.compile(
    r"class\s+(\w+)\s*\n?\s*:\s*public\s+(Described\w*)\s*<\s*\1\s*,\s*(\w+)\s*,\s*(\w+)\s*>")
SNAME_RE = re.compile(r'\b(s\w+)\s*=\s*"([^"]+)"')
# PropDescriptor<Cls, Type> [Cls::]prop_X("Script", cat, &get, &setOrNULL, attrs...)
PROP_RE = re.compile(
    r"(?:const\s+)?(?:Enum)?PropDescriptor\s*<\s*\w+\s*,\s*([^>]+)>\s+"
    r"(?:\w+::)?(prop_\w+)\s*\(\s*\"([^\"]+)\"\s*,\s*(\w+)\s*,\s*([^,]+?)\s*,\s*"
    r"(NULL|&[\w:]+)\s*(?:,\s*(.*?))?\)\s*;")
# BoundFuncDesc<Cls, Sig>(...) / CustomBoundFuncDesc<Cls, Sig>(...) "Script"
FUNC_RE = re.compile(
    r"(?:Custom)?BoundFuncDesc\s*<\s*\w+\s*,\s*(.+?)>\s+\w+\s*\([^)]*?"
    r"\"([^\"]+)\"")
# event_Touched / dep_Touched / func_getTouchingParts style: (... , "Script"[, ...])
EVENT_RE = re.compile(
    r"(?:>|\b)(?:event|dep|func)_(\w+)\s*\([^;]*?\"([^\"]+)\"")
ENUM_PAIR_RE = re.compile(r"addPair\s*\(\s*[\w:]+\s*,\s*\"([^\"]+)\"\s*\)")
ENUM_DESC_RE = re.compile(
    r"EnumDesc\s*<\s*([\w:]+)\s*>\s*::\s*EnumDesc\s*\(\s*\)\s*"
    r":\s*EnumDescriptor\s*\(\s*\"([^\"]+)\"\s*\)")


def sname_map():
    """sVar -> script class name, scraped from all sources + headers."""
    m = {}
    for d in (INC, SRC):
        if not os.path.isdir(d):
            continue
        for f in os.listdir(d):
            if not f.endswith((".h", ".cpp")):
                continue
            try:
                txt = open(os.path.join(d, f), encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            for var, name in SNAME_RE.findall(txt):
                # definitions look like: const char* const sPart = "Part"
                m.setdefault(var, name)
    return m


def parse_header(path, snames):
    txt = open(path, encoding="utf-8", errors="replace").read()
    m = CLASS_RE.search(txt)
    if not m:
        return None
    cls, _described, super_, svar = m.groups()
    props = []
    for pm in re.finditer(
            r"static\s+const\s+Reflection::(?:Enum)?PropDescriptor<\s*\w+\s*,\s*([^>]+)>\s+(prop_\w+)\s*;",
            txt):
        props.append({"decl": pm.group(2).strip(), "ctype": pm.group(1).strip()})
    return {"name": snames.get(svar, None), "class": cls, "super": super_,
            "svar": svar, "header_props": props, "file": os.path.basename(path)}


def parse_source(path):
    txt = open(path, encoding="utf-8", errors="replace").read()
    props, methods, events, enums = [], [], [], []
    for m in PROP_RE.finditer(txt):
        ctype, _decl, script, _cat, _get, setter, attrs = m.groups()
        props.append({"name": script, "ctype": ctype.strip(),
                      "readonly": setter.strip() == "NULL",
                      "attrs": (attrs or "").strip()})
    for m in FUNC_RE.finditer(txt):
        sig, script = m.groups()
        methods.append({"name": script, "sig": " ".join(sig.split())})
    for m in EVENT_RE.finditer(txt):
        _declpfx, script = m.groups()
        if script not in ("None", ""):
            events.append({"name": script})
    for m in ENUM_DESC_RE.finditer(txt):
        _ctype, ename = m.groups()
        enums.append({"name": ename})
    # enum items per EnumDescriptor block (approx: nearest preceding name)
    items = {}
    for m in ENUM_DESC_RE.finditer(txt):
        block = txt[m.start():m.start() + 4000]
        items[m.group(2)] = ENUM_PAIR_RE.findall(block)
    for e in enums:
        e["items"] = items.get(e["name"], [])
    return props, methods, events, enums


def main():
    if not os.path.isdir(INC):
        sys.exit(f"2016 tree not found at {TREE} (pass the path explicitly)")
    snames = sname_map()
    classes = []
    for f in sorted(os.listdir(INC)):
        if not f.endswith(".h"):
            continue
        h = parse_header(os.path.join(INC, f), snames)
        if not h:
            continue
        src = os.path.join(SRC, f[:-2] + ".cpp")
        props, methods, events, enums = ([], [], [], [])
        if os.path.isfile(src):
            props, methods, events, enums = parse_source(src)
        # de-dupe events by script name (decl + deprecated twins share it)
        seen, uevents = set(), []
        for e in events:
            if e["name"] not in seen:
                seen.add(e["name"])
                uevents.append(e)
        classes.append({"name": h["name"], "class": h["class"], "super": h["super"],
                        "header": h["file"], "has_source": os.path.isfile(src),
                        "props": props, "methods": methods, "events": uevents,
                        "enums": enums})
    # type coverage report (raw 2016 C++ types across all properties)
    types = {}
    for c in classes:
        for p in c["props"]:
            types[p["ctype"]] = types.get(p["ctype"], 0) + 1
    out = {"classes": classes, "class_count": len(classes),
           "with_source": sum(1 for c in classes if c["has_source"]),
           "unnamed": sum(1 for c in classes if not c["name"]),
           "property_ctypes": dict(sorted(types.items(), key=lambda kv: -kv[1]))}
    os.makedirs(os.path.dirname(OUT) or ".", exist_ok=True)
    json.dump(out, open(OUT, "w"), indent=1)
    n_props = sum(len(c["props"]) for c in classes)
    n_methods = sum(len(c["methods"]) for c in classes)
    n_events = sum(len(c["events"]) for c in classes)
    print(f"classes={len(classes)} with_source={out['with_source']} unnamed={out['unnamed']}")
    print(f"props={n_props} methods={n_methods} events={n_events}")
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
