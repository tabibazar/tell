# Every .c in this directory is part of the firmware, so a new page or payload
# section is a new file and nothing here changes -- but CMake lists the
# directory only when it configures, so after adding a file run
# `idf.py -B <build dir> reconfigure` (or touch this file) before building.
# The exceptions to "every .c" are per board. Each has its own display driver
# and only one may be compiled; and the IMU, the barometer and the spirit
# level they feed exist only on the Feather, so the big board does not carry
# them at all.
if(CONFIG_SCREEN_BOARD_CROWPANEL_7)
    set(BOARD_EXCLUDES "display_st7789.c" "qmi8658.c" "level.c")
else()
    set(BOARD_EXCLUDES "display_rgb.c")
endif()

idf_component_register(
    SRC_DIRS "."
    EXCLUDE_SRCS ${BOARD_EXCLUDES}
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_driver_i2c esp_timer bt nvs_flash)
