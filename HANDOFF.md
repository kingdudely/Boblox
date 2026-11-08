# Handoff — Boblox headless client (movement blocker)

## Project
From-scratch interoperability client for Roblox's RakNet protocol. C++17, Linux.
Source `~/work/Boblox`, binary `~/work/Boblox/build/client/roblox`.
Target: live Android client build **2.743.0.1136**. Embeds upstream Luau (pinned,
unmodified) with an executor surface. 20 doctest suites, all green.

## Build / run
```
source ~/work/env.sh
cmake -B build -DCMAKE_BUILD_TYPE=Release \
  -DNLOHMANN_JSON_INCLUDE_DIR=$HOME/work/Rum/third_party/nlohmann_json/include \
  -DRBX_VIEWER=OFF
cmake --build build -j --target roblox
cd build && ctest
```
Run: `./tools/run.sh <place> [--exec file] [--seconds N]`
Cookie file: `~/.config/boblox-cookie` (main), `~/.config/alt-cookie` (alt).
`./tools/cookie.sh` writes it, `--show` inspects. `--cookie-file` also works.
Live script socket: `BOBLOX_SOCKET=/tmp/boblox.sock ./tools/run.sh ...` then
`./tools/push.sh file.lua`.

## Accounts (ids only, never paste cookies)
- main `poopnest5002` 11754932511 — runs in Rum, used as the observer
- alt `poopnestomegahater` 11924576267 — runs in Boblox

Observer workflow: start the main in Rum on a place, then read its job id from
the presence API with the ALT cookie and pin Boblox to it with `--job-id`.
Presence: POST https://presence.roblox.com/v1/presence/users
(body `{"userIds":[<main id>]}`) with the alt cookie + CSRF token; read `gameId`.
CSRF token: POST https://auth.roblox.com/v2/logout, take `x-csrf-token` from the
403 response.

## Verified working
- Session establishment, RUPP framing, X25519 handshake, AES-256-GCM datagrams.
- Full world application: 1818 (~2k instances), Arsenal (~124k), Natural
  Disaster Survival 189707 (~12k).
- 0x9B join challenge computed locally.
- Executor runs arbitrary Luau in the client; `loadstring` + `game:HttpGet`
  fetch and execute remote scripts (tested with a 135 KB obfuscated payload).
- Outbound event batches `0x83/0x07` accepted by the server.
- Multi-instance launcher (one process per account).
- Offline tooling: LD_PRELOAD packet probe, capture decryptor, TX dumps.

## NOT working: the character never moves
Character is visible to the observer and collidable (pushing it with a body
moves it), so the server simulates it. Nothing we send changes its position.

Attempts, all acknowledged by the server, none applied:
1. `0x83/0x03` direction property (prop `0x0f08`, Vector3), 10 Hz
2. `0x83/0x03` CFrame property (pos + kind + 3x3 matrix)
3. `0x83/0x05` 45-byte position upload, ref/tick/ref/pos, reliable and unreliable
4. `0x83/0x06` 49-byte physics/counters claim
5. local integrator (velocity, gravity 196.2, ground clamp) feeding 3 and 4

## Protocol findings (from decrypted reference captures)
- Reference upstream on a walkable place: `0x83/0x0a` ~4/s idle → ~30/s walking,
  nearly every item naming the SAME ref, clock advancing ~0x38 per item,
  reliable-ordered, channel 0. Shape:
  `83 0a | 01 <ref u32> b8 15 <flag u8> <clock u32> 00 00`
- `0x83/0x05`: 95 distinct refs, each appearing exactly TWICE — no per-instance
  position series. Not a position stream on this build.
- `0x83/0x06`: ref pair + small u32 counters, not floats.
- Clock rate ≈ 57,000–57,900 units/sec (not ms, not 16.16 fixed point).
- Sending `0x83/0x0a` at 30 Hz kills the session with `0x15/0105`
  (DisconnectReceivePacketStreamError). A SINGLE item survives. 10 Hz and 5 Hz
  survive. So it is a rate/bookkeeping limit, not a payload error.
- `0xA6` does not exist in 2.743 (four sessions, idle and driven).
- RUPP subtype: reference sends 3,539 datagrams on subtype 2 vs 5 on subtype 1.
- Place 1818 is a bare UGC datamodel with no PlayerModule: the reference client
  cannot walk there either. Never use it for movement work.
- Arsenal's in-game player list only shows players in an active match; not a
  join-completeness signal. Arsenal's "lobby" is a menu, no 3D body.
- Natural Disaster Survival (189707) is the correct test place: world loads,
  character spawns, walking works for the reference.

## THE CURRENT BUG (where to continue)
`Players.LocalPlayer` resolves to the wrong player, so every movement write is
aimed at a stranger's body.

Evidence from a live run:
```
[id] local uid=11924576267 name='poopnestomegahater'
[ros] children=5  local= SamanthaCraphenter (Player)
  SamanthaCraphenter  uid= -17003389
  jelenadodik         uid= -1579903814
  xXWeksterisXx       uid= -1908471570
  poopnest5002        uid= -1129969377
```
Two defects:
1. Every `UserId` decodes as a garbage negative. Real ids exceed 2^31, so the
   value is being applied through a narrow signed path and wrapping.
2. Our own Player is not in the roster. Roblox does not replicate your own
   Player to you; the client must set LocalPlayer from the join data. Ours
   never does, so `Players.LocalPlayer` (a live view in
   `rbx_runtime/src/instance/instance.cpp`) falls back to the first child.

Partial fix already in the tree: `syncPlayers()` in
`client/src/engine/live_runtime.cpp` now synthesises the local Player from
`rbx::local_user_name()` / `local_user_id()`, parents it to Players, and links
the Workspace model of the same name as its Character. It compiles and tests
pass, but it does NOT fire — no `[id] local Player synthesised` line appears.
Debug why: either a roster child already satisfies the `present` check, or
`rbx::instantiate(rbx::find_class("Player"))` returns null.

Also add `const std::string& local_user_name();` to
`rbx_runtime/src/instance/instance.h` if it is missing (it is declared in the
current tree; definition in instance.cpp).

## Code changes made this session
- `client/src/send/entity_batch.{h,cpp}` (new): byte-exact builders for
  `0x83/0x07` events (string/bool/number/Vector3/Instance args), `0x83/0x03`
  Vector3 and CFrame writes, `0x83/0x05` position upload, `0x83/0x06` counters,
  `0x83/0x0a` control/report items. Pinned by `tests/unit/unit_batch.cpp`.
- `client/src/exec/exec.{h,cpp}`: executor surface (loadstring, getgenv,
  HttpGet/request, filesystem, crypto, TweenInfo/Drawing/gethwid stubs),
  `boblox.fire`, `boblox.move`, `boblox.teleport`, socket script queue,
  WorldView bridge (instance->ref, class+event->index, class+member->index,
  local humanoid).
- `client/src/send/bridge.{h,cpp}` (new): executor <-> transport seam.
- `client/src/engine/live_runtime.{h,cpp}`: installs the executor after attach,
  runs `--exec` scripts, local physics integrator, position/claim/report senders.
- `client/src/raknet/{send.cpp,raknet.cpp,join_flow.cpp,join_msg.cpp}`: upstream
  send path, RUPP subtype-2 default, ordering.
- `rbx_runtime/src/instance/instance.cpp`: LocalPlayer live view now matches by
  user name as well as id (incomplete — see bug above).
- `tests/unit/{unit_batch,unit_exec,unit_send}.cpp` (new).

Patch of all of the above: `~/Downloads/movement-edits.patch` (may be stale;
regenerate with `git diff HEAD`).

## Tooling
- `tools/native_probe.c` → `native_probe.so`: LD_PRELOAD probe; dumps UDP,
  injects touch/key into the reference client, scans process memory.
- `tools/native_capture.sh`: runs the reference client (Rum) with the probe.
- `tools/nat_decode.c` → `nat_decode`: recovers session keys from a capture and
  prints plaintext datagrams.
- `RBX_RAKNET_TXDUMP=/dir` dumps our own plaintext TX datagrams.
- `tools/alt_walk.lua`, `run/live_test.lua`, `run/cf_probe.lua`,
  `run/roster.lua`, `run/teleport_test.lua`, `run/move_test.lua`.
- `tools/multi.sh`: one process per account.
- `COMMANDS.md`, `SEND.md`, `EXECUTOR.md`: protocol and CLI notes.

## Next steps
1. Fix the `UserId` decode (32-bit truncation) — this may fix matching alone.
2. Debug why the local-Player synthesis block does not fire; make LocalPlayer
   resolve to the alt account.
3. Once LocalPlayer is correct, re-run movement on 189707 with the observer:
   `BOBLOX_INTEREST=1 BOBLOX_INTEREST_MS=100 ./tools/run.sh 189707 --exec run/alt_walk.lua`.
4. If movement still fails with correct identity, revisit the `0x0a` clock base
   (rate ≈57.8k/s) and the repeated-item separator/flag bytes.
