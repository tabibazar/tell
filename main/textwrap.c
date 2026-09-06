#include "textwrap.h"

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
