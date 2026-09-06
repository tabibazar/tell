/* The clock table, selected on device by CONFIG_SCREEN_BOARD_CROWPANEL_7.
   Included directly so it is covered on the host too. The body font is the
   shared 12x24 cell, covered by test_font.c. */
#include "font_clock.h"

#include <stdio.h>

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

static int clock_empty(int code)
{
    const uint8_t *g = clock_glyphs[code - CLOCK_FIRST];
    for (int i = 0; i < CLOCK_H * CLOCK_STRIDE; i++) if (g[i]) return 0;
    return 1;
}

int main(void)
{

    expect("clock cell is 96x160", CLOCK_W == 96 && CLOCK_H == 160);
    expect("clock covers '0'..':'", CLOCK_FIRST == '0' && CLOCK_LAST == ':');
    expect("clock has 11 glyphs",
           (int)(sizeof clock_glyphs / sizeof clock_glyphs[0]) == 11);
    expect("clock '0' has pixels", !clock_empty('0'));
    expect("clock ':' has pixels", !clock_empty(':'));
    expect("HH:MM:SS fits 800px", 8 * CLOCK_W <= 800);
    expect("clock fits 480px tall", CLOCK_H <= 480);

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
