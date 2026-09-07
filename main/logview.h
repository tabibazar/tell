#ifndef LOGVIEW_H
#define LOGVIEW_H

/* A scrolling log of the last few lines. New text appends; the oldest lines
   fall off the top. Pure: no hardware, no canvas. */

#define LOG_MAX_LINES 48
#define LOG_MAX_COLS  64

typedef struct {
    char lines[LOG_MAX_LINES][LOG_MAX_COLS + 1];
    int count;   /* total ever appended, so the ring knows where it is */
} logview_t;

void logview_clear(logview_t *l);

/* Appends text, splitting on '\n' and wrapping anything wider than `cols`. */
void logview_append(logview_t *l, const char *text, int cols);

/* How many lines are currently held, at most LOG_MAX_LINES. */
int logview_held(const logview_t *l);

/* The i-th of the last `rows` lines, oldest first. NULL when out of range. */
const char *logview_visible(const logview_t *l, int rows, int i);

#endif /* LOGVIEW_H */
