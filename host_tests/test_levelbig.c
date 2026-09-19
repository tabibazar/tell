#include "levelbig.h"

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

/* envio's actual panel: 320 wide, 480 tall, portrait. */
#define W 320
#define H 480

static uint16_t guarded[8 + W * H + 8];
static canvas_t c;

/* Host builds see palette.h's default (non-vivid, non-envio) accents:
   PAL_A0 (blue) 0x55BD, PAL_A1 (amber) 0xE4E0. */
#define BLUE  0x55BD
#define AMBER 0xE4E0

static void draw(float tx, float ty, bool armed)
{
    for (unsigned i = 0; i < sizeof guarded / sizeof guarded[0]; i++)
        guarded[i] = 0xABAB;
    canvas_init(&c, guarded + 8, W, H, 1);
    levelbig_draw(&c, tx, ty, 0.0f, 0.0f, 0.0f, 0.0f, armed);
}

static int guards_intact(void)
{
    for (int i = 0; i < 8; i++)
        if (guarded[i] != 0xABAB || guarded[8 + W * H + i] != 0xABAB) return 0;
    return 1;
}

static int has_pixel(uint16_t colour)
{
    for (int i = 0; i < W * H; i++)
        if (c.fb[i] == colour) return 1;
    return 0;
}

static uint16_t px(int x, int y)
{
    if (x < 0 || x >= W || y < 0 || y >= H) return 0xABAB;
    return c.fb[y * W + x];
}

int main(void)
{
    /* Same geometry as levelbig.c's own cx/cy/r -- this test knows the shape
       it is checking, not just its silhouette. */
    int cx = W / 2;
    int cy = W / 2 + 40;
    int r  = W / 2 - 20;

    draw(0.0f, 0.0f, true);
    expect("drawing stays in the framebuffer", guards_intact());
    expect("a blue ring pixel exists", has_pixel(BLUE));
    expect("bubble centred at tilt 0", px(cx, cy) == AMBER);

    /* levelbig.c matches level.c's own bubble convention exactly: the bubble
       floats to the HIGH side, against gravity, the same negation level.c's
       level_draw performs for the same reason (`sx = clampf(-tilt_x_deg,
       ...)`, main/level.c) -- and which host_tests/test_level.c locks in as
       "right edge down [a positive tilt_x_deg] floats the bubble left". So a
       positive tilt_x_deg here moves the bubble toward -x, not +x. */
    float tilt = 2.5f;
    int shift = (int)(tilt / LEVEL_FULL_SCALE_DEG * (float)r);

    draw(tilt, 0.0f, true);
    expect("positive tilt_x moves the bubble toward -x, matching level.c",
           px(cx - shift, cy) == AMBER);
    expect("...and not toward +x",
           px(cx + shift, cy) != AMBER);

    draw(-tilt, 0.0f, true);
    expect("negative tilt_x moves the bubble toward +x",
           px(cx + shift, cy) == AMBER);

    draw(0.0f, 90.0f, true);
    expect("wildly off scale still draws in bounds", guards_intact());

    draw(0.0f, 0.0f, false);
    expect("not-armed hint draws in bounds too", guards_intact());

    printf("%s\n", failures ? "FAILURES" : "all tests passed");
    return failures ? 1 : 0;
}
