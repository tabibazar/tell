#include "envflash.h"

#include "esp_log.h"
#include "esp_partition.h"

/*
 * The envlog partition, wired to envstore's four function pointers.
 *
 * This is the only file that knows the log lives in flash at all; everything
 * about the ring itself is in envstore.c and runs on the host. Keeping the
 * split here is what lets the wrap, the recovery after a power cut and the
 * "never raise a bit without erasing" rule be tested against a fake NOR flash
 * rather than against a board that has to be power-cycled by hand.
 */

static const char *TAG = "envflash";
static const esp_partition_t *s_part;

static bool part_read(void *ctx, size_t off, void *dst, size_t len)
{
    (void)ctx;
    return esp_partition_read(s_part, off, dst, len) == ESP_OK;
}

static bool part_write(void *ctx, size_t off, const void *src, size_t len)
{
    (void)ctx;
    return esp_partition_write(s_part, off, src, len) == ESP_OK;
}

static bool part_erase(void *ctx, size_t off, size_t len)
{
    (void)ctx;
    return esp_partition_erase_range(s_part, off, len) == ESP_OK;
}

bool envflash_open(envflash_t *out)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      (esp_partition_subtype_t)0x40, "envlog");
    if (s_part == NULL) {
        ESP_LOGW(TAG, "no envlog partition; readings will not be kept");
        return false;
    }
    out->ctx = NULL;
    out->size = s_part->size;
    out->sector_size = 4096;
    out->read = part_read;
    out->write = part_write;
    out->erase = part_erase;
    ESP_LOGI(TAG, "envlog: %u KB at 0x%lx", (unsigned)(s_part->size / 1024),
             (unsigned long)s_part->address);
    return true;
}
