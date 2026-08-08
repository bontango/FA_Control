idf_component_register(
    SRCS "board.c" "main.c" "app_config.c" "lisy.c" "fa_connect.c" "power_mgr.c" "wifi_mgr.c" "web_server.c" "fw_update.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_http_server esp_wifi nvs_flash esp_netif esp_timer esp_driver_uart esp_driver_gpio lwip
                  esp_https_ota esp_http_client app_update esp_app_format mbedtls esp_hw_support
)

# Gzip the web page at build time and embed it into the binary
idf_build_get_property(python PYTHON)
set(WEB_SRC ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html)
set(WEB_GZ ${CMAKE_CURRENT_BINARY_DIR}/index.html.gz)

add_custom_command(
    OUTPUT ${WEB_GZ}
    COMMAND ${python} ${CMAKE_CURRENT_SOURCE_DIR}/web/gzip_file.py ${WEB_SRC} ${WEB_GZ}
    DEPENDS ${WEB_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/web/gzip_file.py
    VERBATIM)

add_custom_target(web_gz_target DEPENDS ${WEB_GZ})
add_dependencies(${COMPONENT_LIB} web_gz_target)
target_add_binary_data(${COMPONENT_LIB} ${WEB_GZ} BINARY DEPENDS web_gz_target)
