"""Simulate one row of data-centre racks and write the sensor feed envmond reads.

    python sim/rack_sim.py --days 7 --seed 1 --out feed.txt --truth truth.json --config envmond.conf

The row has 6 racks, each with an inlet temperature sensor and a door contact, plus two
humidity sensors and two floor leak detectors (16 sensors, one sample every 10 seconds).

What happens in it:
* Normal operation: cooled supply air with noise, rack-to-rack differences, a slow daily load
  cycle, staff opening doors for a few minutes, and one rack that runs close to its warning
  limit for hours without actually being in trouble (the classic source of alarm flapping).
* Incidents (ground truth): cooling unit (CRAC) failure, a hot spot on one rack, falling
  humidity, a door left open, a water leak, a sensor that stops reporting.
* Feed defects: 1-Wire style error values (85.0, -127, 999) and malformed lines.
"""

from __future__ import annotations

import argparse
import json
import math
import random
from dataclasses import asdict, dataclass

START = 1767225600  # 2026-01-01T00:00:00Z
STEP = 10
RACKS = [f"r{i}" for i in range(1, 7)]
SENSORS = ([(f"{r}-inlet", "temperature") for r in RACKS] + [("row-rh-a", "humidity"), ("row-rh-b", "humidity")]
           + [(f"{r}-door", "door") for r in RACKS] + [("floor-leak-a", "leak"), ("floor-leak-b", "leak")])
THRESHOLDS = {"temperature": {"warn_high": 27.0, "crit_high": 32.0, "hysteresis": 1.0, "delay": 120},
              "humidity": {"warn_low": 20.0, "warn_high": 80.0, "hysteresis": 3.0, "delay": 120},
              "door": {"delay": 300}, "leak": {"delay": 0}}


@dataclass
class Event:
    id: int
    kind: str          # crac_failure, hotspot, humidity_low, door_left_open, leak, sensor_dropout
    start: int
    end: int
    sensors: list[str]


def write_config(path: str, collector_port: int = 7070, spool_dir: str = "/var/lib/envmond") -> None:
    lines = ["[device]", "id = dc1-row3", "collector_host = 127.0.0.1", f"collector_port = {collector_port}",
             f"spool_dir = {spool_dir}", "report_interval = 60", "stale_after = 180", ""]
    for sensor, kind in SENSORS:
        lines += [f"[sensor {sensor}]", f"kind = {kind}"]
        lines += [f"{k} = {v}" for k, v in THRESHOLDS[kind].items()]
        lines.append("")
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))


def simulate(days: int, seed: int):
    rng = random.Random(seed)
    n = days * 86400 // STEP
    times = [START + i * STEP for i in range(n)]
    events: list[Event] = []

    def add(kind: str, per_week: float, minutes: tuple[float, float], sensors_fn) -> None:
        for _ in range(max(1, round(per_week * days / 7 + rng.uniform(-0.5, 0.5)))):
            start = START + rng.randrange(3600, days * 86400 - 4 * 3600)
            length = int(rng.uniform(*minutes) * 60)
            events.append(Event(len(events), kind, start, start + length, sensors_fn()))

    add("crac_failure", 2, (20, 50), lambda: [f"{r}-inlet" for r in RACKS])
    add("hotspot", 3, (60, 360), lambda: [f"{rng.choice(RACKS)}-inlet"])
    add("humidity_low", 1, (240, 480), lambda: [rng.choice(["row-rh-a", "row-rh-b"])])
    add("door_left_open", 2, (15, 60), lambda: [f"{rng.choice(RACKS)}-door"])
    add("leak", 1, (30, 120), lambda: [rng.choice(["floor-leak-a", "floor-leak-b"])])
    add("sensor_dropout", 3, (10, 60), lambda: [rng.choice([s for s, k in SENSORS if k in ("temperature", "humidity")])])

    clean: dict[str, list[float]] = {}
    offsets = {r: rng.uniform(1.0, 3.5) for r in RACKS}
    hover_rack = RACKS[-1]
    offsets[hover_rack] = 4.0  # runs warm: hovers around 26-27 C at peak load

    # Supply air temperature: setpoint plus CRAC failures (exponential rise, then recovery).
    supply = []
    crac = [e for e in events if e.kind == "crac_failure"]
    for t in times:
        load = 1.2 * math.sin(2 * math.pi * ((t - START) % 86400) / 86400 - 1.2)  # daily IT load cycle
        rise = 0.0
        for e in crac:
            if e.start <= t <= e.end:
                rise = max(rise, 15 * (1 - math.exp(-(t - e.start) / 720)))
            elif e.end < t < e.end + 3600:
                peak = 15 * (1 - math.exp(-(e.end - e.start) / 720))
                rise = max(rise, peak * math.exp(-(t - e.end) / 600))
        supply.append(20.0 + load + rise)

    for r in RACKS:
        hot = [e for e in events if e.kind == "hotspot" and f"{r}-inlet" in e.sensors]
        clean[f"{r}-inlet"] = [
            supply[i] + offsets[r] + sum(6.0 for e in hot if e.start <= t <= e.end) for i, t in enumerate(times)]
    for s in ("row-rh-a", "row-rh-b"):
        base = rng.uniform(42, 50)
        dry = [e for e in events if e.kind == "humidity_low" and s in e.sensors]
        values = []
        for t in times:
            v = base + 3 * math.sin(2 * math.pi * (t - START) / (3 * 86400))
            for e in dry:
                if e.start <= t <= e.end:
                    v -= min(1.0, (t - e.start) / 5400) * (base - 14)  # dries out over 90 minutes
                elif e.end < t < e.end + 3600:
                    v -= (base - 14) * math.exp(-(t - e.end) / 900)
            values.append(v)
        clean[s] = values
    for r in RACKS:
        door = [0.0] * n
        for _ in range(int(6 * days)):  # routine visits: a few minutes each, no alarm expected
            i = rng.randrange(n)
            for k in range(i, min(n, i + int(rng.uniform(1, 4.5) * 60 / STEP))):
                door[k] = 1.0
        for e in events:
            if e.kind == "door_left_open" and f"{r}-door" in e.sensors:
                for k in range((e.start - START) // STEP, min(n, (e.end - START) // STEP + 1)):
                    door[k] = 1.0
        clean[f"{r}-door"] = door
    for s in ("floor-leak-a", "floor-leak-b"):
        clean[s] = [1.0 if any(e.kind == "leak" and s in e.sensors and e.start <= t <= e.end for e in events) else 0.0
                    for t in times]
    return times, clean, events, rng


def write_feed(times, clean, events, rng, path: str) -> dict:
    dropouts = [e for e in events if e.kind == "sensor_dropout"]
    noise = {"temperature": 0.25, "humidity": 0.8}
    stats = {"lines": 0, "error_values": 0, "malformed": 0}
    with open(path, "w", encoding="utf-8") as fh:
        for i, t in enumerate(times):
            for sensor, kind in SENSORS:
                if any(sensor in e.sensors and e.start <= t <= e.end for e in dropouts):
                    continue
                v = clean[sensor][i]
                if kind in noise:
                    v += rng.gauss(0, noise[kind])
                    if rng.random() < 0.0005:
                        v = rng.choice([85.0, -127.0, 999.0]) if kind == "temperature" else 150.0
                        stats["error_values"] += 1
                    text = f"{v:.2f}"
                else:
                    text = str(int(v))
                if rng.random() < 0.0001:
                    fh.write(f"{t} {sensor}\n")  # truncated line
                    stats["malformed"] += 1
                else:
                    fh.write(f"{t} {sensor} {text}\n")
                stats["lines"] += 1
    return stats


def expected_alarms(times, clean, events) -> list[dict]:
    """Which (event, sensor) pairs should raise an alarm, and from when.

    An alarm is due when the noise-free value is beyond its warning limit for at least the
    configured delay (or immediately for leaks), and for every sensor dropout.
    """
    due = []
    kinds = dict(SENSORS)
    for e in events:
        for sensor in e.sensors:
            kind = kinds[sensor]
            if e.kind == "sensor_dropout":
                due.append({"event": e.id, "kind": e.kind, "sensor": sensor, "condition_start": e.start,
                            "window_end": e.end + 600})
                continue
            th = THRESHOLDS[kind]
            delay = th["delay"]
            first, run = None, 0
            for i, t in enumerate(times):
                if t < e.start or t > e.end + 3600:
                    continue
                v = clean[sensor][i]
                beyond = (v > th.get("warn_high", math.inf) if kind != "humidity"
                          else v < th["warn_low"]) if kind in ("temperature", "humidity") else v >= 0.5
                if beyond:
                    run += STEP
                    if first is None:
                        first = t
                    if run >= max(delay, STEP):
                        due.append({"event": e.id, "kind": e.kind, "sensor": sensor, "condition_start": first,
                                    "window_end": e.end + 3600})
                        break
                else:
                    first, run = None, 0
    return due


def main(argv: list[str] | None = None) -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--days", type=int, default=7)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--out", default="feed.txt")
    p.add_argument("--truth", default="truth.json")
    p.add_argument("--config")
    p.add_argument("--port", type=int, default=7070)
    p.add_argument("--spool-dir", default="/var/lib/envmond")
    args = p.parse_args(argv)
    times, clean, events, rng = simulate(args.days, args.seed)
    stats = write_feed(times, clean, events, rng, args.out)
    truth = {"seed": args.seed, "days": args.days, "start": START, "events": [asdict(e) for e in events],
             "expected_alarms": expected_alarms(times, clean, events), "feed": stats}
    with open(args.truth, "w", encoding="utf-8") as fh:
        json.dump(truth, fh, indent=1)
    if args.config:
        write_config(args.config, args.port, args.spool_dir)
    print(json.dumps({"events": len(events), "expected_alarms": len(truth["expected_alarms"]), **stats}))


if __name__ == "__main__":
    main()
