CFLAGS = -std=c11 -Wall -Wextra -Werror -g -I../main

# Every page and every payload section is its own file; pick them all up.
VIEW_SRCS = $(wildcard ../main/view_*.c)
UD_SRCS = ../main/usagedata.c $(wildcard ../main/ud_*.c)

all: test_textwrap test_font test_font_clock test_timecalc test_canvas test_usagedata test_pages test_views test_drift test_envstore test_envpage test_ens160 test_settings test_ds3231 test_level test_levelbig test_particles test_pip test_maze test_bubblelevel test_vector test_moonphase test_pcf85063 test_face

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

test_drift: test_drift.c ../main/drift.c ../main/canvas.c ../main/textwrap.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_envstore: test_envstore.c ../main/envstore.c
	$(CC) $(CFLAGS) -o $@ $^

test_envpage: test_envpage.c ../main/envpage.c ../main/canvas.c ../main/textwrap.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_ens160: test_ens160.c ../main/ens160.c ../main/aht21.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

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

test_levelbig: test_levelbig.c ../main/levelbig.c ../main/level.c ../main/canvas.c ../main/textwrap.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_particles: test_particles.c ../main/particles.c ../main/canvas.c ../main/textwrap.c
	cc $(CFLAGS) -o $@ $^

test_vector: test_vector.c ../main/vector.c ../main/canvas.c ../main/textwrap.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_moonphase: test_moonphase.c ../main/moonphase.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_pcf85063: test_pcf85063.c ../main/pcf85063.c ../main/ds3231.c
	$(CC) $(CFLAGS) -o $@ $^

test_face: test_face.c ../main/face.c ../main/vector.c ../main/vfont.c ../main/moonphase.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

clean:
	rm -f test_vector test_moonphase test_pcf85063 test_face test_ens160 test_envpage test_envstore test_drift test_textwrap test_font test_font_clock test_timecalc test_canvas test_usagedata test_pages test_views test_settings test_ds3231 test_level test_levelbig test_particles test_pip test_maze

.PHONY: all clean

test_pip: test_pip.c ../main/pip.c ../main/canvas.c ../main/textwrap.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_maze: test_maze.c ../main/maze.c ../main/maze_levels.c ../main/canvas.c ../main/textwrap.c
	$(CC) $(CFLAGS) -o $@ $^ -lm

test_bubblelevel: test_bubblelevel.c ../main/bubblelevel.c ../main/canvas.c ../main/textwrap.c
	$(CC) $(CFLAGS) -o $@ $^ -lm
