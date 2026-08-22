idf_component_register(
    SRCS "audio.c"
    INCLUDE_DIRS "."
    REQUIRES aw88194 board codec_es8156 esp_audio_codec yoto_vfs driver
)
