/*
 * MCP (Model Context Protocol) server for FS-UAE.
 * Provides JSON-RPC 2.0 introspection via Unix domain socket or TCP.
 */

#ifndef MCP_SERVER_H
#define MCP_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif

void mcp_start(const char* endpoint);
void mcp_stop(void);

// Called by console_get to check for MCP-injected debugger commands.
// Returns 1 if a command was available (copied into buf), 0 otherwise.
int mcp_console_get(char* buf, int maxlen);

// Called by the debugger when it breaks (breakpoint, step, watchpoint, etc).
// Sends a notification to the MCP client with break reason and CPU state.
void mcp_debugger_notify(void);

#ifdef __cplusplus
}
#endif

#endif
