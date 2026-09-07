#ifndef PALETTE_H
#define PALETTE_H

/*
 * RGB565, from the Okabe-Ito qualitative palette, which is chosen to stay
 * distinguishable under deuteranopia, protanopia and tritanopia.
 *
 * Colour is never the only cue in these charts: every bar carries a text
 * label, and the daily chart marks its notable bars with a symbol as well as
 * a colour. Someone who cannot separate two hues still reads the chart.
 */

#define PAL_BG       0x0000  /* black */
#define PAL_FG       0xFFFF  /* white */
#define PAL_DIM      0x9CD3  /* Okabe-Ito grey, for labels, axes, gridlines */
#define PAL_TITLE_BG 0x0396  /* blue */

/* Accents, in the order they are handed out. Adjacent entries were picked to
   stay far apart for the common forms of colour blindness. */
#define PAL_A0       0x55BD  /* sky blue */
#define PAL_A1       0xE4E0  /* orange */
#define PAL_A2       0x04EE  /* bluish green */
#define PAL_A3       0xCBD4  /* reddish purple */
#define PAL_A4       0xF728  /* yellow */
#define PAL_A5       0xD2E0  /* vermillion */

#define PAL_ACCENTS  6

/* Highlights on the daily chart, paired with a symbol so the meaning does not
   depend on telling the colours apart. */
#define PAL_PEAK     0xF728  /* yellow */
#define PAL_LATEST   0xE4E0  /* orange */

static inline unsigned short pal_accent(int i)
{
    const unsigned short a[PAL_ACCENTS] = {
        PAL_A0, PAL_A1, PAL_A2, PAL_A3, PAL_A4, PAL_A5
    };
    return a[i % PAL_ACCENTS];
}

/* A lighter cap sits on top of each bar, which reads as depth and also gives
   a second, brightness-based cue where hues are hard to separate. */
static inline unsigned short pal_lighten(unsigned short c)
{
    unsigned r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
    r += (0x1F - r) / 2;
    g += (0x3F - g) / 2;
    b += (0x1F - b) / 2;
    return (unsigned short)((r << 11) | (g << 5) | b);
}

#endif /* PALETTE_H */
