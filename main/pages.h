#ifndef PAGES_H
#define PAGES_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    /* Tap order. The clock is home; the pages you glance at most come first,
       and the two text-heavy ones sit last, just before the clock returns. */
    PAGE_CLOCK = 0,
    PAGE_MENU,       /* a tile per page; first tap from the clock opens it */
    PAGE_NOW,        /* today so far, and whether Claude is busy */
    PAGE_STORY,      /* a recap of the day, written by Claude */
    PAGE_LIMITS,     /* session and weekly limits, counting down */
    PAGE_STATS,
    PAGE_TODAY,
    PAGE_MODELS,     /* tokens per day, one line per model */
    PAGE_PROJECTS,   /* tokens by repository */
    PAGE_YEAR,       /* the last twelve months as a heatmap */
    PAGE_RHYTHM,     /* messages by weekday and hour */
    PAGE_COST,       /* what it would have cost on the API */
    PAGE_CACHE,      /* prompt-cache hit rate and savings */
    PAGE_TOOLS,      /* which tools Claude calls */
    PAGE_RUNS,       /* the programs behind the Bash calls */
    PAGE_THINKING,   /* thinking versus visible output */
    PAGE_RECORDS,    /* personal bests */
    PAGE_TURNS,      /* how long Claude takes */
    PAGE_MESSAGE,
    PAGE_DAILY,      /* tokens per day as bars */
    PAGE_PIP,        /* envio's desk familiar: a face that watches gravity */
    PAGE_PARTICLES,  /* tilt-poured sand; the Feather and wave, which have IMUs */
    PAGE_TIMER,      /* a countdown you shake to restart */
    PAGE_STOPWATCH,  /* counting up; shake to start and stop */
    PAGE_TEMPS,      /* die and crystal, charted over time */
    PAGE_RTC,        /* the clock chip, and how far the board has drifted */
    PAGE_LEVEL,      /* the Feather's spirit level; needs its IMU */
    PAGE_BUBBLE,     /* envio's "Level" slot: the same spirit-level game,
                        via draw_level(); bubblelevel.c is unused */
    PAGE_SYSTEM,     /* envio: battery, power, chip temp, uptime, clock state */
    PAGE_USAGE,      /* all-time totals, drawn for a small panel */
    PAGE_ROOM_TEMP,  /* the three room readings, charted from the flash log */
    PAGE_ROOM_RH,
    PAGE_ROOM_HPA,
    PAGE_ROOM_VOC,   /* volatile organics, with the air quality index */
    PAGE_ROOM_CO2,   /* equivalent CO2 -- derived from the VOCs, not measured */
    PAGE_TREND,      /* temperature and humidity together, across the day */
    PAGE_WEEK,       /* each day's low and high; cycles the three readings */
    PAGE_FORECAST,   /* the multi-day weather forecast, pushed from the Mac */
    PAGE_CLIMATE,    /* temperature over humidity, stacked; the tall panel */
    PAGE_AIR,        /* air quality over CO2, stacked */
    PAGE_SETTINGS,   /* touch-only; out of the way, near the end of the tap order */
    PAGE_CAMERA,     /* envio's OV5640: viewfinder-when-held, tap to shoot.
                        Kept last (right before PAGE_COUNT) rather than
                        wherever it fits topically: settings.c persists
                        cycle_off as a PAGE_BIT(page) mask in NVS, and
                        inserting a page anywhere but the end would reindex
                        every later page's bit against an old saved mask. */
    PAGE_GALLERY,    /* envio: browse the JPEGs PAGE_CAMERA saved to the
                        card. Kept last, right before PAGE_COUNT, for the
                        same NVS cycle_off reason as PAGE_CAMERA above. */
    PAGE_FACE,       /* watch: the grand-complication analog face. Appended,
                        like the two above, so no saved mask reindexes. */
    PAGE_MOON,       /* watch: the big moon, its age and the next full/new */
    PAGE_SOUND,      /* watch: speaker's sound level, relayed by the Mac */
    PAGE_DAYS,       /* watch: speaker's daily levels, today vs a usual day */
    PAGE_COUNT
} page_t;

/*
 * A bit per page. Sixty-four of them, and the assertion below is not
 * decoration: with a 32-bit mask, PAGE_BIT of page 32 is undefined behaviour
 * that in practice shifts round to bit 0 and silently aliases the clock. A
 * board would then offer a page it does not have, and hide one it does, with
 * nothing in the build to say so. Adding the thirty-third page is exactly the
 * kind of ordinary change that should not be able to do that.
 */
#define PAGE_BIT(p) (1ULL << (p))

_Static_assert(PAGE_COUNT <= 64, "the page mask is 64 bits wide");

/*
 * The type to hold one. Spelled out because a mask kept in a plain `unsigned`
 * is a bug that hides: PAGE_BIT is 64 bits, so `mask &= ~narrow` promotes the
 * complement by zero-extending it and silently clears every page above the
 * thirty-second. That cost an afternoon once -- the pressure page simply
 * stopped appearing in the rotation, with no warning from the compiler.
 */
typedef uint64_t page_mask_t;

/* How long a page stays pinned after a touch or a new message. */
#define PAGES_IDLE_US (5 * 60 * 1000000LL)

/* The default rotation interval. Zero disables rotation, leaving the display
   wherever it was last put; a board sets its own with pages_set_rotate.
   Rotation exists for two reasons: a static image on an LCD risks retention,
   and a board with no touch and two buttons is better at showing you things
   in turn than at being navigated. */
#define PAGES_ROTATE_US 0

/* How long a touch, a button or a message holds the rotation off. Shorter
   than PAGES_IDLE_US, which pins a page against the screensaver: a board that
   rotates is meant to keep moving, and a minute is long enough to read what
   you pressed a button to see. */
#define PAGES_ROTATE_PIN_US (60 * 1000000LL)

/* After this long with no touch and no message, the screensaver takes over,
   either cycling the pages or drifting the clock, so nothing sits still long
   enough to burn in. This is the default; the Settings page changes it at
   run time through pages_set_saver. */
#define PAGES_SAVER_US (10 * 60 * 1000000LL)

typedef struct {
    uint64_t available;      /* bitmask of PAGE_BIT(...) */
    page_t current;
    int64_t last_activity_us;
    int64_t last_rotate_us;
    int64_t saver_us;        /* idle time before the saver; 0 disables it */
    int64_t rotate_us;       /* time on each page once idle; 0 disables it */
} pages_t;

void pages_init(pages_t *p, uint64_t available);

/* Changes the idle time before the screensaver. Zero means never. */
void pages_set_saver(pages_t *p, int64_t us);

/* How long each page gets before the next one. Zero leaves the display where
   it was last put, which is the default. A board that rotates needs no
   screensaver -- nothing on it stays still long enough to burn in -- so this
   turns the saver off, and pages_saver_active says so. */
void pages_set_rotate(pages_t *p, int64_t us);

/* Moves to the next available page not in `skip`, without counting as
   activity, so a slideshow can step through the pages while the saver stays
   active. Returns the page it landed on, unchanged if there is nowhere to go. */
page_t pages_step(pages_t *p, uint64_t skip);

/* Moves to the next available page and returns it. */
page_t pages_advance(pages_t *p, int64_t now_us);

/* Moves to the previous available page and returns it. A tap on the left
   half of the screen; the right half advances. */
page_t pages_back(pages_t *p, int64_t now_us);

/* Jumps to a page, ignored if that page is not available on this board. */
void pages_show(pages_t *p, page_t page, int64_t now_us);

/* True when the page was pinned by a touch or message and that has expired. */
bool pages_idle_expired(const pages_t *p, int64_t now_us);

/* True when the screensaver should be showing. */
bool pages_saver_active(const pages_t *p, int64_t now_us);

/* Call every loop. Once nothing has been pinned for PAGES_ROTATE_PIN_US,
   advances to the next page every rotate_us, leaving out `skip` -- the same
   mask the screensaver uses, so a page struck off the round stays off it
   here too. Returns true if the page changed. */
bool pages_tick(pages_t *p, int64_t now_us, uint64_t skip);

#endif /* PAGES_H */
