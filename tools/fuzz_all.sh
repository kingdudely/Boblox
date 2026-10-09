#!/bin/bash
# fuzz_all.sh — Phase K driver: build the libFuzzer harnesses (clang) and run
# each one over a corpus assembled from the tracked fixtures.
#
#   ./tools/fuzz_all.sh [seconds-per-target]   (default 120)
#
# Corpus (all derived from tracked files — nothing binary is checked in):
#   blob         raw 0x9B messages: run/oracle_challenge.bin, run/*/wire_chal*.bin
#   standardize  wire programs: run/*/wire.bin, tests/vectors/*.wire.bin
#   frames       framed oracle: compactVarint(len)+oracle_challenge, junk-prefix
#                and truncated variants
# Crash artifacts land in fuzz/artifacts/ (gitignored).
set -u
SECS="${1:-120}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-fuzz"
CORPUS="$ROOT/fuzz/corpus"
ART="$ROOT/fuzz/artifacts"

command -v clang++ >/dev/null || { echo "clang++ required (libFuzzer)"; exit 1; }

cmake -S "$ROOT/fuzz" -B "$BUILD" -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=Release >/dev/null || exit 1
cmake --build "$BUILD" -j || exit 1

rm -rf "$CORPUS" && mkdir -p "$CORPUS/blob" "$CORPUS/standardize" "$CORPUS/frames" "$ART"

cp "$ROOT"/run/oracle_challenge.bin "$ROOT"/run/*/wire_chal*.bin "$CORPUS/blob/" 2>/dev/null
cp "$ROOT"/run/*/wire.bin "$ROOT"/tests/vectors/*.wire.bin "$CORPUS/standardize/" 2>/dev/null

python3 - "$ROOT/run/oracle_challenge.bin" "$CORPUS/frames" <<'EOF'
import sys
msg = open(sys.argv[1], 'rb').read()
outdir = sys.argv[2]
def cv(v):  # compactVarint (client/src/util.h)
    if v < 0x40: return bytes([v])
    if v < 0x4000: return bytes([0x40 | (v >> 8), v & 0xff])
    if v < 0x40000000: return bytes([0x80 | (v >> 24), (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff])
    return bytes([0xC0 | (v >> 56), (v >> 48) & 0xff, (v >> 40) & 0xff, (v >> 32) & 0xff,
                  (v >> 24) & 0xff, (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff])
open(f"{outdir}/framed", 'wb').write(cv(len(msg)) + msg)
open(f"{outdir}/junk_prefixed", 'wb').write(b"\x05hello" + cv(len(msg)) + msg)
open(f"{outdir}/two_frames", 'wb').write(cv(3) + b"abc" + cv(len(msg)) + msg)
open(f"{outdir}/truncated", 'wb').write((cv(len(msg)) + msg)[:len(msg) // 2])
open(f"{outdir}/zero_frames", 'wb').write(b"\x00\x00\x00" + cv(len(msg)) + msg)
EOF

echo "corpus: blob=$(ls "$CORPUS/blob" | wc -l) standardize=$(ls "$CORPUS/standardize" | wc -l) frames=$(ls "$CORPUS/frames" | wc -l)"

pass=1
run_one() {
    local t="$1"; shift
    echo "--- fuzz_$t (${SECS}s) ---"
    # No pipe (it would mask libFuzzer's exit status): log to a file, tail it.
    (cd "$ART" && "$BUILD/fuzz_$t" -max_total_time="$SECS" -timeout=10 \
        -rss_limit_mb=512 "$@" "$CORPUS/$t" >"$ART/fuzz_$t.log" 2>&1)
    local rc=$?
    tail -4 "$ART/fuzz_$t.log"
    # Nonzero exit (crash/hang/OOM) OR stray artifacts both fail the run.
    if [ "$rc" -ne 0 ]; then
        echo "fuzz_$t exited rc=$rc (see $ART/fuzz_$t.log)"; pass=0
    fi
    if ls "$ART"/crash-* "$ART"/timeout-* "$ART"/oom-* >/dev/null 2>&1; then
        echo "FINDINGS in fuzz/artifacts/ for $t"; pass=0
    fi
}
run_one blob
run_one standardize -max_len=16384
run_one frames

if [ "$pass" = 1 ]; then echo "FUZZ CLEAN: no crashes, hangs, or OOMs"; else exit 1; fi
