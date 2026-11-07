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

- `0xA1` (the camera/focus state stream, ~1.4/s in the native) is the message the
  RCC acts on: sending the captured body with our ref at offset 40 made a
  **different player's** character move (observed live). At offset 18 nobody else
  moves, but our own body is still server-simulated (visible, pushable, static).
- `Player.MaxSimulationRadius` is in the join schema (id 6065). Writing `9e9`
  is accepted by the server (no `0104`), 40 times in one session, with no
  observable change to ownership.
- `NetworkOwnerV3` assignments do arrive for parts we already hold, but never for
  our own body parts while the body is server-simulated.
- `0x83/0a` claims at the native's rate (14/s) are **rejected**: the RCC answers
  `0x15/0105` (packet stream error) and drops the session. The native sends the
  same rate safely because its claims name parts it already owns.

## Where that leaves it

Two candidate explanations, both consistent with the data:

1. **The focus message is right but partially wrong.** Offset 40 is an identity
   the server attributes the focus to; offset 18 is the target. Getting both
   right may be possible by deriving them from our own session instead of a
   capture.
2. **Ownership is granted only to sessions the server classifies as players**,
   and that classification happens earlier (join/auth), which would explain why
   the *body itself* is sometimes never created for us.

Next experiment that would separate them: send `0xA1` with **our** character ref
at 18 and a ref derived from our own Player/character at 40 (not a capture value),
then check for `NetworkOwnerV3` on our body parts. If those appear, ownership was
granted and movement is a formality.
