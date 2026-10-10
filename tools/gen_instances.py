#!/usr/bin/env python3
"""gen_instances.py — Mini-API-Dump (tools/api_dump.json) -> generated
DataModel class registrations (rbx_runtime/src/instance/classes/generated.cpp).

Same discipline as tools/gen_tables_inc.py: the JSON fixture is the source
of truth; the generated file is checked in; `ctest instances-parity` runs
this with --check and fails on drift. Regenerate with no arguments.

Coverage rules (deliberately conservative — behavior comes later):
  * every dump class EXCEPT the hand-overridden {Instance, Part, Workspace}
    gets a registration (superclass resolved from the dump, topo-sorted so
    supers register first; unknown supers fall back to Instance + report).
  * properties: scriptable members only (NotScriptable skipped; WriteOnly
    skipped — our glue has no write-only concept yet). Mappable types:
    bool/int/int64/float/double/string, Vector3, any Class reference
    (-> Instance). Everything else (Color3, CFrame, enums, Content, ...)
    is skipped and REPORTED — that report is the unlock list for the next
    value types. ReadOnly tag -> readonly.
  * Name always defaults to the class name (Roblox parity: instances are
    born named after their class) — forced on every generated class, since
    most classes inherit an unusable root default instead of declaring one.
  * creatable = no NotCreatable tag. instance_new enforces it with the
    native message ("Unable to create an Instance of type 'X'").
  * methods/events are emitted as STUBS (name only): the member exists
    (typeof gives "function" / signals connect) but calling a method raises
    "Cls.Member is not implemented". Hand behavior attaches real fns by name
    (attach_method / event_signal firing) — same registry, no second tree.
  * Service-tagged classes (except RunService, which stays a hand table for
    the frozen Challenge profile, and Workspace, owned by engine setup) are
    emitted as register_service_singletons(): Engine-profile singletons
    behind game:GetService. Challenge states keep the fresh-table fallback.

Only API FACTS (names, types, hierarchy) flow into the output.
"""
import json
import os
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)  # repo root whatever the CWD (ctest runs elsewhere)
FIXTURE = os.path.join(ROOT, "tools/api_dump.json")
OUT = os.path.join(ROOT, "rbx_runtime/src/instance/classes/generated.cpp")
SOURCE_NOTE = ("Mini-API-Dump (MaximumADHD/Roblox-Client-Tracker, roblox branch), "
               "pinned 2026-10-09")
OVERRIDES = {"Instance", "Part", "Workspace"}  # hand-owned in classes* (behavior)
SKIP = {"Object"}  # dump-internal root above Instance; not a script class
ROOT_SUPERS = {None, "Object", "Instance", "<<<ROOT>>>"}  # -> hand Instance root

# (Category, Name) -> (PropType, C++ default expr)
TYPES = {
    ("Primitive", "bool"): ("Bool", "false"),
    ("Primitive", "int"): ("Int", "int64_t(0)"),
    ("Primitive", "int64"): ("Int", "int64_t(0)"),
    ("Primitive", "float"): ("Double", "0.0"),
    ("Primitive", "double"): ("Double", "0.0"),
    ("Primitive", "string"): ("String", 'std::string()'),
    ("DataType", "Vector3"): ("Vector3", "Vector3{0.0, 0.0, 0.0}"),
    ("DataType", "Color3"): ("Color3", "Color3{}"),
    ("DataType", "CFrame"): ("CFrame", "CFrame{}"),
    ("DataType", "Vector2"): ("Vector2", "Vector2{}"),
    ("DataType", "BrickColor"): ("BrickColor", "BrickColor{}"),
    ("DataType", "UDim"): ("UDim", "UDim{}"),
    ("DataType", "UDim2"): ("UDim2", "UDim2{}"),
    ("DataType", "Rect"): ("Rect", "Rect{}"),
    ("DataType", "NumberRange"): ("NumberRange", "NumberRange{}"),
    ("DataType", "NumberSequence"): ("NumberSequence", "NumberSequence{}"),
    ("DataType", "ColorSequence"): ("ColorSequence", "ColorSequence{}"),
    ("DataType", "Content"): ("Content", "Content{}"),
    ("DataType", "PhysicalProperties"): ("PhysicalProperties", "PhysicalProperties{}"),
    ("DataType", "Ray"): ("Ray", "Ray{}"),
    ("DataType", "Region3"): ("Region3", "Region3{}"),
    ("DataType", "DateTime"): ("DateTime", "DateTime{}"),
    # String-compatible datatypes: ContentId is an asset-ID string;
    # BinaryString/SharedString are byte blobs and Lua strings are byte-safe
    # (embedded NULs round-trip through lua_pushlstring/lua_tolstring), so a
    # std::string Variant is faithful for property VALUES. (Sharing/dedup
    # semantics of SharedString don't apply to values.)
    ("DataType", "ContentId"): ("String", 'std::string()'),
    ("DataType", "BinaryString"): ("String", 'std::string()'),
    ("DataType", "SharedString"): ("String", 'std::string()'),
}


def is_str_list(tags):
    return isinstance(tags, list)


def has_tag(tags, name):
    return is_str_list(tags) and name in tags


def cpp_str(s):
    """C++ string literal body (property names can contain quotes: `"TODO" Color`)."""
    return s.replace("\\", "\\\\").replace('"', '\\"')


def main(check=False):
    with open(FIXTURE) as f:
        dump = json.load(f)
    classes = [c for c in dump["Classes"]
               if c["Name"] not in OVERRIDES and c["Name"] not in SKIP]
    by_name = {c["Name"]: c for c in classes}
    # Overridden supers resolve at runtime: hand classes register BEFORE the
    # generated batch (see classes.cpp), so Seat:Part etc. stay intact.
    resolvable = set(by_name) | OVERRIDES

    skipped_types = defaultdict(int)
    skipped_nonscript = 0
    skipped_writeonly = 0
    fallback_supers = []
    n_methods = n_events = 0

    shells, links = [], []
    services = []
    n_props = 0
    # Dump order is fine: linking happens by name AFTER all shells exist, so
    # no topological sort is needed (and none could express Seat:Part +
    # Part:FormFactorPart across the hand/generated split anyway).
    for c in classes:
        name = c["Name"]
        sup = c.get("Superclass")
        if sup in ROOT_SUPERS:
            sup = "Instance"  # hand-registered root always exists
        elif sup not in resolvable:
            fallback_supers.append((name, sup))
            sup = "Instance"
        tags = c.get("Tags") or []
        creatable = not has_tag(tags, "NotCreatable")
        if has_tag(tags, "Service") and name not in ("RunService", "Workspace"):
            services.append(name)
        props, declared = [], set()
        methods, mevents = [], []
        seen_methods, seen_events = set(), set()
        for m in c.get("Members", []):
            mt = m.get("MemberType")
            if mt == "Function":
                n_methods += 1
                if m.get("Name") not in seen_methods:
                    seen_methods.add(m.get("Name"))
                    methods.append(m.get("Name"))
                continue
            if mt == "Event":
                n_events += 1
                if m.get("Name") not in seen_events:
                    seen_events.add(m.get("Name"))
                    mevents.append(m.get("Name"))
                continue
            if mt == "Callback":
                continue
            if mt != "Property":
                continue
            ptags = m.get("Tags") or []
            if has_tag(ptags, "NotScriptable"):
                skipped_nonscript += 1
                continue
            if has_tag(ptags, "WriteOnly"):
                skipped_writeonly += 1
                continue
            vt = m.get("ValueType", {})
            key = (vt.get("Category"), vt.get("Name"))
            pname = m.get("Name")
            if key[0] == "Class":
                propt, default = "Instance", "static_cast<Instance*>(nullptr)"
            elif key in TYPES:
                propt, default = TYPES[key]
            else:
                skipped_types[key] += 1
                continue
            if pname == "Name" and propt == "String":
                default = f'std::string("{cpp_str(name)}")'  # Roblox parity
            readonly = "true" if has_tag(ptags, "ReadOnly") else "false"
            props.append(
                f'        PropInfo{{"{cpp_str(pname)}", PropType::{propt}, {default}, {readonly}}},')
            declared.add(pname)
            n_props += 1
        if "Name" not in declared:
            # Instances are born named after their class (see docstring);
            # appended last = most-derived within this class's own props.
            props.append(f'        PropInfo{{"Name", PropType::String, std::string("{cpp_str(name)}"), false}},')
            n_props += 1
        shells.append(
            "    { ClassInfo c;\n"
            f'      c.name = "{cpp_str(name)}";\n'
            "      c.props = {\n" + "\n".join(props) + ("\n" if props else "") +
            "      };\n" +
            ("      c.methods = {\n" + "".join(
                f'        MethodInfo{{"{cpp_str(m)}", nullptr}},\n' for m in methods) +
             "      };\n" if methods else "") +
            ("      c.events = {\n" + "".join(
                f'        EventInfo{{"{cpp_str(e)}"}},\n' for e in mevents) +
             "      };\n" if mevents else "") +
            f'      c.creatable = {"true" if creatable else "false"};\n'
            "      register_class(std::move(c)); }\n")
        links.append(f'    link_super("{cpp_str(name)}", "{cpp_str(sup)}");\n')

    body = ("// generated.cpp — DO NOT EDIT. Generated by tools/gen_instances.py\n"
            f"// Source: {SOURCE_NOTE} (see --check parity: ctest instances-parity).\n"
            "// Hand-overridden classes (behavior): Instance, Part, Workspace.\n"
            '#include "instance/class_registry.h"\n'
            '#include "instance/instance.h"\n'
            '#include "api/api.h" // register_service (engine singletons)\n'
            "\n#include <string>\n"
            "\nnamespace rbx {\n"
            "\nvoid register_generated_classes() {\n"
            "    static bool done = false;\n"
            "    if (done)\n"
            "        return;\n"
            "    done = true;\n"
            + "".join(shells) +
            "    // Links run after every shell (hand + generated) exists, so\n"
            "    // registration order never matters (see class_registry.h).\n"
            + "".join(links) +
            "}\n"
            "\n"
            "// Engine-profile service singletons (game:GetService stable identity).\n"
            "// Called from register_engine only — Challenge states keep the\n"
            "// fresh-table fallback, so the frozen solve path never changes.\n"
            "void register_service_singletons(lua_State* L) {\n"
            "    static const char* kServices[] = {\n" +
            "".join(f'        "{cpp_str(s)}",\n' for s in sorted(services)) +
            "    };\n"
            "    for (const char* name : kServices) {\n"
            "        const ClassInfo* cls = find_class(name);\n"
            "        if (!cls)\n"
            "            continue;\n"
            "        Instance::push(L, instantiate(cls).get());\n"
            "        rbxch::api::register_service(L, name);\n"
            "    }\n"
            "}\n"
            "\n} // namespace rbx\n")

    if check:
        with open(OUT) as f:
            current = f.read()
        if current != body:
            print(f"STALE: {OUT} does not match {FIXTURE} (run tools/gen_instances.py)")
            return 1
        print(f"{OUT} in sync with {FIXTURE}")
        return 0

    with open(OUT, "w") as f:
        f.write(body)
    print(f"classes={len(by_name)} props={n_props} methods_seen={n_methods} "
          f"events_seen={n_events}")
    print(f"skipped: nonscriptable={skipped_nonscript} writeonly={skipped_writeonly} "
          f"fallback_supers={len(fallback_supers)}")
    print("skipped property types (unlock list for next value types):")
    for k in sorted(skipped_types, key=lambda k: -skipped_types[k])[:15]:
        print(f"  {k}: {skipped_types[k]}")
    for name, sup in fallback_supers[:10]:
        print(f"  fallback super: {name} (dump super {sup} unknown) -> Instance")
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main(check="--check" in sys.argv[1:]))
