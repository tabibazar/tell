#include "roomsui.h"

#include <stdio.h>
#include <string.h>

#include "aafont.h"

/* panel1/main/fonts.c */
extern const aafont_t aafont_rooms_head;    /* Inter SemiBold 30: the day, the clock */
extern const aafont_t aafont_rooms_name;    /* Inter Bold 16: a room's name */
extern const aafont_t aafont_rooms_title;   /* Inter SemiBold 17: a booking's title */
extern const aafont_t aafont_rooms_small;   /* Inter Medium 15: organisers, hours, states */

#define W 480
#define H 480

#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define C_BG       RGB(0x0E, 0x10, 0x14)
#define C_TEXT     RGB(0xF2, 0xF4, 0xF7)
#define C_DIM      RGB(0x8A, 0x92, 0x9E)
#define C_FAINT    RGB(0x55, 0x5C, 0x66)
#define C_RULE     RGB(0x24, 0x28, 0x30)
#define C_HALF     RGB(0x1A, 0x1D, 0x23)
#define C_AMBER    RGB(0xF5, 0xB0, 0x41)
#define C_NOW      RGB(0xFF, 0x4D, 0x4D)

#define C_HEAD     RGB(0x22, 0x26, 0x2E)   /* a room's header on another day */
#define C_FREE     RGB(0x1E, 0x7A, 0x4C)
#define C_BUSY     RGB(0xB3, 0x36, 0x36)

#define C_EV       RGB(0x2A, 0x4E, 0x8C)   /* a booking to come */
#define C_EV_BAR   RGB(0x6E, 0xA8, 0xFF)
#define C_EV_WHO   RGB(0xB8, 0xCB, 0xEA)
#define C_ON       RGB(0x3B, 0x6F, 0xD0)   /* the booking on now */
#define C_ON_BAR   RGB(0xFF, 0xFF, 0xFF)
#define C_PAST     RGB(0x23, 0x27, 0x2F)   /* one that is over */
#define C_PAST_BAR RGB(0x4A, 0x50, 0x5A)

/* The frame. */
#define HEAD_H   52                 /* day and clock */
#define ROOM_Y   HEAD_H
#define ROOM_H   40                 /* a room's name and state */
#define GRID_Y   (ROOM_Y + ROOM_H + 6)
#define GRID_B   (H - 6)
#define GUTTER   26                 /* hour figures */
#define COL_GAP  4
#define MARGIN   4

static const char *const WDAY[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const MON[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* A filled rectangle with its corners rounded by r (no anti-aliasing: at
   r = 4 the steps are a pixel each and read as round). */
static void round_rect(canvas_t *c, int x, int y, int w, int h, int r, uint16_t col)
{
    if (w <= 0 || h <= 0) return;
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    canvas_fill_rect(c, x, y + r, w, h - 2 * r, col);
    for (int i = 0; i < r; i++) {
        /* Row i from the top edge: inset where (r-i-0.5)^2 + (r-k-0.5)^2 > r^2. */
        int dy = r - i;
        int k = 0;
        while (k < r && (r - k) * (r - k) + dy * dy > r * r + r) k++;
        canvas_fill_rect(c, x + k, y + i, w - 2 * k, 1, col);
        canvas_fill_rect(c, x + k, y + h - 1 - i, w - 2 * k, 1, col);
    }
}

/* `s` cut to fit `maxw` with "..." on the end, into out. */
static void ellipsize(const aafont_t *f, const char *s, int maxw, char *out, size_t n)
{
    snprintf(out, n, "%s", s);
    if (aafont_width(f, out) <= maxw) return;
    size_t len = strlen(out);
    while (len > 0) {
        out[--len] = 0;
        while (len > 0 && out[len - 1] == ' ') out[--len] = 0;
        char t[80];
        snprintf(t, sizeof t, "%s...", out);
        size_t tl = strlen(t);
        if (tl < n && aafont_width(f, t) <= maxw) {
            memcpy(out, t, tl + 1);
            return;
        }
    }
    out[0] = 0;
}

/* Greedy word wrap of `s` into at most `max` lines of `maxw` pixels; the last
   line takes the rest, ellipsized. A word wider than a line is cut. Returns
   the lines used. */
#define LINE_LEN 64
static int wrap(const aafont_t *f, const char *s, int maxw, char lines[][LINE_LEN], int max)
{
    int used = 0;
    while (*s == ' ') s++;
    while (*s && used < max) {
        if (used == max - 1) {
            ellipsize(f, s, maxw, lines[used], LINE_LEN);
            return used + 1;
        }
        /* The longest run of whole words that fits. */
        char line[LINE_LEN];
        int best = 0, i = 0;
        while (s[i] && i < LINE_LEN - 1) {
            int j = i;
            while (s[j] && s[j] != ' ' && j < LINE_LEN - 1) j++;
            memcpy(line, s, j);
            line[j] = 0;
            if (aafont_width(f, line) > maxw) break;
            best = j;
            if (!s[j]) break;
            i = j + 1;
        }
        if (best == 0) {
            /* One word too long for the line: as many letters as fit. */
            int j = 1;
            while (s[j] && s[j] != ' ' && j < LINE_LEN - 1) {
                memcpy(line, s, j + 1);
                line[j + 1] = 0;
                if (aafont_width(f, line) > maxw) break;
                j++;
            }
            best = j;
        }
        memcpy(lines[used], s, best);
        lines[used][best] = 0;
        used++;
        s += best;
        while (*s == ' ') s++;
    }
    return used;
}

void roomsui_hours(const rooms_day_t *day, int *first, int *last)
{
    int a = 9 * 60, b = 17 * 60;
    for (int r = 0; day && r < day->nrooms; r++) {
        for (int i = 0; i < day->room[r].n; i++) {
            if (day->room[r].ev[i].start < a) a = day->room[r].ev[i].start;
            if (day->room[r].ev[i].end > b) b = day->room[r].ev[i].end;
        }
    }
    *first = a / 60;
    *last = (b + 59) / 60;
}

static void draw_head(canvas_t *c, const roomsui_view_t *v, const rooms_day_t *day)
{
    const aafont_t *big = &aafont_rooms_head, *sub = &aafont_inter_sub;
    const char *word = v->offset == 0 ? "Today" : v->offset == 1 ? "Tomorrow"
                     : v->offset == -1 ? "Yesterday" : WDAY[v->wday % 7];
    int y = (HEAD_H - big->cap) / 2;
    int x = 14 + aafont_draw(c, big, 14, y, word, C_TEXT, AAFONT_LEFT) + 12;
    char date[32];
    if (day) {
        if (v->offset >= -1 && v->offset <= 1)
            snprintf(date, sizeof date, "%.3s %d %s", WDAY[v->wday % 7], day->d, MON[(day->m + 11) % 12]);
        else
            snprintf(date, sizeof date, "%d %s", day->d, MON[(day->m + 11) % 12]);
    } else {
        date[0] = 0;
    }
    int base = y + big->cap;    /* the date sits on the big word's baseline */
    if (date[0]) x += aafont_draw(c, sub, x, base - sub->cap, date, C_DIM, AAFONT_LEFT) + 12;

    int right = W - 14;
    if (v->now >= 0) {
        char clk[12];
        snprintf(clk, sizeof clk, "%d:%02d", v->now / 60, v->now % 60);
        right -= aafont_draw(c, big, right, y, clk, C_TEXT, AAFONT_RIGHT | AAFONT_ADVANCE) + 14;
    }
    if (v->note && v->note[0]) {
        char t[48];
        ellipsize(&aafont_inter_label, v->note, right - x, t, sizeof t);
        aafont_draw(c, &aafont_inter_label, right, base - aafont_inter_label.cap, t, C_AMBER, AAFONT_RIGHT);
    }
}

static void col_span(int nrooms, int i, int *x, int *w)
{
    int avail = W - GUTTER - MARGIN - COL_GAP * (nrooms - 1);
    int x0 = GUTTER + i * (avail / nrooms + COL_GAP);
    *x = x0;
    *w = avail / nrooms;
}

static void draw_room_head(canvas_t *c, const room_t *r, int x, int w, const roomsui_view_t *v)
{
    bool today = v->offset == 0 && v->now >= 0;
    const room_ev_t *on = today ? rooms_at(r, v->now) : NULL;
    uint16_t bg = !today ? C_HEAD : on ? C_BUSY : C_FREE;
    round_rect(c, x, ROOM_Y, w, ROOM_H, 5, bg);

    const aafont_t *nf = &aafont_rooms_name, *lf = &aafont_rooms_small;
    char name[24];
    ellipsize(nf, r->name, w - 10, name, sizeof name);
    aafont_draw(c, nf, x + w / 2, ROOM_Y + 6, name, C_TEXT, AAFONT_CENTRE);

    /* What the room is doing: till when it is taken or free, today; how many
       seats and bookings, another day. */
    char line[32];
    if (today) {
        const room_ev_t *nx = rooms_next(r, v->now);
        if (on) {
            /* Back-to-back bookings run on: the room is taken till the last. */
            int till = on->end;
            for (int i = 0; i < r->n; i++)
                if (r->ev[i].start <= till && r->ev[i].end > till) till = r->ev[i].end;
            snprintf(line, sizeof line, "till %d:%02d", till / 60, till % 60);
        } else if (nx) {
            snprintf(line, sizeof line, "free till %d:%02d", nx->start / 60, nx->start % 60);
            if (aafont_width(lf, line) > w - 8)
                snprintf(line, sizeof line, "till %d:%02d", nx->start / 60, nx->start % 60);
        } else {
            snprintf(line, sizeof line, "free");
        }
    } else if (r->cap) {
        snprintf(line, sizeof line, "%d seats", r->cap);
    } else {
        line[0] = 0;
    }
    aafont_draw(c, lf, x + w / 2, ROOM_Y + 24, line, today ? C_TEXT : C_DIM, AAFONT_CENTRE);
}

static void draw_booking(canvas_t *c, const room_ev_t *e, int x, int y, int w, int h, int state)
{
    /* state: 0 to come, 1 on now, 2 over */
    uint16_t bg = state == 1 ? C_ON : state == 2 ? C_PAST : C_EV;
    uint16_t bar = state == 1 ? C_ON_BAR : state == 2 ? C_PAST_BAR : C_EV_BAR;
    uint16_t fg = state == 2 ? C_DIM : C_TEXT;
    uint16_t who = state == 2 ? C_FAINT : C_EV_WHO;
    round_rect(c, x, y, w, h, 4, bg);
    canvas_fill_rect(c, x, y + 1, 3, h - 2, bar);

    const aafont_t *tf = &aafont_rooms_title, *lf = &aafont_rooms_small;
    const int pitch = 17, wpitch = 16;   /* title line to title line; title to organiser */
    int inner = w - 10;
    int pad = h >= tf->cap + 12 ? 4 : h >= tf->cap + 2 ? 1 : 0;
    int room = h - 2 * pad;
    if (room < tf->cap) return;          /* a sliver: the block says enough */

    /* The organiser gets the second line when there is one; any more lines
       go to the title, up to three. */
    bool show_who = e->who[0] && room >= tf->cap + wpitch;
    int max = 1 + (room - tf->cap - (show_who ? wpitch : 0)) / pitch;
    if (max > 3) max = 3;
    char lines[3][LINE_LEN];
    int n = wrap(tf, e->title, inner, lines, max);
    if (n < 1) return;

    int used = tf->cap + (n - 1) * pitch + (show_who ? wpitch : 0);
    int ty = n == 1 && !show_who ? y + (h - tf->cap) / 2 : y + pad + (room - used > 6 ? 3 : (room - used) / 2);
    for (int i = 0; i < n; i++) aafont_draw(c, tf, x + 7, ty + i * pitch, lines[i], fg, AAFONT_LEFT);
    if (show_who) {
        char t[32];
        ellipsize(lf, e->who, inner, t, sizeof t);
        aafont_draw(c, lf, x + 7, ty + (n - 1) * pitch + wpitch, t, who, AAFONT_LEFT);
    }
}

void roomsui_draw(canvas_t *c, const rooms_day_t *day, const roomsui_view_t *v)
{
    canvas_fill_rect(c, 0, 0, W, H, C_BG);
    draw_head(c, v, day);

    if (!day) {
        const char *msg = "Fetching bookings...";
        aafont_draw(c, &aafont_inter_sub, W / 2, H / 2 - 8, msg, C_DIM, AAFONT_CENTRE);
        return;
    }

    int first, last;
    roomsui_hours(day, &first, &last);
    int span = (last - first) * 60;
    int gh = GRID_B - GRID_Y;
#define YOF(m) (GRID_Y + ((m) - first * 60) * gh / span)

    /* Hour rules and figures, half hours fainter. */
    const aafont_t *lf = &aafont_rooms_small;
    for (int hr = first; hr <= last; hr++) {
        int y = YOF(hr * 60);
        canvas_fill_rect(c, GUTTER - 2, y, W - GUTTER - MARGIN + 2, 1, C_RULE);
        if (hr < last) canvas_fill_rect(c, GUTTER, YOF(hr * 60 + 30), W - GUTTER - MARGIN, 1, C_HALF);
        char t[12];
        snprintf(t, sizeof t, "%d", hr);
        int ty = y - lf->cap / 2;
        if (ty < GRID_Y) ty = GRID_Y;
        if (hr < last) aafont_draw(c, lf, GUTTER - 6, ty, t, C_DIM, AAFONT_RIGHT);
    }

    bool today = v->offset == 0 && v->now >= 0;
    int total = 0;
    for (int r = 0; r < day->nrooms; r++) {
        int x, w;
        col_span(day->nrooms, r, &x, &w);
        draw_room_head(c, &day->room[r], x, w, v);
        for (int i = 0; i < day->room[r].n; i++) {
            const room_ev_t *e = &day->room[r].ev[i];
            int y0 = YOF(e->start) + 1, y1 = YOF(e->end) - 1;
            if (y1 - y0 < 8) y1 = y0 + 8;
            int state = !today ? (v->offset < 0 ? 2 : 0)
                      : e->end <= v->now ? 2 : e->start <= v->now ? 1 : 0;
            draw_booking(c, e, x, y0, w, y1 - y0, state);
            total++;
        }
    }

    if (!total) {
        const char *msg = v->wday == 0 || v->wday == 6 ? "No bookings - it's the weekend" : "No bookings";
        int y = YOF(first * 60 + span / 2) - aafont_inter_sub.cap / 2;
        int tw = aafont_width(&aafont_inter_sub, msg);
        canvas_fill_rect(c, (W + GUTTER - tw) / 2 - 12, y - 10, tw + 24, aafont_inter_sub.cap + 20, C_BG);
        aafont_draw(c, &aafont_inter_sub, (W + GUTTER) / 2, y, msg, C_DIM, AAFONT_CENTRE);
    }

    if (today && v->now >= first * 60 && v->now <= last * 60) {
        int y = YOF(v->now);
        canvas_fill_rect(c, GUTTER - 4, y - 1, W - GUTTER - MARGIN + 4, 2, C_NOW);
        canvas_disc(c, GUTTER - 4, y, 4, C_NOW);
    }
#undef YOF
}
