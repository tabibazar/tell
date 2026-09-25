/* For mkdir, which C11 alone does not declare; and for mmap's MAP_ANON,
   which strict POSIX hides on macOS and glibc alike. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include "face.h"
#include "moonphase.h"
#include "vfont.h"

#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/*
 * watch's face on the host: that it never writes outside its buffers
 * whatever it is given, that it draws the same pixels every time, that the
 * cached background changes nothing, with and without a photo behind the
 * dial, that the moon page's moon is a round disc a third of the way down
 * at every phase, and renders of it all to look at.
 *
 * The photos are read from ../assets/watch/{earth,moon,space}.bin, the
 * files the firmware embeds, so run this from host_tests/.
 *
 * The renders go to host_tests/renders/ as 24-bit BMPs, each also at three
 * times the size (pixels repeated, not smoothed) since a 240x280 image is
 * too small to judge a hairline in. Turn each into a PNG with
 *     sips -s format png X.bmp --out X.png
 */

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

#define W 240
#define H 280
#define GUARD 64
#define CANARY 0xA5C3

/* A framebuffer with a guard band of canaries either side. */
typedef struct {
    uint16_t mem[GUARD + W * H + GUARD];
} guarded_t;

static void guard_fill(uint16_t *mem, size_t n)
{
    for (size_t i = 0; i < GUARD; i++) {
        mem[i] = CANARY;
        mem[GUARD + n + i] = CANARY;
    }
}

static int guard_ok(const uint16_t *mem, size_t n)
{
    for (size_t i = 0; i < GUARD; i++)
        if (mem[i] != CANARY || mem[GUARD + n + i] != CANARY) return 0;
    return 1;
}

/* ---- dates, for building states ---------------------------------------- */

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yoe + era * 400) + (*m <= 2);
}

/* A self-consistent state for a local date and time: the weekday from the
   date, the moon from the same instant in UTC. */
static face_state_t state_at(int y, int mo, int d, int h, int mi, int s,
                             int battery, bool charging, int32_t offset)
{
    face_state_t st;
    memset(&st, 0, sizeof st);
    int64_t days = days_from_civil(y, mo, d);
    int64_t local = days * 86400 + h * 3600 + mi * 60 + s;
    moonphase_t m;
    moonphase_compute(local - offset, &m);
    st.hour = h;
    st.minute = mi;
    st.second = s;
    st.year = y;
    st.month = mo;
    st.day = d;
    st.weekday = (int)(((days % 7) + 11) % 7);
    st.moon_phase = m.phase;
    st.moon_age_days = m.age_days;
    st.battery_pct = battery;
    st.charging = charging;
    st.next_new = m.next_new;
    st.next_full = m.next_full;
    st.utc_offset = offset;
    return st;
}

/* ---- photos ------------------------------------------------------------- */

#define PHOTO_N (FACE_PHOTO_W * FACE_PHOTO_H)
static uint16_t s_earth[PHOTO_N], s_moon[PHOTO_N], s_space[PHOTO_N];

/* One of assets/watch's .bin photos: exactly 240x280 little-endian RGB565,
   decoded byte by byte so the host's own byte order does not matter. */
static int load_photo(const char *name, uint16_t *out)
{
    char path[256];
    snprintf(path, sizeof path, "../assets/watch/%s.bin", name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) return 0;
    static unsigned char raw[PHOTO_N * 2 + 1];
    size_t n = fread(raw, 1, sizeof raw, f);
    fclose(f);
    if (n != (size_t)PHOTO_N * 2) return 0;
    for (int i = 0; i < PHOTO_N; i++)
        out[i] = (uint16_t)(raw[2 * i] | (raw[2 * i + 1] << 8));
    return 1;
}

static void load_photos(void)
{
    expect("photo: earth.bin is 240x280 RGB565", load_photo("earth", s_earth));
    expect("photo: moon.bin is 240x280 RGB565", load_photo("moon", s_moon));
    expect("photo: space.bin is 240x280 RGB565", load_photo("space", s_space));
}

static const struct { const char *name; const uint16_t *px; } s_photos[3] = {
    { "earth", s_earth }, { "moon", s_moon }, { "space", s_space },
};

/* ---- measuring renders -------------------------------------------------- */

static float lum565(uint16_t p)
{
    unsigned r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
    return 0.2126f * (float)((r << 3) | (r >> 2)) + 0.7152f * (float)((g << 2) | (g >> 4))
         + 0.0722f * (float)((b << 3) | (b >> 2));
}

/* Luminance averaged over the 3x3 round a pixel, which takes out the
   ordered dither; edge pixels average what is there. */
static float lum_at(const uint16_t *fb, int w, int h, int i, int j)
{
    float acc = 0.0f;
    int n = 0;
    for (int dj = -1; dj <= 1; dj++)
        for (int di = -1; di <= 1; di++) {
            int x = i + di, y = j + dj;
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            acc += lum565(fb[y * w + x]);
            n++;
        }
    return n ? acc / (float)n : 0.0f;
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
static int write_bmp(const char *path, const uint16_t *fb, int w, int h, int zoom)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) return 0;
    int ow = w * zoom, oh = h * zoom;
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
            uint16_t p = fb[(y / zoom) * w + x / zoom];
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
    snprintf(path, sizeof path, "renders/%s.bmp", name);
    int ok = write_bmp(path, fb, W, H, 1);
    snprintf(path, sizeof path, "renders/%s_3x.bmp", name);
    ok = ok && write_bmp(path, fb, W, H, 3);
    char what[300];
    snprintf(what, sizeof what, "render %s written", name);
    expect(what, ok);
}

/* ---- the tests ---------------------------------------------------------- */

static guarded_t s_fb, s_fb2, s_bg, s_bg2;

static void test_vfont(void)
{
    static uint16_t fb[64 * 32];
    canvas_t c = { fb, 64, 32, 1, 0, 0, 0, 0 };
    vfont_style_t st = { 10.0f, 1.2f, 0.0f };

    expect("vfont: empty string is zero wide", vfont_width(&st, "") == 0.0f);
    expect("vfont: NULL string is zero wide", vfont_width(&st, NULL) == 0.0f);
    expect("vfont: a longer string is wider",
           vfont_width(&st, "MON") > vfont_width(&st, "MO"));
    vfont_style_t tr = st;
    tr.tracking = 2.0f;
    expect("vfont: tracking adds between letters only",
           fabsf(vfont_width(&tr, "ABC") - vfont_width(&st, "ABC") - 4.0f) < 1e-4f);

    memset(fb, 0, sizeof fb);
    vfont_draw(&c, &st, 32, 16, 0.0f, VFONT_CENTRE, 0xFFFF, "8");
    int lit = 0;
    for (int i = 0; i < 64 * 32; i++) lit += fb[i] != 0;
    expect("vfont: a glyph lights pixels", lit > 20);

    static uint16_t fb2[64 * 32];
    canvas_t c2 = { fb2, 64, 32, 1, 0, 0, 0, 0 };
    memset(fb, 0, sizeof fb);
    memset(fb2, 0, sizeof fb2);
    vfont_draw(&c, &st, 32, 16, 0.0f, VFONT_CENTRE, 0xFFFF, "sun");
    vfont_draw(&c2, &st, 32, 16, 0.0f, VFONT_CENTRE, 0xFFFF, "SUN");
    expect("vfont: lower case draws as capitals", memcmp(fb, fb2, sizeof fb) == 0);

    memset(fb, 0, sizeof fb);
    vfont_draw(&c, &st, NAN, 16, 0.0f, VFONT_CENTRE, 0xFFFF, "A");
    vfont_draw(&c, &st, 32, 16, INFINITY, VFONT_CENTRE, 0xFFFF, "A");
    vfont_style_t bad = { NAN, 1.0f, 0.0f };
    vfont_draw(&c, &bad, 32, 16, 0.0f, VFONT_CENTRE, 0xFFFF, "A");
    lit = 0;
    for (int i = 0; i < 64 * 32; i++) lit += fb[i] != 0;
    expect("vfont: NaN and infinite arguments draw nothing", lit == 0);

    /* A stroke of the same weight is the same weight whether it is one
       straight run or a curve made of many: the joins are not blended in
       twice. An O's ink is about its perimeter times the weight. */
    memset(fb, 0, sizeof fb);
    vfont_style_t o = { 20.0f, 1.5f, 0.0f };
    vfont_draw(&c, &o, 32, 16, 0.0f, VFONT_CENTRE, 0xFFFF, "O");
    double ink = 0.0;
    for (int i = 0; i < 64 * 32; i++) ink += (double)((fb[i] >> 5) & 0x3F) / 63.0;
    /* Ellipse 15.2 x 20: Ramanujan's perimeter, about 55.6 px. */
    double want = 55.6 * 1.5;
    expect("vfont: a curve's ink matches its length times its weight",
           fabs(ink - want) < 0.08 * want);

    /* Clipping: text across every edge, turned, never writes outside. */
    static uint16_t mem[GUARD + 30 * 20 + GUARD];
    guard_fill(mem, 30 * 20);
    canvas_t e = { mem + GUARD, 30, 20, 1, 0, 0, 0, 0 };
    for (int k = 0; k < 40; k++) {
        float a = (float)k * 0.4f;
        vfont_draw(&e, &st, -5.0f + (float)k, -6.0f + (float)(k % 9) * 4.0f, a,
                   k % 3 - 1, 0xFFFF, "WED 31 %:/,");
    }
    vfont_draw(&e, &(vfont_style_t){ 400.0f, 30.0f, 5.0f }, 15, 10, 0.3f,
               VFONT_CENTRE, 0xFFFF, "MQ");
    vfont_draw(&e, &st, 1e9f, 1e9f, 0.0f, VFONT_LEFT, 0xFFFF, "A");
    expect("vfont: clipped at every edge", guard_ok(mem, 30 * 20));
}

static void test_bounds(void)
{
    face_t f;
    int bad = 0, n = 0;
    guard_fill(s_fb.mem, W * H);
    guard_fill(s_bg.mem, W * H);
    canvas_t c = { s_fb.mem + GUARD, W, H, 1, 0, 0, 0, 0 };
    face_init(&f, s_bg.mem + GUARD, W, H);

    static const int batteries[] = { -5, -1, 0, 1, 5, 16, 17, 55, 99, 100, 101, 1000 };
    for (int k = 0; k < 400; k++) {
        int h = (k * 7) % 24, mi = (k * 13) % 60, s = (k * 29) % 60;
        int y = 2020 + k % 12, mo = 1 + k % 12, d = 1 + (k * 5) % 31;
        face_state_t st = state_at(y, mo, d > 28 ? 28 : d, h, mi, s,
                                   batteries[k % 12], k % 5 == 0, -4 * 3600);
        st.day = d;
        face_draw(&f, &c, &st);
        n++;
        if (!guard_ok(s_fb.mem, W * H) || !guard_ok(s_bg.mem, W * H)) bad++;
    }
    expect("bounds: 400 faces across times, dates and batteries stay inside",
           bad == 0);

    /* Nonsense in every field. */
    face_state_t st = state_at(2026, 9, 24, 10, 8, 37, 55, false, 0);
    st.hour = -7; st.minute = 1234; st.second = -61;
    st.day = 0; st.month = 13; st.weekday = -9;
    st.moon_phase = NAN; st.moon_age_days = INFINITY;
    st.battery_pct = 12345;
    face_draw(&f, &c, &st);
    face_draw_moon_page(&c, &st);
    st.moon_phase = -3.75; st.day = 99; st.weekday = 99; st.hour = 99;
    st.next_full = -5; st.next_new = INT64_MAX / 2;
    face_draw(&f, &c, &st);
    face_draw_moon_page(&c, &st);
    /* The ends of every integer range: next_new + utc_offset and the
       year's y - 1 once overflowed here (seen under -fsanitize=undefined;
       a plain build only proves nothing lands outside). */
    face_state_t ex = st;
    ex.next_new = INT64_MAX; ex.next_full = INT64_MAX - 1; ex.utc_offset = INT32_MAX;
    ex.year = INT_MIN; ex.month = 1;
    face_draw_moon_page(&c, &ex);
    ex.utc_offset = INT32_MIN; ex.year = INT_MIN + 1; ex.next_full = 1;
    face_draw_moon_page(&c, &ex);
    ex.year = INT_MAX; ex.month = 1; ex.day = INT_MIN;
    face_draw_moon_page(&c, &ex);
    expect("bounds: nonsense fields stay inside",
           guard_ok(s_fb.mem, W * H) && guard_ok(s_bg.mem, W * H));

    /* The moon page at every phase. */
    for (int k = 0; k < 60; k++) {
        face_state_t m = state_at(2026, 1 + k % 12, 1 + k % 28, 12, 0, 0, 50, false,
                                  3600 * (k % 25 - 12));
        m.moon_phase = (double)k / 60.0;
        face_draw_moon_page(&c, &m);
    }
    expect("bounds: moon page at 60 phases stays inside", guard_ok(s_fb.mem, W * H));

    /* Other panel shapes, down to a single pixel, with no background. */
    static const int sizes[][2] = {
        { 320, 172 }, { 172, 320 }, { 200, 200 }, { 1, 1 }, { 3, 280 },
        { 240, 5 }, { 17, 31 }, { 120, 140 },
    };
    int shapes_ok = 1;
    for (int k = 0; k < (int)(sizeof sizes / sizeof sizes[0]); k++) {
        int w = sizes[k][0], h = sizes[k][1];
        guard_fill(s_fb2.mem, (size_t)(w * h));
        canvas_t o = { s_fb2.mem + GUARD, w, h, 1, 0, 0, 0, 0 };
        face_draw(&f, &o, &st);             /* a size the cache is not */
        face_draw_moon_page(&o, &st);
        face_draw_full(&o, &st);
        if (!guard_ok(s_fb2.mem, (size_t)(w * h))) shapes_ok = 0;
    }
    expect("bounds: other panel shapes stay inside", shapes_ok);
    expect("bounds: background canaries intact", guard_ok(s_bg.mem, W * H));

    /* Every path again with each photo behind the dial: cached, drawn
       afresh (no background), onto other panel shapes (the photo scaled
       with the dial), and with nonsense in every field. */
    for (int ph = 0; ph < 3; ph++) {
        face_t pf, nf;
        face_init(&pf, s_bg.mem + GUARD, W, H);
        face_set_photo(&pf, s_photos[ph].px);
        face_init(&nf, NULL, W, H);
        face_set_photo(&nf, s_photos[ph].px);
        int pbad = 0;
        for (int k = 0; k < 60; k++) {
            int h = (k * 7) % 24, mi = (k * 13) % 60, s = (k * 29) % 60;
            face_state_t ps = state_at(2020 + k % 12, 1 + k % 12, 1 + (k * 5) % 28, h, mi,
                                       s, batteries[k % 12], k % 3 == 0, -4 * 3600);
            face_draw(&pf, &c, &ps);
            if (k % 10 == 0) face_draw(&nf, &c, &ps);
            if (!guard_ok(s_fb.mem, W * H) || !guard_ok(s_bg.mem, W * H)) pbad++;
        }
        face_state_t ns = st;               /* the nonsense from above */
        face_draw(&pf, &c, &ns);
        face_draw(&nf, &c, &ns);
        ns.moon_phase = NAN; ns.hour = -7; ns.minute = 1234; ns.second = -61;
        face_draw(&pf, &c, &ns);
        if (!guard_ok(s_fb.mem, W * H) || !guard_ok(s_bg.mem, W * H)) pbad++;
        for (int k = 0; k < (int)(sizeof sizes / sizeof sizes[0]); k++) {
            int w = sizes[k][0], h = sizes[k][1];
            guard_fill(s_fb2.mem, (size_t)(w * h));
            canvas_t o = { s_fb2.mem + GUARD, w, h, 1, 0, 0, 0, 0 };
            face_draw(&pf, &o, &ns);        /* a size the cache is not */
            face_draw(&nf, &o, &st);
            if (!guard_ok(s_fb2.mem, (size_t)(w * h))) pbad++;
        }
        char what[96];
        snprintf(what, sizeof what, "bounds: every face path with the %s photo stays inside",
                 s_photos[ph].name);
        expect(what, pbad == 0);
    }

    /* The moon page with the moon photo as its surface. */
    face_set_moon_texture(s_moon);
    for (int k = 0; k < 60; k++) {
        face_state_t m = state_at(2026, 1 + k % 12, 1 + k % 28, 12, 0, 0, 50, false,
                                  3600 * (k % 25 - 12));
        m.moon_phase = (double)k / 60.0;
        face_draw_moon_page(&c, &m);
    }
    face_draw_moon_page(&c, &st);
    int tex_ok = guard_ok(s_fb.mem, W * H);
    for (int k = 0; k < (int)(sizeof sizes / sizeof sizes[0]); k++) {
        int w = sizes[k][0], h = sizes[k][1];
        guard_fill(s_fb2.mem, (size_t)(w * h));
        canvas_t o = { s_fb2.mem + GUARD, w, h, 1, 0, 0, 0, 0 };
        face_draw_moon_page(&o, &st);
        if (!guard_ok(s_fb2.mem, (size_t)(w * h))) tex_ok = 0;
    }
    face_set_moon_texture(NULL);
    expect("bounds: the textured moon page at 60 phases and every shape stays inside",
           tex_ok);
    (void)n;
}

static void test_determinism_and_cache(void)
{
    canvas_t a = { s_fb.mem + GUARD, W, H, 1, 0, 0, 0, 0 };
    canvas_t b = { s_fb2.mem + GUARD, W, H, 1, 0, 0, 0, 0 };
    face_state_t st = state_at(2026, 9, 24, 10, 8, 37, 55, false, -4 * 3600);

    face_draw_full(&a, &st);
    face_draw_full(&b, &st);
    expect("determinism: the same state draws the same pixels",
           memcmp(a.fb, b.fb, W * H * 2) == 0);

    /* One face reused across a run of states changing one thing at a time,
       against one drawn afresh each time: they must agree at every step,
       and the background must be redrawn only when the moon moves. */
    face_t fresh, cached;
    face_init(&cached, s_bg.mem + GUARD, W, H);
    face_state_t seq[12];
    int n = 0;
    seq[n++] = state_at(2026, 9, 24, 10, 8, 37, 55, false, -4 * 3600);
    seq[n] = seq[n - 1]; seq[n].second = 38; n++;
    seq[n] = seq[n - 1]; seq[n].minute = 9; n++;
    seq[n] = seq[n - 1]; seq[n].hour = 11; n++;
    seq[n] = seq[n - 1]; seq[n].day = 25; n++;
    seq[n] = seq[n - 1]; seq[n].weekday = 5; n++;
    seq[n] = seq[n - 1]; seq[n].battery_pct = 54; n++;
    seq[n] = seq[n - 1]; seq[n].battery_pct = -1; n++;
    seq[n] = seq[n - 1]; seq[n].charging = true; n++;
    seq[n] = seq[n - 1]; seq[n].moon_phase += 0.02; n++;       /* 3.6 degrees */
    seq[n] = seq[n - 1]; seq[n].second = 0; n++;
    int agree = 1;
    uint32_t rebuilds_before_moon = 0;
    static uint16_t bg_before[W * H];
    for (int k = 0; k < n; k++) {
        face_init(&fresh, s_bg2.mem + GUARD, W, H);
        face_draw(&fresh, &a, &seq[k]);
        if (k == 1) memcpy(bg_before, cached.bg, sizeof bg_before);
        face_draw(&cached, &b, &seq[k]);
        if (k == 1)
            expect("cache: a seconds-only draw leaves the background alone",
                   memcmp(bg_before, cached.bg, sizeof bg_before) == 0);
        if (memcmp(a.fb, b.fb, W * H * 2) != 0) agree = 0;
        if (k == n - 3) rebuilds_before_moon = cached.rebuilds;
    }
    expect("cache: cached and fresh faces agree at every step", agree);
    expect("cache: only the first draw built the dial before the moon moved",
           rebuilds_before_moon == 1);
    expect("cache: the moon moving rebuilt it once more", cached.rebuilds == 2);

    face_draw_full(&b, &seq[n - 1]);
    expect("cache: face_draw matches face_draw_full", memcmp(a.fb, b.fb, W * H * 2) == 0);

    /* Drawn with its own background as the canvas, a face paints its
       hands into that background; the next draw onto a real canvas must
       rebuild it, not copy those hands out as if they were the dial. */
    face_t own;
    face_init(&own, s_bg.mem + GUARD, W, H);
    face_draw(&own, &b, &seq[0]);
    canvas_t into_bg = { own.bg, W, H, 1, 0, 0, 0, 0 };
    face_draw(&own, &into_bg, &seq[3]);           /* other hands, same moon */
    face_draw(&own, &b, &seq[0]);
    face_draw_full(&a, &seq[0]);
    expect("cache: drawing into the background itself does not leave its hands behind",
           memcmp(a.fb, b.fb, W * H * 2) == 0);
    expect("cache: and the dial was rebuilt for it", own.rebuilds == 2);

    face_draw_moon_page(&a, &st);
    face_draw_moon_page(&b, &st);
    expect("determinism: the moon page draws the same pixels",
           memcmp(a.fb, b.fb, W * H * 2) == 0);
}

/* A face that drew nothing would pass everything above; this checks that
   each input actually moves something. */
static void test_responds(void)
{
    canvas_t a = { s_fb.mem + GUARD, W, H, 1, 0, 0, 0, 0 };
    canvas_t b = { s_fb2.mem + GUARD, W, H, 1, 0, 0, 0, 0 };
    face_t f;
    face_init(&f, s_bg.mem + GUARD, W, H);
    face_state_t base = state_at(2026, 9, 24, 10, 8, 37, 55, false, -4 * 3600);

    struct { const char *what; face_state_t st; } alt[8];
    int n = 0;
    alt[n].what = "responds: a second later";
    alt[n].st = base; alt[n].st.second = 38; n++;
    alt[n].what = "responds: another weekday";
    alt[n].st = base; alt[n].st.weekday = 5; n++;
    alt[n].what = "responds: another date";
    alt[n].st = base; alt[n].st.day = 25; n++;
    alt[n].what = "responds: another battery level";
    alt[n].st = base; alt[n].st.battery_pct = 54; n++;
    alt[n].what = "responds: no battery reading is not an empty one";
    alt[n].st = base; alt[n].st.battery_pct = -1; n++;
    alt[n].what = "responds: charging";
    alt[n].st = base; alt[n].st.charging = true; n++;
    alt[n].what = "responds: another moon";
    alt[n].st = base; alt[n].st.moon_phase = 0.9; n++;
    alt[n].what = "responds: another hour";
    alt[n].st = base; alt[n].st.hour = 4; n++;

    for (int k = 0; k < n; k++) {
        face_state_t ref = base;
        if (k == 4) ref.battery_pct = 0;       /* unknown against empty */
        face_draw(&f, &a, &ref);
        face_draw(&f, &b, &alt[k].st);
        expect(alt[k].what, memcmp(a.fb, b.fb, W * H * 2) != 0);
    }

    face_state_t m1 = base, m2 = base;
    m2.moon_phase = 0.2;
    face_draw_moon_page(&a, &m1);
    face_draw_moon_page(&b, &m2);
    expect("responds: the moon page follows the moon", memcmp(a.fb, b.fb, W * H * 2) != 0);
}

/* The run of states the cache tests step through, changing one thing at a
   time; the moon moves at the third from last. */
static int cache_sequence(face_state_t seq[12])
{
    int n = 0;
    seq[n++] = state_at(2026, 9, 24, 10, 8, 37, 55, false, -4 * 3600);
    seq[n] = seq[n - 1]; seq[n].second = 38; n++;
    seq[n] = seq[n - 1]; seq[n].minute = 9; n++;
    seq[n] = seq[n - 1]; seq[n].hour = 11; n++;
    seq[n] = seq[n - 1]; seq[n].day = 25; n++;
    seq[n] = seq[n - 1]; seq[n].weekday = 5; n++;
    seq[n] = seq[n - 1]; seq[n].battery_pct = 54; n++;
    seq[n] = seq[n - 1]; seq[n].battery_pct = -1; n++;
    seq[n] = seq[n - 1]; seq[n].charging = true; n++;
    seq[n] = seq[n - 1]; seq[n].moon_phase += 0.02; n++;       /* 3.6 degrees */
    seq[n] = seq[n - 1]; seq[n].second = 0; n++;
    return n;
}

/* Inside the moon aperture on the 240x280 panel, with a margin of `m`
   pixels clear of its edge: the half disc of radius 32 over its base at
   y = 105, less the two humps of radius 10.9 at x = 120 -+ 19. */
static int in_aperture(int i, int j, float m)
{
    float x = (float)i + 0.5f - 120.0f, y = (float)j + 0.5f - 105.0f;
    if (y > -m || sqrtf(x * x + y * y) > 32.0f - m) return 0;
    float h1 = hypotf(x + 19.0f, y), h2 = hypotf(x - 19.0f, y);
    return h1 > 10.9f + m && h2 > 10.9f + m;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static void test_photo(void)
{
    canvas_t a = { s_fb.mem + GUARD, W, H, 1, 0, 0, 0, 0 };
    canvas_t b = { s_fb2.mem + GUARD, W, H, 1, 0, 0, 0, 0 };
    face_state_t seq[12];
    int n = cache_sequence(seq);
    char what[128];

    /* With a photo, the cached face against one with no background, which
       shades its dial afresh every time, through the whole run. */
    for (int ph = 0; ph < 3; ph++) {
        face_t cached, fresh;
        face_init(&cached, s_bg.mem + GUARD, W, H);
        face_set_photo(&cached, s_photos[ph].px);
        face_init(&fresh, NULL, W, H);
        face_set_photo(&fresh, s_photos[ph].px);
        int agree = 1;
        for (int k = 0; k < n; k++) {
            face_draw(&fresh, &a, &seq[k]);
            face_draw(&cached, &b, &seq[k]);
            if (memcmp(a.fb, b.fb, W * H * 2) != 0) agree = 0;
        }
        snprintf(what, sizeof what, "photo %s: cached and fresh faces agree at every step",
                 s_photos[ph].name);
        expect(what, agree);
        snprintf(what, sizeof what, "photo %s: the dial was built twice, once per moon",
                 s_photos[ph].name);
        expect(what, cached.rebuilds == 2);
    }

    /* Setting a photo invalidates the background only when it changes. */
    face_t f;
    face_init(&f, s_bg.mem + GUARD, W, H);
    expect("photo: a new face has none", f.photo == NULL);
    face_set_photo(&f, s_earth);
    face_draw(&f, &a, &seq[0]);
    face_draw(&f, &a, &seq[1]);
    expect("photo: set once, the dial is built once", f.rebuilds == 1);
    face_set_photo(&f, s_earth);
    face_draw(&f, &a, &seq[2]);
    expect("photo: setting the same photo again costs nothing", f.rebuilds == 1);
    static uint16_t earth_bg[W * H];
    memcpy(earth_bg, f.bg, sizeof earth_bg);
    face_set_photo(&f, s_space);
    face_draw(&f, &a, &seq[2]);
    expect("photo: another photo rebuilds the dial", f.rebuilds == 2);
    static uint16_t space_bg[W * H];
    memcpy(space_bg, f.bg, sizeof space_bg);
    face_set_photo(&f, NULL);
    face_draw(&f, &a, &seq[2]);
    expect("photo: NULL rebuilds it again", f.rebuilds == 3);
    face_draw_full(&b, &seq[2]);
    expect("photo: NULL is the sunburst dial exactly", memcmp(a.fb, b.fb, W * H * 2) == 0);
    static uint16_t plain_bg[W * H];
    memcpy(plain_bg, f.bg, sizeof plain_bg);

    expect("photo: the earth dial is not the sunburst",
           memcmp(earth_bg, plain_bg, sizeof plain_bg) != 0);
    expect("photo: the earth and space dials differ",
           memcmp(earth_bg, space_bg, sizeof plain_bg) != 0);

    /* The moon aperture is the same whatever is behind the dial. */
    int same = 1, inside = 0;
    for (int j = 0; j < H; j++)
        for (int i = 0; i < W; i++) {
            if (!in_aperture(i, j, 2.5f)) continue;
            inside++;
            if (earth_bg[j * W + i] != plain_bg[j * W + i]
                || space_bg[j * W + i] != plain_bg[j * W + i]) same = 0;
        }
    expect("photo: the moon aperture is untouched by the photo", same && inside > 800);

    /* The sub-dials stay dark enough to read over every photo, and the
       photo is dimmed: the dial's median brightness well under the
       photo's own. Medians, so the cream print does not count. */
    static const int sub_c[3][2] = { { 66, 140 }, { 174, 140 }, { 120, 194 } };
    static float vals[W * H];
    for (int ph = 0; ph < 3; ph++) {
        face_t pf;
        face_init(&pf, s_bg.mem + GUARD, W, H);
        face_set_photo(&pf, s_photos[ph].px);
        face_draw(&pf, &a, &seq[0]);
        float worst = 0.0f;
        for (int k = 0; k < 3; k++) {
            int nv = 0;
            for (int j = sub_c[k][1] - 24; j <= sub_c[k][1] + 24; j++)
                for (int i = sub_c[k][0] - 24; i <= sub_c[k][0] + 24; i++) {
                    float dx = (float)i + 0.5f - (float)sub_c[k][0];
                    float dy = (float)j + 0.5f - (float)sub_c[k][1];
                    if (dx * dx + dy * dy < 24.0f * 24.0f)
                        vals[nv++] = lum565(pf.bg[j * W + i]);
                }
            qsort(vals, (size_t)nv, sizeof vals[0], cmp_float);
            if (vals[nv / 2] > worst) worst = vals[nv / 2];
        }
        int nv = 0, np = 0;
        static float pv[W * H];
        for (int i = 0; i < W * H; i++) {
            vals[nv++] = lum565(pf.bg[i]);
            pv[np++] = lum565(s_photos[ph].px[i]);
        }
        qsort(vals, (size_t)nv, sizeof vals[0], cmp_float);
        qsort(pv, (size_t)np, sizeof pv[0], cmp_float);
        printf("     %s: sub-dial median lum %.0f; dial median %.0f against the photo's %.0f\n",
               s_photos[ph].name, worst, vals[nv / 2], pv[np / 2]);
        snprintf(what, sizeof what, "photo %s: the sub-dials stay dark", s_photos[ph].name);
        expect(what, worst < 48.0f);
        snprintf(what, sizeof what, "photo %s: the photo is dimmed", s_photos[ph].name);
        expect(what, vals[nv / 2] < 0.6f * pv[np / 2] + 1.0f);
    }
}

/* ---- reads stay inside the photo ---------------------------------------
 * The three photos above sit side by side in static memory, so a tap one
 * texel before or past one of them lands in its neighbour and shows
 * nothing. Here each photo is copied up against an inaccessible page,
 * once ending exactly where the page begins and once starting exactly
 * where it ends, and then drawn at the panel's own size and at others,
 * where the photo is scaled with the dial and the bilinear taps fall
 * between its edge texels. Any read outside the photo faults at once. */

static void on_fault(int sig)
{
    static const char msg[] =
        "FAIL photo: a read landed outside the photo, on its guard page\n";
    (void)sig;
    ssize_t r = write(1, msg, sizeof msg - 1);
    (void)r;
    _exit(1);
}

typedef struct {
    unsigned char *map;
    size_t len;
    uint16_t *px;
} guarded_photo_t;

/* A copy of `src` against a guard page: before it if `after` is 0, after
   it otherwise. False if the host will not map one. */
static int guarded_photo(const uint16_t *src, int after, guarded_photo_t *gp)
{
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0) return 0;
    size_t page = (size_t)pg, bytes = PHOTO_N * sizeof(uint16_t);
    size_t span = (bytes + page - 1) / page * page;
    gp->len = span + page;
    gp->map = mmap(NULL, gp->len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (gp->map == MAP_FAILED) return 0;
    unsigned char *guard = after ? gp->map + span : gp->map;
    unsigned char *photo = after ? gp->map + span - bytes : gp->map + page;
    if (mprotect(guard, page, PROT_NONE) != 0) {
        munmap(gp->map, gp->len);
        return 0;
    }
    memcpy(photo, src, bytes);
    gp->px = (uint16_t *)(void *)photo;
    return 1;
}

static void test_photo_guard(void)
{
    static const int sizes[][2] = {
        { 240, 280 }, { 480, 560 }, { 243, 283 }, { 239, 279 }, { 320, 172 },
        { 172, 320 }, { 1000, 50 }, { 17, 31 },
    };
    static uint16_t fb[480 * 560], bg[480 * 560];
    face_state_t st = state_at(2026, 9, 24, 10, 8, 37, 55, false, -4 * 3600);
    int mapped = 1;

    /* The previous handlers are put back after, so a sanitizer's own
       fault reporting still covers the tests that follow. */
    void (*prev_segv)(int) = signal(SIGSEGV, on_fault);
    void (*prev_bus)(int) = signal(SIGBUS, on_fault);
    for (int after = 0; after < 2; after++) {
        for (int ph = 0; ph < 3; ph++) {
            guarded_photo_t gp;
            if (!guarded_photo(s_photos[ph].px, after, &gp)) {
                mapped = 0;
                continue;
            }
            face_set_moon_texture(gp.px);
            for (int k = 0; k < (int)(sizeof sizes / sizeof sizes[0]); k++) {
                int w = sizes[k][0], h = sizes[k][1];
                canvas_t c = { fb, w, h, 1, 0, 0, 0, 0 };
                face_t f;
                face_init(&f, k == 0 ? bg : NULL, w, h);    /* cached at 240x280 */
                face_set_photo(&f, gp.px);
                face_draw(&f, &c, &st);
                face_draw_moon_page(&c, &st);
            }
            face_set_moon_texture(NULL);
            munmap(gp.map, gp.len);
        }
    }
    signal(SIGSEGV, prev_segv == SIG_ERR ? SIG_DFL : prev_segv);
    signal(SIGBUS, prev_bus == SIG_ERR ? SIG_DFL : prev_bus);
    expect("photo: a guard page could be mapped either side of a photo", mapped);
    expect("photo: no read before or past a photo, dial or moon page, at any panel size",
           1);                              /* a stray read never gets here */
}

/* ---- the moon page ------------------------------------------------------ */

/* Real dates for each phase, in EDT: local wall time. */
typedef struct {
    const char *tag, *name;
    int y, mo, d, h, mi, s;
} moon_date_t;

static const moon_date_t s_moon_dates[] = {
    { "new", "New moon", 2026, 10, 10, 20, 0, 0 },
    { "waxing_crescent", "Waxing crescent", 2026, 10, 14, 20, 0, 0 },
    { "first_quarter", "First quarter", 2026, 10, 18, 20, 0, 0 },
    { "waxing_gibbous_95", "Waxing gibbous", 2026, 9, 24, 10, 8, 37 },
    { "full", "Full moon", 2026, 9, 26, 3, 45, 12 },
    { "waning_gibbous", "Waning gibbous", 2026, 9, 29, 20, 0, 0 },
    { "last_quarter", "Last quarter", 2026, 10, 3, 20, 0, 0 },
    { "waning_crescent", "Waning crescent", 2026, 10, 7, 20, 0, 0 },
};
#define MOON_DATES ((int)(sizeof s_moon_dates / sizeof s_moon_dates[0]))

/* Where the moon page's moon is meant to be on 240x280: across the middle,
   a third of the way down, 72 px in radius. */
#define MP_CX  (W / 2.0f)
#define MP_CY  (H / 3.0f)
#define MP_R   72.0f

typedef struct {
    float sky;      /* the brightest sky just outside the limb */
    float dark;     /* the darkest of the disc just inside it */
    float thr;      /* halfway between: the disc's edge */
    int left, right, top, bottom;   /* the disc's extent, inclusive */
    float right_mean, left_mean;    /* each half's mean, clear of the middle */
} disc_t;

/* Measures the disc from the render alone. The sky and the disc are
   sampled on rings just outside and inside where the disc should be, and
   the edge is taken halfway between their brightest and darkest; then,
   from the middle, the extent along the centre row and column is where
   two pixels in a row fall below that. A disc off centre or not round
   puts disc pixels on the outer ring (or sky on the inner), and it shows
   either in the margin between them or in the extents. */
static disc_t measure_disc(const uint16_t *fb)
{
    disc_t m;
    memset(&m, 0, sizeof m);
    m.sky = 0.0f;
    m.dark = 1e9f;
    double rs = 0.0, ls = 0.0;
    int rn = 0, ln = 0;
    for (int j = 0; j < H; j++)
        for (int i = 0; i < W; i++) {
            float dx = (float)i + 0.5f - MP_CX, dy = (float)j + 0.5f - MP_CY;
            float d = sqrtf(dx * dx + dy * dy);
            if (d >= MP_R + 3.0f && d <= MP_R + 7.0f) {
                float l = lum_at(fb, W, H, i, j);
                if (l > m.sky) m.sky = l;
            } else if (d <= MP_R - 3.0f) {
                float l = lum_at(fb, W, H, i, j);
                if (l < m.dark) m.dark = l;
                if (dx > 4.0f) { rs += l; rn++; }
                if (dx < -4.0f) { ls += l; ln++; }
            }
        }
    m.right_mean = rn ? (float)(rs / rn) : 0.0f;
    m.left_mean = ln ? (float)(ls / ln) : 0.0f;
    m.thr = 0.5f * (m.sky + m.dark);

    int i0 = (int)MP_CX, j0 = (int)MP_CY;
    int i, j;
    for (i = i0; i > 0; i--)
        if (lum_at(fb, W, H, i - 1, j0) < m.thr && lum_at(fb, W, H, i - 2, j0) < m.thr) break;
    m.left = i;
    for (i = i0; i < W - 1; i++)
        if (lum_at(fb, W, H, i + 1, j0) < m.thr && lum_at(fb, W, H, i + 2, j0) < m.thr) break;
    m.right = i;
    for (j = j0; j > 0; j--)
        if (lum_at(fb, W, H, i0, j - 1) < m.thr && lum_at(fb, W, H, i0, j - 2) < m.thr) break;
    m.top = j;
    for (j = j0; j < H - 1; j++)
        if (lum_at(fb, W, H, i0, j + 1) < m.thr && lum_at(fb, W, H, i0, j + 2) < m.thr) break;
    m.bottom = j;
    return m;
}

static void test_moon_page(void)
{
    canvas_t c = { s_fb.mem + GUARD, W, H, 1, 0, 0, 0, 0 };
    int32_t edt = -4 * 3600;
    char what[160], name[96];

    for (int tex = 1; tex >= 0; tex--) {
        face_set_moon_texture(tex ? s_moon : NULL);
        const char *kind = tex ? "photo" : "drawn";
        /* Each half of the disc at full, to measure the others against,
           which takes the surface's own light and dark out of it. */
        face_state_t fs = state_at(2026, 9, 26, 3, 45, 12, 50, false, edt);
        fs.moon_phase = 0.5;
        face_draw_moon_page(&c, &fs);
        disc_t full = measure_disc(c.fb);
        for (int k = 0; k < MOON_DATES; k++) {
            const moon_date_t *md = &s_moon_dates[k];
            face_state_t st = state_at(md->y, md->mo, md->d, md->h, md->mi, md->s, 50,
                                       false, edt);
            double illum = 0.5 * (1.0 - cos(6.283185307179586 * st.moon_phase));
            if (tex) {
                printf("     %04d-%02d-%02d %02d:%02d  phase %.4f  %3.0f%% lit  %s\n",
                       md->y, md->mo, md->d, md->h, md->mi, st.moon_phase, 100.0 * illum,
                       moonphase_name(st.moon_phase));
                snprintf(what, sizeof what, "moon page: %04d-%02d-%02d is %s", md->y,
                         md->mo, md->d, md->name);
                expect(what, strcmp(moonphase_name(st.moon_phase), md->name) == 0);
            }
            face_draw_moon_page(&c, &st);
            snprintf(name, sizeof name, "moon_page_%04d%02d%02d_%s%s", md->y, md->mo,
                     md->d, md->tag, tex ? "" : "_drawn");
            render(name, c.fb);

            disc_t m = measure_disc(c.fb);
            int wdt = m.right - m.left + 1, hgt = m.bottom - m.top + 1;
            float cx = 0.5f * (float)(m.left + m.right + 1);
            float cy = 0.5f * (float)(m.top + m.bottom + 1);
            float lr = m.left_mean / full.left_mean, rr = m.right_mean / full.right_mean;
            printf("     %-18s %s: sky %.0f, disc's darkest %.0f; halves lit %.2f | %.2f;"
                   " %d x %d at (%.1f, %.1f)\n", md->tag, kind, m.sky, m.dark, lr, rr,
                   wdt, hgt, cx, cy);
            snprintf(what, sizeof what,
                     "moon page %s %s: the whole disc stands clear of the sky", md->tag, kind);
            expect(what, m.dark >= m.sky + 8.0f);
            snprintf(what, sizeof what, "moon page %s %s: the disc is round (%d x %d)",
                     md->tag, kind, wdt, hgt);
            expect(what, abs(wdt - hgt) <= 2 && fabsf((float)wdt - 2.0f * MP_R) <= 3.0f);
            snprintf(what, sizeof what,
                     "moon page %s %s: centred across and a third of the way down",
                     md->tag, kind);
            expect(what, fabsf(cy - MP_CY) <= 2.0f && fabsf(cx - MP_CX) <= 2.0f);

            /* Waxing is lit on the right, waning on the left: the half
               towards the sun keeps more of its light at full. */
            double p = st.moon_phase;
            if ((p > 0.05 && p < 0.45) || (p > 0.55 && p < 0.95)) {
                int waxing = p < 0.5;
                snprintf(what, sizeof what, "moon page %s %s: lit on the %s", md->tag, kind,
                         waxing ? "right" : "left");
                expect(what, waxing ? rr > lr + 0.03f : lr > rr + 0.03f);
            }
        }
    }
    face_set_moon_texture(NULL);

    /* The texture changes the surface, not where the moon is. */
    face_state_t st = state_at(2026, 9, 24, 10, 8, 37, 50, false, edt);
    canvas_t d = { s_fb2.mem + GUARD, W, H, 1, 0, 0, 0, 0 };
    face_draw_moon_page(&c, &st);
    face_set_moon_texture(s_moon);
    face_draw_moon_page(&d, &st);
    face_set_moon_texture(NULL);
    expect("moon page: the texture shows", memcmp(c.fb, d.fb, W * H * 2) != 0);
    face_set_moon_texture(s_moon);
    face_draw_moon_page(&c, &st);
    face_set_moon_texture(NULL);
    expect("moon page: the textured page draws the same pixels every time",
           memcmp(c.fb, d.fb, W * H * 2) == 0);
}

static void test_render(void)
{
    canvas_t c = { s_fb.mem + GUARD, W, H, 1, 0, 0, 0, 0 };
    mkdir("renders", 0755);                 /* fine if it is already there */
    face_t f;
    face_init(&f, s_bg.mem + GUARD, W, H);

    int32_t edt = -4 * 3600, est = -5 * 3600;
    face_state_t st = state_at(2026, 9, 24, 10, 8, 37, 72, false, edt);
    expect("2026-09-24 is a Thursday", st.weekday == 4);
    face_draw(&f, &c, &st);
    render("face_20260924_100837", c.fb);

    /* The next full moon after that, at 03:45:12 local on its date. */
    int64_t local = st.next_full + edt;
    int64_t days = local >= 0 ? local / 86400 : (local - 86399) / 86400;
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    face_state_t full = state_at(y, m, d, 3, 45, 12, 88, false, edt);
    printf("     full moon date %04d-%02d-%02d, phase %.4f (%s)\n", y, m, d,
           full.moon_phase, moonphase_name(full.moon_phase));
    expect("the full-moon render is near full",
           fabs(full.moon_phase - 0.5) < 0.04);
    face_draw(&f, &c, &full);
    render("face_fullmoon_034512", c.fb);

    /* Midnight on the same date: both hands straight up across the full
       gold moon in the aperture and a full reserve's champagne arch, where
       only their outline keeps them apart from what they cross. */
    face_state_t midnight = state_at(y, m, d, 0, 0, 0, 100, false, edt);
    face_draw(&f, &c, &midnight);
    render("face_fullmoon_000000", c.fb);

    face_state_t nye = state_at(2026, 12, 31, 23, 59, 59, 40, false, est);
    face_draw(&f, &c, &nye);
    render("face_20261231_235959", c.fb);

    static const int batt[4] = { 5, 55, 100, -1 };
    static const char *const bname[4] = { "5", "55", "100", "unknown" };
    for (int k = 0; k < 4; k++) {
        face_state_t b = state_at(2026, 9, 24, 10, 8, 37, batt[k], false, edt);
        face_draw(&f, &c, &b);
        char name[64];
        snprintf(name, sizeof name, "face_battery_%s", bname[k]);
        render(name, c.fb);
    }
    face_state_t chg = state_at(2026, 9, 24, 10, 8, 37, 55, true, edt);
    face_draw(&f, &c, &chg);
    render("face_battery_55_charging", c.fb);

    /* Each photo behind the dial, and none, at 10:08:37 and at 19:41:23
       on 2027-02-14, when both hands lie over the brightest part of every
       photo, the lower half. */
    face_state_t val = state_at(2027, 2, 14, 19, 41, 23, 12, true, est);
    face_draw(&f, &c, &val);
    render("face_20270214_194123", c.fb);
    for (int ph = 0; ph < 3; ph++) {
        face_t pf;
        face_init(&pf, s_bg2.mem + GUARD, W, H);
        face_set_photo(&pf, s_photos[ph].px);
        char name[64];
        face_draw(&pf, &c, &st);
        snprintf(name, sizeof name, "face_%s_20260924_100837", s_photos[ph].name);
        render(name, c.fb);
        face_draw(&pf, &c, &val);
        snprintf(name, sizeof name, "face_%s_20270214_194123", s_photos[ph].name);
        render(name, c.fb);
    }

    /* A new moon, so the humps are seen hiding it. */
    local = st.next_new + edt;
    days = local / 86400;
    civil_from_days(days, &y, &m, &d);
    face_state_t newm = state_at(y, m, d, 7, 21, 3, 30, false, edt);
    face_draw(&f, &c, &newm);
    render("face_newmoon", c.fb);

}

static void test_timing(void)
{
    canvas_t c = { s_fb.mem + GUARD, W, H, 1, 0, 0, 0, 0 };
    face_t f;
    face_init(&f, s_bg.mem + GUARD, W, H);
    face_state_t st = state_at(2026, 9, 24, 10, 8, 37, 55, false, -4 * 3600);

    clock_t t0 = clock();
    for (int k = 0; k < 10; k++) {
        st.moon_phase = 0.1 + 0.01 * k;          /* force a rebuild each time */
        face_draw(&f, &c, &st);
    }
    clock_t t1 = clock();
    for (int k = 0; k < 200; k++) {
        st.second = k % 60;
        face_draw(&f, &c, &st);
    }
    clock_t t2 = clock();
    printf("     dial rebuild %.2f ms, a second's redraw %.3f ms on this host\n",
           1000.0 * (double)(t1 - t0) / CLOCKS_PER_SEC / 10.0,
           1000.0 * (double)(t2 - t1) / CLOCKS_PER_SEC / 200.0);
    expect("timing: the per-second draw did not rebuild", f.rebuilds == 10);

    face_set_photo(&f, s_earth);
    t0 = clock();
    for (int k = 0; k < 10; k++) {
        st.moon_phase = 0.2 + 0.01 * k;
        face_draw(&f, &c, &st);
    }
    t1 = clock();
    for (int k = 0; k < 200; k++) {
        st.second = k % 60;
        face_draw(&f, &c, &st);
    }
    t2 = clock();
    printf("     photo dial rebuild %.2f ms, a second's redraw %.3f ms on this host\n",
           1000.0 * (double)(t1 - t0) / CLOCKS_PER_SEC / 10.0,
           1000.0 * (double)(t2 - t1) / CLOCKS_PER_SEC / 200.0);

    face_set_moon_texture(s_moon);
    t0 = clock();
    for (int k = 0; k < 10; k++) {
        st.moon_phase = 0.1 * k;
        face_draw_moon_page(&c, &st);
    }
    t1 = clock();
    face_set_moon_texture(NULL);
    printf("     textured moon page %.2f ms on this host\n",
           1000.0 * (double)(t1 - t0) / CLOCKS_PER_SEC / 10.0);
}

int main(void)
{
    load_photos();
    test_vfont();
    test_bounds();
    test_determinism_and_cache();
    test_responds();
    test_photo();
    test_photo_guard();
    mkdir("renders", 0755);                 /* fine if it is already there */
    test_moon_page();
    test_render();
    test_timing();

    if (failures) {
        printf("FAIL test_face: %d failure(s)\n", failures);
        return 1;
    }
    printf("PASS test_face\n");
    return 0;
}
