# ESP32-S3 quad-PSRAM main component.
#
# Identical to the octal port: it shares the entire component definition
# (sources, includes, demos, flags) via sources.cmake, and the PSRAM heap
# slab is sized at boot from the chip actually detected (hal_psram_esp32.c),
# so there is nothing PSRAM-size-specific to set here. The ONLY thing that
# distinguishes this port from ports/esp32_s3 is the PSRAM line mode in
# sdkconfig.psram.quad (CONFIG_SPIRAM_MODE_QUAD).
include(${CMAKE_CURRENT_LIST_DIR}/../../esp32_s3/main/sources.cmake)
