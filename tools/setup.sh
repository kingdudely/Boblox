#!/bin/bash
# setup.sh — one-shot environment bootstrap (run once per machine).
#
#   ./tools/setup.sh
#
# Does two things:
#   1. checks out the pinned third-party submodules, and
#   2. installs the Python QUIC stack (aioquic) needed by the LIVE probes
#      in py/ (real server joins) and RE tools (capture decryption).
#
# The TEST battery needs none of this: message builders live in the
# stdlib-only py/msgbuild.py and handshake captures ship frozen
# (run/cpp_tx/*.hs.bin), so ctest goes green on stdlib + compilers alone.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

echo "--- submodules ---"
git submodule update --init --recursive || exit 1

echo "--- python live-probe deps ---"
if python3 -c "import aioquic" 2>/dev/null; then
    echo "aioquic already present"
else
    python3 -m pip install -r py/requirements.txt \
        || python3 -m pip install --break-system-externally-allowed \
            -r py/requirements.txt || {
        echo "pip install failed (no network? no pip?) — live probes unavailable;"
        echo "the test battery still works (stdlib only)."
        exit 1
    }
fi
python3 -c "import aioquic; print('aioquic OK:', aioquic.__version__ if hasattr(aioquic, '__version__') else 'present')"

command -v node >/dev/null || echo "NOTE: node not found — extension-codec test will fail (see README)"
command -v cmake >/dev/null || echo "NOTE: cmake not found — required to build"
echo "setup complete"
