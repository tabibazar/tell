# Year, Models and Cost pages — design

*2026-09-07. Written and built in one autonomous session from a screenshot of
Claude Code's own `/stats` screen; the approval step was skipped, so treat
this as the record of what was decided, not what was agreed.*

## What

A new page on the CrowPanel, after TODAY, that looks like Claude Code's
`/stats` overview: a GitHub-style activity grid of the last 53 weeks
(columns are weeks, rows Sunday to Saturday), month labels above, a
`Less ▮▮▮▮ More` legend, and beneath it the headline figures — favourite
model, total tokens, sessions, longest session, active days, streaks, most
active day, and the input / output / cache breakdown.

A second new page, MODELS, mirrors Claude Code's *Models* tab: a step line
of tokens per day for each of the three biggest models over the last sixty
days, a legend, and beneath it a card per model with its share, input,
output and cache figures. It is fed by the existing `!stats` payload, whose
`m` rows gain optional trailing fields — input, cache write, calls, and a
sixty-character grid — after two new `start`/`today` lines. Older senders
still parse.

A third page, COST, answers "what would this have cost on the API": all-time,
last 30 and last 7 day totals, a bar per model, and a bar per day for the last
sixty days, with the peak named. Fed by a `!cost` payload of cents plus a
sixty-character daily grid in thousandths of a dollar. Prices are API list
rates per model; cache writes at the 5-minute rate unless the transcript
records a 1-hour write. `CLAUDE_PLAN_USD`, if set, adds a "vs plan" ratio.

## Where the history comes from

Transcripts on disk covered only two months, but Claude Code's `/stats`
showed activity since April. `~/.claude/stats-cache.json` holds that: daily
activity (message counts) back to the first session, per-model daily tokens
since July, all-time per-model totals, session count, longest session. The
collector treats the cache as truth up to its `lastComputedDate` and the
transcripts as truth after it, so nothing double counts. Days the cache
knows only as message counts are estimated from the average message on days
where both are known, and the count of such days rides along as `estimated`
so the board can footnote it. The result matches Claude Code's own figures.

The existing STATS (bars per model) and DAILY (bars per day) pages stay.

## Why the payload is one character per day

BLE messages are capped at 2048 bytes. A row per day would be about 17 bytes
a day and unbounded in the number of active days; a heavy year would not fit.
So the Mac sends a **grid string**, one character per day from a Sunday about
a year ago to today: `.` for no activity, otherwise `0-9A-Za-z` encoding the
day's tokens on a half-octave log scale (`i = round(2·log2(tokens/1000))`,
decoded as `1000·2^(i/2)`). That is at most 371 bytes, bounded forever, and
accurate to about 19%, which is plenty for a colour level and for picking the
busiest day.

```
!year
host air
start 20338          days since 1970-01-01, local; always a Sunday
today 20703          the last cell of the grid
first 20561          first day with any transcript, for "active days a/b"
grid ....3A..B...    one char per day, start..today inclusive
sessions 167
longest 1628340      seconds, first to last message of one session
fav opus-5
tok 949800 51700000 12700000000 292900000     in out cache-read cache-write
```

Days are **absolute day numbers**, not `MM-DD`, so two Macs that pushed on
different days still line up on the board. The board gains a small piece of
calendar arithmetic (`timecalc_civil`, days → year/month/day, and the
weekday) so it can label months and name the busiest day itself; none of
that involves time zones, which stay the Mac's problem.

Days are bucketed by the **Mac's local date**. The daily page used UTC dates
before; it now uses local too, since the same line of Python feeds both.

## Merging several machines

As with the other pages each machine's payload replaces only its own slot.
The view is built on the window of whichever machine has the latest `today`;
other machines' cells are added by absolute day. Then: tokens and sessions
sum, longest session is the max, first day the min, favourite model is the
one from the machine with the most tokens. Levels, streaks, active-day count
and busiest day are computed from the merged grid.

Levels follow GitHub: quartiles of the non-zero days, so level 4 is the
busiest quarter of active days rather than a fixed threshold.

## Rendering

64×20 cells of 12×24 px. The heat grid has its own horizontal pitch of
14 px with a 1 px gap (13×23 px cells), so 53 weeks span 742 px beside a
48 px weekday gutter and the map is a mosaic of touching cells. The first
cut used 10×20 cells inset in text cells, and it looked like a row of block
glyphs; hence `canvas_puts_px` for pixel-positioned month labels. Rows keep
the 24 px text pitch so weekday labels align. Empty days are a small dim
dot. Four-step single-hue ramp (amber, by request; the
original is terracotta); adjacent steps are ≥14 ΔE apart under every common form of
colour blindness and lightness is monotone, so the scale reads without hue.

No grow-in animation: cells are not bars.

## Testing

Pure units on the host: `timecalc_civil` against Python-computed vectors
(epoch, leap day, today); `usagedata` for parsing, alignment across machines
with different windows, quartile levels, streaks. A `test_views` renders the
page into an 800×480 host framebuffer and can write a PPM, which was
converted to PNG and inspected, once with synthetic data and once with the
real output of `claude-stats.py --format data --section year`.

ESP-IDF v5.5.5 was installed during the session (`~/esp/esp-idf`, with the
Python 3.13 environment `tools/idf-env.sh` expects) and both firmware
variants build with no warnings. Nothing was flashed: no board was attached.
