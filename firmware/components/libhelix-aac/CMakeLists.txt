idf_component_register(
    SRCS
        "aacdec.c"
        "aactabs.c"
        "bitstream.c"
        "buffers.c"
        "dct4.c"
        "decelmnt.c"
        "dequant.c"
        "fft.c"
        "filefmt.c"
        "huffmanaac.c"
        "hufftabs.c"
        "imdct.c"
        "noiseless.c"
        "pns.c"
        "stproc.c"
        "tns.c"
        "trigtabs.c"
    INCLUDE_DIRS "."
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE USE_DEFAULT_STDLIB)
