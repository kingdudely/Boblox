#!/usr/bin/env python3
"""fleet_join.py — run N join-only clients from distinct cookie files.

One account per session is mandatory: Roblox rejects two clients on the same
account, so the runner requires one cookie file per account and de-duplicates
files with identical contents (a backup copy of the same cookie must not look
like a second account). Cookie contents are hashed only for that check and are
never printed, logged, or written into the summary.

Each child is launched with `--mode=join --pump-ms N --seconds S`, and its
wall time, CPU time, peak RSS and join outcome are recorded. CPU comes from
wait4(2) rusage (exact, not sampled); RSS is the maximum of VmHWM samples and
ru_maxrss.

Usage:
  python3 tools/fleet_join.py --cookie-dir run/accounts --count 10 \
      --place-id 1818 --seconds 120

The cookie directory must contain one `*.txt` per account. The summary is
written to `<out>/summary.json`; per-session logs are `<out>/session_NN.log`.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import signal
import statistics
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STOP = False


def _on_signal(signum, frame):  # noqa: ARG001
    global STOP
    STOP = True


def unique_cookie_files(directory: Path) -> list[Path]:
    """Return one file per distinct cookie value, in stable order."""
    if not directory.is_dir():
        return []
    seen: set[bytes] = set()
    out: list[Path] = []
    for path in sorted(directory.glob("*.txt")):
        try:
            data = path.read_bytes().strip()
        except OSError:
            continue
        if len(data) < 64:
            continue
        digest = hashlib.sha256(data).digest()
        if digest in seen:
            continue
        seen.add(digest)
        out.append(path)
    return out


def _proc_cpu_ticks(pid: int) -> int | None:
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return None
    # The comm field is parenthesised and may contain spaces; fields after the
    # final ')' start at state (field 3). utime/stime are fields 14/15.
    rest = stat.rsplit(")", 1)[-1].split()
    if len(rest) < 13:
        return None
    try:
        return int(rest[11]) + int(rest[12])
    except ValueError:
        return None


def _proc_hwm_kb(pid: int) -> int | None:
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmHWM:"):
                return int(line.split()[1])
    except (OSError, ValueError, IndexError):
        return None
    return None


def _proc_pss_kb(pid: int) -> int | None:
    """PSS counts shared pages once per process; RSS counts them in full."""
    try:
        for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines():
            if line.startswith("Pss:"):
                return int(line.split()[1])
    except (OSError, ValueError, IndexError):
        return None
    return None


def _spawn(binary: str, argv: list[str], log_path: Path, env: dict[str, str]) -> int:
    """fork/exec so the parent can use wait4(2) for exact rusage."""
    pid = os.fork()
    if pid == 0:  # child
        try:
            os.setsid()
            fd = os.open(log_path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            os.dup2(fd, 1)
            os.dup2(fd, 2)
            devnull = os.open(os.devnull, os.O_RDONLY)
            os.dup2(devnull, 0)
            os.chdir(ROOT)
            os.execvpe(binary, argv, env)
        except BaseException:
            pass
        os._exit(127)
    return pid


def _classify(text: str, exit_code: int) -> dict:
    return {
        "joined": exit_code == 0
        and "reply2=verified" in text
        and "data=armed" in text,
        "disconnected": "raknet: 0x15" in text,
        "join_error": "joinGame:" in text,
        "spawn_retry": "fresh-session retry" in text,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cookie-dir", default="run/accounts",
                    help="directory with one *.txt cookie per account")
    ap.add_argument("--count", type=int, default=10,
                    help="concurrent sessions (distinct accounts)")
    ap.add_argument("--seconds", type=int, default=120,
                    help="run window per session")
    ap.add_argument("--place-id", type=int, default=1818)
    ap.add_argument("--stagger", type=float, default=2.0,
                    help="seconds between launches (avoid a join burst)")
    # 4 ms measured 2.04% of one core against 2.24% at 2 ms for the same
    # join-only soak, with no disconnect; fleet runs favor the lower CPU.
    ap.add_argument("--pump-ms", type=int, default=4)
    ap.add_argument("--sample-ms", type=int, default=1000,
                    help="RSS sampling period; CPU comes from rusage")
    ap.add_argument("--binary", default="build/client/roblox")
    ap.add_argument("--out", default="run/fleet")
    args = ap.parse_args()

    if args.count < 1:
        print("fleet: --count must be >= 1", file=sys.stderr)
        return 2
    if args.seconds < 1:
        print("fleet: --seconds must be >= 1", file=sys.stderr)
        return 2

    cookie_dir = (ROOT / args.cookie_dir).resolve()
    cookies = unique_cookie_files(cookie_dir)
    if len(cookies) < args.count:
        print(
            f"fleet: need {args.count} distinct cookie files, found "
            f"{len(cookies)} in {args.cookie_dir}",
            file=sys.stderr,
        )
        print(
            "fleet: put one `*.txt` per account in that directory "
            "(never reuse an account across files)",
            file=sys.stderr,
        )
        return 2

    binary = args.binary
    if not os.path.isabs(binary):
        binary = str((ROOT / binary).resolve())
    if not os.access(binary, os.X_OK):
        print(f"fleet: {args.binary} is not executable", file=sys.stderr)
        return 2

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out = (ROOT / args.out / stamp).resolve()
    out.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    # The cookie file is the identity. Ambient auth/feature config must not
    # silently redirect a fleet member to another account or profile.
    for key in list(env):
        if key.startswith("BOBLOX_") or key in {
            "RBX_ROBLOSECURITY",
            "ROBLOX_ROBLOSECURITY",
            "RBX_SPAWN_RETRY",
            "RBX_SPAWN_RETRY_AFTER",
        }:
            env.pop(key, None)

    signal.signal(signal.SIGINT, _on_signal)
    signal.signal(signal.SIGTERM, _on_signal)

    sessions: list[dict] = []
    started = time.monotonic()
    tick_hz = os.sysconf("SC_CLK_TCK")

    for index, cookie in enumerate(cookies[: args.count]):
        if STOP:
            break
        log_path = out / f"session_{index:02d}.log"
        argv = [
            binary,
            "--connection-type=raknet",
            "--mode=join",
            "--place-id", str(args.place_id),
            "--cookie-file", str(cookie),
            "--seconds", str(args.seconds),
            "--pump-ms", str(args.pump_ms),
        ]
        launch = time.monotonic()
        pid = _spawn(binary, argv, log_path, env)
        sessions.append({
            "index": index,
            "pid": pid,
            "log": log_path.name,
            "launch": launch,
            "ticks": 0,
            "peak_kb": 0,
            "peak_pss_kb": 0,
            "exit": None,
            "cpu_s": None,
            "wall_s": None,
            "rusage_kb": None,
        })
        print(f"fleet: started {index + 1}/{args.count}", flush=True)
        if index + 1 < args.count:
            until = time.monotonic() + args.stagger
            while time.monotonic() < until and not STOP:
                time.sleep(min(0.1, until - time.monotonic()))

    sample_s = max(0.1, args.sample_ms / 1000.0)
    while not STOP:
        running = [s for s in sessions if s["exit"] is None]
        if not running:
            break
        for s in running:
            pid = s["pid"]
            ticks = _proc_cpu_ticks(pid)
            if ticks is not None:
                s["ticks"] = ticks
            hwm = _proc_hwm_kb(pid)
            if hwm is not None:
                s["peak_kb"] = max(s["peak_kb"], hwm)
            pss = _proc_pss_kb(pid)
            if pss is not None:
                s["peak_pss_kb"] = max(s["peak_pss_kb"], pss)
            try:
                waited, status, usage = os.wait4(pid, os.WNOHANG)
            except ChildProcessError:
                waited, status, usage = pid, 0, None
            if waited == pid:
                s["exit"] = (
                    os.WEXITSTATUS(status) if os.WIFEXITED(status) else 128 + os.WTERMSIG(status)
                )
                s["wall_s"] = time.monotonic() - s["launch"]
                if usage is not None:
                    s["cpu_s"] = usage.ru_utime + usage.ru_stime
                    s["rusage_kb"] = usage.ru_maxrss
                else:
                    s["cpu_s"] = s["ticks"] / tick_hz
                if s["rusage_kb"]:
                    s["peak_kb"] = max(s["peak_kb"], s["rusage_kb"])
        if any(s["exit"] is None for s in sessions):
            time.sleep(sample_s)

    # Interrupted: stop the remaining children and collect their rusage.
    for s in sessions:
        if s["exit"] is not None:
            continue
        try:
            os.kill(s["pid"], signal.SIGTERM)
        except ProcessLookupError:
            pass
    for s in sessions:
        if s["exit"] is not None:
            continue
        try:
            _, status, usage = os.wait4(s["pid"], 0)
        except ChildProcessError:
            continue
        s["exit"] = os.WEXITSTATUS(status) if os.WIFEXITED(status) else 128 + os.WTERMSIG(status)
        s["wall_s"] = time.monotonic() - s["launch"]
        s["cpu_s"] = usage.ru_utime + usage.ru_stime
        s["rusage_kb"] = usage.ru_maxrss
        if s["rusage_kb"]:
            s["peak_kb"] = max(s["peak_kb"], s["rusage_kb"])

    results = []
    for s in sessions:
        log = out / s["log"]
        try:
            text = log.read_text(errors="replace")
        except OSError:
            text = ""
        wall = max(0.001, s["wall_s"] or 0.0)
        cpu = s["cpu_s"] or 0.0
        peak_mb = (s["peak_kb"] or 0) / 1024.0
        pss_mb = (s["peak_pss_kb"] or 0) / 1024.0
        outcome = _classify(text, s["exit"] if s["exit"] is not None else -1)
        results.append({
            "index": s["index"],
            "exit": s["exit"],
            "wall_seconds": round(wall, 3),
            "cpu_seconds": round(cpu, 3),
            "cpu_percent": round(cpu / wall * 100.0, 2),
            "peak_rss_mb": round(peak_mb, 1),
            "peak_pss_mb": round(pss_mb, 1),
            "joined": outcome["joined"],
            "disconnected": outcome["disconnected"],
            "join_error": outcome["join_error"],
            "spawn_retry": outcome["spawn_retry"],
            "log": s["log"],
        })

    wall_span = time.monotonic() - started
    cpu_pcts = [r["cpu_percent"] for r in results]
    rss = [r["peak_rss_mb"] for r in results]
    pss = [r["peak_pss_mb"] for r in results]
    # A failed join exits early, so its CPU/wall ratio is not a session cost.
    # Fleet sizing must use the joined-only aggregate.
    joined_results = [r for r in results if r["joined"]]
    joined_cpu = [r["cpu_percent"] for r in joined_results]
    joined_pss = [r["peak_pss_mb"] for r in joined_results]
    summary = {
        "generated_at": stamp,
        "place_id": args.place_id,
        "mode": "join",
        "requested": args.count,
        "started": len(results),
        "joined": sum(1 for r in results if r["joined"]),
        "wall_seconds": round(wall_span, 2),
        "cpu_seconds_total": round(sum(r["cpu_seconds"] for r in results), 3),
        "cpu_percent_mean": round(statistics.fmean(cpu_pcts), 2) if cpu_pcts else 0.0,
        "cpu_percent_max": round(max(cpu_pcts), 2) if cpu_pcts else 0.0,
        "peak_rss_mb_mean": round(statistics.fmean(rss), 1) if rss else 0.0,
        "peak_rss_mb_max": round(max(rss), 1) if rss else 0.0,
        "peak_pss_mb_mean": round(statistics.fmean(pss), 1) if pss else 0.0,
        "peak_pss_mb_max": round(max(pss), 1) if pss else 0.0,
        "peak_rss_mb_total": round(sum(rss), 1),
        "peak_pss_mb_total": round(sum(pss), 1),
        "joined_cpu_percent_mean": round(statistics.fmean(joined_cpu), 2)
        if joined_cpu else 0.0,
        "joined_peak_pss_mb_mean": round(statistics.fmean(joined_pss), 1)
        if joined_pss else 0.0,
        "sessions": results,
    }
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")

    print()
    print(f"fleet: {summary['joined']}/{summary['started']} joined, "
          f"wall {summary['wall_seconds']}s")
    print(f"fleet: cpu mean {summary['cpu_percent_mean']}% "
          f"max {summary['cpu_percent_max']}% "
          f"total {summary['cpu_seconds_total']}s")
    if summary["joined"]:
        print(f"fleet: joined cpu mean {summary['joined_cpu_percent_mean']}% "
              f"pss mean {summary['joined_peak_pss_mb_mean']}MB")
    print(f"fleet: rss mean {summary['peak_rss_mb_mean']}MB "
          f"max {summary['peak_rss_mb_max']}MB "
          f"pss mean {summary['peak_pss_mb_mean']}MB "
          f"max {summary['peak_pss_mb_max']}MB "
          f"total {summary['peak_pss_mb_total']}MB")
    print(f"fleet: summary {out / 'summary.json'}")
    return 0 if summary["joined"] == summary["started"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
