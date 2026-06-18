/***********************************************************************************************************************
PicoMite MMBasic

SPI-LCD.c

<COPYRIGHT HOLDERS>  Geoff Graham, Peter Mather
Copyright (c) 2021, <COPYRIGHT HOLDERS> All rights reserved.
Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
1.	Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
2.	Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
    in the documentation and/or other materials provided with the distribution.
3.	The name MMBasic be used when referring to the interpreter in any documentation and promotional material and the original copyright message be displayed
    on the console at startup (additional copyright messages may be added).
4.	All advertising materials mentioning features or use of this software must display the following acknowledgement: This product includes software developed
    by the <copyright holder>.
5.	Neither the name of the <copyright holder> nor the names of its contributors may be used to endorse or promote products derived from this software
    without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDERS> AS IS AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDERS> BE LIABLE FOR ANY DIRECT,
INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

************************************************************************************************************************/

#include <stdarg.h>
#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "hal/hal_spi_lcd_mem332.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/multicore.h"
int CurrentSPIDevice = NONE_SPI_DEVICE;
void __not_in_flash_func(spi_write_fast)(spi_inst_t * spi, const uint8_t * src, size_t len) {
    // Write to TX FIFO whilst ignoring RX, then clean up afterward. When RX
    // is full, PL022 inhibits RX pushes, and sets a sticky flag on
    // push-on-full, but continues shifting. Safe if SSPIMSC_RORIM is not set.
    for (size_t i = 0; i < len; ++i) {
        while (!spi_is_writable(spi)) tight_loop_contents();
        spi_get_hw(spi)->dr = (uint32_t)src[i];
    }
}
void __not_in_flash_func(spi_finish)(spi_inst_t * spi) {
    // Drain RX FIFO, then wait for shifting to finish (which may be *after*
    // TX FIFO drains), then drain RX FIFO again
    while (spi_is_readable(spi))
        (void)spi_get_hw(spi)->dr;
    while (spi_get_hw(spi)->sr & SPI_SSPSR_BSY_BITS)
        tight_loop_contents();
    while (spi_is_readable(spi))
        (void)spi_get_hw(spi)->dr;

    // Don't leave overrun flag set
    spi_get_hw(spi)->icr = SPI_SSPICR_RORIC_BITS;
}
#if !HAL_PORT_IS_VGA
int LCD_CS_PIN = 0;
int LCD_CD_PIN = 0;
int LCD_Reset_PIN = 0;
unsigned char LCDBuffer[1440] = {0};

void DefineRegionSPI(int xstart, int ystart, int xend, int yend, int rw);
void DrawBitmapSPI(int x1, int y1, int width, int height, int scale, int fc, int bc, unsigned char * bitmap);
extern const int SPISpeeds[];
extern void spi_write_command(unsigned char command);
extern void I2C_Send_Data(unsigned char * data, int n);
void I2C_Send_Command(char command);
extern int mmI2Cvalue; // value of MM.I2C
void waitwhilebusy(void);
/* All LCD bus writes go through lcd_xmit_byte_multi; on non-NEXTGEN
 * ports it's initialized to share the system SPI sender (set in
 * SPISpeedSet alongside xmit_byte_multi). NEXTGEN ports may
 * override it to a separate sender when the LCD has its own clock. */
#define SPIsend(a)                  \
    {                               \
        uint8_t b = a;              \
        lcd_xmit_byte_multi(&b, 1); \
    }
#define SPIqueue(a)                                                                                                                                                       \
    {                                                                                                                                                                     \
        (Option.DISPLAY_TYPE == ILI9488 || Option.DISPLAY_TYPE == ILI9488P || Option.DISPLAY_TYPE == ILI9481IPS) ? lcd_xmit_byte_multi(a, 3) : lcd_xmit_byte_multi(a, 2); \
    }
#define SPIsend2(a) \
    {               \
        SPIsend(0); \
        SPIsend(a); \
    }
int PackHorizontal = 0;
int fullrefreshcount = 0;
void DrawRectangleMEM(int x1, int y1, int x2, int y2, int c);
void DrawBitmapMEM(int x1, int y1, int width, int height, int scale, int fc, int bc, unsigned char * bitmap);
void DrawBufferMEM(int x1, int y1, int x2, int y2, unsigned char * p);
void ReadBufferMEM(int x1, int y1, int x2, int y2, unsigned char * buff);
void spi_write_CommandData(const uint8_t * pCommandData, uint8_t datalen);
void ST7920command(unsigned char data);
// utility function for routines that want to reserve a pin for special I/O
// this ignores any previous settings and forces the pin to its new state
// pin is the pin number
// inp is true if an input or false if an output
// init is the value used to initialise the pin if it is an output (hi or lo)
// type is the final tag for the pin in ExtCurrentConfig[]
void SetAndReserve(int pin, int inp, int init, int type) {
    if (pin == 0) return; // do nothing if not set
}

void MIPS16 ConfigDisplaySPI(unsigned char * p) {
    char code, CD, RESET, CS = 0;
    uint8_t BACKLIGHT = 0;
    int DISPLAY_TYPE = 0;
    int orientation = 1;
    getargs(&p, 13, (unsigned char *)",");
    if (checkstring(argv[0], (unsigned char *)"ILI9163")) {
        DISPLAY_TYPE = ILI9163;
    } else if (checkstring(argv[0], (unsigned char *)"SSD1331")) {
        DISPLAY_TYPE = SSD1331;
    } else if (checkstring(argv[0], (unsigned char *)"ST7735S")) {
        DISPLAY_TYPE = ST7735S;
    } else if (checkstring(argv[0], (unsigned char *)"ST7735")) {
        DISPLAY_TYPE = ST7735;
    } else if (checkstring(argv[0], (unsigned char *)"ST7789")) {
        DISPLAY_TYPE = ST7789;
    } else if (checkstring(argv[0], (unsigned char *)"ST7789_135")) {
        DISPLAY_TYPE = ST7789A;
    } else if (checkstring(argv[0], (unsigned char *)"ST7789_320")) {
        DISPLAY_TYPE = ST7789B;
    } else if (checkstring(argv[0], (unsigned char *)"ILI9481IPS")) {
        DISPLAY_TYPE = ILI9481IPS;
    } else if (checkstring(argv[0], (unsigned char *)"ILI9481")) {
        DISPLAY_TYPE = ILI9481;
    } else if (checkstring(argv[0], (unsigned char *)"ILI9488")) {
        DISPLAY_TYPE = ILI9488;
    } else if (checkstring(argv[0], (unsigned char *)"ILI9488P")) {
        DISPLAY_TYPE = ILI9488P;
    } else if (checkstring(argv[0], (unsigned char *)"ILI9488W")) {
        DISPLAY_TYPE = ILI9488W;
    } else if (checkstring(argv[0], (unsigned char *)"ST7796S")) {
        DISPLAY_TYPE = ST7796S;
    } else if (checkstring(argv[0], (unsigned char *)"ST7796SP")) {
        DISPLAY_TYPE = ST7796SP;
    } else if (checkstring(argv[0], (unsigned char *)"ILI9341")) {
        DISPLAY_TYPE = ILI9341;
    } else if (checkstring(argv[0], (unsigned char *)"ST7735S_W")) {
        DISPLAY_TYPE = ST7735S_W;
    } else if (checkstring(argv[0], (unsigned char *)"GC9A01")) {
        DISPLAY_TYPE = GC9A01;
    } else if (checkstring(argv[0], (unsigned char *)"N5110")) {
        DISPLAY_TYPE = N5110;
    } else if (checkstring(argv[0], (unsigned char *)"SSD1306SPI")) {
        DISPLAY_TYPE = SSD1306SPI;
    } else if (checkstring(argv[0], (unsigned char *)"ST7920")) {
        DISPLAY_TYPE = ST7920;
    } else {
        /* MEM332 buffered family — real matcher in spi_lcd_mem332.c
         * (linked on rp2350 PicoMite); stub returns 0 on every other
         * port and the "unknown display" return below fires. */
        int mt = hal_spi_lcd_mem332_match_option(argv[0]);
        if (mt)
            DISPLAY_TYPE = mt;
        else
            return;
    }
    /* Option.LCD_CLK is a flat field in struct option_s on every port
     * (P1); on non-MEM332 ports it's always 0, so the OR with
     * SYSTEM_CLK reduces to the SYSTEM_CLK-only check. */
    if (!(Option.SYSTEM_CLK || Option.LCD_CLK)) error("SPI not configured");
    if (!(argc == 7 || argc == 9 || argc == 11 || argc == 13)) error("Argument count");
    if (*argv[2]) {
        if (checkstring(argv[2], (unsigned char *)"L") || checkstring(argv[2], (unsigned char *)"LANDSCAPE"))
            orientation = LANDSCAPE;
        else if (checkstring(argv[2], (unsigned char *)"P") || checkstring(argv[2], (unsigned char *)"PORTRAIT"))
            orientation = PORTRAIT;
        else if (checkstring(argv[2], (unsigned char *)"RL") || checkstring(argv[2], (unsigned char *)"RLANDSCAPE"))
            orientation = RLANDSCAPE;
        else if (checkstring(argv[2], (unsigned char *)"RP") || checkstring(argv[2], (unsigned char *)"RPORTRAIT"))
            orientation = RPORTRAIT;
        else
            error("Orientation");
    }
    /* DISPLAY_TYPE never reaches NEXTGEN on non-MEM332 ports (the
	 * matcher above doesn't accept those names there), so the check
	 * is a no-op there. On MEM332 ports it enforces a dedicated LCD
	 * SPI channel. */
    if (DISPLAY_TYPE >= NEXTGEN && Option.LCD_CLK == Option.SYSTEM_CLK) error("Buffered drivers need a dedicated SPI channel");
    Option.DISPLAY_ORIENTATION = orientation;
    if (DISPLAY_TYPE == ST7789 || DISPLAY_TYPE == ST7789A || DISPLAY_TYPE == ST7789A) Option.DISPLAY_ORIENTATION = (Option.DISPLAY_ORIENTATION + 2) % 4;
    if (!(code = codecheck(argv[4]))) argv[4] += 2;
    CD = getinteger(argv[4]);
    if (!code) CD = codemap(CD);
    if (!(code = codecheck(argv[6]))) argv[6] += 2;
    RESET = getinteger(argv[6]);
    if (!code) RESET = codemap(RESET);
    if (DISPLAY_TYPE != ST7920) {
        if (!(code = codecheck(argv[8]))) argv[8] += 2;
        CS = getinteger(argv[8]);
        if (!code) CS = codemap(CS);
        Option.LCDVOP = 0xB1;
        Option.I2Coffset = 0;
        if (argc >= 11 && *argv[10]) {
            if (DISPLAY_TYPE == N5110)
                Option.LCDVOP = getint(argv[10], 0, 255);
            else if (DISPLAY_TYPE == SSD1306SPI)
                Option.I2Coffset = getint(argv[10], 0, 10);
            else {
                if (!(code = codecheck(argv[10]))) argv[10] += 2;
                BACKLIGHT = getinteger(argv[10]);
                if (!code) BACKLIGHT = codemap(BACKLIGHT);
                CheckPin(BACKLIGHT, CP_IGNORE_INUSE);
                if ((PinDef[BACKLIGHT].slice & 0x7f) == Option.AUDIO_SLICE) error("Channel in use for Audio");
            }
        }
        CheckPin(CS, CP_IGNORE_INUSE);
        Option.LCD_CS = CS;
        if (argc == 13) {
            if (checkstring(argv[12], (unsigned char *)"INVERT")) Option.BGR = 1;
        } else
            Option.BGR = 0;
    }
    CheckPin(CD, CP_IGNORE_INUSE);
    CheckPin(RESET, CP_IGNORE_INUSE);
    if (CS == CD || CS == RESET || (CS == BACKLIGHT && DISPLAY_TYPE != ST7920) || CD == RESET || CD == BACKLIGHT || RESET == BACKLIGHT) error("Duplicated pin");
    Option.LCD_CD = CD;
    Option.LCD_Reset = RESET;
    Option.DISPLAY_BL = BACKLIGHT;
    Option.DISPLAY_TYPE = DISPLAY_TYPE;
    if (!(Option.DISPLAY_TYPE > I2C_PANEL && Option.DISPLAY_TYPE < BufferedPanel)) Option.Refresh = 1;
}

// initialise the display controller
// this is used in the initial boot sequence of the Micromite
void MIPS16 InitDisplaySPI(int InitOnly) {
    /* The `< NEXTGEN` term is meaningful only on MEM332 ports (where
     * DISPLAY_TYPE may reach NEXTGEN values that should NOT bail).
     * On non-MEM332 ports DISPLAY_TYPE never reaches NEXTGEN so the
     * extra term is always-true and reduces to the simple bail. */
    if (Option.DISPLAY_TYPE == 0 || (Option.DISPLAY_TYPE >= DISP_USER && Option.DISPLAY_TYPE < NEXTGEN) || Option.DISPLAY_TYPE <= I2C_PANEL) return;
    DisplayHRes = display_details[Option.DISPLAY_TYPE].horizontal;
    DisplayVRes = display_details[Option.DISPLAY_TYPE].vertical;
    if (!InitOnly) {
        //        SPI2on();
        // open the SPI port and reserve the I/O pins
        /* MEM332 ports may have a separate LCD SPI clock pin
		 * (Option.LCD_CLK) distinct from the system SPI clock. Other
		 * ports always have LCD_CLK == 0, making this branch dead. */
        if (Option.SYSTEM_CLK != Option.LCD_CLK) {
            gpio_set_function(LCD_CLK_PIN, GPIO_FUNC_SPI);
            gpio_set_function(LCD_MOSI_PIN, GPIO_FUNC_SPI);
            gpio_set_function(LCD_MISO_PIN, GPIO_FUNC_SPI);
            gpio_set_drive_strength(LCD_MOSI_PIN, GPIO_DRIVE_STRENGTH_8MA);
            gpio_set_drive_strength(LCD_CLK_PIN, GPIO_DRIVE_STRENGTH_8MA);
            gpio_set_input_hysteresis_enabled(LCD_MISO_PIN, true);
            if (PinDef[Option.LCD_CLK].mode & SPI0SCK) {
                spi_init(spi0, display_details[Option.DISPLAY_TYPE].speed);
                spi_set_format(spi0, 8, display_details[Option.DISPLAY_TYPE].CPOL, display_details[Option.DISPLAY_TYPE].CPHASE, SPI_MSB_FIRST);
                lcd_xmit_byte_multi = HW0SendSPI;
                lcd_rcvr_byte_multi = HW0ReadSPI;
            } else {
                spi_init(spi1, display_details[Option.DISPLAY_TYPE].speed);
                spi_set_format(spi1, 8, display_details[Option.DISPLAY_TYPE].CPOL, display_details[Option.DISPLAY_TYPE].CPHASE, SPI_MSB_FIRST);
                lcd_xmit_byte_multi = HW1SendSPI;
                lcd_rcvr_byte_multi = HW1ReadSPI;
            }
        }
        // setup the pointers to the drawing primitives
        if (Option.DISPLAY_TYPE > I2C_PANEL && Option.DISPLAY_TYPE < BufferedPanel) {
            gui_colour_depth = 16; /* RGB565 SPI panels */
            if (Option.DISPLAY_ORIENTATION == PORTRAIT) {
                DrawRectangle = DrawRectangleSPISCR;
                DrawBitmap = DrawBitmapSPISCR;
                DrawBuffer = DrawBufferSPISCR;
                DrawPixel = DrawPixelNormal;
                ScrollLCD = ScrollLCDSPISCR;
                DrawBLITBuffer = DrawBufferSPISCR;
                if (Option.DISPLAY_TYPE == ILI9341 || Option.DISPLAY_TYPE == ST7796SP || Option.DISPLAY_TYPE == ST7796S || Option.DISPLAY_TYPE == ILI9488 || Option.DISPLAY_TYPE == ILI9488P || Option.DISPLAY_TYPE == ST7789B) {
                    ReadBuffer = ReadBufferSPISCR;
                    ReadBLITBuffer = ReadBufferSPISCR;
                }
            } else {
                DrawRectangle = DrawRectangleSPI;
                DrawBitmap = DrawBitmapSPI;
                DrawBuffer = DrawBufferSPI;
                DrawBLITBuffer = DrawBufferSPI;
                DrawPixel = DrawPixelNormal;
                if (Option.DISPLAY_TYPE == ILI9341 || Option.DISPLAY_TYPE == ST7796SP || Option.DISPLAY_TYPE == ST7796S || Option.DISPLAY_TYPE == ILI9488 || Option.DISPLAY_TYPE == ILI9488P || Option.DISPLAY_TYPE == ST7789B) {
                    ReadBLITBuffer = ReadBufferSPI;
                    ReadBuffer = ReadBufferSPI;
                    ScrollLCD = ScrollLCDSPI;
                }
            }
        } else if (Option.DISPLAY_TYPE >= NEXTGEN) {
            /* MEM332 buffered family — function pointers link to the
			 * real impls in spi_lcd_mem332.c on rp2350-PicoMite, or
			 * to no-op stubs in spi_lcd_mem332_stub.c elsewhere. The
			 * branch only fires on MEM332 ports (DISPLAY_TYPE never
			 * reaches NEXTGEN values otherwise). */
            gui_colour_depth = 8; /* RGB332 buffered panels */
            DrawRectangle = DrawRectangleMEM332;
            DrawBitmap = DrawBitmapMEM332;
            DrawBuffer = DrawBufferMEM332;
            ReadBuffer = ReadBufferMEM332;
            DrawBLITBuffer = DrawBlitBufferMEM332;
            ReadBLITBuffer = ReadBlitBufferMEM332;
            ScrollLCD = ScrollLCDMEM332;
        } else {
            gui_colour_depth = (Option.DISPLAY_TYPE == ILI9341_8) ? 8 : 16; /* RGB332 vs SSD1963 RGB565 */
            DrawRectangle = DrawRectangleMEM;
            DrawBitmap = DrawBitmapMEM;
            DrawBuffer = DrawBufferMEM;
            ReadBuffer = ReadBufferMEM;
            DrawBLITBuffer = DrawBufferMEM;
            ReadBLITBuffer = ReadBufferMEM;
        }
        DrawPixel = DrawPixelNormal;
    }
    // the parameters for the display panel are set here
    // the initialisation sequences and the SPI driver code was written by Peter Mather (matherp on The Back Shed forum)
    if (!spi_lcd_panel_init()) {
        // ST7920 is initialised here: its CD line doubles as an
        // inverted-polarity select, handled by this file's bus helpers.
        switch (Option.DISPLAY_TYPE) {
        case ST7920:
            PackHorizontal = 1;
            gpio_put(LCD_CD_PIN, GPIO_PIN_RESET);
            uSec(40000);
            SetCS();
            ResetController();
            ST7920command(1);
            uSec(20000);
            ST7920command(0b00001100); //display on
            uSec(20000);
            ST7920command(1); //DISPLAY CLEAR
            uSec(20000);
            ST7920command(0b00100110); //graphic mode
            uSec(20000);
            ClearCS(Option.LCD_CD);
            break;
        }
    }
    if (Option.DISPLAY_ORIENTATION & 1) {
        HRes = DisplayHRes;
        VRes = DisplayVRes;
    } else {
        HRes = DisplayVRes;
        VRes = DisplayHRes;
    }

    if (!InitOnly) {
        ResetDisplay();
        ClearScreen(Option.DISPLAY_CONSOLE ? Option.DefaultBC : 0);
        if (Option.Refresh) Display_Refresh();
    }
}

// set Chip Select for the LCD low
// this also checks the configuration of the SPI channel and if required reconfigures it to suit the LCD controller
void SetCS(void) {
    SPISpeedSet(Option.DISPLAY_TYPE);
    if (Option.DISPLAY_TYPE != ST7920)
        gpio_put(LCD_CS_PIN, GPIO_PIN_RESET); // set CS low
    else
        gpio_put(LCD_CD_PIN, GPIO_PIN_SET);
}

void hal_oled_spi_set_contrast(int level_percent) {
    int level = (level_percent * 255) / 100;
    spi_write_command(0x81);
    spi_write_command((unsigned char)level);
}
void ST7920command(unsigned char data) {
    unsigned char a[3];
    a[0] = ST7920setcommand;
    a[1] = data & 0xF0;
    a[2] = ((data & 0x0F) << 4) & 0xF0;
    SetCS();
    /* lcd_xmit_byte_multi is unconditionally assigned at SPISpeedSet
	 * time — defaults to the system SPI sender on non-MEM332 ports. */
    lcd_xmit_byte_multi(a, 3);
    ClearCS(Option.LCD_CD);
}

/****************************************************************************************************
 ****************************************************************************************************

 Basic drawing primitives
 all drawing on the LCD is done using either one of these two functions

 ****************************************************************************************************
****************************************************************************************************/
void spisendfast(unsigned char * n, int i) {
    lcd_xmit_byte_multi(n, i); //		HAL_SPI_Transmit(&hspi3,coord,4,500);
}
// Draw a filled rectangle
// this is the basic drawing promitive used by most drawing routines
//    x1, y1, x2, y2 - the coordinates
//    c - the colour
void DrawRectangleSPI(int x1, int y1, int x2, int y2, int c) {
    // convert the colours to 565 format
    unsigned char col[3];
    if (x1 == x2 && y1 == y2) {
        if (x1 < 0) return;
        if (x1 >= HRes) return;
        if (y1 < 0) return;
        if (y1 >= VRes) return;
        DefineRegionSPI(x1, y1, x2, y2, 1);
        if (Option.DISPLAY_TYPE == ILI9488 || Option.DISPLAY_TYPE == ILI9488P || Option.DISPLAY_TYPE == ILI9481IPS) {
            col[0] = (c >> 16) & 0xFC;
            col[1] = (c >> 8) & 0xFC;
            col[2] = (c & 0xFC);
        } else {
            col[0] = ((c >> 16) & 0b11111000) | ((c >> 13) & 0b00000111);
            col[1] = ((c >> 5) & 0b11100000) | ((c >> 3) & 0b00011111);
        }
        if (Option.DISPLAY_TYPE == GC9A01) {
            col[0] = ~col[0];
            col[1] = ~col[1];
        }
        SPIqueue(col);
    } else {
        int i, t, y;
        unsigned char * p;
        // make sure the coordinates are kept within the display area
        if (x2 <= x1) {
            t = x1;
            x1 = x2;
            x2 = t;
        }
        if (y2 <= y1) {
            t = y1;
            y1 = y2;
            y2 = t;
        }
        if (x1 < 0) x1 = 0;
        if (x1 >= HRes) x1 = HRes - 1;
        if (x2 < 0) x2 = 0;
        if (x2 >= HRes) x2 = HRes - 1;
        if (y1 < 0) y1 = 0;
        if (y1 >= VRes) y1 = VRes - 1;
        if (y2 < 0) y2 = 0;
        if (y2 >= VRes) y2 = VRes - 1;
        DefineRegionSPI(x1, y1, x2, y2, 1);
        if (Option.DISPLAY_TYPE == ILI9488 || Option.DISPLAY_TYPE == ILI9488P || Option.DISPLAY_TYPE == ILI9481IPS) {
            i = x2 - x1 + 1;
            i *= 3;
            p = LCDBuffer;
            col[0] = (c >> 16) & 0xFC;
            col[1] = (c >> 8) & 0xFC;
            col[2] = (c & 0xFC);
            for (t = 0; t < i; t += 3) {
                p[t] = col[0];
                p[t + 1] = col[1];
                p[t + 2] = col[2];
            }
            for (y = y1; y <= y2; y++) {
                if (PinDef[Option.LCD_CLK ? Option.LCD_CLK : Option.SYSTEM_CLK].mode & SPI0SCK)
                    spi_write_fast(spi0, p, i);
                else
                    spi_write_fast(spi1, p, i);
            }
        } else {
            i = x2 - x1 + 1;
            i *= 2;
            p = LCDBuffer;
            col[0] = ((c >> 16) & 0b11111000) | ((c >> 13) & 0b00000111);
            col[1] = ((c >> 5) & 0b11100000) | ((c >> 3) & 0b00011111);
            if (Option.DISPLAY_TYPE == GC9A01) {
                col[0] = ~col[0];
                col[1] = ~col[1];
            }
            for (t = 0; t < i; t += 2) {
                p[t] = col[0];
                p[t + 1] = col[1];
            }
            if (PinDef[Option.LCD_CLK ? Option.LCD_CLK : Option.SYSTEM_CLK].mode & SPI0SCK) {
                for (t = y1; t <= y2; t++) {
                    spi_write_fast(spi0, p, i);
                }
            } else {
                for (t = y1; t <= y2; t++) {
                    spi_write_fast(spi1, p, i);
                }
            }
        }
    }
    if (PinDef[Option.LCD_CLK ? Option.LCD_CLK : Option.SYSTEM_CLK].mode & SPI0SCK)
        spi_finish(spi0);
    else
        spi_finish(spi1);
    ClearCS(Option.LCD_CS); //set CS high
}
void PhysicalDrawRectSPI(int x1, int y1, int x2, int y2, int c) {
    int i, t, y;
    unsigned char * p;
    unsigned char col[3];
    DefineRegionSPI(x1, y1, x2, y2, 1);
    if (Option.DISPLAY_TYPE == ILI9488 || Option.DISPLAY_TYPE == ILI9488P || Option.DISPLAY_TYPE == ILI9481IPS) {
        i = x2 - x1 + 1;
        i *= 3;
        p = LCDBuffer;
        col[0] = (c >> 16) & 0xFC;
        col[1] = (c >> 8) & 0xFC;
        col[2] = (c & 0xFC);
        for (t = 0; t < i; t += 3) {
            p[t] = col[0];
            p[t + 1] = col[1];
            p[t + 2] = col[2];
        }
        for (y = y1; y <= y2; y++) {
            if (PinDef[Option.LCD_CLK ? Option.LCD_CLK : Option.SYSTEM_CLK].mode & SPI0SCK)
                spi_write_fast(spi0, p, i);
            else
                spi_write_fast(spi1, p, i);
        }
    } else {
        i = x2 - x1 + 1;
        i *= 2;
        p = LCDBuffer;
        col[0] = ((c >> 16) & 0b11111000) | ((c >> 13) & 0b00000111);
        col[1] = ((c >> 5) & 0b11100000) | ((c >> 3) & 0b00011111);
        if (Option.DISPLAY_TYPE == GC9A01) {
            col[0] = ~col[0];
            col[1] = ~col[1];
        }
        for (t = 0; t < i; t += 2) {
            p[t] = col[0];
            p[t + 1] = col[1];
        }
        if (PinDef[Option.LCD_CLK ? Option.LCD_CLK : Option.SYSTEM_CLK].mode & SPI0SCK) {
            for (t = y1; t <= y2; t++) {
                spi_write_fast(spi0, p, i);
            }
        } else {
            for (t = y1; t <= y2; t++) {
                spi_write_fast(spi1, p, i);
            }
        }
    }

    if (PinDef[Option.LCD_CLK ? Option.LCD_CLK : Option.SYSTEM_CLK].mode & SPI0SCK)
        spi_finish(spi0);
    else
        spi_finish(spi1);
    ClearCS(Option.LCD_CS); //set CS high
}
void DrawRectangleSPISCR(int x1, int y1, int x2, int y2, int c) {
    // convert the colours to 565 format
    int t;
    // make sure the coordinates are kept within the display area
    if (x2 <= x1) {
        t = x1;
        x1 = x2;
        x2 = t;
    }
    if (y2 <= y1) {
        t = y1;
        y1 = y2;
        y2 = t;
    }
    if (x1 < 0) x1 = 0;
    if (x1 >= HRes) x1 = HRes - 1;
    if (x2 < 0) x2 = 0;
    if (x2 >= HRes) x2 = HRes - 1;
    if (y1 < 0) y1 = 0;
    if (y1 >= VRes) y1 = VRes - 1;
    if (y2 < 0) y2 = 0;
    if (y2 >= VRes) y2 = VRes - 1;
    t = y2 - y1; // get the distance between the top and bottom
    // set y1 to the physical location in the frame buffer (only really has an effect when scrolling is in action)
    y1 = (y1 + ScrollStart) % VRes;
    y2 = y1 + t;                                      // and set y2 to the same
    if (y2 >= VRes) {                                 // if the box splits over the frame buffer boundary
        PhysicalDrawRectSPI(x1, y1, x2, VRes - 1, c); // draw the top part
        PhysicalDrawRectSPI(x1, 0, x2, y2 - VRes, c); // and the bottom part
    } else
        PhysicalDrawRectSPI(x1, y1, x2, y2, c); // the whole box is within the frame buffer - much easier
}

//Print the bitmap of a char on the video output
//    x, y - the top left of the char
//    width, height - size of the char's bitmap
//    scale - how much to scale the bitmap
//	  fc, bc - foreground and background colour
//    bitmap - pointer to the bitmap
void DrawBitmapSPI(int x1, int y1, int width, int height, int scale, int fc, int bc, unsigned char * bitmap) {
    int i, j, k, m, n;
    char f[3], b[3];
    int vertCoord, horizCoord, XStart, XEnd, YEnd;
    char * p = 0;
    union colourmap {
        char rgbbytes[4];
        unsigned int rgb;
    } c;
    if (bc == -1 && (void *)ReadBuffer == (void *)DisplayNotSet) bc = 0x0;
    if (x1 >= HRes || y1 >= VRes || x1 + width * scale < 0 || y1 + height * scale < 0) return;
    // adjust when part of the bitmap is outside the displayable coordinates
    vertCoord = y1;
    if (y1 < 0) y1 = 0; // the y coord is above the top of the screen
    XStart = x1;
    if (XStart < 0) XStart = 0; // the x coord is to the left of the left marginn
    XEnd = x1 + (width * scale) - 1;
    if (XEnd >= HRes) XEnd = HRes - 1; // the width of the bitmap will extend beyond the right margin
    YEnd = y1 + (height * scale) - 1;
    if (YEnd >= VRes) YEnd = VRes - 1; // the height of the bitmap will extend beyond the bottom margin
    if (bc == -1) {                    //special case of overlay text
        i = 0;
        j = width * height * scale * scale * 3;
        p = GetMemory(j); //allocate some temporary memory
        ReadBuffer(XStart, y1, XEnd, YEnd, (unsigned char *)p);
    }
    // convert the colours to 565 format
    if (Option.DISPLAY_TYPE == ILI9488 || Option.DISPLAY_TYPE == ILI9488P || Option.DISPLAY_TYPE == ILI9481IPS) {
        f[0] = (fc >> 16);
        f[1] = (fc >> 8) & 0xFF;
        f[2] = (fc & 0xFF);
        b[0] = (bc >> 16);
        b[1] = (bc >> 8) & 0xFF;
        b[2] = (bc & 0xFF);
    } else {
        f[0] = ((fc >> 16) & 0b11111000) | ((fc >> 13) & 0b00000111);
        f[1] = ((fc >> 5) & 0b11100000) | ((fc >> 3) & 0b00011111);
        b[0] = ((bc >> 16) & 0b11111000) | ((bc >> 13) & 0b00000111);
        b[1] = ((bc >> 5) & 0b11100000) | ((bc >> 3) & 0b00011111);
    }
    if (Option.DISPLAY_TYPE == GC9A01) {
        f[0] = ~f[0];
        b[0] = ~b[0];
        f[1] = ~f[1];
        b[1] = ~b[1];
    }

    DefineRegionSPI(XStart, y1, XEnd, YEnd, 1);

    n = 0;
    for (i = 0; i < height; i++) {         // step thru the font scan line by line
        for (j = 0; j < scale; j++) {      // repeat lines to scale the font
            if (vertCoord++ < 0) continue; // we are above the top of the screen
            if (vertCoord > VRes) {        // we have extended beyond the bottom of the screen
                ClearCS(Option.LCD_CS);    //set CS high
                if (p != NULL) FreeMemory((unsigned char *)p);
                return;
            }
            horizCoord = x1;
            for (k = 0; k < width; k++) {            // step through each bit in a scan line
                for (m = 0; m < scale; m++) {        // repeat pixels to scale in the x axis
                    if (horizCoord++ < 0) continue;  // we have not reached the left margin
                    if (horizCoord > HRes) continue; // we are beyond the right margin
                    if ((bitmap[((i * width) + k) / 8] >> (((height * width) - ((i * width) + k) - 1) % 8)) & 1) {
                        SPIqueue((uint8_t *)&f);
                    } else {
                        if (bc == -1) {
                            c.rgbbytes[0] = p[n];
                            c.rgbbytes[1] = p[n + 1];
                            c.rgbbytes[2] = p[n + 2];
                            if (Option.DISPLAY_TYPE == ILI9488 || Option.DISPLAY_TYPE == ILI9488P || Option.DISPLAY_TYPE == ILI9481IPS) {
                                b[0] = c.rgbbytes[2];
                                b[1] = c.rgbbytes[1];
                                b[2] = c.rgbbytes[0];
                            } else {
                                b[0] = ((c.rgb >> 16) & 0b11111000) | ((c.rgb >> 13) & 0b00000111);
                                b[1] = ((c.rgb >> 5) & 0b11100000) | ((c.rgb >> 3) & 0b00011111);
                            }
                        }
                        SPIqueue((uint8_t *)&b);
                    }
                    n += 3;
                }
            }
        }
    }

    ClearCS(Option.LCD_CS); //set CS high

    // revert to non enhanced SPI mode
    if (p != NULL) FreeMemory((unsigned char *)p);
}
void DrawBitmapSPISCR(int x1, int y1, int width, int height, int scale, int fc, int bc, unsigned char * bitmap) {
    int i, j, k, m, y, yt, n;
    char f[3], b[3];
    int vertCoord, horizCoord, XStart, XEnd, YEnd;
    char * p = 0;
    union colourmap {
        char rgbbytes[4];
        unsigned int rgb;
    } c;
    // convert the colours to 565 format
    if (Option.DISPLAY_TYPE == ILI9488 || Option.DISPLAY_TYPE == ILI9488P || Option.DISPLAY_TYPE == ILI9481IPS) {
        f[0] = (fc >> 16);
        f[1] = (fc >> 8) & 0xFF;
        f[2] = (fc & 0xFF);
        b[0] = (bc >> 16);
        b[1] = (bc >> 8) & 0xFF;
        b[2] = (bc & 0xFF);
    } else {
        f[0] = ((fc >> 16) & 0b11111000) | ((fc >> 13) & 0b00000111);
        f[1] = ((fc >> 5) & 0b11100000) | ((fc >> 3) & 0b00011111);
        b[0] = ((bc >> 16) & 0b11111000) | ((bc >> 13) & 0b00000111);
        b[1] = ((bc >> 5) & 0b11100000) | ((bc >> 3) & 0b00011111);
    }
    if (Option.DISPLAY_TYPE == GC9A01) {
        f[0] = ~f[0];
        b[0] = ~b[0];
        f[1] = ~f[1];
        b[1] = ~b[1];
    }
    if (bc == -1 && (void *)ReadBuffer == (void *)DisplayNotSet) bc = 0x0;
    if (x1 >= HRes || y1 >= VRes || x1 + width * scale < 0 || y1 + height * scale < 0) return;
    // adjust when part of the bitmap is outside the displayable coordinates
    vertCoord = y1;
    if (y1 < 0) y1 = 0; // the y coord is above the top of the screen
    XStart = x1;
    if (XStart < 0) XStart = 0; // the x coord is to the left of the left marginn
    XEnd = x1 + (width * scale) - 1;
    if (XEnd >= HRes) XEnd = HRes - 1; // the width of the bitmap will extend beyond the right margin
    if (bc == -1) {                    //special case of overlay text
        j = width * height * scale * scale * 3;
        p = GetMemory(j); //allocate some temporary memory
        ReadBuffer(XStart, y1, XEnd, (y1 + (height * scale) - 1), (unsigned char *)p);
    }
    yt = y = (y1 + ScrollStart) % VRes;
    YEnd = (y + (height * scale) - 1) % VRes;
    if (YEnd < y) YEnd = VRes - 1;
    DefineRegionSPI(XStart, y, XEnd, YEnd, 1);
    n = 0;
    for (i = 0; i < height; i++) {         // step thru the font scan line by line
        for (j = 0; j < scale; j++) {      // repeat lines to scale the font
            if (vertCoord++ < 0) continue; // we are above the top of the screen
            if (vertCoord > VRes) {        // we have extended beyond the bottom of the screen
                ClearCS(Option.LCD_CS);    //set CS high
                if (p != NULL) FreeMemory((unsigned char *)p);
                return;
            }
            if (y++ == VRes) {
                DefineRegionSPI(XStart, 0, XEnd, ((yt + (height * scale) - 1) % VRes), 1);
            }
            horizCoord = x1;
            for (k = 0; k < width; k++) {            // step through each bit in a scan line
                for (m = 0; m < scale; m++) {        // repeat pixels to scale in the x axis
                    if (horizCoord++ < 0) continue;  // we have not reached the left margin
                    if (horizCoord > HRes) continue; // we are beyond the right margin
                    if ((bitmap[((i * width) + k) / 8] >> (((height * width) - ((i * width) + k) - 1) % 8)) & 1) {
                        SPIqueue((uint8_t *)&f);
                    } else {
                        if (bc == -1) {
                            c.rgbbytes[0] = p[n];
                            c.rgbbytes[1] = p[n + 1];
                            c.rgbbytes[2] = p[n + 2];
                            if (Option.DISPLAY_TYPE == ILI9488 || Option.DISPLAY_TYPE == ILI9488P || Option.DISPLAY_TYPE == ILI9481IPS) {
                                b[0] = c.rgbbytes[2];
                                b[1] = c.rgbbytes[1];
                                b[2] = c.rgbbytes[0];
                            } else {
                                b[0] = ((c.rgb >> 16) & 0b11111000) | ((c.rgb >> 13) & 0b00000111);
                                b[1] = ((c.rgb >> 5) & 0b11100000) | ((c.rgb >> 3) & 0b00011111);
                            }
                        }
                        SPIqueue((uint8_t *)&b);
                    }
                    n += 3;
                }
            }
        }
    }

    ClearCS(Option.LCD_CS); //set CS high

    // revert to non enhanced SPI mode
    if (p != NULL) FreeMemory((unsigned char *)p);
}
const unsigned char map32[256];

void DrawBufferSPI(int x1, int y1, int x2, int y2, unsigned char * p) {
    union colourmap {
        char rgbbytes[4];
        unsigned int rgb;
    } c;
    unsigned char q[3];
    int i, t;
    if (x2 <= x1) {
        t = x1;
        x1 = x2;
        x2 = t;
    }
    if (y2 <= y1) {
        t = y1;
        y1 = y2;
        y2 = t;
    }
    if (x1 < 0) x1 = 0;
    if (x1 >= HRes) x1 = HRes - 1;
    if (x2 < 0) x2 = 0;
    if (x2 >= HRes) x2 = HRes - 1;
    if (y1 < 0) y1 = 0;
    if (y1 >= VRes) y1 = VRes - 1;
    if (y2 < 0) y2 = 0;
    if (y2 >= VRes) y2 = VRes - 1;
    i = (x2 - x1 + 1) * (y2 - y1 + 1);
    DefineRegionSPI(x1, y1, x2, y2, 1);
    while (i--) {
        c.rgbbytes[0] = *p++; //this order swaps the bytes to match the .BMP file
        c.rgbbytes[1] = *p++;
        c.rgbbytes[2] = *p++;
        // convert the colours to 565 format
        // convert the colours to 565 format
        if (Option.DISPLAY_TYPE == ILI9488 || Option.DISPLAY_TYPE == ILI9488P || Option.DISPLAY_TYPE == ILI9481IPS) {
            q[0] = c.rgbbytes[2];
            q[1] = c.rgbbytes[1];
            q[2] = c.rgbbytes[0];
        } else {
            q[0] = ((c.rgb >> 16) & 0b11111000) | ((c.rgb >> 13) & 0b00000111);
            q[1] = ((c.rgb >> 5) & 0b11100000) | ((c.rgb >> 3) & 0b00011111);
        }
        if (Option.DISPLAY_TYPE == GC9A01) {
            q[0] = ~q[0];
            q[1] = ~q[1];
        }
        SPIqueue(q);
    }
    ClearCS(Option.LCD_CS); //set CS high
}
void DrawBufferSPISCR(int x1, int y1, int x2, int y2, unsigned char * p) {
    union colourmap {
        char rgbbytes[4];
        unsigned int rgb;
    } c;
    unsigned char q[3];
    int i, t;
    if (x2 <= x1) {
        t = x1;
        x1 = x2;
        x2 = t;
    }
    if (y2 <= y1) {
        t = y1;
        y1 = y2;
        y2 = t;
    }
    if (x1 < 0) x1 = 0;
    if (x1 >= HRes) x1 = HRes - 1;
    if (x2 < 0) x2 = 0;
    if (x2 >= HRes) x2 = HRes - 1;
    if (y1 < 0) y1 = 0;
    if (y1 >= VRes) y1 = VRes - 1;
    if (y2 < 0) y2 = 0;
    if (y2 >= VRes) y2 = VRes - 1;
    t = y2 - y1; // get the distance between the top and bottom
    y1 = (y1 + ScrollStart) % VRes;
    y2 = y1 + t;
    i = (x2 - x1 + 1) * (y2 - y1 + 1);
    if (y2 >= VRes) {
        DefineRegionSPI(x1, y1, x2, VRes - 1, 1);
        for (i = (x2 - x1 + 1) * ((VRes - 1) - y1 + 1); i > 0; i--) {
            c.rgbbytes[0] = *p++; //this order swaps the bytes to match the .BMP file
            c.rgbbytes[1] = *p++;
            c.rgbbytes[2] = *p++;
            // convert the colours to 565 format
            // convert the colours to 565 format
            if (Option.DISPLAY_TYPE == ILI9488 || Option.DISPLAY_TYPE == ILI9488P || Option.DISPLAY_TYPE == ILI9481IPS) {
                q[0] = c.rgbbytes[2];
                q[1] = c.rgbbytes[1];
                q[2] = c.rgbbytes[0];
            } else {
                q[0] = ((c.rgb >> 16) & 0b11111000) | ((c.rgb >> 13) & 0b00000111);
                q[1] = ((c.rgb >> 5) & 0b11100000) | ((c.rgb >> 3) & 0b00011111);
            }
            if (Option.DISPLAY_TYPE == GC9A01) {
                q[0] = ~q[0];
                q[1] = ~q[1];
            }
            SPIqueue(q);
        }
        DefineRegionSPI(x1, 0, x2, y2 - VRes, 1);
        for (i = (x2 - x1 + 1) * (y2 - VRes + 1); i > 0; i--) {
            c.rgbbytes[0] = *p++; //this order swaps the bytes to match the .BMP file
            c.rgbbytes[1] = *p++;
            c.rgbbytes[2] = *p++;
            // convert the colours to 565 format
            // convert the colours to 565 format
            if (Option.DISPLAY_TYPE == ILI9488 || Option.DISPLAY_TYPE == ILI9488P || Option.DISPLAY_TYPE == ILI9481IPS) {
                q[0] = c.rgbbytes[2];
                q[1] = c.rgbbytes[1];
                q[2] = c.rgbbytes[0];
            } else {
                q[0] = ((c.rgb >> 16) & 0b11111000) | ((c.rgb >> 13) & 0b00000111);
                q[1] = ((c.rgb >> 5) & 0b11100000) | ((c.rgb >> 3) & 0b00011111);
            }
            if (Option.DISPLAY_TYPE == GC9A01) {
                q[0] = ~q[0];
                q[1] = ~q[1];
            }
            SPIqueue(q);
        }
    } else {
        DefineRegionSPI(x1, y1, x2, y2, 1);
        while (i--) {
            c.rgbbytes[0] = *p++; //this order swaps the bytes to match the .BMP file
            c.rgbbytes[1] = *p++;
            c.rgbbytes[2] = *p++;
            // convert the colours to 565 format
            // convert the colours to 565 format
            if (Option.DISPLAY_TYPE == ILI9488 || Option.DISPLAY_TYPE == ILI9488P || Option.DISPLAY_TYPE == ILI9481IPS) {
                q[0] = c.rgbbytes[2];
                q[1] = c.rgbbytes[1];
                q[2] = c.rgbbytes[0];
            } else {
                q[0] = ((c.rgb >> 16) & 0b11111000) | ((c.rgb >> 13) & 0b00000111);
                q[1] = ((c.rgb >> 5) & 0b11100000) | ((c.rgb >> 3) & 0b00011111);
            }
            if (Option.DISPLAY_TYPE == GC9A01) {
                q[0] = ~q[0];
                q[1] = ~q[1];
            }
            SPIqueue(q);
        }
    }
    ClearCS(Option.LCD_CS); //set CS high
}

void ScrollLCDSPISCR(int lines) {
    if (lines == 0) return;
    int t;
    t = ScrollStart;
    if (lines >= 0) {
        DrawRectangle(0, 0, HRes - 1, lines - 1, gui_bcolour); // erase the line to be scrolled off
        while (lines--) {
            if (++t >= VRes) t = 0;
        }
    } else {
        while (lines++) {
            if (--t < 0) t = VRes - 1;
        }
        //        DrawRectangle(0, 0, HRes - 1, linesave - 1, gui_bcolour); // erase the line introduced at the top
    }
    spi_write_command(CMD_SET_SCROLL_START);
    spi_write_data(t >> 8);
    spi_write_data(t);
    ScrollStart = t;
}
void ScrollLCDSPI(int lines) {
    if (lines == 0) return;
    unsigned char * buff = GetMemory(3 * HRes);
    if (lines >= 0) {
        for (int i = 0; i < VRes - lines; i++) {
            ReadBLITBuffer(0, i + lines, HRes - 1, i + lines, buff);
            DrawBLITBuffer(0, i, HRes - 1, i, buff);
        }
        DrawRectangle(0, VRes - lines, HRes - 1, VRes - 1, gui_bcolour); // erase the lines to be scrolled off
    } else {
        lines = -lines;
        for (int i = VRes - 1; i >= lines; i--) {
            ReadBLITBuffer(0, i - lines, HRes - 1, i - lines, buff);
            DrawBLITBuffer(0, i, HRes - 1, i, buff);
        }
        DrawRectangle(0, 0, HRes - 1, lines - 1, gui_bcolour); // erase the lines introduced at the top
    }
    FreeMemory(buff);
}
void DrawBufferMEM(int x1, int y1, int x2, int y2, unsigned char * p) {
    int x, y;
    union colourmap {
        char rgbbytes[4];
        unsigned int rgb;
    } c;
    for (y = y1; y <= y2; y++) {
        for (x = x1; x <= x2; x++) {
            c.rgbbytes[0] = *p++; //this order swaps the bytes to match the .BMP file
            if (c.rgbbytes[0] < 0x40) c.rgbbytes[0] = 0;
            c.rgbbytes[1] = *p++;
            if (c.rgbbytes[1] < 0x40) c.rgbbytes[1] = 0;
            c.rgbbytes[2] = *p++;
            if (c.rgbbytes[2] < 0x40) c.rgbbytes[2] = 0;
            c.rgbbytes[3] = 0;
            DrawPixel(x, y, c.rgb);
        }
    }
}
void ReadBufferMEM(int x1, int y1, int x2, int y2, unsigned char * buff) {
    unsigned char * p = (void *)((unsigned int)LCDBuffer);
    int x, y, loc, t;
    unsigned char mask;
    if (x1 < 0) x1 = 0;
    if (x1 >= HRes) x1 = HRes - 1;
    if (x2 < 0) x2 = 0;
    if (x2 >= HRes) x2 = HRes - 1;
    if (y1 < 0) y1 = 0;
    if (y1 >= VRes) y1 = VRes - 1;
    if (y2 < 0) y2 = 0;
    if (y2 >= VRes) y2 = VRes - 1;
    if (x2 <= x1) {
        t = x1;
        x1 = x2;
        x2 = t;
    }
    if (y2 <= y1) {
        t = y1;
        y1 = y2;
        y2 = t;
    }
    if (Option.DISPLAY_ORIENTATION == PORTRAIT) {
        t = x1;
        x1 = VRes - y2 - 1;
        y2 = t;
        t = x2;
        x2 = VRes - y1 - 1;
        y1 = t;
    }
    if (Option.DISPLAY_ORIENTATION == RLANDSCAPE) {
        x1 = HRes - x1 - 1;
        x2 = HRes - x2 - 1;
        y1 = VRes - y1 - 1;
        y2 = VRes - y2 - 1;
    }
    if (Option.DISPLAY_ORIENTATION == RPORTRAIT) {
        t = y1;
        y1 = HRes - x1 - 1;
        x1 = t;
        t = y2;
        y2 = HRes - x2 - 1;
        x2 = t;
    }
    if (x2 <= x1) {
        t = x1;
        x1 = x2;
        x2 = t;
    }
    if (y2 <= y1) {
        t = y1;
        y1 = y2;
        y2 = t;
    }

    if (y1 < low_y) low_y = y1;
    if (y2 > high_y) high_y = y2;
    if (x1 < low_x) low_x = x1;
    if (x2 > high_x) high_x = x2;
    for (x = x1; x <= x2; x++) {
        for (y = y1; y <= y2; y++) {
            if (!PackHorizontal) {
                loc = x + (y / 8) * DisplayHRes; //get the byte address for this bit
                mask = 1 << (y % 8);             //get the bit position for this bit
            } else {
                loc = x / 8 + y * DisplayHRes / 8; //get the byte address for this bit
                mask = 1 << (7 - (x % 8));         //get the bit position for this bit
            }
            if (p[loc] & mask) {
                *buff++ = 0xFF;
                *buff++ = 0xFF;
                *buff++ = 0xFF;
            } else {
                *buff++ = 0x0;
                *buff++ = 0x0;
                *buff++ = 0x0;
            }
        }
    }
}
void DrawRectangleMEM(int x1, int y1, int x2, int y2, int c) {
    unsigned char * p = (void *)((unsigned int)LCDBuffer);
    int x, y, loc, t;
    unsigned char mask;
    if (x1 < 0) x1 = 0;
    if (x1 >= HRes) x1 = HRes - 1;
    if (x2 < 0) x2 = 0;
    if (x2 >= HRes) x2 = HRes - 1;
    if (y1 < 0) y1 = 0;
    if (y1 >= VRes) y1 = VRes - 1;
    if (y2 < 0) y2 = 0;
    if (y2 >= VRes) y2 = VRes - 1;
    if (x2 <= x1) {
        t = x1;
        x1 = x2;
        x2 = t;
    }
    if (y2 <= y1) {
        t = y1;
        y1 = y2;
        y2 = t;
    }
    if (Option.DISPLAY_ORIENTATION == PORTRAIT) {
        t = x1;
        x1 = VRes - y2 - 1;
        y2 = t;
        t = x2;
        x2 = VRes - y1 - 1;
        y1 = t;
    }
    if (Option.DISPLAY_ORIENTATION == RLANDSCAPE) {
        x1 = HRes - x1 - 1;
        x2 = HRes - x2 - 1;
        y1 = VRes - y1 - 1;
        y2 = VRes - y2 - 1;
    }
    if (Option.DISPLAY_ORIENTATION == RPORTRAIT) {
        t = y1;
        y1 = HRes - x1 - 1;
        x1 = t;
        t = y2;
        y2 = HRes - x2 - 1;
        x2 = t;
    }
    if (x2 <= x1) {
        t = x1;
        x1 = x2;
        x2 = t;
    }
    if (y2 <= y1) {
        t = y1;
        y1 = y2;
        y2 = t;
    }

    if (y1 < low_y) low_y = y1;
    if (y2 > high_y) high_y = y2;
    if (x1 < low_x) low_x = x1;
    if (x2 > high_x) high_x = x2;
    for (x = x1; x <= x2; x++) {
        for (y = y1; y <= y2; y++) {
            if (!PackHorizontal) {
                loc = x + (y / 8) * DisplayHRes; //get the byte address for this bit
                mask = 1 << (y % 8);             //get the bit position for this bit
            } else {
                loc = x / 8 + y * DisplayHRes / 8; //get the byte address for this bit
                mask = 1 << (7 - (x % 8));         //get the bit position for this bit
            }
            if (c) {
                p[loc] |= mask;
            } else {
                p[loc] &= (~mask);
            }
        }
    }
}
void DrawPixelMEM(int x1, int y1, int c) {
    DrawRectangleMEM(x1, y1, x1, y1, c);
}
void DrawBitmapMEM(int x1, int y1, int width, int height, int scale, int fc, int bc, unsigned char * bitmap) {
    int i, j, k, m, x, y, t, loc;
    unsigned char omask, amask;
    unsigned char * p = (void *)((unsigned int)LCDBuffer);
    if (x1 >= HRes || y1 >= VRes || x1 + width * scale < 0 || y1 + height * scale < 0) return;
    for (i = 0; i < height; i++) {            // step thru the font scan line by line
        for (j = 0; j < scale; j++) {         // repeat lines to scale the font
            for (k = 0; k < width; k++) {     // step through each bit in a scan line
                for (m = 0; m < scale; m++) { // repeat pixels to scale in the x axis
                    x = x1 + k * scale + m;
                    y = y1 + i * scale + j;
                    if (Option.DISPLAY_ORIENTATION == PORTRAIT) {
                        t = x;
                        x = VRes - y - 1;
                        y = t;
                    }
                    if (Option.DISPLAY_ORIENTATION == RLANDSCAPE) {
                        x = HRes - x - 1;
                        y = VRes - y - 1;
                    }
                    if (Option.DISPLAY_ORIENTATION == RPORTRAIT) {
                        t = y;
                        y = HRes - x - 1;
                        x = t;
                    }
                    if (y < low_y) low_y = y;
                    if (y > high_y) high_y = y;
                    if (x < low_x) low_x = x;
                    if (x > high_x) high_x = x;
                    if (!PackHorizontal) {
                        loc = x + (y / 8) * DisplayHRes; //get the byte address for this bit
                        omask = 1 << (y % 8);            //get the bit position for this bit
                        amask = ~omask;
                    } else {
                        loc = x / 8 + y * DisplayHRes / 8; //get the byte address for this bit
                        omask = 1 << (7 - (x % 8));        //get the bit position for this bit
                        amask = ~omask;
                    }
                    if (x >= 0 && x < DisplayHRes && y >= 0 && y < DisplayVRes) { // if the coordinates are valid
                        if ((bitmap[((i * width) + k) / 8] >> (((height * width) - ((i * width) + k) - 1) % 8)) & 1) {
                            if (fc) {
                                p[loc] |= omask;
                            } else {
                                p[loc] &= amask;
                            }
                        } else {
                            if (bc > 0) {
                                p[loc] |= omask;
                            } else if (bc == 0) {
                                p[loc] &= amask;
                            }
                        }
                    }
                }
            }
        }
    }
}

void N5110SetXY(int x, int y) {
    int LcdData;
    LcdData = 0b01000000 | y;
    spi_write_command(LcdData);
    LcdData = 0b10000000 | x;
    spi_write_command(LcdData);
}
void SSD1306I2CSetXY(uint8_t x, uint8_t y) {
    uint8_t xn = x;
    I2C_Send_Command(0xB0 | y);
    I2C_Send_Command(0x10 | ((xn >> 4) & 0xF));
    I2C_Send_Command(0x00 | (xn & 0xF));
}
void SSD1306SPISetXY(uint8_t x, uint8_t y) {
    uint8_t xn = x;
    spi_write_command(0xB0 | y);
    spi_write_command(0x10 | ((xn >> 4) & 0xF));
    spi_write_command(0x00 | (xn & 0xF));
}
void ST7920SetXY(int x, int y) {
    int xx = x, yy = y;
    if (yy > 31) {
        xx = xx + 8;
        yy = yy - 32;
    }
    unsigned char a[5];
    a[0] = ST7920setcommand;
    a[1] = (yy & 0x10) | 0x80;
    a[2] = (yy & 0x0F) << 4;
    a[3] = 0x80;
    a[4] = xx << 4;
    SetCS();
    lcd_xmit_byte_multi(a, 5);
    uSec(50);
    ClearCS(Option.LCD_CD);
}
void Display_Refresh(void) {
    /* The MEM332 (>=NEXTGEN) bail and the DISPLAY_TYPE==0 bail are
	 * always-true on non-MEM332 ports anyway (DISPLAY_TYPE never
	 * reaches NEXTGEN there), but they're harmless extras. */
    if (Option.DISPLAY_TYPE >= NEXTGEN || Option.DISPLAY_TYPE == 0 || !(Option.DISPLAY_TYPE <= I2C_PANEL || Option.DISPLAY_TYPE >= BufferedPanel)) return;
    unsigned char * p = (void *)((unsigned int)LCDBuffer);
    if (low_x == silly_low && high_x == silly_high && low_y == silly_low && high_y == silly_high) return; //Nothing to do
    if (low_x < 0) low_x = 0;
    if (low_y < 0) low_y = 0;
    if (high_x > DisplayHRes) high_x = DisplayHRes - 1;
    if (high_y > DisplayVRes) high_y = DisplayVRes - 1;
    if (Option.DISPLAY_TYPE == N5110) {
        int y;
        for (y = low_y / 8; y < (high_y & 0xf8) / 8 + 1; y++) {
            N5110SetXY(low_x, y);
            SetCS();
            gpio_put(LCD_CD_PIN, GPIO_PIN_SET);
            lcd_xmit_byte_multi(p + (y * DisplayHRes) + low_x, high_x - low_x + 1);
            ClearCS(Option.LCD_CS);
        }
    } else if (Option.DISPLAY_TYPE <= I2C_PANEL) {
        int y;
        for (y = low_y / 8; y < (high_y & 0xf8) / 8 + 1; y++) {
            SSD1306I2CSetXY(Option.I2Coffset + low_x, y);
            I2C_Send_Data(p + (y * DisplayHRes) + low_x, high_x - low_x + 1);
        }
    } else if (Option.DISPLAY_TYPE == SSD1306SPI) {
        int y;
        for (y = low_y / 8; y < (high_y & 0xf8) / 8 + 1; y++) {
            SSD1306SPISetXY(Option.I2Coffset + low_x, y);
            SetCS();
            gpio_put(LCD_CD_PIN, GPIO_PIN_SET);
            lcd_xmit_byte_multi(p + (y * DisplayHRes) + low_x, high_x - low_x + 1);
            ClearCS(Option.LCD_CS);
        }
    } else if (Option.DISPLAY_TYPE == ST7920) {
        int y, i;
        unsigned char x_array[33];
        unsigned char * q;
        for (y = low_y; y <= high_y; y++) {
            q = p + y * 16;
            x_array[0] = ST7920setata;
            for (i = 1; i < 33; i += 2) {
                x_array[i] = *q & 0xF0;
                x_array[i + 1] = ((*q++) << 4) & 0xF0;
            }
            ST7920SetXY(0, y);
            SetCS();
            lcd_xmit_byte_multi(x_array, 33);
            ClearCS(Option.LCD_CD);
        }
    }
    low_x = silly_low;
    high_y = silly_high;
    low_y = silly_low;
    high_x = silly_high;
}
#endif
void DisplayNotSet(void) {
    error("Display not configured");
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// These three functions allow the SPI port to be used by multiple drivers (LCD/touch/SD card)
// The BASIC use of the SPI port does NOT use these functions
// The MX170 uses SPI channel 1 which is shared by the BASIC program
// The MX470 uses SPI channel 2 which it has exclusive control of (needed because touch can be used at any time)
//
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
extern uint16_t SPI_CLK_PIN;
// config the SPI port for output
// it will not touch the port if it has already been opened
void SPISpeedSet(int device) {
    /* MEM332 ports with a separate LCD SPI clock have everything
	 * configured already and bail early. Other ports always have
	 * Option.LCD_CLK == 0 so this branch is dead. */
    if (Option.LCD_CLK && Option.LCD_CLK != Option.SYSTEM_CLK && device > I2C_PANEL && device != TOUCH && device != SLOWTOUCH) return;
    if (CurrentSPIDevice != device) {
        if (device == SDSLOW || (device == SDFAST && SPI_CLK_PIN != SD_CLK_PIN)) {
            //			MMPrintString("Slow Bitbang\r\n");
            xchg_byte = BitBangSwapSPI;
            xmit_byte_multi = BitBangSendSPI;
            rcvr_byte_multi = BitBangReadSPI;
            SET_SPI_CLK = BitBangSetClk;
            SET_SPI_CLK(SD_SPI_SPEED, false, false);
        } else {
            if (PinDef[Option.SYSTEM_CLK].mode & SPI0SCK && PinDef[Option.SYSTEM_MOSI].mode & SPI0TX && PinDef[Option.SYSTEM_MISO].mode & SPI0RX) {
                //				MMPrintString("SPI0\r\n");
                xchg_byte = HW0SwapSPI;
                xmit_byte_multi = HW0SendSPI;
                rcvr_byte_multi = HW0ReadSPI;
                /* On non-NEXTGEN ports Option.LCD_CLK is always 0
				 * (no separate LCD clock pin), so the inner branch
				 * always takes — unifying lcd_xmit_byte_multi with
				 * xmit_byte_multi. NEXTGEN ports may have a separate
				 * LCD clock; the assignment still applies when
				 * LCD_CLK == SYSTEM_CLK. */
                if (!Option.LCD_CLK || Option.LCD_CLK == Option.SYSTEM_CLK) {
                    lcd_xmit_byte_multi = HW0SendSPI;
                    lcd_rcvr_byte_multi = HW0ReadSPI;
                }
                SET_SPI_CLK = HW0Clk;
                gpio_set_input_enabled(PinDef[Option.SYSTEM_CLK].GPno, false);
                gpio_set_input_enabled(PinDef[Option.SYSTEM_MOSI].GPno, false);
                gpio_set_input_enabled(PinDef[Option.SYSTEM_MISO].GPno, false);
            } else if (PinDef[Option.SYSTEM_CLK].mode & SPI1SCK && PinDef[Option.SYSTEM_MOSI].mode & SPI1TX && PinDef[Option.SYSTEM_MISO].mode & SPI1RX) {
                //				MMPrintString("SPI1\r\n");
                xchg_byte = HW1SwapSPI;
                xmit_byte_multi = HW1SendSPI;
                rcvr_byte_multi = HW1ReadSPI;
                if (!Option.LCD_CLK || Option.LCD_CLK == Option.SYSTEM_CLK) {
                    lcd_xmit_byte_multi = HW1SendSPI;
                    lcd_rcvr_byte_multi = HW1ReadSPI;
                }
                SET_SPI_CLK = HW1Clk;
            } else {
                //				MMPrintString("Fast Bitbang\r\n");
                xchg_byte = BitBangSwapSPI;
                xmit_byte_multi = BitBangSendSPI;
                rcvr_byte_multi = BitBangReadSPI;
                /* Default LCD pointers to share the bit-bang sender —
				 * non-NEXTGEN ports always take this path, NEXTGEN
				 * ports override when they have a separate LCD clock. */
                lcd_xmit_byte_multi = BitBangSendSPI;
                lcd_rcvr_byte_multi = BitBangReadSPI;
                SET_SPI_CLK = BitBangSetClk;
            }
            SET_SPI_CLK(display_details[device].speed, display_details[device].CPOL, display_details[device].CPHASE);
        }
        CurrentSPIDevice = device;
    }
}

// set the chip select for SPI to high (disabled)
void ClearCS(int pin) {
    if (pin) {
        if (Option.DISPLAY_TYPE != ST7920)
            gpio_put(PinDef[pin].GPno, GPIO_PIN_SET);
        else
            gpio_put(PinDef[pin].GPno, GPIO_PIN_RESET);
    }
}
#if !HAL_PORT_IS_VGA
int GetLineILI9341(void) {
    SetCS();
    SPISpeedSet(Option.DISPLAY_TYPE == ILI9341 ? SPIReadSpeed : ST7789RSpeed);
    gpio_put(LCD_CD_PIN, GPIO_PIN_RESET);
    SPIsend(ILI9341_GETSCANLINE);
    gpio_put(LCD_CD_PIN, GPIO_PIN_SET);
    uSec(3);
    /* The MEM332 and legacy paths use different SPI-read protocols
	 * to recover the response byte. Per-port impl in spi_lcd_mem332*.c. */
    int q = hal_spi_lcd_read_response_byte();
    ClearCS(Option.LCD_CS);
    SPISpeedSet(Option.DISPLAY_TYPE);
    return q;
}
#endif
