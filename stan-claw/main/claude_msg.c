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
                     const claude_history_t *h, const char *facts, const char *question, const char *language)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "model", model);
    cJSON_AddNumberToObject(o, "max_tokens", 1024);
    /* The prompt, what is known about the person, and the language to answer in. */
    bool other = language && language[0] && strcmp(language, "English") != 0;
    if ((facts && facts[0]) || other) {
        size_t n = strlen(CLAUDE_SYSTEM) + (facts ? strlen(facts) : 0) + strlen(CLAUDE_LANGUAGE) + (other ? 3 * strlen(language) : 0) + 64;
        char *sys = malloc(n);
        if (sys) {
            size_t at = (size_t)snprintf(sys, n, "%s", CLAUDE_SYSTEM);
            if (facts && facts[0] && at < n) at += (size_t)snprintf(sys + at, n - at, " What you already know about the person: %s", facts);
            if (other && at + 1 < n) {
                sys[at++] = ' ';
                snprintf(sys + at, n - at, CLAUDE_LANGUAGE, language, language, language);
            }
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

bool claude_take_confirm(char *text, char *summary, size_t n)
{
    summary[0] = 0;
    char *m = strstr(text, "[CONFIRM:");
    if (!m) return false;
    char *start = m + 9;
    while (*start == ' ') start++;
    char *close = strchr(start, ']');
    const char *end = close ? close : start + strlen(start);     /* tolerate a missing "]" */
    size_t len = (size_t)(end - start);
    while (len && start[len - 1] == ' ') len--;
    if (len >= n) len = n ? n - 1 : 0;
    memcpy(summary, start, len);
    summary[len] = 0;
    char *stop = close ? close + 1 : start + strlen(start);
    memmove(m, stop, strlen(stop) + 1);
    size_t l = strlen(text);
    while (l && (text[l - 1] == ' ' || text[l - 1] == '\n')) text[--l] = 0;
    return true;
}

bool claude_find_homeowner_token(const char *raw_json, char *out, size_t n)
{
    const char *needle = "homeowner_token: ";
    const char *p = raw_json, *last = NULL;
    while ((p = strstr(p, needle)) != NULL) { last = p; p += strlen(needle); }
    if (!last) return false;
    const char *start = last + strlen(needle);
    if (strncmp(start, "hw_", 3) != 0) return false;
    size_t len = 0;
    while (start[len] && (isalnum((unsigned char)start[len]) || start[len] == '_')) len++;
    if (len >= n) len = n ? n - 1 : 0;
    memcpy(out, start, len);
    out[len] = 0;
    return true;
}

/* A Latin-1 letter (U+00C0..U+00FF) without its accent, lower case: "e" for
   É or è, "ss" for ß; "" for the two signs among them (multiply, divide). */
static const char *unaccent(unsigned cp)
{
    static const char *const map[64] = {
        "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
        "d", "n", "o", "o", "o", "o", "o", "", "o", "u", "u", "u", "u", "y", "th", "ss",
        "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
        "d", "n", "o", "o", "o", "o", "o", "", "o", "u", "u", "u", "u", "y", "th", "y",
    };
    return cp >= 0xC0 && cp <= 0xFF ? map[cp - 0xC0] : "";
}

bool claude_is_signoff(const char *heard)
{
    /* Lower case, accents off, letters and single spaces only, then
       whole-phrase checks: a short utterance that is (or ends with) a
       sign-off, not one that merely contains "stop" somewhere in a question.
       Apostrophes (straight or curly) vanish: "c'est" is "cest". */
    char t[160];
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)heard; *p && o + 3 < sizeof t; p++) {
        char c = (char)*p;
        const char *add = NULL;
        if (*p == 0xC3 && (p[1] & 0xC0) == 0x80) {                 /* U+00C0..U+00FF: a letter of another language */
            add = unaccent(0xC0u | (p[1] & 0x3Fu));
            p++;
        } else if (*p == 0xC5 && (p[1] == 0x92 || p[1] == 0x93)) {   /* Œ, œ */
            add = "oe";
            p++;
        } else if (*p == 0xE2 && p[1] == 0x80 && (p[2] == 0x98 || p[2] == 0x99)) {   /* ‘ ’ */
            p += 2;
            continue;
        }
        if (add) {
            for (; *add; add++) t[o++] = *add;
            continue;
        }
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z')) t[o++] = c;
        else if (c == '\'') continue;
        else if (o > 0 && t[o - 1] != ' ') t[o++] = ' ';           /* one space between words, however they were split */
    }
    while (o > 0 && t[o - 1] == ' ') o--;
    t[o] = 0;
    int words = 0;
    for (char *p = t; *p; p++) if (*p != ' ' && (p == t || p[-1] == ' ')) words++;
    if (words == 0 || words > 8) return false;
    static const char *const ends[] = { "thats all", "that is all", "thats it", "that is it", "goodbye", "bye",
                                        "im done", "i am done", "were done", "nothing else", "no thanks",
                                        "no thank you", "stop", "see you", "talk later", "never mind",
                                        /* Spanish, French, Portuguese, Italian, German, Tagalog */
                                        "eso es todo", "gracias adios", "adios", "hasta luego",
                                        "cest tout", "merci au revoir", "au revoir",
                                        "obrigado tchau", "obrigada tchau", "tchau", "adeus",
                                        "grazie arrivederci", "arrivederci",
                                        "das ist alles", "danke tschuss", "danke tschus", "tschuss", "tschus", "auf wiedersehen",
                                        "iyon lang", "salamat paalam", "paalam" };
    /* Short ones that only close an utterance: "é tudo" is "that's all",
       "e tudo bem?" is not. */
    static const char *const tails[] = { "e tudo", "e tutto" };
    for (size_t i = 0; i < sizeof ends / sizeof ends[0] + sizeof tails / sizeof tails[0]; i++) {
        bool tail = i >= sizeof ends / sizeof ends[0];
        const char *ph = tail ? tails[i - sizeof ends / sizeof ends[0]] : ends[i];
        for (const char *hit = strstr(t, ph); hit; hit = strstr(hit + 1, ph)) {
            const char *after = hit + strlen(ph);
            bool start_ok = hit == t || hit[-1] == ' ';
            bool end_ok = *after == 0 || (!tail && *after == ' ');
            if (start_ok && end_ok) return true;
        }
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
