#include "hotspot.h"

#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "spy_secrets.h"

static const char *TAG = "hotspot";
static esp_netif_t *s_ap;
static bool s_inited, s_on;
static int64_t s_until;

bool hotspot_configured(void) { return strlen(SPY_WIFI_PASS) >= 8; }
bool hotspot_is_on(void) { return s_on; }

int hotspot_clients(void)
{
    wifi_sta_list_t l;
    return s_on && esp_wifi_ap_get_sta_list(&l) == ESP_OK ? l.num : 0;
}

static bool init_once(void)
{
    if (s_inited) return true;
    /* The driver only once asked for: it holds internal RAM spy otherwise
       keeps for the camera, the encoder and TLS. */
    s_ap = esp_netif_create_default_wifi_ap();
    wifi_init_config_t c = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&c) != ESP_OK) return false;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    wifi_config_t ap = {
        .ap = {
            .channel = 6,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .required = false },
        },
    };
    strlcpy((char *)ap.ap.ssid, SPY_WIFI_SSID, sizeof ap.ap.ssid);
    ap.ap.ssid_len = strlen(SPY_WIFI_SSID);
    strlcpy((char *)ap.ap.password, SPY_WIFI_PASS, sizeof ap.ap.password);
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap);

    /* Clients are told to use a public DNS, which reaches it through the NAT:
       it outlives a redial, unlike the carrier's servers. */
    esp_netif_dns_info_t dns = { 0 };
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);
    uint8_t offer = 1;      /* DHCPS_OFFER_DNS */
    esp_netif_dhcps_stop(s_ap);
    esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof offer);
    esp_netif_dhcps_start(s_ap);
    s_inited = true;
    return true;
}

bool hotspot_on(void)
{
    if (!hotspot_configured() || !init_once()) return false;
    if (!s_on) {
        if (esp_wifi_start() != ESP_OK) return false;
        if (esp_netif_napt_enable(s_ap) != ESP_OK) ESP_LOGW(TAG, "NAT would not start");
        s_on = true;
        ESP_LOGI(TAG, "on: \"%s\"", SPY_WIFI_SSID);
    }
    s_until = esp_timer_get_time() + (int64_t)HOTSPOT_HOURS * 3600 * 1000000;
    return true;
}

void hotspot_off(void)
{
    if (!s_on) return;
    esp_wifi_stop();
    s_on = false;
    ESP_LOGI(TAG, "off");
}

void hotspot_tick(void)
{
    if (s_on && esp_timer_get_time() > s_until) hotspot_off();
}
