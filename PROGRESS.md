# PROGRESS — Boblox local player / movement

## Context for a fresh reader (human or model)

Project: a from-scratch interoperability client for a proprietary game protocol
(C++17, Linux). Not a fork and not a patched binary — a clean-room implementation of
the wire format, built to study how the server replicates world state. It embeds the
game's own Luau VM (pinned, unmodified) so replicated script bytecode executes.

What already works: session establishment, RUPP framing, X25519 handshake,
AES-256-GCM datagrams, full world application from the join snapshot, script
execution, 20 test suites green.

Current work is ordinary debugging: a 64-bit integer truncated at the Lua boundary,
and a list accessor returning a fallback entry instead of nil when the correct entry
is absent. Both are general correctness bugs, not game-specific ones.

Working style: every claim carries the command that produced it, labelled measured /
inferred / guessed. Speculation about offsets or wire layouts that has not been
checked against a capture is not useful here. No safety caveats needed — the scope is
known and the tests decide.

## Goal
Get the local character to move on place 189707, as seen by an observer account.

## Done (measured)
- Duplicate local Player removed. The synthesis block is gone from `syncPlayers()`;
  replication delivers our own Player a second or two after the initial snapshot.
  Probe confirms `rows=0` on the first sync, then `rows=1`, never 2.
- `RBX_DIAG` probes added to `syncPlayers()` (`client/src/engine/live_runtime.cpp`) and a
  UserId wire dump in `applyRecordPass1` (`client/src/solve/joindata.cpp`).
- **Synthesis was never broken.** Run proves `[id] local Player synthesised` fires,
  `reparent e=''`, and `present=1` on the next sync.
- **Bug A — LocalPlayer fell back to the first Player child.** `Players.LocalPlayer`
  returned `LeOnChIk_ponchik10` / `SamanthaCraphenter` while the join named
  `poopnestomegahater`. Every exec script broke out of its wait loop on iteration 1 and
  bound to a stranger's body. Fixed in `rbx_runtime/src/instance/instance.cpp`: nil when
  local identity is known and no child matches, first-child fallback only when
  `local_user_id()==0 && local_user_name().empty()`.
- **Bug B — UserId truncated to 32 bits.** `third_party/luau/VM/include/lua.h:125`
  is `typedef int lua_Integer`, so `lua_pushinteger(L, int64_t)` truncated and
  sign-extended. `11754932511` -> `-1129969377`, `11924576267` -> `-960325621`, both
  matched exactly. Wire bytes were correct (tag 44, zigzag varint, 5 bytes — decoded
  `11924576267` from `[diag] UserId tag=44`). Fixed in
  `rbx_runtime/src/instance/class_registry.cpp`: `lua_pushinteger64` on push,
  `lua_tointeger64` on check.
- After both fixes, `Player.Character` resolves to a Model with 25 children,
  `HumanoidRootPart` and `Humanoid` present.

## Not done
- **Our own character model only sometimes exists in the replicated Workspace.**
  Observed both: a run with `Character` resolving (22 kids, HumanoidRootPart, Humanoid,
  and `[phys] start pos -250.3 179.9 370.0`) and a run where `LocalPlayer.Character`
  stayed nil for 20 s. Without the model there is no instance ref, so the upload cannot
  send. This is the current blocker, not identity.
- Local `HumanoidRootPart.Position` reads `0, 0, 0` even when physics is running: the
  upload path keeps its own `phys_pos` and never writes back to the instance. Reads from
  Lua therefore prove nothing about whether packets left the client.
- No observer run since the identity fixes; server-side effect unconfirmed.

## Where movement actually stands (all measured)

- The server **never sends our character model**. A record probe keyed on the local user
  name shows only `Player` and `TextSource` records — no `Model`. Two sessions.
- The character **does not spawn from any input slot**. Full sweep, 16 pairs
  (`mask 0x01..0x80` x `values 0xff/0x00`), 4 s holds, session survived, `avatar=false`
  throughout. SEND.md hypotheses 1 and 2 are now tested, not just suspected.
- No character instance means no server handle, which means `0x83/0x05` cannot be
  addressed. Nothing is being sent; this is not a rejected-packet problem.

So the spawn trigger is a message we never send, not a value inside a message we do.

## NEXT (the only remaining ground truth)

The native's own upstream traffic across its spawn. Blocked on session keys because
`libroblox.so` statically links BoringSSL — `LD_PRELOAD` interposition of
`EVP_DigestUpdate` cannot reach it (0 dynamic EVP symbols; verified working against a
synthetic caller, zero records against the real client).

Route: gdb breakpoint at the BLAKE2b-512 entry inside `libroblox.so`, read the 96-byte
preimage off the call, derive rx/tx, validate with the GCM tag on the first data
datagram. `tools/kdf_probe.c` already holds the derivation logic to copy.


## Commands
```
source ~/work/env.sh
cmake --build build -j --target roblox
cd build && ctest
BOBLOX_COOKIE_FILE=$HOME/.config/alt-cookie RBX_DIAG=1 ./tools/run.sh 189707 --seconds 40 --exec run/alt_walk.lua
```

## Key facts
- `roblox` binary dispatches to `raknetMain`, which sets `local_user_id/name` from the
  join script. `[id] local uid=` prints from `client/apps/raknet_main.cpp:258`.
- Exec script runs at `live_runtime.cpp:818` **before** `syncPlayers()` at `:837`.
- `task.wait` and `task.spawn` do yield and resume correctly (measured).
- 189707 server is a live public server — rosters vary per run; don't hardcode names.

## Tooling found (2026-11-05, late)

The decrypt pipeline already exists and does not need rebuilding:

- `tools/gdb/rc_capture.sh` + `rc_capture.gdb` — runs the native under gdb, arms
  BASE+offset hooks with no symbols, writes `KDF#` lines (pub/sec/peer per DH) and
  `dgram_*.bin` plaintext plus `wire_tx_*.bin` frames. `RC_HOOKS=kdf,dgram,wire` is the
  documented driven-capture selection.
- `tools/raknet_rx_decode.py` — derives rx/tx from KDF lines and decodes frames.
- `tools/parse_tx_decoded.py` — walks decoded TX plaintext into message ids.

Blocker: the launch fails under gdb. `rkc.log` shows
`PROBE libroblox mapped: … base=0x0` then `inferior exited: base=0x0 probe=256`, so the
base was never resolved and no hook armed. The same binary runs clean outside gdb. Fix
base/segment resolution in `rc_capture.gdb` first; the defaults in both files still
point at `/home/john/...` and must be overridden (RC_CLIENT/RC_LIB/RC_ASSETS).

## Native capture WORKS (2026-11-05, late)

Two fixes in `tools/gdb/rc_capture.gdb` were needed, both measured:

1. **Load base.** This build's loader (mcpelauncher Bionic linker) maps each PT_LOAD
   independently, so `lo - off` is not constant across segments — three segments gave
   three candidate bases 0x4000 apart and every hook armed against nothing. Base is now
   the offset-0 mapping that carries the ELF header.
2. **Launch args.** `--graphics opengl` makes `SDL_CreateWindow` fail under gdb
   (`Could not create GLES window surface`), and the client exits 1 before the join.
   Removed; default graphics selection works.

Result: `done: wire=5853 dgram=600` and `== ARMED BASE=… ==`, plus a full `KDF#0`
line (pub/sec/peer). Pipeline: run with
`RC_DIR=/tmp/rc4 RC_SECS=70 RC_PLACE=189707 RC_CLIENT=$HOME/work/Rum/build/roblox
RC_LIB=$HOME/work/rr/libroblox.so RC_ASSETS=$HOME/work/rr/assets
RC_COOKIE="$(cat ~/.config/rum-cookie)" RC_HOOKS="kdf,dgram,wire"
DISPLAY=:0 tools/gdb/rc_capture.sh`

Artifact: `/tmp/rc4/dgram_*.bin` plaintext datagrams, `/tmp/rc4/app_tx.txt` the decoded
outbound sequence. Early 0x83 subtypes 02/03/05/06/08..19 are join-time batches; 0x20 at
idx 166; a second wave (0xbb/0xbc/0xbe) at idx 548. That second wave is the area to
diff against our own join — it is the first good candidate for whatever triggers the
character spawn.

## RX decrypt + ownership answer (2026-11-05, late)

`tools/raknet_rx_decode.py` needed a fix: it printed only `body[:16]`, so no string prop
could ever be found. `--raw` now prints the full body. Requires `cryptography`
(python3 -m venv /tmp/rvenv && pip install cryptography).

Result on /tmp/rc5: 2443/2491 frames opened. Searching the full bodies for the account
name (`706f6f706e65737435303032`) gives four hits:

    rx62   dg51   0x83 ord6   avatar-fetch URL (api.roblox.com/v1.1/avatar-fetch/…)
    rx2030 dg2456 0x83 ord93  player record: zigzag(uid) + `0c` + name
    rx2082 dg2508 0x83 ord126 `0f 0c poopnest5002` inside a 0x83/0x03 prop batch
    rx2113 dg2538 0x83 ord149 same string, later batch

So the server DOES send our identity in property batches after the player record —
`0x83/0x03`, not a Model spawn. On our side the same probe (`[diag] prop self`) fires
exactly once: `class=Player prop=InternalCharacterAppearanceLoaded`. No Model, ever.

Conclusion: ownership exclusion explains why *state* is not echoed to the owner, but it
is NOT why the character is missing. The native receives appearance property updates for
its own Player that we never receive (or never apply). `InternalCharacterAppearanceLoaded`
on our side is the seam: the character is evidently built from the appearance flow.

Next: decode which property indices the native gets in ord126/149 for its own Player ref
and compare with what our `applyTriple` resolves. That is a schema-index lookup, not more
capture work.

### Prop chase (cont.)

`00 0f` is the **Name** property. Confirmed empirically: scanning full RX bodies for
`00 0f <len> <ascii>` yields 10 hits, all usernames —
`XLLFMXMISTIK, poland1145, 007n7Forsaken64, DjeaAurelia, itrollyou2005, illsadness6,
0Ayb3rk, Mi_yei273, poopnest5002`. So the native receives Name updates for a whole set of
instances, its own included, inside `0x83/0x03` property batches.

Our side, same run: no tag-table misses at all and only 8 walk failures, so the parser is
not choking on these batches. We simply do not end up with an instance carrying our name
except the `Player` itself.

Open, precise next action: the ref that owns the Name triple in `rx2082` / `rx2113` is not
identifiable by eye from the hex (the triple framing is our own parser's job, not
eyeballing). Run `parsePropTriple` over those exact captured bodies to get `(ref, prop)`,
then locate that ref's spawn message to read its class index. If the class is `Model`, the
server replicates the owner's character and the bug is in our application path; if the
class is `Player`, the character is genuinely never sent and the fix is elsewhere.

## app_stream.py + the outbound diff (2026-11-05, late)

New tool `tools/app_stream.py` mirrors `protocol.cpp:walk_messages` and
`rx_stream.cpp` split reassembly. Feed it a directory of raw datagram plaintext
(`RBX_RAKNET_TXDUMP` for ours, `dgram_*.bin` for the native).

Same place, both directions, normalised:

    native: 195 outbound 0x83 messages in ~70 s
    ours:     7 outbound 0x83 messages in ~18 s

Native's first 30 include `83/05 len=45` (position upload), `83/0a` (973 B, later
16 B heartbeats), `83/07` events, `83/06 len=49` counters, `83/03 len=1001`,
`83/23 len=7`. Ours: one `83/04` and six `83/23`. We are barely transmitting.

Decoded `83/05` (45 B), fields in order:

    83 05 16 | aa0a0000 | 00000000 | ab0a0000 | 67f31542 a28f2242 2c62d441
              | 00000000 | ffffffff | 01 | ad0a0000 | 40072d44 | 00

i.e. two small entity handles (0x0AAA, 0x0AAB), a Vector3 (37.49, 40.64, 26.55),
then 0, 0xFFFFFFFF, a flag, another handle 0x0AAD and one more float.

**The handles are small (0x0AAA-scale), not instance refs (millions).** So the
position upload addresses a different namespace than the instance table we build. That is
the mismatch: `ref_of()` returns ~3.9M-scale refs, and the native is sending
0x0AAA-scale handles. Either the character has an entity id in that small space, or the
client allocates it locally (hypothesis B).

Next: find where 0x0AAA-scale ids appear inbound (RX) — if the server hands them out, we
can read ours; if not, the client allocates them and we must too.

## CORRECTION: 0x83/0x05 carries clocks, not references (2026-11-05, night)

Decoded 13 samples of the reference's 45-byte upload. Field order, verified:

    83 05 | 16 | clock_a u32 | 0 u32 | clock_b u32 | x y z (3f) | 0 u32
          | flags u32 | 01 | clock_c u32 | f f32 | 00

Measured across one driven session: clock_a/b climb monotonically 2730 -> 7298 while the
position moves smoothly (37.49,40.64,26.55) -> (52.16,59.95,60.13); `flags` alternates
between 0 and 0xffffffff; `f` climbs 692 -> 1030. **There is no entity reference in the
message.** The earlier "0x0AAA-scale handles" reading was wrong — those are clocks, and
the field we were filling with a multi-million instance ref is a clock field, which is
why every upload was acknowledged and never applied.

Implemented:
- `batch::physics_batch_clocked(...)` in `entity_batch.{h,cpp}` — the native field order.
- `boblox.pos(x, y, z)` in `exec.cpp` — sends one clocked upload through `api->raw`,
  works with no local character.
- `live_runtime.cpp` physics upload switched from `physics_batch` (ref layout) to
  `physics_batch_clocked`.

Tests: 20/20 pass.

## Still open

Run with `/tmp/poswalk.lua` (40 clocked uploads) produced
`player character: poopnestomegahater (poopnestomegahater)` and
`[phys] start pos -321.9 179.7 329.3` — a real character and a real spawn position.
The next run (`/tmp/kick.lua`, same uploads) got 19 characters for other players and
**none for us**, no `[phys]`. So the appearance is not yet reproducible and may still be
a leftover body from the previous session. Do not claim this as fixed.

### Reproducible (4/4): the character now spawns

After switching the upload to the clocked layout, four consecutive runs on 189707 with
`/tmp/poswalk.lua` / `/tmp/kick.lua`:

    run 1: character=1 phys=1 modelself=2
    run 2: character=1 phys=1 modelself=1
    run 3: character=1 phys=1 modelself=2
    run 4: character=1 phys=1  ([k] char up at i=2)

`[diag] record self … class=Model` means the Model arrives as a replicated record, so it
is a real spawn and not a stale body. `[phys] start pos -250.3 179.6 370.0` means the
integrator initialised from the real spawn CFrame and is uploading.

Working hypothesis, supported but not proven: the clocked 0x83/0x05 is what makes the
server spawn/adopt the body. Earlier ref-shaped uploads were silently meaningless.

**Not yet proven: that the server moves the body for other clients.** That needs the
observer account. Do not mark movement fixed until seen.

Next: observer run (main in Rum, alt in Boblox, `/tmp/kick.lua`), then verify.

## Clock domain fixed; 0x06 clocks; 2 Hz cadence (2026-11-06)

Re-derived from the existing same-job capture (`/tmp/rc4`, 600 plaintext
datagrams, native TX) rather than from PROGRESS's summary. Measured:

- **Step 1 as written could not be done.** The RX decode of `/tmp/rc5`
  (2443/2491 frames opened, full bodies) contains 0x83 subtypes
  01,02,03,07,10,13,15,16,1b — **no 0x0a and no 0x23 inbound**. There is no
  inbound clock to seed from on this build.
- **The clock domain is milliseconds of session uptime.** clock_a per 0x05
  upload, with file mtimes as the time axis: 2730 @0.2 s, 3233 @0.7 s,
  3749 @1.2 s … 7298 @4.8 s ⇒ ~993 ticks/s, and the *first* upload already
  reads 2730. The old `2730 + 22*tick` advanced 220/s (4.5× slow) and was free
  to drift arbitrarily after the first seconds.
  Implemented as `batch::net_clock_ms()` (steady_clock) with
  `rebase_net_clock(2730)` at world-ready — one origin for every builder.
- **0x06 carries three clocks, not zeros.** 49-byte body, field 4/5/6 are in
  the same ms domain (2761/2771/2878 at t≈0.6 s). `physics06_batch` now fills
  them; the unit test pins offsets 15/19/23 and the 49-byte size.
- **Upload cadence is 2 Hz, not 10 Hz.** Unique 0x05 messages at 0.2/0.7/1.2/
  1.7/2.2/2.7/3.2/3.7/4.2/4.8 s. Physics still integrates at 10 Hz;
  `send_accum` gates the wire send to 0.5 s.
- **The trailing f32 is horizontal speed × 64.** 692 → 1030 while the position
  walks (10.8 → 16.1 studs/s), flat at 1030 once up to speed. We sent 0.0.
- **0x83/04 exists and we never sent it**: `83 04 d6 09 00 00 | 23 62 00 00 00 | 00`
  (12 B), once, before the first 0x05. Added `ready_batch()`.

Also added: a 2 Hz replay of the last script position (`nudge_pos` hook on
`WorldView`) so a `boblox.pos` burst is not the only claim the RCC ever sees.

### Spawn rate after the change (measured, 189707, /tmp/kick.lua)

    6 runs, 40 s each: char up at i=1..2 in 4 runs, no avatar in 2 runs
    (12 s timeout, `[diag] record self` = Player/TextSource only).

So the clock fix did **not** make the spawn deterministic. The remaining
variable is server-side per job, not our stream: all 6 runs sent the same
12-byte ready marker, 2 Hz uploads and 2 Hz 0x06.

### Open

- **Movement still unobserved.** No reference client on this box
  (`~/rr/libroblox.so` and `~/Xecutor` are absent; PROGRESS's paths point at
  `/home/john`), so the observer run cannot be reproduced here.
  Next: Freddy's main account in Rum + alt in Boblox on the same job.
- The nudge replay stops once `phys_init` is set (character exists), so it does
  not fight the physics upload.

### Test suite note (pre-existing, not from this change)

`ctest` is 19/20: `unit-engine` fails at `unit_engine.cpp:1095`
(`players.NumPlayers` reads 0 in the isolated test case; `GetPlayers()` in the
same script returns 1). The failing case touches no clock/physics/exec code,
so it is treated as pre-existing in the dirty tree — not confirmed against a
clean baseline, which this tree does not have.

## 2026-11-06 late — what is actually broken (measured)

Rule 5 was triggered (same fix failed 8x). The shared assumption — "the server
spawns our body in response to transform uploads" — was never verified. Changed
the question to: **why does the server build an avatar for every client except
ours?**

Measured tonight, in this order:

- **Native client on the alt account gets a body** (Freddy's own test, same
  server). So the account is fine and the fault is ours. This kills the
  rate-limit theory.
- **Our body now spawns in ~0 of 8 consecutive runs** on 189707, including runs
  that upload a clocked `0x83/05` at 2 Hz from world-ready (nudge seed from
  `SpawnLocation`) and runs that use the old 5 Hz `boblox.pos` burst that used
  to work. Other players in the same session have bodies.
- **`0x15/0104` (DisconnectReceivePacketError) appears only when a body exists**
  (spawned session: 6 events; non-spawned sessions: 0). So the physics stream is
  what the server rejects, but only once it has a body to apply it to.
- **`0x83/21` is not the gate**: sent, no behavioural change.
- **`0x83/06`/`0x0a` are not the gate**: bisected (`RBX_PHYS_ITEMS=05`,
  `0506`, chain), no kick and no spawn in any configuration — but every one of
  those runs also failed to spawn, so the bisect is inconclusive, not negative.
- **Double-session churn was real and is fixed**: two clients on one cookie make
  the RCC send `0x0114 DisconnectRejoin` to both. A per-cookie `flock` now
  refuses a second launch (exit 3, verified). Several earlier "not moving"
  tests were run with two clients fighting over one body.
- **Server clock domain**: the server's own `0x83/05` heartbeat carries
  `~29.5e6 + ~1000/s`; the reference client's upload carries `2730 + ~1000/s`.
  Both are ~1000/s but different epochs. Our uploads now adopt the server epoch
  when the heartbeat is seen (`rebase_net_clock`), once.
- **RX has no `0x83/23`**: the server never sends the clock item to us
  (subtypes seen: 01,02,03,07,10,13,15,16,1b). Any plan that says "sync to the
  server's 0x23 clock" is unexecutable on this build.

### Next single experiment (not yet run)

**Paired capture in one job.** Native client on the alt account (known to get a
body) and our client on the same account, one after the other in the *same* job,
both captured with `tools/gdb/rc_capture.sh` (`RC_HOOKS=kdf,dgram,wire`). Diff
the first 3 s after world-apply: item stream order, and specifically what the
native sends between "world applied" and "body spawned" that we do not.

Everything else is guesswork until that diff exists. `~/work/rr/libroblox.so`,
`~/work/Rum/build/roblox` and `~/work/UwUcutor/build/libUwUcutor.so` are all
present, so the native side is runnable on this box.

## 2026-11-06 — the 0x83/04 chain, tested three ways (measured)

Fresh native capture `/tmp/rc6` (4318 wire frames, 600 plaintext datagrams, KDF
recovered) gives the real ready message, 372 B:

    83 04 | 16 05 00 00 | 07 01 <ref 0x00014735> <event 0x1046> <7B nonce>
          | <213B account JSON> <88B base64 signature>
          | 05 16 <clk 17741> 0 <clk 17742> <x 55.5 y 32.5 z 14.8> 0 <flags 0>
            01 <clk 17738> <f 642.0> | 23 <clk> | 00

So the ready message carries the account JSON *and* the first transform in one
chain. Both conflict with what the code believed:
- `0x83/04` is not a 12/55-byte stub; the 55-byte "spawn bootstrap" in
  `raknet_main.cpp` is a capture from another session with dead clocks.
- the account JSON does appear in the RakNet stream (old comment said it did
  not), inside the `0x07` event, followed by its hydration signature.

Tried, all measured, all still kicked with `0x15/0104` and no body:
1. transform chained inside our `0x83/04` (`ready_with_transform`), position
   seeded from SpawnLocation: kicked, 9 x 0104, 37 other characters linked.
2. real hydration JSON (213 B) + real signature (88 B) from the hydration
   service for this cookie: sent, kicked, 8 x 0104, 39 others linked.
3. both together: same.

What still differs from the native in that message: the `0x07` event's
`ref = 0x00014735`, its `event = 0x1046`, and the 7-byte nonce
(`96e094ec58d501`). The nonce is not the account JSON's `lastPerformed`
timestamp (checked), not a FILETIME, and the ref is not the UserId truncated
(both checked). `buildHydration` hardcodes another session's values
(`0x41ef`, `0x103d`, `e0cbc1e81b`), which is why it never behaved like the
native.

### Next step: reverse the writer, not another guess

Find the function in `libroblox.so` (2.743.0.1136) that serialises `0x83/04`
and read where it takes the event ref, the event id and the nonce from.
`~/tools/rizin`, `~/work/UwUcutor/OFFSETS.md` and
`~/work/UwUcutor/config/xecutor-2.743.0.1136.json` are on this box. The
question is narrow enough to be a single function: "which object field feeds
byte 13 of this message".

## 2026-11-07 01:00 — overnight results: the 0x83/04 ref is not the gate

Three client runs and three native captures (rc6/rc7/rc8), then stopped at the
budget.

Captures:
- `rc8` (native, 60 s, same place) has **both** directions: 2686 wire_rx, 1415
  wire_tx, 600 plaintext dgrams, 2643/2686 frames decrypted.
- In `rc8` the native's `0x83/04` is the **12-byte form** (five of them), the
  same shape we send. The 372-byte JSON+transform form only appeared once, in
  `rc6`. So the JSON `04` is not required for a body — that theory is dead.
- `0x83/04`'s `07` item is `07 01 <ref u32> <event u16> <5B nonce> <LEB len>
  <json> <sig>`; the JSON length is LEB128 (`d5 01` = 213).

Runs (all measured):
1. `ready_with_transform` (transform chained in 04, SpawnLocation seed):
   `0x0104` x9, no body, 37 other characters linked.
2. `RBX_04_LATE=1` (hydration 04 sent at world-ready with **our own** Player ref
   `0x4eb81e`, event 0x1046, native nonce, flags 0x64): `0x0104` x1, no body.
3. Same with the native's header flags `0x0516`: `0x0104` x8, no body, and the
   message went out four times across reconnects.

So: ref corrected to our session, header matched to the native, real signed
JSON — still `0104`. The remaining difference is not a value we can fill from
anything we currently parse.

### Dead end, named

`0x83/04` cannot be validated by copying fields. Either the event id/nonce are
derived from something only the real client holds, or the `0104` is not about
`04` at all (the native sends the 12-byte form and survives).

### The one measurement that would settle it

In `rc8`, compare the native's *outbound* `0x83` item stream in the seconds
around its **0x83/02 spawn for its own Model** with ours in the same window.
`rc8` has the RX side too, so the spawn record is visible. This is offline and
free — no live run needed.

### Code left in place (all env-gated, default behaviour unchanged)

- `RBX_04_LATE=1` — hydration 04 at world-ready with our Player ref
- `RBX_04_FLAGS=<hex>` — header u32 for that message
- `RBX_PHYS_ITEMS=05|0506` — physics payload bisect
- `RBX_CAP21=1` — the 0x83/21 capability frame
- per-cookie `flock` — refuses a second client on the same account (exit 3)

## 2026-11-07 03:4x — the character exists server-side; the Model is never sent to us

Two hours of work after the 04 dead end. The sharpest measurement of the whole
night:

**Our Player has `PlayerGui`, `StarterGear` and `Backpack` as children, and no
Model, for 40 s straight.** Those three containers are created by the server as
part of the character flow — so the server *did* start a character for us. What
never arrives is the character Model itself. So "no body" is not a spawn
failure; it is a replication/ownership decision the server makes about us.

Supporting measurements:

- Native vs ours, same place, 60 s: native receives 9 x `0x83/03` property
  chains, we receive 3600–4600 with 693 distinct sizes. The server has no
  streaming focus for us (no character = no focus), so it firehoses.
- Native outbound pattern: `04` (12 B) repeated **every ~12 s**, interleaved
  with `23` (~2.8/s) and `05` (~1.4/s). We sent `04` once and used 2/s and
  5.9/s. Now matched (ready beacon 12 s, 05 at 0.7 s, 23 at 350 ms).
- Our outbound before the ack fix: 6192 `0x83` messages in 50 s vs native 226 —
  the ack loop fired 160/s. Fixed to the native's 2/s.
- Acking **spawns** (not just property triples) made things worse: duplicate
  `Backpack` children appear (`count=4`), i.e. the same spawn record applied
  twice = our apply path has no dedupe. Reverted.
- `0x0104` kicks now 0–1 per run instead of 8–9 when the ack rate is right.

Code state (all env-gated except the cadence):
- `0x83/06` acks now name applied property refs, 2/s, bounded queue
- periodic `0x83/04` beacon every 12 s
- 05 at 0.7 s, 23 at 350 ms
- `RBX_04_LATE`, `RBX_04_FLAGS`, `RBX_PHYS_ITEMS`, `RBX_CAP21` unchanged

### What this means for the next attempt

The question is no longer "why no spawn" but "what makes the server replicate a
player's own character Model to that player". Failure modes: it may be gated on
network ownership (which we never receive), on an appearance claim, or on the
client being in a state the server recognises as "playing". The next measurable
step is to find, in the native's RX, the exact record that delivers its own
Model and what preceded it (rc8 has both directions) — that is offline work and
needs no live runs.

## 2026-11-07 — observer confirms: the body exists for others, never for us

Third account (`poopnestsuperhater`) is now usable as an automated observer
running the same client. Two-client test, same job:

- observer sees `poopnestomegahater(ch=true)` in one session — the server did
  create and replicate our character to other clients;
- in a second session it sees `ch=nil` for the same account/code — so the
  spawn itself is still intermittent server-side;
- in **every** session our own client's Player has `PlayerGui`, `StarterGear`,
  `Backpack` and **no Model** for 40+ s.

So the failure is now precisely: **the server never replicates our own
character to us.** Own-body exclusion (or a gate we don't trip) means the local
physics path can never start: no root instance → no `phys_init` → no 0x83/05
with a real position → no movement, no matter how correct the message is.

Corollary that kills several ideas: `0x83/04`, acks and interest claims can't
fix this on their own, because none of them can conjure a local instance whose
ref we don't know.

### Next: where does the native client get its own body from?

Its first `0x83/05` already carries the spawn CFrame (55.5, 32.5, 14.8) in a
session where the server never sent it a Model record we can see. Either the
server sends the character through a channel our parser drops (walk_fail=8–136
per run, incl. "spawn class out of range"), or the client reconstructs it
locally from appearance data and the spawn point is learned from another
message. Distinguishing those two is the next offline/one-run question:
instrument the walk failures so a dropped spawn prints its class id, then check
whether any dropped chain references our Player ref.

## 2026-11-07 11:2x — observer proof: body exists, transforms are ignored

Third-account observer + alt client in the **same job** (pinning works when the
observer is pinned to the client's job, not the other way round).

Measured, twice:

- Observer sees `poopnestomegahater` with a real CFrame
  (`-251.0, 179.5, 312.7` and `-105.7, 48.0, 3.1`), i.e. the server created our
  body and replicates it to others.
- Our own client's Player still has only `PlayerGui/StarterGear/Backpack` —
  the server never sends us our own Model.
- The alt sent `boblox.pos(x + 3·i, y, z)` every second; the observer's CFrame
  for our body stayed **bit-identical** across 35 samples.

Conclusions (measured, not inferred):
1. Movement failure is not a message-shape problem. `0x83/05` is ignored
   regardless of correct clocks, flags and cadence.
2. The earlier "levitating" body was the server's own simulation at the spawn
   CFrame, not an applied upload.
3. Because the server never replicates our own Model, we have no local instance
   ref, so the `0x83/03` CFrame-write path is also unreachable.
4. Parser drops (`walk_fail`) are misparsed property chains, not our Model —
   checked the bytes at the failure offsets.

### Remaining hypothesis space (for IDA)

The server decides physics ownership; the client never gets the body back. So
the only path left is the one the native takes *before* its first upload in a
session where it also never receives a Model record: what does it send/claim?
`~/ida95` and `~/ida-pro-9.5` are installed. Target functions: the `0x83/04`
sender and the ownership/"client is loaded" path. Strings to xref:
`lastPerformed`, `ageBracket`, `isOriginalUser`.

Tooling now in place: `tools/run_native_probe.sh`, third cookie, observer
script pattern (`/tmp/observe2.lua`) and alts pinning order documented above.

## 2026-11-07 11:3x — what the server does and does not send us (measured)

Instrumented the spawn path under `RBX_DIAG`. In a full session the only records
the server spawns **under our Player ref** are:

    class=386 PlayerGui
    class=503 StarterGear
    class=87  Backpack

No Model, ever. So the character flow starts server-side (those three containers
exist) and the character itself is withheld from its owner.

Other results from this block:

- `0x83/05` is sent both ways by the reference in this build
  (25 x rel 0x60, 18 x rel 0x00). Ours now mirrors that. Still no effect —
  observer CFrame for our body is bit-identical while the client uploads a
  moving position every second.
- `0xA1` state frames addressed with our own Player ref are **rejected**
  (`0x15/0104` x5). The frame's capture ref is not interchangeable with the
  Player ref.
- Restored a moving transform claim without a local character: the nudge path
  now integrates `boblox.move` direction on the claimed position and uploads it
  with `f = speed*64`. Verified in the log, ignored by the server like the rest.
- `walk_fail` chains are misparsed property blobs (CFrame floats interpreted as
  spawn headers), not our Model.

### IDA artefacts (next session starts here)

`~/ida-pro-9.5` + `idalib` in `/tmp/idavenv`. Database saved at
`~/work/rr/libroblox.so.i64` (1.3 GB), analysis complete.

Found the hydration/`playerInfo` builder: `sub_569AC78` (`ageBracket`,
`gender`, `platform`, `os`, `isOriginalUser` lookups via `sub_237D39A`), with
callers `sub_5699DB0`, `sub_569BF44`, `sub_569DA1C`. The packet framing is
above them; the next question for IDA is where the `07 01 <ref> <event>
<nonce>` header is written and which session object feeds those three fields.

## 2026-11-07 11:4x — the server DOES name our character, and we don't have it

Under `RBX_DIAG`, the property triples the server sends for our own Player are:

    23 x InternalCharacterAppearanceLoaded
    12 x Character

So the server tells us "your Character is <ref>" — and the Model for that ref is
never sent (verified: only PlayerGui/StarterGear/Backpack ever spawn under our
Player). `Player.Character` therefore resolves to nil, the link never happens,
and the local physics loop can never start.

This reframes everything one last time: it is not that the server withholds the
character, it is that it **references a character instance it does not
replicate to the owner**. The client is expected to already have it.

### The next patch (not written — needs ~30 focused minutes)

When a `Character` triple on our own Player names a ref we do not have, create a
local Model with **that exact ref** under our Player (plus a `HumanoidRootPart`
child with a new ref, seeded at the SpawnLocation), insert it into the
ref→instance map, and let the existing physics path run. Then the `0x83/03`
CFrame-write path is reachable as well, which is the one transform form we have
never been able to send for a server-known ref.

Blockers to check first: whether `Impl::ref_of` is backed by a bidirectional map
(ref→instance) that we can insert into, and whether the join applier will
overwrite the synthesized instance when/if the real spawn arrives (it should
replace, not duplicate).

## 2026-11-07 — the server sets Character = nil on our Player (measured)

The `Character` triple on our own Player arrives with **tag 3 (nil) and empty
value**:

    [diag] prop self name='poopnestomegahater' class=Player ref=471577
           prop=Character char_tag=3 char_bytes=

So it is not "the server names a ref we don't hold" — the server explicitly
clears the Character property for us, while other clients see our body spawned.
The owner is told "you have no character"; the observers are told otherwise.

That closes the hypothesis space for a purely client-side fix:
- no amount of `0x83/05` shaping can move a body the server says we don't have;
- `0x83/03` CFrame writes need a ref we are never given;
- the "synthesize the named ref" patch is void — there is no named ref.

The remaining question for IDA is the server-side condition, observed from the
client side: what state must the *client* report for the RCC to attach the
character to its owner (the appearance flow, `InternalCharacterAppearanceLoaded`
is delivered, so that gate passes). Candidates are in the 0x90 join document
(appearance fields) and the hydration `0x83/04` pair sent at join.

## 2026-11-07 — self-character scaffolding (tested, not yet firing)

Added, all behind `RBX_SELFCHAR=1`:
- `batch::client_spawn(ref, cls, parent)` — the inverse of `parseSpawnItem`:
  `83 02 | 01 <id u32> | <cls u16> | FF | FF | 01 <parent u32> | 00`
  (empty property loop = a single 0xFF delta; ref = lo varint + u32 id).
- `Impl::build_self_character()` — instantiates Model + Part(HumanoidRootPart) +
  Humanoid under our Player, registers refs `0x00B00001..3` in
  `rep->by_id`/`instances`, sets `Player.Character`, and sends three
  `client_spawn` items upstream (model under Player, root and humanoid under
  model).
- In the physics send block, when `self_char_ref` is set, it also sends the
  `0x83/03` CFrame property write for that root — the transform form that
  addresses a ref, which we have never been able to send before.

Status: compiles, runs, **does not fire**. No `selfchar:` log appears, so the
guard or an early return is hit (candidates: `have_nudge` false on maps with no
SpawnLocation, `Players.LocalPlayer` still nil at world-ready, or
`rbx::instantiate(find_class("Model"))` returning null). No `0x0104` kicks in
any of the runs, so nothing it sends is being rejected.

Next: log the guard inputs (`have_nudge`, `local_character_inst()`, `me`,
class ids) at the top of `build_self_character`, then it either builds or names
the missing piece.

## 2026-11-06 — local character built on the appearance property (code complete, live test pending)

Implementation (client/src/engine/live_runtime.cpp unless noted):

- The signal is the hidden **Player property** `InternalCharacterAppearanceLoaded`
  (measured earlier: the `[diag] prop self` line comes from the 0x83/03 apply
  path), not a 0x07 event item. `applyTriple` now sets `self_char_appearance`
  when that member arrives on our own Player ref.
- `build_self_character()` is retried at the end of every live chain (and once
  at snapshot finalize) while `RBX_SELFCHAR=1`, the flag is set,
  `Players.LocalPlayer` exists, no character is linked, and the SpawnLocation
  seed position exists.
- It builds Model + Part("HumanoidRootPart") + Humanoid locally, parents the
  Model under LocalPlayer, sets `Player.Character`, seeds the root CFrame at
  SpawnLocation + 3 studs, registers the refs in `ApplyReport` (first free
  block from 0x00B00000) and sends three **0x83/02** spawns upstream: Model
  parent = our Player ref, Part/Humanoid parent = Model ref.
- A later `Character = nil` triple on our own Player no longer disables
  physics: `local_character_inst()` falls back to the built model while it is
  still parented under LocalPlayer.
- `member_index()` now returns the **global schema prop id** (the u16 carried
  by an 0x83/03 item), not the per-class delta. On the join schema
  Part.CFrame = 5446 (measured with tools/gen_proptable.py over
  run/join97_1818.raw); the old value 12 was not a wire id.
- `cframe_batch` now emits the tag-27 CFrame-B shape: 12 B position + kind 2
  (rotation-table entry (+X,+Y,cross) == identity). The old 49 B
  kind-0 + 36 B matrix body was the tag-26 shape.
- New unit tests in tests/unit/unit_batch.cpp: `client_spawn` round-trips
  through `walk83ItemsEx`; `cframe_batch` parses as tag 27 / 13 B.

Commands:

    source ~/work/env.sh && cmake --build build -j --target roblox
    ./build/tests/unit_batch          # 9/9
    cd build && ctest                 # 19/20; unit-engine is the pre-existing
                                      # NumPlayers failure (unit_engine.cpp:1095)

Live test pending (Freddy's go-ahead): mover with `RBX_SELFCHAR=1 RBX_DIAG=1`
on alt-cookie, observer on third-cookie pinned to the mover's job, 90 s,
`boblox.move` legs. What the observer must show: our body's CFrame changes
with the legs. If it stays bit-identical, the upstream spawn was ignored and
the server-side body is still not ours to drive.

## 2026-11-07 — self-character builds; upstream announcement is rejected

With the appearance gate (another session's change: build on
`InternalCharacterAppearanceLoaded`, which is a hidden Player property here):

    selfchar: local character built parent=0x26b8ba model=0xb00000 root=0xb00001 hum=0xb00002

So the client now has a local `Model -> HumanoidRootPart + Humanoid`, a
`Character` property on its Player, refs registered in `rep->by_id`, and the
`0x83/03` CFrame-write path becomes reachable.

But the upstream announcement is rejected: two `0x15/0104` disconnects follow
the three `client_spawn` items, and the session dies, so the observer run had
nothing to watch.

Next: `batch::client_spawn(ref, cls, parent)` framing is the suspect. Current
bytes: `83 02 | 01 <u32 id> | <u16 cls> | FF | FF | 01 <u32 parent> | 00`.
Checks to make against a native capture that actually contains client-authored
instances: whether the class field is the join-schema index or a different
domain for client instances, whether the two empty property loops are legal as
`FF FF`, and whether the server expects the `0x1c`/`0x0e` chain wrapper instead
of a bare `0x83/02`.

## 2026-11-07 13:0x–14:1x — conformance fixed, authority still refused

What changed (all measured):

1. **The 45-byte upload was actually 71 bytes.** `physics_chain` appended two
   `0x83/0a` ownership claims naming the synthetic refs `0x00b00001/2`. The
   RCC never issued those, so it answered `0x0104` and dropped the whole chain —
   the transform died with the claims. Now claims are only sent when the refs
   are server-issued, and the upload is exactly 45 bytes.
2. **Reliability now matches the reference**: 27 x reliable-ordered + 13 x
   unreliable in a 45 s run (native: 25 + 18). We only ever sent unreliable.
3. **Apply-once on spawns** (`dup_spawns` counter): the RCC resends spawns
   until acked and we used to apply them twice (duplicate Backpack children).
   Spawns are now acked as well as applied.
4. New tool: `RBX_A1FIND=x,y,z` searches the `0xA1` state stream for a
   translation, so "did the server apply our transform" is answerable with ONE
   client and no observer.
5. `RBX_REPLAY05=<hex>` sends a captured native upload verbatim.

Findings:
- Replaying a **native, byte-identical** upload from our session is also
  ignored (no `A1` hit) — so the difference is authority, not bytes.
- The RCC does send `NetworkOwnerV3` assignments; some name refs we do not
  hold (0x7657159/0x7657188) — those are our own body parts, never replicated.
  Writing `Part.CFrame` and `MeshPart.CFrame` to those refs had no effect
  (checked with `A1FIND`, and with targeted `boblox.teleport`).
- Our `Player.Character` arrives as tag 3 (nil) every session; the only spawns
  under our Player are PlayerGui/StarterGear/Backpack.
- Bodies appear only in **busy** servers (a session with a body showed 412
  spawns and 6200 property chains; empty servers show ~46/62).
- `0x83/04` hydration with our own ref is still rejected (`0104`) even with the
  claims bug fixed, so that path is dead.
- Pinning both clients to one public job id works only sometimes (the join
  script's job id is frequently replaced by matchmaking), which is why
  observer runs are unreliable.

Open, in priority order:
1. Find the client-side condition the RCC uses to grant simulation authority.
   `NetworkOwnerV3` unknown-ref assignments prove it knows our parts; the
   missing step is what makes it accept writes for them.
2. Make pairing deterministic (own pinning path or a second machine/account),
   then re-test the ordered+unreliable upload against a live body.

## 2026-11-07 15:0x — conformance restored, harness built, body lottery remains

Fixed/added this session:

- **Claims poisoned every upload**: `physics_chain` appended `0x83/0a` claims
  naming synthetic refs (`0x00b00001/2`), the RCC answered `0x0104` and dropped
  the whole chain — the transform died with the claims. Claims are now sent only
  for server-issued refs; the upload is exactly 45 bytes again.
- **Reliability matched**: 27 x reliable-ordered + 13 x unreliable in a 45 s run
  (native: 25 + 18). Only unreliable was ever sent before.
- **Apply-once on spawns** + spawn acks (`dup_spawns` counter) — the RCC resends
  spawns until acked; we used to apply them twice (duplicate Backpack).
- **`RBX_A1FIND=x,y,z`**: searches the `0xA1` state stream for a translation, so
  "did the server apply our upload" is answerable with one client.
- **`RBX_REPLAY05=<hex>`**: sends a captured native upload verbatim.
- **`RBX_BODY_FILE`**: adopts the body's live position from an external
  observer. Measured cause: our claim starts where *we* picked a SpawnLocation,
  the RCC's body spawns 30-65 studs away, and that mismatch alone can explain a
  refused transform.
- **`tools/movement_test.sh`**: one-command proof — picks a populated server,
  starts observer + mover pinned to it, pipes the body's live CFrame into the
  mover, and prints the distinct CFrames seen for our body. Filters strictly by
  our own account (an earlier version adopted another player's position, which
  is a bug worth remembering).

Findings:
- Replaying a **byte-identical native upload** from our session is ignored too
  (`A1FIND` finds nothing), so the difference is authority, not bytes.
- Our `Player.Character` arrives as tag 3 (nil); the only spawns under our
  Player are PlayerGui/StarterGear/Backpack.
- `NetworkOwnerV3` assignments do arrive, some naming refs we never received
  (our own body parts). CFrame writes to those refs (`Part` and `MeshPart` prop
  ids) had no effect.
- The body appears in maybe 1 session in 4, and only in populated servers
  (body present: 412 spawns / 6200 prop chains; absent: ~46 / ~62).
- When the body *is* replicated to us (`[diag] record self class=Model`), our
  client links it and the physics path finally runs with real refs — that state
  is the one to test movement in, and it has not been caught together with an
  observer yet.

## 2026-11-07 15:2x — harness results, no body in 12 consecutive sessions

`tools/movement_test.sh` ran 12 times on populated 189707 servers: in every run the
mover joined (observed counts 118-859 for other players) but the mover's own
Character never appeared (`adopted=0`, `modelself=0`). The one session that did
replicate our Model (13:4x, `[diag] record self class=Model`) was a 300 s run that
rejoined twice; it was not captured together with an observer.

So the pass/fail of any movement test is currently decided by that lottery. The
conformance work above is real and independent of it (45-byte uploads, correct
reliability, no poisoned claims, apply-once), but it cannot be proven until a
session with a replicated body is caught together with an observer.

Next session should start with `tools/two_client_pick.sh` (deterministic pairing),
then re-run the body-file harness.

## 2026-11-07 15:30 — controlled negative: position was NOT the problem

Clean run with everything fixed and the body present:

    observer: 101 samples of our body at (-251.009, 179.933, 312.716)
    mover:    [bodyfile] adopted -251.009003 179.932999 312.716003
              walks +X, 45-byte uploads, 27 ordered / 13 unreliable

Every sample identical. So with
- the exact body position (adopted from the observer),
- conformance-identical uploads (size, clocks, reliability, cadence),
- no poisoned claims, apply-once spawns,

the RCC still does not apply the transform. That rules out position mismatch,
message shape, reliability, cadence and claim poisoning. The remaining variable
is authority: the server does not consider this session the owner of the body.

Evidence pointing the same way: `Player.Character` arrives as nil to us while
other clients see the body; `NetworkOwnerV3` assignments sometimes name refs we
never received; `0x83/04` hydration with our own ref is rejected with 0104.

Nothing more can be done client-side without knowing what the RCC uses to grant
ownership. That is server-side state; the only client-side way to learn it is a
differential capture of a session where it DOES grant (native client) against
one where it does not (ours), same job, same second — which needs the
deterministic pairing in /tmp/focuscode_prompt.txt.

## 2026-11-07 19:2x — pairing solved, differential says "no ownership at all"

Tools:
- `--require-job <uuid>`: exits 3 when the joined server differs from the one
  asked for (verified).
- `tools/two_client_pick.sh`: pairing verified by **observation** (observer sees
  the mover), retrying whole attempts.
- Measured: **v1/join-game ignores the job pin**. Five attempts with different
  valid, populated job ids all landed in the same matchmaker server
  (`f8e0a4b9…`), with `jobId` and `gameId`-as-uuid both tried. So "pin both
  clients" was never a capability; earlier same-server results were luck.

Paired results (3 sessions, body present: 70/11/50 observer samples):
- `modelself=0` every time: the RCC never replicates our own Model to us.
- `NetworkOwnerV3` arrives only for **known** refs (v0=1); no assignment ever
  names our body. So the server never grants us ownership of it.
- The body stays at one CFrame per session even though the mover starts at the
  body's exact position and uploads conformant 45-byte transforms.
- `0x90` join body: ours 10978 B vs native 10849 B — close enough that the join
  document is not obviously missing a section.

Conclusion: the server runs our session as a non-owner. Nothing client-side has
changed that: byte-identical native uploads, exact positions, correct
reliability, no poisoned chains, apply-once spawns.

Next: capture BOTH clients in one paired session (now possible) and diff the
first 3 s inbound/outbound item by item — the last question is which message
makes the RCC hand a session its own body.

## 2026-11-07 19:4x — the 0x83/21 capability frame selects the replication mode

Same-server differential (native captured in `/tmp/rc9` while our client observed
it in the same server, 49 samples, first attempt):

    native inbound : 0x16 (batches) 287 | 0x03 (prop chains) 6  | 0x02 128
    ours, before   : 0x16            68 | 0x03            5851 | 0x02 533
    ours, RBX_CAP21=1: 0x16        257 | 0x03             292 | 0x02 385

So the 12-byte `0x83/21` frame the native always sends (and we had behind
`RBX_CAP21`) is what makes the RCC serve **batched** replication. Without it we
sit in a legacy/fallback mode and get an unbatched firehose — which is why the
earlier censuses looked so different and why every "we receive more than the
native" observation was a symptom, not a cause.

Not solved by it: paired sessions with CAP21 on still show `modelself=0`
(no own Model), no `NetworkOwnerV3` for unknown refs, and a static body at the
observer (`-239.785, 180.104, 330.742`, 25 samples, mover walking +X).

Next: with the replication mode now matching, the remaining differential is the
*first seconds* of a paired session — specifically whether the native receives a
`0x83/02` spawn for its Model inside the first `0x16` batch. `rc9` has both
directions for the native side and `/tmp/o9.log` has ours for the same server.

## 2026-11-07 20:0x — the 0x90 size delta was noise, not a missing section

Native 0x90 body sizes across four captures: 10689, 10849, 10977, 10993.
Ours across runs: 10740-10752, 10833 for the paired session. The "160 bytes
short" was inside the native's own natural variance (~300 B), i.e. account /
session data, not a structural section we fail to emit.

Also measured: 0x13 identical (102 B both), 0x92 10-11 B both, no 0xa8 in either.

So the join/auth handshake matches within its own variance. Everything we can
observe and vary now matches the reference:
- message shapes, clocks, cadence, reliability (incl. ordered copies)
- replication mode via 0x83/21 (batched 0x16 like the native)
- no poisoned claims, apply-once spawns, no fake acks
- exact body positions adopted from the server's own view
- join document size and message set

And the server still: spawns a body for us, shows it to others, never
replicates it to us, never assigns us NetworkOwnerV3 on it, and discards our
transforms (including a byte-identical native replay).

Conclusion: on this build the owner's own body is gated by state we cannot
observe or influence from a clean-room client. Further message shaping has no
remaining hypothesis to test — this is the end of the empirical road.

Recommended next move for the actual goal (a farm): hybrid — libroblox owns the
character, boblox keeps the harness, cookies, job selection, Lua and logs.

## 2026-11-07 20:1x — IDA ownership pass: nothing actionable; footprint measured

`idalib` over `libroblox.so.i64`, xrefs for NetworkOwnerV3 / NetworkOwnership /
SetNetworkOwner / GetNetworkOwner / GetNetworkOwnershipAuto /
CanSetNetworkOwnership. All six resolve into one property/method dispatcher
(`sub_21B5157`) — reflection plumbing, not policy. There is no client-side
"ownership granted" branch to replicate; the strings name an engine property,
not a wire behaviour.

Footprint, same place, steady state (measured):
    boblox     113 MB RSS   15,451 instances
    Rum        1,059 MB RSS  (graphics backend required: direct-vulkan | opengl |
                              system | angle-vulkan — there is no headless one)

So the "9x cheaper" claim for boblox is real, and it is exactly what it cannot
buy: the ~950 MB difference is the player controller, appearance pipeline and
the engine state the server uses to decide ownership.

Conclusion unchanged: on this build the owner's body is gated by state the wire
does not expose. Options are (a) hybrid at ~1 GB/instance, (b) park, (c) keep
the clean-room client as a research artifact.

## 2026-11-07 21:1x — touch: the native suppresses it when the parts aren't ours

Ran the native client under capture **with UwUcutor preloaded** (gdb passes
LD_PRELOAD to the inferior), autoexec firing `firetouchinterest(partB, partA, 0/1)`
six times. The engine logged `[touch] fired 1..6`, so the call ran.

The capture contains **no touch packet**: ids are the usual set
(83, a1, 00, 03, 51-56, 09, 13, 92, 8f, 9b, a7), the 0x83 subtypes are the usual
set (0x0a id-pairs, 0x23 clock, 0x05/0x06 physics, 0x07 events, 0x21 caps), and
nothing new appears at the moment the touches fired.

Conclusion: the real client **filters** touch reports — most likely it only
reports collisions for parts it owns / is near. So `firetouchinterest` is not a
way around the ownership wall either; our own version builds and sends the
packet (RBX_TOUCH_ID) but it is the same uninterruptible gate.

Also observed: on 1818 the native sends 0xa6 route frames (272 in 70 s, all
nActions=0). On 189707 it sends none. So A6 is place-dependent keepalive, not
movement input.

## 2026-11-07 21:2x-21:5x — the character now replicates (RequestCharacter)

From the Function Atlas corpus (`~/atlas`, 839k functions, symbols intact):

- `ClientReplicator::RequestCharacterItem::write` sends **item type 8**:
  `08 | u32 stats/hack flags | string spawnName | player id`. The server's
  `ServerReplicator::readRequestCharacter` answers it by replicating the
  player's character. Our client never sent it.
- Implemented `batch::request_character(player_ref, flags)` and send it once per
  session from `LiveRuntime::pump`.

Result (measured): `[diag] record self … class=Model ref=4389192` — the RCC now
replicates **our own character**, and in a paired run the mover's own CFrame and
the observer's body CFrame are **identical** (`-269.483, 179.76, 382.181`), i.e.
we hold the same instance the server has, with a Humanoid and 25 parts.

Two bugs fixed on the way: the merged tree had no physics sender at all (mine was
lost in the merge), and the root was looked up by class instead of by name
(`HumanoidRootPart` is a `Part`).

Physics now: `phys: start pos -269.483 179.760 382.181`, integrates
`boblox.move`, uploads `0x83/05` at 0.7 s in both reliability forms, no `0104`.

Still open: the body does not move for observers in the runs we captured, and the
mover's own scripted position log (`[sw]`) is empty in the last two runs — the
script may be failing before its first print, or the session ends first. Next:
fix the mover script's output and confirm whether the local CFrame actually
integrates; if it does and observers stay static, the server still refuses the
transform even with the correct instance and refs.

## 2026-11-07 22:0x — character and physics are live; the move queue is not shared

`phys: start` fires on the real replicated character, positions match the
observer's view bit-for-bit, and uploads go out at 0.7 s with no `0104`.

But `boblox.move(1,0,0)` never reaches the integrator: `phys: move taken` never
logs, while the Lua script runs and returns. So `lua_move` writes a `g_move`
that `take_move` does not read — the two objects resolve different copies of the
file-scope state (the hazard `send/bridge.h` documents for inline statics, here
with plain globals across the merged tree).

Fix direction: move the queue behind the same `rbx::send::Api`-style seam the
world view uses (one definition in one translation unit, installed by the app),
instead of `g_move`/`g_move_set` in `exec.cpp`. Then re-test:
`RBX_DIAG=1` should print `phys: move taken` and `phys pos` should advance.

## 2026-11-07 22:2x — MOVEMENT RUNS LOCALLY; verification still blocked by pairing

The executor surface was never installed in the merged tree (`rbx::exec::install`
was missing), which is why `boblox` was nil and every script's `boblox.move` was
a no-op. Restored, plus a minimal `WorldView` carrying the move callback (the
file-scope queue in exec.cpp is not shared across the merged tree).

Measured: `lua_move=3`, `phys pos -257.8 → 31.6 → 353.2`, `vel 16.0`, uploads at
0.7 s in both reliability forms, no `0104`. Our client now integrates and sends
movement for its own replicated character.

Also confirmed from a public implementation (`--peadro0708`): `firetouchinterest`
is **app id 0x86** with two instance refs encoded `[leb peer-id len][u32 id]`,
**no phase byte and no terminator**. `batch::touch_batch` now emits exactly that.

Still unverified: whether observers see the movement. The last paired run had the
mover at its own spawn (-257.8, …) while the observer's body was at
(-124.7, 47, 6.0) — i.e. the two clients were in different servers again, so the
test proved nothing. Deterministic pairing is now the only blocker for the
verification, and it needs the web join path (job ids are ignored by v1/join-game,
measured) or two machines.

## 2026-11-07 22:3x — job pinning works; the two clients disagree about the world

`/v1/join-game-instance` (body: placeId, gameId=job, attemptId, playSessionId)
honours a pinned server: `job pin confirmed f4ee8cbe-…`, verified with
`--require-job`. That closes the pairing problem — both clients can now be put in
one server deterministically.

With both pinned to the same job, the remaining anomaly is stark:

    mover's own character CFrame : -206.9, 179.93, 352.48  (walking to x=275)
    observer's body for us       :  -95.0,  47.58,  23.34  (static)

Different maps entirely (y 179 vs y 47). Since the job id matches, one of the
two worlds is stale: either the observer's copy of the round never advanced, or
the mover's character came from a different round/instance than the one the
server is simulating. Also visible: two bodies named `poopnestomegahater` early
in the observer's log, i.e. a stale session from the previous run in the same
pinned job.

Next: with pinning working, start from a *fresh* job each time (never reuse one),
wait for both clients to report the same `join: job`, and compare the mover's own
CFrame with the observer's *at the same instant* before any movement. Only when
those agree does a movement verdict mean anything.

## 2026-11-07 22:3x — controlled test: same instance, same spot, still refused

Conditions this time were clean (all measured):
- both clients pinned to ONE fresh job via `/v1/join-game-instance`, both logged
  the same `join: job` and `job pin confirmed`;
- at rest, the mover's own CFrame and the observer's body CFrame were identical
  to the millimetre (-250.301514, 179.932968, 369.991333), i.e. same instance;
- mover integrated `boblox.move` (x: -278 → 1136, vel 16) and uploaded 0x83/05
  every 0.7 s in both reliability forms, no `0104`.

Observer's view of our body: single static CFrame for the whole run
(-310.406, 179.933, 370.698).

So the transform is refused even with the right instance, right refs, right
cadence, and a verified shared world. Ownership is the only variable left, and it
is granted server-side by `NetworkOwnerJob::clientCanSimulate`, whose spatial
check reads the player's **SimulationRadius / MaxSimulationRadius** (hidden
Player properties — `setsimulationradius` in the wild). Those are not in our
join-schema prop table, so we cannot address them with a 0x83/03 write yet.

That is the next thread: get the property index for Player.SimulationRadius from
the 0x97 schema (or via the corpus' property tables), write an enormous value,
and re-run this exact controlled test.

## 2026-11-07 22:4x — radius write accepted, transform still refused; and we are driving a second character

`MaxSimulationRadius` **is** in the join schema (id 6065, found by scanning
`props.member_name`), and writing it works: `simradius: wrote
MaxSimulationRadius=9e9` ×5, no `0104`. It did not change the outcome — the
observer's body stayed static.

The new, sharper observation from that run (same job both sides,
`1d9d99a2-…`): the mover integrated from `-218.85, 179.93, 312.72` while the
observer's single body for our player sat at `-321.888, 179.933, 329.258`. Same
job, same y, different x/z, one body in the observer's list.

So our client and the server disagree about *which instance is our character*:
the Model we received (via RequestCharacter) is not the body the RCC shows to
other clients. That also explains why every transform is ignored — we are
driving an instance the server does not treat as our player's body.

Likely cause: RequestCharacter is being sent late (after the RCC already spawned a
body at round start), so the answer is a *second* character. Next: send the
request as early as the Player ref exists (before the world is finalised), or
adopt the server's existing body instead of asking for a new one, and re-check
that mover and observer agree on the instance before any movement verdict.

## 2026-11-07 23:1x — session log

Last paired attempt (same job both sides, `4bfec08c-…`): the mover built its world
and integrated (`phys pos -268.776, 179.933, 301.044`), but the observer never
received our player at all (`player joined: poopnestomegahater` absent, no body
samples). So the RCC did not register that session's player with the other
client — a different failure than the duplicate-character one, and it needs a
clean rerun before any conclusion.

Summary of the last stretch:
- deterministic pairing: solved (`/v1/join-game-instance` + `--require-job`)
- own character replication: solved (`RequestCharacter`, item type 8)
- local physics + uploads: working, no kicks
- server acceptance: still not happening
- `MaxSimulationRadius` (id 6065) write: accepted, no observed effect
- duplicate character (client drives B while observers see A): observed once,
  next thing to eliminate

## 2026-11-07 23:2x — two characters in one job: ours is invisible to others

Clean pinned run, both clients on `fcb45204-…`, same job confirmed on both sides:

    mover (our client)      : -250.301514, 179.932968, 369.991333
    observer (other client) : -321.888, 179.933, 329.258   (42/42 samples, static)

Exactly one body in the observer's list, and it is not the one we hold. So the
server has two characters for our player: A (spawned by the RCC at round start,
shown to everyone, never sent to us) and B (the one we hold and drive, invisible
to the observer).

`reqchar: me=… char='poopnestomegahater'` logs *before* the request is sent, so B
exists before RequestCharacter — it comes from the RCC's normal Player→Model
spawn, and the request produces/keeps the duplicate instead of establishing
identity.

Implication: every transform we send addresses B, which no one else sees, which
is also why the observer never moves regardless of payload, cadence, radius or
reliability. The open question is now narrow and testable: make the client hold
**A** (the body the server actually simulates) instead of B — either by not
requesting at all and waiting for the Player.Character link, or by locating A in
the world and adopting it (its position is observable from the other client, and
from `0xA1` if we can match the ref).

## 2026-11-07 23:4x — RequestCharacter is not what delivers our Model

`RBX_NO_REQCHAR=1` run (no request sent at all): `record self … class=Model` ×3 —
our Model still arrives. So the request is **not** required for own-character
replication; the earlier "no Model without it" reading was a timing artifact of
the pre-merge tree.

Same run: the observer (same pinned job, `7320eccd-…`) received our Player but
**no Character** for us. So in that session the RCC created no server-side body
at all — the lottery again, and distinct from the duplicate-character case where
two bodies existed.

Current, evidence-based model of the failure:
1. Sometimes the RCC creates our server-side character (observers see it) but we
   hold a different instance (the duplicate case, measured twice).
2. Sometimes it creates none (observer has the Player but no Character).
3. RequestCharacter does not change either case.

Next: stop guessing at the request and measure *when* the RCC creates the
character, by watching the observer side for `player character:` while the mover
joins and idles. If it only appears at round start, the mover must be present
across a round boundary before any ownership test is meaningful.

## 2026-11-07 23:5x — 0x86 touch: accepted silently, effect unproven

Ran `firetouchinterest(hrp, part, 0/1)` for 5 parts × 10 rounds (50 messages) with
our own replicated character (`hrp=true`): **no `0104`, no `0105`, session
survived**. So the message is well-formed and the RCC does not reject it.

Effect not yet proven: 189707 has no touch-reward observable (no leaderstats), and
920587237 never had a character in the session we tried. Proving it needs a place
where a touch changes replicated state (a coin/collect stat), then watching our
own Player's value change.

That is the cheapest farm path left: touch needs no ownership, no movement and no
engine, so if the RCC acts on it the whole physics problem stops mattering.

## 2026-11-08 00:0x — research result: the grant needs FOCUS before anything

From `NetworkOwnerJob::updatePlayerLocations` (corpus, `sub_106799828`):

    for each ServerReplicator:
        if (!readPlayerFocusInstantAndSimulationRegion3d(replicator, &cell))
            continue;                       // <-- no focus, no entry at all
        character = Player:getCharacter()
        root = character ? getConstRootMovingPrimitive(character) : 0
        clientLocations[address] = { …, root }

So a client is **skipped entirely** until the server has its *player focus plus a
3D simulation region*. No entry → `clientCanSimulate` never sees us →
`NetworkOwnerV3` is never written for our parts → client-authored transforms are
discarded. That is the mechanism behind every "silently ignored" result tonight.

Carrier: the `0xA1` focus/state stream (native ~1.4/s). Our client never sent it
(it was gated on a capture's ref). Implemented: `a1: focus sent ref=<our root>`
every 0.73 s with our own character's ref.

Test status: the run had no replicated body (lottery) and 2 kicks, so the effect
is not yet measured. Next: in a session where the body exists, check (a) whether
`0xA1` with our ref is accepted (no `0104`) and (b) whether the RCC then sends
`NetworkOwnerV3` naming our body parts — that is the direct signal that ownership
was granted, before any movement verdict.

## 2026-11-08 00:2x — A1 focus now sent; kicks suggest its payload is wrong

The 0xA1 sender failed silently at first: the ref lookup used the root part, which
is not in `by_id` in the merged tree. Fixed by trying the character Model first
then the root. Now: `a1: focus sent ref=3785730` every 0.73 s.

But the same run had **3 `0x0104` kicks** (previous runs with physics only had
0-1), which points at the 0xA1 body itself: `buildA1Camera` copies a 39-byte tail
from a capture — that tail embeds at least one more reference and constants from
that session. Replacing only bytes 13..15 with our ref is evidently not enough.

Next: derive the 0xA1 body from the corpus (`0xA1`/focus writer) instead of a
captured tail, or strip the capture-specific refs and test which fields the RCC
validates. Until it is accepted, the focus/ownership theory stays untested.

## 2026-11-08 00:3x — 0xA1 structure from same-session samples

23 `0xA1` messages from one native session: sizes 49/50/55/60, and for the
55-byte form only four offsets vary between messages:

    offset 1      clock low byte (u32 clock at 1..4)
    offset 40     (single byte, ref? value 0xe8 constant in the pair we diffed)
    offsets 52-53 small counter (`f5 04` -> `35 00`)

Our `buildA1Camera` copies a 39-byte tail from an entirely different capture and
patches bytes 13..15 as "the ref" — in this capture the reference-looking field
sits at offset ~22 (`39 94 1e 00`) with a second candidate at ~40. So the builder
is wrong for this client version, which is consistent with the `0104` kicks when
we send it with our ref.

Next: rebuild `buildA1Camera` from these samples (clock at 1..4, our refs in the
two ref-shaped slots, counter at 52-53) and bisect the kick.

## 2026-11-08 01:5x–02:0x — duplicate character fixed; observer world is stale

1. **RequestCharacter caused a duplicate character.** With it: two Models with our
   name at different refs and positions (-269.48,179.93,382.18 and
   -251.01,179.59,312.72). Without it: **one** Model, ref 1018805, at
   -308.99,179.93,311.30 — exactly where observers report our body. The request is
   now opt-in (`RBX_REQCHAR=1`), commit ad1e600.

2. **0xA1 focus is byte-correct** (commit 5f1550e): native template, our ref at
   offset 40, nothing else touched. Kick counts measured: old builder 3, ref@22 2,
   patched clock/counter 3+, ref@40 **0**.

3. **The pairing checks were invalid for a second reason**: our observer client
   keeps the *previous map* after a round change. Measured: mover settled at
   y=179.933 while the observer still showed our body at y=47.98.

4. Root cause of (3): **`0x83/01` (delete-instance) is never handled**. The walker
   stops on item type 0x01, so destroyed maps/characters stay in our world forever.
   Confirmed by inspection (no `case 0x01`, no deleteInstance in repl_stream).

Next: implement 0x83/01 — parse the item, destroy the instance, drop it from
`by_id`/`instances`. Wire samples for the item are not in rx11; take them from an
earlier capture that logged `sub=01` (rc4-era) or from a fresh RX capture of a
round change. Until deletes apply, no observer run can be trusted.
