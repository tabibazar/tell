#include "level.h"

#include <stdio.h>
#include <string.h>

static int failures;
static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

#define W 240
#define H 135

static uint16_t guarded[8 + W * H + 8];
static canvas_t c;

static void draw(float roll, float pitch)
{
    for (unsigned i = 0; i < sizeof guarded / sizeof guarded[0]; i++)
        guarded[i] = 0xABAB;
    canvas_init(&c, guarded + 8, W, H, 2);
    level_draw(&c, roll, pitch, 0.0f, 0.0f, 0.0f);
}

static int guards_intact(void)
{
    for (int i = 0; i < 8; i++)
        if (guarded[i] != 0xABAB || guarded[8 + W * H + i] != 0xABAB) return 0;
    return 1;
}

/* Where the brightest cluster of the bubble colour sits, as a rough centroid
   of everything that is not background in the left half. */
static void bubble_at(int *bx, int *by)
{
    long sx = 0, sy = 0, n = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W / 2; x++) {
            uint16_t p = c.fb[y * W + x];
            if (p == 0xE4E0 || p == 0x04EE) { sx += x; sy += y; n++; }
        }
    *bx = n ? (int)(sx / n) : -1;
    *by = n ? (int)(sy / n) : -1;
}

int main(void)
{
    /* The hold clock is drawn only while it is true, so both branches of the
       readout get exercised. */
    expect("dead level is true",      level_is_true(0.0f, 0.0f));
    expect("a tenth of a degree is true", level_is_true(0.1f, -0.1f));
    expect("two degrees is not",     !level_is_true(2.0f, 0.0f));
    expect("either axis alone counts", !level_is_true(0.0f, -2.0f));

    int x0, y0, x1, y1;

    draw(0.0f, 0.0f);
    expect("drawing stays in the framebuffer", guards_intact());
    {
        for (unsigned i = 0; i < sizeof guarded / sizeof guarded[0]; i++)
            guarded[i] = 0xABAB;
        canvas_init(&c, guarded + 8, W, H, 2);
        level_draw(&c, 0.0f, 0.0f, 9999.9f, 999.9f, 99999.9f);
        expect("a very long hold still draws in bounds", guards_intact());
    }
    bubble_at(&x0, &y0);

    draw(8.0f, 0.0f);
    bubble_at(&x1, &y1);
    expect("right edge down floats the bubble left", x1 < x0 - 5);
    expect("and not vertically", y1 > y0 - 4 && y1 < y0 + 4);

    draw(0.0f, 8.0f);
    bubble_at(&x1, &y1);
    expect("far edge down floats the bubble up", y1 < y0 - 5);

    draw(-8.0f, 0.0f);
    bubble_at(&x1, &y1);
    expect("left edge down floats it right", x1 > x0 + 5);

    /* Beyond full scale the bubble stops at the rim instead of leaving. */
    draw(90.0f, 90.0f);
    expect("wildly off scale still draws in bounds", guards_intact());
    bubble_at(&x1, &y1);
    int xr, yr;
    draw(LEVEL_FULL_SCALE_DEG, LEVEL_FULL_SCALE_DEG);
    bubble_at(&xr, &yr);
    expect("the bubble stops at the rim", x1 <= xr + 2 && x1 >= xr - 2);

    printf("%s\n", failures ? "FAILURES" : "all tests passed");
    return failures ? 1 : 0;
}
