"""End-to-end evaluation with the real binaries: simulator -> envmond (C) -> collector -> PostgreSQL.

    python sim/evaluate.py --dsn-base "postgresql://postgres@/?host=/tmp&port=5433" --seeds 11 12 13

For each seed (one simulated week) it runs:
  1. full    : envmond with its alarm delays and hysteresis
  2. naive   : envmond --no-hysteresis (plain thresholds), the baseline
  3. chaos   : full settings while the collector is killed (SIGKILL) and restarted at random and
               envmond itself is restarted twice with messages still waiting in its spool
Alarm quality is scored against the simulator's ground truth; delivery is checked by
comparing every sequence number the device issued with what reached the database.

Seed 1 was used during development; reported numbers use other seeds.
"""

from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import psycopg

ROOT = Path(__file__).resolve().parents[1]
ENVMOND = ROOT / "daemon" / "build" / "envmond"
RAISES = {"warning", "critical", "offline"}
RANK = {"normal": 0, "warning": 1, "critical": 2, "offline": 3}


def db_url(base: str, name: str) -> str:
    return base.replace("/?", f"/{name}?") if "/?" in base else base.rstrip("/") + "/" + name


def fresh_db(base: str, name: str) -> str:
    with psycopg.connect(db_url(base, "postgres"), autocommit=True) as conn:
        conn.execute(f'DROP DATABASE IF EXISTS "{name}"')
        conn.execute(f'CREATE DATABASE "{name}"')
    return db_url(base, name)


def start_collector(dsn: str, port: int, log) -> subprocess.Popen:
    proc = subprocess.Popen([sys.executable, "-m", "dcmon.collector", "--host", "127.0.0.1", "--port", str(port),
                             "--dsn", dsn], cwd=ROOT / "collector", stdout=log, stderr=log)
    deadline = time.time() + 15
    import socket
    while time.time() < deadline:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            return proc
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("collector did not start")


def run_envmond(config: Path, feed: Path, extra: list[str], drain: float, log) -> dict:
    with open(feed, "rb") as fh:
        proc = subprocess.run([str(ENVMOND), "-c", str(config), "--drain-timeout", str(drain), *extra], stdin=fh,
                              stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, check=False)
    lines = proc.stderr.decode().splitlines()
    log.write("\n".join(lines) + "\n")
    exit_line = next(line for line in reversed(lines) if line.startswith('{"event":"exit"'))
    return {**json.loads(exit_line), "returncode": proc.returncode}


def score_alarms(dsn: str, truth: dict) -> dict:
    with psycopg.connect(dsn) as conn:
        rows = conn.execute("SELECT sensor, extract(epoch FROM ts)::bigint, from_severity, to_severity FROM alarms "
                            "ORDER BY ts, seq").fetchall()
    raises = [(s, t, to) for s, t, frm, to in rows if to in RAISES and RANK[to] > RANK[frm] and frm != "offline"]
    expected = truth["expected_alarms"]
    events = truth["events"]
    matched, delays = 0, []
    used = set()
    for due in expected:
        hits = [i for i, (s, t, _) in enumerate(raises)
                if s == due["sensor"] and due["condition_start"] - 60 <= t <= due["window_end"]]
        if hits:
            matched += 1
            first = min(hits, key=lambda i: raises[i][1])
            delays.append(raises[first][1] - due["condition_start"])
            used.update(hits)
    # A raise is justified if it falls inside any incident affecting that sensor.
    justified = 0
    for i, (s, t, _) in enumerate(raises):
        if i in used or any(s in e["sensors"] and e["start"] - 60 <= t <= e["end"] + 3600 for e in events):
            justified += 1
    days = truth["days"]
    delays.sort()
    return {"expected": len(expected), "detected": matched, "recall": round(matched / len(expected), 4),
            "alarms_raised": len(raises), "false_alarms": len(raises) - justified,
            "precision": round(justified / len(raises), 4) if raises else None,
            "alarms_per_day": round(len(raises) / days, 1),
            "median_delay_s": delays[len(delays) // 2] if delays else None}


def check_delivery(dsn: str, issued: int) -> dict:
    with psycopg.connect(dsn) as conn:
        seqs = [r[0] for r in conn.execute(
            "SELECT seq FROM readings UNION ALL SELECT seq FROM alarms UNION ALL SELECT seq FROM device_status "
            "ORDER BY 1").fetchall()]
    distinct = set(seqs)
    missing = sorted(set(range(1, issued + 1)) - distinct)
    return {"issued": issued, "stored": len(seqs), "missing": len(missing), "duplicates_stored": len(seqs) - len(distinct),
            "first_missing": missing[:5]}


def run_seed(seed: int, days: int, base: str, work: Path, port: int) -> dict:
    out = {"seed": seed}
    feed, truth_path, conf = work / "feed.txt", work / "truth.json", work / "envmond.conf"
    spool = work / "spool"
    subprocess.run([sys.executable, str(ROOT / "sim" / "rack_sim.py"), "--days", str(days), "--seed", str(seed),
                    "--out", str(feed), "--truth", str(truth_path), "--config", str(conf), "--port", str(port),
                    "--spool-dir", str(spool)], check=True, stdout=subprocess.DEVNULL)
    truth = json.loads(truth_path.read_text())
    out["feed"] = truth["feed"]
    with open(work / f"seed{seed}.log", "w") as log:
        out.update(_run_modes(seed, base, work, port, feed, conf, spool, truth, log))
    return out


def _run_modes(seed, base, work, port, feed, conf, spool, truth, log) -> dict:
    out = {}

    for mode, extra in (("full", []), ("naive", ["--no-hysteresis"])):
        shutil.rmtree(spool, ignore_errors=True)
        spool.mkdir()
        dsn = fresh_db(base, f"eval_{seed}_{mode}")
        collector = start_collector(dsn, port, log)
        began = time.perf_counter()
        stats = run_envmond(conf, feed, extra, 300, log)
        seconds = time.perf_counter() - began
        collector.send_signal(signal.SIGTERM)
        collector.wait(10)
        out[mode] = {"alarms": score_alarms(dsn, truth), "delivery": check_delivery(dsn, stats["last_seq"]),
                     "device": {k: stats[k] for k in ("accepted", "rejected", "rej_glitch", "rej_out_of_range",
                                                      "rej_bad_format", "last_seq")},
                     "seconds": round(seconds, 1)}

    # Chaos run: feed in three parts (two daemon restarts) while the collector is repeatedly killed.
    shutil.rmtree(spool, ignore_errors=True)
    spool.mkdir()
    dsn = fresh_db(base, f"eval_{seed}_chaos")
    lines = feed.read_text().splitlines(keepends=True)
    parts = []
    for k in range(3):
        part = work / f"part{k}.txt"
        part.write_text("".join(lines[k * len(lines) // 3:(k + 1) * len(lines) // 3]))
        parts.append(part)
    rng = random.Random(seed)
    collector = start_collector(dsn, port, log)
    kills = 0
    stop = {"flag": False}

    import threading

    def chaos():
        nonlocal collector, kills
        while not stop["flag"]:
            time.sleep(rng.uniform(0.3, 1.5))
            if stop["flag"]:
                break
            collector.kill()
            collector.wait()
            kills += 1
            time.sleep(rng.uniform(0.2, 1.0))
            collector = start_collector(dsn, port, log)

    thread = threading.Thread(target=chaos)
    thread.start()
    runs = []
    for i, part in enumerate(parts):
        last = i == len(parts) - 1
        runs.append(run_envmond(conf, part, [], 120 if last else 0.5, log))  # early parts exit with a backlog
        if last:
            stop["flag"] = True
    thread.join()
    if runs[-1]["returncode"] != 0:  # chaos stopped: one more start delivers what is still spooled
        if collector.poll() is not None:
            collector = start_collector(dsn, port, log)
        empty = work / "empty.txt"
        empty.write_text("")
        runs.append(run_envmond(conf, empty, [], 120, log))
    collector.send_signal(signal.SIGTERM)
    collector.wait(10)
    issued = max(r["last_seq"] for r in runs)
    out["chaos"] = {"collector_kills": kills, "daemon_restarts": len(parts) - 1,
                    "backlog_at_restart_bytes": [r["backlog_bytes"] for r in runs[:-1]],
                    "delivery": check_delivery(dsn, issued)}
    return out


def summarise(runs: list[dict]) -> dict:
    def total(mode, key):
        return sum(r[mode]["alarms"][key] for r in runs)
    summary = {}
    for mode in ("full", "naive"):
        expected, detected = total(mode, "expected"), total(mode, "detected")
        raised, false = total(mode, "alarms_raised"), total(mode, "false_alarms")
        delays = sorted(r[mode]["alarms"]["median_delay_s"] for r in runs if r[mode]["alarms"]["median_delay_s"] is not None)
        summary[mode] = {"expected": expected, "detected": detected, "recall": round(detected / expected, 4),
                         "alarms_raised": raised, "false_alarms": false,
                         "precision": round((raised - false) / raised, 4) if raised else None,
                         "alarms_per_day": round(raised / (7 * len(runs)), 1),
                         "median_delay_s": delays[len(delays) // 2] if delays else None}
    chaos = [r["chaos"] for r in runs]
    summary["chaos"] = {"collector_kills": sum(c["collector_kills"] for c in chaos),
                        "daemon_restarts": sum(c["daemon_restarts"] for c in chaos),
                        "messages_issued": sum(c["delivery"]["issued"] for c in chaos),
                        "missing": sum(c["delivery"]["missing"] for c in chaos),
                        "duplicates_stored": sum(c["delivery"]["duplicates_stored"] for c in chaos)}
    summary["feed_lines"] = sum(r["feed"]["lines"] for r in runs)
    summary["rejected"] = sum(r["full"]["device"]["rejected"] for r in runs)
    summary["throughput_lines_per_s"] = round(summary["feed_lines"] / sum(r["full"]["seconds"] for r in runs))
    return summary


def render(result: dict) -> str:
    s = result["summary"]
    f, n, c = s["full"], s["naive"], s["chaos"]
    def pct(v):
        return "n/a" if v is None else f"{v:.1%}"
    return "\n".join([
        "# Evaluation report", "",
        f"Seeds {', '.join(map(str, result['seeds']))}: one simulated week each for a row of 6 racks (16 sensors, "
        f"{s['feed_lines']:,} sensor samples in total), run through the real envmond binary, collector and "
        "PostgreSQL.", "",
        "## Alarm quality", "",
        "| Alarm logic | Incidents due an alarm | Caught | Alarms raised | False alarms | Precision | Alarms per day |",
        "|---|---|---|---|---|---|---|",
        f"| Plain thresholds (--no-hysteresis) | {n['expected']} | {n['detected']} ({pct(n['recall'])}) | "
        f"{n['alarms_raised']} | {n['false_alarms']} | {pct(n['precision'])} | {n['alarms_per_day']} |",
        f"| Delays + hysteresis (default) | {f['expected']} | {f['detected']} ({pct(f['recall'])}) | "
        f"{f['alarms_raised']} | {f['false_alarms']} | {pct(f['precision'])} | {f['alarms_per_day']} |",
        "", f"Median time from the condition starting to the alarm: {f['median_delay_s']} s with delays, "
        f"{n['median_delay_s']} s without.", "",
        "## Delivery under failures", "",
        f"The collector was killed with SIGKILL {c['collector_kills']} times and envmond was restarted "
        f"{c['daemon_restarts']} times with messages still in its spool. Of {c['messages_issued']:,} messages issued, "
        f"**{c['missing']} were lost and {c['duplicates_stored']} were stored twice**.", "",
        f"Input validation rejected {s['rejected']:,} bad samples (probe error values, glitches, malformed lines). "
        f"Throughput end to end: about {s['throughput_lines_per_s']:,} sensor samples per second.",
    ]) + "\n"


def main(argv: list[str] | None = None) -> dict:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dsn-base", default=os.environ.get("DCMON_DSN_BASE", "postgresql://postgres@localhost:5432/"))
    p.add_argument("--seeds", type=int, nargs="+", default=[11, 12, 13])
    p.add_argument("--days", type=int, default=7)
    p.add_argument("--port", type=int, default=7095)
    p.add_argument("--out", type=Path, default=ROOT / "results")
    args = p.parse_args(argv)
    if not ENVMOND.exists():
        sys.exit("build the daemon first: make -C daemon")
    runs = []
    with tempfile.TemporaryDirectory() as tmp:
        for seed in args.seeds:
            work = Path(tmp) / f"seed{seed}"
            work.mkdir()
            runs.append(run_seed(seed, args.days, args.dsn_base, work, args.port))
            print(f"seed {seed}: {json.dumps({m: runs[-1][m]['alarms'] for m in ('full', 'naive')})}", flush=True)
            print(f"  chaos: {json.dumps(runs[-1]['chaos'])}", flush=True)
    result = {"seeds": args.seeds, "days": args.days, "summary": summarise(runs), "runs": runs}
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "metrics.json").write_text(json.dumps(result, indent=2) + "\n")
    (args.out / "REPORT.md").write_text(render(result))
    print(render(result))
    return result


if __name__ == "__main__":
    main()
