# NDS 0x15 disconnects: ground truth and fix

Status: 2026-11-13. This documents the live `0x15/0104` and `0x15/0105`
disconnects on place 189707 (NDS), the evidence used to identify the cause, and
the fix now in the tree.

## What 0x15 is

`0x15` is RakNet `ID_DISCONNECTION_NOTIFICATION`, not a Roblox app message. Its
body is:

```
15 00 00 01 04
^  ^^^^^ ^^^^^
|  |     +-- u16 BE ConnectionError code
|  +-------- u16 BE zero
+----------- packet id
```

The native `ConnectionError` names are present in `~/rr/libroblox.so`:

```
DisconnectReceivePacketError
DisconnectReceivePacketStreamError
```

and the native client's disconnect switch is at `libroblox.so` `sub_33E7C3F`
(described in `client/apps/raknet_main.cpp`). The codes observed here are:

| code | enum | meaning |
|---|---|---|
| `0x0104` | `DisconnectReceivePacketError` | the RCC failed to parse a received packet |
| `0x0105` | `DisconnectReceivePacketStreamError` | a received stream could not be parsed/continued |

Both are **server-side receive/parse** verdicts. They are not a liveness timer:
the liveness timeout is `0x010a`/`266` (`DisconnectTimeout`), and a `0x0114`
(`DisconnectRejoin`) is a deliberate server rejoin request.

The native client does not answer a `0x15` with a protocol reply. It tears the
connection down and reports the reason; a normal reconnect is a fresh join. Our
handler already does the same and the fixed run never reaches that path.

## Isolating the offending stream

The RCC sends the same `0x0104` with every optional stream disabled except the
one under test. All runs used place 189707 and the same account.

| streams enabled | result |
|---|---|
| all normal streams, A1 state records included | `0x0104` at 0.9-14 s |
| A1 sender disabled (`RBX_A1_TX_OFF=1`) | stayed joined 65 s, 4,560 bodies |
| `0x83/04` spawn bootstrap only | stayed joined 35 s |
| A1 state sender only | stayed joined, but sent only the empty 13-byte form |
| A1 **record** only (`Player.CameraViewportSize`) | `0x0104` at 3.5-14 s |
| A1 **record** only (`Humanoid.NetworkHumanoidState`) | `0x0104` at 0.9 s |
| ACKs / NAKs disabled, all app streams disabled | still disconnected, so ACK/NAK encoding is not the cause |
| `MaxSimulationRadius` or `ReplicationFocus` property writes | `0x0105` at 4.8-10 s |

So the kick is caused by the synthetic 0xA1 instance-state records, not by
RakNet ACKs, the spawn bootstrap, physics uploads, or the clock/input streams.
The property-write probes are a separate, also-rejected path and are not part of
the connection fix.

## Native A1 shape

Native 1818 capture `run/parity/r7e1/native/dgram_90.bin` contains a complete
0xA1 frame inside a 51-byte unreliable datagram:

```
81 <dgram> 00 01 60
a1 8c000000 01000000 786ffeff 01000000
01 055d0100 7f01 0900 0001000000000000 0800 00c0aa44 00003844
```

Decoded:

- tick `0x8c` (140 ms), records `1`, ack `0xfffe6f78`, flags `1`
- ref `0x15d05`, class `383` (Player), member_count `9`
- mask bit `8`, body length `8`, value `1366.0 x 736.0`

The 48-byte Player vector frame and 37-byte Humanoid frame in the same capture
use the same grammar. The ack slot is the **one's complement** of the server
timestamp (`0xfffe6d3c` while the server tick is `0x000192c3`); it is not zero.

## Why our records were rejected

The stream has two different index spaces:

- the class **member list** (what the schema exposes), and
- the per-class **writer slot table** carried by the `0x97` join tail.

A 0xA1 record's class + mask + `member_count` address the **writer slot table**.
On 189707 the join schema is:

```
classes=644 members=10411 shared=127 slot_classes=0
```

There is no writer table for Player or Humanoid. Our record builder fell back to
the member index and the class member count (Player class 389, 61 members,
CameraViewportSize at member 8). The RCC decoded mask bit 8 against its own
writer table and read a different property width/type, which is exactly a
receive-parse failure (`0x0104`). Sending the record with `member_count=9` did
not help; the mask still addressed the fallback slot, and both the viewport and
Humanoid records triggered the same kick.

The native empty keepalive is the 13-byte form (`a1 tick records=0 ack`). It
carries no mask and is accepted even when no writer table is available.

## Fix

`classHasWriterSlots()` (in `client/src/solve/replicate.h`) is now the single
gate for non-empty A1 frames. `LiveRuntime` emits:

- the native 13-byte empty keepalive every 5 s, always; and
- a viewport/vector/humanoid record only when the live `0x97` schema actually
  has a writer slot table for that class.

On 189707 the gate logs:

```
0xA1 state records gated: no writer slot table for class 389 (empty keepalive only)
```

and the session stays connected. Once a schema supplies the table (the 1818
server does), the record path uses the slot/count from that table, matching the
native frame layout.

This is not a disconnect special-case: no `0x15` is ignored or answered, and no
malformed record is sent. It removes the fabricated record that the RCC could
not parse and preserves the native keepalive that it can.

## Live result

```
join schema classes=644 members=10411 shared=127 slot_classes=0
0xA1 state records gated: no writer slot table for class 389 (empty keepalive only)
raknet: physics t=10s bodies=2779 ... dynamic=0
raknet: physics t=20s bodies=3524 ... dynamic=0
raknet: physics t=30s bodies=4850 ... dynamic=0
raknet: physics t=40s bodies=5247 ... dynamic=0
raknet: physics t=50s bodies=6028 ... dynamic=0
raknet: physics t=60s bodies=6769 ... dynamic=0
raknet: physics t=70s bodies=7276 ... dynamic=0
raknet: physics t=80s bodies=7315 ... dynamic=0
```

90 seconds, no `0x15`, no rejoin, world still streaming. The earlier 65 s run
also passed. Crossroads still reports `authority=Enum.AuthorityMode.Server`,
`dynamic=0`, character grounded.

## Native ownership check (2026-11-13)

The native client was run against the same 189707 server with the same account
(`~/Rum`, `~/rr/libroblox.so`; 92 s pre-encryption capture in
`/tmp/native189707`). The capture contains 14,812 decrypted RX application
messages and only two `NetworkOwnerV3` property items (`prop 5662`, value
`01`). There is no 1016-style local grant.

The client API is not a way to ask for ownership either. `BasePart:GetNetworkOwner()`
in the native client returns:

```
Network Ownership API can only be called from the Server.
```

The property itself is a hidden, non-scriptable `SystemAddress`; the native
client exposes no client-side ownership query. Boblox raises the same
server-only error from both `GetNetworkOwner()` and
`GetNetworkOwnershipAuto()`.

The behavioural test used the same method as our Jolt probe. Native lifted
eight unanchored, collidable map parts (`Workspace.Structure.Devastation
Station.Pine.Part`) by 30 studs and waited 1.8 s:

```
beforeY=56.80 liftedY=86.80 afterY=86.80 moved=false
beforeY=84.40 liftedY=114.40 afterY=114.40 moved=false
...
beforeY=260.60 liftedY=290.60 afterY=290.60 moved=false
```

The native client did not simulate those bodies (no fall), so the server-owned
map-body result is not a Boblox parsing gap. Native and Boblox agree: on this
place the server grants ownership of the character/root, not streamed map
bodies.

## Cleanup after the evidence

The rejected `MaxSimulationRadius`/`ReplicationFocus`/`SimulationRadius`
client-write experiments (`RBX_SIMRADIUS`, `RBX_FOCUS`, `RBX_FOCUS_SIM`,
`RBX_FOCUS_A1`, and their focus-specific `0x83/03` and A1 writers) have been
removed from `live_runtime.cpp`; they were server-only properties and could only produce
`0x15/0105` on this server. The ordinary empty A1 keepalive and the writer-slot
gated A1 records remain. `BasePart:GetNetworkOwner` now raises the exact native
server-only error instead of returning a fabricated `nil`.

A scripted Boblox check on the same place confirmed both getters:

```
BOBLOX_OWNERAPI getowner_ok=false err=Network Ownership API can only be called from the Server.
BOBLOX_OWNERAPI getauto_ok=false err=Network Ownership API can only be called from the Server.
```

Clean 95 s Boblox run after the cleanup (same place/account):

```
join schema classes=644 members=10411 shared=127 slot_classes=0
0xA1 state records gated: no writer slot table for class 389 (empty keepalive only)
raknet: physics t=60s bodies=3637 ... owner=0x6ac ... dynamic=0 steps=0
raknet: run window complete
```

The session stayed joined for the full window (no rejoin, no `0x15`
disconnect notification, no `0x0104`/`0x0105`). The only local owner is the
character body (`0x6ac` in that run); map bodies remain server/other-client
owned. The synthetic Jolt body still exercises local simulation (`dynamic=1`,
`moved=true`) and the full test suite passes.
