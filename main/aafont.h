#ifndef AAFONT_H
#define AAFONT_H

#include "canvas.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Anti-aliased text in a real typeface, for envo's air pages. The 12x24
 * bitmap font is ink or no ink, so at 247 ppi every curve and diagonal in it
 * is a staircase; vfont is smooth but a monoline stroke, drawn-looking beside
 * a typeface. These are Inter, rendered by FreeType at the sizes the pages use
 * (tools/gen_aafont.py) and kept as 4-bit coverage: sixteen levels of how
 * much of each pixel the outline covers. Each glyph was rendered at whichever
 * quarter-pixel offset lands its vertical stems most nearly on whole pixels,
 * so the stems of a small label are two sharp columns rather than three soft
 * ones.
 *
 * Coverage is blended in linear light. RGB565 values are gamma-encoded (the
 * panel's response is close to a 2.2 power), so laying a colour over black
 * at half coverage by averaging the codes gives a quarter of the light, not
 * half: the edges of bright text on a dark ground come out too dark, the
 * strokes look thin, and a curve reads as a row of steps with dim corners --
 * the jaggedness this is here to remove. Each channel is decoded to linear
 * light, mixed, and encoded back (rounded in the encoded scale), so a
 * half-covered pixel of white text on black is half as bright as white, and
 * black text on the POOR block's red is as thin as its outline rather than
 * half a pixel fatter. Right for any colour over any colour, and cheap: only
 * the edge pixels, a few hundred a word, pay for it; covered ones are a store.
 * The arithmetic is vector.c's vec_blend_linear, which envui's marks use too.
 *
 * Placing text: `y` is the top of the capitals' ink -- a flat-topped H's
 * first row -- as it is for every piece of text in envui, whatever the font.
 * The baseline is y + f->cap. Horizontally `x` is the left edge, the centre
 * or the right edge of the INK by `align`, as with vfont (so a left-aligned
 * 1 in a column of tabular figures sits flush with the 7 above it). A clock,
 * whose width must not twitch as its digits change, aligns the advance box
 * instead by OR-ing in AAFONT_ADVANCE. Edges are pixel edges, as in vector.c:
 * right-aligned at 304, the rightmost ink is column 303.
 *
 * Text is UTF-8: the degree sign, U+00B0, is "\xC2\xB0" in a C string. A
 * byte that does not start a valid sequence is read as Latin-1, so a stray
 * "\xB0" still draws a degree sign. A character the font does not have draws
 * nothing and advances like a space; lower case in a capitals-only font comes
 * out as capitals, as vfont's does -- except the e and s the word faces
 * carry for "eCO2" and "VOCs".
 *
 * Clipped to the canvas: nothing lands outside fb[0 .. w*h) wherever the text
 * is placed. Nothing allocates, nothing is kept between calls, so any task
 * may draw -- on a canvas no other task is drawing on.
 */

#define AAFONT_LEFT     0
#define AAFONT_CENTRE   1
#define AAFONT_RIGHT    2
#define AAFONT_ADVANCE  4   /* OR in: align the advance box, not the ink */

/* Advances and kerning are in 1/AAFONT_SUB px (gen_aafont.py's SUB). */
#define AAFONT_SUB 16

/* One glyph: its bitmap, `w` x `h` 4-bit coverage values, rows padded to a
   whole byte, the LEFT pixel of each pair in the HIGH nibble, starting at
   bits[off]. (x, y) is the bitmap's top-left from the pen on the baseline;
   y is negative above it. A space is 0 x 0. */
typedef struct {
    uint32_t off;
    uint16_t cp;       /* Unicode code point; the table is sorted by it */
    uint16_t adv;      /* advance, 1/16 px */
    uint8_t w, h;
    int8_t x, y;
} aafont_glyph_t;

/* Kerning between two glyphs (indices into the glyph table), 1/16 px.
   Sorted by left, then right. */
typedef struct {
    uint8_t left, right;
    int16_t dx;
} aafont_kern_t;

typedef struct {
    const aafont_glyph_t *glyphs;
    const uint8_t *bits;
    const aafont_kern_t *kerns;     /* NULL when kern_count is 0 */
    uint32_t bits_len;
    uint16_t count, kern_count;
    int16_t cap;        /* capital height in px: the rendered H's ink */
    int16_t x_height;
    int16_t ascent, descent, line;  /* the font's, in px; line = ascent + descent */
} aafont_t;

/* Draws `utf8` in `colour` at (x, y) as above. Returns the ink width, as
   aafont_width gives it. NULL anything, or an empty string, draws nothing. */
int aafont_draw(canvas_t *c, const aafont_t *f, int x, int y,
                const char *utf8, uint16_t colour, int align);

/* The width of the ink `utf8` would draw: from the leftmost inked column to
   just past the rightmost. 0 for nothing, or only spaces. */
int aafont_width(const aafont_t *f, const char *utf8);

/* The width of its advance box: the pen's travel, kerning included, rounded
   to a pixel. What AAFONT_ADVANCE aligns. */
int aafont_advance(const aafont_t *f, const char *utf8);

/* Where its ink begins, from the pen's start: the first glyph's left side
   bearing, as a figure set by its advance box stands in from the box's edge
   (negative for ink that reaches left of it). 0 for nothing, or only spaces. */
int aafont_bearing(const aafont_t *f, const char *utf8);

/* `dst` with `src` laid over it at coverage `a` of 15, in linear light.
   Public so the host test can hold it to the arithmetic. */
uint16_t aafont_blend(uint16_t dst, uint16_t src, unsigned a);

/*
 * The faces envo sets: Inter at six sizes, each named for its job, 78 KB of
 * flash between them. Every one holds the degree sign. Their figures are
 * tabular -- all ten one width -- so a number that changes does not shuffle
 * what stands beside it; all but inter_sub's, which sets dates, where a
 * tabular 1 stands in five pixels more than its own width and "11 Sep" opens
 * up. Each size's capitals match or pass what the page drew before at that
 * job, the trend word alone excepted (see inter_sub); tools/gen_aafont.py
 * has the commands:
 *
 *   inter_label   Medium 17 px, capitals 13, x-height 10. All of ASCII, and
 *                 the minus sign (U+2212) for "-24H", which with the hyphen
 *                 read as a dash before the 24 rather than as minus 24 hours.
 *                 The small grey labels: VOC / ppb, -24H / NOW, the chart's
 *                 axis, key and titles, the week's days and hours -- the
 *                 12x24 font's jobs. Its lower case is that font's height,
 *                 its capitals one pixel shorter, which is what keeps the
 *                 chart key's words in the column right of the plot (POOR
 *                 47 px, GOOD 51; at capitals 14 POOR alone is over 50) and
 *                 "Mo" inside the week's day column.
 *   inter_sub     Medium 23 px, capitals 17. All of ASCII, its figures
 *                 proportional. The secondary size: the clock's date line,
 *                 the minutes so far under WARMING UP, a chart header's
 *                 message where inter_word will not fit -- and the trend word
 *                 under its arrow, a size below the 25 vfont set it at,
 *                 because in Inter's wider letters STEADY at 25 is 136 px and
 *                 left the reading beside it no room: "22.5" would have
 *                 dropped to the state words' size.
 *   inter_word    SemiBold 34 px, capitals 25. Capitals, digits, " .,:+-%/"
 *                 and the e and s of "eCO2" and "VOCs": vfont's 22 px jobs,
 *                 whose ink was 25 tall -- FROM VOCs, NO READING, the chart
 *                 header's word, and its number where the state size will not
 *                 fit -- and WARMING UP, which at the next size up is 291 px,
 *                 three more than the page has.
 *   inter_state   SemiBold 44 px, capitals 32. The same set, for vfont's 28
 *                 and 30 px jobs (ink 32 and 34): GOOD / FAIR / POOR under
 *                 the number, GAS ERROR in its place, the clock's verdict,
 *                 the chart header's number -- and the reading's last step
 *                 down, as a five-figure VOC is up to 203 px in
 *                 inter_number, past the 200 it may take.
 *   inter_number  SemiBold 64 px, capitals 47. Digits and " .:+-%": the
 *                 clock's HH:MM:SS, which centred by its advance keeps its ink
 *                 in x 19..301 -- inside the margins, where the 12x24 font
 *                 magnified three times was 42 px of stair-stepped capitals --
 *                 and the reading's first step down ("1100" is 157 px, "22.5"
 *                 and its degree 166).
 *   inter_reading SemiBold 77 px, capitals 57. Digits and " .-": the reading
 *                 page's number, the one thing on it meant for the far side
 *                 of the room, at the height vfont drew it (ink 63 at 56 px;
 *                 the 47 of inter_number was three quarters of that). "22.5"
 *                 and its degree are 200 px, the most that number may take,
 *                 and "1100" 188; wider, and it steps down to inter_number.
 *
 * In the four figure faces "-" is the minus sign, as wide as the plus and
 * on its axis; in the two text faces it is the hyphen. In all six, the
 * hyphen, colon and plus are the forms cut to sit on the capitals.
 *
 * And POOR a weight heavier, for its knock-out: black on the red block, at
 * the four sizes that can carry one -- SemiBold for the two Medium faces,
 * Bold for the two SemiBold ones, P, O and R alone, 3 KB in all. Dark type on
 * a bright ground reads lighter than the same weight bright on dark (the
 * light spreads into it rather than out of it), and in linear light an edge
 * pixel of black on red is mostly red: at one weight the white VOC beside the
 * block had a three-pixel stroke and the black POOR on it two. Each has the
 * capitals of the face it stands in for, to the pixel, so the block and the
 * line around it are laid out as before.
 */
extern const aafont_t aafont_inter_label;
extern const aafont_t aafont_inter_sub;
extern const aafont_t aafont_inter_word;
extern const aafont_t aafont_inter_state;
extern const aafont_t aafont_inter_number;
extern const aafont_t aafont_inter_reading;
extern const aafont_t aafont_inter_label_knock;
extern const aafont_t aafont_inter_sub_knock;
extern const aafont_t aafont_inter_word_knock;
extern const aafont_t aafont_inter_state_knock;

#endif /* AAFONT_H */
