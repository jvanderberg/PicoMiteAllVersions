# Classic-ESP32 CYD main component.
#
# Shares the ESP32 main-component source lists with ports/esp32_s3
# (sources_lists.cmake) and swaps the chip-specific pieces: classic ESP32
# has no USB Serial/JTAG controller, no USB host, and no LCD_CAM
# peripheral, and its GPIO map is the classic 0..39 range. Everything else
# — MMBasic core, runtime glue, LittleFS/SD storage, Wi-Fi/network
# adapters, LEDC PWM, I2C, RMT WS2812, audio, and the shared SPI-LCD panel
# core — compiles unchanged for this chip.

include(${CMAKE_CURRENT_LIST_DIR}/../../esp32_s3/main/sources_lists.cmake)
set(PORT_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
set(CYD_MAIN_DIR ${CMAKE_CURRENT_LIST_DIR})

list(REMOVE_ITEM PORT_LOCAL_SRCS
    # USB Serial/JTAG console -> UART0 console.
    ${ESP32_S3_MAIN_DIR}/esp32_console.c
    # No native USB: no role switching, no host keyboard.
    ${ESP32_S3_MAIN_DIR}/esp32_usb_role.c
    ${ESP32_S3_MAIN_DIR}/esp32_usb_keyboard.c
    # No LCD_CAM peripheral: no VGA scanout (nor the esp_lcd RGB fork).
    ${ESP32_S3_MAIN_DIR}/esp32_vga.c
    ${REPO_ROOT}/drivers/vga_lcdcam_s3/vga_lcdcam_s3.c
    ${REPO_ROOT}/drivers/vga_lcdcam_s3/vga_lcd_rgb_320d.c
    # S3 GPIO map -> classic 0..39 map.
    ${ESP32_S3_MAIN_DIR}/esp32_pin_tables.c
    # S3 register-level I2C slave ISR -> unsupported on classic (the
    # register layout differs); the I2C master half is shared.
    ${ESP32_S3_MAIN_DIR}/hal_i2c_esp32_slave.c
    # I2S hardware-v2 DAC-style PDM slot -> the v1 single slot shape.
    ${ESP32_S3_MAIN_DIR}/esp32_audio_pdm_slot_s3.c
    # S3/default internal-DAC stub -> CYD onboard GPIO26 DAC path.
    ${ESP32_S3_MAIN_DIR}/esp32_audio_internal_dac_stub.c
    # Hand-rolled SPI-LCD backend -> ESP-IDF esp_lcd (esp32_cyd_esp_lcd.c).
    ${ESP32_S3_MAIN_DIR}/esp32_ili9341_lcd.c
)

# The CYD drives its panel through ESP-IDF's esp_lcd, not the shared Pico
# SPI-LCD controller core, so drop those (and their readback) from this port.
list(REMOVE_ITEM DRIVER_STUBS
    ${REPO_ROOT}/drivers/spi_lcd/spi_lcd_panels.c
    ${REPO_ROOT}/drivers/spi_lcd/spi_lcd_readback.c
)

list(APPEND PORT_LOCAL_SRCS
    ${CYD_MAIN_DIR}/esp32_cyd_esp_lcd.c
    ${CYD_MAIN_DIR}/esp32_cyd_console.c
    ${CYD_MAIN_DIR}/esp32_cyd_usb_stub.c
    ${CYD_MAIN_DIR}/esp32_cyd_vga.c
    ${CYD_MAIN_DIR}/esp32_cyd_vga_text.c
    ${CYD_MAIN_DIR}/esp32_cyd_vga_mode1.c
    ${REPO_ROOT}/drivers/vga_i2s_esp32/vga_i2s_esp32.c
    ${CYD_MAIN_DIR}/esp32_cyd_pin_tables.c
    ${CYD_MAIN_DIR}/esp32_cyd_i2c_slave_stub.c
    ${CYD_MAIN_DIR}/esp32_cyd_audio_dac.c
    ${CYD_MAIN_DIR}/esp32_cyd_audio_pdm_slot.c
)

idf_component_register(
    SRCS
        ${PORT_LOCAL_SRCS}
        ${CORE_SRCS}
        ${STATE_SRCS}
        ${RUNTIME_SRCS}
        ${DEVICE_FACING_SRCS}
        ${DRIVER_STUBS}
        ${BC_SRCS}
    EMBED_TXTFILES
        ${EMBED_DEMOS}
    INCLUDE_DIRS
        ${CYD_MAIN_DIR}
        ${ESP32_S3_MAIN_DIR}
        ${PORT_DIR}
        ${REPO_ROOT}/core/mmbasic
        ${REPO_ROOT}/runtime/vm
        ${REPO_ROOT}/shared/gfx
        ${REPO_ROOT}/shared/peripheral
        ${REPO_ROOT}/shared/audio
        ${REPO_ROOT}/shared/mmbasic
        ${REPO_ROOT}/drivers/bmp_decoder
        ${REPO_ROOT}/drivers/gps
        ${REPO_ROOT}/drivers/gui_controls
        ${REPO_ROOT}/drivers/gui_touch
        ${REPO_ROOT}/drivers/i2c_bus
        ${REPO_ROOT}/drivers/es8311
        ${REPO_ROOT}/drivers/onewire
        ${REPO_ROOT}/drivers/psram_heap
        ${REPO_ROOT}/drivers/serial
        ${REPO_ROOT}/drivers/spi_bus
        ${REPO_ROOT}/drivers/spi_lcd
        ${REPO_ROOT}/drivers/ssd1963
        ${REPO_ROOT}/drivers/vga_lcdcam_s3
        ${REPO_ROOT}/drivers/vga_i2s_esp32
        ${REPO_ROOT}
        ${REPO_ROOT}/third_party/aes
        ${REPO_ROOT}/third_party/cjson
        ${REPO_ROOT}/third_party/dr_libs
        ${REPO_ROOT}/third_party/fatfs
        ${REPO_ROOT}/third_party/hxcmod
        ${REPO_ROOT}/third_party/littlefs
        ${REPO_ROOT}/third_party/picojpeg
        ${REPO_ROOT}/third_party/regex
        ${REPO_ROOT}/third_party/upng
    REQUIRES
        driver
        esp_lcd
        esp_adc
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_rmt
        esp_driver_sdspi
        esp_timer
        esp_psram
        esp_mm
        esp_event
        esp_netif
        esp_wifi
        mqtt
        esp-tls
        mbedtls
        nvs_flash
        spi_flash
        esp_partition
        sdmmc
        vfs
)

target_include_directories(${COMPONENT_LIB} BEFORE PRIVATE
    ${IDF_PATH}/components/lwip/port/include
)

# Force-include the platform header for every C TU in this component.
# Generator-expression-restricted to C so the auto-generated .S assembly
# wrappers IDF emits for EMBED_TXTFILES don't try to assemble it.
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-include esp32_platform.h>
    -funsigned-char
    # Preserve legacy tentative-definition merging without allowing
    # duplicate strong functions through the linker.
    -fcommon
    -Wno-error
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wno-misleading-indentation
    -Wno-missing-braces
    -Wno-format-truncation
    -Wno-maybe-uninitialized
)

if(DEFINED ENV{MMBA_RELEASE_VERSION} AND NOT "$ENV{MMBA_RELEASE_VERSION}" STREQUAL "")
    set(MMBA_RELEASE_VERSION "$ENV{MMBA_RELEASE_VERSION}")
else()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${REPO_ROOT}/VERSION")
    file(STRINGS "${REPO_ROOT}/VERSION" MMBA_RELEASE_VERSION LIMIT_COUNT 1)
endif()
if(MMBA_RELEASE_VERSION STREQUAL "")
    message(FATAL_ERROR "MMBA_RELEASE_VERSION is empty; set it in the environment or update VERSION")
endif()

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    MMBASIC_ESP32
    MMBA_RELEASE_VERSION="${MMBA_RELEASE_VERSION}"
    FF_MAX_LFN_LARGE
    BC_SIM_RP2040
)

# No --wrap and no --allow-multiple-definition here. Per-port hooks must
# have exactly one strong definition in the ESP32 source list; duplicate
# functions should fail at link time.
