#ifndef UI_H
#define UI_H

/*
 * stan-claw's screens on the 480x480 panel: home (a talk button), listening,
 * thinking (what it heard), speaking/answer (what it heard, small; the
 * answer, large), errors, and whatever an agent shows through MCP. Whenever
 * mic_open is set a red LISTENING banner is drawn across the top, last, over
 * everything. UI_RESTING is the screen while it rests: black, a soft orb in
 * the middle (the backlight makes it breathe) and the time under it. Pure: host_tests/test_sc_ui.c renders every screen.
 */
#include <stdbool.h>
#include <stdint.h>

#include "canvas.h"
#include "voices.h"

typedef enum { UI_HOME, UI_LISTENING, UI_THINKING, UI_SPEAKING, UI_ANSWER, UI_ERROR, UI_AGENT_TEXT, UI_AGENT_IMAGE,
               UI_RESTING, UI_SETTINGS, UI_VOICES } ui_mode_t;

typedef struct {
    ui_mode_t mode;
    bool mic_open;
    int now;                 /* minute of the day for the clock; -1 none */
    const char *ssid;        /* the WiFi joined, shown top centre; NULL or "" when none */
    const char *heard;       /* what the person said */
    const char *text;        /* the answer, an error's detail, an agent's text */
    const char *title;       /* an error's or an agent's title */
    const char *note;        /* small amber line: "no WiFi", "asked remote: search" */
    const uint16_t *image;   /* RGB565, img_w x img_h, for UI_AGENT_IMAGE */
    int img_w, img_h;
    bool amber;              /* UI_RESTING: the orb amber, an agent asking for attention */
    const uint8_t *levels;   /* UI_LISTENING: the voice's loudness, 0-255, oldest first */
    int nlevels;             /* how many of UI_WAVE_BARS are filled */
    int listen_ms;           /* UI_LISTENING: how long the mics have been open */
    /* UI_SETTINGS */
    const char *voice_name;  /* the voice in use, "Roger" */
    int volume;              /* 0-100 */
    int speed;               /* 0 slower, 1 normal, 2 faster */
    int model;               /* 0 Sonnet 5, 1 Haiku 4.5 */
    /* UI_VOICES: the wheel of VOICES (voices.h) */
    float spin;              /* which card is in the middle, fractional while it turns */
    int cur;                 /* the voice in use, -1 none */
} ui_view_t;

/* What a touch at (x, y) lands on, on the page v shows; *value carries the
   volume (0-100), the speed or model button, or the voice card's index
   (the middle one, or a neighbour tapped at the side). */
typedef enum { UI_HIT_NONE, UI_HIT_TALK, UI_HIT_GEAR, UI_HIT_BACK, UI_HIT_VOICE, UI_HIT_VOLUME,
               UI_HIT_SPEED, UI_HIT_MODEL, UI_HIT_CARD, UI_HIT_USE } ui_hit_t;
ui_hit_t ui_hit(const ui_view_t *v, int x, int y, int *value);


/* The volume a finger at x on the slider means, 0-100, wherever it has wandered vertically. */
int ui_slider_value(int x);

#define UI_WAVE_BARS 48

void ui_draw(canvas_t *c, const ui_view_t *v);
bool ui_hit_talk(int x, int y);

#endif /* UI_H */
