idf_component_register(SRCS "ap_main.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../payload.bin"
                       REQUIRES esp_wifi esp_netif esp_event nvs_flash)
