# Every .c in this directory is part of the firmware, so a new page or payload
# section is a new file and nothing here changes -- but CMake lists the
# directory only when it configures, so after adding a file run
# `idf.py -B <build dir> reconfigure` (or touch this file) before building.
# The one exception to "every .c" is the display driver: each board has its
# own, and only one may be compiled.
if(CONFIG_SCREEN_BOARD_CROWPANEL_7)
    set(OTHER_DISPLAY "display_st7789.c")
else()
    set(OTHER_DISPLAY "display_rgb.c")
endif()

idf_component_register(
    SRC_DIRS "."
    EXCLUDE_SRCS ${OTHER_DISPLAY}
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_driver_i2c esp_timer bt nvs_flash)
