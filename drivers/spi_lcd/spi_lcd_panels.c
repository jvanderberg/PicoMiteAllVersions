/*
 * drivers/spi_lcd/spi_lcd_panels.c - transport-neutral SPI LCD controller core.
 *
 * Controller knowledge for the SPI LCD family: the display_details[] table,
 * per-controller init sequences, the command/data write helpers, address
 * window construction, and the panel reset. Bytes reach the panel
 * exclusively through hal/hal_spi_lcd_bus.h, so this file compiles on any
 * port that links a bus implementation (drivers/spi_lcd/spi_lcd_bus_pico.c
 * on Pico; the ESP32 port's spi_master-backed entry points).
 *
 * The initialisation sequences and the SPI driver code were written by
 * Peter Mather (matherp on The Back Shed forum).
 *
 * Deliberately NOT here: the shared-bus arbiter (SPISpeedSet), the fast
 * pixel/blit paths, presentation code, and the ST7920 init -- its CD line
 * doubles as an inverted-polarity select, a quirk owned by the pico bus
 * helpers in spi_lcd.c.
 */

#include <stdarg.h>
#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "hal/hal_spi_lcd_bus.h"

const struct Displays display_details[] = {
    {0, "", SDCARD_SPI_SPEED, 0, 0, 0, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {1, "", SDCARD_SPI_SPEED, 0, 0, 0, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {2, "SSD1306I2C", 400, 128, 64, 1, 1, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {3, "SSD1306I2C32", 400, 128, 32, 1, 1, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {4, "ILI9163", LCD_SPI_SPEED, 128, 128, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {5, "ILI9341", 50000000, 320, 240, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {6, "ST7735", LCD_SPI_SPEED, 160, 128, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {7, "ST7735S", LCD_SPI_SPEED, 160, 80, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {8, "SSD1331", LCD_SPI_SPEED, 96, 64, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {9, "ST7789", LCD_SPI_SPEED, 240, 240, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {10, "ILI9481", LCD_SPI_SPEED, 480, 320, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {11, "ILI9488", LCD_SPI_SPEED, 480, 320, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {12, "ILI9488P", LCD_SPI_SPEED, 320, 320, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {13, "ST7789_135", LCD_SPI_SPEED, 240, 135, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {14, "ST7789_320", 50000000, 320, 240, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {15, "ILI9488W", LCD_SPI_SPEED, 480, 320, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {16, "ST7796S", 50000000, 480, 320, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {17, "ST7796SP", 50000000, 320, 320, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {18, "ST7735S_W", LCD_SPI_SPEED, 128, 128, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {19, "GC9A01", LCD_SPI_SPEED, 240, 240, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {20, "ILI9481IPS", 12000000, 480, 320, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {21, "N5110", NOKIA_SPI_SPEED, 84, 48, 1, 1, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {22, "SSD1306SPI", LCD_SPI_SPEED, 128, 64, 1, 1, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {23, "ST7920", ST7920_SPI_SPEED, 128, 64, 1, 1, SPI_POLARITY_HIGH, SPI_PHASE_2EDGE},
    {24, "", TOUCH_SPI_SPEED, 0, 0, 0, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {25, "SPIReadSpeed", 12000000, 480, 320, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {26, "ST7789RSpeed", 6000000, 320, 240, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {27, "", SLOW_TOUCH_SPEED, 0, 0, 0, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {28, "User", 0, 0, 0, 0, 0, 0, 0},
    {29, "Dummy", 0, 0, 0, 0, 0, 0, 0},
    {30, "Dummy", 0, 0, 0, 0, 0, 0, 0},
    {31, "Dummy", 0, 0, 0, 0, 0, 0, 0},
    {32, "Dummy", 0, 0, 0, 0, 0, 0, 0},
    {33, "Dummy", 0, 0, 0, 0, 0, 0, 0},
    {34, "Dummy", 0, 0, 0, 0, 0, 0, 0},
    {35, "Dummy", 0, 0, 0, 0, 0, 0, 0},
    {36, "SSD1963_4", 0, 0, 0, 0, 0, 0, 0},
    {37, "SSD1963_5", 0, 0, 0, 0, 0, 0, 0},
    {38, "SSD1963_5A", 0, 0, 0, 0, 0, 0, 0},
    {39, "SSD1963_7", 0, 0, 0, 0, 0, 0, 0},
    {40, "SSD1963_7A", 0, 0, 0, 0, 0, 0, 0},
    {41, "SSD1963_8", 0, 0, 0, 0, 0, 0, 0},
    {42, "ILI9341_8", 0, 0, 0, 0, 0, 0, 0},
    {43, "SSD1963_4_16", 0, 0, 0, 0, 0, 0, 0},
    {44, "SSD1963_5_16", 0, 0, 0, 0, 0, 0, 0},
    {45, "SSD1963_5A_16", 0, 0, 0, 0, 0, 0, 0},
    {46, "SSD1963_7_16", 0, 0, 0, 0, 0, 0, 0},
    {47, "SSD1963_7A_16", 0, 0, 0, 0, 0, 0, 0},
    {48, "SSD1963_8_16", 0, 0, 0, 0, 0, 0, 0},
    {49, "ILI9341_16", 0, 0, 0, 0, 0, 0, 0},
    {50, "IPS_4_16", 0, 0, 0, 0, 0, 0, 0},
    {51, "SSD1963_5E_16", 0, 0, 0, 0, 0, 0, 0},
    {52, "SSD1963_7E_16", 0, 0, 0, 0, 0, 0, 0},
    {53, "ILI9486_16", 0, 0, 0, 0, 0, 0, 0},
    {54, "VIRTUAL_C", 0, 320, 240, 0, 0, 0, 0},
    {55, "VIRTUAL_M", 0, 640, 480, 0, 0, 0, 0},
    {56, "VS1053slow", 200000, 0, 0, 0, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {57, "VS1053fast", 4000000, 0, 0, 0, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    /* NEXTGEN buffered display rows. Compiled on every port; the
		 * runtime never sets Option.DISPLAY_TYPE to one of these on
		 * non-NEXTGEN ports because the OPTION setter rejects them. */
    {58, "ST7796SPBUFF", 90000000, 320, 320, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {59, "ILI9341BUFF", 50000000, 320, 240, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {60, "ST7796SBUFF", 90000000, 480, 320, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {61, "ILI9488BUFF", 45000000, 480, 320, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {62, "ILI9488PBUFF", 45000000, 320, 320, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {63, "ILI9488WBUFF", 45000000, 480, 320, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
    {64, "ST7789_320BUFF", 50000000, 320, 240, 16, 0, SPI_POLARITY_LOW, SPI_PHASE_1EDGE},
};

#if !HAL_PORT_IS_VGA

#define SPIsend(a)                    \
    {                                 \
        uint8_t b = a;                \
        hal_spi_lcd_bus_write(&b, 1); \
    }
#define SPIsend2(a) \
    {               \
        SPIsend(0); \
        SPIsend(a); \
    }

void spi_write_data(unsigned char data) {
    hal_spi_lcd_bus_dc(1);
    hal_spi_lcd_bus_begin();
    /* ILI9488WBUFF is a NEXTGEN-only DISPLAY_TYPE. Non-NEXTGEN ports
     * never set DISPLAY_TYPE to it, so the extra term is always false
     * there. Unify the comparison. */
    if (Option.DISPLAY_TYPE == ILI9481 || Option.DISPLAY_TYPE == ILI9488W || Option.DISPLAY_TYPE == ILI9488WBUFF) {
        SPIsend2(data);
    } else {
        SPIsend(data);
    }
    hal_spi_lcd_bus_end();
}

void spi_write_command(unsigned char data) {
    hal_spi_lcd_bus_dc(0);
    hal_spi_lcd_bus_begin();
    if (Option.DISPLAY_TYPE == ILI9481 || Option.DISPLAY_TYPE == ILI9488W || Option.DISPLAY_TYPE == ILI9488WBUFF) {
        SPIsend2(data);
    } else {
        SPIsend(data);
    }
    hal_spi_lcd_bus_end();
}

void spi_write_cd(unsigned char command, int data, ...) {
    int i;
    va_list ap;
    va_start(ap, data);
    spi_write_command(command);
    for (i = 0; i < data; i++) spi_write_data((char)va_arg(ap, int));
    va_end(ap);
}

void spi_write_CommandData(const uint8_t * pCommandData, uint8_t datalen) {
    int i;
    spi_write_command(*pCommandData++);
    hal_spi_lcd_bus_dc(1);
    for (i = 1; i < datalen; i++) {
        spi_write_data(*pCommandData++);
    }
}

void MIPS16 ResetController(void) {
    PinSetBit(Option.LCD_Reset, LATSET);
    uSec(10000);
    PinSetBit(Option.LCD_Reset, LATCLR);
    uSec(10000);
    PinSetBit(Option.LCD_Reset, LATSET);
    uSec(200000);
}

/* Initialise the selected SPI LCD controller. Handles every panel in the
 * family except ST7920 (see the file comment); returns 0 for that one so
 * the caller can run its quirk init, 1 otherwise. Unlisted DISPLAY_TYPE
 * values (the MEM332 family initialised via hal_spi_lcd_mem332) fall
 * through the switch untouched, exactly as they always have. */
int MIPS16 spi_lcd_panel_init(void) {
    switch (Option.DISPLAY_TYPE) {
    case ST7920:
        return 0;
    /* The *BUFF cases are MEM332-only DISPLAY_TYPE values; non-
		 * MEM332 ports never set Option.DISPLAY_TYPE to one of them
		 * so the case labels are dead code there. */
    case ST7796S:
    case ST7796SP:
    case ST7796SPBUFF:
    case ST7796SBUFF:
        ResetController();
        spi_write_cd(0xC5, 1, 0x1C); //VCOM  Control 1 [1C]
        spi_write_cd(0x3A, 1, 0x55); //565
        spi_write_command(0xB0);     //Interface     [00]
        uSec(150000);
        //0xB1, 2, 0xB0, 0x11,        //Frame Rate Control [A0 10]
        spi_write_cd(0xB4, 1, 0x01); //Inversion Control [01]
        if (Option.BGR)
            spi_write_command(0x21);
        else
            spi_write_command(0x20);
        spi_write_cd(0xB6, 3, 0x80, 0x02, 0x3B); // Display Function Control [80 02 3B] .kbv SS=1, NL=480
        spi_write_cd(0xB7, 1, 0xC6);             //Entry Mode      [06]
                                                 //    0xF7, 4, 0xA9, 0x51, 0x2C, 0x82,    //Adjustment Control 3 [A9 51 2C 82]
        spi_write_cd(0xF0, 1, 0xC3);             //?? lock manufacturer commands
        spi_write_cd(0xF0, 1, 0x96);             //
                                                 //		spi_write_cd(0xFB, 1, 0x3C);              //
        switch (Option.DISPLAY_ORIENTATION) {
        case LANDSCAPE:
            spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Landscape);
            break;
        case PORTRAIT:
            spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Portrait);
            break;
        case RLANDSCAPE:
            spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Landscape180);
            break;
        case RPORTRAIT:
            spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Portrait180);
            break;
        }
        if (Option.DISPLAY_TYPE == ST7796SP || Option.DISPLAY_TYPE == ST7796SPBUFF) {
            spi_write_cd(0x33, 6, 0x00, 0x00, 0x01, 0x40, 0x00, 0xA0);
        } else {
            spi_write_cd(0x33, 6, 0x00, 0x00, 0x01, 0xE0, 0x00, 0x00);
        }
        spi_write_command(0x11);
        uSec(150000);
        spi_write_command(0x29); //Display on
        uSec(150000);
        break;
    case ILI9488:
    case ILI9488P:
    case ILI9488W:
    case ILI9488PBUFF:
    case ILI9488BUFF:
    case ILI9488WBUFF:
        ResetController();
        if (Option.DISPLAY_TYPE == ILI9488 || Option.DISPLAY_TYPE == ILI9488P || Option.DISPLAY_TYPE == ILI9488PBUFF || Option.DISPLAY_TYPE == ILI9488BUFF) {
            spi_write_command(0xE0); // Positive Gamma Control
            spi_write_data(0x00);
            spi_write_data(0x03);
            spi_write_data(0x09);
            spi_write_data(0x08);
            spi_write_data(0x16);
            spi_write_data(0x0A);
            spi_write_data(0x3F);
            spi_write_data(0x78);
            spi_write_data(0x4C);
            spi_write_data(0x09);
            spi_write_data(0x0A);
            spi_write_data(0x08);
            spi_write_data(0x16);
            spi_write_data(0x1A);
            spi_write_data(0x0F);

            spi_write_command(0XE1); // Negative Gamma Control
            spi_write_data(0x00);
            spi_write_data(0x16);
            spi_write_data(0x19);
            spi_write_data(0x03);
            spi_write_data(0x0F);
            spi_write_data(0x05);
            spi_write_data(0x32);
            spi_write_data(0x45);
            spi_write_data(0x46);
            spi_write_data(0x04);
            spi_write_data(0x0E);
            spi_write_data(0x0D);
            spi_write_data(0x35);
            spi_write_data(0x37);
            spi_write_data(0x0F);

            spi_write_command(0XC0); // Power Control 1
            spi_write_data(0x17);
            spi_write_data(0x15);

            spi_write_command(0xC1); // Power Control 2
            spi_write_data(0x41);

            spi_write_command(0xC5); // VCOM Control
            spi_write_data(0x00);
            spi_write_data(0x12);
            spi_write_data(0x80);

            spi_write_command(TFT_MADCTL); // Memory Access Control
            spi_write_data(0x48);          // MX, BGR

            spi_write_command(0x3A); // Pixel Interface Format
            spi_write_data(0x66);    // 18 bit colour for SPI

            spi_write_command(0xB0); // Interface Mode Control
            spi_write_data(0x00);

            spi_write_command(0xB1); // Frame Rate Control
            spi_write_data(0xA0);
            if (Option.BGR) spi_write_command(0x21);
            spi_write_command(0xB4); // Display Inversion Control
            spi_write_data(0x02);

            spi_write_command(0xB6); // Display Function Control
            spi_write_data(0x02);
            spi_write_data(0x02);
            spi_write_data(0x3B);

            spi_write_command(0xB7); // Entry Mode Set
            spi_write_data(0xC6);

            spi_write_command(0xF7); // Adjust Control 3
            spi_write_data(0xA9);
            spi_write_data(0x51);
            spi_write_data(0x2C);
            spi_write_data(0x82);

            spi_write_command(TFT_SLPOUT); //Exit Sleep
            uSec(120000);
            if (Option.DISPLAY_TYPE == ILI9488P || Option.DISPLAY_TYPE == ILI9488PBUFF) {
                spi_write_command(0x33);
                spi_write_data(0x00);
                spi_write_data(0x00);
                spi_write_data(0x01);
                spi_write_data(0x40);
                spi_write_data(0x00);
                spi_write_data(0xA0);
            } else {
                spi_write_command(0x33);
                spi_write_data(0x00);
                spi_write_data(0x00);
                spi_write_data(0x01);
                spi_write_data(0xE0);
                spi_write_data(0x00);
                spi_write_data(0x00);
            }
            spi_write_command(TFT_DISPON); //Display on
            uSec(25000);
        } else {
            if (Option.BGR)
                spi_write_command(0x20);
            else
                spi_write_command(0x21);
            spi_write_command(0xC2); //Normal mode, increase can change the display quality, while increasing power consumption
            spi_write_data(0x33);
            spi_write_command(0XC5);
            spi_write_data(0x00);
            spi_write_data(0x1e); //VCM_REG[7:0]. <=0X80.
            spi_write_data(0x80);
            spi_write_command(0xB1); //Sets the frame frequency of full color normal mode
            spi_write_data(0xB0);    //0XB0 =70HZ, <=0XB0.0xA0=62HZ
            spi_write_command(0x36);
            spi_write_data(0x28); //2 DOT FRAME MODE,F<=70HZ.
            spi_write_command(0XE0);
            spi_write_data(0x0);
            spi_write_data(0x13);
            spi_write_data(0x18);
            spi_write_data(0x04);
            spi_write_data(0x0F);
            spi_write_data(0x06);
            spi_write_data(0x3a);
            spi_write_data(0x56);
            spi_write_data(0x4d);
            spi_write_data(0x03);
            spi_write_data(0x0a);
            spi_write_data(0x06);
            spi_write_data(0x30);
            spi_write_data(0x3e);
            spi_write_data(0x0f);
            spi_write_command(0XE1);
            spi_write_data(0x0);
            spi_write_data(0x13);
            spi_write_data(0x18);
            spi_write_data(0x01);
            spi_write_data(0x11);
            spi_write_data(0x06);
            spi_write_data(0x38);
            spi_write_data(0x34);
            spi_write_data(0x4d);
            spi_write_data(0x06);
            spi_write_data(0x0d);
            spi_write_data(0x0b);
            spi_write_data(0x31);
            spi_write_data(0x37);
            spi_write_data(0x0f);
            spi_write_command(0X3A); //Set Interface Pixel Format
            spi_write_data(0x55);
            spi_write_command(0x11); //sleep out
            uSec(120000);
            spi_write_command(0x29); //Turn on the LCD display
        }
        //			spi_write_command(TFT_MADCTL);
        switch (Option.DISPLAY_ORIENTATION) {
        case LANDSCAPE:
            spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Landscape);
            break;
        case PORTRAIT:
            spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Portrait);
            break;
        case RLANDSCAPE:
            spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Landscape180);
            break;
        case RPORTRAIT:
            spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Portrait180);
            break;
        }
        break;
    case ILI9481IPS:
        ResetController();
        //3.5IPS ILI9481+CMI
        spi_write_command(0x01); //Soft_rese
        uSec(220000);

        spi_write_command(0x11);
        uSec(280000);

        spi_write_command(0xd0); //Power_Setting
        spi_write_data(0x07);    //07  VC[2:0] Sets the ratio factor of Vci to generate the reference voltages Vci1
        spi_write_data(0x44);    //41  BT[2:0] Sets the Step up factor and output voltage level from the reference voltages Vci1
        spi_write_data(0x1E);    //1f  17   1C  VRH[3:0]: Sets the factor to generate VREG1OUT from VCILVL
        uSec(220000);

        spi_write_command(0xd1); //VCOM Control
        spi_write_data(0x00);    //00
        spi_write_data(0x0C);    //1A   VCM [6:0] is used to set factor to generate VCOMH voltage from the reference voltage VREG1OUT  15    09
        spi_write_data(0x1A);    //1F   VDV[4:0] is used to set the VCOM alternating amplitude in the range of VREG1OUT x 0.70 to VREG1OUT   1F   18

        spi_write_command(0xC5); //Frame Rate
        spi_write_data(0x03);    // 03   02

        spi_write_command(0xd2); //Power_Setting for Normal Mode
        spi_write_data(0x01);    //01
        spi_write_data(0x11);    //11

        spi_write_command(0xE4); //?
        spi_write_data(0xa0);
        spi_write_command(0xf3);
        spi_write_data(0x00);
        spi_write_data(0x2a);

        //1  OK
        spi_write_command(0xc8);
        spi_write_data(0x00);
        spi_write_data(0x26);
        spi_write_data(0x21);
        spi_write_data(0x00);
        spi_write_data(0x00);
        spi_write_data(0x1f);
        spi_write_data(0x65);
        spi_write_data(0x23);
        spi_write_data(0x77);
        spi_write_data(0x00);
        spi_write_data(0x0f);
        spi_write_data(0x00);
        //GAMMA SETTING

        spi_write_command(0xC0); //Panel Driving Setting
        spi_write_data(0x00);    //1//00  REV  SM  GS
        spi_write_data(0x3B);    //2//NL[5:0]: Sets the number of lines to drive the LCD at an interval of 8 lines.
        spi_write_data(0x00);    //3//SCN[6:0]
        spi_write_data(0x02);    //4//PTV: Sets the Vcom output in non-display area drive period
        spi_write_data(0x11);    //5//NDL: Sets the source output level in non-display area.  PTG: Sets the scan mode in non-display area.

        spi_write_command(0xc6); //Interface Control
        spi_write_data(0x83);
        //GAMMA SETTING

        spi_write_command(0xf0); //?
        spi_write_data(0x01);

        spi_write_command(0xE4); //?
        spi_write_data(0xa0);

        spi_write_command(0x3a);
        spi_write_data(0x66);

        uSec(280000);

        switch (Option.DISPLAY_ORIENTATION) {
        case LANDSCAPE:
            spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9481_Landscape);
            break;
        case PORTRAIT:
            spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9481_Portrait);
            break;
        case RLANDSCAPE:
            spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9481_Landscape180);
            break;
        case RPORTRAIT:
            spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9481_Portrait180);
            break;
        }
        spi_write_command(0x2a);
        spi_write_data(0x00);
        spi_write_data(0x00);
        spi_write_data(0x01);
        spi_write_data(0x3F); //3F

        spi_write_command(0x2b);
        spi_write_data(0x00);
        spi_write_data(0x00);
        spi_write_data(0x01);
        spi_write_data(0xDf); //DF

        if (Option.BGR) spi_write_command(0x21);
        spi_write_command(0x29);
        break;
    case ILI9481:
        DisplayHRes = 480;
        DisplayVRes = 320;
        ResetController();
        spi_write_command(0x11);
        uSec(20000);
        spi_write_cd(0xD0, 3, 0x07, 0x42, 0x18);
        spi_write_cd(0xD1, 3, 0x00, 0x07, 0x10);
        spi_write_cd(0xD2, 2, 0x01, 0x02);
        spi_write_cd(0xC0, 5, 0x10, 0x3B, 0x00, 0x02, 0x11);
        //            spi_write_cd(0xC1, 3,0x10, 0x12, 0xC8);
        //            spi_write_cd(0xC5,1,0x01);
        spi_write_cd(0xB3, 4, 0x00, 0x00, 0x00, 0x10);
        spi_write_cd(0xC8, 12, 0x00, 0x32, 0x36, 0x45, 0x06, 0x16, 0x37, 0x75, 0x77, 0x54, 0x0C, 0x00);
        spi_write_cd(0xE0, 15, 0x0f, 0x24, 0x1c, 0x0a, 0x0f, 0x08, 0x43, 0x88, 0x03, 0x0f, 0x10, 0x06, 0x0f, 0x07, 0x00);
        spi_write_cd(0xE1, 15, 0x0F, 0x38, 0x30, 0x09, 0x0f, 0x0f, 0x4e, 0x77, 0x3c, 0x07, 0x10, 0x05, 0x23, 0x1b, 0x00);
        spi_write_cd(0x36, 0x0A);
        spi_write_cd(0x3A, 1, 0x55);
        spi_write_cd(0x2A, 4, 0x00, 0x00, 0x01, 0x3F);
        spi_write_cd(0x2B, 4, 0x00, 0x00, 0x01, 0xE0);
        if (Option.BGR) spi_write_command(0x21);
        uSec(120000);
        spi_write_command(0x29);
        switch (Option.DISPLAY_ORIENTATION) {
        case LANDSCAPE:
            spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Landscape);
            break;
        case PORTRAIT:
            spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Portrait);
            break;
        case RLANDSCAPE:
            spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Landscape180);
            break;
        case RPORTRAIT:
            spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Portrait180);
            break;
        }
        break;
    case SSD1331:
        ResetController();
        spi_write_command(SSD1331_CMD_DISPLAYOFF); // 0xAE
        spi_write_command(SSD1331_CMD_SETREMAP);   // 0xA0
        if (Option.DISPLAY_ORIENTATION == 1)
            spi_write_command(0x72);
        else if (Option.DISPLAY_ORIENTATION == 2)
            spi_write_command(0x63);
        else if (Option.DISPLAY_ORIENTATION == 3)
            spi_write_command(0x60);
        else
            spi_write_command(0x71);
        spi_write_command(SSD1331_CMD_STARTLINE); // 0xA1
        spi_write_command(0x0);
        spi_write_command(SSD1331_CMD_DISPLAYOFFSET); // 0xA2
        spi_write_command(0x0);
        spi_write_command(SSD1331_CMD_NORMALDISPLAY); // 0xA4
        spi_write_command(SSD1331_CMD_SETMULTIPLEX);  // 0xA8
        spi_write_command(0x3F);                      // 0x3F 1/64 duty
        spi_write_command(SSD1331_CMD_SETMASTER);     // 0xAD
        spi_write_command(0x8E);
        spi_write_command(SSD1331_CMD_POWERMODE); // 0xB0
        spi_write_command(0x0B);
        spi_write_command(SSD1331_CMD_PRECHARGE); // 0xB1
        spi_write_command(0x31);
        spi_write_command(SSD1331_CMD_CLOCKDIV);   // 0xB3
        spi_write_command(0xF0);                   // 7:4 = Oscillator Frequency, 3:0 = CLK Div Ratio (A[3:0]+1 = 1..16)
        spi_write_command(SSD1331_CMD_PRECHARGEA); // 0x8A
        spi_write_command(0x64);
        spi_write_command(SSD1331_CMD_PRECHARGEB); // 0x8B
        spi_write_command(0x78);
        spi_write_command(SSD1331_CMD_PRECHARGEA); // 0x8C
        spi_write_command(0x64);
        spi_write_command(SSD1331_CMD_PRECHARGELEVEL); // 0xBB
        spi_write_command(0x3A);
        spi_write_command(SSD1331_CMD_VCOMH); // 0xBE
        spi_write_command(0x3E);
        spi_write_command(SSD1331_CMD_MASTERCURRENT); // 0x87
        spi_write_command(0x06);
        spi_write_command(SSD1331_CMD_CONTRASTA); // 0x81
        spi_write_command(0x91);
        spi_write_command(SSD1331_CMD_CONTRASTB); // 0x82
        spi_write_command(0x50);
        spi_write_command(SSD1331_CMD_CONTRASTC); // 0x83
        spi_write_command(0x7D);
        spi_write_command(SSD1331_CMD_DISPLAYON); //--turn on oled panel
        break;
    case ILI9341:
    case ILI9341BUFF:
        ResetController();
        spi_write_command(ILI9341_SOFTRESET); //software reset
        uSec(20000);
        spi_write_command(ILI9341_DISPLAYOFF);
        spi_write_cd(ILI9341_POWERCONTROL1, 1, 0x23);
        spi_write_cd(ILI9341_POWERCONTROL2, 1, 0x10);
        spi_write_cd(ILI9341_VCOMCONTROL1, 2, 0x2B, 0x2B);
        spi_write_cd(ILI9341_VCOMCONTROL2, 1, 0xC0);
        spi_write_cd(ILI9341_PIXELFORMAT, 1, 0x55);
        spi_write_cd(ILI9341_FRAMECONTROL, 2, 0x00, 0x1B);
        spi_write_cd(ILI9341_ENTRYMODE, 1, 0x07);
        spi_write_cd(ILI9341_SLEEPOUT, 1, 0);
        uSec(50000);
        spi_write_command(ILI9341_NORMALDISP);
        if (Option.BGR) spi_write_command(ILI9341_INVERTON);
        spi_write_command(ILI9341_DISPLAYON);
        uSec(100000);
        switch (Option.DISPLAY_ORIENTATION) {
        case LANDSCAPE:
            spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Landscape);
            break;
        case PORTRAIT:
            spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Portrait);
            break;
        case RLANDSCAPE:
            spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Landscape180);
            break;
        case RPORTRAIT:
            spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Portrait180);
            break;
        }
        break;

    case GC9A01:
        ResetController();
        spi_write_command(0xEF);
        spi_write_cd(0xEB, 1, 0x14);
        spi_write_command(0xFE);
        spi_write_command(0xEF);
        spi_write_cd(0xEB, 1, 0x14);
        spi_write_cd(0x84, 1, 0x40);
        spi_write_cd(0x85, 1, 0xFF);
        spi_write_cd(0x86, 1, 0xFF);
        spi_write_cd(0x87, 1, 0xFF);
        spi_write_cd(0x88, 1, 0x0A);
        spi_write_cd(0x89, 1, 0x21);
        spi_write_cd(0x8A, 1, 0x00);
        spi_write_cd(0x8B, 1, 0x80);
        spi_write_cd(0x8C, 1, 0x01);
        spi_write_cd(0x8D, 1, 0x01);
        spi_write_cd(0x8E, 1, 0xFF);
        spi_write_cd(0x8F, 1, 0xFF);
        spi_write_cd(0xB6, 2, 0x00, 0x20);
        spi_write_cd(0x3A, 1, 0x05);
        spi_write_cd(0x90, 4, 0x08, 0x08, 0x08, 0x08);
        spi_write_cd(0xBD, 1, 0x06);
        spi_write_cd(0xBC, 1, 0x00);
        spi_write_cd(0xFF, 3, 0x60, 0x01, 0x04);
        spi_write_cd(0xC3, 1, 0x13);
        spi_write_cd(0xC4, 1, 0x13);
        spi_write_cd(0xC9, 1, 0x22);
        spi_write_cd(0xBE, 1, 0x11);
        spi_write_cd(0xE1, 2, 0x10, 0x0E);
        spi_write_cd(0xDF, 3, 0x21, 0x0c, 0x02);
        spi_write_cd(0xF0, 6, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A);
        spi_write_cd(0xF1, 6, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F);
        spi_write_cd(0xF2, 6, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A);
        spi_write_cd(0xF3, 6, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F);
        spi_write_cd(0xED, 2, 0x1B, 0x0B);
        spi_write_cd(0xAE, 1, 0x77);
        spi_write_cd(0xCD, 1, 0x63);
        spi_write_cd(0x70, 9, 0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03);
        spi_write_cd(0xE8, 1, 0x34);
        spi_write_cd(0x62, 12, 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70);
        spi_write_cd(0x63, 12, 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70);
        spi_write_cd(0x64, 7, 0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07);
        spi_write_cd(0x66, 10, 0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00);
        spi_write_cd(0x67, 10, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98);
        spi_write_cd(0x74, 7, 0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00);
        spi_write_cd(0x98, 2, 0x3e, 0x07);
        spi_write_command(0x35);
        spi_write_command(GC9A01_SLPOUT);
        uSec(10000);
        spi_write_command(GC9A01_DISPON);
        switch (Option.DISPLAY_ORIENTATION) {
        case LANDSCAPE:
            spi_write_cd(GC9A01_MADCTL, 1, 0x08);
            break;
        case PORTRAIT:
            spi_write_cd(GC9A01_MADCTL, 1, 0x68);
            break;
        case RLANDSCAPE:
            spi_write_cd(GC9A01_MADCTL, 1, 0xc8);
            break;
        case RPORTRAIT:
            spi_write_cd(GC9A01_MADCTL, 1, 0xa8);
            break;
        }
        break;
    case ILI9163:
        ResetController();
        spi_write_command(ILI9341_SOFTRESET); //software reset
        uSec(20000);
        spi_write_command(ILI9163_SLPOUT); //exit sleep
        uSec(5000);
        spi_write_cd(ILI9163_PIXFMT, 1, 0x05);
        uSec(5000);
        spi_write_cd(ILI9163_GAMMASET, 1, 0x04); //0x04
        uSec(1000);
        spi_write_cd(ILI9163_GAMRSEL, 1, 0x01);
        uSec(1000);
        if (Option.BGR) spi_write_command(ILI9163_DINVON);
        spi_write_command(ILI9163_NORML);
        spi_write_cd(ILI9163_DFUNCTR, 2, 0b11111111, 0b00000110);                                                                    //
        spi_write_cd(ILI9163_PGAMMAC, 15, 0x36, 0x29, 0x12, 0x22, 0x1C, 0x15, 0x42, 0xB7, 0x2F, 0x13, 0x12, 0x0A, 0x11, 0x0B, 0x06); //Positive Gamma Correction Setting
        spi_write_cd(ILI9163_NGAMMAC, 15, 0x09, 0x16, 0x2D, 0x0D, 0x13, 0x15, 0x40, 0x48, 0x53, 0x0C, 0x1D, 0x25, 0x2E, 0x34, 0x39); //Negative Gamma Correction Setting
        spi_write_cd(ILI9163_FRMCTR1, 2, 0x08, 0x02);                                                                                //0x0C//0x08
        uSec(1000);
        spi_write_cd(ILI9163_DINVCTR, 1, 0x07);
        uSec(1000);
        spi_write_cd(ILI9163_PWCTR1, 2, 0x0A, 0x02); //4.30 - 0x0A
        uSec(1000);
        spi_write_cd(ILI9163_PWCTR2, 1, 0x02);
        uSec(1000);
        spi_write_cd(ILI9163_VCOMCTR1, 2, 0x50, 99); //0x50
        uSec(1000);
        spi_write_cd(ILI9163_VCOMOFFS, 1, 0); //0x40
        uSec(1000);
        spi_write_cd(ILI9163_VSCLLDEF, 5, 0, 0, DisplayVRes, 0, 0);
        spi_write_command(ILI9163_DISPON); //display ON
        uSec(1000);
        switch (Option.DISPLAY_ORIENTATION) {
        case LANDSCAPE:
            spi_write_cd(ILI9163_MADCTL, 1, ILI9163_Landscape);
            break;
        case PORTRAIT:
            spi_write_cd(ILI9163_MADCTL, 1, ILI9163_Portrait);
            break;
        case RLANDSCAPE:
            spi_write_cd(ILI9163_MADCTL, 1, ILI9163_Landscape180);
            break;
        case RPORTRAIT:
            spi_write_cd(ILI9163_MADCTL, 1, ILI9163_Portrait180);
            break;
        }
        uSec(1000);
        break;
    case ST7735:
    case ST7735S:
    case ST7735S_W:
        ResetController();
        spi_write_command(ILI9341_SOFTRESET); //software reset
        uSec(20000);
        spi_write_command(ST7735_SLPOUT); //out of sleep mode
        uSec(500000);
        spi_write_cd(ST7735_FRMCTR1, 3, 0x01, 0x2C, 0x2d);                   //frame rate control - normal mode
        spi_write_cd(ST7735_FRMCTR2, 3, 0x01, 0x2C, 0x2D);                   //frame rate control - idle mode
        spi_write_cd(ST7735_FRMCTR3, 6, 0x01, 0x2c, 0x2D, 0x01, 0x2C, 0x2D); //frame rate control - partial mode
        spi_write_cd(ST7735_INVCTR, 1, 0x07);                                //display inversion control
        spi_write_cd(ST7735_PWCTR1, 3, 0xA2, 0x02, 0x84);                    //power control
        spi_write_cd(ST7735_PWCTR2, 1, 0xC5);                                //power control
        spi_write_cd(ST7735_PWCTR3, 2, 0x0A, 0x00);                          //power control
        spi_write_cd(ST7735_PWCTR4, 2, 0x8A, 0x2A);                          //power control
        spi_write_cd(ST7735_PWCTR5, 2, 0x8A, 0xEE);                          //power control
        spi_write_cd(ST7735_VMCTR1, 1, 0x0E);                                //power control
        if (Option.DISPLAY_TYPE == ST7735 || Option.DISPLAY_TYPE == ST7735S_W)
            Option.BGR ? spi_write_command(ST7735_INVON) : spi_write_command(ST7735_INVOFF); //don't invert display
        else
            Option.BGR ? spi_write_command(ST7735_INVOFF) : spi_write_command(ST7735_INVON);
        spi_write_cd(ST7735_COLMOD, 1, 0x05);         //set color mode
        spi_write_cd(ST7735_CASET, 4, 0, 0, 0, 0x7F); //column addr set
        spi_write_cd(ST7735_RASET, 4, 0, 0, 0, 0x9F); //row addr set
        spi_write_cd(ST7735_GMCTRP1, 16, 0x02, 0x1c, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D, 0x25, 0x29, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10);
        spi_write_cd(ST7735_GMCTRN1, 16, 0x03, 0x1d, 0x07, 0x06, 0x2E, 0x2c, 0x29, 0x2d, 0x2E, 0x2E, 0x37, 0x3f, 0x00, 0x00, 0x02, 0x10);
        spi_write_command(ST7735_NORON); //normal display on
        uSec(10000);
        spi_write_command(ST7735_DISPON);
        switch (Option.DISPLAY_ORIENTATION) {
        case LANDSCAPE:
            spi_write_cd(ST7735_MADCTL, 1, ST7735_Landscape | (Option.DISPLAY_TYPE == ST7735 ? 0 : 8));
            break;
        case PORTRAIT:
            spi_write_cd(ST7735_MADCTL, 1, ST7735_Portrait | (Option.DISPLAY_TYPE == ST7735 ? 0 : 8));
            break;
        case RLANDSCAPE:
            spi_write_cd(ST7735_MADCTL, 1, ST7735_Landscape180 | (Option.DISPLAY_TYPE == ST7735 ? 0 : 8));
            break;
        case RPORTRAIT:
            spi_write_cd(ST7735_MADCTL, 1, ST7735_Portrait180 | (Option.DISPLAY_TYPE == ST7735 ? 0 : 8));
            break;
        }
        break;
    case ST7789:
    case ST7789A:
    case ST7789B:
    case ST7789C:
        ResetController();
        spi_write_command(ST77XX_SWRESET);
        uSec(150000);
        spi_write_command(ST77XX_SLPOUT);
        uSec(500000);
        spi_write_command(ST77XX_COLMOD);
        spi_write_data(0x55);
        uSec(10000);
        //            if(Option.DISPLAY_TYPE==ST7789){spi_write_command(ST77XX_CASET); spi_write_data(0x0); spi_write_data(0x0); spi_write_data(0x0); spi_write_data(239);}
        //			else if(Option.DISPLAY_ORIENTATION & 1){spi_write_command(ST77XX_CASET); spi_write_data(0x0); spi_write_data(40); spi_write_data(0x1); spi_write_data(23);}
        //				 else {spi_write_command(ST77XX_CASET); spi_write_data(0x0); spi_write_data(52); spi_write_data(0x0); spi_write_data(186);}
        //            if(Option.DISPLAY_TYPE==ST7789){spi_write_command(ST77XX_RASET); spi_write_data(0x0); spi_write_data(0); spi_write_data(0); spi_write_data(239);}
        //			else if(Option.DISPLAY_ORIENTATION & 1){spi_write_command(ST77XX_RASET); spi_write_data(0x0); spi_write_data(53); spi_write_data(0); spi_write_data(187);}
        //				 else {spi_write_command(ST77XX_RASET); spi_write_data(0x0); spi_write_data(40); spi_write_data(1); spi_write_data(23);}
        if (Option.BGR)
            spi_write_command(ST77XX_INVOFF);
        else
            spi_write_command(ST77XX_INVON);
        uSec(10000);
        spi_write_command(ST77XX_NORON);
        uSec(10000);
        spi_write_command(ST77XX_DISPON);
        uSec(500000);
        switch (Option.DISPLAY_ORIENTATION) {
        case LANDSCAPE:
            spi_write_cd(ST7735_MADCTL, 1, ST7735_Landscape);
            break;
        case PORTRAIT:
            spi_write_cd(ST7735_MADCTL, 1, ST7735_Portrait);
            break;
        case RLANDSCAPE:
            spi_write_cd(ST7735_MADCTL, 1, ST7735_Landscape180);
            break;
        case RPORTRAIT:
            spi_write_cd(ST7735_MADCTL, 1, ST7735_Portrait180);
            break;
        }
        break;
    case N5110:
        ResetController();
        spi_write_command(0x21); // LCD Extended Commands.
        uSec(20000);
        spi_write_command(Option.LCDVOP); // Set LCD Vop (Contrast). //0xB0 for 5V, 0XB1 for 3.3v, 0XBF if screen too dark
        uSec(20000);
        spi_write_command(0x04); // Set Temp coefficient. //0x04
        uSec(20000);
        spi_write_command(0x14); // LCD bias mode 1:48. //0x13 or 0X14
        uSec(20000);
        spi_write_command(0x20); //We must send 0x20 before modifying the display control mode
        uSec(20000);
        spi_write_command(0x0C); // Set display control, normal mode. 0x0D for inverse, 0x0C for normal
        uSec(20000);
        break;
    case SSD1306SPI:
        ResetController();
        spi_write_command(0xAE); //DISPLAYOFF
        spi_write_command(0xD5); //DISPLAYCLOCKDIV
        spi_write_command(0x80); //the suggested ratio &H80
        spi_write_command(0xA8); //MULTIPLEX
        spi_write_command(0x3F); //
        spi_write_command(0xD3); //DISPLAYOFFSET
        spi_write_command(0x0);  //no offset
        spi_write_command(0x40); //STARTLINE
        spi_write_command(0x8D); //CHARGEPUMP
        spi_write_command(0x14);
        spi_write_command(0x20); //MEMORYMODE
        spi_write_command(0x00); //&H0 act like ks0108
        spi_write_command(0xA1); //SEGREMAP OR 1
        spi_write_command(0xC8); //COMSCANDEC
        spi_write_command(0xDA); //COMPINS
        spi_write_command(0x12);
        spi_write_command(0x81); //SETCONTRAST
        spi_write_command(0xCF);
        spi_write_command(0xd9); //SETPRECHARGE
        spi_write_command(0xF1);
        spi_write_command(0xDB); //VCOMDETECT
        spi_write_command(0x40);
        spi_write_command(0xA4); //DISPLAYALLON_RESUME
        spi_write_command(0xA6); //NORMALDISPLAY
        spi_write_command(0xAF); //DISPLAYON
        break;
    }
    return 1;
}

void DefineRegionSPI(int xstart, int ystart, int xend, int yend, int rw) {
    unsigned char coord[4];
    if (Option.DISPLAY_TYPE == ILI9481 || Option.DISPLAY_TYPE == ILI9488W || Option.DISPLAY_TYPE == ILI9488WBUFF) {
        hal_spi_lcd_bus_begin();
        hal_spi_lcd_bus_dc(0);
        SPIsend2(ILI9341_COLADDRSET);
        hal_spi_lcd_bus_dc(1);
        SPIsend2(xstart >> 8);
        SPIsend2(xstart);
        SPIsend2(xend >> 8);
        SPIsend2(xend);
        hal_spi_lcd_bus_dc(0);
        SPIsend2(ILI9341_PAGEADDRSET);
        hal_spi_lcd_bus_dc(1);
        SPIsend2(ystart >> 8);
        SPIsend2(ystart);
        SPIsend2(yend >> 8);
        SPIsend2(yend);
        hal_spi_lcd_bus_dc(0);
        if (rw) {
            SPIsend2(ILI9341_MEMORYWRITE);
        } else {
            SPIsend2(ILI9341_RAMRD);
        }
        hal_spi_lcd_bus_dc(1);
    } else if (Option.DISPLAY_TYPE == SSD1331) {
        if (Option.DISPLAY_ORIENTATION & 1) {
            spi_write_command(0x15); // Column addr set
            spi_write_command(xstart);
            spi_write_command(xend);

            spi_write_command(0x75); // Row addr set
            spi_write_command(ystart);
            spi_write_command(yend);
        } else {
            spi_write_command(0x75); // Row addr set
            spi_write_command(xstart);
            spi_write_command(xend);

            spi_write_command(0x15); // Column addr set
            spi_write_command(ystart);
            spi_write_command(yend);
        }
        hal_spi_lcd_bus_begin();
        hal_spi_lcd_bus_dc(1);
    } else {
        if (Option.DISPLAY_TYPE == 0) error("Display not configured");
        if (Option.DISPLAY_TYPE == ST7789) {
            if (Option.DISPLAY_ORIENTATION == 2) {
                ystart += 80;
                yend += 80;
            }
            if (Option.DISPLAY_ORIENTATION == 1) {
                xstart += 80;
                xend += 80;
            }
        }
        if (Option.DISPLAY_TYPE == ST7789A) {
            if (Option.DISPLAY_ORIENTATION == 1) {
                xstart += 40;
                xend += 40;
                ystart += 52;
                yend += 52;
            } else if (Option.DISPLAY_ORIENTATION == 3) {
                xstart += 40;
                xend += 40;
                ystart += 53;
                yend += 53;
            } else if (Option.DISPLAY_ORIENTATION == 0) {
                ystart += 40;
                yend += 40;
                xstart += 52;
                xend += 52;
            } else if (Option.DISPLAY_ORIENTATION == 2) {
                ystart += 40;
                yend += 40;
                xstart += 53;
                xend += 53;
            }
        }
        if (Option.DISPLAY_TYPE == ST7735S) {
            if (Option.DISPLAY_ORIENTATION & 1) {
                ystart += 26;
                yend += 26;
                xstart++;
                xend++;
            } else {
                xstart += 26;
                xend += 26;
                ystart++;
                yend++;
            }
        }
        if (Option.DISPLAY_TYPE == ST7735S_W) {
            switch (Option.DISPLAY_ORIENTATION) {
            case LANDSCAPE:
                ystart += 2;
                yend += 2;
                xstart += 3;
                xend += 3;
                break;
            case PORTRAIT:
                xstart += 2;
                xend += 2;
                ystart += 3;
                yend += 3;
                break;
            case RLANDSCAPE:
                ystart += 2;
                yend += 2;
                xstart += 1;
                xend += 1;
                break;
            case RPORTRAIT:
                xstart += 2;
                xend += 2;
                ystart += 1;
                yend += 1;
                break;
            }
        }
        hal_spi_lcd_bus_begin();
        hal_spi_lcd_bus_dc(0);
        SPIsend(ILI9341_COLADDRSET);
        hal_spi_lcd_bus_dc(1);
        coord[0] = xstart >> 8;
        coord[1] = xstart;
        coord[2] = xend >> 8;
        coord[3] = xend;
        hal_spi_lcd_bus_write(coord, 4); //		HAL_SPI_Transmit(&hspi3,coord,4,500);
        hal_spi_lcd_bus_dc(0);
        SPIsend(ILI9341_PAGEADDRSET);
        hal_spi_lcd_bus_dc(1);
        coord[0] = ystart >> 8;
        coord[1] = ystart;
        coord[2] = yend >> 8;
        coord[3] = yend;
        hal_spi_lcd_bus_write(coord, 4); //		HAL_SPI_Transmit(&hspi3,coord,4,500);
        hal_spi_lcd_bus_dc(0);
        if (rw) {
            SPIsend(ILI9341_MEMORYWRITE);
        } else {
            SPIsend(ILI9341_RAMRD);
        }
        hal_spi_lcd_bus_dc(1);
    }
}

#endif /* !HAL_PORT_IS_VGA */
