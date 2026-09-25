#include "sdcard.h"

#include "ds3231.h"

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "sdcard";

static bool s_mounted = false;

/*
 * SDMMC pins are per board, 1-bit in every case.
 *   envio   (Touch-LCD-3.5B): the B1 spike proved clk=11 cmd=10 d0=9.
 *   envo    (Touch-LCD-1.47):  from Waveshare's bsp_sdcard.h -- clk=16 cmd=15 d0=17.
 *   speaker (AUDIO-Board):     clk=40 cmd=42 d0=41, from the schematic and the
 *                              factory demo (docs/hardware/speaker-pinout.md).
 *                              The card's D3 is on the TCA9555's EXIO3, not a
 *                              GPIO: speaker_app.c leaves it an input, so its
 *                              10k pull-up holds it high and the card powers up
 *                              in SD mode rather than SPI. D1/D2 are pulled up
 *                              and not wired to the chip at all.
 */
#if CONFIG_SCREEN_BOARD_TOUCH_LCD_147
#define SD_PIN_CLK 16
#define SD_PIN_CMD 15
#define SD_PIN_D0  17
#elif CONFIG_SCREEN_BOARD_AUDIO_S3
#define SD_PIN_CLK 40
#define SD_PIN_CMD 42
#define SD_PIN_D0  41
#else
#define SD_PIN_CLK 11
#define SD_PIN_CMD 10
#define SD_PIN_D0  9
#endif

esp_err_t sd_mount(void)
{
    if (s_mounted) return ESP_OK;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = SD_PIN_CLK;
    slot.cmd = SD_PIN_CMD;
    slot.d0 = SD_PIN_D0;
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mnt = {
        .format_if_mount_failed = false,   /* never format the user's card */
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mnt, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        return err;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "mounted (%lluMB)",
             ((uint64_t)card->csd.capacity * card->csd.sector_size) >> 20);
    return ESP_OK;
}

/* mkdir, treating "already there" as success -- the normal case once a
   session has created a given day's or board's directory once. */
static esp_err_t mkdir_ok(const char *path)
{
    if (mkdir(path, 0775) == 0) return ESP_OK;
    if (errno == EEXIST) return ESP_OK;
    return ESP_FAIL;
}

/* Creates every directory in `path` (relative to /sdcard) up to but not
   including its final component. `path` has no leading slash. */
static esp_err_t make_parents(const char *path)
{
    char full[320];
    int n = snprintf(full, sizeof full, "/sdcard/%s", path);
    if (n <= 0 || (size_t)n >= sizeof full) return ESP_ERR_INVALID_ARG;

    for (char *p = full + strlen("/sdcard/"); *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        esp_err_t err = mkdir_ok(full);
        *p = '/';
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t sd_write(const char *path, const uint8_t *data, size_t len)
{
    if (!path) return ESP_ERR_INVALID_ARG;

    esp_err_t err = sd_mount();
    if (err != ESP_OK) return err;

    err = make_parents(path);
    if (err != ESP_OK) return err;

    char full[320];
    int n = snprintf(full, sizeof full, "/sdcard/%s", path);
    if (n <= 0 || (size_t)n >= sizeof full) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(full, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen %s failed: %s", full, strerror(errno));
        return ESP_FAIL;
    }
    size_t written = (len == 0) ? 0 : fwrite(data, 1, len, f);
    int close_err = fclose(f);
    if (written != len || close_err != 0) {
        ESP_LOGE(TAG, "write %s failed (wrote %u of %u)", full,
                 (unsigned)written, (unsigned)len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t sd_append(const char *path, const char *data, size_t len)
{
    if (!path) return ESP_ERR_INVALID_ARG;

    esp_err_t err = sd_mount();
    if (err != ESP_OK) return err;

    err = make_parents(path);
    if (err != ESP_OK) return err;

    char full[320];
    int n = snprintf(full, sizeof full, "/sdcard/%s", path);
    if (n <= 0 || (size_t)n >= sizeof full) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(full, "a");
    if (!f) {
        ESP_LOGE(TAG, "fopen(append) %s failed: %s", full, strerror(errno));
        return ESP_FAIL;
    }
    size_t written = (len == 0) ? 0 : fwrite(data, 1, len, f);
    int close_err = fclose(f);
    if (written != len || close_err != 0) {
        ESP_LOGE(TAG, "append %s failed (wrote %u of %u)", full,
                 (unsigned)written, (unsigned)len);
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool sd_exists(const char *path)
{
    if (!path || !s_mounted) return false;
    char full[320];
    int n = snprintf(full, sizeof full, "/sdcard/%s", path);
    if (n <= 0 || (size_t)n >= sizeof full) return false;
    struct stat st;
    return stat(full, &st) == 0;
}

esp_err_t sd_first_line(const char *path, char *line, size_t size)
{
    if (!path || !line || size == 0) return ESP_ERR_INVALID_ARG;
    line[0] = '\0';

    esp_err_t err = sd_mount();
    if (err != ESP_OK) return err;

    char full[320];
    int n = snprintf(full, sizeof full, "/sdcard/%s", path);
    if (n <= 0 || (size_t)n >= sizeof full) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(full, "r");
    if (!f) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    /* Only a file that could not be read is an error; an empty one is an
       empty first line. */
    bool failed = !fgets(line, (int)size, f) && ferror(f);
    fclose(f);
    if (failed) {
        line[0] = '\0';
        return ESP_FAIL;
    }
    line[strcspn(line, "\r\n")] = '\0';
    return ESP_OK;
}

void sd_photo_name(char *out, size_t n)
{
    static uint32_t s_boot_seq = 0;

    ds3231_date_t date;
    uint32_t secs_since_midnight;
    if (ds3231_read_date(&date) && date.year >= 2000 &&
        ds3231_read(&secs_since_midnight)) {
        int hh = (int)(secs_since_midnight / 3600) % 24;
        int mm = (int)(secs_since_midnight / 60) % 60;
        int ss = (int)(secs_since_midnight % 60);
        snprintf(out, n, "envio/IMG_%04d%02d%02d_%02d%02d%02d.jpg",
                 date.year, date.month, date.day, hh, mm, ss);
        return;
    }

    snprintf(out, n, "envio/IMG_boot_%u.jpg", (unsigned)(s_boot_seq++));
}

bool sd_free_bytes(uint64_t *out_free)
{
    if (!s_mounted || !out_free) return false;
    uint64_t total = 0, free_b = 0;
    esp_err_t err = esp_vfs_fat_info("/sdcard", &total, &free_b);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sd_free_bytes: esp_vfs_fat_info failed: %s", esp_err_to_name(err));
        return false;
    }
    *out_free = free_b;
    return true;
}
