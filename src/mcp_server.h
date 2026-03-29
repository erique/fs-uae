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

#ifdef __cplusplus
}
#endif

#endif
