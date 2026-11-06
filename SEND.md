# Send path

The upstream (client → RCC) path, as a module. The transport has been able to
send since the RakNet handshake was cracked; what this adds is the **stream**:
the native's 13.3 Hz `0xA6` route cadence, step continuity, a ControlState
block with explicit slot control, and an executor surface for driving it from
a script.

Source: `client/src/raknet/send.{h,cpp}` · seam: `client/src/send/bridge.{h,cpp}`
· tests: `tests/unit/unit_send.cpp` · sweep: `run/input_sweep.lua`.

## What is proven

| claim | pinned by |
| --- | --- |
| route frames are byte-identical to the captured native shape | `unit_send` vs `input::routeFrame` |
| cadence is 75 ms (13.3 Hz), no catch-up burst after a stall | `unit_send` |
| stepIds advance +4 per message, frame index 0,1,2,3,3 | `unit_send` |
| a ControlState block replaces the final `nActions` byte | `unit_send` |
| `hold` is sustained, `control` is one-shot, `release` clears | `unit_send` |
| transport failure does not advance the step | `unit_send` |
| the executor bridge dispatches hold/control/release/raw/ready | `unit_exec` |
| the marker ack (`0x04` for a server `0x84`) is sent once per marker | live, 2026-10-12 |

## What is NOT proven — say it plainly

**The slot → action mapping.** The bool sweep over slots 0–7 (presence bit set,
value 0 and 1, sustained holds) produced no character spawn in three full
sessions (2026-10-12). Our Player is assigned an id; our Character Model never
appears. Until a slot/value pair spawns it, there is no position to echo, so
movement cannot be verified either.

Remaining hypotheses, in the order the evidence points:

1. non-bool slot magnitudes (the block may carry a type tag, not just a value)
2. `A`/`B` liveness (both are written zero today; the native writes its own
   input clock — zeros may be treated as "no state change")
3. the camera blob (`0xA1`, 11 bytes, body contains a session-specific instance
   id — held back until the reference semantics are recovered)
4. per-frame rather than per-message ControlState (`Placement::EveryFrame`
   exists in the module for exactly this)

None of these can be settled by more code. They need ground truth: the native's
own bytes while a key is actually held.

## The next discovery step (not built)

The native client sends nothing while idle, so a passive capture has no driven
frames to diff. The 2026-10-12 plan stands: interpose `SDL_PollEvent` in Rum
(SDL3 is dynamically linked — `libSDL3.so.0` in `ldd Rum/build/roblox`), inject
W-down / hold / up, and capture the native's upstream window. Decryption needs
the session keys; the oracle for the right candidate private key is the GCM tag
on the first post-handshake datagram, against the server's
`EphemeralEarlyPubKey` from the join script.

That work belongs in `tools/`, not in this module. Until it lands, this module
is the safe, spec-shaped driver for experiments — not a working autofarm.

## Executor surface

```lua
boblox.send.ready()                 -- true once the data AEAD keys are installed
boblox.send.hold(mask, values)      -- sustained ControlState (mask/values 0..255)
boblox.send.control(mask, values)   -- one frame only
boblox.send.release()               -- clear
boblox.send.raw(payload, ordered)   -- one raw application message (id + body)
```

`mask` is the 8-bit presence mask (`0x01` = slot 0), `values` the matching value
bits. Both are explicit on purpose: nothing here guesses which bit is "forward".

## Live procedure

```sh
# alt account, short session (the RUPP token is not rotated, ~160 s wall)
send_s() { ./build/client/roblox --connection-type=RakNet --place-id 1818 \
  --roblosecurity "$(cat "$1")" --exec "$2"; }

send_s ~/.cookie.alt run/input_sweep.lua | tee /tmp/sweep.log
grep -E "\[sweep\]|local avatar|0x15" /tmp/sweep.log
```

Verdict lines: `raknet: local avatar linked: <name>` is the hit signal.
`0x15 01 04` (receive error) means the frame shape was rejected — stop, and
compare the emitted block against `input::controlBlock`.

## Safety rules kept from FINDINGS

- No blind writes: the server watches (`0x9B`), and a malformed upstream frame
  kills the session inside ~300 ms.
- Prefer the alt account for the first writes on any new shape.
- Stay under ~150 s per session until the RUPP token rotation is handled.
- One novel shape at a time, so a death attributes to exactly one change.

---

# Native capture (2.743.0.1136) — 2026-11-05

The ground-truth path is now working end to end: drive the native client, capture
its upstream, decrypt it. Tools: `tools/native_probe.c` (LD_PRELOAD probe),
`tools/native_capture.sh` (runner), `tools/nat_decode.c` (offline decoder).

```
cc -shared -fPIC -O2 -o tools/native_probe.so tools/native_probe.c -ldl
cc -O2 -o tools/nat_decode tools/nat_decode.c -lcrypto
BOBLOX_DRIVE_MODE=seq ./tools/native_capture.sh 10449761463 /tmp/nat.bin
./tools/nat_decode /tmp/nat.bin > /tmp/txplain.txt
```

## What the probe does

- interposes `sendto`/`sendmsg`/`recvfrom`/`recvmsg` and dumps every datagram
- interposes `getrandom` (see below) and dumps entropy draws
- interposes `SDL_PeepEvents` (Rum drains with it, **not** `SDL_PollEvent`) and
  injects key or finger events; `BOBLOX_DRIVE_MODE=key|touch|seq`
- on Req2, scans `/proc/self/mem` for the client public key and dumps ±64 KB

## Key recovery (the part that was not obvious)

`libroblox.so` draws its X25519 scalar through the **getrandom syscall**, not
libc, so the `getrandom` interposer sees nothing (0 candidates in 84 draws).
The scalar is instead recovered from memory: the client public key is plaintext
in Req2 at offset 23, and every 32-byte window of the probed regions is tested
against it with `X25519_base`. The handshake peer key is then recovered by using
the Reply2 ChaCha20-Poly1305 tag as an oracle. Both land within 300 bytes of a
public-key hit, which is why a ±64 KB dump is enough.

## Findings

1. **There is no `0xA6` in this build.** Across four sessions (idle and driven)
   the client sends zero app id `0xA6`. The gameplay upstream is `0x83/0x0a`
   batches. `input.cpp`'s route frames are the 2.739 shape; the 2.743 client
   moved the carrier.
2. **The carrier is proven by rate.** `0x83/0x0a` runs ~15/s idle and ~41/s
   during a sustained touch hold, then returns to ~15/s. 45 → 124 → 54 frames
   per 3 s window.
3. **Touch injection works.** Rum logs `[input] first touch event reached
   Roblox native input`, and the client answers with the rate change above.
4. **Structure.** `83 0a` then items; a route item is
   `01 <ref3> 00 b8 15 01 <clock u32> 00 00` (16 B). While moving it is 17 B —
   one extra trailing byte. The 4-byte field is a clock (increments steadily in
   both directions), so the movement vector is not in the route frame.
5. **Open.** The direction is not yet isolated. Forward drags produced 88
   repeated `0a` sub-items vs 21 for backward in the same session, and the `05`
   items carry CFrame floats; one of those two is the vector. Next experiment:
   hold forward, then left, then right in one session and diff the sub-item
   contents.

## Consequence for this module

The `0xA6` stream `send.cpp` emits is still the shape the RCC historically
accepted, but it is **not** what the 2.743 client uses for movement. A working
`hold()` needs the `0x83/0x0a` carrier; that is the next change, not a tweak to
`input.cpp`.

---

# Remote + movement encoding — cracked 2026-11-05

Method: run Rum with **both** preloads (`BOBLOX_EXECUTOR=1`, probe first, UwUcutor
second), send a Lua script over `/tmp/xecutor.sock` that fires a RemoteEvent with
a 21-byte ASCII marker and calls `Humanoid:Move(0,0,-1)`. The marker and the
vector are visible in the decrypted plaintext, so the encoding is read, not guessed.

Script: `tools/remote_probe.lua`.

## RemoteEvent FireServer

One RakNet message, app id `0x83`, sub-id `0x03`, containing an event item per call:

```
83 03                                  ID_DATA / replication batch
  01 9e f8 3a                          item 01: entity selector, ref u32 LE
  22 08 0f 00 00 00 00 00 00 00 00 00 80 bf   item 22: prop 0f, Vector3(0,0,-1)
  07 01 37 99 02 00 bb 11 01 02 15 <21B>      item 07: event
  07 01 38 99 02 00 bb 11 01 02 15 <21B>
  07 01 39 99 02 00 bb 11 01 02 15 <21B>
00                                     batch terminator
```

Event item layout: `07` `01` `<ref u32 LE>` `<event id u16 LE>` `<argc u8>` `<args>`.
Arg `02 <len> <bytes>` = string. The three refs (0x029937/38/39) are the three
RemoteEvent instances; the shared id `0x11bb` is their event index in the join
schema (RemoteEvent's `OnServerEvent`).

## Humanoid move vector

Same batch, item before the events: `22 08 0f` followed by 12 bytes =
`Vector3(0, 0, -1)`. It follows an `01 <ref>` entity selector, so it is a
property write (`0f`) on the local Humanoid.

**Consequence:** movement is not `0xA6` and not `0x83/0x0a` — it is a property
write in `0x83/0x03`. `0x83/0x0a` is streaming/interest (instance refs, rises
when new parts stream in), which is why it looked like input.

## Still to pin

- Arg type tags for number / bool / Vector3 / Instance (only `02` string proven).
- The exact property id for the move vector per class (0x0f observed) and whether
  jump uses the same item.
- Whether the RCC accepts our own `0x03` batches or requires the entity refs to
  exist first (they must: the item is a write against a live instance).

## Encoder (shipped 2026-11-05)

`client/src/send/entity_batch.{h,cpp}` builds the two proven batch shapes, and
`unit_batch` pins them byte-for-byte against the capture.

```
event batch : 83 07  01 <ref u32> <event u16> <argc> <args...> 00
property    : 83 03  01 <ref u32> <prop u16> <value>           00
args        : 02 <len> <bytes> | 09 <0|1> | 0c <f64> | 16 <f32 x y z>
              argc 0 = no arguments
```

Executor surface: `boblox.fire(ref, event_id, {args})` and
`boblox.vector(ref, prop_id, x, y, z)`, both delivered reliable-ordered.

Next gap is not encoding: it is exposing an instance's replication ref and its
class event index to Lua, which lives in the join schema the client already
parses.

## Live blocker (2026-11-05)

`sendJoinFlow` holds the 0x90 back unless a join template is present:

```
raknet: 0x90 template run/raknet90_template.bin missing ... join body held back
```

Without it the client sends no 0x90/0x8a, the RCC answers `0x8a`/`0x90`/`0x97`
disconnects, and no snapshot ever arrives. Both `--90-template` candidates in
the tree fail their own checks (`run/cpp_msgs/m90.bin`: "too short for its own
length"; a native-captured 0x90: "no {\"UserId\" JSON", because the native body
is sealed). `run/raknet90_template.bin`, `raknet8a_template.bin` and
`raknet_pre_join.bin` are untracked data files — a working copy has them.

## Corrections (2026-11-05, later)

- **`0x83/0x0a` is not input.** It is streaming/interest: instance refs that rise
  when new parts stream in. Movement is a `0x83/0x03` property write.
- **`key8a` uses the truncated 32-bit place id**, not the full id. Verified by
  unsealing a live 0x8a with `1859826871-17338` (place 10449761463 mod 2^32) and
  parsing it; the uint64 form does not parse. Do not "fix" this to uint64.
- **The `0x8a` key is 16 chars**, so for an 11-digit place id `tail2` never
  enters the key. `tail2` needs no RE.
- **Ordering slots moved in 2.743**: `0x90@2, 0x92@3, 0x8a@4, 0x8f@5, 0x9b@6`
  (2.739 was 1,2,3,4,5). One slot is reserved before the join body; verified.
- **`0xA6` does not exist in 2.743.** The route stream is now opt-in (`RBX_A6=1`).
- **Live join still stops before `0x9B`** even with body, key, ordering and
  sequence matching the native. Every measurable axis matches; the residue is in
  the datagram token layer or the join ticket itself.
