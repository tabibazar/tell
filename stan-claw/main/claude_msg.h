#ifndef CLAUDE_MSG_H
#define CLAUDE_MSG_H

/*
 * The Messages API request stan-claw sends and the reply it reads. With an
 * MCP URL the request carries Anthropic's MCP connector (mcp_servers +
 * an mcp_toolset tool; the caller adds the anthropic-beta header). The reply's
 * text blocks become the answer; mcp_tool_use names are collected for the
 * screen. Pure: host_tests/test_sc_claude.c.
 */
#include <stdbool.h>

#define CLAUDE_TURNS 7
#define CLAUDE_SYSTEM "You are stan-claw, a voice assistant on a small screen. Answer in one to three short spoken sentences."

typedef struct {
    char user[600];
    char assistant[1500];
} claude_pair_t;

typedef struct {
    claude_pair_t pair[CLAUDE_TURNS];
    int n;
} claude_history_t;

typedef struct {
    char text[1500];       /* the answer, text blocks joined by a space */
    char tools[160];       /* "search, fetch": remote tools used, each once */
    bool mcp_error;        /* a remote tool call failed */
    char error[160];       /* why claude_parse returned false */
} claude_reply_t;

void claude_history_add(claude_history_t *h, const char *user, const char *assistant);
void claude_history_clear(claude_history_t *h);
char *claude_request(const char *model, const char *mcp_url, const char *mcp_token,
                     const claude_history_t *h, const char *question);
bool claude_parse(const char *json, claude_reply_t *out);
/* A refusal because of the remote MCP server: a 4xx whose error names it
   (an unreachable server is a 400, "mcp_servers[0] 'remote': Error while
   communicating with MCP server."). The caller asks again without it. */
bool claude_mcp_failed(int code, const claude_reply_t *r);

#endif /* CLAUDE_MSG_H */
