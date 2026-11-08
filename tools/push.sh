#!/usr/bin/env bash
# push.sh — run a script in a live Boblox client via its script socket.
#   ./tools/push.sh script.lua
#   echo 'print(1)' | ./tools/push.sh -
set -euo pipefail
SOCK="${BOBLOX_SOCKET:-/tmp/boblox.sock}"
[ -S "$SOCK" ] || { echo "[error] $SOCK not up — start the client with BOBLOX_SOCKET=$SOCK" >&2; exit 1; }
python3 - "$SOCK" "${1:--}" <<'PY'
import socket, sys
path, src = sys.argv[1], sys.argv[2]
data = sys.stdin.buffer.read() if src == '-' else open(src, 'rb').read()
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(path); s.sendall(data); s.shutdown(socket.SHUT_WR); s.close()
PY
echo "[push] sent"
