/*
 * esp32_lcd_options.c - OPTION SYSTEM SPI / OPTION LCDPANEL for ESP32-S3.
 *
 * Display wiring is user-assignable, PicoMite-style:
 *
 *   OPTION SYSTEM SPI clk, mosi, miso
 *   OPTION LCDPANEL ILI9341, LANDSCAPE, DC, RST, CS [, BL]
 *
 * Pins are stored in Option.LCD_* / Option.DISPLAY_BL as pin indices and
 * resolved to raw GPIOs by the drivers (esp32_ili9341_lcd.c,
 * esp32_backlight.c). Board profiles seed the same fields, so OPTION RESET
 * FREENOVE ILI9341 still configures the display in one step. RST may be
 * given as 0 for panels with no reset line (the Freenove wiring); BL is
 * optional. Like OPTION SDCARD, a successful setter saves and reboots so
 * the new wiring is validated and reserved by the normal boot path.
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"

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
        if (Option.DISPLAY_TYPE) error("Disable the LCD panel first");
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
        lcd_option_save_and_reset();
        return 1;
    }
    if (Option.DISPLAY_TYPE) error("Display already configured");
    if (!Option.LCD_CLK) error("Set OPTION SYSTEM SPI first");
    getargs(&tp, 11, (unsigned char *)",");
    if (argc != 9 && argc != 11)
        error("OPTION LCDPANEL ILI9341, orientation, DC, RST, CS [,BL]");
    if (!checkstring(argv[0], (unsigned char *)"ILI9341"))
        error("Display type not supported");
    if (!(checkstring(argv[2], (unsigned char *)"LANDSCAPE") ||
          checkstring(argv[2], (unsigned char *)"L")))
        error("Orientation not supported");

    int dc = esp32_parse_pin_arg(argv[4]);
    int rst = esp32_parse_pin_arg(argv[6]); /* 0 = no reset line */
    int cs = esp32_parse_pin_arg(argv[8]);
    int bl = (argc == 11) ? esp32_parse_pin_arg(argv[10]) : 0;
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
    Option.DISPLAY_TYPE = ILI9341;
    Option.DISPLAY_ORIENTATION = LANDSCAPE;
    lcd_option_save_and_reset();
    return 1;
}

int esp32_lcdpanel_option_setter(unsigned char * cmdline) {
    unsigned char * tp = checkstring(cmdline, (unsigned char *)"SYSTEM SPI");
    if (tp) return lcd_system_spi_setter(tp);
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
    if (Option.DISPLAY_TYPE == ILI9341) {
        MMPrintString("OPTION LCDPANEL ILI9341, LANDSCAPE, ");
        lcd_print_pin(Option.LCD_CD);
        MMPrintString(", ");
        lcd_print_pin(Option.LCD_Reset);
        MMPrintString(", ");
        lcd_print_pin(Option.LCD_CS);
        if (Option.DISPLAY_BL) {
            MMPrintString(", ");
            lcd_print_pin(Option.DISPLAY_BL);
        }
        MMPrintString("\r\n");
    }
}
