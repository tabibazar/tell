CFLAGS = -std=c11 -Wall -Wextra -Werror -g -I../main

all: test_textwrap test_font

test_textwrap: test_textwrap.c ../main/textwrap.c
	cc $(CFLAGS) -o $@ $^

test_font: test_font.c ../main/font8x16.h
	cc $(CFLAGS) -o $@ test_font.c

clean:
	rm -f test_textwrap test_font

.PHONY: all clean
