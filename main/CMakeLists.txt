idf_component_register(
    SRCS "main.c" "display.c" "textwrap.c" "ble_uart.c" "timecalc.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_timer bt nvs_flash)
