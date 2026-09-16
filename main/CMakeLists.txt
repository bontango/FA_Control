idf_component_register(
    SRCS "repo.c" "board.c" "main.c" "app_config.c" "lisy.c" "fa_connect.c" "power_mgr.c" "wifi_mgr.c" "web_server.c" "fw_update.c" "names.c" "rom_boot.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_http_server esp_wifi nvs_flash esp_netif esp_timer esp_driver_uart esp_driver_gpio lwip
                  esp_https_ota esp_http_client app_update esp_app_format mbedtls esp_hw_support
                  esp_partition joltwallet__littlefs
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

# Logo der Kopfleiste. Als eigene Binaerdatei statt als data-URI in der Seite:
# ein PNG ist bereits komprimiert, base64 wuerde es um ein Drittel aufblaehen
# und liesse sich vom gzip der Seite nicht wieder einholen.
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/web/logo.png" BINARY)
