# Shared ESP32-S3 main-component registration.
#
# Included by both ports/esp32_s3/main/CMakeLists.txt (octal PSRAM) and
# ports/esp32_s3_quad/main/CMakeLists.txt (quad PSRAM). The two ports differ
# ONLY in the sdkconfig PSRAM line mode; the source list, includes, demos,
# build flags, and the runtime-detected PSRAM slab below are identical for
# both. The source lists live in sources_lists.cmake so non-S3 sibling ports
# can reuse them with per-chip adjustments.

include(${CMAKE_CURRENT_LIST_DIR}/sources_lists.cmake)
set(PORT_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

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
        # vga_lcd_rgb_320d.c is a fork of esp_lcd's RGB panel driver and
        # needs the component's private header (esp_lcd_common.h).
        $ENV{IDF_PATH}/components/esp_lcd/priv_include
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
        usb
        usb_host_hid
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
