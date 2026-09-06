if(CONFIG_SCREEN_BOARD_CROWPANEL_7)
    set(DISPLAY_SRC "display_rgb.c")
else()
    set(DISPLAY_SRC "display_st7789.c")
endif()

idf_component_register(
    SRCS "main.c" "canvas.c" "textwrap.c" "timecalc.c" "ble_uart.c" ${DISPLAY_SRC}
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_timer bt nvs_flash)
