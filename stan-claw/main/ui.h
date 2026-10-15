#ifndef UI_H
#define UI_H

/*
 * stan-claw's screens on the 480x480 panel: home (a talk button), listening,
 * thinking (what it heard), speaking/answer (what it heard, small; the
 * answer, large), errors, and whatever an agent shows through MCP. Whenever
 * mic_open is set a red LISTENING banner is drawn across the top, last, over
 * everything. UI_RESTING is the screen while it rests: black, a soft orb in
 * the middle (the backlight makes it breathe), the time under it and the
 * day in words. In a conversation the listening and speaking pages carry big
 * buttons (Again, Slower, Done; Stop), and a question needing a yes gets a
 * Yes/No card. UI_EASY is the See and hear settings page; UI_MESSAGE and
 * UI_REMINDER are cards for a family message and a spoken reminder. Home
 * carries the day and date, large, over the talk button. With an image set,
 * UI_RESTING shows that family photo (time and day on a dark band) in place
 * of the orb -- unless the orb is amber -- and UI_MESSAGE becomes a photo card.
 * Pure: host_tests/test_sc_ui.c renders every screen.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "canvas.h"
#include "voices.h"
#include "weather.h"

typedef enum { UI_HOME, UI_LISTENING, UI_THINKING, UI_SPEAKING, UI_ANSWER, UI_ERROR, UI_AGENT_TEXT, UI_AGENT_IMAGE,
               UI_RESTING, UI_SETTINGS, UI_VOICES, UI_EASY, UI_MESSAGE, UI_REMINDER, UI_SETUP } ui_mode_t;

typedef struct {
    ui_mode_t mode;
    bool mic_open;
    int now;                 /* minute of the day for the clock; -1 none */
    const char *ssid;        /* the WiFi joined, shown top centre; NULL or "" when none */
    const char *heard;       /* what the person said */
    const char *text;        /* the answer, an error's detail, an agent's text */
    const char *title;       /* an error's or an agent's title */
    const char *note;        /* small amber line: "no WiFi", "asked remote: search" */
    const uint16_t *image;   /* RGB565, img_w x img_h (each <= 480): UI_AGENT_IMAGE; a photo on UI_RESTING / UI_MESSAGE */
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
    /* care features */
    int wday, month, mday;   /* for the friendly clock: 0=Sunday..6, 1..12, 1..31; month 0 = unknown */
    int textsize;            /* 0 large (rooms_head 30 px answers), 1 extra large (sc_answer_xl 40 px) */
    bool contrast;           /* high contrast: pure black, yellow text, white for secondary */
    bool clarity;            /* UI_EASY: the clarity-for-hearing switch's state */
    bool convo;              /* UI_LISTENING / UI_SPEAKING inside a conversation: show the conversation buttons */
    const char *confirm;     /* UI_LISTENING: non-empty = the Yes/No confirm card with this summary */
    int spoken;              /* UI_SPEAKING: captions -- chars of `text` already spoken; <0 = all bright */
    const char *photo_from;  /* UI_RESTING / UI_MESSAGE: who sent v->image, e.g. "Reza" */
    const weather_t *weather;  /* UI_RESTING: now, today and tomorrow (NULL or !ok: none yet) */
    const char *place;       /* UI_RESTING: the neighbourhood the weather is for, "Willowdale" (NULL: none) */
    /* UI_SETUP: WiFi set up from a phone. The QR code joins the phone to the
       board's own setup network; v->text is the network being tried. */
    const uint8_t *qr;       /* qr_n x qr_n modules, 1 = dark */
    int qr_n;
    const char *setup_net, *setup_pass;   /* the setup network and its password, also shown in words */
    int setup_state;         /* 0 waiting for the phone, 1 joining v->text, 2 joined, 3 that failed */
} ui_view_t;

/* What a touch at (x, y) lands on, on the page v shows; *value carries the
   volume (0-100), the speed or model button, or the voice card's index
   (the middle one, or a neighbour tapped at the side), or a See and hear
   switch's choice (0 or 1). */
typedef enum { UI_HIT_NONE, UI_HIT_TALK, UI_HIT_GEAR, UI_HIT_BACK, UI_HIT_VOICE, UI_HIT_VOLUME,
               UI_HIT_SPEED, UI_HIT_MODEL, UI_HIT_CARD, UI_HIT_USE,
               UI_HIT_AGAIN, UI_HIT_SLOWER, UI_HIT_DONE, UI_HIT_YES, UI_HIT_NO, UI_HIT_EASY, UI_HIT_TEXTSIZE,
               UI_HIT_CONTRAST, UI_HIT_CLARITY, UI_HIT_CLOSE, UI_HIT_WIFI } ui_hit_t;
ui_hit_t ui_hit(const ui_view_t *v, int x, int y, int *value);


/* The volume a finger at x on the slider means, 0-100, wherever it has wandered vertically. */
int ui_slider_value(int x);

/* The friendly day for the resting screen: "Saturday afternoon" and "October 10".
   Part of day by minute: morning 05:00-11:59, afternoon 12:00-16:59, evening 17:00-20:59, night otherwise.
   month 0 (unknown) -> both empty. */
void ui_day_words(int wday, int month, int mday, int minute, char *part, size_t np, char *date, size_t nd);

#define UI_WAVE_BARS 48

void ui_draw(canvas_t *c, const ui_view_t *v);
bool ui_hit_talk(int x, int y);

#endif /* UI_H */
