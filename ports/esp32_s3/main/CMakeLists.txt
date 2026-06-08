# ESP32-S3 (octal PSRAM) main component.
#
# The full component definition — source list, includes, demos, build flags —
# lives in sources.cmake, shared verbatim with ports/esp32_s3_quad. This port
# uses the default HAL_PORT_PSRAM_SLAB_BYTES (6 MB, octal R8 module).
include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)
