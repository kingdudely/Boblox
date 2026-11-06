# REVIEW — one open question, three file changes

## What this is

A clean-room implementation of a proprietary game wire protocol in C++17 (Linux).
Not a fork, not a patched binary: session establishment, key exchange, encrypted
datagrams and world-state application implemented from observation, with the game's own
Luau VM embedded (pinned, unmodified) so replicated script bytecode executes.

Scope: the author's own accounts, on a public test place, to study how the server
replicates state. 20 test suites, all passing.

## What changed in this session (3 files, ~90 lines net)

`rbx_runtime/src/instance/class_registry.cpp`
    Luau's `lua_Integer` is `int` in this build, so `lua_pushinteger(L, int64_t)`
    truncated 64-bit values to 32 bits and sign-extended them. Every user id above
    2^31 read back as garbage. Now `lua_pushinteger64` / `lua_tointeger64`.

`rbx_runtime/src/instance/instance.cpp`
    A computed property (`Players.LocalPlayer`) returned the first child in a list
    when the correct entry was absent, instead of nil. Callers polling for the local
    player therefore resolved to an unrelated entry immediately and cached it.
    Now returns nil until the correct entry exists.

`client/src/engine/live_runtime.cpp`
    Removed a block that fabricated a `Player` instance when replication had not yet
    delivered the real one. Replication does deliver it, a second or two late, so the
    fabricated instance became a second row with the same name and a different
    identity. Deletion, not addition — no fallback path remains.

## Evidence, all measured

    [diag] local name='poopnestomegahater' id=11924576267 rows=0
    [diag] local name='poopnestomegahater' id=11924576267 rows=1     (never 2)
    player joined: poopnestomegahater:11924576267                    (id exact)
    player joined: poopnest5002:11754932511                          (id exact)

## The open question

Movement. The client uploads its own body transform via an entity-update message that
carries two references: the character's `Humanoid` and its root part. Both are resolved
by looking up an instance created from replication (`ref_of(instance)`).

Measured across two sessions in the same server as an observer account:

- 25 players in the roster; 19 got a `player character: <name>` line.
- Our own `Player.Character` property was **never set** — for any other player it was.
- The upload path never initialised: zero `[phys]` lines, zero datagrams sent.

Conclusion: the server does not replicate our own character to us. There is no instance,
therefore no reference, therefore nothing to address an upload to. This is expected
behaviour for an owning client — the official client does not wait to be told where it
is either.

Two candidate mechanisms, mutually exclusive:

**A. Missing handshake.** The client asks the server to spawn a character; the reply
carries the character's reference; only then does the server accept transforms for it.
If so, we are missing an outbound message, and the fix is to find and send it.

**B. Client-authoritative construction.** The client builds the character locally from
avatar data, assigns its own reference, and the server adopts the entity on first
transform upload. If so, the fix is local construction plus the reference the server
associates with the player entity.

The distinguishing evidence is a capture of a real client's outbound stream across its
own spawn: whether the reference used in the first transform upload ever appeared in an
inbound message beforehand. Capture tooling already exists in this repo
(`tools/native_capture.sh`, offline decrypt path).

## What I want from you

1. Read the three changes. Flag anything that is wrong, unsafe in C++ terms, or that
   violates the intent stated in the comments.
2. On the open question: say which candidate you consider more likely and what single
   observation would separate them. Do not speculate about wire layouts that are not in
   this document — the project rule is that every claim carries a command.
3. Produce an execution brief for the working model, in the schema below.

## Output schema — use these headings exactly

    VERDICT:
      One paragraph. Correct / needs changes / wrong, and why.

    DEFECTS:
      Bullet list, file:line where possible, one line each. Empty is fine.

    HYPOTHESIS:
      A or B. One sentence of reasoning.

    DISCRIMINATING_OBSERVATION:
      The single measurement that decides it, phrased as something that can be run.

    EXECUTION_BRIEF:
      A self-contained prompt for the working model. It must state: the goal in one
      line, the next action, the expected signal, the branch for each outcome, and
      what not to do. Under 400 words. No restatement of this document.

Keep it tight. One reply is enough — this is a review, not a conversation.
