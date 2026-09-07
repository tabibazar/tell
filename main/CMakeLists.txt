if(CONFIG_SCREEN_BOARD_CROWPANEL_7)
    set(DISPLAY_SRC "display_rgb.c")
else()
    set(DISPLAY_SRC "display_st7789.c")
endif()

idf_component_register(
    SRCS "main.c" "canvas.c" "textwrap.c" "timecalc.c" "ble_uart.c"
         "usagedata.c" "pages.c" "views.c" "settings.c" "gt911.c" "i2cbus.c" ${DISPLAY_SRC}
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_driver_i2c esp_timer bt nvs_flash)
