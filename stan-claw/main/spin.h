#ifndef SPIN_H
#define SPIN_H

/*
 * The voice wheel's motion: a row of cards that follows the finger, keeps
 * spinning when flicked (slowing as if on a bearing), and settles exactly
 * on one card. Positions are in cards: 2.0 means card 2 is in the middle.
 * Pure: host_tests/test_sc_spin.c.
 */
#include <stdbool.h>

#define SPIN_CARD_PX 210.0f   /* a finger moving this far moves one card */

typedef struct {
    float pos, vel;           /* cards, cards per second */
    float grab_pos;           /* pos when the finger came down */
    int n;
    bool held, moving;
} spin_t;

void spin_init(spin_t *s, int n, int index);
void spin_grab(spin_t *s);                       /* a finger comes down */
void spin_drag(spin_t *s, float dx_px);          /* it has moved dx since coming down (right = earlier cards) */
void spin_release(spin_t *s, float vx_px_s);     /* it lifts, moving at vx */
void spin_go(spin_t *s, int index);              /* glide to a card */
bool spin_step(spin_t *s, float dt);             /* advance dt seconds; true while still moving */
int spin_index(const spin_t *s);                 /* the card nearest the middle */

#endif /* SPIN_H */
