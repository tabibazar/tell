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

    expect("particles marker recognised", parse("!particles\n") == UD_PARTICLES);
    expect("an unknown marker is still nothing", parse("!nope\n") == UD_NONE);

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

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
