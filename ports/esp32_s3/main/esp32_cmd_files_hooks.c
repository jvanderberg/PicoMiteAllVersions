/*
 * esp32_cmd_files_hooks.c — ESP32 filesystem hooks for cmd_files.c.
 *
 * Three port_* hooks core code calls when handling drive-letter ops:
 *
 *   port_drive_check(drive)       — error if the drive isn't valid here.
 *   port_mount_sd_drive(void)     — mount B: when the selected board has SD.
 *   port_apply_load_overrides()   — board-specific Option fixups after
 *                                   LoadOptions().
 *
 * These are sole strong definitions in the ESP32 source list. Do not
 * reintroduce --wrap or link-order overrides for these hooks.
 */

#include <string.h>

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"

#include "FileIO.h"
#include "esp32_board_profile.h"
#include "esp32_option_ext.h"
#include "diskio.h"
#include "ff.h"

extern FATFS FatFs;
extern int FatFSFileSystem;
extern volatile BYTE SDCardStat;
extern hal_fs_fd_t hal_fds[];
extern void ErrorThrow(int e, int type);

/* Drive availability. A: is LittleFS on internal flash. B: is FatFs whenever an
 * SD card is configured (OPTION SDCARD, or seeded from a board profile). */
void port_drive_check(char drive) {
    if (drive == 'A') return;
    if (drive == 'B' && Option.SD_CS) return;
    error("Invalid disk");
}

/* port_drivecheck_remap + port_filesystem_prefix live in
 * runtime/runtime_filesystem_defaults.c — shared with Pico + host.
 * pc386 (FatFs on every volume, no LFS) overrides both. */

int port_mount_sd_drive(void) {
    ErrorThrow(0, NONEFILE); /* reset mm.errno */
    if (!Option.SD_CS) {
        ErrorThrow(FR_NOT_ENABLED, FATFSFILE);
        return 0;
    }
    if (!(SDCardStat & STA_NOINIT))
        return 1; /* already mounted */
    for (int i = 0; i < MAXOPENFILES; i++)
        if (FileTable[i].com > MAXCOMPORTS && hal_fds[i] != 0)
            ForceFileClose(i);
    FRESULT r = f_mount(&FatFs, "", 1);
    if (r != FR_OK) {
        FatFSFileSystem = 0;
        ErrorThrow(r, FATFSFILE);
        return 0;
    }
    return 2;
}

/* Per-board Option fixups after LoadOptions() reads the saved blob. */
void port_apply_load_overrides(void) {
    const esp32_board_profile_t * profile =
        esp32_board_profile_by_id(esp32_board_profile_current_id());
    if (!profile) profile = esp32_board_profile_by_id(ESP32_BOARD_PROFILE_ID_GENERIC);
    esp32_board_profile_set(profile->id);
    if (ESP32_OPTION_AUDIO_KIND > ESP32_AUDIO_KIND_ES8311)
        ESP32_OPTION_AUDIO_KIND = ESP32_AUDIO_KIND_OFF;
    if (Option.AUDIO_L && Option.AUDIO_R) {
        ESP32_OPTION_AUDIO_KIND = ESP32_AUDIO_KIND_PDM;
        ESP32_OPTION_AUDIO_AMP_EN = 0;
        ESP32_OPTION_AUDIO_AMP_ACTIVE_HIGH = 0;
        ESP32_OPTION_AUDIO_I2S_WS = 0;
        ESP32_OPTION_AUDIO_I2S_MCLK = 0;
    } else if (Option.audio_i2s_bclk && Option.audio_i2s_data) {
        if (ESP32_OPTION_AUDIO_KIND == ESP32_AUDIO_KIND_OFF)
            ESP32_OPTION_AUDIO_KIND = ESP32_AUDIO_KIND_I2S;
        if (!ESP32_OPTION_AUDIO_I2S_WS) {
            int ws_gpio = Option.audio_i2s_bclk <= NBRPINS
                              ? PinDef[Option.audio_i2s_bclk].GPno + 1
                              : -1;
            if (ws_gpio >= 0) ESP32_OPTION_AUDIO_I2S_WS = codemap(ws_gpio);
        }
        /* The ES8311 recipe needs the control bus; without OPTION SYSTEM
         * I2C fall back to plain I2S so the channel still runs. */
        if (ESP32_OPTION_AUDIO_KIND == ESP32_AUDIO_KIND_ES8311 &&
            !Option.SYSTEM_I2C_SDA)
            ESP32_OPTION_AUDIO_KIND = ESP32_AUDIO_KIND_I2S;
        if (ESP32_OPTION_AUDIO_AMP_EN > NBRPINS)
            ESP32_OPTION_AUDIO_AMP_EN = 0;
    } else {
        ESP32_OPTION_AUDIO_KIND = ESP32_AUDIO_KIND_OFF;
        ESP32_OPTION_AUDIO_AMP_EN = 0;
        ESP32_OPTION_AUDIO_AMP_ACTIVE_HIGH = 0;
        ESP32_OPTION_AUDIO_I2S_WS = 0;
        ESP32_OPTION_AUDIO_I2S_MCLK = 0;
    }
}
