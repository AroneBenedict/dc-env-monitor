"""TCP collector: receives JSON-lines messages from envmond devices and stores them in PostgreSQL.

Protocol: the device sends one JSON object per line. After each batch is committed, the
collector replies "ACK <seq>" with the highest sequence number stored, and the device may
then forget everything up to it. Anything resent after a reconnect is a duplicate by
(device, seq) and is ignored by the database.

    python -m dcmon.collector --port 7070 --dsn postgresql://...
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import signal
from dataclasses import dataclass, field
from datetime import datetime, timezone

import psycopg

from .db import dsn, init_schema_async

log = logging.getLogger("dcmon.collector")
MAX_LINE = 4096
SEVERITIES = {"normal", "warning", "critical", "offline"}
KINDS = {"temperature", "humidity", "door", "leak"}


class InvalidMessage(ValueError):
    pass


@dataclass
class Stats:
    connections: int = 0
    messages: int = 0
    stored: int = 0
    duplicates: int = 0
    invalid: int = 0
    by_type: dict = field(default_factory=dict)


def _ts(value) -> datetime:
    if not isinstance(value, int) or not 946684800 <= value <= 4102444800:
        raise InvalidMessage("ts must be a unix time between 2000 and 2100")
    return datetime.fromtimestamp(value, tz=timezone.utc)


def _id(value, name: str) -> str:
    if not isinstance(value, str) or not 0 < len(value) < 48 or not all(c.isalnum() or c in "._-" for c in value):
        raise InvalidMessage(f"{name} must be a short id")
    return value


def _num(value, name: str, nullable: bool = False) -> float | None:
    if value is None and nullable:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise InvalidMessage(f"{name} must be a number")
    return float(value)


def parse(line: bytes) -> tuple[str, int, str, tuple]:
    """Validate one message. Returns (device, seq, type, row values for its table)."""
    try:
        msg = json.loads(line)
    except (ValueError, UnicodeDecodeError) as exc:
        raise InvalidMessage("not JSON") from exc
    if not isinstance(msg, dict):
        raise InvalidMessage("not an object")
    device = _id(msg.get("device"), "device")
    seq = msg.get("seq")
    if not isinstance(seq, int) or isinstance(seq, bool) or seq < 1:
        raise InvalidMessage("seq must be a positive integer")
    kind = msg.get("type")
    if kind == "reading":
        if msg.get("kind") not in KINDS:
            raise InvalidMessage("unknown sensor kind")
        n = msg.get("n")
        if not isinstance(n, int) or n < 1:
            raise InvalidMessage("n must be a positive integer")
        lo, avg, hi = (_num(msg.get(k), k) for k in ("min", "avg", "max"))
        if not lo <= avg <= hi:
            raise InvalidMessage("min <= avg <= max violated")
        return device, seq, kind, (device, seq, _id(msg.get("sensor"), "sensor"), msg["kind"], _ts(msg.get("ts")),
                                   n, lo, avg, hi)
    if kind == "alarm":
        if msg.get("kind") not in KINDS or msg.get("from") not in SEVERITIES or msg.get("to") not in SEVERITIES:
            raise InvalidMessage("bad alarm fields")
        return device, seq, kind, (device, seq, _id(msg.get("sensor"), "sensor"), msg["kind"], _ts(msg.get("ts")),
                                   msg["from"], msg["to"], _num(msg.get("value"), "value", nullable=True))
    if kind == "status":
        counts = []
        for k in ("accepted", "rejected", "dropped", "spool_bytes"):
            v = msg.get(k)
            if not isinstance(v, int) or v < 0:
                raise InvalidMessage(f"{k} must be a non-negative integer")
            counts.append(v)
        return device, seq, kind, (device, seq, _ts(msg.get("ts")), *counts)
    raise InvalidMessage("unknown message type")


INSERT = {
    "reading": "INSERT INTO readings (device, seq, sensor, kind, ts, n, min, avg, max) "
               "VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s) ON CONFLICT DO NOTHING",
    "alarm": "INSERT INTO alarms (device, seq, sensor, kind, ts, from_severity, to_severity, value) "
             "VALUES (%s, %s, %s, %s, %s, %s, %s, %s) ON CONFLICT DO NOTHING",
    "status": "INSERT INTO device_status (device, seq, ts, accepted, rejected, dropped, spool_bytes) "
              "VALUES (%s, %s, %s, %s, %s, %s, %s) ON CONFLICT DO NOTHING",
}


class Collector:
    def __init__(self, conninfo: str, batch: int = 500):
        self.conninfo = conninfo
        self.batch = batch
        self.stats = Stats()

    async def store(self, conn: psycopg.AsyncConnection, lines: list[bytes]) -> int:
        """Store a batch in one transaction. Returns the highest seq to acknowledge (0 if none)."""
        rows: dict[str, list[tuple]] = {"reading": [], "alarm": [], "status": []}
        devices: dict[str, int] = {}
        top = 0
        for line in lines:
            self.stats.messages += 1
            try:
                device, seq, kind, values = parse(line)
            except InvalidMessage as exc:
                # A malformed message would block the device forever if never acknowledged; it is
                # logged, counted and skipped instead.
                self.stats.invalid += 1
                log.warning("invalid message skipped: %s", exc)
                continue
            rows[kind].append(values)
            devices[device] = max(devices.get(device, 0), seq)
            top = max(top, seq)
        async with conn.transaction():
            for device, seq in devices.items():
                await conn.execute(
                    "INSERT INTO devices (id, last_seq) VALUES (%s, %s) ON CONFLICT (id) DO UPDATE "
                    "SET last_seen = now(), last_seq = GREATEST(devices.last_seq, EXCLUDED.last_seq)", (device, seq))
            async with conn.cursor() as cur:
                for kind, values in rows.items():
                    for value in values:  # one statement per row so duplicates are counted
                        await cur.execute(INSERT[kind], value)
                        if cur.rowcount == 1:
                            self.stats.stored += 1
                            self.stats.by_type[kind] = self.stats.by_type.get(kind, 0) + 1
                        else:
                            self.stats.duplicates += 1
        return top

    async def handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        peer = writer.get_extra_info("peername")
        self.stats.connections += 1
        log.info("device connected from %s", peer)
        pending = b""
        try:
            async with await psycopg.AsyncConnection.connect(self.conninfo) as conn:
                while True:
                    chunk = await reader.read(65536)
                    if not chunk:
                        break
                    pending += chunk
                    *lines, pending = pending.split(b"\n")
                    if len(pending) > MAX_LINE:
                        log.warning("dropping connection from %s: line longer than %d bytes", peer, MAX_LINE)
                        break
                    # Commit what has arrived in batches, acknowledging after each commit.
                    for start in range(0, len(lines), self.batch):
                        top = await self.store(conn, [line for line in lines[start:start + self.batch] if line])
                        if top:
                            writer.write(f"ACK {top}\n".encode())
                            await writer.drain()
        except (ConnectionError, OSError) as exc:
            log.info("connection from %s lost: %s", peer, exc)
        finally:
            writer.close()
            log.info("device %s disconnected", peer)

    async def serve(self, host: str, port: int, ready: asyncio.Event | None = None) -> None:
        async with await psycopg.AsyncConnection.connect(self.conninfo) as conn:
            await init_schema_async(conn)
        server = await asyncio.start_server(self.handle, host, port)
        log.info("collector listening on %s:%d", host, port)
        if ready:
            ready.set()
        async with server:
            await server.serve_forever()


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=7070)
    parser.add_argument("--dsn", default=dsn())
    args = parser.parse_args(argv)
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(name)s %(levelname)s %(message)s")
    collector = Collector(args.dsn)
    loop = asyncio.new_event_loop()
    task = loop.create_task(collector.serve(args.host, args.port))
    for sig in (signal.SIGTERM, signal.SIGINT):
        loop.add_signal_handler(sig, task.cancel)
    try:
        loop.run_until_complete(task)
    except asyncio.CancelledError:
        pass
    finally:
        s = collector.stats
        print(json.dumps({"event": "exit", "messages": s.messages, "stored": s.stored, "duplicates": s.duplicates,
                          "invalid": s.invalid}), flush=True)


if __name__ == "__main__":
    main()
