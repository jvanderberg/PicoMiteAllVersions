/*
 * esp32_board_profile.c - ESP32-S3 board profile selection.
 */

#include <stddef.h>
#include <string.h>

#include "esp_system.h"

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "hal/hal_i2c.h"
#include "esp32_board_profile.h"
#include "esp32_option_ext.h"
#include "esp32_ft6336u_touch.h"
#include "esp32_touch_port.h"

extern int esp32_ft6336u_touch_is_ready(void);

static int s_sd_pins_reserved;
static int s_lcd_pins_reserved;
static int s_touch_pins_reserved;
static int s_ws2812_pins_reserved;

static const esp32_board_profile_t s_profiles[] = {
    {
        .id = ESP32_BOARD_PROFILE_ID_GENERIC,
        .configure_name = ESP32_BOARD_GENERIC_NAME,
        .device_name = ESP32_BOARD_GENERIC_DEVICE_NAME,
        .platform_name = ESP32_BOARD_GENERIC_NAME,
        .has_sd = 0,
        .has_lcd = 0,
        .has_touch = 0,
        .has_audio = 0,
        .has_ws2812 = 0,
        .sd = {ESP32_BOARD_PROFILE_NO_PIN, ESP32_BOARD_PROFILE_NO_PIN,
               ESP32_BOARD_PROFILE_NO_PIN, ESP32_BOARD_PROFILE_NO_PIN,
               ESP32_BOARD_PROFILE_NO_PIN, ESP32_BOARD_PROFILE_NO_PIN, 0},
        .lcd = {ESP32_BOARD_PROFILE_NO_PIN, ESP32_BOARD_PROFILE_NO_PIN,
                ESP32_BOARD_PROFILE_NO_PIN, ESP32_BOARD_PROFILE_NO_PIN,
                ESP32_BOARD_PROFILE_NO_PIN, ESP32_BOARD_PROFILE_NO_PIN,
                ESP32_BOARD_PROFILE_NO_PIN, 0},
        .touch = {ESP32_BOARD_PROFILE_NO_PIN, ESP32_BOARD_PROFILE_NO_PIN,
                  ESP32_BOARD_PROFILE_NO_PIN, ESP32_BOARD_PROFILE_NO_PIN},
        .audio = {ESP32_AUDIO_SINK_NONE, ESP32_BOARD_PROFILE_NO_PIN,
                  ESP32_BOARD_PROFILE_NO_PIN, ESP32_BOARD_PROFILE_NO_PIN,
                  ESP32_BOARD_PROFILE_NO_PIN, ESP32_BOARD_PROFILE_NO_PIN,
                  ESP32_BOARD_PROFILE_NO_PIN, 0, ESP32_BOARD_PROFILE_NO_PIN,
                  ESP32_BOARD_PROFILE_NO_PIN, 0},
        .ws2812_pin = ESP32_BOARD_PROFILE_NO_PIN,
    },
    {
        .id = ESP32_BOARD_PROFILE_ID_METRO,
        .configure_name = ESP32_BOARD_METRO_NAME,
        .device_name = ESP32_BOARD_METRO_DEVICE_NAME,
        .platform_name = ESP32_BOARD_METRO_NAME,
        .has_sd = 1,
        .has_lcd = 0,
        .has_touch = 0,
        .has_audio = 1,
        .has_ws2812 = 1,
        .sd = {ESP32_BOARD_METRO_SD_SCLK, ESP32_BOARD_METRO_SD_MOSI,
               ESP32_BOARD_METRO_SD_MISO, ESP32_BOARD_METRO_SD_CS,
               ESP32_BOARD_PROFILE_NO_PIN, ESP32_BOARD_PROFILE_NO_PIN,
               ESP32_BOARD_PROFILE_SD_SPI_FREQ_KHZ},
        .lcd = {ESP32_BOARD_PROFILE_NO_PIN, ESP32_BOARD_PROFILE_NO_PIN,
                ESP32_BOARD_PROFILE_NO_PIN, ESP32_BOARD_PROFILE_NO_PIN,
                ESP32_BOARD_PROFILE_NO_PIN, ESP32_BOARD_PROFILE_NO_PIN,
                ESP32_BOARD_PROFILE_NO_PIN, 0},
        .touch = {ESP32_BOARD_PROFILE_NO_PIN, ESP32_BOARD_PROFILE_NO_PIN,
                  ESP32_BOARD_PROFILE_NO_PIN, ESP32_BOARD_PROFILE_NO_PIN},
        .audio = {ESP32_AUDIO_SINK_I2S_DAC, ESP32_BOARD_PROFILE_NO_PIN,
                  ESP32_BOARD_METRO_AUDIO_BCLK, ESP32_BOARD_METRO_AUDIO_WS,
                  ESP32_BOARD_METRO_AUDIO_DOUT, ESP32_BOARD_PROFILE_NO_PIN,
                  ESP32_BOARD_PROFILE_NO_PIN, 0, ESP32_BOARD_PROFILE_NO_PIN,
                  ESP32_BOARD_PROFILE_NO_PIN, 0},
        .ws2812_pin = ESP32_BOARD_METRO_WS2812_PIN,
    },
    {
        .id = ESP32_BOARD_PROFILE_ID_FREENOVE_ILI9341,
        .configure_name = ESP32_BOARD_FREENOVE_ILI9341_NAME,
        .device_name = ESP32_BOARD_FREENOVE_ILI9341_DEVICE_NAME,
        .platform_name = ESP32_BOARD_FREENOVE_ILI9341_NAME,
        .has_sd = 1,
        .has_lcd = 1,
        .has_touch = 1,
        .has_audio = 1,
        .has_ws2812 = 0,
        .sd = {ESP32_BOARD_FREENOVE_SD_SCLK, ESP32_BOARD_FREENOVE_SD_MOSI,
               ESP32_BOARD_FREENOVE_SD_MISO, ESP32_BOARD_FREENOVE_SD_CS,
               ESP32_BOARD_FREENOVE_SD_D1, ESP32_BOARD_FREENOVE_SD_D2,
               ESP32_BOARD_PROFILE_SD_SPI_FREQ_KHZ},
        .lcd = {ESP32_BOARD_FREENOVE_LCD_SCLK, ESP32_BOARD_FREENOVE_LCD_MOSI,
                ESP32_BOARD_FREENOVE_LCD_MISO, ESP32_BOARD_FREENOVE_LCD_CS,
                ESP32_BOARD_FREENOVE_LCD_DC, ESP32_BOARD_FREENOVE_LCD_RST,
                ESP32_BOARD_FREENOVE_LCD_BL, ESP32_BOARD_FREENOVE_LCD_SPI_HZ,
                ILI9341, LANDSCAPE, 1},
        .touch = {ESP32_BOARD_FREENOVE_TOUCH_SDA, ESP32_BOARD_FREENOVE_TOUCH_SCL,
                  ESP32_BOARD_FREENOVE_TOUCH_INT, ESP32_BOARD_FREENOVE_TOUCH_RST},
        .audio = {ESP32_AUDIO_SINK_ES8311, ESP32_BOARD_FREENOVE_AUDIO_MCLK,
                  ESP32_BOARD_FREENOVE_AUDIO_BCLK, ESP32_BOARD_FREENOVE_AUDIO_WS,
                  ESP32_BOARD_FREENOVE_AUDIO_DOUT, ESP32_BOARD_FREENOVE_AUDIO_DIN,
                  ESP32_BOARD_FREENOVE_AUDIO_AMP_EN,
                  ESP32_BOARD_FREENOVE_AUDIO_AMP_ACTIVE_LEVEL,
                  ESP32_BOARD_FREENOVE_AUDIO_I2C_SDA,
                  ESP32_BOARD_FREENOVE_AUDIO_I2C_SCL,
                  ESP32_BOARD_FREENOVE_ES8311_ADDR},
        .ws2812_pin = ESP32_BOARD_PROFILE_NO_PIN,
    },
    {
        .id = ESP32_BOARD_PROFILE_ID_CYD,
        .configure_name = ESP32_BOARD_CYD_NAME,
        .device_name = ESP32_BOARD_CYD_DEVICE_NAME,
        .platform_name = ESP32_BOARD_CYD_NAME,
        .has_sd = 1,
        .has_lcd = 1,
        .has_touch = 1,
        .has_audio = 1,
        .has_ws2812 = 0,
        .sd = {ESP32_BOARD_CYD_SD_SCLK, ESP32_BOARD_CYD_SD_MOSI,
               ESP32_BOARD_CYD_SD_MISO, ESP32_BOARD_CYD_SD_CS,
               ESP32_BOARD_PROFILE_NO_PIN, ESP32_BOARD_PROFILE_NO_PIN,
               ESP32_BOARD_PROFILE_SD_SPI_FREQ_KHZ},
        .lcd = {ESP32_BOARD_CYD_LCD_SCLK, ESP32_BOARD_CYD_LCD_MOSI,
                ESP32_BOARD_CYD_LCD_MISO, ESP32_BOARD_CYD_LCD_CS,
                ESP32_BOARD_CYD_LCD_DC, ESP32_BOARD_CYD_LCD_RST,
                ESP32_BOARD_CYD_LCD_BL, ESP32_BOARD_CYD_LCD_SPI_HZ,
                ST7789B, LANDSCAPE, 1},
        .touch = {ESP32_BOARD_PROFILE_NO_PIN, ESP32_BOARD_PROFILE_NO_PIN,
                  ESP32_BOARD_CYD_TOUCH_IRQ, ESP32_BOARD_CYD_TOUCH_CS},
        .audio = {ESP32_AUDIO_SINK_INTERNAL_DAC, ESP32_BOARD_PROFILE_NO_PIN,
                  ESP32_BOARD_PROFILE_NO_PIN, ESP32_BOARD_PROFILE_NO_PIN,
                  ESP32_BOARD_CYD_AUDIO_DAC, ESP32_BOARD_PROFILE_NO_PIN,
                  ESP32_BOARD_PROFILE_NO_PIN, 0, ESP32_BOARD_PROFILE_NO_PIN,
                  ESP32_BOARD_PROFILE_NO_PIN, 0},
        .ws2812_pin = ESP32_BOARD_PROFILE_NO_PIN,
    },
};

static const size_t s_profile_count = sizeof(s_profiles) / sizeof(s_profiles[0]);

const esp32_board_profile_t * esp32_board_profile_by_id(uint8_t id) {
    for (size_t i = 0; i < s_profile_count; i++)
        if (s_profiles[i].id == id) return &s_profiles[i];
    return NULL;
}

const esp32_board_profile_t * esp32_board_profile_by_name(unsigned char * p) {
    for (size_t i = 0; i < s_profile_count; i++)
        if (checkstring(p, (unsigned char *)s_profiles[i].configure_name))
            return &s_profiles[i];
    if (checkstring(p, (unsigned char *)"FREENOVE"))
        return esp32_board_profile_by_id(ESP32_BOARD_PROFILE_ID_FREENOVE_ILI9341);
    return NULL;
}

const esp32_board_profile_t * esp32_board_profile_current(void) {
    const esp32_board_profile_t * profile =
        esp32_board_profile_by_id(ESP32_OPTION_BOARD_PROFILE);
    return profile ? profile : &s_profiles[0];
}

const char * esp32_board_profile_device_name(void) {
    return esp32_board_profile_current()->device_name;
}

uint8_t esp32_board_profile_current_id(void) {
    return esp32_board_profile_current()->id;
}

/* Resolve the active SD card pins into raw GPIO numbers for the SDSPI driver.
 * The pins come from Option.SD_* (set by OPTION SDCARD, or seeded from a board
 * profile's defaults), so SD configuration works like the other PicoMite ports.
 * Returns 0 when no SD card is configured. */
int esp32_sd_option_gpios(int * cs, int * sclk, int * mosi, int * miso) {
    if (!Option.SD_CS) return 0;
    *cs = PinDef[Option.SD_CS].GPno;
    *sclk = Option.SD_CLK_PIN ? PinDef[Option.SD_CLK_PIN].GPno : ESP32_BOARD_PROFILE_NO_PIN;
    *mosi = Option.SD_MOSI_PIN ? PinDef[Option.SD_MOSI_PIN].GPno : ESP32_BOARD_PROFILE_NO_PIN;
    *miso = Option.SD_MISO_PIN ? PinDef[Option.SD_MISO_PIN].GPno : ESP32_BOARD_PROFILE_NO_PIN;
    return 1;
}

void esp32_board_profile_set(uint8_t id) {
    if (!esp32_board_profile_by_id(id)) id = ESP32_BOARD_PROFILE_ID_GENERIC;
    ESP32_OPTION_BOARD_PROFILE = id;
}

static int profile_pin(int gpio) {
    return gpio >= 0 ? codemap(gpio) : 0;
}

static int profile_pin_invalid(int pin) {
    return pin <= 0 || pin > NBRPINS || (PinDef[pin].mode & UNUSED);
}

/* Reserve a pin given its codemap'd index (as stored in Option.SD_*), versus
 * reserve_profile_gpio() which takes a raw GPIO from the profile table. */
static void reserve_pin_index(int pin) {
    if (!profile_pin_invalid(pin)) ExtCurrentConfig[pin] = EXT_BOOT_RESERVED;
}

static void reserve_profile_gpio(int gpio) {
    int pin = profile_pin(gpio);
    if (!profile_pin_invalid(pin)) ExtCurrentConfig[pin] = EXT_BOOT_RESERVED;
}

static void release_pin_index(int pin) {
    if (!profile_pin_invalid(pin) && ExtCurrentConfig[pin] == EXT_BOOT_RESERVED) {
        ExtCurrentConfig[pin] = EXT_NOT_CONFIG;
    }
}

static int profile_pin_matches_gpio(int pin, int gpio) {
    return !profile_pin_invalid(pin) && pin == profile_pin(gpio);
}

static int current_audio_profile_uses_shared_i2c(void) {
    /* The ES8311 codec's control channel rides the shared I2C bus. */
    return ESP32_OPTION_AUDIO_KIND == ESP32_AUDIO_KIND_ES8311;
}

static int current_shared_i2c_enabled(void) {
    return esp32_ft6336u_touch_is_ready() || current_audio_profile_uses_shared_i2c();
}

/* Resolve the pin indices of the shared touch/audio I2C bus: the OPTION
 * SYSTEM I2C wiring when configured, else the Freenove audio profile's bus
 * pins. The fallback keys on the profile id, not on the live audio state,
 * so the pins still resolve (and can be released) after OPTION AUDIO
 * DISABLE has cleared the audio fields. */
static void shared_i2c_bus_pins(int * sda, int * scl) {
    *sda = Option.SYSTEM_I2C_SDA;
    *scl = Option.SYSTEM_I2C_SCL;
    if (*sda && *scl) return;
    const esp32_board_profile_t * profile = esp32_board_profile_current();
    if (profile->id != ESP32_BOARD_PROFILE_ID_FREENOVE_ILI9341) {
        *sda = 0;
        *scl = 0;
        return;
    }
    *sda = profile_pin(profile->audio.i2c_sda);
    *scl = profile_pin(profile->audio.i2c_scl);
}

static int current_profile_shared_i2c_pin(int pin) {
    if (profile_pin_invalid(pin)) return 0;
    int sda, scl;
    shared_i2c_bus_pins(&sda, &scl);
    if (!sda || !scl) return 0;
    return pin == sda || pin == scl;
}

void esp32_board_profile_apply_defaults(const esp32_board_profile_t * profile) {
    if (!profile) profile = &s_profiles[0];
    esp32_board_profile_set(profile->id);

    Option.SD_CS = 0;
    Option.SD_CLK_PIN = 0;
    Option.SD_MOSI_PIN = 0;
    Option.SD_MISO_PIN = 0;
    Option.SYSTEM_CLK = 0;
    Option.SYSTEM_MOSI = 0;
    Option.SYSTEM_MISO = 0;
    Option.LCD_CLK = 0;
    Option.LCD_MOSI = 0;
    Option.LCD_MISO = 0;
    Option.LCD_CD = 0;
    Option.LCD_CS = 0;
    Option.LCD_Reset = 0;
    Option.DISPLAY_BL = 0;
    Option.DISPLAY_TYPE = 0;
    Option.DISPLAY_CONSOLE = 0;
    Option.BGR = 0;

    Option.SYSTEM_I2C_SDA = 0;
    Option.SYSTEM_I2C_SCL = 0;
    Option.SYSTEM_I2C_SLOW = 0;
    Option.TOUCH_CAP = 0;
    Option.TOUCH_IRQ = 0;
    Option.TOUCH_CS = 0;
    Option.TOUCH_Click = 0;
    Option.THRESHOLD_CAP = 0;

    Option.SYSTEM_I2C_SDA = 0;
    Option.SYSTEM_I2C_SCL = 0;
    Option.SYSTEM_I2C_SLOW = 0;

    Option.AUDIO_L = 0;
    Option.AUDIO_R = 0;
    Option.AUDIO_SLICE = 99;
    Option.audio_i2s_bclk = 0;
    Option.audio_i2s_data = 0;
    ESP32_OPTION_AUDIO_KIND = ESP32_AUDIO_KIND_OFF;
    ESP32_OPTION_AUDIO_AMP_EN = 0;
    ESP32_OPTION_AUDIO_AMP_ACTIVE_HIGH = 0;
    ESP32_OPTION_AUDIO_I2S_WS = 0;
    ESP32_OPTION_AUDIO_I2S_MCLK = 0;

    if (profile->has_sd) {
        Option.SD_CS = profile_pin(profile->sd.cs);
        Option.SD_CLK_PIN = profile_pin(profile->sd.sclk);
        Option.SD_MOSI_PIN = profile_pin(profile->sd.mosi);
        Option.SD_MISO_PIN = profile_pin(profile->sd.miso);
    }
    if (profile->has_lcd) {
        Option.LCD_CLK = profile_pin(profile->lcd.sclk);
        Option.LCD_MOSI = profile_pin(profile->lcd.mosi);
        Option.LCD_MISO = profile_pin(profile->lcd.miso);
        Option.LCD_CD = profile_pin(profile->lcd.dc);
        Option.LCD_CS = profile_pin(profile->lcd.cs);
        Option.LCD_Reset = profile_pin(profile->lcd.rst);
        Option.DISPLAY_BL = profile_pin(profile->lcd.backlight);
        Option.DISPLAY_TYPE = profile->lcd.type;
        Option.DISPLAY_ORIENTATION = profile->lcd.orientation;
        Option.BGR = profile->lcd.bgr;
        Option.DefaultFont = 0x01;
        Option.DefaultFC = WHITE;
        Option.DefaultBC = BLACK;
        Option.ColourCode = 1;
    }

    if (profile->has_touch &&
        profile->id == ESP32_BOARD_PROFILE_ID_FREENOVE_ILI9341) {
        /* Seed the shared system I2C bus on the touch pins. These are
         * overridable defaults: a user OPTION SYSTEM I2C or SETPIN
         * reconfigures the pins and persists over this seeding. */
        Option.SYSTEM_I2C_SDA = profile_pin(profile->touch.sda);
        Option.SYSTEM_I2C_SCL = profile_pin(profile->touch.scl);
        Option.TOUCH_IRQ = profile_pin(profile->touch.interrupt);
        Option.TOUCH_CS = profile_pin(profile->touch.reset);
        Option.TOUCH_CAP = 1;
        Option.THRESHOLD_CAP = 22;
        esp32_touch_port_set_default_calibration();
    } else if (profile->has_touch &&
               profile->id == ESP32_BOARD_PROFILE_ID_CYD) {
        Option.TOUCH_IRQ = profile_pin(profile->touch.interrupt);
        Option.TOUCH_CS = profile_pin(profile->touch.reset);
        Option.TOUCH_CAP = 2;
        Option.THRESHOLD_CAP = 0;
        esp32_touch_port_set_default_calibration();
    } else {
        esp32_touch_port_set_identity_calibration();
    }

    if (profile->audio.sink == ESP32_AUDIO_SINK_I2S_DAC) {
        Option.audio_i2s_bclk = profile_pin(profile->audio.bclk);
        Option.audio_i2s_data = profile_pin(profile->audio.dout);
        ESP32_OPTION_AUDIO_I2S_WS = profile_pin(profile->audio.ws);
        ESP32_OPTION_AUDIO_KIND = ESP32_AUDIO_KIND_I2S;
    } else if (profile->audio.sink == ESP32_AUDIO_SINK_INTERNAL_DAC) {
        Option.audio_i2s_data = profile_pin(profile->audio.dout);
        ESP32_OPTION_AUDIO_KIND = ESP32_AUDIO_KIND_INTERNAL_DAC;
    } else if (profile->audio.sink == ESP32_AUDIO_SINK_ES8311) {
        Option.audio_i2s_bclk = profile_pin(profile->audio.bclk);
        Option.audio_i2s_data = profile_pin(profile->audio.dout);
        ESP32_OPTION_AUDIO_I2S_WS = profile_pin(profile->audio.ws);
        ESP32_OPTION_AUDIO_I2S_MCLK = profile_pin(profile->audio.mclk);
        ESP32_OPTION_AUDIO_AMP_EN = profile_pin(profile->audio.amp_enable);
        ESP32_OPTION_AUDIO_AMP_ACTIVE_HIGH =
            profile->audio.amp_active_level ? 1 : 0;
        ESP32_OPTION_AUDIO_KIND = ESP32_AUDIO_KIND_ES8311;
    }
}

void esp32_board_profile_reserve_pins(void) {
    const esp32_board_profile_t * profile = esp32_board_profile_current();
    if (Option.SD_CS) {
        /* SD pins come from Option.SD_* (OPTION SDCARD, or seeded from the
         * profile). D1/D2 are extra SDIO-socket lines that only a profile
         * carries, so reserve those from the profile when it has an SD. */
        reserve_pin_index(Option.SD_CS);
        reserve_pin_index(Option.SD_CLK_PIN);
        reserve_pin_index(Option.SD_MOSI_PIN);
        reserve_pin_index(Option.SD_MISO_PIN);
        if (profile->has_sd) {
            reserve_profile_gpio(profile->sd.d1);
            reserve_profile_gpio(profile->sd.d2);
        }
        s_sd_pins_reserved = 1;
    }
    if (profile->has_ws2812) {
        reserve_profile_gpio(profile->ws2812_pin);
        s_ws2812_pins_reserved = 1;
    }
}

/* Open the shared system I²C bus seeded by the board profile (Freenove seeds
 * SDA/SCL on the touch header). Mirrors the RP boot bring-up: reserve the
 * pins, then drive the bus the pins advertise (I2C0SDA -> bus 0, otherwise
 * bus 1) through the HAL. Leaves I2C0locked / I2C0SDApin / I2C0SCLpin /
 * I2C_enabled / I2C_Timeout in the same state the BASIC I2C layer reads, so
 * I2C READ/WRITE and the system bus work without an explicit OPTION SYSTEM
 * I2C. A user OPTION SYSTEM I2C DISABLE / reconfigure still overrides this. */
void esp32_board_profile_open_system_i2c(void) {
    if (!Option.SYSTEM_I2C_SDA) return;
    ExtCfg(Option.SYSTEM_I2C_SCL, EXT_BOOT_RESERVED, 0);
    ExtCfg(Option.SYSTEM_I2C_SDA, EXT_BOOT_RESERVED, 0);
    uint32_t baud = Option.SYSTEM_I2C_SLOW ? HAL_PORT_I2C_SLOW_HZ : 400000;
    if (PinDef[Option.SYSTEM_I2C_SDA].mode & I2C0SDA) {
        I2C0locked = 1;
        I2C0SDApin = Option.SYSTEM_I2C_SDA;
        I2C0SCLpin = Option.SYSTEM_I2C_SCL;
        I2C_enabled = 1;
        I2C_Timeout = SystemI2CTimeout;
        hal_i2c_master_init(0, PinDef[Option.SYSTEM_I2C_SDA].GPno,
                            PinDef[Option.SYSTEM_I2C_SCL].GPno, baud);
    } else {
        I2C1locked = 1;
        I2C1SDApin = Option.SYSTEM_I2C_SDA;
        I2C1SCLpin = Option.SYSTEM_I2C_SCL;
        I2C2_enabled = 1;
        I2C2_Timeout = SystemI2CTimeout;
        hal_i2c_master_init(1, PinDef[Option.SYSTEM_I2C_SDA].GPno,
                            PinDef[Option.SYSTEM_I2C_SCL].GPno, baud);
    }
}

void esp32_board_profile_reserve_lcd_pins(void) {
    /* LCD pins come from Option.LCD_* / Option.DISPLAY_BL (OPTION LCDPANEL
     * and OPTION SYSTEM SPI, or seeded from a profile's defaults). The
     * control pins are the panel-configured signal; DISPLAY_TYPE is bound
     * state and the web console rewrites it. */
    if (!Option.LCD_CD || !Option.LCD_CS) return;
    reserve_pin_index(Option.LCD_CLK);
    reserve_pin_index(Option.LCD_MOSI);
    reserve_pin_index(Option.LCD_MISO);
    reserve_pin_index(Option.LCD_CS);
    reserve_pin_index(Option.LCD_CD);
    reserve_pin_index(Option.LCD_Reset);
    reserve_pin_index(Option.DISPLAY_BL);
    s_lcd_pins_reserved = 1;
}

void esp32_board_profile_reserve_touch_pins(void) {
    /* Touch config lives in the PicoMite Option fields. Freenove uses the
     * shared system I2C bus; CYD uses XPT2046 GPIOs and does not touch I2C. */
    if (!Option.TOUCH_CAP &&
        esp32_board_profile_current_id() == ESP32_BOARD_PROFILE_ID_CYD) {
        const esp32_board_profile_t * profile = esp32_board_profile_current();
        Option.TOUCH_IRQ = profile_pin(profile->touch.interrupt);
        Option.TOUCH_CS = profile_pin(profile->touch.reset);
        Option.TOUCH_CAP = 2;
        esp32_touch_port_set_default_calibration();
    }
    if (!Option.TOUCH_CAP) return;
    if (Option.TOUCH_CAP == 1) esp32_board_profile_update_shared_i2c_pins();
    reserve_pin_index(Option.TOUCH_IRQ);
    reserve_pin_index(Option.TOUCH_CS);
    reserve_pin_index(Option.TOUCH_Click);
#if defined(ESP32_BOARD_CYD_TOUCH_SCLK)
    if (esp32_board_profile_current_id() == ESP32_BOARD_PROFILE_ID_CYD) {
        reserve_profile_gpio(ESP32_BOARD_CYD_TOUCH_SCLK);
        reserve_profile_gpio(ESP32_BOARD_CYD_TOUCH_MOSI);
        reserve_profile_gpio(ESP32_BOARD_CYD_TOUCH_MISO);
    }
#endif
    s_touch_pins_reserved = 1;
}

void esp32_board_profile_update_shared_i2c_pins(void) {
    int sda, scl;
    shared_i2c_bus_pins(&sda, &scl);
    if (!sda || !scl) return;
    if (current_shared_i2c_enabled()) {
        reserve_pin_index(sda);
        reserve_pin_index(scl);
    } else {
        release_pin_index(sda);
        release_pin_index(scl);
    }
}

int esp32_board_profile_pin_owned_by_shared_i2c(int pin) {
    return current_profile_shared_i2c_pin(pin) &&
           current_shared_i2c_enabled() &&
           ExtCurrentConfig[pin] == EXT_BOOT_RESERVED;
}

static int pin_is_audio_amp_pin(int pin) {
    return ESP32_OPTION_AUDIO_AMP_EN && pin == ESP32_OPTION_AUDIO_AMP_EN;
}

static int pin_is_generic_audio_pin(int pin) {
    if (Option.AUDIO_L && (pin == Option.AUDIO_L || pin == Option.AUDIO_R))
        return 1;
    if (!Option.audio_i2s_bclk) return 0;
    return pin == Option.audio_i2s_bclk ||
           pin == ESP32_OPTION_AUDIO_I2S_WS ||
           pin == Option.audio_i2s_data ||
           pin == ESP32_OPTION_AUDIO_I2S_MCLK;
}

static int pin_is_vga_pin(int pin) {
    if (pin == Option.VGA_HSYNC || pin == ESP32_OPTION_VGA_VSYNC)
        return 1;
    for (int i = 0; i < ESP32_OPTION_VGA_DATA_COUNT; i++)
        if (pin == ESP32_OPTION_VGA_DATA[i]) return 1;
    return 0;
}

const char * port_pin_reserved_label(int pin) {
    if (profile_pin_invalid(pin) || ExtCurrentConfig[pin] != EXT_BOOT_RESERVED)
        return NULL;

    const esp32_board_profile_t * profile = esp32_board_profile_current();
    if (s_sd_pins_reserved &&
        (pin == Option.SD_CS || pin == Option.SD_CLK_PIN ||
         pin == Option.SD_MOSI_PIN || pin == Option.SD_MISO_PIN ||
         profile_pin_matches_gpio(pin, profile->sd.d1) ||
         profile_pin_matches_gpio(pin, profile->sd.d2)))
        return "Boot Reserved : SD";

    if (s_lcd_pins_reserved &&
        (pin == Option.LCD_CLK || pin == Option.LCD_MOSI ||
         pin == Option.LCD_MISO || pin == Option.LCD_CS ||
         pin == Option.LCD_CD || pin == Option.LCD_Reset ||
         pin == Option.DISPLAY_BL))
        return "Boot Reserved : LCD";

    if (esp32_board_profile_pin_owned_by_shared_i2c(pin))
        return "Boot Reserved : Shared I2C touch/audio";

    if (s_touch_pins_reserved &&
        (pin == Option.TOUCH_IRQ || pin == Option.TOUCH_CS ||
         pin == Option.TOUCH_Click))
        return "Boot Reserved : Touch";

    if (pin_is_audio_amp_pin(pin) || pin_is_generic_audio_pin(pin))
        return "Boot Reserved : Audio";

    if (pin_is_vga_pin(pin))
        return "Boot Reserved : VGA";

    if (s_ws2812_pins_reserved && profile_pin_matches_gpio(pin, profile->ws2812_pin))
        return "Boot Reserved : WS2812";

    return NULL;
}

void esp32_board_profile_print_option(void) {
    /* Standard PicoMite OPTION PLATFORM dump: the user-set label, only when
     * one is set. Board identity is reported separately via MM.INFO$(DEVICE). */
    if (*Option.platform && (unsigned char)Option.platform[0] != 0xFF) {
        MMPrintString("OPTION PLATFORM ");
        MMPrintString((char *)Option.platform);
        MMPrintString("\r\n");
    }
}
