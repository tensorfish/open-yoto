idf_component_register(
    SRCS "gc9306.c"
    INCLUDE_DIRS "."
    REQUIRES board iox esp_driver_spi
)
