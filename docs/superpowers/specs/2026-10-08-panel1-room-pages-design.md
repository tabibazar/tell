# panel1: a page per room

2026-10-08.

**Board:** panel1, a Waveshare ESP32-S3-Touch-LCD-4B (480x480 ST7701, GT911
touch). It shows the office's meeting room bookings, one day at a time, from
the Apps Script relay (`panel1/relay/rooms.gs`).

**Why:** the office has six rooms: Grande, Ristretto, Short1, Short2, Tall and
Venti. Six columns on 480 px are about 72 px each, too narrow for a booking's
title. The overview stays for the at-a-glance picture. Each room also gets its
own full-width page, where titles and who booked them can be read.

Chosen from three browser mockups (layout "A"). The others were room pages
only, and a "right now" list of rooms.

## Not in this

- **The missing Ristretto.** The relay reads `CalendarApp.getAllCalendars()`,
  which returns only the calendars on the relay account's own list. Ristretto
  is not on it. The fix is in Google Calendar, as that account: Other
  calendars, then +, then Browse resources, then tick Ristretto. No code.
  panel1's own limit (`ROOMS_MAX` 6) already fits all six rooms.
- **No relay change.** The JSON and `rooms.c` stay as they are.

## Getting around

- **Overview to a room:** on the overview, tap a room's name strip (the 40 px
  coloured strip under the header). That room's page opens on the same day.
- **On a room page:**
  - Swipe left or right: the previous or next working day, as on the overview.
    The same room stays open, Saturday and Sunday are skipped, and the 14-day
    span still applies.
  - Swipe up or down: the next or previous room, in the relay's order (by
    name). It wraps, so Venti is followed by Grande. A finger dragged up
    brings the next room in.
  - Tap the top of the page (the header or the status strip, the top 92 px):
    back to the overview.
  - Tap a booking: the booking card the overview already opens
    (`roomsui_detail`).
- **Idle:** after `IDLE_HOME` (60 s) untouched, panel1 goes home, to the
  overview on the home day. The screen saver and its wake-up go home the same
  way.
- **The day changes under an open room:** after a fetch the room is looked up
  by index. If that day has fewer rooms than the index, panel1 goes to the
  overview.

## The room page

On the 480 x 480 screen, top to bottom:

- **Header (52 px, as the overview):** the room's name in the big font, then
  in the dim font "6 seats · Today" (or "Tomorrow", or "Thu 8 Oct"), and the
  clock on the right. The seat count is left out when the room has none. An
  amber note ("offline since …") goes where it goes on the overview.
- **Status strip (40 px, full width, rounded like the overview's room
  headers):**
  - Today, while the room is in use: red, "Taken till 10:45 · then free till
    14:00". The "till" time runs on through back-to-back bookings, as the
    overview's does. "· then free till 14:00" is dropped when nothing follows
    that day, and dropped too when the line does not fit.
  - Today, while the room is free: green, "Free till 11:00", or "Free" when
    nothing more is booked.
  - Another day: grey, "3 bookings", "1 booking" or "No bookings".
- **Timeline:** the same hours as the overview: 8 to 16, stretched to take in
  any booking outside them (`roomsui_hours`). The hour figures are in the left
  gutter. Today has the red now-line, past bookings are dimmed and the one on
  now is lit, all as on the overview.
- **A booking:** spans the full width right of the gutter.
  - First line: "10:00–10:45 · Hiring sync". The time is in the small font and
    the title in the title font, cut with "…" when it does not fit.
  - Second line, when the block is at least 34 px tall: "Nora Ali · 4 invited"
    (just the organiser when nobody is invited).
  - A block 16-33 px tall gets the first line only, and one under 16 px is
    just the coloured block. At 8 to 16 an hour is 47 px, so a 30-minute
    booking gets one line and an hour gets two.
- **Which room:** a row of six small dots at the bottom centre, the current
  room's dot lit. It is drawn over the grid's last few pixels; at 16:00 that
  part of the grid is empty more often than not.

## Code

- **`roomsui.h` / `roomsui.c`** (pure, drawn on the host):
  - `void roomsui_room_draw(canvas_t *c, const rooms_day_t *day, int room, const roomsui_view_t *v)`:
    the room page. `day` NULL draws the header and an empty grid, as
    `roomsui_draw` does before the first fetch.
  - `int roomsui_head_hit(const rooms_day_t *day, int x, int y)`: the room
    whose name strip on the overview is under (x, y), or -1.
  - `const room_ev_t *roomsui_room_hit(const rooms_day_t *day, int room, int x, int y)`:
    the booking under (x, y) on that room's page, or NULL. It has the same
    finger margin as `roomsui_hit`.
  - `bool roomsui_room_head_hit(int x, int y)`: is (x, y) in the top 92 px of
    the room page (the way back)?
  - The overview's status wording and the booking blocks are shared with the
    room page where they can be, rather than copied.
- **`panel1.c`:**
  - `s_room`: -1 for the overview, else the open room's index.
  - The touch loop also tracks the last y. A swipe is horizontal or vertical,
    whichever way moved more, and only a swipe of at least `SWIPE_PX` counts.
    Today `moved` is set only by x movement; y movement must set it too, so a
    vertical drag is not taken as a tap.
  - Going home (idle, the saver waking, the home day moving) also sets
    `s_room = -1`.
  - `draw()` calls `roomsui_room_draw` when `s_room >= 0`.
  - The header comment describes the room pages.

## Testing

- **`host_tests/test_rooms.c`:**
  - Draws a room page (today with a booking on now; another day) to a PPM, to
    be looked at, as the overview is.
  - `roomsui_head_hit`: the centre of each of six name strips gives its index,
    and a point in the grid gives -1.
  - `roomsui_room_hit`: a booking's middle hits it, and an empty hour misses.
  - The status wording: taken with a free stretch after, taken till the end
    of the day, free till the next booking, free all day, another day.
- **On the board:** open each room from the overview, swipe up and down
  through all six and round, swipe days on a room page, tap back, and leave
  it for a minute to see it go home.
