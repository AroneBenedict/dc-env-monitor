"""REST API over the collected data, for dashboards and operators.

    DCMON_DSN=postgresql://... uvicorn dcmon.api:app --port 8000

    GET  /devices                         devices and when they last reported
    GET  /alarms/active                   sensors currently not normal
    GET  /alarms?device=&sensor=&since=   alarm history
    POST /alarms/{device}/{seq}/ack       acknowledge an alarm (who, note)
    GET  /readings?device=&sensor=&since=&until=
    GET  /export.xlsx?device=&day=YYYY-MM-DD   one day of readings and alarms as an Excel workbook
"""

from __future__ import annotations

import io
from datetime import date, datetime, timedelta, timezone

import psycopg
from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import Response
from openpyxl import Workbook
from openpyxl.styles import Font, PatternFill
from psycopg.rows import dict_row
from pydantic import BaseModel, Field

from . import __version__
from .db import dsn

HEADER = PatternFill("solid", fgColor="1F3A5F")


class Ack(BaseModel):
    by: str = Field(..., min_length=1, max_length=64)
    note: str | None = Field(None, max_length=500)


def create_app(conninfo: str | None = None) -> FastAPI:
    app = FastAPI(title="Data-centre environment monitor", version=__version__)

    def query(sql: str, params: tuple = ()) -> list[dict]:
        with psycopg.connect(conninfo or dsn(), row_factory=dict_row) as conn:
            return conn.execute(sql, params).fetchall()

    @app.get("/health")
    def health() -> dict:
        query("SELECT 1")
        return {"status": "ok", "version": __version__}

    @app.get("/devices")
    def devices() -> list[dict]:
        return query("SELECT d.*, (SELECT count(*) FROM active_alarms a WHERE a.device = d.id AND a.severity <> 'normal') "
                     "AS active_alarms FROM devices d ORDER BY id")

    @app.get("/alarms/active")
    def active(device: str | None = None) -> list[dict]:
        return query("SELECT * FROM active_alarms WHERE severity <> 'normal' AND (%s::text IS NULL OR device = %s) "
                     "ORDER BY CASE severity WHEN 'critical' THEN 0 WHEN 'offline' THEN 1 ELSE 2 END, ts",
                     (device, device))

    @app.get("/alarms")
    def alarms(device: str | None = None, sensor: str | None = None, since: datetime | None = None,
               limit: int = Query(200, ge=1, le=5000)) -> list[dict]:
        return query("SELECT * FROM alarms WHERE (%s::text IS NULL OR device = %s) AND (%s::text IS NULL OR sensor = %s) "
                     "AND (%s::timestamptz IS NULL OR ts >= %s) ORDER BY ts DESC, seq DESC LIMIT %s",
                     (device, device, sensor, sensor, since, since, limit))

    @app.post("/alarms/{device}/{seq}/ack")
    def acknowledge(device: str, seq: int, body: Ack) -> dict:
        with psycopg.connect(conninfo or dsn(), row_factory=dict_row) as conn:
            row = conn.execute("UPDATE alarms SET acknowledged_by = %s, acknowledged_at = now(), note = %s "
                               "WHERE device = %s AND seq = %s AND acknowledged_at IS NULL RETURNING *",
                               (body.by, body.note, device, seq)).fetchone()
            if row is None:
                exists = conn.execute("SELECT 1 FROM alarms WHERE device = %s AND seq = %s", (device, seq)).fetchone()
                raise HTTPException(409 if exists else 404,
                                    "alarm already acknowledged" if exists else "alarm not found")
            conn.commit()
            return row

    @app.get("/readings")
    def readings(device: str, sensor: str, since: datetime | None = None, until: datetime | None = None,
                 limit: int = Query(1440, ge=1, le=20000)) -> list[dict]:
        return query("SELECT ts, n, min, avg, max FROM readings WHERE device = %s AND sensor = %s "
                     "AND (%s::timestamptz IS NULL OR ts >= %s) AND (%s::timestamptz IS NULL OR ts < %s) "
                     "ORDER BY ts LIMIT %s", (device, sensor, since, since, until, until, limit))

    @app.get("/export.xlsx")
    def export(device: str, day: date) -> Response:
        start = datetime(day.year, day.month, day.day, tzinfo=timezone.utc)
        end = start + timedelta(days=1)
        summary = query("SELECT sensor, kind, count(*) AS windows, round(min(min)::numeric, 2) AS min, "
                        "round((sum(avg * n) / sum(n))::numeric, 2) AS avg, round(max(max)::numeric, 2) AS max "
                        "FROM readings WHERE device = %s AND ts >= %s AND ts < %s GROUP BY sensor, kind ORDER BY sensor",
                        (device, start, end))
        if not summary:
            raise HTTPException(404, f"no readings for {device} on {day}")
        events = query("SELECT ts, sensor, kind, from_severity, to_severity, value, acknowledged_by FROM alarms "
                       "WHERE device = %s AND ts >= %s AND ts < %s ORDER BY ts", (device, start, end))
        detail = query("SELECT ts, sensor, n, min, avg, max FROM readings WHERE device = %s AND ts >= %s AND ts < %s "
                       "ORDER BY sensor, ts", (device, start, end))
        wb = Workbook()
        wb.remove(wb.active)
        for title, rows in (("Summary", summary), ("Alarms", events), ("Readings", detail)):
            ws = wb.create_sheet(title)
            columns = list(rows[0].keys()) if rows else ["ts", "sensor", "kind", "from_severity", "to_severity"]
            ws.append(columns)
            for cell in ws[1]:
                cell.font, cell.fill = Font(bold=True, color="FFFFFF"), HEADER
            for row in rows:
                ws.append([v.replace(tzinfo=None) if isinstance(v, datetime) else
                           float(v) if hasattr(v, "is_finite") else v for v in row.values()])
            for i, column in enumerate(columns, 1):
                ws.column_dimensions[ws.cell(1, i).column_letter].width = max(12, len(column) + 4)
            ws.freeze_panes = "A2"
        buf = io.BytesIO()
        wb.save(buf)
        return Response(buf.getvalue(), media_type="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
                        headers={"Content-Disposition": f'attachment; filename="{device}-{day}.xlsx"'})

    return app


app = create_app()
