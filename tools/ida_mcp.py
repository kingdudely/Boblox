#!/usr/bin/env python3
"""ida_mcp.py — run Python against the open IDA database via the IDA MCP server.

Speaks the MCP stdio protocol to the same server opencode launches:

    uvx --exclude-newer=1s --with ida-nexus==0.13.4 ida-mcp stdio

Usage:
    python3 tools/ida_mcp.py script.py     # run a file
    python3 tools/ida_mcp.py -c 'code'     # run a one-liner
    python3 tools/ida_mcp.py --list        # list MCP tools

The database must be open in IDA (the nexus instance metadata in
~/.idapro/nexus/instances/*.json is how the server finds it).
"""
import json
import subprocess
import sys
import time

# Must match the server opencode launches (see ~/.config/opencode/opencode.json):
# ida-nexus 0.13.x cannot see a database opened by the 0.14.2 session, so a
# mismatch answers "no open database instance" even while IDA has one loaded.
CMD = ["/home/john/.local/bin/uvx", "--with", "ida-nexus==0.14.2",
       "ida-mcp==20261028.0.3", "stdio"]


class Mcp:
    def __init__(self, cmd=CMD):
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.framed = None  # None = unknown, False = newline JSON, True = LSP framing

    def send(self, obj):
        data = json.dumps(obj)
        if self.framed:
            body = data.encode()
            self.p.stdin.write(f"Content-Length: {len(body)}\r\n\r\n{data}")
        else:
            self.p.stdin.write(data + "\n")
        self.p.stdin.flush()

    def recv(self, timeout=180):
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.p.stdout.readline()
            if not line:
                return None
            line = line.strip()
            if not line:
                continue
            if line.lower().startswith("content-length:"):
                self.framed = True
                n = int(line.split(":")[1])
                while True:  # skip headers
                    h = self.p.stdout.readline()
                    if not h or not h.strip():
                        break
                return json.loads(self.p.stdout.read(n))
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                continue
            if self.framed is None:
                self.framed = False
            return msg
        return None

    def request(self, msg_id, method, params=None):
        msg = {"jsonrpc": "2.0", "id": msg_id, "method": method}
        if params is not None:
            msg["params"] = params
        self.send(msg)
        while True:
            r = self.recv()
            if r is None:
                return None
            if r.get("id") == msg_id:
                return r

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self.send(msg)

    def init(self):
        self.request(1, "initialize", {
            "protocolVersion": "2024-11-05", "capabilities": {},
            "clientInfo": {"name": "buffy", "version": "1"},
        })
        self.notify("notifications/initialized")

    def call(self, name, args, msg_id=2):
        r = self.request(msg_id, "tools/call", {"name": name, "arguments": args})
        if not r:
            return "no response from the IDA MCP server"
        if "error" in r:
            return json.dumps(r["error"], indent=2)
        result = r.get("result", {})
        out = [b.get("text", "") for b in result.get("content", []) or []
               if b.get("type") == "text"]
        if not out and result.get("structuredContent"):
            out.append(json.dumps(result["structuredContent"], indent=2))
        return "\n".join(out)


def main():
    args = sys.argv[1:]
    m = Mcp()
    m.init()
    if args and args[0] == "--list":
        r = m.request(2, "tools/list")
        for t in r.get("result", {}).get("tools", []):
            print(f"{t['name']}: {t.get('description', '')[:110]}")
        return
    code = args[1] if args and args[0] == "-c" else sys.stdin.read()
    print(m.call("execute_python", {"code": code}))


if __name__ == "__main__":
    main()
