/* tiny1, first light: what is on the board. Powers the I2C/TFT rail (GPIO7
   on Adafruit's Feather ESP32-S3 TFT), tries every safe pin pair as I2C and
   lists what answers with its ID register, and reads the ADC1 pins, in case
   the battery is divided down onto one. Prints it all every 10 s. */
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tiny1";

static void id_of(i2c_master_bus_handle_t bus, uint8_t addr)
{
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 100000 };
    i2c_master_dev_handle_t d;
    if (i2c_master_bus_add_device(bus, &dc, &d) != ESP_OK) return;
    /* The usual WHO_AM_I / chip-id registers of the likely parts. */
    static const uint8_t regs[] = { 0xD0, 0x00, 0x0F, 0x75, 0x00 };
    char line[96] = "";
    for (size_t i = 0; i < sizeof regs - 1; i++) {
        uint8_t v = 0xEE;
        if (i2c_master_transmit_receive(d, &regs[i], 1, &v, 1, 50) == ESP_OK)
            snprintf(line + strlen(line), sizeof line - strlen(line), " [%02X]=%02X", regs[i], v);
    }
    ESP_LOGI(TAG, "   0x%02X:%s", addr, line);
    i2c_master_bus_rm_device(d);
}

static void direct(int sda, int scl)
{
    i2c_master_bus_config_t bc = { .i2c_port = 1, .sda_io_num = sda, .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true };
    i2c_master_bus_handle_t bus;
    esp_err_t e = i2c_new_master_bus(&bc, &bus);
    if (e != ESP_OK) { ESP_LOGE(TAG, "bus %d/%d: %s", sda, scl, esp_err_to_name(e)); return; }
    char line[256] = "";
    int n = 0;
    for (int a = 0x08; a < 0x78; a++) {
        esp_err_t r = i2c_master_probe(bus, a, 50);
        if (r == ESP_OK) { snprintf(line + strlen(line), sizeof line - strlen(line), " 0x%02X", a); n++; }
        else if (r != ESP_ERR_NOT_FOUND && a == 0x08) ESP_LOGW(TAG, "probe 0x08 on %d/%d: %s", sda, scl, esp_err_to_name(r));
    }
    ESP_LOGI(TAG, "direct SDA %d SCL %d: %d device(s)%s", sda, scl, n, line);
    for (int a = 0x08; a < 0x78; a++) if (i2c_master_probe(bus, a, 50) == ESP_OK) id_of(bus, (uint8_t)a);
    i2c_del_master_bus(bus);
}

static void scan(void)
{
    direct(42, 41);
    direct(41, 42);
    /* Not: 0/3/45/46 straps (probing them later is harmless, but 45 is
       the backlight), 19/20 USB, 26-32 flash/PSRAM, 43/44 UART0. */
    /* 42 and 41 first: Adafruit's SDA and SCL. 21 is the power switch, 7
       the TFT's CS. */
    static const int pins[] = { 42, 41, 1, 2, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
                                33, 34, 35, 36, 37, 38, 39, 40, 47, 48 };
    const int np = sizeof pins / sizeof pins[0];
    int hits = 0;
    for (int i = 0; i < np; i++) {
        for (int j = 0; j < np; j++) {
            if (i == j) continue;
            /* Idle I2C lines rest high (pulled up). A pin held low is not
               one, and probing it only times out. */
            gpio_config_t in = { .pin_bit_mask = (1ULL << pins[i]) | (1ULL << pins[j]),
                                 .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_DISABLE };
            gpio_config(&in);
            esp_rom_delay_us(50);
            if (!gpio_get_level(pins[i]) || !gpio_get_level(pins[j])) continue;
            i2c_master_bus_config_t bc = { .i2c_port = 0, .sda_io_num = pins[i], .scl_io_num = pins[j],
                .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true };
            i2c_master_bus_handle_t bus;
            if (i2c_new_master_bus(&bc, &bus) != ESP_OK) continue;
            uint8_t found[16];
            int n = 0;
            for (int a = 0x08; a < 0x78 && n < 16; a++)
                if (i2c_master_probe(bus, a, 5) == ESP_OK) found[n++] = (uint8_t)a;
            if (n) {
                hits++;
                ESP_LOGI(TAG, "I2C SDA %d SCL %d: %d device(s)", pins[i], pins[j], n);
                for (int k = 0; k < n; k++) id_of(bus, found[k]);
            }
            i2c_del_master_bus(bus);
        }
    }
    ESP_LOGI(TAG, "I2C search done: %d pin pair(s)", hits);
}

static void adc_survey(void)
{
    adc_oneshot_unit_handle_t u;
    adc_oneshot_unit_init_cfg_t uc = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&uc, &u) != ESP_OK) return;
    char line[200] = "ADC1 (GPIO: mV approx):";
    for (int ch = 0; ch < 10; ch++) {               /* ADC1 ch n = GPIO n+1 */
        adc_oneshot_chan_cfg_t cc = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
        adc_oneshot_config_channel(u, ch, &cc);
        int raw = 0;
        adc_oneshot_read(u, ch, &raw);
        snprintf(line + strlen(line), sizeof line - strlen(line), " %d:%d", ch + 1, raw * 3100 / 4095);
    }
    ESP_LOGI(TAG, "%s", line);
    adc_oneshot_del_unit(u);
}

void app_main(void)
{
    /* TFT_I2C_POWER is GPIO21 on Adafruit's Feather ESP32-S3 TFT (GPIO7 is
       the TFT's chip select there): high powers the screen and the I2C
       sensors and their pull-ups. */
    gpio_config_t io = { .pin_bit_mask = 1ULL << 21, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    gpio_set_level(21, 1);
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    vTaskDelay(pdMS_TO_TICKS(300));
    for (;;) {
        /* Which pins rest high with our pull-ups off: I2C lines among them. */
        char hl[200] = "high with no pull-up:";
        static const int all[] = { 1, 2, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 33, 34, 35, 36,
                                   37, 38, 39, 40, 41, 42, 47, 48 };
        for (size_t k = 0; k < sizeof all / sizeof all[0]; k++) {
            gpio_config_t in = { .pin_bit_mask = 1ULL << all[k], .mode = GPIO_MODE_INPUT,
                                 .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_ENABLE };
            gpio_config(&in);
            esp_rom_delay_us(200);
            int pd = gpio_get_level(all[k]);           /* high even against a pull-down: driven or pulled up */
            if (pd) snprintf(hl + strlen(hl), sizeof hl - strlen(hl), " %d", all[k]);
        }
        ESP_LOGI(TAG, "%s", hl);
        adc_survey();
        scan();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
