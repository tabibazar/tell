/* For mkdir, which C11 alone does not declare. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "envui.h"
#include "aafont.h"
#include "vector.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * envo's air pages on the host: that no page ever writes outside the
 * framebuffer whatever it is handed, that the same input draws the same
 * pixels, that the rules the spec makes about colour hold in the pixels
 * (eCO2 is never blue, TEMP and HUMIDITY carry no state colour, an invalid
 * reading never colours anything), that the scale is fixed, that the number
 * is big enough to read across a room, that text keeps inside the margins
 * and the clock's digits hold still -- and renders of every page in every
 * state, the clock page's included, to look at beside docs/design/envo-ui/ref/.
 *
 * The renders go to host_tests/renders/envui/ as 24-bit BMPs, each also at
 * three times the size (pixels repeated, not smoothed). Turn each into a PNG
 * with
 *     sips -s format png X.bmp --out X.png
 */

/*
 * envui draws with envstate's limits table. Until envstate.c exists this
 * stands in for it -- the Makefile defines ENVUI_LIMITS_STUB only when the
 * real file is missing, so the stand-in drops out by itself once it lands.
 * The numbers are the spec's (ENS160 datasheet v1.3, Tables 5-6).
 */
#ifdef ENVUI_LIMITS_STUB
const envs_limits_t *envs_limits(envs_series_t s)
{
    static const envs_limits_t voc = { 220, 650, 2200, 0 }, eco2 = { 800, 1000, 1500, 400 };
    return s == ENVS_VOC ? &voc : s == ENVS_ECO2 ? &eco2 : NULL;
}
#endif

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

#define W 320
#define H 172
#define GUARD 64
#define CANARY 0xA5C3

#define WHITE 0xFFFF
#define GOOD  0x04BF
#define FAIR  0xFD40
#define POOR  0xF8C1
#define TINT  0x00E7
#define GREY  0x8410
#define RULE  0x5ACB

/* Two framebuffers with a guard band of canaries either side: one to draw,
   one to draw the same thing again and compare. */
static uint16_t s_mem[GUARD + W * H + GUARD], s_mem2[GUARD + W * H + GUARD];

static void guard_fill(uint16_t *mem)
{
    for (size_t i = 0; i < GUARD + W * H + GUARD; i++) mem[i] = CANARY;
}

static int guard_ok(const uint16_t *mem)
{
    for (size_t i = 0; i < GUARD; i++)
        if (mem[i] != CANARY || mem[GUARD + W * H + i] != CANARY) return 0;
    return 1;
}

static canvas_t canvas_on(uint16_t *mem)
{
    canvas_t c;
    guard_fill(mem);
    canvas_init(&c, mem + GUARD, W, H, 1);
    return c;
}

/* ---- a day of air, deterministic -------------------------------------- */

/* A small LCG rather than rand(): the renders and the determinism check must
   come out the same on every host. */
static uint32_t s_seed;
static float noise(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (float)(s_seed >> 8) / (float)(1u << 24) * 2.0f - 1.0f;
}

/* Now is Friday 14:30, so slot i ends at 14:30 - (287 - i) * 5 minutes,
   counted from Friday 00:00 (negative is Thursday). Midnight is slot 113. */
#define N ENVUI_SLOTS
#define NOW_MIN (14 * 60 + 30)
static int slot_minute(int i) { return NOW_MIN - (N - 1 - i) * 5; }
#define MIDNIGHT_SLOT (N - 1 - NOW_MIN / 5)

static float s_voc[N], s_co2[N], s_tc[N], s_rh[N];
static bool s_ok[N];

/*
 * Thursday's dinner at 18:15 and Friday's lunch at 12:30 lift the VOCs; the
 * power bank ran flat 03:10-06:00 and the gas sensor then spent 5 minutes
 * warming up, both gaps. `now_voc` > 0 ramps the last 40 minutes up to it.
 * The eCO2 is invented from the VOCs, as in the mockups: it fills a page.
 */
static void make_day(float now_voc)
{
    s_seed = 7;
    for (int i = 0; i < N; i++) {
        int m = slot_minute(i);
        float v = 40.0f + 7.0f * noise();
        float dt = (float)(m - (-24 * 60 + 18 * 60 + 15));
        if (dt >= 0) v += 380.0f * expf(-dt / 55.0f);
        dt = (float)(m - (12 * 60 + 30));
        if (dt >= 0) v += 190.0f * expf(-dt / 40.0f);
        s_voc[i] = v;
        s_ok[i] = !(m >= 3 * 60 + 10 && m < 6 * 60 + 5);
        float occ = (m > -24 * 60 + 17 * 60 && m < 60) || (m > 7 * 60 && m < 9 * 60) || m > 12 * 60 ? 1.0f : 0.0f;
        s_co2[i] = 400.0f + 0.85f * (v - 30.0f) + occ * 60.0f + 10.0f * noise();
        if (s_co2[i] < 400.0f) s_co2[i] = 400.0f;
        float h = fmodf((float)m / 60.0f + 48.0f, 24.0f);
        s_tc[i] = 25.6f + 1.3f * sinf((h - 9.0f) / 24.0f * 6.2832f) + 0.08f * noise();
        s_rh[i] = 41.0f - 4.0f * sinf((h - 9.0f) / 24.0f * 6.2832f) + 0.5f * noise() + ((h > 7.2f && h < 7.6f) ? 8.0f : 0.0f);
    }
    if (now_voc > 0) {
        for (int k = 0; k < 8; k++) {
            int i = N - 1 - k;
            s_voc[i] = now_voc * (1.0f - (float)k * 0.11f);
            s_co2[i] = 400.0f + 0.85f * (s_voc[i] - 30.0f) + 60.0f;
        }
    }
}

static envui_series_t s_ser;

/* One series of the day above, in envstate's units, with the day's peak as
   main.c's air_series builds it: VOC's the stored 30 s maximum, which stands
   a little above the 5-minute mean (here 12 %); eCO2's, which has no stored
   maximum, the highest 5-minute mean itself. */
static const envui_series_t *series(envs_series_t which, int32_t now, envs_state_t st, envs_trend_t tr)
{
    envui_series_t *s = &s_ser;
    memset(s, 0, sizeof *s);
    s->series = which;
    s->peak_slot = -1;
    float best = -1.0f;
    for (int i = 0; i < N; i++) {
        float v = which == ENVS_VOC ? s_voc[i] : which == ENVS_ECO2 ? s_co2[i]
                : which == ENVS_TEMP ? s_tc[i] * 100.0f : s_rh[i] * 100.0f;
        s->slot[i] = (int32_t)lroundf(v);
        s->valid[i] = s_ok[i];
        if ((which == ENVS_VOC || which == ENVS_ECO2) && s_ok[i] && v > best) {
            best = v;
            s->peak_slot = i;
            s->peak_value = (int32_t)lroundf(which == ENVS_VOC ? v * 1.12f : v);
        }
    }
    s->midnight_slot = MIDNIGHT_SLOT;
    s->midnight_day = "FRI";
    s->last_slot_hour = NOW_MIN / 60;
    s->have_now = true;
    s->now_value = now;
    s->state = st;
    s->trend = tr;
    return s;
}

/* ---- BMP ---------------------------------------------------------------- */

static void put16(FILE *f, unsigned v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
static void put32(FILE *f, unsigned long v)
{
    put16(f, (unsigned)(v & 0xFFFF));
    put16(f, (unsigned)((v >> 16) & 0xFFFF));
}

/* A 24-bit BMP of an RGB565 image, each pixel repeated `zoom` times each
   way. 565 goes to 888 by repeating the top bits into the bottom, so white
   stays 255 and black 0. */
static int write_bmp(const char *path, const uint16_t *fb, int zoom)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) return 0;
    int ow = W * zoom, oh = H * zoom;
    int stride = (ow * 3 + 3) & ~3;
    unsigned long size = 54ul + (unsigned long)stride * (unsigned long)oh;
    fputc('B', f); fputc('M', f);
    put32(f, size); put32(f, 0); put32(f, 54);
    put32(f, 40); put32(f, (unsigned long)ow); put32(f, (unsigned long)oh);
    put16(f, 1); put16(f, 24); put32(f, 0);
    put32(f, (unsigned long)stride * (unsigned long)oh);
    put32(f, 2835); put32(f, 2835); put32(f, 0); put32(f, 0);
    for (int y = oh - 1; y >= 0; y--) {        /* bottom-up */
        int n = 0;
        for (int x = 0; x < ow; x++) {
            uint16_t p = fb[(y / zoom) * W + x / zoom];
            unsigned r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
            fputc((int)((b << 3) | (b >> 2)), f);
            fputc((int)((g << 2) | (g >> 4)), f);
            fputc((int)((r << 3) | (r >> 2)), f);
            n += 3;
        }
        while (n < stride) { fputc(0, f); n++; }
    }
    fclose(f);
    return 1;
}

static void render(const char *name, const uint16_t *fb)
{
    char path[256];
    snprintf(path, sizeof path, "renders/envui/%s.bmp", name);
    int ok = write_bmp(path, fb, 1);
    snprintf(path, sizeof path, "renders/envui/%s_3x.bmp", name);
    ok = ok && write_bmp(path, fb, 3);
    char what[300];
    snprintf(what, sizeof what, "render %s written", name);
    expect(what, ok);
}

/* ---- drawing, checked --------------------------------------------------- */

#define PAGES 6
enum { DRAW_READING, DRAW_DETAIL };

/*
 * Draws a page twice, into two guarded buffers, and checks both the guards
 * and that the two came out byte for byte the same: a page that depends on
 * anything but its input (an uninitialised variable, leftover static state
 * from the page before) shows up here as a difference.
 */
static const uint16_t *draw(int kind, const envui_series_t *s, int page, const char *name)
{
    canvas_t a = canvas_on(s_mem), b = canvas_on(s_mem2);
    if (kind == DRAW_READING) { envui_reading(&a, s, page, PAGES); envui_reading(&b, s, page, PAGES); }
    else                      { envui_detail(&a, s); envui_detail(&b, s); }
    char what[200];
    snprintf(what, sizeof what, "%s stays inside the framebuffer", name);
    expect(what, guard_ok(s_mem) && guard_ok(s_mem2));
    snprintf(what, sizeof what, "%s draws the same pixels twice", name);
    expect(what, memcmp(s_mem, s_mem2, sizeof s_mem) == 0);
    if (name[0] != '!') render(name, s_mem + GUARD);
    return s_mem + GUARD;
}

static int count(const uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t col)
{
    int n = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (fb[y * W + x] == col) n++;
    return n;
}

/* Nothing but black in a box. */
static int dark(const uint16_t *fb, int x0, int y0, int x1, int y1)
{
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (fb[y * W + x] != 0) return 0;
    return 1;
}

/* The first and last rows in a box holding a colour exactly; the count of
   rows between them, or 0 when it is not there. */
static int rows_with(const uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t col,
                     int *top, int *bottom)
{
    *top = -1; *bottom = -1;
    for (int y = y0; y <= y1; y++)
        if (count(fb, x0, y, x1, y, col) > 0) {
            if (*top < 0) *top = y;
            *bottom = y;
        }
    return *top < 0 ? 0 : *bottom - *top + 1;
}

/* Rows that hold near-white ink in a box: the anti-aliased edge of a white
   stroke fades towards black, so "near" is every channel well over half on --
   over, not at, so the grey labels (0x8410, exactly half) do not count. */
static int ink_rows(const uint16_t *fb, int x0, int y0, int x1, int y1, int *top, int *bottom)
{
    *top = -1; *bottom = -1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            uint16_t p = fb[y * W + x];
            if (((p >> 11) & 0x1F) >= 20 && ((p >> 5) & 0x3F) >= 40 && (p & 0x1F) >= 20) {
                if (*top < 0) *top = y;
                *bottom = y;
                break;
            }
        }
    return *top < 0 ? 0 : *bottom - *top + 1;
}

/* Nothing lit in the panel's rounded corners: the 14 px bands at top and
   bottom, outside x 12..308. */
static int corners_dark(const uint16_t *fb)
{
    for (int y = 0; y < H; y++) {
        if (y >= 14 && y < H - 14) continue;
        for (int x = 0; x < W; x++)
            if ((x < 12 || x > 308) && fb[y * W + x] != 0) return 0;
    }
    return 1;
}

/* ---- the tests ---------------------------------------------------------- */

static void test_reading_pages(void)
{
    const uint16_t *fb;
    int top, bottom;

    make_day(0);
    fb = draw(DRAW_READING, series(ENVS_VOC, 47, ENVS_OK, ENVS_STEADY), 0, "voc_good");
    int h = ink_rows(fb, 16, 20, 170, 100, &top, &bottom);
    printf("     the VOC number's ink: rows %d..%d, %d px\n", top, bottom, h);
    expect("the number is at least 56 px tall, readable across a room", h >= 56);
    expect("GOOD is blue", count(fb, 16, 90, 200, 136, GOOD) > 100);
    expect("and nothing is amber or red at 45 ppb above the strip",
           count(fb, 0, 0, W - 1, 134, FAIR) == 0 && count(fb, 0, 0, W - 1, 134, POOR) == 0);
    expect("the corners stay dark", corners_dark(fb));
    /* The blue tint is the full chart's: in a 14 px strip six rows of it
       read as data. */
    expect("the strip has no GOOD tint", count(fb, 0, 136, W - 1, 152, TINT) == 0);

    make_day(262);
    fb = draw(DRAW_READING, series(ENVS_VOC, 262, ENVS_FAIR, ENVS_RISING), 0, "voc_fair");
    expect("FAIR is amber", count(fb, 16, 90, 200, 136, FAIR) > 100);
    expect("the corners stay dark", corners_dark(fb));

    make_day(1366);
    fb = draw(DRAW_READING, series(ENVS_VOC, 1366, ENVS_POOR, ENVS_RISING), 0, "voc_poor");
    expect("POOR is a red block", count(fb, 16, 90, 200, 136, POOR) > 1500);
    expect("the corners stay dark", corners_dark(fb));
    /* The block begins its side pad, 6 px, left of the margin so the letters
       on it line up with the number's at x 16. */
    expect("POOR's block starts left of the text margin, its letters on it",
           count(fb, 10, 94, 15, 133, POOR) == 6 * 40 && count(fb, 0, 90, 9, 136, POOR) == 0
           && fb[112 * W + 16] != POOR);

    /*
     * The number is as big as fits beside the trend word: 1300 is too wide
     * for the full size beside STEADY, so it steps down one and keeps 10 px
     * clear of the word's grey ink on the rows they share.
     */
    make_day(1366);
    fb = draw(DRAW_READING, series(ENVS_VOC, 1366, ENVS_POOR, ENVS_STEADY), 0, "voc_poor_steady");
    {
        int nt, nb, right = -1, left = W;
        ink_rows(fb, 16, 20, 304, 96, &nt, &nb);
        for (int y = 73; y <= 89; y++)
            for (int x = 16; x < 304; x++) {
                uint16_t p = fb[y * W + x];
                if (p == WHITE && x > right) right = x;
                if (p == GREY && x < left) left = x;
            }
        printf("     1300 beside STEADY: rows %d..%d, its ink to x %d, STEADY's from x %d\n",
               nt, nb, right, left);
        expect("a wide number beside STEADY steps down a size", nb - nt + 1 < 56 && nb - nt + 1 >= 46);
        expect("and keeps 10 px clear of the word", right >= 0 && right + 10 < left);
    }

    /* A temperature keeps its full size beside STEADY: its degree sign hangs
       from the top of the digits, above the word, so only the digits need to
       clear it. */
    make_day(0);
    fb = draw(DRAW_READING, series(ENVS_TEMP, 2687, ENVS_OK, ENVS_STEADY), 2, "temp_steady");
    {
        int nt, nb, digits = -1, deg = -1;
        ink_rows(fb, 16, 20, 200, 96, &nt, &nb);
        for (int y = 33; y <= 89; y++)
            for (int x = 16; x < 304; x++)
                if (fb[y * W + x] == WHITE) {
                    if (y >= 66 && x > digits) digits = x;   /* below the degree sign */
                    if (y < 66 && x > deg) deg = x;
                }
        printf("     26.8 beside STEADY: rows %d..%d, digits to x %d, degree sign to x %d\n",
               nt, nb, digits, deg);
        expect("a temperature beside STEADY keeps the full size", nb - nt + 1 >= 56);
        expect("its degree sign stands over the word's shoulder, not in it",
               deg > digits && dark(fb, digits + 2, 61, 304, 72)
               && count(fb, 200, 73, 304, 89, GREY) > 50);
    }

    /*
     * The digits hold still: the reading is set by its advance box, so going
     * from 19.9 to 20.0 changes the digits and nothing else -- the point and
     * the degree sign stand on the same pixels. Set by its ink, the number
     * moved with its first digit's side bearing, 2 px here.
     */
    {
        static uint16_t a[W * H];
        make_day(0);
        fb = draw(DRAW_READING, series(ENVS_TEMP, 1999, ENVS_OK, ENVS_STEADY), 2, "!temp_19_9");
        memcpy(a, fb, sizeof a);
        fb = draw(DRAW_READING, series(ENVS_TEMP, 2000, ENVS_OK, ENVS_STEADY), 2, "!temp_20_0");
        /* The degree sign: the columns lit on its rows and dark all the way
           down the digits' lower half, where only it has none. */
        int l = W, r = -1, same = 1;
        for (int x = 150; x < 260; x++)
            if (count(a, x, 30, x, 50, WHITE) > 0 && dark(a, x, 64, x, 91)) { if (x < l) l = x; if (x > r) r = x; }
        for (int y = 28; y < 64 && r >= 0; y++)
            for (int x = l - 1; x <= r + 1; x++)
                if (a[y * W + x] != fb[y * W + x]) same = 0;
        /* The point: the only ink between the second digit and the third on
           the baseline's rows, found in the first render. */
        int pl = W, pr = -1;
        for (int x = 100; x < 150; x++)
            if (a[86 * W + x] == WHITE && dark(a, x, 60, x, 70)) { if (x < pl) pl = x; if (x > pr) pr = x; }
        for (int y = 78; y < 91 && pr >= 0; y++)
            for (int x = pl; x <= pr; x++)
                if (a[y * W + x] != fb[y * W + x]) same = 0;
        printf("     19.9 and 20.0: the degree sign at x %d..%d, the point at x %d..%d\n", l, r, pl, pr);
        expect("19.9 and 20.0 put their point and degree sign on the same pixels", same && r > l && pr >= pl);
        expect("and the number's ink never crosses the margin",
               dark(a, 0, 20, 15, 96) && dark(fb, 0, 20, 15, 96));
    }

    /* Pinned: above the chart's top the number still says it, the strip
       holds the trace at its top rather than off the panel. */
    make_day(2500);
    fb = draw(DRAW_READING, series(ENVS_VOC, 2500, ENVS_POOR, ENVS_RISING), 0, "voc_pinned");
    expect("a pinned reading keeps the corners dark", corners_dark(fb));

    /*
     * A FAIR stretch in the strip is a block of amber from the FAIR line up
     * to the trace, not a 1-2 px tick: 400 ppb for five hours puts the trace
     * at y 144.1 over the FAIR line at 146, so the two rows under the line of
     * the trace are amber all along the stretch.
     */
    make_day(0);
    for (int i = 100; i <= 160; i++) { s_voc[i] = 400.0f; s_ok[i] = true; }
    fb = draw(DRAW_READING, series(ENVS_VOC, 47, ENVS_OK, ENVS_STEADY), 0, "voc_fair_stretch");
    expect("a FAIR stretch in the strip is filled down to its line",
           count(fb, 126, 145, 166, 146, FAIR) == 41 * 2);
    /* 06:40 to 11:40 on Friday: a GOOD morning, nothing to fill -- no amber
       but the dotted FAIR line's own, on row 146. */
    expect("and GOOD is not filled",
           count(fb, 210, 138, 270, 145, FAIR) == 0 && count(fb, 210, 147, 270, 151, FAIR) == 0);

    /* Warming up: no number, whatever value is lying in the struct. */
    make_day(0);
    for (int i = N - 8; i < N; i++) s_ok[i] = false;
    envui_series_t *s = (envui_series_t *)series(ENVS_VOC, 1366, ENVS_WAIT, ENVS_UNKNOWN);
    s->have_now = false;
    s->warming = true;
    s->warm_minutes = 12;
    fb = draw(DRAW_READING, s, 0, "voc_warming");
    expect("warming up shows no state colour",
           count(fb, 0, 0, W - 1, 134, GOOD) == 0 && count(fb, 0, 0, W - 1, 134, POOR) == 0
           && count(fb, 0, 0, W - 1, 134, FAIR) == 0);
    expect("and no white number", ink_rows(fb, 16, 24, 300, 96, &top, &bottom) == 0);
    s->warming = false;
    s->gas_error = true;
    fb = draw(DRAW_READING, s, 0, "voc_gas_error");
    /* The minutes count from the sensor's start: under an error they would
       read as progress. The slot under GAS ERROR stays empty. */
    expect("GAS ERROR has no MIN SO FAR under it", dark(fb, 0, 76, W - 1, 134));

    /* eCO2 is never GOOD: below its first limit the slot says FROM VOCs. */
    make_day(0);
    fb = draw(DRAW_READING, series(ENVS_ECO2, 447, ENVS_OK, ENVS_STEADY), 1, "eco2_ok");
    expect("eCO2 at 440 is never blue", count(fb, 0, 0, W - 1, H - 1, GOOD) == 0
           && count(fb, 0, 0, W - 1, H - 1, TINT) == 0);
    for (int k = 0; k < 12; k++) { s_co2[N - 1 - k] = 870.0f + 6.0f * (float)k; }
    fb = draw(DRAW_READING, series(ENVS_ECO2, 874, ENVS_FAIR, ENVS_FALLING), 1, "eco2_fair");
    expect("eCO2 FAIR is amber", count(fb, 16, 90, 200, 136, FAIR) > 100);
    /* The unit is a plain "ppm", clear of the label, not a second phrase
       running on from it across the whole top line. */
    expect("eCO2's top line has a gap between label and unit",
           dark(fb, 110, 0, 250, 24));
    make_day(760);
    fb = draw(DRAW_READING, series(ENVS_ECO2, 1085, ENVS_POOR, ENVS_RISING), 1, "eco2_poor");
    expect("eCO2 POOR is a red block", count(fb, 12, 90, 200, 136, POOR) > 1500);
    make_day(0);
    for (int i = N - 8; i < N; i++) s_ok[i] = false;
    s = (envui_series_t *)series(ENVS_ECO2, 0, ENVS_WAIT, ENVS_UNKNOWN);
    s->have_now = false;
    s->warming = true;
    s->warm_minutes = 12;
    fb = draw(DRAW_READING, s, 1, "eco2_warming");
    expect("eCO2 warming up shows no state colour above the strip",
           count(fb, 0, 0, W - 1, 134, FAIR) == 0 && count(fb, 0, 0, W - 1, 134, POOR) == 0);

    /* Raw temperature and humidity: no word, no state colour anywhere. */
    make_day(0);
    fb = draw(DRAW_READING, series(ENVS_TEMP, 2687, ENVS_OK, ENVS_RISING), 2, "temp");
    expect("TEMP carries no state colour", count(fb, 0, 0, W - 1, H - 1, GOOD) == 0
           && count(fb, 0, 0, W - 1, H - 1, FAIR) == 0 && count(fb, 0, 0, W - 1, H - 1, POOR) == 0);
    expect("the corners stay dark", corners_dark(fb));
    fb = draw(DRAW_READING, series(ENVS_RH, 3765, ENVS_OK, ENVS_STEADY), 3, "rh");
    expect("HUMIDITY carries no state colour", count(fb, 0, 0, W - 1, H - 1, GOOD) == 0
           && count(fb, 0, 0, W - 1, H - 1, FAIR) == 0 && count(fb, 0, 0, W - 1, H - 1, POOR) == 0);
    /* "37" alone could be a temperature from across the room: the percent
       sign stands after it, top to top with it, about half its size -- white
       ink right of the two digits (x 16..105), starting on their top row and
       ending in the upper half of their band. */
    {
        int nt, nb, top, bottom;
        ink_rows(fb, 16, 20, 105, 96, &nt, &nb);
        ink_rows(fb, 108, 20, 150, 96, &top, &bottom);
        printf("     the digits' ink: rows %d..%d; the percent sign's: rows %d..%d\n", nt, nb, top, bottom);
        expect("humidity's number carries a percent sign",
               top >= 0 && abs(top - nt) <= 1 && bottom <= (nt + nb) / 2);
    }

    /* The strip's reference lines are gone: unlabelled, they lay on the
       trace and said nothing. The chart, which labels them, keeps them. */
    for (int i = 0; i < N; i++) s_ok[i] = true;
    fb = draw(DRAW_READING, series(ENVS_TEMP, 2687, ENVS_OK, ENVS_STEADY), 2, "!temp_strip");
    /* The 20 C line's row, y 148, which the day's 24-27 C never reaches:
       the dotted line put a hairline pixel in every fourth column there.
       (Not the whole strip: a white trace's anti-aliased edge can land on
       the hairline grey exactly.) */
    expect("the TEMP strip has no reference lines", count(fb, 16, 147, 304, 149, RULE) == 0);

    /* No reading at all: said in words, not a number. */
    make_day(0);
    s = (envui_series_t *)series(ENVS_TEMP, 0, ENVS_OK, ENVS_UNKNOWN);
    s->have_now = false;
    draw(DRAW_READING, s, 2, "temp_no_reading");
    s = (envui_series_t *)series(ENVS_RH, 0, ENVS_OK, ENVS_UNKNOWN);
    s->have_now = false;
    draw(DRAW_READING, s, 3, "rh_no_reading");

    /* Temperatures the room should never see, drawn without harm. */
    draw(DRAW_READING, series(ENVS_TEMP, -1234, ENVS_OK, ENVS_FALLING), 2, "temp_negative");
    draw(DRAW_READING, series(ENVS_TEMP, -5, ENVS_OK, ENVS_STEADY), 2, "!temp_minus_zero");
}

static void test_detail_pages(void)
{
    const uint16_t *fb;

    make_day(0);
    fb = draw(DRAW_DETAIL, series(ENVS_VOC, 47, ENVS_OK, ENVS_STEADY), 0, "voc_24h");
    expect("the VOC chart tints its GOOD zone", count(fb, 16, 100, 249, 141, TINT) > 3000);
    expect("the detail corners stay dark", corners_dark(fb));
    /* The zone lines are whole: the midnight line goes under them, and the
       peak's knock-out drops below the POOR line rather than cutting it. */
    expect("the POOR line runs unbroken across the plot",
           count(fb, 16, 68, 243, 68, POOR) == 228);
    /* The plot stops 10 px short of the key, so the now-dot is not a bullet
       on the key word level with it; and the key, GOOD the widest of its
       words, ends on the margin. */
    expect("a clear gap between the now-dot and the key", dark(fb, 248, 44, 252, 141));
    expect("the key keeps inside the text margin",
           dark(fb, 304, 44, W - 1, 141) && count(fb, 300, 110, 303, 130, GOOD) > 0);
    /* The header's number and word stand on one baseline. */
    {
        int nt, nb, wt, wb;
        ink_rows(fb, 150, 0, 230, 43, &nt, &nb);
        rows_with(fb, 230, 0, 304, 43, GOOD, &wt, &wb);
        printf("     header ink: number %d..%d, word %d..%d\n", nt, nb, wt, wb);
        expect("the header's number and word share a baseline", abs(nb - wb) <= 1);
    }

    /*
     * A trace is coloured by the zone it runs through: an hour at 1100 ppb
     * rises white to the FAIR line, amber to the POOR line and red above it,
     * and comes back down the same way -- not white through POOR because the
     * segment ends in GOOD, nor amber from the floor because it ends in FAIR.
     */
    make_day(0);
    for (int i = 200; i <= 220; i++) s_voc[i] = 1100.0f;           /* 06:40-08:25 */
    envui_series_t *s = (envui_series_t *)series(ENVS_VOC, 47, ENVS_OK, ENVS_STEADY);
    s->peak_slot = -1;
    fb = draw(DRAW_DETAIL, s, 0, "voc_24h_plateau");
    /* Near-white, not exact: a steep 2 px line covers no pixel wholly, so
       the old white drop out of POOR never lit one at exactly 0xFFFF. */
    {
        int top, bottom;
        expect("no white trace above the FAIR line", ink_rows(fb, 16, 44, 243, 97, &top, &bottom) == 0);
    }
    expect("no amber or red trace below it",
           count(fb, 16, 102, 243, 141, FAIR) == 0 && count(fb, 16, 102, 243, 141, POOR) == 0);
    expect("and red and amber where the plateau is",
           count(fb, 170, 44, 200, 66, POOR) > 20 && count(fb, 170, 70, 200, 98, FAIR) > 20);

    /*
     * VOC's peak is a stored 30 s maximum over a trace of 5-minute means: a
     * 1500 ppb spike in an hour whose mean was 300 stands 40 px clear of the
     * line, and a grey stem ties it back down (column x 178, slot 205).
     */
    make_day(0);
    for (int i = 200; i <= 210; i++) s_voc[i] = 300.0f;
    s = (envui_series_t *)series(ENVS_VOC, 47, ENVS_OK, ENVS_STEADY);
    s->peak_slot = 205;
    s->peak_value = 1500;
    fb = draw(DRAW_DETAIL, s, 0, "voc_24h_spike");
    expect("a floating peak is stemmed to its trace", count(fb, 178, 52, 178, 92, GREY) >= 30);
    expect("and the stem passes behind the zone lines",
           count(fb, 16, 68, 243, 68, POOR) == 228 && count(fb, 16, 100, 243, 100, FAIR) >= 200);
    /*
     * The header's number holds still too, set right-aligned by its advance
     * box: 45 % and 44 % put their 4 on the same pixels, where set by the ink
     * the whole number moved with the last digit's side bearing -- 2 px here,
     * a 5 having that much more of it than a 4.
     */
    {
        static uint16_t a[W * H];
        make_day(0);
        fb = draw(DRAW_DETAIL, series(ENVS_RH, 4500, ENVS_OK, ENVS_STEADY), 3, "!rh_24h_45");
        memcpy(a, fb, sizeof a);
        fb = draw(DRAW_DETAIL, series(ENVS_RH, 4400, ENVS_OK, ENVS_STEADY), 3, "!rh_24h_44");
        int l = W;
        for (int y = 8; y < 40; y++)
            for (int x = 150; x < 304; x++)
                if (a[y * W + x] == WHITE && x < l) l = x;
        int same = l < W;
        for (int y = 6; y < 42 && same; y++)
            for (int x = l - 2; x <= l + 12; x++)
                if (a[y * W + x] != fb[y * W + x]) same = 0;
        printf("     the header's first 4 starts at x %d\n", l);
        expect("45 and 44 put the header's first digit on the same pixels", same);
        /* With no word and no arrow the box's right edge is the margin
           itself, and only the figures' own side bearings keep their ink
           inside it -- the tightest the header gets. A temperature ends in
           its degree sign instead. */
        expect("a header with nothing beside its number keeps inside the margin",
               dark(a, 304, 0, W - 1, 43) && dark(fb, 304, 0, W - 1, 43));
        fb = draw(DRAW_DETAIL, series(ENVS_TEMP, 2687, ENVS_OK, ENVS_STEADY), 2, "!temp_24h_steady");
        expect("and so does a temperature's, degree sign and all", dark(fb, 304, 0, W - 1, 43)
               && count(fb, 280, 8, 303, 30, WHITE) > 0);
    }
    make_day(2500);
    draw(DRAW_DETAIL, series(ENVS_VOC, 2500, ENVS_POOR, ENVS_RISING), 0, "voc_24h_pinned");
    make_day(262);
    draw(DRAW_DETAIL, series(ENVS_VOC, 262, ENVS_FAIR, ENVS_RISING), 0, "voc_24h_fair");
    make_day(1366);
    fb = draw(DRAW_DETAIL, series(ENVS_VOC, 1366, ENVS_POOR, ENVS_RISING), 0, "voc_24h_poor");
    expect("the POOR word's block stays right of the title",
           count(fb, 16, 0, 120, 40, POOR) == 0);
    make_day(0);
    fb = draw(DRAW_DETAIL, series(ENVS_ECO2, 447, ENVS_OK, ENVS_STEADY), 1, "eco2_24h");
    expect("the eCO2 chart has no GOOD zone, tint or word",
           count(fb, 0, 0, W - 1, H - 1, GOOD) == 0 && count(fb, 0, 0, W - 1, H - 1, TINT) == 0);
    for (int k = 0; k < 12; k++) { s_co2[N - 1 - k] = 870.0f + 6.0f * (float)k; }
    draw(DRAW_DETAIL, series(ENVS_ECO2, 874, ENVS_FAIR, ENVS_FALLING), 1, "eco2_24h_fair");
    make_day(0);
    draw(DRAW_DETAIL, series(ENVS_TEMP, 2687, ENVS_OK, ENVS_RISING), 2, "temp_24h");
    draw(DRAW_DETAIL, series(ENVS_RH, 3765, ENVS_OK, ENVS_STEADY), 3, "rh_24h");
    /* A hot afternoon past the chart's 30 C top: pinned, not off the plot. */
    for (int k = 0; k < 24; k++) s_tc[N - 1 - k] = 31.4f - 0.15f * (float)k;
    fb = draw(DRAW_DETAIL, series(ENVS_TEMP, 3140, ENVS_OK, ENVS_RISING), 2, "temp_24h_hot");
    expect("a temperature over 30 C keeps the corners dark", corners_dark(fb));

    make_day(0);
    for (int i = N - 8; i < N; i++) s_ok[i] = false;
    s = (envui_series_t *)series(ENVS_ECO2, 0, ENVS_WAIT, ENVS_UNKNOWN);
    s->have_now = false;
    s->warming = true;
    s->warm_minutes = 12;
    draw(DRAW_DETAIL, s, 1, "eco2_24h_warming");

    /*
     * The scale is fixed: a flat 110 ppb, half of GOOD, sits half way up the
     * 42 px GOOD zone (y 121) whatever else the day held -- no autoscale
     * stretching a quiet day's noise across the plot.
     */
    make_day(0);
    for (int i = 0; i < N; i++) { s_voc[i] = 110.0f; s_ok[i] = true; }
    fb = draw(DRAW_DETAIL, series(ENVS_VOC, 110, ENVS_OK, ENVS_STEADY), 0, "!voc_flat");
    int top, bottom;
    ink_rows(fb, 100, 44, 100, 141, &top, &bottom);
    printf("     a flat 110 ppb trace: rows %d..%d\n", top, bottom);
    expect("a flat 110 ppb draws at y 121, half way up GOOD", top >= 118 && bottom <= 123);

    /*
     * An invalid reading never sets a colour: slots flagged invalid carry
     * 5000 ppb, the valid ones 40, and nothing amber or red may appear.
     */
    make_day(0);
    s = (envui_series_t *)series(ENVS_VOC, 40, ENVS_OK, ENVS_STEADY);
    for (int i = 0; i < N; i++) { if (i % 3 == 0) { s->valid[i] = false; s->slot[i] = 5000; } else s->slot[i] = 40; }
    s->peak_slot = -1;
    fb = draw(DRAW_DETAIL, s, 0, "!voc_invalid_high");
    /* Amber and red belong only on their zone lines (y 100 and 68). */
    expect("invalid slots colour nothing in the chart",
           count(fb, 16, 44, 249, 99, FAIR) == 0 && count(fb, 16, 101, 249, 141, FAIR) == 0
           && count(fb, 16, 44, 249, 67, POOR) == 0 && count(fb, 16, 69, 249, 141, POOR) == 0
           && count(fb, 16, 44, 249, 110, WHITE) == 0);
    fb = draw(DRAW_READING, s, 0, "!voc_invalid_high_reading");
    expect("nor in the strip", count(fb, 16, 136, 304, 151, WHITE) > 0
           && count(fb, 16, 136, 304, 144, WHITE) == 0);
}

/* The marks are drawn in linear light, as the type is -- and the setting is
   the page's to borrow, not to keep: vector.c is left as it was found. */
static void test_linear_light(void)
{
    make_day(262);
    const envui_series_t *s = series(ENVS_VOC, 262, ENVS_FAIR, ENVS_RISING);
    canvas_t c = canvas_on(s_mem);
    envui_reading(&c, s, 0, PAGES);
    bool after_reading = vec_linear_light(false);
    envui_detail(&c, s);
    bool after_detail = vec_linear_light(false);
    vec_linear_light(true);
    envui_detail(&c, s);
    bool kept_on = vec_linear_light(false);
    expect("the pages leave vector.c's blend as they found it", !after_reading && !after_detail && kept_on);

    /* The arrow's edges, drawn on black, are the linear blend's: every edge
       pixel of it is a grey whose channels agree the way white laid over
       black in linear light makes them, not the darker grey of the codes
       mixed. Its box: right-aligned at x 304, the top of the number's band. */
    c = canvas_on(s_mem);
    envui_reading(&c, s, 0, PAGES);
    const uint16_t *fb = s_mem + GUARD;
    int edges = 0, linear = 0;
    for (int y = 30; y < 62; y++)
        for (int x = 280; x < 304; x++) {
            uint16_t p = fb[y * W + x];
            if (p == 0 || p == WHITE) continue;
            edges++;
            for (unsigned a = 1; a < 255; a++)
                if (vec_blend_linear(0, WHITE, (uint8_t)a) == p) { linear++; break; }
        }
    printf("     the arrow: %d edge pixels, %d of them the linear blend's\n", edges, linear);
    expect("the trend arrow's edges are blended in linear light", edges > 10 && linear == edges);
}

static void test_edge_series(void)
{
    envui_series_t *s;

    /* Nothing at all yet: a fresh board. */
    make_day(0);
    s = (envui_series_t *)series(ENVS_VOC, 0, ENVS_WAIT, ENVS_UNKNOWN);
    for (int i = 0; i < N; i++) s->valid[i] = false;
    s->have_now = false;
    s->peak_slot = -1;
    const uint16_t *fb = draw(DRAW_READING, s, 0, "voc_all_gaps");
    expect("an empty strip is empty: no tint passing for data",
           count(fb, 0, 136, W - 1, 152, TINT) == 0);
    fb = draw(DRAW_DETAIL, s, 0, "voc_24h_all_gaps");
    /* An empty chart: no GOOD tint over nothing, NO DATA between the zone
       lines on no knock-out, and NO READING in words where the number goes. */
    expect("an empty chart has no GOOD tint", count(fb, 0, 0, W - 1, H - 1, TINT) == 0);
    {
        int top, bottom;
        rows_with(fb, 60, 69, 200, 99, GREY, &top, &bottom);
        expect("NO DATA sits between the FAIR and POOR lines", top >= 70 && bottom <= 98);
        expect("the zone lines run whole behind it",
               count(fb, 16, 68, 243, 68, POOR) == 228 && count(fb, 16, 100, 243, 100, FAIR) == 228);
        rows_with(fb, 150, 0, 304, 43, GREY, &top, &bottom);
        printf("     the header's NO READING: rows %d..%d\n", top, bottom);
        expect("the header says NO READING, not a 4 px \"--\"", bottom - top >= 14);
    }

    /* One valid slot mid-day, then nothing until a reading now. */
    s->valid[150] = true;
    s->slot[150] = 48;
    s->have_now = true;
    s->now_value = 52;
    s->state = ENVS_OK;
    draw(DRAW_READING, s, 0, "voc_single_slot");
    draw(DRAW_DETAIL, s, 0, "voc_24h_single_slot");

    /* Midnight just before now: the weekday label must not run into NOW. */
    make_day(0);
    s = (envui_series_t *)series(ENVS_VOC, 47, ENVS_OK, ENVS_STEADY);
    s->midnight_slot = N - 7;
    s->midnight_day = "SAT";
    s->last_slot_hour = 0;
    draw(DRAW_DETAIL, s, 0, "voc_24h_midnight_late");
    /* Midnight at the very start of the window. */
    s->midnight_slot = 0;
    s->last_slot_hour = 23;
    draw(DRAW_DETAIL, s, 0, "voc_24h_midnight_early");
    /* No midnight known: the ticks come from the latest slot's hour. */
    s->midnight_slot = -1;
    s->midnight_day = NULL;
    s->last_slot_hour = 14;
    draw(DRAW_DETAIL, s, 0, "voc_24h_no_midnight");

    /*
     * Anything at all, and nothing is written outside the framebuffer.
     * These are the values that turn into a y a million pixels away, or a
     * negative array index, if anything is missing a clamp.
     */
    const int32_t wild[] = { INT32_MIN, -1, 0, 1, 65535, INT32_MAX };
    for (int k = 0; k < (int)(sizeof wild / sizeof wild[0]); k++) {
        for (int ser = 0; ser <= ENVS_N; ser++) {
            s = (envui_series_t *)series(ser == ENVS_N ? ENVS_VOC : (envs_series_t)ser, wild[k], ENVS_POOR, ENVS_RISING);
            if (ser == ENVS_N) s->series = (envs_series_t)7;
            for (int i = 0; i < N; i++) s->slot[i] = wild[(i + k) % 6];
            s->peak_slot = k % 2 ? 100000 : N - 1;
            s->peak_value = wild[5 - k];
            s->midnight_slot = k % 2 ? -7 : 5000;
            s->midnight_day = k % 2 ? NULL : "WEDNESDAYWEDNESDAY";
            s->last_slot_hour = k % 2 ? 99 : -3;
            s->state = (envs_state_t)(k + 3);
            s->trend = (envs_trend_t)(k * 7);
            s->warm_minutes = k % 2 ? INT_MIN : INT_MAX;
            s->warming = k == 2;
            s->gas_error = k == 3;
            draw(DRAW_READING, s, k * 5 - 10, "!wild_reading");
            draw(DRAW_DETAIL, s, 0, "!wild_detail");
            canvas_t c = canvas_on(s_mem);
            envui_reading(&c, s, 3, k % 2 ? -1 : 1000);
            expect("any page count stays inside the framebuffer", guard_ok(s_mem));
        }
    }
}

static void week_draw(const envui_week_t *w, const char *name)
{
    canvas_t a = canvas_on(s_mem), b = canvas_on(s_mem2);
    envui_week(&a, w, 4, PAGES);
    envui_week(&b, w, 4, PAGES);
    char what[200];
    snprintf(what, sizeof what, "%s stays inside the framebuffer", name);
    expect(what, guard_ok(s_mem) && guard_ok(s_mem2));
    snprintf(what, sizeof what, "%s draws the same pixels twice", name);
    expect(what, memcmp(s_mem, s_mem2, sizeof s_mem) == 0);
    if (name[0] != '!') render(name, s_mem + GUARD);
}

#define W_GY_T 28                           /* the week grid's first row */

static void test_week(void)
{
    envui_week_t w;
    static const char *days[] = { "SA", "SU", "MO", "TU", "WE", "TH", "FR" };

    /* The mockup's week: evenings FAIR, two POOR hours, a gap Monday and
       Tuesday morning and one early today; today runs to 14:00. */
    memset(&w, 0, sizeof w);
    for (int d = 0; d < 7; d++) {
        memcpy(w.day[d], days[d], 3);
        for (int h = 0; h < 24; h++) {
            uint8_t s = ENVUI_CELL_OK;
            if (d == 6 && h > 14) s = ENVUI_CELL_FUTURE;
            else if (((d == 2 || d == 3) && h < 17) || (d == 6 && h >= 3 && h < 6)) s = ENVUI_CELL_NONE;
            else {
                if (h >= 18 && h <= 20 && d != 1) s = ENVUI_CELL_FAIR;
                if (d == 5 && (h == 18 || h == 19)) s = ENVUI_CELL_POOR;
                if (d == 6 && h == 12) s = ENVUI_CELL_FAIR;
                if (d == 0 && h == 11) s = ENVUI_CELL_POOR;
            }
            w.cell[d][h] = s;
            w.fair_hours += s == ENVUI_CELL_FAIR;
            w.poor_hours += s == ENVUI_CELL_POOR;
        }
    }
    week_draw(&w, "week");
    const uint16_t *fb = s_mem + GUARD;
    expect("the week shows its POOR hours in red", count(fb, 43, 28, 304, 140, POOR) >= 3 * 9 * 12);
    expect("the week corners stay dark", corners_dark(fb));
    /* The hour labels sit on the grid, 5 px clear of the page dots. */
    expect("a clear band between the hour labels and the page dots",
           dark(fb, 0, 155, W - 1, 158));

    /* The day names clear the grid: the widest, "We", beside a POOR hour at
       00 -- a full-height red cell -- still has black between them. */
    {
        envui_week_t e = w;
        for (int d = 0; d < 7; d++) e.cell[d][0] = ENVUI_CELL_POOR;
        week_draw(&e, "week_poor_at_00");
        const uint16_t *p = s_mem + GUARD;
        int right = -1, cell = W;
        for (int y = W_GY_T; y < W_GY_T + 7 * 16; y++)
            for (int x = 0; x < 60; x++) {
                if (p[y * W + x] == POOR) { if (x < cell) cell = x; }
                else if (p[y * W + x] != 0 && x < 43 && x > right) right = x;
            }
        printf("     the day names end at x %d, the grid starts at x %d\n", right, cell);
        expect("the day names keep a 2 px gap from the grid", right >= 16 && cell - right >= 3);
    }

    /* All good. */
    for (int d = 0; d < 7; d++)
        for (int h = 0; h < 24; h++)
            w.cell[d][h] = d == 6 && h > 14 ? ENVUI_CELL_FUTURE : ENVUI_CELL_OK;
    w.fair_hours = w.poor_hours = 0;
    week_draw(&w, "week_all_good");
    expect("an all-good week has no amber or red",
           count(s_mem + GUARD, 0, 0, W - 1, H - 1, FAIR) == 0 && count(s_mem + GUARD, 0, 0, W - 1, H - 1, POOR) == 0);

    /* Nothing logged yet. */
    for (int d = 0; d < 7; d++)
        for (int h = 0; h < 24; h++)
            w.cell[d][h] = d == 6 && h > 14 ? ENVUI_CELL_FUTURE : ENVUI_CELL_NONE;
    week_draw(&w, "week_empty");

    /* Garbage: every byte value in the cells, unterminated day names,
       impossible counts. */
    for (int d = 0; d < 7; d++) {
        memset(w.day[d], 'X', 3);
        for (int h = 0; h < 24; h++) w.cell[d][h] = (uint8_t)(d * 24 + h + 37);
    }
    w.poor_hours = INT_MAX;
    w.fair_hours = INT_MIN;
    week_draw(&w, "!week_garbage");
    canvas_t c = canvas_on(s_mem);
    envui_week(&c, &w, -5, 100000);
    expect("a week with any page count stays inside", guard_ok(s_mem));
}

static void test_verdict(void)
{
    const envs_verdict_t v[] = {
        { ENVS_VOC, ENVS_OK }, { ENVS_VOC, ENVS_POOR }, { ENVS_VOC, ENVS_FAIR },
        { ENVS_ECO2, ENVS_POOR }, { ENVS_ECO2, ENVS_FAIR }, { ENVS_VOC, ENVS_WAIT },
    };
    canvas_t a = canvas_on(s_mem), b = canvas_on(s_mem2);
    canvas_clear(&a);
    canvas_clear(&b);
    for (int k = 0; k < 6; k++) {
        envui_verdict(&a, 16.0f, 7.0f + 28.0f * (float)k, 20.0f, v[k]);
        envui_verdict(&b, 16.0f, 7.0f + 28.0f * (float)k, 20.0f, v[k]);
    }
    expect("the verdicts stay inside the framebuffer", guard_ok(s_mem) && guard_ok(s_mem2));
    expect("and draw the same pixels twice", memcmp(s_mem, s_mem2, sizeof s_mem) == 0);
    const uint16_t *fb = s_mem + GUARD;
    expect("AIR GOOD is blue", count(fb, 16, 0, W - 1, 30, GOOD) > 50);
    expect("VOC POOR is on a red block", count(fb, 16, 28, W - 1, 62, POOR) > 500);
    expect("VOC FAIR is amber", count(fb, 16, 62, W - 1, 86, FAIR) > 50);
    expect("WARMING UP carries no state colour",
           count(fb, 0, 145, W - 1, H - 1, GOOD) == 0 && count(fb, 0, 145, W - 1, H - 1, FAIR) == 0
           && count(fb, 0, 145, W - 1, H - 1, POOR) == 0);
    render("verdicts", fb);

    /* A bigger one, as the clock might set it. */
    a = canvas_on(s_mem);
    canvas_clear(&a);
    envui_verdict(&a, 16.0f, 40.0f, 34.0f, (envs_verdict_t){ ENVS_ECO2, ENVS_POOR });
    envui_verdict(&a, 16.0f, 110.0f, 34.0f, (envs_verdict_t){ ENVS_VOC, ENVS_OK });
    expect("a large verdict stays inside", guard_ok(s_mem));
    render("verdicts_34", s_mem + GUARD);

    /*
     * No verdict while the gas sensor has none: the clock says which kind of
     * none, as the VOC page does -- GAS ERROR under an error, never the
     * WARMING UP that envs_verdict's ENVS_WAIT alone would give. Grey, like
     * the pages' own, and no state colour.
     */
    a = canvas_on(s_mem);
    b = canvas_on(s_mem2);
    canvas_clear(&a);
    canvas_clear(&b);
    envui_verdict_wait(&a, 16.0f, 40.0f, 28.0f, true);
    envui_verdict_wait(&b, 16.0f, 40.0f, 28.0f, true);
    expect("GAS ERROR stays inside the framebuffer", guard_ok(s_mem) && guard_ok(s_mem2));
    expect("and draws the same pixels twice", memcmp(s_mem, s_mem2, sizeof s_mem) == 0);
    fb = s_mem + GUARD;
    expect("GAS ERROR is grey and carries no state colour",
           count(fb, 0, 0, W - 1, H - 1, GREY) > 50
           && count(fb, 0, 0, W - 1, H - 1, GOOD) == 0 && count(fb, 0, 0, W - 1, H - 1, FAIR) == 0
           && count(fb, 0, 0, W - 1, H - 1, POOR) == 0 && count(fb, 0, 0, W - 1, H - 1, WHITE) == 0);
    envui_verdict_wait(&a, 16.0f, 100.0f, 28.0f, false);
    render("verdicts_wait", fb);
    b = canvas_on(s_mem2);
    canvas_clear(&b);
    envui_verdict_wait(&b, 16.0f, 40.0f, 28.0f, false);
    expect("and it is not the WARMING UP line",
           memcmp(s_mem + GUARD, s_mem2 + GUARD, (size_t)W * 90 * sizeof(uint16_t)) != 0);
    b = canvas_on(s_mem2);
    canvas_clear(&b);
    envui_verdict(&b, 16.0f, 40.0f, 28.0f, (envs_verdict_t){ ENVS_VOC, ENVS_WAIT });
    a = canvas_on(s_mem);
    canvas_clear(&a);
    envui_verdict_wait(&a, 16.0f, 40.0f, 28.0f, false);
    expect("a verdict of ENVS_WAIT is the WARMING UP line",
           memcmp(s_mem, s_mem2, sizeof s_mem) == 0);

    /* Its width, which the clock centres it by, is where its ink ends: within
       a pixel of anti-aliasing, whatever the state, POOR's block included. */
    {
        int off = 0;
        for (int k = 0; k < 6; k++) {
            a = canvas_on(s_mem);
            canvas_clear(&a);
            float w = envui_verdict_width(28.0f, v[k]);
            envui_verdict(&a, 10.0f, 60.0f, 28.0f, v[k]);
            const uint16_t *p = s_mem + GUARD;
            int right = -1;
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                    if (p[y * W + x] != 0 && x > right) right = x;
            if (right < 0 || fabsf((float)(right + 1) - (10.0f + w)) > 1.5f) off++;
        }
        expect("the verdict's width is where its ink ends", off == 0);
        off = 0;
        for (int k = 0; k < 2; k++) {
            a = canvas_on(s_mem);
            canvas_clear(&a);
            float w = envui_verdict_wait_width(28.0f, k == 1);
            envui_verdict_wait(&a, 10.0f, 60.0f, 28.0f, k == 1);
            const uint16_t *p = s_mem + GUARD;
            int right = -1;
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                    if (p[y * W + x] != 0 && x > right) right = x;
            if (right < 0 || fabsf((float)(right + 1) - (10.0f + w)) > 1.5f) off++;
        }
        expect("and so is the waiting line's, GAS ERROR or WARMING UP", off == 0);
        expect("and a width is never negative or NaN",
               envui_verdict_width(-3.0f, v[0]) == 0.0f
               && envui_verdict_width(NAN, v[0]) == 0.0f
               && envui_verdict_width(1e30f, v[0]) > 0.0f
               && isfinite(envui_verdict_width(1e30f, v[0])));
    }

    /* Anywhere, any size, any state. */
    const float xs[] = { -1e30f, -50.0f, 0.0f, 300.0f, 1e30f, NAN, INFINITY };
    for (int i = 0; i < 7; i++)
        for (int j = 0; j < 7; j++) {
            a = canvas_on(s_mem);
            envui_verdict(&a, xs[i], xs[j], xs[(i + j) % 7] < 0 ? 5.0f : 400.0f,
                          (envs_verdict_t){ (envs_series_t)(i + j), (envs_state_t)(i * j) });
            envui_verdict(&a, xs[i], xs[j], xs[(i + j) % 7],
                          (envs_verdict_t){ ENVS_ECO2, ENVS_POOR });
            envui_verdict_wait(&a, xs[i], xs[j], xs[(i + j) % 7], (i + j) % 2 == 0);
            if (!isfinite(envui_verdict_wait_width(xs[(i + j) % 7], true))
                || envui_verdict_wait_width(xs[(i + j) % 7], false) < 0.0f) {
                expect("a waiting line's width is never NaN or negative", 0);
                return;
            }
            if (!guard_ok(s_mem)) { expect("a verdict anywhere stays inside", 0); return; }
        }
    expect("a verdict anywhere stays inside", 1);
}

/* ---- the clock ------------------------------------------------------------ */

static void clock_draw(const envui_clock_t *k, const char *name)
{
    canvas_t a = canvas_on(s_mem), b = canvas_on(s_mem2);
    envui_clock(&a, k);
    envui_clock(&b, k);
    char what[200];
    snprintf(what, sizeof what, "%s stays inside the framebuffer", name);
    expect(what, guard_ok(s_mem) && guard_ok(s_mem2));
    snprintf(what, sizeof what, "%s draws the same pixels twice", name);
    expect(what, memcmp(s_mem, s_mem2, sizeof s_mem) == 0);
    /* Every line is centred, the verdict included, and none is so wide that
       centring puts it into the rounded corners' columns. */
    snprintf(what, sizeof what, "%s keeps its text within x 16..304", name);
    expect(what, dark(s_mem + GUARD, 0, 0, 15, H - 1) && dark(s_mem + GUARD, 305, 0, W - 1, H - 1));
    if (name[0] != '!') render(name, s_mem + GUARD);
}

/* The columns a glyph's bitmap covers when `s` is set as the clock sets its
   time -- centred on x 160 by its advance -- with `before` in front of it. */
static void clock_glyph_cols(const char *s, const char *before, uint32_t cp, int *x0, int *x1)
{
    const aafont_t *f = &aafont_inter_number;
    int start = W / 2 - aafont_advance(f, s) / 2;
    *x0 = *x1 = -1;
    for (int i = 0; i < f->count; i++)
        if (f->glyphs[i].cp == cp) {
            *x0 = start + aafont_advance(f, before) + f->glyphs[i].x;
            *x1 = *x0 + f->glyphs[i].w - 1;
        }
}

static void test_clock(void)
{
    envui_clock_t k = { "Thu 25 Sep 2026", "21:47:09", true, { ENVS_VOC, ENVS_OK }, false };
    const uint16_t *fb = s_mem + GUARD;
    int top, bottom;

    clock_draw(&k, "clock_good");
    ink_rows(fb, 16, 45, 304, 110, &top, &bottom);
    printf("     the clock's digits: rows %d..%d\n", top, bottom);
    expect("the time is at least 46 px tall, a size up from the old 42", bottom - top + 1 >= 46);
    expect("AIR GOOD is blue, under the time", count(fb, 16, bottom + 1, 304, H - 1, GOOD) > 200);
    expect("and nothing on the clock is amber or red",
           count(fb, 0, 0, W - 1, H - 1, FAIR) == 0 && count(fb, 0, 0, W - 1, H - 1, POOR) == 0);
    expect("the clock's corners stay dark", corners_dark(fb));

    k.verdict = (envs_verdict_t){ ENVS_VOC, ENVS_FAIR };
    clock_draw(&k, "clock_fair");
    expect("VOC FAIR is amber", count(fb, 16, 110, 304, H - 1, FAIR) > 200);
    k.verdict = (envs_verdict_t){ ENVS_ECO2, ENVS_POOR };
    clock_draw(&k, "clock_eco2_poor");
    expect("eCO2 POOR is on a red block", count(fb, 16, 110, 304, H - 1, POOR) > 1500);
    expect("and the block stays clear of the panel's foot", dark(fb, 0, H - 14, W - 1, H - 1));

    /* No verdict to give: the waiting line in grey, WARMING UP a size down
       rather than into the margins, GAS ERROR as the VOC page says it. */
    k.verdict = (envs_verdict_t){ ENVS_VOC, ENVS_WAIT };
    clock_draw(&k, "clock_warming");
    expect("WARMING UP on the clock carries no state colour",
           count(fb, 0, 0, W - 1, H - 1, GOOD) == 0 && count(fb, 0, 0, W - 1, H - 1, FAIR) == 0
           && count(fb, 0, 0, W - 1, H - 1, POOR) == 0 && count(fb, 0, 110, W - 1, H - 1, GREY) > 100);
    k.gas_error = true;
    clock_draw(&k, "clock_gas_error");
    expect("GAS ERROR on the clock is grey", count(fb, 0, 110, W - 1, H - 1, GREY) > 100);

    /* No gas sensor answering: no line at all, not a verdict about nothing. */
    k.air = false;
    clock_draw(&k, "clock_no_air");
    expect("with no air to speak of, nothing under the time", dark(fb, 0, 106, W - 1, H - 1));

    /* The clock not yet set: dashes, and no date. */
    envui_clock_t unset = { NULL, "--:--:--", false, { ENVS_VOC, ENVS_WAIT }, false };
    clock_draw(&unset, "clock_unset");
    expect("the unset clock has no date line", dark(fb, 0, 0, W - 1, 44));

    /*
     * The digits hold still. The time is centred by its advance, which
     * tabular figures make one width for every time of day, so the colons
     * land on the same columns whether the digits either side are narrow
     * 1s or wide 0s -- centred by its ink, "11:11:11" would sit a few pixels
     * off "20:08:00" and the whole line would shuffle every second.
     */
    {
        int x0, x1, y0 = 45, y1 = 110;
        clock_glyph_cols("20:08:00", "20", ':', &x0, &x1);
        envui_clock_t a = { "Thu 25 Sep 2026", "20:08:00", false, { ENVS_VOC, ENVS_OK }, false };
        envui_clock_t b = { "Thu 25 Sep 2026", "11:11:11", false, { ENVS_VOC, ENVS_OK }, false };
        canvas_t ca = canvas_on(s_mem), cb = canvas_on(s_mem2);
        envui_clock(&ca, &a);
        envui_clock(&cb, &b);
        const uint16_t *pa = s_mem + GUARD, *pb = s_mem2 + GUARD;
        int same = x0 >= 0, lit = 0;
        for (int y = y0; y <= y1 && same; y++)
            for (int x = x0; x <= x1; x++) {
                if (pa[y * W + x] != pb[y * W + x]) { same = 0; break; }
                lit += pa[y * W + x] == WHITE;
            }
        printf("     the first colon: columns %d..%d, %d px fully lit\n", x0, x1, lit);
        expect("the colons stand on the same columns at 20:08:00 and 11:11:11", same && lit > 20);
    }

    /* Whatever the date says, and none of it at all. */
    const char *dates[] = { "", "Wednesday 30 September 2026 and then some more text than fits",
                            "\xC2\xB0\xFF\x80", "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~" };
    for (int i = 0; i < 4; i++) {
        envui_clock_t w = { dates[i], i == 2 ? NULL : "23:59:59", i % 2 == 1,
                            { (envs_series_t)(i * 3), (envs_state_t)(i * 5) }, i == 3 };
        canvas_t c = canvas_on(s_mem);
        envui_clock(&c, &w);
        if (!guard_ok(s_mem)) { expect("any clock stays inside", 0); return; }
    }
    canvas_t c = canvas_on(s_mem);
    envui_clock(&c, NULL);
    expect("any clock stays inside", guard_ok(s_mem));
}

int main(void)
{
    mkdir("renders", 0755);                 /* fine if they are already there */
    mkdir("renders/envui", 0755);

    test_reading_pages();
    test_detail_pages();
    test_linear_light();
    test_edge_series();
    test_week();
    test_verdict();
    test_clock();

    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("all passed\n");
    return 0;
}
