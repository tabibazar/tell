#include "font.h"

#include <stdio.h>

static int failures;

static int glyph_empty(int code)
{
    const uint8_t *g = font_glyphs[code - FONT_FIRST];
    for (int i = 0; i < FONT_H * FONT_STRIDE; i++) if (g[i]) return 0;
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
           (int)(sizeof font_glyphs / sizeof font_glyphs[0]) == FONT_LAST - FONT_FIRST + 1);
    expect("space is blank", glyph_empty(' '));
    expect("'A' has pixels", !glyph_empty('A'));
    expect("'g' has pixels", !glyph_empty('g'));
    expect("'~' has pixels", !glyph_empty('~'));

    printf("\n'A' rendered (%dx%d):\n", FONT_W, FONT_H);
    const uint8_t *a = font_glyphs['A' - FONT_FIRST];
    for (int y = 0; y < FONT_H; y++) {
        uint32_t bits = 0;
        for (int b = 0; b < FONT_STRIDE; b++)
            bits = (bits << 8) | a[y * FONT_STRIDE + b];
        for (int x = 0; x < FONT_W; x++)
            putchar((bits >> (FONT_W - 1 - x)) & 1 ? '#' : '.');
        putchar('\n');
    }

    if (failures == 0) { printf("\nall tests passed\n"); return 0; }
    printf("\n%d test(s) failed\n", failures);
    return 1;
}
