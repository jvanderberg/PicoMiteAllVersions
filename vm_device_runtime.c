#ifdef PICOMITE_VM_DEVICE_ONLY

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <stdlib.h>
#include <time.h>

#include "pico/stdlib.h"
#include "pico/time.h"
#include "tusb.h"
#include "hardware/adc.h"
#include "hardware/exception.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/spi.h"

#include "configuration.h"
#include "lfs.h"
#include "FileIO.h"
#include "diskio.h"
#include "Hardware_Includes.h"
#include "Draw.h"
#include "SPI-LCD.h"
#include "bc_alloc.h"
#include "font1.h"
#include "Misc_12x20_LE.h"
#include "Hom_16x24_LE.h"
#include "Fnt_10x16.h"
#include "Inconsola.h"
#include "ArialNumFontPlus.h"
#include "arial_bold.h"
#include "Font_8x6.h"
#include "smallfont.h"
#include "font-8x10.h"
#include "picocalc/i2ckbd.h"

static uint8_t vm_display_fb[320 * 320];
static uint64_t vm_boot_us;
static int vm_soft_display_initialised = 0;
static int vm_fastgfx_active = 0;
static int vm_fastgfx_fps = 60;
static uint64_t vm_fastgfx_next_sync_us = 0;
static int vm_console_display_enabled = 0;
static int vm_lcd_initialised = 0;
static unsigned char vm_lcd_linebuf[320 * 2];
static repeating_timer_t vm_tick_timer;

static void vm_lcd_clear_cs(int pin);
static void vm_lcd_write_fast(const uint8_t *src, int len);
static void vm_lcd_set_cs(void);
static void vm_lcd_draw_rect_physical(int x1, int y1, int x2, int y2, int c);

static int vm_has_active_writebuf(void) {
    return WriteBuf != NULL && (WriteBuf == FrameBuf || WriteBuf == LayerBuf);
}

void error(char *msg, ...);

static void vm_boot_log(const char *msg) {
    while (*msg) putchar_raw(*msg++);
    putchar_raw('\r');
    putchar_raw('\n');
}

static void vm_boot_panic_colour(int c) {
    if (!vm_lcd_initialised) return;
    vm_lcd_draw_rect_physical(0, 0, DisplayHRes - 1, DisplayVRes - 1, c);
}

volatile long long int mSecTimer = 0;
volatile unsigned int InkeyTimer = 0;
volatile unsigned int PauseTimer = 0;
volatile unsigned int IntPauseTimer = 0;
volatile int MMAbort = false;
unsigned char BreakKey = BREAK_KEY;

static bool vm_timer_callback(repeating_timer_t *rt) {
    (void)rt;
    mSecTimer++;
    InkeyTimer++;
    PauseTimer++;
    IntPauseTimer++;
    if (++CursorTimer > CURSOR_OFF + CURSOR_ON) CursorTimer = 0;
    return true;
}

static void vm_sigbus(void) {
    vm_boot_log("VMBOOT FAULT");
    vm_boot_panic_colour(RGB(255, 128, 0));
    while (1) {
        tight_loop_contents();
    }
}

jmp_buf mark;

struct option_s Option = {0};
unsigned char *CurrentLinePtr = NULL;
int g_OptionBase = 0;
int last_fcolour = WHITE;
int last_bcolour = BLACK;
uint64_t timeroffset = 0;
int ticks_per_second = 0;
int PromptFont = 0x01;
int PromptFC = WHITE;
int PromptBC = BLACK;
int FatFSFileSystem = 0;
int OptionFileErrorAbort = 1;
int OptionConsole = 0;
int irqs = 0;
int MMCharPos = 1;
int MMPromptPos = 1;
int ListCnt = 0;
int ScreenSize = 0;
int PrintPixelMode = 0;
char LCDAttrib = 0;
int RGB121map[16];
int FSerror = 0;
int FlashLoad = 0;
int InvokingCtrl = 0;
int CameraSlice = 0;
int CameraChannel = 0;
const uint8_t *flash_option_contents = (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);
bool mergerunning = false;
uint32_t mergetimer = 0;
bool rp2350a = true;
volatile BYTE SDCardStat = STA_NOINIT;
volatile unsigned int Timer1 = 0, Timer2 = 0, Timer3 = 0, Timer4 = 0, Timer5 = 0;
volatile unsigned int diskchecktimer = 0;
int64_t TimeOffsetToUptime = 1704067200;
volatile int ExtCurrentConfig[NBRPINS + 1] = {0};
uint32_t pinmask = 0;
int last_adc = 99;
bool fast_timer_active = false;
bool dmarunning = false;
bool ADCDualBuffering = false;
int LocalKeyDown[7] = {0};
int TOUCH_CS_PIN = 0;
volatile int ytileheight = 8;
volatile int X_TILE = 0;
volatile int Y_TILE = 0;
int stackoverflow = 0;
int PromptFCdummy = 0;
volatile unsigned int GPSTimer = 0;
volatile int CursorTimer = 0;
volatile short low_x = silly_low, high_x = silly_high, low_y = silly_low, high_y = silly_high;
volatile int DISPLAY_TYPE = 0;
short gui_font = 0x01;
int gui_fcolour = WHITE;
int gui_bcolour = BLACK;
short gui_font_width = 6;
short gui_font_height = 8;
short CurrentX = 0;
short CurrentY = 0;
short DisplayHRes = 0;
short DisplayVRes = 0;
short HRes = 0;
short VRes = 0;
char CMM1 = 0;
char id_out[12] = {0};
bool screen320 = false;
unsigned char *WriteBuf = NULL;
unsigned char *FrameBuf = NULL;
unsigned char *LayerBuf = NULL;
unsigned char *FRAMEBUFFER = NULL;
uint32_t heap_memory_size = 0;
uint32_t framebuffersize = 0;
uint8_t sprite_transparent = 0;
unsigned char filesource[MAXOPENFILES + 1] = {0};
unsigned char *CFunctionFlash = NULL;
unsigned char *CFunctionLibrary = NULL;
unsigned char *buff320 = NULL;
unsigned char *strtmp[MAXTEMPSTRINGS] = {0};
unsigned char *DOS_ProgMemory = NULL;
const uint8_t *flash_progmemory = NULL;
const int CMM1map[16] = {
    BLACK, BLUE, GREEN, CYAN, RED, MAGENTA, YELLOW, WHITE,
    MYRTLE, COBALT, MIDGREEN, CERULEAN, RUST, FUCHSIA, BROWN, LILAC
};
unsigned char *FontTable[FONT_TABLE_SIZE] = {0};
struct s_ctrl *Ctrl = NULL;
struct blitbuffer blitbuff[MAXBLITBUF + 1] = {0};
struct spritebuffer spritebuff[MAXBLITBUF + 1] = {0};
struct D3D *struct3d[MAX3D + 1] = {0};
s_camera camera[MAXCAM + 1] = {0};
FATFS fs;
lfs_t lfs;
lfs_dir_t lfs_dir;
struct lfs_info lfs_info;
char inpbuf[STRINGSIZE + 2] = {0};
const struct s_PinDef PinDef[] = {
    { 0, 99, "NULL", UNUSED, 99, 99},
    { 1,  0, "GP0",  DIGITAL_IN | DIGITAL_OUT | SPI0RX | UART0TX | I2C0SDA | PWM0A, 99, 0},
    { 2,  1, "GP1",  DIGITAL_IN | DIGITAL_OUT | UART0RX | I2C0SCL | PWM0B, 99, 128},
    { 3, 99, "GND",  UNUSED, 99, 99},
    { 4,  2, "GP2",  DIGITAL_IN | DIGITAL_OUT | SPI0SCK | I2C1SDA | PWM1A, 99, 1},
    { 5,  3, "GP3",  DIGITAL_IN | DIGITAL_OUT | SPI0TX | I2C1SCL | PWM1B, 99, 129},
    { 6,  4, "GP4",  DIGITAL_IN | DIGITAL_OUT | SPI0RX | UART1TX | I2C0SDA | PWM2A, 99, 2},
    { 7,  5, "GP5",  DIGITAL_IN | DIGITAL_OUT | UART1RX | I2C0SCL | PWM2B, 99, 130},
    { 8, 99, "GND",  UNUSED, 99, 99},
    { 9,  6, "GP6",  DIGITAL_IN | DIGITAL_OUT | SPI0SCK | I2C1SDA | PWM3A, 99, 3},
    {10,  7, "GP7",  DIGITAL_IN | DIGITAL_OUT | SPI0TX | I2C1SCL | PWM3B, 99, 131},
    {11,  8, "GP8",  DIGITAL_IN | DIGITAL_OUT | SPI1RX | UART1TX | I2C0SDA | PWM4A, 99, 4},
    {12,  9, "GP9",  DIGITAL_IN | DIGITAL_OUT | UART1RX | I2C0SCL | PWM4B, 99, 132},
    {13, 99, "GND",  UNUSED, 99, 99},
    {14, 10, "GP10", DIGITAL_IN | DIGITAL_OUT | SPI1SCK | I2C1SDA | PWM5A, 99, 5},
    {15, 11, "GP11", DIGITAL_IN | DIGITAL_OUT | SPI1TX | I2C1SCL | PWM5B, 99, 133},
    {16, 12, "GP12", DIGITAL_IN | DIGITAL_OUT | SPI1RX | UART0TX | I2C0SDA | PWM6A, 99, 6},
    {17, 13, "GP13", DIGITAL_IN | DIGITAL_OUT | UART0RX | I2C0SCL | PWM6B, 99, 134},
    {18, 99, "GND",  UNUSED, 99, 99},
    {19, 14, "GP14", DIGITAL_IN | DIGITAL_OUT | SPI1SCK | I2C1SDA | PWM7A, 99, 7},
    {20, 15, "GP15", DIGITAL_IN | DIGITAL_OUT | SPI1TX | I2C1SCL | PWM7B, 99, 135},
    {21, 16, "GP16", DIGITAL_IN | DIGITAL_OUT | SPI0RX | UART0TX | I2C0SDA | PWM0A, 99, 0},
    {22, 17, "GP17", DIGITAL_IN | DIGITAL_OUT | UART0RX | I2C0SCL | PWM0B, 99, 128},
    {23, 99, "GND",  UNUSED, 99, 99},
    {24, 18, "GP18", DIGITAL_IN | DIGITAL_OUT | SPI0SCK | I2C1SDA | PWM1A, 99, 1},
    {25, 19, "GP19", DIGITAL_IN | DIGITAL_OUT | SPI0TX | I2C1SCL | PWM1B, 99, 129},
    {26, 20, "GP20", DIGITAL_IN | DIGITAL_OUT | SPI0RX | UART1TX | I2C0SDA | PWM2A, 99, 2},
    {27, 21, "GP21", DIGITAL_IN | DIGITAL_OUT | UART1RX | I2C0SCL | PWM2B, 99, 130},
    {28, 99, "GND",  UNUSED, 99, 99},
    {29, 22, "GP22", DIGITAL_IN | DIGITAL_OUT | SPI0SCK | I2C1SDA | PWM3A, 99, 3},
    {30, 99, "RUN",  UNUSED, 99, 99},
    {31, 26, "GP26", DIGITAL_IN | DIGITAL_OUT | ANALOG_IN | SPI1SCK | I2C1SDA | PWM5A, 0, 5},
    {32, 27, "GP27", DIGITAL_IN | DIGITAL_OUT | ANALOG_IN | SPI1TX | I2C1SCL | PWM5B, 1, 133},
    {33, 99, "AGND", UNUSED, 99, 99},
    {34, 28, "GP28", DIGITAL_IN | DIGITAL_OUT | ANALOG_IN | SPI1RX | UART0TX | I2C0SDA | PWM6A, 2, 6},
    {35, 99, "VREF", UNUSED, 99, 99},
    {36, 99, "3V3",  UNUSED, 99, 99},
    {37, 99, "3V3E", UNUSED, 99, 99},
    {38, 99, "GND",  UNUSED, 99, 99},
    {39, 99, "VSYS", UNUSED, 99, 99},
    {40, 99, "VBUS", UNUSED, 99, 99},
    {41, 23, "GP23", DIGITAL_IN | DIGITAL_OUT | SPI0TX | I2C1SCL | PWM3B, 99, 131},
    {42, 24, "GP24", DIGITAL_IN | DIGITAL_OUT | SPI1RX | UART1TX | I2C0SDA | PWM4A, 99, 4},
    {43, 25, "GP25", DIGITAL_IN | DIGITAL_OUT | UART1RX | I2C0SCL | PWM4B, 99, 132},
    {44, 29, "GP29", DIGITAL_IN | DIGITAL_OUT | ANALOG_IN | UART0RX | I2C0SCL | PWM6B, 3, 134},
    {45, 30, "GP30", DIGITAL_IN | DIGITAL_OUT | SPI1SCK | I2C1SDA | PWM7A, 99, 7},
    {46, 31, "GP31", DIGITAL_IN | DIGITAL_OUT | SPI1TX | I2C1SCL | PWM7B, 99, 135},
    {47, 32, "GP32", DIGITAL_IN | DIGITAL_OUT | UART0TX | SPI0RX | I2C0SDA | PWM8A, 99, 8},
    {48, 33, "GP33", DIGITAL_IN | DIGITAL_OUT | UART0RX | I2C0SCL | PWM8B, 99, 136},
    {49, 34, "GP34", DIGITAL_IN | DIGITAL_OUT | SPI0SCK | I2C1SDA | PWM9A, 99, 9},
    {50, 35, "GP35", DIGITAL_IN | DIGITAL_OUT | SPI0TX | I2C1SCL | PWM9B, 99, 137},
    {51, 36, "GP36", DIGITAL_IN | DIGITAL_OUT | UART1TX | SPI0RX | I2C0SDA | PWM10A, 99, 10},
    {52, 37, "GP37", DIGITAL_IN | DIGITAL_OUT | UART1RX | I2C0SCL | PWM10B, 99, 138},
    {53, 38, "GP38", DIGITAL_IN | DIGITAL_OUT | SPI0SCK | I2C1SDA | PWM11A, 99, 11},
    {54, 39, "GP39", DIGITAL_IN | DIGITAL_OUT | SPI0TX | I2C1SCL | PWM11B, 99, 139},
    {55, 40, "GP40", DIGITAL_IN | DIGITAL_OUT | ANALOG_IN | UART1TX | SPI1RX | I2C0SDA | PWM8A, 0, 8},
    {56, 41, "GP41", DIGITAL_IN | DIGITAL_OUT | ANALOG_IN | UART1RX | I2C0SCL | PWM8B, 1, 136},
    {57, 42, "GP42", DIGITAL_IN | DIGITAL_OUT | ANALOG_IN | SPI1SCK | I2C1SDA | PWM9A, 2, 9},
    {58, 43, "GP43", DIGITAL_IN | DIGITAL_OUT | ANALOG_IN | SPI1TX | I2C1SCL | PWM9B, 3, 137},
    {59, 44, "GP44", DIGITAL_IN | DIGITAL_OUT | UART0TX | ANALOG_IN | SPI1RX | I2C0SDA | PWM10A, 4, 10},
    {60, 45, "GP45", DIGITAL_IN | DIGITAL_OUT | UART0RX | ANALOG_IN | I2C0SCL | PWM10B, 5, 138},
    {61, 46, "GP46", DIGITAL_IN | DIGITAL_OUT | ANALOG_IN | SPI1SCK | I2C1SDA | PWM11A, 6, 11},
    {62, 47, "GP47", DIGITAL_IN | DIGITAL_OUT | ANALOG_IN | SPI1TX | I2C1SCL | PWM11B, 7, 139},
};

static void vm_device_display_not_set(void) {
}

void (*DrawRectangle)(int, int, int, int, int) = (void (*)(int, int, int, int, int))vm_device_display_not_set;
void (*DrawBitmap)(int, int, int, int, int, int, int, unsigned char *) =
    (void (*)(int, int, int, int, int, int, int, unsigned char *))vm_device_display_not_set;
void (*ScrollLCD)(int) = (void (*)(int))vm_device_display_not_set;
void (*DrawBuffer)(int, int, int, int, unsigned char *) =
    (void (*)(int, int, int, int, unsigned char *))vm_device_display_not_set;
void (*ReadBuffer)(int, int, int, int, unsigned char *) =
    (void (*)(int, int, int, int, unsigned char *))vm_device_display_not_set;
void (*DrawBLITBuffer)(int, int, int, int, unsigned char *) =
    (void (*)(int, int, int, int, unsigned char *))vm_device_display_not_set;
void (*ReadBLITBuffer)(int, int, int, int, unsigned char *) =
    (void (*)(int, int, int, int, unsigned char *))vm_device_display_not_set;
void (*DrawPixel)(int, int, int) = (void (*)(int, int, int))vm_device_display_not_set;
void (*ReadBufferFast)(int, int, int, int, unsigned char *) =
    (void (*)(int, int, int, int, unsigned char *))vm_device_display_not_set;

int LCD_CS_PIN = 0;
int LCD_CD_PIN = 0;
int LCD_Reset_PIN = 0;
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
    {28, "User", 0, 0, 0, 0, 0, 0, 0}
};
extern uint16_t SPI_MOSI_PIN, SPI_MISO_PIN;
extern uint16_t SPI_CLK_PIN;
extern int CurrentSPISpeed;
extern BYTE (*xchg_byte)(BYTE data_out);
extern void (*xmit_byte_multi)(const BYTE *buff, int cnt);
extern void (*rcvr_byte_multi)(BYTE *buff, int cnt);
extern int (*SET_SPI_CLK)(int speed, int polarity, int edge);
extern BYTE HW0SwapSPI(BYTE data_out);
extern void HW0SendSPI(const BYTE *buff, int cnt);
extern void HW0ReadSPI(BYTE *buff, int cnt);
extern int HW0Clk(int speed, int polarity, int edge);
extern BYTE HW1SwapSPI(BYTE data_out);
extern void HW1SendSPI(const BYTE *buff, int cnt);
extern void HW1ReadSPI(BYTE *buff, int cnt);
extern int HW1Clk(int speed, int polarity, int edge);

static uint8_t vm_rgb_to_rgb332(int c) {
    return (uint8_t)(((c >> 16) & 0xE0) | (((c >> 11) & 0x1C)) | (((c >> 6) & 0x03)));
}

uint8_t RGB121(uint32_t c) {
    int r = (c >> 16) & 0xFF;
    int g = (c >> 8) & 0xFF;
    int b = c & 0xFF;
    return (uint8_t)((r >= 128 ? 0x8 : 0) |
                     (g >= 192 ? 0x4 : (g >= 64 ? 0x2 : 0)) |
                     (b >= 128 ? 0x1 : 0));
}

uint16_t RGB121pack(uint32_t c) {
    uint8_t v = RGB121(c);
    return (uint16_t)((v << 12) | (v << 8) | (v << 4) | v);
}

static int vm_rgb121_to_rgb(int c121) {
    return RGB121map[c121 & 0xF];
}

static void vm_softfb_set_rgb332(int x, int y, int rgb) {
    if (x < 0 || y < 0 || x >= HRes || y >= VRes) return;
    vm_display_fb[y * HRes + x] = vm_rgb_to_rgb332(rgb);
}

static int vm_softfb_get_rgb(int x, int y) {
    uint8_t px;
    if (x < 0 || y < 0 || x >= HRes || y >= VRes) return 0;
    px = vm_display_fb[y * HRes + x];
    return ((px & 0xE0) << 16) | ((px & 0x1C) << 11) | ((px & 0x03) << 6);
}

static void vm_lcd_flush_rect_from_shadow(int x1, int y1, int x2, int y2) {
    int x, y, t;
    int line_bytes;

    if (!vm_lcd_initialised) return;
    if (x2 < x1) { t = x1; x1 = x2; x2 = t; }
    if (y2 < y1) { t = y1; y1 = y2; y2 = t; }
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= HRes) x2 = HRes - 1;
    if (y2 >= VRes) y2 = VRes - 1;
    if (x2 < x1 || y2 < y1) return;

    line_bytes = (x2 - x1 + 1) * 2;
    DefineRegionSPI(x1, y1, x2, y2, 1);
    vm_lcd_set_cs();
    gpio_put(LCD_CD_PIN, GPIO_PIN_SET);
    for (y = y1; y <= y2; ++y) {
        int out = 0;
        for (x = x1; x <= x2; ++x) {
            int rgb = vm_softfb_get_rgb(x, y);
            vm_lcd_linebuf[out++] = (uint8_t)(((rgb >> 16) & 0xF8) | ((rgb >> 13) & 0x07));
            vm_lcd_linebuf[out++] = (uint8_t)(((rgb >> 5) & 0xE0) | ((rgb >> 3) & 0x1F));
        }
        vm_lcd_write_fast(vm_lcd_linebuf, line_bytes);
    }
    vm_lcd_clear_cs(Option.LCD_CS);
}

static void vm_draw_pixel_impl(int x, int y, int c) {
    uint8_t colour;
    uint8_t *pp;

    if (x < 0 || y < 0 || x >= HRes || y >= VRes) return;
    if (vm_has_active_writebuf()) {
        colour = RGB121((uint32_t)c);
        pp = WriteBuf + (y * (HRes >> 1)) + (x >> 1);
        if (x & 1) {
            *pp &= 0x0F;
            *pp |= (uint8_t)(colour << 4);
        } else {
            *pp &= 0xF0;
            *pp |= colour;
        }
        return;
    }
    vm_softfb_set_rgb332(x, y, c);
}

static void vm_lcd_clear_cs(int pin) {
    if (!pin) return;
    gpio_put(PinDef[pin].GPno, Option.DISPLAY_TYPE != ST7920 ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void vm_lcd_write_fast(const uint8_t *src, int len) {
    xmit_byte_multi(src, len);
}

static void vm_lcd_set_cs(void) {
    gpio_put(LCD_CS_PIN, GPIO_PIN_RESET);
}

void spi_write_command(unsigned char data) {
    vm_lcd_set_cs();
    gpio_put(LCD_CD_PIN, GPIO_PIN_RESET);
    vm_lcd_write_fast(&data, 1);
    vm_lcd_clear_cs(Option.LCD_CS);
}

void spi_write_data(unsigned char data) {
    vm_lcd_set_cs();
    gpio_put(LCD_CD_PIN, GPIO_PIN_SET);
    vm_lcd_write_fast(&data, 1);
    vm_lcd_clear_cs(Option.LCD_CS);
}

void spi_write_cd(unsigned char command, int data, ...) {
    int i;
    va_list ap;
    va_start(ap, data);
    spi_write_command(command);
    for (i = 0; i < data; ++i) {
        spi_write_data((unsigned char)va_arg(ap, int));
    }
    va_end(ap);
}

void ResetController(void) {
    gpio_put(LCD_Reset_PIN, GPIO_PIN_SET);
    uSec(10000);
    gpio_put(LCD_Reset_PIN, GPIO_PIN_RESET);
    uSec(10000);
    gpio_put(LCD_Reset_PIN, GPIO_PIN_SET);
    uSec(200000);
}

void DefineRegionSPI(int xstart, int ystart, int xend, int yend, int rw) {
    uint8_t coord[4];

    coord[0] = (uint8_t)(xstart >> 8);
    coord[1] = (uint8_t)xstart;
    coord[2] = (uint8_t)(xend >> 8);
    coord[3] = (uint8_t)xend;
    spi_write_command(ILI9341_COLADDRSET);
    vm_lcd_set_cs();
    gpio_put(LCD_CD_PIN, GPIO_PIN_SET);
    vm_lcd_write_fast(coord, 4);
    vm_lcd_clear_cs(Option.LCD_CS);

    coord[0] = (uint8_t)(ystart >> 8);
    coord[1] = (uint8_t)ystart;
    coord[2] = (uint8_t)(yend >> 8);
    coord[3] = (uint8_t)yend;
    spi_write_command(ILI9341_PAGEADDRSET);
    vm_lcd_set_cs();
    gpio_put(LCD_CD_PIN, GPIO_PIN_SET);
    vm_lcd_write_fast(coord, 4);
    vm_lcd_clear_cs(Option.LCD_CS);

    spi_write_command(rw ? ILI9341_MEMORYWRITE : ILI9341_RAMRD);
}

static void vm_lcd_draw_rect_physical(int x1, int y1, int x2, int y2, int c) {
    int t;
    int line_bytes;
    int row;
    uint8_t hi;
    uint8_t lo;

    if (!vm_lcd_initialised) return;
    if (x2 <= x1) { t = x1; x1 = x2; x2 = t; }
    if (y2 <= y1) { t = y1; y1 = y2; y2 = t; }
    if (x1 < 0) x1 = 0;
    if (x1 >= HRes) x1 = HRes - 1;
    if (x2 < 0) x2 = 0;
    if (x2 >= HRes) x2 = HRes - 1;
    if (y1 < 0) y1 = 0;
    if (y1 >= VRes) y1 = VRes - 1;
    if (y2 < 0) y2 = 0;
    if (y2 >= VRes) y2 = VRes - 1;

    line_bytes = (x2 - x1 + 1) * 2;
    hi = (uint8_t)(((c >> 16) & 0xF8) | ((c >> 13) & 0x07));
    lo = (uint8_t)(((c >> 5) & 0xE0) | ((c >> 3) & 0x1F));
    for (t = 0; t < line_bytes; t += 2) {
        vm_lcd_linebuf[t] = hi;
        vm_lcd_linebuf[t + 1] = lo;
    }

    DefineRegionSPI(x1, y1, x2, y2, 1);
    vm_lcd_set_cs();
    gpio_put(LCD_CD_PIN, GPIO_PIN_SET);
    for (row = y1; row <= y2; ++row) {
        vm_lcd_write_fast(vm_lcd_linebuf, line_bytes);
    }
    vm_lcd_clear_cs(Option.LCD_CS);
}

static void vm_draw_rectangle_screen_impl(int x1, int y1, int x2, int y2, int c) {
    int x, y, t;

    if (x2 < x1) { t = x1; x1 = x2; x2 = t; }
    if (y2 < y1) { t = y1; y1 = y2; y2 = t; }
    for (y = y1; y <= y2; ++y) {
        for (x = x1; x <= x2; ++x) {
            vm_softfb_set_rgb332(x, y, c);
        }
    }
    vm_lcd_draw_rect_physical(x1, y1, x2, y2, c);
}

static void vm_draw_buffer_impl(int x1, int y1, int x2, int y2, unsigned char *p) {
    int x, y, t;
    union {
        char rgbbytes[4];
        unsigned int rgb;
    } c;

    if (x2 <= x1) { t = x1; x1 = x2; x2 = t; }
    if (y2 <= y1) { t = y1; y1 = y2; y2 = t; }
    if (x1 < 0) x1 = 0;
    if (x2 >= HRes) x2 = HRes - 1;
    if (y1 < 0) y1 = 0;
    if (y2 >= VRes) y2 = VRes - 1;
    for (y = y1; y <= y2; ++y) {
        for (x = x1; x <= x2; ++x) {
            c.rgbbytes[0] = *p++;
            c.rgbbytes[1] = *p++;
            c.rgbbytes[2] = *p++;
            vm_draw_pixel_impl(x, y, (int)c.rgb);
        }
    }
}

static void vm_read_buffer_impl(int x1, int y1, int x2, int y2, unsigned char *c) {
    int x, y, t;
    if (x2 <= x1) { t = x1; x1 = x2; x2 = t; }
    if (y2 <= y1) { t = y1; y1 = y2; y2 = t; }
    for (y = y1; y <= y2; ++y) {
        for (x = x1; x <= x2; ++x) {
            int rgb;
            if (vm_has_active_writebuf()) {
                uint8_t *pp = WriteBuf + (y * (HRes >> 1)) + (x >> 1);
                rgb = vm_rgb121_to_rgb((x & 1) ? ((*pp >> 4) & 0xF) : (*pp & 0xF));
            } else {
                rgb = vm_softfb_get_rgb(x, y);
            }
            *c++ = rgb & 0xFF;
            *c++ = (rgb >> 8) & 0xFF;
            *c++ = (rgb >> 16) & 0xFF;
        }
    }
}

void copyframetoscreen(uint8_t *s, int xstart, int xend, int ystart, int yend, int odd) {
    int x, y;
    low_x = xstart;
    high_x = xend;
    low_y = ystart;
    high_y = yend;
    for (y = ystart; y <= yend; ++y) {
        for (x = xstart; x <= xend; ++x) {
            int rgb;
            if (odd) {
                rgb = vm_rgb121_to_rgb((*s >> 4) & 0xF);
                ++s;
            } else {
                rgb = vm_rgb121_to_rgb(*s & 0xF);
            }
            odd ^= 1;
            vm_softfb_set_rgb332(x, y, rgb);
        }
        odd = 0;
    }
}

void restorepanel(void) {
}

void setframebuffer(void) {
}

void Display_Refresh(void) {
}

void DrawPixel16(int x, int y, int c) {
    vm_draw_pixel_impl(x, y, c);
}

static void vm_fill_rect_pixels(int x1, int y1, int x2, int y2, int c) {
    int x, y, t;
    if (x2 < x1) { t = x1; x1 = x2; x2 = t; }
    if (y2 < y1) { t = y1; y1 = y2; y2 = t; }
    for (y = y1; y <= y2; ++y)
        for (x = x1; x <= x2; ++x)
            vm_draw_pixel_impl(x, y, c);
}

void DrawRectangle16(int x1, int y1, int x2, int y2, int c) {
    vm_fill_rect_pixels(x1, y1, x2, y2, c);
}

void DrawBitmap16(int x1, int y1, int width, int height, int scale, int fc, int bc, unsigned char *bitmap) {
    (void)x1; (void)y1; (void)width; (void)height; (void)scale; (void)fc; (void)bc; (void)bitmap;
}

void ScrollLCD16(int lines) {
    (void)lines;
}

void DrawBuffer16(int x1, int y1, int x2, int y2, unsigned char *p) {
    vm_draw_buffer_impl(x1, y1, x2, y2, p);
}

void DrawBuffer16Fast(int x1, int y1, int x2, int y2, int blank, unsigned char *p) {
    (void)blank;
    vm_draw_buffer_impl(x1, y1, x2, y2, p);
}

void ReadBuffer16(int x1, int y1, int x2, int y2, unsigned char *c) {
    vm_read_buffer_impl(x1, y1, x2, y2, c);
}

void ReadBuffer16Fast(int x1, int y1, int x2, int y2, unsigned char *c) {
    vm_read_buffer_impl(x1, y1, x2, y2, c);
}

void DrawPixelNormal(int x, int y, int c) {
    if (vm_has_active_writebuf() || DrawRectangle == (void (*)(int, int, int, int, int))vm_device_display_not_set) {
        vm_draw_pixel_impl(x, y, c);
        return;
    }
    DrawRectangle(x, y, x, y, c);
}

void ReadBuffer2(int x1, int y1, int x2, int y2, unsigned char *c) {
    vm_read_buffer_impl(x1, y1, x2, y2, c);
}

void copybuffertoscreen(unsigned char *s, int lowx, int lowy, int highx, int highy) {
    vm_draw_buffer_impl(lowx, lowy, highx, highy, s);
}

int GetFontWidth(int fnt) {
    return FontTable[fnt >> 4][0] * (fnt & 0x0F);
}

int GetFontHeight(int fnt) {
    return FontTable[fnt >> 4][1] * (fnt & 0x0F);
}

void initFonts(void) {
    FontTable[0] = (unsigned char *)font1;
    FontTable[1] = (unsigned char *)Misc_12x20_LE;
#if defined(PICOMITEWEB)
    FontTable[2] = (unsigned char *)arial_bold;
#else
    FontTable[2] = (unsigned char *)Hom_16x24_LE;
#endif
    FontTable[3] = (unsigned char *)Fnt_10x16;
    FontTable[4] = (unsigned char *)Inconsola;
    FontTable[5] = (unsigned char *)ArialNumFontPlus;
    FontTable[6] = (unsigned char *)F_6x8_LE;
    FontTable[7] = (unsigned char *)TinyFont;
    FontTable[8] = (unsigned char *)font8x10;
}

void SetFont(int fnt) {
    if (FontTable[fnt >> 4] == NULL) error("Invalid font number");
    gui_font = fnt;
    gui_font_width = GetFontWidth(fnt);
    gui_font_height = GetFontHeight(fnt);
    Option.Height = gui_font_height ? (VRes / gui_font_height) : 0;
    Option.Width = gui_font_width ? (HRes / gui_font_width) : 0;
}

void ClearScreen(int c) {
    int x, y;
    if (vm_has_active_writebuf()) {
        memset(WriteBuf, (RGB121((uint32_t)c) & 0xF) * 0x11, (size_t)(HRes * VRes / 2));
    } else {
        if (vm_lcd_initialised) {
            vm_lcd_draw_rect_physical(0, 0, HRes - 1, VRes - 1, c);
        }
        for (y = 0; y < VRes; ++y)
            for (x = 0; x < HRes; ++x)
                vm_softfb_set_rgb332(x, y, c);
    }
    CurrentX = 0;
    CurrentY = 0;
}

void DrawLine(int x1, int y1, int x2, int y2, int w, int c) {
    if (y1 == y2 && w > 0) {
        vm_fill_rect_pixels(x1, y1, x2, y2 + w - 1, c);
        return;
    }
    if (x1 == x2 && w > 0) {
        vm_fill_rect_pixels(x1, y1, x2 + w - 1, y2, c);
        return;
    }
    if (w == 1 || w == -1) {
        int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
        int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
        int err = dx + dy;
        while (1) {
            vm_draw_pixel_impl(x1, y1, c);
            if (x1 == x2 && y1 == y2) break;
            int e2 = 2 * err;
            if (e2 >= dy) {
                if (x1 == x2) break;
                err += dy;
                x1 += sx;
            }
            if (e2 <= dx) {
                if (y1 == y2) break;
                err += dx;
                y1 += sy;
            }
        }
        return;
    }
    DrawLine(x1, y1, x2, y2, 1, c);
}

void DrawBox(int x1, int y1, int x2, int y2, int w, int c, int fill) {
    if (fill >= 0) vm_fill_rect_pixels(x1, y1, x2, y2, fill);
    if (w > 0) {
        for (int i = 0; i < w; ++i) {
            DrawLine(x1 + i, y1 + i, x2 - i, y1 + i, 1, c);
            DrawLine(x1 + i, y2 - i, x2 - i, y2 - i, 1, c);
            DrawLine(x1 + i, y1 + i, x1 + i, y2 - i, 1, c);
            DrawLine(x2 - i, y1 + i, x2 - i, y2 - i, 1, c);
        }
    }
}

void ResetDisplay(void) {
    ClearScreen(Option.DefaultBC);
}

void ShowCursor(int show) {
    (void)show;
}

static int vm_console_font_has_char(const unsigned char *font, int ch) {
    return font && ch >= font[2] && ch < font[2] + font[3];
}

static const unsigned char *vm_console_font_glyph(const unsigned char *font, int ch) {
    int width = font[0];
    int height = font[1];
    return font + 4 + (int)(((ch - font[2]) * height * width) / 8);
}

static int vm_console_glyph_bit(const unsigned char *glyph, int width, int height, int col, int row) {
    int bit_number = row * width + col;
    return (glyph[bit_number / 8] >> (((height * width) - bit_number - 1) % 8)) & 1;
}

static void vm_console_fill_rect(int x1, int y1, int x2, int y2, int c) {
    int x, y, t;
    if (x2 < x1) { t = x1; x1 = x2; x2 = t; }
    if (y2 < y1) { t = y1; y1 = y2; y2 = t; }
    for (y = y1; y <= y2; ++y) {
        for (x = x1; x <= x2; ++x) {
            vm_softfb_set_rgb332(x, y, c);
        }
    }
}

static void vm_console_draw_char_at(int x, int y, int fnt, int fc, int bc, int ch) {
    unsigned char *fp = FontTable[fnt >> 4];
    int scale = fnt & 0x0F;
    int width;
    int height;
    int draw_scale = scale;
    const unsigned char *glyph;
    int row, col, sx, sy;

    if (PrintPixelMode == 1) bc = -1;
    if (PrintPixelMode == 2) {
        int s = bc;
        bc = fc;
        fc = s;
    }
    if (PrintPixelMode == 5) {
        fc = bc;
        bc = -1;
    }
    if ((fnt & 0xF0) == 0x50 && (ch == '-' || ch == '+' || ch == '=')) {
        fp = FontTable[0];
        draw_scale = scale * 4;
    }
    if (!fp) return;

    width = fp[0];
    height = fp[1];
    if (bc >= 0) {
        vm_console_fill_rect(x, y, x + width * draw_scale - 1, y + height * draw_scale - 1, bc);
    }
    if (!vm_console_font_has_char(fp, ch)) return;

    glyph = vm_console_font_glyph(fp, ch);
    for (row = 0; row < height; ++row) {
        for (col = 0; col < width; ++col) {
            if (!vm_console_glyph_bit(glyph, width, height, col, row)) continue;
            for (sy = 0; sy < draw_scale; ++sy) {
                for (sx = 0; sx < draw_scale; ++sx) {
                    vm_softfb_set_rgb332(x + col * draw_scale + sx, y + row * draw_scale + sy, fc);
                }
            }
        }
    }
    vm_lcd_flush_rect_from_shadow(x, y, x + width * draw_scale - 1, y + height * draw_scale - 1);
}

void GUIPrintString(int x, int y, int fnt, int jh, int jv, int jo, int fc, int bc, char *str) {
    int len;

    if (!str) return;
    CurrentX = x;
    CurrentY = y;
    len = (int)strlen(str);
    if (jo == ORIENT_NORMAL) {
        if (jh == JUSTIFY_CENTER) CurrentX -= (len * GetFontWidth(fnt)) / 2;
        if (jh == JUSTIFY_RIGHT) CurrentX -= len * GetFontWidth(fnt);
        if (jv == JUSTIFY_MIDDLE) CurrentY -= GetFontHeight(fnt) / 2;
        if (jv == JUSTIFY_BOTTOM) CurrentY -= GetFontHeight(fnt);
    }
    while (*str) {
        vm_console_draw_char_at(CurrentX, CurrentY, fnt, fc, bc, (unsigned char)*str++);
        CurrentX += GetFontWidth(fnt);
    }
}

void DisplayPutC(char c) {
    if (!Option.DISPLAY_CONSOLE || !vm_console_display_enabled) return;
    if (c >= FontTable[gui_font >> 4][2] && c < FontTable[gui_font >> 4][2] + FontTable[gui_font >> 4][3]) {
        if (CurrentX + gui_font_width > HRes) {
            DisplayPutC('\r');
            DisplayPutC('\n');
        }
    }
    switch (c) {
        case '\b':
            CurrentX -= gui_font_width;
            if (CurrentX < 0) {
                CurrentY -= gui_font_height;
                if (CurrentY < 0) CurrentY = 0;
                CurrentX = (Option.Width > 0 ? Option.Width - 1 : 0) * gui_font_width;
            }
            return;
        case '\r':
            CurrentX = 0;
            return;
        case '\n':
            if (CurrentY + 2 * gui_font_height > VRes) {
                ClearScreen(gui_bcolour);
                CurrentX = 0;
                CurrentY = 0;
            } else {
                CurrentY += gui_font_height;
            }
            return;
        case '\t':
            do {
                DisplayPutC(' ');
            } while ((CurrentX / gui_font_width) % Option.Tab);
            return;
        default:
            break;
    }
    vm_console_draw_char_at(CurrentX, CurrentY, gui_font, gui_fcolour, gui_bcolour, (unsigned char)c);
    CurrentX += gui_font_width;
}

void DisplayNotSet(void) {
    error("Display not configured");
}

void InitDisplaySPI(int InitOnly) {
    if (Option.DISPLAY_TYPE != ST7796SP) return;

    DisplayHRes = display_details[Option.DISPLAY_TYPE].horizontal;
    DisplayVRes = display_details[Option.DISPLAY_TYPE].vertical;
    HRes = DisplayHRes;
    VRes = DisplayVRes;
    if (PinDef[Option.SYSTEM_CLK].mode & SPI0SCK) {
        xchg_byte = HW0SwapSPI;
        xmit_byte_multi = HW0SendSPI;
        rcvr_byte_multi = HW0ReadSPI;
        SET_SPI_CLK = HW0Clk;
    } else {
        xchg_byte = HW1SwapSPI;
        xmit_byte_multi = HW1SendSPI;
        rcvr_byte_multi = HW1ReadSPI;
        SET_SPI_CLK = HW1Clk;
    }
    SET_SPI_CLK(display_details[Option.DISPLAY_TYPE].speed,
                display_details[Option.DISPLAY_TYPE].CPOL,
                display_details[Option.DISPLAY_TYPE].CPHASE);

    DrawRectangle = vm_draw_rectangle_screen_impl;
    DrawPixel = DrawPixelNormal;
    vm_lcd_initialised = 1;

    ResetController();
    spi_write_cd(0xC5, 1, 0x1C);
    spi_write_cd(0x3A, 1, 0x55);
    spi_write_command(0xB0);
    uSec(150000);
    spi_write_cd(0xB4, 1, 0x01);
    if (Option.BGR) spi_write_command(0x21);
    else spi_write_command(0x20);
    spi_write_cd(0xB6, 3, 0x80, 0x02, 0x3B);
    spi_write_cd(0xB7, 1, 0xC6);
    spi_write_cd(0xF0, 1, 0xC3);
    spi_write_cd(0xF0, 1, 0x96);
    switch (Option.DISPLAY_ORIENTATION) {
        case LANDSCAPE:  spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Landscape); break;
        case PORTRAIT:   spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Portrait); break;
        case RLANDSCAPE: spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Landscape180); break;
        case RPORTRAIT:  spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Portrait180); break;
        default:         spi_write_cd(ILI9341_MEMCONTROL, 1, ILI9341_Portrait); break;
    }
    spi_write_cd(0x33, 6, 0x00, 0x00, 0x01, 0x40, 0x00, 0xA0);
    spi_write_command(0x11);
    uSec(150000);
    spi_write_command(0x29);
    uSec(150000);

    if (!InitOnly) {
        vm_lcd_draw_rect_physical(0, 0, HRes - 1, VRes - 1, RED);
        uSec(250000);
        vm_lcd_draw_rect_physical(0, 0, HRes - 1, VRes - 1, MAGENTA);
        uSec(250000);
    }
}

int GetLineILI9341(void) {
    return 0;
}

void HideAllControls(void) {
}

int GetJustification(char *p, int *jh, int *jv, int *jo) {
    (void)p;
    *jh = JUSTIFY_LEFT;
    *jv = JUSTIFY_TOP;
    *jo = ORIENT_NORMAL;
    return 1;
}

int rgb(int r, int g, int b) {
    return RGB(r, g, b);
}

void cmd_guiBasic(void) {}
void cmd_guiMX170(void) {}
void DrawTriangle(int x0, int y0, int x1, int y1, int x2, int y2, int c, int fill) {
    (void)x0; (void)y0; (void)x1; (void)y1; (void)x2; (void)y2; (void)c; (void)fill;
}
int getColour(char *c, int minus) {
    (void)c; (void)minus;
    return gui_fcolour;
}
void setmode(int mode, bool clear) {
    (void)mode;
    if (clear) ClearScreen(Option.DefaultBC);
}
void UnloadFont(int a) { (void)a; }

void uSec(int us) {
    sleep_us((uint64_t)us);
}

uint64_t readusclock(void) {
    return time_us_64() - timeroffset;
}

void PRet(void) {
    MMPrintString("\r\n");
}

void PInt(int64_t n) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)n);
    MMPrintString(buf);
}

void PIntComma(int64_t n) {
    PInt(n);
    MMPrintString(", ");
}

void SRet(void) {}
void SInt(int64_t n) { (void)n; }
void SIntComma(int64_t n) { (void)n; }
void PIntH(unsigned long long int n) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%llX", n);
    MMPrintString(buf);
}
void PIntB(unsigned long long int n) { (void)n; }
void PIntBC(unsigned long long int n) { (void)n; }
void PIntHC(unsigned long long int n) { (void)n; }
void PFlt(MMFLOAT flt) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%g", (double)flt);
    MMPrintString(buf);
}
void PFltComma(MMFLOAT n) { PFlt(n); MMPrintString(", "); }

char SerialConsolePutC(char c, int flush) {
    (void)flush;
    putchar_raw(c);
    return c;
}

void putConsole(int c, int flush) {
    (void)flush;
    putchar_raw((char)c);
}

int getConsole(void) {
    int c = getchar_timeout_us(0);
    return c == PICO_ERROR_TIMEOUT ? -1 : c;
}

void MMPrintString(char *s) {
    while (*s) MMputchar(*s++, 1);
}

void SSPrintString(char *s) {
    MMPrintString(s);
}

void myprintf(char *s) {
    MMPrintString(s);
}

void Mstrcpy(unsigned char *dest, unsigned char *src) {
    strcpy((char *)dest, (const char *)src);
}

int Mstrcmp(unsigned char *s1, unsigned char *s2) {
    return strcmp((const char *)s1, (const char *)s2);
}

long long int FloatToInt64(MMFLOAT x) {
    return (long long)x;
}

void IntToStr(char *strr, long long int nbr, unsigned int base) {
    if (base == 16) snprintf(strr, STRINGSIZE, "%llX", nbr);
    else snprintf(strr, STRINGSIZE, "%lld", nbr);
}

void FloatToStr(char *p, MMFLOAT f, int m, int n, unsigned char ch) {
    (void)m; (void)n; (void)ch;
    snprintf(p, STRINGSIZE, "%g", (double)f);
}

void ClearVars(int level, bool all) {
    (void)level;
    (void)all;
}

void error(char *msg, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, msg);
    vsnprintf(buf, sizeof(buf), msg, ap);
    va_end(ap);
    MMPrintString("\r\n");
    MMPrintString(buf);
    MMPrintString("\r\n");
    longjmp(mark, 1);
}

int check_interrupt(void) {
    return 0;
}

int MMInkey(void) {
    int c = read_i2c_kbd();
    if (c >= 0) return c;
    c = getchar_timeout_us(0);
    return c == PICO_ERROR_TIMEOUT ? -1 : c;
}

uint64_t gettimefromepoch(int *year, int *month, int *day, int *hour, int *minute, int *second) {
    time_t epochnow = (time_t)(time_us_64() / 1000000ULL + TimeOffsetToUptime);
    struct tm *tm = gmtime(&epochnow);
    if (tm == NULL) return 0;
    if (year) *year = tm->tm_year + 1900;
    if (month) *month = tm->tm_mon + 1;
    if (day) *day = tm->tm_mday;
    if (hour) *hour = tm->tm_hour;
    if (minute) *minute = tm->tm_min;
    if (second) *second = tm->tm_sec;
    return (uint64_t)epochnow * 1000000ULL;
}

int MMgetchar(void) {
    int c;
    do {
        c = MMInkey();
        tight_loop_contents();
    } while (c < 0);
    return c;
}

char MMputchar(char c, int flush) {
    (void)flush;
    putchar_raw(c);
    DisplayPutC(c);
    return c;
}

void CheckAbort(void) {
    int c = MMInkey();
    if (c == ESC) longjmp(mark, 1);
}

extern BYTE (*xchg_byte)(BYTE data_out);
extern void (*xmit_byte_multi)(const BYTE *buff, int cnt);
extern void (*rcvr_byte_multi)(BYTE *buff, int cnt);
extern int (*SET_SPI_CLK)(int speed, int polarity, int edge);
extern uint16_t SPI_CLK_PIN, SD_CLK_PIN;
extern int SD_SPI_SPEED;
extern BYTE BitBangSwapSPI(BYTE data_out);
extern void BitBangSendSPI(const BYTE *buff, int cnt);
extern void BitBangReadSPI(BYTE *buff, int cnt);
extern int BitBangSetClk(int speed, int polarity, int edge);
extern BYTE HW0SwapSPI(BYTE data_out);
extern void HW0SendSPI(const BYTE *buff, int cnt);
extern void HW0ReadSPI(BYTE *buff, int cnt);
extern int HW0Clk(int speed, int polarity, int edge);
extern BYTE HW1SwapSPI(BYTE data_out);
extern void HW1SendSPI(const BYTE *buff, int cnt);
extern void HW1ReadSPI(BYTE *buff, int cnt);
extern int HW1Clk(int speed, int polarity, int edge);

void SPISpeedSet(int device) {
    (void)device;
    if (PinDef[Option.SYSTEM_CLK].mode & SPI0SCK &&
        PinDef[Option.SYSTEM_MOSI].mode & SPI0TX &&
        PinDef[Option.SYSTEM_MISO].mode & SPI0RX) {
        xchg_byte = HW0SwapSPI;
        xmit_byte_multi = HW0SendSPI;
        rcvr_byte_multi = HW0ReadSPI;
        SET_SPI_CLK = HW0Clk;
    } else if (PinDef[Option.SYSTEM_CLK].mode & SPI1SCK &&
               PinDef[Option.SYSTEM_MOSI].mode & SPI1TX &&
               PinDef[Option.SYSTEM_MISO].mode & SPI1RX) {
        xchg_byte = HW1SwapSPI;
        xmit_byte_multi = HW1SendSPI;
        rcvr_byte_multi = HW1ReadSPI;
        SET_SPI_CLK = HW1Clk;
    } else {
        xchg_byte = BitBangSwapSPI;
        xmit_byte_multi = BitBangSendSPI;
        rcvr_byte_multi = BitBangReadSPI;
        SET_SPI_CLK = BitBangSetClk;
    }
    SPI_CLK_PIN = SD_CLK_PIN;
    if (SET_SPI_CLK) SET_SPI_CLK(SD_SPI_SPEED, false, false);
}

void bc_fastgfx_swap(void) {
}

void bc_fastgfx_sync(void) {
    uint64_t now;
    uint64_t frame_us;
    if (!vm_fastgfx_active || vm_fastgfx_fps <= 0) return;
    frame_us = 1000000ULL / (uint64_t)vm_fastgfx_fps;
    now = time_us_64();
    if (vm_fastgfx_next_sync_us == 0) vm_fastgfx_next_sync_us = now + frame_us;
    if (now < vm_fastgfx_next_sync_us) sleep_us(vm_fastgfx_next_sync_us - now);
    vm_fastgfx_next_sync_us += frame_us;
}

void bc_fastgfx_create(void) {
    vm_fastgfx_active = 1;
    vm_fastgfx_next_sync_us = 0;
}

void bc_fastgfx_close(void) {
    vm_fastgfx_active = 0;
    vm_fastgfx_next_sync_us = 0;
}

void bc_fastgfx_reset(void) {
    vm_fastgfx_active = 0;
    vm_fastgfx_next_sync_us = 0;
}

void bc_fastgfx_set_fps(int fps) {
    if (fps < 1 || fps > 1000) error("Number out of bounds");
    vm_fastgfx_fps = fps;
    vm_fastgfx_next_sync_us = 0;
}

void EditInputLine(void) {
}

void SaveProgramToFlash(unsigned char *pm, int msg) {
    (void)pm; (void)msg;
}

void SPIClose(void) {}
void SPI2Close(void) {}
void CallExecuteProgram(char *p) { (void)p; }
long long int *GetReceiveDataBuffer(unsigned char *p, unsigned int *nbr) {
    (void)p; (void)nbr;
    return NULL;
}

void disable_interrupts_pico(void) {}
void enable_interrupts_pico(void) {}
void ClearSavedVars(void) {}
void ResetAllFlash(void) {}
void ResetOptions(bool startup) { (void)startup; }
void FlashWriteBlock(void) {}
void FlashWriteWord(unsigned int i) { (void)i; }
void FlashWriteByte(unsigned char b) { (void)b; }
void FlashWriteAlign(void) {}
void FlashWriteClose(void) {}
void FlashWriteInit(int region) { (void)region; }
void FlashSetAddress(int address) { (void)address; }
void positionfile(int fnbr, int idx) { (void)fnbr; (void)idx; }
void ErrorCheck(int fnbr) { (void)fnbr; }
int ForceFileClose(int fnbr) { (void)fnbr; return 0; }
int FileLoadProgram(unsigned char *fname, bool chain) { (void)fname; (void)chain; return 0; }
int FileLoadSourceProgram(unsigned char *fname, char **source_out) { (void)fname; (void)source_out; return 0; }
int FileLoadCMM2Program(char *fname, bool message) { (void)fname; (void)message; return 0; }
int FileEOF(int fnbr) { (void)fnbr; return 1; }
int MMfgetc(int filenbr) { (void)filenbr; return -1; }
void MMgetline(int filenbr, char *p) { (void)filenbr; if (p) *p = 0; }
void CheckSDCard(void) {}

static void vm_device_init_option_defaults(void) {
    memset(&Option, 0, sizeof(Option));
    Option.Tab = 4;
    Option.continuation = '_';
    Option.DefaultFC = WHITE;
    Option.DefaultBC = BLACK;
    Option.DefaultFont = 0x01;
    Option.Refresh = 1;
    Option.CPU_Speed = 252000;
    Option.FlashSize = PICO_FLASH_SIZE_BYTES;
    Option.DISPLAY_TYPE = ILI9488P;
    Option.DISPLAY_CONSOLE = 1;
    Option.KeyboardConfig = CONFIG_I2C;
#ifdef PICOCALC
    Option.LOCAL_KEYBOARD = 0;
    Option.SYSTEM_CLK = 14;
    Option.SYSTEM_MOSI = 15;
    Option.SYSTEM_MISO = 16;
    Option.SYSTEM_I2C_SDA = 9;
    Option.SYSTEM_I2C_SCL = 10;
    Option.SYSTEM_I2C_SLOW = 1;
    Option.AUDIO_L = 31;
    Option.AUDIO_R = 32;
    Option.AUDIO_SLICE = 5;
    Option.AUDIO_CLK_PIN = 0;
    Option.AUDIO_MOSI_PIN = 0;
    Option.AUDIO_DCS_PIN = 0;
    Option.AUDIO_DREQ_PIN = 0;
    Option.AUDIO_RESET_PIN = 0;
    Option.DefaultFC = GREEN;
    Option.BGR = 1;
    Option.BackLightLevel = 20;
    Option.ColourCode = 1;
    Option.DISPLAY_TYPE = ST7796SP;
    Option.DISPLAY_BL = 0;
    Option.DISPLAY_ORIENTATION = PORTRAIT;
    Option.LCD_CD = 19;
    Option.LCD_Reset = 20;
    Option.LCD_CS = 17;
    Option.LCD_CLK = Option.SYSTEM_CLK;
    Option.LCD_MOSI = Option.SYSTEM_MOSI;
    Option.LCD_MISO = Option.SYSTEM_MISO;
    Option.TOUCH_CS = 0;
    Option.TOUCH_IRQ = 0;
    Option.CombinedCS = 0;
    Option.SD_CS = 22;
    Option.SD_CLK_PIN = 24;
    Option.SD_MOSI_PIN = 25;
    Option.SD_MISO_PIN = 21;
    Option.SerialConsole = 1;
    Option.SerialTX = 1;
    Option.SerialRX = 2;
    strcpy((char *)Option.platform, "PicoCalc");
#endif
}

static void vm_device_init_reserved_io(void) {
    if (Option.LCD_CD) {
        LCD_CD_PIN = PinDef[Option.LCD_CD].GPno;
        LCD_CS_PIN = PinDef[Option.LCD_CS].GPno;
        LCD_Reset_PIN = PinDef[Option.LCD_Reset].GPno;
        gpio_init(LCD_CD_PIN);
        gpio_put(LCD_CD_PIN, Option.DISPLAY_TYPE != ST7920 ? GPIO_PIN_SET : GPIO_PIN_RESET);
        gpio_set_dir(LCD_CD_PIN, GPIO_OUT);
        gpio_init(LCD_CS_PIN);
        gpio_set_drive_strength(LCD_CS_PIN, GPIO_DRIVE_STRENGTH_8MA);
        if (Option.DISPLAY_TYPE != ST7920) {
            gpio_put(LCD_CS_PIN, GPIO_PIN_SET);
            gpio_set_dir(LCD_CS_PIN, GPIO_OUT);
        }
        gpio_init(LCD_Reset_PIN);
        gpio_put(LCD_Reset_PIN, GPIO_PIN_RESET);
        gpio_set_dir(LCD_Reset_PIN, GPIO_OUT);
        CurrentSPISpeed = NONE_SPI_SPEED;
    }
    if (Option.SYSTEM_I2C_SDA) {
        gpio_set_function(PinDef[Option.SYSTEM_I2C_SCL].GPno, GPIO_FUNC_I2C);
        gpio_set_function(PinDef[Option.SYSTEM_I2C_SDA].GPno, GPIO_FUNC_I2C);
        if (PinDef[Option.SYSTEM_I2C_SDA].mode & I2C0SDA) {
            i2c_init(i2c0, (Option.SYSTEM_I2C_SLOW ? 10000 : 400000));
        } else {
            i2c_init(i2c1, (Option.SYSTEM_I2C_SLOW ? 10000 : 400000));
        }
        gpio_pull_up(PinDef[Option.SYSTEM_I2C_SCL].GPno);
        gpio_pull_up(PinDef[Option.SYSTEM_I2C_SDA].GPno);
    }
    if (Option.SYSTEM_CLK) {
        SPI_CLK_PIN = PinDef[Option.SYSTEM_CLK].GPno;
        SPI_MOSI_PIN = PinDef[Option.SYSTEM_MOSI].GPno;
        SPI_MISO_PIN = PinDef[Option.SYSTEM_MISO].GPno;
        if (PinDef[Option.SYSTEM_CLK].mode & SPI0SCK &&
            PinDef[Option.SYSTEM_MOSI].mode & SPI0TX &&
            PinDef[Option.SYSTEM_MISO].mode & SPI0RX) {
            SET_SPI_CLK = HW0Clk;
        } else if (PinDef[Option.SYSTEM_CLK].mode & SPI1SCK &&
                   PinDef[Option.SYSTEM_MOSI].mode & SPI1TX &&
                   PinDef[Option.SYSTEM_MISO].mode & SPI1RX) {
            SET_SPI_CLK = HW1Clk;
        } else {
            SET_SPI_CLK = BitBangSetClk;
        }
        gpio_init(SPI_CLK_PIN);
        gpio_set_drive_strength(SPI_CLK_PIN, GPIO_DRIVE_STRENGTH_8MA);
        gpio_put(SPI_CLK_PIN, GPIO_PIN_RESET);
        gpio_set_dir(SPI_CLK_PIN, GPIO_OUT);
        gpio_set_slew_rate(SPI_CLK_PIN, GPIO_SLEW_RATE_FAST);
        gpio_init(SPI_MOSI_PIN);
        gpio_set_drive_strength(SPI_MOSI_PIN, GPIO_DRIVE_STRENGTH_8MA);
        gpio_put(SPI_MOSI_PIN, GPIO_PIN_RESET);
        gpio_set_dir(SPI_MOSI_PIN, GPIO_OUT);
        gpio_set_slew_rate(SPI_MOSI_PIN, GPIO_SLEW_RATE_FAST);
        gpio_init(SPI_MISO_PIN);
        gpio_set_pulls(SPI_MISO_PIN, true, false);
        gpio_set_dir(SPI_MISO_PIN, GPIO_IN);
        gpio_set_input_hysteresis_enabled(SPI_MISO_PIN, true);
        CurrentSPISpeed = NONE_SPI_SPEED;
    }
    if (Option.SD_CS || Option.CombinedCS) {
        if (!Option.CombinedCS) {
            SD_CS_PIN = PinDef[Option.SD_CS].GPno;
            gpio_init(SD_CS_PIN);
            gpio_set_drive_strength(SD_CS_PIN, GPIO_DRIVE_STRENGTH_8MA);
            gpio_put(SD_CS_PIN, GPIO_PIN_SET);
            gpio_set_dir(SD_CS_PIN, GPIO_OUT);
            gpio_set_slew_rate(SD_CS_PIN, GPIO_SLEW_RATE_SLOW);
        }
        CurrentSPISpeed = NONE_SPI_SPEED;
        if (Option.SD_CLK_PIN) {
            SD_CLK_PIN = PinDef[Option.SD_CLK_PIN].GPno;
            SD_MOSI_PIN = PinDef[Option.SD_MOSI_PIN].GPno;
            SD_MISO_PIN = PinDef[Option.SD_MISO_PIN].GPno;
            gpio_init(SD_CLK_PIN);
            gpio_set_drive_strength(SD_CLK_PIN, GPIO_DRIVE_STRENGTH_8MA);
            gpio_put(SD_CLK_PIN, GPIO_PIN_RESET);
            gpio_set_dir(SD_CLK_PIN, GPIO_OUT);
            gpio_set_slew_rate(SD_CLK_PIN, GPIO_SLEW_RATE_FAST);
            gpio_init(SD_MOSI_PIN);
            gpio_set_drive_strength(SD_MOSI_PIN, GPIO_DRIVE_STRENGTH_8MA);
            gpio_put(SD_MOSI_PIN, GPIO_PIN_RESET);
            gpio_set_dir(SD_MOSI_PIN, GPIO_OUT);
            gpio_set_slew_rate(SD_MOSI_PIN, GPIO_SLEW_RATE_FAST);
            gpio_init(SD_MISO_PIN);
            gpio_set_pulls(SD_MISO_PIN, true, false);
            gpio_set_dir(SD_MISO_PIN, GPIO_IN);
            gpio_set_input_hysteresis_enabled(SD_MISO_PIN, true);
            xchg_byte = BitBangSwapSPI;
            xmit_byte_multi = BitBangSendSPI;
            rcvr_byte_multi = BitBangReadSPI;
        } else {
            SD_CLK_PIN = SPI_CLK_PIN;
            SD_MOSI_PIN = SPI_MOSI_PIN;
            SD_MISO_PIN = SPI_MISO_PIN;
        }
    }
}

void LoadOptions(void) {
    if (Option.Magic == MagicKey) return;
    vm_device_init_option_defaults();
    Option.Magic = MagicKey;
}

void SaveOptions(void) {
}

void InitHeap(bool all) {
    (void)all;
    bc_alloc_reset();
}

void m_alloc(int type) {
    (void)type;
}

void ClearTempMemory(void) {}
void ClearSpecificTempMemory(void *addr) { (void)addr; }
void TestStackOverflow(void) {}
unsigned char *HeapBottom(void) { return NULL; }
int FreeSpaceOnHeap(void) { return (int)(bc_alloc_bytes_capacity() - bc_alloc_bytes_used()); }
int LargestContiguousHeap(void) { return FreeSpaceOnHeap(); }
void *ReAllocMemory(void *addr, size_t msize) { (void)addr; (void)msize; return NULL; }
void FreeMemorySafe(void **addr) { if (addr) *addr = NULL; }
void *GetAlignedMemory(int size) { (void)size; return NULL; }
int MemSize(void *addr) { (void)addr; return 0; }
unsigned char *MMHeap = NULL;

void vm_device_runtime_init(void) {
    int i;
    if (vm_soft_display_initialised) return;
    vm_soft_display_initialised = 1;
    stdio_init_all();
    sleep_ms(750);
    vm_boot_log("VMBOOT 1 runtime_init");
    vm_boot_us = time_us_64();
    timeroffset = vm_boot_us;
    ticks_per_second = clock_get_hz(clk_sys);
    adc_init();
    adc_set_temp_sensor_enabled(true);
    add_repeating_timer_us(-1000, vm_timer_callback, NULL, &vm_tick_timer);
    exception_set_exclusive_handler(HARDFAULT_EXCEPTION, vm_sigbus);
    exception_set_exclusive_handler(SVCALL_EXCEPTION, vm_sigbus);
    exception_set_exclusive_handler(PENDSV_EXCEPTION, vm_sigbus);
    exception_set_exclusive_handler(NMI_EXCEPTION, vm_sigbus);
    exception_set_exclusive_handler(SYSTICK_EXCEPTION, vm_sigbus);
    {
        uint64_t t0 = time_us_64();
        while (!tud_cdc_connected() && time_us_64() - t0 < 2000000) {
            sleep_ms(10);
        }
    }
    vm_boot_log("VMBOOT 2 load_options");
    LoadOptions();
    initFonts();
    vm_boot_log("VMBOOT 3 reserved_io");
    vm_device_init_reserved_io();
    vm_boot_log("VMBOOT 4 init_display");
    InitDisplaySPI(0);
    vm_boot_log("VMBOOT 5 display_inited");
    if (vm_lcd_initialised) {
        vm_lcd_draw_rect_physical(0, 0, DisplayHRes - 1, DisplayVRes - 1, GREEN);
        uSec(250000);
    }
    HRes = DisplayHRes;
    VRes = DisplayVRes;
    DISPLAY_TYPE = Option.DISPLAY_TYPE;
    PromptFC = gui_fcolour = Option.DefaultFC;
    PromptBC = gui_bcolour = Option.DefaultBC;
    for (i = 0; i < 16; ++i) RGB121map[i] = CMM1map[i];
    DrawPixel = vm_draw_pixel_impl;
    DrawBuffer = vm_draw_buffer_impl;
    ReadBuffer = vm_read_buffer_impl;
    DrawBLITBuffer = vm_draw_buffer_impl;
    ReadBLITBuffer = vm_read_buffer_impl;
    ReadBufferFast = vm_read_buffer_impl;
    SetFont(Option.DefaultFont);
    memset(vm_display_fb, 0, sizeof(vm_display_fb));
    if (vm_lcd_initialised) {
        vm_lcd_draw_rect_physical(0, 0, HRes - 1, VRes - 1, BLUE);
        uSec(250000);
    }
    vm_boot_log("VMBOOT 6 init_i2c_kbd");
    init_i2c_kbd();
    vm_boot_log("VMBOOT 6a i2c_kbd_ok");
    if (vm_lcd_initialised) {
        vm_lcd_draw_rect_physical(0, 0, HRes - 1, VRes - 1, CYAN);
        uSec(250000);
    }
    if (Option.BackLightLevel > 0) {
        vm_boot_log("VMBOOT 6b set_kbd_backlight");
        set_kbd_backlight((uint8_t)((Option.BackLightLevel * 255) / 100));
        vm_boot_log("VMBOOT 6c kbd_backlight_ok");
    }
    if (vm_lcd_initialised) {
        vm_lcd_draw_rect_physical(0, 0, HRes - 1, VRes - 1, YELLOW);
        uSec(250000);
    }
    vm_boot_log("VMBOOT 6d clear");
    ClearScreen(Option.DefaultBC);
    vm_console_display_enabled = 1;
    if (vm_lcd_initialised) {
        vm_lcd_draw_rect_physical(0, 0, HRes - 1, VRes - 1, WHITE);
        uSec(250000);
        vm_lcd_draw_rect_physical(0, 0, gui_font_width * 2, gui_font_height * 2, BLACK);
    }
    vm_boot_log("VMBOOT 7 ready");
}

#endif
