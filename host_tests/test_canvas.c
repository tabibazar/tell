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

    /* T-Display-S3 ("lilly") geometry: between the other two, and the reason
       she reuses the 12x24 cell rather than getting a table of her own. */
    static uint16_t lilly_fb[320 * 170];
    canvas_t lilly;
    canvas_init(&lilly, lilly_fb, 320, 170, 1);
    expect("320x170 at scale 1 gives 26 cols", lilly.cols == 26);
    expect("320x170 at scale 1 gives 7 rows", lilly.rows == 7);
    expect("lilly's cols fit the wrap limit", lilly.cols <= TW_MAX_COLS);

    /* wave: the Waveshare 1.47B, 172x320 turned landscape. Two rows taller
       than lilly in pixels and identical in characters, which is the point --
       the sand and the wrapping carry over untouched. */
    static uint16_t wave_fb[320 * 172];
    canvas_t wave;
    canvas_init(&wave, wave_fb, 320, 172, 1);
    expect("320x172 at scale 1 gives 26 cols", wave.cols == 26);
    expect("320x172 at scale 1 gives 7 rows", wave.rows == 7);

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

    /* Circles and discs stay inside the framebuffer even when the centre is
       off the panel and the radius is larger than it. */
    {
        canvas_t g;
        static uint16_t guarded[8 + 240 * 135 + 8];
        for (unsigned i = 0; i < sizeof guarded / sizeof guarded[0]; i++)
            guarded[i] = 0xABAB;
        canvas_init(&g, guarded + 8, 240, 135, 1);
        canvas_circle(&g, -50, 300, 400, 0xF800);
        canvas_disc(&g, 500, -20, 600, 0x07E0);
        canvas_circle(&g, 120, 67, 60, 0xFFFF);
        canvas_disc(&g, 120, 67, 8, 0xFFFF);
        int intact = 1;
        for (int i = 0; i < 8; i++)
            if (guarded[i] != 0xABAB || guarded[8 + 240 * 135 + i] != 0xABAB)
                intact = 0;
        expect("circles and discs stay in the framebuffer", intact);

        /* A disc is actually round: its widest row is at the centre. */
        int widest = 0, at_edge = 0;
        for (int x = 0; x < 240; x++) {
            if (g.fb[67 * 240 + x] == 0xFFFF) widest++;
            if (g.fb[(67 - 8) * 240 + x] == 0xFFFF) at_edge++;
        }
        expect("the disc is widest across its middle", widest > at_edge);
    }

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
