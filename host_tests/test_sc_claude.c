/* stan-claw's Claude messages: the request, with and without the MCP
   connector, and the reply's text, MCP tool use and errors. */
#include "claude_msg.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    static claude_history_t h;
    claude_history_clear(&h);
    char *req = claude_request("claude-sonnet-5", "", "", &h, "What time is it?");
    cJSON *j = cJSON_Parse(req);
    CHECK(j != NULL);
    CHECK(strcmp(cJSON_GetObjectItem(j, "model")->valuestring, "claude-sonnet-5") == 0);
    CHECK(cJSON_GetObjectItem(j, "max_tokens")->valueint == 1024);
    CHECK(strcmp(cJSON_GetObjectItem(j, "system")->valuestring, CLAUDE_SYSTEM) == 0);
    CHECK(cJSON_GetObjectItem(j, "mcp_servers") == NULL && cJSON_GetObjectItem(j, "tools") == NULL);
    cJSON *msgs = cJSON_GetObjectItem(j, "messages");
    CHECK(cJSON_GetArraySize(msgs) == 1);
    CHECK(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(msgs, 0), "content")->valuestring, "What time is it?") == 0);
    cJSON_Delete(j);
    free(req);

    claude_history_add(&h, "Hi", "Hello!");
    req = claude_request("m", "https://mcp.example/mcp", "tok", &h, "And now?");
    j = cJSON_Parse(req);
    cJSON *srv = cJSON_GetArrayItem(cJSON_GetObjectItem(j, "mcp_servers"), 0);
    CHECK(srv && strcmp(cJSON_GetObjectItem(srv, "type")->valuestring, "url") == 0);
    CHECK(strcmp(cJSON_GetObjectItem(srv, "url")->valuestring, "https://mcp.example/mcp") == 0);
    CHECK(strcmp(cJSON_GetObjectItem(srv, "name")->valuestring, "remote") == 0);
    CHECK(strcmp(cJSON_GetObjectItem(srv, "authorization_token")->valuestring, "tok") == 0);
    cJSON *tool = cJSON_GetArrayItem(cJSON_GetObjectItem(j, "tools"), 0);
    CHECK(tool && strcmp(cJSON_GetObjectItem(tool, "type")->valuestring, "mcp_toolset") == 0);
    CHECK(strcmp(cJSON_GetObjectItem(tool, "mcp_server_name")->valuestring, "remote") == 0);
    msgs = cJSON_GetObjectItem(j, "messages");
    CHECK(cJSON_GetArraySize(msgs) == 3);
    CHECK(strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(msgs, 1), "role")->valuestring, "assistant") == 0);
    cJSON_Delete(j);
    free(req);

    req = claude_request("m", "https://mcp.example/mcp", "", &h, "x");     /* no token: no field */
    j = cJSON_Parse(req);
    CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(j, "mcp_servers"), 0), "authorization_token") == NULL);
    cJSON_Delete(j);
    free(req);

    for (int i = 0; i < 9; i++) { char u[8]; snprintf(u, sizeof u, "q%d", i); claude_history_add(&h, u, "a"); }
    CHECK(h.n == CLAUDE_TURNS && strcmp(h.pair[0].user, "q3") == 0);       /* the oldest dropped */

    claude_reply_t r;
    CHECK(claude_parse("{\"type\":\"message\",\"content\":[{\"type\":\"text\",\"text\":\"It is noon.\"}],\"stop_reason\":\"end_turn\"}", &r));
    CHECK(strcmp(r.text, "It is noon.") == 0 && r.tools[0] == 0 && !r.mcp_error);

    const char *withmcp =
        "{\"type\":\"message\",\"content\":["
        "{\"type\":\"text\",\"text\":\"Let me check.\"},"
        "{\"type\":\"mcp_tool_use\",\"id\":\"t1\",\"name\":\"search\",\"server_name\":\"remote\",\"input\":{}},"
        "{\"type\":\"mcp_tool_result\",\"tool_use_id\":\"t1\",\"is_error\":false,\"content\":[{\"type\":\"text\",\"text\":\"...\"}]},"
        "{\"type\":\"mcp_tool_use\",\"id\":\"t2\",\"name\":\"search\",\"server_name\":\"remote\",\"input\":{}},"
        "{\"type\":\"mcp_tool_use\",\"id\":\"t3\",\"name\":\"fetch\",\"server_name\":\"remote\",\"input\":{}},"
        "{\"type\":\"mcp_tool_result\",\"tool_use_id\":\"t3\",\"is_error\":true,\"content\":[]},"
        "{\"type\":\"text\",\"text\":\"Done.\"}]}";
    CHECK(claude_parse(withmcp, &r));
    CHECK(strcmp(r.text, "Let me check. Done.") == 0);
    CHECK(strcmp(r.tools, "search, fetch") == 0);                           /* each name once */
    CHECK(r.mcp_error);

    CHECK(!claude_parse("{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}", &r));
    CHECK(strcmp(r.error, "Overloaded") == 0);
    CHECK(!claude_parse("<html>", &r) && r.error[0]);
    CHECK(!claude_parse("{\"type\":\"message\",\"content\":[]}", &r));       /* nothing to say */

    /* The Messages API's answer (seen live) when the MCP server can't be reached. */
    CHECK(!claude_parse("{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\","
                        "\"message\":\"mcp_servers[0] 'remote': Error while communicating with MCP server.\"}}", &r));
    CHECK(claude_mcp_failed(400, &r));
    CHECK(!claude_mcp_failed(200, &r) && !claude_mcp_failed(529, &r) && !claude_mcp_failed(-1, &r));
    CHECK(!claude_parse("{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"invalid x-api-key\"}}", &r));
    CHECK(!claude_mcp_failed(401, &r));                                      /* a bad key is not the server */

    printf(fails ? "%d FAILED\n" : "sc_claude: all passed\n", fails);
    return fails != 0;
}
