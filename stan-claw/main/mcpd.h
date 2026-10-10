#ifndef MCPD_H
#define MCPD_H
#include "mcp_rpc.h"
/* http://stan-claw.local/mcp: MCP over Streamable HTTP, one JSON answer per
   POST, a bearer token on every request (the "serve token" setting). */
void mcpd_start(const mcp_tools_t *tools);
#endif /* MCPD_H */
