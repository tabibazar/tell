#include "pagedefs.h"

#include "views.h"

#define SEC(n) ((int64_t)(n) * 1000000LL)

/* Order here is irrelevant; the tap order is the enum in pages.h. */
const page_def_t page_defs[PAGE_COUNT] = {
    /*                 name              feeds                          draw            anim   refresh   where      touch saver */
    [PAGE_CLOCK]    = { "Clock",         UD_FEED(UD_CLOCK),             NULL,           false, 0,        PG_BOTH,  false, true  },
    [PAGE_MENU]     = { "Menu",          0,                             NULL,           false, 0,        PG_BIG,  true,  false },
    [PAGE_NOW]      = { "Today, live",   UD_FEED(UD_NOW),               views_now,      false, SEC(10),  PG_BIG,  false, true  },
    [PAGE_STORY]    = { "Today's story", UD_FEED(UD_STORY),             views_story,    false, 0,        PG_BIG,  false, true  },
    [PAGE_LIMITS]   = { "Limits",        UD_FEED(UD_LIMITS),            views_limits,   false, SEC(1),   PG_BOTH, false, true  },
    [PAGE_STATS]    = { "By model",      UD_FEED(UD_STATS),             views_stats,    true,  0,        PG_BIG,  false, true  },
    [PAGE_TODAY]    = { "Almanac",       UD_FEED(UD_TODAY),             NULL,           false, 0,        PG_BIG,  false, true  },
    [PAGE_MODELS]   = { "Models per day", UD_FEED(UD_STATS),            views_models,   true,  0,        PG_BIG,  false, true  },
    [PAGE_PROJECTS] = { "By project",    UD_FEED(UD_PROJECTS),          views_projects, true,  0,        PG_BIG,  false, true  },
    [PAGE_YEAR]     = { "Last 12 months", UD_FEED(UD_YEAR),             views_year,     false, 0,        PG_BIG,  false, true  },
    [PAGE_RHYTHM]   = { "When you work", UD_FEED(UD_RHYTHM),            views_rhythm,   false, 0,        PG_BIG,  false, true  },
    [PAGE_COST]     = { "API cost",      UD_FEED(UD_COST),              views_cost,     true,  0,        PG_BIG,  false, true  },
    [PAGE_CACHE]    = { "Cache",         UD_FEED(UD_CACHE),             views_cache,    true,  0,        PG_BIG,  false, true  },
    [PAGE_TOOLS]    = { "Tools",         UD_FEED(UD_TOOLS),             views_tools,    true,  0,        PG_BIG,  false, true  },
    [PAGE_RUNS]     = { "What it runs",  UD_FEED(UD_RUNS),              views_runs,     true,  0,        PG_BIG,  false, true  },
    [PAGE_THINKING] = { "Thinking",      UD_FEED(UD_THINKING),          views_thinking, true,  0,        PG_BIG,  false, true  },
    [PAGE_RECORDS]  = { "Records",       UD_FEED(UD_RECORDS),           views_records,  false, 0,        PG_BIG,  false, true  },
    [PAGE_TURNS]    = { "Turns",         UD_FEED(UD_TURNS),             views_turns,    true,  0,        PG_BIG,  false, true  },
    [PAGE_MESSAGE]  = { "Message",       0,                             NULL,           false, 0,        PG_BOTH,  false, true  },
    [PAGE_DAILY]    = { "Tokens per day", UD_FEED(UD_DAILY),            views_daily,    true,  0,        PG_BIG,  false, true  },
    [PAGE_PARTICLES]= { "Sand",          0,                             NULL,           false, 0,        PG_BOTH,  false, false },
    [PAGE_TIMER]    = { "Timer",         0,                             NULL,           false, 0,        PG_BOTH,  false, false },
    [PAGE_STOPWATCH]= { "Stopwatch",     0,                             NULL,           false, 0,        PG_BOTH,  false, false },
    [PAGE_TEMPS]    = { "Temps",         0,                             NULL,           false, SEC(2),   PG_BOTH,  false, true  },
    [PAGE_RTC]      = { "RTC",           0,                             NULL,           false, SEC(1),   PG_BOTH,  false, false },
    [PAGE_LEVEL]    = { "Level",         0,                             NULL,           false, 0,        PG_BOTH,  false, false },
    /* Small panels only: 26x7 of the figures worth glancing at, where the
       big panel has a page each for models, cost and the year. */
    [PAGE_USAGE]    = { "Usage",         UD_FEED(UD_STATS) | UD_FEED(UD_COST)
                                         | UD_FEED(UD_YEAR),            views_usage,    false, 0,        PG_SMALL, false, true  },
    [PAGE_SETTINGS] = { "Settings",      0,                             NULL,           false, 0,        PG_BIG,  true,  false },
};
