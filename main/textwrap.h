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

/*
 * As textwrap, but breaking at the text's own field boundaries where it can.
 *
 * Some of what the Mac sends is a row of fields separated by two spaces
 * rather than a sentence -- the weather line is "Partly cloudy 26.7C" then
 * "feels 27C" then "48%" and so on. Wrapped as prose on a narrow panel that
 * splits "feels" from "27C" and "lo" from "16", which reads as damage even
 * though every character is present.
 *
 * Fields are packed greedily, one space between them, and a run of two or
 * more spaces is where a line may break. A single field too long for `cols`
 * falls back to ordinary word wrapping, as does text with no field
 * separators in it at all, so this is never worse than textwrap.
 *
 * Truncation is marked the same way, with "..." in the final line.
 */
size_t textwrap_fields(const char *s, size_t cols, size_t max_lines,
                       char out[][TW_MAX_COLS + 1]);

#endif /* TEXTWRAP_H */
