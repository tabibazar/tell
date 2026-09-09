#include "settings.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

int main(void)
{
    settings_t s;
    settings_defaults(&s);
    expect("default saver is ten minutes", s.saver_min == 10);
    expect("default saver cycles the pages", s.saver_cycle);
    expect("default dwell is twenty seconds", s.dwell_s == 20);

    for (int r = 0; r < SETTINGS_ROWS; r++) {
        const settings_row_t *row = settings_row(r);
        expect("row has a label", row != NULL && row->label && row->label[0]);
        expect("row has a sane number of choices",
               row->count >= 2 && row->count <= SETTINGS_MAX_CHOICES);
        for (int i = 0; i < row->count; i++)
            expect("choice is named", row->choices[i] && row->choices[i][0]);
        int cur = settings_choice(&s, r);
        expect("current choice is in range", cur >= 0 && cur < row->count);
    }
    expect("past the last row is NULL", settings_row(SETTINGS_ROWS) == NULL);
    expect("before the first row is NULL", settings_row(-1) == NULL);

    /* The defaults must be reachable from the table, or the page would show
       nothing lit. */
    expect("default delay names 10m",
           strcmp(settings_row(0)->choices[settings_choice(&s, 0)], "10m") == 0);
    expect("default style names cycling",
           strcmp(settings_row(1)->choices[settings_choice(&s, 1)], "cycling pages") == 0);
    expect("default dwell names 20",
           strcmp(settings_row(2)->choices[settings_choice(&s, 2)], "20") == 0);

    expect("selecting the current choice changes nothing",
           !settings_select(&s, 0, settings_choice(&s, 0)));
    expect("selecting 1m changes the delay",
           settings_select(&s, 0, 0) && s.saver_min == 1 && settings_choice(&s, 0) == 0);
    expect("selecting never zeroes the delay",
           settings_select(&s, 0, settings_row(0)->count - 1) && s.saver_min == 0);
    expect("never reads back as the last choice",
           settings_choice(&s, 0) == settings_row(0)->count - 1);
    expect("selecting the drifting clock",
           settings_select(&s, 1, 1) && !s.saver_cycle && settings_choice(&s, 1) == 1);
    expect("selecting sixty seconds a page",
           settings_select(&s, 2, 3) && s.dwell_s == 60 && settings_choice(&s, 2) == 3);

    expect("default is to jump to the live page", s.auto_now && settings_choice(&s, 3) == 0);
    expect("selecting stay put",
           settings_select(&s, 3, 1) && !s.auto_now && settings_choice(&s, 3) == 1);

    expect("default home is the clock", !s.home_now && settings_choice(&s, 4) == 0);
    expect("selecting today, live as home",
           settings_select(&s, 4, 1) && s.home_now && settings_choice(&s, 4) == 1);

    settings_t before = s;
    expect("an out-of-range row is ignored", !settings_select(&s, 7, 0));
    expect("an out-of-range choice is ignored", !settings_select(&s, 0, 99));
    expect("and nothing moved", memcmp(&before, &s, sizeof s) == 0);

    settings_load(&s);
    expect("host load gives the defaults", s.saver_min == 10 && s.saver_cycle);
    expect("host save succeeds", settings_save(&s));

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
