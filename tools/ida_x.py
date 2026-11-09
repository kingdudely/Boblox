#!/usr/bin/env python3
"""ida_x.py — execute Python in the IDA database that is open in the IDA GUI.

`tools/ida_mcp.py` drives the MCP server, which needs `open_database` first and
re-analyses the 121 MB library when it does. This talks to the nexus HTTP
endpoint the GUI already serves, through the same client library
(ida-nexus 0.14.2), so a script runs against the loaded, analysed database:

    uv run --offline --with ida-nexus==0.14.2 python tools/ida_x.py script.py
    uv run --offline --with ida-nexus==0.14.2 python tools/ida_x.py -c 'code'

IDA_TIMEOUT (seconds, default 300) bounds the execution.
"""
import json
import os
import sys

from ida_nexus.manager import DatabaseManager


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    if sys.argv[1] == "-c":
        code = sys.argv[2]
    else:
        with open(sys.argv[1], "r", encoding="utf-8") as f:
            code = f.read()

    mgr = DatabaseManager(execute_timeout=float(os.environ.get("IDA_TIMEOUT", "300")))
    # Attaches to the instance the GUI already has open (status "available" in
    # list_databases) instead of re-analysing a 121 MB library.
    path = os.environ.get("IDA_DB", "/home/john/rr/libroblox.so")
    opened = mgr.open_database(path, set_current=True)
    if os.environ.get("IDA_LIST"):
        print(json.dumps(opened, indent=2, default=str))
    out = mgr.execute_python(code, None)
    print(json.dumps(out, indent=2, default=str))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
