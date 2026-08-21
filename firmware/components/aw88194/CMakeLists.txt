idf_component_register(
    SRCS "aw88194.c"
    INCLUDE_DIRS "."
    REQUIRES board iox driver
    EMBED_FILES
        "factory_aw88194_fw.bin"
        "factory_aw88194_cfg_rev04.bin"
)
