#ifndef PALETTE_H
#define PALETTE_H

/* sdkconfig.h is not force-included, so without this the vivid test below is
   silently false and every board gets the muted set. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

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

#if defined(CONFIG_SCREEN_BOARD_TOUCH_LCD_35B)
/*
 * envio wears blue, amber and white, and nothing else -- a two-hue scheme,
 * not the six-accent chart palette, because she shows the weather and the
 * room, not the colour-coded usage charts. The two readings on a stacked page
 * take one hue each, so they stay apart at a glance.
 */
#define PAL_DIM      0x8410  /* dim grey-white, for axes and gridlines */
#define PAL_TITLE_BG 0x02D8  /* blue header bar, white text on it */
#elif defined(CONFIG_SCREEN_VIVID_PALETTE)
#define PAL_DIM      0xBDF7  /* a lighter grey: the muted one disappears here */
#define PAL_TITLE_BG 0x0396  /* blue */
#else
#define PAL_DIM      0x9CD3  /* Okabe-Ito grey, for labels, axes, gridlines */
#define PAL_TITLE_BG 0x0396  /* blue */
#endif

/*
 * Accents, in the order they are handed out. Adjacent entries were picked to
 * stay far apart for the common forms of colour blindness.
 *
 * Okabe-Ito is a print palette, and print is not backlit: its colours are
 * deliberately desaturated so they stay distinguishable, which on a big matte
 * panel reads as restraint and on a small glossy one reads as washed out. Its
 * sky blue is (86,190,233) -- a pale, milky blue.
 *
 * So a board may ask for the vivid set instead. Same hues in the same order,
 * so nothing that relies on "A0 is the blue one" changes; saturation pushed to
 * where the panel can show it. The cost is real and is why this is not the
 * default: at full saturation the orange and the vermillion move closer
 * together for a deuteranope, and the charts lean on that pair.
 */
#if defined(CONFIG_SCREEN_BOARD_TOUCH_LCD_35B)
/* Blue and amber, alternating, so a stacked pair reads as two. */
#define PAL_A0       0x04BF  /* blue   (0,150,255) */
#define PAL_A1       0xFD40  /* amber  (255,168,0) */
#define PAL_A2       0x04BF  /* blue */
#define PAL_A3       0x04BF  /* blue */
#define PAL_A4       0xFD40  /* amber */
#define PAL_A5       0xFD40  /* amber */
#elif defined(CONFIG_SCREEN_VIVID_PALETTE)
#define PAL_A0       0x055F  /* sky blue      (0,170,255) */
#define PAL_A1       0xFC60  /* orange        (255,140,0) */
#define PAL_A2       0x06D0  /* bluish green  (0,220,130) */
#define PAL_A3       0xFA99  /* reddish purple(255,80,200) */
#define PAL_A4       0xFF80  /* yellow        (255,240,0) */
#define PAL_A5       0xF9E0  /* vermillion    (255,60,0) */
#else
#define PAL_A0       0x55BD  /* sky blue */
#define PAL_A1       0xE4E0  /* orange */
#define PAL_A2       0x04EE  /* bluish green */
#define PAL_A3       0xCBD4  /* reddish purple */
#define PAL_A4       0xF728  /* yellow */
#define PAL_A5       0xD2E0  /* vermillion */
#endif

#define PAL_ACCENTS  6

/* Highlights on the daily chart, paired with a symbol so the meaning does not
   depend on telling the colours apart. */
#define PAL_PEAK     0xF728  /* yellow */
#define PAL_LATEST   0xE4E0  /* orange */

/* The year page's heat ramp: one hue, amber, dark to bright. A single hue
   whose lightness climbs is readable without distinguishing hues at all, and
   adjacent steps are at least 14 dE apart under every common form of colour
   blindness (checked, not eyeballed). Index 0 is the dot for an empty day. */
#define PAL_HEAT_STEPS 4
static inline unsigned short pal_heat(int level)
{
    const unsigned short a[PAL_HEAT_STEPS + 1] = {
        0x4208,  /* none   #404040 */
        0x3941,  /* 1      #3d2a08 */
        0x7AA2,  /* 2      #7d5510 */
        0xBC23,  /* 3      #b9841a */
        0xF5C6,  /* 4      #f5b836 */
    };
    if (level < 0) level = 0;
    if (level > PAL_HEAT_STEPS) level = PAL_HEAT_STEPS;
    return a[level];
}

/* One colour per weekday, so a single machine's chart still reads as more
   than a wall of one hue -- and the colour means something: you can see the
   working week against the weekend. */
static inline unsigned short pal_weekday(int dow)
{
    const unsigned short a[7] = {
        0x55BD,  /* Mon  sky blue */
        0x04EE,  /* Tue  bluish green */
        0xF728,  /* Wed  yellow */
        0xE4E0,  /* Thu  orange */
        0xCBD4,  /* Fri  reddish purple */
        0xD2E0,  /* Sat  vermillion */
        0xFBEE,  /* Sun  light vermillion */
    };
    if (dow < 0 || dow > 6) return 0x55BD;
    return a[dow];
}

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

/* The counterpart, for something that must sit behind a line of its own
   colour: the same hue, too dark to compete with it, so the pair reads as
   one series drawn twice rather than as two. */
static inline unsigned short pal_darken(unsigned short c)
{
    unsigned r = ((c >> 11) & 0x1F) / 3, g = ((c >> 5) & 0x3F) / 3, b = (c & 0x1F) / 3;
    return (unsigned short)((r << 11) | (g << 5) | b);
}

#endif /* PALETTE_H */
