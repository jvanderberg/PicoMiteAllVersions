/*
 * host_stubs.c - Stub implementations for host (macOS) build
 *
 * Provides stub definitions for symbols from hardware-dependent files
 * (Draw.c, FileIO.c, External.c, Audio.c, SPI-LCD.c, etc.) that are NOT
 * compiled for the host build.  Functions are no-ops or return 0/NULL;
 * variables are zero-initialized globals.
 */

#include "Hardware_Includes.h"
#include "bytecode.h"
#include <ctype.h>
#include <errno.h>
#include <time.h>

/* Forward declarations for output capture */
extern void (*host_output_hook)(const char *text, int len);
static void host_print(const char *s, int len);
static void host_prints(const char *s);

static uint64_t host_now_us(void);
static void host_sync_msec_timer(void);
static void host_fb_reset(int colour);
static void host_write_screenshot(const char *path);
static void host_runtime_check_timeout(void);
static int host_parse_escaped_char(const char **src);

static uint32_t *host_framebuffer = NULL;
static int host_fb_width = 320;
static int host_fb_height = 240;
static int host_runtime_timeout_ms = 0;
static uint64_t host_runtime_deadline_us = 0;
static int host_runtime_timed_out_flag = 0;
static int host_screenshot_written = 0;
static char host_screenshot_path[1024] = {0};
static char host_key_script[512] = {0};
static size_t host_key_script_len = 0;
static size_t host_key_script_pos = 0;
static int host_fastgfx_active = 0;
static int host_fastgfx_fps = 0;
static uint64_t host_fastgfx_next_sync_us = 0;
static unsigned char host_font_metrics[2] = {6, 8};

/* =========================================================================
 *  Global Variables — definitions (declared as extern in headers)
 * ========================================================================= */

/* Hardware / system state */
uint32_t _excep_code = 0;
uint64_t _persistent = 0;
uint32_t ADC_dma_chan = 0;
uint32_t ADC_dma_chan2 = 0;
bool ADCDualBuffering = 0;
volatile unsigned int AHRSTimer = 0;
volatile int ConsoleTxBufHead = 0;
volatile int ConsoleTxBufTail = 0;
uint32_t core1stack[256] = {[0] = 0x12345678};
volatile e_CurrentlyPlaying CurrentlyPlaying = P_NOTHING;
short CurrentX = 0;
short CurrentY = 0;
volatile int DISPLAY_TYPE = 0;
/* DisplayNotSet is a function - see function stubs below */
int dma_rx_chan = 0;
int dma_rx_chan2 = 0;
int dma_tx_chan = 0;
int dma_tx_chan2 = 0;
bool dmarunning = 0;
long long int *ds18b20Timers = NULL;
volatile int ExtCurrentConfig[NBRPINS + 1] = {0};
union uFileTable FileTable[MAXOPENFILES + 1] = {{0}};
const uint8_t *flash_progmemory = NULL;
unsigned char *FontTable[16] = {NULL};
int FSerror = 0;
int GPSchannel = 0;
int gui_bcolour = 0;
int gui_fcolour = 0xFFFFFF;
short gui_font = 0;
short gui_font_height = 8;
short gui_font_width = 6;
short HRes = 320;
short VRes = 240;
uint8_t I2C0locked = 0;
uint8_t I2C1locked = 0;
unsigned char IgnorePIN = 0;
unsigned char *InterruptReturn = NULL;
int InterruptUsed = 0;
int last_adc = 0;
lfs_t lfs;
bool mergerunning = 0;
uint32_t mergetimer = 0;
int MMCharPos = 0;
int mmI2Cvalue = 0;
int mmOWvalue = 0;
bool mouse0 = 0;
unsigned char *OnKeyGOSUB = NULL;
unsigned char *OnPS2GOSUB = NULL;
MMFLOAT optionangle = 0;
bool optionfastaudio = 0;
bool optionfulltime = 0;
bool optionlogging = 0;
int PrintPixelMode = 0;
int PromptFont = 1;
int PromptFC = 0xFFFFFF;
int PromptBC = 0;
volatile int PS2code = 0;
/* ReadBuffer is a function pointer - defined in function pointers section below */
volatile uint32_t realflashpointer = 0;
unsigned char *SavedVarsFlash = NULL;
volatile unsigned int ScrewUpTimer = 0;
/* ScrollLCDSPISCR is a function - see function stubs below */
volatile int ScrollStart = 0;
int StartEditChar = 0;
unsigned char *StartEditPoint = NULL;
unsigned char *TickInt[NBRSETTICKS] = {NULL};
volatile int TickTimer[NBRSETTICKS] = {0};
int TickPeriod[NBRSETTICKS] = {0};
volatile unsigned char TickActive[NBRSETTICKS] = {0};
MMFLOAT VCC = 3.3;
bool useoptionangle = 0;
unsigned char WatchdogSet = 0;
bool WAVcomplete = 0;
char *WAVInterrupt = NULL;
volatile unsigned int WDTimer = 0;
/* Display_Refresh is a function - see function stubs below */

/* struct option_s Option */
struct option_s Option = {0};

/* PinDef array */
const struct s_PinDef PinDef[NBRPINS + 1] = {{0}};

/* CFunctionFlash / CFunctionLibrary */
unsigned char *CFunctionFlash = NULL;
unsigned char *CFunctionLibrary = NULL;

/* Timer/system variables */
volatile long long int mSecTimer = 0;
uint64_t timeroffset = 0;
int64_t TimeOffsetToUptime = 1704067200;
volatile unsigned int PauseTimer = 0;
volatile unsigned int IntPauseTimer = 0;
volatile unsigned int Timer1 = 0, Timer2 = 0, Timer3 = 0, Timer4 = 0, Timer5 = 0;
volatile unsigned int diskchecktimer = 0;
volatile unsigned int clocktimer = 0;
volatile int ds18b20Timer = 0;
volatile int CursorTimer = 0;
volatile unsigned int I2CTimer = 0;
volatile unsigned int MouseTimer = 0;
volatile unsigned int SecondsTimer = 0;
volatile int day_of_week = 0;
unsigned char PulsePin[NBR_PULSE_SLOTS] = {0};
unsigned char PulseDirection[NBR_PULSE_SLOTS] = {0};
int PulseCnt[NBR_PULSE_SLOTS] = {0};
int PulseActive = 0;
volatile int ClickTimer = 0;
int calibrate = 0;
volatile unsigned int InkeyTimer = 0;
volatile char ConsoleRxBuf[CONSOLE_RX_BUF_SIZE] = {0};
volatile int ConsoleRxBufHead = 0;
volatile int ConsoleRxBufTail = 0;
volatile char ConsoleTxBuf[CONSOLE_TX_BUF_SIZE] = {0};
unsigned char SPIatRisk = 0;
int ExitMMBasicFlag = 0;
unsigned int _excep_peek = 0;
int OptionErrorCheck = 0;
unsigned int CurrentCpuSpeed = 0;
unsigned int PeripheralBusSpeed = 0;
unsigned char EchoOption = 0;
int OptionFileErrorAbort = 0;
unsigned char filesource[MAXOPENFILES + 1] = {0};
int FatFSFileSystemSave = 0;
int FatFSFileSystem = 0;
int FlashLoad = 0;
volatile unsigned int GPSTimer = 0;
uint16_t AUDIO_L_PIN = 0, AUDIO_R_PIN = 0, AUDIO_SLICE = 0;
uint16_t AUDIO_WRAP = 0;
int ticks_per_second = 1000;
lfs_dir_t lfs_dir;
struct lfs_info lfs_info;
int lfs_FileFnbr = 0;
short DisplayHRes = 0, DisplayVRes = 0;
volatile short low_y = 0, high_y = 0, low_x = 0, high_x = 0;
uint8_t sprite_transparent = 0;
char CMM1 = 0;
int ScreenSize = 0;
char LCDAttrib = 0;
struct D3D *struct3d[MAX3D + 1] = {NULL};
s_camera camera[MAXCAM + 1] = {{0}};
struct spritebuffer spritebuff[MAXBLITBUF + 1] = {{0}};
struct blitbuffer blitbuff[MAXBLITBUF + 1] = {{0}};
char *COLLISIONInterrupt = NULL;
bool CollisionFound = 0;
int RGB121map[16] = {0};

/* Interrupt-related */
volatile int INT0Value = 0, INT0InitTimer = 0, INT0Timer = 0;
volatile int INT1Value = 0, INT1InitTimer = 0, INT1Timer = 0;
volatile int INT2Value = 0, INT2InitTimer = 0, INT2Timer = 0;
volatile int INT3Value = 0, INT3InitTimer = 0, INT3Timer = 0;
volatile int INT4Value = 0, INT4InitTimer = 0, INT4Timer = 0;
volatile int64_t INT1Count = 0, INT2Count = 0, INT3Count = 0, INT4Count = 0;
volatile uint64_t INT5Count = 0, INT5Value = 0, INT5InitTimer = 0, INT5Timer = 0;
struct s_inttbl inttbl[NBRINTERRUPTS] = {{0}};

/* PWM pin vars */
uint8_t PWM0Apin = 0, PWM0Bpin = 0;
uint8_t PWM1Apin = 0, PWM1Bpin = 0;
uint8_t PWM2Apin = 0, PWM2Bpin = 0;
uint8_t PWM3Apin = 0, PWM3Bpin = 0;
uint8_t PWM4Apin = 0, PWM4Bpin = 0;
uint8_t PWM5Apin = 0, PWM5Bpin = 0;
uint8_t PWM6Apin = 0, PWM6Bpin = 0;
uint8_t PWM7Apin = 0, PWM7Bpin = 0;

/* UART/SPI/I2C pin vars */
uint8_t UART1RXpin = 0, UART1TXpin = 0;
uint8_t UART0TXpin = 0, UART0RXpin = 0;
uint8_t SPI1TXpin = 0, SPI1RXpin = 0, SPI1SCKpin = 0;
uint8_t SPI0TXpin = 0, SPI0RXpin = 0, SPI0SCKpin = 0;
uint8_t I2C1SDApin = 0, I2C1SCLpin = 0;
uint8_t I2C0SDApin = 0, I2C0SCLpin = 0;
uint8_t slice0 = 0, slice1 = 0, slice2 = 0, slice3 = 0;
uint8_t slice4 = 0, slice5 = 0, slice6 = 0, slice7 = 0;
uint8_t SPI0locked = 0, SPI1locked = 0;
volatile int CallBackEnabled = 0;
int ADCopen = 0;
volatile MMFLOAT *volatile a1float = NULL, *volatile a2float = NULL;
volatile MMFLOAT *volatile a3float = NULL, *volatile a4float = NULL;
uint32_t ADCmax = 0;
char *ADCInterrupt = NULL;
short *ADCbuffer = NULL;
volatile uint8_t *adcint = NULL;
uint8_t *adcint1 = NULL, *adcint2 = NULL;
unsigned char *KeypadInterrupt = NULL;
MMFLOAT ADCscale[4] = {0}, ADCbottom[4] = {0};

/* IR related */
void *IrDev = NULL, *IrCmd = NULL;
volatile char IrVarType = 0, IrState = 0, IrGotMsg = 0;
int IrBits = 0, IrCount = 0;
unsigned char *IrInterrupt = NULL;
unsigned int CFuncInt1 = 0, CFuncInt2 = 0, CFuncInt3 = 0, CFuncInt4 = 0;

int p100interrupts[NBRPINS + 1] = {0};

int BacklightSlice = 0, BacklightChannel = 0;

/* QVGA / display */
int QVGA_CLKDIV = 0;
volatile int ytileheight = 0;
volatile int X_TILE = 0, Y_TILE = 0;
int CameraSlice = 0, CameraChannel = 0;
char id_out[256] = {0};
uint8_t *buff320 = NULL;
uint16_t SD_CLK_PIN = 0, SD_MOSI_PIN = 0, SD_MISO_PIN = 0, SD_CS_PIN = 0;
bool screen320 = 0;

/* g_myrand provided by MATHS.c */

/* Function pointers for draw */
void (*DrawPixel)(int x1, int y1, int c) = NULL;
void (*DrawRectangle)(int x1, int y1, int x2, int y2, int c) = NULL;
void (*DrawBitmap)(int x1, int y1, int width, int height, int scale, int fc, int bc, unsigned char *bitmap) = NULL;
void (*ReadBuffer)(int x1, int y1, int x2, int y2, unsigned char *c) = NULL;
void (*ReadBufferFast)(int x1, int y1, int x2, int y2, unsigned char *c) = NULL;
void (*DrawBLITBuffer)(int x1, int y1, int x2, int y2, unsigned char *c) = NULL;
void (*ReadBLITBuffer)(int x1, int y1, int x2, int y2, unsigned char *c) = NULL;
void (*ScrollLCD)(int lines) = NULL;
void (*DrawBuffer)(int x1, int y1, int x2, int y2, unsigned char *c) = NULL;

/* PINMAP */
#ifdef rp2350
const uint8_t PINMAP[48] = {0};
#else
const uint8_t PINMAP[30] = {0};
#endif

/* PinFunction */
const char *PinFunction[64] = {NULL};

/* volatile HID */
volatile struct s_HID HID[4] = {{0}};

/* LFS config */
struct lfs_config pico_lfs_cfg = {0};

/* Tile color arrays */
uint8_t map16[16] = {0};
uint16_t tilefcols[80*40] = {0};
uint16_t tilebcols[80*40] = {0};

/* MOUSE_CLOCK, MOUSE_DATA */
int MOUSE_CLOCK = 0, MOUSE_DATA = 0;

volatile uint64_t IRoffset = 0;

/* dma_hw and watchdog_hw - dummy storage for host build.
 * Commands.c accesses dma_hw->intf0 etc, so provide enough storage. */
#include "hardware/dma.h"
#include "hardware/structs/watchdog.h"
static dma_hw_t _dma_hw_store = {0};
static watchdog_hw_t _wdog_hw_store = {0};
dma_hw_t *dma_hw = &_dma_hw_store;
watchdog_hw_t *watchdog_hw = &_wdog_hw_store;

static void host_sync_msec_timer_value(uint64_t now_us) {
    mSecTimer = (long long)(now_us / 1000ULL);
}

static uint64_t host_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void host_sync_msec_timer(void) {
    host_sync_msec_timer_value(host_now_us());
}

uint64_t host_time_us_64(void) {
    uint64_t now = host_now_us();
    host_sync_msec_timer_value(now);
    return now;
}

void host_sleep_us(uint64_t us) {
    if (us == 0) {
        host_sync_msec_timer();
        return;
    }

    struct timespec req;
    req.tv_sec = (time_t)(us / 1000000ULL);
    req.tv_nsec = (long)((us % 1000000ULL) * 1000ULL);
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
    host_sync_msec_timer();
}

static void host_fb_ensure(void) {
    size_t pixels = (size_t)host_fb_width * (size_t)host_fb_height;
    if (!host_framebuffer) {
        host_framebuffer = calloc(pixels, sizeof(*host_framebuffer));
    }
}

static inline int host_clamp_int(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static inline uint32_t host_colour24(int c) {
    return (uint32_t)c & 0x00FFFFFFu;
}

static inline void host_put_pixel(int x, int y, int c) {
    if (!host_framebuffer) host_fb_ensure();
    if (!host_framebuffer) return;
    if (x < 0 || y < 0 || x >= host_fb_width || y >= host_fb_height) return;
    host_framebuffer[(size_t)y * (size_t)host_fb_width + (size_t)x] = host_colour24(c);
}

static void host_fill_rect_pixels(int x1, int y1, int x2, int y2, int c) {
    if (!host_framebuffer) host_fb_ensure();
    if (!host_framebuffer) return;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    x1 = host_clamp_int(x1, 0, host_fb_width - 1);
    x2 = host_clamp_int(x2, 0, host_fb_width - 1);
    y1 = host_clamp_int(y1, 0, host_fb_height - 1);
    y2 = host_clamp_int(y2, 0, host_fb_height - 1);
    if (x1 > x2 || y1 > y2) return;

    uint32_t colour = host_colour24(c);
    for (int y = y1; y <= y2; ++y) {
        uint32_t *row = host_framebuffer + (size_t)y * (size_t)host_fb_width;
        for (int x = x1; x <= x2; ++x) {
            row[x] = colour;
        }
    }
}

static void host_draw_line_pixels(int x1, int y1, int x2, int y2, int width, int c) {
    if (width < 0) width = -width;
    if (width == 0) width = 1;
    int radius = width / 2;

    int dx = abs(x2 - x1);
    int sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1);
    int sy = y1 < y2 ? 1 : -1;
    int err = dx + dy;

    while (1) {
        host_fill_rect_pixels(x1 - radius, y1 - radius, x1 + radius, y1 + radius, c);
        if (x1 == x2 && y1 == y2) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x1 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y1 += sy;
        }
    }
}

static void host_glyph_rows(char ch, uint8_t rows[7]) {
    memset(rows, 0, 7);
    switch (toupper((unsigned char)ch)) {
        case '0': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x13; rows[3]=0x15; rows[4]=0x19; rows[5]=0x11; rows[6]=0x0E; break;
        case '1': rows[0]=0x04; rows[1]=0x0C; rows[2]=0x04; rows[3]=0x04; rows[4]=0x04; rows[5]=0x04; rows[6]=0x0E; break;
        case '2': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x01; rows[3]=0x02; rows[4]=0x04; rows[5]=0x08; rows[6]=0x1F; break;
        case '3': rows[0]=0x1E; rows[1]=0x01; rows[2]=0x01; rows[3]=0x0E; rows[4]=0x01; rows[5]=0x01; rows[6]=0x1E; break;
        case '4': rows[0]=0x02; rows[1]=0x06; rows[2]=0x0A; rows[3]=0x12; rows[4]=0x1F; rows[5]=0x02; rows[6]=0x02; break;
        case '5': rows[0]=0x1F; rows[1]=0x10; rows[2]=0x10; rows[3]=0x1E; rows[4]=0x01; rows[5]=0x01; rows[6]=0x1E; break;
        case '6': rows[0]=0x0E; rows[1]=0x10; rows[2]=0x10; rows[3]=0x1E; rows[4]=0x11; rows[5]=0x11; rows[6]=0x0E; break;
        case '7': rows[0]=0x1F; rows[1]=0x01; rows[2]=0x02; rows[3]=0x04; rows[4]=0x08; rows[5]=0x08; rows[6]=0x08; break;
        case '8': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x0E; rows[4]=0x11; rows[5]=0x11; rows[6]=0x0E; break;
        case '9': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x0F; rows[4]=0x01; rows[5]=0x01; rows[6]=0x0E; break;
        case 'A': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x1F; rows[4]=0x11; rows[5]=0x11; rows[6]=0x11; break;
        case 'C': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x10; rows[3]=0x10; rows[4]=0x10; rows[5]=0x11; rows[6]=0x0E; break;
        case 'E': rows[0]=0x1F; rows[1]=0x10; rows[2]=0x10; rows[3]=0x1E; rows[4]=0x10; rows[5]=0x10; rows[6]=0x1F; break;
        case 'F': rows[0]=0x1F; rows[1]=0x10; rows[2]=0x10; rows[3]=0x1E; rows[4]=0x10; rows[5]=0x10; rows[6]=0x10; break;
        case 'G': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x10; rows[3]=0x17; rows[4]=0x11; rows[5]=0x11; rows[6]=0x0F; break;
        case 'H': rows[0]=0x11; rows[1]=0x11; rows[2]=0x11; rows[3]=0x1F; rows[4]=0x11; rows[5]=0x11; rows[6]=0x11; break;
        case 'I': rows[0]=0x0E; rows[1]=0x04; rows[2]=0x04; rows[3]=0x04; rows[4]=0x04; rows[5]=0x04; rows[6]=0x0E; break;
        case 'K': rows[0]=0x11; rows[1]=0x12; rows[2]=0x14; rows[3]=0x18; rows[4]=0x14; rows[5]=0x12; rows[6]=0x11; break;
        case 'L': rows[0]=0x10; rows[1]=0x10; rows[2]=0x10; rows[3]=0x10; rows[4]=0x10; rows[5]=0x10; rows[6]=0x1F; break;
        case 'O': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x11; rows[4]=0x11; rows[5]=0x11; rows[6]=0x0E; break;
        case 'P': rows[0]=0x1E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x1E; rows[4]=0x10; rows[5]=0x10; rows[6]=0x10; break;
        case 'R': rows[0]=0x1E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x1E; rows[4]=0x14; rows[5]=0x12; rows[6]=0x11; break;
        case 'S': rows[0]=0x0F; rows[1]=0x10; rows[2]=0x10; rows[3]=0x0E; rows[4]=0x01; rows[5]=0x01; rows[6]=0x1E; break;
        case 'T': rows[0]=0x1F; rows[1]=0x04; rows[2]=0x04; rows[3]=0x04; rows[4]=0x04; rows[5]=0x04; rows[6]=0x04; break;
        case 'V': rows[0]=0x11; rows[1]=0x11; rows[2]=0x11; rows[3]=0x11; rows[4]=0x11; rows[5]=0x0A; rows[6]=0x04; break;
        case 'Y': rows[0]=0x11; rows[1]=0x11; rows[2]=0x0A; rows[3]=0x04; rows[4]=0x04; rows[5]=0x04; rows[6]=0x04; break;
        case '!': rows[0]=0x04; rows[1]=0x04; rows[2]=0x04; rows[3]=0x04; rows[4]=0x04; rows[5]=0x00; rows[6]=0x04; break;
        case ':': rows[0]=0x00; rows[1]=0x04; rows[2]=0x00; rows[3]=0x00; rows[4]=0x00; rows[5]=0x04; rows[6]=0x00; break;
        case '.': rows[0]=0x00; rows[1]=0x00; rows[2]=0x00; rows[3]=0x00; rows[4]=0x00; rows[5]=0x00; rows[6]=0x04; break;
        case '-': rows[0]=0x00; rows[1]=0x00; rows[2]=0x00; rows[3]=0x1F; rows[4]=0x00; rows[5]=0x00; rows[6]=0x00; break;
        case ' ': break;
        default:  rows[0]=0x1F; rows[1]=0x11; rows[2]=0x15; rows[3]=0x15; rows[4]=0x15; rows[5]=0x11; rows[6]=0x1F; break;
    }
}

static void host_draw_char(int x, int y, int scale, int fc, int bc, char ch) {
    uint8_t rows[7];
    if (scale < 1) scale = 1;
    host_glyph_rows(ch, rows);

    if (bc >= 0) {
        host_fill_rect_pixels(x, y, x + 6 * scale - 1, y + 8 * scale - 1, bc);
    }

    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if (rows[row] & (1u << (4 - col))) {
                host_fill_rect_pixels(
                    x + col * scale,
                    y + row * scale,
                    x + (col + 1) * scale - 1,
                    y + (row + 1) * scale - 1,
                    fc
                );
            }
        }
    }
}

static void host_draw_text(int x, int y, int scale, int jh, int jv, int fc, int bc, const char *str) {
    size_t len = str ? strlen(str) : 0;
    int char_w = 6 * scale;
    int char_h = 8 * scale;
    int text_w = (int)len * char_w;
    int text_h = char_h;

    if (jh == JUSTIFY_CENTER) x -= text_w / 2;
    else if (jh == JUSTIFY_RIGHT) x -= text_w;

    if (jv == JUSTIFY_MIDDLE) y -= text_h / 2;
    else if (jv == JUSTIFY_BOTTOM) y -= text_h;

    for (size_t i = 0; i < len; ++i) {
        host_draw_char(x + (int)i * char_w, y, scale, fc, bc, str[i]);
    }
}

static void host_fb_reset(int colour) {
    host_fb_ensure();
    host_fill_rect_pixels(0, 0, host_fb_width - 1, host_fb_height - 1, colour);
}

static void host_write_screenshot(const char *path) {
    if (!path || !*path || host_screenshot_written) return;
    host_fb_ensure();
    if (!host_framebuffer) return;

    FILE *fp = fopen(path, "wb");
    if (!fp) return;

    fprintf(fp, "P6\n%d %d\n255\n", host_fb_width, host_fb_height);
    for (int y = 0; y < host_fb_height; ++y) {
        for (int x = 0; x < host_fb_width; ++x) {
            uint32_t pixel = host_framebuffer[(size_t)y * (size_t)host_fb_width + (size_t)x];
            unsigned char rgb[3] = {
                (unsigned char)((pixel >> 16) & 0xFF),
                (unsigned char)((pixel >> 8) & 0xFF),
                (unsigned char)(pixel & 0xFF),
            };
            fwrite(rgb, 1, 3, fp);
        }
    }
    fclose(fp);
    host_screenshot_written = 1;
}

static int host_parse_escaped_char(const char **src) {
    const char *p = *src;
    if (*p != '\\') {
        int ch = (unsigned char)*p;
        if (*p) p++;
        *src = p;
        return ch;
    }

    p++;
    switch (*p) {
        case 'n': *src = p + 1; return '\n';
        case 'r': *src = p + 1; return '\r';
        case 't': *src = p + 1; return '\t';
        case '\\': *src = p + 1; return '\\';
        case 'x':
            if (isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                char hex[3] = {p[1], p[2], 0};
                *src = p + 3;
                return (int)strtol(hex, NULL, 16);
            }
            break;
        default:
            break;
    }

    *src = p;
    return '\\';
}

static void host_load_key_script(void) {
    const char *env = getenv("MMBASIC_HOST_KEYS");
    host_key_script_len = 0;
    host_key_script_pos = 0;
    if (!env || !*env) return;

    const char *p = env;
    while (*p && host_key_script_len + 1 < sizeof(host_key_script)) {
        host_key_script[host_key_script_len++] = (char)host_parse_escaped_char(&p);
    }
    host_key_script[host_key_script_len] = '\0';
}

void host_runtime_configure(int timeout_ms, const char *screenshot_path) {
    host_runtime_timeout_ms = timeout_ms;
    host_screenshot_written = 0;
    host_runtime_timed_out_flag = 0;
    host_screenshot_path[0] = '\0';
    if (screenshot_path && *screenshot_path) {
        snprintf(host_screenshot_path, sizeof(host_screenshot_path), "%s", screenshot_path);
    }
}

void host_runtime_begin(void) {
    host_runtime_timed_out_flag = 0;
    host_screenshot_written = 0;
    host_fastgfx_active = 0;
    host_fastgfx_fps = 0;
    host_fastgfx_next_sync_us = 0;
    host_load_key_script();
    timeroffset = host_time_us_64();
    host_runtime_deadline_us = 0;
    if (host_runtime_timeout_ms > 0) {
        host_runtime_deadline_us = timeroffset + (uint64_t)host_runtime_timeout_ms * 1000ULL;
    }
    host_fb_reset(gui_bcolour);
    FontTable[0] = host_font_metrics;
}

void host_runtime_finish(void) {
    if (host_screenshot_path[0]) {
        host_write_screenshot(host_screenshot_path);
    }
}

int host_runtime_timed_out(void) {
    return host_runtime_timed_out_flag;
}

static void host_runtime_check_timeout(void) {
    if (!host_runtime_deadline_us || host_runtime_timed_out_flag) return;
    uint64_t now = host_time_us_64();
    if (now < host_runtime_deadline_us) return;

    host_runtime_timed_out_flag = 1;
    if (host_screenshot_path[0]) {
        host_write_screenshot(host_screenshot_path);
    }
    longjmp(mark, 1);
}

/* =========================================================================
 *  Stub Functions -- cmd_* (hardware commands): void cmd_xxx(void) {}
 * ========================================================================= */

void cmd_3D(void) {}
void cmd_adc(void) {}
void cmd_arc(void) {}
void cmd_autosave(void) {}
void cmd_backlight(void) {}
void cmd_blit(void) {}
void cmd_blitmemory(void) {}
void cmd_box(void) {
    int x1, y1, width, height, w = 1, c = gui_fcolour, f = -1;
    int wmod, hmod;
    getargs(&cmdline, 13, (unsigned char *)",");
    if (!(argc & 1) || argc < 7) error("Argument count");
    x1 = getinteger(argv[0]);
    y1 = getinteger(argv[2]);
    width = getinteger(argv[4]);
    height = getinteger(argv[6]);
    wmod = (width > 0) ? -1 : 1;
    hmod = (height > 0) ? -1 : 1;
    if (argc > 7 && *argv[8]) w = getint(argv[8], 0, 100);
    if (argc > 9 && *argv[10]) c = getint(argv[10], 0, WHITE);
    if (argc == 13 && *argv[12]) f = getint(argv[12], -1, WHITE);
    if (width != 0 && height != 0) {
        DrawBox(x1, y1, x1 + width + wmod, y1 + height + hmod, w, c, f);
    }
}
void cmd_camera(void) {}
void cmd_cfunction(void) {}
void cmd_chdir(void) {}
void cmd_circle(void) {
    int x, y, r, w = 1, c = gui_fcolour, f = -1;
    MMFLOAT a = 1.0;
    getargs(&cmdline, 13, (unsigned char *)",");
    if (!(argc & 1) || argc < 5) error("Argument count");
    x = getinteger(argv[0]);
    y = getinteger(argv[2]);
    r = getinteger(argv[4]);
    if (argc > 5 && *argv[6]) w = getint(argv[6], 0, 100);
    if (argc > 7 && *argv[8]) a = getnumber(argv[8]);
    if (argc > 9 && *argv[10]) c = getint(argv[10], 0, WHITE);
    if (argc > 11 && *argv[12]) f = getint(argv[12], -1, WHITE);
    DrawCircle(x, y, r, w, c, f, a);
}
void cmd_Classic(void) {}
void cmd_close(void) {}
void cmd_cls(void) {
    int colour = gui_bcolour;
    if (cmdline && *cmdline) colour = getint(cmdline, 0, WHITE);
    ClearScreen(colour);
    CurrentX = 0;
    CurrentY = 0;
}
void cmd_colour(void) {}
void cmd_configure(void) {}
void cmd_copy(void) {}
void cmd_cpu(void) {}
void cmd_csubinterrupt(void) {}
void cmd_date(void) {}
void cmd_device(void) {}
void cmd_DHT22(void) {}
void cmd_disk(void) {}
void cmd_ds18b20(void) {}
void cmd_edit(void) {}
void cmd_editfile(void) {}
void cmd_endprogram(void) {}
void cmd_fastgfx(void) {
    unsigned char *p;

    if ((p = checkstring(cmdline, (unsigned char *)"CREATE"))) {
        checkend(p);
        host_fastgfx_active = 1;
        host_fastgfx_next_sync_us = 0;
        return;
    }
    if ((p = checkstring(cmdline, (unsigned char *)"CLOSE"))) {
        checkend(p);
        host_fastgfx_active = 0;
        host_fastgfx_next_sync_us = 0;
        return;
    }
    if ((p = checkstring(cmdline, (unsigned char *)"SWAP"))) {
        checkend(p);
        return;
    }
    if ((p = checkstring(cmdline, (unsigned char *)"SYNC"))) {
        checkend(p);
        if (!host_fastgfx_active || host_fastgfx_fps <= 0) return;
        uint64_t frame_us = 1000000ULL / (uint64_t)host_fastgfx_fps;
        uint64_t now = host_time_us_64();
        if (host_fastgfx_next_sync_us == 0) host_fastgfx_next_sync_us = now + frame_us;
        if (now < host_fastgfx_next_sync_us) host_sleep_us(host_fastgfx_next_sync_us - now);
        host_fastgfx_next_sync_us += frame_us;
        return;
    }
    if ((p = checkstring(cmdline, (unsigned char *)"FPS"))) {
        host_fastgfx_fps = getint(p, 1, 1000);
        host_fastgfx_next_sync_us = 0;
        return;
    }

    error("Syntax");
}
void cmd_files(void) {}
void cmd_flash(void) {}
void cmd_flush(void) {}
void cmd_font(void) {}
void cmd_framebuffer(void) {}
void cmd_guiMX170(void) {}
void cmd_i2c(void) {}
void cmd_i2c2(void) {}
void cmd_in(void) {}
void cmd_ir(void) {}
void cmd_ireturn(void) {}
void cmd_irq(void) {}
void cmd_irqclear(void) {}
void cmd_irqnowait(void) {}
void cmd_irqset(void) {}
void cmd_irqwait(void) {}
void cmd_jmp(void) {}
void cmd_keypad(void) {}
void cmd_kill(void) {}
void cmd_label(void) {}
void cmd_lcd(void) {}
void cmd_library(void) {}
void cmd_line(void) {
    int x1, y1, x2, y2, w = 1, c = gui_fcolour;
    getargs(&cmdline, 11, (unsigned char *)",");
    if (!(argc & 1) || argc < 7) error("Argument count");
    x1 = getinteger(argv[0]);
    y1 = getinteger(argv[2]);
    x2 = getinteger(argv[4]);
    y2 = getinteger(argv[6]);
    if (argc > 7 && *argv[8]) {
        w = getint(argv[8], -100, 100);
        if (!w) return;
    }
    if (argc == 11 && *argv[10]) c = getint(argv[10], 0, WHITE);
    DrawLine(x1, y1, x2, y2, w, c);
}
void cmd_load(void) {}
void cmd_longString(void) {}
void cmd_mkdir(void) {}
void cmd_mouse(void) {}
void cmd_mov(void) {}
void cmd_name(void) {}
void cmd_nop(void) {}
void cmd_Nunchuck(void) {}
void cmd_onewire(void) {}
void cmd_open(void) {}
void cmd_option(void) {}
void cmd_out(void) {}
void cmd_pause(void) {
    MMFLOAT f = getnumber(cmdline) * 1000.0;
    if (f < 0) error("Number out of bounds");
    if (f < 2) return;
    host_sleep_us((uint64_t)f);
}
void cmd_pin(void) {}
void cmd_pio(void) {}
void cmd_PIOline(void) {}
void cmd_pixel(void) {}
void cmd_play(void) {}
void cmd_poke(void) {}
void cmd_polygon(void) {}
void cmd_port(void) {}
void cmd_program(void) {}
void cmd_pull(void) {}
void cmd_pulse(void) {}
void cmd_push(void) {}
void cmd_pwm(void) {}
void cmd_rbox(void) {}
void cmd_refresh(void) {}
void cmd_rmdir(void) {}
void cmd_rtc(void) {}
void cmd_save(void) {}
void cmd_seek(void) {}
void cmd_Servo(void) {}
void cmd_set(void) {}
void cmd_setpin(void) {}
void cmd_settick(void) {}
void cmd_sideset(void) {}
void cmd_sort(void) {}
void cmd_spi(void) {}
void cmd_spi2(void) {}
void cmd_sprite(void) {}
void cmd_sync(void) {}
int GetJustification(char *p, int *jh, int *jv, int *jo) {
    switch (toupper((unsigned char)*p++)) {
        case 'L': *jh = JUSTIFY_LEFT; break;
        case 'C': *jh = JUSTIFY_CENTER; break;
        case 'R': *jh = JUSTIFY_RIGHT; break;
        case 0: return true;
        default: p--;
    }
    skipspace(p);
    switch (toupper((unsigned char)*p++)) {
        case 'T': *jv = JUSTIFY_TOP; break;
        case 'M': *jv = JUSTIFY_MIDDLE; break;
        case 'B': *jv = JUSTIFY_BOTTOM; break;
        case 0: return true;
        default: p--;
    }
    skipspace(p);
    switch (toupper((unsigned char)*p++)) {
        case 'N': *jo = ORIENT_NORMAL; break;
        case 0: return true;
        default: return false;
    }
    return *p == 0;
}

void cmd_text(void) {
    int x, y, font, scale, fc, bc;
    char *s;
    int jh = JUSTIFY_LEFT, jv = JUSTIFY_TOP, jo = ORIENT_NORMAL;

    getargs(&cmdline, 17, (unsigned char *)",");
    if (!(argc & 1) || argc < 5) error("Argument count");
    x = getinteger(argv[0]);
    y = getinteger(argv[2]);
    s = (char *)getCstring(argv[4]);

    if (argc > 5 && *argv[6]) {
        if (!GetJustification((char *)argv[6], &jh, &jv, &jo)) {
            if (!GetJustification((char *)getCstring(argv[6]), &jh, &jv, &jo)) {
                error("Justification");
            }
        }
    }

    font = (gui_font >> 4) + 1;
    scale = gui_font & 0x0F;
    if (scale == 0) scale = 1;
    fc = gui_fcolour;
    bc = gui_bcolour;
    if (argc > 7 && *argv[8]) font = getint(argv[8], 1, FONT_TABLE_SIZE);
    if (argc > 9 && *argv[10]) scale = getint(argv[10], 1, 15);
    if (argc > 11 && *argv[12]) fc = getint(argv[12], 0, WHITE);
    if (argc == 15 && *argv[14]) bc = getint(argv[14], -1, WHITE);
    (void)font;
    (void)jo;
    GUIPrintString(x, y, scale, jh, jv, jo, fc, bc, s);
}
void cmd_time(void) {}
void cmd_timer(void) {
    uint64_t mytime = host_time_us_64();
    while (*cmdline && tokenfunction(*cmdline) != op_equal) cmdline++;
    if (!*cmdline) error("Syntax");
    timeroffset = mytime - (uint64_t)getint(++cmdline, 0, (int)(mytime / 1000ULL)) * 1000ULL;
}
void cmd_triangle(void) {}
void cmd_update(void) {}
void cmd_var(void) {}
void cmd_wait(void) {}
void cmd_watchdog(void) {}
void cmd_wrap(void) {}
void cmd_wraptarget(void) {}
void cmd_WS2812(void) {}
void cmd_xmodem(void) {}

/* =========================================================================
 *  Stub Functions -- fun_* (hardware functions): void fun_xxx(void) {}
 * ========================================================================= */

void fun_at(void) {}
void fun_cwd(void) {}
void fun_date(void) {}
void fun_datetime(void) {}
void fun_day(void) {}
void fun_dev(void) {}
void fun_device(void) {}
void fun_dir(void) {}
void fun_distance(void) {}
void fun_ds18b20(void) {}
void fun_eof(void) {}
void fun_epoch(void) {}
void fun_format(void) {}
void fun_GPS(void) {}
void fun_info(void) {}
void fun_inputstr(void) {}
void fun_LCompare(void) {}
void fun_LGetByte(void) {}
void fun_LGetStr(void) {}
void fun_LInstr(void) {}
void fun_LLen(void) {}
void fun_loc(void) {}
void fun_lof(void) {}
void fun_peek(void) {}
void fun_pin(void) {}
void fun_pio(void) {}
void fun_pixel(void) {}
void fun_port(void) {}
void fun_pulsin(void) {}
void fun_rgb(void) {
    getargs(&ep, 5, (unsigned char *)",");
    if (argc == 5) {
        iret = rgb(getint(argv[0], 0, 255), getint(argv[2], 0, 255), getint(argv[4], 0, 255));
    } else if (argc == 1) {
        if(checkstring(argv[0], (unsigned char *)"WHITE"))        iret = WHITE;
        else if(checkstring(argv[0], (unsigned char *)"YELLOW"))  iret = YELLOW;
        else if(checkstring(argv[0], (unsigned char *)"LILAC"))   iret = LILAC;
        else if(checkstring(argv[0], (unsigned char *)"BROWN"))   iret = BROWN;
        else if(checkstring(argv[0], (unsigned char *)"FUCHSIA")) iret = FUCHSIA;
        else if(checkstring(argv[0], (unsigned char *)"RUST"))    iret = RUST;
        else if(checkstring(argv[0], (unsigned char *)"MAGENTA")) iret = MAGENTA;
        else if(checkstring(argv[0], (unsigned char *)"RED"))     iret = RED;
        else if(checkstring(argv[0], (unsigned char *)"CYAN"))    iret = CYAN;
        else if(checkstring(argv[0], (unsigned char *)"GREEN"))   iret = GREEN;
        else if(checkstring(argv[0], (unsigned char *)"CERULEAN"))iret = CERULEAN;
        else if(checkstring(argv[0], (unsigned char *)"MIDGREEN"))iret = MIDGREEN;
        else if(checkstring(argv[0], (unsigned char *)"COBALT"))  iret = COBALT;
        else if(checkstring(argv[0], (unsigned char *)"MYRTLE"))  iret = MYRTLE;
        else if(checkstring(argv[0], (unsigned char *)"BLUE"))    iret = BLUE;
        else if(checkstring(argv[0], (unsigned char *)"BLACK"))   iret = BLACK;
        else if(checkstring(argv[0], (unsigned char *)"GRAY"))    iret = GRAY;
        else if(checkstring(argv[0], (unsigned char *)"GREY"))    iret = GRAY;
        else if(checkstring(argv[0], (unsigned char *)"LIGHTGRAY")) iret = LITEGRAY;
        else if(checkstring(argv[0], (unsigned char *)"LIGHTGREY")) iret = LITEGRAY;
        else if(checkstring(argv[0], (unsigned char *)"ORANGE"))  iret = ORANGE;
        else if(checkstring(argv[0], (unsigned char *)"PINK"))    iret = PINK;
        else if(checkstring(argv[0], (unsigned char *)"GOLD"))    iret = GOLD;
        else if(checkstring(argv[0], (unsigned char *)"SALMON"))  iret = SALMON;
        else if(checkstring(argv[0], (unsigned char *)"BEIGE"))   iret = BEIGE;
        else error("Invalid colour: $", argv[0]);
    } else {
        error("Syntax");
    }
    targ = T_INT;
}
void fun_spi(void) {}
void fun_spi2(void) {}
void fun_sprite(void) {}
void fun_time(void) {
    time_t now = (time_t)(host_time_us_64() / 1000000ULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&now, &tm_buf);
    sret = GetTempMemory(STRINGSIZE);
    if (!tm) strcpy((char *)sret, "00:00:00");
    else snprintf((char *)sret, STRINGSIZE, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    CtoM(sret);
    targ = T_STR;
}
void fun_timer(void) {
    fret = (MMFLOAT)(host_time_us_64() - timeroffset) / 1000.0;
    targ = T_NBR;
}
void fun_touch(void) {}

/* =========================================================================
 *  Stub Functions -- other signatures
 * ========================================================================= */

/* Hardware interaction */
void CheckAbort(void) { host_runtime_check_timeout(); }
int check_interrupt(void) { return 0; }
void ClearExternalIO(void) {}
void ClearScreen(int c) { host_fb_reset(c); }
void CloseAllFiles(void) {}
void CloseAudio(int all) { (void)all; }
void closeframebuffer(char layer) { (void)layer; }
void clear320(void) {}
void DisplayPutC(char c) { host_print(&c, 1); }
void enable_interrupts_pico(void) {}
void disable_interrupts_pico(void) {}
void initFonts(void) { FontTable[0] = host_font_metrics; }
void initMouse0(int sensitivity) { (void)sensitivity; }
void restorepanel(void) {}
void routinechecks(void) { host_runtime_check_timeout(); }
void SoftReset(void) {}
void uSec(int us) { (void)us; }
uint32_t __get_MSP(void) { return 0xFFFFFFFF; }  /* always pass stack overflow check */

/* Console I/O -- hooks into host_output_hook for output capture */
extern void (*host_output_hook)(const char *text, int len);

static void host_print(const char *s, int len) {
    if (host_output_hook) host_output_hook(s, len);
    else fwrite(s, 1, len, stdout);
}

static void host_prints(const char *s) {
    if (s) host_print(s, strlen(s));
}

int MMInkey(void) {
    host_runtime_check_timeout();
    if (host_key_script_pos < host_key_script_len) {
        return (unsigned char)host_key_script[host_key_script_pos++];
    }
    return -1;
}
int MMgetchar(void) { return getchar(); }
char MMputchar(char c, int flush) {
    if (host_output_hook) host_output_hook(&c, 1);
    else {
        fputc(c, stdout);
        if (flush) fflush(stdout);
    }
    return c;
}
void MMPrintString(char *s) { host_prints(s); }
void SSPrintString(char *s) { host_prints(s); }
void PRet(void) { host_prints("\r\n"); }
void PInt(int64_t n) { char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)n); host_prints(buf); }
void PIntComma(int64_t n) { char buf[32]; snprintf(buf, sizeof(buf), "%lld, ", (long long)n); host_prints(buf); }
void PFlt(MMFLOAT flt) { char buf[64]; snprintf(buf, sizeof(buf), "%g", flt); host_prints(buf); }
void PFltComma(MMFLOAT n) { char buf[64]; snprintf(buf, sizeof(buf), "%g, ", n); host_prints(buf); }
void PIntH(unsigned long long int n) { char buf[32]; snprintf(buf, sizeof(buf), "0x%llX", n); host_prints(buf); }
void PIntHC(unsigned long long int n) { char buf[32]; snprintf(buf, sizeof(buf), "0x%llX, ", n); host_prints(buf); }
void PIntB(unsigned long long int n) { (void)n; }
void PIntBC(unsigned long long int n) { (void)n; }
void SRet(void) {}
void SInt(int64_t n) { (void)n; }
void SIntComma(int64_t n) { (void)n; }
void MMfputs(unsigned char *p, int filenbr) {
    (void)filenbr;
    if (p) { int len = *p; host_print((char *)(p + 1), len); }
}
int MMfeof(int fnbr) { (void)fnbr; return 1; }
unsigned char MMfputc(unsigned char c, int fnbr) { (void)fnbr; host_print((char *)&c, 1); return c; }
int MMfgetc(int filenbr) { (void)filenbr; return -1; }
void MMfopen(unsigned char *fname, unsigned char *mode, int fnbr) { (void)fname; (void)mode; (void)fnbr; }
void MMfclose(int fnbr) { (void)fnbr; }
void MMgetline(int filenbr, char *p) { (void)filenbr; p[0] = 0; }
void printoptions(void) {}
void putConsole(int c, int flush) { char ch = c; host_print(&ch, 1); if (flush) fflush(stdout); }
int getConsole(void) { return -1; }
void myprintf(char *s) { host_prints(s); }
char SerialConsolePutC(char c, int flush) { host_print(&c, 1); if (flush) fflush(stdout); return c; }
int kbhitConsole(void) { return 0; }

/* File I/O stubs */
int BasicFileOpen(char *fname, int fnbr, int mode) { (void)fname; (void)fnbr; (void)mode; return 0; }
void FileClose(int fnbr) { (void)fnbr; }
int FileEOF(int fnbr) { (void)fnbr; return 1; }
char FileGetChar(int fnbr) { (void)fnbr; return -1; }
int FileLoadProgram(unsigned char *fname, bool chain) { (void)fname; (void)chain; return 0; }
int FileLoadCMM2Program(char *fname, bool message) { (void)fname; (void)message; return 0; }
void FilePutStr(int count, char *s, int fnbr) { (void)count; (void)s; (void)fnbr; }
char FilePutChar(char c, int fnbr) { (void)c; (void)fnbr; return c; }
int FindFreeFileNbr(void) { return 1; }
int ExistsFile(char *fname) { (void)fname; return 0; }
void FlashWriteByte(unsigned char b) { (void)b; }
void FlashWriteClose(void) {}
void FlashWriteInit(int region) { (void)region; }
void FlashWriteBlock(void) {}
void FlashWriteWord(unsigned int i) { (void)i; }
void FlashWriteAlign(void) {}
void FlashWriteAlignWord(void) {}
void FlashSetAddress(int address) { (void)address; }
void CallCFunction(unsigned char *p, unsigned char *args, int *t, unsigned char **s) { (void)p; (void)args; (void)t; (void)s; }
void CallExecuteProgram(char *p) { (void)p; }
void LoadOptions(void) {}
void SaveOptions(void) {}
void ResetAllFlash(void) {}
void ResetOptions(bool startup) { (void)startup; }
void ResetFlashStorage(int umount) { (void)umount; }
void SaveProgramToFlash(unsigned char *pm, int msg) { (void)pm; (void)msg; }
void CheckSDCard(void) {}
void CrunchData(unsigned char **p, int c) { (void)p; (void)c; }
void ClearSavedVars(void) {}
int ForceFileClose(int fnbr) { (void)fnbr; return 0; }
void ErrorCheck(int fnbr) { (void)fnbr; }
void positionfile(int fnbr, int idx) { (void)fnbr; (void)idx; }
int drivecheck(char *p, int *waste) { (void)p; (void)waste; return 0; }
void getfullfilename(char *fname, char *q) { (void)fname; (void)q; }
char *GetCWD(void) { return (char *)""; }
int InitSDCard(void) { return 0; }

/* LFS stubs */
int lfs_file_close(lfs_t *l, lfs_file_t *file) { (void)l; (void)file; return 0; }
int lfs_file_open(lfs_t *l, lfs_file_t *file, const char *path, int flags) { (void)l; (void)file; (void)path; (void)flags; return -1; }
lfs_ssize_t lfs_file_read(lfs_t *l, lfs_file_t *file, void *buf, lfs_size_t size) { (void)l; (void)file; (void)buf; (void)size; return 0; }
lfs_soff_t lfs_file_seek(lfs_t *l, lfs_file_t *file, lfs_soff_t off, int whence) { (void)l; (void)file; (void)off; (void)whence; return 0; }
lfs_ssize_t lfs_file_write(lfs_t *l, lfs_file_t *file, const void *buf, lfs_size_t size) { (void)l; (void)file; (void)buf; (void)size; return 0; }
lfs_ssize_t lfs_fs_size(lfs_t *l) { (void)l; return 0; }
int lfs_remove(lfs_t *l, const char *path) { (void)l; (void)path; return 0; }
int lfs_stat(lfs_t *l, const char *path, struct lfs_info *info) { (void)l; (void)path; (void)info; return -1; }

/* FatFS pattern_matching stub (declared as function in ff.h) */
int pattern_matching(const TCHAR *pat, const TCHAR *nam, int skip, int inf) {
    (void)pat; (void)nam; (void)skip; (void)inf;
    return 0;
}

/* Drawing stubs */
void SetFont(int f) {
    int scale = f & 0x0F;
    if (scale == 0) scale = 1;
    gui_font = f;
    gui_font_width = (short)(6 * scale);
    gui_font_height = (short)(8 * scale);
    FontTable[0] = host_font_metrics;
}
void UnloadFont(int f) { (void)f; }
void ResetDisplay(void) {}
int GetFontWidth(int fnt) { (void)fnt; return 6; }
int GetFontHeight(int fnt) { (void)fnt; return 8; }
void DrawLine(int x1, int y1, int x2, int y2, int w, int c) { host_draw_line_pixels(x1, y1, x2, y2, w, c); }
void DrawBox(int x1, int y1, int x2, int y2, int w, int c, int fill) {
    if (fill >= 0) host_fill_rect_pixels(x1, y1, x2, y2, fill);
    if (w > 0) {
        for (int i = 0; i < w; ++i) {
            host_draw_line_pixels(x1 + i, y1 + i, x2 - i, y1 + i, 1, c);
            host_draw_line_pixels(x1 + i, y2 - i, x2 - i, y2 - i, 1, c);
            host_draw_line_pixels(x1 + i, y1 + i, x1 + i, y2 - i, 1, c);
            host_draw_line_pixels(x2 - i, y1 + i, x2 - i, y2 - i, 1, c);
        }
    }
}
void DrawRBox(int x1, int y1, int x2, int y2, int radius, int c, int fill) { (void)x1; (void)y1; (void)x2; (void)y2; (void)radius; (void)c; (void)fill; }
void DrawCircle(int x, int y, int radius, int w, int c, int fill, MMFLOAT aspect) {
    if (radius <= 0) return;
    if (w < 0) w = -w;
    if (aspect <= 0) aspect = 1.0;

    int ry = (int)ceil(radius * aspect);
    MMFLOAT outer = (MMFLOAT)radius + (w > 0 ? (MMFLOAT)w / 2.0 : 0.5);
    MMFLOAT inner = (MMFLOAT)radius - (w > 0 ? (MMFLOAT)w / 2.0 : 0.5);
    MMFLOAT outer2 = outer * outer;
    MMFLOAT inner2 = inner * inner;
    MMFLOAT fill2 = (MMFLOAT)radius * (MMFLOAT)radius;

    for (int py = y - ry - w - 1; py <= y + ry + w + 1; ++py) {
        MMFLOAT dy = ((MMFLOAT)py - (MMFLOAT)y) / aspect;
        for (int px = x - radius - w - 1; px <= x + radius + w + 1; ++px) {
            MMFLOAT dx = (MMFLOAT)px - (MMFLOAT)x;
            MMFLOAT dist2 = dx * dx + dy * dy;
            if (fill >= 0 && dist2 <= fill2) host_put_pixel(px, py, fill);
            if (w > 0 && dist2 <= outer2 && dist2 >= inner2) host_put_pixel(px, py, c);
        }
    }
}
void DrawTriangle(int x0, int y0, int x1, int y1, int x2, int y2, int c, int fill) { (void)x0; (void)y0; (void)x1; (void)y1; (void)x2; (void)y2; (void)c; (void)fill; }
void GUIPrintString(int x, int y, int fnt, int jh, int jv, int jo, int fc, int bc, char *str) {
    int scale = fnt & 0x0F;
    if (scale == 0) scale = 1;
    (void)jo;
    host_draw_text(x, y, scale, jh, jv, fc, bc, str);
}
void ShowCursor(int show) { (void)show; }
int getColour(char *c, int minus) { (void)c; (void)minus; return 0; }
void setmode(int mode, bool clear) { (void)mode; (void)clear; }
int rgb(int r, int g, int b) { return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF); }
void DrawPixel16(int x, int y, int c) { (void)x; (void)y; (void)c; }
void DrawRectangle16(int x1, int y1, int x2, int y2, int c) { (void)x1; (void)y1; (void)x2; (void)y2; (void)c; }
void DrawBitmap16(int x1, int y1, int width, int height, int scale, int fc, int bc, unsigned char *bitmap) { (void)x1; (void)y1; (void)width; (void)height; (void)scale; (void)fc; (void)bc; (void)bitmap; }
void ScrollLCD16(int lines) { (void)lines; }
void DrawBuffer16(int x1, int y1, int x2, int y2, unsigned char *p) { (void)x1; (void)y1; (void)x2; (void)y2; (void)p; }
void DrawBuffer16Fast(int x1, int y1, int x2, int y2, int blank, unsigned char *p) { (void)x1; (void)y1; (void)x2; (void)y2; (void)blank; (void)p; }
void ReadBuffer16(int x1, int y1, int x2, int y2, unsigned char *c) { (void)x1; (void)y1; (void)x2; (void)y2; (void)c; }
void ReadBuffer16Fast(int x1, int y1, int x2, int y2, unsigned char *c) { (void)x1; (void)y1; (void)x2; (void)y2; (void)c; }
void DrawPixelNormal(int x, int y, int c) { (void)x; (void)y; (void)c; }
void ReadBuffer2(int x1, int y1, int x2, int y2, unsigned char *c) { (void)x1; (void)y1; (void)x2; (void)y2; (void)c; }
void copyframetoscreen(uint8_t *s, int xstart, int xend, int ystart, int yend, int odd) { (void)s; (void)xstart; (void)xend; (void)ystart; (void)yend; (void)odd; }
void copybuffertoscreen(unsigned char *s, int lx, int ly, int hx, int hy) { (void)s; (void)lx; (void)ly; (void)hx; (void)hy; }
void closeall3d(void) {}
void closeallsprites(void) {}
void InitDisplayVirtual(void) {}
void ConfigDisplayVirtual(unsigned char *p) { (void)p; }
void merge(uint8_t colour) { (void)colour; }
void blitmerge(int x0, int y0, int w, int h, uint8_t colour) { (void)x0; (void)y0; (void)w; (void)h; (void)colour; }
uint8_t RGB121(uint32_t c) { (void)c; return 0; }
uint8_t RGB332(uint32_t c) { (void)c; return 0; }
uint16_t RGB555(uint32_t c) { (void)c; return 0; }
uint16_t RGB121pack(uint32_t c) { (void)c; return 0; }
void DrawRectangleUser(int x1, int y1, int x2, int y2, int c) { (void)x1; (void)y1; (void)x2; (void)y2; (void)c; }
void DrawBitmapUser(int x1, int y1, int width, int height, int scale, int fc, int bc, unsigned char *bitmap) { (void)x1; (void)y1; (void)width; (void)height; (void)scale; (void)fc; (void)bc; (void)bitmap; }

/* SPI stubs */
void spi_write_command(unsigned char data) { (void)data; }
void spi_write_data(unsigned char data) { (void)data; }
unsigned char SerialPutchar(int comnbr, unsigned char c) { (void)comnbr; (void)c; return c; }
void WriteComand(int cmd) { (void)cmd; }
void WriteData(int data) { (void)data; }
void SPIClose(void) {}
void SPI2Close(void) {}

/* Memory stubs */
unsigned int GetPeekAddr(unsigned char *p) { (void)p; return 0; }
unsigned int GetPokeAddr(unsigned char *p) { (void)p; return 0; }
unsigned char *GetIntAddress(unsigned char *p) { (void)p; return NULL; }
long long int *GetReceiveDataBuffer(unsigned char *p, unsigned int *nbr) { (void)p; (void)nbr; return NULL; }
uint32_t getFreeHeap(void) { return 0; }

/* Regex stubs */
int xregcomp(void *preg, const char *pattern, int cflags) { (void)preg; (void)pattern; (void)cflags; return -1; }
int xregexec(void *preg, const char *string, int nmatch, void *pmatch, int eflags) { (void)preg; (void)string; (void)nmatch; (void)pmatch; (void)eflags; return -1; }
void xregfree(void *preg) { (void)preg; }

/* CRC functions provided by MATHS.c */

/* AES stubs */
void AES_init_ctx(void *ctx, const uint8_t *key) { (void)ctx; (void)key; }
void AES_init_ctx_iv(void *ctx, const uint8_t *key, const uint8_t *iv) { (void)ctx; (void)key; (void)iv; }
void AES_ECB_encrypt(void *ctx, uint8_t *buf) { (void)ctx; (void)buf; }
void AES_ECB_decrypt(void *ctx, uint8_t *buf) { (void)ctx; (void)buf; }
void AES_CBC_encrypt_buffer(void *ctx, uint8_t *buf, int len) { (void)ctx; (void)buf; (void)len; }
void AES_CBC_decrypt_buffer(void *ctx, uint8_t *buf, int len) { (void)ctx; (void)buf; (void)len; }
void AES_CTR_xcrypt_buffer(void *ctx, uint8_t *buf, int len) { (void)ctx; (void)buf; (void)len; }

/* IRQ stubs - irq_set_enabled provided by hardware/irq.h inline */

/* External.c function stubs */
void PinSetBit(int pin, unsigned int offset) { (void)pin; (void)offset; }
/* PinRead is a macro in Draw.h, not a function */
volatile unsigned int GetPinStatus(int pin) { (void)pin; return 0; }
int GetPinBit(int pin) { (void)pin; return 0; }
void WriteCoreTimer(unsigned long timeset) { (void)timeset; }
unsigned long ReadCoreTimer(void) { return 0; }
uint64_t readusclock(void) { return host_time_us_64(); }
void writeusclock(uint64_t timeset) { (void)timeset; }
uint64_t readIRclock(void) { return 0; }
void writeIRclock(uint64_t timeset) { (void)timeset; }
void initExtIO(void) {}
void ExtCfg(int pin, int cfg, int option) { (void)pin; (void)cfg; (void)option; }
void ExtSet(int pin, int val) { (void)pin; (void)val; }
int64_t ExtInp(int pin) { (void)pin; return 0; }
int IsInvalidPin(int pin) { (void)pin; return 1; }
unsigned long ReadCount5(void) { return 0; }
void WriteCount5(unsigned long timeset) { (void)timeset; }
void SetADCFreq(float frequency) { (void)frequency; }
#ifndef PICOCALC
void setBacklight(int level, int frequency) { (void)level; (void)frequency; }
#else
void setBacklight(int level) { (void)level; }
#endif
void gpio_callback(uint gpio, uint32_t events) { (void)gpio; (void)events; }
int CheckPin(int pin, int action) { (void)pin; (void)action; return 0; }
void CallCFuncInt1(void) {}
void CallCFuncInt2(void) {}
void CallCFuncInt3(void) {}
void CallCFuncInt4(void) {}
void IrInit(void) {}
void IrReset(void) {}
void IRSendSignal(int pin, int half_cycles) { (void)pin; (void)half_cycles; }
void TM_EXTI_Handler_5(char *buf, uint32_t events) { (void)buf; (void)events; }
int KeypadCheck(void) { return 0; }
int codemap(int pin) { (void)pin; return 0; }
int codecheck(unsigned char *line) { (void)line; return 0; }
int getslice(int pin) { (void)pin; return 0; }
void setpwm(int pin, int *PWMChannel, int *PWMSlice, MMFLOAT frequency, MMFLOAT duty) { (void)pin; (void)PWMChannel; (void)PWMSlice; (void)frequency; (void)duty; }

/* Editor stubs */
void EditInputLine(void) {}

/* Misc stubs */
void OtherOptions(void) {}
void disable_sd(void) {}
void disable_systemspi(void) {}
void disable_systemi2c(void) {}
void mT4IntEnable(int status) { (void)status; }
void InitReservedIO(void) {}

/* Random number functions provided by MATHS.c */

/* GPS timegm/gmtime stubs -- renamed by host_platform.h macros */
time_t mmbasic_timegm(const struct tm *tm) { struct tm tmp = *tm; return mktime(&tmp); }
struct tm *mmbasic_gmtime(const time_t *timer) { return localtime(timer); }

/* str_replace/STR_REPLACE provided by MATHS.c */

/* Functions that were mistakenly treated as variables */
void DisplayNotSet(void) {}
void ScrollLCDSPISCR(int lines) { (void)lines; }
void Display_Refresh(void) {}

/* cmd_guiBasic from Draw.h */
void cmd_guiBasic(void) {}
