#include "voices.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"

/* ASCII only (the faces have nothing else), underscores to spaces. */
static void clean(char *dst, size_t n, const char *s)
{
    size_t o = 0;
    for (; *s && o + 1 < n; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch >= 0x80) continue;
        dst[o++] = ch == '_' ? ' ' : (char)ch;
    }
    dst[o] = 0;
}

static void add_tag(char *tags, size_t n, const cJSON *labels, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(labels, key);
    if (!cJSON_IsString(v) || !v->valuestring[0]) return;
    char t[24];
    clean(t, sizeof t, v->valuestring);
    size_t l = strlen(tags);
    snprintf(tags + l, n - l, "%s%s", l ? ", " : "", t);
}

int voices_parse(const char *json, voice_t *out, int max)
{
    cJSON *j = cJSON_Parse(json);
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(j, "voices");
    if (!cJSON_IsArray(arr)) { cJSON_Delete(j); return -1; }
    int n = 0;
    const cJSON *v;
    cJSON_ArrayForEach(v, arr) {
        if (n == max) break;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(v, "voice_id");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(v, "name");
        if (!cJSON_IsString(id) || !cJSON_IsString(name) || strlen(id->valuestring) >= sizeof out[0].id) continue;
        voice_t *o = &out[n];
        memset(o, 0, sizeof *o);
        snprintf(o->id, sizeof o->id, "%s", id->valuestring);
        char full[96];
        clean(full, sizeof full, name->valuestring);
        char *dash = strstr(full, " - ");
        if (dash) {
            *dash = 0;
            snprintf(o->desc, sizeof o->desc, "%.*s", (int)sizeof o->desc - 1, dash + 3);
        }
        snprintf(o->name, sizeof o->name, "%.*s", (int)sizeof o->name - 1, full);
        const cJSON *labels = cJSON_GetObjectItemCaseSensitive(v, "labels");
        add_tag(o->tags, sizeof o->tags, labels, "gender");
        add_tag(o->tags, sizeof o->tags, labels, "accent");
        add_tag(o->tags, sizeof o->tags, labels, "age");
        n++;
    }
    cJSON_Delete(j);
    return n;
}
