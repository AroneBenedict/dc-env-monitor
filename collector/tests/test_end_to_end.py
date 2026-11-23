"""The real C daemon talking to the real collector."""

import json
import signal
import subprocess
import sys
import time

import psycopg
import pytest
from conftest import ROOT

ENVMOND = ROOT / "daemon" / "build" / "envmond"
pytestmark = pytest.mark.skipif(not ENVMOND.exists(), reason="build the daemon first: make -C daemon")


def make_feed(tmp_path, port, days=1, seed=3):
    feed, truth, conf = tmp_path / "feed.txt", tmp_path / "truth.json", tmp_path / "envmond.conf"
    (tmp_path / "spool").mkdir()
    subprocess.run([sys.executable, str(ROOT / "sim" / "rack_sim.py"), "--days", str(days), "--seed", str(seed),
                    "--out", str(feed), "--truth", str(truth), "--config", str(conf), "--port", str(port),
                    "--spool-dir", str(tmp_path / "spool")], check=True, capture_output=True)
    return feed, conf


def run(conf, feed, drain=60):
    with open(feed, "rb") as fh:
        proc = subprocess.run([str(ENVMOND), "-c", str(conf), "--drain-timeout", str(drain)], stdin=fh,
                              capture_output=True, check=False)
    exit_line = [ln for ln in proc.stderr.decode().splitlines() if ln.startswith('{"event":"exit"')][-1]
    return proc.returncode, json.loads(exit_line)


def stored_seqs(dsn):
    with psycopg.connect(dsn) as conn:
        return [r[0] for r in conn.execute("SELECT seq FROM readings UNION ALL SELECT seq FROM alarms "
                                           "UNION ALL SELECT seq FROM device_status ORDER BY 1")]


def test_every_message_arrives_exactly_once(collector, tmp_path):
    feed, conf = make_feed(tmp_path, collector["port"])
    code, stats = run(conf, feed)
    assert code == 0 and stats["backlog_bytes"] == 0 and stats["acked_seq"] == stats["last_seq"]
    assert stored_seqs(collector["dsn"]) == list(range(1, stats["last_seq"] + 1))
    assert stats["rej_out_of_range"] + stats["rej_glitch"] > 0 and stats["rej_bad_format"] > 0


def test_spool_survives_collector_outage_and_daemon_restart(collector, tmp_path):
    feed, conf = make_feed(tmp_path, collector["port"])
    collector["proc"].send_signal(signal.SIGSTOP)  # collector frozen: nothing can be delivered
    code, first = run(conf, feed, drain=0.5)
    assert code == 3 and first["backlog_bytes"] > 0 and first["acked_seq"] < first["last_seq"]
    collector["proc"].send_signal(signal.SIGCONT)
    time.sleep(0.5)
    empty = tmp_path / "empty.txt"
    empty.write_text("")
    code, second = run(conf, empty)  # a fresh daemon process delivers the spooled backlog
    assert code == 0 and second["acked_seq"] == first["last_seq"]
    assert stored_seqs(collector["dsn"]) == list(range(1, first["last_seq"] + 1))


def test_bad_config_is_refused(tmp_path):
    conf = tmp_path / "bad.conf"
    conf.write_text("[device]\nid = x\n[sensor a]\nkind = plasma\n")
    proc = subprocess.run([str(ENVMOND), "-c", str(conf)], stdin=subprocess.DEVNULL, capture_output=True)
    assert proc.returncode == 2 and b"line 4: unknown kind plasma" in proc.stderr
