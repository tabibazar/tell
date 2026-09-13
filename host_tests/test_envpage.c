#include "envpage.h"
#include "palette.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

#define W 320
#define H 172

int main(void)
{
    envchart_t day, month;

    envchart_reset(&day);
    int16_t lo, hi;
    expect("an empty chart has no range", !envchart_range(&day, &lo, &hi));
    expect("and no columns", day.count == 0);

    /*
     * A column stands for every reading that fell in it. This is the whole
     * reason the chart bins rather than samples: thirty days is 270 readings
     * per column, and picking one of them would let a cold night vanish
     * depending on where the picking landed.
     */
    envchart_add(&day, 5, 100);
    envchart_add(&day, 5, 400);
    envchart_add(&day, 5, 250);
    expect("a column keeps the lowest and the highest",
           day.lo[5] == 100 && day.hi[5] == 400);
    expect("and counts as one column", day.count == 1);
    expect("the column is marked used", envchart_used(&day, 5));
    expect("its neighbours are not", !envchart_used(&day, 4) && !envchart_used(&day, 6));

    envchart_add(&day, 9, -50);
    expect("the range spans every used column",
           envchart_range(&day, &lo, &hi) && lo == -50 && hi == 400);

    /* Columns off the end are dropped, so callers may compute a column
       arithmetically without bounds-checking every one. */
    envchart_add(&day, -1, 9999);
    envchart_add(&day, ENVCHART_COLS, 9999);
    envchart_add(&day, ENVCHART_COLS + 500, 9999);
    expect("out-of-range columns are ignored",
           envchart_range(&day, &lo, &hi) && hi == 400 && day.count == 2);

    /* Negative readings: this could end up in a shed in February. */
    envchart_reset(&day);
    envchart_add(&day, 0, -2500);
    envchart_add(&day, 1, -1000);
    expect("freezing readings chart", envchart_range(&day, &lo, &hi)
           && lo == -2500 && hi == -1000);

    /*
     * Drawing. The guard words are the assertion that matters: a chart scales
     * data into pixels, and one bad reading is exactly how that walks off the
     * end of the framebuffer.
     */
    {
        static uint16_t guarded[8 + W * H + 8];
        canvas_t c;
        int intact = 1;

        int16_t cases[5] = { 0, 1, 32767, -32768, 2600 };
        for (int k = 0; k < 5; k++) {
            memset(guarded, 0xAB, sizeof guarded);
            canvas_init(&c, guarded + 8, W, H, 1);
            envchart_reset(&day); envchart_reset(&month);
            for (int i = 0; i < ENVCHART_COLS; i++) {
                envchart_add(&day, i, (int16_t)(cases[k] - i));
                envchart_add(&month, i, (int16_t)(cases[k] + i));
            }
            envpage_draw(&c, "TEMPERATURE", "26.1C", "24h 23.8-27.1 30d 18-29",
                         PAL_A1, &day, &month);
            for (int i = 0; i < 8; i++)
                if (guarded[i] != 0xABAB || guarded[8 + W * H + i] != 0xABAB) intact = 0;
        }
        expect("the page never draws outside the framebuffer", intact);

        /* An empty page must not draw a chart, and must not crash. */
        memset(guarded, 0xAB, sizeof guarded);
        canvas_init(&c, guarded + 8, W, H, 1);
        envchart_reset(&day); envchart_reset(&month);
        envpage_draw(&c, "PRESSURE", "--", "waiting", PAL_A2, &day, &month);
        intact = 1;
        for (int i = 0; i < 8; i++)
            if (guarded[i] != 0xABAB || guarded[8 + W * H + i] != 0xABAB) intact = 0;
        expect("an empty page is safe too", intact);

        /* Both charts are drawn, in their own colours, in their own bands. */
        memset(guarded, 0, sizeof guarded);
        canvas_init(&c, guarded + 8, W, H, 1);
        envchart_reset(&day); envchart_reset(&month);
        for (int i = 0; i < ENVCHART_COLS; i++) {
            envchart_add(&day, i, (int16_t)(2400 + (i % 40) * 10));
            envchart_add(&month, i, (int16_t)(2000 + (i % 90) * 10));
        }
        envpage_draw(&c, "TEMPERATURE", "26.1C", "24h 23.8-27.1 30d 18-29",
                     PAL_A1, &day, &month);

        int recent = 0, longer = 0;
        for (int i = 0; i < W * H; i++) {
            if (c.fb[i] == PAL_A1) recent++;
            if (c.fb[i] == pal_darken(PAL_A1)) longer++;
        }
        expect("the recent chart is drawn", recent > 300);
        expect("and the long one below it", longer > 100);

        /* The month strip must stay in its own rows: if it bled into the day
           chart the two would be unreadable, and the day chart is the one
           anyone actually watches. */
        int long_above = 0;
        for (int y = 0; y < 5 * 24 - 1; y++)
            for (int x = 0; x < W; x++)
                if (c.fb[y * W + x] == pal_darken(PAL_A1)) long_above++;
        expect("the month strip stays out of the day chart", long_above == 0);

        /* And nothing is drawn over the title bar's text. */
        int title_row = 0;
        for (int x = 0; x < W; x++)
            if (c.fb[0 * W + x] == PAL_A1 || c.fb[0 * W + x] == pal_darken(PAL_A1))
                title_row++;
        expect("no chart is drawn into the title bar", title_row == 0);
    }

    /* A panel too short for the layout must be refused, not drawn on badly. */
    {
        static uint16_t small[8 + 64 * 30 + 8];
        canvas_t c;
        memset(small, 0xAB, sizeof small);
        canvas_init(&c, small + 8, 64, 30, 1);
        envchart_reset(&day); envchart_reset(&month);
        for (int i = 0; i < 20; i++) envchart_add(&day, i, (int16_t)i);
        envpage_draw(&c, "T", "1", "f", PAL_A1, &day, &month);
        int intact = 1;
        for (int i = 0; i < 8; i++)
            if (small[i] != 0xABAB || small[8 + 64 * 30 + i] != 0xABAB) intact = 0;
        expect("a panel too short is left alone, not overrun", intact);
    }

    /*
     * The week: seven days, each a bar from its low to its high.
     */
    {
        envweek_t w;
        int16_t lo, hi;

        envweek_reset(&w);
        expect("an empty week has no range", !envweek_range(&w, &lo, &hi));
        expect("and no day is used", !envweek_used(&w, 0) && !envweek_used(&w, 6));

        envweek_add(&w, 3, 2200);
        envweek_add(&w, 3, 2700);
        envweek_add(&w, 3, 2450);
        expect("a day keeps its lowest and highest", w.lo[3] == 2200 && w.hi[3] == 2700);
        expect("that day is used", envweek_used(&w, 3));
        expect("its neighbours are not", !envweek_used(&w, 2) && !envweek_used(&w, 4));

        envweek_add(&w, 0, 1800);
        expect("the range spans the week", envweek_range(&w, &lo, &hi)
               && lo == 1800 && hi == 2700);

        envweek_add(&w, -1, 9999);
        envweek_add(&w, ENVWEEK_DAYS, 9999);
        expect("days off the end are ignored",
               envweek_range(&w, &lo, &hi) && hi == 2700);

        /*
         * The bars share one scale. This is the opposite of every other chart
         * here and it is the whole point: seven days each scaled to itself
         * would draw seven identical bars, which answers nothing.
         */
        {
            static uint16_t guarded[8 + W * H + 8];
            canvas_t c;
            memset(guarded, 0, sizeof guarded);
            canvas_init(&c, guarded + 8, W, H, 1);

            envweek_reset(&w);
            for (int i = 0; i < ENVWEEK_DAYS; i++) {
                envweek_add(&w, i, (int16_t)(2000 + i * 100));
                envweek_add(&w, i, (int16_t)(2100 + i * 100));
                envweek_label(&w, i, "SMTWTFS"[i]);
            }
            envweek_draw(&c, "WEEK TEMP", "21.4-27.8", PAL_A1, &w);

            /* Each day's bar must sit at a different height, or the shared
               scale is not being applied. */
            int tops[ENVWEEK_DAYS];
            int slot = W / ENVWEEK_DAYS;
            for (int i = 0; i < ENVWEEK_DAYS; i++) {
                tops[i] = H;
                for (int y = 0; y < H; y++)
                    for (int x = i * slot; x < (i + 1) * slot; x++)
                        if (c.fb[y * W + x] == PAL_A1 && y < tops[i]) tops[i] = y;
            }
            int rising = 1;
            for (int i = 1; i < ENVWEEK_DAYS; i++) if (tops[i] >= tops[i - 1]) rising = 0;
            expect("a warmer day draws a higher bar", rising);
            expect("every day is drawn", tops[0] < H && tops[ENVWEEK_DAYS - 1] < H);

            /* Labels go on their own row, under the bars, never over them. */
            int label_row_bars = 0;
            for (int y = (H / 24 - 1) * 24; y < H; y++)
                for (int x = 0; x < W; x++)
                    if (c.fb[y * W + x] == PAL_A1) label_row_bars++;
            expect("no bar reaches into the row of day letters", label_row_bars == 0);
        }

        /* A missing day must read as missing rather than as zero: a board
           switched on yesterday has five blank days, and drawing those as
           floor-height bars would invent a very cold week. */
        {
            static uint16_t guarded[8 + W * H + 8];
            canvas_t c;
            memset(guarded, 0, sizeof guarded);
            canvas_init(&c, guarded + 8, W, H, 1);
            envweek_reset(&w);
            envweek_add(&w, 5, 2400);
            envweek_add(&w, 6, 2600);
            envweek_draw(&c, "WEEK", "24-26", PAL_A1, &w);
            int bars = 0;
            for (int i = 0; i < W * H; i++) if (c.fb[i] == PAL_A1) bars++;
            int gaps = 0;
            for (int i = 0; i < W * H; i++) if (c.fb[i] == PAL_DIM) gaps++;
            expect("the days with readings are drawn", bars > 20);
            expect("and the empty ones are marked as empty, not as zero", gaps > 10);
        }

        /* One day only: low equals high, and a zero-height bar would be
           invisible. It must still show something. */
        {
            static uint16_t guarded[8 + W * H + 8];
            canvas_t c;
            memset(guarded, 0, sizeof guarded);
            canvas_init(&c, guarded + 8, W, H, 1);
            envweek_reset(&w);
            envweek_add(&w, 6, 2400);
            envweek_draw(&c, "WEEK", "24", PAL_A1, &w);
            int bars = 0;
            for (int i = 0; i < W * H; i++) if (c.fb[i] == PAL_A1) bars++;
            expect("a single flat day is still visible", bars > 0);
        }

        /* And the framebuffer stays intact for anything. */
        {
            static uint16_t guarded[8 + W * H + 8];
            canvas_t c;
            int intact = 1;
            int16_t cases[4] = { 32767, -32768, 0, 2500 };
            for (int k = 0; k < 4; k++) {
                memset(guarded, 0xAB, sizeof guarded);
                canvas_init(&c, guarded + 8, W, H, 1);
                envweek_reset(&w);
                for (int i = 0; i < ENVWEEK_DAYS; i++) {
                    envweek_add(&w, i, cases[k]);
                    envweek_add(&w, i, (int16_t)(cases[k] / 2));
                }
                envweek_draw(&c, "WEEK", "x", PAL_A1, &w);
                for (int i = 0; i < 8; i++)
                    if (guarded[i] != 0xABAB || guarded[8 + W * H + i] != 0xABAB) intact = 0;
            }
            expect("the week never draws outside the framebuffer", intact);
        }
    }

    printf("%s\n", failures ? "FAILURES" : "all tests passed");
    return failures ? 1 : 0;
}
