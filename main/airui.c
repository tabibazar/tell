#include "airui.h"

#include "aafont.h"
#include "vector.h"

#include <math.h>
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

/* ---- AIR 24H -------------------------------------------------------------- */

#define D_X0        16
#define D_X1        224
#define D_H         100
#define COL_TINT    0x00E7      /* navy under VOC's GOOD zone, as on envo */

/* The zoned scale, as on envo's full chart: GOOD gets the most height, POOR
   the least, and a value over the top is pinned to it. */
typedef struct { float lo, fair, poor, top; } zones_t;
static const zones_t s_voc_z = { 0.0f, 220.0f, 650.0f, 2200.0f };
static const zones_t s_eco2_z = { 400.0f, 800.0f, 1000.0f, 1500.0f };
#define Z_GOOD      44
#define Z_FAIR      34
#define Z_POOR      22

/* Height above the chart's floor for a value, in px. */
static float zone_y(const zones_t *z, float v)
{
    if (v <= z->lo) return 0.0f;
    if (v < z->fair) return (v - z->lo) / (z->fair - z->lo) * Z_GOOD;
    if (v < z->poor) return Z_GOOD + (v - z->fair) / (z->poor - z->fair) * Z_FAIR;
    if (v < z->top) return Z_GOOD + Z_FAIR + (v - z->poor) / (z->top - z->poor) * Z_POOR;
    return (float)D_H;
}

static uint16_t zone_col(const zones_t *z, float v)
{
    return v >= z->poor ? COL_POOR : v >= z->fair ? COL_FAIR : COL_WHITE;
}

static void chart(canvas_t *c, int top, const char *title, const char *unit, const zones_t *z,
                  const float *v, int now_slot, bool tint)
{
    aafont_draw(c, F_LABEL, D_X0, top, title, COL_WHITE, AAFONT_LEFT);
    aafont_draw(c, F_LABEL, D_X1, top, unit, COL_GREY, AAFONT_RIGHT);
    int y0 = top + 22, floor_y = y0 + D_H;
    if (tint) canvas_fill_rect(c, D_X0, floor_y - Z_GOOD, D_X1 - D_X0, Z_GOOD, COL_TINT);
    canvas_fill_rect(c, D_X0, floor_y - Z_GOOD, D_X1 - D_X0, 1, COL_FAIR);
    canvas_fill_rect(c, D_X0, floor_y - Z_GOOD - Z_FAIR, D_X1 - D_X0, 1, COL_POOR);
    canvas_fill_rect(c, D_X0, floor_y, D_X1 - D_X0, 1, COL_RULE);

    /* Hour marks every six hours: a tick under the floor and the hour. */
    int cols = D_X1 - D_X0;
    for (int k = 0; k < AIRUI_SLOTS; k++) {
        int slot_tod = (now_slot - (AIRUI_SLOTS - 1 - k) + AIRUI_SLOTS * 2) % AIRUI_SLOTS;
        if (slot_tod % 72 != 0) continue;             /* 72 slots = 6 h */
        int x = D_X0 + k * cols / AIRUI_SLOTS;
        canvas_fill_rect(c, x, floor_y + 1, 1, 3, COL_GREY);
        if (slot_tod == 0) canvas_fill_rect(c, x, y0, 1, D_H, COL_RULE);   /* midnight */
    }

    /* The trace: each column the mean of its slots, joined to the one before
       by a vertical run, in the colour of the state it is in. */
    float prev = NAN;
    for (int x = 0; x < cols; x++) {
        int k0 = x * AIRUI_SLOTS / cols, k1 = (x + 1) * AIRUI_SLOTS / cols;
        double sum = 0;
        int n = 0;
        for (int k = k0; k < k1 && k < AIRUI_SLOTS; k++)
            if (v[k] == v[k]) { sum += v[k]; n++; }
        if (n == 0) { prev = NAN; continue; }
        float m = (float)(sum / n);
        int y = floor_y - 1 - (int)(zone_y(z, m) + 0.5f);
        int yp = prev == prev ? floor_y - 1 - (int)(zone_y(z, prev) + 0.5f) : y;
        int ya = y < yp ? y : yp, yb = y < yp ? yp : y;
        canvas_fill_rect(c, D_X0 + x, ya - 1, 1, yb - ya + 2, zone_col(z, m));
        prev = m;
    }
}

static void hour_labels(canvas_t *c, int y, int now_slot)
{
    int cols = D_X1 - D_X0;
    for (int k = 0; k < AIRUI_SLOTS; k++) {
        int slot_tod = (now_slot - (AIRUI_SLOTS - 1 - k) + AIRUI_SLOTS * 2) % AIRUI_SLOTS;
        if (slot_tod % 72 != 0) continue;
        int x = D_X0 + k * cols / AIRUI_SLOTS;
        char b[4];
        snprintf(b, sizeof b, "%02d", slot_tod / 12);
        if (x > D_X0 + 10 && x < D_X1 - 30) aafont_draw(c, F_LABEL, x, y, b, COL_GREY, AAFONT_CENTRE);
    }
    aafont_draw(c, F_LABEL, D_X1, y, "NOW", COL_GREY, AAFONT_RIGHT);
}

void airui_draw_day(canvas_t *c, const airui_day_t *d)
{
    if (c == NULL || c->fb == NULL || c->w <= 0 || c->h <= 0) return;
    canvas_fill_rect(c, 0, 0, c->w, c->h, COL_BG);
    if (d == NULL) return;
    if (!d->have_sensor) {
        aafont_draw(c, F_WORD, c->w / 2, 130, "NO AIR SENSOR", COL_GREY, AAFONT_CENTRE);
        return;
    }
    chart(c, 12, "VOC 24H", "ppb", &s_voc_z, d->voc, d->now_slot, true);
    chart(c, 158, "eCO2 est 24H", "ppm", &s_eco2_z, d->eco2, d->now_slot, false);
    hour_labels(c, 158 + 22 + D_H + 8, d->now_slot);
}

/* ---- the week -------------------------------------------------------------- */

#define WK_TOP      60
#define WK_PITCH    30
#define WK_CELL_H   20
#define WK_X0       50
#define WK_CW       7
#define WK_CELL_W   5

void airui_draw_week(canvas_t *c, const airui_week_t *w)
{
    if (c == NULL || c->fb == NULL || c->w <= 0 || c->h <= 0) return;
    canvas_fill_rect(c, 0, 0, c->w, c->h, COL_BG);
    if (w == NULL) return;
    aafont_draw(c, F_LABEL, XL, 14, "AIR WEEK", COL_WHITE, AAFONT_LEFT);
    if (!w->have_sensor) {
        aafont_draw(c, F_WORD, c->w / 2, 130, "NO AIR SENSOR", COL_GREY, AAFONT_CENTRE);
        return;
    }
    bool any = false;
    for (int d = 0; d < 7; d++) {
        int y = WK_TOP + d * WK_PITCH;
        aafont_draw(c, F_LABEL, XL, y + (WK_CELL_H - F_LABEL->cap) / 2, w->day[d],
                    d == 6 ? COL_WHITE : COL_GREY, AAFONT_LEFT);
        for (int h = 0; h < 24; h++) {
            int x = WK_X0 + h * WK_CW;
            switch (w->cell[d][h]) {
            case AIRUI_CELL_NONE:
                canvas_fill_rect(c, x + WK_CELL_W / 2, y + WK_CELL_H / 2, 1, 1, COL_RULE);
                break;
            case AIRUI_CELL_OK:
                canvas_fill_rect(c, x, y + WK_CELL_H - 4, WK_CELL_W, 4, COL_GOOD);
                any = true;
                break;
            case AIRUI_CELL_FAIR:
                canvas_fill_rect(c, x, y + WK_CELL_H / 2, WK_CELL_W, WK_CELL_H - WK_CELL_H / 2, COL_FAIR);
                any = true;
                break;
            case AIRUI_CELL_POOR:
                canvas_fill_rect(c, x, y, WK_CELL_W, WK_CELL_H, COL_POOR);
                any = true;
                break;
            default:
                break;
            }
        }
    }
    /* The takeaway, as on envo's week, on its own line under the title:
       hours that went bad, or that none did. */
    char b[24];
    const int ty = 34;
    if (w->poor_hours == 0 && w->fair_hours == 0) {
        aafont_draw(c, F_LABEL, XL, ty, any ? "ALL GOOD" : "NO DATA", any ? COL_GOOD : COL_GREY, AAFONT_LEFT);
    } else {
        int x = XL;
        if (w->poor_hours > 0) {
            snprintf(b, sizeof b, "POOR %dH", w->poor_hours);
            x += aafont_draw(c, F_LABEL, x, ty, b, COL_POOR, AAFONT_LEFT) + 14;
        }
        if (w->fair_hours > 0) {
            snprintf(b, sizeof b, "FAIR %dH", w->fair_hours);
            aafont_draw(c, F_LABEL, x, ty, b, COL_FAIR, AAFONT_LEFT);
        }
    }
    int ly = WK_TOP + 7 * WK_PITCH + 2;
    static const char *const hl[5] = { "00", "06", "12", "18", "24" };
    for (int k = 0; k < 5; k++) {
        int x = WK_X0 + k * 6 * WK_CW - (k == 4 ? 2 : 0);
        aafont_draw(c, F_LABEL, x, ly, hl[k], COL_GREY, k == 0 ? AAFONT_LEFT : k == 4 ? AAFONT_RIGHT : AAFONT_CENTRE);
    }
}
