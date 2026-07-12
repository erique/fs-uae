#!/usr/bin/env python3
"""
MCP bridge between Claude Code (stdio) and FS-UAE's MCP server (socket).

The bridge handles the MCP protocol itself (initialize, tools/list) so that
Claude Code always sees a healthy MCP server, even when FS-UAE isn't running.
When FS-UAE connects or disconnects, the bridge sends a tools/list_changed
notification so Claude Code refreshes the tool list automatically.

The bridge exposes one locally-served tool, `bridge_info`, which reports its
socket path and transport so clients can discover how to launch FS-UAE. Each
Claude Code session spawns its own bridge process; with no endpoint argument
the bridge derives a per-session Unix socket from its PID, so multiple agents
never collide on the same endpoint. Each session then launches its own FS-UAE
instance pointed at its bridge's endpoint.

Supports both Unix domain sockets and TCP:
  mcp_bridge.py                          Unix socket at /tmp/fs-uae-mcp-<pid>.sock
  mcp_bridge.py /tmp/fs-uae-mcp.sock     Unix socket at explicit path
  mcp_bridge.py tcp:6789                  TCP localhost
  mcp_bridge.py tcp:host:port             TCP remote
"""

import json
import os
import socket
import sys
import threading
import time


BRIDGE_INFO_TOOL = {
    "name": "bridge_info",
    "description": (
        "Returns information about the MCP bridge: its socket path / TCP endpoint, "
        "transport, connection status to FS-UAE, and the exact `mcp` config value to "
        "use when launching FS-UAE so it connects to this bridge's session. Call this "
        "before starting FS-UAE to discover the correct endpoint."
    ),
    "inputSchema": {
        "type": "object",
        "properties": {},
        "required": [],
    },
}


def parse_endpoint(endpoint):
    """Parse endpoint string into (socket_family, address) tuple."""
    if endpoint.startswith("tcp:"):
        rest = endpoint[4:]
        last_colon = rest.rfind(":")
        if last_colon >= 0:
            host = rest[:last_colon]
            port = int(rest[last_colon + 1:])
        else:
            host = "127.0.0.1"
            port = int(rest)
        return (socket.AF_INET, (host, port))
    else:
        return (socket.AF_UNIX, endpoint)


def log(msg):
    print(f"mcp_bridge: {msg}", file=sys.stderr, flush=True)


def send_to_client(obj):
    """Send a JSON-RPC message to Claude Code via stdout."""
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def jsonrpc_result(req_id, result):
    return {"jsonrpc": "2.0", "id": req_id, "result": result}


def jsonrpc_error(req_id, code, message):
    return {"jsonrpc": "2.0", "id": req_id, "error": {"code": code, "message": message}}


class FSUAEBridge:
    def __init__(self, endpoint):
        self.family, self.address = parse_endpoint(endpoint)
        self.endpoint = endpoint
        self.transport = "tcp" if endpoint.startswith("tcp:") else "unix"
        self.sock = None
        self.lock = threading.Lock()
        self.reader_thread = None
        self.monitor_thread = None
        self.fsuae_tools = []
        self.pending = {}  # id -> threading.Event, response
        self.pending_lock = threading.Lock()
        self.was_connected = False

    @property
    def cached_tools(self):
        return [BRIDGE_INFO_TOOL] + self.fsuae_tools

    def bridge_info(self):
        return {
            "transport": self.transport,
            "endpoint": self.endpoint,
            "mcp_config": self.endpoint,
            "connected": self.is_connected(),
            "bridge_pid": os.getpid(),
        }

    def connect(self):
        """Try to connect to FS-UAE. Returns True on success."""
        with self.lock:
            self._close_unlocked()
            try:
                s = socket.socket(self.family, socket.SOCK_STREAM)
                s.settimeout(2)
                s.connect(self.address)
                s.settimeout(None)
                self.sock = s
                return True
            except (ConnectionRefusedError, FileNotFoundError, OSError):
                return False

    def _close_unlocked(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def is_connected(self):
        with self.lock:
            return self.sock is not None

    def send(self, data):
        with self.lock:
            if not self.sock:
                return False
            try:
                self.sock.sendall(data)
                return True
            except OSError:
                self._close_unlocked()
                return False

    def send_request(self, method, params=None, timeout=10):
        """Send a JSON-RPC request to FS-UAE and wait for the response."""
        import random
        req_id = random.randint(100000, 999999)
        msg = {"jsonrpc": "2.0", "id": req_id, "method": method}
        if params is not None:
            msg["params"] = params

        event = threading.Event()
        entry = {"event": event, "response": None}
        with self.pending_lock:
            self.pending[req_id] = entry

        if not self.send((json.dumps(msg) + "\n").encode()):
            with self.pending_lock:
                del self.pending[req_id]
            return None

        event.wait(timeout=timeout)
        with self.pending_lock:
            entry = self.pending.pop(req_id, entry)
        return entry["response"]

    def _reader_loop(self):
        """Read from FS-UAE socket, dispatch responses and forward tool results."""
        buf = b""
        while True:
            with self.lock:
                sock = self.sock
            if not sock:
                break
            try:
                data = sock.recv(65536)
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        msg = json.loads(line)
                    except json.JSONDecodeError:
                        continue

                    msg_id = msg.get("id")
                    with self.pending_lock:
                        entry = self.pending.get(msg_id)
                    if entry is not None:
                        # internal request (tools/list fetch etc)
                        entry["response"] = msg
                        entry["event"].set()
                    else:
                        # proxied tool call response - forward to Claude Code
                        send_to_client(msg)
            except OSError:
                break

        with self.lock:
            self._close_unlocked()

    def start_reader(self):
        if self.reader_thread and self.reader_thread.is_alive():
            self.reader_thread.join(timeout=1)
        self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader_thread.start()

    def fetch_tools(self):
        """Fetch tool list from FS-UAE and cache it."""
        resp = self.send_request("tools/list")
        if resp and "result" in resp:
            tools = resp["result"].get("tools", [])
            self.fsuae_tools = tools
            log(f"fetched {len(tools)} tools from FS-UAE")
            return True
        return False

    def on_connected(self):
        """Called when FS-UAE connection is established."""
        log(f"connected to FS-UAE at {self.endpoint}")
        self.start_reader()
        # initialize the MCP session with FS-UAE
        self.send_request("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "mcp_bridge", "version": "1.0"}
        })
        self.send((json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n").encode())
        self.fetch_tools()
        self.was_connected = True
        # notify Claude Code that tools changed
        send_to_client({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})

    def on_disconnected(self):
        """Called when FS-UAE connection is lost."""
        if self.was_connected:
            log("FS-UAE disconnected")
            self.fsuae_tools = []
            self.was_connected = False
            # notify Claude Code that tools changed (now empty)
            send_to_client({"jsonrpc": "2.0", "method": "notifications/tools/list_changed"})

    def start_monitor(self):
        """Background thread that monitors connection to FS-UAE."""
        def monitor():
            while True:
                time.sleep(2)
                if not self.is_connected():
                    if self.was_connected:
                        self.on_disconnected()
                    if self.connect():
                        self.on_connected()

        self.monitor_thread = threading.Thread(target=monitor, daemon=True)
        self.monitor_thread.start()


def handle_bridge_info_call(bridge, req_id):
    """Serve a tools/call for bridge_info locally."""
    info = bridge.bridge_info()
    send_to_client(jsonrpc_result(req_id, {
        "content": [{"type": "text", "text": json.dumps(info, indent=2)}],
    }))


def main():
    if len(sys.argv) > 1:
        endpoint = sys.argv[1]
    else:
        endpoint = f"/tmp/fs-uae-mcp-{os.getpid()}.sock"

    bridge = FSUAEBridge(endpoint)

    # Structured marker for external discovery (e.g. `grep '^BRIDGE_INFO:'` in bridge stderr).
    print(f"BRIDGE_INFO: {json.dumps(bridge.bridge_info())}", file=sys.stderr, flush=True)

    # try initial connection
    if bridge.connect():
        bridge.on_connected()
    else:
        log(f"waiting for FS-UAE on {bridge.endpoint}...")

    # start background monitor for (re)connections
    bridge.start_monitor()

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue

        req_id = msg.get("id")
        method = msg.get("method", "")

        # handle MCP protocol locally
        if method == "initialize":
            send_to_client(jsonrpc_result(req_id, {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {"listChanged": True}},
                "serverInfo": {"name": "fs-uae-mcp", "version": "1.0.0"}
            }))
            continue

        if method == "notifications/initialized":
            # client ack, nothing to do
            continue

        if method == "tools/list":
            send_to_client(jsonrpc_result(req_id, {"tools": bridge.cached_tools}))
            continue

        if method == "tools/call":
            tool_name = msg.get("params", {}).get("name")
            if tool_name == "bridge_info":
                handle_bridge_info_call(bridge, req_id)
                continue

        # proxy everything else (tools/call etc) to FS-UAE
        if not bridge.is_connected():
            if req_id is not None:
                send_to_client(jsonrpc_error(req_id, -32000, "FS-UAE is not running"))
            continue

        if not bridge.send((json.dumps(msg) + "\n").encode()):
            # send failed, try reconnect
            if req_id is not None:
                send_to_client(jsonrpc_error(req_id, -32000, "FS-UAE connection lost"))
            continue
        # response will be forwarded by the reader thread


if __name__ == "__main__":
    main()
