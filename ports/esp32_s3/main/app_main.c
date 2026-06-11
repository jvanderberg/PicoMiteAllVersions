/*
 * ports/esp32_s3/main/app_main.c
 *
 * Phase C: USB Serial/JTAG REPL.
 *
 * Initialises USB Serial/JTAG for non-blocking line-edited I/O,
 * brings up MMBasic core, then enters MMBasic_RunPromptLoop().
 * The chip's onboard LED blinks on a separate FreeRTOS task as a
 * liveness indicator independent of MMBasic.
 */

#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "hal/hal_psram.h"
#include "runtime/runtime.h"
#include "esp32_board_profile.h"
#include "esp32_option_ext.h"

extern jmp_buf mark;
extern unsigned char * flash_prog_buf;
extern void esp32_flash_prog_buf_init(void);
extern const uint8_t * flash_progmemory;
extern void flash_range_erase(uint32_t off, uint32_t count);
extern void esp32_console_init(void);
extern void MMBasic_PrintBanner(void);
extern int esp32_flash_storage_init(void);
extern int esp32_web_console_display_init(void);
extern void esp32_mmbasic_console_glue_init(void);
extern void esp32_usb_role_resolve_boot(void);
extern int esp32_usb_role_is_serial(void);
extern int esp32_usb_role_is_keyboard(void);
extern void esp32_usb_role_prepare_keyboard_host(void);
extern void esp32_usb_keyboard_start_host(void);
extern int esp32_usb_keyboard_has_keyboard(void);

static const char * TAG = "app_main";

/* Consecutive crash-induced reboots. Lives in RTC slow memory, so it
 * survives a panic/watchdog/brownout reset (which is a soft reset) but is
 * cleared by a true power-cycle. Boot uses it to shed risky init and, on
 * repeats, drop to GENERIC defaults — breaking a crash loop so the user
 * always reaches a prompt. Cleared once boot completes (just before the
 * banner). RTC_NOINIT memory is indeterminate on cold power-on, so only a
 * crash-reset path is allowed to carry it forward (see app_main). */
RTC_NOINIT_ATTR static uint32_t s_crash_boot_count;

/* A crash-class reset is one the firmware did not ask for: a CPU panic, a
 * watchdog timeout, or a brownout. Clean reboots (power-on, EN button,
 * esp_restart from CPU RESTART / OPTION changes) are not crashes. */
static int reset_reason_is_crash(esp_reset_reason_t reason) {
    return reason == ESP_RST_PANIC || reason == ESP_RST_TASK_WDT ||
           reason == ESP_RST_INT_WDT || reason == ESP_RST_WDT ||
           reason == ESP_RST_BROWNOUT;
}

static int esp32_saved_options_valid(const char ** reason) {
    if (Option.Magic != MagicKey) {
        if (reason) *reason = "bad magic";
        return 0;
    }
    if (Option.Width <= 0) {
        if (reason) *reason = "bad width";
        return 0;
    }
    if (Option.Height <= 0) {
        if (reason) *reason = "bad height";
        return 0;
    }
    if (!(Option.Tab == 2 || Option.Tab == 3 || Option.Tab == 4 || Option.Tab == 8)) {
        if (reason) *reason = "bad tab";
        return 0;
    }
    if (Option.PROG_FLASH_SIZE != MAX_PROG_SIZE) {
        if (reason) *reason = "program flash size mismatch";
        return 0;
    }
    if (!(ESP32_OPTION_USB_ROLE == USB_ROLE_SERIAL || ESP32_OPTION_USB_ROLE == USB_ROLE_KEYBOARD)) {
        if (reason) *reason = "bad usb role";
        return 0;
    }
    if (reason) *reason = "valid";
    return 1;
}

static void esp32_keyboard_mode_recovery(void) {
    if (!esp32_usb_role_is_keyboard()) return;

    for (int i = 0; i < 100; i++) {
        if (esp32_usb_keyboard_has_keyboard()) {
            MMPrintString("USB keyboard attached\r\n\r\n");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    MMPrintString("\r\nUSB keyboard not enumerated yet; staying in USB KEYBOARD mode\r\n");
}

void app_main(void) {
    esp_log_level_set("gpio", ESP_LOG_WARN);

    /* Crash-recovery boot. A crash-class reset carries the counter
     * forward; any clean reset (or an out-of-range value left by RTC RAM
     * that did not survive a deep brownout) resets it to zero. */
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    const int crash_reset = reset_reason_is_crash(reset_reason);
    if (!crash_reset || s_crash_boot_count > 32) s_crash_boot_count = 0;
    if (crash_reset) s_crash_boot_count++;
    /* Any crash sheds Wi-Fi (lighter, lower-current boot). Only a repeated
     * software crash drops to defaults: a brownout loop is a power problem,
     * not a bad saved config, so it must never wipe the user's options. */
    const int safe_mode = s_crash_boot_count >= 1;
    const int deep_safe_mode =
        s_crash_boot_count >= 2 && reset_reason != ESP_RST_BROWNOUT;
    if (safe_mode) {
        ESP_LOGW(TAG, "crash-recovery boot (count=%u, deep=%d)",
                 (unsigned)s_crash_boot_count, deep_safe_mode);
    }

    /* Acquire the PSRAM slab and publish PSRAMbase / PSRAMsize before
     * any code reads them. mmbasic_runtime_init_common's heap init does
     * not depend on PSRAM, but the shared boot banner (and any later
     * BASIC code referencing MM.INFO(PSRAM SIZE)) does. */
    hal_psram_init();

    /* MMBasic boot. flash_prog_buf is sized MAX_PROG_SIZE + 4096 in
     * esp32_compat.c; the init 0xff-fills both the program region
     * and the trailer to mirror erased-flash semantics. PrepareProgramExt
     * walks past the program terminator looking for 0xff as the "end of
     * program / start of CFunction area" sentinel — non-0xff bytes there
     * cause it to deref garbage. */
    esp32_flash_prog_buf_init();
    flash_progmemory = flash_prog_buf;
    esp32_mmbasic_console_glue_init();

    LoadOptions();
    const char * options_reason = NULL;
    if (deep_safe_mode) {
        /* Repeated crashes: a saved peripheral config (SD/display/audio
         * pins) is the prime suspect, so drop to GENERIC defaults regardless
         * of whether the options validate. ResetOptions(true) PERSISTS the
         * reset, permanently breaking the loop. */
        ESP_LOGE(TAG, "repeated crash reboots (%u); resetting options to GENERIC defaults",
                 (unsigned)s_crash_boot_count);
        ResetOptions(true);
    } else if (!esp32_saved_options_valid(&options_reason)) {
        ESP_LOGE(TAG, "saved options rejected at boot: %s; resetting to board defaults",
                 options_reason ? options_reason : "invalid");
        ResetOptions(true);
    } else if (safe_mode) {
        /* Single crash: boot as GENERIC for this boot only. Applying the
         * GENERIC defaults in RAM clears the SD / display / touch / audio
         * Option fields, so every profile-gated init below no-ops — skipping
         * the external-peripheral bring-up that is the likely crash source on
         * unknown hardware. This does NOT save: the configured profile stays
         * on flash and a clean reboot restores it. */
        ESP_LOGW(TAG, "safe boot: applying GENERIC profile in RAM (saved config preserved)");
        esp32_board_profile_apply_defaults(
            esp32_board_profile_by_id(ESP32_BOARD_PROFILE_ID_GENERIC));
    }

    esp32_usb_role_resolve_boot();
    if (esp32_usb_role_is_serial()) {
        esp32_console_init();

        /* Brief pause so the host monitor has a chance to attach before
         * the banner flies past. */
        for (int i = 0; i < 5; i++) {
            printf(".");
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        printf("\n");
    }

    extern short gui_font_width, gui_font_height;
    gui_font_width = 8;
    gui_font_height = 12;
    ApplyDefaultConsoleColours();
    esp32_flash_storage_init();

    mmbasic_runtime_init_common(NULL,
                                MMBASIC_RUNTIME_INIT_FLAG_INIT_BASIC |
                                    MMBASIC_RUNTIME_INIT_FLAG_INIT_HEAP |
                                    MMBASIC_RUNTIME_INIT_FLAG_CLEAR_ERROR);

    extern void esp32_sd_diskio_reset(void);
    extern void vm_sys_file_reset(void);
    extern void vm_sys_pin_reset(void);
    esp32_sd_diskio_reset();
    vm_sys_file_reset();
    vm_sys_pin_reset();
    extern void esp32_board_profile_reserve_pins(void);
    extern void esp32_board_profile_open_system_i2c(void);
    extern void esp32_audio_reserve_option_pins(void);
    extern void esp32_vga_reserve_option_pins(void);
    esp32_board_profile_reserve_pins();
    esp32_board_profile_open_system_i2c();
    esp32_audio_reserve_option_pins();
    esp32_vga_reserve_option_pins();

    /* ClearRuntime initialises OptionConsole (= 3 BOTH) and several other
     * runtime globals MMBasic expects to be sane before the first
     * EditInputLine. Without it, putConsole's `if (OptionConsole & 1)`
     * gate is false and PRINT / cmd_print produce no output until the
     * first error() call retroactively sets OptionConsole=1. */
    ClearRuntime(true);
    (void)esp32_web_console_display_init();

    /* Bring up local displays before USB host, Wi-Fi, and LittleFS. */
    extern void esp32_ili9341_lcd_init(void);
    esp32_ili9341_lcd_init();
    extern void esp32_ft6336u_touch_init(void);
    esp32_ft6336u_touch_init();
    extern void InitTouch(void);
    InitTouch();
    extern void esp32_vga_display_init(void);
    esp32_vga_display_init();

    if (esp32_usb_role_is_keyboard()) {
        esp32_usb_role_prepare_keyboard_host();
        esp32_usb_keyboard_start_host();
        esp32_keyboard_mode_recovery();
    }

    /* Bring up Wi-Fi at boot only when an SSID is configured, and never on
     * a crash-recovery boot. The Wi-Fi stack is the largest and most
     * memory-hungry piece of pre-prompt work, so an unconfigured (e.g.
     * GENERIC) or recovering board reaches the prompt without it. OPTION
     * WIFI / WEB / WEB SCAN still bring Wi-Fi up on demand. In keyboard
     * mode USB host starts first because Wi-Fi consumes scarce internal RAM
     * needed by the USB host controller and transfer tasks. */
    extern void WebConnect(void);
    if (!safe_mode && *Option.SSID) WebConnect();

    /* Mount LittleFS for A: drive eagerly so cmd_files / cmd_save /
     * cmd_load can call lfs_*_open directly without going through
     * hal_fs_* (which would lazy-mount). First boot formats + writes
     * the bundled demo .bas files. */
    extern int esp32_lfs_mount(void);
    esp32_lfs_mount();

    /* Survived every init path — clear the crash-loop counter so the next
     * boot starts fresh. */
    s_crash_boot_count = 0;

    MMBasic_PrintBanner();
    MMPrintString("Profile: ");
    MMPrintString((char *)esp32_board_profile_current()->platform_name);
    MMPrintString("\r\n");
    if (safe_mode) {
        if (deep_safe_mode)
            MMPrintString("Safe mode: repeated resets — options reset to "
                          "GENERIC defaults.\r\n");
        else
            MMPrintString("Safe mode: recovered from an unexpected reset; "
                          "booted as GENERIC (display/SD/audio/Wi-Fi skipped). "
                          "Reboot to restore your saved configuration.\r\n");
    }
    MMPrintString("\r\n");

    /* MMBasic_RunPromptLoop is its own setjmp loop — it longjmps back
     * to its own `mark` on error / Ctrl-C / END / NEW. We don't return. */
    mmbasic_runtime_enter_repl(NULL, 0);
}
