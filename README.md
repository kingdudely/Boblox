# Boblox

A from-scratch client stack for Roblox's protocol, written as a research
project. It joins **real Roblox servers** over QUIC, and solves Roblox's
`0x9B` anti-cheat challenge **fully locally** — blob decode, bytecode
standardization, and execution in a byte-exact Roblox-flavored Luau sandbox
in ~1 ms, with no native client or oracle in the loop. Verified live against
Crossroads (place 1818).

The full reverse-engineering record — protocol notes, byte layouts, what was
cracked and how — lives in **[FINDINGS.md](FINDINGS.md)**. It is the canonical
knowledge base; this README is just orientation.

## Layout

| Path | What lives there |
| --- | --- |
| `client/` | C++ network stack: QUIC (ngtcp2 + OpenSSL), RUPP wrapping, session layer, handshake/message builders. Targets: `rbx_solve`, `rbx_session`, `rbx_transport`, and the test CLIs `regress9b`, `hs1818`, `rbxplay`. |
| `rbx_runtime/` | The Roblox **scripting environment**: hosts the Luau VM and provides Roblox globals (`game`, `RunService`, `UserSettings`, `Random`, …) + the sandbox rules. Modular: one file per API surface, see below. |
| `py/` | Python reference solver (`solve9b.py`) and the RE probes used against live servers. Research tooling — the hot path is C++. |
| `tools/` | Test harnesses (`regress9b.py`, `ch_diff.py`, `msg_parity.py`), bytecode RE tools, and gdb capture scripts. |
| `test/` | Browser-extension WebTransport codec tests (`codec.test.mjs`) and a test server. |
| `src/`, `manifest.json` | The `RBXWeb` browser-extension experiments (WebTransport bridge). |
| `third_party/` | Git submodules: `ngtcp2` (QUIC), `luau` (pinned; never modified). |
| `run/` | Captured datasets and fixtures (mostly gitignored, a few whitelisted for tests). **Account cookies live here and are never committed.** |
| `tests/` | CTest wiring for the whole battery, `run_all.sh` (standalone flow), doctest unit suites (`tests/unit/`), golden stage vectors (`tests/vectors/`). |

## Build

```sh
git clone --recurse-submodules https://github.com/kingdudely/Boblox
cd Boblox

cmake --preset release          # superbuild: runtime + client + tests
cmake --build --preset release -j
```

Presets: `release` (build dir `./build`), `debug` (`./build-debug`),
`asan` (Debug + ASan/UBSan, `./build-asan`).

The per-project entry points still configure and build standalone (the
superbuild is additive):

```sh
cmake -S rbx_runtime -B rbx_runtime/build -DCMAKE_BUILD_TYPE=Release
cmake --build rbx_runtime/build -j

cmake -S client -B client/build -DCMAKE_BUILD_TYPE=Release
cmake --build client/build -j
```

Dependencies: CMake ≥ 3.16 (≥ 3.21 for the presets), a C++17 compiler,
OpenSSL, zstd, libcurl, nlohmann/json, Node.js (for the extension codec
tests).

## Test

```sh
ctest --preset release
```

Eleven suites, all of which must stay green:

1. **solver-regress-py** — Python reference solver over 5 captured native
   datasets; answers must equal what the real client sent, byte for byte.
2. **solver-regress-cpp** — the same 5 datasets through the C++ in-process
   path (`rbx_solve`), the one the live client uses.
3. **handshake-parity** — our QUIC ClientHello byte-compared against the
   native client's captured one.
4. **message-parity** — session messages byte-compared against the Python
   builders (earlyauth, 0x8A, 0x90).
5. **extension-codec** — WebTransport framing unit tests for the extension.
6. **unit-runtime** — doctest unit suite running Luau source end-to-end
   through `rbxch::run` (sandbox shims, profile options, Random known-answer).
7. **unit-vectors** — golden per-stage vectors (`tests/vectors/`) through the
   C++ pipeline: extract → decode → standardize → solve, byte-compared stage
   by stage against frozen goldens + the native answers.
8. **unit-messages** — the C++ session message builders vs golden bytes built
   by the Python reference builders.
9. **unit-rupp** — RUPP datagram framing vs a native-captured datagram
   (byte-exact strip/re-wrap round-trip).
10. **vectors-py** — `tools/gen_vectors.py --check`: the goldens stay in sync
    with the tracked capture fixtures.
11. **tables-parity** — `tools/gen_tables_inc.py --check`: `client/src/tables.inc`
    stays in sync with `tools/roblox_bc_tables.json` (regenerate with
    `cmake --build build --target gen_tables`).

`tests/run_all.sh` runs the same standalone suites outside the superbuild
(1–5 plus the two generator-parity checks; the doctest suites 6–9 need the
superbuild build).

## Live run

```sh
./client/build/rbxplay --seconds 45 --a7 real --dummy full \
    --cookie-file run/cookie2.txt
```

Success looks like `challenge=yes answered=yes peer=yes resets=0` followed by
~1 MB of replication traffic. As a control: a deliberately wrong answer gets
exactly one challenge rejected (server drains after ~63 KB), a correct one
streams indefinitely.

Note: back-to-back runs on the *same account* can get a transient early
`ERR_DRAINING` before any challenge arrives (the previous session's kick /
server-side state — see FINDINGS.md). Wait ~20s and retry; it is not a
client-side fault.

## How the 0x9B challenge is solved

```
server blob
  -> XOR-decode + xxhash32 check + zstd        (tools/challenge_blob.py, C++ twin in client/)
  -> wire bytecode -> opcode standardize        (tools/standardize_wire.py, C++ twin in client/)
  -> run in Roblox-flavored Luau sandbox        (rbx_runtime: exact PCG Random, game/UserSettings/…)
  -> answer u32                                 (~1 ms in-process; the server's window is ~16 ms)
```

The Python solver (`py/solve9b.py`) is the readable reference; the C++ path
exists because the server only waits ~16 ms for the answer.

## Adding a Roblox API

`rbx_runtime` is built so the (large) Roblox API surface can grow one surface
at a time — hundreds of Instance classes, services, eventually more:

1. Create `rbx_runtime/src/api/<name>.cpp` with
   `void rbxch::api::register_<name>(lua_State*, const Options&)`.
2. Add one line to the module list in `rbx_runtime/src/api/registry.cpp`.
3. Add the file to `challenge_core` in `rbx_runtime/CMakeLists.txt`.

Services (`workspace`, `Players`, …) also register themselves into the
service table that backs `game:GetService` — `game.cpp` never changes. The
contract, helpers (`api::opts(L)`, `register_service`), and the ordering rules
are documented in `rbx_runtime/src/api/api.h`. Run `tests/run_all.sh` after
any change: the captured challenge programs genuinely exercise these shims.

## Notes

- The Luau checkout under `third_party/luau` is pinned and **never modified**;
  everything Roblox-specific lives in `rbx_runtime/`.
- Account credentials (`.ROBLOSECURITY` cookies) must never be committed;
  `.gitignore` enforces this — keep it that way.
