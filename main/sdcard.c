#include "sdcard.h"

#include "ds3231.h"

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "sdcard";

static bool s_mounted = false;

/* Pins the B1 spike proved on real hardware: 1-bit SDMMC, clk=11 cmd=10 d0=9. */
esp_err_t sd_mount(void)
{
    if (s_mounted) return ESP_OK;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = 11;
    slot.cmd = 10;
    slot.d0 = 9;
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
