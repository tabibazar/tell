#include "usagedata.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *what, int cond)
{
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what);
    failures++;
}

static usagedata_t d;
static ud_view_t v;

/* Every assertion below reads the merged view, which is what the charts draw. */
static ud_kind_t parse(const char *payload)
{
    ud_kind_t k = usagedata_parse(&d, payload, 0);
    usagedata_merge(&d, &v);
    return k;
}

int main(void)
{
    memset(&d, 0, sizeof d);
    usagedata_merge(&d, &v);

    expect("plain text is not data", parse("hello world") == UD_NONE);
    expect("unknown marker is not data", parse("!nope\nx 1") == UD_NONE);
    expect("NULL is not data", parse(NULL) == UD_NONE);

    expect("stats marker recognised",
           parse("!stats\nm opus-5 13000000 3300000000\n"
                               "m sonnet-5 1200000 429000000\n") == UD_STATS);
    expect("two models parsed", v.model_count == 2);
    expect("model name kept", strcmp(v.models[0].name, "opus-5") == 0);
    expect("output tokens parsed", v.models[0].out == 13000000ULL);
    expect("cache tokens exceed 32 bits", v.models[0].cread == 3300000000ULL);

    expect("daily marker recognised",
           parse("!daily\nd 09-01 571000000\nd 09-02 282000000\n") == UD_DAILY);
    expect("two days parsed", v.day_count == 2);
    expect("day label kept", strcmp(v.days[0].label, "09-01") == 0);
    expect("day tokens parsed", v.days[1].tokens == 282000000ULL);
    expect("stats survived the daily parse", v.model_count == 2);

    parse("!stats\nm a 1 2\ngarbage line\nm b 3 4\n");
    expect("a bad line does not lose good rows", v.model_count == 2);

    parse("!stats\nm only-name\n");
    expect("a row missing fields is skipped", v.model_count == 0);

    parse("!stats\nm trunc 5");
    expect("a payload truncated mid-line still parses", v.model_count == 1);

    char many[1024] = "!stats\n";
    for (int i = 0; i < UD_MAX_MODELS + 4; i++) {
        char row[40];
        snprintf(row, sizeof row, "m mdl%d %d %d\n", i, i, i);
        strncat(many, row, sizeof many - strlen(many) - 1);
    }
    parse(many);
    expect("model count is capped", v.model_count == UD_MAX_MODELS);

    parse("!stats\nm verylongmodelnamethatoverflows 1 2\n");
    expect("long name is truncated, not overflowed",
           strlen(v.models[0].name) <= UD_NAME_MAX);

    parse("!stats\n");
    expect("empty section yields no rows", v.model_count == 0);

    expect("clock marker recognised",
           parse("!clock\ndate Sunday 06 September 2026\n"
                               "wx Moderate rain  18C feels 19C\n") == UD_CLOCK);
    expect("date kept verbatim",
           strcmp(d.date, "Sunday 06 September 2026") == 0);
    expect("weather keeps its spaces",
           strcmp(d.weather, "Moderate rain  18C feels 19C") == 0);

    parse("!clock\ndate only a date\n");
    expect("a clock payload replaces both fields",
           strcmp(d.date, "only a date") == 0 && d.weather[0] == '\0');

    {
        char longp[256] = "!clock\nwx ";
        for (int i = 0; i < 100; i++) strncat(longp, "y", sizeof longp - strlen(longp) - 1);
        parse(longp);
        expect("long weather is truncated, not overflowed",
               strlen(d.weather) == UD_TEXT_MAX);
    }

    parse("!stats\nm a 1 2\n");
    expect("stats still parse after clock", v.model_count == 1);

    /* Two machines: each replaces only its own share, and the view sums. */
    memset(&d, 0, sizeof d);
    parse("!stats\nhost air\nm opus-5 100 1000\n");
    parse("!stats\nhost studio\nm opus-5 50 500\nm haiku 1 2\n");
    expect("two machines counted", v.host_count == 2);
    expect("shared model summed",
           v.models[0].out == 150 && v.models[0].cread == 1500);
    expect("model only one machine has survives", v.model_count == 2);

    parse("!stats\nhost air\nm opus-5 200 2000\n");
    expect("a re-push replaces only its own machine",
           v.models[0].out == 250 && v.models[0].cread == 2500);
    expect("the other machine is untouched", v.model_count == 2);

    memset(&d, 0, sizeof d);
    parse("!daily\nhost air\nd 09-02 5\nd 09-01 10\n");
    parse("!daily\nhost studio\nd 09-01 7\n");
    expect("same day summed across machines", v.days[0].tokens == 17);
    expect("days sorted oldest first",
           strcmp(v.days[0].label, "09-01") == 0
           && strcmp(v.days[1].label, "09-02") == 0);

    memset(&d, 0, sizeof d);
    parse("!stats\nm opus-5 1 2\n");
    expect("a payload with no host still works", v.model_count == 1);
    expect("and counts as one machine", v.host_count == 1);

    memset(&d, 0, sizeof d);
    for (int i = 0; i < UD_MAX_HOSTS + 2; i++) {
        char p2[64];
        snprintf(p2, sizeof p2, "!stats\nhost h%d\nm m%d 1 1\n", i, i);
        parse(p2);
    }
    expect("host count is capped", v.host_count <= UD_MAX_HOSTS);

    memset(&d, 0, sizeof d);
    parse("!stats\nhost air\nm opus-5 5 5\n");
    parse("!clock\ndate Monday\n");
    expect("a clock payload leaves stats alone", v.model_count == 1);

    memset(&d, 0, sizeof d);
    expect("today marker recognised",
           parse("!today\nmoon 0.85 Waning crescent\n"
                 "sun rise 06:48   set 19:42\n"
                 "day day 250 of 365\n"
                 "hol Thanksgiving in 35 days\n") == UD_TODAY);
    expect("moon phase parsed", d.moon_phase > 0.84f && d.moon_phase < 0.86f);
    expect("moon name follows the number",
           strcmp(d.moon_name, "Waning crescent") == 0);
    expect("sun line kept", strcmp(d.sun, "rise 06:48   set 19:42") == 0);
    expect("day line kept", strcmp(d.dayinfo, "day 250 of 365") == 0);
    expect("holiday kept", strcmp(d.holiday, "Thanksgiving in 35 days") == 0);

    parse("!today\nsun only this\n");
    expect("a today payload replaces all its fields",
           d.moon_name[0] == '\0' && d.dayinfo[0] == '\0');

    /* A machine that has sent nothing must not become a choice. */
    memset(&d, 0, sizeof d);
    parse("!stats\nhost air\n");
    expect("a machine that sent no rows does not count",
           usagedata_hosts(&d) == 0);

    /* Each machine's share is kept alongside the total, which is what lets a
       bar be drawn stacked. */
    memset(&d, 0, sizeof d);
    parse("!stats\nhost air\nm opus-5 100 900\n");
    parse("!stats\nhost studio\nm opus-5 50 450\n");
    expect("total is the sum", v.models[0].out + v.models[0].cread == 1500);
    expect("first machine's share kept", v.model_by_host[0][0] == 1000);
    expect("second machine's share kept", v.model_by_host[0][1] == 500);
    expect("shares add up to the total",
           v.model_by_host[0][0] + v.model_by_host[0][1] == 1500);
    expect("machines are named in order",
           strcmp(v.host_names[0], "air") == 0
           && strcmp(v.host_names[1], "studio") == 0);

    memset(&d, 0, sizeof d);
    parse("!daily\nhost air\nd 09-02 60\nd 09-01 10\n");
    parse("!daily\nhost studio\nd 09-01 30\n");
    expect("day shares survive the sort",
           v.day_by_host[0][0] == 10 && v.day_by_host[0][1] == 30);
    expect("and match their day", v.days[0].tokens == 40);

    /* Freshness: the view reports the newest machine's update time, which the
       title turns into "updated Nm ago". */
    memset(&d, 0, sizeof d);
    usagedata_parse(&d, "!stats\nhost air\nm opus-5 1 2\n", 5000000);
    usagedata_merge(&d, &v);
    expect("update time recorded", v.updated_us == 5000000);

    usagedata_parse(&d, "!stats\nhost studio\nm opus-5 1 2\n", 9000000);
    usagedata_merge(&d, &v);
    expect("the newest machine wins", v.updated_us == 9000000);

    usagedata_parse(&d, "!stats\nhost air\nm opus-5 3 4\n", 12000000);
    usagedata_merge(&d, &v);
    expect("a later push from either machine updates it",
           v.updated_us == 12000000);

    memset(&d, 0, sizeof d);
    usagedata_merge(&d, &v);
    expect("no data means no update time", v.updated_us == 0);

    memset(&d, 0, sizeof d);
    usagedata_parse(&d, "!clock\ndate Monday\n", 7000000);
    usagedata_merge(&d, &v);
    expect("a clock payload is not chart data", v.updated_us == 0);

    /* The optional weekday, which the chart colours by. */
    memset(&d, 0, sizeof d);
    parse("!daily\nhost air\nd 09-07 100 0\nd 09-06 50 6\n");
    expect("weekday parsed", v.days[1].dow == 0 && v.days[0].dow == 6);

    parse("!daily\nhost air\nd 09-07 100\n");
    expect("a day without a weekday is marked unknown", v.days[0].dow == -1);

    parse("!daily\nhost air\nd 09-07 100 9\n");
    expect("an out-of-range weekday is rejected", v.days[0].dow == -1);

    /* The year page. Grid characters decode on a half-octave log scale. */
    expect("dot is no tokens", usagedata_grid_value('.') == 0);
    expect("zero char is a thousand", usagedata_grid_value('0') == 1000);
    expect("two steps double", usagedata_grid_value('2') == 2000);
    expect("odd step is root two",
           usagedata_grid_value('1') >= 1400 && usagedata_grid_value('1') <= 1415);
    expect("letters continue the scale", usagedata_grid_value('A') == 32000);
    expect("lower case continues too",
           usagedata_grid_value('a') == 1000ULL << 18);
    expect("unknown char is nothing", usagedata_grid_value('?') == 0);

    memset(&d, 0, sizeof d);
    expect("year marker recognised",
           parse("!year\nhost air\nstart 20338\ntoday 20344\nfirst 20300\n"
                 "grid 5...5.5\nsessions 12\nlongest 3600\nfav opus-5\n"
                 "tok 1 2 3 4\n") == UD_YEAR);
    expect("year present", v.year.present);
    expect("window is the sender's", v.year.start == 20338 && v.year.len == 7);
    expect("active days counted", v.year.active_days == 3);
    expect("headline figures kept",
           v.year.sessions == 12 && v.year.longest_secs == 3600
           && strcmp(v.year.fav, "opus-5") == 0
           && v.year.tok[0] == 1 && v.year.tok[3] == 4);
    expect("span is capped at the window", v.year.span_days == 7);
    expect("equal days share the top level",
           v.year.level[0] == 4 && v.year.level[4] == 4 && v.year.level[6] == 4);
    expect("empty days are level zero", v.year.level[1] == 0);
    expect("longest streak", v.year.longest_streak == 1);
    expect("current streak ends today", v.year.current_streak == 1);
    expect("peak is the latest of equals", v.year.peak_index == 6);
    expect("a year payload counts as a machine", v.host_count == 1);
    expect("and leaves the models alone", v.model_count == 0);

    /* Four distinct days rank into four levels. */
    memset(&d, 0, sizeof d);
    parse("!year\nstart 0\ntoday 6\ngrid 0.2.4.6\n");
    expect("quartiles give four levels",
           v.year.level[0] == 1 && v.year.level[2] == 2
           && v.year.level[4] == 3 && v.year.level[6] == 4);

    /* Eight days: two per level. */
    memset(&d, 0, sizeof d);
    parse("!year\nstart 0\ntoday 7\ngrid 01234567\n");
    expect("eight days split two per level",
           v.year.level[0] == 1 && v.year.level[1] == 1
           && v.year.level[2] == 2 && v.year.level[3] == 2
           && v.year.level[4] == 3 && v.year.level[5] == 3
           && v.year.level[6] == 4 && v.year.level[7] == 4);

    /* Streaks. */
    memset(&d, 0, sizeof d);
    parse("!year\nstart 0\ntoday 6\ngrid 11.111.\n");
    expect("longest streak spans the run", v.year.longest_streak == 3);
    expect("a quiet today does not end the current streak",
           v.year.current_streak == 3);
    parse("!year\nstart 0\ntoday 6\ngrid 111....\n");
    expect("two quiet days end it", v.year.current_streak == 0);

    /* Two machines with windows sent on different days line up by date. */
    memset(&d, 0, sizeof d);
    parse("!year\nhost air\nstart 0\ntoday 13\nfirst 3\n"
          "grid 5.............\nsessions 2\nlongest 10\nfav opus-5\n"
          "tok 10 10 10 10\n");
    parse("!year\nhost studio\nstart 7\ntoday 13\nfirst 9\n"
          "grid .....5.\nsessions 3\nlongest 20\nfav haiku\ntok 1 1 1 1\n");
    expect("window is the anchor's", v.year.start == 0 && v.year.len == 14);
    expect("the other machine's day lands on its date",
           v.year.level[12] > 0 && v.year.level[0] > 0 && v.year.active_days == 2);
    expect("sessions sum", v.year.sessions == 5);
    expect("longest session is the max", v.year.longest_secs == 20);
    expect("tokens sum", v.year.tok[2] == 11);
    expect("favourite comes from the bigger machine",
           strcmp(v.year.fav, "opus-5") == 0);
    expect("span runs from the earliest first day", v.year.span_days == 11);

    /* The newer window wins; days before it fall off. */
    memset(&d, 0, sizeof d);
    parse("!year\nhost air\nstart 0\ntoday 6\ngrid 5......\n");
    parse("!year\nhost studio\nstart 7\ntoday 13\ngrid ......5\n");
    expect("newest today sets the window", v.year.start == 7);
    expect("days before the window are dropped", v.year.active_days == 1);

    /* Robustness. */
    memset(&d, 0, sizeof d);
    parse("!year\nstart 10\ntoday 5\ngrid 5\n");
    expect("a backwards window is rejected", !v.year.present);
    parse("!year\nstart 0\ntoday 1000\ngrid 5\n");
    expect("an oversized window is rejected", !v.year.present);
    parse("!year\nstart 0\ntoday 6\ngrid 5\n");
    expect("a short grid is padded with nothing",
           v.year.present && v.year.active_days == 1);
    parse("!year\nstart 0\ntoday 2\ngrid 5555555555\n");
    expect("a long grid is clipped to the window",
           v.year.present && v.year.active_days == 3);
    parse("!year\n");
    expect("an empty year payload is not drawn", !v.year.present);

    memset(&d, 0, sizeof d);
    parse("!stats\nhost air\nm opus-5 1 2\n");
    parse("!year\nhost air\nstart 1\ntoday 1\ngrid 5\n");
    expect("a year payload leaves the same machine's models alone",
           v.model_count == 1 && v.year.present);
    expect("and does not double count the machine", v.host_count == 1);

    /* Model rows with the optional tail: input, cache write, calls, days. */
    memset(&d, 0, sizeof d);
    parse("!stats\nhost air\nstart 100\ntoday 106\n"
          "m opus-5 10 20 30 40 50 5.5....\n"
          "m haiku 1 2\n");
    expect("extended model fields parsed",
           v.models[0].in == 30 && v.models[0].cwrite == 40
           && v.models[0].calls == 50);
    expect("a short row still parses, with zeros",
           v.model_count == 2 && v.models[1].in == 0 && v.models[1].calls == 0);
    expect("model window taken from the payload",
           v.mstart == 100 && v.mtoday == 106 && v.mlen == 7);
    expect("model days decoded onto the window",
           v.model_day[0][0] > 5000 && v.model_day[0][1] == 0
           && v.model_day[0][2] > 5000 && v.model_day[0][3] == 0);
    expect("a model without a grid has empty days", v.model_day[1][0] == 0);

    /* Two machines: rows sum, and days line up by date. */
    parse("!stats\nhost studio\nstart 103\ntoday 106\n"
          "m opus-5 1 1 1 1 1 ...5\n");
    expect("extended fields sum across machines",
           v.models[0].in == 31 && v.models[0].calls == 51);
    expect("the other machine's day lands on its date",
           v.model_day[0][6] > 5000 && v.model_day[0][3] == 0);
    expect("window stays the newest", v.mstart == 100 && v.mlen == 7);

    /* Sorting moves the days with their model. */
    memset(&d, 0, sizeof d);
    parse("!stats\nstart 100\ntoday 101\n"
          "m small 1 1 0 0 0 5.\n"
          "m big 100 100 0 0 0 .5\n");
    expect("models are ranked largest first",
           strcmp(v.models[0].name, "big") == 0);
    expect("days travel with their model when ranked",
           v.model_day[0][1] > 5000 && v.model_day[0][0] == 0
           && v.model_day[1][0] > 5000);

    /* An old client sends no window: totals still work, no lines. */
    memset(&d, 0, sizeof d);
    parse("!stats\nm opus-5 10 20\n");
    expect("no window means no model days", v.mlen == 0 && v.model_count == 1);

    parse("!stats\nstart 100\ntoday 1000\nm opus-5 10 20 1 1 1 5\n");
    expect("an oversized model window is ignored", v.mlen == 0);

    /* The cost page. */
    memset(&d, 0, sizeof d);
    expect("cost marker recognised",
           parse("!cost\nhost air\nstart 100\ntoday 106\ntotal 93767\n"
                 "last30 39847\nlast7 6280\nc opus-5 37639\nc haiku 182\n"
                 "grid 5.5....\nplan 20000\n") == UD_COST);
    expect("cost present with its window",
           v.cost.present && v.cost.start == 100 && v.cost.len == 7);
    expect("cost totals kept",
           v.cost.total == 93767 && v.cost.last30 == 39847 && v.cost.last7 == 6280
           && v.cost.plan == 20000);
    expect("cost models ranked",
           v.cost.model_count == 2 && strcmp(v.cost.models[0].name, "opus-5") == 0);
    expect("cost days decoded", v.cost.day[0] > 5000 && v.cost.day[1] == 0);
    expect("a cost payload counts as a machine", v.host_count == 1);

    parse("!cost\nhost studio\nstart 103\ntoday 106\ntotal 1000\n"
          "c haiku 18\nc sonnet-5 500\ngrid ...5\n");
    expect("costs sum across machines", v.cost.total == 94767);
    expect("plan is not summed", v.cost.plan == 20000);
    expect("shared model summed and a new one added",
           v.cost.model_count == 3 && v.cost.models[1].cents == 500
           && v.cost.models[2].cents == 200);
    expect("cost days line up by date", v.cost.day[6] > 5000);

    parse("!cost\nhost studio\nstart 100\ntoday 0\n");
    expect("a cost payload without a date is not drawn but the other machine's is",
           v.cost.present && v.cost.total == 93767);

    memset(&d, 0, sizeof d);
    parse("!year\nstart 1\ntoday 3\ngrid 555\nestimated 35\n");
    expect("estimated day count kept", v.year.estimated == 35);

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
