import os
import socket
import subprocess
import sys
import time
import uuid
from pathlib import Path

import psycopg
import pytest

ROOT = Path(__file__).resolve().parents[2]
BASE = os.environ.get("DCMON_TEST_DSN_BASE", "postgresql://postgres@localhost:5432/")


def url(name: str) -> str:
    return BASE.replace("/?", f"/{name}?") if "/?" in BASE else BASE.rstrip("/") + "/" + name


@pytest.fixture
def dsn():
    name = "t_" + uuid.uuid4().hex[:10]
    with psycopg.connect(url("postgres"), autocommit=True) as conn:
        conn.execute(f'CREATE DATABASE "{name}"')
    yield url(name)
    with psycopg.connect(url("postgres"), autocommit=True) as conn:
        conn.execute(f'DROP DATABASE "{name}" WITH (FORCE)')


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture
def collector(dsn):
    """A real collector process on a free port."""
    port = free_port()
    proc = subprocess.Popen([sys.executable, "-m", "dcmon.collector", "--host", "127.0.0.1", "--port", str(port),
                             "--dsn", dsn], cwd=ROOT / "collector", stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.1)
    yield {"port": port, "proc": proc, "dsn": dsn}
    proc.terminate()
    proc.wait(10)
