idf_component_register(
    SRCS "blinky1.c"
    INCLUDE_DIRS "." "../../main"
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_timer)
