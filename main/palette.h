#ifndef PALETTE_H
#define PALETTE_H

/* RGB565. One palette so pages cannot drift apart. */
#define PAL_BG       0x0000  /* black */
#define PAL_FG       0xFFFF  /* white */
#define PAL_DIM      0x8410  /* grey, for labels and axes */
#define PAL_TITLE_BG 0x001F  /* blue title bar */

/* Accent colours, assigned to models in rank order. */
#define PAL_A0       0x07FF  /* cyan */
#define PAL_A1       0xF81F  /* magenta */
#define PAL_A2       0xFFE0  /* yellow */
#define PAL_A3       0x07E0  /* green */

static inline unsigned short pal_accent(int i)
{
    const unsigned short a[4] = { PAL_A0, PAL_A1, PAL_A2, PAL_A3 };
    return a[i & 3];
}

#endif /* PALETTE_H */
