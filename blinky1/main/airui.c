#include "airui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "font_12x24.h"

/* ---- one-bit drawing ---------------------------------------------------- */

static void px(uint8_t *fb, int x, int y)
{
    if (x < 0 || x >= AIRUI_W || y < 0 || y >= AIRUI_H) return;
    fb[(y >> 3) * AIRUI_W + x] |= (uint8_t)(1u << (y & 7));
}

static bool glyph_bit(char ch, int x, int y)       /* the 12x24 font */
{
    if (ch < FONT_FIRST || ch > FONT_LAST) return false;
    const uint8_t *g = font_glyphs[ch - FONT_FIRST];
    unsigned row = ((unsigned)g[2 * y] << 8) | g[2 * y + 1];
    return row & (0x800u >> x);
}

/* The big font: 12x24, its ink in rows 4..17. Drawn rows 3..19 only, so a
   line of it is 17 px tall. */
static int big(uint8_t *fb, int x, int y, const char *s)
{
    for (; *s; s++, x += 12)
        for (int gy = 3; gy < 20; gy++)
            for (int gx = 0; gx < 12; gx++)
                if (glyph_bit(*s, gx, gy)) px(fb, x + gx, y + gy - 3);
    return x;
}

/* The small font: the same glyphs halved, 6x12 cells, ink about 7 px tall.
   A cell is lit if any of its 2x2 source pixels is: strokes are two pixels
   wide in the source, so none is lost. Drawn rows 2..9 of the half cell. */
static int small(uint8_t *fb, int x, int y, const char *s)
{
    for (; *s; s++, x += 6)
        for (int hy = 2; hy < 10; hy++)
            for (int hx = 0; hx < 6; hx++) {
                bool on = glyph_bit(*s, 2 * hx, 2 * hy) || glyph_bit(*s, 2 * hx + 1, 2 * hy)
                       || glyph_bit(*s, 2 * hx, 2 * hy + 1) || glyph_bit(*s, 2 * hx + 1, 2 * hy + 1);
                if (on) px(fb, x + hx, y + hy - 2);
            }
    return x;
}

static int small_w(const char *s) { return 6 * (int)strlen(s); }

static void hline(uint8_t *fb, int x0, int x1, int y, int dash)
{
    for (int x = x0; x <= x1; x++) if (!dash || (x % 3) == 0) px(fb, x, y);
}

static void line(uint8_t *fb, int x0, int y0, int x1, int y1)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for (;;) {
        px(fb, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * e;
        if (e2 >= dy) { e += dy; x0 += sx; }
        if (e2 <= dx) { e += dx; y0 += sy; }
    }
}

/* ---- the air in words and colour --------------------------------------- */

const char *airui_word(const air_now_t *n)
{
    if (n->validity == 2) return "first start";
    if (n->validity == 1) return "warming up";
    static const char *const W[] = { "?", "Excellent", "Good", "Moderate", "Poor", "Unhealthy" };
    return n->aqi >= 1 && n->aqi <= 5 ? W[n->aqi] : "?";
}

void airui_colour(const air_now_t *n, uint8_t *r, uint8_t *g, uint8_t *b)
{
    static const uint8_t C[6][3] = {
        { 0, 0, 60 },        /* no reading yet / warming: dim blue */
        { 0, 255, 0 },       /* 1 excellent */
        { 120, 255, 0 },     /* 2 good */
        { 255, 190, 0 },     /* 3 moderate */
        { 255, 80, 0 },      /* 4 poor */
        { 255, 0, 0 },       /* 5 unhealthy */
    };
    int k = n->validity == 0 && n->aqi >= 1 && n->aqi <= 5 ? n->aqi : 0;
    *r = C[k][0]; *g = C[k][1]; *b = C[k][2];
}

/* ---- pages -------------------------------------------------------------- */

static const char *const NAME[AIR_N] = { "Temp", "Humidity", "eCO2", "VOC" };
static const char *const RANGE[AIR_RANGES] = { "1h", "24h", "7d" };

static void fmt(char *out, size_t cap, int reading, int v)
{
    if (v == AIRLOG_NONE) { snprintf(out, cap, "--"); return; }
    switch (reading) {
    case AIR_TEMP: snprintf(out, cap, "%d.%dC", v / 10, (v < 0 ? -v : v) % 10); break;
    case AIR_RH:   snprintf(out, cap, "%d%%", (v + 5) / 10); break;
    case AIR_ECO2: snprintf(out, cap, "%dppm", v); break;
    default:       snprintf(out, cap, "%dppb", v); break;
    }
}

static void draw_now(uint8_t *fb, const air_now_t *n)
{
    char b[24];
    /* Top: the air in a word. */
    small(fb, 0, 0, "Air");
    small(fb, 24, 0, airui_word(n));
    /* Middle: temperature and humidity, big. */
    if (isnan(n->temp)) snprintf(b, sizeof b, "--");
    else snprintf(b, sizeof b, "%.1fC", (double)n->temp);
    big(fb, 0, 16, b);
    if (isnan(n->rh)) snprintf(b, sizeof b, "--");
    else snprintf(b, sizeof b, "%.0f%%", (double)n->rh);
    big(fb, AIRUI_W - 2 - 12 * (int)strlen(b), 16, b);   /* the % ink runs to the cell's edge */
    /* Bottom: the gas numbers. */
    if (n->validity == 0) {
        snprintf(b, sizeof b, "eCO2 %d", n->eco2);
        small(fb, 0, 44, b);
        snprintf(b, sizeof b, "VOC %d", n->tvoc);
        small(fb, AIRUI_W - small_w(b), 44, b);
        snprintf(b, sizeof b, "AQI %d of 5", n->aqi);
        small(fb, 0, 56, b);
    } else {
        small(fb, 0, 50, n->validity == 2 ? "gas sensor: ~1 hour" : "gas sensor: ~3 min");
    }
}

static void draw_chart(uint8_t *fb, int reading, air_range_t r, const air_now_t *now, const airlog_t *log)
{
    int16_t s[AIRLOG_DAY];
    int n = airlog_series(log, reading, r, s);
    char t[24], v[16];
    snprintf(t, sizeof t, "%s %s", NAME[reading], RANGE[r]);
    small(fb, 0, 0, t);
    /* The latest value at the top right: the live one, not the last point. */
    int live = AIRLOG_NONE;
    if (reading == AIR_TEMP && !isnan(now->temp)) live = (int)lroundf(now->temp * 10);
    if (reading == AIR_RH && !isnan(now->rh)) live = (int)lroundf(now->rh * 10);
    if (reading == AIR_ECO2 && now->validity == 0) live = now->eco2;
    if (reading == AIR_TVOC && now->validity == 0) live = now->tvoc;
    fmt(v, sizeof v, reading, live);
    small(fb, AIRUI_W - small_w(v), 0, v);

    int lo = 32767, hi = -32768, have = 0;
    for (int i = 0; i < n; i++) if (s[i] != AIRLOG_NONE) { have++; if (s[i] < lo) lo = s[i]; if (s[i] > hi) hi = s[i]; }
    const int top = 12, bottom = 63, left = 0, right = AIRUI_W - 1;
    if (have < 2) {
        small(fb, 0, 32, r == AIR_RANGE_HOUR ? "collecting..." : r == AIR_RANGE_DAY ? "first point: 15 min" : "first point: 2 hours");
        return;
    }
    /* Pad a flat line so it does not fill the whole height with noise. */
    int minspan = reading == AIR_TEMP ? 10 : reading == AIR_RH ? 20 : reading == AIR_ECO2 ? 100 : 50;
    if (hi - lo < minspan) { int mid = (hi + lo) / 2; lo = mid - minspan / 2; hi = lo + minspan; }
    hline(fb, left, right, bottom, 1);
    hline(fb, left, right, top + 8, 1);
    /* Max and min, small, at the left of their gridlines. */
    fmt(v, sizeof v, reading, hi);
    small(fb, 0, top + 10, v);
    fmt(v, sizeof v, reading, lo);
    small(fb, 0, bottom - 9, v);
    /* The line, oldest at the left; a range not yet full starts part way. */
    int len = r == AIR_RANGE_HOUR ? AIRLOG_HOUR : r == AIR_RANGE_DAY ? AIRLOG_DAY : AIRLOG_WEEK;
    int px0 = -1, py0 = -1;
    for (int i = 0; i < n; i++) {
        if (s[i] == AIRLOG_NONE) { px0 = -1; continue; }
        int x = left + (right - left) * (len - n + i) / (len - 1);
        int y = (top + 8) + (bottom - top - 8) * (hi - s[i]) / (hi - lo);
        if (px0 >= 0) line(fb, px0, py0, x, y);
        else px(fb, x, y);
        px0 = x; py0 = y;
    }
}

void airui_draw(uint8_t *fb, int page, const air_now_t *now, const airlog_t *log)
{
    memset(fb, 0, AIRUI_W * AIRUI_H / 8);
    if (page <= 0 || page >= AIRUI_PAGES) { draw_now(fb, now); return; }
    int k = page - 1;
    draw_chart(fb, k / AIR_RANGES, (air_range_t)(k % AIR_RANGES), now, log);
}
