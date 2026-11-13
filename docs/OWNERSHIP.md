# Network ownership: how the engine decides, and what we measured

Source of truth: `~/atlas/Functions` (decompiled Roblox Studio 2.743 with symbols).

## The rule (server side)

`NetworkOwnerJob::updatePlayerLocations` runs periodically and builds the map of
"this client is here" that everything else depends on:

```
for each ServerReplicator (one per connected client):
    if (!readPlayerFocusInstantAndSimulationRegion3d(replicator, cell))
        continue;                        // client SKIPPED entirely
    character = Player:getCharacter()
    root      = character ? getConstRootMovingPrimitive(character) : null
    clientLocations[address] = { cell (focus + region), root }
```

`readPlayerFocusInstantAndSimulationRegion3d` itself just reads the player's
**ReplicationFocus CFrame** and a **radius** (`Player::getReplicationFocusCFrame2`,
then stores `v3+216` as the region radius).

Then, per part:

```
NetworkOwnerJob::clientCanSimulate(part, client):
    if (part's root moving primitive == client's character root) return true
    if (assembly is grounded && assembly extents fit the client's region) return true
    ... otherwise compare the client's region centre against the assembly extents
```

`isClientCharacterNetworkOwnershipUnit` is the first clause: a part is yours
iff its assembly root **is** your character's root.

Grant = writing the `NetworkOwnerV3` property on the part and replicating it.
`BasePartProp::setNetworkOwner` / `ServerReplicator::isPropertyInvalidNetworkOwner`
guard the write; `NetworkOwnerJob::switchOwners` performs the change.

## What the client must do

Nothing explicit — there is no "request ownership" message. `SetNetworkOwner` is
server-only, and running it in the real client (`Rum + UwUcutor`, capture of 600
datagrams) produced **no packet at all**: the client only stops simulating
locally, and the server re-evaluates on its own schedule.

What the client *does* need is for the server to have:
1. a **Player** with a **Character** (so `root` is non-null), and
2. a **focus + simulation region** for that client's replicator.

Without (2), `updatePlayerLocations` `continue`s before it ever looks at the
character, so the client owns nothing, forever.

## What we measured on our client

- **Correction (2026-11-07): `0xA1` is not the focus message.** The Atlas
  reader/writer (`RbxInstanceReplicatorManager::receiveISRPacket`,
  `InstanceStateReceiver::readReceivedPacket`, `InstanceStateSender`) shows it
  is the generic **ISR instance-state replication** channel. The 55-byte frames
  in `run/native_packets.txt` are records of the form
  `[a1][u32 tick][u32 records][u32 ack][u32 flags]`, then
  `[ref lo+u32][u16 class][u16 member_count][mask][u16 body_len][values]`.
  Offset 18 is the ISR object id and offset 22 the class id, not an identity
  pair. Sending a captured body upstream injected an ISR property update for
  whoever owned that object id — which explains why a different player moved
  when the captured object happened to be theirs. It was never a focus claim.
- Two pinned native-vs-ours sessions (`~/a1run`, `~/a1run2`) measured the
  direction: the native's outbound A1 frames were only empty 13-byte frames
  (2-3 per run), while our client received 452-498 A1 frames carrying
  Part/MeshPart/Animator/Wedge records. A1 is therefore normal server->client
  state replication here, not the client's ownership/focus carrier.
- `Player.MaxSimulationRadius` is in the join schema (id 6065). Writing `9e9`
  is accepted by the server (no `0104`), 40 times in one session, with no
  observable change to ownership.
- `NetworkOwnerV3` assignments do arrive for parts we already hold, but never for
  our own body parts while the body is server-simulated.
- `0x83/0a` claims at the native's rate (14/s) are **rejected**: the RCC answers
  `0x15/0105` (packet stream error) and drops the session. The native sends the
  same rate safely because its claims name parts it already owns.

## Where that leaves it

The ownership rule still requires a focus + simulation region, but A1 is not
how that state is claimed. The current 189707 schema has the properties to try
directly on **our own Player ref** (`Player` = class 383 in that schema):

- `ReplicationFocus` = prop 6073, member 41, tag 28 (instance ref)
- `SimulationRadius` = prop 6075, member 43, tag 11 (f32)
- `MaxSimulationRadius` = prop 6065, member 33, tag 11 (f32)

**Result (2026-11-07): rejected, all three encodings.** Writing
`Player.ReplicationFocus` to our own character root via ordinary `0x83/03`
tag-28, via A1 ISR with the optional timestamp, and via A1 ISR without the
timestamp all ended the session with `0x15/0105`
(`DisconnectReceivePacketStreamError`). No `NetworkOwnerV3` arrived for our
body refs before the disconnect. The server therefore rejects client-authored
`ReplicationFocus` updates, or none of the three encodings is the accepted
client->server shape; either way, the wire-only focus route is closed. Do
**not** send a captured A1 body either; its object id belongs to another
session.

Separately, the existing A1 body decoder is not exact: it treats the dirty-set
id as a schema member index, so inbound Part records decode as `Anchored` with
leftover tails (8,693 records in the second run). That is a correctness bug in
inbound state application, not evidence about movement.

## 2026-11-13 native result and cleanup

The native client (same account, same 189707 server) confirms that the open
map-ownership result is a server property, not a client parsing gap:

- `BasePart:GetNetworkOwner()` raises `Network Ownership API can only be
  called from the Server.` in the native client; `NetworkOwnerV3` is hidden and
  non-scriptable.
- Native's 92 s capture contains only two `NetworkOwnerV3` items (prop 5662,
  value `01`), with no 1016-style local grant for map bodies.
- Native lifted eight unanchored map parts by 30 studs and none fell during the
  next 1.8 s, so native did not simulate those bodies either.

The client-side `MaxSimulationRadius` / `ReplicationFocus` / `SimulationRadius`
probes and their `0x83/03`/A1 writers have been removed. They wrote properties
the native protocol treats as server-only and were answered with `0x0105`.
`BasePart:GetNetworkOwner` now raises the captured native error rather than
returning a fabricated owner. The connection fix, empty A1 keepalive and
writer-slot gated A1 records are unchanged.
