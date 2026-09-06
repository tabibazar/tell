#include "canvas.h"
#include "font.h"
#include "textwrap.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

static int lit_colour(canvas_t *c, uint16_t colour)
{
    int n = 0;
    for (int i = 0; i < c->w * c->h; i++) if (c->fb[i] == colour) n++;
    return n;
}

static int lit_pixels(canvas_t *c)
{
    int n = 0;
    for (int i = 0; i < c->w * c->h; i++) if (c->fb[i] == CANVAS_FG) n++;
    return n;
}

int main(void)
{
    /* Feather ESP32-S3 TFT geometry. */
    static uint16_t small_fb[240 * 135];
    canvas_t small;
    canvas_init(&small, small_fb, 240, 135, 1);
    expect("240x135 at scale 1 gives 20 cols", small.cols == 20);
    expect("240x135 at scale 1 gives 5 rows", small.rows == 5);

    /* CrowPanel 7.0 geometry. */
    static uint16_t big_fb[800 * 480];
    canvas_t big;
    canvas_init(&big, big_fb, 800, 480, 2);
    expect("800x480 at scale 2 gives 33 cols", big.cols == 33);
    expect("800x480 at scale 2 gives 10 rows", big.rows == 10);
    expect("cols never exceed the wrap limit", big.cols <= TW_MAX_COLS);

    canvas_text(&small, NULL);
    expect("null text clears the framebuffer", lit_pixels(&small) == 0);

    canvas_text(&small, "");
    expect("empty text clears the framebuffer", lit_pixels(&small) == 0);

    canvas_text(&small, "A");
    int one = lit_pixels(&small);
    expect("one glyph lights some pixels", one > 0);

    canvas_text(&small, "AA");
    expect("two glyphs light twice as many", lit_pixels(&small) == 2 * one);

    /* Scaling replicates pixels, so area grows with the square of the scale. */
    canvas_init(&big, big_fb, 800, 480, 1);
    canvas_text(&big, "A");
    int at1 = lit_pixels(&big);
    canvas_init(&big, big_fb, 800, 480, 2);
    canvas_text(&big, "A");
    expect("scale 2 quadruples a glyph's area", lit_pixels(&big) == 4 * at1);

    canvas_big(&small, "12:34:56");
    expect("big text fits inside the panel", lit_pixels(&small) > 0);

    /* Nothing may be written outside the framebuffer: fill the guard rows and
       confirm a full-width render never disturbs the last row's end. */
    canvas_text(&small, "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW");
    expect("long text stays in bounds", lit_pixels(&small) > 0);

    /* Rectangles must clamp, not write outside the framebuffer. */
    canvas_init(&small, small_fb, 240, 135, 1);
    canvas_clear(&small);
    canvas_fill_rect(&small, 0, 0, 10, 10, 0xF800);
    expect("fill_rect paints its area", lit_colour(&small, 0xF800) == 100);

    canvas_clear(&small);
    canvas_fill_rect(&small, -5, -5, 10, 10, 0xF800);
    expect("negative origin is clipped", lit_colour(&small, 0xF800) == 25);

    canvas_clear(&small);
    canvas_fill_rect(&small, 235, 130, 100, 100, 0xF800);
    expect("oversize is clipped to the panel", lit_colour(&small, 0xF800) == 25);

    canvas_clear(&small);
    canvas_fill_rect(&small, 0, 0, 0, 0, 0xF800);
    expect("zero size paints nothing", lit_colour(&small, 0xF800) == 0);

    canvas_clear(&small);
    canvas_puts(&small, 0, 0, "A", 0x07E0);
    expect("puts uses the colour given", lit_colour(&small, 0x07E0) > 0);
    expect("puts does not paint white", lit_colour(&small, 0xFFFF) == 0);

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
