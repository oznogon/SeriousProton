import os

# Publicly reachable hostname/IP of this proxy server.
# Clients and game servers connect here to reach assigned proxies
# by passing the address as the proxy_registry_url preference,
# along with the service's port (35668 by default).
PROXY_PUBLIC_HOST = '127.0.0.1'

# Proxy port range (inclusive).
PROXY_PORT_MIN = 40000
PROXY_PORT_MAX = 40100

# Absolute path to the EmptyEpsilon binary.
# EE_BINARY = '/path/to/binary/EmptyEpsilon'

# Working directory for the spawned GameServerProxy process.
# EmptyEpsilon loads resources (textures, scripts, etc.) relative to this
# path. Usually the directory of the binary, or the EmptyEpsilon repository
# root when building from source.
# EE_WORK_DIR = '/path/to/directory/EmptyEpsilon'

# Shared secret that game servers must present to register
# as the proxy_registery_password preference value.
# This is also used as the server password for clients.
REGISTRY_PASSWORD = 'change-me'

# Seconds without a heartbeat before a proxy slot is considered stale.
HEARTBEAT_TIMEOUT = 120

# Absolute path to the database directory (must be writable by the
# server process user). Defaults to the data subdirectory of server.py's
# location.
DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data')

# Absolute path to the log directory (writable by the server process user).
# Defaults to the logs subdirectory of the data directory.
LOG_DIR = os.path.join(DATA_DIR, 'logs')

# Maximum seconds to wait for a spawned proxy process to start listening.
# Increase this if your host takes a long time to launch EmptyEpsilon.
PROXY_START_TIMEOUT = 5

# Master server registration URL (optional, empty = disabled).
# If set, the proxy registry will register each assigned proxy slot on this
# master server so proxied servers appear in the public server list.
# This lists your proxy server in the server browser of _all_ EmptyEpsilon
# clients launched with the same server registry URL and version number!
# Example: 'http://daid.eu/ee/register.php'
MASTER_SERVER_URL = ''

# Seconds between master server heartbeat POSTs for each assigned slot.
MASTER_HEARTBEAT_INTERVAL = 60
