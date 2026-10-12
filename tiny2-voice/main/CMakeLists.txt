idf_component_register(
    SRCS "voice.c" "amp.c" "speak.c" "settings.c"
         "../../stan-claw/main/https.c" "../../stan-claw/main/speechfmt.c"
    INCLUDE_DIRS "." "../../stan-claw/main" "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES esp_wifi esp_netif esp_event esp_http_client esp_http_server esp-tls mbedtls nvs_flash json
             esp_driver_ledc esp_driver_gptimer esp_driver_gpio esp_driver_uart esp_timer mdns)

# The WiFi networks, the ElevenLabs key and voice, and the token the /say
# endpoint wants, from gitignored secrets into a header in the BUILD
# directory (this repo is public). They seed the first boot only; NVS keeps
# them after that.
set(_lines "")
foreach(_f stan-claw.env panel1.env tiny2.env)
    set(_p "${CMAKE_CURRENT_LIST_DIR}/../../secrets/${_f}")
    if(EXISTS "${_p}")
        file(STRINGS "${_p}" _l)
        list(APPEND _lines ${_l})
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_p}")
    endif()
endforeach()
function(t2_escape var)
    string(REPLACE "\\" "\\\\" _v "${${var}}")
    string(REPLACE "\"" "\\\"" _v "${_v}")
    set(${var} "${_v}" PARENT_SCOPE)
endfunction()
set(T2_NETS "")
foreach(_n "" 2 3 4 5 6)
    set(_s "")
    set(_pw "")
    foreach(_l IN LISTS _lines)
        if(_l MATCHES "^WIFI${_n}_SSID=(.*)$")
            set(_s "${CMAKE_MATCH_1}")
        elseif(_l MATCHES "^WIFI${_n}_PASS=(.*)$")
            set(_pw "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(NOT _s STREQUAL "")
        t2_escape(_s)
        t2_escape(_pw)
        string(APPEND T2_NETS "{\"${_s}\", \"${_pw}\"}, ")
    endif()
endforeach()
set(T2_VALS "")
foreach(_k ELEVENLABS_KEY ELEVENLABS_VOICE TINY2_TOKEN)
    set(_v "")
    foreach(_l IN LISTS _lines)
        if(_l MATCHES "^${_k}=(.*)$")
            set(_v "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    t2_escape(_v)
    string(APPEND T2_VALS "\"${_v}\", ")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/tiny2_secrets.h.tmp"
     "/* Generated from secrets/ -- never commit. */\n"
     "#define T2_SEED_NETS { ${T2_NETS} }\n#define T2_SEED_VALS { ${T2_VALS} }\n")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/tiny2_secrets.h.tmp" "${CMAKE_CURRENT_BINARY_DIR}/tiny2_secrets.h" COPYONLY)
