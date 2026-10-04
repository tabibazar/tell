idf_component_register(SRCS "tiny1.c" INCLUDE_DIRS "." "../../main"
    REQUIRES esp_driver_i2c esp_driver_gpio esp_driver_spi esp_lcd esp_wifi esp_event esp_netif nvs_flash esp_timer esp_pm esp_driver_tsens)
