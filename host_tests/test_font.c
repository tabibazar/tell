#include "font8x16.h"

#include <stdio.h>

static int failures;

static int glyph_empty(int code)
{
    const uint8_t *g = font8x16[code - FONT_FIRST];
    for (int y = 0; y < FONT_H; y++) if (g[y]) return 0;
    return 1;
}

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

int main(void)
{
    expect("table covers ASCII 32..126",
           (int)(sizeof font8x16 / sizeof font8x16[0]) == FONT_LAST - FONT_FIRST + 1);
    expect("space is blank", glyph_empty(' '));
    expect("'A' has pixels", !glyph_empty('A'));
    expect("'g' has pixels", !glyph_empty('g'));
    expect("'~' has pixels", !glyph_empty('~'));

    printf("\n'A' rendered:\n");
    const uint8_t *a = font8x16['A' - FONT_FIRST];
    for (int y = 0; y < FONT_H; y++) {
        for (int x = 0; x < FONT_W; x++) putchar((a[y] >> (7 - x)) & 1 ? '#' : '.');
        putchar('\n');
    }

    if (failures == 0) { printf("\nall tests passed\n"); return 0; }
    printf("\n%d test(s) failed\n", failures);
    return 1;
}
