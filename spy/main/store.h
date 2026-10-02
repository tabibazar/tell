#ifndef STORE_H
#define STORE_H

/*
 * Where spy keeps its pictures: the microSD card (SDMMC 1-bit: CLK GPIO5,
 * CMD GPIO4, D0 GPIO6, the S3's internal pull-ups) at /sdcard.
 *
 *   /sdcard/tl/20261002/0731.jpg     the time-lapse frame taken at 07:31
 *   /sdcard/tl/20261002/07.mp4       the clip of 07:00-07:59
 *   /sdcard/pics/20261002-073112.jpg a "pic" asked for over Telegram
 */
#include <stdbool.h>
#include <stdint.h>

bool store_mount(void);
bool store_ok(void);

/* Free and total space in MB, or false without a card. */
bool store_space(uint32_t *free_mb, uint32_t *total_mb);

/* mkdir -p for one level under an existing parent; true if it is there. */
bool store_mkdir(const char *path);

#endif /* STORE_H */
