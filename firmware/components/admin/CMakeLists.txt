idf_component_register(SRCS "admin.c"
                       INCLUDE_DIRS "."
                       EMBED_FILES "html/index.html"
                       REQUIRES board content esp_wifi esp_http_server esp_http_client esp_netif esp_event json mbedtls)
