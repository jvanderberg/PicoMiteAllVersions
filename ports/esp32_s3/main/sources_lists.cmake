# Shared ESP32 main-component source lists.
#
# Defines the source/demo lists and the _esp32_prefix_relative() helper, with
# every bare (port-local) entry resolved against ports/esp32_s3/main. Included
# by ports/esp32_s3/main/sources.cmake (which registers the component for the
# S3 ports) and by sibling-port component files (ports/esp32_cyd) that adjust
# the lists for their chip before registering their own component.
#
set(REPO_ROOT          ${CMAKE_CURRENT_LIST_DIR}/../../..)
set(ESP32_S3_MAIN_DIR  ${CMAKE_CURRENT_LIST_DIR})

# Prefix any non-absolute entry with `dir`; pass absolute paths through. Lets
# the bare port-local filenames below resolve to esp32_s3/main even when the
# component being registered lives in a sibling port directory.
function(_esp32_prefix_relative out_var dir)
    set(_result "")
    foreach(_item IN LISTS ARGN)
        if(IS_ABSOLUTE "${_item}")
            list(APPEND _result "${_item}")
        else()
            list(APPEND _result "${dir}/${_item}")
        endif()
    endforeach()
    set(${out_var} "${_result}" PARENT_SCOPE)
endfunction()

set(CORE_SRCS
    ${REPO_ROOT}/core/mmbasic/MMBasic.c
    ${REPO_ROOT}/core/mmbasic/Commands.c
    ${REPO_ROOT}/core/mmbasic/Functions.c
    ${REPO_ROOT}/core/mmbasic/Operators.c
    ${REPO_ROOT}/core/mmbasic/MATHS.c
    ${REPO_ROOT}/core/mmbasic/Memory.c
    ${REPO_ROOT}/core/mmbasic/MMBasic_Print.c
    ${REPO_ROOT}/core/mmbasic/MMBasic_REPL.c
    ${REPO_ROOT}/core/mmbasic/MMBasic_Prompt.c
    ${REPO_ROOT}/core/mmbasic/XModem.c
    ${REPO_ROOT}/core/mmbasic/Editor.c
    ${REPO_ROOT}/shared/gfx/gfx_box_shared.c
    ${REPO_ROOT}/shared/gfx/gfx_circle_shared.c
    ${REPO_ROOT}/shared/gfx/gfx_line_shared.c
    ${REPO_ROOT}/shared/gfx/gfx_pixel_shared.c
    ${REPO_ROOT}/shared/gfx/gfx_cls_shared.c
    ${REPO_ROOT}/shared/gfx/gfx_text_shared.c
    ${REPO_ROOT}/shared/gfx/gfx_console_shared.c
    ${REPO_ROOT}/shared/mmbasic/mm_misc_shared.c
    ${REPO_ROOT}/drivers/fs_lfs_fatfs/fs_lfs_fatfs_helpers.c
    ${REPO_ROOT}/core/mmbasic/OptionCommands.c
    ${REPO_ROOT}/shared/cmd_ws2812_shared.c
    ${REPO_ROOT}/shared/peripheral/pwm_cmd.c
    ${REPO_ROOT}/shared/peripheral/i2c_config.c
    ${REPO_ROOT}/shared/peripheral/uart_config.c
    ${REPO_ROOT}/shared/peripheral/spi_config.c
    ${REPO_ROOT}/shared/cmd_psram.c
    ${REPO_ROOT}/shared/audio/synth_pcm.c
)

set(AUDIO_SHARED_CORE_SRCS
    ${REPO_ROOT}/shared/audio/Audio.c
    ${REPO_ROOT}/shared/audio/audio_runtime.c
    ${REPO_ROOT}/shared/audio/audio_stream.c
)

set(AUDIO_BACKEND_SRCS
    hal_audio_esp32.c                       # I2S/PDM command/control + PCM sink backend
    esp32_audio_pdm_slot_s3.c               # I2S hardware-v2 (DAC-style) PDM TX slot shape
)

set(STATE_SRCS
    ${REPO_ROOT}/core/state/display_state.c
    ${REPO_ROOT}/core/state/pin_state.c
    ${REPO_ROOT}/core/state/option_state.c
    ${REPO_ROOT}/core/state/audio_state.c
    ${REPO_ROOT}/shared/net/mm_net_interrupts.c
    ${REPO_ROOT}/shared/net/mm_net_http.c
    ${REPO_ROOT}/shared/net/mm_net_http_file.c
    ${REPO_ROOT}/shared/net/mm_net_http_page.c
    ${REPO_ROOT}/shared/net/mm_net_mqtt_cmd.c
    ${REPO_ROOT}/shared/net/mm_net_mqtt_hal_cmd.c
    ${REPO_ROOT}/shared/net/mm_net_ntp.c
    ${REPO_ROOT}/shared/net/mm_net_telnet_rx.c
    ${REPO_ROOT}/shared/net/mm_net_ntp_hal.c
    ${REPO_ROOT}/shared/net/mm_net_lifecycle.c
    ${REPO_ROOT}/shared/net/mm_net_options.c
    ${REPO_ROOT}/shared/net/mm_net_service.c
    ${REPO_ROOT}/shared/net/mm_net_state.c
    ${REPO_ROOT}/shared/net/mm_net_tcp_client_cmd.c
    ${REPO_ROOT}/shared/net/mm_net_tcp_server_cmd.c
    ${REPO_ROOT}/shared/net/mm_net_tftp.c
    ${REPO_ROOT}/shared/net/mm_net_transmit_cmd.c
    ${REPO_ROOT}/shared/net/mm_net_udp_cmd.c
    ${REPO_ROOT}/shared/net/mm_net_web_cmd.c
    ${REPO_ROOT}/shared/net/mm_net_websocket.c
    ${REPO_ROOT}/shared/net/mm_net_wifi_cmd.c
    ${REPO_ROOT}/shared/net/mm_net_json_fun.c
    ${REPO_ROOT}/third_party/cjson/cJSON.c
    ${REPO_ROOT}/drivers/web_console/web_console_assets.c
    ${REPO_ROOT}/drivers/web_console/web_console_display.c
    ${REPO_ROOT}/drivers/web_console/web_console_input.c
    ${REPO_ROOT}/drivers/web_console/web_console_protocol.c
)

set(RUNTIME_SRCS
    ${REPO_ROOT}/runtime/runtime_boot.c
    ${REPO_ROOT}/runtime/runtime_program.c
    ${REPO_ROOT}/runtime/runtime_abort.c
    ${REPO_ROOT}/runtime/runtime_interrupt.c
)

set(DEVICE_FACING_SRCS
    ${REPO_ROOT}/core/mmbasic/Draw.c
    ${REPO_ROOT}/shared/gfx/RGB121.c
    ${REPO_ROOT}/shared/gfx/Tilemap.c
    ${REPO_ROOT}/core/mmbasic/FileIO.c
    ${AUDIO_SHARED_CORE_SRCS}
    ${REPO_ROOT}/drivers/bmp_decoder/BmpDecoder.c
    ${REPO_ROOT}/drivers/i2c_bus/I2C.c
    ${REPO_ROOT}/third_party/regex/re.c
    ${REPO_ROOT}/third_party/picojpeg/picojpeg.c
)

set(DRIVER_STUBS
    ${REPO_ROOT}/drivers/audio_mp3/audio_mp3_real.c
    ${REPO_ROOT}/third_party/hxcmod/hxcmod.c
    ${REPO_ROOT}/drivers/spi_lcd/spi_lcd_panels.c
    ${REPO_ROOT}/drivers/spi_lcd/spi_lcd_mem332_stub.c
    ${REPO_ROOT}/drivers/display_merge/display_merge_stub.c
    ${REPO_ROOT}/drivers/vga_pio/vga_ops_stub.c
    ${REPO_ROOT}/drivers/gfx_3d/gfx_3d.c
    ${REPO_ROOT}/drivers/upng_sprite/upng_sprite_stub.c
    ${REPO_ROOT}/drivers/gui_touch/gui_touch_stub.c
    ${REPO_ROOT}/drivers/gui_controls/gui_controls_real.c
    ${REPO_ROOT}/drivers/gui_controls/GUI.c
    ${REPO_ROOT}/drivers/psram_heap/psram_heap_real.c
    ${REPO_ROOT}/drivers/heartbeat/heartbeat_stub.c
    ${REPO_ROOT}/drivers/editor_console/editor_console_stub.c
    ${REPO_ROOT}/drivers/display_pixel_readbuffer/display_pixel_readbuffer.c
)

set(BC_SRCS
    ${REPO_ROOT}/third_party/littlefs/lfs.c # vendored LittleFS used by A: drive
    ${REPO_ROOT}/third_party/littlefs/lfs_util.c
    ${REPO_ROOT}/runtime/vm/bc_bridge.c
    ${REPO_ROOT}/runtime/vm/bc_source.c
    ${REPO_ROOT}/runtime/vm/bc_compiler_core.c
    ${REPO_ROOT}/runtime/vm/bc_vm.c
    ${REPO_ROOT}/runtime/vm/bc_runtime.c
    ${REPO_ROOT}/runtime/vm/bc_debug.c
    ${REPO_ROOT}/third_party/fatfs/ff.c
    ${REPO_ROOT}/third_party/fatfs/ffunicode.c
    ${REPO_ROOT}/third_party/fatfs/ffsystem.c
    ${REPO_ROOT}/runtime/vm/vm_sys_input.c
    ${REPO_ROOT}/runtime/vm/vm_sys_graphics.c
    ${REPO_ROOT}/runtime/vm/vm_sys_time.c
    ${REPO_ROOT}/runtime/vm/vm_sys_pwm.c
    esp32_bc_alloc.c                        # VM runtime on MMBasic heap; compiler scratch on ESP-IDF internal heap
    ${REPO_ROOT}/runtime/vm/vm_sys_pin.c
    ${REPO_ROOT}/runtime/vm/vm_sys_file.c
)

# ESP32 port-local sources. Unsupported peripherals are represented by
# explicit ESP32 stubs here instead of being inherited from host_native.
set(PORT_LOCAL_SRCS
    app_main.c                              # IDF entry, MMBasic boot, REPL
    esp32_console.c                         # USB Serial/JTAG read/write (raw bytes)
    hal_serial_console_esp32.c              # raw byte console HAL for XMODEM
    esp32_mmbasic_console_glue.c            # MMPrintString / MMInkey + ring state (MMputchar / MMgetline are shared)
    esp32_usb_role.c                        # OPTION USB SERIAL/KEYBOARD persisted role selection
    esp32_usb_keyboard.c                    # USB host HID boot-keyboard input queue
    esp32_board_profile.c                   # Generic/Metro/Freenove board profile table
    ${REPO_ROOT}/runtime/runtime_getline.c        # shared console-line reader (see runtime/runtime_getline.c)
    ${REPO_ROOT}/runtime/runtime_console_putchar.c # shared MMputchar (Finding 1 in docs/port-duplication-audit.md)
    ${REPO_ROOT}/runtime/runtime_console_printstring.c # shared MMPrintString/SSPrintString (Finding 1 remainder)
    ${REPO_ROOT}/runtime/runtime_console_adapter.c # shared adapter ptr storage + set/get
    ${REPO_ROOT}/runtime/runtime_console_escdecode.c # shared escape-sequence decoder (Finding 2)
    ${REPO_ROOT}/runtime/runtime_filesystem_defaults.c # shared port_drivecheck_remap + port_filesystem_prefix (Finding 7)
    ${REPO_ROOT}/drivers/calendar/calendar_bare.c      # hal_calendar: port-agnostic timegm/gmtime replacement
    esp32_system.c                          # CPU RESTART / SLEEP
    esp32_terminal.c                        # VT100 CLS + COLOUR via core/mmbasic/Draw.c port hooks
    esp32_lfs.c                             # LittleFS over esp_partition_* (A: drive)
    esp32_flash_storage.c                   # flash_target_* / flash_option_* / flash_range_* / esp32_options_snapshot
    esp32_compat.c                          # small porting bits
    hal_psram_esp32.c                       # PSRAM HAL: slab acquisition, cache sync, PSRAMbase/PSRAMsize publication
    hal_pin_esp32.c                         # real GPIO/ADC over driver/gpio.h + esp_adc
    hal_pwm_esp32.c                         # PWM/SERVO over LEDC (hal_pwm.h)
    hal_i2c_esp32.c                         # I2C master over driver/i2c_master.h (hal_i2c.h)
    hal_i2c_esp32_slave.c                   # ESP32-S3 register-level I2C slave ISR (hal_i2c.h)
    esp32_pin_tables.c                      # ESP32-S3 GPIO map + GPn codemap
    hal_ws2812_esp32.c                      # WS2812/SK6812 over ESP32 RMT
    esp32_audio_options.c                   # OPTION AUDIO / MM.INFO$(AUDIO) surface
    esp32_audio_es8311.c                    # ES8311 codec glue (OPTION AUDIO ES8311)
    ${REPO_ROOT}/drivers/es8311/es8311.c    # MCU-neutral ES8311 register recipe
    ${AUDIO_BACKEND_SRCS}
    esp32_backlight.c                       # Option-pin LCD backlight PWM
    hal_vm_framebuffer_esp32.c             # ESP32 web display + Freenove ILI9341 FRAMEBUFFER backend
    esp32_ili9341_lcd.c                     # Option-pin SPI ILI9341 local LCD
    esp32_lcd_options.c                     # OPTION SYSTEM SPI / OPTION LCDPANEL setters
    esp32_freenove_i2c.c                    # shared Freenove touch/audio I2C bus
    esp32_ft6336u_touch.c                   # Freenove FNK0104B FT6336U touch reader
    esp32_touch_function.c                  # ESP32 TOUCH() function over port-local touch readers
    esp32_gui_touch_adapter.c               # Freenove FT6336U bridge for shared GUI-control touch state
    esp32_fastgfx.c                         # FASTGFX for ESP32 VGA scanout and Freenove ILI9341
    esp32_vga.c                             # OPTION VGA parser + LCD_CAM display bring-up
    ${REPO_ROOT}/drivers/vga_lcdcam_s3/vga_lcdcam_s3.c  # ESP32-S3 LCD_CAM VGA scanout (RGB332)
    ${REPO_ROOT}/drivers/draw_rgb332/draw_rgb332.c      # shared RGB332 draw primitives
    hal_time_esp32.c
    hal_random_esp32.c
    hal_net_esp32.c                         # ESP-IDF socket transport HAL
    esp32_mbedtls_mem.c                     # TLS allocator: PSRAM-first, internal fallback
    ${REPO_ROOT}/hal/generic/hal_watchdog_noop.c
    hal_flash_esp32.c                         # NVS-backed Options plus mmslots-backed program/slot persistence
    hal_storage_esp32_stub.c
    ${REPO_ROOT}/ports/pico_sdk_common/hal_filesystem_pico.c  # shared A:/B: LFS/FatFS dispatcher
    esp32_sd_diskio.c                       # board-profile SDSPI diskio backend for B:
    hal_keyboard_esp32_stub.c
    # D-decouple: ESP32 owns the small port surface that was originally
    # borrowed from host_native during bring-up.
    esp32_default_hooks.c                   # 17 port_* no-ops
    esp32_globals.c                         # 14 tentative-def globals
    esp32_runtime.c                         # 9 VM trampoline / abort hooks + state
    esp32_cmd_files_hooks.c                 # 3 A:-only filesystem hooks
    esp32_peripheral_stubs.c                # 117 cmd_/fun_/AES_/GPS_/etc decls
    esp32_wifi.c                            # ESP-IDF native WiFi OPTION / WEB / MM.INFO surface
    esp32_mqtt.c                            # ESP32 MQTT command/poll adapter over HAL MQTT
    esp32_ntp.c                             # ESP32 WEB NTP adapter over shared HAL transport
    esp32_tcp_client.c                      # ESP32 TCP client/stream command adapter
    esp32_tcp_server.c                      # ESP32 TCP server service and WEB TRANSMIT adapter
    esp32_tftp.c                            # ESP32 TFTP service adapter over shared core
    esp32_telnet.c                          # ESP32 Telnet console service over HAL TCP
    esp32_udp.c                             # ESP32 UDP service and WEB UDP adapter
)

set(EMBED_DEMOS
    # Bundled demo programs. esp32_lfs writes these into the LFS partition on
    # first-boot format, so the user lands in a working REPL with example
    # code already in A:.
    demos/hello.bas
    demos/fizzbuzz.bas
    demos/sieve.bas
    demos/mand.bas
    demos/web_hello.bas
    demos/site.bas
    demos/site_index.htm
    demos/site_status.htm
    demos/site_about.htm
    demos/site_gpio.htm
    demos/site_files.htm
    demos/site_style.css
)

# Resolve every bare (port-local) entry against esp32_s3/main so a sibling
# port directory compiles the identical files. Absolute paths pass through.
_esp32_prefix_relative(CORE_SRCS          ${ESP32_S3_MAIN_DIR} ${CORE_SRCS})
_esp32_prefix_relative(STATE_SRCS         ${ESP32_S3_MAIN_DIR} ${STATE_SRCS})
_esp32_prefix_relative(RUNTIME_SRCS       ${ESP32_S3_MAIN_DIR} ${RUNTIME_SRCS})
_esp32_prefix_relative(DEVICE_FACING_SRCS ${ESP32_S3_MAIN_DIR} ${DEVICE_FACING_SRCS})
_esp32_prefix_relative(DRIVER_STUBS       ${ESP32_S3_MAIN_DIR} ${DRIVER_STUBS})
_esp32_prefix_relative(BC_SRCS            ${ESP32_S3_MAIN_DIR} ${BC_SRCS})
_esp32_prefix_relative(PORT_LOCAL_SRCS    ${ESP32_S3_MAIN_DIR} ${PORT_LOCAL_SRCS})
_esp32_prefix_relative(EMBED_DEMOS        ${ESP32_S3_MAIN_DIR} ${EMBED_DEMOS})
