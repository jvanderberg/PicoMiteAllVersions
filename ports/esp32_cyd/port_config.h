/*
 * ports/esp32_cyd/port_config.h - port-config for the classic ESP32 CYD build.
 *
 * Port-scoped compile-time constants. Every value is defined here rather
 * than inherited from another port, so this port's hardware shape is
 * explicit.
 */

#ifndef ESP32_CYD_PORT_CONFIG_H
#define ESP32_CYD_PORT_CONFIG_H

/* Sentinel for hal/hal_port_assert.h: every TU that uses HAL_PORT_HAS_* in
 * a #if directive can include hal_port_assert.h to turn a missing
 * port_config.h into a build error instead of a silent eval-to-zero. */
#define HAL_PORT_CONFIG_INCLUDED 1

/* Chip-level: classic ESP32. GPIO 0..39 exist on the chip (with holes:
 * 20, 24, 28..31 are not bonded, 34..39 are input-only). The port does not
 * expose RP2040-style PWM slices or PIO blocks; BASIC PWM/SERVO uses the
 * ESP32 LEDC backend. */
#define HAL_PORT_PWM_SLICE_COUNT 0
#define HAL_PORT_GPIO_COUNT 40
#define HAL_PORT_PIO_COUNT 0
#define HAL_PORT_PULLDOWN_NEEDS_RESET 0

/* ADC OPEN streaming is not wired on ESP32. BASIC SETPIN ...,ARAW uses the
 * classic-ESP32 pin table and hal_pin_esp32.c directly, so this remains
 * zero. */
#define HAL_PORT_ADC_CHANNEL_MAX 0
#define HAL_PORT_BACKLIGHT_VIA_KEYPAD_I2C 0

/* cmd_files flist[] cap. The ESP32 MMBasic heap is intentionally small
 * while WiFi is enabled, so keep the conservative host-sized listing cap
 * rather than the larger Pico device cap. */
#define HAL_PORT_FILES_MAX 128

/* Non-VGA serial REPL port. */
#define HAL_PORT_IS_VGA 0
#define HAL_PORT_HAS_HDMI 0
#define HAL_PORT_HAS_NEXTGEN_DISPLAY 0

/* ESP32 has a native ESP-IDF network implementation selected by its
 * port-local WEB commands and HAL sources. This macro gates the legacy
 * Pico WEB compile path, so it stays disabled here. */
#define HAL_PORT_HAS_WIFI 0
#define HAL_PORT_HAS_GUICONTROLS 1
/* Small GUI-control cap: the control table is static DRAM, and classic
 * ESP32 has roughly a third of the S3's internal RAM for it. Keep enough
 * slots for the packaged GUI kitchen-sink demo (#1..#17). */
#define HAL_PORT_GUI_MAX_CONTROLS 24
#define HAL_PORT_KEYBOARD_USB_HOST 0
#define HAL_PORT_HAS_I2C_KEYPAD 0

/* I2C and audio constants are compile-time defaults for shared code paths. */
#define HAL_PORT_I2C_TIMEOUT_MS 5
#define HAL_PORT_I2C_SLOW_HZ 100000
#define HAL_PORT_AUDIO_FLAC_MAX_BASE_HZ 44100
#define HAL_PORT_AUDIO_MOD_BUFFER_SIZE 6144
#define HAL_PORT_HAS_MP3 0
/* Classic CYD boards usually have no PSRAM. Keep streamed audio buffering
 * small enough to allocate from internal heap after Wi-Fi/LCD/SD init. */
#define HAL_PORT_AUDIO_SAMPLE_RING_FRAMES 1024u
#define HAL_PORT_AUDIO_WORKMEM_USE_BASIC_HEAP 1
#define HAL_PORT_AUDIO_MOD_MAX_FILE_BYTES (24u * 1024u)

/* Compile-time fallback I2S DAC pins (e.g. MAX98357A, PCM5102, UDA1334).
 * PLAY TONE / SOUND / NOTE synthesize 16-bit stereo PCM
 * (shared/audio/synth_pcm.c). OPTION AUDIO can switch the saved
 * configuration to either standard I2S or I2S PDM TX. Factory-reset
 * profiles decide whether any audio pins are enabled. */
#define HAL_PORT_AUDIO_SAMPLE_RATE 44100
#define HAL_PORT_AUDIO_I2S_BCLK_PIN 5 /* bit clock  (BCLK/SCK) */
#define HAL_PORT_AUDIO_I2S_WS_PIN 6   /* word select (LRCLK/WS) */
#define HAL_PORT_AUDIO_I2S_DOUT_PIN 7 /* serial data (DIN/SD)  */

#define PORT_RAM_FUNC(name) name
#define PORT_TIMING_CRITICAL_FUNC(name) name
#define MMB_HOT_FUNC(name) name
#define MMB_DISPATCH_FUNC(name) name

#define HAL_PORT_FRAMEBUFFER_TRAILER_BYTES 0
#define HAL_PORT_ALLMEMORY_ALIGN 256

#define HAL_PORT_DEVICE_NAME "MMBasic ESP32"

/* Ports without SPI LCD still need this as a compile-time expression for
 * shared display code that is linked with stubs. */
#define HAL_PORT_LCD_SPI_CLK_PIN Option.SYSTEM_CLK

/* Serial console medium font. Value is a symbol name, resolved in Draw.c. */
#define HAL_PORT_CONSOLE_FONT_MEDIUM Hom_16x24_LE

/* Preserve current ESP32 behaviour: RANDOMIZE with no seed uses the same
 * deterministic seed the port had before this config was made explicit. */
#define HAL_PORT_RANDOMIZE_DEFAULT_SEED() ((int64_t)42)

/* Banner identifies the port to anyone connected to the UART console. */
#define MMBASIC_BANNER_NAME "MMBasic Anywhere (esp32-cyd)"

#define MMBASIC_BANNER_TRAILER "ESP32 REPL.\r\n\r\n"

/* 48 KB MMBasic heap (RUN capacity vs FRUN compile headroom balance). Classic ESP32 has 520 KB of internal SRAM with
 * roughly 300 KB usable as DRAM, and the static dram0_0 segment (which
 * holds AllMemory and the variable table) is the scarcest slice of it.
 * MAX_PROG_SIZE is pinned separately below so growing the heap does not
 * move the program mirror, the mmslots flash layout, or the saved-option
 * validation. Bytecode compiler scratch tables allocate from the ESP-IDF
 * internal heap, but VM runtime allocations come from this heap. */
#define HAL_PORT_HEAP_MEMORY_SIZE (48 * 1024)
#define MAX_PROG_SIZE (32 * 1024)

/* Per-port memory + clock + MMBasic-table values. The flash offsets remain
 * the legacy 1 MB values because FileIO.c still computes absolute offsets;
 * esp32_flash_storage.c translates them to the mmslots partition at the
 * port boundary. */
#define HAL_PORT_MAX_CPU 240000
#define HAL_PORT_MIN_CPU 48000
/* 256 variables: the variable table is 56 bytes per slot of static DRAM
 * (g_vartbl), so the table is half the S3's. */
#define HAL_PORT_MAX_VARS 256
#define HAL_PORT_MAX_SUBFUN 256
#define HAL_PORT_FLASH_TARGET_OFFSET (1024 * 1024)
#define HAL_PORT_FLASH_TARGET_OFFSET_USB (1024 * 1024)
#define HAL_PORT_MAGIC_KEY 0xE1799B93
#define HAL_PORT_MAGIC_KEY_USB 0xE1799B93
#define HAL_PORT_HEAP_TOP 0
#define HAL_PORT_HEAP_TOP_USB 0
/* WiFi-capable port: telnet delivers data in TCP segments (lwIP MSS ~1460).
 * The 256-byte ring used by non-WiFi ports overflows on long single-segment
 * bursts before MMgetline can drain, silently dropping the tail of the
 * line, so reserve a TCP_MSS-sized buffer here. */
#define HAL_PORT_CONSOLE_RX_BUF_SIZE 1536
#define HAL_PORT_PIOMAX 0
#define HAL_PORT_NBR_PINS 40

/* ESP32 has no RP2040 PIO blocks. */
#define HAL_PORT_PIO0_CLAIMED false
#define HAL_PORT_PIO1_CLAIMED false
#define HAL_PORT_PIO2_CLAIMED false

/* BCCrashInfo storage placement. ESP32 currently uses regular BSS. */
#define BC_CRASH_INFO_ATTR

/* Minimum SPIRAM held back from the MMBasic slab for ESP-IDF's own later
 * use. Most CYD boards have no PSRAM at all (hal_psram_esp32.c then
 * publishes PSRAMbase/PSRAMsize = 0); WROVER-based clones get the same
 * detect-at-boot slab sizing as the S3 ports. */
#define HAL_PORT_PSRAM_RESERVE_MIN (128u * 1024u)

/* Slot region size for `RAM SAVE` / `RAM LOAD` numbered slots. The shared
 * formula PSRAMblock = PSRAMbase + PSRAMsize + 0x60000 puts this past the
 * heap region, so hal_psram_esp32.c allocates a physical slab of
 * heap + 0x60000 + HAL_PORT_PSRAM_BLOCK_SIZE bytes and publishes only the
 * heap portion as PSRAMsize so Memory.c's bitmap allocator stays within
 * it. */
#define HAL_PORT_PSRAM_BLOCK_SIZE (MAXRAMSLOTS * MAX_PROG_SIZE)

/* Compiler-table sizes: a classic-ESP32 CYD set. The compiler's transient
 * allocations first try ESP-IDF heap, then fall back to the MMBasic heap.
 * The no-PSRAM classic ESP32 does not always have a contiguous internal-heap
 * window large enough after Wi-Fi/LCD/FS init, while the BASIC heap is fully
 * reclaimed after the compile/compact step. */
#define HAL_PORT_BC_COMPILE_USE_BASIC_HEAP 1
#include "../bc_tables_esp32_cyd.h"

/* PinDef[] slot of the supply-voltage ADC input MM.SUPPLY reads (GP29 on
 * RP2 boards). Ports with no supply-rail ADC use slot 0, the NULL row,
 * which never reads EXT_ANA_IN, so MM.SUPPLY reports -1. */
#define HAL_PORT_SUPPLY_ADC_PIN 0

#endif /* ESP32_CYD_PORT_CONFIG_H */
