# MMBasic Anywhere - ESP32-S3 port (quad PSRAM, R2 modules; also no-PSRAM).
#
# Standard ESP-IDF project, built the documented way:
#   . ~/esp/esp-idf/export.sh
#   cd ports/esp32_s3_quad
#   idf.py set-target esp32s3
#   idf.py build
#
# Shares the entire main component with ports/esp32_s3 via
# ../esp32_s3/main/sources.cmake; differs only in the PSRAM line mode
# (sdkconfig.psram.quad) and the heap slab size (-D in main/CMakeLists.txt).

cmake_minimum_required(VERSION 3.16)

# Shared base config + this port's quad PSRAM fragment.
set(SDKCONFIG_DEFAULTS "../esp32_s3/sdkconfig.common;sdkconfig.psram.quad")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mmbasic_anywhere_esp32_s3_quad)
