#include "ud_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void begin_projects(usagedata_t *d, ud_host_t *h, int64_t now_us)
{
    (void)d; (void)h; (void)now_us;
    memset(&h->projects, 0, sizeof h->projects);
}

static void line_projects(usagedata_t *d, ud_host_t *h, const char *tag, const char *q)
{
    (void)d; (void)h;
    ud_projects_t *pj = &h->projects;
    char num[24];
    if (strcmp(tag, "days") == 0) {
        if (ud_token(q, num, sizeof num - 1) != NULL) pj->days = atoi(num);
    } else if (strcmp(tag, "p") == 0) {
        if (pj->count >= UD_MAX_MODELS) return;
        ud_project_t row;
        memset(&row, 0, sizeof row);
        q = ud_token(q, row.name, UD_NAME_MAX);
        if (q == NULL || (q = ud_token(q, num, sizeof num - 1)) == NULL) return;
        row.tokens = strtoull(num, NULL, 10);
        if (q && (q = ud_token(q, num, sizeof num - 1)) != NULL) row.cents = strtoull(num, NULL, 10);
        if (q && (q = ud_token(q, num, sizeof num - 1)) != NULL) row.sessions = (uint32_t)strtoul(num, NULL, 10);
        if (q && (q = ud_token(q, num, sizeof num - 1)) != NULL) row.msgs = (uint32_t)strtoul(num, NULL, 10);
        pj->rows[pj->count++] = row;
    }
}

static void end_projects(usagedata_t *d, ud_host_t *h)
{
    (void)d;
    h->projects.used = h->projects.count > 0;
}

/* Projects merged by name across machines, largest first. */
static void merge_projects(const usagedata_t *d, ud_view_t *out)
{
    ud_projects_view_t *v = &out->projects;
    memset(v, 0, sizeof *v);
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->projects.used) continue;
        v->present = true;
        if (h->projects.days > v->days) v->days = h->projects.days;
        for (int r = 0; r < h->projects.count; r++) {
            const ud_project_t *row = &h->projects.rows[r];
            int j;
            for (j = 0; j < v->count; j++)
                if (strcmp(v->rows[j].name, row->name) == 0) break;
            if (j == v->count) {
                if (v->count >= UD_MAX_MODELS) continue;
                v->rows[v->count++] = *row;
            } else {
                v->rows[j].tokens += row->tokens;
                v->rows[j].cents += row->cents;
                v->rows[j].sessions += row->sessions;
                v->rows[j].msgs += row->msgs;
            }
            v->total_tokens += row->tokens;
        }
    }
    for (int i = 1; i < v->count; i++) {
        ud_project_t key = v->rows[i];
        int j = i - 1;
        while (j >= 0 && v->rows[j].tokens < key.tokens) { v->rows[j + 1] = v->rows[j]; j--; }
        v->rows[j + 1] = key;
    }
}

static bool used_projects(const ud_host_t *h)
{
    return h->projects.used;
}

const ud_section_t ud_section_projects = {
    "!projects", UD_PROJECTS, true,
    begin_projects, line_projects, end_projects, merge_projects, used_projects
};
