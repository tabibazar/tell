# Graphics and Touch — Design

**Date:** 2026-09-06
**Status:** Approved for planning

Give the CrowPanel colour graphics and touch input: coloured charts of Claude
usage, a clock, and the last message, cycled by tapping the screen.

## Context

The boards currently render monochrome text sent as UTF-8 over BLE. The
CrowPanel is a full RGB565 framebuffer with a capacitive touch panel, neither of
which the firmware uses.

### What is already there

| Unit | Responsibility |
|---|---|
| `ble_uart` | NimBLE peripheral, GATT server, message reassembly |
| `textwrap` | Word wrap. Pure C |
| `canvas` | Framebuffer and glyph rendering. Panel independent, host tested |
| `timecalc` | Seconds-since-midnight arithmetic. Pure C |
| `display_st7789` / `display_rgb` | Panel transport, one file per board |

`tools/claude-stats.py` already aggregates usage from `~/.claude/projects` and
renders an ASCII block. Its aggregation is reused unchanged.

It gains a `--format` option: `ascii` (today's block, the default, so existing
use keeps working) or `data` (the marker lines below). Sending both pages is
then two invocations piped to `tell`, which the repo will wrap in a small
`tools/push-stats.sh` so it is one command in practice.

### Hardware

Touch is a **GT911** on I²C: **SCL 20, SDA 19**. Neither INT nor RST is wired
(both `-1` in Elecrow's configuration), so the controller must be **polled**,
and its I²C address may be `0x5D` or `0x14` depending on how RST floats at
power-on.

Elecrow's calibration maps X from 800→0 and Y from 480→0, implying both axes are
inverted. This does not matter here (see Non-goals).

### Constraint that shapes everything

**The Mac cannot send rendered frames.** A full 800×480 frame is 750 KB and BLE
moves roughly 10–20 KB/s — about a minute per frame. The firmware must own the
drawing; the Mac sends data.

## Architecture

Five units, extending the existing one-way dependency rule. `main` wires them
together; nothing above `display` knows about panels, and nothing above
`gt911` knows about I²C.

```
BLE message ──> usagedata ──┐
                            ├──> pages ──> canvas ──> display_*
touch (gt911) ──────────────┘
```

| Unit | Responsibility | Host testable |
|---|---|---|
| `gt911` | Poll the touch controller, debounce, report a tap | No |
| `pages` | Which page shows, what advances it, when the clock returns | **Yes** |
| `usagedata` | Parse `!stats` / `!daily` payloads into structs | **Yes** |
| `canvas` | Framebuffer, glyphs, rectangles, colour | **Yes** |
| `display_*` | Transport only, unchanged | No |

### Page model

```
CLOCK -> STATS -> DAILY -> MESSAGE -> CLOCK
```

- A tap advances one step.
- Arriving data jumps straight to its own page, so sending stats shows them.
- After **5 minutes** with no touch and no new message, the clock returns.
  This preserves the current idle behaviour.

The available page set comes from the board rather than being special-cased
through the code. The Feather has no touch and a 20×5 screen, so it offers only
`CLOCK` and `MESSAGE`, and pages advance there only when data arrives.

### Touch

Polled every **50 ms**, since no interrupt line exists. A tap fires on
**release**, debounced, so a resting finger does not cycle pages. Both I²C
addresses are probed at init and the one that answers is logged.

## Protocol

Data travels on the **existing text characteristic**, reusing its reassembly.
No new BLE surface.

A payload whose first line is a marker is data; anything else is a plain text
message exactly as today. An unrecognised marker is treated as text, so the
failure mode is "your data appeared as words", not a crash.

**One marker per message.** Each payload carries exactly one marker and
replaces exactly one page:

```
!stats
m opus-5 13000000 3300000000
m sonnet-5 1200000 429000000
```

```
!daily
d 09-01 571000000
d 09-02 282000000
```

Fields are whitespace-separated: `m <name> <output-tokens> <cache-read-tokens>`
and `d <label> <tokens>`. Each marker replaces its page's data **wholesale** —
no partial updates, so there is no merge logic and no stale-row problem.

Chosen over binary records (opaque, needs a struct kept in sync in two
languages) and JSON (a general parser for a fixed three-field schema). Text
lines keep the property that has repeatedly earned its keep on this project:
a page can be typed by hand from nRF Connect to tell firmware bugs from client
bugs.

Payloads are ~1 KB, within the 2048-byte message cap.

## Drawing

`canvas` gains exactly two things: a colour argument on the existing text
calls, and `canvas_fill_rect(x, y, w, h, colour)`. Bars, title bars and
separators are all filled rectangles. Anything richer — rounded corners,
gradients — buys nothing at arm's length on a 7" panel.

**One palette, defined once**, so pages cannot drift: near-black background,
white text, grey labels and axes, and four accent colours assigned to models in
rank order. A model keeps its colour across the stats and daily pages so the eye
carries meaning between them.

### Pages

- **Clock** — the existing large digits, from the dedicated 96×160 table.
- **Stats** — title bar, then one row per model: name, a proportional bar in
  its accent colour, right-aligned token count.
- **Daily** — 14 vertical bars, height proportional to tokens, dates beneath,
  peak value labelled. This is the view that justifies graphics; a sparkline
  reads instantly where a column of numbers does not.
- **Message** — the last text sent with `tell`, retained as a page rather than
  expiring.

### Colour will test the RGB pin order for the first time

White is all-bits-set and black all-bits-clear, so every permutation of the 16
data lines renders identically today. The pin order in `display_rgb.c` is
therefore **unverified**. The first coloured bar tests it: if reds come out
blue, the fix is reversing the channel groups in `RGB_DATA_PINS`.

The Feather has the same exposure for a different reason — RGB565 over SPI
usually needs byte-swapped pixels, which white-on-black also hides. It may need
`lsb_first` where the CrowPanel does not.

**Expect one round of colour correction per board.** This is predicted, not a
surprise to be debugged from scratch.

## Error handling

| Condition | Behaviour |
|---|---|
| Malformed data line | Skip that line, keep the rest. A bad row must not lose a good page. |
| More models than display slots | Show the top rows by rank, drop the rest. |
| Unknown marker | Treat the whole payload as a text message. |
| Number too large for the field | Clamp, do not wrap. |
| Empty data section | Show the page with a "no data" line rather than a blank screen. |
| GT911 absent or not answering | Log once, continue without touch. The display still works. |
| Touch held down | One tap per press, on release. |

## Testing

Three of the five units are pure, so host tests carry most of the weight.

1. **`usagedata`** — the highest-value target, because it parses input from the
   network. Malformed lines, missing fields, numbers past 2³², unknown markers,
   a payload truncated mid-line, more models than slots.
2. **`canvas`** — bounds clamping above all: rectangles partly off-screen,
   negative origins, widths past the panel. A primitive that writes outside the
   framebuffer corrupts adjacent heap, which surfaces later as random crashes.
3. **`pages`** — cycle order, wrapping, the reduced set on the Feather, the
   5-minute fallback, and data jumping to its own page.
4. **Touch** — hardware only: tap, observe the page change, one log line per tap.
5. **End to end** — send `!stats` and `!daily`, then tap through all four pages.

## Risks

Ranked by expected impact:

1. **Colour channel order**, both boards. Expect one correction each.
2. **GT911 address** — probing both `0x5D` and `0x14` removes the guess.
3. **Redraw cost per tap** — clearing and blitting 750 KB on every page change.
   Measure it; if it is slow enough to feel, redraw only the changed region.

**A risk that disappears on inspection:** touch calibration. Elecrow's mapping
inverts both axes, which would normally require a calibration pass — but a tap
*anywhere* advances the page, so the coordinates are never used. Recorded here
so it is not rediscovered as a problem later.

## Non-goals

- Touch zones, buttons, or hit-testing. Only "was there a tap".
- Touch calibration, which follows from the above.
- Drawing primitives beyond text and filled rectangles.
- Sending images or rendered frames from the Mac — ruled out by bandwidth.
- A Mac-side daemon. The board never asks the Mac for anything; data is pushed.
- Subscription or account data, which is not obtainable (see the ASCII stats
  work: nothing local records a plan, and there is no public API).
