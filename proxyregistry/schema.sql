-- ProxyRegistry schema
-- Run: sqlite3 data/registry.db < schema.sql

CREATE TABLE IF NOT EXISTS proxy_slots (
    port            INTEGER PRIMARY KEY,
    status          TEXT NOT NULL DEFAULT 'free',
    pid             INTEGER DEFAULT NULL,
    server_password TEXT DEFAULT NULL,
    server_name     TEXT DEFAULT NULL,
    server_version  INTEGER DEFAULT NULL,
    server_ip       TEXT DEFAULT NULL,
    assigned_at     DATETIME DEFAULT NULL,
    last_heartbeat  DATETIME DEFAULT NULL,
    last_error      TEXT DEFAULT NULL
);

CREATE INDEX IF NOT EXISTS idx_status ON proxy_slots(status);
