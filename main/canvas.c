#include "canvas.h"

#include "font.h"
#include "textwrap.h"

#include <math.h>
#include <stdbool.h>

void canvas_init(canvas_t *c, uint16_t *fb, int w, int h, int scale)
{
    if (scale < 1) scale = 1;
    c->fb = fb;
    c->w = w;
    c->h = h;
    c->scale = scale;
    c->cell_w = FONT_W * scale;
    c->cell_h = FONT_H * scale;
    c->cols = w / c->cell_w;
    c->rows = h / c->cell_h;
    if (c->cols > TW_MAX_COLS) c->cols = TW_MAX_COLS;
}

void canvas_clear(canvas_t *c)
{
    for (int i = 0; i < c->w * c->h; i++) c->fb[i] = CANVAS_BG;
}

/* Draws one glyph at an absolute pixel position, magnified by `scale`.
   Pixel replication keeps the font table the single source of truth. */
static void glyph(canvas_t *c, char ch, int ox, int oy, int scale,
                  uint16_t colour)
{
    if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
    const uint8_t *g = font_glyphs[(unsigned char)ch - FONT_FIRST];

    for (int y = 0; y < FONT_H; y++) {
        /* Each row is FONT_STRIDE bytes, big-endian, pixel 0 the highest bit. */
        uint32_t bits = 0;
        for (int b = 0; b < FONT_STRIDE; b++)
            bits = (bits << 8) | g[y * FONT_STRIDE + b];
        if (bits == 0) continue;

        for (int x = 0; x < FONT_W; x++) {
            if (!((bits >> (FONT_W - 1 - x)) & 1)) continue;
            for (int sy = 0; sy < scale; sy++) {
                int py = oy + y * scale + sy;
                if (py < 0 || py >= c->h) continue;
                for (int sx = 0; sx < scale; sx++) {
                    int px = ox + x * scale + sx;
                    if (px >= 0 && px < c->w) c->fb[py * c->w + px] = colour;
                }
            }
        }
    }
}

void canvas_text(canvas_t *c, const char *utf8)
{
    char lines[TW_MAX_LINES][TW_MAX_COLS + 1];
    int rows = c->rows > TW_MAX_LINES ? TW_MAX_LINES : c->rows;
    size_t n = textwrap(utf8, (size_t)c->cols, (size_t)rows, lines);

    int cell_w = FONT_W * c->scale;
    int cell_h = FONT_H * c->scale;
    /* Centre the block in whatever pixels the cells do not divide evenly. */
    int mx = (c->w - c->cols * cell_w) / 2;
    int my = (c->h - rows * cell_h) / 2;

    canvas_clear(c);
    for (size_t r = 0; r < n; r++)
        for (int col = 0; lines[r][col] != '\0'; col++)
            glyph(c, lines[r][col], mx + col * cell_w,
                  my + (int)r * cell_h, c->scale, CANVAS_FG);
}

#ifdef HAVE_CLOCK_FONT
/* Draws from the dedicated large table, which is rendered at its final size
   rather than magnified. */
static void clock_glyph(canvas_t *c, char ch, int ox, int oy, uint16_t colour)
{
    if (ch < CLOCK_FIRST || ch > CLOCK_LAST) return;
    const uint8_t *g = clock_glyphs[(unsigned char)ch - CLOCK_FIRST];

    for (int y = 0; y < CLOCK_H; y++) {
        int py = oy + y;
        if (py < 0 || py >= c->h) continue;
        for (int x = 0; x < CLOCK_W; x++) {
            int byte = x >> 3;
            if (!((g[y * CLOCK_STRIDE + byte] >> (7 - (x & 7))) & 1)) continue;
            int px = ox + x;
            if (px >= 0 && px < c->w) c->fb[py * c->w + px] = colour;
        }
    }
}

/* True when every character is in the clock table and the line fits. */
static int clock_font_suits(canvas_t *c, const char *text, int len)
{
    if (len * CLOCK_W > c->w || CLOCK_H > c->h) return 0;
    for (int i = 0; i < len; i++)
        if (text[i] < CLOCK_FIRST || text[i] > CLOCK_LAST) return 0;
    return 1;
}
#endif

/* Chooses the rendering for a short line: the dedicated clock table when the
   text fits it, otherwise the body font magnified. */
static void big_metrics(canvas_t *c, const char *text, int *len_out,
                        int *scale_out, int *w_out, int *h_out)
{
    int len = 0;
    while (text[len] != '\0') len++;
    *len_out = len;

#ifdef HAVE_CLOCK_FONT
    if (clock_font_suits(c, text, len)) {
        *scale_out = 0;                 /* 0 marks the clock table */
        *w_out = len * CLOCK_W;
        *h_out = CLOCK_H;
        return;
    }
#endif
    int scale = 1;
    while ((scale + 1) * FONT_W * len <= c->w && (scale + 1) * FONT_H <= c->h)
        scale++;
    *scale_out = scale;
    *w_out = len * FONT_W * scale;
    *h_out = FONT_H * scale;
}

void canvas_big_size(canvas_t *c, const char *text, int *w, int *h)
{
    if (text == NULL || *text == '\0') { *w = 0; *h = 0; return; }
    int len, scale;
    big_metrics(c, text, &len, &scale, w, h);
}

void canvas_big_at(canvas_t *c, const char *text, int ox, int oy)
{
    canvas_clear(c);
    if (text == NULL || *text == '\0') return;

    int len, scale, tw, th;
    big_metrics(c, text, &len, &scale, &tw, &th);

#ifdef HAVE_CLOCK_FONT
    if (scale == 0) {
        for (int i = 0; i < len; i++)
            clock_glyph(c, text[i], ox + i * CLOCK_W, oy, CANVAS_FG);
        return;
    }
#endif
    for (int i = 0; i < len; i++)
        glyph(c, text[i], ox + i * FONT_W * scale, oy, scale, CANVAS_FG);
}

void canvas_big(canvas_t *c, const char *text)
{
    if (text == NULL || *text == '\0') { canvas_clear(c); return; }
    int w, h;
    canvas_big_size(c, text, &w, &h);
    canvas_big_at(c, text, (c->w - w) / 2, (c->h - h) / 2);
}

void canvas_fill_rect(canvas_t *c, int x, int y, int w, int h, uint16_t colour)
{
    if (w <= 0 || h <= 0) return;

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > c->w ? c->w : x + w;
    int y1 = y + h > c->h ? c->h : y + h;

    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++)
            c->fb[py * c->w + px] = colour;
}

void canvas_puts(canvas_t *c, int col, int row, const char *s, uint16_t colour)
{
    if (s == NULL) return;
    for (int i = 0; s[i] != '\0'; i++) {
        int x = (col + i) * c->cell_w;
        if (x >= c->w) return;
        glyph(c, s[i], x, row * c->cell_h, c->scale, colour);
    }
}

void canvas_puts_px(canvas_t *c, int x, int y, const char *s, uint16_t colour)
{
    if (s == NULL) return;
    for (int i = 0; s[i] != '\0'; i++) {
        int px = x + i * c->cell_w;
        if (px >= c->w) return;
        glyph(c, s[i], px, y, c->scale, colour);
    }
}

void canvas_moon(canvas_t *c, int cx, int cy, int r, float phase,
                 uint16_t lit, uint16_t dark)
{
    if (r <= 0) return;
    while (phase < 0.0f) phase += 1.0f;
    while (phase >= 1.0f) phase -= 1.0f;

    /* The terminator is an ellipse across the disc: its half-width goes from
       the full radius at new moon, through zero at half, to the radius again
       at full. cos(2*pi*phase) gives exactly that, signed. */
    float k = cosf(2.0f * (float)M_PI * phase);
    bool waxing = phase < 0.5f;

    for (int dy = -r; dy <= r; dy++) {
        int py = cy + dy;
        if (py < 0 || py >= c->h) continue;

        int hw = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
        int term = (int)(k * (float)hw);      /* terminator offset on this row */

        for (int dx = -hw; dx <= hw; dx++) {
            int px = cx + dx;
            if (px < 0 || px >= c->w) continue;
            /* Waxing lights the right-hand side, waning the left. */
            bool is_lit = waxing ? (dx >= term) : (dx <= -term);
            c->fb[py * c->w + px] = is_lit ? lit : dark;
        }
    }
}

static void plot(canvas_t *c, int x, int y, uint16_t colour)
{
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    c->fb[y * c->w + x] = colour;
}

void canvas_circle(canvas_t *c, int cx, int cy, int r, uint16_t colour)
{
    /* Midpoint circle: integer only, and each octant mirrored, so there is
       no trigonometry and nothing to round wrongly. */
    if (r < 0) return;
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        plot(c, cx + x, cy + y, colour); plot(c, cx - x, cy + y, colour);
        plot(c, cx + x, cy - y, colour); plot(c, cx - x, cy - y, colour);
        plot(c, cx + y, cy + x, colour); plot(c, cx - y, cy + x, colour);
        plot(c, cx + y, cy - x, colour); plot(c, cx - y, cy - x, colour);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void canvas_disc(canvas_t *c, int cx, int cy, int r, uint16_t colour)
{
    if (r < 0) return;
    for (int dy = -r; dy <= r; dy++) {
        int span = 0;
        while ((span + 1) * (span + 1) + dy * dy <= r * r) span++;
        canvas_fill_rect(c, cx - span, cy + dy, 2 * span + 1, 1, colour);
    }
}
