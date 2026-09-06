CFLAGS = -std=c11 -Wall -Wextra -Werror -g -I../main

all: test_textwrap test_font test_timecalc

test_textwrap: test_textwrap.c ../main/textwrap.c
	cc $(CFLAGS) -o $@ $^

test_font: test_font.c ../main/font.h
	cc $(CFLAGS) -o $@ test_font.c

test_timecalc: test_timecalc.c ../main/timecalc.c
	cc $(CFLAGS) -o $@ $^

clean:
	rm -f test_textwrap test_font test_timecalc

.PHONY: all clean
