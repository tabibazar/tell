#include "logview.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

int main(void)
{
    logview_t l;
    logview_clear(&l);

    expect("starts empty", logview_held(&l) == 0);
    expect("nothing visible when empty", logview_visible(&l, 5, 0) == NULL);

    logview_append(&l, "first", 64);
    expect("one line held", logview_held(&l) == 1);
    expect("that line is visible", strcmp(logview_visible(&l, 5, 0), "first") == 0);

    logview_append(&l, "second\nthird", 64);
    expect("newlines split into lines", logview_held(&l) == 3);
    expect("oldest first", strcmp(logview_visible(&l, 5, 0), "first") == 0);
    expect("newest last", strcmp(logview_visible(&l, 5, 2), "third") == 0);

    /* A window smaller than the log shows the newest lines. */
    expect("window shows the tail", strcmp(logview_visible(&l, 2, 0), "second") == 0);
    expect("tail ends at newest", strcmp(logview_visible(&l, 2, 1), "third") == 0);

    logview_clear(&l);
    logview_append(&l, "aaaaaaaaaa", 4);
    expect("long lines wrap", logview_held(&l) == 3);
    expect("first chunk", strcmp(logview_visible(&l, 9, 0), "aaaa") == 0);
    expect("last chunk", strcmp(logview_visible(&l, 9, 2), "aa") == 0);

    logview_clear(&l);
    logview_append(&l, "a\n\nb", 64);
    expect("blank lines are kept", logview_held(&l) == 3);
    expect("blank line is empty", strcmp(logview_visible(&l, 9, 1), "") == 0);

    logview_clear(&l);
    for (int i = 0; i < LOG_MAX_LINES + 10; i++) {
        char row[16];
        snprintf(row, sizeof row, "line%d", i);
        logview_append(&l, row, 64);
    }
    expect("held is capped", logview_held(&l) == LOG_MAX_LINES);
    char newest[16];
    snprintf(newest, sizeof newest, "line%d", LOG_MAX_LINES + 9);
    expect("newest survives the wrap",
           strcmp(logview_visible(&l, 1, 0), newest) == 0);
    expect("oldest scrolled off",
           strcmp(logview_visible(&l, LOG_MAX_LINES, 0), "line10") == 0);

    logview_clear(&l);
    expect("clear empties it", logview_held(&l) == 0);
    logview_append(&l, NULL, 64);
    expect("NULL is ignored", logview_held(&l) == 0);

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
