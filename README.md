# Boblox

[![CI](https://github.com/kingdudely/Boblox/actions/workflows/ci.yml/badge.svg)](https://github.com/kingdudely/Boblox/actions/workflows/ci.yml)

A from-scratch client stack for Roblox's protocol, written as a research
project. It joins **real Roblox servers** over RakNet and receives live
replication — join data, snapshot batches and the streaming world are built
from the server's `0x97`/`0x83` messages — and solves Roblox's `0x9B`
anti-cheat challenge **fully locally**: blob decode, bytecode standardization,
and execution in a byte-exact Roblox-flavored Luau sandbox in ~1 ms, with no
native client or oracle in the loop. Verified live against Crossroads (place
1818): a 20/20 join soak, with 1,300–3,600 instances spawned per session.

The full reverse-engineering record — protocol notes, byte layouts, what was
cracked and how — lives in **[FINDINGS.md](FINDINGS.md)**. It is the canonical
knowledge base; this README is just orientation.

## Transports

**RakNet** is the supported transport and the only one built by default.
**RbxTransport** (the earlier QUIC stack) is still in the tree but disabled,
because it does not currently work against live servers. A native
**`rbxwebtransport`** connection type over HTTP/3 (ngtcp2) is planned for when
Roblox's web client is available to all games, not just enrolled universes;
nothing for it exists or is built today.

## Layout

| Path | What lives there |
| --- | --- |
| `client/` | C++ network stack. `roblox --connection-type=RakNet` (RUPP framing, offline handshake, session crypto, join builders) is the default and the only transport built by default. `--connection-type=RbxTransport` (archived QUIC via ngtcp2 + OpenSSL; `hs1818` stays as its handshake diagnostic) still exists but does not currently work against live servers, so it is opt-in at configure time. Switches: `-DRBX_RAKNET=OFF`, `-DRBX_TRANSPORT=ON`; at least one transport is required. A native `rbxwebtransport` (HTTP/3 via ngtcp2) is planned for when Roblox enables the web client for all games. Sources are split by subsystem — see the table below. |
| `rbx_runtime/` | The Roblox **scripting environment**: hosts the Luau VM and provides Roblox globals (`game`, `RunService`, `UserSettings`, `Random`, …) + the sandbox rules. Modular: one file per API surface, see below. `src/instance/` is the **instance world** (class registry, tree, signals, task scheduler) behind the Engine profile; `engine.h` is the persistent-environment facade for embedders (the future renderer). |
| `py/` | Python reference solver (`solve9b.py`) and the RE probes used against live servers. Research tooling — the hot path is C++. |
| `tools/` | Test harnesses (`regress9b.py`, `ch_diff.py`, `msg_parity.py`), bytecode RE tools, and gdb capture scripts. |
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
| `client/src/view/` | SDL3 + OpenGL headful viewer |
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
OpenSSL, zstd, libcurl, nlohmann/json.

Which transports the binary contains is fixed at configure time. RakNet is
built by default; RbxTransport is not built unless asked for, because it does
not currently work against live servers. Asking for neither is a configure
error:

```sh
cmake -B build                          # RakNet only (default)
cmake -B build -DRBX_TRANSPORT=ON       # also build the archived QUIC stack
cmake -B build -DRBX_RAKNET=OFF -DRBX_TRANSPORT=ON   # RbxTransport only
```

Both layouts still build the single `roblox` client; a
`--connection-type` naming a transport that was left out fails with the
configure switch in the message instead of silently doing something else.

## Test

```sh
ctest --preset release
```

Sixteen suites, all of which must stay green (the RakNet-specific
`join_msg_raknet` and `raknet_rx_stream` are wired in `client/`):

1. **solver-regress-py** — Python reference solver over 5 captured native
   datasets; answers must equal what the real client sent, byte for byte.
2. **solver-regress-cpp** — the same 5 datasets through the C++ in-process
   path (`rbx_solve`), the one the live client uses.
3. **handshake-parity** — our QUIC ClientHello byte-compared against the
   native client's captured one.
4. **message-parity** — session messages byte-compared against the Python
   builders (earlyauth, 0x8A, 0x90).
5. **unit-runtime** — doctest unit suite running Luau source end-to-end
   through `rbxch::run` (sandbox shims, profile options, Random known-answer).
6. **unit-vectors** — golden per-stage vectors (`tests/vectors/`) through the
   C++ pipeline: extract → decode → standardize → solve, byte-compared stage
   by stage against frozen goldens + the native answers.
7. **unit-messages** — the C++ session message builders vs golden bytes built
   by the Python reference builders (only when `RBX_TRANSPORT=ON`).
8. **unit-rupp** — RUPP datagram framing vs a native-captured datagram
   (byte-exact strip/re-wrap round-trip).
9. **unit-profile** — the two-profile sandbox split: the Challenge (0x9B
    solve) surface is frozen to the historical six modules, the Engine
    profile is a superset, and every module sits on exactly one side.
10. **unit-engine** — the instance world (Engine profile only): tree,
    typed properties, signals, the task scheduler on the virtual clock,
    workspace identity, Vector3, and Challenge-profile isolation.
11. **unit-fuzzregress** — libFuzzer-found parser crashes pinned as
    permanent regressions (extract/decode/standardize/frame-walk must
    reject them cleanly).
12. **unit-replicate** — replication frame parsers vs frozen captures:
     frame walk, 0x97 outer→inner byte-exact, 0x81 peer ids on both
     captures, 0x16 batch split, 0x07 records, 0x02 player triples.
13. **vectors-py** — `tools/gen_vectors.py --check`: the goldens stay in sync
     with the tracked capture fixtures.
14. **tables-parity** — `tools/gen_tables_inc.py --check`: `client/src/solve/tables.inc`
    stays in sync with `tools/roblox_bc_tables.json` (regenerate with
    `cmake --build build --target gen_tables`).
15. **instances-parity** — `tools/gen_instances.py --check`: the
     generated DataModel registry stays in sync with the pinned API dump.

`tests/run_all.sh` runs the same suites outside the superbuild
(1–4 plus the two generator-parity checks; the doctest suites 5–12 need the
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
    --roblosecurity "$(cat run/cookie2.txt)" --seconds 30   # bounded run
```

A live run **stays connected until Ctrl+C** (SIGINT/SIGTERM); the summary is
printed on exit either way. `--seconds N` bounds it to N seconds for scripted
and soak runs (`--seconds 0`, the default, means until interrupted).

Both live transports share the same renderer and Engine world: **headful is
the default** (SDL3 window, third-person/orbit camera), `--headless` runs the
identical world with no GL, and `--shot PATH` saves one frame once replication
has been applied. RakNet feeds the viewer from the same `LiveRuntime` path
RbxTransport uses; its socket is drained non-blocking every frame so the
~1 MB join snapshot is not dropped while a frame is drawn.

On the wire, RakNet sends the upstream `0xA6` input stream the native client
sends (unreliable): the four route frames, then the count-4 steady state at
~13 Hz. **Driven movement is not solved yet** — those route frames carry
`nActions = 0`. A local Character does arrive on 1818, and on some 189707
sessions, once the RCC calls LoadCharacter, but 189707 is not deterministic:
two of six post-parser joins (12–30 s) linked `ChatGGPT` while the other four
reached the world without a Character. `--input=control --slots=…` sends an
explicit ControlState probe, but the slot-to-movement mapping is not
recovered, so it is never presented as a "forward" key.

With no `--connection-type`, a binary that contains both transports probes
RbxTransport for 20 s (the path that is natively closest but currently ends
in a server drain) and falls back to RakNet, which is the transport that
completes a join today. The default build contains RakNet only, so no probe
runs and RakNet is used directly. RakNet-only options (`--seconds`,
`--90-template`, `--skip-join`, …) skip the probe automatically. A forced
transport is exactly that: no fallback.

(Runs until interrupted — bound it with `timeout 45s …`. `--place-id` is
always required (the server rejects job-only joins with status 12);
`--job-id` optionally pins one running instance of that place. The RakNet
join follows the native's post-handshake sequence (`0x90` → `0x92` → `0x8A` →
`0x8F` → the `0x9B` join step) and answers the server's `0x9B` challenge in
process. Its body templates live under `run/` and are resolved against the
CWD, the binary's directory and the repo root, so `./roblox` works from
`build/client` as well as the repo root; set `BOBLOX_DATA_DIR` to point at a
different fixture directory. Standalone builds land in `client/build/`
instead — see above.)

On the RbxTransport path `roblox` opens an SDL3 window immediately and
renders the live Engine world as replication arrives; `--headless` disables
GL and runs the same DataModel/scheduler with no window. The RakNet path uses
the same viewer by default and logs the handshake/join stages alongside it.
Parts
render with their replicated Color/Transparency/Shape (Ball, Block,
Cylinder; Wedge/CornerWedge are approximated as blocks), and the camera
frames the 90th-percentile playable reach so skybox outliers cannot shrink
the map to specks. `--shot PATH` captures the live window once the world is
visible. The live path
consumes `0x97` JoinData, snapshot `0x83/0x16` batches, and streaming
`0x83` items (`0x02` spawns, `0x03` props, `0x0B`/`0x25` entry batches,
`0x0D` records, `0x07` events, `0x78` script bulks). `RBX_REPLAY=file`

Instance member lookup is the native one: declared properties/methods/events
first, then **children by name** (`workspace.Baseplate`, `gui.Frame`). The
DataModel carries every engine service as a real child (the replicated service
when the server sends one, the Engine singleton otherwise), so
`game.Players == game:GetService("Players")` and `game.RunService` exist even
though the server never replicates RunService. `game.Workspace`, `workspace`,
`lighting` and `game.RunService` are live views over those children, not
stored nulls. Script bytecode arrives as `0x78` content-addressed bulks keyed
by the script's 16-byte `Source` hash; the client keeps that hash (and both
8-byte halves) from the snapshot, so a bulk is attributed to its script even
when the spawn item came much earlier in the stream. A native-client capture
shows no script-content request on the wire at all: the RCC pushes the bulks,
so missing script bodies mean parse gaps, not a missing client request.
`RBX_REPLAY=file`
applies a captured chan1 stream through the same path without a join;
combine it with `--shot PATH` to render a capture offline
(`screenshots/replay_view.png` is that path on `run/repl_1818.bin`).

Success looks like `challenge=yes answered=yes peer=yes resets=0` followed by
replication traffic and `== live == world=yes` on RbxTransport. On RakNet it
is `0x9B answer … (solved in 1ms)`, then `0x97`, then
`world spawned=… instances=… engine=on` and `live_frames=…`. As a control: a
deliberately wrong answer gets exactly one challenge rejected (server drains
after ~63 KB); a correct one streams indefinitely.

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

## RakNet protocol coverage and feature flags

The transport has one wire vocabulary now (`client/src/raknet/protocol.*`):
packet ids and their meanings, all five reliability modes, the
datagram/message/split headers, ACK and NAK records (the client's compact form
and the RCC's trailing-u24 form), a bit-exact stream reader/writer, and a
per-session census printed in the exit summary as
`raknet: census dgrams=… internal=… app=… sub=… unknown=…`. Unknown ids are
counted and named as unknown once; nothing is silently dropped. The
`raknet_protocol` unit suite pins every layout, and `raknet_rx_stream` pins the
reassembly policy that now runs on top of the shared codec.

Upstream input lives in `client/src/raknet/input.*`: the captured 0xA6 route
frames plus the ControlState block (`[N][A][B][len][presence][values]`) with
explicit slot selection. `--input=control --slots=0,3` sends a chosen
ControlState; the slot-to-movement mapping is **not** recovered, so the client
never invents a "forward" key.

Optional subsystems are CLI switches, so a run pays only for what it uses:

| Flag | Effect |
| --- | --- |
| `--no-render` (or `--headless`) | no GL window; the identical Engine world runs headless |
| `--no-scripts` | parse replication and build the world, skip Luau chunk execution |
| `--gui` / `--no-gui` | install PlayerGui/CurrentCamera (default on; Font is always available). `--no-gui` drops the tree for the smallest client |
| `--no-physics` | accepted for symmetry; physics is server-authoritative and this build has no client solver |
| `--input=native\|control` + `--slots=N[,N]` | captured route frames, or an explicit ControlState probe |
| `--render` / `--scripts` / `--physics` | positive forms; win over the `--no-` counterpart |
| `--packet-manifest` | print every app id 0x81..0xa9 with direction and receive/send codec status, then exit |
| `--app-probe=96,96f,99` | send native-shaped 0x96/0x99 client messages once after the join (opt-in) |

These switches choose local behavior; they are not part of the RakNet wire
codec. The renderer and the Luau runtime consume the same replicated Engine
world, and physics is server-authoritative, so disabling any of them does not
change the bytes on the wire. The client has no local physics solver to turn
on: `--physics` records the choice and the server remains authoritative.
The unfinished reference client at `alexkkork/roblox-client` is useful as a
Luau/animation reference, but it does not supply the missing RakNet app-id or
camera-reference layouts; packet coverage is tracked only by the capture-backed
manifest (`--packet-manifest`).

The local avatar path is capture-backed but not fully deterministic. The
client sends the native's exact 55-byte 0x83/04 character-state bootstrap at
ordering slot 6. On 1818 both test accounts then link their own Character and
the viewer reframes on it (`--headful`; `RBX_SHOT_AVATAR=1` holds `--shot`
until the avatar is framed). On 189707 two of six post-parser runs linked
`ChatGGPT`; the other four reached the world with no local Character, and the
default route input stays at `nActions = 0`. `RBX_NO_8304=1` disables the bootstrap for A/B runs and
`RBX_8304_RAW=<file>` overrides its bytes.

After the spawn the RCC streams 0xA1 client-state messages (~1 KB, ~40-100/s;
separate from the 44-byte outbound camera frame). The 17-byte envelope is
parsed (`a1 <u32 tick> <u32 records> <u32 ack> <u32 flags>`), then records
with `[ref][u16 class][u16 member_count][ceil(member_count/64) mask words]
[u16 body_len][body]`. Multi-word masks (68/69/70/82-member classes) and the
variable four-byte inter-record marker are handled; a live run parsed 1,166
messages / 14,978 records with zero A1 failures. Per-class bodies are still
being mapped, so they are carried opaque (`a1=<messages>/<records>`) rather
than treated as an unknown id.

The server's 0x93 network-settings push is decoded too: `[93][u8 flags]
[u16 count]` plus name/value strings. The live 51-entry push logs as
`0x93 network settings: flags=0 entries=51`; it is server-to-client only.

The 44-byte outbound 0xA1 camera frame now has a tested
`buildA1Camera(clock, ref)` builder. It remains gated on the capture id
actually existing in the current world: sending the frame with a live
character-model or HumanoidRootPart id was rejected by the RCC with
`0x15 01 04` on 1818, so no live-ref substitution is enabled.

The observed client handshake messages also have builders now: `0x96`
(idle/flag form), `0x99` (14-byte capability report), and the `0x04`
marker acknowledgement. They are sent by `--app-probe=96,99` when the
caller wants the native-shaped probe; they are not forced into the proven
join ladder. A live probe was accepted and the session stayed joined.

The app-id range is now 0x81..0xa9, not 0x81..0xa7: the native checks 0xa9 as
a compressed-packet wrapper and 0xa8 is a real early-auth builder, while 0xa0
is the transport NAK and is not an app message. `--packet-manifest` prints all
41 ids with direction and receive/send codec status; ids with no capture are
shown as `(unobserved)` instead of being given invented names.

The 0xA9 wrapper is decoded: `[A9][LEB128 decompressed size][LZ4 block]`, with
the native's optional trailing u16 hash and an optional 9-byte `0x1B` timestamp
prefix. The decoder is the LZ4 block grammar (native `sub_253DF1C`), not zstd;
the decompressed inner payload is fed through the normal application sink. No
0xA9 packet has appeared in the captured live sessions yet, so it is marked
receive-decoded on the native layout evidence, not on a live capture.

The formerly unobserved 0x86 server stream is now structurally decoded. A
189707 capture carried 50 messages / 74 records: `[86][u8 version=01]` then
N × 11-byte records, with N implied by the remaining length (13/24/35/46/57/90
bytes = 1/2/3/4/5/8 records; the byte after 0x86 is a version, not a count).
Each record is `{u32 ref_a, u8 ref_kind=1, u32 ref_b, u8 state, u8 flag}`.
Across all 74 records `ref_kind` is always 1, `state` is 0–5, and `flag` is
0/1. The manifest reports `APP_INSTANCE_PAIRS s2c recv=yes send=no`; the pair's
semantic role is deliberately unassigned because no native 0x86 handler was
located, only size/priority comparisons. `RBX_PAIR_CLASSES=1` prints the
DataModel classes behind both refs when the message recurs. Six follow-up
joins after the parser landed did not contain 0x86, so the live application
effect and the name of the relationship remain open rather than guessed.

The 0x83 chain walker handles the 4-byte parent-ref `lo` and the typeless
script bytecode bulk (whose first byte can collide with a real item type).
It still stops loudly on bytes outside the native dispatch tables. A prior
25 s run pinned the documented 24-byte `0xa3` atomic unknown; a
20 s probe run reached `stops=0 walk_fail=0 unk_flag=0 unk_id=0 unk_app=0`
with the local avatar linked. `0xb9`/`0xe3`/`0xe5` appeared in other sessions
and are left unassigned rather than given invented semantics.

A live `--app-probe=96,99` run sent the 2-byte `0x96` and 14-byte `0x99`
messages, kept the avatar linked, parsed 794 A1 messages / 9,395 records, and
saw no 0x15 disconnect.

## Notes

- The Luau checkout under `third_party/luau` is pinned and **never modified**;
  everything Roblox-specific lives in `rbx_runtime/`.
- Account credentials (`.ROBLOSECURITY` cookies) must never be committed;
  `.gitignore` enforces this — keep it that way.
