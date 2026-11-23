import io

import psycopg
from fastapi.testclient import TestClient
from openpyxl import load_workbook

from dcmon.api import create_app
from dcmon.db import init_schema


def seed(dsn):
    with psycopg.connect(dsn) as conn:
        init_schema(conn)
        conn.execute("INSERT INTO devices (id, last_seq) VALUES ('dc1', 4)")
        for seq, minute, avg in ((1, 0, 21.0), (2, 1, 22.0)):
            conn.execute("INSERT INTO readings VALUES ('dc1', %s, 'r1-inlet', 'temperature', "
                         "'2026-01-01T00:00:00Z'::timestamptz + %s * interval '1 minute', 6, %s, %s, %s)",
                         (seq, minute, avg - 0.5, avg, avg + 0.5))
        conn.execute("INSERT INTO alarms VALUES ('dc1', 3, 'r1-inlet', 'temperature', '2026-01-01T00:05:00Z', "
                     "'normal', 'critical', 33.1, NULL, NULL, NULL)")
        conn.execute("INSERT INTO alarms VALUES ('dc1', 4, 'floor-leak', 'leak', '2026-01-01T00:06:00Z', "
                     "'normal', 'warning', 1, NULL, NULL, NULL)")
        conn.commit()


def test_api(dsn):
    seed(dsn)
    client = TestClient(create_app(dsn))
    assert client.get("/health").json()["status"] == "ok"
    assert client.get("/devices").json()[0]["active_alarms"] == 2
    active = client.get("/alarms/active").json()
    assert [a["severity"] for a in active] == ["critical", "warning"]  # most severe first
    ok = client.post("/alarms/dc1/3/ack", json={"by": "arone", "note": "CRAC 2 restarted"})
    assert ok.status_code == 200 and ok.json()["acknowledged_by"] == "arone"
    assert client.post("/alarms/dc1/3/ack", json={"by": "arone"}).status_code == 409
    assert client.post("/alarms/dc1/99/ack", json={"by": "arone"}).status_code == 404
    assert client.post("/alarms/dc1/4/ack", json={"by": ""}).status_code == 422
    readings = client.get("/readings", params={"device": "dc1", "sensor": "r1-inlet"}).json()
    assert [r["avg"] for r in readings] == [21.0, 22.0]
    assert len(client.get("/alarms", params={"sensor": "floor-leak"}).json()) == 1

    xlsx = client.get("/export.xlsx", params={"device": "dc1", "day": "2026-01-01"})
    assert xlsx.status_code == 200
    wb = load_workbook(io.BytesIO(xlsx.content))
    assert wb.sheetnames == ["Summary", "Alarms", "Readings"]
    summary = wb["Summary"]
    assert [c.value for c in summary[1]] == ["sensor", "kind", "windows", "min", "avg", "max"]
    assert summary["F2"].value == 22.5 and summary["E2"].value == 21.5
    assert client.get("/export.xlsx", params={"device": "dc1", "day": "2025-01-01"}).status_code == 404
