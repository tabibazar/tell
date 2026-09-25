# envo environment UI: design proposal, revision 2

**Why he can't read the charts.** In envo's normal conditions, the VOC and eCO2 charts rescale so that about ±8 ppb of noise fills the whole plot. The value axis has no numbers, and nothing on the screen says whether the air is OK.

**The fix, unchanged from revision 1:**
- **A home screen that gives a verdict.**
- **Charts on a fixed scale taken from the chip's own limits.**

**What revision 2 changes after the critique:**
- **One verdict.** It names the reading at fault ("VOC FAIR"), and eCO2 can only make it worse.
- **Glance items sized for 70 cm.**
- **One number drives everything.** The number, word, colour and chart dot all come from the same value.
- **Three states and three words.** Each chart shows two lines and a key beside it.
- **Only honest words.** No invented countdowns or remaining times.
- **Nothing below 3:1 contrast carries data.**

**Mockups.** All of them are rendered with the firmware's real `canvas.c`, `vector.c` and `vfont.c` at 320x172. Every coordinate below comes from those renders.
- Code: `/private/tmp/envo-ui/mock2/mock2.c`. Build with `cc -std=c11 -I<repo>/main mock2.c <repo>/main/{canvas,textwrap,vector,vfont}.c -lm`.
- Renders: `/private/tmp/envo-ui/mock2/out/`. Each has a `_3x` twin, and `sheet_v2.png` shows all ten.
  - `v1_air_good`, `v6_air_voc_fair`, `v7_air_eco2_fair`, `v8_air_poor`, `v10_air_warmup`
  - `v2_voc_24h`, `v9_voc_24h_poor`, `v3_eco2_24h`, `v4_room`, `v5_week`
- The eCO2 data in the mock is **invented**: 400 + 0.85·(VOC−30) + 60·occupancy. It fills the pages; it does not model the ENS160.
- Approaches A and B still use the revision 1 renders: `/private/tmp/envo-ui/mock/out/a1_single_voc.png` and `b1_clock_home.png`.
- The repository was only read. Nothing was flashed or committed.

---

## 0. Response to the critique

| # | Point | Decision | Reason / what changed |
|---|---|---|---|
| — | Ship the chart fixes first and check them on the panel | **Accept** | Step 1: charts, storage and the display value. Step 2: the new home screen. |
| 1 | One signal gets two verdicts | **Accept, modified** | See the notes below this table. |
| 2 | The answer is below the legibility floor | **Accept; citation corrected** | Both documents said 16′. The source here (FAA HFDS 5.6.1, citing MIL-STD-1472F) says **at least 15′**. 20–22′ is the preferred size (HFDS 5.1.2.15, citing ANSI 1988). The critique's conclusion holds. Word raised to vfont **34** (23.6′ at 50 cm, 16.9′ at 70 cm). Secondary text is at least **22 px**, not 18: 18 px is 12.5′ at 50 cm, under the floor. The space comes from deleting the second word and both sparklines, as suggested. |
| 3 | Five estimators | **Accept** | **The display value** is the median of the last 4 valid 30 s readings. The state comes from that value, **unrounded**, through one table with the same relative hysteresis for both gases. The number, word, colour and now-dot all follow from it. The VOC word comes from ppb (datasheet Table 6); the `aqi` register is only logged, as a check. |
| 4 | 3:1 floor broken | **Accept the finding; one fix rejected** | Verified: envelope 2.45, OK cells 1.40, future outlines 1.43, tint 1.23 to 1. Envelope and outlines are dropped. I reject "fill the OK cells at ≥3:1": 168 cells of 3:1 blue hide the exceptions. Instead only exceptions get ink: an OK hour is a 3 px bar in `0x04BF` (6.8:1), FAIR is half height, POOR full height. Height replaces the 4×4 hole as the cue that doesn't rely on colour. The chart tint stays as decoration only (rule restated in §4). Wake brightness 40 is proposed as the default, but it is a battery trade, so it is an owner question. |
| 5 | Dishonest words | **Accept all** | The week takeaway becomes "POOR 3H FAIR 14H": hours with at least 15 min in that state. "COMFORTABLE" is gone, and ROOM shows no words until the offset is measured. The provisional −3.0 °C and the RH correction are applied now, marked "est". The 3 °C is the owner's estimate, not a measurement. **`ens160_compensate` stays as it is:** `main.c` feeds it the raw AHT21 reading on purpose ("the chip wants the air at its own surface"). The display correction is for display only. The step "send corrected values to `ens160_compensate`" from revision 1 is withdrawn. |
| 6 | Too many bands, words and line styles; the ladder is close to log; the gutters are too wide | **Accept, and go further; reject inline labels** | See the notes below this table. |
| 7 | The peak is neither clear nor the true peak | **Accept** | The label gets a black knock-out. It shows only if the day reached FAIR and the peak is more than 20 px from now. Storage change: see firmware change 4. |
| 8 | No data for an envelope | **Accept** | Envelope cut. With 234 columns over 288 records there are 1.2 records per column. |
| 9 | Trend buffer too short | **Accept** | Buffer of 96 × 30 s (48 min). No arrow until 40 minutes of valid data exist. |
| 10 | Double arrow never drawn | **Accept** | Bug confirmed (`(void)xy2`). One arrow, up or down, and **none when steady**. The band-edge clause is removed. |
| 11 | Warm-up text ticks and invents a remainder | **Accept; strings changed** | vfont has no `~` or `·`. Flags 1 and 2 share one message: "WARMING UP" plus "12 MIN SO FAR", counted from sensor start and redrawn each minute. Nothing about time remaining. Flag 3 shows "GAS ERROR". |
| 12 | Corner margin | **Accept** | All text now ends at x ≤ 304 and starts at x ≥ 16. |
| 13 | Month view vanishes | **Accept "say so"** | A 30-day strip under the week grid did not fit once the day rows had a readable 17 px spacing. The month view leaves the screen; the 90-day ring and SD log still hold the data. Owner question. |
| 14 | 600 ms grow animation | **Accept** | `ANIM_US` is off on the new pages. |
| 15 | Tap meaning on home | **Accept** | On AIR NOW, any tap goes to the next page. Elsewhere: left = back, right = forward. **Paging wraps**, because `pages_step/advance/back` all use `% PAGE_COUNT`. Swiping right from AIR NOW reaches CLOCK. |
| 16 | Idle return is a page changing on its own | **Accept** | Cut. Wake-to-home is enough. |
| 17 | No page dots; m2 and m3 look the same | **Accept** | Every page has a fixed title at (16, 8). The eCO2 page also looks different: no GOOD zone, no tint, subtitle "ppm, from VOCs". |
| 18 | Warm-up blanks home | **Accept** | Temperature and humidity are shown large during warm-up (`v10`). Confirmed: OPMODE is written only in `ens160_init` (`ens160.c:215–217`), so warm-up follows power-on only. |
| 19 | "OPEN A WINDOW" is wrong outdoors or in a car | **Accept the intent; only partly met** | "VENTILATE - FIND THE SOURCE" measures 414 px at 22 px, too wide for 288 px, and `·` can't be drawn. What ships is "VENTILATE SOON" (231 px) and "VENTILATE NOW" (215 px). "Find the source" is not on screen; owner question. Waking on POOR is off by default. |
| 20 | The last digit is noise | **Accept, with truncation** | VOC in 5 ppb steps below 100, 10 up to 1000, 100 above. eCO2 in 10 ppm steps. Values are **truncated**, so the number never shows an edge the reading hasn't reached: 218 shows 210, 1366 shows 1300. |
| — | Over-designed: sparklines, steady arrow, three warm-up messages, envelope | **Accept** | All removed. |
| — | Over-designed: CLOCK repeats the time | **Partly** | Home shows the time only in the 14 px bitmap font. CLOCK is existing code and sits one swipe right of home. Kept as the last page; owner question. |
| — | Missing: datasheet §8.1 baseline | **Accept** | Finding 13. "A constant source fades toward GOOD" is an **inference**; the datasheet doesn't state it. |
| — | Missing: TVOC is ethanol-calibrated | **Accept** | §16.2.11: DATA_ETOH is "a virtual mirror of the ethanol-calibrated DATA_TVOC register". Finding 6 revised. |
| — | Missing: 70 cm viewing distance | **Accept** | Used for the glance tier. Owner question. |
| — | Missing: eCO2 against TVOC on this unit | **Accept; can't be done here** | No SD CSVs are on this Mac. It is the first open question. |

**Notes on point 1 (one signal, two verdicts):**
- **One verdict** equal to the worse of the VOC and eCO2 states. eCO2 can make it worse, never better. eCO2 is never blue and never GOOD. The chip computes eCO2 "from measured VOCs plus hydrogen" (datasheet §5.2).
- **Change from the critique:** the number that caused the verdict is **not** coloured. The verdict names the reading instead: "VOC FAIR", "ECO2 POOR", or "AIR GOOD" when both are OK. Numbers stay white (21:1), so colour means status in one place only, and text satisfies WCAG 1.4.1 better than colour does. When both gases are equally bad, VOC is named, because it is his priority.
- **The separate eCO2 page stays for now,** until the eCO2-against-TVOC scatter from the SD log is plotted. The log already records `tvoc_ppb`, `eco2_ppm` and `gas_valid` every 30 s.
- **Rule for merging:** if eCO2 almost never reaches FAIR or POOR while TVOC is GOOD, drop the page and keep eCO2 as a number on home only.

**Notes on point 6 (bands, lines and gutters):**
- **Three zones, one line style.** Solid amber at 220 ppb / 800 ppm and solid red at 650 / 1000. The dotted 65/2200 and 600/1500 lines are gone.
- **The 65/600 breakpoint is dropped entirely.** It had been an invisible change of slope inside GOOD.
- **BAD is dropped as a word.** Three states, three words (GOOD, FAIR, POOR), three colours, three zones. Readings above 2200 ppb or 1500 ppm are pinned at the top of the chart, and the number or peak label gives the value.
- **The log-like spacing is accepted and dealt with.** The arithmetic holds (the ln gaps between edges were 1.22, 1.09, 1.22, 0.91). Now only two edges carry numbers, and the zones are visibly unequal in height, so there are no evenly spaced magnitude labels to misread.
- **Inline knock-out labels rejected.** I rendered them. At the left end they hid about the oldest 4 hours of the line at exactly the heights that matter, and they collided with the peak marker.
- **Instead, a key on the right,** aligned to the lines: POOR / 650 / FAIR / 220 / GOOD, at x 256–304. There is no left gutter, and the plot grows from 213 to 234 px.

---

## 1. Findings that matter most

1. **Monitors with a screen lead with one large current number and a status word or colour, not a chart.**
   - Only the Qingping Air Monitor 2 has on-device charts, one tap away on a 4" screen.
   - Sources: Aranet4, Airthings View Plus, Kaiterra Sensedge, AirGradient ONE, Qingping; breathesafeair.com reviews.
2. **Reviewers want a number, a word and a colour for each pollutant.**
   - They criticise a status light that doesn't say which reading is bad (Airthings Wave Plus, Netatmo).
   - They criticise VOC shown only as an arrow (IKEA Vindstyrka).
   - The Airthings View shows the reading that is bad. This supports putting the reading's name in the verdict.
3. **Three bands is the norm.**
   - Aranet, Airthings, GO IAQS and the UBA ventilation traffic light all use three.
   - Few: at most five ranges, "ideally three" (Bullet Graph Design Spec; UBA 2008 CO2 guideline, Tabelle 4).
4. **The chip's own limits** (ENS160 datasheet v1.3, pp. 10–11; checked against the text):
   - TVOC: 65 / 220 / 650 / 2200 ppb (Table 6).
   - eCO2: 600 / 800 / 1000 / 1500 ppm (Table 5).
   - This design uses only the 220/650 and 800/1000 edges.
5. **eCO2 is calculated from VOCs plus hydrogen, not measured** (datasheet §5.2, Figs. 3–4; ScioSense white paper).
   - It is largely the TVOC signal in another unit.
   - How closely the two track on this unit is still to be measured (open question 1).
6. **This chip's TVOC is ethanol-calibrated,** because DATA_ETOH mirrors DATA_TVOC (datasheet §16.2.11).
   - Its ppb values are ethanol-equivalent. They can't be compared with other brands' "ppb TVOC".
   - The limits are consistent only because they are the chip's own table.
   - The industry is moving to relative indexes (AirGradient; Sensirion VOC Index).
7. **Warm-up** (datasheet Table 10 and footnote 24):
   - 3 minutes after every power-on (flag 1).
   - Up to 1 hour of initial start-up (flag 2) until the chip has run 24 hours without a break.
   - If power is lost before then, the chip "will resume Initial Start-up". How much time remains is not knowable.
8. **Use a fixed axis range when the minimum and maximum mean something** (Apple HIG, Charts; Correll, Bertini and Franconeri, CHI 2020; Grafana soft min/max).
9. **Most people misread log axes:** 83.8% correct on linear charts against 40.7% on log charts (Romano et al. 2020).
10. **Shade or mark the normal range, and tie the latest value to the latest point** (Tufte; the AGP glucose report's target band).
11. **Text size, corrected.**
    - Minimum 15′ (FAA HFDS 5.6.1, citing MIL-STD-1472F). Preferred 20–22′ (HFDS 5.1.2.15, citing ANSI 1988).
    - At 0.101 mm per pixel, capitals need at least **22 px at 50 cm** (29–32 px preferred) and at least **31 px at 70 cm** (40–45 px preferred).
    - Today's text is 14 px, which is 9.7′ at 50 cm.
    - The 16′ figure in both earlier documents could not be checked against a source here and has been dropped.
12. **Colour must never be the only signal** (WCAG 1.4.1).
    - Blue and amber are at least 81 ΔE apart for every type of colour blindness.
    - Amber and red are only 23 ΔE apart for deutan viewers, which is why severity is also carried by words and heights (`sim/cvd.py`, Machado 2009).
13. **The chip corrects its own baseline automatically and stores it in non-volatile memory,** starting "from the latest valid level of background air after re-powering" (datasheet §8.1).
    - History is relative, not absolute.
    - A new field site starts from the old site's baseline.
    - A slow, steady source may be partly absorbed into the baseline. That last point is an inference; the datasheet doesn't state it.

## 2. Problems with today's pages, most important first

1. **The gas charts rescale to fit whatever the air did.**
   - With VOC at 30–49 ppb and eCO2 at 413–456 ppm, the log axis has **zero labels**.
   - About ±8 ppb of noise fills 94 px, while the title says "excellent" (`q1`, `q2`).
2. **Nothing gives a verdict.**
   - The only status word is 14 px white text in the VOC title bar.
   - Nothing changes colour when the air gets bad.
   - The clock page's "48ppb" looks the same at 1366 ppb.
3. **All text except the clock digits is 14 px (9.7′).** The largest type on the device shows the time.
4. **VOC, his top priority, is 4 swipes from home and eCO2 is 3.** The first swipe lands on a chip-temperature debug page, and waking returns to the last page shown.
5. **The eCO2 number prints over "CO2e"** (`envpage_draw` has no overlap check).
6. **Invalid readings are drawn as real ones.**
   - The warm-up spike sets the chart's ceiling.
   - 0 ppb during warm-up pulls the log axis down to 1.
7. **The limits disagree with each other and are never drawn.** The code has 300/1000 ppb and 800/1200 ppm, which don't match the chip's tables.
8. **Log-axis labels are snapped to 24 px text rows.** They float between gridlines or overlap.
9. **The week page is broken.**
   - It rotates on its own every 6 s.
   - A third of the time it shows "PRESSURE WEEK – no days logged yet".
   - It never shows VOC or eCO2.
   - Its bars have no axis.
10. **Colour identifies the reading, not the state.**
11. **The temperature axis is mislabelled** (26.5 is labelled "26"), and there are no units, no "now" marker and no midnight marker.
12. **The TREND page overlays two readings, each scaled to itself, with no axis.**
13. **The 30-day strip and the gridlines are 1.4–1.9:1 against black.**
14. **Temperature and humidity are not corrected.** The AHT21 reads about 3 °C warm, which also makes humidity read about 8 points low.

## 3. Three approaches

### A. One reading per page, Aranet style (`a1_single_voc.png`, revision 1 render)
- **Pages:** VOC → eCO2 → TEMP → RH → WEEK → CLOCK.
- **Each page:** the number in vfont at 64 px (44′ at 50 cm, 22′ at 1 m) and a word at 24 px.
- **Pros:** the largest type, readable across the room; one purpose per page.
- **Cons:**
  - The verdict is split across pages.
  - "When was it bad?" rests on a thin strip.
  - Comparing readings takes 6 swipes.
- **Keep A in mind if** open question 2 answers "across the room".

### B. Fix the charts and keep the clock as home (`b1_clock_home.png`)
- **Clock page:** gets a verdict line, "AIR GOOD", in vfont at 18 px.
- **Chart pages:** as in C.
- **Pros:** cheapest; keeps his current mental map of the pages.
- **Cons:** the time is still the largest thing on screen, and the verdict is 18 px (12.5′ at 50 cm, under the floor).

### C. A verdict on home, fixed-zone charts behind it (RECOMMENDED; `v1`–`v10`)
- **Pages:** AIR NOW (home) → VOC 24H → eCO2 est 24H (until the scatter decides) → ROOM 24H → WEEK → CLOCK, then back to AIR NOW. TEMPS appears only in debug builds.
- **It answers his three questions:**
  - **OK now?** The verdict: 34 px, which is 16.9′ at 70 cm.
  - **Getting better or worse?** The arrow beside the VOC number, shown only when the reading is moving.
  - **When was it bad?** The 24H chart with its peak and midnight line, then the WEEK grid.
- **Ships in two steps:**
  1. The chart pages, 5-minute means and the display value.
  2. The AIR NOW home screen.
- **Rejected:**
  - A dashboard with no charts: it fails "when was it bad?".
  - Round gauges: Few; Blascheck et al., IEEE TVCG 2019.

### Rules the recommended design depends on

**Readability tiers** (capital height; minutes of arc at 50 cm / 70 cm)

| Tier | Items | Size |
|---|---|---|
| Glance, at 70 cm | Verdict (34 px: 23.6′ / 16.9′); VOC number (46 px: 31.9′ / 22.8′) | At least 31 px |
| Read on purpose, at 50 cm or closer | eCO2 number (30 px: 20.8′ / 14.9′); advice and room row (22 px: 15.3′ / 10.9′); chart header number (30 px, shrinking to no less than 22); chart word (22 px); ROOM values (30 px) | At least 22 px |
| Labels (14 px bitmap, 9.7′) | Titles, units, key, axis ticks, time on home, week takeaway | Never the only place a verdict appears |

**States: three states, three words**

| State | Word | Look | VOC (ppb) | eCO2 est (ppm) |
|---|---|---|---|---|
| OK | GOOD. The eCO2 page shows no word. | Blue `0x04BF` text on black (6.8:1) | below 220 | below 800. Never labelled GOOD; it simply doesn't make the verdict worse. |
| FAIR | FAIR | Amber `0xFD40` text on black (11:1) | 220–649 | 800–999 |
| POOR | POOR | **Black text on a red `0xF8C1` block** (5.4:1) | 650 or more | 1000 or more |
| WAIT | WARMING UP (flag 1 or 2), GAS ERROR (flag 3) | Grey `0x8410` (5.5:1), no status colour | — | — |

- **Hysteresis:** the state gets worse as soon as the display value crosses an edge. It improves only after the value falls 10% below the edge: VOC below 198 or 585, eCO2 below 720 or 900.
- **By design,** the number and the word can disagree inside that 10% (for example "210 FAIR" while falling). This stops the word flickering.
- **Verdict** = the worse of the two states. The reading at fault is named; VOC is named when they tie. When both are OK, the verdict reads "AIR GOOD".
- **eCO2 turns POOR at 1000 ppm** (UBA says to ventilate from 1000; the datasheet agrees). The looser 1400 is an owner question.
- **Temperature and humidity:** no words until the AHT21 offset is measured.

**Display value, numbers and trend**
- **Display value:** the median of the last 4 valid 30 s readings (fewer right after warm-up).
- **Numbers:** the display value truncated to its step (VOC 5/10/100 ppb, eCO2 10 ppm).
- **Arrow:**
  - Compares the mean of the last 10 minutes with the mean of the 10 minutes ending 30 minutes ago.
  - VOC: shown when the change is at least max(15 ppb, 25%). eCO2: at least 50 ppm.
  - One head, up or down; nothing when steady.
  - It shows the trend of the number it sits beside.
  - Not drawn until 40 minutes of valid data exist.
- **Redraw only** when a 30 s sample arrives or the minute changes.

**Chart scale for the gases**
- **Three zones, each linear inside:**
  - GOOD: 0–220 ppb (400–800 ppm), 42 px.
  - FAIR: 220–650 (800–1000), 32 px.
  - POOR: 650–2200 (1000–1500), 24 px.
  - Values above the top are pinned there.
- **The scale never changes.** The change of slope at a zone edge always happens at a drawn, labelled line. Scale per pixel: VOC 5.2, 13.4 and 64.6 ppb per px; eCO2 9.5, 6.3 and 20.8 ppm per px.
- **What he would actually see:**
  - VOC at 30–49 ppb sits 6–9 px above the floor, and ±8 ppb of noise moves the line about ±1.5 px.
  - eCO2 at 413–456 ppm sits 1–6 px above the floor.
  - The result is a flat, low line in the blue zone.

**Colour**
- Colour means status and nothing else.
- Red appears only as a block with black text, or as a line, trace or key entry beside the word POOR.
- Marks that carry information are at least 3:1 against black. The GOOD-zone tint (`0x00E7`, 1.23:1) only repeats what the amber line and the key already say, so it may fade away at low brightness.
- Wake brightness 40% is proposed; see open question 5.

**Invalid readings**
- A sample with gas validity ≠ 0 becomes a gap (hatched `0x5ACB`).
- It never sets a scale, a word or a colour.

**Navigation**
- Swipe left or tap the right half: next page. Swipe right or tap the left half: previous page. On AIR NOW, **any tap** goes to the next page.
- Paging wraps.
- Waking always opens AIR NOW, and the tap that wakes the screen is swallowed (`s_wake_grace_us`). `home_page()` returns AIR NOW on envo.
- Nothing rotates on its own, and there is no idle return.
- Waking the screen when the air turns POOR is off by default.

### Pixel layout for the recommended approach (320x172 landscape)

Every y is the **top of the capitals' ink**:
- For `vfont_draw`, pass anchor y = top + size/2.
- For `canvas_puts_px` at 1x, pass y = top − 4.

Text stays within x 16–304.

**AIR NOW: `v1` good, `v6` VOC FAIR, `v7` ECO2 FAIR, `v8` POOR, `v10` warm-up**

| Region | Where | Content |
|---|---|---|
| Title row | top 8 | "AIR NOW" at 1x grey, x 16. The time at 1x grey, right-aligned to 304. |
| Verdict | top 28, ink 28–62 | vfont 34, weight 5.1. The name in white at x 16: AIR 57 px, VOC 84, ECO2 107. The state word starts 20 px after the name: GOOD 122, FAIR 82, POOR 115. The POOR block is padded 7 px (y 21–69). Widest case, "ECO2 POOR": its block ends at x 265. |
| Label row | top 74 | "VOC ppb" at 1x grey, x 16. "eCO2 est ppm" at 1x grey, right-aligned to 304 (x 160–304). |
| VOC number | bottom 140; top 94 at size 46 | vfont 46, weight 6, white, x 16. Steps down 2 px while wider than 190 (minimum 34). Measured widths: "45" 64, "260" 100, "1300" 123, "12000" 159 px. |
| Arrow | 24 px box centred on (16 + number width + 20, 140 − size/2) | White `vec_polygon`, only when moving. |
| eCO2 number | right-aligned to 304, top 110, ink 110–140 | vfont 30, weight 4.5, white. "440" 67 px, "1550" 80 px. |
| Bottom row, OK | top 148, ink 148–170 | "ROOM" at 1x grey (16, 152). Then, from x 70 in vfont 22 white: the temperature, a `vec_ring` degree sign, "C", a 13 px gap, the humidity. "est" at 1x grey 8 px after, until the offset is measured. |
| Bottom row, FAIR | top 148 | "VENTILATE SOON" in vfont 22, weight 3.3, amber, x 16 (231 px). |
| Bottom row, POOR | red fill y 143–171, full width | "VENTILATE NOW" in vfont 22, black, centred on 160 (215 px). |
| Warm-up / error | same grid | Verdict slot "WARMING UP" in grey (263 px) or "GAS ERROR" (239 px). Label row "TEMP est" / "HUMIDITY est". Temperature in vfont 46 at x 16, with a ring and "C" at 1x grey. Humidity in vfont 46, right-aligned to 304. Bottom row "12 MIN SO FAR" in vfont 22 grey (191 px). |

Temperature and humidity here come from the same corrected series as the ROOM page.

**VOC 24H and eCO2 est 24H: `v2`, `v9`, `v3`**

| Region | Where | Content |
|---|---|---|
| Title slot | (16, 8) white; (16, 24) grey | "VOC 24H" with "ppb" below, or "eCO2 est 24H" with "ppm, from VOCs" below. All 1x. |
| Word | right-aligned to 304, top 12 | vfont 22, weight 3.3, styled by state. The POOR block is padded 4 px (y 8–38). The eCO2 page shows no word below 800. |
| Arrow | 18 px box centred on (word left − 19, 23) | Only when moving. |
| Number | right-aligned 10 px left of the word or arrow, top 8 | vfont 30, weight 4.5, white. Steps down 2 px (minimum 22) until its left edge is at least 12 px right of the title. "1300" beside an arrow and POOR fits at 30, left edge x 112. |
| Plot | x 16–249 (234 columns, 6.2 min each), y 44–141 | Floor line at y 142 in `0x5ACB`. GOOD zone y 100–141; FAIR y 68–99; POOR y 44–67. Solid amber line at y 100 (220 / 800), solid red at y 68 (650 / 1000). Tint `0x00E7` over y 100–141 on the VOC page only. |
| Key | x 256, 1x | "POOR" in red, top 45. "650" / "1000" in grey, top 61. "FAIR" in amber, top 77. "220" / "800" in grey, top 93. "GOOD" in blue, top 114, VOC page only. |
| Trace | inside the plot | The mean of each column's valid 5-minute records, drawn as a joined 2 px `vec_line`: white in GOOD, amber in FAIR, red in POOR. Nothing is drawn across a gap. The last segment runs to the **now-dot** (white, radius 3.5, at x 249.5), placed at the display value and coloured by its state. |
| Peak | only if the day reached FAIR and the peak is more than 20 px left of now | A white triangle 8 px wide and 6 px tall, 3 px above the point. Beside it, the stored 5-minute maximum at 1x white on a black knock-out (2 px pad), at top = point − 16 (never above the plot's top). Placed right of the triangle, or left if there's no room. |
| Time axis | top 150 | "18", "06", "12" at 1x grey on 3 px ticks. Midnight gets a solid 1 px grey line through the plot and the weekday in white. "NOW" in white, right-aligned to 249. Any label that would touch "NOW" is dropped. |

**ROOM 24H: `v4`**

| Region | Where | Content |
|---|---|---|
| Title | top 8 | "ROOM 24H" at 1x white, then "est, -3.0C" at 1x grey 12 px after it, until the offset is measured. |
| Temperature | label (16, 30); value top 48 | "TEMP" at 1x grey. Value in vfont 30, weight 4.5, then a `vec_ring` degree sign and "C" at 1x grey. |
| Temperature chart | x 132–276, y 30–82 | Fixed 16–30 °C. Solid `0x5ACB` reference lines at 20 and 25, labelled at 1x grey at x 282. |
| Divider | y 90, x 16–304 | `0x5ACB` |
| Humidity | label (16, 98); value top 116; chart y 98–146 | Fixed 20–80 %, lines at 30 and 60. |
| Time labels | top 152 | "-24H" in grey at x 132; "NOW" in white, right-aligned to 276. |

No state words appear until the offset is measured.

**WEEK: `v5`**

| Region | Where | Content |
|---|---|---|
| Title | (16, 8) | "WEEK" at 1x white. |
| Takeaway | right-aligned to 304, top 8 | "POOR nH" in red, then "FAIR nH" in amber, 24 px apart, at 1x. Zero counts are left out; "ALL GOOD" in blue when both are zero. |
| Day labels | x 16, top 30 + 17·day | Two letters at 1x. Today in white, the rest grey. |
| Cells | x 44 + 11·hour, y 31 + 17·day, 9×13 px | OK: a 3 px blue bar at the bottom. FAIR: a 7 px amber bar. POOR: the full 13 px in red. No data: a 2×2 `0x5ACB` dot at the centre. Hours still to come: nothing. |
| Hour labels | top 151 | 00 / 06 / 12 / 18 / 24 at 1x grey. |

**How a cell is chosen:** each hour shows the worst state that lasted at least 3 of its 12 five-minute means, taking the worse of VOC and eCO2.

**CLOCK:** the existing page, with its sensor line replaced by the verdict line.

### Firmware changes

1. **Build the new pages with `vfont_draw` and `vec_*`.** `canvas_big_at` clears the whole canvas, so it can't be used on a page with other elements.
2. **vfont's limits.** Its glyphs are only A–Z, 0–9, space and `. , : - / + % \`.
   - Draw the degree sign with `vec_ring` and the arrow with `vec_polygon`.
   - Lower case and `~` go in the 12x24 bitmap font.
   - This is why the verdict reads "ECO2" while the label row reads "eCO2".
3. **A 30 s RAM ring of 96 entries** (tvoc, eco2, aqi, validity).
   - It is fed by a 30 s sampler that runs whether or not an SD card is present.
   - It supplies the display value, the arrow and the 5-minute mean.
4. **Flash record.** At each 5-minute tick:
   - Store the **mean of the valid 30 s readings** in `tvoc_ppb` and `eco2_ppm`.
   - Store the **5-minute TVOC maximum** in `hpa_x10`, with `ENV_HAVE_HPA` clear and a new flag bit `0x40`. The bit is free: `0x0F` are the "have" bits, `0x30` validity, `0x80` synthetic.
   - The flag also marks new records as means, which separates them from the older single readings.
   - If no reading in the window was valid, store the latest validity flag. It shows as a gap.
   - **Safe today:** every reader checks `ENV_HAVE_HPA` first (`envstore.c:63`, `envstore.c:106`, `main.c:2628`), and `tools/envlog.py` parses the older 12-byte ENV1 layout, not this ring.
   - **Known trade-off:** a future board with both a barometer and an ENS160 would collide. Change the record format then.
5. **`envchart_t`:**
   - Add `int32_t sum[]` and `uint16_t n[]` for the mean.
   - Set `ENVCHART_COLS` to 234.
   - Skip samples with validity ≠ 0.
   - Remove the log scale for the gases.
6. **One limits table:** VOC 220 / 650 (top 2200) and eCO2 800 / 1000 (top 1500). It replaces `voc_thresh {300,1000}` and `co2_thresh {800,1200}` near `main.c:2505`.
7. **SD CSV:** add an `aqi` column at the end. `pressure_hpa` stays empty because `envcsv_line` checks `ENV_HAVE_HPA`.
8. **AHT21:** correct the display only.
   - T = T_raw − Δ.
   - RH = RH_raw × es(T_raw)/es(T), where es is the saturation vapour pressure.
   - Measure Δ against a reference thermometer, once with the screen on and once asleep.
   - `ens160_compensate` keeps getting the raw reading.
9. **Delete** TREND, the pressure week and the week page's rotation. Move TEMPS to debug builds. Turn off `ANIM_US` on the new pages.
10. **Before field use,** run envo on USB for 24 hours without a break, so the ENS160 finishes its initial start-up.

## 4. What must NOT be done

- **Scaling a chart to fit its own data** (Correll et al., CHI 2020; Apple HIG, Charts).
- **Log value axes, or evenly spaced labels on a scale that works like a log,** for a lay reader (Romano et al. 2020).
- **A change of slope or scale inside a zone with no line to show it.**
- **White text on a coloured fill.** White on amber is 1.9:1 and white on blue 3.1:1. Use coloured text on black, or black text on a fill.
- **Colour as the only signal.** This includes marking the bad reading by colouring its number: name it instead. Amber next to red always needs a second cue (a word or a height).
- **Giving each reading its own colour.** Colour means status only.
- **A status that doesn't say which reading is bad;** VOC shown only as an arrow or index.
- **Pages that change on their own, idle returns, and axes that change without notice.**
- **Round gauges on a panel 172 px tall.**
- **Charting, colouring or classifying readings the chip flags as invalid;** showing warm-up output as "0 ppb".
- **Countdowns the firmware can't know,** or seconds ticking outside CLOCK.
- **Calling eCO2 "CO2", or letting eCO2 make the verdict better.**
- **Comfort words on uncorrected or estimated AHT21 readings.**
- **Words the font can't draw** (`·`, `~`, °, lower case in vfont), and strings not measured on the real font.
- **The verdict or the headline number below 15′ at the design distance;** critical information only in 14 px text.
- **Marks that carry information below 3:1,** such as `pal_darken()` on data. Only a tint that repeats a stronger cue is allowed below that.
- **Labels knocked out over data, or text within 16 px of the side edges.**
- **Classifying the rounded number,** or estimating the same reading in different ways on one screen.
- **Limits that disagree between the word, the lines and the key.** Keep one table.

## 5. Open questions only the owner can answer

1. **eCO2 as a chart page, or only a number?** Pull a few days of SD CSVs and plot eCO2 against TVOC. If eCO2 almost never reaches FAIR or POOR while TVOC is GOOD, drop the eCO2 24H page. Separately: does he want eCO2 on screen at all?
2. **Where does envo sit?** On a desk (50–70 cm), on a shelf, or across the room? If it's read from more than 1 m, approach A's larger type wins.
3. **eCO2 POOR at 1000 ppm** (UBA and the datasheet) **or 1400 ppm** (Aranet and GO IAQS)?
4. **Should the screen wake for 60 s when the air turns POOR?** It's off by default. If on, set quiet hours (proposed 22:00–07:00).
5. **Wake brightness 40% instead of today's 20%** on the power bank? The panel is the largest steady power draw; the 5-minute sleep limits the cost.
6. **Does he use today's month charts?** If yes, add a 30-DAYS page with one cell per day.
7. **A reference thermometer for the AHT21 offset.** Where did the "~3 °C warm" figure come from?
8. **Keep the CLOCK page** (one swipe right of home), or drop it?
9. **Temperature and humidity on home** when the air is good: wanted, or should home show only the gases?
10. **At POOR, is "FIND THE SOURCE" worth a second line?** It would replace the label row.
11. **Should ≥2200 ppb get its own word (BAD),** or is POOR plus the number enough?
12. **Can envo run 24 hours on USB before its first field trip?**