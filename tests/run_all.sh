#!/usr/bin/env bash
# Boblox test battery — the standalone suites every change must keep green.
# (The superbuild's `ctest --preset release` adds the doctest unit suites on
# top of these; see README.md.)
# Runs from any CWD. Binaries resolve from the superbuild (./build/...) with
# fallback to standalone project builds (client/build, rbx_runtime/build),
# so both documented build flows work:
#   cmake --preset release && cmake --build --preset release -j   (superbuild)
#   cmake -S client -B client/build && cmake --build client/build -j  (standalone)
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# --- binary resolution (superbuild first, standalone fallback) --------------
resolve() { # $1 = superbuild path, $2 = standalone path -> prints the one that exists
  if [ -x "$1" ]; then printf '%s' "$1";
  else printf '%s' "$2"; fi
}
CHALLENGE_RUNNER="$(resolve build/rbx_runtime/challenge_runner rbx_runtime/build/challenge_runner)"
REGRESS9B="$(resolve build/client/regress9b client/build/regress9b)"

# --- prerequisite checks (friendly errors instead of confusing failures) -----
missing=0
for b in "$CHALLENGE_RUNNER" "$REGRESS9B"; do
  if [ ! -x "$b" ]; then
    echo "missing binary: $b — build first (commands above)"
    missing=1
  fi
done
[ "$missing" -eq 0 ] || exit 2

fails=0
suite() {
  local name="$1"; shift
  printf '\n=== %s ===\n' "$name"
  if "$@"; then
    printf 'PASS  %s\n' "$name"
  else
    printf 'FAIL  %s\n' "$name"
    fails=$((fails + 1))
  fi
}

# 1. Python reference solver over the 5 captured native datasets
#    (blob -> answer must equal what the native client itself sent).
suite "solver-regress-py     tools/regress9b.py" \
  python3 tools/regress9b.py
# 2. Same 5 datasets through the C++ in-process path (rbx_solve), which is
#    what the live client uses (the Python path is the reference only).
suite "solver-regress-cpp    $REGRESS9B" \
  "$REGRESS9B" "$ROOT"
# 3. ClientHello byte-parity vs the native client's captured handshake.
suite "handshake-parity      tools/ch_diff.py" \
  python3 tools/ch_diff.py run/cpp_tx/tx001.bin
# 4. Session messages byte-parity vs the Python builders (earlyauth/m8a/m90).
suite "message-parity        tools/msg_parity.py" \
  python3 tools/msg_parity.py run/join_full.json run/cpp_msgs
# 5. Golden per-stage vectors stay in sync with the tracked captures.
suite "vectors-py            tools/gen_vectors.py --check" \
  python3 tools/gen_vectors.py --check
# 6. tables.inc stays in sync with tools/roblox_bc_tables.json.
suite "tables-parity         tools/gen_tables_inc.py --check" \
  python3 tools/gen_tables_inc.py --check

printf '\n----------------------------------------\n'
if [ "$fails" -eq 0 ]; then
  echo "ALL SUITES PASS"
else
  echo "$fails SUITE(S) FAILED"
  exit 1
fi
