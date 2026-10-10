idf_component_register(
    SRCS "panel1.c" "net.c" "config.c" "console.c" "lcd.c" "touch.c" "rooms.c" "roomsui.c" "fonts.c"
         "../../main/aafont.c" "../../main/vector.c" "../../main/canvas.c" "../../main/textwrap.c"
    INCLUDE_DIRS "." "../../main" "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES esp_wifi esp_netif esp_event esp_http_client esp-tls mbedtls nvs_flash json esp_timer
             esp_lcd esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_uart)

# The WiFi and the relay's URL come from secrets/panel1.env (gitignored) into
# a header in the BUILD directory, never the source tree: this repo is public.
# They only seed panel1's settings at its first boot; after that they live in
# its NVS and change over the console (console.h), with no rebuild.
set(P1_SECRETS "${CMAKE_CURRENT_LIST_DIR}/../../secrets/panel1.env")
set(P1_NETS "")
set(P1_URL "")
if(EXISTS "${P1_SECRETS}")
    file(STRINGS "${P1_SECRETS}" _lines)
    # WIFI_SSID/WIFI_PASS, then WIFI2_SSID/WIFI2_PASS ... WIFI6_*: every network
    # a freshly erased panel1 should know.
    foreach(_n "" 2 3 4 5 6)
        set(_s "")
        set(_p "")
        foreach(_l IN LISTS _lines)
            if(_l MATCHES "^WIFI${_n}_SSID=(.*)$")
                set(_s "${CMAKE_MATCH_1}")
            elseif(_l MATCHES "^WIFI${_n}_PASS=(.*)$")
                set(_p "${CMAKE_MATCH_1}")
            endif()
        endforeach()
        if(NOT _s STREQUAL "")
            foreach(_v _s _p)
                string(REPLACE "\\" "\\\\" ${_v} "${${_v}}")
                string(REPLACE "\"" "\\\"" ${_v} "${${_v}}")
            endforeach()
            string(APPEND P1_NETS "{\"${_s}\", \"${_p}\"}, ")
        endif()
    endforeach()
    foreach(_l IN LISTS _lines)
        if(_l MATCHES "^RELAY_URL=(.*)$")
            set(P1_URL "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${P1_SECRETS}")
endif()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/panel1_secrets.h" CONTENT
"#define P1_SEED_NETS { ${P1_NETS}{ 0 } }\n#define P1_RELAY_URL \"${P1_URL}\"\n")
