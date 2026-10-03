/* tiny1: a clone of Adafruit's Feather ESP32-S3 TFT. For now: the I2C rail
   up, the sensors checked, and the NeoPixel dark.

   Pins as found on the board (2026-10-03) and as on Adafruit's design:
     GPIO21  TFT + I2C power (high = on)
     GPIO42  SDA, GPIO41 SCL: BMP280 at 0x77, QMI8658 at 0x6B
     GPIO33  NeoPixel data, GPIO34 its power (high = on)
   The pin search that found them left the NeoPixel latched bright white
   (it took the I2C probing on GPIO33 for colour data); it is now sent
   black and its power cut, and nothing else touches those pins. */
#include <stdio.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "tiny1";

static void neopixel_off(void)
{
    led_strip_config_t sc = { .strip_gpio_num = 33, .max_leds = 1, .led_model = LED_MODEL_WS2812,
                              .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB };
    led_strip_rmt_config_t rc = { .resolution_hz = 10 * 1000 * 1000 };
    led_strip_handle_t led;
    gpio_config_t pw = { .pin_bit_mask = 1ULL << 34, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&pw);
    gpio_set_level(34, 1);                     /* powered just long enough to hear "black" */
    vTaskDelay(pdMS_TO_TICKS(5));
    if (led_strip_new_rmt_device(&sc, &rc, &led) == ESP_OK) {
        led_strip_clear(led);
        vTaskDelay(pdMS_TO_TICKS(5));
        led_strip_del(led);
    }
    gpio_set_level(34, 0);                     /* and then not at all */
}

static void sensors_check(void)
{
    i2c_master_bus_config_t bc = { .i2c_port = 0, .sda_io_num = 42, .scl_io_num = 41,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return;
    static const struct { uint8_t addr, reg; const char *name; } parts[] = {
        { 0x77, 0xD0, "BMP280 (id 0x58)" }, { 0x6B, 0x00, "QMI8658 (id 0x05)" },
    };
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = parts[i].addr, .scl_speed_hz = 100000 };
        i2c_master_dev_handle_t d;
        uint8_t id = 0;
        if (i2c_master_bus_add_device(bus, &dc, &d) == ESP_OK) {
            i2c_master_transmit_receive(d, &parts[i].reg, 1, &id, 1, 50);
            i2c_master_bus_rm_device(d);
        }
        ESP_LOGI(TAG, "0x%02X %s: id 0x%02X", parts[i].addr, parts[i].name, id);
    }
    i2c_del_master_bus(bus);
}

void app_main(void)
{
    neopixel_off();
    gpio_config_t io = { .pin_bit_mask = 1ULL << 21, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    gpio_set_level(21, 1);                     /* TFT + I2C power */
    vTaskDelay(pdMS_TO_TICKS(100));
    sensors_check();
    ESP_LOGI(TAG, "NeoPixel off; waiting for a purpose");
}
