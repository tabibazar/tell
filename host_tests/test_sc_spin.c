/* stan-claw's voice wheel: drag, flick, rubber band, settling on a card. */
#include "spin.h"
#include <math.h>
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int settle(spin_t *s)
{
    int steps = 0;
    while (spin_step(s, 0.03f) && steps < 1000) steps++;
    return steps;
}

int main(void)
{
    spin_t s;
    spin_init(&s, 25, 5);
    CHECK(spin_index(&s) == 5 && !spin_step(&s, 0.03f));

    /* Drag left by one card and a bit, let go slowly: settles on the next card. */
    spin_grab(&s);
    spin_drag(&s, -SPIN_CARD_PX * 1.3f);
    CHECK(fabsf(s.pos - 6.3f) < 0.01f);
    spin_release(&s, 0);
    int st = settle(&s);
    CHECK(st < 100 && s.pos == 6.0f && spin_index(&s) == 6);

    /* A flick to the left travels several cards and still lands on one. */
    spin_grab(&s);
    spin_drag(&s, -40);
    spin_release(&s, -2000);
    settle(&s);
    CHECK(spin_index(&s) >= 9 && s.pos == (float)spin_index(&s));

    /* A hard flick right from card 3 runs into the first card and stops there. */
    spin_go(&s, 3);
    settle(&s);
    spin_grab(&s);
    spin_release(&s, 9000);
    settle(&s);
    CHECK(s.pos == 0.0f);

    /* Dragging past the start gives only a third. */
    spin_grab(&s);
    spin_drag(&s, SPIN_CARD_PX * 3);
    CHECK(fabsf(s.pos + 1.0f) < 0.01f);
    spin_release(&s, 0);
    settle(&s);
    CHECK(s.pos == 0.0f);

    /* Going to a card glides there. */
    spin_go(&s, 3);
    CHECK(spin_step(&s, 0.03f));
    settle(&s);
    CHECK(s.pos == 3.0f);
    spin_go(&s, 24);
    settle(&s);
    CHECK(s.pos == 24.0f);

    spin_init(&s, 0, 0);                                     /* no voices: still sane */
    CHECK(spin_index(&s) == 0 && !spin_step(&s, 0.03f));

    printf(fails ? "%d FAILED\n" : "sc_spin: all passed\n", fails);
    return fails != 0;
}
