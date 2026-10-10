/* stan-claw's Claude messages: the request, with and without the MCP
   connector, and the reply's text, MCP tool use and errors. */
#include "claude_msg.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    static claude_history_t h;
    claude_history_clear(&h);
    char *req = claude_request("claude-sonnet-5", "", "", &h, "", "What time is it?");
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
    req = claude_request("m", "https://mcp.example/mcp", "tok", &h, NULL, "And now?");
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

    req = claude_request("m", "https://mcp.example/mcp", "", &h, NULL, "x");     /* no token: no field */
    j = cJSON_Parse(req);
    CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(j, "mcp_servers"), 0), "authorization_token") == NULL);
    cJSON_Delete(j);
    free(req);

    for (int i = 0; i < 9; i++) { char u[8]; snprintf(u, sizeof u, "q%d", i); claude_history_add(&h, u, "a"); }
    CHECK(h.n == CLAUDE_TURNS && strcmp(h.pair[0].user, "q2") == 0);       /* the oldest dropped */

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
    CHECK(strcmp(r.text, "Done.") == 0);                                      /* "Let me check." before the tools is dropped */
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

    char t1[64] = "Goodbye, have a lovely evening! [END]";
    CHECK(claude_take_end(t1) && strcmp(t1, "Goodbye, have a lovely evening!") == 0);
    char t2[64] = "Your balance is $595.";
    CHECK(!claude_take_end(t2) && strcmp(t2, "Your balance is $595.") == 0);
    CHECK(claude_is_signoff("That's all, thanks."));
    CHECK(claude_is_signoff("No thanks, I'm done"));
    CHECK(claude_is_signoff("Bye!"));
    CHECK(claude_is_signoff("ok that is it"));
    CHECK(!claude_is_signoff("How do I stop my neighbor's dog from barking at night?"));   /* a question, not a sign-off */
    CHECK(!claude_is_signoff("Can you tell me about the bylaws?"));
    CHECK(!claude_is_signoff("byelaws for sheds"));
    CHECK(!claude_is_signoff(""));
    CHECK(strstr(CLAUDE_SYSTEM, "[END]") != NULL);

    /* Notes: taken out of the reply, kept as facts, sent with the system prompt. */
    static claude_facts_t f;
    f.n = 0;
    char r1[200] = "You're signed in. [NOTE: lives in Sheppard Center] [NOTE: email info@avriz.io]";
    CHECK(claude_take_notes(r1, &f) == 2 && strcmp(r1, "You're signed in.") == 0 && f.n == 2);
    char r2[200] = "Sure. [NOTE: Lives in Sheppard Center.]";              /* the same fact again */
    CHECK(claude_take_notes(r2, &f) == 1 && f.n == 2 && strcmp(r2, "Sure.") == 0);
    char r3[64] = "Odd [NOTE: never closed";
    claude_take_notes(r3, &f);
    CHECK(strcmp(r3, "Odd") == 0 && f.n == 2);                               /* a broken marker is dropped, not spoken */
    char line[400];
    claude_facts_line(&f, line, sizeof line);
    CHECK(strcmp(line, "lives in Sheppard Center; email info@avriz.io") == 0);
    req = claude_request("m", "", "", &h, line, "hi");
    j = cJSON_Parse(req);
    CHECK(strstr(cJSON_GetObjectItem(j, "system")->valuestring, "What you already know about the person: lives in Sheppard Center") != NULL);
    cJSON_Delete(j);
    free(req);
    char txt[800];
    claude_facts_text(&f, txt, sizeof txt);
    static claude_facts_t f2;
    claude_facts_parse(txt, &f2);
    CHECK(f2.n == 2 && strcmp(f2.fact[1], "email info@avriz.io") == 0);
    for (int i = 0; i < 40; i++) { char r[64]; snprintf(r, sizeof r, "x [NOTE: fact %d]", i); claude_take_notes(r, &f2); }
    CHECK(f2.n == FACTS_MAX && strcmp(f2.fact[FACTS_MAX - 1], "fact 39") == 0);   /* the oldest dropped */

    /* A conversation saved with its day, and read back. */
    claude_history_clear(&h);
    claude_history_add(&h, "When is the meeting?", "Thursday at 7 pm.");
    claude_history_add(&h, "Can I speak?", "Yes, sign up a day before.");
    char *js = claude_history_json(&h, 20261012);
    static claude_history_t h2;
    int day = 0;
    CHECK(js && claude_history_parse(js, &h2, &day) && day == 20261012 && h2.n == 2 && strcmp(h2.pair[1].assistant, "Yes, sign up a day before.") == 0);
    free(js);
    CHECK(!claude_history_parse("garbage", &h2, &day));

    /* The day turns at 04:00. */
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
    struct tm a1 = { .tm_year = 126, .tm_mon = 9, .tm_mday = 12, .tm_hour = 3, .tm_min = 59, .tm_isdst = -1 };
    struct tm a2 = a1; a2.tm_hour = 4; a2.tm_min = 0;
    CHECK(claude_day(mktime(&a1)) == 20261011 && claude_day(mktime(&a2)) == 20261012);

    printf(fails ? "%d FAILED\n" : "sc_claude: all passed\n", fails);
    return fails != 0;
}
