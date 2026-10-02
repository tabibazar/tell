#include "store.h"

#include <errno.h>
#include <sys/stat.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "sdmmc_cmd.h"

static const char *TAG = "store";
static sdmmc_card_t *s_card;

bool store_ok(void) { return s_card != NULL; }

bool store_mount(void)
{
    if (s_card) return true;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;      /* 20 MHz: the pull-ups are internal */
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = GPIO_NUM_5;
    slot.cmd = GPIO_NUM_4;
    slot.d0 = GPIO_NUM_6;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false,         /* never wipe a card we cannot read */
        .max_files = 6,
        .allocation_unit_size = 32 * 1024,
    };
    esp_err_t e = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mc, &s_card);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "no card: %s", esp_err_to_name(e));
        s_card = NULL;
        return false;
    }
    uint32_t fr, tot;
    store_space(&fr, &tot);
    ESP_LOGI(TAG, "card %s, %lu MB free of %lu", s_card->cid.name, (unsigned long)fr, (unsigned long)tot);
    store_mkdir("/sdcard/tl");
    store_mkdir("/sdcard/pics");
    return true;
}

bool store_space(uint32_t *free_mb, uint32_t *total_mb)
{
    *free_mb = *total_mb = 0;
    if (!s_card) return false;
    uint64_t tot = 0, fr = 0;
    if (esp_vfs_fat_info("/sdcard", &tot, &fr) != ESP_OK) return false;
    *free_mb = (uint32_t)(fr >> 20);
    *total_mb = (uint32_t)(tot >> 20);
    return true;
}

bool store_mkdir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(path, 0775) == 0 || errno == EEXIST;
}
