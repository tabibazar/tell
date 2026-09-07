CFLAGS = -std=c11 -Wall -Wextra -Werror -g -I../main

all: test_textwrap test_font test_font_clock test_timecalc test_canvas test_usagedata test_pages test_views test_settings test_ds3231

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

test_usagedata: test_usagedata.c ../main/usagedata.c
	cc $(CFLAGS) -o $@ $^

test_pages: test_pages.c ../main/pages.c
	cc $(CFLAGS) -o $@ $^

test_views: test_views.c ../main/views.c ../main/canvas.c ../main/textwrap.c ../main/usagedata.c ../main/timecalc.c ../main/settings.c
	cc $(CFLAGS) -o $@ $^ -lm

test_settings: test_settings.c ../main/settings.c
	cc $(CFLAGS) -o $@ $^

test_ds3231: test_ds3231.c ../main/ds3231.c
	cc $(CFLAGS) -o $@ $^

clean:
	rm -f test_textwrap test_font test_font_clock test_timecalc test_canvas test_usagedata test_pages test_views test_settings test_ds3231

.PHONY: all clean
