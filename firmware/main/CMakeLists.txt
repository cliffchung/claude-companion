# companion/ is picked up by the glob below. cc_sessions.c and cc_cmd.c are pure C with no ESP-IDF headers
# (also built by test/host); cc_link.c and cc_console.c are the device side. voice/ (Plan 5) too: voice_rules.c is
# pure (host-tested), mic.c, voice.c and voice_console.c are the device side.
file(GLOB_RECURSE APP_SRCS
    "*.c")
list(FILTER APP_SRCS EXCLUDE REGEX ".*/fonts/font_.*_unused\\.c$")
idf_component_register(
    SRCS ${APP_SRCS}
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event lwip json mbedtls fatfs vfs esp_http_client
                  app_update esp_partition esp_timer esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_driver_jpeg esp_driver_ppa
                  esp_psram spi_flash esp_app_format sdmmc driver
                  libssh microlink console
    REQUIRES esp_lvgl_port lvgl m5stack_tab5 esp_lcd_touch esp_lcd esp_lcd_touch esp_lcd usb_host_hid esp_wifi_remote)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format -Wno-unused-function -Wno-missing-field-initializers -Wno-misleading-indentation -Wno-unused-variable -Wno-unused-but-set-variable -Wno-misleading-indentation -Wno-unused-variable -Wno-unused-but-set-variable)
