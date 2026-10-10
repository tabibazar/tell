#ifndef MCP_RPC_H
#define MCP_RPC_H

/*
 * stan-claw's MCP server, minus the HTTP: one JSON-RPC message in, one out
 * (protocol 2025-06-18, tools only). The tools themselves are callbacks the
 * device supplies; each returns false with a message for a tool error, which
 * goes back as isError, not as a protocol error. Pure: host_tests/test_sc_mcp.c.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MCP_IMAGE_MAX (200 * 1024)

typedef struct {
    bool (*speak)(const char *text, char *out, size_t n);
    bool (*listen)(int max_seconds, char *out, size_t n);
    bool (*ask)(const char *question, char *out, size_t n);
    bool (*show_text)(const char *title, const char *text, char *out, size_t n);
    bool (*show_image)(const uint8_t *jpeg, size_t len, char *out, size_t n);
    bool (*clear_screen)(char *out, size_t n);
    bool (*set_volume)(int level, char *out, size_t n);
    bool (*status)(char *out, size_t n);
} mcp_tools_t;

/* Returns the HTTP status: 200 with *resp, 202 with *resp NULL (a
   notification), 400 with *resp (unparseable or not a single request).
   *initialize says the message was `initialize`, for the session header. */
int mcp_rpc_handle(const mcp_tools_t *t, const char *body, size_t len, char **resp, bool *initialize);

/* "Bearer <token>", compared in constant time; an empty token admits no one. */
bool mcp_auth_ok(const char *authorization_header, const char *token);

/* Standard base64 into out; 0 for bad input or when it would pass max. */
size_t mcp_b64_decode(const char *in, uint8_t *out, size_t max);

#endif /* MCP_RPC_H */
