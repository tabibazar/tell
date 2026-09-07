#include "ud_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void begin_story(usagedata_t *d, ud_host_t *h, int64_t now_us)
{
    (void)d; (void)h; (void)now_us;
    memset(&h->story, 0, sizeof h->story);
}

static void line_story(usagedata_t *d, ud_host_t *h, const char *tag, const char *q)
{
    (void)d; (void)h;
    ud_story_t *st = &h->story;
    char num[24];
    if (strcmp(tag, "text") == 0) {
        /* Lines of the recap, joined with spaces, verbatim. */
        while (*q == ' ' || *q == '\t') q++;
        size_t have = strlen(st->text);
        if (have > 0 && have < UD_STORY_MAX) st->text[have++] = ' ';
        while (*q && *q != '\n' && *q != '\r' && have < UD_STORY_MAX)
            st->text[have++] = *q++;
        st->text[have] = '\0';
        st->used = true;
    } else if (ud_token(q, num, sizeof num - 1) != NULL) {
        if (strcmp(tag, "date") == 0) st->day = (int32_t)strtol(num, NULL, 10);
        else if (strcmp(tag, "prompts") == 0) st->prompts = (uint32_t)strtoul(num, NULL, 10);
    }
}

static void merge_story(const usagedata_t *d, ud_view_t *out)
{
    ud_story_view_t *v = &out->story;
    memset(v, 0, sizeof *v);
    for (int i = 0; i < UD_MAX_HOSTS; i++) {
        const ud_host_t *h = &d->hosts[i];
        if (!h->used || !h->story.used) continue;
        const ud_story_t *st = &h->story;
        bool better = !v->present || st->day > v->day
                   || (st->day == v->day && st->prompts > v->prompts);
        if (!better) continue;
        v->present = true;
        v->day = st->day;
        v->prompts = st->prompts;
        strncpy(v->text, st->text, UD_STORY_MAX);
    }
}

static bool used_story(const ud_host_t *h)
{
    return h->story.used;
}

const ud_section_t ud_section_story = {
    "!story", UD_STORY, true,
    begin_story, line_story, NULL, merge_story, used_story
};
