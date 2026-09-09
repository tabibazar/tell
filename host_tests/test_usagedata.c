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

    expect("level marker recognised", parse("!level\n") == UD_LEVEL);
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

    parse("!clock\ndate Tuesday 08 September 2026\nutc -240\ntz EDT\nwx Clear 13C\n");
    expect("clock offset and zone parsed",
           d.have_utc && d.utc_offset_min == -240
           && strcmp(d.tz, "EDT") == 0 && strcmp(d.weather, "Clear 13C") == 0);
    parse("!clock\ndate x\nutc 99999\n");
    expect("an absurd offset is ignored", !d.have_utc && d.utc_offset_min == 0);
    parse("!clock\ntz AVeryLongZoneName\n");
    expect("zone name is truncated, not overflowed", strlen(d.tz) <= 7);

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

    /* When you work: a 7x24 grid of messages. */
    memset(&d, 0, sizeof d);
    {
        char grid[UD_RHYTHM_CELLS + 1];
        memset(grid, '.', UD_RHYTHM_CELLS);
        grid[UD_RHYTHM_CELLS] = '\0';
        grid[1 * 24 + 14] = 'A';     /* Tuesday 14:00: 32 messages */
        grid[1 * 24 + 15] = '5';     /* Tuesday 15:00: ~5 */
        grid[6 * 24 + 2] = '0';      /* Sunday 02:00: 1 */
        char payload[300];
        snprintf(payload, sizeof payload, "!rhythm\nhost air\ndays 23\nmsgs 38\ngrid %s\n", grid);
        expect("rhythm marker recognised", parse(payload) == UD_RHYTHM);
    }
    expect("rhythm present", v.rhythm.present && v.rhythm.days == 23 && v.rhythm.msgs == 38);
    expect("rhythm cells decoded to messages",
           v.rhythm.cell[1 * 24 + 14] == 32 && v.rhythm.cell[6 * 24 + 2] == 1
           && v.rhythm.cell[0] == 0);
    expect("rhythm levels ranked",
           v.rhythm.level[1 * 24 + 14] == 4 && v.rhythm.level[6 * 24 + 2] == 1
           && v.rhythm.level[0] == 0);
    expect("peak cell found", v.rhythm.peak_dow == 1 && v.rhythm.peak_hour == 14);
    expect("busiest weekday and hour", v.rhythm.busiest_dow == 1 && v.rhythm.busiest_hour == 14);
    expect("night and weekend shares",
           v.rhythm.night_pct == 3 && v.rhythm.weekend_pct == 3);

    parse("!rhythm\nhost studio\ndays 10\nmsgs 2\ngrid .5\n");
    expect("second machine's cells add in",
           v.rhythm.cell[1] > 0 && v.rhythm.cell[1 * 24 + 14] == 32 && v.rhythm.days == 23);
    expect("rhythm messages sum", v.rhythm.msgs == 40);

    parse("!rhythm\nhost studio\n");
    expect("a rhythm payload without a grid is not drawn for that machine",
           v.rhythm.present && v.rhythm.cell[1] == 0);

    /* Today, live. */
    memset(&d, 0, sizeof d);
    usagedata_parse(&d, "!now\nhost air\ntokens 85406000\ncost 8337\nmsgs 290\n"
                        "sessions 1\navg 306469905\nlast 93\nmodel fable-5.1\n"
                        "project tell\nsession 4883\n", 50000000);
    usagedata_merge(&d, &v);
    expect("now marker parsed",
           v.now.present && v.now.tokens == 85406000ULL && v.now.cost == 8337
           && v.now.msgs == 290 && v.now.sessions == 1 && v.now.avg == 306469905ULL);
    expect("now's last message kept",
           v.now.have_last && v.now.last_secs == 93 && v.now.session_secs == 4883
           && strcmp(v.now.model, "fable-5.1") == 0 && strcmp(v.now.project, "tell") == 0);
    expect("now records when it was sent", v.now.last_sent_us == 50000000);

    usagedata_parse(&d, "!now\nhost studio\ntokens 1000\nmsgs 10\nsessions 2\n"
                        "last 600\nmodel haiku\nproject other\n", 60000000);
    usagedata_merge(&d, &v);
    expect("today sums across machines",
           v.now.tokens == 85407000ULL && v.now.msgs == 300 && v.now.sessions == 3);
    expect("the more recent last message wins after ageing",
           strcmp(v.now.model, "fable-5.1") == 0);

    usagedata_parse(&d, "!now\nhost studio\nlast 5\nmodel haiku\nproject other\n", 70000000);
    usagedata_merge(&d, &v);
    expect("a fresher machine takes over last",
           strcmp(v.now.model, "haiku") == 0 && v.now.last_sent_us == 70000000);

    memset(&d, 0, sizeof d);
    parse("!now\nhost air\ntokens 5\n");
    expect("now without a last message is still present",
           v.now.present && !v.now.have_last);

    /* Percent characters: linear, unlike the token grid. */
    expect("percent zero", usagedata_pct_value('0') == 0);
    expect("percent full", usagedata_pct_value('z') == 100);
    expect("percent midpoint",
           usagedata_pct_value('U') >= 49 && usagedata_pct_value('U') <= 51);
    expect("percent none", usagedata_pct_value('.') == -1);

    /* By project. */
    memset(&d, 0, sizeof d);
    expect("projects marker recognised",
           parse("!projects\nhost air\ndays 23\np stan 1500 125 10 4372\n"
                 "p tf 900 65 10 2795\np other 700 70 19 4228\n") == UD_PROJECTS);
    expect("projects present and ranked",
           v.projects.present && v.projects.count == 3
           && strcmp(v.projects.rows[0].name, "stan") == 0
           && v.projects.rows[0].sessions == 10 && v.projects.rows[0].msgs == 4372);
    expect("project total", v.projects.total_tokens == 3100 && v.projects.days == 23);
    parse("!projects\nhost studio\ndays 30\np tf 1000 10 1 1\np new 5 1 1 1\n");
    expect("projects merge by name across machines",
           v.projects.rows[0].tokens == 1900 && strcmp(v.projects.rows[0].name, "tf") == 0
           && v.projects.count == 4 && v.projects.days == 30);
    parse("!projects\nhost studio\n");
    expect("an empty projects payload drops that machine's rows",
           v.projects.count == 3 && v.projects.rows[0].tokens == 1500);

    /* Cache efficiency. */
    memset(&d, 0, sizeof d);
    expect("cache marker recognised",
           parse("!cache\nhost air\nstart 100\ntoday 106\n"
                 "m opus-5 100 900 0 810\nm haiku 50 50 0 4\n"
                 "saved 814\ncost 200\ngrid zU.0zzz\n") == UD_CACHE);
    expect("cache present with window", v.cache.present && v.cache.len == 7);
    expect("cache models ranked by volume",
           v.cache.model_count == 2 && strcmp(v.cache.models[0].name, "opus-5") == 0
           && v.cache.models[0].saved_cents == 810);
    expect("all-time hit rate", v.cache.hit_pct == 86);   /* 950 of 1100 */
    expect("saved and cost kept", v.cache.saved == 814 && v.cache.cost == 200);
    expect("daily percentages decoded",
           v.cache.pct[0] == 100 && v.cache.pct[1] >= 49 && v.cache.pct[1] <= 51
           && v.cache.pct[2] == -1 && v.cache.pct[3] == 0);
    parse("!cache\nhost studio\nstart 103\ntoday 106\nm opus-5 100 100 0 90\n"
          "saved 90\ncost 50\ngrid 000z\n");
    expect("cache models sum across machines",
           v.cache.models[0].cread == 1000 && v.cache.saved == 904 && v.cache.cost == 250);
    expect("daily percentages average where both report",
           v.cache.pct[3] == 0 && v.cache.pct[4] == 50 && v.cache.pct[6] == 100);
    memset(&d, 0, sizeof d);
    parse("!cache\nhost air\nm opus-5 1 9 0 8\nsaved 8\ncost 1\n");
    expect("cache without dates still gives the totals",
           v.cache.present && v.cache.hit_pct == 90 && v.cache.len == 0);
    parse("!cache\nhost air\nstart 1\ntoday 5\n");
    expect("cache without models is not drawn", !v.cache.present);

    /* Tools. */
    memset(&d, 0, sizeof d);
    expect("tools marker recognised",
           parse("!tools\nhost air\ndays 23\ncalls 100\nmsgs 500\nsessions 7\n"
                 "start 100\ntoday 106\nt Bash 79\nt Edit 11\nt other 10\n"
                 "grid 5..0..A\n") == UD_TOOLS);
    expect("tools present and ranked",
           v.tools.present && v.tools.count == 3 && strcmp(v.tools.rows[0].name, "Bash") == 0
           && v.tools.calls == 100 && v.tools.msgs == 500 && v.tools.sessions == 7);
    expect("tool calls per day decoded",
           v.tools.len == 7 && v.tools.day[0] == 5 && v.tools.day[3] == 1
           && v.tools.day[6] == 32 && v.tools.day[1] == 0);
    parse("!tools\nhost studio\ndays 5\ncalls 20\nmsgs 50\nsessions 1\n"
          "start 105\ntoday 106\nt Edit 15\nt Read 5\ngrid .5\n");
    expect("tools merge by name and re-rank",
           v.tools.count == 4 && strcmp(v.tools.rows[0].name, "Bash") == 0
           && strcmp(v.tools.rows[1].name, "Edit") == 0 && v.tools.rows[1].calls == 26
           && v.tools.calls == 120);
    expect("tool days line up by date", v.tools.day[6] == 37);
    memset(&d, 0, sizeof d);
    parse("!tools\nhost air\ncalls 3\nt Bash 3\n");
    expect("tools without dates still list", v.tools.present && v.tools.len == 0);
    parse("!tools\nhost air\ncalls 0\n");
    expect("tools without rows is not drawn", !v.tools.present);

    /* Thinking share. */
    memset(&d, 0, sizeof d);
    expect("thinking marker recognised",
           parse("!thinking\nhost air\ndays 23\nstart 100\ntoday 106\n"
                 "m opus-5 350 650 1 2\nm haiku 0 10 0 0\ngrid zU.0zzz\n") == UD_THINKING);
    expect("thinking present, ranked by output",
           v.thinking.present && v.thinking.model_count == 2
           && strcmp(v.thinking.models[0].name, "opus-5") == 0);
    expect("thinking share overall", v.thinking.share_pct == 35);   /* 350 of 1010 */
    expect("thinking totals",
           v.thinking.thinking == 350 && v.thinking.visible == 660
           && v.thinking.think_cents == 1 && v.thinking.out_cents == 2);
    expect("thinking daily percentages",
           v.thinking.pct[0] == 100 && v.thinking.pct[2] == -1 && v.thinking.pct[3] == 0);
    parse("!thinking\nhost studio\nstart 103\ntoday 106\nm opus-5 100 100 1 1\ngrid 000z\n");
    expect("thinking sums across machines",
           v.thinking.models[0].thinking == 450 && v.thinking.pct[3] == 0
           && v.thinking.pct[4] == 50 && v.thinking.pct[6] == 100);
    memset(&d, 0, sizeof d);
    parse("!thinking\nhost air\nstart 1\ntoday 3\n");
    expect("thinking without models is not drawn", !v.thinking.present);

    /* This week against last. */
    memset(&d, 0, sizeof d);
    expect("week marker recognised",
           parse("!week\nhost air\ntoday 20703\nw tokens 28000 1000\nw cost 500 200\n"
                 "w msgs 15 40\nw sessions 2 5\nw tools 42 7\nw days 7 2\n"
                 "grid ......5......A\n") == UD_WEEK);
    expect("week rows kept in order",
           v.week.present && v.week.this_week[0] == 28000 && v.week.last_week[0] == 1000
           && v.week.this_week[1] == 500 && v.week.this_week[2] == 15
           && v.week.this_week[3] == 2 && v.week.this_week[4] == 42
           && v.week.this_week[5] == 7 && v.week.last_week[5] == 2);
    expect("week days decoded oldest first",
           v.week.day[6] > 5000 && v.week.day[13] == 32000 && v.week.day[0] == 0);
    parse("!week\nhost studio\ntoday 20702\nw tokens 100 100\nw days 3 3\ngrid .............5\n");
    expect("weeks sum across machines and cap active days",
           v.week.this_week[0] == 28100 && v.week.this_week[5] == 7 && v.week.last_week[5] == 5);
    expect("an older machine's days shift onto the newest today",
           v.week.day[12] > 5000 && v.week.today == 20703);
    parse("!week\nhost studio\ntoday 20702\nw bogus 1 1\n");
    expect("a week with only unknown rows is dropped for that machine",
           v.week.this_week[0] == 28000);

    /* Records. */
    memset(&d, 0, sizeof d);
    expect("records marker recognised",
           parse("!records\nhost air\nr bigday 7000 20703\nr streak 9 20703\n"
                 "r session 10800 20697\nr early 365 20698\nr late 1420 20700\n"
                 "since 20561\n") == UD_RECORDS);
    expect("records present", v.records.present && v.records.count == 5 && v.records.since == 20561);
    {
        const ud_record_t *r = usagedata_record(&v.records, "bigday");
        expect("record looked up by key", r && r->value == 7000 && r->day == 20703);
        expect("missing record is NULL", usagedata_record(&v.records, "nope") == NULL);
    }
    parse("!records\nhost studio\nr bigday 9000 20600\nr early 300 20500\nr late 1000 20500\n"
          "r response 555 20600\nsince 20400\n");
    {
        const ud_record_t *big = usagedata_record(&v.records, "bigday");
        const ud_record_t *early = usagedata_record(&v.records, "early");
        const ud_record_t *late = usagedata_record(&v.records, "late");
        expect("the larger record wins with its day", big && big->value == 9000 && big->day == 20600);
        expect("the earlier early wins", early && early->value == 300);
        expect("the later late is kept", late && late->value == 1420);
        expect("a new key is added", usagedata_record(&v.records, "response") != NULL);
        expect("since is the earliest", v.records.since == 20400);
    }
    parse("!records\nhost studio\n");
    expect("an empty records payload drops that machine's rows",
           usagedata_record(&v.records, "response") == NULL);

    /* What Claude runs. */
    memset(&d, 0, sizeof d);
    expect("runs marker recognised",
           parse("!runs\nhost air\ndays 23\ncalls 8810\ncommands 100\n"
                 "c other 30\nc git 40\nc make 20\nk git 40\nk build 20\nk other 40\n") == UD_RUNS);
    expect("runs present with totals",
           v.runs.present && v.runs.calls == 8810 && v.runs.commands == 100 && v.runs.days == 23);
    expect("programs ranked with other last",
           v.runs.count == 3 && strcmp(v.runs.rows[0].name, "git") == 0
           && strcmp(v.runs.rows[1].name, "make") == 0 && strcmp(v.runs.rows[2].name, "other") == 0);
    expect("categories by name", v.runs.cats[0] == 40 && v.runs.cats[1] == 20 && v.runs.cats[6] == 40
           && v.runs.cats[2] == 0 && v.runs.cats[5] == 0);
    parse("!runs\nhost studio\ncalls 10\ncommands 30\nc make 30\nk build 30\n");
    expect("runs sum and re-rank across machines",
           strcmp(v.runs.rows[0].name, "make") == 0 && v.runs.rows[0].calls == 50
           && v.runs.cats[1] == 50 && v.runs.commands == 130);
    parse("!runs\nhost studio\n");
    expect("an empty runs payload drops that machine", v.runs.rows[0].calls == 40);

    /* Turns. */
    memset(&d, 0, sizeof d);
    expect("turns marker recognised",
           parse("!turns\nhost air\ndays 23\nturns 100\ninterrupted 4\nfirst 8 24\n"
                 "turn 99 535\nstart 100\ntoday 106\nlongest 46144 105\ngrid 5...A.0\n") == UD_TURNS);
    expect("turn figures kept",
           v.turns.present && v.turns.turns == 100 && v.turns.interrupted == 4
           && v.turns.first_med == 8 && v.turns.first_p90 == 24
           && v.turns.turn_med == 99 && v.turns.turn_p90 == 535
           && v.turns.longest == 46144 && v.turns.longest_day == 105);
    expect("turn days decoded, empty days marked",
           v.turns.len == 7 && v.turns.day[0] == 5 && v.turns.day[1] == -1
           && v.turns.day[4] == 32 && v.turns.day[6] == 1);
    parse("!turns\nhost studio\nturns 300\nfirst 4 10\nturn 19 100\nlongest 50 1\n"
          "start 103\ntoday 106\ngrid ...5\n");
    expect("medians average weighted by turns",
           v.turns.turns == 400 && v.turns.first_med == 5 && v.turns.turn_med == 39);
    expect("longest is the longest anywhere", v.turns.longest == 46144);
    expect("turn days average where both report", v.turns.day[6] == 3);
    memset(&d, 0, sizeof d);
    parse("!turns\nhost air\ndays 3\n");
    expect("turns without a count is not drawn", !v.turns.present);

    /* Today's story. */
    memset(&d, 0, sizeof d);
    expect("story marker recognised",
           parse("!story\nhost air\ndate 20703\nprompts 41\ntext You spent the day on tell,\n"
                 "text adding pages.\n") == UD_STORY);
    expect("story lines joined",
           v.story.present && v.story.day == 20703 && v.story.prompts == 41
           && strcmp(v.story.text, "You spent the day on tell, adding pages.") == 0);
    parse("!story\nhost studio\ndate 20702\nprompts 90\ntext Yesterday's.\n");
    expect("the newer day's story wins", strcmp(v.story.text, "You spent the day on tell, adding pages.") == 0);
    parse("!story\nhost studio\ndate 20703\nprompts 90\ntext Busier machine.\n");
    expect("same day: the machine with more prompts wins", strcmp(v.story.text, "Busier machine.") == 0);
    {
        char longp[1200] = "!story\ndate 30000\nprompts 1\ntext ";
        for (int i = 0; i < 900; i++) strncat(longp, "x", sizeof longp - strlen(longp) - 1);
        parse(longp);
        expect("a long story is truncated, not overflowed", strlen(v.story.text) == UD_STORY_MAX);
    }
    parse("!story\ndate 5\n");
    expect("a story without text is not drawn for that machine", !v.story.present || v.story.day != 5);

    if (failures == 0) { printf("all tests passed\n"); return 0; }
    printf("%d test(s) failed\n", failures);
    return 1;
}
