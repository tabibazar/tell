#include "logview.h"

#include <string.h>

void logview_clear(logview_t *l)
{
    l->count = 0;
}

int logview_held(const logview_t *l)
{
    return l->count < LOG_MAX_LINES ? l->count : LOG_MAX_LINES;
}

static void push(logview_t *l, const char *s, int len)
{
    if (len > LOG_MAX_COLS) len = LOG_MAX_COLS;
    char *slot = l->lines[l->count % LOG_MAX_LINES];
    memcpy(slot, s, (size_t)len);
    slot[len] = '\0';
    l->count++;
}

void logview_append(logview_t *l, const char *text, int cols)
{
    if (text == NULL) return;
    if (cols < 1 || cols > LOG_MAX_COLS) cols = LOG_MAX_COLS;

    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);

        if (len == 0) {
            push(l, "", 0);            /* keep blank lines as spacing */
        } else {
            /* Wrap anything wider than the screen rather than truncating. */
            int off = 0;
            while (off < len) {
                int chunk = len - off > cols ? cols : len - off;
                push(l, p + off, chunk);
                off += chunk;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
}

const char *logview_visible(const logview_t *l, int rows, int i)
{
    int held = logview_held(l);
    if (rows > held) rows = held;
    if (i < 0 || i >= rows) return NULL;

    /* Oldest of the visible window, in ring coordinates. */
    int first = l->count - rows;
    return l->lines[(first + i) % LOG_MAX_LINES];
}
