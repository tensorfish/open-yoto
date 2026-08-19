idf_component_register(SRCS "audio.c"
                       INCLUDE_DIRS "."
                       REQUIRES board iox codec_es8156 libhelix-mp3 driver)
