#include "display.h"

/*
 * The display on a board that has none: speaker, the AUDIO-Board, whose LCD
 * connector is empty.
 *
 * main.c is shared by every board and names these functions all through its
 * page UI. speaker never reaches that UI -- app_main hands over to
 * speaker_app_main() on its first line -- but the code is still compiled and
 * linked, so something has to answer to the names. This does, and does
 * nothing: there is no panel to bring up, draw on or light.
 *
 * The canvas is real, only tiny, so that anything which did draw into it
 * would find a valid framebuffer rather than a NULL. One character cell at the
 * smallest scale is enough to be valid and too small to matter.
 */

static uint16_t s_fb[16 * 24];
static canvas_t s_canvas;
static bool s_ready;

esp_err_t display_init(void)
{
    if (!s_ready) {
        canvas_init(&s_canvas, s_fb, 16, 24, 1);
        s_ready = true;
    }
    return ESP_OK;
}

void display_show_text(const char *utf8) { (void)utf8; }

void display_show_big(const char *text) { (void)text; }

void display_set_brightness(int percent) { (void)percent; }

void display_sleep(bool asleep) { (void)asleep; }

canvas_t *display_canvas(void)
{
    display_init();
    return &s_canvas;
}

void display_blit(void) {}
