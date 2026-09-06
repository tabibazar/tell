#ifndef TEXTWRAP_H
#define TEXTWRAP_H

#include <stddef.h>

/* Maximum columns any caller may request. Sizes the caller's line buffer. */
#define TW_MAX_COLS 64

/* Maximum lines any caller may request. Sizes the caller's line buffer. */
#define TW_MAX_LINES 20

/*
 * Greedy word-wrap of `s` into at most `max_lines` lines of at most `cols`
 * columns. Lines are written NUL-terminated into `out`.
 *
 * Bytes outside printable ASCII (0x20..0x7E) become '?'. A '\n' forces a break.
 * A word longer than `cols` is broken mid-word. If the text does not fit in
 * `max_lines`, the last three cells of the final line become "..." so that
 * truncation is always visible.
 *
 * Returns the number of lines written. Empty or NULL input returns 0, which
 * callers render as a cleared screen.
 */
size_t textwrap(const char *s, size_t cols, size_t max_lines,
                char out[][TW_MAX_COLS + 1]);

#endif /* TEXTWRAP_H */
