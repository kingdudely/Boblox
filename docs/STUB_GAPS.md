# Stub-gap audit — 2026-11-13

This is the durable audit behind the current parity pass.  It covers the
subsystems named in the mission: RakNet/NDS networking, Luau, voice chat
(VC), Face Control, renderer, physics, Gui, Audio, `rbxasset://` loading and
the Jolt / NanoVG / stb / FreeType integrations.

## Method

1. Scanned `client/src`, `client/apps` and `rbx_runtime/src` for explicit
   markers (`TODO`, `FIXME`, `stub`, `not implemented`, `unsupported`,
   `no-op`, `approx`, `fallback`, `not yet`, `placeholder`, `fabricat`).
2. Followed each hit into its call path and excluded legitimate fallbacks
   (for example a renderer stub selected only when SDL3 is absent).
3. Compared the surviving gaps with the protocol, native-session and IDA
   evidence already in `FINDINGS.md`, `docs/PARITY.md`,
   `docs/NDS_CONNECTION.md` and `docs/OWNERSHIP.md`.
4. Ranked by impact and reachability.  A gap is **deferred** when the native
   behaviour cannot be established without guessing; the exhausted evidence
   is recorded instead of shipping an invented implementation.

Status key:

- **deferred** — native behaviour is not established, or a measured attempt
  made the client worse.
- **open** — the native behaviour is known, but the implementation was not
  part of this pass.
- **implemented** — completed in this pass, with a unit test.
- **closed / not a gap** — previously listed in an older ledger but already
  implemented; noted so the audit does not repeatedly re-open it.

## Ranked gaps

### 1. `0x83/1c` upstream client-instance replication — deferred

- **Location:** `client/src/solve/repl_stream.cpp:1499`
  (`walk83ItemsEx`) has no `0x1c` receive/chain case; there is no upstream
  builder for it in `client/src/send/`.
- **Native behaviour:** the native client sends
  `83 1c 00 <i32> 06 00 <items...> 00` for client-owned instances.  The
  observed bodies carry ordinary chain items and include PlayerGui,
  LocalizationTable and GUI text; the ids are session-local, not the RCC's
  world ids.
- **Evidence:** `FINDINGS.md:4221-4253` (framing, body contents, writer
  anchors), `FINDINGS.md:4470-4474` (still-open candidates), and
  `docs/PARITY.md:43-45`.
- **Why deferred:** reproducing this requires a client-owned instance
  registry, an upstream change queue and a way to allocate ids that the RCC
  accepts.  Emitting a capture's ids or inventing ids was explicitly rejected
  as fabricated behaviour.  The IDA writer search already failed to isolate a
  per-type serializer (`FINDINGS.md:4196-4219`).

### 2. `0x83/0e` upstream client-owned replication — deferred

- **Location:** same receive walker as above; no `0x0e` case or sender.
- **Native behaviour:** `83 0e 00 <items...> 00`; bodies carry events,
  `0x0a` id pairs, clock ticks and timing records from client-authored
  instances.  This is the non-GUI counterpart of gap 1, not an ack bag.
- **Evidence:** `FINDINGS.md:4221-4253`; the earlier ack-bag reading is
  corrected there; `docs/PARITY.md:43-45`.
- **Why deferred:** same missing local-registry contract as gap 1.  A
  wrapped ack frame would be a behavioural guess and is not shipped.

### 3. Voice chat / microphone — deferred

- **Location:** `client/src/common/features.h:201-203` rejects `mic`;
  `client/apps/raknet_main.cpp:300-302` prints the same rejection.  There is
  no capture/encode/RTP path in `client/src/audio/`.
- **Native behaviour:** the native client has an `rbx-rtcio` WebRTC voice
  region and marks RTP audio/video with a WebRTC DSCP field.  Game traffic is
  not carried on that path.
- **Evidence:** `FINDINGS.md:523` (ALPN `rbx-rtcio`),
  `FINDINGS.md:1551-1554` (WebRTC voice region / RTP), and the feature gate
  above.
- **Why deferred:** no native voice-service capture, codec negotiation,
  microphone permission flow or RTP framing has been obtained.  Implementing
  a codec or signalling shape from memory would be fabricated.

### 4. Face Control / facial tracking — deferred

- **Location:** `rbx_runtime/src/instance/instance.cpp:356-365` clamps the
  `FaceControls` numeric properties to `0..1`; that is the complete surface.
  `FaceControls` exists in the generated class table
  (`rbx_runtime/src/instance/classes/generated.cpp:5026`), but no tracking
  source writes it.
- **Native behaviour:** FaceControls exposes FACS values driven by camera /
  face tracking on supported clients; property writes are clamped, but the
  native client also produces tracking updates.
- **Evidence:** the property clamp and generated class are local; there is no
  face-tracking capture in `FINDINGS.md`, no camera frame stream in the
  protocol notes, and no `rbxasset://` tracking model in `~/rr/assets`.
- **Why deferred:** no native tracking stream or asset mapping was found.
  Writing synthetic FACS values would be a guess.

### 5. Driven `0xA6` ControlState movement — implemented

- **Location:** `client/src/raknet/input.{h,cpp}` owns the 141-byte serializer;
  `client/src/engine/local_player.{h,cpp}` exposes the four 240 Hz control
  samples; `client/src/engine/live_runtime.{h,cpp}` owns the local
  `PlayerControlState`; `client/apps/raknet_main.cpp` selects the long or
  short frame and pumps it at the native cadence.
- **Before:** the default was the 37-byte steady route frame with
  `nActions = 0`.  The 141-byte serializer existed behind `RBX_A6_LONG`, and
  its four actions repeated one state sample; the local Jolt avatar's real
  move vector was not connected to the state.
- **Native behaviour:** the same-job capture contains 37,726 of 48,649
  `0xA6` frames in the 141-byte form: header `a6 04 01 00`, the 14-byte route
  field, the two step u32s, flag `0c`, and four 29-byte input frames.  Each
  input frame carries two actions (`7f` and `11`) with the same live u24 ref,
  a `04 00` type pair, three little-endian int16s (the quantised UnitVector3
  direction), and the constant tail `43`.  The four actions are successive
  240 Hz samples; the route runs at the measured 60 Hz and both step slots
  advance by four.  The short 34/37-byte route form is the pre-character
  fallback, not the steady state.
- **Implementation:** the live client now creates the local
  `PlayerControlState` for its own Player, feeds it from the Jolt
  `CharacterController` move vector while the keyboard/controller has input,
  and falls back to the executor's `boblox.move` value when it does not.  The
  local avatar records one sample after each 240 Hz `controller_.step`; the
  `0xA6` pump drains those four samples and sends the long form by default.
  `RBX_A6_SHORT=1` restores the 37-byte form for A/B runs; the short form is
  still used when the local Player has no ref or no UnitVector3 state.
- **Evidence:** live Boblox 1818 and 189707 runs with
  `tests/live/a6_move_probe.lua` set `boblox.move(0, 0, -1)`, printed
  `FIELDS_AFTER MoveDirection=0, 0, -1`, emitted `2,278` / `2,884` 141-byte
  frames, and every decoded action pair read `(0, 0, -32767)`; neither run
  received a `0x15`.  The native 189707 probe reaches a Character and
  `Humanoid:Move` completes; the native build does not expose
  `Player:GetControlState` to scripts, so the wire capture above is the
  direct native evidence for the frame contents.
- **Verification:** `unit_engine` pins the local Player ref, the
  `PlayerControlState` UnitVector3 update/clear path, and the 141-byte
  serializer is already byte-exact against the native capture in
  `tests/unit/unit_send.cpp`.  The live artifact directories are
  `/tmp/boblox_a6_tx3` (1818) and `/tmp/boblox_a6_tx4` (189707).

### 6. `0xA1` attribute replication and null-ref boundaries — implemented

- **Location:** `client/src/solve/repl_attributes.{h,cpp}` (the attribute
  codec), `client/src/solve/repl_pv.{h,cpp}` (tag 57),
  `client/src/solve/repl_stream.cpp` (mask pairing and boundary walk),
  `client/src/solve/joindata.{h,cpp}` (apply/defer and `AttributeChanged`
  queue), and `client/src/engine/live_runtime.cpp` (persistent per-entity
  schema and signal drain).
- **Native behaviour:** a record's mask has a writer-slot region followed by
  paired attribute entries.  Entry `named + 2*i` is a schema entry
  `00 <tag> [u32 enum-name length][enum name when tag is 7]
  <u32 name length> <name>`; entry `named + 2*i + 1` is that attribute's
  value in the ordinary tagged-value grammar.  Schema and value can arrive in
  separate records, so the schema is retained per entity.  A null ref has a
  one-byte ref instead of the five-byte ref form, making its head four bytes
  shorter; the boundary is accepted only at the exact end of the previous
  body and only when the candidate body decodes.  Tag 57 is
  `BasePart.ReplicationPV`: `sub_6427536` reads a flags byte, a bit-packed
  position/linear-velocity/angular-velocity set and the orientation selector
  trailer.  The value is Hidden, NotScriptable and RobloxSecurity, so it stays
  in `ApplyReport::replication_pv` and is never installed as a Luau property;
  that is native parity, not a missing accessor.
- **Evidence:** IDA `sub_6431154` (`InstanceStateSerialization`
  deserializer, lines ~543–636) establishes the paired-entry layout and the
  separate schema/value branches; `sub_6432F94` case `0x39` calls
  `sub_6427536`/`sub_5E7C3C8` for the parsed PV.  The tag-57 samples in
  `unit_replicate` are live 1818 wire values and pin every field, width,
  dequantized component, orientation selector and exact extent.  On the 1818
  corpus, 2,000/2,000 frames close (`27,666` records, `4,447` null-ref
  records, `0` frame failures, `0` unread tails); `tag 57` no longer appears
  in any unknown-tag bucket.  Attribute values observed: string `2` ×415,
  bool `9` ×1, Vector2 `21` ×13, Vector3 `22` ×23.  Native and Boblox both
  report the same 11 stable `RBX*` attributes on the instances they share
  (native had five more on accessory instances Boblox had not spawned).  The
  synthetic unit test covers a normal record, a marker-prefixed null record,
  an adjacent null record and the enum-schema extra name; the live 1818
  corpus exercises 4,447 null boundaries.
- **Residual:** 213 of the 27,666 values in the 1818 capture are attribute
  values whose schema was established before the capture window; they are
  surfaced as partial (never guessed) because the name/type is not on the
  wire.  The 189707 capture is the client-authority `0xA1` variant (header
  flags `0x44000001`).  Its 0x97 tail uses the sparse writer-slot form:
  `[u32 entry_count]` followed by only the 18 classes that have slots, then
  the same 8-byte trailer as the dense 1818 form.  With that tail loaded,
  class 366 resolves slot 2 to `Part.ReplicationPV`, and all 333 frames and
  5,508 records now close: 5,468 tag-57 values decode with zero tail bytes.
  The u16 after the mask is the native stream *end*, not a cap on a value:
  `sub_6431154` records `stream_pos + u16` and restores it after the named
  loop, and the serializer patches the same span (`v125 - v143 - 2`).  A
  value may therefore consume up to three bytes past the end (the next
  record's guard, or the native zeroed stream guard at a frame end); 2,755
  bodies instead end past their value with 11,516 total skip bytes (max 8),
  which native discards when it seeks to the recorded end.  A null-ref
  candidate with a zero-length body is rejected
  as ambiguous; no such record appeared in the live corpora.  Unsupported
  attribute value tags are counted rather than coerced.
- **Verification:** `unit_replicate` has the paired-schema/value cases, the
  enum-name case, the tag-57 vectors, the body-end guard/padding case and the
  null-boundary case (80 cases, 1,248 assertions); the full 27-test `ctest`
  suite passes, including `unit_gui` text metrics.

### 7. Rigid-joint assembly queries — implemented

- **Location:** `rbx_runtime/src/instance/assembly.h` and
  `rbx_runtime/src/instance/assembly.cpp` (the one shared graph and root
  model), called by `rbx_runtime/src/instance/methods.cpp:743-766` for
  `GetConnectedParts`, `GetJoints` and `GetRootPart`.  The native `Enabled`
  defaults are restored in `rbx_runtime/src/instance/classes.cpp:52-56`.
- **Native behaviour:** `BasePart:GetConnectedParts(recursive)` returns the
  receiver **and** every part joined by a rigid joint (`Weld`, `Snap`,
  `ManualWeld`, `Motor`, `Motor6D`, `WeldConstraint`), optionally
  transitively — including an orphan/out-of-World part, which returns only
  itself.  `GetJoints()` returns all joint/constraint instances attached to
  the part.  Joints join parts inside a `Workspace`/`WorldModel` (and the
  legacy `JointsService`); overlap alone never joins two parts, and
  `Enabled = false` removes a joint from the assembly.  `GetJoints` still
  reports the disabled joint itself.  The receiver was missing from Boblox's
  earlier result; the native `native_root_edges_probe.lua` run showed
  `connected=[RootEdge_Orphan]` for an orphan and the middle of a two-anchor
  split returning both anchors and itself.
- **Evidence:** Roblox API documentation for
  [GetConnectedParts](https://create.roblox.com/docs/reference/engine/classes/BasePart/GetConnectedParts)
  and [GetJoints](https://create.roblox.com/docs/reference/engine/classes/BasePart/GetJoints),
  the assembly rules at
  <https://create.roblox.com/docs/en-us/physics/assemblies>, and the native
  probes under `tests/live/native_root_*.lua` (1818, 2026-11-13).  The old
  code used axis-aligned overlap; the native probe corrected both that and
  the self-exclusion.
- **Implementation:** direct neighbours come from enabled rigid joints in the
  nearest `Workspace`/`WorldModel`; recursive mode is a graph walk with
  deduplication and includes the receiver.  `GetJoints` reports all
  `JointInstance` and `WeldConstraint` instances attached to the part,
  including disabled joints and joints in an inactive model.  The same
  traversal feeds gap 8, so Lua and the property getter cannot drift.
- **Proof:** `tests/unit/unit_engine.cpp:3350-3495` builds two joined parts
  plus a third part that only overlaps the first and a disabled weld, then
  checks direct recursion, `GetJoints`, the enabled defaults, and that the
  overlap-only part is excluded.  Live 189707 proof
  (`/tmp/joint_probe_live_self.log`, 2026-11-13) printed:
  `direct=2 [JointProbeA,JointProbeB] recursive=3
  [JointProbeA,JointProbeB,JointProbeC] joints=2
  [JointProbeDisabled,JointProbeWeld] bJoints=2 overlapDirect=false
  overlapRecursive=false defaults=true/true disabledIgnored=true`.
- **Remaining boundary:** `GetRootPart`/`AssemblyRootPart` is gap 8.  Contact
  queries and touch signals are implemented in gap 9.

### 8. Assembly roots and `JointInstance.Active` — implemented

- **Location:** `rbx_runtime/src/instance/assembly.cpp` (`resolve_assembly`,
  `root_part`, `joint_active`), shared by
  `rbx_runtime/src/instance/methods.cpp:761` and the dynamic
  `AssemblyRootPart` getter at `rbx_runtime/src/instance/instance.cpp:191`
  plus the dynamic `Active` getter at `instance.cpp:197`.
  `Instance` now carries a monotonic `creation_seq`
  (`instance.h:87`) for the native local-instance tie order.
- **Native behaviour, established from IDA and native probes:**
  1. An anchored query returns itself.  With multiple anchored members the
     solver splits the assembly; each anchored part is its own root and the
     unanchored side follows the winning anchored component.
  2. Between anchored candidates the same root ranking decides the winner:
     non-massless before massless, then higher `RootPriority`, then the
     largest-face score.  A native run with a small anchor welded via the
     newer link still returned the larger anchor, so rank beats the active
     link; a small anchor with `RootPriority = 100` still beat the larger
     plain anchor (`P5-middle-priority-small`).
  3. With no anchor: non-massless before massless, then higher
     `RootPriority`, then the native score
     `floor(largest_face_area * 50.0) * multiplier`; multipliers are 20x for
     `Seat`/`VehicleSeat`, 10x for the exact name `HumanoidRootPart`, and 5x
     for the exact name `Torso`.
  4. Equal rank anchors with a direct unanchored middle follow the newest
     active anchor link; reversing weld creation order reverses the winner
     (native `native_root_anchor_ties_probe.lua` T1-T4).  A longer chain with
     no direct anchor link follows the newest anchor-incident link as well
     (`native_root_long_chain_probe.lua` G1-G3).  Equal unanchored parts use
     one shared creation sequence across replicated and client-created
     instances; the replicated debug id is not itself the key.  This is the order used by
     both replicated and local candidates in the mixed tie, where the later
     local part won over the replicated candidates (native 1818 and Boblox
     189707 `MIXED mixed ... isL2=true`).
  5. `Model.PrimaryPart` is ignored for assembly root selection.
     `Humanoid.RootPart` is not a separate override; a character root is the
     part named exactly `HumanoidRootPart` through the 10x multiplier.
  6. Orphan/out-of-World parts return `nil` from both `GetRootPart` and
     `AssemblyRootPart`; `GetConnectedParts` still returns the receiver.
  7. `JointInstance.Active` is a read-only, non-replicated solver view.  It
     is true exactly for the selected spanning-forest links: the winning
     anchor link in a split, the newest link in an unanchored mutual weld,
     the only link in a humanoid/massless assembly, and false for a disabled,
     outside-World, or fully-anchored-to-anchored link.  A script write raises
     `Unable to assign property Active. Property is read only`; changing
     `Enabled` re-derives the flags immediately.
- **Evidence:** IDA on `~/rr/libroblox.so`: `sub_21B5157` registers the
  surface; `sub_5DEC30E` (`GetRootPart`) reads assembly-primitive `+520`;
  `sub_5DEC398`/`sub_5D49948` (`AssemblyRootPart`) reads the same field;
  `sub_5DEB69C` is `GetConnectedParts`; `sub_5D4AEA8` reads the signed-byte
  `RootPriority` at `[part+200]+620`.  Native probes (1818):
  `native_root_probe.lua`, `native_root_ties_probe.lua`,
  `native_root_detail_probe.lua`, `native_root_edges_probe.lua`,
  `native_root_multipliers_probe.lua`, `native_root_reverse_probe.lua`,
  `native_root_anchor_ties_probe.lua` and
  `native_root_anchor_precedence_probe.lua`,
  `native_root_long_chain_probe.lua`.  The public
  [Assemblies](https://create.roblox.com/docs/en-us/physics/assemblies) and
  the DevForum `Massless`/`RootPriority` announcement supply the score and
  multiplier definitions.  Native output examples: `T2-middle`
  `root=RootATie2_A activeA=true activeB=false`; `T1-middle`
  `root=RootATie1_B activeA=false activeB=true`; `P1-middle-small-active`
  `root=RootAP_LargeB active1=false active2=true`; `ROOTCHAIN G2-M2`
  `root=RootChain2_A` with the B link inactive.
  `native_joint_active_probe.lua` (1818) printed
  `ACTIVE anchoredSplit root=ActiveSplitA ActiveSplitWA=true
  ActiveSplitWB=false`, `ACTIVE humanoid ... ActiveHumanoidWeld=true`,
  `ACTIVE mutual ... ActiveMutual12=false ActiveMutual21=true`, `ACTIVE
  massless ... ActiveMasslessWeld=true`, `ACTIVE fullyAnchored ...
  ActiveAnchoredWeld=false`, `ACTIVE disabled ...
  ActiveDisabledWeld=false`, `ACTIVE weldConstraint ... ActiveWC=true`,
  `ACTIVE outside ... ActiveOutsideWeld=false`, and the exact read-only
  write error.  `native_mixed_tie_probe.lua` (1818) printed replicated
  debug ids (`1_...`), local debug ids (`0_...`), and
  `MIXED mixed root=MixedTieLocalB ... isL2=true`; the replicated-only tie
  picked the higher debug id while the mixed tie picked the later local
  creation.  The final 1818 run selected `Pillar` debug ids `1_1838473` and
  `1_1838460`; after a two-second settle, the mixed root was
  `MixedTieLocalB` (`0_61612`).  `native_mixed_tie_discovery_probe.lua`
  established the debug-id namespaces used by the probe.
  `native_root_long_chain_probe.lua` supplied the two-anchor chain evidence:
  G1 `RootChain1_J1=false J2=true J3=true J4=true` with root B, G2 the
  reversed winner with root A, and G3 only the newest anchor link active.
- **Implementation:** one module builds a self-inclusive graph from enabled
  rigid joints, handles cycles with a visited set, and feeds the query and
  property surface.  `resolve_assembly` builds the unanchored components,
  chooses one winning anchor link per component by anchor rank then newest
  joint, and builds a newest-first spanning forest inside that component.
  Anchored parts stay separate roots; every other anchor link and every
  cycle-closing link is inactive.  Both
  `AssemblyRootPart` and `JointInstance.Active` consume that same forest;
  root ranking is one comparator (massless, clamped signed-byte
  `RootPriority`, exact score/multiplier, creation order).  The `Active`
  getter is dynamic and the generated read-only default is never read.
- **Proof:** `tests/unit/unit_engine.cpp:3497` covers isolated, welded chain,
  anchored split, equal-anchor/newer-link ties, rank-over-link, anchor
  priority, long-chain anchor ties, largest part, RootPriority, massless,
  humanoid, mutual welds, ignored `PrimaryPart`, Seat and `Torso`
  multipliers, and orphan/outside `nil`.  The live 189707 probe
  (`tests/live/root_probe.lua`, `/tmp/root_probe_live5.log`) printed
  `isolated=RootIsolated chain=RootChainAnchor larger=RootLargest
  priority=RootPriority massless=RootSolid humanoid=HumanoidRootPart
  humanoidProperty=true mutual=RootMutualTwo primary=RootPrimaryLarge
  anchoredSplit=true anchorTie=RootTieAnchorA
  anchorSize=RootSizeAnchorLarge longChain=RootLongAnchorA orphan=nil
  connectedSelf=true`, plus one line per characteristic part.  The native
  1818 runs printed the same ordering, including `ROOTATIE T2-middle ...
  root=RootATie2_A`, `ROOTAPREC P1-middle-small-active ...
  root=RootAP_LargeB`, and `ROOTCHAIN G2-M2 root=RootChain2_A`.
  `tests/unit/unit_engine.cpp:3707` covers `Active` for anchored splits,
  humanoid, mutual welds, massless, fully anchored, disabled, outside,
  `WeldConstraint`, the exact read-only write error and `Enabled` flipping.
  `tests/unit/unit_engine.cpp:3853` covers the legacy/non-rigid set:
  `Glue`/`ManualGlue` roots and connections, anchored split, duplicate and
  mixed `Weld`/`Glue` pairs, the three-part cycle, detach/reattach, and
  `Rotate`/`VelocityMotor` non-connection.  `:4390` covers mixed
  replicated/local creation ordering with intentionally huge wire ids.  The
  extended 189707 run (`tests/live/joint_active_probe.lua`,
  `/tmp/boblox_active_hinge6.log`) printed the same 34 `ACTIVE` lines as the
  native 1818 run (`tests/live/native_joint_active_probe.lua`,
  `/tmp/native_active_hinge5.log`), including `glue`, `glueAnchored`,
  `glueCycle`, `manualGlue`, `nonRigid`, `duplicateGlue`, `mixedPair`, the
  three `detach` lines, and all hinge lines.  The mixed local-B root matched the native 1818 run
  (`/tmp/native_mixed_tie_final3.log`).  The Boblox chain probe
  (`tests/live/root_chain_probe.lua`, `/tmp/root_chain_boblox.log`) printed
  the same G1/G2/G3 roots and Active flags as the native chain log.
- **Hinge:** native has no `Hinge` instance class.  `tools/api_dump.json`
  contains only `SurfaceType.Hinge` and `HingeConstraint`; the analysed
  library has no `RBX::Hinge` RTTI or class descriptor, and
  `Instance.new("Hinge")` raises native's exact
  `Unable to create an Instance of type "Hinge"`.  The saved-instance path
  is equally strict: `InsertService:LoadLocalAsset` on an XML model whose
  class name is `Hinge` loads the sibling `Part` and silently drops the
  `Hinge` record, with no error or substitute instance (native probe
  `tests/live/native_legacy_hinge_load_probe.lua` against
  `rbxasset://content/legacy_hinge.rbxmx`).  Boblox matches both paths: the
  registry contains no `Hinge` class, `Instance.new` uses the same message,
  and `Joindata.applyJoinSnapshot` skips an unknown class by name while
  keeping its known siblings.  `SurfaceType.Hinge` is a surface value, not
  an instance class: `BasePart.MakeJoints` evaluates each contact pair, not
  just the receiver.  A lower part with `TopSurface=Hinge` creates a
  `Rotate` parented to that part, `Part0` the lower part, `Part1` the upper
  part, `Enabled=true`, `Active=false`; its local `C0` rotation is
  `[-1,0,0; 0,0,1; 0,1,0]` at `(0,+size.y/2,0)`.  A lower part whose top is
  non-special (Smooth, Weld, Glue, any other surface) still creates the
  edge when the upper part's `BottomSurface=Hinge`; the Hinge part owns the
  `Rotate` and its `C0` is `[1,0,0; 0,0,-1; 0,1,0]` at
  `(0,-size.y/2,0)`.  `C1` carries the winning face's constant rotation and
  that face-centre world point expressed in the other part's local frame,
  not the overlap centroid.  The six constant face frames are Top
  `(0,+hy,0)` / `[-1,0,0;0,0,1;0,1,0]`, Bottom `(0,-hy,0)` /
  `[1,0,0;0,0,-1;0,1,0]`, Right `(+hx,0,0)` / `[0,0,1;0,1,0;-1,0,0]`,
  Left `(-hx,0,0)` / `[0,0,-1;0,1,0;1,0,0]`, Front `(0,0,-hz)` /
  `[-1,0,0;0,1,0;0,0,-1]`, and Back `(0,0,+hz)` identity.  A lower Top
  Hinge wins over an upper Bottom
  Motor; a lower Top Motor or SteppingMotor wins over an upper Bottom
  Hinge, creating `RotateV` or `RotateP` with the same frames and owner
  (native dynamic probe).  When both contacting faces are dynamic, the
  caller's own face wins: the same lower Hinge/upper Motor pair yields
  `Rotate` when the lower calls and `RotateV` when the upper calls.
  Rigid precedence is Weld > Glue > Snap: any contacting Weld face creates
  `Weld`, otherwise any Glue creates `Glue`, otherwise a Snap-compatible
  pair creates `Snap`.  `Weld`/`Glue` need only one joinable face; Snap
  requires both faces in `{Studs, Inlet, Universal}`.  The mating pairs are
  `Studs+Inlet`, `Studs+Universal`, `Inlet+Universal` and
  `Universal+Universal`; equal `Studs+Studs` and `Inlet+Inlet` do not Snap.
  Rigid joints are caller-owned (`Parent`/`Part0` = caller) regardless of
  which face supplied the surface.  `Smooth` and `SmoothNoOutlines` never
  join by themselves and are not Snap faces.  All six faces participate,
  including Right/Left/Front/Back; the 100-cell side matrix matches native.
  Repeating `MakeJoints` is idempotent while the same edge exists.
  Interpenetrating boxes create no joint (native overlap probe); the
  separating-axis test rejects a volume overlap deeper than the contact
  slop and rejects coplanar faces that meet only along an edge (zero
  in-plane contact area).  `BasePart:BreakJoints` removes the rigid
  auto/manual `Weld`/`Glue`/`ManualGlue` forest and leaves the `Rotate`
  untouched; destroying the `Rotate` and calling `MakeJoints` again creates
  a new one with the same frames, and detaching/reattaching the parts
  removes/restores the edge.  `Model:MakeJoints` walks the model subtree in
  live child order (nested models before later siblings) and runs the
  BasePart pass for every part; each per-part pass still sees the whole
  Workspace as contact geometry (native model-scope probe).
  `Model:BreakJoints` destroys every legacy `JointInstance` and
  `WeldConstraint` in the subtree, including `Rotate` (native model probe).
  `Workspace:MakeJoints` and `Workspace:BreakJoints` require a table of
  parts: no argument raises `Argument 1 missing or nil`, nil/userdata raise
  `Expected table argument, got nil`/`userdata`; the table form runs the
  same per-part passes and `BreakJoints` still leaves the Hinge `Rotate`;
  the native and Boblox runs of `native_workspace_joint_probe.lua` print the
  same seven `WS` lines.  Live matrix coverage: `native_surface_matrix_all_probe.lua`
  prints the 100 top/bottom pairs for lower-first and upper-first callers,
  `native_surface_order_all_probe.lua` prints 200 caller-order rows,
  `native_surface_side_probe.lua` prints all 100 Right/Left pairs,
  `native_surface_face_frames_probe.lua` prints the six face frames (15
  rows), `native_surface_break_all_probe.lua` prints all 47 break rows,
  `native_model_surface_matrix_probe.lua` prints the 100-cell model matrix,
  and `native_model_scope_probe.lua` covers outside/nested scope.  All
  compared rows are line-identical against live native; the native runner
  suppresses the first descriptor of a dense burst, so the 200-cell matrix
  baseline is the union of two spaced native runs, with zero row diffs.
  The lower-case `makeJoints`/`breakJoints` aliases
  are real methods, not stubs; the generated `TriangleMeshPart` copies are
  attached too so `MeshPart` descendants do not shadow `BasePart`.
  `Rotate`, `RotateV` and `RotateP` share the existing non-rigid `assembly`
  path: `Active=false`, one-part `GetConnectedParts` and separate self
  roots in their pair topology.  Unit coverage:
  `SurfaceType.MakeJoints and BreakJoints cover every joint family`,
  `SurfaceType.Hinge MakeJoints builds native Rotate edges` and
  `legacy Hinge records are dropped like native InsertService` in
  `tests/unit/unit_engine.cpp`; the live probe
  `tests/live/joint_active_probe.lua` and its native pair
  `tests/live/native_joint_active_probe.lua` print the same 34 `ACTIVE`
  lines, including `hingeSurface`, `hingeRotZ90`, `hingeRotX90`,
  `hingeRotZ180`, `hingeBottom`, `hingeOther`, `hingeMotor`, `hingeStep`,
  `hingeBreak`, `hingeRecreate`, `hingeDetach`, `hingeReattach` and
  `hingeClass`.  The rotated cases come from
  `tests/live/native_surface_rotation_probe.lua`, which established that
  native still creates the Rotate when the caller's TopSurface normal is
  not world-up; the face rectangle/other-OBB separating-axis test in
  `surface_joints.cpp` preserves that behaviour.  Ownership and dynamic
  precedence come from `native_surface_ownership_probe.lua`,
  `native_surface_dynamic_probe.lua`, `native_surface_scope_probe.lua`
  and `native_workspace_joint_probe.lua`.
  `tests/live/native_model_joint_probe.lua` first exposed the
  Model/BasePart split; the complete model matrix and scope probes above
  supersede its Hinge-only interpenetration case.
- **Native `Active` path:** reflection registers `JointInstance.Active` as a
  read-only getter (`sub_5D7B58C`) that dispatches virtual slot `+680` on the
  internal joint object.  The base implementation (`sub_5E0EF08`) delegates
  to `RBX::Joint`'s getter (`sub_4208286`, byte `+0xDD`); `Rotate` overrides
  it (`sub_5E10488`, `[joint+8] >= 11`) and stays false in every captured
  topology.  The flag is therefore the solver's assembly-selection flag, not
  a separate hinge rule.
- **Legacy surface joints:** `Glue` and `ManualGlue` are rigid natively.
  Native 1818 probes print `root=<newer part> conn=2 Active=true`, anchored
  endpoints inactive, the newer edge active in duplicate and mixed
  `Weld`+`Glue` pairs, and the oldest link inactive in a three-part cycle.
  Detaching `Part1` prints `Active=false` with a one-part assembly;
  reattaching it prints `Active=true` and roots the assembly at the new part.
  The implementation now includes `Glue`/`ManualGlue` in the rigid set;
  `Rotate`/`VelocityMotor` remain non-rigid with constant false, matching
  native.  No `Hinge` class is invented or special-cased.

### 9. Contact queries and touch signals — implemented

- **Location:** the solver-neutral bridge is
  `rbx_runtime/src/instance/contact_host.{h,cpp}`; `m_part_get_touching_parts`
  and `m_part_is_grounded` in `rbx_runtime/src/instance/methods.cpp` use its
  backend, with the old AABB scan retained only for the engine-only test
  profile that has no solver.  `client/src/physics/physics.cpp` installs a
  Jolt `ContactListener` and `BodyActivationListener` (`ContactRouter`),
  answers queries through a narrow-phase `CollideShape` pass, and reads the
  character capsule through `CharacterVirtual::GetActiveContacts`.
  `client/src/engine/local_player.{h,cpp}` implements the backend and
  `client/src/engine/live_runtime.cpp` drains the transitions and fires the
  native `Touched`/`TouchEnded` signals on both instances.
- **Native behaviour:** `GetTouchingParts` lists parts whose collision
  geometry overlaps the receiver; `IsGrounded` is true for an upward-facing
  contact.  Static/static pairs answer queries but produce no `Touched`
  events (the native solver only reports solver contacts).  Dynamic contacts
  emit `Touched` on both sides and `TouchEnded` when they separate or a part
  is destroyed.  A destroyed peer does not receive its own `TouchEnded`.
- **Sleep handling:** `ContactRouter` tracks body deactivation and keeps
  sleeping contacts dormant instead of synthesising `TouchEnded`; a removed
  body flushes its active/dormant pairs while both instance mappings still
  exist.
- **Workspace defaults prerequisite:** the native 189707 probe reports
  `gravity=196.1999969482422 authority=Enum.AuthorityMode.Automatic` before
  any replicated `Gravity`/`AuthorityMode` write.  The hand-owned Workspace
  class now starts at `196.2`/`Automatic`; the old API-dump zero defaults
  left a client-authority place with no gravity.  `unit_engine` pins both
  defaults.
- **Proof:** `tests/live/native_contact_probe.lua` was run against native
  189707 and Boblox 189707 on 2026-11-13.  Native printed:
  `CONTACT anchored touchA=1 touchB=1 groundA=true events=5/5/4/4
  delta=4/4 bpos=51.9920768737793` and
  `CONTACT destroyed touchA=0 events=5/5/5/4 deltaEnd=1/0
  bDestroyed=true playerHRP=true playerGrounded=false`.  Boblox printed:
  `CONTACT anchored touchA=1 touchB=1 groundA=true events=5/5/4/4
  delta=5/5 bpos=51.99999237060547` and
  `CONTACT destroyed touchA=0 events=5/5/5/4 deltaEnd=1/0
  bDestroyed=true playerHRP=true playerGrounded=false`.  The query,
  grounding, total event counts, destroy transition and both sides' missing
  `TouchEnded` on the destroyed peer are identical; only the per-settle
  interval and floating-point resting height differ.  Native 1818
  (`authority=Enum.AuthorityMode.Server`) reports the same query result
  (`touchA=1 touchB=1 groundA=true`) and destroy result; Boblox 1818
  reproduces both.
- **1818 fallback-avatar grounding residual:** before this fix the local
  fallback avatar was a locally simulated capsule, so it reported
  `HumanoidRootPart:IsGrounded()=true` while the native replicated avatar
  reported `false`.  Native 1818 printed
  `CONTACT destroyed touchA=0 events=6/6/6/5 deltaEnd=1/0 bDestroyed=true
  playerHRP=true playerGrounded=false hrpTouch=2 hrpTouchNames=Torso,Head
  hrpVelY=0.04248046875 floor=Enum.Material.Plastic`; the old Boblox line was
  `... playerGrounded=true hrpTouch=1 hrpY=21.2624883859321 ...`.  The
  fallback root is now gated on `Workspace.AuthorityMode == Automatic` in
  `LocalPlayer::grounded`; a fresh Boblox 1818 run printed
  `CONTACT destroyed touchA=0 events=5/5/5/4 deltaEnd=1/0 bDestroyed=true
  playerHRP=true playerGrounded=false hrpTouch=1
  hrpTouchNames=SpawnLocation hrpY=21.26128019796647
  hrpVelY=0.0732421875 hrpAnchored=false floor=Enum.Material.Plastic`.
  The remaining touch-set/height difference is the independently replicated
  fallback-vs-server avatar stream, not a grounding claim.
- **Tests:** `unit_physics` pins narrow-phase query/grounding, a falling
  body's began contact, the character capsule's floor contact, and script
  CFrame teleports of dynamic bodies.  `unit_engine` pins backend-driven
  `GetTouchingParts`/`IsGrounded`, both-side `Touched`/`TouchEnded` dispatch,
  and the Workspace defaults.  `unit_physics` also pins that a
  server-authority fallback root never claims capsule grounding while the
  local capsule itself is standing (`local.character().grounded` true).

### 10. Renderer `CornerWedgePart` silhouette — implemented

- **Location:** `client/src/view/view_sdl.cpp` now uploads a dedicated
  `cornerWedgeMesh()` for `Enum.PartType.CornerWedge`; the Jolt adapter's
  CornerWedge hull and mass use the same native primitive.
- **Native behaviour:** `CornerWedge` is a five-vertex square pyramid, not a
  second Wedge: the four bottom corners plus the apex at `(+X, +Y, -Z)`.
  Native `Mesh::makeCornerWedge` (`ROBLOX2016 App/v8world/Mesh.cpp`) lists
  vertices `(+x,-y,+z)`, `(+x,-y,-z)`, `(-x,-y,-z)`, `(-x,-y,+z)`,
  `(+x,+y,-z)` and faces `(0,1,4)`, `(0,4,3)`, `(2,3,4)`, `(1,0,3,2)`,
  `(1,2,4)`.  Renderer normals now follow those five faces; the previously
  shared wedge mesh produced a visibly different silhouette and a degenerate
  bottom triangulation.  The first headful capture also exposed a hole in a
  quad: four-vertex faces were triangulated as `(a,b,c)+(b,c,d)`, whose
  crossing diagonals miss a triangle.  All four-vertex native faces now go through
  `MeshData::quad`, which emits `(a,b,c)+(a,c,d)`; both primitives render
  closed.
- **Physics and mass:** `client/src/physics/physics.cpp` uses the same five
  points for the Jolt convex hull, and `BasePart:GetMass` uses the pyramid
  volume `x*y*z/3`.  The native 1818 mass probe
  (`tests/live/native_corner_wedge_mass_probe.lua`) reports for a 4x1x2
  part: Ball `0.366519`, Block `5.6`, Cylinder `2.199115`, Wedge `2.8`,
  CornerWedge `1.866667`; `unit_engine` asserts those five values.
- **Evidence and test:** the source of truth is the untouched ROBLOX2016
  `App/v8world/Mesh.cpp` implementation above (not a vertex guess), plus
  `App/include/v8world/CornerWedgeMesh.h` and the Bullet hull in
  `App/v8world/BulletGeometryPoolObjects.cpp`.  `client/tools/mesh_preview.cpp`
  renders the five primitive meshes through the real `runViewer` path without
  a live world; `screenshots/mesh_preview.png` shows the corner-wedge (pink)
  and Wedge (yellow) as closed solids beside Block (blue), Cylinder (green)
  and Ball (red).  A temporary wireframe inspection confirmed the corner's
  five vertices and four apex edges; from the default camera its apex projects
  onto a base corner, so the solid reads as a compact diamond.  This closes
  the previous pass's screenshot-verification hole.

### 11. MeshPart / texture rendering — implemented

- **Location:** `client/src/view/mesh_asset.{h,cpp}` parses the native
  `version 2.00`, `5.00`, `6.00` and `7.00` containers and resolves both
  `rbxasset://` files and asset-delivery URLs (including the legacy
  `https://www.roblox.com/asset/?id=...` spelling).  `view_collect.cpp`
  carries `MeshId`/`TextureID` from `MeshPart` and `MeshId`/`TextureId`,
  `Scale`, `Offset` and `MeshType` from a `SpecialMesh`/`FileMesh` child into
  `ViewBox`; `view_sdl.cpp` uploads per-asset meshes, samples the texture in
  the scene shader, includes the custom mesh in the shadow pass, and applies
  the same material/transparency path as primitives.
- **Native behaviour:** a MeshPart renders its mesh scaled to the replicated
  `Size`; a `SpecialMesh` on a Part renders the file in native stud-space
  multiplied by `Scale` plus `Offset` (its Part `Size` is collision-only);
  `MeshType` without a file selects the native primitive.  A MeshPart
  `TextureID` modulates RGB but, like the native renderer, its image alpha is
  ignored; a SpecialMesh/FileMesh texture also modulates alpha.  Asset-delivery responses in the
  observed 1818 stream are gzip-compressed and use all three URL spellings.
- **Evidence:** native 1818 `tests/live/native_mesh_probe.lua` printed
  `MESH ready meshpart=rbxasset://avatar/meshes/torso.mesh
  texture=rbxasset://content/5a7912dc5a2b6e67ab01e464037d51624a5a9ebd.png
  size=4, 6, 3 special=rbxasset://avatar/heads/head.mesh scale=4, 4, 4` and
  `MESH still mesh=true special=true`.  The same probe on Boblox printed the
  same property values and the renderer loaded the assets:
  `view: mesh loaded rbxasset://avatar/heads/head.mesh tris=846 tex=5` and
  `view: mesh loaded rbxasset://avatar/meshes/torso.mesh tris=44 tex=6`.
  In the same live 1818 session the replicated R15 stream also decoded
  `rbxassetid://74689216175538 tris=2314 tex=7` (v7 Draco) and legacy
  mesh URLs such as `...id=8650168398 tris=168` (v2); the native client uses
  the same asset bytes.  `screenshots/mesh_part_plain.png` and
  `screenshots/mesh_part_texture.png` are the headful `mesh_preview` proof of
  the local torso mesh, before and after the texture sampler (the second uses
  a checker image so the UV path is unambiguous).
- **Tests:** `unit_engine` parses synthetic v2/v5/v6 containers (including
  the 36-byte and 40-byte vertex layouts), decodes the committed v7 Draco
  fixture `tests/data/mesh_v7_draco.mesh`, and pins that
  `MeshPart`/`SpecialMesh` children populate the geometry, scale, offset,
  texture and MeshType fields in `ViewBox`.  The live probe is
  `tests/live/native_mesh_probe.lua`.
- **Container boundary:** the v7 Draco blob is decoded through the pinned
  `third_party/draco` decoder; unsupported/corrupt mesh bytes are cached as a
  failed asset and the primitive fallback keeps the part visible instead of
  crashing the renderer.

### 12. GUI `rbxassetid://` images — implemented

- **Location:** `client/src/view/gui_renderer.cpp` now owns an asynchronous
  image cache: local `rbxasset://`/paths load synchronously, while
  `rbxassetid://`/`rbxthumb://` are fetched off the render thread and uploaded
  with `nvgCreateImageMem` on the main thread.  A failed
  load is cached once and drawn as the label background instead of retrying
  every frame.  `client/src/common/cookie.h` normalizes a stored raw
  `.ROBLOSECURITY` value to the named Cookie header; the old raw header was a
  401 on authenticated asset delivery, and the same fix now covers audio.
- **Native behaviour:** `ImageLabel.Image`/`ImageButton.Image` accept a direct
  image asset or `rbxthumb://`; the image is loaded asynchronously, `IsLoaded`
  flips to true when the texture is resident, and
  `ImageColor3`/`ImageTransparency` are applied at draw time.  Thumbnail APIs
  return JSON whose `imageUrl` is the second hop.  Redirects are followed by
  hand so the session cookie is never sent to the CDN.  A non-image asset
  (RBXMX) stays undecoded and leaves `IsLoaded=false`, matching native.
- **Evidence:** native 1818 `tests/live/native_gui_image_probe.lua` printed
  `GUIIMG native loaded=true image=rbxassetid://284402752 thumb_loaded=true
  thumb=rbxthumb://type=Asset&w=150&h=150&id=284402752`; the same probe on
  Boblox 189707 printed the identical line.  `screenshots/gui_assetid_thumb.png`
  is the headful capture: the native docs' 64x64 padlock appears twice, once
  through the direct asset id and once through the thumbnail JSON hop.
  `screenshots/mesh_preview.png` is the separate deterministic primitive
  capture (see gap 10).
- **Test:** `unit_gui` creates an ImageLabel, checks the collected
  `ImageColor3`/URI command, verifies `IsLoaded` follows the installed cache
  query, and the live probe above verifies the real assetdelivery transition.

### 13. Audio built-in coverage and spatialization — implemented

- **Location:** `client/src/audio/audio_assets.{h,cpp}` and
  `audio_builtins.inc` own the native redirect table; `audio_spatial.{h,cpp}`
  owns rolloff/listener math; `audio.cpp` mixes all voices through one stereo
  device stream and updates `Sound`/`SoundGroup`/listener properties each
  frame.  The runtime's `Sound` defaults now match the native values.
- **Native behaviour:** the shipped Android client maps 192
  `rbxasset://sounds/*` names.  182 come from the redirect table
  (`rbxasset://sounds/<name>` -> numeric asset id; ten are bundled local
  content).  181 names resolve to numeric ids, `action_falling.mp3` aliases
  `action_falling.ogg`, and `uuhhh`/`ouch` alias `oof.ogg` with the legacy
  `12222242` fallback.  The map was decoded from the redirect-table
  initialization at `~/rr/libroblox.so` offsets `0x23f9e00`-`0x23fc600`
  (IDA/objdump); it is not guessed from observed place content.
- **Native defaults measured (1818):** `Sound.Volume=0.5`,
  `PlaybackSpeed=1`, `RollOffMinDistance=10`, `EmitterSize=10`,
  `RollOffMaxDistance=10000`, `RollOffMode=Inverse`, `Looped=false`;
  `SoundService.DistanceFactor=10`, `RolloffScale=1`, `DopplerScale=1`,
  `ListenerType=Camera`, `DefaultListenerLocation=Default`.  These were
  checked in `unit_engine` as generated defaults.
- **Attenuation evidence:** `tests/live/native_audio_probe.lua` ran on the
  native client (1818) and Boblox (1818, `SDL_AUDIODRIVER=dummy`) with
  identical source distances and `EmitterSize=1`, `RollOffMinDistance=1`,
  `RollOffMaxDistance=50`.  `RollOffGain` matched (native / Boblox):
  Inverse d10 `0.100002043 / 0.100000000`, d20 `0.049999714 / 0.050000000`,
  d49.999 `0.020000415 / 0.020000400`, d2 `0.499981046 / 0.500000000`;
  Linear d2 `0.979590297 / 0.979591837`, d10 `0.816330731 / 0.816326531`,
  d20 `0.612242579 / 0.612244898`, d49.999 `0.000021175 / 0.000020408`;
  LinearSquare d2 `0.959597170 / 0.959600167`, d10 `0.666395843 /
  0.666389005`, d20 `0.374840975 / 0.374843815`; InverseTapered d2
  `0.499981165 / 0.500000000`, d10 `0.100002043 / 0.100000000`, d20
  `0.049999714 / 0.050000000`.  `EmitterSize=5` at d10 gave
  `0.500010192 / 0.500000000`, proving EmitterSize wins over
  RollOffMinDistance.  Both clients printed the same panning direction
  (`PAN side=-1 dot=-1.000000000`, `PAN side=1 dot=1.000000000`).  Native
  exposes only the direction; Boblox pins the equal-power left/right gains
  in `unit_audio` because the native `Sound` API has no channel-gain readback.
- **Time/group evidence:** the native and Boblox probes both printed
  `TIME ... length=0.720000 group=0.250000`; `TimePosition` advanced, a seek
  to `0.10` was applied, and `PlaybackSpeed=2` advanced it faster.
- **Test:** `unit_audio` checks all four rolloff modes, hard cutoff, effective
  minimum, equal-power gains, pan direction, 192 built-in names and exact ids;
  `unit_engine` checks `Sound:GetWorldCFrame`,
  `SoundService:GetListenerWorldCFrame`, Camera/ListenerCFrame/ListenerObject
  selection.  Live pair: `tests/live/native_audio_probe.lua` run on both
  clients (native 1818 / Boblox 1818).
- **Residual, exact:** ten native bundled names have no numeric redirect id
  and are absent from `~/rr/assets`: `action_falling.ogg`,
  `action_footsteps_plastic.mp3`, `action_get_up.mp3`, `action_jump.mp3`,
  `action_jump_land.mp3`, `action_swim.mp3`, `impact_explosion_03.mp3`,
  `impact_water.mp3`, `oof.ogg`, `volume_slider.ogg`.  They resolve from the
  local content tree when present; without it Play reports that no asset id
  exists instead of playing a substituted sound.  The native build also logs
  `SoundService.ListenerType is not enabled yet` when ListenerType is set to
  CFrame; its `GetListenerWorldCFrame` remains the camera even after
  `ListenerCFrame=(1000,1000,1000)`.  Boblox intentionally implements the
  documented ListenerCFrame/ListenerObject semantics for its mixer while
  exposing the same method shape; the native cross-check is recorded above.
  One native timing quirk remains: its first spatial voice can report
  `RollOffGain=0` until a later audio callback, so the probe measures stable
  rows after warmup; Boblox computes the gain when Play starts.

### 14. Generated Luau method stubs — open (broad)

- **Location:** `rbx_runtime/src/instance/instance.cpp:559-610`
  (`stub_method`) returns type-correct defaults; `instance.cpp:756` installs
  it for generated methods without a host implementation.  `docs/PARITY.md`
  counts 3,486 of 3,661 generated methods in this state.
- **Native behaviour:** each generated method has its own native semantics,
  often server-only, plugin-only or asynchronous.
- **Evidence:** `docs/PARITY.md:39-42`; `RBX_STRICT_API=1` raises instead of
  returning a default.  The current live `run/api_audit/live_*.json` captures
  show no stub calls for the exercised scripts, so this is broad but not the
  highest-impact reachable item for the current workloads.
- **Why open:** implementing the whole generated surface is a separate
  project; the audit keeps the strict-mode escape hatch and the telemetry.

### 15. Web-backed `Player` APIs — implemented

- **Location:** `rbx_runtime/src/instance/player_web.{h,cpp}` owns the group
  and online-friend documents; `web_http.{h,cpp}` owns the authenticated HTTP
  transport and cookie.  `raknet_main.cpp` installs the join cookie once, and
  `methods.cpp` attaches `IsInGroup[Async]`, `GetRankInGroup[Async]`,
  `GetRoleInGroup[Async]`, and `GetFriendsOnline[Async]` to the real paths.
- **Native endpoints (measured):** `GET /v1/users/{userId}/groups/roles` for
  group roles, `GET /v1/users/{userId}/friends/online?origin=getFriendsOnline`
  for online friends, and the batch `POST https://users.roblox.com/v1/users`
  for `UserName`/`DisplayName`.  The native client sends the account cookie;
  Boblox now installs the same cookie from `--cookie-file`/`RBX_ROBLOSECURITY`.
- **Native group evidence:** the probe `tests/live/native_player_web_probe.lua`
  ran against `kingdudely_alt` (UserId 3733467888) on native 1818 and Boblox
  1818.  Both printed exactly:
  `gid=825735094 member=true rank=0 role=Guest` (a base role),
  `gid=646080861 member=true rank=255 role=Owner`,
  `gid=9685810 member=true rank=2 role=Member`,
  `gid=16536679 member=true rank=1 role=Member`,
  `gid=5101636 member=true rank=1 role=Customer`,
  `gid=2629950 member=true rank=1 role=Dank`,
  `gid=3486412 member=true rank=9 role=Experienced [🕒]`, and
  `gid=1 member=false rank=0 role=Guest`.  The async rows printed the same
  values.  The group document calls base role 825735094 rank 1/role Member;
  the native **observable** result is rank 0/Guest while `IsInGroup` stays
  true, and Boblox preserves that split rather than inventing rank 1.
- **Native friend evidence:** both clients returned the same table schema:
  `VisitorId` (number), `UserName`, `DisplayName`, `IsOnline`, `LastOnline`,
  `LastLocation`, and `LocationType` (number).  For the Studio friend both
  printed `UserName=ChatGGPT DisplayName=Chat GPT IsOnline=true
  LastLocation=Studio LocationType=6`, with `PlaceId`, `GameId`, `UniverseId`
  and `LastLocationType` absent.  When the native presence contained game
  data, both clients printed `PlaceId=9391468976
  GameId=3471f3c3-3aee-47a0-9107-f5c0a07766a2 LocationType=4`; native also
  printed `LastLocation=Website LocationType=2` for a Page friend, and the
  unit fixture pins that mapping for Boblox.  `UniverseId` remains absent in
  the native table and in Boblox.  `GetFriendsOnline(1)` still returned the
  complete online list on both clients (native 10, Boblox 14 in the
  23:53/00:04 runs), so the
  caller's limit is ignored like the native build's endpoint response.  The
  live count is dynamic, so the probe records the schema and representative
  values instead of treating the count as a fixture.
- **Failure evidence:** a detached `Instance.new("Player")` produced the
  native error `Player not in Workspace` for `GetFriendsOnline` and
  `Player not in datamodel` for the group methods; Boblox prints the same
  messages.  A session without a cookie now raises
  `Player:IsInGroup failed because not signed in` instead of returning a fake
  no-relationship answer.  The unit test uses a fake transport and verifies
  the cookie header, all three group APIs plus their async aliases, the exact
  friend fields (including Studio/Game/Page location types), the ignored
  limit, and both error paths.
- **Residual:** the async aliases perform the same authenticated HTTP request
  synchronously inside the Lua call; the native client yields internally while
  waiting.  The returned data and errors match.  `GetFriendsInServerAsync`,
  `GetFriendsInUniverseAsync` and `GetFriendsWhoPlayedAsync` are covered by
  gap 18.
- **Probe limitation:** the Xecutor script entry crosses a C-call boundary;
  a native web call made before the native web client warms up yields and
  fails there with `attempt to yield across metamethod/C-call boundary`.  The
  probe therefore waits 10 seconds after `LocalPlayer` appears (the successful
  native run made its first group call at 25s and then returned
  synchronously).  Boblox has no such warmup requirement; its transport is
  synchronous for this surface.

### 16. Teleport actions — implemented

- **Location:** `rbx_runtime/src/instance/teleport.{h,cpp}` owns the
  process-wide request/arrival/settings/history state; the Lua surface is in
  `rbx_runtime/src/instance/methods.cpp`.  The session handoff is
  `client/apps/raknet_main.cpp` + `client/src/join/join.cpp`; arrival data and
  the DataModel place identity are installed by
  `client/src/engine/live_runtime.cpp`.
- **Native behaviour:** `Teleport` returns `nil` synchronously, tears down the
  current DataModel and joins the target with `isTeleport=true`; the carried
  `teleportData` is readable on the destination through
  `GetLocalPlayerTeleportData` and `LocalPlayerArrivedFromTeleport`.  Invalid
  place ids raise `Cannot teleport to invalid place id. Aborting teleport.`;
  a second call while one is in flight returns `nil` and fires
  `TeleportInitFailed` with `Enum.TeleportResult.IsTeleporting` and
  `The previous teleport is in processing (IsTeleporting)`.  `TeleportAsync`
  and `TeleportToPrivateServer` are server-only and raise the native errors;
  `TeleportSwitchServer(Instance)` raises `Unable to cast Instance to Array`;
  `SetTeleportSetting(key, nil)` raises `Argument 2 missing or nil`.
  `GetArrivingTeleportGui()` returns `nil` in this native build even after
  `SetTeleportGui`, so the runtime intentionally preserves that quirk.
- **Evidence:** native probes (1818 -> 189707) preserved
  `{probe="native", n=42, arr={1,2,3}, nested={ok=true}}`; `TeleportedPlacesBackHistory()`
  and `TeleportedUniversesBackHistory()` were live arrays; IDA registration and
  the native transition log showed `isTeleport:true` and the normal join path
  (`docs/PARITY.md`, `FINDINGS.md` teleport section).
- **Verification:** `unit_engine` covers invalid ids, successful/duplicate
  requests, nested data round-trip, settings, history, server-only errors, the
  GUI nil quirk, and `TeleportInitFailed` (54 assertions).  A live Boblox run
  joined 1818, teleported to 189707, and printed the same data, `{1818}` place
  history, `{13058}` universe history, and a `LocalPlayerArrivedFromTeleport`
  event with `gui=nil`.  The probe is `tests/live/teleport_probe.lua`.

### 17. `firetouchinterest` message id — deferred (executor surface)

- **Location:** `client/src/exec/exec.cpp:1003-1026` uses `0x86` by default
  and allows `RBX_TOUCH_ID` probing.
- **Native status:** `firetouchinterest` is an executor API, not a native
  Roblox client API; there is no native client writer to match.  The comment
  already states the id is unconfirmed.
- **Evidence:** `exec.cpp:1003-1005`; no native capture exists because the
  native client never exposes this global.
- **Why deferred:** it is outside native-client parity.  It remains an
  explicitly selectable probe rather than a claimed native behaviour.

### 18. Remaining Player friend APIs — implemented

- **Location:** `rbx_runtime/src/instance/player_web.{h,cpp}` owns the
  authenticated `friendsWhoPlayed` request and parser;
  `rbx_runtime/src/instance/methods.cpp` binds the three Lua methods.
- **Native behaviour:** `Player:GetFriendsWhoPlayedAsync()` returns an array
  of numeric user ids for friends who have ever played the current universe.
  The native request is
  `GET https://friends.roblox.com/v1/users/{userId}/friendsWhoPlayed?universeId={universeId}`
  with the `Roblox-Place-Id`, `Roblox-Universe-Id` and `Roblox-User-Id` session
  headers.  The endpoint returns objects containing `friendUserId` and
  `lastPlayedTimestamp`; the public API exposes only the ids.  A detached
  Player raises `Player not in Workspace`.  `GetFriendsInServerAsync` is
  exposed but unwired in this build and raises exactly `Not Implemented`.
  `GetFriendsInUniverseAsync` is also unwired, but its calling coroutine never
  resumes; the native probe stayed pending through its watchdog and the rest
  of the script finished normally.
- **Evidence:** native 2.743 probe (1818, account 2477467842) printed
  `WHO_PLAYED type=table count=10` with ids including `10113391421` and
  `681007536`, `SERVER_ASYNC ok=false ... value=Not Implemented`, and
  `WHO_DETACHED ok=false ... value=Player not in Workspace`;
  `GetFriendsInUniverseAsync` produced no result while the watchdog and
  `MAIN-DONE` still ran.  Direct authenticated calls to the endpoint returned
  the same ten ids; the endpoint order is not stable between the native cache
  path and a direct request, so the live check compares the set.
- **Implementation:** one shared request uses the native headers and parser.
  The Lua binding preserves endpoint order and pushes ids as doubles (ids above
  2^32 remain exact), rejects a detached Player with the native message, and
  parks a `GetFriendsInUniverseAsync` task forever rather than inventing an
  empty result.  `GetFriendsInServerAsync` raises the native `Not Implemented`
  message.  The old empty-table fallbacks are removed.
- **Verification:** `unit_engine` covers the endpoint URL/headers, the id
  schema including a 10,113,391,421 id, the exact server error, the parked
  universe task, and the detached error (59 assertions in the Player web
  case).  A live Boblox 1818 run printed the same ten ids (`WHO_PLAYED
  count=10`), `SERVER_ASYNC ... Not Implemented`, `WHO_DETACHED ... Player not
  in Workspace`, then `WATCHDOG` and `MAIN-DONE` while the universe call stayed
  parked.  The probe is `tests/live/native_friend_edge_probe.lua`.

### 19. GUI absolute geometry and auto-layout — implemented

- **Location:** `rbx_runtime/src/gui/gui_layout.cpp` owns the shared walk,
  `rbx_runtime/src/api/gui.cpp` owns the native defaults, and
  `rbx_runtime/src/instance/instance.cpp` refreshes geometry when a script
  reads an `Absolute*` property.  `client/src/view/view_sdl.cpp` supplies the
  real framebuffer viewport to the same walk.
- **Before:** `AbsolutePosition`, `AbsoluteSize`, `AbsoluteContentSize`,
  `AbsoluteCellCount` and `AbsoluteCellSize` were all the read-only zero
  defaults from the generated API table.  `UIListLayout`, `UIGridLayout` and
  `UIPadding` were present as classes but ignored by the walk, so their
  children stayed at their raw `Position`/`Size` values.  A live Boblox
  probe printed `0.0,0.0 0.0x0.0` for every object.
- **Native behaviour:** the native 2.743 client lays out direct `GuiObject`
  siblings, resolves `UIPadding` against the parent rect, and reports the
  resulting geometry synchronously.  Measured defaults: `UIListLayout` fills
  Vertical, aligns Left/Top, sorts Name, 0 padding; `UIGridLayout` uses
  `{0,100},{0,100}` cells, `{0,5},{0,5}` padding, Horizontal fill, Left/Top,
  TopLeft start and Name order.  The probe's relative results are: vertical
  list `B=0,0`, `A=0,40`, `C=0,90`, content `200x110`; horizontal center list
  `H1=103,40`, `H2=190,30`, `H3=257,35`, content `194x40`; three-column grid
  `0/110/220` and row 2 at `60`, content `320x110`; padded list content
  `380x55` with children at `(10,20)` and `(10,55)`; vertical bottom-right
  grid cells mirrored to `220,60`/`220,0`/...; auto grid `4x2` cells, content
  `390x90`; wrapped rows `0,0`/`160,0` and `0,50`/`160,50`, content
  `310x100`.
  Flex measurements add: `Fill` grows every item by an equal main-axis share
  (and shrinks proportionally when the content is smaller), the `Space*` modes
  replace `Padding` with the distributed gap, `ItemLineAlignment` offsets each
  item inside its wrapped line, `Automatic` follows the cross-axis alignment,
  and `FillDirectionMaxCells` is a cap on the number of cells that actually fit
  (a max of 10 in a 400 px/100 px-cell frame still yields three columns).
- **Implementation:** one layout pass now writes `AbsolutePosition`,
  `AbsoluteSize`, `AbsoluteRotation`, `RawRect2D` and `ClippedRect` for every
  visited `GuiBase2d`; it resolves `UIPadding` before sizing children, applies
  `UIListLayout` (both axes, alignment, LayoutOrder/Name sorting and `Wraps`)
  and `UIGridLayout` (cell size/padding, both fill directions, automatic or
  capped cells, all four start corners and alignment), including
  `HorizontalFlex`/`VerticalFlex` (Fill and all Space modes, with proportional
  Fill shrinking), `ItemLineAlignment` (Automatic/Start/Center/End/Stretch) and
  padding-aware cell capacities.  `ApplyLayout` and `SetCustomSortFunction`
  are attached to both layout classes; Custom order invokes the script
  comparator (ordinary `Name` reads now use the identity field rather than the
  generated class-default string); it updates the layout objects' absolute
  content/cell properties.  Reading an `Absolute*` property
  performs the same walk synchronously if the renderer has not run yet, using
  the renderer viewport or the 1280x720 headless fallback.
- **Evidence:** `tests/live/gui_layout_probe.lua` is the reproducible probe.
  Native 2.743 and Boblox 1818 produced 103 identical relative rows for every
  case (`/tmp/gui_final_native.txt`, `/tmp/gui_final_boblox.txt`).  Native's
  host topbar makes the absolute ScreenGui origin `-58` when
  `IgnoreGuiInset=true` and `0` otherwise; the probe reports positions
  relative to the probe Frame so the host inset cannot hide layout parity.
  The absolute geometry, content size, cell count and cell size values are
  identical.
- **Verification:** `unit_gui` covers list order/alignment/content size, grid
  cell geometry/counts and capacity capping, padding, wrapping, flex
  fill/spacing (including shrink) and the native defaults; the live probe
  covers the same cases against the real 2.743 client.

### 20. GUI scale and size constraints — implemented

- **Location:** `rbx_runtime/src/gui/gui_layout.cpp` computes effective sizes
  before the shared walk; `rbx_runtime/src/api/gui.cpp` installs the native
  defaults.  The same pass feeds `AbsoluteSize`, layout cell/child geometry,
  `UIPadding`, scroll metrics and renderer text size.
- **Before:** `UIScale` multiplied nothing, `UISizeConstraint`'s generated
  `MinSize`/`MaxSize` stayed at `0,0`, and `UIAspectRatioConstraint` was
  ignored.  A scaled frame stayed at its base size and constraints never
  clamped it.
- **Native behaviour:** the native 2.743 client uses `UIScale` as a
  cumulative multiplier for the object's own size and its descendants, while
  leaving the object's own top-left position alone.  Multiple components
  multiply.  Sizing order is base `Size` -> `UISizeConstraint` clamp ->
  `UIAspectRatioConstraint` -> own `UIScale`.  `UISizeConstraint` defaults to
  `MinSize=(0,0)` and `MaxSize=(inf,inf)`; `UIAspectRatioConstraint` defaults
  to ratio `1`, `FitWithinMaxSize`, `DominantAxis.Width`.  `FitWithinMaxSize`
  fits the ratio inside the constrained size; `ScaleWithParentSize` keeps the
  dominant axis and derives the other.  `UIPadding` offsets and `CanvasSize`
  offsets live in the scaled coordinate space.  A zero scale collapses the
  subtree to `0x0`.
- **Implementation:** `constrained_size` applies the two child components in
  native order and then the object's own cumulative `UIScale`; every walk
  passes the parent's cumulative scale so `Position`/`Size` offsets, padding,
  list/grid cells and scroll metrics use the same coordinate space.  Text
  commands receive the same scaled text size, so a scaled label renders at
  the native effective size.  The generated zero defaults are replaced with
  the native `UIScale=1`, unbounded constraint and ratio defaults.
- **Evidence:** `tests/live/ui_constraints_probe.lua` is the reproducible
  probe.  Native 2.743 and Boblox 1818 produced 25 identical relative rows
  (`/tmp/ui_con_native_final.txt`, `/tmp/ui_con_boblox_final.txt`), covering own/nested
  scale, scale plus child position, constraints before scale, aspect plus
  constraint, both aspect modes, scaled padding, zero scale and all three
  default tables.
- **Verification:** `unit_gui` adds scale inheritance, constraint-before-
  aspect-before-scale ordering, nested scales, scaled padding and default
  assertions.  `AutomaticSize` is gap 21; `UITextSizeConstraint` is gap 22.

### 21. `GuiObject.AutomaticSize` — implemented

- **Location:** `rbx_runtime/src/gui/gui_layout.cpp` measures automatic
  content and folds it into the same `constrained_size` pipeline that
  `UISizeConstraint`, `UIAspectRatioConstraint` and `UIScale` already use.
  `TextWrap`/`TextWrapped` share one native flag in
  `rbx_runtime/src/instance/instance.cpp`; the generated
  `GuiObject.AutomaticSize` default remains `None`.
- **Native behaviour:** the enum is `None=0`, `X=1`, `Y=2`, `XY=3` and
  defaults to `None`.  A visible `GuiObject` child contributes its resolved
  right/bottom edge, including negative positions; an invisible child is
  ignored.  `UIPadding` is additive and `BorderSizePixel` is not part of the
  content box.  `UIListLayout` contributes its items, padding and parent
  padding; `UIGridLayout` grows a horizontal grid to one row and a vertical
  grid to one column, with cell size, cell padding and parent padding.
  Authored `Size` is the starting box, so its offset and scale components
  survive unless a constraint max shrinks an axis; the measured content is a
  floor applied before aspect fitting and again after the constraint/aspect
  stage, then the object's own `UIScale` runs last.  Nested automatic parents
  resolve inside out.  Text labels read the same native Legacy metric tables
  as TextBounds (`rbx_runtime/src/gui/gui_font_metrics.cpp`), covering Legacy,
  Arial, ArialBold, SourceSans and SourceSansBold at every integer size 1..100
  with Legacy's 1.5em and the other faces' 1em line heights.  `TextWrap` wraps
  at the authored width using those advances, and an automatic X axis removes
  that authored wrapping width, as native does.  A TextScaled label with a
  non-automatic parent reports the 1x1 automatic floor; when both the label
  and its parent are automatically sized on both axes, native's circular
  fallback measures one 67 px Legacy line (100 px line height), which Boblox
  reproduces, and label `UIPadding` feeds the measured content.
- **Implementation:** `auto_content_size` handles text, list, grid and flat
  child content plus `UIPadding`; `constrained_size` applies the content
  floor before constraints/aspect and after them, before `UIScale`.  The GUI
  walk now lays out children of zero-sized and single-axis frames so
  `AutomaticSize.X`/`Y` parents still position their descendants.  There are
  no placeholder sizes or ignored enum values.
- **Evidence:** `tests/live/automatic_size_probe.lua` is the reproducible
  probe.  Native 2.743 and Boblox 1818 produced 97 identical AutomaticSize
  data rows plus 18 identical `UITextSizeConstraint` rows (116 lines with
  `DONE`),
  covering all four enum values, defaults, empty/degenerate/negative boxes,
  padding, border, invisible children, nested parents, `Frame`/
  `TextButton`/`ImageLabel` children, list/grid padding and spacing, explicit
  offset/scale sizes, constraint/aspect/scale ordering, `AbsolutePosition`/
  `AbsoluteSize`, and text measuring.  The nested TextScaled rows cover
  AutoXY, long, multiline, empty, padded and explicit-size fallbacks; nested TextWrapped
  AutoXY removes the authored wrap width.  Committed outputs:
  `tests/live/output/automatic_size_native.txt` and
  `tests/live/output/automatic_size_boblox.txt` (diff empty).  The constraint
  raw samples are in
  `tests/live/output/automatic_size_uitextsizeconstraint_native_samples.txt`.
  Three fresh
  native processes are preserved in
  `tests/live/output/automatic_size_textscaled_native_samples.txt` (frames
  1..5 in the first run, initial/frame 5 in the other two); `201x100` /
  `1110x100` / `0x100` / `206x105` and the nested TextWrapped values are
  stable across them; the full 116-row probe also reproduced
  byte-identically on three fresh native processes.  The existing
  `tests/live/ui_constraints_probe.lua` (25 rows) and
  `tests/live/gui_layout_probe.lua` (103 rows) still diff empty after the
  change.
- **Verification:** `unit_gui` adds 414 assertions for the enum/defaults,
  modes, padding/border, invisible children, nested/zero parents, list/grid
  content, constraint/aspect/scale ordering, parent scale, text bounds, wrap,
  the nested TextScaled fallbacks (including multiline line height), nested
  TextWrapped AutoXY, label padding and the `TextWrapped` alias; the
  `UITextSizeConstraint` case adds property clamping, min/max precedence,
  fixed/wrapped/alignment, add/remove ordering, non-scaled labels and nested
  `AutomaticSize.XY`.  The full binary is 28 cases / 1,835 assertions, all
  passing.

### 22. `UITextSizeConstraint` — implemented

- **Location:** `rbx_runtime/src/instance/instance.cpp` stores and clamps the
  two Int properties and couples the shared TextScaled/TextWrap flags;
  `rbx_runtime/src/api/gui.cpp` installs the native defaults;
  `rbx_runtime/src/gui/gui_layout.cpp` clamps the effective scaled font and
  derives `TextBounds`/`TextFits`.
- **Native behaviour (2.743):** a new constraint reads
  `MinTextSize=1`, `MaxTextSize=100`; every write clamps independently to
  `[1,100]`.  The stored values may still have min > max, in which case Min
  wins (`30/10` -> font 30).  Scaling enables both wrap spellings; clearing
  either spelling also clears TextScaled.  The natural TextScaled fit is
  height-limited and capped at a 100 px line (66.667 px font), giving
  `Hello` in a 400x200 label `201x100`.  The constraint can lower or raise
  that font, including above the cap.  A fixed label reports the effective
  line height (`floor(font * 1.5)`) and full unwrapped width; when that width
  exceeds the label, TextScaled wraps character-by-character and reports the
  visible lines only (`floor(rect.h / line_height)`, at least one), while
  `TextFits` stays false unless the entire wrapped text fits.  The constraint
  also clamps a non-TextScaled label's authored size.  Alignment and
  TextButton/TextBox do not change the measured box.  Nested label+parent
  `AutomaticSize.XY` follows the same font/constraint path and reports
  matching `AbsoluteSize` and `TextBounds`.
- **Implementation:** `make_text_layout` computes the natural fit, applies
  the constraint, handles the cap fallback, then either the ordinary line or
  the character-wrapped visible-line box.  `append_text` passes the clamped
  font to the renderer with `scaled=false`, so NanoVG draws the same size that
  the runtime reports.  No path ignores the constraint when TextScaled is
  false or when an automatic parent changes the layout later.
- **Evidence:** the extended
  `tests/live/automatic_size_probe.lua` keeps the original AutomaticSize
  rows byte-identical, appends 18 constraint rows and produces 116 lines
  including `DONE`.  Three fresh native 1818 runs are byte-identical; the
  Boblox output is byte-identical to them.  Committed outputs:
  `tests/live/output/automatic_size_{native,boblox}.txt` and
  `tests/live/output/automatic_size_uitextsizeconstraint_native_samples.txt`.
  The same change keeps `ui_constraints_probe.lua` (25 rows) and
  `gui_layout_probe.lua` (103 rows) byte-identical on fresh native/Boblox
  runs.
- **Verification:** the new `unit_gui` case covers defaults and write
  clamping, min>max, zero writes, natural/max/min/equal/min-one boxes,
  remove/re-add ordering, non-scaled clamping and nested AutoXY geometry,
  including clamped non-scaled auto content.  The full binary is 28 cases /
  1,835 assertions, all passing.  Legacy advances are native-exact at every
  integer size 1..100 for all five faces; the new probe and assertions are
  described in gap 23 below.

### 23. Legacy text metrics — implemented

- **Location:** `rbx_runtime/src/gui/gui_font_metrics.{h,cpp}` owns the native
  advances, GPOS adjustments and line heights; generated
  `gui_font_metrics_data.inc`, `gui_font_metrics_unicode_data.inc` and
  `gui_font_metrics_pairs_data.inc` carry the captured native data.
  `gui_layout.cpp`, `gui_input.cpp` and `client/src/view/gui_renderer.cpp`
  all read that module.  The renderer no longer installs a NanoVG measurer:
  TextBounds, wrapping, TextFits, AutomaticSize, input caret geometry and the
  painted caret/selection all share the same table.
- **Native behaviour:** the native client shapes five Legacy faces with
  FreeType hinted advances and HarfBuzz GPOS kerning at fractional pixel
  sizes.  Legacy/Arial use Arimo, ArialBold uses Arimo-Bold, SourceSans uses
  SourceSansPro-Regular and SourceSansBold uses SourceSansPro-Bold.  Line
  height is `floor(1.5 * TextSize)` for Legacy and `floor(TextSize)` for the
  other four.  TextSize clamps to `[1, 100]` and then floors before the
  advance lookup; the same effective size drives line height.  Integer
  advances plus quantized GPOS adjustments are additive across a line
  (including around spaces); a line's leading spaces carry no kerning until
  the first non-space glyph.  TextBounds rounds the line width and reports
  the integer line height; newline height is lines times that height.
- **Coverage:** the generated Unicode table contains native standalone
  advances for 727 codepoints on all five faces at every integer size 1..100.
  The charset is Latin-1, Latin Extended-A, combining marks, general
  punctuation, currency, letterlike symbols, arrows, mathematical operators,
  box drawing, block/geometric shapes, common UI symbols and U+FFFD.  The
  ASCII tables remain native-exact.  A codepoint outside the captured set
  uses the captured per-face/size U+FFFD replacement advance, matching the
  native replacement glyph rather than a constant heuristic.  Non-ASCII pair
  adjustments are captured
  for eight native target pairs (`Vé`, `fé`, `o”`, `rö`, `rø`, `s”`, `é?`,
  `Łó`) on SourceSans and SourceSansBold, derived from native pair widths
  minus native standalone advances.  A pair outside those tables contributes
  zero; no guessed kerning is applied.  Multi-pair contextual shaping is not
  synthesized: the observed `! ¿Q` context is 3 px narrower on native than
  the additive model at some large sizes and is recorded rather than
  special-cased.  Combining-mark advances are exact; painted mark offsets
  remain renderer shaping.
- **Evidence:** `tests/live/font_metrics_probe.lua` covers all five faces at
  sizes 1, 8 and every integer 9..100, with eight strings (plain advances,
  AV/VA/AT/TA pairs, spaces, punctuation), a wrapped long line, TextFits and
  AutomaticSize.XY.  `tests/live/font_metrics_capture.lua` is the generated
  727-codepoint capture.  `tests/live/font_metrics_extended_probe.lua` adds
  18 non-ASCII/fractional strings at sizes 0.25, 1.99, 13.01, 13.99, 50.50,
  99.99, 100, 100.01, 150 and 1000 on all five faces with AutomaticSize;
  its 50 data rows plus `DONE` are byte-identical native/Boblox and committed
  as `tests/live/output/font_metrics_extended_native.txt`.  For fixed wrapped
  labels native clips `TextBounds.Y` to the authored height and still clips X
  to the box; `TextFits` describes the whole string.  Direct native capture:
  the probe line is `74x300` in an 80x300 SourceSans-50 label (fit false) and
  `74x350` in 80x400 (fit true).  Overlong words break at characters.  Raw
  native and Boblox outputs remain `tests/live/output/font_metrics_native.txt`
  and `font_metrics_boblox.txt`; repeated native runs are byte-identical.
  The original 97 AutomaticSize data rows, the 18 constraint rows (116 lines
  total), `ui_constraints_probe.lua` (25 rows) and `gui_layout_probe.lua`
  (103 rows) remain byte-identical.
- **Reproduction:** `python3 tests/live/generate_font_metrics.py pairprobe
  --out tests/live/font_metrics_pairs_probe.lua`, run it natively, then
  `pairfixture --out tests/live/output/font_metrics_pairs_native.txt <logs>`
  and `pairemit --out rbx_runtime/src/gui/gui_font_metrics_pairs_data.inc
  --fixture tests/live/output/font_metrics_pairs_native.txt`.  The pair
  fixture stores raw native pair and standalone widths, not hand-entered
  adjustments.  The charset workflow remains `probe`, `fixture`, `emit` with
  `tests/live/output/font_metrics_charset_native.txt`.
- **Verification:** `unit_gui` pins the native widths for `Hello` at 10 px and
  every size 67..100 for all five faces, plus `AVATAR`, `Hello world` and
  line heights at 10/100 px.  Two added cases cover 12 non-ASCII/fractional
  rows across all faces, caret prefix geometry, the 80x300/80x400 wrapped
  clip and TextFits pair, AutomaticSize at 50/1000/0.25, and the UTF-8
  codepoint boundaries.  The full binary is 28 cases / 1,835 assertions, all
  passing.

## Closed / not gaps (stale ledger entries)

- `0x83/07` RemoteEvent receive and `FireServer` send are implemented
  (`rbx_runtime/src/instance/remote.cpp`, `client/src/exec/exec.cpp`).
- `0x83/0a` ack records, `0x83/05`/`0x83/06` streams, `0x83/23` clock and
  `0x51`-`0x56` sync builders are implemented and pinned in
  `tests/unit/unit_batch.cpp`; `docs/PARITY.md` predates the clock burst and
  should not be read as current for those ids.
- `GetNetworkOwner` / `GetNetworkOwnershipAuto` are not stubs: the native
  client raises the exact server-only error, and Boblox matches it
  (`rbx_runtime/src/instance/methods.cpp:903-919`, `docs/OWNERSHIP.md`).
- The SDL-less `view_stub.cpp` and `audio_stub.cpp` are build fallbacks, not
  the configured live client; they fail loudly rather than pretending to
  render or play.

## Selected implementation

The highest-ranked gap with enough native evidence was the coupled
**rigid-assembly and assembly-root** surface (ranks 7 and 8).  It was
implemented as one shared model rather than a second traversal:

- `rbx_runtime/src/instance/assembly.h` / `assembly.cpp` — graph, root
  selection and tie rules.
- `rbx_runtime/src/instance/methods.cpp:743-766` — `GetConnectedParts`,
  `GetJoints`, `GetRootPart` bindings.
- `rbx_runtime/src/instance/instance.cpp:191` — dynamic
  `AssemblyRootPart` getter.
- `rbx_runtime/src/instance/instance.h:87` — creation sequence for the
  native local-instance tie order.
- `rbx_runtime/src/instance/classes.cpp:52-56` — native `Enabled` defaults.
- `tests/unit/unit_engine.cpp:3350-3495` and `:3497` — connected graph,
  joint edge cases and the full root-precedence matrix.
- `tests/live/root_probe.lua` and the `tests/live/native_root_*.lua`
  evidence probes; `tests/live/joint_probe.lua` updated for self-inclusive
  `GetConnectedParts`.

The unit test creates actual `Weld`/`WeldConstraint` instances and checks
direct and recursive connected parts, disabled joints, self inclusion, the
root precedence rules, anchor splits and the orphan boundary.  Live 189707
runs repeat the root matrix and the joint matrix against a real session.  Full
build and test results are recorded in the final report for this pass.

The later primitive pass closed **CornerWedge** using the untouched
ROBLOX2016 `Mesh::makeCornerWedge` vertex/face table: the renderer now has a
dedicated five-vertex mesh, the Jolt hull uses the same five points, and
`GetMass` uses the native pyramid volume and float rounding.  The live mass
probe is line-identical between native 1818 and Boblox 189707 for all five
`Enum.PartType` shapes; `unit_engine` pins the same values.

The GUI pass closed **rbxassetid/rbxthumb image loading** with an asynchronous
asset cache, authenticated delivery, thumbnail second hops, image tint and the
runtime `IsLoaded` query.  The native and Boblox probes print the same
loaded/thumb line, and the headful `screenshots/gui_assetid_thumb.png` capture
shows both fetched images.  The deterministic
`screenshots/mesh_preview.png` capture renders all five primitives (Ball,
Block, Cylinder, Wedge, CornerWedge) from the native mesh tables for visual
comparison.
