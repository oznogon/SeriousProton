#!/usr/bin/env python3
"""
ProxyRegistry — Dynamic proxy allocation for EmptyEpsilon.

Standalone HTTP server (stdlib only, no pip dependencies) that manages a pool
of GameServerProxy processes.  Game servers behind NAT register via HTTP, get
assigned a public relay port, and clients connect through that relay.

Usage
-----
    # Initialize the database (first run only)
    ./server.py --init-db

    # Start the HTTP server
    ./server.py

    # Custom port
    ./server.py --port 35668
"""

import argparse
import os
import signal
import socket
import sqlite3
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

import config


os.makedirs(config.DATA_DIR, exist_ok=True)
os.makedirs(config.LOG_DIR, exist_ok=True)

DB_PATH = os.path.join(config.DATA_DIR, 'registry.db')

SCHEMA_SQL = """\
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
"""


def get_db():
    db = sqlite3.connect(DB_PATH)
    db.row_factory = sqlite3.Row
    db.execute('PRAGMA journal_mode = WAL')
    db.execute('PRAGMA busy_timeout = 3000')
    return db


def init_db():
    db = get_db()
    db.executescript(SCHEMA_SQL)
    for port in range(config.PROXY_PORT_MIN, config.PROXY_PORT_MAX + 1):
        db.execute(
            "INSERT OR IGNORE INTO proxy_slots(port, status) VALUES (?, 'free')",
            (port,)
        )
    db.commit()
    db.close()
    count = config.PROXY_PORT_MAX - config.PROXY_PORT_MIN + 1
    print(f"Initialised {count} slots ({config.PROXY_PORT_MIN}-{config.PROXY_PORT_MAX})")


# ── master server registration ──────────────────────────────────
# Tracks assigned slots that need periodic heartbeats to the
# external master server (e.g. daid.eu/ee).
_assigned_slots = {}  # port -> {'name': str, 'version': int}
_assigned_lock = threading.Lock()


def _master_register(port, name, version):
    """Immediately register a single slot with the master server."""
    url = config.MASTER_SERVER_URL
    if not url:
        return
    data = urllib.parse.urlencode({
        'port': port,
        'name': name,
        'version': version,
    }).encode()
    req = urllib.request.Request(url, data=data, method='POST')
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            body = resp.read().decode('utf-8')
            if resp.status != 200 or body != 'OK':
                print(f"master_register: port {port} failed — HTTP {resp.status} {body}",
                      file=sys.stderr)
    except Exception as e:
        print(f"master_register: port {port} — {e}", file=sys.stderr)


def _master_heartbeat_loop():
    """Background thread: POST heartbeats to the master server every
    MASTER_HEARTBEAT_INTERVAL seconds for all currently assigned slots."""
    interval = config.MASTER_HEARTBEAT_INTERVAL
    while True:
        time.sleep(interval)
        with _assigned_lock:
            slots = list(_assigned_slots.items())
        for port, info in slots:
            _master_register(port, info['name'], info['version'])


class ProxyRegistryHandler(BaseHTTPRequestHandler):
    """HTTP request handler for the proxy registry API."""

    def do_POST(self):
        length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(length).decode('utf-8')
        params = parse_qs(body)

        path = self.path.rstrip('/')
        if path in ('/register.php', '/register'):
            self.handle_register(params)
        elif path in ('/deregister.php', '/deregister'):
            self.handle_deregister(params)
        elif path in ('/heartbeat.php', '/heartbeat'):
            self.handle_heartbeat(params)
        else:
            self.send_error(404, 'Not found')

    def do_GET(self):
        path = self.path.rstrip('/')
        if path in ('/list.php', '/list'):
            self.handle_list()
        elif path == '/status':
            self.handle_status()
        else:
            self.send_error(404, 'Not found')

    # ── helpers ────────────────────────────────────────────────

    def _ok(self, body: str):
        encoded = body.encode('utf-8')
        self.send_response(200)
        self.send_header('Content-Type', 'text/plain; charset=utf-8')
        self.send_header('Content-Length', str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def _err(self, code: int, msg: str):
        encoded = msg.encode('utf-8')
        self.send_response(code)
        self.send_header('Content-Type', 'text/plain; charset=utf-8')
        self.send_header('Content-Length', str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def _get_param(self, params: dict, key: str, default=None):
        vals = params.get(key)
        return vals[0] if vals else default

    # ── register ──────────────────────────────────────────────

    def handle_register(self, params: dict):
        password = self._get_param(params, 'password')
        name = self._get_param(params, 'name')
        version_raw = self._get_param(params, 'version', '0')

        if not name or not version_raw:
            self._err(400, 'Missing required parameters')
            return
        if password != config.REGISTRY_PASSWORD:
            self._err(403, 'Invalid password')
            return

        version = int(version_raw)
        db = get_db()
        try:
            row = db.execute(
                "SELECT port FROM proxy_slots WHERE status = 'free' ORDER BY port LIMIT 1"
            ).fetchone()

            if not row:
                self._err(503, 'NO_PORTS_AVAILABLE')
                return

            port = row['port']
            db.execute("UPDATE proxy_slots SET status = 'starting' WHERE port = ?", (port,))
            db.commit()

            # Spawn the GameServerProxy process (listen mode).
            log_file = os.path.join(config.LOG_DIR, f'proxy_{port}.log')
            cmd = [
                config.EE_BINARY,
                f'proxy=listen:{port}:{password}:{port}:{name}'
            ]

            try:
                with open(log_file, 'w') as f:
                    proc = subprocess.Popen(
                        cmd,
                        cwd=config.EE_WORK_DIR,
                        stdout=f,
                        stderr=subprocess.STDOUT,
                        stdin=subprocess.DEVNULL,
                        preexec_fn=os.setpgrp,
                    )
            except FileNotFoundError:
                db.execute(
                    "UPDATE proxy_slots SET status = 'free', last_error = 'EE_BINARY not found' WHERE port = ?",
                    (port,)
                )
                db.commit()
                self._err(500, 'FAILED_TO_START_PROXY')
                return
            except OSError as e:
                db.execute(
                    "UPDATE proxy_slots SET status = 'free', last_error = ? WHERE port = ?",
                    (str(e), port),
                )
                db.commit()
                self._err(500, 'FAILED_TO_START_PROXY')
                return

            pid = proc.pid

            # Wait briefly for the proxy TCP port to be reachable.
            ready = False
            for _ in range(config.PROXY_START_TIMEOUT):
                try:
                    with socket.create_connection(
                        (config.PROXY_PUBLIC_HOST, port), timeout=1
                    ):
                        ready = True
                        break
                except (OSError, socket.error):
                    pass
                time.sleep(1)

            if not ready:
                print(f"proxy_registry: port {port} (pid {pid}) not ready within timeout, continuing",
                      file=sys.stderr)

            server_ip = self.client_address[0]
            db.execute(
                """UPDATE proxy_slots SET
                    status = 'assigned',
                    pid = ?,
                    server_password = ?,
                    server_name = ?,
                    server_version = ?,
                    server_ip = ?,
                    assigned_at = datetime('now'),
                    last_heartbeat = datetime('now'),
                    last_error = NULL
                WHERE port = ?""",
                (pid, password, name, version, server_ip, port),
            )
            db.commit()

            self._ok(f'ASSIGNED {config.PROXY_PUBLIC_HOST} {port} {password}')

            # Register with the external master server (if configured).
            _master_register(port, name, version)
            with _assigned_lock:
                _assigned_slots[port] = {'name': name, 'version': version}
        finally:
            db.close()

    # ── deregister ────────────────────────────────────────────

    def handle_deregister(self, params: dict):
        password = self._get_param(params, 'password')
        port_raw = self._get_param(params, 'port')

        if not port_raw:
            self._err(400, 'Missing port')
            return
        if password != config.REGISTRY_PASSWORD:
            self._err(403, 'Invalid password')
            return

        port = int(port_raw)
        db = get_db()
        try:
            row = db.execute(
                "SELECT pid, status FROM proxy_slots WHERE port = ?", (port,)
            ).fetchone()

            if row and row['status'] == 'assigned':
                pid = row['pid']
                if pid:
                    try:
                        os.kill(pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass

                db.execute(
                    """UPDATE proxy_slots SET
                        status = 'free', pid = NULL,
                        server_password = NULL, server_name = NULL,
                        server_version = NULL, server_ip = NULL,
                        assigned_at = NULL, last_heartbeat = NULL,
                        last_error = NULL
                    WHERE port = ?""",
                    (port,),
                )
                db.commit()

            self._ok('OK')

            with _assigned_lock:
                _assigned_slots.pop(port, None)
        finally:
            db.close()

    # ── heartbeat ─────────────────────────────────────────────

    def handle_heartbeat(self, params: dict):
        password = self._get_param(params, 'password')
        port_raw = self._get_param(params, 'port')
        name = self._get_param(params, 'name', '')
        version_raw = self._get_param(params, 'version', '0')

        if not port_raw:
            self._err(400, 'Missing port')
            return
        if password != config.REGISTRY_PASSWORD:
            self._err(403, 'Invalid password')
            return

        port = int(port_raw)
        version = int(version_raw)

        db = get_db()
        try:
            row = db.execute(
                "SELECT status FROM proxy_slots WHERE port = ?", (port,)
            ).fetchone()

            if not row or row['status'] != 'assigned':
                self._err(404, 'NOT_ASSIGNED')
                return

            db.execute(
                """UPDATE proxy_slots SET
                    last_heartbeat = datetime('now'),
                    server_name = ?,
                    server_version = ?
                WHERE port = ?""",
                (name, version, port),
            )
            db.commit()

            self._ok('OK')

            with _assigned_lock:
                _assigned_slots[port] = {'name': name, 'version': version}
        finally:
            db.close()

    # ── list ──────────────────────────────────────────────────

    def handle_list(self):
        db = get_db()
        try:
            rows = db.execute(
                "SELECT port, server_version, server_name FROM proxy_slots WHERE status = 'assigned' ORDER BY port"
            ).fetchall()

            lines = '\n'.join(
                f'{config.PROXY_PUBLIC_HOST}:{r["port"]}:{r["server_version"]}:{r["server_name"]}'
                for r in rows
            )
            self._ok(lines + ('\n' if lines else ''))
        finally:
            db.close()

    # ── status (admin/debug) ──────────────────────────────────

    def handle_status(self):
        db = get_db()
        try:
            total = db.execute("SELECT COUNT(*) AS c FROM proxy_slots").fetchone()['c']
            free = db.execute("SELECT COUNT(*) AS c FROM proxy_slots WHERE status = 'free'").fetchone()['c']
            assigned = db.execute("SELECT COUNT(*) AS c FROM proxy_slots WHERE status = 'assigned'").fetchone()['c']
            starting = db.execute("SELECT COUNT(*) AS c FROM proxy_slots WHERE status = 'starting'").fetchone()['c']

            self._ok(
                f'slots_total: {total}\n'
                f'slots_free: {free}\n'
                f'slots_assigned: {assigned}\n'
                f'slots_starting: {starting}\n'
            )
        finally:
            db.close()

    def log_message(self, fmt, *args):
        print(f"[{self.log_date_time_string()}] {self.address_string()} - {fmt % args}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description='ProxyRegistry for EmptyEpsilon')
    parser.add_argument('--port', type=int, default=35668, help='HTTP listen port')
    parser.add_argument('--init-db', action='store_true', help='Initialise the database and exit')
    args = parser.parse_args()

    if args.init_db:
        init_db()
        return

    server = HTTPServer(('0.0.0.0', args.port), ProxyRegistryHandler)
    print(f'ProxyRegistry listening on http://0.0.0.0:{args.port}')

    if config.MASTER_SERVER_URL:
        thread = threading.Thread(target=_master_heartbeat_loop, daemon=True)
        thread.start()
        print(f'Master server registration enabled: {config.MASTER_SERVER_URL}')

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print('\nShutting down')
        server.server_close()


if __name__ == '__main__':
    main()
