#include "bubblelevel.h"
#include "canvas.h"
#include "palette.h"
#include <stdio.h>

static int failures;
static void expect(const char *what, int cond) {
    if (cond) { printf("ok   %s\n", what); return; }
    printf("FAIL %s\n", what); failures++;
}

static void step(bubble_t *b, float gx, float gy, float secs) {
    for (float t = 0; t < secs; t += 0.02f) bubble_update(b, gx, gy, 0.02f);
}

static void test_flat_advances(void) {
    bubble_t b; bubble_init(&b);
    int level0 = b.level;
    step(&b, 0.0f, 0.0f, 2.02f);   /* just over BUBBLE_HOLD_WIN, flat board */
    expect("holding flat advances the level", b.level == level0 + 1);
    expect("held_s resets after advancing", b.held_s < 0.05f);
}

static void test_tilt_moves_bubble_and_resets_hold(void) {
    bubble_t b; bubble_init(&b);
    step(&b, 0.9f, 0.0f, 0.5f);   /* hard tilt, long enough to settle */
    expect("hard tilt moves the bubble off centre", b.bx != 0.0f || b.by != 0.0f);
    expect("held_s stays zero while off target", b.held_s == 0.0f);
}

static void test_bubble_floats_opposite_gravity(void) {
    bubble_t b; bubble_init(&b);
    step(&b, 0.9f, 0.0f, 0.5f);
    /* gravity to the right (+gx) => bubble floats left (-bx) */
    expect("bubble floats opposite gravity", b.bx < 0.0f);
}

static void test_win_after_all_levels(void) {
    bubble_t b; bubble_init(&b);
    for (int i = 0; i < BUBBLE_LEVELS; i++) {
        expect("not won yet", !b.won);
        step(&b, 0.0f, 0.0f, 2.2f);
    }
    expect("won after holding through all levels", b.won);
}

static void test_reset_keeps_level(void) {
    bubble_t b; bubble_init(&b);
    step(&b, 0.0f, 0.0f, 2.2f);   /* advance to level 1 */
    int level_after = b.level;
    expect("advanced past level 0", level_after > 0);
    step(&b, 0.9f, 0.0f, 0.3f);   /* build up some held_s risk / move bubble */
    bubble_reset(&b);
    expect("reset keeps the level", b.level == level_after);
    expect("reset clears held_s", b.held_s == 0.0f);
}

static void test_angles(void) {
    bubble_t b; bubble_init(&b);
    bubble_update(&b, 0.5f, 0.0f, 0.02f);
    expect("roll_deg follows gx sign", b.roll_deg > 0.0f);
}

static void test_draw(void) {
    static uint16_t fb[320 * 480];
    canvas_t c; canvas_init(&c, fb, 320, 480, 1);
    bubble_t b; bubble_init(&b);
    bubble_draw(&c, &b);
    /* bubble at centre should be amber somewhere near the middle */
    int found_bubble = 0;
    int cx = c.w / 2, cy = c.h / 2;
    for (int y = cy - 12; y <= cy + 12; y++)
        for (int x = cx - 12; x <= cx + 12; x++)
            if (fb[y * c.w + x] == PAL_A1) found_bubble = 1;
    expect("bubble drawn in amber near centre", found_bubble);
}

int main(void) {
    test_flat_advances();
    test_tilt_moves_bubble_and_resets_hold();
    test_bubble_floats_opposite_gravity();
    test_win_after_all_levels();
    test_reset_keeps_level();
    test_angles();
    test_draw();
    printf("%s\n", failures ? "FAILURES" : "all pass");
    return failures ? 1 : 0;
}
