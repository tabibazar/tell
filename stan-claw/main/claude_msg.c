#include "claude_msg.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
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
                     const claude_history_t *h, const char *facts, const char *question)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "model", model);
    cJSON_AddNumberToObject(o, "max_tokens", 1024);
    if (facts && facts[0]) {
        size_t n = strlen(CLAUDE_SYSTEM) + strlen(facts) + 64;
        char *sys = malloc(n);
        if (sys) {
            snprintf(sys, n, "%s What you already know about the person: %s", CLAUDE_SYSTEM, facts);
            cJSON_AddStringToObject(o, "system", sys);
            free(sys);
        }
    } else {
        cJSON_AddStringToObject(o, "system", CLAUDE_SYSTEM);
    }
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
            out->text[0] = 0;              /* what came before a tool call was "let me check": not the answer */
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

bool claude_mcp_failed(int code, const claude_reply_t *r)
{
    if (code < 400 || code > 499) return false;
    for (const char *p = r->error; p[0] && p[1] && p[2]; p++)
        if (tolower((unsigned char)p[0]) == 'm' && tolower((unsigned char)p[1]) == 'c' && tolower((unsigned char)p[2]) == 'p')
            return true;
    return false;
}

bool claude_take_end(char *text)
{
    char *m = strstr(text, "[END]");
    if (!m) return false;
    memmove(m, m + 5, strlen(m + 5) + 1);
    size_t l = strlen(text);
    while (l && (text[l - 1] == ' ' || text[l - 1] == '\n')) text[--l] = 0;
    return true;
}

bool claude_is_signoff(const char *heard)
{
    /* Lower case, letters and spaces only, then whole-phrase checks: a short
       utterance that is (or ends with) a sign-off, not one that merely
       contains "stop" somewhere in a question. */
    char t[160];
    size_t o = 0;
    for (const char *p = heard; *p && o + 1 < sizeof t; p++) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || c == ' ') t[o++] = c;
        else if (c == '\'') continue;
        else t[o++] = ' ';
    }
    t[o] = 0;
    int words = 0;
    for (char *p = t; *p; p++) if (*p != ' ' && (p == t || p[-1] == ' ')) words++;
    if (words == 0 || words > 8) return false;
    static const char *const ends[] = { "thats all", "that is all", "thats it", "that is it", "goodbye", "bye",
                                        "im done", "i am done", "were done", "nothing else", "no thanks",
                                        "no thank you", "stop", "see you", "talk later", "never mind" };
    for (size_t i = 0; i < sizeof ends / sizeof ends[0]; i++) {
        const char *hit = strstr(t, ends[i]);
        if (!hit) continue;
        const char *after = hit + strlen(ends[i]);
        bool start_ok = hit == t || hit[-1] == ' ';
        bool end_ok = *after == 0 || *after == ' ';
        if (start_ok && end_ok) return true;
    }
    return false;
}

static bool same_fact(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return false;
    }
    return *a == *b;
}

static void add_fact(claude_facts_t *f, const char *s)
{
    char t[FACT_LEN];
    while (*s == ' ') s++;
    snprintf(t, sizeof t, "%s", s);
    size_t l = strlen(t);
    while (l && (t[l - 1] == ' ' || t[l - 1] == '.' || t[l - 1] == '\n')) t[--l] = 0;
    if (!t[0]) return;
    for (int i = 0; i < f->n; i++) if (same_fact(f->fact[i], t)) return;
    if (f->n == FACTS_MAX) {
        memmove(f->fact[0], f->fact[1], sizeof f->fact[0] * (FACTS_MAX - 1));
        f->n--;
    }
    snprintf(f->fact[f->n++], FACT_LEN, "%s", t);
}

int claude_take_notes(char *text, claude_facts_t *f)
{
    int count = 0;
    char *m;
    while ((m = strstr(text, "[NOTE:")) != NULL) {
        char *e = strchr(m, ']');
        char *stop = e ? e + 1 : m + strlen(m);
        if (e) {
            char note[FACT_LEN];
            size_t len = (size_t)(e - (m + 6));
            if (len >= sizeof note) len = sizeof note - 1;
            memcpy(note, m + 6, len);
            note[len] = 0;
            add_fact(f, note);
            count++;
        }
        memmove(m, stop, strlen(stop) + 1);
    }
    size_t l = strlen(text);
    while (l && (text[l - 1] == ' ' || text[l - 1] == '\n')) text[--l] = 0;
    return count;
}

void claude_facts_line(const claude_facts_t *f, char *out, size_t n)
{
    out[0] = 0;
    size_t o = 0;
    for (int i = 0; i < f->n && o + 1 < n; i++)
        o += (size_t)snprintf(out + o, n - o, "%s%s", i ? "; " : "", f->fact[i]);
}

void claude_facts_text(const claude_facts_t *f, char *out, size_t n)
{
    out[0] = 0;
    size_t o = 0;
    for (int i = 0; i < f->n && o + 1 < n; i++) o += (size_t)snprintf(out + o, n - o, "%s\n", f->fact[i]);
}

void claude_facts_parse(const char *text, claude_facts_t *f)
{
    f->n = 0;
    char line[FACT_LEN];
    size_t o = 0;
    for (const char *p = text; ; p++) {
        if (*p == '\n' || *p == 0) {
            line[o] = 0;
            if (o) add_fact(f, line);
            o = 0;
            if (!*p) break;
        } else if (o + 1 < sizeof line) {
            line[o++] = *p;
        }
    }
}

char *claude_history_json(const claude_history_t *h, int day)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "day", day);
    cJSON *arr = cJSON_AddArrayToObject(o, "pairs");
    for (int i = 0; i < h->n; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "u", h->pair[i].user);
        cJSON_AddStringToObject(p, "a", h->pair[i].assistant);
        cJSON_AddItemToArray(arr, p);
    }
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

bool claude_history_parse(const char *json, claude_history_t *h, int *day)
{
    cJSON *o = cJSON_Parse(json);
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(o, "day");
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(o, "pairs");
    if (!cJSON_IsNumber(d) || !cJSON_IsArray(arr)) { cJSON_Delete(o); return false; }
    claude_history_clear(h);
    const cJSON *p;
    cJSON_ArrayForEach(p, arr) {
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(p, "u"), *a = cJSON_GetObjectItemCaseSensitive(p, "a");
        if (cJSON_IsString(u) && cJSON_IsString(a)) claude_history_add(h, u->valuestring, a->valuestring);
    }
    *day = d->valueint;
    cJSON_Delete(o);
    return true;
}

int claude_day(time_t t)
{
    time_t s = t - 4 * 3600;
    struct tm lt;
    localtime_r(&s, &lt);
    return (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday;
}
