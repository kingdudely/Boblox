#!/usr/bin/env python3
"""parity_audit.py — build the RakNet / Luau parity ledger.

    python3 tools/parity_audit.py --capture run/parity/seq120_clean \
        --luau-audit run/api_audit/*.json --out docs/PARITY.md

The RakNet side reads plaintext datagram dumps (native `dgram_*.bin`, ours
`t*.bin`) with tools/app_stream.py, then reports every application id and every
0x83 subtype with counts, per-second rates, sizes and a stable-head shape.  The
manifest comes from `roblox --packet-manifest` (or a saved copy via --manifest)
so unobserved ids keep their declared direction and layout.

The Luau side merges the JSON inventories written by RBX_API_AUDIT (see
rbx_runtime/src/instance/api_audit.h) and ranks missing globals, instance
members and called stubs by actual script usage.  Member kinds come from the
pinned API dump, not from name casing.

Nothing here needs a live session; it is a pure reader over captures and audit
files so the ledger can be regenerated after every parity round.
"""
import argparse
import datetime
import glob
import json
import os
import re
import statistics
import subprocess
import sys
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import app_stream  # noqa: E402


class Stats:
    def __init__(self):
        self.lens = []
        self.heads = Counter()

    def add(self, msg):
        self.lens.append(len(msg))
        self.heads[bytes(msg[:12]).hex()] += 1

    @property
    def count(self):
        return len(self.lens)

    def summary(self):
        if not self.lens:
            return "—"
        p50 = int(statistics.median(self.lens))
        head = self.heads.most_common(1)[0][0] if self.heads else ""
        return f"{min(self.lens)}–{max(self.lens)}B p50={p50} head={head[:24]}"


class Side:
    def __init__(self, path):
        self.path = path
        self.files = app_stream.files_in(path) if os.path.isdir(path) else []
        self.span = 1.0
        if self.files:
            times = []
            for f in self.files:
                try:
                    times.append(os.path.getmtime(f))
                except OSError:
                    pass
            if len(times) >= 2 and max(times) > min(times):
                self.span = max(times) - min(times)
        self.messages = app_stream.stream(self.files)
        self.by_id = defaultdict(Stats)
        self.by_sub = defaultdict(Stats)
        self.bytes = 0
        for msg in self.messages:
            if not msg:
                continue
            self.bytes += len(msg)
            self.by_id[msg[0]].add(msg)
            if msg[0] == 0x83 and len(msg) > 1:
                self.by_sub[msg[1]].add(msg)

    @property
    def count(self):
        return len(self.messages)

    def rate(self, stats):
        return stats.count / self.span if stats.count and self.span else 0.0


def parse_manifest(args):
    if args.manifest:
        try:
            text = open(args.manifest, "r", encoding="utf-8").read()
        except OSError as e:
            print(f"parity_audit: cannot read manifest: {e}", file=sys.stderr)
            return []
    else:
        client = args.client
        if not os.path.isabs(client):
            client = os.path.join(ROOT, client)
        if not os.path.exists(client):
            print(f"parity_audit: manifest client not found: {client}", file=sys.stderr)
            return []
        try:
            p = subprocess.run([client, "--packet-manifest"], cwd=ROOT,
                               capture_output=True, text=True, timeout=30)
        except (OSError, subprocess.SubprocessError) as e:
            print(f"parity_audit: cannot run packet manifest: {e}", file=sys.stderr)
            return []
        text = (p.stdout or "") + (p.stderr or "")
    out = []
    pat = re.compile(r"^(0x[0-9a-fA-F]+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s*\|\s*(.*)$")
    for line in text.splitlines():
        m = pat.match(line.strip())
        if not m:
            continue
        out.append({
            "id": int(m.group(1), 16),
            "direction": m.group(2),
            "recv": m.group(3),
            "send": m.group(4),
            "name": m.group(5),
            "layout": m.group(6).strip(),
        })
    return out


DATATYPE_NAMES = {
    "Axes", "BrickColor", "CFrame", "CatalogSearchParams", "Color3",
    "ColorSequence", "ColorSequenceKeypoint", "Content", "DateTime", "DockWidgetPluginGuiInfo",
    "Enum", "EnumItem", "Faces", "Font", "Instance", "NumberRange",
    "NumberSequence", "NumberSequenceKeypoint", "OverlapParams", "PathWaypoint",
    "PhysicalProperties", "Random", "Ray", "RaycastParams", "Rect", "Region3",
    "Region3int16", "TweenInfo", "UDim", "UDim2", "Vector2", "Vector2int16",
    "Vector3", "Vector3int16",
}


def load_api_dump(path):
    try:
        doc = json.load(open(path, "r", encoding="utf-8"))
    except (OSError, ValueError) as e:
        print(f"parity_audit: cannot read API dump: {e}", file=sys.stderr)
        return {}, set(DATATYPE_NAMES)
    classes = {}
    for c in doc.get("Classes", []):
        classes[c.get("Name", "")] = c
    datatypes = set(DATATYPE_NAMES)
    for c in classes.values():
        for m in c.get("Members", []):
            for key in ("ReturnType", "ValueType"):
                t = m.get(key)
                if isinstance(t, dict) and t.get("Category") == "DataType":
                    datatypes.add(t.get("Name", ""))
            for p in m.get("Parameters", []) or []:
                t = p.get("Type")
                if isinstance(t, dict) and t.get("Category") == "DataType":
                    datatypes.add(t.get("Name", ""))
    datatypes.discard("")
    return classes, datatypes


def member_kind(classes, cls, member):
    c = classes.get(cls)
    seen = set()
    while c and c.get("Name") not in seen:
        seen.add(c.get("Name"))
        for m in c.get("Members", []):
            if m.get("Name") == member:
                mt = m.get("MemberType", "")
                return {
                    "Function": "method",
                    "Event": "event",
                    "Property": "property",
                    "Callback": "callback",
                }.get(mt, mt.lower() or "member")
        c = classes.get(c.get("Superclass", ""))
    return "unknown"


def load_audits(paths):
    merged = {
        "missing_globals": Counter(),
        "missing_members": Counter(),
        "stub_calls": Counter(),
        "fallback_calls": Counter(),
        "event_connects": Counter(),
        "event_fires": Counter(),
    }
    used = []
    for path in paths:
        try:
            doc = json.load(open(path, "r", encoding="utf-8"))
        except (OSError, ValueError) as e:
            print(f"parity_audit: skipping audit {path}: {e}", file=sys.stderr)
            continue
        used.append(path)
        for section in merged:
            values = doc.get(section, {})
            if isinstance(values, dict):
                for k, v in values.items():
                    try:
                        merged[section][k] += int(v)
                    except (TypeError, ValueError):
                        pass
    return merged, used


def compact_path(path):
    try:
        rel = os.path.relpath(path, ROOT)
        return path if rel.startswith("..") else rel
    except ValueError:
        return path


def fmt_rate(x):
    return f"{x:,.2f}"


def status_for(n, o):
    if n and o:
        return "both"
    if n:
        return "native-only"
    if o:
        return "ours-only"
    return "unobserved"


def verdict(nrate, orate, ncount=0, ocount=0):
    if not nrate and not orate:
        return "unobserved"
    if not nrate:
        return "ours only"
    if not orate:
        return "missing"
    # Below 1/s the span correction is noisier than the counts themselves;
    # compare the raw totals with a small absolute floor (handshake IDs are
    # bursty and a 48-vs-2 difference is real, not sampling noise).
    if max(nrate, orate) < 1.0:
        hi = max(ncount, ocount)
        return "match" if abs(ncount - ocount) <= max(2, hi * 0.20) else "differs"
    hi = max(nrate, orate)
    return "match" if abs(nrate - orate) <= max(0.5, hi * 0.20) else "differs"


def app_rows(sides, manifest):
    ids = set()
    for side in sides:
        ids.update(side.by_id)
    ids.update(m["id"] for m in manifest)
    mby = {m["id"]: m for m in manifest}
    rows = []
    for aid in sorted(ids):
        n = sum((s.by_id.get(aid, Stats()).count for s in sides if s.label == "native"), 0)
        o = sum((s.by_id.get(aid, Stats()).count for s in sides if s.label == "ours"), 0)
        nr = sum((s.rate(s.by_id.get(aid, Stats())) for s in sides if s.label == "native"), 0.0)
        orate = sum((s.rate(s.by_id.get(aid, Stats())) for s in sides if s.label == "ours"), 0.0)
        shape_n = next((s.by_id[aid].summary() for s in sides
                        if s.label == "native" and aid in s.by_id), "—")
        shape_o = next((s.by_id[aid].summary() for s in sides
                        if s.label == "ours" and aid in s.by_id), "—")
        m = mby.get(aid, {})
        rows.append({
            "id": aid,
            "name": m.get("name", "(unknown)"),
            "direction": m.get("direction", "?"),
            "recv": m.get("recv", "?"),
            "send": m.get("send", "?"),
            "native": n,
            "ours": o,
            "native_rate": nr,
            "ours_rate": orate,
            "shape_native": shape_n,
            "shape_ours": shape_o,
            "status": status_for(n, o),
            "verdict": verdict(nr, orate, n, o),
        })
    return rows


def sub_rows(sides):
    subs = set()
    for s in sides:
        subs.update(s.by_sub)
    rows = []
    for sub in sorted(subs):
        n = sum((s.by_sub.get(sub, Stats()).count for s in sides if s.label == "native"), 0)
        o = sum((s.by_sub.get(sub, Stats()).count for s in sides if s.label == "ours"), 0)
        nr = sum((s.rate(s.by_sub.get(sub, Stats())) for s in sides if s.label == "native"), 0.0)
        orate = sum((s.rate(s.by_sub.get(sub, Stats())) for s in sides if s.label == "ours"), 0.0)
        shape_n = next((s.by_sub[sub].summary() for s in sides
                        if s.label == "native" and sub in s.by_sub), "—")
        shape_o = next((s.by_sub[sub].summary() for s in sides
                        if s.label == "ours" and sub in s.by_sub), "—")
        rows.append({
            "sub": sub, "native": n, "ours": o,
            "native_rate": nr, "ours_rate": orate,
            "shape_native": shape_n, "shape_ours": shape_o,
            "status": status_for(n, o),             "verdict": verdict(nr, orate, n, o),
        })
    return rows


def table(header, rows):
    out = ["| " + " | ".join(header) + " |",
           "|" + "|".join("---" for _ in header) + "|"]
    out.extend("| " + " | ".join(str(c) for c in row) + " |" for row in rows)
    return "\n".join(out)


def markdown(captures, sides, manifest, app, subs, audits, audit_files,
             api_classes, datatypes, top):
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    lines = [
        "# Parity ledger",
        "",
        f"Generated by `tools/parity_audit.py` at {now}.  This is a ledger, not a",
        "claim of completeness: an unobserved id means no capture has exercised it,",
        "not that the client handles it.",
        "",
        "## Current slice",
        "",
        "- Closed: `0x83/0a` now uses the native record grammar (optional value byte,",
        "  u32 clock, optional `0x23` tick, required chain terminator).  Three",
        "  byte-exact native frames are pinned in `tests/unit/unit_batch.cpp`.",
        "- Post-fix shape: the 21 B one-record-plus-tick frame is the default;",
        "  the 34 B same-ref has-value=0 pair is added every fourth frame.",
        "  Live 1818 check: 86 x 21 B and 28 x 34 B frames, 114 total over",
        "  40 s (2.85/s), with the session still replicating at exit.",
        "- Clock sync: `0x51`-`0x53` (9 B u24 session-ms) and `0x54`-`0x56`",
        "  (25 B system/session-ms + nonce) are emitted every 5 s after the",
        "  handshake.  A 75 s live 1818 TX dump captured 16 each of `0x51`-`0x53`",
        "  (two pre-join plus 14 periodic) and 14 each of `0x54`-`0x56`, with the",
        "  world still replicating and no disconnect; a 35 s dump showed 8/6 and",
        "  the 370731277 receive census saw the server echo 2/2/2 and 6/6/6.",
        "  Byte-exact builders are pinned in `tests/unit/unit_batch.cpp`.",
        "- Luau implemented from the host: tags, Model pivot/scale/translate,",
        "  BasePart mass/overlap/query helpers, Camera projection/rays, Player",
        "  GetMouse fed by real SDL pointer state, UserInputService key/mouse",
        "  queries, Humanoid state/move/accessories, TweenService-backed legacy",
        "  GUI tweens, and TeleportService local state.  Live 1818 headful",
        "  verification moved the X pointer and saw `Mouse.X/Y` change; the",
        "  audit recorded `Mouse.Move`.",
        "- Sound is host-backed for WAV through SDL3 and authenticated asset",
        "  delivery.  WAV playback produced `Playing` then `Completed`; the",
        "  observed built-in ping is OGG and is explicitly diagnosed as",
        "  unsupported compressed audio (no false `IsPlaying`).",
        "- Compatibility fallbacks remain for group/friend web data and teleport",
        "  actions.  They return documented no-relationship/no-teleport values,",
        "  are counted under `fallback_calls`, and raise under",
        "  `RBX_STRICT_API=1`.  Generated method defaults are counted under",
        "  `stub_calls`; strict mode raises for those too.",
        "- Still open: 3,433 of the 3,661 generated methods fall back to",
        "  type-correct defaults rather than native semantics.  Compatibility mode",
        "  keeps scripts alive; `RBX_STRICT_API=1` raises for them instead.",
        "  Their server/plugin/async behavior is not implemented.",
        "- Open: `0x83/0e` (client-owned instance replication) needs a local instance",
        "  registry before it can be emitted without fabricating refs; `0x83/07` and",
        "  `0x83/1c` are the next observed containers.",
        "- The Luau section is usage-ranked; an empty table means the captured",
        "  LocalScripts did not touch that gap, not that the API is complete.",
        "- The comparison tables below are the pre-fix `seq120_clean` baseline",
        "  unless a newer `--capture` is passed; they predate the clock burst, so",
        "  the ours column still shows the old `0x51`-`0x56` counts.  The live",
        "  check above is the current behavior.",
        "",
        "## Captures",
        "",
    ]
    cap_rows = []
    for c in captures:
        for side in [s for s in sides if s.capture == c]:
            cap_rows.append([
                f"`{compact_path(c)}`", side.label,
                f"{len(side.files):,}", f"{side.count:,}",
                f"{side.span:.1f}s", fmt_rate(side.count / side.span),
                f"{side.bytes / side.span:,.0f}",
            ])
    lines.append(table(
        ["capture", "side", "datagrams", "app msgs", "span", "msgs/s", "bytes/s"],
        cap_rows))

    lines += ["", "## Application IDs", "",
              "Shapes are `min–maxB p50=<bytes> head=<first 12 bytes>`; the head is",
              "session-specific for id-bearing frames, so compare sizes and framing,",
              "not literal bytes.  Rates are over each side's own capture span.", ""]
    rows = []
    for r in app:
        rows.append([
            f"`0x{r['id']:02x}`", r.get("name", "—"), r["direction"],
            f"{r['recv']}/{r['send']}",
            f"{r['native']:,}", f"{r['ours']:,}",
            fmt_rate(r["native_rate"]), fmt_rate(r["ours_rate"]),
            r["status"], r["verdict"],
            f"`{r['shape_native']}`", f"`{r['shape_ours']}`",
        ])
    lines.append(table(
        ["id", "manifest name", "dir", "recv/send", "native", "ours",
         "native/s", "ours/s", "presence", "verdict", "native shape",
         "ours shape"], rows))

    lines += ["", "## 0x83 subtypes", "",
              "The first item byte labels a container; rates are frames/s, not",
              "records/s.  `0x0a` is therefore a lower bound on ack traffic when",
              "several acks ride in one frame.", ""]
    rows = []
    for r in subs:
        rows.append([
            f"`0x{r['sub']:02x}`", f"{r['native']:,}", f"{r['ours']:,}",
            fmt_rate(r["native_rate"]), fmt_rate(r["ours_rate"]),
            r["status"], r["verdict"],
            f"`{r['shape_native']}`", f"`{r['shape_ours']}`",
        ])
    lines.append(table(
        ["subtype", "native", "ours", "native/s", "ours/s", "presence", "verdict",
         "native shape", "ours shape"], rows))

    unknown = [r for r in app if r["name"] == "(unknown)"]
    unobserved = [r for r in app if r["status"] == "unobserved"]
    lines += ["", "## Manifest coverage", ""]
    if unknown:
        lines.append("- Observed without a manifest entry: " +
                     ", ".join(f"`0x{r['id']:02x}`" for r in unknown))
    if unobserved:
        lines.append("- Declared but not observed in these captures: " +
                     ", ".join(f"`0x{r['id']:02x}` ({r['direction']})" for r in unobserved))
    if not unknown and not unobserved:
        lines.append("- Every manifest id was observed and every observed id is declared.")

    lines += ["", "## Luau API gaps", ""]
    if not audit_files:
        lines += ["No `RBX_API_AUDIT` inventory was supplied, so this section is empty.",
                  "Run a script-mode session with `RBX_API_AUDIT=run/api_audit/<name>.json`",
                  "and pass it with `--luau-audit`.", ""]
    else:
        lines += [f"Source inventories: {', '.join('`' + compact_path(p) + '`' for p in audit_files)}",
                  ""]
        missing_globals = audits["missing_globals"]
        globals_rows = []
        for name, count in sorted(missing_globals.items(), key=lambda kv: (-kv[1], kv[0]))[:top]:
            kind = "datatype" if name in datatypes else "global"
            globals_rows.append([f"`{name}`", f"{count:,}", kind])
        lines += ["### Missing globals (actual reads)", ""]
        lines.append(table(["name", "reads", "kind"], globals_rows) if globals_rows
                     else "No missing globals were recorded.")

        member_rows = []
        for key, count in sorted(audits["missing_members"].items(),
                                 key=lambda kv: (-kv[1], kv[0]))[:top]:
            cls, _, member = key.partition(".")
            member_rows.append([f"`{key}`", f"{count:,}",
                                member_kind(api_classes, cls, member)])
        lines += ["", "### Missing instance members", "",
                  "These raised `not a valid member`; the kind comes from the API dump.",
                  ""]
        lines.append(table(["member", "reads", "kind"], member_rows) if member_rows
                     else "No missing instance members were recorded.")

        stub_rows = []
        for key, count in sorted(audits["stub_calls"].items(),
                                 key=lambda kv: (-kv[1], kv[0]))[:top]:
            cls, _, member = key.partition(".")
            stub_rows.append([f"`{key}`", f"{count:,}",
                              member_kind(api_classes, cls, member)])
        lines += ["", "### Unimplemented stubs called", "",
                  "A generated method exists but returns only a type default.", ""]
        lines.append(table(["method", "calls", "kind"], stub_rows) if stub_rows
                     else "No generated stub was called.")

        fallback_rows = []
        for key, count in sorted(audits["fallback_calls"].items(),
                                 key=lambda kv: (-kv[1], kv[0]))[:top]:
            cls, _, member = key.partition(".")
            fallback_rows.append([f"`{key}`", f"{count:,}",
                                  member_kind(api_classes, cls, member)])
        lines += ["", "### Compatibility fallbacks called", "",
                  "A subsystem is absent (group/friend web data, teleport backend); "
                  "these return documented no-op values in compatibility mode and "
                  "raise under `RBX_STRICT_API=1`.", ""]
        lines.append(table(["method", "calls", "kind"], fallback_rows)
                     if fallback_rows else "No compatibility fallback was called.")

        silent = []
        connects = audits["event_connects"]
        fires = audits["event_fires"]
        for name, count in connects.items():
            if fires.get(name, 0) != 0:
                continue
            # Signal names are class-qualified.  A property callback
            # (Workspace.CurrentCamera) is not an event and would be a false
            # positive in this table; drop it when the dump says so.
            if "." in name:
                cls, member = name.split(".", 1)
                if member_kind(api_classes, cls, member) == "property":
                    continue
            silent.append([f"`{name}`", f"{count:,}", "0"])
        silent.sort(key=lambda r: (-int(r[1].replace(",", "")), r[0]))
        lines += ["", "### Events connected but never fired", ""]
        lines.append(table(["event", "connects", "fires"], silent[:top]) if silent
                     else "Every connected event fired at least once.")

        dt_rows = []
        for name, count in sorted(missing_globals.items(), key=lambda kv: (-kv[1], kv[0])):
            if name in datatypes:
                dt_rows.append([f"`{name}`", f"{count:,}"])
            if len(dt_rows) >= top:
                break
        lines += ["", "### Datatype globals missing", ""]
        lines.append(table(["datatype", "reads"], dt_rows) if dt_rows
                     else "No datatype global was missing.")

    lines += ["", "## Rebuild", "", "```",
              "python3 tools/parity_audit.py --capture <capture-dir> \\",
              "  --luau-audit run/api_audit/*.json --out docs/PARITY.md",
              "```", ""]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--capture", action="append", default=[],
                    help="capture root containing native/ and ours/ (repeatable)")
    ap.add_argument("--manifest", help="saved --packet-manifest output")
    ap.add_argument("--client", default="build/client/roblox",
                    help="client binary used for --packet-manifest")
    ap.add_argument("--api-dump", default=os.path.join(ROOT, "tools/api_dump.json"))
    ap.add_argument("--luau-audit", nargs="*", default=[],
                    help="RBX_API_AUDIT JSON files")
    ap.add_argument("--out", default=os.path.join(ROOT, "docs/PARITY.md"))
    ap.add_argument("--json-out", help="optional machine-readable ledger")
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()

    captures = args.capture
    if not captures:
        preferred = os.path.join(ROOT, "run/parity/seq120_clean")
        if os.path.isdir(preferred):
            captures = [preferred]
        else:
            root = os.path.join(ROOT, "run/parity")
            candidates = []
            if os.path.isdir(root):
                for name in os.listdir(root):
                    p = os.path.join(root, name)
                    if os.path.isdir(os.path.join(p, "native")) and os.path.isdir(os.path.join(p, "ours")):
                        candidates.append(p)
            captures = [max(candidates, key=os.path.getmtime)] if candidates else []
    if not captures:
        print("parity_audit: no capture roots found; pass --capture", file=sys.stderr)
        return 2

    sides = []
    for cap in captures:
        for label in ("native", "ours"):
            side = Side(os.path.join(cap, label))
            side.label = label
            side.capture = cap
            sides.append(side)

    manifest = parse_manifest(args)
    app = app_rows(sides, manifest)
    subs = sub_rows(sides)
    audits, audit_files = load_audits(args.luau_audit)
    api_classes, datatypes = load_api_dump(args.api_dump)
    text = markdown(captures, sides, manifest, app, subs, audits, audit_files,
                    api_classes, datatypes, args.top)

    out = args.out if os.path.isabs(args.out) else os.path.join(ROOT, args.out)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        f.write(text + "\n")
    print(f"wrote {compact_path(out)}")

    if args.json_out:
        jout = args.json_out if os.path.isabs(args.json_out) else os.path.join(ROOT, args.json_out)
        doc = {
            "generated_at": datetime.datetime.now().isoformat(timespec="seconds"),
            "captures": captures,
            "application_ids": [{k: v for k, v in r.items() if k != "id"} | {"id": f"0x{r['id']:02x}"} for r in app],
            "subtypes": [{k: v for k, v in r.items() if k != "sub"} | {"sub": f"0x{r['sub']:02x}"} for r in subs],
            "luau": {k: dict(v) for k, v in audits.items()},
        }
        with open(jout, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1, sort_keys=True)
        print(f"wrote {compact_path(jout)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
