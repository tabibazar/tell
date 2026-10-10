#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "aafont.h"

extern const aafont_t aafont_rooms_head;    /* Inter SemiBold 30 */
extern const aafont_t aafont_rooms_title;   /* Inter SemiBold 17 */
extern const aafont_t aafont_rooms_small;   /* Inter Medium 15 */

#define W 480
#define H 480
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define C_BG     RGB(0x0E, 0x10, 0x14)
#define C_TEXT   RGB(0xF2, 0xF4, 0xF7)
#define C_DIM    RGB(0x8A, 0x92, 0x9E)
#define C_AMBER  RGB(0xF5, 0xB0, 0x41)
#define C_BLUE   RGB(0x3B, 0x6F, 0xD0)
#define C_RED    RGB(0xD9, 0x3A, 0x3A)
#define C_BANNER RGB(0xE0, 0x1E, 0x1E)

#define TALK_X 240
#define TALK_Y 250
#define TALK_R 100

bool ui_hit_talk(int x, int y)
{
    int dx = x - TALK_X, dy = y - TALK_Y;
    return dx * dx + dy * dy <= (TALK_R + 20) * (TALK_R + 20);
}

/* Greedy word wrap of `s` in face `f` into lines of `maxw`, drawn from
   (x, y) every `pitch` px, at most `max` lines (the last one "..."-cut).
   Returns the y below the last line. */
static int para(canvas_t *c, const aafont_t *f, const char *s, int x, int y, int maxw, int pitch, int max, uint16_t col)
{
    char line[160];
    char trunc[164];
    int lines = 0;
    while (s && *s && lines < max) {
        while (*s == ' ') s++;
        int best = 0, i = 0;
        while (s[i] && i < (int)sizeof line - 1) {
            int j = i;
            while (s[j] && s[j] != ' ' && j < (int)sizeof line - 1) j++;
            memcpy(line, s, j);
            line[j] = 0;
            if (aafont_width(f, line) > maxw) break;
            best = j;
            if (!s[j]) break;
            i = j + 1;
        }
        if (best == 0) {                       /* one word wider than the line */
            best = 1;
            while (s[best] && s[best] != ' ' && best < (int)sizeof line - 1) {
                memcpy(line, s, best + 1);
                line[best + 1] = 0;
                if (aafont_width(f, line) > maxw) break;
                best++;
            }
        }
        memcpy(line, s, best);
        line[best] = 0;
        s += best;
        const char *out = line;
        if (lines == max - 1 && *s) {          /* the last allowed line, with more to come */
            size_t l = strlen(line);
            while (l > 0) {
                snprintf(trunc, sizeof trunc, "%.*s...", (int)l, line);
                if (aafont_width(f, trunc) <= maxw) { out = trunc; break; }
                l--;
            }
        }
        aafont_draw(c, f, x, y, out, col, AAFONT_LEFT);
        y += pitch;
        lines++;
    }
    return y;
}

static void header(canvas_t *c, const ui_view_t *v)
{
    aafont_draw(c, &aafont_rooms_small, 16, 18, "stan-claw", C_DIM, AAFONT_LEFT);
    if (v->now >= 0) {
        char t[24];
        snprintf(t, sizeof t, "%d:%02d", v->now / 60 % 24, v->now % 60);
        aafont_draw(c, &aafont_rooms_small, W - 16, 18, t, C_DIM, AAFONT_RIGHT);
    }
}

static void note(canvas_t *c, const char *n)
{
    if (n && n[0]) aafont_draw(c, &aafont_rooms_small, W / 2, H - 34, n, C_AMBER, AAFONT_CENTRE);
}

static void talk_button(canvas_t *c, uint16_t col, const char *label, const char *hint)
{
    canvas_disc(c, TALK_X, TALK_Y, TALK_R, col);
    aafont_draw(c, &aafont_rooms_head, TALK_X, TALK_Y - aafont_rooms_head.cap / 2, label, C_TEXT, AAFONT_CENTRE);
    if (hint) aafont_draw(c, &aafont_rooms_small, W / 2, TALK_Y + TALK_R + 24, hint, C_DIM, AAFONT_CENTRE);
}

void ui_draw(canvas_t *c, const ui_view_t *v)
{
    canvas_fill_rect(c, 0, 0, W, H, C_BG);
    header(c, v);
    const aafont_t *sub = &aafont_inter_sub;
    switch (v->mode) {
    case UI_HOME:
        talk_button(c, C_BLUE, "Talk", "tap, or press BOOT");
        note(c, v->note);
        break;
    case UI_LISTENING:
        talk_button(c, C_RED, "Listening", "stop talking to finish");
        break;
    case UI_THINKING:
        para(c, &aafont_rooms_title, v->heard, 24, 70, W - 48, 22, 3, C_DIM);
        aafont_draw(c, &aafont_rooms_head, W / 2, H / 2, "Thinking...", C_TEXT, AAFONT_CENTRE);
        note(c, v->note);
        break;
    case UI_SPEAKING:
    case UI_ANSWER: {
        int y = para(c, &aafont_rooms_title, v->heard, 24, 70, W - 48, 22, 2, C_DIM);
        para(c, sub, v->text, 24, y + 22, W - 48, 32, 10, C_TEXT);
        note(c, v->note);
        break;
    }
    case UI_ERROR:
        aafont_draw(c, &aafont_rooms_head, W / 2, 150, v->title ? v->title : "Something went wrong", C_RED, AAFONT_CENTRE);
        para(c, sub, v->text, 24, 210, W - 48, 32, 6, C_TEXT);
        break;
    case UI_AGENT_TEXT: {
        int y = 70;
        if (v->title && v->title[0]) y = para(c, &aafont_rooms_head, v->title, 24, 70, W - 48, 38, 2, C_TEXT) + 10;
        para(c, sub, v->text, 24, y, W - 48, 32, 11, C_TEXT);
        break;
    }
    case UI_AGENT_IMAGE:
        if (v->image && v->img_w > 0 && v->img_h > 0 && v->img_w <= W && v->img_h <= H)
            canvas_blit(c, v->image, v->img_w, v->img_h, (W - v->img_w) / 2, (H - v->img_h) / 2);
        break;
    }
    if (v->mic_open) {
        /* Last, over everything: the microphones are open. */
        canvas_fill_rect(c, 0, 0, W, 44, C_BANNER);
        canvas_disc(c, W / 2 - 78, 22, 7, C_TEXT);
        aafont_draw(c, &aafont_rooms_title, W / 2 + 10, 22 - aafont_rooms_title.cap / 2, "LISTENING", C_TEXT, AAFONT_CENTRE);
    }
}
