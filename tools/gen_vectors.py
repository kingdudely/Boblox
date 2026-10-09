#!/usr/bin/env python3
"""Generate golden per-stage vectors under tests/vectors/.

Each vector set locks one stage of the 0x9B pipeline (see FINDINGS.md):

  msg    --extract-->    u1, u2, blob     [9b][u1][u2][len][blob]
  blob   --decode-->     wire program     XOR + xxhash32(seed=0x2A) + zstd
  wire   --standardize--> standard prog   internal opcode walk
  msg+job --solve-->     u32 answer       (recorded from the NATIVE client)

Sources are the tracked capture fixtures under run/ (the same set the solver
regressions use); the derived artifacts live in tests/vectors/ and are
consumed by BOTH:

  python3 tools/gen_vectors.py --check          (ctest `vectors-py`)
  tests/unit/unit_vectors.cpp                   (ctest `unit-vectors`)

so the Python reference and the C++ pipeline must agree with each other AND
with the frozen goldens.

Also emits tests/vectors/msgs/*.bin — golden session messages built by the
Python reference builders (probe5 / probe7 / rbx_client), byte-compared
against the C++ builders by tests/unit/unit_messages.cpp.

Usage:
  python3 tools/gen_vectors.py           # (re)write tests/vectors/
  python3 tools/gen_vectors.py --check   # verify on-disk == derived
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "py"))

from challenge_blob import decode_blob, extract_blob, standardize_bytes  # noqa: E402
# Functional core (stdlib-only): builders without the probes' aioquic imports.
from msgbuild import build_8a, build_90, early_auth_payload  # noqa: E402

VEC = os.path.join(ROOT, "tests", "vectors")

# mirror kDatasets in client/test/regress_main.cpp
DATASETS = [
    ("comb1", "run/comb_1"),
    ("comb2", "run/comb_2"),
    ("comb3", "run/comb_3"),
    ("full", "run/full"),
    ("rounds_r1", "run/rounds/r1"),
]
# extra: the live-captured oracle message (stage-locked, no native answer)
DATASETS.append(("oracle", "run/oracle_challenge.bin"))


def read(p):
    with open(p, "rb") as f:
        return f.read()


def read_text(p):
    return read(p).decode("utf-8", "replace").strip()


def first_match(d, prefix, suffix):
    """First file in dir matching prefix+suffix (mirrors regress_main.cpp)."""
    if not os.path.isdir(d):
        return None
    names = sorted(os.listdir(d))
    for n in names:
        if n.startswith(prefix) and n.endswith(suffix) and len(n) >= len(suffix):
            return os.path.join(d, n)
    return None


def derive_chal(name, rel, out):
    """msg -> (u1, u2, blob) -> wire -> standard; plus native answer/job."""
    if rel.endswith(".bin"):  # oracle: a bare tracked message file
        chal = os.path.join(ROOT, rel)
        src_rel = rel
    else:
        chal = first_match(os.path.join(ROOT, rel), "wire_chal", ".bin")
        src_rel = os.path.relpath(chal, ROOT) if chal else None
    if not chal or not os.path.exists(chal):
        raise SystemExit(f"missing capture: {rel}")

    msg = read(chal)
    u1, u2, blob = extract_blob(msg)  # raises unless 0x9B-framed
    wire = decode_blob(blob)          # raises on checksum mismatch
    std, stats = standardize_bytes(wire)
    if stats["bad_walks"]:
        raise SystemExit(f"{name}: proto walk failed: {stats['bad_walks']}")

    # native answer: [9b][u32][u32 answer-le] — same parse as regress_main.cpp
    ans_p = first_match(os.path.join(ROOT, rel), "wire_ans", ".bin") if not rel.endswith(".bin") else None
    answer = None
    if ans_p:
        raw = read(ans_p)
        if len(raw) >= 9:
            answer = int.from_bytes(raw[5:9], "little")

    job_p = os.path.join(ROOT, rel, "jobid.txt") if not rel.endswith(".bin") else None
    job = read_text(job_p) if job_p and os.path.exists(job_p) else None

    meta = {
        "name": name,
        "source": src_rel,
        "u1": u1,
        "u2": u2,
        "job": job,
        "answer": answer,
        "msg_len": len(msg),
        "wire_len": len(wire),
        "standard_len": len(std),
    }
    out[f"tests/vectors/{name}.json"] = (
        json.dumps(meta, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")
    out[f"tests/vectors/{name}.wire.bin"] = wire
    out[f"tests/vectors/{name}.standard.bin"] = std
    return meta


def derive_msgs(out):
    """Golden session messages from the Python reference builders."""
    j = json.load(open(os.path.join(ROOT, "run", "join_full.json")))
    js = j["joinScript"]
    reply = j.get("reply") or {}
    ct = js.get("ClientTicket", "") or ""
    version = int(ct.split(";")[-1]) if ct else 0

    out["tests/vectors/msgs/earlyauth.bin"] = early_auth_payload(ct, version)
    out["tests/vectors/msgs/m8a.bin"] = build_8a(js)
    if isinstance(reply, dict) and reply.get("joinTicket"):
        out["tests/vectors/msgs/m90.bin"] = build_90(js, reply)


def derive_all():
    out = {}
    summaries = []
    for name, rel in DATASETS:
        summaries.append(derive_chal(name, rel, out))
    derive_msgs(out)
    return out, summaries


def write_all(files):
    for rel, data in files.items():
        p = os.path.join(ROOT, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)


def check_all(files):
    ok = True
    for rel, want in sorted(files.items()):
        p = os.path.join(ROOT, rel)
        if not os.path.exists(p):
            print(f"  MISSING {rel}")
            ok = False
            continue
        got = read(p)
        if got != want:
            print(f"  DIFF {rel}: on-disk {len(got)}B != derived {len(want)}B")
            ok = False
    # stale managed files (e.g. renamed dataset) must not linger
    known = set(files)
    for base, _, names in os.walk(VEC):
        for n in names:
            rel = os.path.relpath(os.path.join(base, n), ROOT)
            if rel not in known and (n.endswith(".json") or n.endswith(".bin")):
                print(f"  STALE {rel}")
                ok = False
    return ok


def main():
    check = "--check" in sys.argv
    files, summaries = derive_all()
    for m in summaries:
        ans = f"0x{m['answer']:08x}" if m["answer"] is not None else "none"
        print(f"  {m['name']:<9} u1=0x{m['u1']:08x} u2=0x{m['u2']:08x} "
              f"answer={ans} wire={m['wire_len']}B std={m['standard_len']}B")
    if check:
        if check_all(files):
            print(f"vectors OK: {len(files)} files match derived values")
            return 0
        print("vectors OUT OF SYNC — run: python3 tools/gen_vectors.py")
        return 1
    write_all(files)
    print(f"wrote {len(files)} files under tests/vectors/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
