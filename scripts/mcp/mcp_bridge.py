#!/usr/bin/env python3
"""
Bridge between stdio (for Claude Code MCP) and an FS-UAE MCP server.
Reads JSON-RPC from stdin, forwards to socket. Reads responses from socket, writes to stdout.

Supports both Unix domain sockets and TCP:
  mcp_bridge.py /tmp/fs-uae-mcp.sock     Unix socket
  mcp_bridge.py tcp:6789                  TCP localhost
  mcp_bridge.py tcp:host:port             TCP remote

Resilient: waits for FS-UAE to appear, reconnects if FS-UAE restarts.
"""

import json
import socket
import sys
import threading
import time


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


class FSUAEBridge:
    def __init__(self, endpoint):
        self.family, self.address = parse_endpoint(endpoint)
        self.endpoint = endpoint
        self.sock = None
        self.lock = threading.Lock()
        self.reader_thread = None

    def connect(self):
        """Try to connect. Returns True on success."""
        with self.lock:
            self.close_unlocked()
            try:
                s = socket.socket(self.family, socket.SOCK_STREAM)
                s.connect(self.address)
                self.sock = s
                return True
            except (ConnectionRefusedError, FileNotFoundError, OSError):
                return False

    def close_unlocked(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def send(self, data):
        with self.lock:
            if not self.sock:
                return False
            try:
                self.sock.sendall(data)
                return True
            except OSError:
                self.close_unlocked()
                return False

    def socket_to_stdout(self):
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
                    if line.strip():
                        sys.stdout.write(line.decode() + "\n")
                        sys.stdout.flush()
            except OSError:
                break
        with self.lock:
            self.close_unlocked()

    def start_reader(self):
        if self.reader_thread and self.reader_thread.is_alive():
            self.reader_thread.join(timeout=1)
        self.reader_thread = threading.Thread(target=self.socket_to_stdout, daemon=True)
        self.reader_thread.start()

    def make_error_response(self, req_id, code, message):
        resp = {"jsonrpc": "2.0", "id": req_id, "error": {"code": code, "message": message}}
        sys.stdout.write(json.dumps(resp) + "\n")
        sys.stdout.flush()


def main():
    if len(sys.argv) < 2:
        print("Usage: mcp_bridge.py <endpoint>", file=sys.stderr)
        print("  endpoint: /path/to/socket | tcp:port | tcp:host:port", file=sys.stderr)
        sys.exit(1)

    bridge = FSUAEBridge(sys.argv[1])

    if bridge.connect():
        bridge.start_reader()
        print(f"mcp_bridge: connected to FS-UAE at {bridge.endpoint}", file=sys.stderr)
    else:
        print(f"mcp_bridge: waiting for FS-UAE on {bridge.endpoint}...", file=sys.stderr)

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        req_id = None
        try:
            msg = json.loads(line)
            req_id = msg.get("id")
        except json.JSONDecodeError:
            pass

        with bridge.lock:
            connected = bridge.sock is not None
        if not connected:
            if not bridge.connect():
                if req_id is not None:
                    bridge.make_error_response(req_id, -32000, "FS-UAE is not running")
                continue
            bridge.start_reader()
            print(f"mcp_bridge: connected to FS-UAE at {bridge.endpoint}", file=sys.stderr)

        if not bridge.send((line + "\n").encode()):
            if bridge.connect():
                bridge.start_reader()
                print(f"mcp_bridge: reconnected to FS-UAE at {bridge.endpoint}", file=sys.stderr)
                if not bridge.send((line + "\n").encode()):
                    if req_id is not None:
                        bridge.make_error_response(req_id, -32000, "Failed to send to FS-UAE")
            else:
                if req_id is not None:
                    bridge.make_error_response(req_id, -32000, "FS-UAE is not running")


if __name__ == "__main__":
    main()
