idf_component_register(
    SRCS "stanclaw.c" "lcd.c" "touch.c" "net.c" "config.c" "console.c" "cmdline.c"
         "audio.c" "vad.c" "https.c" "stt.c" "tts.c" "agent.c" "claude_msg.c" "speechfmt.c"
         "mcpd.c" "mcp_rpc.c" "ui.c" "fonts.c"
         "../../main/aafont.c" "../../main/vector.c" "../../main/canvas.c" "../../main/textwrap.c"
    INCLUDE_DIRS "." "../../main" "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES esp_wifi esp_netif esp_event esp_http_client esp_http_server esp-tls mbedtls nvs_flash json
             esp_timer esp_lcd esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_ledc esp_driver_uart mdns)

# Keys and WiFi come from gitignored secrets into a header in the BUILD
# directory, never the source tree: this repo is public. They only seed the
# first boot; after that NVS holds them and the console changes them.
set(SC_SECRETS "${CMAKE_CURRENT_LIST_DIR}/../../secrets/stan-claw.env")
set(SC_WIFI_FILE "${SC_SECRETS}")
set(SC_LINES "")
if(EXISTS "${SC_SECRETS}")
    file(STRINGS "${SC_SECRETS}" SC_LINES)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SC_SECRETS}")
endif()
set(_wifi_lines ${SC_LINES})
if(NOT "${SC_LINES}" MATCHES "WIFI_SSID=")
    set(_p1 "${CMAKE_CURRENT_LIST_DIR}/../../secrets/panel1.env")
    if(EXISTS "${_p1}")
        file(STRINGS "${_p1}" _wifi_lines)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_p1}")
    endif()
endif()
function(sc_escape var)
    string(REPLACE "\\" "\\\\" _v "${${var}}")
    string(REPLACE "\"" "\\\"" _v "${_v}")
    set(${var} "${_v}" PARENT_SCOPE)
endfunction()
set(SC_NETS "")
foreach(_n "" 2 3 4 5 6)
    set(_s "")
    set(_p "")
    foreach(_l IN LISTS _wifi_lines)
        if(_l MATCHES "^WIFI${_n}_SSID=(.*)$")
            set(_s "${CMAKE_MATCH_1}")
        elseif(_l MATCHES "^WIFI${_n}_PASS=(.*)$")
            set(_p "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(NOT _s STREQUAL "")
        sc_escape(_s)
        sc_escape(_p)
        string(APPEND SC_NETS "{\"${_s}\", \"${_p}\"}, ")
    endif()
endforeach()
set(SC_VALS "")
foreach(_k CLAUDE_KEY DEEPGRAM_KEY ELEVENLABS_KEY ELEVENLABS_VOICE MODEL MCP_URL MCP_TOKEN SERVE_TOKEN)
    set(_v "")
    foreach(_l IN LISTS SC_LINES)
        if(_l MATCHES "^${_k}=(.*)$")
            set(_v "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    sc_escape(_v)
    string(APPEND SC_VALS "\"${_v}\", ")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/stanclaw_secrets.h" CONTENT
"#define SC_SEED_NETS { ${SC_NETS}{ 0 } }\n#define SC_SEED_VALS { ${SC_VALS} }\n")
