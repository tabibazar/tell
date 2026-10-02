#include "stamp.h"

/* 5x7 digits and a colon, one row per byte, bit 4 the leftmost column. */
static const uint8_t GLYPH[11][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },   /* 0 */
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },   /* 1 */
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },   /* 2 */
    { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },   /* 3 */
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },   /* 4 */
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },   /* 5 */
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },   /* 6 */
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },   /* 7 */
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },   /* 8 */
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },   /* 9 */
    { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 },   /* : */
};

#define SCALE 3
#define PAD   6

void stamp_time(uint8_t *i420, int w, int h, int hour, int minute)
{
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return;
    const int g[5] = { hour / 10, hour % 10, 10, minute / 10, minute % 10 };
    const int cw = 6 * SCALE;                          /* glyph + one column gap */
    const int tw = 5 * cw - SCALE, th = 7 * SCALE;
    /* Even coordinates, so the box covers whole chroma samples. */
    int bx = (w - tw - 3 * PAD) & ~1, by = (h - th - 3 * PAD) & ~1;
    int bw = (tw + 2 * PAD + 1) & ~1, bh = (th + 2 * PAD + 1) & ~1;
    if (bx < 0 || by < 0) return;
    uint8_t *Y = i420, *U = i420 + w * h, *V = U + (w / 2) * (h / 2);
    for (int y = by; y < by + bh; y++)
        for (int x = bx; x < bx + bw; x++) Y[y * w + x] = (uint8_t)(16 + (Y[y * w + x] - 16) / 4);
    for (int y = by / 2; y < (by + bh) / 2; y++)
        for (int x = bx / 2; x < (bx + bw) / 2; x++) { U[y * (w / 2) + x] = 128; V[y * (w / 2) + x] = 128; }
    for (int i = 0; i < 5; i++) {
        for (int r = 0; r < 7; r++) {
            for (int c = 0; c < 5; c++) {
                if (!(GLYPH[g[i]][r] & (0x10 >> c))) continue;
                for (int sy = 0; sy < SCALE; sy++)
                    for (int sx = 0; sx < SCALE; sx++)
                        Y[(by + PAD + r * SCALE + sy) * w + bx + PAD + i * cw + c * SCALE + sx] = 235;
            }
        }
    }
}
