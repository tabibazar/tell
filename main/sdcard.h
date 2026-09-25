#ifndef SDCARD_H
#define SDCARD_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * envio's microSD card: 1-bit SDMMC on the pins B1's bring-up spike proved
 * (clk=11, cmd=10, d0=9), mounted at /sdcard. envio-only -- no other board
 * carries a card slot.
 */

/* Mounts the card once. Idempotent: a second call is a no-op that returns
   ESP_OK if the first mount succeeded. Never formats the user's card. */
esp_err_t sd_mount(void);

/* Unmounts the card, if mounted, and stops the SDMMC host, so the next
   sd_mount starts over: how a card that was pulled and pushed back in, which
   the old mount can no longer reach, is found again. Any file still open on
   it is lost. */
void sd_unmount(void);

/* Writes `data` (len bytes) to `path`, a path relative to /sdcard (e.g.
   "envio/IMG_20260919_120000.jpg"). Creates any parent directory under
   /sdcard that does not already exist, then creates or truncates the file.
   Mounts the card first if it is not already mounted. */
esp_err_t sd_write(const char *path, const uint8_t *data, size_t len);

/* Appends `data` (len bytes) to `path` (relative to /sdcard), creating the
   file and any parent directory if absent. Mounts the card first. Used by
   envo's 30 s environment CSV log. */
esp_err_t sd_append(const char *path, const char *data, size_t len);

/* True if `path` (relative to /sdcard) exists and the card is mounted. Lets a
   caller write a CSV header row only when it is creating the file. */
bool sd_exists(const char *path);

/* The first line of `path` (relative to /sdcard), without its line ending,
   into `line` -- empty for an empty file. Mounts the card first, so "not
   there" means not there rather than not yet looked at. ESP_OK, or
   ESP_ERR_NOT_FOUND when there is no such file, or another error when the
   card could not say. */
esp_err_t sd_first_line(const char *path, char *line, size_t size);

/* Formats "envio/IMG_YYYYMMDD_HHMMSS.jpg" from the DS3231, if the chip is
   present and its calendar holds a real year. Otherwise falls back to
   "envio/IMG_boot_<seq>.jpg" with a process-lifetime incrementing <seq>, so
   photos taken before the clock is set still get distinct names. */
void sd_photo_name(char *out, size_t n);

/* Free space on the card, in bytes, via esp_vfs_fat_info. Returns false
   (leaving *out_free untouched) if the card is not mounted or the FATFS
   query fails -- the caller (the gallery caption) just omits the figure. */
bool sd_free_bytes(uint64_t *out_free);

#endif /* SDCARD_H */
