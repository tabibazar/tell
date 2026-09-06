#include "canvas.h"

#include "font.h"
#include "textwrap.h"

void canvas_init(canvas_t *c, uint16_t *fb, int w, int h, int scale)
{
    if (scale < 1) scale = 1;
    c->fb = fb;
    c->w = w;
    c->h = h;
    c->scale = scale;
    c->cols = w / (FONT_W * scale);
    c->rows = h / (FONT_H * scale);
    if (c->cols > TW_MAX_COLS) c->cols = TW_MAX_COLS;
}

void canvas_clear(canvas_t *c)
{
    for (int i = 0; i < c->w * c->h; i++) c->fb[i] = CANVAS_BG;
}

/* Draws one glyph at an absolute pixel position, magnified by `scale`.
   Pixel replication keeps the font table the single source of truth. */
static void glyph(canvas_t *c, char ch, int ox, int oy, int scale)
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
                    if (px >= 0 && px < c->w) c->fb[py * c->w + px] = CANVAS_FG;
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
                  my + (int)r * cell_h, c->scale);
}

#ifdef HAVE_CLOCK_FONT
/* Draws from the dedicated large table, which is rendered at its final size
   rather than magnified. */
static void clock_glyph(canvas_t *c, char ch, int ox, int oy)
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
            if (px >= 0 && px < c->w) c->fb[py * c->w + px] = CANVAS_FG;
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

void canvas_big(canvas_t *c, const char *text)
{
    canvas_clear(c);
    if (text == NULL) return;

    int len = 0;
    while (text[len] != '\0') len++;
    if (len == 0) return;

#ifdef HAVE_CLOCK_FONT
    if (clock_font_suits(c, text, len)) {
        int ox = (c->w - len * CLOCK_W) / 2;
        int oy = (c->h - CLOCK_H) / 2;
        for (int i = 0; i < len; i++)
            clock_glyph(c, text[i], ox + i * CLOCK_W, oy);
        return;
    }
#endif

    int scale = 1;
    while ((scale + 1) * FONT_W * len <= c->w && (scale + 1) * FONT_H <= c->h)
        scale++;

    int ox = (c->w - len * FONT_W * scale) / 2;
    int oy = (c->h - FONT_H * scale) / 2;
    for (int i = 0; i < len; i++)
        glyph(c, text[i], ox + i * FONT_W * scale, oy, scale);
}
