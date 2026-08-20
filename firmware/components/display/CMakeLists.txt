idf_component_register(
    SRCS "display.c"
    INCLUDE_DIRS "."
    REQUIRES board ht16d35x gc9306 esp_driver_ledc
)
