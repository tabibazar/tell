# Every .c in this directory is part of the firmware, so a new page or payload
# section is a new file and nothing here changes -- but CMake lists the
# directory only when it configures, so after adding a file run
# `idf.py -B <build dir> reconfigure` (or touch this file) before building.
# The exceptions to "every .c" are per board. Each has its own display driver
# and only one may be compiled. The IMU driver goes to the two boards that can
# have the sensor plugged into them, and both pour the grains; the spirit
# level is the Feather's alone, and so are lilly's buttons hers. The CrowPanel
# carries none of it.
if(CONFIG_SCREEN_BOARD_CROWPANEL_7)
    set(BOARD_EXCLUDES "display_st7789.c" "display_i80.c" "buttons.c"
                       "qmi8658.c" "level.c" "particles.c")
elseif(CONFIG_SCREEN_BOARD_TDISPLAY_S3)
    set(BOARD_EXCLUDES "display_st7789.c" "display_rgb.c" "level.c")
else()
    set(BOARD_EXCLUDES "display_rgb.c" "display_i80.c" "buttons.c")
endif()

idf_component_register(
    SRC_DIRS "."
    EXCLUDE_SRCS ${BOARD_EXCLUDES}
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_driver_i2c esp_timer bt nvs_flash)
