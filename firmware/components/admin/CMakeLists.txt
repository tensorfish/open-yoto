idf_component_register(SRCS "admin.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "html/upload.html"
                       REQUIRES board content fatfs esp_wifi esp_http_server esp_http_client esp_netif esp_event esp_timer json mbedtls heap)
