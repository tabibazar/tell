/* tiny2, wiring search: where did the OV7670's wires land? The pins that
   rest high are its SCCB lines (pulled up on the module); RESET and PWDN are
   left floating (the module's own defaults run it); a 10 MHz clock goes on
   each other candidate pin in turn as XCLK while 0x21 is asked on each
   order of the high pins. */
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tiny2";
/* 0, 3, 45 and 46 included now: straps only matter at reset, and the photo of
   2026-10-04 shows wires on the header's 3/46 positions. */
static const int PINS[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 45, 46, 47 };
#define NP (int)(sizeof PINS / sizeof PINS[0])

static bool ask(int sda, int scl, uint8_t *pid)
{
    i2c_master_bus_config_t bc = { .i2c_port = 0, .sda_io_num = sda, .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return false;
    /* Slowly (10 kHz): this module may have no pull-up on one SCCB line,
       leaving only the S3's weak internal ones. */
    i2c_device_config_t sd = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x21, .scl_speed_hz = 10000 };
    i2c_master_dev_handle_t sdev;
    bool ok = false;
    if (i2c_master_bus_add_device(bus, &sd, &sdev) == ESP_OK) {
        uint8_t r = 0x0A;
        ok = i2c_master_transmit(sdev, &r, 1, 50) == ESP_OK;
        i2c_master_bus_rm_device(sdev);
    }
    if (ok && pid) {
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x21, .scl_speed_hz = 100000 };
        i2c_master_dev_handle_t d;
        uint8_t r = 0x0A;
        if (i2c_master_bus_add_device(bus, &dc, &d) == ESP_OK) {
            i2c_master_transmit(d, &r, 1, 50);
            i2c_master_receive(d, pid, 1, 50);
            i2c_master_bus_rm_device(d);
        }
    }
    i2c_del_master_bus(bus);
    return ok;
}

/* Reza: "6 and 7 are reset and pwdn". RESET (6) high, PWDN (7) low; SIOD on
   5 (it rests high); every other pin tried as SIOC with every other as
   XCLK. Then, if nothing, every pin as SIOD too. */
static void search(void)
{
    {
        char hl[200] = "pins resting high (pull-downs on):";
        for (int i = 0; i < NP; i++) {
            gpio_reset_pin(PINS[i]);
            gpio_config_t in = { .pin_bit_mask = 1ULL << PINS[i], .mode = GPIO_MODE_INPUT, .pull_down_en = GPIO_PULLDOWN_ENABLE };
            gpio_config(&in);
            esp_rom_delay_us(300);
            if (gpio_get_level(PINS[i])) snprintf(hl + strlen(hl), sizeof hl - strlen(hl), " %d", PINS[i]);
            gpio_set_pull_mode(PINS[i], GPIO_FLOATING);
        }
        ESP_LOGI(TAG, "%s", hl);
    }
    gpio_config_t out = { .pin_bit_mask = (1ULL << 6) | (1ULL << 7), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&out);
    gpio_set_level(7, 0);              /* PWDN: awake */
    gpio_set_level(6, 0);              /* RESET pulsed */
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(6, 1);
    ledc_timer_config_t tc = { .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_1_BIT,
                               .timer_num = LEDC_TIMER_0, .freq_hz = 10000000, .clk_cfg = LEDC_AUTO_CLK };
    ledc_timer_config(&tc);
    int hits = 0;
    for (int pass = 0; pass < 2 && !hits; pass++) {
        for (int x = 0; x < NP && !hits; x++) {
            int xc = PINS[x];
            if (xc == 6 || xc == 7) continue;
            ledc_channel_config_t cc = { .gpio_num = xc, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
                                         .timer_sel = LEDC_TIMER_0, .duty = 1, .hpoint = 0 };
            ledc_channel_config(&cc);
            vTaskDelay(pdMS_TO_TICKS(15));
            for (int i = 0; i < NP && !hits; i++) {
                int sda = pass == 0 ? 5 : PINS[i];
                if (pass == 0 && i > 0) break;
                if (sda == xc || sda == 6 || sda == 7) continue;
                for (int j = 0; j < NP && !hits; j++) {
                    int scl = PINS[j];
                    if (scl == sda || scl == xc || scl == 6 || scl == 7) continue;
                    uint8_t pid = 0;
                    if (ask(sda, scl, &pid)) {
                        hits++;
                        ESP_LOGI(TAG, "FOUND: XCLK GPIO%d, SIOD GPIO%d, SIOC GPIO%d -> PID 0x%02X", xc, sda, scl, pid);
                    }
                }
            }
            ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
            gpio_reset_pin(xc);
        }
    }
    ESP_LOGI(TAG, "search done: %d hit(s)", hits);
}

void app_main(void)
{
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    vTaskDelay(pdMS_TO_TICKS(300));
    for (;;) {
        search();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
