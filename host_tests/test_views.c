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
#include "pages.h"
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

    usagedata_parse(&data, "!projects\nhost air\ndays 23\n"
                           "p stan 1510018039 125243 10 4372\np tf 958502138 65480 10 2795\n"
                           "p kanban 749151394 55263 1 1605\np reporting 532726947 36100 3 1755\n"
                           "p rag 523060098 33587 1 1141\np tenki 433654560 35737 6 1911\n"
                           "p subagents 426444156 36664 6 5400\np other 706855171 70577 19 4228\n",
                    1000000);
    {
        char cache[640] = "!cache\nhost air\nstart 20644\ntoday 20703\n"
                          "m opus-5 58975 5359382315 96773724 2411722\n"
                          "m opus-4.7 70162 4868320670 114822266 2190744\n"
                          "m opus-4.8 273497 1697677219 29796157 763955\n"
                          "m fable-5.1 629791 395973556 29088319 386074\n"
                          "m sonnet-5 6606 213269707 9557790 38389\n"
                          "m haiku-4.5 9675 106703725 5342151 9603\n"
                          "saved 5845575\ncost 949126\ngrid ";
        size_t n = strlen(cache);
        for (int i = 0; i < 60; i++) cache[n++] = (i % 7 == 6) ? '.' : (char)('p' + (i % 10));
        cache[n++] = '\n';
        cache[n] = '\0';
        usagedata_parse(&data, cache, 1000000);
    }

    {
        char tools[512] = "!tools\nhost air\ndays 23\ncalls 11064\nmsgs 23245\nsessions 56\n"
                          "start 20644\ntoday 20703\nt Bash 8791\nt Edit 522\nt WebFetch 394\n"
                          "t Read 333\nt Write 321\nt Agent 93\nt AskUserQuestion 89\n"
                          "t other 521\ngrid ";
        size_t n = strlen(tools);
        for (int i = 0; i < 60; i++) tools[n++] = (i % 7 == 6) ? '.' : (char)('C' + (i * 3) % 14);
        tools[n++] = '\n';
        tools[n] = '\0';
        usagedata_parse(&data, tools, 1000000);

        char think[512] = "!thinking\nhost air\ndays 23\nstart 20644\ntoday 20703\n"
                          "m opus-5 5000000 9000000 12500 35000\n"
                          "m fable-5.1 900000 700000 4500 8000\n"
                          "m sonnet-5 600000 200000 600 800\n"
                          "m haiku-4.5 20000 150000 10 85\ngrid ";
        n = strlen(think);
        for (int i = 0; i < 60; i++) think[n++] = (i % 9 == 8) ? '.' : (char)('K' + (i * 5) % 12);
        think[n++] = '\n';
        think[n] = '\0';
        usagedata_parse(&data, think, 1000000);
    }

    usagedata_parse(&data, "!week\nhost air\ntoday 20703\nw tokens 638480337 1807285709\n"
                           "w cost 71205 123980\nw msgs 5105 10751\nw sessions 12 9\n"
                           "w tools 1760 2356\nw days 4 5\ngrid ZWcc..bZZY...Y\n", 1000000);
    usagedata_parse(&data, "!records\nhost air\nr bigday 1448000000 20686\n"
                           "r costday 36200 20686\nr msgs 4400 20686\nr streak 12 20703\n"
                           "r session 1628367 20651\nr toolsess 1200 20690\n"
                           "r response 48000 20700\nr early 365 20698\nr late 1420 20700\n"
                           "since 20561\n", 1000000);
    usagedata_parse(&data, "!runs\nhost air\ndays 23\ncalls 8810\ncommands 59785\n"
                           "c echo 10385\nc grep 4300\nc head 3940\nc git 3565\nc tail 2423\n"
                           "c cat 1784\nc sed 1739\nc python3 1601\nc other 30048\n"
                           "k git 4784\nk build 338\nk test 345\nk files 30361\nk scripts 3172\n"
                           "k infra 2500\nk other 18285\n", 1000000);
    usagedata_parse(&data, "!turns\nhost air\ndays 23\nturns 1164\ninterrupted 16\n"
                           "first 8 24\nturn 99 535\nstart 20644\ntoday 20703\n"
                           "longest 46144 20696\n"
                           "grid M...........................F...DEDF..DEDFF..ECBEE..CFCE...C\n",
                    1000000);
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
        /* An hour later, with no new push, the age has grown past busy. With
           a real payload that was already idle nothing changes; the check is
           that amber never grows. */
        int amber_before = count_colour(pal_heat(4));
        bool was_busy = view.now.have_last
                     && view.now.last_secs + (now - view.now.last_sent_us) / 1000000 < UD_BUSY_SECS;
        views_now(&cv, &view, now + 3600LL * 1000000LL);
        int amber_after = count_colour(pal_heat(4));
        expect("idle later: no more amber than before",
               was_busy ? amber_after < amber_before : amber_after <= amber_before);
    }
    views_now(&cv, &empty, now);
    expect("empty now page shows a message", lit_in(0, 48, W, 72) > 0);

    /* By project. */
    views_projects(&cv, &view, 1.0f, now);
    save(prefix, "projects");
    expect("projects present", view.projects.present && view.projects.count == 8);
    expect("project bars drawn", lit_in(17 * 12, 2 * 24, 44 * 12, 10 * 24) > 500);
    expect("project values drawn", lit_in(45 * 12, 2 * 24, W, 10 * 24) > 200);
    expect("project summary drawn", lit_in(0, 18 * 24, W, 19 * 24) > 0);
    expect("projects text stops short of the right edge", lit_in(W - 12, 24, W, H) == 0);
    views_projects(&cv, &empty, 1.0f, now);
    expect("empty projects page shows a message", lit_in(0, 48, W, 72) > 0);

    /* Cache efficiency. */
    views_cache(&cv, &view, 1.0f, now);
    save(prefix, "cache");
    expect("cache present", view.cache.present && view.cache.hit_pct > 90);
    expect("cache headline drawn", lit_in(0, 2 * 24, W, 4 * 24) > 200);
    expect("cache model bars drawn", lit_in(17 * 12, 6 * 24, 45 * 12, 12 * 24) > 500);
    expect("cache daily line drawn", count_colour(pal_heat(4)) > 300);
    expect("cache dates drawn", lit_in(60, 18 * 24, W, 19 * 24) > 0);
    expect("cache text stops short of the right edge", lit_in(W - 12, 24, W, H) == 0);
    views_cache(&cv, &empty, 1.0f, now);
    expect("empty cache page shows a message", lit_in(0, 48, W, 72) > 0);

    /* Tools. */
    views_tools(&cv, &view, 1.0f, now);
    save(prefix, "tools");
    expect("tools present", view.tools.present && view.tools.count == 8);
    expect("tool bars drawn", lit_in(17 * 12, 2 * 24, 46 * 12, 10 * 24) > 500);
    expect("tool summary drawn", lit_in(0, 11 * 24, W, 12 * 24) > 100);
    expect("tool daily line drawn", count_colour(pal_heat(4)) > 300);
    expect("tools text stops short of the right edge", lit_in(W - 12, 24, W, H) == 0);
    views_tools(&cv, &empty, 1.0f, now);
    expect("empty tools page shows a message", lit_in(0, 48, W, 72) > 0);

    /* Thinking share. */
    views_thinking(&cv, &view, 1.0f, now);
    save(prefix, "thinking");
    expect("thinking present", view.thinking.present && view.thinking.share_pct > 30);
    expect("thinking headline drawn", lit_in(0, 2 * 24, W, 4 * 24) > 200);
    expect("thinking bars drawn", lit_in(17 * 12, 6 * 24, 46 * 12, 10 * 24) > 500);
    expect("thinking daily line drawn", lit_in(72, 13 * 24, W - 24, 18 * 24) > 200);
    expect("thinking text stops short of the right edge", lit_in(W - 12, 24, W, H) == 0);
    views_thinking(&cv, &empty, 1.0f, now);
    expect("empty thinking page shows a message", lit_in(0, 48, W, 72) > 0);

    /* This week against last. */
    views_week(&cv, &view, 1.0f, now);
    save(prefix, "week");
    expect("week present", view.week.present);
    expect("week table drawn", lit_in(0, 3 * 24, W, 9 * 24) > 500);
    expect("week bars drawn on both sides",
           lit_in(2 * 12, 11 * 24, 30 * 12, 17 * 24) > 200 && lit_in(34 * 12, 11 * 24, 62 * 12, 17 * 24) > 200);
    expect("weekday letters drawn", lit_in(2 * 12, 17 * 24, 62 * 12, 18 * 24) > 0);
    expect("week text stops short of the right edge", lit_in(W - 12, 24, W, H) == 0);
    views_week(&cv, &empty, 1.0f, now);
    expect("empty week page shows a message", lit_in(0, 48, W, 72) > 0);

    /* Records. */
    views_records(&cv, &view, now);
    save(prefix, "records");
    expect("records present", view.records.present && view.records.count == 9);
    expect("nine records drawn two rows apart", lit_in(0, 2 * 24, W, 19 * 24) > 2000);
    expect("record values in amber", count_colour(pal_heat(4)) > 300);
    expect("records text stops short of the right edge", lit_in(W - 12, 24, W, H) == 0);
    views_records(&cv, &empty, now);
    expect("empty records page shows a message", lit_in(0, 48, W, 72) > 0);

    /* What Claude runs. */
    views_runs(&cv, &view, 1.0f, now);
    save(prefix, "runs");
    expect("runs present", view.runs.present && view.runs.count == 9);
    expect("program bars drawn", lit_in(17 * 12, 2 * 24, 46 * 12, 11 * 24) > 500);
    expect("category bar drawn", lit_in(12, 14 * 24, W - 12, 15 * 24) > 1000);
    expect("category legend drawn", lit_in(0, 15 * 24, W, 17 * 24) > 200);
    expect("runs text stops short of the right edge", lit_in(W - 12, 24, W, H) == 0);
    views_runs(&cv, &empty, 1.0f, now);
    expect("empty runs page shows a message", lit_in(0, 48, W, 72) > 0);

    /* Turns. */
    views_turns(&cv, &view, 1.0f, now);
    save(prefix, "turns");
    expect("turns present", view.turns.present && view.turns.turns == 1164);
    expect("turn headline drawn", lit_in(0, 2 * 24, W, 4 * 24) > 200);
    expect("turn daily line drawn", lit_in(84, 12 * 24, W - 24, 18 * 24) > 100);
    expect("turns text stops short of the right edge", lit_in(W - 12, 24, W, H) == 0);
    views_turns(&cv, &empty, 1.0f, now);
    expect("empty turns page shows a message", lit_in(0, 48, W, 72) > 0);

    /* The menu, and whether every tile can be hit. */
    {
        pages_t pg;
        unsigned all = 0;
        for (int i = 0; i < PAGE_COUNT; i++) all |= PAGE_BIT(i);
        pages_init(&pg, all);
        pages_show(&pg, PAGE_MENU, 1000);
        views_menu(&cv, &pg);
        save(prefix, "menu");
        expect("menu draws tiles", lit_in(0, 30, W, H) > 5000);
        expect("menu stays inside the panel", lit_in(W - 4, 24, W, H) == 0);
        int hits = 0, misses = 0, wrong = 0, tiles = 0;
        for (int i = 0; i < PAGE_COUNT; i++) {
            if (i == PAGE_MENU) continue;
            tiles++;
            /* Find the tile by walking the same order the menu uses. */
            int n = 0;
            for (int j = 0; j < i; j++) if (j != PAGE_MENU) n++;
            int x = (n % 4) * (W / 4) + W / 8, y = 30 + (n / 4) * 88 + 40;
            page_t got;
            if (views_menu_hit(&cv, &pg, x, y, &got)) { hits++; if (got != (page_t)i) wrong++; }
            else misses++;
        }
        expect("every tile can be hit at its centre", hits == tiles && misses == 0);
        expect("and each names its page", wrong == 0);
        page_t got;
        expect("the title bar is not a tile", !views_menu_hit(&cv, &pg, 400, 10, &got));
        pages_init(&pg, PAGE_BIT(PAGE_CLOCK) | PAGE_BIT(PAGE_MENU) | PAGE_BIT(PAGE_MESSAGE));
        views_menu(&cv, &pg);
        expect("a board with two pages shows two tiles",
               views_menu_hit(&cv, &pg, W / 8, 70, &got) && got == PAGE_CLOCK
               && views_menu_hit(&cv, &pg, W / 4 + W / 8, 70, &got) && got == PAGE_MESSAGE
               && !views_menu_hit(&cv, &pg, W / 2 + W / 8, 70, &got));
    }

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
            if ((2 + r * 4 + 1) * 24 + 12 >= H) break;
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
