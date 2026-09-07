#ifndef USAGEDATA_H
#define USAGEDATA_H

#include <stdbool.h>
#include <stdint.h>

#define UD_MAX_MODELS 8
#define UD_MAX_DAYS   14
#define UD_MAX_HOSTS  3
#define UD_NAME_MAX   15
#define UD_LABEL_MAX  7
#define UD_HOST_MAX   15
#define UD_TEXT_MAX   63
#define UD_YEAR_MAX   371        /* 53 weeks of one character per day */
#define UD_MDAYS      60         /* days of per-model history, one char each */
#define UD_RHYTHM_CELLS 168      /* 7 weekdays x 24 hours */
#define UD_HEAT_LEVELS 4

typedef struct {
    char name[UD_NAME_MAX + 1];
    uint64_t out;
    uint64_t cread;
    /* Sent by newer clients; zero and empty from older ones. */
    uint64_t in;
    uint64_t cwrite;
    uint32_t calls;
    char grid[UD_MDAYS + 1];     /* this model's last days, a char per day */
} ud_model_t;

typedef struct {
    char label[UD_LABEL_MAX + 1];
    uint64_t tokens;
    int8_t dow;              /* 0 Monday .. 6 Sunday, -1 when not sent */
} ud_day_t;

/* One machine's year, as sent: a character per day so a whole year fits one
   BLE message. Days are counts since 1970-01-01 in the Mac's local time, so
   two machines' grids can be lined up even if they were sent on different
   days; the board has no calendar of its own to do that with. */
typedef struct {
    bool used;
    int32_t start;           /* the first grid cell; the Mac makes it a Sunday */
    int32_t today;           /* the last grid cell */
    int32_t first;           /* first day with any transcript, 0 if not sent */
    char grid[UD_YEAR_MAX + 1];
    uint32_t sessions;
    uint32_t longest_secs;   /* longest single session */
    char fav[UD_NAME_MAX + 1];
    uint64_t tok[4];         /* input, output, cache read, cache write */
    int estimated;           /* days the Mac could only estimate */
} ud_year_t;

/* What one machine's usage would have cost on the API, in cents, as sent. */
typedef struct {
    char name[UD_NAME_MAX + 1];
    uint64_t cents;
} ud_cost_model_t;

typedef struct {
    bool used;
    int32_t start, today;
    char grid[UD_MDAYS + 1]; /* cost per day in thousandths of a dollar */
    uint64_t total, last30, last7;
    uint64_t plan;           /* the subscription's monthly price, 0 if unknown */
    ud_cost_model_t models[UD_MAX_MODELS];
    int model_count;
} ud_cost_t;

/* Messages by weekday and hour, Monday first, one character per cell. */
typedef struct {
    bool used;
    int days;                /* distinct days the grid was built from */
    uint32_t msgs;
    char grid[UD_RHYTHM_CELLS + 1];
} ud_rhythm_t;

/* Today so far on one machine, and what happened last. */
typedef struct {
    bool used;
    uint64_t tokens, avg;    /* today, and a typical day of the last 30 */
    uint64_t cost;           /* cents */
    uint32_t msgs, sessions;
    bool have_last;
    uint32_t last_secs;      /* age of the last message when this was sent */
    uint32_t session_secs;   /* length of the session that message was in */
    char model[UD_NAME_MAX + 1];
    char project[UD_NAME_MAX + 1];
} ud_now_t;

/* Tokens by project directory, from one machine's transcripts. */
typedef struct {
    char name[UD_NAME_MAX + 1];
    uint64_t tokens, cents;
    uint32_t sessions, msgs;
} ud_project_t;

typedef struct {
    bool used;
    int days;
    ud_project_t rows[UD_MAX_MODELS];
    int count;
} ud_projects_t;

/* Prompt-cache use: per model all time, and per day as a percentage. */
typedef struct {
    char name[UD_NAME_MAX + 1];
    uint64_t in, cread, cwrite;
    uint64_t saved_cents;    /* cached tokens at input price minus at read price */
} ud_cache_model_t;

typedef struct {
    bool used;
    int32_t start, today;
    char grid[UD_MDAYS + 1]; /* hit rate per day, linear percent chars; '.' none */
    ud_cache_model_t models[UD_MAX_MODELS];
    int model_count;
    uint64_t saved, cost;    /* cents, all time */
} ud_cache_t;

/* Tool calls by tool name, one machine. */
typedef struct {
    char name[UD_NAME_MAX + 1];
    uint32_t calls;
} ud_tool_t;

typedef struct {
    bool used;
    int days;
    uint32_t calls, msgs, sessions;
    int32_t start, today;
    char grid[UD_MDAYS + 1];     /* calls per day, log-scale, a thousand = one */
    ud_tool_t rows[UD_MAX_MODELS];
    int count;
} ud_tools_t;

/* Thinking versus visible output per model, one machine, for the messages
   that reported the split. */
typedef struct {
    char name[UD_NAME_MAX + 1];
    uint64_t thinking, visible;
    uint64_t think_cents, out_cents;   /* thinking, and all output, at output price */
} ud_think_model_t;

typedef struct {
    bool used;
    int days;
    int32_t start, today;
    char grid[UD_MDAYS + 1];     /* thinking share per day, linear percent chars */
    ud_think_model_t models[UD_MAX_MODELS];
    int model_count;
} ud_thinking_t;

/* One machine's contribution. Kept separately so a re-push from one Mac
   replaces only its own share instead of clobbering the other's. */
typedef struct {
    char host[UD_HOST_MAX + 1];
    ud_model_t models[UD_MAX_MODELS];
    int model_count;
    int32_t mstart, mtoday;  /* the model grids' first and last day */
    ud_day_t days[UD_MAX_DAYS];
    int day_count;
    ud_year_t year;
    ud_cost_t cost;
    ud_rhythm_t rhythm;
    ud_now_t now;
    ud_projects_t projects;
    ud_cache_t cache;
    ud_tools_t tools;
    ud_thinking_t thinking;
    int64_t now_sent_us;     /* when the now section arrived, to age "last" */
    int64_t updated_us;      /* when this machine last sent anything */
    bool used;
} ud_host_t;

typedef struct {
    ud_host_t hosts[UD_MAX_HOSTS];
    /* Sent from the Mac: the board knows only seconds since midnight, so it
       cannot work out a date, and it has no network to ask about weather. */
    char date[UD_TEXT_MAX + 1];
    char weather[UD_TEXT_MAX + 1];
    /* Almanac, also computed on the Mac: the board has neither a calendar
       nor the ephemeris to work any of it out. */
    float moon_phase;                    /* 0 new, 0.5 full, 1 new again */
    char moon_name[UD_TEXT_MAX + 1];
    char sun[UD_TEXT_MAX + 1];
    char dayinfo[UD_TEXT_MAX + 1];
    char holiday[UD_TEXT_MAX + 1];
} usagedata_t;

/* The merged year, ready to draw: every machine's days added by date, then
   each active day ranked into a quartile so the busiest quarter of days is the
   brightest colour, however busy that is in absolute terms. */
typedef struct {
    bool present;
    int32_t start, today;
    int len;                          /* cells from start to today inclusive */
    uint8_t level[UD_YEAR_MAX];       /* 0 none, 1..UD_HEAT_LEVELS */
    uint64_t tok[4];                  /* input, output, cache read, cache write */
    uint32_t sessions;
    uint32_t longest_secs;
    char fav[UD_NAME_MAX + 1];        /* from the machine with the most tokens */
    int active_days;                  /* cells with any tokens */
    int span_days;                    /* days since the first transcript, capped at len */
    int longest_streak, current_streak;
    int peak_index;                   /* cell with the most tokens, -1 if none */
    int estimated;                    /* older days estimated from message counts */
} ud_year_view_t;

/* Every machine's API-equivalent cost, summed. */
typedef struct {
    bool present;
    int32_t start, today;
    int len;
    uint64_t total, last30, last7, plan;      /* cents */
    ud_cost_model_t models[UD_MAX_MODELS];    /* ranked, costliest first */
    int model_count;
    uint64_t day[UD_MDAYS];                   /* thousandths of a dollar */
} ud_cost_view_t;

/* Every machine's messages by weekday and hour, ranked into quartiles. */
typedef struct {
    bool present;
    int days;
    uint64_t msgs;
    uint32_t cell[UD_RHYTHM_CELLS];
    uint8_t level[UD_RHYTHM_CELLS];
    int peak_dow, peak_hour;          /* the single busiest cell */
    int busiest_dow, busiest_hour;    /* row and column with the most */
    int night_pct, weekend_pct;       /* 22:00-05:59, Saturday and Sunday */
} ud_rhythm_view_t;

/* Today across every machine; "last" is from whichever spoke most recently. */
typedef struct {
    bool present;
    uint64_t tokens, avg, cost;
    uint32_t msgs, sessions;
    bool have_last;
    uint32_t last_secs;
    int64_t last_sent_us;             /* so the age can keep counting */
    uint32_t session_secs;
    char model[UD_NAME_MAX + 1];
    char project[UD_NAME_MAX + 1];
} ud_now_view_t;

/* Every machine's projects merged by name, largest first. */
typedef struct {
    bool present;
    int days;
    ud_project_t rows[UD_MAX_MODELS];
    int count;
    uint64_t total_tokens;
} ud_projects_view_t;

/* Every machine's cache use. Daily percentages are averaged across the
   machines that reported that day, since the volumes behind them are not
   sent. */
typedef struct {
    bool present;
    int32_t start, today;
    int len;
    int8_t pct[UD_MDAYS];             /* -1 when no machine had input that day */
    ud_cache_model_t models[UD_MAX_MODELS];
    int model_count;
    uint64_t saved, cost;
    int hit_pct;                      /* all time, all models */
} ud_cache_view_t;

/* Every machine's tool calls, by name, most called first. */
typedef struct {
    bool present;
    int days;
    uint32_t calls, msgs, sessions;
    ud_tool_t rows[UD_MAX_MODELS];
    int count;
    int32_t start, today;
    int len;
    uint64_t day[UD_MDAYS];               /* calls per day */
} ud_tools_view_t;

/* Every machine's thinking share, by model, most output first. */
typedef struct {
    bool present;
    int days;
    ud_think_model_t models[UD_MAX_MODELS];
    int model_count;
    uint64_t thinking, visible, think_cents, out_cents;
    int share_pct;                        /* all models */
    int32_t start, today;
    int len;
    int8_t pct[UD_MDAYS];                 /* -1 when no machine reported that day */
} ud_thinking_view_t;

/* Every machine's data summed, which is what the charts draw. */
typedef struct {
    ud_model_t models[UD_MAX_MODELS];
    int model_count;
    ud_day_t days[UD_MAX_DAYS];
    int day_count;

    /* Every machine's totals, and each machine's share of them, so a bar can
       be drawn stacked rather than needing a filter to see the split. */
    int host_count;
    char host_names[UD_MAX_HOSTS][UD_HOST_MAX + 1];
    uint64_t model_by_host[UD_MAX_MODELS][UD_MAX_HOSTS];
    uint64_t day_by_host[UD_MAX_DAYS][UD_MAX_HOSTS];
    int64_t updated_us;      /* the most recent machine's update, 0 if none */
    ud_year_view_t year;
    ud_cost_view_t cost;
    ud_rhythm_view_t rhythm;
    ud_now_view_t now;
    ud_projects_view_t projects;
    ud_cache_view_t cache;
    ud_tools_view_t tools;
    ud_thinking_view_t thinking;

    /* Each model's tokens per day, every machine summed, on the window of the
       machine that sent most recently. mlen is 0 when no machine sent grids. */
    int32_t mstart, mtoday;
    int mlen;
    uint64_t model_day[UD_MAX_MODELS][UD_MDAYS];
} ud_view_t;

typedef enum {
    UD_NONE = 0, UD_STATS, UD_DAILY, UD_CLOCK, UD_TODAY, UD_YEAR, UD_COST,
    UD_RHYTHM, UD_NOW, UD_PROJECTS, UD_CACHE, UD_TOOLS, UD_THINKING
} ud_kind_t;

/* Parses one payload. "!stats" and "!daily" replace that section for the
   sending machine, named by a "host <name>" line and defaulting to "mac".
   "!clock" carries the date and weather; "!year" a machine's
   heatmap grid and headline figures; "!cost" what its usage would have cost
   on the API; "!rhythm" messages by weekday and hour; "!now" today so far;
   "!projects" tokens by repository; "!cache" prompt-cache use; "!tools"
   tool calls; "!thinking" thinking versus visible output. Anything else returns UD_NONE and
   leaves `d` untouched, so the caller can treat it as a text message. */
ud_kind_t usagedata_parse(usagedata_t *d, const char *payload, int64_t now_us);

/* Sums every machine into one view: models added by name, days by label,
   both ordered largest and latest first respectively. */
void usagedata_merge(const usagedata_t *d, ud_view_t *out);

/* The tokens one grid character stands for: '.' is none, otherwise a
   half-octave log scale, 1000 * 2^(i/2) for the i-th of 0-9A-Za-z. */
uint64_t usagedata_grid_value(char c);

/* A percentage from a linear grid character (the same alphabet, 0..100
   spread over its 62 steps); -1 for '.' or anything unknown. */
int usagedata_pct_value(char c);

/* How many machines have sent anything, and the name of the nth. */
int usagedata_hosts(const usagedata_t *d);
const char *usagedata_host_name(const usagedata_t *d, int which);

#endif /* USAGEDATA_H */
