#!/usr/bin/env python3
"""ida_run.py — open the libroblox IDA database and execute a script's code.

Unlike ida_mcp.py (which assumes a database is already open in the current
MCP session), this helper always acquires the database first, so it can be
invoked as a one-shot from the shell:

    python3 tools/ida_run.py script.py     # run a file
    python3 tools/ida_run.py -c 'code'     # run inline code
"""
import importlib.util
import sys

spec = importlib.util.spec_from_file_location("ida_mcp", "tools/ida_mcp.py")
_m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(_m)

DB_PATH = "/home/john/rr/libroblox.so"

code = sys.argv[2] if len(sys.argv) > 1 and sys.argv[1] == "-c" else sys.stdin.read()

c = _m.Mcp()
c.init()
c.call("open_database", {"path": DB_PATH})
print(c.call("execute_python", {"code": code}))
