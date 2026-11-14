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

### 5. Driven `0xA6` movement (`nActions`) — deferred

- **Location:** `client/apps/raknet_main.cpp:1072-1106` describes the
  ControlState probe; `client/apps/raknet_main.cpp:1981-1983` records the
  `nActions == 0` limitation.  `README.md:200-211` documents the same.
- **Native behaviour:** the native route frames carry real
  ControlState/action slots; ours deliberately sends the native-shaped
  frame with `nActions = 0`, and the `--slots` probe is explicitly labelled
  as an experiment.
- **Evidence:** `README.md:200-211`, `FINDINGS.md:4448`,
  `docs/PARITY.md:110` (native 37 B versus the older oversized shape).
- **Why deferred:** the slot-to-movement mapping was never recovered.  The
  client already sends native-shaped frames rather than inventing a
  direction; a fabricated "forward" slot would be worse than the honest
  no-input state.

### 6. `0xA1` records with a null ref — deferred (measured)

- **Location:** `client/src/solve/repl_stream.cpp:37-62` rejects a boundary
  whose first byte is zero because it assumes the five-byte ref form.
- **Native behaviour:** about 8% of record boundaries open a record with
  `ref lo == 0`; the head is four bytes shorter.  Accepting the layout fixes
  nine of eleven dumped failures but desyncs other frames when used as a
  general header rule.
- **Evidence:** `FINDINGS.md:7160-7186` (counts and the body-decodability
  table).  Three body-decodability variants were measured on the same 2,012
  frames: permissive closure lost 23, strict closure lost 1,056, and the
  mixed rule lost 81, against 12 for the current rule.  The reason is that
  15% of real records are intentionally partial because the attribute section
  is not modelled.
- **Why deferred:** the next step is modelling the attribute section, not a
  framing guess.  `tools/a1_gap_stats.py` remains the measurement front end.

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
  waiting.  The returned data and errors match.  `GetFriendsInServerAsync` and
  `GetFriendsInUniverseAsync` remain their pre-existing empty-table/backend
  rows outside this gap; they were not represented as implemented here.
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
