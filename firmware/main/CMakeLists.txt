idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES board iox battery ht16d35x cr95hf encoder content audio admin nvs_flash
)
