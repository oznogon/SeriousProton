# proxyregistry: Dynamic reverse proxy allocation for SeriousProton

A standalone Python server that manages a pool of
[GameServerProxy](../src/multiplayer_proxy.h) instances.
Game servers behind NAT register via HTTP to be assigned a reverse proxy as a
public relay port, and clients connect to that port.

THe registry can optionally also register the spawned reverse proxy to a
[master server](../masterserver/) to allow clients to discover them.

## Architecture

- proxyregistry initializes a SQLite database on its host to manage reverse proxy slots
- GameServer (i.e. EmptyEpsilon server behind a NAT) sends POST to server.py /register with the proxyregistry's password
- proxyregistry allocates a port and spawns EmptyEpsilon at that port on the proxyregistry host as a reverse-proxy GameServerProxy
  - TODO: Spawn the version of EmptyEpsilon that matches the GameServer's version, or return an error if that version isn't available
- proxyregistry responds to the NAT GameServer, which reports "ASSIGNED host port pw"
- proxyregistry optionally sends POST to register the spawned reverse proxy with a SeriousProton-compatible master server
- Clients connect to the GameServerProxy on the assigned port of the proxyregistry host
- GameServer sends POST to proxyregistry's /heartbeat every 60s to retain its slot
- On GameServer disconnection from GameServerProxy, GameServerProxy sends POST to proxyregistry's /deregister to clear its slot

## Prerequisites

- Linux host with a public IP (or a reachable address)
- Python 3.8+
- EmptyEpsilon binary compiled for the host's architecture

## Quick start

1. Edit config.py to set `PROXY_PUBLIC_HOST`, `EE_BINARY`, `REGISTRY_PASSWORD`,
   and the port range (`PROXY_PORT_MIN` and `PROXY_PORT_MAX`). Optionally also
   set `MASTER_SERVER_URL` and `MASTER_HEARTBEAT_INTERVAL`.
2. Initialise the proxyregistry database, necessary on first run only:

   ```bash
   cd proxyregistry
   ./server.py --init-db
   ```
3. Start the proxyregistry server:

   ```bash
   ./server.py
   ```

   Or run in the background, or as a systemd service:

   ```bash
   nohup ./server.py --port 35668 > proxyregistry.log 2>&1 &
   ```
4. Add `proxy-manager` to cron for cleanup, i.e.:

   ```cron
   */5 * * * * /path/to/proxyregistry/proxy-manager
   ```
5. On the EmptyEpsilon GameServer process, set `proxy_registry_...` preferences
   to register it with `proxyregistry` and spawn a reverse proxy for it, i.e.:

   ```
   proxy_registry_url=http://proxy.example.com:35668
   proxy_registry_password=change-me
   ```

### systemd service example

```ini
[Unit]
Description=EmptyEpsilon ProxyRegistry
After=network.target

[Service]
Type=simple
User=nobody
ExecStart=/path/to/proxyregistry/server.py --port 35668
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

## HTTP API

### `POST /register`

GameServer requests a proxyregistry reverse proxy.

| Param | Required | Description |
| ----- | -------- | ----------- |
| `password` | yes | Shared secret from `config.py` |
| `name` | yes | Server display name |
| `version` | yes | Integer version number |

**Success (200):** `ASSIGNED <host> <port> <password>`
**Errors:** 400 (bad request), 403 (auth), 503 (no free ports), 500 (start failed)

### `POST /deregister`

Release a proxy slot (called on graceful server shutdown).

| Param | Required | Description |
| ----- | -------- | ----------- |
| `password` | yes | Shared secret |
| `port` | yes | Assigned proxy port |

**Success (200):** `OK`

### `POST /heartbeat`

Keep a proxy slot alive. Must be called at least every 120 seconds.

| Param | Required | Description |
| ----- | -------- | ----------- |
| `password` | yes | Shared secret |
| `port` | yes | Assigned proxy port |
| `name` | no  | Updated server name |
| `version` | no  | Updated version |

**Success (200):** `OK`
**Error (404):** `NOT_ASSIGNED`

### `GET /list`

List all active proxies. Same format as the master server
(`host:port:version:name`, one per line).

### `GET /status`

Pool health summary (text/plain):

```
slots_total: 101
slots_free: 82
slots_assigned: 19
slots_starting: 0
```

## Game server preferences

| Preference | Example | Description |
| ---------- | ------- | ----------- |
| `proxy_registry_url` | `http://proxy.example.com:35668` | Registry endpoint |
| `proxy_registry_password` | `change-me` | Shared secret |

## Cleanup

`proxy-manager` handles three stale conditions:

1. **Stale assigned**: If the GameServerProxy's alive but its GameServer hasn't
   sent a heartbeat for 120 seconds, kill the GameServerProxy and free up its
   proxyregistry slot.
2. **Stuck starting**: If `register.php` crashed after spawning but before
   marking its slot as assigned, kill the orphaned process and free up its slot.
3. **Pre-warming**: (optional, commented out) Keep a number of idle proxies
   pre-spawned for zero-wait assignment.
