#include "claude_msg.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

void claude_history_clear(claude_history_t *h) { h->n = 0; }

void claude_history_add(claude_history_t *h, const char *user, const char *assistant)
{
    if (h->n == CLAUDE_TURNS) {
        memmove(&h->pair[0], &h->pair[1], sizeof h->pair[0] * (CLAUDE_TURNS - 1));
        h->n--;
    }
    snprintf(h->pair[h->n].user, sizeof h->pair[0].user, "%s", user);
    snprintf(h->pair[h->n].assistant, sizeof h->pair[0].assistant, "%s", assistant);
    h->n++;
}

static void add_msg(cJSON *msgs, const char *role, const char *text)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "content", text);
    cJSON_AddItemToArray(msgs, m);
}

char *claude_request(const char *model, const char *mcp_url, const char *mcp_token,
                     const claude_history_t *h, const char *question)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "model", model);
    cJSON_AddNumberToObject(o, "max_tokens", 1024);
    cJSON_AddStringToObject(o, "system", CLAUDE_SYSTEM);
    if (mcp_url && mcp_url[0]) {
        cJSON *servers = cJSON_AddArrayToObject(o, "mcp_servers");
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "type", "url");
        cJSON_AddStringToObject(s, "url", mcp_url);
        cJSON_AddStringToObject(s, "name", "remote");
        if (mcp_token && mcp_token[0]) cJSON_AddStringToObject(s, "authorization_token", mcp_token);
        cJSON_AddItemToArray(servers, s);
        cJSON *tools = cJSON_AddArrayToObject(o, "tools");
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", "mcp_toolset");
        cJSON_AddStringToObject(t, "mcp_server_name", "remote");
        cJSON_AddItemToArray(tools, t);
    }
    cJSON *msgs = cJSON_AddArrayToObject(o, "messages");
    for (int i = 0; i < h->n; i++) {
        add_msg(msgs, "user", h->pair[i].user);
        add_msg(msgs, "assistant", h->pair[i].assistant);
    }
    add_msg(msgs, "user", question);
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

static void append(char *dst, size_t n, const char *sep, const char *s)
{
    size_t l = strlen(dst);
    if (l && l + strlen(sep) < n) { strncat(dst, sep, n - l - 1); l = strlen(dst); }
    strncat(dst, s, n - l - 1);
}

bool claude_parse(const char *json, claude_reply_t *out)
{
    memset(out, 0, sizeof *out);
    cJSON *j = cJSON_Parse(json);
    if (!j) { snprintf(out->error, sizeof out->error, "unreadable reply"); return false; }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(j, "type");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "error") == 0) {
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "error"), "message");
        snprintf(out->error, sizeof out->error, "%s", cJSON_IsString(msg) ? msg->valuestring : "Claude error");
        cJSON_Delete(j);
        return false;
    }
    const cJSON *b;
    cJSON_ArrayForEach(b, cJSON_GetObjectItemCaseSensitive(j, "content")) {
        const cJSON *bt = cJSON_GetObjectItemCaseSensitive(b, "type");
        if (!cJSON_IsString(bt)) continue;
        if (strcmp(bt->valuestring, "text") == 0) {
            const cJSON *t = cJSON_GetObjectItemCaseSensitive(b, "text");
            if (cJSON_IsString(t) && t->valuestring[0]) append(out->text, sizeof out->text, " ", t->valuestring);
        } else if (strcmp(bt->valuestring, "mcp_tool_use") == 0) {
            const cJSON *nm = cJSON_GetObjectItemCaseSensitive(b, "name");
            if (!cJSON_IsString(nm)) continue;
            /* Each name once: look for it as a whole item of the list. */
            char want[80];
            snprintf(want, sizeof want, ", %s,", nm->valuestring);
            char have[sizeof out->tools + 4];
            snprintf(have, sizeof have, ", %s,", out->tools);
            if (!strstr(have, want)) append(out->tools, sizeof out->tools, ", ", nm->valuestring);
        } else if (strcmp(bt->valuestring, "mcp_tool_result") == 0) {
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(b, "is_error"))) out->mcp_error = true;
        }
    }
    cJSON_Delete(j);
    if (!out->text[0]) { snprintf(out->error, sizeof out->error, "Claude said nothing"); return false; }
    return true;
}
