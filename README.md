# Boblox

[![CI](https://github.com/kingdudely/Boblox/actions/workflows/ci.yml/badge.svg)](https://github.com/kingdudely/Boblox/actions/workflows/ci.yml)

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
| `client/` | C++ network stack with **two transports in one binary**: `roblox --connection-type=RakNet` (RUPP framing, offline handshake, session crypto, join builders) or `--connection-type=RbxTransport` (archived QUIC via ngtcp2 + OpenSSL; `hs1818` stays as its handshake diagnostic). With no flag, RbxTransport is probed briefly and RakNet takes over. `-DRBX_RAKNET=OFF` / `-DRBX_TRANSPORT=OFF` decide which transports are compiled in (both ON by default; at least one required). Sources are split by subsystem — see the table below. |
| `rbx_runtime/` | The Roblox **scripting environment**: hosts the Luau VM and provides Roblox globals (`game`, `RunService`, `UserSettings`, `Random`, …) + the sandbox rules. Modular: one file per API surface, see below. `src/instance/` is the **instance world** (class registry, tree, signals, task scheduler) behind the Engine profile; `engine.h` is the persistent-environment facade for embedders (the future renderer). |
| `py/` | Python reference solver (`solve9b.py`) and the RE probes used against live servers. Research tooling — the hot path is C++. |
| `tools/` | Test harnesses (`regress9b.py`, `ch_diff.py`, `msg_parity.py`), bytecode RE tools, and gdb capture scripts. |
| `test/` | Browser-extension WebTransport codec tests (`codec.test.mjs`) and a test server. |
| `RbxWebTransport/` | Self-contained Chrome extension (load unpacked from this folder): popup launcher for the official web client, plus the RbxTransport/WebTransport protocol inspector and headless probe. |
| `third_party/` | Git submodules: `ngtcp2` (QUIC), `luau` (pinned; never modified). |
| `run/` | Captured datasets and fixtures (mostly gitignored, a few whitelisted for tests). **Account cookies live here and are never committed.** |
| `tests/` | CTest wiring for the whole battery, `run_all.sh` (layout-agnostic flow), doctest unit suites (`tests/unit/`), golden stage vectors (`tests/vectors/`). |

Inside `client/`, each folder is one subsystem and includes are rooted at
`client/src/`, so `#include "quic/session.h"` is unambiguous:

| Path | Contents |
| --- | --- |
| `client/src/common/` | RUPP framing, `util.h`, `hex.h`, `cli.h`, xxh32 — the only code both transports share |
| `client/src/raknet/` | RakNet transport, session crypto, 0x90/0x8a join builders, native post-handshake flow, datagram→app-message reassembly (`rx_stream`) |
| `client/src/quic/` | RbxTransport client, its session and send messages, joinScript/env → config |
| `client/src/join/` | HTTPS join **and** the shared joinScript reading (`target.h`) |
| `client/src/solve/` | blob decode, 0x9B solve, replication stream |
| `client/src/engine/` | live Engine runtime fed by replication |
| `client/src/view/` | raylib snapshot viewer |
| `client/apps/` | entry points: `roblox_main.cpp` + one main per transport |
| `client/tests/` | client-side test executables (known-answer, regression) |

## Build

```sh
git clone --recurse-submodules https://github.com/kingdudely/Boblox
cd Boblox

./tools/setup.sh                # one-shot: submodules + live-probe Python deps
cmake --preset release          # superbuild: runtime + client + tests
cmake --build --preset release -j
```

The test battery itself needs no `pip` packages: message builders live in
the stdlib-only `py/msgbuild.py` and handshake captures ship frozen
(`run/cpp_tx/*.hs.bin`), so ctest goes green on stdlib + compilers alone.
`setup.sh`'s Python deps serve only the live probes in `py/` (real server
joins) and capture-decryption RE tools.

Presets: `release` (build dir `./build`), `debug` (`./build-debug`),
`asan` (Debug + ASan/UBSan, `./build-asan`).

The per-project entry points still configure and build standalone (the
superbuild is additive). Binary locations depend on the layout you built:
superbuild → `./build/client/`, `./build/rbx_runtime/`; standalone →
`client/build/`, `rbx_runtime/build/`:

```sh
cmake -S rbx_runtime -B rbx_runtime/build -DCMAKE_BUILD_TYPE=Release
cmake --build rbx_runtime/build -j

cmake -S client -B client/build -DCMAKE_BUILD_TYPE=Release
cmake --build client/build -j
```

Dependencies: CMake ≥ 3.16 (≥ 3.21 for the presets), a C++17 compiler,
OpenSSL, zstd, libcurl, nlohmann/json, Node.js (for the extension codec
tests).

Which transports the binary contains is fixed at configure time (both on by
default; asking for neither is a configure error):

```sh
cmake -B build -DRBX_TRANSPORT=OFF   # RakNet only, no ngtcp2
cmake -B build -DRBX_RAKNET=OFF      # RbxTransport only
```

Both layouts still build the single `roblox` client; a
`--connection-type` naming a transport that was left out fails with the
configure switch in the message instead of silently doing something else.

## Test

```sh
ctest --preset release
```

Seventeen suites, all of which must stay green:

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
10. **unit-profile** — the two-profile sandbox split: the Challenge (0x9B
    solve) surface is frozen to the historical six modules, the Engine
    profile is a superset, and every module sits on exactly one side.
11. **unit-engine** — the instance world (Engine profile only): tree,
    typed properties, signals, the task scheduler on the virtual clock,
    workspace identity, Vector3, and Challenge-profile isolation.
12. **unit-fuzzregress** — libFuzzer-found parser crashes pinned as
    permanent regressions (extract/decode/standardize/frame-walk must
    reject them cleanly).
13. **unit-replicate** — replication frame parsers vs frozen captures:
     frame walk, 0x97 outer→inner byte-exact, 0x81 peer ids on both
     captures, 0x16 batch split, 0x07 records, 0x02 player triples.
14. **vectors-py** — `tools/gen_vectors.py --check`: the goldens stay in sync
     with the tracked capture fixtures.
15. **tables-parity** — `tools/gen_tables_inc.py --check`: `client/src/solve/tables.inc`
    stays in sync with `tools/roblox_bc_tables.json` (regenerate with
    `cmake --build build --target gen_tables`).
16. **instances-parity** — `tools/gen_instances.py --check`: the
     generated DataModel registry stays in sync with the pinned API dump.

`tests/run_all.sh` runs the same suites outside the superbuild
(1–5 plus the two generator-parity checks; the doctest suites 6–13 need the
superbuild build). It resolves binaries from `./build/` first, falling back
to standalone project builds.

Fuzzing (Phase K, clang only — libFuzzer is an LLVM facility):

```sh
./tools/fuzz_all.sh [seconds]   # blob/standardize/frame-walk, default 120s each
```

Corpora are assembled from the tracked fixtures on every run; crash
artifacts land in `fuzz/artifacts/` (gitignored). Every crasher gets pinned
in `unit-fuzzregress` so the fix can never regress. CI runs all three
harnesses for 60s each on every push.

## Live run

```sh
./build/client/roblox --place-id 1818 --roblosecurity "$(cat run/cookie2.txt)"
./build/client/roblox --connection-type=RakNet --place-id 1818 \
    --roblosecurity "$(cat run/cookie2.txt)" --seconds 30
```

With no `--connection-type`, the client probes RbxTransport for 20 s (the
path that is natively closest but currently ends in a server drain) and
falls back to RakNet, which is the transport that completes a join today.
RakNet-only options (`--seconds`, `--90-template`, `--skip-join`, …) skip
the probe automatically. A forced transport is exactly that: no fallback.

(Runs until interrupted — bound it with `timeout 45s …`. `--place-id` is
always required (the server rejects job-only joins with status 12);
`--job-id` optionally pins one running instance of that place. Session
behavior is native-fixed: real A7 attestation and full dummy traffic,
always. Standalone builds land in `client/build/` instead — see above.)

On the RbxTransport path `roblox` opens a raylib window immediately and
renders the live Engine world as replication arrives; `--headless` disables
GL and runs the same DataModel/scheduler with no window. The RakNet path is
console-only: it logs the handshake/join stages and exits after `--seconds`.
Parts
render with their replicated Color/Transparency/Shape (Ball, Block,
Cylinder; Wedge/CornerWedge are approximated as blocks), and the camera
frames the 90th-percentile playable reach so skybox outliers cannot shrink
the map to specks. `--shot PATH` captures the live window once the world is
visible. The live path
consumes `0x97` JoinData, snapshot `0x83/0x16` batches, and streaming
`0x83` items (`0x02` spawns, `0x03` props, `0x0B`/`0x25` entry batches,
`0x0D` records, `0x07` events, `0x78` script bulks). `RBX_REPLAY=file`
applies a captured chan1 stream through the same path without a join;
combine it with `--shot PATH` to render a capture offline
(`screenshots/replay_view.png` is that path on `run/repl_1818.bin`).

Success looks like `challenge=yes answered=yes peer=yes resets=0` followed by
replication traffic and `== live == world=yes`. As a control: a deliberately
wrong answer gets exactly one challenge rejected (server drains after ~63 KB),
a correct one streams indefinitely.

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

## Adding an Instance class

The Engine profile (never the challenge path) carries the instance world in
`rbx_runtime/src/instance/`: a class registry, the base `Instance` tree, the
Lua userdata glue, signals (`RBXScriptConnection`, `Wait`), and the `task`
scheduler on a deterministic virtual clock. One class = one file:

1. Create `rbx_runtime/src/instance/classes/<name>.cpp` exposing
   `void rbx::register_class_<name>()` that fills a `ClassInfo`
   (name, superclass, declarative `PropInfo` list, factory) and calls
   `register_class`.
2. Declare + call it in `rbx_runtime/src/instance/classes.cpp`.
3. Add the `.cpp` to `challenge_core` in `rbx_runtime/CMakeLists.txt`.

Properties get typed marshalling, defaults, read-only flags, and
`GetPropertyChangedSignal` support for free; base methods (`FindFirstChild`,
`IsA`, `Destroy`, …) come from the Instance glue — no per-class Lua code.
Run `ctest` after any change: `unit_engine` pins the behavior and
`unit_profile` pins that none of it leaks into the frozen challenge surface.
Embedders (the renderer and headless client) drive the world through
`rbx_runtime/src/engine.h` (`create` / `execute` / `step` / `now`). The
Engine also implements the script-facing parts of `Players` (dynamic
`LocalPlayer`/`NumPlayers`, `GetPlayers`, `GetPlayerByUserId`,
`GetPlayerFromCharacter`) and `Player` (`GetNetworkPing`,
`DistanceFromCharacter`).

## Notes

- The Luau checkout under `third_party/luau` is pinned and **never modified**;
  everything Roblox-specific lives in `rbx_runtime/`.
- Account credentials (`.ROBLOSECURITY` cookies) must never be committed;
  `.gitignore` enforces this — keep it that way.
