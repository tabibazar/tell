#include "ud_internal.h"

extern const ud_section_t ud_section_stats;
extern const ud_section_t ud_section_daily;
extern const ud_section_t ud_section_clock;
extern const ud_section_t ud_section_today;
extern const ud_section_t ud_section_year;
extern const ud_section_t ud_section_cost;
extern const ud_section_t ud_section_rhythm;
extern const ud_section_t ud_section_now;
extern const ud_section_t ud_section_projects;
extern const ud_section_t ud_section_cache;
extern const ud_section_t ud_section_tools;
extern const ud_section_t ud_section_thinking;
extern const ud_section_t ud_section_week;
extern const ud_section_t ud_section_records;
extern const ud_section_t ud_section_runs;
extern const ud_section_t ud_section_turns;
extern const ud_section_t ud_section_story;
extern const ud_section_t ud_section_level;
extern const ud_section_t ud_section_flip;
extern const ud_section_t ud_section_zero;

/* Every payload the board understands. A new section is a new ud_*.c file
   and one line here. */
const ud_section_t *const ud_sections[] = {
    &ud_section_stats,
    &ud_section_daily,
    &ud_section_clock,
    &ud_section_today,
    &ud_section_year,
    &ud_section_cost,
    &ud_section_rhythm,
    &ud_section_now,
    &ud_section_projects,
    &ud_section_cache,
    &ud_section_tools,
    &ud_section_thinking,
    &ud_section_week,
    &ud_section_records,
    &ud_section_runs,
    &ud_section_turns,
    &ud_section_story,
    /* The Feather's level. The big board has no IMU and never offers the
       page, so these three do nothing there; they are registered anyway
       because the registry is a plain table and a board-conditional one
       would cost more clarity than the three rows are worth. */
    &ud_section_level,
    &ud_section_flip,
    &ud_section_zero,
};
const int ud_section_count = (int)(sizeof ud_sections / sizeof ud_sections[0]);
