# MMBasic Anywhere - ESP32-S3 port (octal PSRAM, R8 modules).
#
# Standard ESP-IDF project. Build it the documented way:
#   . ~/esp/esp-idf/export.sh
#   cd ports/esp32_s3
#   idf.py set-target esp32s3
#   idf.py build
#
# The quad-PSRAM sibling lives in ports/esp32_s3_quad and shares this port's
# main component (see main/sources.cmake).

cmake_minimum_required(VERSION 3.16)

# Shared base config + this port's PSRAM line-mode fragment. The default
# HAL_PORT_PSRAM_SLAB_BYTES (6 MB, octal) is used — no -D override needed.
set(SDKCONFIG_DEFAULTS "sdkconfig.common;sdkconfig.psram.octal")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mmbasic_anywhere_esp32_s3)
