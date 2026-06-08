# ESP32-S3 (octal PSRAM) main component.
#
# The full component definition — source list, includes, demos, build flags —
# lives in sources.cmake, shared verbatim with ports/esp32_s3_quad. The PSRAM
# heap slab is sized at boot from the detected chip (hal_psram_esp32.c).
include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)
