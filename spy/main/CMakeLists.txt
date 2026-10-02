idf_component_register(
    SRCS "spy.c" "mp4mux.c" "net.c" "tg.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES esp_driver_gpio esp_driver_uart esp_driver_i2c esp_driver_sdmmc fatfs sdmmc
             esp_netif esp_event esp_http_client esp-tls mbedtls nvs_flash json esp_timer)

# The bot token and chat id come from secrets/spy-telegram.env (gitignored)
# into a header in the BUILD directory, never the source tree: this repo is
# public. A build without the file still works and simply has no Telegram.
set(SPY_SECRETS "${CMAKE_CURRENT_LIST_DIR}/../../secrets/spy-telegram.env")
set(SPY_TOKEN "")
set(SPY_CHAT "")
if(EXISTS "${SPY_SECRETS}")
    file(STRINGS "${SPY_SECRETS}" _lines)
    foreach(_l IN LISTS _lines)
        if(_l MATCHES "^TELEGRAM_BOT_TOKEN=(.*)$")
            set(SPY_TOKEN "${CMAKE_MATCH_1}")
        elseif(_l MATCHES "^TELEGRAM_CHAT_ID=(.*)$")
            set(SPY_CHAT "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SPY_SECRETS}")
endif()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/spy_secrets.h" CONTENT
"#define SPY_TG_TOKEN \"${SPY_TOKEN}\"\n#define SPY_TG_CHAT \"${SPY_CHAT}\"\n")
