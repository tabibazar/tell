# envo: readable air -- one big reading per page

2026-09-25. Board: envo (`CONFIG_SCREEN_BOARD_TOUCH_LCD_147`, 320x172 landscape
JD9853, AXS5106L touch). Research, mockups and citations:
`docs/design/envo-ui/` (research-proposal.md, mock_rev1.c, mock_rev2.c, ref/*.png).

## Why

Reza: "I cannot read or make sense of the charts." The research found why:
- **Auto-scaled charts.** With clean air (VOC 30-49 ppb) the charts rescale until
  about ±8 ppb of sensor noise fills them, and the value axis has no numbers.
- **No verdict.** The one status word is 14 px white text in a title bar;
  nothing changes colour, and the home screen looks the same at 1366 ppb.
- **Text too small.** Everything but the clock digits is 14 px, below the
  legibility floor at desk distance.
- **VOC is hard to reach,** four swipes from home.

## Decisions (Reza)

- **Style A:** one big reading per page (Aranet-like), readable across the room.
- **History:** a thin 24 h strip under each reading; a **tap opens the full 24 h
  chart** for that reading, and a tap again goes back. Swipes page.
- **Temperature and humidity are shown raw.** The gas sensor is isolated, so the
  AHT21 is not assumed to read warm: no offset, no "est", and no comfort words
  until a trusted thermometer says otherwise.
- **VOC and eCO2 come first.**

## Pages

Swipe left = next, swipe right = previous; the order wraps. Waking the screen
always opens VOC, and the tap that wakes it is swallowed. Nothing rotates on
its own.

1. **VOC** (home)  2. **eCO2 est**  3. **TEMP**  4. **HUMIDITY**  5. **WEEK**  6. **CLOCK**

Removed from envo:
- TREND;
- the pressure pages and week (no barometer);
- the week page's 6 s rotation;
- TEMPS (die vs crystal: debug builds only);
- the grow animation on these pages.

## A reading page (320x172; mockup `ref/a1_single_voc.png`, `mock_rev1.c` page_single)

Text stays within x 16..304 (rounded corners). Big type uses vfont (vector
stroke font, `main/vfont.c`), marks use `main/vector.c`, and 14 px labels use
the 12x24 bitmap font.

- **Label** top-left (x 16, top 8), grey, 14 px: VOC / eCO2 est / TEMP / HUMIDITY.
- **Unit** top-right, right-aligned to 304, grey, 14 px: ppb / ppm / C / %.
  (Amended after the design critique: "ppm, from VOCs" ran into the "eCO2 est"
  label as one phrase; "est" and FROM VOCS already say where it comes from.)
- **The number:** vfont ~64 px capital height, white, x 16, readable across a
  room. It shrinks in 2 px steps while wider than 200 px (minimum 40).
  - Quantised by truncation so the last digit is not noise: VOC in steps of 5
    below 100, 10 up to 1000, 100 above; eCO2 in 10 ppm; temperature 0.1 C;
    humidity 1 %.
  - A degree sign is drawn with `vec_ring`.
- **State word** under the number, vfont ~34 px:
  - **GOOD** blue `0x04BF`, **FAIR** amber `0xFD40`, **POOR** black on a red
    `0xF8C1` block.
  - The eCO2 page never says GOOD: below its first limit the slot says
    `FROM VOCS` in grey, 22 px.
  - TEMP and HUMIDITY have no word; the slot is empty.
- **Trend,** right side (right-aligned to 304, level with the number): an arrow
  (`vec_polygon`) and RISING / FALLING in white 22 px, or STEADY in grey.
  - Compares the mean of the last 10 min with the mean of the 10 min ending
    30 min ago.
  - Moving means: VOC at least max(15 ppb, 25 %); eCO2 at least 50 ppm;
    temperature at least 0.5 C; humidity at least 3 %.
  - Not drawn until 40 min of valid data exist.
- **24 h strip** along the bottom, x 16..304, about 16 px tall, on a FIXED scale:
  - Gas: the three zones (below).
  - TEMP: 16..30 C. HUMIDITY: 20..80 %.
  - A joined line of the 5-minute means: white in GOOD, amber in FAIR, red in
    POOR; white for TEMP and HUMIDITY.
  - Gaps where no valid data exists, and a now-dot at the right end.
  - "-24H" left and "NOW" right in grey 14 px.
- **Page dots** centred at the bottom: six dots, the current one filled.

### While the gas sensor warms up (ENS160 validity 1 or 2) or errs (3)

The gas pages show no number:
- `WARMING UP` (or `GAS ERROR`) in grey vfont ~34.
- Under WARMING UP, `12 MIN SO FAR` in grey 22 px, counted from sensor start and
  redrawn each minute. Never a remaining time: the chip cannot know it. Nothing
  under GAS ERROR: a count going up there reads as progress that is not made.
- The strip still shows the valid history.

## The full 24 h chart (a tap on a reading page; mockups `ref/v2_voc_24h.png`, `v3_eco2_24h.png`)

- **Title slot** at (16, 8) white 14 px: "VOC 24H", with the unit on the line below in grey.
- **Top right:** the number (vfont 30) and state word (vfont 22), styled by
  state, plus the trend arrow when moving.
- **Plot** x 16..246 (231 columns of about 6.2 min), y 44..141, on a FIXED scale
  (ends 10 px short of the key, so the now-dot is not read as a bullet on it).
  - **Gas: three linear zones.**
    - GOOD 0-220 ppb / 400-800 ppm, 42 px.
    - FAIR 220-650 / 800-1000, 32 px.
    - POOR 650-2200 / 1000-1500, 24 px.
    - Values above the top are pinned.
    - A solid amber line at the FAIR edge and a solid red line at the POOR edge.
    - A faint blue tint over GOOD on the VOC chart only.
    - A key at x 256: POOR / 650 / FAIR / 220 / GOOD, in each state's colour
      and grey.
  - **TEMP:** fixed 16..30 C with grey reference lines at 20 and 25, labelled at the right.
  - **HUMIDITY:** fixed 20..80 % with lines at 30 and 60.
  - **Trace:** the mean of each column's valid 5-minute records, a joined 2 px
    `vec_line` coloured by state (white for TEMP/HUMIDITY). Nothing is drawn
    across a gap. It ends at the now-dot.
  - **Peak marker:** a triangle plus the stored 5-minute maximum on a black
    knock-out. Shown only if the day reached FAIR and the peak is more than
    20 px from now.
  - **Time axis:** 3 px ticks for 18 / 06 / 12. At midnight, a 1 px grey line
    through the plot and the weekday in white. "NOW" at the right.
- A tap, or any swipe, returns to the reading page. The detail view never stays
  up by itself past the screen sleeping.

## WEEK (mockup `ref/v5_week.png`)

- **One row per day** for 7 days, today at the bottom in white, and 24 hourly cells per row.
  - Each cell shows the worst state (of VOC and eCO2) that lasted at least 3 of
    its twelve 5-minute means.
  - OK is a 3 px blue bar, FAIR a 7 px amber bar, POOR a full-height red cell.
  - No data is a small grey dot; future hours are blank.
- **Takeaway** top right: "POOR 3H FAIR 14H" (hours with at least 15 min in
  that state), or "ALL GOOD".
- **Hour labels** 00 / 06 / 12 / 18 / 24.

## CLOCK

The existing clock page. Its small sensor line becomes the verdict: "AIR GOOD",
"VOC FAIR" or "ECO2 POOR", naming the worse gas (VOC on a tie), styled by state.

## States: one table drives everything

| State | Word | VOC ppb | eCO2 est ppm |
|---|---|---|---|
| OK | GOOD (not on eCO2) | < 220 | < 800 |
| FAIR | FAIR | 220-649 | 800-999 |
| POOR | POOR | >= 650 | >= 1000 |

- **Source:** the ENS160 datasheet v1.3, Tables 5-6. They replace today's
  inconsistent `voc_thresh {300,1000}` and `co2_thresh {800,1200}`.
- **Hysteresis:** worse at once; better only 10 % below the edge.
- **The display value** is the median of the last 4 valid 30 s readings. The
  number, word, colour, now-dot and verdict all come from it, unrounded.
- **eCO2 can make the verdict worse, never better.**
- **Readings flagged invalid** (validity != 0) never set a scale, a word or a
  colour; they are gaps.

## Data (firmware)

- **A 30 s RAM ring** of 96 entries (tvoc, eco2, aqi, validity, T, RH), fed by
  the existing 30 s sampling whether or not an SD card is present. It supplies
  the display value, the trend and the 5-minute means.
- **The flash record** (5 min) stores the MEAN of the valid 30 s readings in
  `tvoc_ppb` / `eco2_ppm`, and the 5-minute TVOC maximum in `hpa_x10`, with
  `ENV_HAVE_HPA` clear and a new flag bit `0x40` ("mean + peak"). Every reader
  checks `ENV_HAVE_HPA` first, so this is safe. If nothing in the window was
  valid, the record carries the latest validity and shows as a gap.
- **SD CSV:** unchanged except a trailing `aqi` column.
- **Brightness** while awake: `CONFIG_SCREEN_BRIGHTNESS=40` (was 20). The screen
  still sleeps after 5 min, and field sleep is unchanged.

## Code structure

- `main/envstate.c/.h` (new, pure, host-tested):
  - the limits table, state with hysteresis, and verdict;
  - the 30 s ring, display value (median), truncation and trend;
  - the 5-minute mean + peak.
- `main/envui.c/.h` (new, pure, host-tested and host-rendered):
  - draws a reading page, the full chart, the week grid and the warm-up state
    from a small data struct (24 h of 5-minute means + validity + peak, the
    display value, state, trend, page index);
  - no globals from main.c.
- `main.c`: feeds envstate, builds the envui data from envstore, owns the page
  set, touch semantics (tap = detail toggle, swipe = page), wake-to-VOC, the
  clock verdict, and the removals.

## Testing

- **Host:**
  - `test_envstate`: limits, hysteresis, median, truncation, trend, the gap and
    warm-up rules, and the mean/peak fold.
  - `test_envui`: bounds and canary checks; renders of every page in the good,
    fair, poor, warm-up and no-data states, and the full charts, at 320x172, as
    BMP -> PNG for review.
  - The existing envstore/envpage tests keep passing.
- **Renders** are compared with the research mockups and published to Reza's
  design page before flashing.
- **Board:** flash envo, then watch the log: display value, state and trend.
  Reza checks legibility across the room, tap to detail, swipe paging, and
  wake to VOC.
- **Regression:** lilly, wave and watch still build.
