# MMBasic Anywhere - classic ESP32 "CYD" port (ESP32-2432S028R class boards).
#
# Standard ESP-IDF project. Build it the documented way:
#   . ~/esp/esp-idf/export.sh
#   cd ports/esp32_cyd
#   idf.py set-target esp32
#   idf.py build
#
# Shares the ESP32 main-component source lists with ports/esp32_s3
# (see main/CMakeLists.txt); differs in the chip target, the UART0 console,
# the classic 0..39 GPIO map, and the absence of the S3-only USB and
# LCD_CAM VGA stacks.

cmake_minimum_required(VERSION 3.16)

set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mmbasic_anywhere_esp32_cyd)
