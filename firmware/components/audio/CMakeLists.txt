idf_component_register(
    SRCS "audio.c"
    INCLUDE_DIRS "."
    REQUIRES aw88194 board codec_es8156 libhelix-mp3 libhelix-aac yoto_vfs driver
)
