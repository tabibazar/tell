#ifndef MCPD_H
#define MCPD_H
#include "mcp_rpc.h"
/* http://stan-claw.local/mcp: MCP over Streamable HTTP, one JSON answer per
   POST, a bearer token on every request (the "serve token" setting). */
void mcpd_start(const mcp_tools_t *tools);

/* Called on every request that carries the right token: an agent at work counts as use. */
void mcpd_on_request(void (*cb)(void));
#endif /* MCPD_H */
