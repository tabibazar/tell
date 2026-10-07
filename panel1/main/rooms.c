#include "rooms.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* UTF-8 to what the faces can draw: ASCII kept, the common accented Latin
   letters to their base letter, curly quotes and dashes to plain ones,
   everything else (emoji, CJK) dropped. Runs of spaces close up. */
static void ascii(char *dst, size_t n, const char *s)
{
    static const char latin1[] = /* U+00C0..U+00FF */
        "AAAAAAACEEEEIIII" "DNOOOOO*OUUUUYTs" "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && o + 1 < n) {
        unsigned cp;
        int len;
        if (*p < 0x80) { cp = *p; len = 1; }
        else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); len = 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); len = 3; }
        else { len = 1; while (p[len] && (p[len] & 0xC0) == 0x80 && len < 4) len++; cp = 0; }
        p += len;
        char ch = 0;
        if (cp >= 0x20 && cp < 0x7F) ch = (char)cp;
        else if (cp == '\t' || cp == '\n' || cp == 0xA0) ch = ' ';
        else if (cp >= 0xC0 && cp <= 0xFF) ch = latin1[cp - 0xC0];
        else if (cp == 0x2018 || cp == 0x2019) ch = '\'';
        else if (cp == 0x201C || cp == 0x201D) ch = '"';
        else if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212) ch = '-';
        if (!ch) continue;
        if (ch == ' ' && (o == 0 || dst[o - 1] == ' ')) continue;
        dst[o++] = ch;
    }
    while (o && dst[o - 1] == ' ') o--;
    dst[o] = 0;
}

static int by_start(const void *a, const void *b)
{
    const room_ev_t *x = a, *y = b;
    return x->start != y->start ? x->start - y->start : x->end - y->end;
}

bool rooms_parse(const char *json, size_t len, rooms_day_t *out)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;
    bool ok = false;
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "d");
    const cJSON *rooms = cJSON_GetObjectItemCaseSensitive(root, "rooms");
    int y, m, dd;
    if (!cJSON_IsString(d) || sscanf(d->valuestring, "%4d-%2d-%2d", &y, &m, &dd) != 3 ||
        !cJSON_IsArray(rooms) || cJSON_GetArraySize(rooms) == 0)
        goto done;

    rooms_day_t *t = calloc(1, sizeof *t);
    if (!t) goto done;
    t->y = y; t->m = m; t->d = dd;
    const cJSON *r;
    cJSON_ArrayForEach(r, rooms) {
        if (t->nrooms == ROOMS_MAX) break;
        room_t *rm = &t->room[t->nrooms];
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(r, "n");
        const cJSON *ev = cJSON_GetObjectItemCaseSensitive(r, "ev");
        if (!cJSON_IsString(n) || !cJSON_IsArray(ev)) continue;
        char name[48];
        ascii(name, sizeof name, n->valuestring);
        /* "GRANDE (8) [TV with HDMI and WebCam]", "Short-1 (Closest to the
           door) (1)": the seats are the bracket that is all figures; every
           bracket, round or square, comes out of the name. */
        char bare[48];
        size_t o = 0;
        for (const char *p = name; *p && o + 1 < sizeof bare; p++) {
            char close = *p == '(' ? ')' : *p == '[' ? ']' : 0;
            const char *q = close ? strchr(p, close) : NULL;
            if (!q) { bare[o++] = *p; continue; }
            bool figures = q > p + 1;
            for (const char *f = p + 1; f < q; f++) if (*f < '0' || *f > '9') figures = false;
            if (close == ')' && figures) rm->cap = atoi(p + 1);
            p = q;
        }
        bare[o] = 0;
        ascii(rm->name, sizeof rm->name, bare);   /* closes up the spaces left */
        for (char *c = rm->name; *c; c++) *c = (char)toupper((unsigned char)*c);
        const cJSON *e;
        cJSON_ArrayForEach(e, ev) {
            if (rm->n == ROOM_EV_MAX) break;
            if (!cJSON_IsArray(e) || cJSON_GetArraySize(e) < 2) continue;
            const cJSON *s = cJSON_GetArrayItem(e, 0), *en = cJSON_GetArrayItem(e, 1);
            const cJSON *ti = cJSON_GetArrayItem(e, 2), *who = cJSON_GetArrayItem(e, 3);
            if (!cJSON_IsNumber(s) || !cJSON_IsNumber(en)) continue;
            int a = s->valueint < 0 ? 0 : s->valueint > 1440 ? 1440 : s->valueint;
            int b = en->valueint < 0 ? 0 : en->valueint > 1440 ? 1440 : en->valueint;
            if (b <= a) continue;
            room_ev_t *x = &rm->ev[rm->n++];
            x->start = (int16_t)a;
            x->end = (int16_t)b;
            ascii(x->title, sizeof x->title, cJSON_IsString(ti) ? ti->valuestring : "");
            if (!x->title[0]) strcpy(x->title, "Busy");
            ascii(x->who, sizeof x->who, cJSON_IsString(who) ? who->valuestring : "");
        }
        qsort(rm->ev, rm->n, sizeof rm->ev[0], by_start);
        t->nrooms++;
    }
    if (t->nrooms) {
        *out = *t;
        ok = true;
    }
    free(t);
done:
    cJSON_Delete(root);
    return ok;
}

const room_ev_t *rooms_at(const room_t *r, int now)
{
    for (int i = 0; i < r->n; i++)
        if (r->ev[i].start <= now && now < r->ev[i].end) return &r->ev[i];
    return NULL;
}

const room_ev_t *rooms_next(const room_t *r, int now)
{
    for (int i = 0; i < r->n; i++)
        if (r->ev[i].start > now) return &r->ev[i];
    return NULL;
}
