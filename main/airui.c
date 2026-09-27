#include "airui.h"

#include "aafont.h"
#include "vector.h"

#include <stdio.h>

/* envo's colours: state is the only thing with colour on the page. */
#define COL_BG      0x0000
#define COL_WHITE   0xFFFF
#define COL_GREY    0x8410
#define COL_RULE    0x31A6
#define COL_GOOD    0x04BF
#define COL_FAIR    0xFD40
#define COL_POOR    0xF8C1

#define F_LABEL     (&aafont_inter_label)       /* capitals 13 */
#define F_SUB       (&aafont_inter_sub)         /* 17 */
#define F_WORD      (&aafont_inter_word)        /* 25 */
#define F_STATE     (&aafont_inter_state)       /* 32 */
#define F_READING   (&aafont_inter_reading)     /* 57 */
#define F_WORD_K    (&aafont_inter_word_knock)  /* POOR's black on red */

#define XL          16
#define XR          (AIRUI_WIDTH - 16)

static uint16_t state_col(envs_state_t st)
{
    return st == ENVS_POOR ? COL_POOR : st == ENVS_FAIR ? COL_FAIR : COL_GOOD;
}

static const char *state_word(envs_state_t st)
{
    return st == ENVS_POOR ? "POOR" : st == ENVS_FAIR ? "FAIR" : "GOOD";
}

/* A state word at (x, top): blue or amber type, or POOR knocked out of a red
   block, as envo sets it. */
static void word(canvas_t *c, int x, int top, envs_state_t st, const char *w, bool right)
{
    int ww = aafont_width(F_WORD, w);
    if (right) x -= ww + (st == ENVS_POOR ? 6 : 0);
    if (st == ENVS_POOR) {
        canvas_fill_rect(c, x - 6, top - 6, ww + 12, F_WORD->cap + 12, COL_POOR);
        aafont_draw(c, F_WORD_K, x, top, w, 0x0000, AAFONT_LEFT);
    } else {
        aafont_draw(c, F_WORD, x, top, w, state_col(st), AAFONT_LEFT);
    }
}

static void arrow(canvas_t *c, float cx, float cy, float s, bool up, uint16_t col)
{
    float sg = up ? -1.0f : 1.0f;
    float xy[] = {
        cx - s * 0.13f, cy - sg * s / 2, cx - s * 0.13f, cy + sg * s * 0.02f,
        cx - s * 0.4f,  cy + sg * s * 0.02f, cx, cy + sg * s / 2,
        cx + s * 0.4f,  cy + sg * s * 0.02f, cx + s * 0.13f, cy + sg * s * 0.02f,
        cx + s * 0.13f, cy - sg * s / 2,
    };
    vec_polygon(c, xy, 7, col);
}

/* VOC: label and unit, the big number, its word, a trend arrow when moving. */
static void voc(canvas_t *c, const airui_t *s)
{
    aafont_draw(c, F_LABEL, XL, 14, "VOC", COL_GREY, AAFONT_LEFT);
    aafont_draw(c, F_LABEL, XR, 14, "ppb", COL_GREY, AAFONT_RIGHT);
    int top = 38;
    if (s->warming || s->gas_error) {
        const char *t = s->gas_error ? "GAS ERROR" : "WARMING UP";
        const aafont_t *f = aafont_width(F_WORD, t) <= XR - XL ? F_WORD : F_SUB;
        aafont_draw(c, f, XL, top + 6, t, COL_GREY, AAFONT_LEFT);
        if (!s->gas_error) {
            char b[32];
            if (s->warm_minutes < 1) snprintf(b, sizeof b, "JUST STARTED");
            else if (s->warm_minutes < 60) snprintf(b, sizeof b, "%d MIN SO FAR", s->warm_minutes);
            else snprintf(b, sizeof b, "%d H %d MIN SO FAR", s->warm_minutes / 60, s->warm_minutes % 60);
            aafont_draw(c, F_SUB, XL, top + 46, b, COL_GREY, AAFONT_LEFT);
        }
        return;
    }
    if (!s->have_voc) {
        aafont_draw(c, F_READING, XL, top, "--", COL_GREY, AAFONT_LEFT);
        return;
    }
    char b[16];
    snprintf(b, sizeof b, "%ld", (long)envs_quantise(ENVS_VOC, s->voc_ppb));
    aafont_draw(c, F_READING, XL, top, b, COL_WHITE, AAFONT_LEFT);
    int wt = top + F_READING->cap + 16;
    word(c, XL, wt, s->voc_state, state_word(s->voc_state), false);
    /* The trend on the word's line, at the right: beside the number a
       four-digit reading ran into it. */
    if (s->voc_trend == ENVS_RISING || s->voc_trend == ENVS_FALLING)
        arrow(c, (float)XR - 12.0f, (float)wt + (float)F_WORD->cap / 2.0f, 28.0f, s->voc_trend == ENVS_RISING, COL_WHITE);
}

/* eCO2 est: a rule over it, the number at the word's size, its word or FROM VOCs. */
static void eco2(canvas_t *c, const airui_t *s)
{
    int y = 170;
    canvas_fill_rect(c, XL, y - 12, XR - XL, 1, COL_RULE);
    aafont_draw(c, F_LABEL, XL, y, "eCO2 est", COL_GREY, AAFONT_LEFT);
    aafont_draw(c, F_LABEL, XR, y, "ppm", COL_GREY, AAFONT_RIGHT);
    if (s->warming || s->gas_error || !s->have_eco2) {
        aafont_draw(c, F_STATE, XL, y + 24, "--", COL_GREY, AAFONT_LEFT);
        return;
    }
    char b[16];
    snprintf(b, sizeof b, "%ld", (long)envs_quantise(ENVS_ECO2, s->eco2_ppm));
    /* The word stands at the right on the number's baseline; a number that
       would run into it steps down a size. */
    const char *wtxt = s->eco2_state == ENVS_OK ? NULL : state_word(s->eco2_state);
    int room_w = XR - XL - (wtxt ? aafont_width(F_WORD, wtxt) + 24 : 0);
    const aafont_t *nf = aafont_width(F_STATE, b) <= room_w ? F_STATE : F_WORD;
    int base = y + 24 + F_STATE->cap;
    aafont_draw(c, nf, XL, base - nf->cap, b, COL_WHITE, AAFONT_LEFT);
    int wt = base - F_WORD->cap;
    if (s->eco2_state == ENVS_OK)
        aafont_draw(c, F_LABEL, XR, y + 24 + F_STATE->cap - F_LABEL->cap, "FROM VOCs", COL_GREY, AAFONT_RIGHT);
    else word(c, XR, wt, s->eco2_state, state_word(s->eco2_state), true);
}

/* The room, raw from the AHT21: two readings side by side. */
static void room(canvas_t *c, const airui_t *s)
{
    int y = 250;
    canvas_fill_rect(c, XL, y - 12, XR - XL, 1, COL_RULE);
    aafont_draw(c, F_LABEL, XL, y, "TEMP", COL_GREY, AAFONT_LEFT);
    aafont_draw(c, F_LABEL, AIRUI_WIDTH / 2 + 8, y, "HUMIDITY", COL_GREY, AAFONT_LEFT);
    char b[16];
    if (s->have_temp) snprintf(b, sizeof b, "%.1f\xC2\xB0", (double)s->temp_c);
    else snprintf(b, sizeof b, "--");
    aafont_draw(c, F_WORD, XL, y + 22, b, s->have_temp ? COL_WHITE : COL_GREY, AAFONT_LEFT);
    if (s->have_rh) snprintf(b, sizeof b, "%.0f%%", (double)s->rh);
    else snprintf(b, sizeof b, "--");
    aafont_draw(c, F_WORD, AIRUI_WIDTH / 2 + 8, y + 22, b, s->have_rh ? COL_WHITE : COL_GREY, AAFONT_LEFT);
}

void airui_draw(canvas_t *c, const airui_t *s)
{
    if (c == NULL || c->fb == NULL || c->w <= 0 || c->h <= 0) return;
    canvas_fill_rect(c, 0, 0, c->w, c->h, COL_BG);
    if (s == NULL) return;
    if (!s->have_sensor) {
        aafont_draw(c, F_WORD, c->w / 2, 130, "NO AIR SENSOR", COL_GREY, AAFONT_CENTRE);
        aafont_draw(c, F_LABEL, c->w / 2, 170, "ENS160 not found on the bus", COL_GREY, AAFONT_CENTRE);
        return;
    }
    bool was = vec_linear_light(true);
    voc(c, s);
    eco2(c, s);
    room(c, s);
    vec_linear_light(was);
}
