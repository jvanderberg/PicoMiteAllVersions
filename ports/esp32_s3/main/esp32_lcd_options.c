/*
 * esp32_lcd_options.c - system bus + LCD panel OPTIONs for ESP32-S3.
 *
 * Display and bus wiring is user-assignable, PicoMite-style:
 *
 *   OPTION SYSTEM SPI clk, mosi, miso
 *   OPTION SYSTEM I2C sda, scl [, SLOW]
 *   OPTION LCDPANEL ILI9341, LANDSCAPE, DC, RST, CS [, BL] [, INVERT]
 *
 * Pins are stored in Option.LCD_* / Option.DISPLAY_BL /
 * Option.SYSTEM_I2C_* as pin indices and resolved to raw GPIOs by the
 * drivers (esp32_ili9341_lcd.c, esp32_backlight.c, esp32_ft6336u_touch.c).
 * Board profiles seed the same fields, so OPTION RESET FREENOVE ILI9341
 * still configures the display in one step. RST may be given as 0 for
 * panels with no reset line (the Freenove wiring); BL is optional. Like
 * OPTION SDCARD, a successful setter saves and reboots so the new wiring
 * is validated and reserved by the normal boot path.
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "esp32_board_profile.h"

extern int esp32_parse_pin_arg(unsigned char * arg);

static void lcd_option_validate_pin(int pin) {
    if (pin < 1 || pin > NBRPINS || (PinDef[pin].mode & UNUSED))
        error("Invalid pin");
    if (ExtCurrentConfig[pin] != EXT_NOT_CONFIG)
        error("Pin %/| is in use", pin, pin);
}

static void lcd_option_check_distinct(const int * pins, int count) {
    for (int i = 0; i < count; i++) {
        if (!pins[i]) continue;
        for (int j = i + 1; j < count; j++)
            if (pins[i] == pins[j]) error("Pin %/| is in use", pins[i], pins[i]);
    }
}

static void lcd_option_save_and_reset(void) {
    SaveOptions();
    _excep_code = RESET_COMMAND;
    SoftReset();
}

static int lcd_system_spi_setter(unsigned char * tp) {
    if (CurrentLinePtr) error("Invalid in a program");
    if (checkstring(tp, (unsigned char *)"DISABLE")) {
        /* The configured control pins are the panel selection (DISPLAY_TYPE
         * is bound state the web console rewrites). */
        if (Option.LCD_CD && Option.LCD_CS) error("Disable the LCD panel first");
        Option.LCD_CLK = 0;
        Option.LCD_MOSI = 0;
        Option.LCD_MISO = 0;
        lcd_option_save_and_reset();
        return 1;
    }
    if (Option.LCD_CLK) error("SPI already configured");
    getargs(&tp, 5, (unsigned char *)",");
    if (argc != 5) error("OPTION SYSTEM SPI clk, mosi, miso");
    int pins[3];
    pins[0] = esp32_parse_pin_arg(argv[0]); /* CLK */
    pins[1] = esp32_parse_pin_arg(argv[2]); /* MOSI */
    pins[2] = esp32_parse_pin_arg(argv[4]); /* MISO */
    for (int i = 0; i < 3; i++) lcd_option_validate_pin(pins[i]);
    lcd_option_check_distinct(pins, 3);
    Option.LCD_CLK = pins[0];
    Option.LCD_MOSI = pins[1];
    Option.LCD_MISO = pins[2];
    lcd_option_save_and_reset();
    return 1;
}

static int lcd_panel_setter(unsigned char * tp) {
    if (CurrentLinePtr) error("Invalid in a program");
    if (checkstring(tp, (unsigned char *)"DISABLE")) {
        Option.LCD_CD = 0;
        Option.LCD_CS = 0;
        Option.LCD_Reset = 0;
        Option.DISPLAY_BL = 0;
        Option.DISPLAY_TYPE = 0;
        Option.DISPLAY_CONSOLE = 0;
        Option.BGR = 0;
        Option.DISPLAY_MIRROR = 0;
        lcd_option_save_and_reset();
        return 1;
    }
    /* OPTION LCDPANEL MIRROR ON|OFF — horizontal flip for panel variants
     * whose scan is reflected relative to the validated default (some CYD
     * ST7789 sub-variants). Toggles in place without re-stating the panel. */
    unsigned char * mp;
    if ((mp = checkstring(tp, (unsigned char *)"MIRROR"))) {
        if (checkstring(mp, (unsigned char *)"ON"))
            Option.DISPLAY_MIRROR = 1;
        else if (checkstring(mp, (unsigned char *)"OFF"))
            Option.DISPLAY_MIRROR = 0;
        else
            error("OPTION LCDPANEL MIRROR ON|OFF");
        lcd_option_save_and_reset();
        return 1;
    }
    if (Option.LCD_CD && Option.LCD_CS) error("Display already configured");
    if (!Option.LCD_CLK) error("Set OPTION SYSTEM SPI first");
    getargs(&tp, 13, (unsigned char *)",");
    if (argc != 9 && argc != 11 && argc != 13)
        error("OPTION LCDPANEL ILI9341|ST7789, orientation, DC, RST, CS [,BL] [,INVERT]");
    int paneltype;
    if (checkstring(argv[0], (unsigned char *)"ILI9341"))
        paneltype = ILI9341;
    else if (checkstring(argv[0], (unsigned char *)"ST7789"))
        paneltype = ST7789B; /* 320x240 ST7789 (CYD2USB-class panels) */
    else
        error("Display type not supported");
    int orient;
    if (checkstring(argv[2], (unsigned char *)"LANDSCAPE") ||
        checkstring(argv[2], (unsigned char *)"L"))
        orient = LANDSCAPE;
    else if (checkstring(argv[2], (unsigned char *)"RLANDSCAPE") ||
             checkstring(argv[2], (unsigned char *)"RL"))
        orient = RLANDSCAPE;
    else if (checkstring(argv[2], (unsigned char *)"PORTRAIT") ||
             checkstring(argv[2], (unsigned char *)"P"))
        orient = PORTRAIT;
    else if (checkstring(argv[2], (unsigned char *)"RPORTRAIT") ||
             checkstring(argv[2], (unsigned char *)"RP"))
        orient = RPORTRAIT;
    else
        error("Orientation not supported");

    /* INVERT (panel colour-polarity inversion, PicoMite's Option.BGR) may
     * appear as the last argument, either in the BL slot or after it. */
    int invert = 0;
    int bl = 0;
    if (argc == 13) {
        if (!checkstring(argv[12], (unsigned char *)"INVERT")) error("Syntax");
        invert = 1;
        bl = esp32_parse_pin_arg(argv[10]);
    } else if (argc == 11) {
        if (checkstring(argv[10], (unsigned char *)"INVERT"))
            invert = 1;
        else
            bl = esp32_parse_pin_arg(argv[10]);
    }
    int dc = esp32_parse_pin_arg(argv[4]);
    int rst = esp32_parse_pin_arg(argv[6]); /* 0 = no reset line */
    int cs = esp32_parse_pin_arg(argv[8]);
    lcd_option_validate_pin(dc);
    lcd_option_validate_pin(cs);
    if (rst) lcd_option_validate_pin(rst);
    if (bl) lcd_option_validate_pin(bl);
    const int pins[] = {dc, rst, cs, bl,
                        Option.LCD_CLK, Option.LCD_MOSI, Option.LCD_MISO};
    lcd_option_check_distinct(pins, (int)(sizeof(pins) / sizeof(pins[0])));

    Option.LCD_CD = dc;
    Option.LCD_Reset = rst;
    Option.LCD_CS = cs;
    Option.DISPLAY_BL = bl;
    Option.DISPLAY_TYPE = paneltype;
    Option.DISPLAY_ORIENTATION = orient;
    Option.BGR = invert;
    lcd_option_save_and_reset();
    return 1;
}

/* OPTION SYSTEM I2C sda, scl [, SLOW] — the shared I2C bus (touch, and the
 * Freenove ES8311 codec). Mirrors the PicoMite command; the bus is declared
 * here, then OPTION TOUCH FT6336 attaches the controller to it. */
static int lcd_system_i2c_setter(unsigned char * tp) {
    if (CurrentLinePtr) error("Invalid in a program");
    if (checkstring(tp, (unsigned char *)"DISABLE")) {
        if (Option.TOUCH_CAP) error("Disable the touch first");
        Option.SYSTEM_I2C_SDA = 0;
        Option.SYSTEM_I2C_SCL = 0;
        Option.SYSTEM_I2C_SLOW = 0;
        lcd_option_save_and_reset();
        return 1;
    }
    if (Option.SYSTEM_I2C_SCL) error("I2C already configured");
    getargs(&tp, 5, (unsigned char *)",");
    if (argc != 3 && argc != 5) error("OPTION SYSTEM I2C sda, scl [,SLOW]");
    int slow = 0;
    if (argc == 5) {
        if (!checkstring(argv[4], (unsigned char *)"SLOW")) error("Syntax");
        slow = 1;
    }
    int pins[2];
    pins[0] = esp32_parse_pin_arg(argv[0]); /* SDA */
    pins[1] = esp32_parse_pin_arg(argv[2]); /* SCL */
    for (int i = 0; i < 2; i++) {
        if (pins[i] < 1 || pins[i] > NBRPINS || (PinDef[pins[i]].mode & UNUSED))
            error("Invalid pin");
        /* The Freenove ES8311 audio profile may already hold the shared
         * bus — declaring the same wiring is the normal arrangement. */
        if (ExtCurrentConfig[pins[i]] != EXT_NOT_CONFIG &&
            !esp32_board_profile_pin_owned_by_shared_i2c(pins[i]))
            error("Pin %/| is in use", pins[i], pins[i]);
    }
    if (pins[0] == pins[1]) error("Pin %/| is in use", pins[0], pins[0]);
    Option.SYSTEM_I2C_SDA = pins[0];
    Option.SYSTEM_I2C_SCL = pins[1];
    Option.SYSTEM_I2C_SLOW = slow;
    lcd_option_save_and_reset();
    return 1;
}

int esp32_lcdpanel_option_setter(unsigned char * cmdline) {
    unsigned char * tp = checkstring(cmdline, (unsigned char *)"SYSTEM SPI");
    if (tp) return lcd_system_spi_setter(tp);
    tp = checkstring(cmdline, (unsigned char *)"SYSTEM I2C");
    if (tp) return lcd_system_i2c_setter(tp);
    tp = checkstring(cmdline, (unsigned char *)"LCDPANEL");
    if (tp) return lcd_panel_setter(tp);
    return 0;
}

static void lcd_print_pin(int pin) {
    if (pin)
        MMPrintString((char *)PinDef[pin].pinname);
    else
        MMPrintString("0");
}

void esp32_lcd_print_options(void) {
    if (Option.LCD_CLK) {
        MMPrintString("OPTION SYSTEM SPI ");
        lcd_print_pin(Option.LCD_CLK);
        MMPrintString(", ");
        lcd_print_pin(Option.LCD_MOSI);
        MMPrintString(", ");
        lcd_print_pin(Option.LCD_MISO);
        MMPrintString("\r\n");
    }
    if (Option.LCD_CD && Option.LCD_CS) {
        const char * orient = "LANDSCAPE";
        switch (Option.DISPLAY_ORIENTATION) {
        case RLANDSCAPE:
            orient = "RLANDSCAPE";
            break;
        case PORTRAIT:
            orient = "PORTRAIT";
            break;
        case RPORTRAIT:
            orient = "RPORTRAIT";
            break;
        }
        MMPrintString("OPTION LCDPANEL ");
        MMPrintString(Option.DISPLAY_TYPE == ST7789B ? "ST7789" : "ILI9341");
        MMPrintString(", ");
        MMPrintString((char *)orient);
        MMPrintString(", ");
        lcd_print_pin(Option.LCD_CD);
        MMPrintString(", ");
        lcd_print_pin(Option.LCD_Reset);
        MMPrintString(", ");
        lcd_print_pin(Option.LCD_CS);
        if (Option.DISPLAY_BL || Option.BGR) {
            MMPrintString(", ");
            lcd_print_pin(Option.DISPLAY_BL);
        }
        if (Option.BGR) MMPrintString(", INVERT");
        MMPrintString("\r\n");
        if (Option.DISPLAY_MIRROR) MMPrintString("OPTION LCDPANEL MIRROR ON\r\n");
    }
}
