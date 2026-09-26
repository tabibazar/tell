/* For mkdir, which C11 alone does not declare. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "daysui.h"
#include "noiseui.h"
#include "vector.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * watch's Days page on the host: the arithmetic first -- energy means, which
 * days a baseline may draw on (earlier same weekdays, then earlier days of
 * any kind, never today), the background's median, the weekday of a date --
 * then that the page never writes outside the framebuffer whatever it is
 * handed, draws the same pixels twice, keeps out of the panel's rounded
 * corners, colours a level as the Sound page and speaker's ring do, and
 * changes only EST for calibration. And renders of it to look at.
 *
 * The renders go to host_tests/renders/daysui/ as 24-bit BMPs, each also at
 * three times the size. Turn each into a PNG with
 *     sips -s format png X.bmp --out X.png
 */

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

static int close_to(float a, float b) { return fabsf(a - b) < 1e-3f; }

#define W DAYSUI_WIDTH
#define H DAYSUI_HEIGHT
#define GUARD 64
#define CANARY 0xA5C3

/* ---- dates, the test's own -------------------------------------------------- */

/* Days since 1970-01-01 and back (Howard Hinnant's algorithms), written out
   again here so the test does not check daysui with daysui's arithmetic. */
static long dnum(int y, int m, int d)
{
    y -= m <= 2;
    long era = y / 400, yoe = y - era * 400;
    long doy = (153L * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

static void civil(long z, int *y, int *m, int *d)
{
    z += 719468;
    long era = z / 146097, doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

/* ---- building days ---------------------------------------------------------- */

static daysui_t s_d;

/* An empty struct, received, calibrated, fresh. */
static daysui_t *fresh(void)
{
    memset(&s_d, 0, sizeof s_d);
    s_d.have_data = true;
    s_d.calibrated = true;
    return &s_d;
}

/* Appends the day `back` days before `today_dn`, with its levels. */
static daysui_day_t *add(daysui_t *s, long today_dn, int back, float laeq, float l90)
{
    daysui_day_t *d = &s->day[s->n++];
    int y, m, dd;
    civil(today_dn - back, &y, &m, &dd);
    d->year = (uint16_t)y;
    d->month = (uint8_t)m;
    d->day = (uint8_t)dd;
    d->laeq = laeq;
    d->l90 = l90;
    d->today = back == 0;
    return d;
}

/* The day `back` days before today in s, or NULL. */
static daysui_day_t *find(daysui_t *s, long today_dn, int back)
{
    for (int i = 0; i < s->n; i++)
        if (dnum(s->day[i].year, s->day[i].month, s->day[i].day) == today_dn - back) return &s->day[i];
    return NULL;
}

/* Friday 25 September 2026: the day these were written. */
#define TODAY dnum(2026, 9, 25)

static uint32_t s_seed;
static float noise(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (float)(s_seed >> 8) / (float)(1u << 24) * 2.0f - 1.0f;
}

/*
 * Five weeks of a flat, ending today: quiet weeknights, louder weekends, a
 * party one Saturday, speaker unplugged for a day now and then when `gaps`.
 * `days` of them (1..35), today's running level `today`.
 */
static daysui_t *month_at(long today_dn, int days, float today, bool gaps)
{
    daysui_t *s = fresh();
    s_seed = 7;
    for (int back = days - 1; back >= 0; back--) {
        long dn = today_dn - back;
        int wd = (int)((dn % 7 + 7 + 3) % 7);
        float v = 47.0f + 1.5f * noise() + (wd >= 5 ? 4.0f : 0.0f);
        if (back == 13) v += 12.0f;                     /* the party */
        float l90 = 34.0f + 1.2f * noise();
        if (back == 0) { v = today; l90 = 35.2f; }
        if (gaps && (back == 3 || back == 9 || back == 10 || back == 20)) { v = NAN; l90 = NAN; }
        add(s, today_dn, back, v, l90);
    }
    return s;
}

static daysui_t *month(int days, float today, bool gaps) { return month_at(TODAY, days, today, gaps); }

/* ---- the arithmetic --------------------------------------------------------- */

static void test_energy(void)
{
    float a[] = { 40.0f, 50.0f };
    expect("40 and 50 dBA are 47.40 dBA together, not 45", close_to(daysui_energy_mean(a, 2), 47.404f));
    float b[] = { 60.0f, 60.0f, 60.0f };
    expect("three 60s are 60", close_to(daysui_energy_mean(b, 3), 60.0f));
    float c[] = { 70.0f, 40.0f };
    expect("70 and 40 are 67, the loud day dominating", close_to(daysui_energy_mean(c, 2), 66.994f));
    expect("one level is itself", close_to(daysui_energy_mean(a + 1, 1), 50.0f));
    expect("none is NaN", isnan(daysui_energy_mean(a, 0)) && isnan(daysui_energy_mean(NULL, 2)));
}

static void test_weekday(void)
{
    expect("25 Sep 2026 is a Friday", daysui_weekday(2026, 9, 25) == 4);
    expect("1 Jan 2000 was a Saturday", daysui_weekday(2000, 1, 1) == 5);
    expect("29 Feb 2024 was a Thursday", daysui_weekday(2024, 2, 29) == 3);
    expect("a Monday is 0, not 1 as tm_wday has it", daysui_weekday(2026, 9, 21) == 0);
    expect("a Sunday is 6, not 0", daysui_weekday(2026, 9, 27) == 6);
    expect("29 Feb 2023 is not a date", daysui_weekday(2023, 2, 29) == -1);
    expect("31 Sep is not a date", daysui_weekday(2026, 9, 31) == -1);
    expect("month 0 and 13, day 0, year 0 are not dates",
           daysui_weekday(2026, 0, 1) == -1 && daysui_weekday(2026, 13, 1) == -1
           && daysui_weekday(2026, 9, 0) == -1 && daysui_weekday(0, 9, 25) == -1);
    int bad = 0;
    for (long dn = dnum(2000, 1, 1); dn < dnum(2100, 1, 1); dn++) {
        int y, m, d;
        civil(dn, &y, &m, &d);
        if (daysui_weekday(y, m, d) != (int)((dn + 3) % 7)) bad++;
    }
    expect("every date this century has its weekday", bad == 0);
}

static void test_baseline(void)
{
    daysui_t *s = month(35, 52.4f, false);
    daysui_base_t b = daysui_baseline(s);
    float fri[4];
    for (int k = 0; k < 4; k++) fri[k] = find(s, TODAY, 7 * (k + 1))->laeq;
    float want = daysui_energy_mean(fri, 4);
    printf("     full month: kind %d, %.2f dBA over %d, weekday %d\n", b.kind, (double)b.level, b.n, b.weekday);
    expect("a full month: the four earlier Fridays", b.kind == DAYSUI_BASE_WEEKDAY && b.n == 4 && b.weekday == 4);
    expect("...their energy mean", close_to(b.level, want));

    /* Not a Thursday, however loud. */
    find(s, TODAY, 1)->laeq = 95.0f;
    find(s, TODAY, 8)->laeq = 95.0f;
    expect("a loud Thursday does not move a Friday's baseline", close_to(daysui_baseline(s).level, want));

    /* Not today, however loud or quiet. */
    s->day[s->n - 1].laeq = 90.0f;
    expect("today's own level is never in its baseline", close_to(daysui_baseline(s).level, want));
    s->day[s->n - 1].laeq = NAN;
    expect("...nor its absence", close_to(daysui_baseline(s).level, want) && daysui_baseline(s).n == 4);

    /* A Friday speaker missed is skipped: three, of the rest. */
    s = month(35, 52.4f, false);
    find(s, TODAY, 14)->laeq = NAN;
    float three[3] = { fri[0], fri[2], fri[3] };
    b = daysui_baseline(s);
    expect("a missed Friday leaves three", b.kind == DAYSUI_BASE_WEEKDAY && b.n == 3
           && close_to(b.level, daysui_energy_mean(three, 3)));

    /* The order main.c keeps them in does not matter, nor a day twice. */
    s = month(35, 52.4f, false);
    for (int i = 0; i < s->n / 2; i++) {
        daysui_day_t t = s->day[i];
        s->day[i] = s->day[s->n - 1 - i];
        s->day[s->n - 1 - i] = t;
    }
    expect("newest first gives the same baseline", close_to(daysui_baseline(s).level, want) && daysui_baseline(s).n == 4);

    /* No earlier Friday yet: the latest days of any kind, up to seven, a
       missed one skipped -- ten days, the Friday among them missed, a
       Tuesday missed, so the seven reach back to the Thursday before. */
    s = fresh();
    for (int back = 10; back >= 0; back--) add(s, TODAY, back, 40.0f + (float)back, 30.0f);
    find(s, TODAY, 7)->laeq = NAN;    /* the Friday */
    find(s, TODAY, 3)->laeq = NAN;    /* a Tuesday */
    b = daysui_baseline(s);
    float recent[7] = { 41, 42, 44, 45, 46, 48, 49 };
    printf("     no Friday: kind %d, %.2f dBA over %d\n", b.kind, (double)b.level, b.n);
    expect("no earlier Friday: recent days", b.kind == DAYSUI_BASE_RECENT && b.n == 7);
    expect("...the latest seven with a level, missed days skipped", close_to(b.level, daysui_energy_mean(recent, 7)));

    /* Three days in: recent, three. */
    s = month(4, 50.0f, false);
    b = daysui_baseline(s);
    expect("three days in: recent days, three", b.kind == DAYSUI_BASE_RECENT && b.n == 3);

    /* The first day: nothing to compare with. */
    s = month(1, 50.0f, false);
    b = daysui_baseline(s);
    expect("the first day: no baseline", b.kind == DAYSUI_BASE_NONE && b.n == 0 && isnan(b.level) && b.weekday == 4);

    /* Earlier days all "--": none either. */
    s = month(6, 50.0f, false);
    for (int i = 0; i < s->n - 1; i++) s->day[i].laeq = NAN;
    expect("earlier days with no level: no baseline", daysui_baseline(s).kind == DAYSUI_BASE_NONE);

    /* No day flagged today: every day is earlier, no weekday to match. */
    s = month(10, 50.0f, false);
    s->day[s->n - 1].today = false;
    b = daysui_baseline(s);
    expect("no today: recent days of all of them", b.kind == DAYSUI_BASE_RECENT && b.n == 7 && b.weekday == -1);

    /* A day dated after today is not earlier. */
    s = month(3, 50.0f, false);
    add(s, TODAY, -1, 80.0f, 60.0f)->today = false;
    b = daysui_baseline(s);
    expect("a day after today is not in the baseline", b.n == 2 && b.level < 60.0f);

    /* A nonsense date is nowhere. */
    s = month(3, 50.0f, false);
    daysui_day_t *d = add(s, TODAY, 5, 80.0f, 60.0f);
    d->month = 13;
    expect("a nonsense date is in no baseline", daysui_baseline(s).n == 2);
    expect("a NULL struct has none", daysui_baseline(NULL).kind == DAYSUI_BASE_NONE);
}

static void test_background(void)
{
    int n = -1;
    daysui_t *s = fresh();
    float l90[] = { 50.0f, 30.0f, 36.0f, 31.0f, 33.0f, 38.0f, 32.0f, 35.0f };  /* back 8..1 */
    for (int k = 0; k < 8; k++) add(s, TODAY, 8 - k, 45.0f, l90[k]);
    add(s, TODAY, 0, 45.0f, 20.0f);
    float v = daysui_background(s, &n);
    printf("     background: %.2f over %d\n", (double)v, n);
    expect("the median of the latest seven, the eighth back left out", n == 7 && close_to(v, 33.0f));

    s->day[s->n - 1].l90 = 90.0f;
    expect("today's L90 is not in it", close_to(daysui_background(s, NULL), 33.0f));

    s = fresh();
    add(s, TODAY, 4, 45.0f, 30.0f);
    add(s, TODAY, 3, 45.0f, 40.0f);
    add(s, TODAY, 2, 45.0f, NAN);
    add(s, TODAY, 1, 45.0f, 34.0f);
    add(s, TODAY, 0, 45.0f, 20.0f);
    add(s, TODAY, 5, 45.0f, 32.0f);     /* out of order */
    v = daysui_background(s, &n);
    expect("an even count: the mean of the middle two, a missing L90 skipped", n == 4 && close_to(v, 33.0f));

    s = month(1, 50.0f, false);
    v = daysui_background(s, &n);
    expect("the first day: today's L90", n == 0 && close_to(v, 35.2f));
    s->day[0].l90 = NAN;
    expect("no L90 at all: NaN", isnan(daysui_background(s, &n)) && n == 0);
    expect("a NULL struct: NaN", isnan(daysui_background(NULL, &n)) && n == 0);
}

/* ---- framebuffers, BMPs ------------------------------------------------------- */

static uint16_t s_mem[GUARD + W * H + GUARD], s_mem2[GUARD + W * H + GUARD];
static uint16_t s_ground[W * H];

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

static void put16(FILE *f, unsigned v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
static void put32(FILE *f, unsigned long v)
{
    put16(f, (unsigned)(v & 0xFFFF));
    put16(f, (unsigned)((v >> 16) & 0xFFFF));
}

/* test_noiseui's writer: a 24-bit BMP of RGB565, each pixel `zoom` times. */
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
    for (int y = oh - 1; y >= 0; y--) {
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
    snprintf(path, sizeof path, "renders/daysui/%s.bmp", name);
    int ok = write_bmp(path, fb, 1);
    snprintf(path, sizeof path, "renders/daysui/%s_3x.bmp", name);
    ok = ok && write_bmp(path, fb, 3);
    char what[300];
    snprintf(what, sizeof what, "render %s written", name);
    expect(what, ok);
}

/* Where ink may go: test_noiseui's rule. The corners are R5 mm, 43 px; ink
   is kept 12 px from the sides, 8 from the top and foot, 8 inside the arcs. */
#define CORNER_R 43.0f
#define ARC_KEEP 8.0f
static int safe(int x, int y)
{
    if (x < 12 || x >= W - 12 || y < 8 || y >= H - 8) return 0;
    float px = (float)x + 0.5f, py = (float)y + 0.5f;
    float cx = px < CORNER_R ? CORNER_R : px > (float)W - CORNER_R ? (float)W - CORNER_R : px;
    float cy = py < CORNER_R ? CORNER_R : py > (float)H - CORNER_R ? (float)H - CORNER_R : py;
    float dx = px - cx, dy = py - cy;
    return sqrtf(dx * dx + dy * dy) <= CORNER_R - ARC_KEEP;
}

static int corners_clear(const uint16_t *fb)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (!safe(x, y) && fb[y * W + x] != s_ground[y * W + x]) {
                printf("     ink at (%d, %d), outside the safe area\n", x, y);
                return 0;
            }
    return 1;
}

/* Draws twice into guarded buffers: inside, the same both times, clear of
   the corners. A name starting '!' is not rendered. */
static const uint16_t *draw(const daysui_t *s, const char *name)
{
    canvas_t a = canvas_on(s_mem), b = canvas_on(s_mem2);
    daysui_draw(&a, s);
    daysui_draw(&b, s);
    const char *nm = name[0] == '!' ? name + 1 : name;
    char what[200];
    snprintf(what, sizeof what, "%s stays inside the framebuffer", nm);
    expect(what, guard_ok(s_mem) && guard_ok(s_mem2));
    snprintf(what, sizeof what, "%s draws the same pixels twice", nm);
    expect(what, memcmp(s_mem, s_mem2, sizeof s_mem) == 0);
    snprintf(what, sizeof what, "%s keeps out of the corners", nm);
    expect(what, corners_clear(s_mem + GUARD));
    if (name[0] != '!') render(name, s_mem + GUARD);
    return s_mem + GUARD;
}

static int ink(const uint16_t *fb, int x0, int y0, int x1, int y1)
{
    int n = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (fb[y * W + x] != s_ground[y * W + x]) n++;
    return n;
}

static int count(const uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t col)
{
    int n = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (fb[y * W + x] == col) n++;
    return n;
}

static int same_band(const uint16_t *a, const uint16_t *b, int y0, int y1)
{
    return memcmp(a + y0 * W, b + y0 * W, (size_t)(y1 - y0 + 1) * W * sizeof a[0]) == 0;
}

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define HATCH RGB(52, 58, 74)

/* The page's bands, as daysui.c lays them out, with slack. */
#define NUM_Y0 32
#define NUM_Y1 88
#define DELTA_Y0 90
#define DELTA_Y1 150
#define BARS_Y0 172
#define BARS_Y1 240

/* ---- the page ----------------------------------------------------------------- */

static void test_pages(void)
{
    const uint16_t *fb;
    static uint16_t keep[W * H];

    {
        canvas_t c = canvas_on(s_mem);
        daysui_draw(&c, NULL);
        expect("a NULL struct draws the ground and stays inside", guard_ok(s_mem));
        memcpy(s_ground, s_mem + GUARD, sizeof s_ground);
    }

    /* A full month, today louder than a usual Friday. */
    daysui_t *s = month(35, 52.4f, false);
    fb = draw(s, "full_month");
    int in_num = count(fb, 0, NUM_Y0, W - 1, NUM_Y1, noiseui_colour(52.4f));
    printf("     full month: %d px of today's colour in the figure\n", in_num);
    expect("today's figure is in the Sound page's colour for it", in_num > 800);
    expect("the bars are drawn", ink(fb, 15, BARS_Y0, 224, BARS_Y1) > 1000);
    {
        /* Each bar's cap is its day's colour. */
        int bad = 0;
        for (int k = 0; k < 7; k++) {
            float lv = find(s, TODAY, 6 - k)->laeq;
            int cx = (int)lroundf(15.0f + ((float)k + 0.5f) * 210.0f / 7.0f);
            if (count(fb, cx - 8, BARS_Y0, cx + 7, BARS_Y1, noiseui_colour(lv)) < 12) bad++;
        }
        expect("each bar is in its day's colour", bad == 0);
    }

    /* speaker's 30 days: narrow bars, today's figure only, and the day's
       name under today and each same weekday before it. */
    s = month(35, 52.4f, false);
    s->bars = 30;
    fb = draw(s, "month_30");
    expect("30 days: the bars are drawn", ink(fb, 15, BARS_Y0, 224, BARS_Y1) > 1000);
    s = month(35, 50.3f, true);
    s->bars = 30;
    draw(s, "month_30_sparse");

    /* The same, quieter: the minus path. */
    draw(month(35, 44.1f, false), "full_month_quieter");
    /* About the same. */
    draw(month(35, 48.6f, false), "full_month_same");

    /* Sparse: a missed day inside the week and a few further back. */
    fb = draw(month(35, 50.3f, true), "sparse");
    expect("sparse: the missed day inside the week is hatched", count(fb, 15, BARS_Y0, 224, BARS_Y1, HATCH) > 40);

    /* No earlier same weekday yet: five days in. */
    draw(month(5, 51.0f, false), "no_same_weekday");
    /* The first day. */
    fb = draw(month(1, 49.2f, false), "first_day");
    expect("first day: no hatching before speaker started", count(fb, 15, BARS_Y0, 224, BARS_Y1, HATCH) == 0);
    /* Today with no figure yet: just past midnight. */
    draw(month(35, NAN, false), "today_unknown");

    /* A loud week: a party two days ago at the top of the scale, a loud
       Monday, today loud -- the tallest bar's figure against BACKGROUND. */
    s = month(35, 72.0f, false);
    find(s, TODAY, 2)->laeq = 80.0f;
    find(s, TODAY, 4)->laeq = 66.0f;
    fb = draw(s, "loud_week");
    expect("loud week: the tallest bar's figure clears BACKGROUND", ink(fb, 12, 168, W - 13, 172) == 0);

    /* A Wednesday: its full name will not fit "than a usual ..." and it is
       said in three letters. */
    draw(month_at(TODAY + 5, 35, 51.3f, false), "wednesday");

    /* Nothing received. */
    s = month(35, 52.4f, false);
    s->have_data = false;
    fb = draw(s, "no_data");
    expect("no data: something is written", ink(fb, 12, 20, W - 13, 130) > 300);
    expect("no data: no level's colour anywhere",
           count(fb, 0, 0, W - 1, H - 1, noiseui_colour(52.4f)) == 0
           && count(fb, 0, 0, W - 1, H - 1, noiseui_colour(47.0f)) == 0);
    expect("no data: no bars", ink(fb, 12, BARS_Y0, W - 13, H - 9) == 0);

    /* Uncalibrated: EST, and nothing else changes. */
    draw(month(35, 52.4f, false), "!cal");
    memcpy(keep, s_mem + GUARD, sizeof keep);
    s = month(35, 52.4f, false);
    s->calibrated = false;
    fb = draw(s, "uncalibrated");
    {
        int diff = 0, x0 = W, x1 = -1, y0 = H, y1 = -1;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (fb[y * W + x] != keep[y * W + x]) {
                    diff++;
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
        printf("     EST: %d px differ, in x %d..%d, y %d..%d\n", diff, x0, x1, y0, y1);
        expect("uncalibrated adds EST and changes nothing else", diff > 40 && y0 >= NUM_Y0 && y1 <= 60 && x0 > 120);
    }

    /* Stale: dimmed -- today's figure out of its colour, the past bars kept. */
    s = month(35, 52.4f, false);
    s->stale = true;
    fb = draw(s, "stale");
    expect("stale: today's figure is not in its colour",
           count(fb, 0, NUM_Y0, W - 1, NUM_Y1, noiseui_colour(52.4f)) == 0);
    expect("stale: the past days' bars are still drawn", ink(fb, 16, BARS_Y0, 160, BARS_Y1) > 600);

    /* The 1 dB line: 0.99 either way is the same words; 1.0 is not. */
    {
        static uint16_t up[W * H];
        s = fresh();
        add(s, TODAY, 7, 50.0f, 35.0f);
        add(s, TODAY, 0, 50.99f, 35.0f);
        draw(s, "!plus_0.99");
        memcpy(up, s_mem + GUARD, sizeof up);
        s->day[1].laeq = 49.01f;
        fb = draw(s, "!minus_0.99");
        expect("+0.99 and -0.99 dB are both about the same", same_band(up, fb, DELTA_Y0, DELTA_Y0 + 32));
        s->day[1].laeq = 51.0f;
        fb = draw(s, "!plus_1.0");
        expect("+1.0 dB is not", !same_band(up, fb, DELTA_Y0, DELTA_Y0 + 32));
        memcpy(up, fb, sizeof up);
        s->day[1].laeq = 49.0f;
        fb = draw(s, "!minus_1.0");
        expect("-1.0 dB is not +1.0", !same_band(up, fb, DELTA_Y0, DELTA_Y0 + 32));
    }
}

/* Whatever it is handed, nothing lands outside the framebuffer. */
static void test_garbage(void)
{
    const float bad[] = { NAN, INFINITY, -INFINITY, 1e9f, -1e9f, 0.0f, -3.0f, 199.4f, 250.0f, 29.9f, 80.1f, 55.0f };
    int nb = (int)(sizeof bad / sizeof bad[0]);
    int ok = 1;
    for (int a = 0; a < nb && ok; a++)
        for (int b = 0; b < nb && ok; b++) {
            daysui_t *s = month(35, bad[a], (a & 1) != 0);
            s->calibrated = (b & 1) != 0;
            s->stale = (a + b) % 3 == 0;
            for (int i = 0; i < s->n; i++) {
                s->day[i].laeq = bad[(i + a) % nb];
                s->day[i].l90 = bad[(i + b) % nb];
                s->day[i].today = (i * 7 + a) % 5 == 0;
                if ((i + b) % 6 == 0) s->day[i].month = (uint8_t)(i * 37 + a);
                if ((i + a) % 7 == 0) s->day[i].day = (uint8_t)(i * 11 + b);
                if ((i + a + b) % 9 == 0) s->day[i].year = (uint16_t)(i * 997 + a);
            }
            canvas_t c = canvas_on(s_mem);
            daysui_draw(&c, s);
            if (!guard_ok(s_mem) || !corners_clear(s_mem + GUARD)) ok = 0;
            (void)daysui_baseline(s);
            (void)daysui_background(s, NULL);
        }
    expect("any levels and dates at all stay inside and out of the corners", ok);

    /* A count out of range is held to the array. */
    int counts[] = { -5, -1, 0, 36, 1000, 2147483647, -2147483647 - 1 };
    ok = 1;
    for (size_t k = 0; k < sizeof counts / sizeof counts[0]; k++) {
        daysui_t *s = month(35, 52.4f, false);
        s->n = counts[k];
        canvas_t c = canvas_on(s_mem);
        daysui_draw(&c, s);
        daysui_base_t b = daysui_baseline(s);
        if (!guard_ok(s_mem) || !corners_clear(s_mem + GUARD) || b.n > 7) ok = 0;
    }
    expect("n out of range is clamped", ok);

    /* A canvas of another size is clipped, not overrun. */
    static uint16_t small[GUARD + 100 * 50 + GUARD];
    for (size_t i = 0; i < sizeof small / sizeof small[0]; i++) small[i] = CANARY;
    canvas_t cs;
    canvas_init(&cs, small + GUARD, 100, 50, 1);
    daysui_draw(&cs, month(35, 52.4f, true));
    int sok = 1;
    for (size_t i = 0; i < GUARD; i++)
        if (small[i] != CANARY || small[GUARD + 100 * 50 + i] != CANARY) sok = 0;
    expect("a small canvas is clipped", sok);
    daysui_draw(NULL, &s_d);
    expect("a NULL canvas is a no-op", 1);

    bool before = vec_linear_light(false);
    canvas_t c = canvas_on(s_mem);
    daysui_draw(&c, month(35, 52.4f, false));
    bool after = vec_linear_light(before);
    expect("linear light is put back", after == false);
}

int main(void)
{
    mkdir("renders", 0755);
    mkdir("renders/daysui", 0755);

    test_energy();
    test_weekday();
    test_baseline();
    test_background();
    test_pages();
    test_garbage();

    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("all passed\n");
    return 0;
}
