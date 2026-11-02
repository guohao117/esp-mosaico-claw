set(hanzi_sources)
if(CONFIG_HANZI_WRITER_ENABLE)
    set(hanzi_sources "src/cap_hanzi_writer.c" "src/hanzi_download.c"
                      "src/hanzi_data.c" "src/hanzi_player.c"
                      "src/hanzi_path.c" "src/hanzi_render.c" "src/hanzi_view.c")
endif()

# Declare dependencies during IDF's early expansion as well as the full pass.
idf_component_register(
    SRCS ${hanzi_sources}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES claw_cap
    PRIV_REQUIRES display_service esp_http_client esp-tls esp_timer mbedtls
                  freertos espressif__cjson lvgl__lvgl)
