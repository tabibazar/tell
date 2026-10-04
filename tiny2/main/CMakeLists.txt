idf_component_register(SRCS "tiny2.c" INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c esp_driver_gpio esp_driver_ledc esp_timer)
