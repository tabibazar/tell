# Every .c in this directory is part of the firmware, so a new page or payload
# section is a new file and nothing here changes -- but CMake lists the
# directory only when it configures, so after adding a file run
# `idf.py -B <build dir> reconfigure` (or touch this file) before building.
# The exceptions to "every .c" are per board. Each has its own display driver
# and only one may be compiled, though the two SPI boards share theirs. The
# IMU and the sand go to the boards that have a sensor: the Feather, where it
# is a breakout on the STEMMA QT port, and wave, where a QMI8658 is soldered
# on. The spirit level is the Feather's alone; the buttons are lilly's. lilly
# has no sensor of any kind, so she carries none of it and neither does the
# CrowPanel.
if(CONFIG_SCREEN_BOARD_CROWPANEL_7)
    set(BOARD_EXCLUDES "display_st7789.c" "display_i80.c"
                       "qmi8658.c" "level.c" "particles.c" "shaketimer.c")
elseif(CONFIG_SCREEN_BOARD_TDISPLAY_S3)
    set(BOARD_EXCLUDES "display_st7789.c" "display_rgb.c"
                       "qmi8658.c" "level.c" "particles.c" "shaketimer.c")
elseif(CONFIG_SCREEN_BOARD_WAVESHARE_147B)
    set(BOARD_EXCLUDES "display_rgb.c" "display_i80.c" "level.c")
else()
    set(BOARD_EXCLUDES "display_rgb.c" "display_i80.c")
endif()

idf_component_register(
    SRC_DIRS "."
    EXCLUDE_SRCS ${BOARD_EXCLUDES}
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_driver_ledc esp_driver_i2c esp_driver_tsens esp_timer bt nvs_flash)
