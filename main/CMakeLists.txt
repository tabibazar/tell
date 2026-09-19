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
# The IMU and the pages that need one went with the boards that had one; the
# drivers stay in the tree for whatever turns up next, excluded here rather
# than deleted. envio has a QMI8658 soldered on, so the sand could come back to
# her -- but not in this first cut, which brings the panel up and no more.
# Three boards now, three panel buses: the CrowPanel's RGB, lilly's i80, and
# envio's AXS15231B over QSPI. Each has its own display_*.c and only one is
# compiled. envio also carries axp2101.c, the PMIC that powers her panel rail;
# the others have no such chip, so it is excluded from their builds.
# envio keeps the IMU (qmi8658) and the shake detector (in shaketimer.c) for
# Pip, her desk familiar (pip.c), but not the sand or the level. The other
# boards have no sensor and carry none of it, Pip included.
if(CONFIG_SCREEN_BOARD_CROWPANEL_7)
    set(BOARD_EXCLUDES "display_i80.c" "display_axs15231b.c" "axp2101.c"
                       "qmi8658.c" "level.c" "particles.c" "shaketimer.c"
                       "pip.c" "axs_touch.c")
elseif(CONFIG_SCREEN_BOARD_TOUCH_LCD_35B)
    # An environment + weather display: her own QSPI panel and PMIC, the
    # AXS15231B touch for swipes, and the soldered QMI8658 for the maze --
    # but not Pip, the sand or the level. The GT911 is the CrowPanel's touch,
    # not hers.
    set(BOARD_EXCLUDES "display_rgb.c" "display_i80.c"
                       "level.c" "particles.c" "shaketimer.c"
                       "pip.c" "gt911.c")
else()
    set(BOARD_EXCLUDES "display_rgb.c" "display_axs15231b.c" "axp2101.c"
                       "qmi8658.c" "level.c" "particles.c" "shaketimer.c"
                       "pip.c" "axs_touch.c")
endif()

# One touch driver per board -- both define the same touch_* interface, so
# exactly one is compiled. gt911 on the CrowPanel, axs_touch on envio, and on a
# board with no touch it does not matter which is left in; gt911 stays.

# An environment logger has no use for the sensor toys, and dropping them
# takes the IMU driver with them. See SCREEN_ENV_ONLY in Kconfig.projbuild.
if(CONFIG_SCREEN_ENV_ONLY)
    # envio is ENV_ONLY but keeps her soldered QMI8658 for the maze; the other
    # sensor toys stay out.
    if(CONFIG_SCREEN_BOARD_TOUCH_LCD_35B)
        list(APPEND BOARD_EXCLUDES "level.c" "particles.c" "shaketimer.c")
    else()
        list(APPEND BOARD_EXCLUDES "qmi8658.c" "level.c" "particles.c" "shaketimer.c")
    endif()
else()
    list(APPEND BOARD_EXCLUDES "envstore.c" "envpage.c" "envflash.c")
endif()

# maze.c/maze_levels.c are new; only envio has the panel + IMU pairing the
# maze wants, so every other board excludes them.
if(NOT CONFIG_SCREEN_BOARD_TOUCH_LCD_35B)
    list(APPEND BOARD_EXCLUDES "maze.c" "maze_levels.c")
endif()

idf_component_register(
    SRC_DIRS "."
    EXCLUDE_SRCS ${BOARD_EXCLUDES}
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_driver_ledc esp_driver_i2c esp_driver_spi esp_driver_tsens esp_timer bt nvs_flash esp_event fatfs esp_driver_sdmmc sdmmc)
