#ifndef AGENT_H
#define AGENT_H
#include "claude_msg.h"
/* One exchange with Claude, the remote MCP server attached when one is set.
   Keeps the last seven exchanges (CLAUDE_TURNS) and what it has learnt about the
   person, on flash (memory.h): a conversation lasts until 04:00 the next day.
   [END] and [NOTE: ...] are taken out of the reply. 200 ok;
   0 key missing; otherwise the HTTP status (-1 no connection); out->error
   says more. If the remote MCP server makes Claude refuse the request, it
   asks again without the server and sets out->mcp_error. */
int agent_ask(const char *question, claude_reply_t *out);
#endif /* AGENT_H */
