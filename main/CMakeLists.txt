set(WEB_SRC    "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html")
set(WEB_GZ     "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
set(WASM_SRC   "${CMAKE_CURRENT_SOURCE_DIR}/web/dither.wasm")
set(WASM_GZ    "${CMAKE_CURRENT_BINARY_DIR}/dither.wasm.gz")

idf_component_register(
    SRCS
        "main.c"
        "epd_4in0e.c"
        "sd_storage.c"
        "image_loader.c"
        "color_pipeline.c"
        "dither.c"
        "bmp_decoder.c"
        "jpeg_decoder.c"
        "palette.c"
        "resize.c"
        "photo_store.c"
        "config_store.c"
        "wifi_ap.c"
        "dns_hijack.c"
        "http_server.c"
        "loop_display.c"
        "pm1.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_spi
        esp_driver_sdspi
        esp_driver_gpio
        sdmmc
        fatfs
        spiffs
        vfs
        esp_timer
        esp_rom
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        lwip
        nvs_flash
    EMBED_FILES "${WEB_GZ}" "${WASM_GZ}"
)

# Build-time: gzip the web bundle so the firmware ships the compressed blob
# and the HTTP handler emits it verbatim with Content-Encoding: gzip.
# Must come AFTER idf_component_register() — the component-scan phase is
# script mode and forbids add_custom_command.
add_custom_command(
    OUTPUT  "${WEB_GZ}"
    COMMAND ${CMAKE_COMMAND} -E remove -f "${WEB_GZ}"
    COMMAND gzip -9 -c "${WEB_SRC}" > "${WEB_GZ}"
    DEPENDS "${WEB_SRC}"
    COMMENT "gzipping web UI"
    VERBATIM
)
add_custom_command(
    OUTPUT  "${WASM_GZ}"
    COMMAND ${CMAKE_COMMAND} -E remove -f "${WASM_GZ}"
    COMMAND gzip -9 -c "${WASM_SRC}" > "${WASM_GZ}"
    DEPENDS "${WASM_SRC}"
    COMMENT "gzipping dither.wasm"
    VERBATIM
)
add_custom_target(web_gz DEPENDS "${WEB_GZ}" "${WASM_GZ}")
add_dependencies(${COMPONENT_LIB} web_gz)
