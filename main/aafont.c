#include "aafont.h"
#include "vector.h"

#include <stdbool.h>

/* The faces themselves. Each header defines one aafont_t over its own static
   tables, so this is the one file that may include them. */
#include "font_inter_label.h"
#include "font_inter_small.h"
#include "font_inter_sub.h"
#include "font_inter_word.h"
#include "font_inter_state.h"
#include "font_inter_number.h"
#include "font_inter_reading.h"
#include "font_inter_label_knock.h"
#include "font_inter_sub_knock.h"
#include "font_inter_word_knock.h"
#include "font_inter_state_knock.h"

/* ---- blending, in linear light -------------------------------------------- */

/* vector.c keeps the tables, so the marks envui draws with it and this type
   come out of one blend. Coverage a of 15 is a * 17 of 255, exactly. */
uint16_t aafont_blend(uint16_t dst, uint16_t src, unsigned a)
{
    if (a == 0) return dst;
    if (a >= 15) return src;
    return vec_blend_linear(dst, src, (uint8_t)(a * 17u));
}

/* ---- text to glyphs --------------------------------------------------------- */

/*
 * The next code point, moving *p past it. A byte that does not begin a valid
 * sequence -- a stray continuation byte, a truncated or overlong sequence, a
 * surrogate -- is taken on its own as Latin-1, so nothing is ever skipped
 * over and a NUL is never stepped past.
 */
static uint32_t utf8_next(const unsigned char **p)
{
    const unsigned char *s = *p;
    uint32_t c = s[0];
    int n = c >= 0xF0 && c <= 0xF4 ? 3 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xC2 && c <= 0xDF ? 1 : 0;
    uint32_t v = n == 3 ? c & 0x07u : n == 2 ? c & 0x0Fu : c & 0x1Fu;
    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xC0u) != 0x80u) { n = 0; break; }
        v = (v << 6) | (s[i] & 0x3Fu);
    }
    if (n == 2 && (v < 0x800u || (v >= 0xD800u && v <= 0xDFFFu))) n = 0;
    if (n == 3 && (v < 0x10000u || v > 0x10FFFFu)) n = 0;
    *p = s + 1 + n;
    return n > 0 ? v : c;
}

static int find(const aafont_t *f, uint32_t cp)
{
    int lo = 0, hi = (int)f->count - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        uint32_t g = f->glyphs[m].cp;
        if (g == cp) return m;
        if (g < cp) lo = m + 1;
        else hi = m - 1;
    }
    return -1;
}

/* The glyph a character is drawn with, or -1: its own, else for a lower-case
   letter its capital. */
static int glyph_of(const aafont_t *f, uint32_t cp)
{
    int i = find(f, cp);
    if (i < 0 && cp >= 'a' && cp <= 'z') i = find(f, cp - 'a' + 'A');
    return i;
}

static int kern(const aafont_t *f, int left, int right)
{
    int lo = 0, hi = (int)f->kern_count - 1;
    unsigned key = (unsigned)left << 8 | (unsigned)right;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        unsigned k = (unsigned)f->kerns[m].left << 8 | f->kerns[m].right;
        if (k == key) return f->kerns[m].dx;
        if (k < key) lo = m + 1;
        else hi = m - 1;
    }
    return 0;
}

/* Sixteenths to the nearest whole pixel, halves up, for negatives too. */
static int64_t px_of(int64_t sub)
{
    int64_t v = sub + AAFONT_SUB / 2;
    return v >= 0 ? v / AAFONT_SUB : -((-v + AAFONT_SUB - 1) / AAFONT_SUB);
}

/*
 * Walks the string glyph by glyph: each call gives the next glyph to draw
 * (-1 for one the font lacks, which only advances) and the whole-pixel x of
 * the pen it stands at, measured from the pen's start. The pen is kept in
 * sixteenths and only the glyph's placement is rounded, so the rounding
 * never accumulates along a line.
 */
typedef struct {
    const aafont_t *f;
    const unsigned char *s;
    int64_t pen;             /* 1/16 px */
    int prev;                /* the glyph before, for kerning; -1 for none */
    int space;               /* the space's glyph, what a missing one advances by */
} walk_t;

static void walk_start(walk_t *w, const aafont_t *f, const char *s)
{
    w->f = f;
    w->s = (const unsigned char *)s;
    w->pen = 0;
    w->prev = -1;
    w->space = find(f, ' ');
}

static bool walk_next(walk_t *w, int *gi, int64_t *x)
{
    if (*w->s == 0) return false;
    int i = glyph_of(w->f, utf8_next(&w->s));
    if (i >= 0 && w->prev >= 0 && w->f->kern_count > 0) w->pen += kern(w->f, w->prev, i);
    *gi = i;
    *x = px_of(w->pen);
    int step = i >= 0 ? i : w->space;
    if (step >= 0) w->pen += w->f->glyphs[step].adv;
    w->prev = i;
    return true;
}

typedef struct {
    int64_t ink_l, ink_r;    /* ink columns [ink_l, ink_r) from the pen's start */
    int64_t pen;             /* the advance box, whole px */
    bool inked;
} extent_t;

static bool usable(const aafont_t *f)
{
    return f != NULL && f->glyphs != NULL && f->bits != NULL && f->count > 0
        && (f->kern_count == 0 || f->kerns != NULL);
}

static extent_t measure(const aafont_t *f, const char *s)
{
    extent_t e = { 0, 0, 0, false };
    if (!usable(f) || s == NULL) return e;
    walk_t w;
    walk_start(&w, f, s);
    int gi;
    int64_t x;
    while (walk_next(&w, &gi, &x)) {
        if (gi < 0) continue;
        const aafont_glyph_t *g = &f->glyphs[gi];
        if (g->w == 0 || g->h == 0) continue;
        int64_t l = x + g->x, r = l + g->w;
        if (!e.inked || l < e.ink_l) e.ink_l = l;
        if (!e.inked || r > e.ink_r) e.ink_r = r;
        e.inked = true;
    }
    e.pen = px_of(w.pen);
    return e;
}

int aafont_width(const aafont_t *f, const char *utf8)
{
    extent_t e = measure(f, utf8);
    int64_t w = e.inked ? e.ink_r - e.ink_l : 0;
    return w > 1000000 ? 1000000 : (int)w;
}

int aafont_bearing(const aafont_t *f, const char *utf8)
{
    extent_t e = measure(f, utf8);
    if (!e.inked) return 0;
    return e.ink_l > 1000000 ? 1000000 : e.ink_l < -1000000 ? -1000000 : (int)e.ink_l;
}

int aafont_advance(const aafont_t *f, const char *utf8)
{
    int64_t w = measure(f, utf8).pen;
    return w > 1000000 ? 1000000 : w < -1000000 ? -1000000 : (int)w;
}

/* ---- drawing ------------------------------------------------------------------ */

/* One glyph, its bitmap's top-left at (gx, gy), clipped to the canvas. */
static void glyph(canvas_t *c, const aafont_t *f, const aafont_glyph_t *g,
                  int64_t gx, int64_t gy, uint16_t colour)
{
    if (gx >= c->w || gy >= c->h || gx + g->w <= 0 || gy + g->h <= 0) return;
    int x0 = gx < 0 ? (int)-gx : 0, y0 = gy < 0 ? (int)-gy : 0;
    int x1 = gx + g->w > c->w ? (int)(c->w - gx) : g->w;
    int y1 = gy + g->h > c->h ? (int)(c->h - gy) : g->h;
    int stride = (g->w + 1) / 2;
    if (g->off + (uint32_t)stride * g->h > f->bits_len) return;   /* a bad table draws nothing */
    const uint8_t *bits = f->bits + g->off;
    for (int y = y0; y < y1; y++) {
        const uint8_t *row = bits + y * stride;
        uint16_t *line = c->fb + (size_t)(gy + y) * (size_t)c->w;
        for (int x = x0; x < x1; x++) {
            unsigned a = x & 1 ? row[x >> 1] & 0x0Fu : row[x >> 1] >> 4;
            if (a == 0) continue;
            uint16_t *p = &line[gx + x];         /* 0 <= gx + x < c->w */
            *p = a == 15 ? colour : aafont_blend(*p, colour, a);
        }
    }
}

int aafont_draw(canvas_t *c, const aafont_t *f, int x, int y,
                const char *utf8, uint16_t colour, int align)
{
    if (c == NULL || c->fb == NULL || c->w <= 0 || c->h <= 0) return 0;
    extent_t e = measure(f, utf8);
    if (!e.inked) return 0;

    /* Where the pen starts, so the chosen edge lands on x. */
    int64_t ox;
    int how = align & ~AAFONT_ADVANCE;
    if (align & AAFONT_ADVANCE)
        ox = how == AAFONT_RIGHT ? x - e.pen : how == AAFONT_CENTRE ? x - e.pen / 2 : x;
    else
        ox = how == AAFONT_RIGHT ? x - e.ink_r
           : how == AAFONT_CENTRE ? x - e.ink_l - (e.ink_r - e.ink_l) / 2
           : x - e.ink_l;
    int64_t base = (int64_t)y + f->cap;

    walk_t w;
    walk_start(&w, f, utf8);
    int gi;
    int64_t gx;
    while (walk_next(&w, &gi, &gx)) {
        if (gi < 0) continue;
        const aafont_glyph_t *g = &f->glyphs[gi];
        if (g->w == 0 || g->h == 0) continue;
        glyph(c, f, g, ox + gx + g->x, base + g->y, colour);
    }
    int64_t iw = e.ink_r - e.ink_l;
    return iw > 1000000 ? 1000000 : (int)iw;
}
