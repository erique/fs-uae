# FS-UAE MCP Server

FS-UAE includes a built-in [Model Context Protocol](https://modelcontextprotocol.io/)
(MCP) server that exposes the emulated Amiga for inspection and control via
JSON-RPC 2.0. This lets AI assistants like Claude Code read memory, take
screenshots, disassemble code, and drive the debugger.

## Enabling the MCP server

Add the `mcp` option to your FS-UAE config file, specifying a TCP endpoint
or Unix socket path:

```
# TCP on localhost (recommended, default port 8372)
mcp = tcp:8372

# TCP on all interfaces
mcp = tcp:0.0.0.0:8372

# Unix socket
mcp = /tmp/fs-uae-mcp.sock
```

The server starts when FS-UAE launches and stops when it exits.

## Connecting Claude Code

Claude Code speaks MCP over stdio, so a bridge script is needed to relay
between stdio and the FS-UAE server. The bridge is included at
`scripts/mcp/mcp_bridge.py`.

The bridge handles the MCP protocol itself (initialize, tools/list) so that
Claude Code always sees a healthy MCP server, even when FS-UAE isn't running.
When FS-UAE connects or disconnects, the bridge sends a `tools/list_changed`
notification so Claude Code refreshes the tool list automatically.

Add the following to your Claude Code config (`~/.claude.json`, under
`mcpServers`):

```json
"fs-uae": {
    "type": "stdio",
    "command": "python3",
    "args": [
        "/path/to/fs-uae/scripts/mcp/mcp_bridge.py"
    ],
    "env": {}
}
```

With no endpoint argument the bridge derives a per-session Unix socket from its
own PID (`/tmp/fs-uae-mcp-<pid>.sock`). This is what lets multiple concurrent
Claude Code sessions each drive their own FS-UAE instance (see
[Multiple concurrent agents](#multiple-concurrent-agents) below).

To pin the bridge to a fixed endpoint instead, pass it as the second argument —
a TCP endpoint or a Unix socket path:

```json
"args": [
    "/path/to/fs-uae/scripts/mcp/mcp_bridge.py",
    "tcp:8372"
]
```

```json
"args": [
    "/path/to/fs-uae/scripts/mcp/mcp_bridge.py",
    "/tmp/fs-uae-mcp.sock"
]
```

## Multiple concurrent agents

FS-UAE emulates a single machine, so one FS-UAE process serves exactly one MCP
client at a time. Concurrency is achieved by giving each Claude Code session its
own bridge process **and** its own FS-UAE instance, isolated by a unique
endpoint:

1. Configure the bridge with **no endpoint argument** (as above). Each session's
   bridge picks a distinct `/tmp/fs-uae-mcp-<pid>.sock`, so sessions never
   collide.
2. From the session, call the `bridge_info` tool to discover the endpoint. It
   returns the socket path and the exact value to set for the `mcp` config key,
   for example:

   ```json
   {
     "transport": "unix",
     "endpoint": "/tmp/fs-uae-mcp-12345.sock",
     "mcp_config": "/tmp/fs-uae-mcp-12345.sock",
     "connected": false,
     "bridge_pid": 12345
   }
   ```

3. Launch FS-UAE with `mcp = <endpoint>` (config file) pointed at that socket.
   The bridge auto-connects and its tool list becomes available in that session
   only.

Because the unique endpoint is baked into each FS-UAE launch, a session can find
and stop only its own FS-UAE (`pgrep -f fs-uae-mcp-<bridge_pid>`) without
disturbing sibling sessions. The bridge also prints a `BRIDGE_INFO: {...}` line
to stderr at startup for non-MCP discovery.

## Available tools

| Tool | Description |
|------|-------------|
| `bridge_info` | Bridge endpoint, transport, connection status, and the `mcp` config value to launch FS-UAE with (served locally by the bridge) |
| `machine_info` | Emulated Amiga info (model, CPU, memory sizes) |
| `cpu_registers` | 68k registers (D0-D7, A0-A7, PC, SR, USP, ISP, MSP, VBR); pass `register` to return a single value |
| `memory_read` | Read bytes from emulated memory; `format` selects `hex`, `ascii`, or `both` (default) |
| `memory_write` | Write hex bytes to emulated memory |
| `memory_search` | Search memory for a byte pattern or text string |
| `disassemble` | Disassemble 68k instructions at an address |
| `debug_command` | Execute any FS-UAE debugger command |
| `debugger_break` | Pause the emulated Amiga |
| `debugger_run` | Resume execution |
| `screenshot` | Capture the emulated screen; base64 PNG inline, or pass `path` to write a PNG file and return just the path (keeps the image out of context for a vision agent to read) |

## Building

The MCP server requires libpng. Add `src/mcp_server.cpp` to the build and
link with `-lpng`. The server is compiled in when `mcp_server.h` is included
from `src/fs-uae/main.c`.
