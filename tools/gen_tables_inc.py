#!/usr/bin/env python3
"""Generate client/src/tables.inc from tools/roblox_bc_tables.json.

tools/roblox_bc_tables.json is the single source of truth for the wire
opcode tables (REMAP/OPLEN); the .inc is derived from it byte-for-byte:

  python3 tools/gen_tables_inc.py            # rewrite client/src/tables.inc
  python3 tools/gen_tables_inc.py --check    # verify in sync (ctest `tables-parity`)

The build keeps using the checked-in .inc (no build-time codegen), and the
ctest parity test fails if the JSON is edited without regenerating — so the
two can never drift. Semantic correctness of the values is locked by the
solver regressions (any table change breaks standardize -> wrong answers).
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
JSON = os.path.join(ROOT, "tools", "roblox_bc_tables.json")
INC = os.path.join(ROOT, "client", "src", "tables.inc")

HEADER = "#pragma once\n#include <cstdint>"
GEN_NOTE = "// generated from tools/roblox_bc_tables.json — do not edit"


def section(name, comment, values):
    if len(values) != 256 or any(not isinstance(v, int) or not 0 <= v <= 255 for v in values):
        raise SystemExit(f"{name}: expected 256 ints in [0,255]")
    lines = [
        f"// {comment}",
        GEN_NOTE,
        f"static const uint8_t {name}[256] = {{",
    ]
    for i in range(0, 256, 8):
        row = ", ".join(f"{v:3d}" for v in values[i:i + 8])
        lines.append(f"    {row},")
    lines.append("};")
    return "\n".join(lines)


def generate():
    t = json.load(open(JSON))
    parts = [
        HEADER,
        section("REMAP", "wire opcode -> internal opcode (loader transform)", t["remap"]),
        section("OPLEN", "internal opcode -> standard Luau opcode", t["oplen"]),
    ]
    return "\n\n".join(parts) + "\n"


def main():
    want = generate()
    if "--check" in sys.argv:
        try:
            with open(INC, "r", encoding="utf-8") as f:
                got = f.read()
        except FileNotFoundError:
            print(f"MISSING {INC}")
            return 1
        if got != want:
            print("tables.inc OUT OF SYNC with tools/roblox_bc_tables.json —")
            print("run: python3 tools/gen_tables_inc.py")
            return 1
        print("tables.inc in sync with roblox_bc_tables.json (256+256 entries)")
        return 0
    with open(INC, "w", encoding="utf-8") as f:
        f.write(want)
    print(f"wrote {INC}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
