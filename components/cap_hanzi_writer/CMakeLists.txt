set(hanzi_sources)
if(CONFIG_HANZI_WRITER_ENABLE)
    set(hanzi_sources "src/cap_hanzi_writer.c" "src/hanzi_download.c"
                      "src/hanzi_data.c" "src/hanzi_player.c"
                      "src/hanzi_render.c" "src/hanzi_view.c")
endif()

# Declare dependencies during IDF's early expansion as well as the full pass.
idf_component_register(
    SRCS ${hanzi_sources}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES claw_cap
    PRIV_REQUIRES display_service esp_http_client esp-tls esp_timer mbedtls
                  freertos espressif__cjson lvgl__lvgl)

if(CONFIG_HANZI_WRITER_ENABLE AND NOT CMAKE_BUILD_EARLY_EXPANSION)
        # ThorVG checks size_t against UINT32_MAX for 64-bit hosts. On this
        # 32-bit target GCC reports those guards as always false. Keep the
        # warning visible without making vendor compression code fail -Werror.
        idf_component_get_property(hanzi_lvgl_dir lvgl__lvgl COMPONENT_DIR)
        set_property(SOURCE "${hanzi_lvgl_dir}/src/libs/thorvg/tvgCompressor.cpp"
            DIRECTORY "${hanzi_lvgl_dir}"
            APPEND PROPERTY COMPILE_OPTIONS "-Wno-error=type-limits")
endif()
