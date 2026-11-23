"""Database access: schema setup and idempotent message storage."""

from __future__ import annotations

import os
from pathlib import Path

import psycopg

SCHEMA = (Path(__file__).parent / "schema.sql").read_text(encoding="utf-8")


def dsn() -> str:
    return os.environ.get("DCMON_DSN", "postgresql://postgres@localhost:5432/dcmon")


def init_schema(conn: psycopg.Connection) -> None:
    conn.execute(SCHEMA)
    conn.commit()


async def init_schema_async(conn: psycopg.AsyncConnection) -> None:
    await conn.execute(SCHEMA)
    await conn.commit()
