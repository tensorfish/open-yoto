idf_component_register(
    SRCS "board_i2c.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_uart
        esp_driver_sdmmc
        esp_driver_spi
)
