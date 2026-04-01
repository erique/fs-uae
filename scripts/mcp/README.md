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
        "/path/to/fs-uae/scripts/mcp/mcp_bridge.py",
        "tcp:8372"
    ],
    "env": {}
}
```

For Unix sockets, use the socket path as the second argument:

```json
"fs-uae": {
    "type": "stdio",
    "command": "python3",
    "args": [
        "/path/to/fs-uae/scripts/mcp/mcp_bridge.py",
        "/tmp/fs-uae-mcp.sock"
    ],
    "env": {}
}
```

## Available tools

| Tool | Description |
|------|-------------|
| `machine_info` | Emulated Amiga info (model, CPU, memory sizes) |
| `cpu_registers` | 68k registers (D0-D7, A0-A7, PC, SR, USP, ISP, MSP, VBR) |
| `memory_read` | Read bytes from emulated memory (hex + ASCII) |
| `memory_write` | Write hex bytes to emulated memory |
| `memory_search` | Search memory for a byte pattern or text string |
| `disassemble` | Disassemble 68k instructions at an address |
| `debug_command` | Execute any FS-UAE debugger command |
| `debugger_break` | Pause the emulated Amiga |
| `debugger_run` | Resume execution |
| `screenshot` | Capture the emulated screen as base64-encoded PNG |

## Building

The MCP server requires libpng. Add `src/mcp_server.cpp` to the build and
link with `-lpng`. The server is compiled in when `mcp_server.h` is included
from `src/fs-uae/main.c`.
