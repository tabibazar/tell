idf_component_register(
    SRCS "stanclaw.c" "lcd.c" "touch.c"
         "../../main/aafont.c" "../../main/vector.c" "../../main/canvas.c" "../../main/textwrap.c"
    INCLUDE_DIRS "." "../../main" "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES esp_wifi esp_netif esp_event esp_http_client esp_http_server esp-tls mbedtls nvs_flash json
             esp_timer esp_lcd esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_ledc esp_driver_uart)
