# GC9306 TFT driver. Rev #04-only hardware, but registered on both revisions
# (IOX_TFT_* pins are defined unconditionally in board_pins.h); the linker
# drops the object on rev #05 where nothing references it.
idf_component_register(
    SRCS "gc9306.c"
    INCLUDE_DIRS "."
    REQUIRES board iox driver
)
