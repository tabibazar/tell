#include "lightui.h"

#include <stdio.h>

#include "aafont.h"

const light_colour_t LIGHT_COLOURS[LIGHT_N_COLOURS] = {
    { "Off",    0,   0,   0   },
    { "Warm",   255, 130, 40  },
    { "White",  255, 230, 200 },
    { "Red",    255, 0,   0   },
    { "Green",  0,   255, 40  },
    { "Blue",   0,   60,  255 },
    { "Purple", 180, 0,   255 },
    { "Cyan",   0,   220, 255 },
};
/* Even steps to the eye, not to the meter. */
const uint8_t LIGHT_LEVELS[LIGHT_N_LEVELS] = { 5, 15, 35, 65, 100 };

static uint16_t rgb565(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

#define CREAM     0xF79A
#define CREAM_DIM 0x7BCF

void lightui_draw(canvas_t *c, const light_state_t *s)
{
    canvas_clear(c);
    const light_colour_t *k = &LIGHT_COLOURS[s->colour];
    bool off = s->colour == 0;
    int cx = c->w / 2, cy = 112, r = 70;
    int pct = LIGHT_LEVELS[s->level];

    aafont_draw(c, &aafont_inter_label, cx, 16, "blinky1", CREAM_DIM, AAFONT_CENTRE);
    /* The lamp: a disc in its colour, dimmer for a lower level (never so dim
       it vanishes on the panel), a ring round it when off. */
    if (off) {
        canvas_circle(c, cx, cy, r, CREAM_DIM);
        canvas_circle(c, cx, cy, r - 1, CREAM_DIM);
    } else {
        int f = 40 + pct * 215 / 100;
        canvas_disc(c, cx, cy, r, rgb565(k->r * f / 255, k->g * f / 255, k->b * f / 255));
    }
    aafont_draw(c, &aafont_inter_word, cx, cy + r + 14, k->name, CREAM, AAFONT_CENTRE);

    /* Brightness: five bars, the lit ones filled. */
    int bw = 22, gap = 8, total = LIGHT_N_LEVELS * bw + (LIGHT_N_LEVELS - 1) * gap;
    int bx = cx - total / 2, by = cy + r + 60;
    for (int i = 0; i < LIGHT_N_LEVELS; i++) {
        int h = 6 + i * 4;
        uint16_t col = i <= s->level && !off ? CREAM : CREAM_DIM;
        if (i <= s->level && !off) canvas_fill_rect(c, bx + i * (bw + gap), by + 22 - h, bw, h, col);
        else {
            canvas_fill_rect(c, bx + i * (bw + gap), by + 22 - h, bw, 1, col);
            canvas_fill_rect(c, bx + i * (bw + gap), by + 21, bw, 1, col);
        }
    }
    const char *heard = s->heard == LIGHT_HEARD_YES ? "blinky1 heard"
                      : s->heard == LIGHT_HEARD_NO ? "blinky1 not answering"
                      : s->heard == LIGHT_SENDING ? "sending..." : "BOOT: colour  tilt: brightness";
    aafont_draw(c, &aafont_inter_label, cx, c->h - 22, heard,
                s->heard == LIGHT_HEARD_NO ? rgb565(255, 140, 0) : CREAM_DIM, AAFONT_CENTRE);
}
