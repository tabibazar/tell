CFLAGS = -std=c11 -Wall -Wextra -Werror -g -I../main

# Every page and every payload section is its own file; pick them all up.
VIEW_SRCS = $(wildcard ../main/view_*.c)
UD_SRCS = ../main/usagedata.c $(wildcard ../main/ud_*.c)

all: test_textwrap test_font test_font_clock test_timecalc test_canvas test_usagedata test_pages test_views test_settings test_ds3231 test_level test_particles

test_textwrap: test_textwrap.c ../main/textwrap.c
	cc $(CFLAGS) -o $@ $^

test_font: test_font.c ../main/font.h ../main/font_12x24.h
	cc $(CFLAGS) -o $@ test_font.c

test_font_clock: test_font_clock.c ../main/font_clock.h
	cc $(CFLAGS) -o $@ test_font_clock.c

test_timecalc: test_timecalc.c ../main/timecalc.c
	cc $(CFLAGS) -o $@ $^

test_canvas: test_canvas.c ../main/canvas.c ../main/textwrap.c
	cc $(CFLAGS) -o $@ $^


test_templog: test_templog.c ../main/templog.c ../main/canvas.c ../main/textwrap.c
	cc $(CFLAGS) -o $@ $^

test_bme280: test_bme280.c ../main/bme280.c
	cc $(CFLAGS) -o $@ $^

test_shaketimer: test_shaketimer.c ../main/shaketimer.c
	cc $(CFLAGS) -o $@ $^

test_usagedata: test_usagedata.c $(UD_SRCS)
	cc $(CFLAGS) -o $@ $^

test_pages: test_pages.c ../main/pages.c
	cc $(CFLAGS) -o $@ $^

test_views: test_views.c $(VIEW_SRCS) ../main/pagedefs.c ../main/canvas.c ../main/textwrap.c $(UD_SRCS) ../main/timecalc.c ../main/settings.c ../main/pages.c
	cc $(CFLAGS) -o $@ $^ -lm

test_settings: test_settings.c ../main/settings.c
	cc $(CFLAGS) -o $@ $^

test_ds3231: test_ds3231.c ../main/ds3231.c
	cc $(CFLAGS) -o $@ $^

test_level: test_level.c ../main/level.c ../main/canvas.c ../main/textwrap.c
	cc $(CFLAGS) -o $@ $^ -lm

test_particles: test_particles.c ../main/particles.c ../main/canvas.c ../main/textwrap.c
	cc $(CFLAGS) -o $@ $^

clean:
	rm -f test_textwrap test_font test_font_clock test_timecalc test_canvas test_usagedata test_pages test_views test_settings test_ds3231 test_level test_particles

.PHONY: all clean
