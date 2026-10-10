#include "mcp_rpc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#define OUT_MAX 2048

static const char TOOLS[] =
    "[{\"name\":\"speak\",\"description\":\"Say text aloud through stan-claw's speaker. Returns when it has finished speaking.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"maxLength\":1000}},\"required\":[\"text\"]}},"
    "{\"name\":\"listen\",\"description\":\"Open the microphones until the person stops talking (or max_seconds) and return what they said as text. A red LISTENING banner shows meanwhile.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"max_seconds\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":30,\"default\":10}}}},"
    "{\"name\":\"ask\",\"description\":\"Speak a question, then listen and return the spoken answer as text.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"question\":{\"type\":\"string\",\"maxLength\":500}},\"required\":[\"question\"]}},"
    "{\"name\":\"show_text\",\"description\":\"Show text (and an optional title) on stan-claw's 480x480 screen until a tap or clear_screen.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"maxLength\":2000},\"title\":{\"type\":\"string\",\"maxLength\":60}},\"required\":[\"text\"]}},"
    "{\"name\":\"show_image\",\"description\":\"Show a baseline JPEG (base64, at most 480x480 and 200 KB) on the screen until a tap or clear_screen.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"jpeg_base64\":{\"type\":\"string\"}},\"required\":[\"jpeg_base64\"]}},"
    "{\"name\":\"clear_screen\",\"description\":\"Return the screen to stan-claw's home screen.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
    "{\"name\":\"set_volume\",\"description\":\"Set the speaker volume, 0 to 100.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"level\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100}},\"required\":[\"level\"]}},"
    "{\"name\":\"status\",\"description\":\"WiFi network, IP address, volume, and whether stan-claw is busy.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}]";

static char *finish(cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

static cJSON *envelope(const cJSON *id)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "jsonrpc", "2.0");
    cJSON_AddItemToObject(o, "id", id ? cJSON_Duplicate(id, 1) : cJSON_CreateNull());
    return o;
}

static char *error(const cJSON *id, int code, const char *msg)
{
    cJSON *o = envelope(id);
    cJSON *e = cJSON_AddObjectToObject(o, "error");
    cJSON_AddNumberToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg);
    return finish(o);
}

static char *tool_result(const cJSON *id, bool ok, const char *text)
{
    cJSON *o = envelope(id);
    cJSON *r = cJSON_AddObjectToObject(o, "result");
    cJSON *content = cJSON_AddArrayToObject(r, "content");
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "type", "text");
    cJSON_AddStringToObject(t, "text", text);
    cJSON_AddItemToArray(content, t);
    if (!ok) cJSON_AddBoolToObject(r, "isError", true);
    return finish(o);
}

static const char *str_arg(const cJSON *args, const char *name, size_t max)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(args, name);
    if (!cJSON_IsString(v) || strlen(v->valuestring) > max) return NULL;
    return v->valuestring;
}

static char *call_tool(const mcp_tools_t *t, const cJSON *id, const cJSON *params)
{
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(params, "name");
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    if (!cJSON_IsString(name)) return error(id, -32602, "tools/call needs a name");
    static char out[OUT_MAX];
    out[0] = 0;
    const char *n = name->valuestring;
    bool ok;
    if (strcmp(n, "speak") == 0) {
        const char *text = str_arg(args, "text", 1000);
        if (!text || !text[0]) return tool_result(id, false, "speak needs text (1 to 1000 characters)");
        ok = t->speak(text, out, sizeof out);
    } else if (strcmp(n, "listen") == 0) {
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(args, "max_seconds");
        int secs = cJSON_IsNumber(s) ? s->valueint : 10;
        if (secs < 1 || secs > 30) return tool_result(id, false, "max_seconds is 1 to 30");
        ok = t->listen(secs, out, sizeof out);
    } else if (strcmp(n, "ask") == 0) {
        const char *q = str_arg(args, "question", 500);
        if (!q || !q[0]) return tool_result(id, false, "ask needs a question (1 to 500 characters)");
        ok = t->ask(q, out, sizeof out);
    } else if (strcmp(n, "show_text") == 0) {
        const char *text = str_arg(args, "text", 2000);
        const cJSON *ti = cJSON_GetObjectItemCaseSensitive(args, "title");
        if (!text) return tool_result(id, false, "show_text needs text (up to 2000 characters)");
        if (ti && (!cJSON_IsString(ti) || strlen(ti->valuestring) > 60)) return tool_result(id, false, "title is up to 60 characters");
        ok = t->show_text(ti ? ti->valuestring : NULL, text, out, sizeof out);
    } else if (strcmp(n, "show_image") == 0) {
        const char *b64 = str_arg(args, "jpeg_base64", MCP_IMAGE_MAX * 4 / 3 + 4);
        if (!b64) return tool_result(id, false, "show_image needs jpeg_base64 (at most 200 KB of JPEG)");
        uint8_t *jpg = malloc(MCP_IMAGE_MAX);
        if (!jpg) return tool_result(id, false, "out of memory");
        size_t len = mcp_b64_decode(b64, jpg, MCP_IMAGE_MAX);
        if (len < 4 || jpg[0] != 0xFF || jpg[1] != 0xD8) {
            free(jpg);
            return tool_result(id, false, "that is not a JPEG (or not valid base64)");
        }
        ok = t->show_image(jpg, len, out, sizeof out);
        free(jpg);
    } else if (strcmp(n, "clear_screen") == 0) {
        ok = t->clear_screen(out, sizeof out);
    } else if (strcmp(n, "set_volume") == 0) {
        const cJSON *l = cJSON_GetObjectItemCaseSensitive(args, "level");
        if (!cJSON_IsNumber(l) || l->valueint < 0 || l->valueint > 100) return tool_result(id, false, "level is 0 to 100");
        ok = t->set_volume(l->valueint, out, sizeof out);
    } else if (strcmp(n, "status") == 0) {
        ok = t->status(out, sizeof out);
    } else {
        return error(id, -32602, "no such tool");
    }
    return tool_result(id, ok, out);
}

int mcp_rpc_handle(const mcp_tools_t *t, const char *body, size_t len, char **resp, bool *initialize)
{
    *resp = NULL;
    *initialize = false;
    cJSON *m = cJSON_ParseWithLength(body, len);
    if (!m) { *resp = error(NULL, -32700, "parse error"); return 400; }
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(m, "method");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(m, "id");
    if (!cJSON_IsObject(m) || !cJSON_IsString(method)) {
        *resp = error(cJSON_IsObject(m) ? id : NULL, -32600, "not a single JSON-RPC request");
        cJSON_Delete(m);
        return 400;
    }
    int code = 200;
    const char *meth = method->valuestring;
    if (!id) {
        code = 202;                                   /* notifications and responses: accepted, no body */
    } else if (strcmp(meth, "initialize") == 0) {
        *initialize = true;
        cJSON *o = envelope(id);
        cJSON *r = cJSON_AddObjectToObject(o, "result");
        cJSON_AddStringToObject(r, "protocolVersion", "2025-06-18");
        cJSON *caps = cJSON_AddObjectToObject(r, "capabilities");
        cJSON *tools = cJSON_AddObjectToObject(caps, "tools");
        cJSON_AddBoolToObject(tools, "listChanged", false);
        cJSON *info = cJSON_AddObjectToObject(r, "serverInfo");
        cJSON_AddStringToObject(info, "name", "stan-claw");
        cJSON_AddStringToObject(info, "version", "0.1.0");
        *resp = finish(o);
    } else if (strcmp(meth, "ping") == 0) {
        cJSON *o = envelope(id);
        cJSON_AddObjectToObject(o, "result");
        *resp = finish(o);
    } else if (strcmp(meth, "tools/list") == 0) {
        cJSON *o = envelope(id);
        cJSON *r = cJSON_AddObjectToObject(o, "result");
        cJSON_AddItemToObject(r, "tools", cJSON_Parse(TOOLS));
        *resp = finish(o);
    } else if (strcmp(meth, "tools/call") == 0) {
        *resp = call_tool(t, id, cJSON_GetObjectItemCaseSensitive(m, "params"));
    } else {
        *resp = error(id, -32601, "method not found");
    }
    cJSON_Delete(m);
    return code;
}

bool mcp_auth_ok(const char *h, const char *token)
{
    if (!h || !token || !token[0] || strncmp(h, "Bearer ", 7) != 0) return false;
    const char *given = h + 7;
    size_t a = strlen(given), b = strlen(token);
    unsigned diff = (unsigned)(a ^ b);
    for (size_t i = 0; i < b; i++) diff |= (unsigned char)(i < a ? given[i] : 0) ^ (unsigned char)token[i];
    return diff == 0;
}

static int b64v(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

size_t mcp_b64_decode(const char *in, uint8_t *out, size_t max)
{
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (const char *p = in; *p; p++) {
        if (*p == '=') break;
        if (*p == '\n' || *p == '\r') continue;
        int v = b64v(*p);
        if (v < 0) return 0;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o == max) return 0;
            out[o++] = (uint8_t)(acc >> bits);
        }
    }
    return o;
}
