if(CONFIG_APP_SCAN_MODE)
    set(app_main_src "scan_main.c")
elseif(CONFIG_APP_TEST_MODE)
    set(app_main_src "test_main.c")
else()
    set(app_main_src "app_main.c")
endif()

idf_component_register(
    SRCS "${app_main_src}"
    INCLUDE_DIRS "."
    REQUIRES board iox battery lis2dh12 display ht16d35x gc9306 cr95hf encoder content audio bluetooth admin yoto_vfs nvs_flash esp_driver_gpio esp_driver_ledc
)
