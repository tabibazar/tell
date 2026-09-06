#include "ble_uart.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <string.h>

#define DEVICE_NAME    "ESP32-Screen"
#define MSG_MAX        512
#define FLUSH_IDLE_US  50000  /* a message is complete after 50 ms of quiet */

static const char *TAG = "ble_uart";

static ble_uart_cb_t s_cb;
static ble_uart_time_cb_t s_time_cb;
static uint8_t s_addr_type;
static char s_buf[MSG_MAX + 1];
static size_t s_len;
static esp_timer_handle_t s_flush_timer;

/* Nordic UART Service. BLE_UUID128_INIT takes bytes least-significant first,
   i.e. the textual UUID reversed. */
static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t rx_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
/* ...0004: clock sync, four bytes little-endian, seconds since local midnight. */
static const ble_uuid128_t time_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x04, 0x00, 0x40, 0x6E);

static void advertise(void);

static void flush_message(void *arg)
{
    (void)arg;
    s_buf[s_len] = '\0';
    ESP_LOGI(TAG, "message (%u bytes): %s", (unsigned)s_len, s_buf);
    if (s_cb) s_cb(s_buf, s_len);
    s_len = 0;
}

static int gatt_rx(uint16_t conn_handle, uint16_t attr_handle,
                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    uint16_t chunk = OS_MBUF_PKTLEN(ctxt->om);
    if (chunk > 0 && s_len < MSG_MAX) {
        uint16_t space = (uint16_t)(MSG_MAX - s_len);
        uint16_t take = chunk < space ? chunk : space;
        uint16_t copied = 0;
        ble_hs_mbuf_to_flat(ctxt->om, s_buf + s_len, take, &copied);
        s_len += copied;
    }

    /* Restart the idle timer: a long message arrives as several writes. */
    esp_timer_stop(s_flush_timer);
    esp_timer_start_once(s_flush_timer, FLUSH_IDLE_US);
    return 0;
}

static int gatt_time(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    if (OS_MBUF_PKTLEN(ctxt->om) != 4) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t raw[4];
    uint16_t copied = 0;
    ble_hs_mbuf_to_flat(ctxt->om, raw, sizeof raw, &copied);
    if (copied != 4) return BLE_ATT_ERR_UNLIKELY;

    uint32_t secs = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8)
                  | ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);
    if (secs >= 86400u) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    ESP_LOGI(TAG, "clock synced to %u s past midnight", (unsigned)secs);
    if (s_time_cb) s_time_cb(secs);
    return 0;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &rx_uuid.u,
                .access_cb = gatt_rx,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &time_uuid.u,
                .access_cb = gatt_time,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connect status=%d", event->connect.status);
        if (event->connect.status != 0) advertise();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect reason=%d", event->disconnect.reason);
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu now %d", event->mtu.value);
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    /* The 128-bit UUID (18 bytes) plus flags leaves no room for the name in a
       31-byte advertisement, so the name goes in the scan response. */
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return; }

    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (uint8_t *)DEVICE_NAME;
    rsp.name_len = strlen(DEVICE_NAME);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) { ESP_LOGE(TAG, "adv_rsp_set_fields rc=%d", rc); return; }

    struct ble_gap_adv_params adv = { 0 };
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv,
                           gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "adv_start rc=%d", rc);
    else ESP_LOGI(TAG, "advertising as " DEVICE_NAME);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    int rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) { ESP_LOGE(TAG, "infer_auto rc=%d", rc); return; }
    advertise();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_uart_start(ble_uart_cb_t on_message, ble_uart_time_cb_t on_time)
{
    s_cb = on_message;
    s_time_cb = on_time;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    const esp_timer_create_args_t targs = {
        .callback = flush_message,
        .name = "ble_flush",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_flush_timer));

    err = nimble_port_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init failed"); return err; }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svcs));
    ESP_ERROR_CHECK(ble_svc_gap_device_name_set(DEVICE_NAME));

    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
