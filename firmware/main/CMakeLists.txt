if(CONFIG_APP_TEST_MODE)
    set(app_main_src "test_main.c")
else()
    set(app_main_src "app_main.c")
endif()

idf_component_register(
    SRCS "${app_main_src}"
    INCLUDE_DIRS "."
    REQUIRES board iox battery ht16d35x cr95hf encoder content audio admin nvs_flash
)
