#include "care.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

char *care_rpc_body(const char *tool, const char *args_json)
{
    cJSON *args = args_json ? cJSON_Parse(args_json) : NULL;
    if (!args || !cJSON_IsObject(args)) { cJSON_Delete(args); return NULL; }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(o, "id", 1);
    cJSON_AddStringToObject(o, "method", "tools/call");
    cJSON *params = cJSON_AddObjectToObject(o, "params");
    cJSON_AddStringToObject(params, "name", tool);
    cJSON_AddItemToObject(params, "arguments", args);     /* o owns args now */
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

bool care_rpc_text(const char *resp, char *out, size_t n)
{
    out[0] = 0;
    cJSON *j = cJSON_Parse(resp);
    if (!j) return false;
    bool ok = false;
    if (!cJSON_GetObjectItemCaseSensitive(j, "error")) {
        cJSON *result = cJSON_GetObjectItemCaseSensitive(j, "result");
        cJSON *content = cJSON_GetObjectItemCaseSensitive(result, "content");
        cJSON *first = cJSON_GetArrayItem(content, 0);
        cJSON *text = cJSON_GetObjectItemCaseSensitive(first, "text");
        if (cJSON_IsString(text)) {
            snprintf(out, n, "%s", text->valuestring);
            ok = true;
        }
    }
    cJSON_Delete(j);
    return ok;
}

int care_parse_reminders(const char *text, care_list_t *l)
{
    cJSON *j = cJSON_Parse(text);
    if (!j) return -1;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(j, "reminders");
    if (!cJSON_IsArray(arr)) { cJSON_Delete(j); return -1; }
    care_list_t old = *l;             /* so a reminder still pending keeps its done flag */
    l->n = 0;
    const cJSON *r;
    cJSON_ArrayForEach(r, arr) {
        if (l->n == CARE_MAX) break;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(r, "id");
        const cJSON *at = cJSON_GetObjectItemCaseSensitive(r, "at");
        const cJSON *say = cJSON_GetObjectItemCaseSensitive(r, "say");
        if (!cJSON_IsString(id) || !cJSON_IsNumber(at) || !cJSON_IsString(say)) continue;
        care_item_t *it = &l->item[l->n++];
        memset(it, 0, sizeof *it);
        snprintf(it->id, sizeof it->id, "%s", id->valuestring);
        it->at = (long)at->valuedouble;
        snprintf(it->text, sizeof it->text, "%s", say->valuestring);
        for (int i = 0; i < old.n; i++)
            if (strcmp(old.item[i].id, it->id) == 0) { it->done = old.item[i].done; break; }
    }
    cJSON_Delete(j);
    return l->n;
}

int care_parse_messages(const char *text, care_list_t *l)
{
    cJSON *j = cJSON_Parse(text);
    if (!j) return -1;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(j, "messages");
    if (!cJSON_IsArray(arr)) { cJSON_Delete(j); return -1; }
    int added = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, arr) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(m, "id");
        const cJSON *from = cJSON_GetObjectItemCaseSensitive(m, "from");
        const cJSON *txt = cJSON_GetObjectItemCaseSensitive(m, "text");
        const cJSON *at = cJSON_GetObjectItemCaseSensitive(m, "at");
        if (!cJSON_IsString(id) || !cJSON_IsString(from) || !cJSON_IsString(txt) || !cJSON_IsNumber(at)) continue;
        bool known = false;
        for (int i = 0; i < l->n; i++) if (strcmp(l->item[i].id, id->valuestring) == 0) { known = true; break; }
        if (known) continue;
        if (l->n == CARE_MAX) {
            memmove(&l->item[0], &l->item[1], sizeof l->item[0] * (CARE_MAX - 1));
            l->n--;
        }
        care_item_t *it = &l->item[l->n++];
        memset(it, 0, sizeof *it);
        snprintf(it->id, sizeof it->id, "%s", id->valuestring);
        it->at = (long)at->valuedouble;
        snprintf(it->from, sizeof it->from, "%s", from->valuestring);
        snprintf(it->text, sizeof it->text, "%s", txt->valuestring);
        added++;
    }
    cJSON_Delete(j);
    return added;
}

bool care_may_speak(int minute) { return minute >= 9 * 60 && minute < 19 * 60; }

int care_due(const care_list_t *l, long now, int minute)
{
    if (!care_may_speak(minute)) return -1;
    int best = -1;
    for (int i = 0; i < l->n; i++) {
        const care_item_t *it = &l->item[i];
        if (it->done || it->at > now || now - it->at >= 6 * 3600) continue;
        if (best == -1 || it->at < l->item[best].at) best = i;
    }
    return best;
}

long care_next_wake(const care_list_t *l, long now)
{
    long best = 0;
    for (int i = 0; i < l->n; i++) {
        const care_item_t *it = &l->item[i];
        if (it->done || it->at <= now) continue;
        if (best == 0 || it->at < best) best = it->at;
    }
    return best;
}

char *care_list_json(const care_list_t *l)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "item");
    for (int i = 0; i < l->n; i++) {
        const care_item_t *it = &l->item[i];
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", it->id);
        cJSON_AddNumberToObject(e, "at", (double)it->at);
        cJSON_AddStringToObject(e, "from", it->from);
        cJSON_AddStringToObject(e, "text", it->text);
        cJSON_AddBoolToObject(e, "done", it->done);
        cJSON_AddItemToArray(arr, e);
    }
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

bool care_list_load(const char *json, care_list_t *l)
{
    cJSON *o = cJSON_Parse(json);
    if (!o) return false;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(o, "item");
    if (!cJSON_IsArray(arr)) { cJSON_Delete(o); return false; }
    care_list_t tmp;
    tmp.n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, arr) {
        if (tmp.n == CARE_MAX) break;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(e, "id");
        const cJSON *at = cJSON_GetObjectItemCaseSensitive(e, "at");
        const cJSON *from = cJSON_GetObjectItemCaseSensitive(e, "from");
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(e, "text");
        const cJSON *done = cJSON_GetObjectItemCaseSensitive(e, "done");
        if (!cJSON_IsString(id) || !cJSON_IsNumber(at) || !cJSON_IsString(text)) continue;
        care_item_t *it = &tmp.item[tmp.n++];
        memset(it, 0, sizeof *it);
        snprintf(it->id, sizeof it->id, "%s", id->valuestring);
        it->at = (long)at->valuedouble;
        if (cJSON_IsString(from)) snprintf(it->from, sizeof it->from, "%s", from->valuestring);
        snprintf(it->text, sizeof it->text, "%s", text->valuestring);
        it->done = cJSON_IsTrue(done);
    }
    cJSON_Delete(o);
    *l = tmp;
    return true;
}
