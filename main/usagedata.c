#include "ud_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/*
 * The core of the payload parser: the alphabet, the host slots, and the two
 * dispatch loops. Everything a particular payload means lives in its own
 * ud_*.c, registered in ud_sections.c.
 */

static const char GRID_ALPHABET[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

uint64_t usagedata_grid_value(char c)
{
    const char *p = c ? strchr(GRID_ALPHABET, c) : NULL;
    if (p == NULL) return 0;
    int i = (int)(p - GRID_ALPHABET);
    /* 2^(i/2): a whole power of two, times sqrt(2) for the odd steps. The
       constant is sqrt(2) in 1/65536ths, so no floating point is needed. */
    uint64_t v = 1000ULL << (i / 2);
    if (i & 1) v = (v * 92682ULL) >> 16;
    return v;
}

const char *ud_token(const char *p, char *out, int max)
{
    while (*p == ' ' || *p == '\t') p++;
    int n = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        if (n < max) out[n++] = *p;
        p++;
    }
    out[n] = '\0';
    return n ? p : NULL;
}

static const char *next_line(const char *p)
{
    while (*p && *p != '\n') p++;
    return *p ? p + 1 : p;
}

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static ud_host_t *host_slot(usagedata_t *d, const char *name)
{
    for (int i = 0; i < UD_MAX_HOSTS; i++)
        if (d->hosts[i].used && strcmp(d->hosts[i].host, name) == 0)
            return &d->hosts[i];

    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        if (!d->hosts[i].used) {
            memset(&d->hosts[i], 0, sizeof d->hosts[i]);
            strncpy(d->hosts[i].host, name, UD_HOST_MAX);
            d->hosts[i].used = true;
            return &d->hosts[i];
        }
    }
    return &d->hosts[UD_MAX_HOSTS - 1];
}

static void find_host(const char *payload, char *out)
{
    strcpy(out, "mac");
    for (const char *p = next_line(payload); *p; p = next_line(p)) {
        char tag[12];
        const char *q = ud_token(p, tag, 11);
        if (q == NULL) continue;
        if (strcmp(tag, "host") == 0 && ud_token(q, out, UD_HOST_MAX) != NULL)
            return;
    }
}

int usagedata_pct_value(char c)
{
    const char *p = c ? strchr(GRID_ALPHABET, c) : NULL;
    if (p == NULL) return -1;
    int i = (int)(p - GRID_ALPHABET);
    int steps = (int)sizeof GRID_ALPHABET - 2;          /* 61 */
    return (i * 100 + steps / 2) / steps;
}

/* Only machines that have actually sent something are counted, so an empty
   slot never appears in a legend. Each section says whether it has. */
bool ud_contributes(const ud_host_t *h)
{
    if (!h->used) return false;
    for (int i = 0; i < ud_section_count; i++)
        if (ud_sections[i]->used && ud_sections[i]->used(h)) return true;
    return false;
}

int usagedata_hosts(const usagedata_t *d)
{
    int n = 0;
    for (int i = 0; i < UD_MAX_HOSTS; i++)
        if (ud_contributes(&d->hosts[i])) n++;
    return n;
}

const char *usagedata_host_name(const usagedata_t *d, int which)
{
    int n = 0;
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        if (!ud_contributes(&d->hosts[i])) continue;
        if (n == which) return d->hosts[i].host;
        n++;
    }
    return "";
}

int ud_cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

int ud_rank_levels(const uint64_t *vals, int n, uint8_t *level,
                       uint64_t *scratch)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        level[i] = 0;
        if (vals[i] > 0) scratch[m++] = vals[i];
    }
    if (m == 0) return 0;
    qsort(scratch, (size_t)m, sizeof scratch[0], ud_cmp_u64);
    /* Thresholds round up, so with fewer than four values the smallest is
       still level 1 and the largest still level 4. */
    uint64_t q[UD_HEAT_LEVELS - 1];
    for (int k = 1; k < UD_HEAT_LEVELS; k++) {
        int at = (k * m + UD_HEAT_LEVELS - 1) / UD_HEAT_LEVELS;
        if (at > m - 1) at = m - 1;
        q[k - 1] = scratch[at];
    }
    for (int i = 0; i < n; i++) {
        if (vals[i] == 0) continue;
        int l = 1;
        for (int k = 0; k < UD_HEAT_LEVELS - 1; k++)
            if (vals[i] >= q[k]) l = k + 2;
        level[i] = (uint8_t)l;
    }
    return m;
}

void ud_pick_window(const int32_t *starts, const int32_t *todays, int n,
                        int32_t *start, int32_t *today, int *len)
{
    *start = *today = 0;
    *len = 0;
    for (int i = 0; i < n; i++) {
        if (todays[i] <= 0) continue;
        if (*len == 0 || todays[i] > *today) {
            *start = starts[i];
            *today = todays[i];
            *len = (int)(todays[i] - starts[i] + 1);
        }
    }
}

int ud_host_order(const usagedata_t *d, const ud_host_t *hosts[UD_MAX_HOSTS])
{
    int n = 0;
    for (int i = 0; i < UD_MAX_HOSTS; i++)
        if (ud_contributes(&d->hosts[i])) hosts[n++] = &d->hosts[i];
    return n;
}

void ud_text(const char *q, char *dest, int max)
{
    while (*q == ' ' || *q == '\t') q++;
    int n = 0;
    while (q[n] && q[n] != '\n' && q[n] != '\r' && n < max) n++;
    memcpy(dest, q, (size_t)n);
    dest[n] = '\0';
}

ud_kind_t usagedata_parse(usagedata_t *d, const char *payload, int64_t now_us)
{
    if (d == NULL || payload == NULL) return UD_NONE;

    const ud_section_t *sec = NULL;
    for (int i = 0; i < ud_section_count; i++)
        if (starts_with(payload, ud_sections[i]->marker)) { sec = ud_sections[i]; break; }
    if (sec == NULL) return UD_NONE;

    ud_host_t *h = NULL;
    if (sec->per_host) {
        char name[UD_HOST_MAX + 1];
        find_host(payload, name);
        h = host_slot(d, name);
        h->updated_us = now_us;
    }
    /* A section replaces this machine's rows wholesale: no merge, so no
       stale rows, and the other machine's data is untouched. */
    if (sec->begin) sec->begin(d, h, now_us);

    for (const char *p = next_line(payload); *p; p = next_line(p)) {
        char tag[12];
        const char *q = ud_token(p, tag, 11);
        if (q == NULL) continue;
        sec->line(d, h, tag, q);
    }
    if (sec->end) sec->end(d, h);
    return sec->kind;
}

void usagedata_merge(const usagedata_t *d, ud_view_t *out)
{
    memset(out, 0, sizeof *out);

    /* Who contributes, in a fixed order, so per-machine shares line up. */
    const ud_host_t *hosts[UD_MAX_HOSTS];
    int n = ud_host_order(d, hosts);
    for (int i = 0; i < n; i++) {
        strncpy(out->host_names[out->host_count++], hosts[i]->host, UD_HOST_MAX);
        if (hosts[i]->updated_us > out->updated_us) out->updated_us = hosts[i]->updated_us;
    }

    for (int i = 0; i < ud_section_count; i++)
        if (ud_sections[i]->merge) ud_sections[i]->merge(d, out);
}
