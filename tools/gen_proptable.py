#!/usr/bin/env python3
"""gen_proptable.py — native prop table (tools/proptable.tsv) + solver wire-shape
overrides (tools/proptable_overrides.tsv) -> client/src/solve/proptable.inc.

The .tsv is a live register dump of the replicator's runtime prop table
(10146 entries == prop_bound+1): entry byte[9] is the value-tag number in
snapshot numbering (== v57[1] == the byte sub_340A3E4 dispatches on;
verified per-prop via breakpoints on sub_3404858/0x3404AFB).

The numbers are the client's STORAGE tags. Where live wire traffic proves
a different wire shape (server newer than traced client — version skew),
the override wins for parsing; see FINDINGS.md "Recv v3".

Two tables are emitted:

  * the static fallback (kPropTags/kPropPrefix/kPropFixedLen/kPropAltLen),
    indexed by the frozen dump's prop id — bootstrap only; ids shift on
    every schema that inserts members;
  * the name-keyed overrides (kPropOverrides), applied by
    propTagsForSchema() to the live 0x97 schema's member concatenation.
    Keying by (class name, member name) is what survives insertions: the
    native global prop id is exactly the running index over classes and
    members in schema order (proven on the frozen 630-class schema:
    10145/10145 tags == tools/proptable.tsv), so an insertion shifts
    every id after it.

The generator proves that frozen identity on every run: it parses
run/join97_1818.raw (the 0x97 inner schema), asserts the member-tag
concatenation equals tools/proptable.tsv, maps each override id below the
bound to its (class, member), and asserts every out-of-range override is
the sentinel wire tag (28). Regenerate:
  python3 tools/gen_proptable.py
"""
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TSV = os.path.join(ROOT, "tools", "proptable.tsv")
OVERRIDES = os.path.join(ROOT, "tools", "proptable_overrides.tsv")
SCHEMA = os.path.join(ROOT, "run", "join97_1818.raw")
OUT = os.path.join(ROOT, "client", "src", "solve", "proptable.inc")


def load_tsv(path):
    rows = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            prop, tag = line.split()[:2]
            rows[int(prop)] = int(tag)
    return rows


def load_schema(path):
    """Parse the frozen 0x97 inner prefix -> ([(class, member, tag)], bound).

    Mirrors parseJoinSchema (client/src/solve/repl_snapshot.cpp) through the
    class table; the URL/shared tables that follow are not needed.
    """
    with open(path, "rb") as f:
        d = f.read()
    off = 0

    def varint():
        nonlocal off
        v = 0
        shift = 0
        while True:
            if off >= len(d):
                raise ValueError("truncated varint")
            b = d[off]
            off += 1
            v |= (b & 0x7F) << shift
            shift += 7
            if not (b & 0x80):
                return v
            if shift >= 64:
                raise ValueError("varint too long")

    def take(n):
        nonlocal off
        if off + n > len(d):
            raise ValueError("truncated bytes")
        b = d[off:off + n]
        off += n
        return b

    def string():
        return take(varint()).decode("utf-8")

    varint()  # version
    take(8)   # flags
    for _ in range(varint()):  # enum table
        string()
        assert take(1)[0] == 0x07, "enum trailer"
    nclass = varint()
    prop_bound = varint()
    varint()  # event bound
    members = []
    for _ in range(nclass):
        cls = string()
        for _ in range(varint()):
            name = string()
            tag = take(1)[0]
            if tag == 60:
                varint()
            elif tag == 7:
                take(2)
            members.append((cls, name, tag))
        take(2)  # class flags
        for _ in range(varint()):  # event table
            string()
            take(3 * take(1)[0])
    return members, prop_bound


def cstr(s):
    # Schema names are ASCII identifiers; JSON escapes are C-compatible.
    return json.dumps(s)


def main():
    native = load_tsv(TSV)
    n = max(native) + 1
    assert min(native) == 0 and len(native) == n, "table must be dense 0..N"

    # Ground-truth identity: the native table is the schema member-tag
    # concatenation plus one sentinel entry at prop_bound.
    members, prop_bound = load_schema(SCHEMA)
    assert len(members) == n - 1, (
        f"frozen schema has {len(members)} members, proptable.tsv has {n} "
        f"entries (expected members + sentinel)")
    assert prop_bound == len(members), (
        f"prop_bound {prop_bound} != member count {len(members)}")
    for i, (_, _, tag) in enumerate(members):
        assert native[i] == tag, f"schema/proptable tag mismatch at prop {i}"
    assert native[n - 1] == 28, "native sentinel tag must be 28"

    overrides = {}  # prop -> (wire_tag, prefix, fixed_len, alt_len)
    with open(OVERRIDES) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            p, wt, pfx = parts[:3]
            fl = int(parts[3]) if len(parts) > 3 else 0
            al = int(parts[4]) if len(parts) > 4 else 0
            overrides[int(p)] = (int(wt), int(pfx), fl, al)

    # Name-keyed overrides: every id below the frozen bound maps to its
    # (class, member); ids at/after the bound are sentinel entries from
    # this or later builds (the out-of-band Parent property, tag 28).
    named = []
    sentinel_tag = None
    for p, (wt, pfx, fl, al) in sorted(overrides.items()):
        if p < len(members):
            cls, mem, _ = members[p]
            named.append((cls, mem, wt, pfx, fl, al))
        else:
            assert wt == 25, (
                f"override {p} is past the frozen schema bound but is not "
                f"sentinel wire tag 28 (new member? refresh {SCHEMA.name})")
            assert sentinel_tag in (None, wt)
            sentinel_tag = wt
    if sentinel_tag is None:
        sentinel_tag = 28
    named.sort(key=lambda r: (r[0], r[1]))
    keys = [(c, m) for c, m, *_ in named]
    assert len(set(keys)) == len(keys), "duplicate (class, member) override"

    # Solver-proven props beyond the dumped range (rolling server builds
    # grow prop_bound past the traced table, e.g. 10268): native number
    # unknown -> sentinel 255 (unmapped, fails loudly) unless overridden.
    total = max([n - 1] + list(overrides)) + 1
    tags = []
    prefs = []
    fixed = []
    alta = []
    for i in range(total):
        if i in overrides:
            wt, pfx, fl, al = overrides[i]
        elif i < n:
            wt = native[i]
            pfx = 1 if wt in (33, 61) else 0
            fl, al = 0, 0
        else:
            wt, pfx, fl, al = 255, 0, 0, 0
        assert wt == 255 or (0 <= wt <= 70 and wt not in (4, 5, 45)), f"bad tag {wt}"
        assert fl == 0 or 1 <= fl <= 4096, f"bad fixed_len {fl}"
        assert al == 0 or fl < al <= 4096, f"bad alt_len {al}"
        tags.append(wt)
        prefs.append(pfx)
        fixed.append(fl)
        alta.append(al)

    def blob(vals):
        lines = []
        for i in range(0, len(vals), 24):
            lines.append("    " + ", ".join(str(v) for v in vals[i:i + 24]) + ",")
        return "\n".join(lines)

    def over_blob(rows):
        if not rows:
            return "    {nullptr, nullptr, 0, 0, 0, 0}, // placeholder (count 0)"
        lines = []
        for cls, mem, wt, pfx, fl, al in rows:
            lines.append(
                f"    {{{cstr(cls)}, {cstr(mem)}, {wt}, {pfx}, {fl}, {al}}},")
        return "\n".join(lines)

    body = (
        "// proptable.inc — DO NOT EDIT. Generated by tools/gen_proptable.py\n"
        "// from tools/proptable.tsv (live register dump) +\n"
        "// tools/proptable_overrides.tsv (solver-proven wire shapes) +\n"
        "// run/join97_1818.raw (frozen 0x97 schema, name-keying proof).\n"
        "// tags[]: wire value tag per prop id (index == prop network id);\n"
        "// 255 = beyond the dumped range and not overridden (unmapped).\n"
        "// prefix[]: Content/asset prefix shape flag per prop (0x03 values).\n"
        "// fixedlen[]: opaque fixed value length per prop (0 = use tag\n"
        "// reader). Only for props whose wire shape is a base tag shape\n"
        "// plus fixed trailer bytes, proven by skeleton recurrence +\n"
        "// exact-close (never a bare length guess).\n"
        "// altlen[]: LONG length for two-form props (0 = single form).\n"
        "// The short form (fixedlen) wins iff a valid triple header\n"
        "// starts exactly at +fixedlen (peek); else the long form reads.\n"
        "// The static arrays are a bootstrap/fallback: prop ids are only\n"
        "// stable within one schema. propTagsForSchema() derives the live\n"
        "// table from the 0x97 member order and applies kPropOverrides by\n"
        "// (class, member) name, so insertions cannot shift a wire shape.\n"
        f"static const int kPropTagCount = {total};\n"
        "static const uint8_t kPropTags[] = {\n"
        f"{blob(tags)}\n"
        "};\n"
        "static const uint8_t kPropPrefix[] = {\n"
        f"{blob(prefs)}\n"
        "};\n"
        "static const uint16_t kPropFixedLen[] = {\n"
        f"{blob(fixed)}\n"
        "};\n"
        "static const uint16_t kPropAltLen[] = {\n"
        f"{blob(alta)}\n"
        "};\n"
        "// Name-keyed wire-shape overrides (solver-proven).\n"
        "struct PropTagOverride {\n"
        "    const char* cls;\n"
        "    const char* member;\n"
        "    uint8_t tag;\n"
        "    uint8_t prefix;\n"
        "    uint16_t fixed_len;\n"
        "    uint16_t alt_len;\n"
        "};\n"
        f"static const int kPropOverrideCount = {len(named)};\n"
        "static const PropTagOverride kPropOverrides[] = {\n"
        f"{over_blob(named)}\n"
        "};\n"
        "// Wire tag of the prop_bound sentinel entry: the native table's\n"
        "// out-of-band Parent property, stored and sent as tag 28 (a ref).\n"
        "// Reading it as 25 over-reads one byte (live 2096-spawn capture:\n"
        "// +5 lands on a child spawn whose parent is the triple's entity).\n"
        f"static const uint8_t kPropSentinelTag = {sentinel_tag};\n"
    )
    with open(OUT, "w") as f:
        f.write(body)
    print(f"wrote {OUT}: {total} props ({n} native + {total - n} extended), "
          f"{len(overrides)} overrides ({len(named)} name-keyed, sentinel "
          f"tag {sentinel_tag})")


if __name__ == "__main__":
    main()
