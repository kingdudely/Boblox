#!/usr/bin/env bash
# Boblox test battery — the standalone suites every change must keep green.
# (The superbuild's `ctest --preset release` adds the doctest unit suites on
# top of these; see README.md.)
# Runs from any CWD. Does NOT build; build first (see README.md):
#   cmake -S rbx_runtime -B rbx_runtime/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build rbx_runtime/build -j
#   cmake -S client -B client/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build client/build -j
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# --- prerequisite checks (friendly errors instead of confusing failures) -----
missing=0
for b in rbx_runtime/build/challenge_runner client/build/regress9b; do
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
suite "solver-regress-cpp    client/build/regress9b" \
  ./client/build/regress9b "$ROOT"
# 3. ClientHello byte-parity vs the native client's captured handshake.
suite "handshake-parity      tools/ch_diff.py" \
  python3 tools/ch_diff.py run/cpp_tx/tx001.bin
# 4. Session messages byte-parity vs the Python builders (earlyauth/m8a/m90).
suite "message-parity        tools/msg_parity.py" \
  python3 tools/msg_parity.py run/join_full.json run/cpp_msgs
# 5. Browser-extension WebTransport codec unit tests.
suite "extension-codec       test/codec.test.mjs" \
  node test/codec.test.mjs
# 6. Golden per-stage vectors stay in sync with the tracked captures.
suite "vectors-py            tools/gen_vectors.py --check" \
  python3 tools/gen_vectors.py --check
# 7. tables.inc stays in sync with tools/roblox_bc_tables.json.
suite "tables-parity         tools/gen_tables_inc.py --check" \
  python3 tools/gen_tables_inc.py --check

printf '\n----------------------------------------\n'
if [ "$fails" -eq 0 ]; then
  echo "ALL SUITES PASS"
else
  echo "$fails SUITE(S) FAILED"
  exit 1
fi
