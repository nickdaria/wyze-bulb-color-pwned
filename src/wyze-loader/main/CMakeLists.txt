idf_component_register(SRCS "loader_main.c" "bulb_write.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "${CMAKE_CURRENT_SOURCE_DIR}/index.html"
                       REQUIRES esp_wifi esp_http_server app_update nvs_flash esp_netif esp_event spi_flash driver)
