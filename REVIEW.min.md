# Review — replicated entity handles

## Context

A clean-room C++17 implementation of a proprietary real-time protocol. UDP, encrypted
datagrams, world state streamed as entity records with server-assigned reference ids.
~20 test suites, all passing. This is my own client; every claim below was produced by a
command whose output is quoted.

## Three changes already made

**1. 64-bit values truncated at the scripting boundary.** The embedded VM defines
`lua_Integer` as `int`, so a push helper taking `int64_t` narrowed to 32 bits and
sign-extended. Identifiers above 2^31 read back negative. Fixed with the wide push/read
helpers.

**2. A computed property returned a fallback instead of absence.** A list accessor
returned the first element when the requested entry was absent, so callers polling for
"my own entity" resolved to an unrelated one on the first poll and cached it. Fixed: nil
until the correct entry exists.

**3. Removed a fabricated entity.** A block created an entity locally when replication
had not delivered the real one yet; replication does deliver it, so the fabrication
became a duplicate record. Deletion, not addition.

## The open problem

The client must send its own entity's transform upstream. The message carries entity
handles. Measured, decoded from a first-party client's plaintext capture at the same
place:

    83 05 16 | aa0a0000 | 00000000 | ab0a0000 | 67f31542 a28f2242 2c62d441
              | 00000000 | ffffffff | 01 | ad0a0000 | 40072d44 | 00

Two handles, a Vector3 position, then flags and one more handle.

**The handles are `0x0AAA`-scale. The reference ids used elsewhere in the replication
stream are in the millions** (e.g. `0x03C37CE2`). So transform updates address a
different namespace than the object-reference table.

My client resolves handles by reverse lookup over the instance table built from
replication, so it can only ever produce million-scale ids. It therefore addresses the
wrong space, and the server acknowledges but never applies.

Directional message counts over comparable windows, same place:

    first-party client: 195 outbound 0x83 messages (~70 s)
    my client:            7 outbound 0x83 messages (~18 s)

## The question

In a client-authoritative ownership model, where does the small entity handle come from?

**A.** Allocated by the server and communicated at spawn (look for it in the inbound
stream before the first transform upload).

**B.** Allocated client-side, adopted by the server on first update (look for the server
echoing it back only after that first upload).

Both captures exist, decrypted, with the full ordered application message stream. Tell me
which observation separates A from B, and what to search for.

## Output schema — use these headings

    VERDICT:
      One paragraph on the three changes: correct / needs changes / wrong, and why.

    DEFECTS:
      Bullet list, one line each. Empty is fine.

    HANDLE_PROVENANCE:
      A or B, with the reasoning.

    DISCRIMINATING_OBSERVATION:
      Exactly what to look for in the capture, and what each outcome means.

    NEXT_ACTION:
      The single next step, in one paragraph, actionable without follow-up questions.

One reply. Do not restate this document.
