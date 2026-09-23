# Vendored esp_lcd driver for the JD9853 controller, as used by Waveshare's own
# ESP-IDF BSP for the ESP32-S3-Touch-LCD-1.47 (envo). Apache-2.0, Espressif.
# Only display_jd9853.c references it, so on every other board it is built but
# not linked -- the same arrangement as the managed AXS15231B driver.
idf_component_register(SRCS "esp_lcd_jd9853.c"
                       INCLUDE_DIRS "include"
                       REQUIRES "driver" "esp_lcd")
