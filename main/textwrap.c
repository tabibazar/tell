#include "textwrap.h"

#include <stdbool.h>
#include <string.h>

#include <stdbool.h>

/* Printable ASCII renders as itself; everything else is not in the font. */
static char sanitize(unsigned char c)
{
    return (c >= 0x20 && c <= 0x7E) ? (char)c : '?';
}

size_t textwrap(const char *s, size_t cols, size_t max_lines,
                char out[][TW_MAX_COLS + 1])
{
    if (s == NULL || *s == '\0') return 0;
    if (cols == 0 || cols > TW_MAX_COLS || max_lines == 0) return 0;

    size_t line = 0, col = 0;
    int truncated = 0;

    out[0][0] = '\0';

    for (size_t i = 0; s[i] != '\0'; ) {
        if (s[i] == '\n') {
            out[line][col] = '\0';
            if (++line >= max_lines) { truncated = 1; break; }
            col = 0;
            i++;
            continue;
        }

        if (s[i] == ' ') {
            /* Spaces never start a line and never overflow one. */
            if (col > 0 && col < cols) out[line][col++] = ' ';
            i++;
            continue;
        }

        size_t wlen = 0;
        while (s[i + wlen] != '\0' && s[i + wlen] != ' ' && s[i + wlen] != '\n') wlen++;

        /* Move a word that would overflow down to the next line, unless the
           line is already empty -- then it must be broken mid-word instead. */
        if (col > 0 && col + wlen > cols) {
            out[line][col] = '\0';
            if (++line >= max_lines) { truncated = 1; break; }
            col = 0;
        }

        for (size_t k = 0; k < wlen; k++) {
            if (col == cols) {
                out[line][col] = '\0';
                if (++line >= max_lines) { truncated = 1; break; }
                col = 0;
            }
            out[line][col++] = sanitize((unsigned char)s[i + k]);
        }
        if (truncated) break;
        i += wlen;
    }

    if (!truncated) {
        out[line][col] = '\0';
        return line + 1;
    }

    /* Make truncation visible rather than silently dropping the remainder. */
    size_t last = max_lines - 1;
    size_t start = (cols >= 3) ? cols - 3 : 0;
    for (size_t k = start; k < cols; k++) out[last][k] = '.';
    out[last][cols] = '\0';
    return max_lines;
}

/* The next field: from `p`, up to a run of two or more spaces or the end.
   Returns where the following field starts, or NULL at the end. */
static const char *field_end(const char *p, size_t *len)
{
    const char *q = p;
    while (*q) {
        if (q[0] == ' ' && q[1] == ' ') break;
        q++;
    }
    *len = (size_t)(q - p);
    while (*q == ' ') q++;
    return *q ? q : NULL;
}

size_t textwrap_fields(const char *s, size_t cols, size_t max_lines,
                       char out[][TW_MAX_COLS + 1])
{
    if (s == NULL || *s == '\0' || cols == 0 || max_lines == 0) return 0;
    if (cols > TW_MAX_COLS) cols = TW_MAX_COLS;
    if (max_lines > TW_MAX_LINES) max_lines = TW_MAX_LINES;

    /* No field separators, or a field that will not fit on a line at all:
       ordinary wrapping handles both better than this would. */
    bool has_fields = false;
    for (const char *p = s; p[0] && p[1]; p++)
        if (p[0] == ' ' && p[1] == ' ') { has_fields = true; break; }
    if (!has_fields) return textwrap(s, cols, max_lines, out);
    for (const char *p = s; p; ) {
        size_t len;
        const char *next = field_end(p, &len);
        if (len > cols) return textwrap(s, cols, max_lines, out);
        p = next;
    }

    size_t line = 0, used = 0;
    out[0][0] = '\0';
    for (const char *p = s; p; ) {
        size_t len;
        const char *next = field_end(p, &len);

        /* One space between fields, and none before the first on a line. */
        size_t want = used == 0 ? len : used + 1 + len;
        if (want > cols) {
            if (line + 1 >= max_lines) {
                /* Out of room. Mark it, the way textwrap does, so a dropped
                   tail is always visible rather than silently gone. */
                size_t at = used > cols - 3 ? cols - 3 : used;
                memcpy(out[line] + at, "...", 4);
                return line + 1;
            }
            line++;
            used = 0;
            out[line][0] = '\0';
            want = len;
        }
        if (used > 0) out[line][used++] = ' ';
        memcpy(out[line] + used, p, len);
        used += len;
        out[line][used] = '\0';
        p = next;
    }
    return line + 1;
}
