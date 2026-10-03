idf_component_register(
    SRCS "blinky1.c" "airlog.c" "airui.c"
    INCLUDE_DIRS "." "../../main"
    REQUIRES esp_driver_i2c esp_wifi esp_event esp_netif nvs_flash esp_timer)
