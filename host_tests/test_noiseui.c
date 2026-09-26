/* For mkdir, which C11 alone does not declare. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "noiseui.h"
#include "ring.h"
#include "vector.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * watch's Sound page on the host: that it never writes outside the
 * framebuffer whatever it is handed, that the same input draws the same
 * pixels, that its colour is speaker's ring's colour, that the word comes
 * from LAeq,3s by the spec's thresholds, that with no signal there is no
 * number anywhere on it, that EST is the only thing calibration changes,
 * that nothing lands in the panel's rounded corners -- and renders of it in
 * every state to look at.
 *
 * The renders go to host_tests/renders/noiseui/ as 24-bit BMPs, each also at
 * three times the size (pixels repeated, not smoothed). Turn each into a PNG
 * with
 *     sips -s format png X.bmp --out X.png
 */

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

#define W NOISEUI_WIDTH
#define H NOISEUI_HEIGHT
#define GUARD 64
#define CANARY 0xA5C3
#define N NOISEUI_POINTS

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* Two framebuffers with a guard band of canaries either side, and the ground
   alone, drawn from a NULL struct, to tell ink from background. */
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
    snprintf(path, sizeof path, "renders/noiseui/%s.bmp", name);
    int ok = write_bmp(path, fb, 1);
    snprintf(path, sizeof path, "renders/noiseui/%s_3x.bmp", name);
    ok = ok && write_bmp(path, fb, 3);
    char what[300];
    snprintf(what, sizeof what, "render %s written", name);
    expect(what, ok);
}

/* ---- an hour of a room, deterministic ------------------------------------ */

/* A small LCG rather than rand(): the renders and the determinism check must
   come out the same on every host. */
static uint32_t s_seed;
static float noise(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (float)(s_seed >> 8) / (float)(1u << 24) * 2.0f - 1.0f;
}

static noiseui_t s_n;

/*
 * An hour around `base` dBA: a room's slow drift, a conversation's swell in
 * the middle, a few door-slam spikes, and -- when `gaps` -- speaker's USB
 * unplugged for four minutes and a lone line dropped here and there. The
 * last few minutes lead into `now`, so the strip ends where the number is.
 */
static noiseui_t *room(float laf, float laeq3, float today, bool cal, bool gaps)
{
    noiseui_t *s = &s_n;
    memset(s, 0, sizeof *s);
    s->have_signal = true;
    s->laf = laf;
    s->laeq3 = laeq3;
    s->today = today;
    s->calibrated = cal;
    s_seed = 11;
    float base = laeq3 - 4.0f;
    for (int i = 0; i < N; i++) {
        float t = (float)i / (float)(N - 1);
        float v = base + 3.0f * sinf(t * 9.0f) + 2.0f * noise();
        v += 10.0f * expf(-((t - 0.45f) * (t - 0.45f)) / 0.004f);       /* a conversation */
        if (i == 70 || i == 71 || i == 200 || i == 290) v += 16.0f;       /* a door */
        if (i > N - 20) v += (laeq3 - v) * (float)(i - (N - 20)) / 20.0f;
        s->hist[i] = v;
        s->hist_valid[i] = true;
        if (gaps && ((i >= 130 && i < 154) || i == 250 || i == 251 || i == 318)) s->hist_valid[i] = false;
    }
    return s;
}

/* ---- drawing, checked ---------------------------------------------------- */

/* Draws the page twice, into two guarded buffers, and checks the guards and
   that the two came out the same. A name starting '!' is not rendered. */
static const uint16_t *draw(const noiseui_t *s, const char *name)
{
    canvas_t a = canvas_on(s_mem), b = canvas_on(s_mem2);
    noiseui_draw(&a, s);
    noiseui_draw(&b, s);
    char what[200];
    snprintf(what, sizeof what, "%s stays inside the framebuffer", name);
    expect(what, guard_ok(s_mem) && guard_ok(s_mem2));
    snprintf(what, sizeof what, "%s draws the same pixels twice", name);
    expect(what, memcmp(s_mem, s_mem2, sizeof s_mem) == 0);
    if (name[0] != '!') render(name, s_mem + GUARD);
    return s_mem + GUARD;
}

/* Pixels in a box that are not the ground: ink. */
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

/*
 * Where ink may go. The panel's corners are R5 mm on the glass, 43 px at its
 * 0.1166 mm pitch: a pixel whose centre lies further than 43 px from the
 * nearest corner circle's centre, in a corner square, is not on the glass at
 * all. Ink is kept 12 px from the sides, 8 px from the top and foot, and 8 px
 * inside each corner's arc.
 */
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

/* How many pixels in a box are near a colour: each channel within `tol` of
   it in its own 5- or 6-bit scale. */
static int near(const uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t col, int tol)
{
    int n = 0;
    int cr = (col >> 11) & 31, cg = (col >> 5) & 63, cb = col & 31;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            uint16_t p = fb[y * W + x];
            int r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
            if (abs(r - cr) <= tol && abs(g - cg) <= 2 * tol && abs(b - cb) <= tol) n++;
        }
    return n;
}

#define HATCH RGB(52, 58, 74)

/* The bands of the page, as noiseui.c lays them out -- the number's capitals
   27..83, the word's 100..116, today's line 141..158, the strip's rows
   174..233 over its floor at 234, columns 16..195 -- with a few pixels'
   slack for overshoot. */
#define NUM_Y0 22
#define NUM_Y1 88
#define WORD_Y0 96
#define WORD_Y1 122
#define TODAY_Y0 136
#define TODAY_Y1 162
#define STRIP_X0 16
#define STRIP_X1 195
#define STRIP_Y0 172
#define STRIP_Y1 236

/* ---- the tests ------------------------------------------------------------ */

static void test_colour(void)
{
    /* The ring's four stops come out as the spec's bytes, through the same
       RGB565 conversion as every colour in the codebase. */
    expect("45 dBA is the ring's green, (0, 200, 60)", noiseui_colour(45.0f) == RGB(0, 200, 60));
    expect("55 dBA is the ring's yellow, (255, 200, 0)", noiseui_colour(55.0f) == RGB(255, 200, 0));
    expect("65 dBA is the ring's orange, (255, 110, 0)", noiseui_colour(65.0f) == RGB(255, 110, 0));
    expect("75 dBA is the ring's red, (255, 0, 0)", noiseui_colour(75.0f) == RGB(255, 0, 0));
    expect("below 45 stays green", noiseui_colour(20.0f) == noiseui_colour(45.0f));
    expect("above 75 stays red", noiseui_colour(110.0f) == noiseui_colour(75.0f));

    /* Between the stops it is ring_colour_for's light, encoded: check every
       half dB against the ring's own numbers. */
    int bad = 0;
    for (float d = 40.0f; d <= 80.0f; d += 0.5f) {
        float lin[3];
        ring_colour_for(d, lin);
        int b[3];
        for (int k = 0; k < 3; k++) {
            float v = lin[k] > 0.0f ? powf(lin[k], 1.0f / 2.2f) * 255.0f : 0.0f;
            b[k] = v >= 255.0f ? 255 : (int)(v + 0.5f);
        }
        if (noiseui_colour(d) != RGB(b[0], b[1], b[2])) bad++;
    }
    expect("every level's colour is the ring's", bad == 0);

    /* The midpoint of yellow and orange keeps its light: ring.c blends in
       linear light, so green at 60 dBA is well over the codes' mean (155). */
    uint16_t mid = noiseui_colour(60.0f);
    int g8 = ((mid >> 5) & 63) << 2;
    printf("     60 dBA: green %d of 255\n", g8);
    expect("halfway from yellow to orange does not sag", g8 >= 160);
}

static void test_state(void)
{
    expect("44.9 is QUIET", noiseui_state_for(44.9f) == NOISEUI_QUIET);
    expect("45 is MODERATE", noiseui_state_for(45.0f) == NOISEUI_MODERATE);
    expect("59.9 is MODERATE", noiseui_state_for(59.9f) == NOISEUI_MODERATE);
    expect("60 is LOUD", noiseui_state_for(60.0f) == NOISEUI_LOUD);
    expect("74.9 is LOUD", noiseui_state_for(74.9f) == NOISEUI_LOUD);
    expect("75 is VERY LOUD", noiseui_state_for(75.0f) == NOISEUI_VERY_LOUD);
    expect("NaN is QUIET, and draws no word", noiseui_state_for(NAN) == NOISEUI_QUIET);
    expect("-inf is QUIET", noiseui_state_for(-INFINITY) == NOISEUI_QUIET);
    expect("+inf is VERY LOUD", noiseui_state_for(INFINITY) == NOISEUI_VERY_LOUD);
    expect("the words", strcmp(noiseui_state_name(NOISEUI_QUIET), "QUIET") == 0
           && strcmp(noiseui_state_name(NOISEUI_MODERATE), "MODERATE") == 0
           && strcmp(noiseui_state_name(NOISEUI_LOUD), "LOUD") == 0
           && strcmp(noiseui_state_name(NOISEUI_VERY_LOUD), "VERY LOUD") == 0
           && strcmp(noiseui_state_name((noiseui_state_t)99), "QUIET") == 0);
}

static void test_pages(void)
{
    const uint16_t *fb;

    /* The ground on its own, for telling ink from it. */
    {
        canvas_t c = canvas_on(s_mem);
        noiseui_draw(&c, NULL);
        expect("a NULL struct draws the ground and stays inside", guard_ok(s_mem));
        memcpy(s_ground, s_mem + GUARD, sizeof s_ground);
        render("ground", s_ground);
    }

    /* The four words, each in its colour, at levels in the middle of each. */
    struct { float laf, laeq3; const char *name; } lv[] = {
        { 38.4f, 37.6f, "quiet_38" },
        { 52.2f, 51.7f, "moderate_52" },
        { 68.0f, 67.2f, "loud_68" },
        { 82.3f, 81.5f, "very_loud_82" },
    };
    for (int k = 0; k < 4; k++) {
        fb = draw(room(lv[k].laf, lv[k].laeq3, 47.3f, true, false), lv[k].name);
        char what[200];
        uint16_t col = noiseui_colour(lv[k].laeq3);
        int in_num = count(fb, 0, NUM_Y0, W - 1, NUM_Y1, col);
        int in_word = count(fb, 0, WORD_Y0, W - 1, WORD_Y1, col);
        printf("     %s: %d px of its colour in the number, %d in the word\n", lv[k].name, in_num, in_word);
        snprintf(what, sizeof what, "%s: the number is in the level's colour", lv[k].name);
        expect(what, in_num > 800);
        snprintf(what, sizeof what, "%s: the word is in the level's colour", lv[k].name);
        expect(what, in_word > 80);
        snprintf(what, sizeof what, "%s: nothing in the rounded corners", lv[k].name);
        expect(what, corners_clear(fb));
        snprintf(what, sizeof what, "%s: the strip has its trace", lv[k].name);
        expect(what, ink(fb, STRIP_X0, STRIP_Y0, STRIP_X1, STRIP_Y1) > 1000);
    }

    /* est against cal: the same page but for the EST mark over the unit. */
    static uint16_t cal[W * H];
    draw(room(52.2f, 51.7f, 47.3f, true, false), "!cal");
    memcpy(cal, s_mem + GUARD, sizeof cal);
    fb = draw(room(52.2f, 51.7f, 47.3f, false, false), "moderate_52_est");
    {
        int diff = 0, x0 = W, x1 = -1, y0 = H, y1 = -1;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (fb[y * W + x] != cal[y * W + x]) {
                    diff++;
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
        printf("     EST: %d px differ, in x %d..%d, y %d..%d\n", diff, x0, x1, y0, y1);
        expect("uncalibrated adds EST and changes nothing else", diff > 40 && y0 >= NUM_Y0 && y1 <= 70 && x0 > 120);
        expect("EST is gold", count(fb, x0, y0, x1, y1, RGB(224, 186, 118)) > 10);
    }

    /* No signal: no figure anywhere, the word gone, the strip kept. The laf
       and today handed in are real numbers -- the page must not show them. */
    noiseui_t *s = room(52.2f, 51.7f, 47.3f, true, true);
    s->have_signal = false;
    fb = draw(s, "no_signal");
    expect("no signal: nothing in the level's colour in the number's band",
           near(fb, 0, NUM_Y0, W - 1, NUM_Y1, noiseui_colour(51.7f), 1) == 0);
    expect("no signal: no word", count(fb, 0, WORD_Y0, W - 1, WORD_Y1, noiseui_colour(51.7f)) == 0);
    expect("no signal: NO SIGNAL is written", ink(fb, 12, NUM_Y0, W - 13, NUM_Y1) > 300);
    expect("no signal: the strip is still there", ink(fb, STRIP_X0, STRIP_Y0, STRIP_X1, STRIP_Y1) > 1000);
    expect("no signal: no now-dot past the strip's end",
           near(fb, 198, STRIP_Y0, 201, STRIP_Y1, noiseui_colour(51.7f), 2) == 0);
    expect("no signal: nothing in the corners", corners_clear(fb));
    {
        /* Today's figure is not drawn: the today line is the same whatever
           today says while there is no signal. */
        static uint16_t a[W * H];
        memcpy(a, fb, sizeof a);
        s->today = 71.0f;
        fb = draw(s, "!no_signal_today");
        int same = 1;
        for (int y = TODAY_Y0; y <= TODAY_Y1; y++)
            for (int x = 0; x < W; x++)
                if (fb[y * W + x] != a[y * W + x]) same = 0;
        expect("no signal: today's figure is not shown", same);
    }

    /* Today unknown ("today --"). */
    fb = draw(room(52.2f, 51.7f, NAN, true, false), "today_unknown");
    expect("today unknown: still a today line", ink(fb, 12, TODAY_Y0, W - 13, TODAY_Y1) > 50);

    /* History with gaps: hatched where lines were missing, from the first
       point on; the latest point live. */
    fb = draw(room(47.0f, 46.2f, 45.0f, false, true), "gaps_est");
    expect("gaps: nothing in the corners", corners_clear(fb));
    expect("gaps: hatched", count(fb, STRIP_X0, STRIP_Y0, STRIP_X1, STRIP_Y1, HATCH) > 40);

    /* A point that says it is valid but is not a number is a gap, drawn
       exactly as one that says it is not valid. */
    {
        static uint16_t a[W * H];
        s = room(52.2f, 51.7f, 47.3f, true, false);
        for (int i = 100; i < 120; i++) s->hist_valid[i] = false;
        draw(s, "!gap_flagged");
        memcpy(a, s_mem + GUARD, sizeof a);
        s = room(52.2f, 51.7f, 47.3f, true, false);
        for (int i = 100; i < 120; i++) s->hist[i] = NAN;
        fb = draw(s, "!gap_nan");
        expect("a NaN point is drawn as a gap", memcmp(a, fb, sizeof a) == 0);
    }

    /* A board just started: the history empty but for the last two minutes. */
    s = room(44.0f, 43.1f, NAN, false, false);
    for (int i = 0; i < N - 12; i++) s->hist_valid[i] = false;
    fb = draw(s, "just_started");
    expect("just started: no hatching before the first point", count(fb, STRIP_X0, STRIP_Y0, 150, STRIP_Y1 - 3, HATCH) == 0);

    /* Loud enough to reach the top of the scale, and three digits. */
    fb = draw(room(104.0f, 96.0f, 71.4f, true, false), "pinned_104");
    expect("pinned: nothing in the corners", corners_clear(fb));

    /* The figure runs ahead of the word: a slam, the fast level at 78 while
       the 3 s level is still 52 -- the ring would be filled far round in
       yellow. */
    draw(room(78.4f, 52.0f, 47.3f, true, false), "slam_78_over_52");

    /* A level that is not a number: no figure in colour, no word. */
    fb = draw(room(NAN, NAN, NAN, true, false), "not_a_number");
    expect("NaN: no word", ink(fb, 12, WORD_Y0, W - 13, WORD_Y1) == 0);
}

/* Whatever it is handed, nothing lands outside the framebuffer. */
static void test_garbage(void)
{
    const float bad[] = { NAN, INFINITY, -INFINITY, 1e9f, -1e9f, 0.0f, -3.0f, 199.4f, 250.0f, 29.9f, 90.1f };
    int nb = (int)(sizeof bad / sizeof bad[0]);
    int ok = 1;
    for (int a = 0; a < nb && ok; a++)
        for (int b = 0; b < nb && ok; b++) {
            noiseui_t *s = room(50.0f, 50.0f, 50.0f, (a & 1) != 0, (b & 1) != 0);
            s->laf = bad[a];
            s->laeq3 = bad[b];
            s->today = bad[(a + b) % nb];
            s->have_signal = ((a + b) % 3) != 0;
            for (int i = 0; i < N; i++) s->hist[i] = bad[(i + a) % nb];
            canvas_t c = canvas_on(s_mem);
            noiseui_draw(&c, s);
            if (!guard_ok(s_mem)) ok = 0;
            else if (!corners_clear(s_mem + GUARD)) ok = 0;
        }
    expect("any levels at all stay inside the framebuffer and out of the corners", ok);

    /* Garbage in the validity flags too: every byte pattern a bool may hold
       in memory is 0 or 1 here, so vary which are set. */
    noiseui_t *s = room(60.0f, 60.0f, 60.0f, true, false);
    for (int i = 0; i < N; i++) s->hist_valid[i] = (i * 7) % 3 == 0;
    canvas_t c = canvas_on(s_mem);
    noiseui_draw(&c, s);
    expect("scattered validity stays inside", guard_ok(s_mem));

    /* A canvas of another size is clipped, not overrun. */
    static uint16_t small[GUARD + 100 * 50 + GUARD];
    for (size_t i = 0; i < sizeof small / sizeof small[0]; i++) small[i] = CANARY;
    canvas_t cs;
    canvas_init(&cs, small + GUARD, 100, 50, 1);
    noiseui_draw(&cs, room(82.0f, 81.0f, 70.0f, false, true));
    int sok = 1;
    for (size_t i = 0; i < GUARD; i++)
        if (small[i] != CANARY || small[GUARD + 100 * 50 + i] != CANARY) sok = 0;
    expect("a small canvas is clipped", sok);
    noiseui_draw(NULL, s);
    expect("a NULL canvas is a no-op", 1);

    /* vector.c's blend setting is put back as it was found. */
    bool before = vec_linear_light(false);
    c = canvas_on(s_mem);
    noiseui_draw(&c, s);
    bool after = vec_linear_light(before);
    expect("linear light is put back", after == false);
}

int main(void)
{
    mkdir("renders", 0755);                 /* fine if they are already there */
    mkdir("renders/noiseui", 0755);

    test_colour();
    test_state();
    test_pages();
    test_garbage();

    if (failures) { printf("%d FAILED\n", failures); return 1; }
    printf("all passed\n");
    return 0;
}
