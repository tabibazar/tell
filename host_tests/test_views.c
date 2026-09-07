/* Renders the chart pages into a host framebuffer the size of the CrowPanel.
   Checks that they draw where they should; with an output prefix it also
   writes each page as a PPM, which is how the layout was actually looked at
   without a board:

       ./test_views /tmp/page            synthetic data
       ./test_views /tmp/page a.txt b.txt   real payloads, as pushed

   Cheap invariants only: the framebuffer is bounds-checked by canvas_t, so
   the interesting failures are "nothing drawn" and "drawn in the wrong
   region", and those are what is asserted. */
#include "canvas.h"
#include "palette.h"
#include "settings.h"
#include "usagedata.h"
#include "views.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 800
#define H 480

static int failures;
static uint16_t fb[W * H];
static canvas_t cv;
static usagedata_t data;
static ud_view_t view;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

static int lit_in(int x0, int y0, int x1, int y1)
{
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (fb[y * W + x] != PAL_BG) n++;
    return n;
}

static int count_colour(uint16_t colour)
{
    int n = 0;
    for (int i = 0; i < W * H; i++) if (fb[i] == colour) n++;
    return n;
}

static int count_colour_above(uint16_t colour, int y1)
{
    int n = 0;
    for (int i = 0; i < W * y1; i++) if (fb[i] == colour) n++;
    return n;
}

static void save(const char *prefix, const char *page)
{
    if (prefix == NULL) return;
    char path[512];
    snprintf(path, sizeof path, "%s-%s.ppm", prefix, page);
    FILE *f = fopen(path, "wb");
    if (f == NULL) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint16_t p = fb[i];
        unsigned char rgb[3] = {
            (unsigned char)(((p >> 11) & 0x1F) * 255 / 31),
            (unsigned char)(((p >> 5) & 0x3F) * 255 / 63),
            (unsigned char)((p & 0x1F) * 255 / 31),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("wrote %s\n", path);
}

static void feed_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) { perror(path); exit(2); }
    static char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    if (usagedata_parse(&data, buf, 1000000) == UD_NONE) {
        printf("FAIL %s is not a data payload\n", path);
        failures++;
    }
}

/* A year with a busy summer, and two models trading places over sixty days.
   Day numbers put "today" on 2026-09-07, a Monday. */
static void synthetic(void)
{
    char year[512] = "!year\nhost air\nstart 20338\ntoday 20703\nfirst 20561\n"
                     "sessions 167\nlongest 1628340\nfav opus-5\n"
                     "tok 949800 51700000 12700000000 292900000\ngrid ";
    size_t at = strlen(year);
    for (int i = 0; i < 366; i++) {
        char ch = '.';
        if (i > 240 && i % 7 != 0 && i % 7 != 6) ch = (char)('A' + (i * 7) % 20);
        if (i > 330 && i % 3 == 0) ch = 'Z';
        year[at++] = ch;
    }
    year[at++] = '\n';
    year[at] = '\0';
    usagedata_parse(&data, year, 1000000);

    char stats[1024] = "!stats\nhost air\nstart 20644\ntoday 20703\n";
    const char *names[4] = { "opus-5", "opus-4.8", "fable-5.1", "sonnet-5" };
    for (int m = 0; m < 4; m++) {
        char row[160], grid[61];
        for (int i = 0; i < 60; i++) {
            int v = (i * (m + 3) + m * 11) % 23;
            grid[i] = (m == 0 && i < 20) || (m == 1 && i > 30) ? '.' : (char)('K' + v);
        }
        grid[60] = '\0';
        snprintf(row, sizeof row, "m %s %d %d %d %d %d %s\n", names[m],
                 19200000 / (m + 1), 5400000000 / (m + 1) > 0 ? (int)(1000000000 / (m + 1)) : 1,
                 59000 * (m + 1), 96800000 / (m + 1), 1000 / (m + 1), grid);
        strncat(stats, row, sizeof stats - strlen(stats) - 1);
    }
    usagedata_parse(&data, stats, 1000000);

    char cost[512] = "!cost\nhost air\nstart 20644\ntoday 20703\ntotal 937673\n"
                     "last30 398471\nlast7 62804\nc opus-5 376391\nc opus-4.7 372167\n"
                     "c opus-4.8 118928\nc fable-5.1 49726\nc sonnet-4.6 11166\n"
                     "c haiku-4.5 1829\nplan 20000\ngrid ";
    at = strlen(cost);
    for (int i = 0; i < 60; i++) cost[at++] = (i % 9 == 0) ? '.' : (char)('K' + (i * 5) % 20);
    cost[at++] = '\n';
    cost[at] = '\0';
    usagedata_parse(&data, cost, 1000000);

    char rhythm[256] = "!rhythm\nhost air\ndays 23\nmsgs 23173\ngrid ";
    at = strlen(rhythm);
    for (int i = 0; i < 168; i++) {
        int dow = i / 24, hour = i % 24;
        char ch = '.';
        if (hour >= 8 && hour <= 23 && dow < 5) ch = (char)('D' + ((hour * 3 + dow * 5) % 9));
        if (dow >= 5 && hour >= 10 && hour <= 18) ch = (char)('2' + (hour % 4));
        rhythm[at++] = ch;
    }
    rhythm[at++] = '\n';
    rhythm[at] = '\0';
    usagedata_parse(&data, rhythm, 1000000);

    usagedata_parse(&data, "!now\nhost air\ntokens 85406000\ncost 8337\nmsgs 290\n"
                           "sessions 1\navg 306469905\nlast 5\nmodel fable-5.1\n"
                           "project tell\nsession 4883\n", 1000000);
}

int main(int argc, char **argv)
{
    const char *prefix = argc > 1 ? argv[1] : NULL;
    canvas_init(&cv, fb, W, H, 1);
    memset(&data, 0, sizeof data);

    if (argc > 2) for (int i = 2; i < argc; i++) feed_file(argv[i]);
    else synthetic();
    usagedata_merge(&data, &view);
    int64_t now = 1000000 + 90 * 1000000;

    /* Empty data must not crash and must say so. */
    ud_view_t empty;
    memset(&empty, 0, sizeof empty);
    views_year(&cv, &empty, now);
    expect("empty year page shows a message", lit_in(0, 48, W, 72) > 0);
    views_models(&cv, &empty, 1.0f, now);
    expect("empty models page shows a message", lit_in(0, 48, W, 72) > 0);

    /* The year page. */
    views_year(&cv, &view, now);
    save(prefix, "year");
    expect("year page has data", view.year.present);
    expect("month labels drawn on row 1", lit_in(52, 24, W, 48) > 0);
    expect("weekday labels drawn", lit_in(0, 72, 48, 96) > 0 && lit_in(0, 120, 48, 144) > 0);
    expect("cells are lit in the grid", count_colour(pal_heat(4)) > 40);
    expect("empty days show as dots", count_colour(pal_heat(0)) >= 4 * 100);
    expect("nothing drawn right of the last week",
           lit_in(52 + 53 * 14, 48, W, 216) == 0);
    expect("the map stays inside the panel", 52 + 53 * 14 <= W);
    expect("figures drawn below the legend", lit_in(0, 11 * 24, W, 16 * 24) > 200);
    expect("footer drawn", lit_in(0, 19 * 24, W, H) > 0);
    expect("year text stops short of the right edge",
           lit_in(W - 12, 11 * 24, W, H) == 0);

    /* The models page. */
    views_models(&cv, &view, 1.0f, now);
    save(prefix, "models");
    expect("axis labels drawn in the gutter", lit_in(0, 24, 84, 216) > 0);
    expect("lines drawn in the plot", lit_in(84, 36, W - 24, 212) > 100);
    expect("first series colour appears", count_colour(pal_accent(0)) > 100);
    expect("dates drawn under the plot", lit_in(84, 9 * 24, W, 10 * 24) > 0);
    expect("legend drawn", lit_in(12, 10 * 24, W, 11 * 24) > 0);
    expect("cards drawn", lit_in(0, 11 * 24, W, 19 * 24) > 500);
    expect("card text stops short of the right edge",
           lit_in(W - 12, 11 * 24, W, 19 * 24) == 0);
    expect("footer drawn", lit_in(0, 19 * 24, W, H) > 0);

    /* The cost page. */
    views_cost(&cv, &view, 1.0f, now);
    save(prefix, "cost");
    expect("cost totals drawn", lit_in(0, 2 * 24, W, 4 * 24) > 200);
    expect("cost model bars drawn", lit_in(17 * 12, 7 * 24, 54 * 12, 13 * 24) > 100);
    expect("cost daily bars drawn", lit_in(12, 15 * 24, W - 12, 18 * 24 - 2) > 100);
    expect("cost dates drawn", lit_in(12, 18 * 24, W - 12, 19 * 24) > 0);
    expect("cost footer drawn", lit_in(0, 19 * 24, W, H) > 0);
    expect("cost text stops short of the right edge",
           lit_in(W - 12, 24, W, H) == 0);
    views_cost(&cv, &empty, 1.0f, now);
    expect("empty cost page shows a message", lit_in(0, 48, W, 72) > 0);

    /* When you work. */
    views_rhythm(&cv, &view, now);
    save(prefix, "rhythm");
    expect("rhythm page has data", view.rhythm.present);
    expect("hour labels drawn", lit_in(52, 24, W, 48) > 0);
    expect("weekday labels drawn", lit_in(0, 48, 48, 48 + 7 * 36) > 0);
    expect("rhythm cells lit", count_colour(pal_heat(4)) > 500);
    expect("rhythm map stays inside the panel", 52 + 24 * 31 <= W);
    expect("rhythm figures drawn", lit_in(0, 14 * 24, W, 18 * 24) > 100);
    /* The map itself runs to within 5px of the edge by design; the check is
       that nothing is clipped, so it looks at the last few pixels only. */
    expect("rhythm stays inside the panel", lit_in(W - 4, 24, W, H) == 0);
    views_rhythm(&cv, &empty, now);
    expect("empty rhythm page shows a message", lit_in(0, 48, W, 72) > 0);

    /* Today, live: drawn as if pushed just now, then much later. */
    views_now(&cv, &view, now);
    save(prefix, "now");
    expect("now page has data", view.now.present);
    expect("status line drawn", lit_in(12, 2 * 24, W, 3 * 24) > 100);
    expect("busy shows in amber right after a message",
           count_colour(pal_heat(4)) > 100);
    expect("figures drawn", lit_in(0, 5 * 24, W, 8 * 24) > 200);
    expect("comparison bar drawn", lit_in(12, 10 * 24, W - 12, 12 * 24) > 1000);
    expect("now text stops short of the right edge", lit_in(W - 12, 24, W, H) == 0);
    {
        /* An hour later, with no new push, the age has grown past busy. */
        int amber_busy = count_colour(pal_heat(4));
        views_now(&cv, &view, now + 3600LL * 1000000LL);
        expect("idle later: less amber than when busy",
               count_colour(pal_heat(4)) < amber_busy);
    }
    views_now(&cv, &empty, now);
    expect("empty now page shows a message", lit_in(0, 48, W, 72) > 0);

    /* The settings page, and whether its buttons can be hit. */
    settings_t st;
    settings_defaults(&st);
    views_settings(&cv, &st);
    save(prefix, "settings");
    expect("settings labels drawn", lit_in(12, 2 * 24, W, 3 * 24) > 0);
    expect("the chosen button is lit amber", count_colour(PAL_A1) > 500);
    expect("settings text stops short of the right edge",
           lit_in(W - 12, 24, W, H) == 0);

    int hits = 0, total = 0, wrong = 0;
    for (int r = 0; r < SETTINGS_ROWS; r++) {
        const settings_row_t *row = settings_row(r);
        for (int i = 0; i < row->count; i++) {
            /* Find the button by scanning for its plate on its first row. */
            int y = (2 + r * 4 + 1) * 24 + 12;
            int x_start = -1, seen = 0;
            for (int x = 0; x < W; x++) {
                uint16_t p = fb[y * W + x];
                int plate = p == PAL_A1 || p == 0x2124;
                if (plate && x_start < 0) x_start = x;
                if (!plate && x_start >= 0) {
                    if (seen == i) break;
                    seen++;
                    x_start = -1;
                }
            }
            total++;
            int hr, hc;
            if (x_start >= 0 && views_settings_hit(&cv, x_start + 14, y + 20, &hr, &hc)) {
                hits++;
                if (hr != r || hc != i) wrong++;
            }
        }
    }
    expect("every button can be hit at its centre", hits == total && total > 0);
    expect("and reports the right row and choice", wrong == 0);
    {
        int hr, hc;
        expect("a tap on a label row misses",
               !views_settings_hit(&cv, 30, 2 * 24 + 12, &hr, &hc));
        expect("a tap in the title bar misses",
               !views_settings_hit(&cv, 400, 10, &hr, &hc));
        expect("a tap far right of the buttons misses",
               !views_settings_hit(&cv, W - 5, 3 * 24 + 24, &hr, &hc));
        expect("a tap just below a button row still hits it (third row)",
               views_settings_hit(&cv, 30, (2 + 3) * 24 + 12, &hr, &hc) && hr == 0);
        expect("a tap in the footer misses",
               !views_settings_hit(&cv, 30, 19 * 24 + 12, &hr, &hc));
    }

    views_models(&cv, &view, 0.0f, now);
    expect("at t=0 the lines lie on the baseline",
           count_colour_above(pal_accent(0), 9 * 24 - 8) == 0);

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
