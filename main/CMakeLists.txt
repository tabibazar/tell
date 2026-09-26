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
# The IMU and the pages that need one go to the boards that have a sensor. wave
# is back -- a Waveshare 1.47B with a QMI8658 soldered on -- so she carries the
# IMU driver again and the things it feeds: the sand, the spirit level and the
# shake timer. The drivers a board does not want stay in the tree, excluded here
# rather than deleted.
# Four boards now, four panel situations: the CrowPanel's RGB, lilly's i80,
# wave's ST7789 over SPI (display_st7789.c, back with her), and envio's
# AXS15231B over QSPI. Each has its own display_*.c and only one is compiled.
# envio also carries axp2101.c, the PMIC that powers her panel rail; the others
# have no such chip, so it is excluded from their builds.
# envio keeps the IMU (qmi8658) and the shake detector (in shaketimer.c) for
# Pip, her desk familiar (pip.c), but not the sand or the level. lilly and the
# CrowPanel have no sensor and carry none of it, Pip included.
if(CONFIG_SCREEN_BOARD_CROWPANEL_7)
    set(BOARD_EXCLUDES "display_st7789.c" "display_jd9853.c" "display_i80.c" "display_axs15231b.c" "axp2101.c"
                       "qmi8658.c" "level.c" "levelbig.c" "particles.c" "shaketimer.c"
                       "pip.c" "axs_touch.c" "axs5106l_touch.c" "sdcard.c" "camera.c")
elseif(CONFIG_SCREEN_BOARD_TOUCH_LCD_147)
    # envo: a Waveshare ESP32-S3-Touch-LCD-1.47 with a JD9853 over SPI
    # (display_jd9853.c + the vendored esp_lcd_jd9853 component) and an
    # environment sensor array soldered to its I2C header. An ENV_ONLY logger
    # with NO IMU, so it carries none of the sensor toys. It DOES have a microSD
    # slot, so sdcard.c stays -- the 30 s CSV log lives there. No PMIC, no
    # camera, no AXS15231B/RGB/i80/ST7789 panel. Touch is the AXS5106L at 0x63
    # (axs5106l_touch.c), so gt911.c is excluded here -- both define touch_*.
    set(BOARD_EXCLUDES "display_st7789.c" "display_rgb.c" "display_i80.c" "display_axs15231b.c"
                       "axp2101.c" "axs_touch.c" "gt911.c" "camera.c"
                       "qmi8658.c" "level.c" "levelbig.c" "particles.c" "shaketimer.c" "pip.c")
elseif(CONFIG_SCREEN_BOARD_TOUCH_LCD_169)
    # watch: a Waveshare ESP32-S3-LCD-1.69 -- the variant WITHOUT touch, as the
    # PCB says -- an ST7789V2 standing upright at 240x280 over SPI
    # (display_st7789.c), a QMI8658 for the sand and raise-to-wake, and a
    # PCF85063 clock (pcf85063.c, which answers to the ds3231_* names). No
    # touch, so gt911.c stays in as the harmless default, as on wave and lilly.
    # No PMIC, no card slot, no camera.
    set(BOARD_EXCLUDES "display_rgb.c" "display_jd9853.c" "display_i80.c" "display_axs15231b.c"
                       "axp2101.c" "levelbig.c"
                       "pip.c" "axs_touch.c" "axs5106l_touch.c" "sdcard.c" "camera.c")
elseif(CONFIG_SCREEN_BOARD_WAVESHARE_147B)
    # wave is back: a Waveshare ESP32-S3-LCD-1.47B with an ST7789 over SPI and
    # a QMI8658 soldered on, so she compiles display_st7789.c and the sensor
    # things -- the sand (particles.c), the spirit level (level.c, the small
    # rendering, not envio's levelbig.c) and the shake timer. This unit has no
    # environment sensor wired, so she is not ENV_ONLY and carries none of the
    # weather/air pages. No touch (gt911.c stays as the harmless default, as on
    # lilly), no PMIC, no card slot, no camera, no Pip.
    set(BOARD_EXCLUDES "display_rgb.c" "display_jd9853.c" "display_i80.c" "display_axs15231b.c"
                       "axp2101.c" "levelbig.c"
                       "pip.c" "axs_touch.c" "axs5106l_touch.c" "sdcard.c" "camera.c")
elseif(CONFIG_SCREEN_BOARD_TOUCH_LCD_35B)
    # An environment + weather display: her own QSPI panel and PMIC, the
    # AXS15231B touch for swipes, and the soldered QMI8658 for the Level
    # page -- now the Feather's own spirit-level game (level.c), not the
    # sand or Pip, drawn in her own big rendering (levelbig.c) rather than
    # level.c's small-panel one. The GT911 is the CrowPanel's touch, not
    # hers. sdcard.c is hers alone too: she is the only board with a card
    # slot. camera.c is hers too: the OV5640 rides her DVP header, which no
    # other board has.
    set(BOARD_EXCLUDES "display_st7789.c" "display_jd9853.c" "display_rgb.c" "display_i80.c"
                       "particles.c" "shaketimer.c"
                       "pip.c" "gt911.c" "axs5106l_touch.c")
elseif(CONFIG_SCREEN_BOARD_AUDIO_S3)
    # speaker: a Waveshare ESP32-S3-AUDIO-Board with no screen at all. She runs
    # speaker_app.c from the top of app_main and never reaches the page UI, so
    # every panel driver goes and display_none.c stands in for display.h; the
    # rest of main.c's page machinery (pages, views, canvas, the payload
    # parser) still compiles so the shared main.c links, as it does on lilly.
    # No IMU, no touch panel (gt911.c stays as the harmless default, as on
    # lilly: main.c's loop names touch_*, and touch_init is never reached),
    # no PMIC, no camera. She does have a card slot (sdcard.c) and a PCF85063
    # (pcf85063.c, kept below).
    # Since 2026-09-26 a 2.8" ST7789T3 module sits on her LCD FPC, so she
    # has display_st7789.c and display_none.c goes.
    set(BOARD_EXCLUDES "display_none.c" "display_jd9853.c" "display_rgb.c" "display_i80.c" "display_axs15231b.c"
                       "axp2101.c" "qmi8658.c" "level.c" "levelbig.c" "particles.c" "shaketimer.c"
                       "pip.c" "axs_touch.c" "axs5106l_touch.c" "camera.c")
else()
    set(BOARD_EXCLUDES "display_st7789.c" "display_jd9853.c" "display_rgb.c" "display_axs15231b.c" "axp2101.c"
                       "qmi8658.c" "level.c" "levelbig.c" "particles.c" "shaketimer.c"
                       "pip.c" "axs_touch.c" "axs5106l_touch.c" "sdcard.c" "camera.c")
endif()

# The noise monitor -- the sound level meter (soundlevel.c), the ring's
# colours (ring.c), the app that drives the codecs, the LEDs and the card
# (speaker_app.c) and the stand-in for a display (display_none.c) -- is
# speaker's alone. soundlevel.c and ring.c are pure and host-tested; no other
# board compiles any of the four.
if(NOT CONFIG_SCREEN_BOARD_AUDIO_S3)
    list(APPEND BOARD_EXCLUDES "soundlevel.c" "speaker_app.c")
    # ring.c's colours are also watch's: her Sound page tints speaker's level
    # exactly as speaker's ring does.
    if(NOT CONFIG_SCREEN_BOARD_TOUCH_LCD_169)
        list(APPEND BOARD_EXCLUDES "ring.c")
    endif()
endif()
# watch's Sound and Days pages (noiseui.c, daysui.c); speaker's own screen
# shows the Sound page too.
if(NOT CONFIG_SCREEN_BOARD_TOUCH_LCD_169)
    if(NOT CONFIG_SCREEN_BOARD_AUDIO_S3)
        list(APPEND BOARD_EXCLUDES "noiseui.c" "daysui.c")
    endif()
endif()
# speaker's Live page (the waveform and the octave bars) is hers alone.
if(NOT CONFIG_SCREEN_BOARD_AUDIO_S3)
    list(APPEND BOARD_EXCLUDES "liveui.c")
endif()

# One touch driver per board -- both define the same touch_* interface, so
# exactly one is compiled. gt911 on the CrowPanel, axs_touch on envio, and on a
# board with no touch it does not matter which is left in; gt911 stays.

# An environment logger has no use for the sensor toys, and dropping them
# takes the IMU driver with them. See SCREEN_ENV_ONLY in Kconfig.projbuild.
if(CONFIG_SCREEN_ENV_ONLY)
    # envio is ENV_ONLY but keeps her soldered QMI8658 for the Level page; the
    # other sensor toys stay out.
    if(CONFIG_SCREEN_BOARD_TOUCH_LCD_35B)
        list(APPEND BOARD_EXCLUDES "particles.c" "shaketimer.c")
    else()
        list(APPEND BOARD_EXCLUDES "qmi8658.c" "level.c" "particles.c" "shaketimer.c")
    endif()
else()
    list(APPEND BOARD_EXCLUDES "envstore.c" "envpage.c" "envflash.c")
endif()

# maze.c/maze_levels.c are kept in the tree -- the bubble-level game replaced
# the maze in envio's page slot -- but no board compiles them now.
list(APPEND BOARD_EXCLUDES "maze.c" "maze_levels.c")

# bubblelevel.c was envio's own Level-page game; the Feather's spirit-level
# game (level.c) replaced it there, so bubblelevel.c is kept in the tree,
# unused, on every board.
list(APPEND BOARD_EXCLUDES "bubblelevel.c")

# The CST816D driver is for the Touch variant of watch's board; the unit here
# has no touch controller, so no board compiles it. Kept for a Touch unit.
list(APPEND BOARD_EXCLUDES "cst816d_touch.c")
if(NOT CONFIG_SCREEN_BOARD_TOUCH_LCD_169)
    # The analog face and what only it needs -- the moon's arithmetic -- and
    # watch's power/battery are hers alone. The pure ones build on the host
    # for the tests.
    list(APPEND BOARD_EXCLUDES "face.c" "moonphase.c" "watch.c")
    # The PCF85063 clock is watch's and speaker's: both have one soldered on
    # at 0x51, answering to the ds3231_* names.
    if(NOT CONFIG_SCREEN_BOARD_AUDIO_S3)
        list(APPEND BOARD_EXCLUDES "pcf85063.c")
    endif()
    # The stroke font is the face's alone: its numerals and sub-dial words.
    # envo's pages set their type in Inter now (aafont.c, below).
    list(APPEND BOARD_EXCLUDES "vfont.c")
    # The vector drawing is the face's, and envo's air pages draw their marks
    # with it too -- traces, arrows, page dots. Every other board leaves it out.
    if(NOT CONFIG_SCREEN_BOARD_TOUCH_LCD_147 AND NOT CONFIG_SCREEN_BOARD_AUDIO_S3)
        list(APPEND BOARD_EXCLUDES "vector.c")
    endif()
endif()

# envo's readable air -- the limits, state and trend (envstate.c), the pages
# drawn from them (envui.c) and the anti-aliased Inter they are set in
# (aafont.c, with its faces in font_inter_*.h) -- is hers alone. All three are
# pure and host-tested; no other board compiles them. The linker would drop
# the unused faces anyway, but left out here they cost the other boards not
# even the compile, and their images stay exactly what they were.
if(NOT CONFIG_SCREEN_BOARD_TOUCH_LCD_147)
    list(APPEND BOARD_EXCLUDES "envstate.c" "envui.c")
    # The Inter faces also set watch's Sound page.
    if(NOT CONFIG_SCREEN_BOARD_TOUCH_LCD_169 AND NOT CONFIG_SCREEN_BOARD_AUDIO_S3)
        list(APPEND BOARD_EXCLUDES "aafont.c")
    endif()
endif()

# watch's moon-page moon (NASA, public domain -- see assets/watch/CREDITS.md),
# raw RGB565 240x280, flash-mapped rather than copied into RAM. The Earth and
# Carina photos stay in assets/ but are no longer on the dial.
if(CONFIG_SCREEN_BOARD_TOUCH_LCD_169)
    set(BOARD_EMBEDS "../assets/watch/moon.bin")
else()
    set(BOARD_EMBEDS "")
endif()

idf_component_register(
    SRC_DIRS "."
    EXCLUDE_SRCS ${BOARD_EXCLUDES}
    INCLUDE_DIRS "."
    EMBED_FILES ${BOARD_EMBEDS}
    REQUIRES esp_lcd esp_lcd_jd9853 driver esp_driver_ledc esp_driver_i2c esp_driver_spi esp_driver_tsens esp_timer bt nvs_flash esp_event fatfs esp_driver_sdmmc sdmmc esp_driver_usb_serial_jtag esp_driver_gpio esp_adc esp_driver_i2s)


