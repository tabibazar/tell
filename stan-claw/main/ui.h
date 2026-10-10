#ifndef UI_H
#define UI_H

/*
 * stan-claw's screens on the 480x480 panel: home (a talk button), listening,
 * thinking (what it heard), speaking/answer (what it heard, small; the
 * answer, large), errors, and whatever an agent shows through MCP. Whenever
 * mic_open is set a red LISTENING banner is drawn across the top, last, over
 * everything. Pure: host_tests/test_sc_ui.c renders every screen.
 */
#include <stdbool.h>
#include <stdint.h>

#include "canvas.h"

typedef enum { UI_HOME, UI_LISTENING, UI_THINKING, UI_SPEAKING, UI_ANSWER, UI_ERROR, UI_AGENT_TEXT, UI_AGENT_IMAGE } ui_mode_t;

typedef struct {
    ui_mode_t mode;
    bool mic_open;
    int now;                 /* minute of the day for the clock; -1 none */
    const char *heard;       /* what the person said */
    const char *text;        /* the answer, an error's detail, an agent's text */
    const char *title;       /* an error's or an agent's title */
    const char *note;        /* small amber line: "no WiFi", "asked remote: search" */
    const uint16_t *image;   /* RGB565, img_w x img_h, for UI_AGENT_IMAGE */
    int img_w, img_h;
} ui_view_t;

void ui_draw(canvas_t *c, const ui_view_t *v);
bool ui_hit_talk(int x, int y);

#endif /* UI_H */
