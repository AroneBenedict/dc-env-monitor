import asyncio
import json
import socket

import psycopg
import pytest

from dcmon.collector import Collector, InvalidMessage, parse
from dcmon.db import init_schema

READING = {"seq": 1, "device": "dc1", "type": "reading", "sensor": "r1-inlet", "kind": "temperature",
           "ts": 1767225600, "n": 6, "min": 21.0, "avg": 21.5, "max": 22.0}
ALARM = {"seq": 2, "device": "dc1", "type": "alarm", "sensor": "r1-inlet", "kind": "temperature", "ts": 1767225700,
         "from": "normal", "to": "critical", "value": 33.2}


def line(msg: dict) -> bytes:
    return (json.dumps(msg) + "\n").encode()


def test_parse_accepts_valid_messages():
    device, seq, kind, values = parse(line(READING))
    assert (device, seq, kind) == ("dc1", 1, "reading") and values[-3:] == (21.0, 21.5, 22.0)
    assert parse(line({**ALARM, "to": "offline", "value": None}))[3][-1] is None
    status = {"seq": 3, "device": "dc1", "type": "status", "ts": 1767225600, "accepted": 5, "rejected": 1,
              "dropped": 0, "spool_bytes": 10}
    assert parse(line(status))[2] == "status"


@pytest.mark.parametrize("bad", [
    b"not json\n", b"[1,2]\n", line({**READING, "seq": 0}), line({**READING, "seq": True}),
    line({**READING, "device": "dc1; DROP TABLE readings"}), line({**READING, "ts": 12}),
    line({**READING, "min": 30}), line({**READING, "n": 0}), line({**READING, "kind": "plasma"}),
    line({**READING, "avg": "hot"}), line({**ALARM, "to": "apocalypse"}), line({**READING, "type": "selfie"}),
])
def test_parse_rejects_invalid_messages(bad):
    with pytest.raises(InvalidMessage):
        parse(bad)


def test_store_is_idempotent_and_skips_invalid(dsn):
    with psycopg.connect(dsn) as conn:
        init_schema(conn)
    collector = Collector(dsn)

    async def go():
        async with await psycopg.AsyncConnection.connect(dsn) as conn:
            first = await collector.store(conn, [line(READING), line(ALARM), b"garbage"])
            again = await collector.store(conn, [line(READING), line(ALARM)])  # resent after a reconnect
            return first, again

    first, again = asyncio.run(go())
    assert first == 2 and again == 2
    s = collector.stats
    assert (s.stored, s.duplicates, s.invalid) == (2, 2, 1)
    with psycopg.connect(dsn) as conn:
        assert conn.execute("SELECT count(*) FROM readings").fetchone()[0] == 1
        assert conn.execute("SELECT last_seq FROM devices WHERE id = 'dc1'").fetchone()[0] == 2
        assert conn.execute("SELECT severity FROM active_alarms").fetchone()[0] == "critical"


def test_tcp_protocol_acks_after_commit(collector):
    with socket.create_connection(("127.0.0.1", collector["port"]), timeout=5) as s:
        s.sendall(line(READING) + line(ALARM))
        buf = b""
        while b"ACK 2\n" not in buf:
            buf += s.recv(100)
    with psycopg.connect(collector["dsn"]) as conn:
        assert conn.execute("SELECT count(*) FROM alarms").fetchone()[0] == 1


def test_oversized_line_closes_the_connection(collector):
    with socket.create_connection(("127.0.0.1", collector["port"]), timeout=5) as s:
        s.sendall(b"x" * 10_000)
        assert s.recv(100) == b""  # collector hung up instead of buffering forever
