-- One row per message. (device, seq) is the primary key, so a message resent after a
-- reconnect is stored once: the device delivers at least once, storage makes it exactly once.

CREATE TABLE IF NOT EXISTS devices (
    id          text PRIMARY KEY,
    first_seen  timestamptz NOT NULL DEFAULT now(),
    last_seen   timestamptz NOT NULL DEFAULT now(),
    last_seq    bigint NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS readings (
    device  text NOT NULL REFERENCES devices(id),
    seq     bigint NOT NULL,
    sensor  text NOT NULL,
    kind    text NOT NULL,
    ts      timestamptz NOT NULL,
    n       integer NOT NULL CHECK (n > 0),
    min     double precision NOT NULL,
    avg     double precision NOT NULL,
    max     double precision NOT NULL,
    PRIMARY KEY (device, seq)
);
CREATE INDEX IF NOT EXISTS readings_sensor_ts ON readings (device, sensor, ts);

CREATE TABLE IF NOT EXISTS alarms (
    device           text NOT NULL REFERENCES devices(id),
    seq              bigint NOT NULL,
    sensor           text NOT NULL,
    kind             text NOT NULL,
    ts               timestamptz NOT NULL,
    from_severity    text NOT NULL,
    to_severity      text NOT NULL CHECK (to_severity IN ('normal', 'warning', 'critical', 'offline')),
    value            double precision,
    acknowledged_by  text,
    acknowledged_at  timestamptz,
    note             text,
    PRIMARY KEY (device, seq)
);
CREATE INDEX IF NOT EXISTS alarms_sensor_ts ON alarms (device, sensor, ts);

CREATE TABLE IF NOT EXISTS device_status (
    device       text NOT NULL REFERENCES devices(id),
    seq          bigint NOT NULL,
    ts           timestamptz NOT NULL,
    accepted     bigint NOT NULL,
    rejected     bigint NOT NULL,
    dropped      bigint NOT NULL,
    spool_bytes  bigint NOT NULL,
    PRIMARY KEY (device, seq)
);

-- Current state of every sensor that is not normal: its latest alarm transition.
CREATE OR REPLACE VIEW active_alarms AS
SELECT DISTINCT ON (device, sensor) device, sensor, kind, seq, ts, to_severity AS severity, value,
       acknowledged_by, acknowledged_at
FROM alarms
ORDER BY device, sensor, ts DESC, seq DESC;
