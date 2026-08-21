idf_component_register(
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_uart
        esp_driver_sdmmc
        esp_driver_spi
)
