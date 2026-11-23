# Data-Centre Environment Monitor

[![CI](https://github.com/AroneBenedict/dc-env-monitor/actions/workflows/ci.yml/badge.svg)](https://github.com/AroneBenedict/dc-env-monitor/actions/workflows/ci.yml)

An edge monitoring device for data-centre racks, end to end: a **C daemon for Linux** that
watches rack temperature, humidity, door contacts and floor leak detectors, raises and clears
alarms, and delivers everything to a **Python collector** that stores it in **PostgreSQL** and
serves it through a **REST API** with **Excel export**.

The hard parts are the ones a real device faces: alarms that do not flap when a value hovers at
a limit, no data lost when the network or the server goes down, and input that cannot crash the
daemon however malformed it is.

```
sensors ──► envmond (C, Linux daemon) ──TCP, JSON lines──► collector (Python asyncio) ──► PostgreSQL ──► REST API / Excel
              │  validate, aggregate, alarm state machines        ▲   ACK <seq> after commit
              └─ on-disk store-and-forward spool ─────────────────┘
```

## Results

Three held-out simulated weeks (2.9 million sensor samples) run through the real binaries.
Full report: [`results/REPORT.md`](results/REPORT.md).

**Alarm quality.** With plain thresholds, a row of 6 racks raised 71 alarms a day, half of them
false (a rack hovering near 27 °C, staff opening doors for a minute). With on-delays and
hysteresis, the same incidents produced 5 alarms a day:

| Alarm logic | Incidents caught | Alarms raised | False alarms | Precision |
|---|---|---|---|---|
| Plain thresholds | 66 of 66 | 1,492 | 739 | 50.5% |
| Delays + hysteresis (default) | 64 of 66 | 105 | 6 | 94.3% |

The cost is a median of 140 seconds before an alarm (the configured 120-second delay plus
sampling), and two borderline incidents that barely crossed the limit were missed.

**Delivery under failures.** During those weeks the collector was killed with SIGKILL 143
times and the daemon was restarted 6 times with up to 15 MB of messages waiting in its spool.
Of 484,305 messages issued, **none were lost and none were stored twice**.

**Robustness.** 10 million random and mutated inputs through the parsers under
AddressSanitizer and UndefinedBehaviorSanitizer with no crash; unit tests clean under Valgrind;
compiled with `-Wall -Wextra -Werror -Wconversion`.

## How it works

### envmond (C11, POSIX)

| File | What it does |
|---|---|
| `config.c` | INI configuration with line-numbered errors; reload on SIGHUP keeps alarm state for sensors that remain. |
| `sample.c` | Parses `<unix_time> <sensor_id> <value>` lines with strict bounds: ids, numbers, NaN/inf, embedded NULs, over-long lines. |
| `engine.c` | Validation (range checks, out-of-order samples, one-sample glitches such as a 1-Wire probe's 85.0 °C reset value), 60-second min/avg/max aggregation, offline detection for silent sensors, hourly status messages. |
| `alarm.c` | Per-sensor state machine: normal, warning, critical, offline. A level is entered only after it has held for `delay` seconds and left only after the value has come back by `hysteresis`. |
| `spool.c` | Store-and-forward: every message is appended to `spool.log` before sending and stays until the collector acknowledges it. Survives daemon restarts and crashes (a torn last line is discarded), continues sequence numbers, truncates once fully delivered, and refuses new data rather than filling the disk. |
| `net.c` | Non-blocking TCP with exponential reconnect back-off; partial writes continue from the exact byte where they stopped. |
| `main.c` | `poll()` loop over the sensor input and the socket; SIGTERM flushes and exits, SIGHUP reloads. |

Delivery is **at least once** (the device resends anything not acknowledged); the collector
stores by `(device, seq)`, so each message ends up in the database **exactly once**.

### Collector and API (Python)

- `collector.py`: asyncio TCP server. It validates every message, commits each batch in one
  transaction and only then replies `ACK <seq>`. Duplicates are discarded by the primary key,
  and malformed messages are counted and skipped so they cannot block a device.
- `schema.sql`: readings, alarms, device status, and an `active_alarms` view.
- `api.py`: FastAPI endpoints for devices, active alarms (most severe first), alarm history,
  acknowledgement with who and why, readings, and a one-day Excel export.

### Simulator and evaluation

`sim/rack_sim.py` produces a week for a row of 6 racks, sampled every 10 seconds. Normal
operation includes a daily load cycle, routine door openings and one rack running close to its
limit. On top of that it injects incidents with ground truth: cooling unit (CRAC) failures, hot
spots, falling humidity, a door left open, water leaks and sensors that stop reporting. It also
adds feed defects: probe error values and malformed lines. `sim/evaluate.py` runs the real
binaries against it, scores alarms against the ground truth, and runs the chaos test.

## Quickstart

```bash
make -C daemon && make -C daemon test        # build and unit-test the daemon
make -C daemon asan valgrind fuzz-lite       # sanitizers, Valgrind, fuzzing
pip install -e "collector[dev]"
export DCMON_DSN=postgresql://postgres@localhost:5432/dcmon
python -m dcmon.collector --port 7070 &
python sim/rack_sim.py --days 1 --out feed.txt --truth truth.json --config envmond.conf --spool-dir /tmp
./daemon/build/envmond -c envmond.conf < feed.txt
uvicorn dcmon.api:app --port 8000            # then: curl localhost:8000/alarms/active
```

On a device, `daemon/envmond.service` runs envmond under systemd as an unprivileged user with
a read-only filesystem except its spool directory. The sensor bus driver writes samples into a
FIFO that envmond reads.

## Tests

- **C:** 117 checks covering config errors, parser edge cases, alarm delays and hysteresis
  on both high and low limits, glitches, offline detection, reload, message format, and spool
  partial writes, resend, restart, torn lines, a full spool and truncation. Run plain, under
  Valgrind and under ASan/UBSan, plus a fuzz harness (libFuzzer-compatible).
- **Python:** 20 tests covering message validation including injection-style ids, idempotent
  storage, the ACK protocol, an oversized-line guard, the API with the Excel export, and
  end-to-end runs of the C daemon against the collector, including a frozen collector followed
  by a daemon restart.
- **CI:** both suites, sanitizers, Valgrind, fuzzing, and a short end-to-end evaluation against
  a PostgreSQL service container.

## Limitations

- **Simulated sensors.** The input is a text feed from the simulator. A real device needs a
  driver for its sensor bus (1-Wire, I2C, Modbus or SNMP) writing the same lines.
- **No encryption or authentication** on the device-to-collector link. A deployment would use
  TLS with per-device certificates.
- **The simulator was written alongside the daemon,** so the alarm numbers show the logic
  works as designed, not field accuracy. The one false alarm in the development week was two
  routine door visits back to back, which a real operator might also want to know about.
- **The delay trades speed for quiet:** a value that hovers right at a limit with noise may
  never alarm (the two misses above).
- **One collector process.** No clustering or high availability on the server side.

## Layout

```
daemon/src/        envmond sources        daemon/tests/   C unit tests     daemon/fuzz/  fuzz harness
collector/dcmon/   collector, schema, API collector/tests/ Python tests
sim/               rack simulator and end-to-end evaluation
results/           metrics.json and REPORT.md
```

Built AI-first, with Claude as a coding assistant.
