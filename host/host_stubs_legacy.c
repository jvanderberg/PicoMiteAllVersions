/*
 * host_stubs.c - Stub implementations for host (macOS) build
 *
 * Provides stub definitions for symbols from hardware-dependent files
 * (Draw.c, FileIO.c, External.c, Audio.c, SPI-LCD.c, etc.) that are NOT
 * compiled for the host build.  Functions are no-ops or return 0/NULL;
 * variables are zero-initialized globals.
 */

#include <setjmp.h>
#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "bytecode.h"
#include "gfx_pixel_shared.h"
#include "vm_sys_pin.h"
#include "vm_sys_file.h"
#include "vm_host_fat.h"
#include "font1.h"
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include "host_terminal.h"
#include "host_fs.h"
#include "host_sim_audio.h"
#include "host_sim_server.h"
#include "host_time.h"
#include "host_fb.h"
#include "host_keys.h"

/* Forward declarations for output capture */
extern void (*host_output_hook)(const char *text, int len);
static void host_print(const char *s, int len);
static void host_prints(const char *s);

/* host_sd_root is the REPL filesystem root. Declared early so file
 * commands scattered through this file can see it. Defined below. */
extern const char *host_sd_root;
static void host_resolve_sd_path(const char *fname, char *out, size_t out_cap);
static void host_append_default_ext(char *path, size_t cap, const char *ext);

static void host_runtime_check_timeout(void);
static void host_getargaddress(unsigned char *p, long long int **ip, MMFLOAT **fp, int *n);
static int host_parse_pin_arg(unsigned char *arg);

typedef struct {
    unsigned char *expr;
    long long int *ip;
    MMFLOAT *fp;
} HostBoxArgCtx;

static int host_box_arg_get_int(void *ctx, int index);
static void host_pixel_fail_msg(void *ctx, const char *msg);
static void host_pixel_fail_range(void *ctx, const char *label, int value, int min, int max);

/* Framebuffer state (host_framebuffer, dimensions, fastgfx_back) and the
 * FRAMEBUFFER/LAYER backend live in host_fb.c; see host_fb.h. */
static int host_runtime_timeout_ms = 0;
static uint64_t host_runtime_deadline_us = 0;
static int host_runtime_timed_out_flag = 0;
static int host_screenshot_written = 0;
static char host_screenshot_path[1024] = {0};
/* Scripted-key state (host_key_script / host_config_key_*) moved to
 * host_keys.c; see host_keys.h for the consume/peek API. */
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
uint32_t dma_rx_chan = 0;
uint32_t dma_rx_chan2 = 0;
uint32_t dma_tx_chan = 0;
uint32_t dma_tx_chan2 = 0;
bool dmarunning = 0;
long long int *ds18b20Timers = NULL;
volatile int ExtCurrentConfig[NBRPINS + 1] = {0};
union uFileTable FileTable[MAXOPENFILES + 1] = {{0}};
const uint8_t *flash_progmemory = NULL;
unsigned char *FontTable[16] = {(unsigned char *)font1};
int FSerror = 0;
int GPSchannel = 0;
int gui_bcolour = 0;
int gui_fcolour = 0xFFFFFF;
int last_bcolour = 0;
int last_fcolour = 0xFFFFFF;
short gui_font = 0;
short gui_font_height = 8;
short gui_font_width = 6;
short HRes = 320;
short VRes = 320;
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
/* Simulated erased-flash regions so Memory.c's scan loops terminate on the
 * first iteration instead of segfaulting on NULL. */
static unsigned char host_saved_vars_flash_buf[32] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
static unsigned char host_cfunction_flash_buf[32] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
unsigned char *SavedVarsFlash = host_saved_vars_flash_buf;
volatile unsigned int ScrewUpTimer = 0;
/* ScrollLCDSPISCR is a function - see function stubs below */
volatile int ScrollStart = 0;
/* StartEditChar / StartEditPoint are defined in Editor.c now that it is
 * compiled into the host build. */
unsigned char *TickInt[NBRSETTICKS] = {NULL};
volatile int TickTimer[NBRSETTICKS] = {0};
int TickPeriod[NBRSETTICKS] = {0};
volatile unsigned char TickActive[NBRSETTICKS] = {0};
MMFLOAT VCC = 3.3;
bool useoptionangle = 0;
unsigned char WatchdogSet = 0;
/* Break-key state, normally owned by PicoMite.c. Editor.c saves/restores it
 * around the editing session. CTRL-C (0x03) is the MMBasic default. */
unsigned char BreakKey = 3;
/* editactive is now always defined in Editor.c (outside PICOMITEVGA)
 * because FullScreenEditor uses it unconditionally. Keep this comment
 * as a landmark for anyone re-investigating the symbol. */
/* MMAbort is toggled by interrupt handlers on device; on host nothing
 * flips it, but the REPL loop and ExecuteProgram both read it. */
volatile int MMAbort = 0;
bool WAVcomplete = 0;
char *WAVInterrupt = NULL;
volatile unsigned int WDTimer = 0;
/* Display_Refresh is a function - see function stubs below */

/* struct option_s Option */
struct option_s Option = {0};

/* PinDef array */
const struct s_PinDef PinDef[NBRPINS + 1] = {{0}};

/* CFunctionFlash / CFunctionLibrary — point at an "erased flash" buffer so
 * scan loops terminate immediately (see host_cfunction_flash_buf above). */
unsigned char *CFunctionFlash = host_cfunction_flash_buf;
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
unsigned char *DisplayBuf = NULL;
unsigned char *SecondLayer = NULL;
unsigned char *SecondFrame = NULL;
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

/* Pixel primitive used by the rest of this file (draw_line, glyph, etc.).
 * Keeps the same signature as the old static inline so call sites are
 * unchanged. Will be deleted in Phase 2 along with the drawing helpers. */
static void host_draw_pixel_ptr(int x, int y, int c) {
    host_fb_put_pixel(x, y, c);
}

static inline int host_clamp_int(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

/* host_sim_active: set to 1 when --sim mode is active (see
 * host_sim_server_start). Read by host_sim_audio.c and the drawing
 * primitives below to know whether to record events for the WS stream.
 *
 * The tick thread, key queue, cmd stream, and emit_* recorders all live
 * in host_sim_server.c; this file just declares the flag. */
int host_sim_active = 0;

static void host_fill_rect_pixels(int x1, int y1, int x2, int y2, int c) {
    uint32_t *target = host_fb_current_target();
    if (!target) return;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    x1 = host_clamp_int(x1, 0, host_fb_width - 1);
    x2 = host_clamp_int(x2, 0, host_fb_width - 1);
    y1 = host_clamp_int(y1, 0, host_fb_height - 1);
    y2 = host_clamp_int(y2, 0, host_fb_height - 1);
    if (x1 > x2 || y1 > y2) return;

    uint32_t colour = host_fb_colour24(c);
    for (int y = y1; y <= y2; ++y) {
        uint32_t *row = target + (size_t)y * (size_t)host_fb_width;
        for (int x = x1; x <= x2; ++x) {
            row[x] = colour;
        }
    }
#ifdef MMBASIC_SIM
    host_sim_emit_rect(x1, y1, x2, y2, c);
#endif
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

static void host_calc_triangle_edge(int x0, int y0, int x1, int y1, short *xmin, short *xmax) {
    int absX, absY, offX, offY, err, x, y;
    x = x0;
    y = y0;
    if (y >= 0 && y < host_fb_height) {
        if (x < xmin[y]) xmin[y] = (short)x;
        if (x > xmax[y]) xmax[y] = (short)x;
    }
    absX = abs(x1 - x0);
    absY = abs(y1 - y0);
    offX = x0 < x1 ? 1 : -1;
    offY = y0 < y1 ? 1 : -1;
    if (absX > absY) {
        err = absX / 2;
        while (x != x1) {
            err -= absY;
            if (err < 0) {
                y += offY;
                err += absX;
            }
            x += offX;
            if (y >= 0 && y < host_fb_height) {
                if (x < xmin[y]) xmin[y] = (short)x;
                if (x > xmax[y]) xmax[y] = (short)x;
            }
        }
    } else {
        err = absY / 2;
        while (y != y1) {
            err -= absX;
            if (err < 0) {
                x += offX;
                err += absY;
            }
            y += offY;
            if (y >= 0 && y < host_fb_height) {
                if (x < xmin[y]) xmin[y] = (short)x;
                if (x > xmax[y]) xmax[y] = (short)x;
            }
        }
    }
}

static void host_draw_triangle_pixels(int x0, int y0, int x1, int y1,
                                      int x2, int y2, int c, int fill) {
    if (x0 * (y1 - y2) + x1 * (y2 - y0) + x2 * (y0 - y1) == 0) {
        if (y0 > y1) { int t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
        if (y1 > y2) { int t = y2; y2 = y1; y1 = t; t = x2; x2 = x1; x1 = t; }
        if (y0 > y1) { int t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
        host_draw_line_pixels(x0, y0, x2, y2, 1, c);
        return;
    }

    if (fill == -1) {
        host_draw_line_pixels(x0, y0, x1, y1, 1, c);
        host_draw_line_pixels(x1, y1, x2, y2, 1, c);
        host_draw_line_pixels(x2, y2, x0, y0, 1, c);
        return;
    }

    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y1 > y2) { int t = y2; y2 = y1; y1 = t; t = x2; x2 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }

    if (host_fb_height <= 0) return;
    short *xmin = calloc((size_t)host_fb_height, sizeof(short));
    short *xmax = calloc((size_t)host_fb_height, sizeof(short));
    if (!xmin || !xmax) {
        free(xmin);
        free(xmax);
        return;
    }

    for (int y = y0; y <= y2; y++) {
        if (y >= 0 && y < host_fb_height) {
            xmin[y] = 32767;
            xmax[y] = -1;
        }
    }
    host_calc_triangle_edge(x0, y0, x1, y1, xmin, xmax);
    host_calc_triangle_edge(x1, y1, x2, y2, xmin, xmax);
    host_calc_triangle_edge(x2, y2, x0, y0, xmin, xmax);
    for (int y = y0; y <= y2; y++) {
        if (y >= 0 && y < host_fb_height && xmax[y] >= xmin[y]) {
            host_fill_rect_pixels(xmin[y], y, xmax[y], y, fill);
        }
    }
    host_draw_line_pixels(x0, y0, x1, y1, 1, c);
    host_draw_line_pixels(x1, y1, x2, y2, 1, c);
    host_draw_line_pixels(x2, y2, x0, y0, 1, c);
    free(xmin);
    free(xmax);
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
        case 'B': rows[0]=0x1E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x1E; rows[4]=0x11; rows[5]=0x11; rows[6]=0x1E; break;
        case 'C': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x10; rows[3]=0x10; rows[4]=0x10; rows[5]=0x11; rows[6]=0x0E; break;
        case 'D': rows[0]=0x1E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x11; rows[4]=0x11; rows[5]=0x11; rows[6]=0x1E; break;
        case 'E': rows[0]=0x1F; rows[1]=0x10; rows[2]=0x10; rows[3]=0x1E; rows[4]=0x10; rows[5]=0x10; rows[6]=0x1F; break;
        case 'F': rows[0]=0x1F; rows[1]=0x10; rows[2]=0x10; rows[3]=0x1E; rows[4]=0x10; rows[5]=0x10; rows[6]=0x10; break;
        case 'G': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x10; rows[3]=0x17; rows[4]=0x11; rows[5]=0x11; rows[6]=0x0F; break;
        case 'H': rows[0]=0x11; rows[1]=0x11; rows[2]=0x11; rows[3]=0x1F; rows[4]=0x11; rows[5]=0x11; rows[6]=0x11; break;
        case 'I': rows[0]=0x0E; rows[1]=0x04; rows[2]=0x04; rows[3]=0x04; rows[4]=0x04; rows[5]=0x04; rows[6]=0x0E; break;
        case 'J': rows[0]=0x01; rows[1]=0x01; rows[2]=0x01; rows[3]=0x01; rows[4]=0x11; rows[5]=0x11; rows[6]=0x0E; break;
        case 'K': rows[0]=0x11; rows[1]=0x12; rows[2]=0x14; rows[3]=0x18; rows[4]=0x14; rows[5]=0x12; rows[6]=0x11; break;
        case 'L': rows[0]=0x10; rows[1]=0x10; rows[2]=0x10; rows[3]=0x10; rows[4]=0x10; rows[5]=0x10; rows[6]=0x1F; break;
        case 'M': rows[0]=0x11; rows[1]=0x1B; rows[2]=0x15; rows[3]=0x15; rows[4]=0x11; rows[5]=0x11; rows[6]=0x11; break;
        case 'N': rows[0]=0x11; rows[1]=0x19; rows[2]=0x15; rows[3]=0x13; rows[4]=0x11; rows[5]=0x11; rows[6]=0x11; break;
        case 'O': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x11; rows[4]=0x11; rows[5]=0x11; rows[6]=0x0E; break;
        case 'P': rows[0]=0x1E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x1E; rows[4]=0x10; rows[5]=0x10; rows[6]=0x10; break;
        case 'Q': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x11; rows[4]=0x15; rows[5]=0x12; rows[6]=0x0D; break;
        case 'R': rows[0]=0x1E; rows[1]=0x11; rows[2]=0x11; rows[3]=0x1E; rows[4]=0x14; rows[5]=0x12; rows[6]=0x11; break;
        case 'S': rows[0]=0x0F; rows[1]=0x10; rows[2]=0x10; rows[3]=0x0E; rows[4]=0x01; rows[5]=0x01; rows[6]=0x1E; break;
        case 'T': rows[0]=0x1F; rows[1]=0x04; rows[2]=0x04; rows[3]=0x04; rows[4]=0x04; rows[5]=0x04; rows[6]=0x04; break;
        case 'U': rows[0]=0x11; rows[1]=0x11; rows[2]=0x11; rows[3]=0x11; rows[4]=0x11; rows[5]=0x11; rows[6]=0x0E; break;
        case 'V': rows[0]=0x11; rows[1]=0x11; rows[2]=0x11; rows[3]=0x11; rows[4]=0x11; rows[5]=0x0A; rows[6]=0x04; break;
        case 'W': rows[0]=0x11; rows[1]=0x11; rows[2]=0x11; rows[3]=0x15; rows[4]=0x15; rows[5]=0x15; rows[6]=0x0A; break;
        case 'X': rows[0]=0x11; rows[1]=0x11; rows[2]=0x0A; rows[3]=0x04; rows[4]=0x0A; rows[5]=0x11; rows[6]=0x11; break;
        case 'Y': rows[0]=0x11; rows[1]=0x11; rows[2]=0x0A; rows[3]=0x04; rows[4]=0x04; rows[5]=0x04; rows[6]=0x04; break;
        case 'Z': rows[0]=0x1F; rows[1]=0x01; rows[2]=0x02; rows[3]=0x04; rows[4]=0x08; rows[5]=0x10; rows[6]=0x1F; break;
        case '!': rows[0]=0x04; rows[1]=0x04; rows[2]=0x04; rows[3]=0x04; rows[4]=0x04; rows[5]=0x00; rows[6]=0x04; break;
        case '(': rows[0]=0x02; rows[1]=0x04; rows[2]=0x08; rows[3]=0x08; rows[4]=0x08; rows[5]=0x04; rows[6]=0x02; break;
        case ')': rows[0]=0x08; rows[1]=0x04; rows[2]=0x02; rows[3]=0x02; rows[4]=0x02; rows[5]=0x04; rows[6]=0x08; break;
        case ':': rows[0]=0x00; rows[1]=0x04; rows[2]=0x00; rows[3]=0x00; rows[4]=0x00; rows[5]=0x04; rows[6]=0x00; break;
        case '.': rows[0]=0x00; rows[1]=0x00; rows[2]=0x00; rows[3]=0x00; rows[4]=0x00; rows[5]=0x00; rows[6]=0x04; break;
        case ',': rows[0]=0x00; rows[1]=0x00; rows[2]=0x00; rows[3]=0x00; rows[4]=0x00; rows[5]=0x04; rows[6]=0x08; break;
        case '-': rows[0]=0x00; rows[1]=0x00; rows[2]=0x00; rows[3]=0x1F; rows[4]=0x00; rows[5]=0x00; rows[6]=0x00; break;
        case '/': rows[0]=0x01; rows[1]=0x01; rows[2]=0x02; rows[3]=0x04; rows[4]=0x08; rows[5]=0x10; rows[6]=0x10; break;
        case '?': rows[0]=0x0E; rows[1]=0x11; rows[2]=0x01; rows[3]=0x02; rows[4]=0x04; rows[5]=0x00; rows[6]=0x04; break;
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

static int host_font_glyph_bit(const unsigned char *glyph, int width, int height, int col, int row) {
    int bit_number = row * width + col;
    return (glyph[bit_number / 8] >> (((height * width) - bit_number - 1) % 8)) & 1;
}

static void host_plot_text_pixel(int x, int y, int width, int height,
                                 int scale, int orientation,
                                 int col, int row, int sx, int sy, int colour) {
    int px = x;
    int py = y;

    switch (orientation) {
        case ORIENT_INVERTED:
            px = x - (width * scale - 1) + (width - col - 1) * scale + sx;
            py = y - (height * scale - 1) + (height - row - 1) * scale + sy;
            break;
        case ORIENT_CCW90DEG:
            px = x + row * scale + sy;
            py = y - width * scale + (width - col - 1) * scale + sx;
            break;
        case ORIENT_CW90DEG:
            px = x - (height * scale - 1) + (height - row - 1) * scale + sy;
            py = y + col * scale + sx;
            break;
        case ORIENT_VERT:
        case ORIENT_NORMAL:
        default:
            px = x + col * scale + sx;
            py = y + row * scale + sy;
            break;
    }

    host_fb_put_pixel(px, py, colour);
}

static void host_fill_text_cell(int x, int y, int width, int height,
                                int scale, int orientation, int colour) {
    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    host_plot_text_pixel(x, y, width, height, scale, orientation,
                                         col, row, sx, sy, colour);
                }
            }
        }
    }
}

static void host_draw_font_char(int x, int y, int font_index, int scale,
                                int orientation, int fc, int bc, int ch) {
    unsigned char *font = FontTable[font_index];
    if (!font) return;

    int width = font[0];
    int height = font[1];
    if (bc >= 0) host_fill_text_cell(x, y, width, height, scale, orientation, bc);
    if (ch < font[2] || ch >= font[2] + font[3]) return;

    const unsigned char *glyph = font + 4 + (int)(((ch - font[2]) * height * width) / 8);
    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            if (!host_font_glyph_bit(glyph, width, height, col, row)) continue;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    host_plot_text_pixel(x, y, width, height, scale, orientation,
                                         col, row, sx, sy, fc);
                }
            }
        }
    }
}

static void host_draw_text(int x, int y, int fnt, int jh, int jv, int jo, int fc, int bc, const char *str) {
    size_t len = str ? strlen(str) : 0;
    int font_index = fnt >> 4;
    int scale = fnt & 0x0F;
    if (scale == 0) scale = 1;
    if (font_index < 0 || font_index >= FONT_TABLE_SIZE || !FontTable[font_index]) return;

    int char_w = FontTable[font_index][0] * scale;
    int char_h = FontTable[font_index][1] * scale;

    if (jo == ORIENT_NORMAL) {
        if (jh == JUSTIFY_CENTER) x -= ((int)len * char_w) / 2;
        if (jh == JUSTIFY_RIGHT) x -= (int)len * char_w;
        if (jv == JUSTIFY_MIDDLE) y -= char_h / 2;
        if (jv == JUSTIFY_BOTTOM) y -= char_h;
    } else if (jo == ORIENT_VERT) {
        if (jh == JUSTIFY_CENTER) x -= char_w / 2;
        if (jh == JUSTIFY_RIGHT) x -= char_w;
        if (jv == JUSTIFY_MIDDLE) y -= ((int)len * char_h) / 2;
        if (jv == JUSTIFY_BOTTOM) y -= (int)len * char_h;
    } else if (jo == ORIENT_INVERTED) {
        if (jh == JUSTIFY_CENTER) x += ((int)len * char_w) / 2;
        if (jh == JUSTIFY_RIGHT) x += (int)len * char_w;
        if (jv == JUSTIFY_MIDDLE) y += char_h / 2;
        if (jv == JUSTIFY_BOTTOM) y += char_h;
    } else if (jo == ORIENT_CCW90DEG) {
        if (jh == JUSTIFY_CENTER) x -= char_h / 2;
        if (jh == JUSTIFY_RIGHT) x -= char_h;
        if (jv == JUSTIFY_MIDDLE) y += ((int)len * char_w) / 2;
        if (jv == JUSTIFY_BOTTOM) y += (int)len * char_w;
    } else if (jo == ORIENT_CW90DEG) {
        if (jh == JUSTIFY_CENTER) x += char_h / 2;
        if (jh == JUSTIFY_RIGHT) x += char_h;
        if (jv == JUSTIFY_MIDDLE) y -= ((int)len * char_w) / 2;
        if (jv == JUSTIFY_BOTTOM) y -= (int)len * char_w;
    }

    for (size_t i = 0; i < len; ++i) {
        host_draw_font_char(x, y, font_index, scale, jo, fc, bc, (unsigned char)str[i]);
        if (jo == ORIENT_NORMAL) x += char_w;
        else if (jo == ORIENT_VERT) y += char_h;
        else if (jo == ORIENT_INVERTED) x -= char_w;
        else if (jo == ORIENT_CCW90DEG) y -= char_w;
        else if (jo == ORIENT_CW90DEG) y += char_w;
    }
}

/* Thin wrapper around host_fb_write_screenshot that honors the once-per-
 * runtime-session guard; host_runtime_configure resets the flag. */
static void host_write_screenshot(const char *path) {
    if (!path || !*path || host_screenshot_written) return;
    host_fb_write_screenshot(path);
    host_screenshot_written = 1;
}

/* host_parse_escaped_char, host_load_key_script, host_keys_ready,
 * host_keydown, host_runtime_configure_keys all moved to host_keys.c. */

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
    host_runtime_keys_load();
    timeroffset = host_time_us_64();
    host_runtime_deadline_us = 0;
    if (host_runtime_timeout_ms > 0) {
        host_runtime_deadline_us = timeroffset + (uint64_t)host_runtime_timeout_ms * 1000ULL;
    }
    host_framebuffer_reset_runtime(gui_bcolour);
    FontTable[0] = (unsigned char *)font1;
    DrawPixel = host_draw_pixel_ptr;
    DrawRectangle = host_fb_draw_rectangle;
    DrawBitmap = host_fb_draw_bitmap;
    ScrollLCD = host_fb_scroll_lcd;
}

void host_runtime_finish(void) {
    host_framebuffer_service();
    if (host_screenshot_path[0]) {
        host_write_screenshot(host_screenshot_path);
    }
}

int host_runtime_timed_out(void) {
    return host_runtime_timed_out_flag;
}

/*
 * --slowdown throttle. Non-zero means sleep this many microseconds per
 * poll-tick. The interpreter pokes host_runtime_check_timeout on every
 * statement / MMInkey / routinechecks call; the VM pokes
 * host_sim_apply_slowdown from bc_vm_poll_interrupts on every backward
 * branch. host_sleep_us bumps the msec counter so PAUSE / TIMER / tick
 * interrupts stay on real wall-clock time even when execution crawls.
 */
int host_sim_slowdown_us = 0;

void host_sim_apply_slowdown(void) {
    if (host_sim_slowdown_us > 0) host_sleep_us((uint64_t)host_sim_slowdown_us);
}

static void host_runtime_check_timeout(void) {
    host_framebuffer_service();
    host_sim_apply_slowdown();
    /* Always refresh the msec/CursorTimer so code that polls without
     * going through host_sleep_us (e.g. the Editor's ShowCursor+MMInkey
     * loop) still sees time advance. On device the 1ms timer IRQ does
     * this; here we piggy-back on every MMInkey/routinechecks call. */
    uint64_t now = host_time_us_64();
    if (!host_runtime_deadline_us || host_runtime_timed_out_flag) return;
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
static void host_cmd_single_path(void (*fn)(const char *), const char *msg) {
    char *path = (char *)getFstring(cmdline);
    if (!path || !*path) error((char *)msg);
    fn(path);
}

static int host_file_copy_mode_from_string(unsigned char *mode_text) {
    if (str_equal(mode_text, (const unsigned char *)"A2A")) return 1;
    if (str_equal(mode_text, (const unsigned char *)"A2B")) return 2;
    if (str_equal(mode_text, (const unsigned char *)"B2A")) return 3;
    if (str_equal(mode_text, (const unsigned char *)"B2B")) return 4;
    return 0;
}

static int host_parse_pin_arg(unsigned char *arg) {
    unsigned char *p = arg;
    skipspace(p);
    if ((p[0] == 'G' || p[0] == 'g') && (p[1] == 'P' || p[1] == 'p'))
        return codemap(getinteger(p + 2));
    return getinteger(p);
}

void cmd_camera(void) {}
void cmd_cfunction(void) {}
void cmd_chdir(void) { host_cmd_single_path(vm_sys_file_chdir, "File name"); }
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
void cmd_close(void) {
    int i, fnbr;

    getargs(&cmdline, (MAX_ARG_COUNT * 2) - 1, (unsigned char *)",");
    if ((argc & 1) == 0) error("Syntax");
    for (i = 0; i < argc; i += 2) {
        if (*argv[i] == '#') argv[i]++;
        fnbr = getint(argv[i], 1, MAXOPENFILES);
        if (FileTable[fnbr].com == 0) error("File number is not open");
        FileClose(fnbr);
    }
}
void cmd_cls(void) {
    int colour = gui_bcolour;
    if (cmdline && *cmdline) colour = getint(cmdline, 0, WHITE);
    ClearScreen(colour);
    CurrentX = 0;
    CurrentY = 0;
}
void cmd_colour(void) {}
void cmd_configure(void) {}
void cmd_copy(void) {
    char split[2];
    int mode = 0;
    char *from_name;
    char *to_name;
    unsigned char *p = cmdline;

    split[0] = tokenTO;
    split[1] = 0;
    if ((checkstring(p, (unsigned char *)"A2A")) || (checkstring(p, (unsigned char *)"A2B")) ||
        (checkstring(p, (unsigned char *)"B2A")) || (checkstring(p, (unsigned char *)"B2B"))) {
        unsigned char mode_buf[8] = {0};
        int i = 0;
        while (*p && !isspace(*p) && i < (int)sizeof(mode_buf) - 1) mode_buf[i++] = *p++;
        mode = host_file_copy_mode_from_string(mode_buf);
    }
    getargs(&p, 3, (unsigned char *)split);
    if (argc != 3) error("Syntax");
    from_name = (char *)getFstring(argv[0]);
    to_name = (char *)getFstring(argv[2]);
    vm_sys_file_copy(from_name, to_name, mode);
}
void cmd_cpu(void) {}
void cmd_csubinterrupt(void) {}
void cmd_date(void) {}
void cmd_device(void) {}
void cmd_DHT22(void) {}
void cmd_disk(void) { host_cmd_single_path(vm_sys_file_drive, "Invalid disk"); }
void cmd_ds18b20(void) {}
/* cmd_edit / cmd_editfile are provided by the real Editor.c now. */
void cmd_endprogram(void) {}
void bc_fastgfx_swap(void) {
    /* Present the back buffer: copy it into host_framebuffer so local
     * snapshots stay correct, and emit one BLIT command so browsers
     * get a single full-frame update. Unlike the per-primitive stream
     * this is one message per visible frame — browsers never see a
     * torn or in-progress frame. */
    host_fb_ensure();
    if (!host_fastgfx_active || !host_fastgfx_back || !host_framebuffer) return;
    size_t pixels = (size_t)host_fb_width * (size_t)host_fb_height;
    memcpy(host_framebuffer, host_fastgfx_back, pixels * sizeof(uint32_t));
#ifdef MMBASIC_SIM
    host_sim_emit_blit(0, 0, host_fb_width, host_fb_height, host_fastgfx_back);
#endif
}
void bc_fastgfx_sync(void) {
    if (!host_fastgfx_active || host_fastgfx_fps <= 0) return;
    uint64_t frame_us = 1000000ULL / (uint64_t)host_fastgfx_fps;
    uint64_t now = host_time_us_64();
    if (host_fastgfx_next_sync_us == 0) host_fastgfx_next_sync_us = now + frame_us;
    if (now < host_fastgfx_next_sync_us) host_sleep_us(host_fastgfx_next_sync_us - now);
    host_fastgfx_next_sync_us += frame_us;
}

void bc_fastgfx_create(void) {
    host_fb_ensure();
    size_t pixels = (size_t)host_fb_width * (size_t)host_fb_height;
    if (!host_fastgfx_back) {
        host_fastgfx_back = calloc(pixels, sizeof(uint32_t));
        if (!host_fastgfx_back) error("Not enough memory");
    }
    /* Seed the back buffer with the current front contents so existing
     * text/graphics aren't wiped out at the start of the first frame. */
    if (host_framebuffer) memcpy(host_fastgfx_back, host_framebuffer, pixels * sizeof(uint32_t));
    /* Point the graphics WriteBuf at the back buffer so every primitive
     * draws into it until CLOSE. host_fb_current_target looks at
     * WriteBuf to decide the destination. */
    WriteBuf = (unsigned char *)host_fastgfx_back;
    host_fastgfx_active = 1;
    host_fastgfx_next_sync_us = 0;
}

void bc_fastgfx_close(void) {
    if (!host_fastgfx_active) error("FASTGFX not active");
    /* Flip the final back contents out to the front so the last frame
     * the game drew stays visible after CLOSE. */
    if (host_fastgfx_back && host_framebuffer) {
        size_t pixels = (size_t)host_fb_width * (size_t)host_fb_height;
        memcpy(host_framebuffer, host_fastgfx_back, pixels * sizeof(uint32_t));
    }
    free(host_fastgfx_back);
    host_fastgfx_back = NULL;
    WriteBuf = NULL;
    host_fastgfx_active = 0;
    host_fastgfx_next_sync_us = 0;
}

void bc_fastgfx_reset(void) {
    free(host_fastgfx_back);
    host_fastgfx_back = NULL;
    WriteBuf = NULL;
    host_fastgfx_active = 0;
    host_fastgfx_next_sync_us = 0;
}

void bc_fastgfx_set_fps(int fps) {
    if (fps < 1 || fps > 1000) error("Number out of bounds");
    host_fastgfx_fps = fps;
    host_fastgfx_next_sync_us = 0;
}

void cmd_fastgfx(void) {
    unsigned char *p;

    if ((p = checkstring(cmdline, (unsigned char *)"CREATE"))) {
        checkend(p);
        bc_fastgfx_create();
        return;
    }
    if ((p = checkstring(cmdline, (unsigned char *)"CLOSE"))) {
        checkend(p);
        bc_fastgfx_close();
        return;
    }
    if ((p = checkstring(cmdline, (unsigned char *)"SWAP"))) {
        checkend(p);
        bc_fastgfx_swap();
        return;
    }
    if ((p = checkstring(cmdline, (unsigned char *)"SYNC"))) {
        checkend(p);
        bc_fastgfx_sync();
        return;
    }
    if ((p = checkstring(cmdline, (unsigned char *)"FPS"))) {
        bc_fastgfx_set_fps(getint(p, 1, 1000));
        return;
    }

    error("Syntax");
}
/* FILES — lists entries in host_sd_root (REPL mode) or the in-memory
 * FAT disk (test harness). POSIX directory walking lives in host_fs.c
 * to stay isolated from FatFS's clashing DIR type. */
static void host_files_emit(const char *name) {
    MMPrintString((char *)name);
    MMPrintString("\r\n");
}

void cmd_files(void) {
    char *pattern_arg = NULL;
    if (cmdline && *cmdline) pattern_arg = (char *)getFstring(cmdline);

    if (!host_sd_root) {
        vm_sys_file_files(pattern_arg);
        return;
    }

    if (host_fs_list_dir(host_sd_root, pattern_arg, host_files_emit) != 0) {
        error("File error");
    }
}
void cmd_flash(void) {}
void cmd_flush(void) {}
void cmd_font(void) {
    getargs(&cmdline, 3, (unsigned char *)",");
    if (argc < 1) error("Argument count");
    if (*argv[0] == '#') ++argv[0];
    if (argc == 3)
        SetFont(((getint(argv[0], 1, FONT_TABLE_SIZE) - 1) << 4) | getint(argv[2], 1, 15));
    else
        SetFont(((getint(argv[0], 1, FONT_TABLE_SIZE) - 1) << 4) | 1);
    PromptFont = gui_font;
}
void cmd_framebuffer(void) {
    unsigned char *p = NULL;

    if ((p = checkstring(cmdline, (unsigned char *)"CREATE"))) {
        if (checkstring(p, (unsigned char *)"FAST")) {
            /* FAST flag accepted but no-op on host (no DMA) */
        } else {
            checkend(p);
        }
        host_framebuffer_create();
        return;
    }
    if ((p = checkstring(cmdline, (unsigned char *)"LAYER"))) {
        getargs(&p, 1, (unsigned char *)",");
        if (argc == 0) {
            host_framebuffer_layer(0, 0);
            return;
        }
        if (argc != 1) error("Syntax");
        host_framebuffer_layer(1, getint(argv[0], 0, 0xFFFFFF));
        return;
    }
    if ((p = checkstring(cmdline, (unsigned char *)"WRITE"))) {
        if (checkstring(p, (unsigned char *)"N")) {
            host_framebuffer_write('N');
            return;
        }
        if (checkstring(p, (unsigned char *)"F")) {
            host_framebuffer_write('F');
            return;
        }
        if (checkstring(p, (unsigned char *)"L")) {
            host_framebuffer_write('L');
            return;
        }
        getargs(&p, 1, (unsigned char *)",");
        if (argc != 1) error("Syntax");
        {
            char *q = (char *)getCstring(argv[0]);
            if (strcasecmp(q, "N") == 0) host_framebuffer_write('N');
            else if (strcasecmp(q, "F") == 0) host_framebuffer_write('F');
            else if (strcasecmp(q, "L") == 0) host_framebuffer_write('L');
            else error("Syntax");
        }
        return;
    }
    if ((p = checkstring(cmdline, (unsigned char *)"SYNC"))) {
        checkend(p);
        host_framebuffer_sync();
        return;
    }
    if ((p = checkstring(cmdline, (unsigned char *)"WAIT"))) {
        checkend(p);
        host_framebuffer_wait();
        return;
    }
    if ((p = checkstring(cmdline, (unsigned char *)"COPY"))) {
        char from = 0;
        char to = 0;
        int background = 0;
        getargs(&p, 5, (unsigned char *)",");
        if (!(argc == 3 || argc == 5)) error("Syntax");
        if (checkstring(argv[0], (unsigned char *)"N")) from = 'N';
        else if (checkstring(argv[0], (unsigned char *)"F")) from = 'F';
        else if (checkstring(argv[0], (unsigned char *)"L")) from = 'L';
        else {
            char *q = (char *)getCstring(argv[0]);
            if (strcasecmp(q, "N") == 0) from = 'N';
            else if (strcasecmp(q, "F") == 0) from = 'F';
            else if (strcasecmp(q, "L") == 0) from = 'L';
            else error("Syntax");
        }
        if (checkstring(argv[2], (unsigned char *)"N")) to = 'N';
        else if (checkstring(argv[2], (unsigned char *)"F")) to = 'F';
        else if (checkstring(argv[2], (unsigned char *)"L")) to = 'L';
        else {
            char *q = (char *)getCstring(argv[2]);
            if (strcasecmp(q, "N") == 0) to = 'N';
            else if (strcasecmp(q, "F") == 0) to = 'F';
            else if (strcasecmp(q, "L") == 0) to = 'L';
            else error("Syntax");
        }
        if (argc == 5) {
            if (!checkstring(argv[4], (unsigned char *)"B")) error("Syntax");
            background = 1;
        }
        host_framebuffer_copy(from, to, background);
        return;
    }
    if ((p = checkstring(cmdline, (unsigned char *)"MERGE"))) {
        int colour = 0;
        int has_colour = 0;
        int mode = BC_FB_MERGE_MODE_NOW;
        int has_rate = 0;
        int rate_ms = 0;
        getargs(&p, 5, (unsigned char *)",");
        if (argc >= 1 && *argv[0]) {
            colour = getint(argv[0], 0, 0xFFFFFF);
            has_colour = 1;
        }
        if (argc >= 3 && *argv[2]) {
            if (checkstring(argv[2], (unsigned char *)"B")) mode = BC_FB_MERGE_MODE_B;
            else if (checkstring(argv[2], (unsigned char *)"R")) mode = BC_FB_MERGE_MODE_R;
            else if (checkstring(argv[2], (unsigned char *)"A")) mode = BC_FB_MERGE_MODE_A;
            else error("Syntax");
        }
        if (argc == 5 && *argv[4]) {
            rate_ms = getint(argv[4], 0, 600000);
            has_rate = 1;
        }
        host_framebuffer_merge(has_colour, colour, mode, has_rate, rate_ms);
        return;
    }
    if ((p = checkstring(cmdline, (unsigned char *)"CLOSE"))) {
        if (checkstring(p, (unsigned char *)"F")) {
            host_framebuffer_close('F');
            return;
        }
        if (checkstring(p, (unsigned char *)"L")) {
            host_framebuffer_close('L');
            return;
        }
        checkend(p);
        host_framebuffer_close('A');
        return;
    }

    error("Syntax");
}
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
void cmd_kill(void) { host_cmd_single_path(vm_sys_file_kill, "File name"); }
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
/* Defined in host_main.c — reused here so cmd_load doesn't reimplement
 * the host tokeniser path. */
extern char *read_basic_source_file(const char *filename);
extern int load_basic_source(const char *source);

/* When set (REPL mode with --sd-root DIR, or cwd by default), file
 * commands operate on the real filesystem rooted here rather than on
 * the in-memory FAT disk. NULL for the test harness — commands that
 * require filesystem access error rather than scribble on the user's
 * real files. */
const char *host_sd_root = NULL;

static void host_resolve_sd_path(const char *fname, char *out, size_t out_cap) {
    if (!host_sd_root) { error("No SD root configured"); return; }
    /* Absolute paths pass through unchanged. Relative paths get joined to
     * host_sd_root with a single '/'. */
    if (fname[0] == '/') {
        if (strlen(fname) >= out_cap) error("File name too long");
        strcpy(out, fname);
        return;
    }
    size_t rl = strlen(host_sd_root);
    size_t fl = strlen(fname);
    int need_sep = (rl > 0 && host_sd_root[rl - 1] != '/');
    if (rl + (need_sep ? 1 : 0) + fl + 1 > out_cap) error("File name too long");
    memcpy(out, host_sd_root, rl);
    if (need_sep) out[rl++] = '/';
    memcpy(out + rl, fname, fl + 1);
}

static void host_append_default_ext(char *path, size_t cap, const char *ext) {
    if (strchr(path, '.')) return;
    size_t n = strlen(path);
    size_t el = strlen(ext);
    if (n + el + 1 > cap) return;
    memcpy(path + n, ext, el + 1);
}

void cmd_load(void) {
    char *fname = (char *)getFstring(cmdline);
    if (!fname || !*fname) error("File name");

    char path[4096];
    host_resolve_sd_path(fname, path, sizeof(path));
    host_append_default_ext(path, sizeof(path), ".bas");

    char *source = read_basic_source_file(path);
    if (!source) error("Cannot open file");

    ClearRuntime(true);
    int rc = load_basic_source(source);
    free(source);
    if (rc != 0) error("Cannot parse file");
    PrepareProgram(false);
    /* load_basic_source leaves the last file line in inpbuf; the prompt
     * loop's EditInputLine would then echo it as if the user had typed
     * it. Clear before longjmp, same pattern as cmd_new. */
    memset(inpbuf, 0, STRINGSIZE);
    /* cmd_load tokenises into tknbuf as it loads, which corrupts the
     * tknbuf that ExecuteProgram is currently iterating. Jump back to
     * the prompt so ExecuteProgram doesn't read garbage off the end
     * of our LOAD command. Matches the pattern used by cmd_new. */
    longjmp(mark, 1);
}
void cmd_longString(void) {}
void cmd_mkdir(void) { host_cmd_single_path(vm_sys_file_mkdir, "File name"); }
void cmd_mouse(void) {}
void cmd_mov(void) {}
void cmd_name(void) {
    char split[2];
    char *old_name;
    char *new_name;
    split[0] = tokenAS;
    split[1] = 0;
    getargs(&cmdline, 3, (unsigned char *)split);
    if (argc != 3) error("Syntax");
    old_name = (char *)getFstring(argv[0]);
    new_name = (char *)getFstring(argv[2]);
    vm_sys_file_rename(old_name, new_name);
}
void cmd_nop(void) {}
void cmd_Nunchuck(void) {}
void cmd_onewire(void) {}
static int host_file_mode_from_string(unsigned char *mode_text) {
    if (str_equal(mode_text, (const unsigned char *)"OUTPUT"))
        return FA_WRITE | FA_CREATE_ALWAYS;
    if (str_equal(mode_text, (const unsigned char *)"APPEND"))
        return FA_WRITE | FA_OPEN_APPEND;
    if (str_equal(mode_text, (const unsigned char *)"INPUT"))
        return FA_READ;
    error("File access mode");
    return 0;
}

void cmd_open(void) {
    char split[4];
    char *fname;
    int fnbr;
    int mode;

    split[0] = tokenAS;
    split[1] = tokenFOR;
    split[2] = ',';
    split[3] = 0;
    getargs(&cmdline, 7, (unsigned char *)split);
    if (argc != 5) error("Syntax");

    fname = (char *)getFstring(argv[0]);
    mode = host_file_mode_from_string(argv[2]);
    if (*argv[4] == '#') argv[4]++;
    fnbr = getint(argv[4], 1, MAXOPENFILES);
    BasicFileOpen(fname, fnbr, mode);
}
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
static void host_getargaddress(unsigned char *p, long long int **ip, MMFLOAT **fp, int *n) {
    unsigned char *ptr = NULL;
    char pp[STRINGSIZE] = {0};

    *fp = NULL;
    *ip = NULL;
    strcpy(pp, (char *)p);
    if (!isnamestart(pp[0])) {
        *n = 1;
        return;
    }

    ptr = findvar((unsigned char *)pp, V_FIND | V_EMPTY_OK | V_NOFIND_NULL);
    if (ptr && (g_vartbl[g_VarIndex].type & (T_NBR | T_INT))) {
        if (g_vartbl[g_VarIndex].dims[0] <= 0) {
            *n = 1;
            return;
        } else {
            if (*n == 0) *n = g_vartbl[g_VarIndex].dims[0] + 1 - g_OptionBase;
            else {
                int an = g_vartbl[g_VarIndex].dims[0] + 1 - g_OptionBase;
                *n = an < *n ? an : *n;
            }
            skipspace(p);
            do {
                p++;
            } while (isnamechar(*p));
            if (*p == '!' || *p == '%') p++;
            if (*p == '(') {
                p++;
                skipspace(p);
                if (*p != ')') {
                    *n = 1;
                    return;
                }
            }
        }
        if (g_vartbl[g_VarIndex].dims[1] != 0) error("Invalid variable");
        if (g_vartbl[g_VarIndex].type & T_NBR) *fp = (MMFLOAT *)ptr;
        else *ip = (long long int *)ptr;
    } else {
        *n = 1;
    }
}

static int host_box_arg_get_int(void *ctx, int index) {
    HostBoxArgCtx *arg = (HostBoxArgCtx *)ctx;
    if (arg->ip != NULL) return (int)arg->ip[index];
    if (arg->fp != NULL) return (int)arg->fp[index];
    return getinteger(arg->expr);
}

static void host_pixel_fail_msg(void *ctx, const char *msg) {
    (void)ctx;
    error((char *)msg);
}

static void host_pixel_fail_range(void *ctx, const char *label, int value, int min, int max) {
    (void)ctx;
    (void)label;
    error("% is invalid (valid is % to %)", value, min, max);
}

void cmd_pixel(void) {
    GfxPixelArg args[GFX_PIXEL_ARG_COUNT] = {0};
    HostBoxArgCtx arg_ctx[GFX_PIXEL_ARG_COUNT] = {0};
    GfxPixelErrorSink errors;
    int n = 0;

    getargs(&cmdline, 5, (unsigned char *)",");
    if (!(argc == 3 || argc == 5)) error("Argument count");
    errors.ctx = NULL;
    errors.fail_msg = host_pixel_fail_msg;
    errors.fail_range = host_pixel_fail_range;

    arg_ctx[0].expr = argv[0];
    host_getargaddress(argv[0], &arg_ctx[0].ip, &arg_ctx[0].fp, &n);
    args[0].present = 1;
    args[0].count = n;
    args[0].ctx = &arg_ctx[0];
    args[0].get_int = host_box_arg_get_int;

    arg_ctx[1].expr = argv[2];
    if (n != 1) host_getargaddress(argv[2], &arg_ctx[1].ip, &arg_ctx[1].fp, &n);
    else n = 1;
    args[1].present = 1;
    args[1].count = n;
    args[1].ctx = &arg_ctx[1];
    args[1].get_int = host_box_arg_get_int;

    if (argc == 5 && *argv[4]) {
        arg_ctx[2].expr = argv[4];
        host_getargaddress(argv[4], &arg_ctx[2].ip, &arg_ctx[2].fp, &args[2].count);
        args[2].present = 1;
        args[2].ctx = &arg_ctx[2];
        args[2].get_int = host_box_arg_get_int;
    }

    gfx_pixel_execute((n == 1) ? GFX_PIXEL_MODE_SCALAR : GFX_PIXEL_MODE_VECTOR,
                      args, (argc + 1) / 2, &errors);
}
/*
 * Minimal PLAY implementation for the host simulator. Parses the same
 * subcommands as Audio.c cmd_play(), validates arguments device-style,
 * and emits JSON events over the WS transport so web/audio.js can
 * reproduce the sound in WebAudio. File-based playback (WAV / FLAC /
 * MP3 / MODFILE) is intentionally unimplemented — defer to Phase 5.
 */
static int host_play_parse_channel(unsigned char *arg, int *left, int *right) {
    *left = 0; *right = 0;
    if (checkstring(arg, (unsigned char *)"L")) { *left = 1; return 1; }
    if (checkstring(arg, (unsigned char *)"R")) { *right = 1; return 1; }
    if (checkstring(arg, (unsigned char *)"B")) { *left = *right = 1; return 1; }
    char *p = (char *)getCstring(arg);
    if (!strcasecmp(p, "L")) { *left = 1; return 1; }
    if (!strcasecmp(p, "R")) { *right = 1; return 1; }
    if (!strcasecmp(p, "B") || !strcasecmp(p, "M")) { *left = *right = 1; return 1; }
    return 0;
}

static const char *host_play_parse_type(unsigned char *arg) {
    if (checkstring(arg, (unsigned char *)"O")) return "O";
    if (checkstring(arg, (unsigned char *)"Q")) return "Q";
    if (checkstring(arg, (unsigned char *)"T")) return "T";
    if (checkstring(arg, (unsigned char *)"W")) return "W";
    if (checkstring(arg, (unsigned char *)"S")) return "S";
    if (checkstring(arg, (unsigned char *)"P")) return "P";
    if (checkstring(arg, (unsigned char *)"N")) return "N";
    if (checkstring(arg, (unsigned char *)"U")) return "U";
    char *p = (char *)getCstring(arg);
    if (!strcasecmp(p, "O")) return "O";
    if (!strcasecmp(p, "Q")) return "Q";
    if (!strcasecmp(p, "T")) return "T";
    if (!strcasecmp(p, "W")) return "W";
    if (!strcasecmp(p, "S")) return "S";
    if (!strcasecmp(p, "P")) return "P";
    if (!strcasecmp(p, "N")) return "N";
    if (!strcasecmp(p, "U")) return "U";
    return NULL;
}

void cmd_play(void) {
    unsigned char *tp;

    if (checkstring(cmdline, (unsigned char *)"STOP")) {
        host_sim_audio_stop();
        return;
    }
    if (checkstring(cmdline, (unsigned char *)"PAUSE")) {
        host_sim_audio_pause();
        return;
    }
    if (checkstring(cmdline, (unsigned char *)"RESUME")) {
        host_sim_audio_resume();
        return;
    }
    if (checkstring(cmdline, (unsigned char *)"CLOSE")) {
        host_sim_audio_stop();
        return;
    }
    if ((tp = checkstring(cmdline, (unsigned char *)"VOLUME"))) {
        getargs(&tp, 3, (unsigned char *)",");
        if (argc < 1) error("Argument count");
        int vl = 100, vr = 100;
        if (*argv[0]) vl = getint(argv[0], 0, 100);
        vr = vl;
        if (argc == 3) vr = getint(argv[2], 0, 100);
        host_sim_audio_volume(vl, vr);
        return;
    }
    if ((tp = checkstring(cmdline, (unsigned char *)"TONE"))) {
        getargs(&tp, 7, (unsigned char *)",");
        if (!(argc == 3 || argc == 5 || argc == 7)) error("Argument count");
        MMFLOAT f_left = getnumber(argv[0]);
        MMFLOAT f_right = getnumber(argv[2]);
        if (f_left < 0.0 || f_left > 22050.0) error("Valid is 0Hz to 20KHz");
        if (f_right < 0.0 || f_right > 22050.0) error("Valid is 0Hz to 20KHz");
        int has_dur = 0;
        long long dur_ms = 0;
        if (argc > 4) {
            dur_ms = getint(argv[4], 0, INT_MAX);
            has_dur = 1;
            if (dur_ms == 0) return;
        }
        /* Interrupt arg (argv[6]) is ignored on host — WAV interrupts
         * aren't plumbed through --sim. */
        host_sim_audio_tone((double)f_left, (double)f_right, has_dur, dur_ms);
        return;
    }
    if ((tp = checkstring(cmdline, (unsigned char *)"SOUND"))) {
        getargs(&tp, 9, (unsigned char *)",");
        if (!(argc == 5 || argc == 7 || argc == 9)) error("Argument count");
        int slot = getint(argv[0], 1, 4);
        int left = 0, right = 0;
        if (!host_play_parse_channel(argv[2], &left, &right))
            error("Position must be L, R, or B");
        const char *type = host_play_parse_type(argv[4]);
        if (!type) error("Invalid type");
        if (!left && !right) error("Position must be L, R, or B");
        if (argc == 5 && strcmp(type, "O") != 0) error("Argument count");
        MMFLOAT f_in = 10.0;
        if (argc >= 7) f_in = getnumber(argv[6]);
        if (f_in < 1.0 || f_in > 20000.0) error("Valid is 1Hz to 20KHz");
        int vol = 25;
        if (argc == 9) vol = getint(argv[8], 0, 25);
        const char *ch = (left && right) ? "B" : (left ? "L" : "R");
        host_sim_audio_sound(slot, ch, type, (double)f_in, vol);
        return;
    }
    if (checkstring(cmdline, (unsigned char *)"NEXT") ||
        checkstring(cmdline, (unsigned char *)"PREVIOUS") ||
        checkstring(cmdline, (unsigned char *)"LOAD SOUND")) {
        /* No-op on host: no MOD/FLAC/MP3 player state to step through. */
        return;
    }
    error("Unsupported on host: PLAY WAV/FLAC/MP3/MODFILE etc.");
}
void cmd_poke(void) {}
static void host_fill_polygon_edges(const float *poly_x, const float *poly_y,
                                    int vertex_count, int count,
                                    int ystart, int yend,
                                    int c, int f) {
    float *node_x = (float *)malloc((size_t)count * sizeof(float));
    int y, i, j;

    if (!node_x) error("Not enough memory");

    for (y = ystart; y < yend; y++) {
        int nodes = 0;
        float temp;
        j = vertex_count - 1;
        for (i = 0; i < vertex_count; i++) {
            if ((poly_y[i] < (float)y && poly_y[j] >= (float)y) ||
                (poly_y[j] < (float)y && poly_y[i] >= (float)y)) {
                node_x[nodes++] = (poly_x[i] +
                                   ((float)y - poly_y[i]) /
                                       (poly_y[j] - poly_y[i]) *
                                       (poly_x[j] - poly_x[i]));
            }
            j = i;
        }

        for (i = 1; i < nodes; i++) {
            temp = node_x[i];
            for (j = i; j > 0 && temp < node_x[j - 1]; j--) node_x[j] = node_x[j - 1];
            node_x[j] = temp;
        }

        for (i = 0; i + 1 < nodes; i += 2) {
            int xstart = (int)floorf(node_x[i]) + 1;
            int xend = (int)ceilf(node_x[i + 1]) - 1;
            DrawLine(xstart, y, xend, y, 1, f);
        }
    }

    for (i = 0; i < vertex_count; i++) {
        int x0 = (int)roundf(poly_x[i]);
        int y0 = (int)roundf(poly_y[i]);
        int x1 = (int)roundf(poly_x[(i + 1) % vertex_count]);
        int y1 = (int)roundf(poly_y[(i + 1) % vertex_count]);
        DrawLine(x0, y0, x1, y1, 1, c);
    }
    free(node_x);
}

static void host_draw_polygon_points(const int *x_values, const int *y_values,
                                     int point_count, int c, int f, int close) {
    int ymax = 0;
    int ymin = 1000000;
    int vertex_count = 0;
    float *poly_x;
    float *poly_y;

    if (point_count <= 0) return;
    if (f < 0) {
        int i;
        for (i = 0; i < point_count - 1; i++)
            DrawLine(x_values[i], y_values[i], x_values[i + 1], y_values[i + 1], 1, c);
        if (close)
            DrawLine(x_values[point_count - 1], y_values[point_count - 1], x_values[0], y_values[0], 1, c);
        return;
    }

    poly_x = (float *)malloc((size_t)(point_count + 1) * sizeof(float));
    poly_y = (float *)malloc((size_t)(point_count + 1) * sizeof(float));
    if (!poly_x || !poly_y) {
        if (poly_x) free(poly_x);
        if (poly_y) free(poly_y);
        error("Not enough memory");
    }

    for (int i = 0; i < point_count; i++) {
        poly_x[vertex_count] = (float)x_values[i];
        poly_y[vertex_count] = (float)y_values[i];
        if (y_values[i] > ymax) ymax = y_values[i];
        if (y_values[i] < ymin) ymin = y_values[i];
        vertex_count++;
    }

    if (poly_y[vertex_count - 1] != poly_y[0] || poly_x[vertex_count - 1] != poly_x[0]) {
        poly_x[vertex_count] = poly_x[0];
        poly_y[vertex_count] = poly_y[0];
        vertex_count++;
    }

    if (vertex_count > 5) {
        host_fill_polygon_edges(poly_x, poly_y, vertex_count, point_count, ymin, ymax, c, f);
    } else if (vertex_count == 5) {
        DrawTriangle((int)poly_x[0], (int)poly_y[0], (int)poly_x[1], (int)poly_y[1],
                     (int)poly_x[2], (int)poly_y[2], f, f);
        DrawTriangle((int)poly_x[0], (int)poly_y[0], (int)poly_x[2], (int)poly_y[2],
                     (int)poly_x[3], (int)poly_y[3], f, f);
        if (f != c) {
            DrawLine((int)poly_x[0], (int)poly_y[0], (int)poly_x[1], (int)poly_y[1], 1, c);
            DrawLine((int)poly_x[1], (int)poly_y[1], (int)poly_x[2], (int)poly_y[2], 1, c);
            DrawLine((int)poly_x[2], (int)poly_y[2], (int)poly_x[3], (int)poly_y[3], 1, c);
            DrawLine((int)poly_x[3], (int)poly_y[3], (int)poly_x[4], (int)poly_y[4], 1, c);
        }
    } else {
        DrawTriangle((int)poly_x[0], (int)poly_y[0], (int)poly_x[1], (int)poly_y[1],
                     (int)poly_x[2], (int)poly_y[2], c, f);
    }

    free(poly_x);
    free(poly_y);
}

void cmd_polygon(void) {
    long long int *polycount = NULL, *xptr = NULL, *yptr = NULL, *cptr = NULL, *fptr = NULL;
    MMFLOAT *polycountf = NULL, *xfptr = NULL, *yfptr = NULL, *cfptr = NULL, *ffptr = NULL;
    int n = 0, nx = 0, ny = 0, nc = 0, nf = 0;
    int c = gui_fcolour, f = -1;

    getargs(&cmdline, 9, (unsigned char *)",");
    host_getargaddress(argv[0], &polycount, &polycountf, &n);

    if (n == 1) {
        int xcount = getinteger(argv[0]);
        int xtot = xcount;
        int *x_values;
        int *y_values;
        int i;

        if ((xcount < 3 || xcount > 9999) && xcount != 0) error("Invalid number of vertices");
        host_getargaddress(argv[2], &xptr, &xfptr, &nx);
        if (xcount == 0) xcount = xtot = nx;
        if (nx < xtot) error("X Dimensions %", nx);
        host_getargaddress(argv[4], &yptr, &yfptr, &ny);
        if (ny < xtot) error("Y Dimensions %", ny);
        if (argc > 5 && *argv[6]) c = getint(argv[6], 0, WHITE);
        if (argc > 7 && *argv[8]) f = getint(argv[8], 0, WHITE);

        x_values = (int *)malloc((size_t)xcount * sizeof(int));
        y_values = (int *)malloc((size_t)xcount * sizeof(int));
        if (!x_values || !y_values) {
            if (x_values) free(x_values);
            if (y_values) free(y_values);
            error("Not enough memory");
        }
        for (i = 0; i < xcount; i++) {
            x_values[i] = (xfptr == NULL ? (int)xptr[i] : (int)xfptr[i]);
            y_values[i] = (yfptr == NULL ? (int)yptr[i] : (int)yfptr[i]);
        }
        host_draw_polygon_points(x_values, y_values, xcount, c, f, 1);
        free(x_values);
        free(y_values);
        if (Option.Refresh) Display_Refresh();
        return;
    }

    {
        int *cc = (int *)malloc((size_t)n * sizeof(int));
        int *ff = (int *)malloc((size_t)n * sizeof(int));
        int xtot = 0;
        int xmax = 0;
        int xstart = 0;
        int i, actual_n = 0;

        if (!cc || !ff) {
            if (cc) free(cc);
            if (ff) free(ff);
            error("Not enough memory");
        }

        for (i = 0; i < n; i++) {
            int count = (polycountf == NULL ? (int)polycount[i] : (int)polycountf[i]);
            if (count > xmax) xmax = count;
            if (!count) break;
            xtot += count;
            if (count < 3 || count > 9999) error("Invalid number of vertices, polygon %", i);
            actual_n++;
        }
        n = actual_n;
        host_getargaddress(argv[2], &xptr, &xfptr, &nx);
        if (nx < xtot) error("X Dimensions %", nx);
        host_getargaddress(argv[4], &yptr, &yfptr, &ny);
        if (ny < xtot) error("Y Dimensions %", ny);

        if (argc > 5 && *argv[6]) {
            host_getargaddress(argv[6], &cptr, &cfptr, &nc);
            if (nc == 1) {
                c = getint(argv[6], 0, WHITE);
                for (i = 0; i < n; i++) cc[i] = c;
            } else {
                if (nc < n) error("Foreground colour Dimensions");
                for (i = 0; i < n; i++) {
                    cc[i] = (cfptr == NULL ? (int)cptr[i] : (int)cfptr[i]);
                    if (cc[i] < 0 || cc[i] > WHITE) error("% is invalid (valid is % to %)", cc[i], 0, WHITE);
                }
            }
        } else {
            for (i = 0; i < n; i++) cc[i] = gui_fcolour;
        }

        if (argc > 7 && *argv[8]) {
            host_getargaddress(argv[8], &fptr, &ffptr, &nf);
            if (nf == 1) {
                f = getint(argv[8], 0, WHITE);
                for (i = 0; i < n; i++) ff[i] = f;
            } else {
                if (nf < n) error("Background colour Dimensions");
                for (i = 0; i < n; i++) {
                    ff[i] = (ffptr == NULL ? (int)fptr[i] : (int)ffptr[i]);
                    if (ff[i] < 0 || ff[i] > WHITE) error("% is invalid (valid is % to %)", ff[i], 0, WHITE);
                }
            }
        }

        for (i = 0; i < n; i++) {
            int xcount = (polycountf == NULL ? (int)polycount[i] : (int)polycountf[i]);
            int fill_colour = (argc > 7 && *argv[8]) ? ff[i] : -1;
            int *x_values = (int *)malloc((size_t)xcount * sizeof(int));
            int *y_values = (int *)malloc((size_t)xcount * sizeof(int));
            int j;

            if (!x_values || !y_values) {
                if (x_values) free(x_values);
                if (y_values) free(y_values);
                free(cc);
                free(ff);
                error("Not enough memory");
            }
            for (j = 0; j < xcount; j++) {
                x_values[j] = (xfptr == NULL ? (int)xptr[xstart + j] : (int)xfptr[xstart + j]);
                y_values[j] = (yfptr == NULL ? (int)yptr[xstart + j] : (int)yfptr[xstart + j]);
            }
            host_draw_polygon_points(x_values, y_values, xcount, cc[i], fill_colour, 1);
            free(x_values);
            free(y_values);
            xstart += xcount;
        }

        free(cc);
        free(ff);
    }
    if (Option.Refresh) Display_Refresh();
}
void cmd_port(void) {}
void cmd_program(void) {}
void cmd_pull(void) {}
void cmd_pulse(void) {}
void cmd_push(void) {}
void cmd_pwm(void) {
    unsigned char *tp;
    if ((tp = checkstring(cmdline, (unsigned char *)"SYNC"))) {
        MMFLOAT counts[12];
        uint16_t present = 0;
        int i;
        for (i = 0; i < 12; i++) counts[i] = -1.0;
#ifdef rp2350
        getargs(&tp, 23, (unsigned char *)",");
#else
        getargs(&tp, 15, (unsigned char *)",");
#endif
        for (i = 0; i < argc / 2 + 1 && i < 12; i++) {
            if (i * 2 < argc && *argv[i * 2]) {
                counts[i] = getnumber(argv[i * 2]);
                if ((counts[i] < 0.0 || counts[i] > 100.0) && counts[i] != -1.0)
                    error("Syntax");
                present |= (uint16_t)(1u << i);
            }
        }
        vm_sys_pwm_sync(present, counts);
        return;
    }

    getargs(&cmdline, 11, (unsigned char *)",");
    if (argc < 3) error("Syntax");
    {
        int slice = getint(argv[0], 0, 11);
        int phase = 0;
        int defer = 0;
        int has_duty1 = 0, has_duty2 = 0;
        MMFLOAT frequency, duty1 = 0, duty2 = 0;
        if (checkstring(argv[2], (unsigned char *)"OFF")) {
            vm_sys_pwm_off(slice);
            return;
        }
        if (argc < 5) error("Syntax");
        frequency = getnumber(argv[2]);
        if (*argv[4]) {
            duty1 = getnumber(argv[4]);
            has_duty1 = 1;
        }
        if (argc >= 7 && *argv[6]) {
            duty2 = getnumber(argv[6]);
            has_duty2 = 1;
        }
        if (argc >= 9 && *argv[8]) phase = getint(argv[8], 0, 1);
        if (argc == 11 && *argv[10]) defer = getint(argv[10], 0, 1);
        vm_sys_pwm_configure(slice, frequency, has_duty1, duty1, has_duty2, duty2, phase, defer);
    }
}
void cmd_rbox(void) {}
void cmd_refresh(void) {}
void cmd_rmdir(void) { host_cmd_single_path(vm_sys_file_rmdir, "File name"); }
void cmd_rtc(void) {}
/* SAVE "file.bas" — writes the current program (as source text) to the
 * host filesystem under host_sd_root. Uses llist() from Commands.c to
 * detokenise each program line back into BASIC source. Other SAVE forms
 * (SAVE IMAGE, SAVE CONTEXT, etc.) are not implemented on host yet. */
void cmd_save(void) {
    char *fname = (char *)getFstring(cmdline);
    if (!fname || !*fname) error("File name");

    char path[4096];
    host_resolve_sd_path(fname, path, sizeof(path));
    host_append_default_ext(path, sizeof(path), ".bas");

    FILE *fp = fopen(path, "w");
    if (!fp) error("Cannot open file");

    unsigned char buf[STRINGSIZE];
    unsigned char *p = ProgMemory;
    while (!(p[0] == 0 && p[1] == 0) && *p != 0xff) {
        if (*p == T_NEWLINE) {
            p = llist(buf, p);
            /* llist skips the T_NEWLINE itself and writes the decoded line
             * into buf, returning the cursor past the line. Drop auto-
             * generated line numbers so the saved file is reloadable. */
            unsigned char *src = buf;
            while (*src >= '0' && *src <= '9') src++;
            while (*src == ' ') src++;
            fputs((char *)src, fp);
            fputc('\n', fp);
        } else {
            p++;
        }
    }
    fclose(fp);
}
void cmd_seek(void) {
    int fnbr;
    int pos;
    getargs(&cmdline, 5, (unsigned char *)",");
    if (argc != 3) error("Syntax");
    if (*argv[0] == '#') argv[0]++;
    fnbr = getint(argv[0], 1, MAXOPENFILES);
    pos = getinteger(argv[2]);
    if (FileTable[fnbr].com == 0) error("File number is not open");
    if (pos < 1) pos = 1;
    if (f_lseek(FileTable[fnbr].fptr, (FSIZE_t)(pos - 1)) != FR_OK) error("File error");
}
void cmd_Servo(void) {
    getargs(&cmdline, 5, (unsigned char *)",");
    if (argc < 3) error("Syntax");
    {
        int slice = getint(argv[0], 0, 11);
        int has_pos1 = 0, has_pos2 = 0;
        MMFLOAT pos1 = 0, pos2 = 0;
        if (checkstring(argv[2], (unsigned char *)"OFF")) {
            vm_sys_pwm_off(slice);
            return;
        }
        if (*argv[2]) {
            pos1 = getnumber(argv[2]);
            has_pos1 = 1;
        }
        if (argc >= 5 && *argv[4]) {
            pos2 = getnumber(argv[4]);
            has_pos2 = 1;
        }
        vm_sys_servo_configure(slice, has_pos1, pos1, has_pos2, pos2);
    }
}
void cmd_set(void) {}
void cmd_setpin(void) {
    int pin;
    int mode = -1;
    int option = VM_PIN_OPT_NONE;

    getargs(&cmdline, 7, (unsigned char *)",");
    if (argc % 2 == 0 || argc < 3) error("Argument count");
    pin = host_parse_pin_arg(argv[0]);

    if (checkstring(argv[2], (unsigned char *)"OFF") || checkstring(argv[2], (unsigned char *)"0"))
        mode = VM_PIN_MODE_OFF;
    else if (checkstring(argv[2], (unsigned char *)"DIN"))
        mode = VM_PIN_MODE_DIN;
    else if (checkstring(argv[2], (unsigned char *)"DOUT"))
        mode = VM_PIN_MODE_DOUT;
    else if (checkstring(argv[2], (unsigned char *)"ARAW"))
        mode = VM_PIN_MODE_ARAW;
    else if (checkstring(argv[2], (unsigned char *)"PWM"))
        mode = VM_PIN_MODE_PWM_AUTO;
    else if (checkstring(argv[2], (unsigned char *)"PWM0A"))
        mode = VM_PIN_MODE_PWM0A;
    else if (checkstring(argv[2], (unsigned char *)"PWM0B"))
        mode = VM_PIN_MODE_PWM0B;
    else if (checkstring(argv[2], (unsigned char *)"PWM1A"))
        mode = VM_PIN_MODE_PWM1A;
    else if (checkstring(argv[2], (unsigned char *)"PWM1B"))
        mode = VM_PIN_MODE_PWM1B;
    else if (checkstring(argv[2], (unsigned char *)"PWM2A"))
        mode = VM_PIN_MODE_PWM2A;
    else if (checkstring(argv[2], (unsigned char *)"PWM2B"))
        mode = VM_PIN_MODE_PWM2B;
    else if (checkstring(argv[2], (unsigned char *)"PWM3A"))
        mode = VM_PIN_MODE_PWM3A;
    else if (checkstring(argv[2], (unsigned char *)"PWM3B"))
        mode = VM_PIN_MODE_PWM3B;
    else if (checkstring(argv[2], (unsigned char *)"PWM4A"))
        mode = VM_PIN_MODE_PWM4A;
    else if (checkstring(argv[2], (unsigned char *)"PWM4B"))
        mode = VM_PIN_MODE_PWM4B;
    else if (checkstring(argv[2], (unsigned char *)"PWM5A"))
        mode = VM_PIN_MODE_PWM5A;
    else if (checkstring(argv[2], (unsigned char *)"PWM5B"))
        mode = VM_PIN_MODE_PWM5B;
    else if (checkstring(argv[2], (unsigned char *)"PWM6A"))
        mode = VM_PIN_MODE_PWM6A;
    else if (checkstring(argv[2], (unsigned char *)"PWM6B"))
        mode = VM_PIN_MODE_PWM6B;
    else if (checkstring(argv[2], (unsigned char *)"PWM7A"))
        mode = VM_PIN_MODE_PWM7A;
    else if (checkstring(argv[2], (unsigned char *)"PWM7B"))
        mode = VM_PIN_MODE_PWM7B;
#ifdef rp2350
    else if (checkstring(argv[2], (unsigned char *)"PWM8A"))
        mode = VM_PIN_MODE_PWM8A;
    else if (checkstring(argv[2], (unsigned char *)"PWM8B"))
        mode = VM_PIN_MODE_PWM8B;
    else if (checkstring(argv[2], (unsigned char *)"PWM9A"))
        mode = VM_PIN_MODE_PWM9A;
    else if (checkstring(argv[2], (unsigned char *)"PWM9B"))
        mode = VM_PIN_MODE_PWM9B;
    else if (checkstring(argv[2], (unsigned char *)"PWM10A"))
        mode = VM_PIN_MODE_PWM10A;
    else if (checkstring(argv[2], (unsigned char *)"PWM10B"))
        mode = VM_PIN_MODE_PWM10B;
    else if (checkstring(argv[2], (unsigned char *)"PWM11A"))
        mode = VM_PIN_MODE_PWM11A;
    else if (checkstring(argv[2], (unsigned char *)"PWM11B"))
        mode = VM_PIN_MODE_PWM11B;
#endif
    else
        error("Unsupported SETPIN mode");

    if (argc >= 5 && *argv[4]) {
        if (checkstring(argv[4], (unsigned char *)"PULLUP"))
            option = VM_PIN_OPT_PULLUP;
        else if (checkstring(argv[4], (unsigned char *)"PULLDOWN"))
            option = VM_PIN_OPT_PULLDOWN;
        else
            error("Unsupported SETPIN option");
    }
    vm_sys_pin_setpin(pin, mode, option);
}
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
void cmd_triangle(void) {
    int x1, y1, x2, y2, x3, y3, c = gui_fcolour, f = -1, n = 0, i, nc = 0, nf = 0;
    long long int *x1ptr = NULL, *y1ptr = NULL, *x2ptr = NULL, *y2ptr = NULL, *x3ptr = NULL, *y3ptr = NULL;
    long long int *cptr = NULL, *fptr = NULL;
    MMFLOAT *x1fptr = NULL, *y1fptr = NULL, *x2fptr = NULL, *y2fptr = NULL, *x3fptr = NULL, *y3fptr = NULL;
    MMFLOAT *cfptr = NULL, *ffptr = NULL;

    getargs(&cmdline, 15, (unsigned char *)",");
    if (!(argc & 1) || argc < 11) error("Argument count");

    host_getargaddress(argv[0], &x1ptr, &x1fptr, &n);
    if (n != 1) {
        int cn = n;
        host_getargaddress(argv[2], &y1ptr, &y1fptr, &n); if (n < cn) cn = n;
        host_getargaddress(argv[4], &x2ptr, &x2fptr, &n); if (n < cn) cn = n;
        host_getargaddress(argv[6], &y2ptr, &y2fptr, &n); if (n < cn) cn = n;
        host_getargaddress(argv[8], &x3ptr, &x3fptr, &n); if (n < cn) cn = n;
        host_getargaddress(argv[10], &y3ptr, &y3fptr, &n); if (n < cn) cn = n;
        n = cn;
    }

    if (n == 1) {
        x1 = getinteger(argv[0]);
        y1 = getinteger(argv[2]);
        x2 = getinteger(argv[4]);
        y2 = getinteger(argv[6]);
        x3 = getinteger(argv[8]);
        y3 = getinteger(argv[10]);
        if (argc >= 13 && *argv[12]) c = getint(argv[12], BLACK, WHITE);
        if (argc == 15) f = getint(argv[14], -1, WHITE);
        DrawTriangle(x1, y1, x2, y2, x3, y3, c, f);
    } else {
        if (argc >= 13 && *argv[12]) {
            host_getargaddress(argv[12], &cptr, &cfptr, &nc);
            if (nc == 1) c = getint(argv[12], 0, WHITE);
            else if (nc > 1) {
                if (nc < n) n = nc;
                for (i = 0; i < nc; i++) {
                    c = (cfptr == NULL ? (int)cptr[i] : (int)cfptr[i]);
                    if (c < 0 || c > WHITE) error("% is invalid (valid is % to %)", c, 0, WHITE);
                }
            }
        }
        if (argc == 15) {
            host_getargaddress(argv[14], &fptr, &ffptr, &nf);
            if (nf == 1) f = getint(argv[14], -1, WHITE);
            else if (nf > 1) {
                if (nf < n) n = nf;
                for (i = 0; i < nf; i++) {
                    f = (ffptr == NULL ? (int)fptr[i] : (int)ffptr[i]);
                    if (f < -1 || f > WHITE) error("% is invalid (valid is % to %)", f, -1, WHITE);
                }
            }
        }
        for (i = 0; i < n; i++) {
            x1 = (x1fptr == NULL ? (int)x1ptr[i] : (int)x1fptr[i]);
            y1 = (y1fptr == NULL ? (int)y1ptr[i] : (int)y1fptr[i]);
            x2 = (x2fptr == NULL ? (int)x2ptr[i] : (int)x2fptr[i]);
            y2 = (y2fptr == NULL ? (int)y2ptr[i] : (int)y2fptr[i]);
            x3 = (x3fptr == NULL ? (int)x3ptr[i] : (int)x3fptr[i]);
            y3 = (y3fptr == NULL ? (int)y3ptr[i] : (int)y3fptr[i]);
            if (x1 == -1 && y1 == -1 && x2 == -1 && y2 == -1 && x3 == -1 && y3 == -1) return;
            if (nc > 1) c = (cfptr == NULL ? (int)cptr[i] : (int)cfptr[i]);
            if (nf > 1) f = (ffptr == NULL ? (int)fptr[i] : (int)ffptr[i]);
            DrawTriangle(x1, y1, x2, y2, x3, y3, c, f);
        }
    }
    if (Option.Refresh) Display_Refresh();
}
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
void fun_date(void) {
    sret = GetTempMemory(STRINGSIZE);
    /* Tests set MMBASIC_HOST_DATE to pin a deterministic value across
     * interpreter + VM comparison; otherwise fall back to wall clock. */
    const char *mock = getenv("MMBASIC_HOST_DATE");
    if (mock && *mock) {
        strncpy((char *)sret, mock, 15);
        ((char *)sret)[15] = '\0';
    } else {
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        snprintf((char *)sret, 16, "%02d-%02d-%04d",
                 lt.tm_mday, lt.tm_mon + 1, lt.tm_year + 1900);
    }
    CtoM(sret);
    targ = T_STR;
}
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
void fun_info(void) {
    if (checkstring(ep, (unsigned char *)"HRES")) {
        iret = HRes;
        targ = T_INT;
        return;
    }
    if (checkstring(ep, (unsigned char *)"VRES")) {
        iret = VRes;
        targ = T_INT;
        return;
    }
    iret = 0;
    targ = T_INT;
}
void fun_inputstr(void) {}
void fun_keydown(void) {
    int n = getint(ep, 0, 8);
    iret = host_keydown(n);
    targ = T_INT;
}
void fun_LCompare(void) {}
void fun_LGetByte(void) {}
void fun_LGetStr(void) {}
void fun_LInstr(void) {}
void fun_LLen(void) {}
void fun_loc(void) {}
void fun_lof(void) {}
void fun_peek(void) {}
void fun_pin(void) {
    int pin;
    pin = host_parse_pin_arg(ep);
    iret = vm_sys_pin_read(pin);
    targ = T_INT;
}
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
    sret = GetTempMemory(STRINGSIZE);
    const char *mock = getenv("MMBASIC_HOST_TIME");
    if (mock && *mock) {
        strncpy((char *)sret, mock, 15);
        ((char *)sret)[15] = '\0';
    } else {
        time_t now = time(NULL);
        struct tm lt;
        localtime_r(&now, &lt);
        snprintf((char *)sret, 16, "%02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
    }
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
void ClearScreen(int c) { host_framebuffer_clear_target(c); }
void CloseAllFiles(void) {}
void CloseAudio(int all) { (void)all; }
void closeframebuffer(char layer) { host_framebuffer_close(layer); }
void clear320(void) {}
/* DisplayPutC is now the real one from gfx_console_shared.c. It gates on
 * Option.DISPLAY_CONSOLE and calls through the DrawBitmap / DrawRectangle
 * function pointers set up in host_runtime_begin. */
void enable_interrupts_pico(void) {}
void disable_interrupts_pico(void) {}
void initFonts(void) { FontTable[0] = (unsigned char *)font1; }
void initMouse0(int sensitivity) { (void)sensitivity; }
void restorepanel(void) { WriteBuf = NULL; }
void routinechecks(void) { host_runtime_check_timeout(); }
void SoftReset(void) {}
void uSec(int us) { (void)us; }
uint32_t __get_MSP(void) { return 0xFFFFFFFF; }  /* always pass stack overflow check */

/* Console I/O -- hooks into host_output_hook for output capture */
extern void (*host_output_hook)(const char *text, int len);

/* The bespoke --sim console emulator that once lived here has been
 * removed. Console output now flows through the real device path:
 *   MMputchar → putConsole → DisplayPutC → GUIPrintChar → DrawBitmap
 * where DisplayPutC / GUIPrintChar are the shared functions in
 * gfx_console_shared.c and DrawBitmap points at host_draw_bitmap_fn. */

static void host_print(const char *s, int len) {
    /* Bypass the console-routing machinery — this is only used by
     * MMfputs(stdout) and output-capture, which want raw stdout only. */
    if (host_output_hook) host_output_hook(s, len);
    else fwrite(s, 1, len, stdout);
}

static void host_prints(const char *s) {
    if (s) host_print(s, strlen(s));
}

/* =========================================================================
 *  Escape-sequence decoding layered on top of host_terminal.c.
 *  Active only when host_repl_mode is set; the test-harness path below
 *  still consumes from host_key_script.
 * ========================================================================= */

extern int host_repl_mode;

/* Parse what we have after seeing ESC. Returns a decoded keycode
 * (UP/DOWN/F1/… or ESC itself) and consumes the bytes. */
static int host_decode_escape_sequence(void) {
    int c1 = host_read_byte_blocking_ms(30);
    if (c1 < 0) return ESC;

    if (c1 == '[') {
        int c2 = host_read_byte_blocking_ms(30);
        if (c2 < 0) return ESC;  /* malformed; swallow */
        switch (c2) {
            case 'A': return UP;
            case 'B': return DOWN;
            case 'C': return RIGHT;
            case 'D': return LEFT;
            case 'H': return HOME;
            case 'F': return END;
        }
        if (c2 >= '0' && c2 <= '9') {
            /* Numeric parameter. Collect digits until '~' or letter. */
            int n = c2 - '0';
            int c3;
            while ((c3 = host_read_byte_blocking_ms(30)) >= 0) {
                if (c3 >= '0' && c3 <= '9') { n = n * 10 + (c3 - '0'); continue; }
                break;
            }
            if (c3 == '~') {
                switch (n) {
                    case 1:  return HOME;
                    case 2:  return INSERT;
                    case 3:  return DEL;
                    case 4:  return END;
                    case 5:  return PUP;
                    case 6:  return PDOWN;
                    case 15: return F5;
                    case 17: return F6;
                    case 18: return F7;
                    case 19: return F8;
                    case 20: return F9;
                    case 21: return F10;
                    case 23: return F11;
                    case 24: return F12;
                }
            }
        }
        return ESC;  /* unknown CSI — swallow rather than confuse caller */
    }

    if (c1 == 'O') {
        int c2 = host_read_byte_blocking_ms(30);
        switch (c2) {
            case 'P': return F1;
            case 'Q': return F2;
            case 'R': return F3;
            case 'S': return F4;
        }
        return ESC;
    }

    /* ESC followed by a regular char (Alt-<key>) — drop the ESC, keep char. */
    host_push_back_byte(c1);
    return ESC;
}

int MMInkey(void) {
    host_runtime_check_timeout();

    /* Test-harness path: pre-scripted key stream. Returns -2 if no
     * script is queued (fall through), -1 if queued-but-waiting, or
     * the next consumed char. */
    {
        int scripted = host_runtime_keys_consume();
        if (scripted != -2) return scripted;
    }

#ifdef MMBASIC_SIM
    /* --sim path: keys injected by the WebSocket server from the browser.
     * When the server is active we always prefer it; if the queue is
     * empty and stdin isn't a live TTY, yield 1ms and return -1 so the
     * caller's polling loop (Editor, INKEY$) doesn't pin a core. The
     * sleep also advances CursorTimer so the blinker runs at a constant
     * rate regardless of how hard the caller is polling. */
    extern int host_sim_active;
    if (host_sim_active) {
        int c = host_sim_pop_key();
        if (c >= 0) return c;
        if (!host_raw_mode_is_active()) {
            host_sleep_us(1000);
            return -1;
        }
    }
#endif

    /* REPL path: live terminal. */
    if (host_raw_mode_is_active()) {
        int c = host_read_byte_nonblock();
        if (c < 0) return -1;
        /* Ctrl-D at the prompt (outside EDIT) exits cleanly, like a shell.
         * Inside EDIT the device treats Ctrl-D as CTRLKEY('D') = RIGHT
         * cursor, so we only intercept when editactive == 0. */
        if (c == 4 && !editactive) {
            MMPrintString("\r\n");
            exit(0);
        }
        if (c == 0x1b) return host_decode_escape_sequence();
        if (c == 0x7f) return BKSP;       /* macOS/iTerm Backspace → BKSP */
        if (c == '\n') return ENTER;      /* normalise LF → CR for MMBasic */
        return c;
    }

    /* REPL piped into stdin (not a TTY) — read cooked, line-buffered.
     * Used by CI and scripted tests that feed commands through a pipe.
     * No escape-sequence decoding here; we just stream chars as-is,
     * mapping LF to CR so EditInputLine's ENTER branch fires. */
    if (host_repl_mode) {
        int c = fgetc(stdin);
        if (c == EOF) exit(0);
        if (c == '\n') return ENTER;
        return c;
    }

    return -1;
}

/* Matches PicoMite.c:786-794: blink the cursor while waiting for a key,
 * hide it once we have one. ShowCursor reads CursorTimer (ticked by
 * host_sync_msec_timer_value); host_sleep_us() calls host_sync_msec_timer
 * so CursorTimer advances on every spin. */
int MMgetchar(void) {
    int ch;
    do {
        ShowCursor(1);
        ch = MMInkey();
        if (ch == -1) host_sleep_us(1000);
    } while (ch == -1);
    ShowCursor(0);
    return ch;
}
/*
 * Matches PicoMite.c:573-575 + 615-622 verbatim — both the dispatch and
 * MMCharPos tracking. Keeping this shape means `SSPrintString`
 * (serial-only, emits VT100 escapes from the Editor) never reaches
 * DisplayPutC, and that the device's console-routing rules apply
 * identically on host.
 */
void putConsole(int c, int flush) {
    if (OptionConsole & 2) DisplayPutC((char)c);
    if (OptionConsole & 1) SerialConsolePutC((char)c, flush);
}

char MMputchar(char c, int flush) {
    if (host_output_hook) {
        host_output_hook(&c, 1);
    } else {
        putConsole(c, flush);
    }
    if (isprint((unsigned char)c)) MMCharPos++;
    if (c == '\r') MMCharPos = 1;
    return c;
}

void MMPrintString(char *s) {
    while (*s) MMputchar(*s++, 0);
    fflush(stdout);
}

void SSPrintString(char *s) {
    /* Serial-only. The Editor emits VT100 escapes through this path; they
     * must never reach DisplayPutC, or the screen console would render
     * them as literal glyphs. */
    while (*s) SerialConsolePutC(*s++, 0);
    fflush(stdout);
}
/* PRet/PInt/PFlt/SRet/SInt/SIntComma/PIntComma/PIntH/PIntB/PIntHC/PIntBC/
 * PFltComma are now in MMBasic_Print.c (shared with the device build),
 * not stubbed here. */
/*
 * MMfputs / MMfputc: match FileIO.c:3254 / 3386 verbatim. When filenbr==0,
 * route through MMputchar so output reaches both the terminal (stdout)
 * and the framebuffer console (via putConsole → DisplayPutC), instead of
 * going to stdout alone. PRINT, INPUT echo, CAT, and friends all land
 * here.
 */
void MMfputs(unsigned char *p, int filenbr) {
    if (!p) return;
    int i = *p++;
    if (filenbr == 0) {
        while (i--) MMputchar(*p++, 1);
    } else {
        FilePutStr(i, (char *)p, filenbr);
    }
}
int MMfeof(int fnbr) {
    if (fnbr == 0) return 1;
    if (fnbr < 1 || fnbr > MAXOPENFILES) error("File number");
    if (FileTable[fnbr].com == 0) error("File number is not open");
    return FileEOF(fnbr);
}
unsigned char MMfputc(unsigned char c, int fnbr) {
    if (fnbr == 0) return (unsigned char)MMputchar((char)c, 1);
    return (unsigned char)FilePutChar((char)c, fnbr);
}
int MMfgetc(int filenbr) {
    if (filenbr == 0) return -1;
    if (filenbr < 1 || filenbr > MAXOPENFILES) error("File number");
    if (FileTable[filenbr].com == 0) error("File number is not open");
    return (unsigned char)FileGetChar(filenbr);
}
void MMfopen(unsigned char *fname, unsigned char *mode, int fnbr) { (void)fname; (void)mode; (void)fnbr; }
void MMfclose(int fnbr) { FileClose(fnbr); }
void MMgetline(int filenbr, char *p) {
    int c;
    int nbrchars = 0;

    while (1) {
        if (filenbr != 0 && FileEOF(filenbr)) break;
        c = MMfgetc(filenbr);
        if (c <= 0) {
            if (filenbr == 0) break;
            continue;
        }
        if (c == '\n') break;
        if (c == '\r') continue;
        if (c == '\t') {
            do {
                if (++nbrchars > MAXSTRLEN) error("Line is too long");
                *p++ = ' ';
            } while (nbrchars % 4);
            continue;
        }
        if (++nbrchars > MAXSTRLEN) error("Line is too long");
        *p++ = (char)c;
    }
    *p = 0;
}
void printoptions(void) {}
/* putConsole defined above — matches the device dispatch. */
int getConsole(void) { return -1; }
void myprintf(char *s) { host_prints(s); }
char SerialConsolePutC(char c, int flush) {
    /* Host's "serial port" is stdout. The output-capture hook has already
     * been taken care of at the MMputchar level for console text.
     *
     * In raw mode OPOST is disabled, so '\n' on its own no longer returns
     * the cursor to column 0 — without this translation, every prompt and
     * error message stair-steps down the terminal. */
    if (c == '\n' && host_raw_mode_is_active()) fputc('\r', stdout);
    fputc(c, stdout);
    if (flush) fflush(stdout);
    return c;
}
int kbhitConsole(void) { return 0; }

/* Host legacy file shim: interpreter core stays untouched; storage is the
 * shared in-memory FAT disk used by VM host file syscalls. */
int BasicFileOpen(char *fname, int fnbr, int mode) {
    FRESULT res;
    char path[FF_MAX_LFN] = {0};

    if (fnbr < 1 || fnbr > MAXOPENFILES) error("File number");
    if (FileTable[fnbr].com != 0) error("File number already open");
    res = vm_host_fat_mount();
    if (res != FR_OK) error("Host FAT init failed");
    vm_sys_file_host_resolve_path(fname, path, sizeof(path));
    FileTable[fnbr].fptr = malloc(sizeof(FIL));
    if (!FileTable[fnbr].fptr) error("Not enough memory");
    res = f_open(FileTable[fnbr].fptr, path, (BYTE)mode);
    if (res != FR_OK) {
        free(FileTable[fnbr].fptr);
        FileTable[fnbr].fptr = NULL;
        error("File error");
    }
    filesource[fnbr] = FATFSFILE;
    return 1;
}
void FileClose(int fnbr) {
    if (fnbr < 1 || fnbr > MAXOPENFILES) error("File number");
    if (FileTable[fnbr].com == 0) error("File number is not open");
    FRESULT res = f_close(FileTable[fnbr].fptr);
    free(FileTable[fnbr].fptr);
    FileTable[fnbr].fptr = NULL;
    filesource[fnbr] = NONEFILE;
    if (res != FR_OK) error("File error");
}
int FileEOF(int fnbr) {
    if (fnbr < 1 || fnbr > MAXOPENFILES) error("File number");
    if (FileTable[fnbr].com == 0) error("File number is not open");
    return f_eof(FileTable[fnbr].fptr);
}
char FileGetChar(int fnbr) {
    char ch = 0;
    UINT read = 0;
    if (fnbr < 1 || fnbr > MAXOPENFILES) error("File number");
    if (FileTable[fnbr].com == 0) error("File number is not open");
    if (f_read(FileTable[fnbr].fptr, &ch, 1, &read) != FR_OK) error("File error");
    return read == 1 ? ch : 0;
}
/* FileLoadProgram is called by do_run() when the user types RUN "file.bas".
 * It must parse the filename out of the fname buffer (quoted, optionally
 * followed by a comma and cmd args), load the source, tokenise into
 * ProgMemory, and return true on success. Shares the same resolve+read+
 * tokenise path as cmd_load. */
int FileLoadProgram(unsigned char *fname, bool chain) {
    (void)chain;
    if (!fname || !*fname) return 0;

    /* do_run hands us a buffer formatted as "\"filename\",args" (see
     * Commands.c:725). Extract the filename. */
    char name[FF_MAX_LFN];
    const unsigned char *p = fname;
    if (*p == '"') {
        p++;
        size_t n = 0;
        while (*p && *p != '"' && n + 1 < sizeof(name)) name[n++] = (char)*p++;
        name[n] = '\0';
    } else {
        size_t n = 0;
        while (*p && *p != ',' && n + 1 < sizeof(name)) name[n++] = (char)*p++;
        name[n] = '\0';
    }
    if (!*name) return 0;

    char path[FF_MAX_LFN];
    host_resolve_sd_path(name, path, sizeof(path));
    host_append_default_ext(path, sizeof(path), ".bas");

    char *source = read_basic_source_file(path);
    if (!source) error("Cannot find file");

    ClearRuntime(true);
    int rc = load_basic_source(source);
    free(source);
    if (rc != 0) error("Cannot parse file");
    PrepareProgram(false);
    /* Same hygiene as cmd_load — don't leave the last file line in inpbuf
     * for the next EditInputLine to pick up. */
    memset(inpbuf, 0, STRINGSIZE);
    return 1;
}
int FileLoadSourceProgram(unsigned char *fname, char **source_out) { (void)fname; (void)source_out; return 0; }
int FileLoadSourceProgramVM(unsigned char *fname, char **source_out) { (void)fname; (void)source_out; return 0; }
int FileLoadCMM2Program(char *fname, bool message) { (void)fname; (void)message; return 0; }
void FilePutStr(int count, char *s, int fnbr) {
    UINT wrote = 0;
    if (fnbr < 1 || fnbr > MAXOPENFILES) error("File number");
    if (FileTable[fnbr].com == 0) error("File number is not open");
    if (count <= 0) return;
    if (f_write(FileTable[fnbr].fptr, s, (UINT)count, &wrote) != FR_OK ||
        wrote != (UINT)count) {
        error("File error");
    }
}
char FilePutChar(char c, int fnbr) {
    FilePutStr(1, &c, fnbr);
    return c;
}
int FindFreeFileNbr(void) { return 1; }
int ExistsFile(char *fname) { (void)fname; return 0; }
/* Simulated flash operations. On the device these hit XIP flash; on host
 * they write through to flash_prog_buf (allocated in host_main.c, 128 KB).
 * Layout: first 64 KB is program area, second 64 KB is the CFunction area
 * (erased = 0xFF). */
extern uint8_t flash_prog_buf[];
#define HOST_FLASH_SIZE        (256 * 1024)
#define HOST_FLASH_PROG_SIZE   (HOST_FLASH_SIZE / 2)

void flash_range_erase(uint32_t off, uint32_t count) {
    if (off >= HOST_FLASH_SIZE) return;
    if (off + count > HOST_FLASH_SIZE) count = HOST_FLASH_SIZE - off;
    /* Device erase fills with 0xFF. The program region additionally gets
     * a leading zero terminator written by cmd_new right after the erase,
     * but host's ProgMemory scan accepts either 0 or 0xFF as end-of-program. */
    memset(flash_prog_buf + off, 0xFF, count);
}

void flash_range_program(uint32_t off, const uint8_t *data, uint32_t count) {
    if (off >= HOST_FLASH_SIZE) return;
    if (off + count > HOST_FLASH_SIZE) count = HOST_FLASH_SIZE - off;
    memcpy(flash_prog_buf + off, data, count);
}

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
/* Called from Editor.c's SaveToProgMemory() when the user hits F1/F2 to
 * save their edits. `pm` points to NUL-terminated source text (EdBuff),
 * not tokens, so reuse the same host_main tokeniser path that LOAD uses.
 *
 * Do NOT call ClearRuntime here: EdBuff is allocated via GetTempMemory
 * inside edit(), and ClearRuntime frees temp memory — freeing the very
 * buffer we're about to tokenise. SaveToProgMemory's caller runs
 * ClearProgram(true) right after us for any runtime reset needed. */
void SaveProgramToFlash(unsigned char *pm, int msg) {
    (void)msg;
    if (!pm) return;
    load_basic_source((const char *)pm);
    PrepareProgram(false);
}
void CheckSDCard(void) {}
void CrunchData(unsigned char **p, int c) { (void)p; (void)c; }
void ClearSavedVars(void) {}
int ForceFileClose(int fnbr) {
    if (fnbr >= 1 && fnbr <= MAXOPENFILES && FileTable[fnbr].com != 0) {
        f_close(FileTable[fnbr].fptr);
        free(FileTable[fnbr].fptr);
        FileTable[fnbr].fptr = NULL;
        filesource[fnbr] = NONEFILE;
    }
    return FATFSFILE;
}
void ErrorCheck(int fnbr) { (void)fnbr; }
void positionfile(int fnbr, int idx) { (void)fnbr; (void)idx; }
int drivecheck(char *p, int *waste) { (void)p; (void)waste; return 0; }
void getfullfilename(char *fname, char *q) { (void)fname; (void)q; }
char *GetCWD(void) { return (char *)""; }
int InitSDCard(void) { return vm_host_fat_mount() == FR_OK; }

/* LFS stubs */
int lfs_file_close(lfs_t *l, lfs_file_t *file) { (void)l; (void)file; return 0; }
int lfs_file_open(lfs_t *l, lfs_file_t *file, const char *path, int flags) { (void)l; (void)file; (void)path; (void)flags; return -1; }
lfs_ssize_t lfs_file_read(lfs_t *l, lfs_file_t *file, void *buf, lfs_size_t size) { (void)l; (void)file; (void)buf; (void)size; return 0; }
lfs_soff_t lfs_file_seek(lfs_t *l, lfs_file_t *file, lfs_soff_t off, int whence) { (void)l; (void)file; (void)off; (void)whence; return 0; }
lfs_ssize_t lfs_file_write(lfs_t *l, lfs_file_t *file, const void *buf, lfs_size_t size) { (void)l; (void)file; (void)buf; (void)size; return 0; }
lfs_ssize_t lfs_fs_size(lfs_t *l) { (void)l; return 0; }
int lfs_remove(lfs_t *l, const char *path) { (void)l; (void)path; return 0; }
int lfs_stat(lfs_t *l, const char *path, struct lfs_info *info) { (void)l; (void)path; (void)info; return -1; }

/* Drawing stubs */
void SetFont(int f) {
    int scale = f & 0x0F;
    if (scale == 0) scale = 1;
    gui_font = f;
    unsigned char *font = FontTable[f >> 4];
    gui_font_width = (short)((font ? font[0] : host_font_metrics[0]) * scale);
    gui_font_height = (short)((font ? font[1] : host_font_metrics[1]) * scale);
    FontTable[0] = (unsigned char *)font1;
}
void UnloadFont(int f) { (void)f; }
void ResetDisplay(void) {}
int GetFontWidth(int fnt) {
    int scale = fnt & 0x0F;
    if (scale == 0) scale = 1;
    unsigned char *font = FontTable[fnt >> 4];
    return (font ? font[0] : host_font_metrics[0]) * scale;
}
int GetFontHeight(int fnt) {
    int scale = fnt & 0x0F;
    if (scale == 0) scale = 1;
    unsigned char *font = FontTable[fnt >> 4];
    return (font ? font[1] : host_font_metrics[1]) * scale;
}
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
            if (fill >= 0 && dist2 <= fill2) host_fb_put_pixel(px, py, fill);
            if (w > 0 && dist2 <= outer2 && dist2 >= inner2) host_fb_put_pixel(px, py, c);
        }
    }
}
void DrawTriangle(int x0, int y0, int x1, int y1, int x2, int y2, int c, int fill) {
    host_draw_triangle_pixels(x0, y0, x1, y1, x2, y2, c, fill);
}
void GUIPrintString(int x, int y, int fnt, int jh, int jv, int jo, int fc, int bc, char *str) {
    host_draw_text(x, y, fnt, jh, jv, jo, fc, bc, str);
}
/* ShowCursor is now the real one from gfx_console_shared.c. It reads
 * CursorTimer (ticked below in host_sync_msec_timer_value) and draws the
 * blinking underline via DrawLine. */
int getColour(char *c, int minus) { (void)c; (void)minus; return 0; }
void setmode(int mode, bool clear) { (void)mode; (void)clear; }
int rgb(int r, int g, int b) { return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF); }
void DrawPixel16(int x, int y, int c) { host_fb_put_pixel(x, y, c); }
void DrawRectangle16(int x1, int y1, int x2, int y2, int c) { (void)x1; (void)y1; (void)x2; (void)y2; (void)c; }
void DrawBitmap16(int x1, int y1, int width, int height, int scale, int fc, int bc, unsigned char *bitmap) { (void)x1; (void)y1; (void)width; (void)height; (void)scale; (void)fc; (void)bc; (void)bitmap; }
void ScrollLCD16(int lines) { (void)lines; }
void DrawBuffer16(int x1, int y1, int x2, int y2, unsigned char *p) { (void)x1; (void)y1; (void)x2; (void)y2; (void)p; }
void DrawBuffer16Fast(int x1, int y1, int x2, int y2, int blank, unsigned char *p) { (void)x1; (void)y1; (void)x2; (void)y2; (void)blank; (void)p; }
void ReadBuffer16(int x1, int y1, int x2, int y2, unsigned char *c) { (void)x1; (void)y1; (void)x2; (void)y2; (void)c; }
void ReadBuffer16Fast(int x1, int y1, int x2, int y2, unsigned char *c) { (void)x1; (void)y1; (void)x2; (void)y2; (void)c; }
void DrawPixelNormal(int x, int y, int c) { host_fb_put_pixel(x, y, c); }
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
void ExtCfg(int pin, int cfg, int option) {
    if (cfg == EXT_NOT_CONFIG) {
        vm_sys_pin_setpin(pin, VM_PIN_MODE_OFF, VM_PIN_OPT_NONE);
    } else if (cfg == EXT_DIG_IN) {
        int vm_option = VM_PIN_OPT_NONE;
        if (option == CNPUSET) vm_option = VM_PIN_OPT_PULLUP;
        else if (option == CNPDSET) vm_option = VM_PIN_OPT_PULLDOWN;
        vm_sys_pin_setpin(pin, VM_PIN_MODE_DIN, vm_option);
    } else if (cfg == EXT_DIG_OUT) {
        vm_sys_pin_setpin(pin, VM_PIN_MODE_DOUT, VM_PIN_OPT_NONE);
    } else if (cfg == EXT_ADCRAW) {
        vm_sys_pin_setpin(pin, VM_PIN_MODE_ARAW, VM_PIN_OPT_NONE);
    }
}
void ExtSet(int pin, int val) { vm_sys_pin_write(pin, val); }
int64_t ExtInp(int pin) { return vm_sys_pin_read(pin); }
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

/* host_repl_mode is still used by other host stubs to branch on "are we
 * running the interactive REPL?". The bespoke EditInputLine previously
 * here was replaced by the shared device implementation now in
 * MMBasic_Prompt.c. */
int host_repl_mode = 0;

/* lfs_file_size is only reached from the FLASHFILE branch in Editor.c, which
 * is unreachable on host (filesource[] is always FATFSFILE). Stubbed so the
 * link succeeds. */
lfs_soff_t lfs_file_size(lfs_t *lfs, lfs_file_t *fp) { (void)lfs; (void)fp; return 0; }

/* setterminal writes VT100 escape codes to resize the host terminal. On a
 * real desktop terminal we just leave the user's window size alone. */
void setterminal(int height, int width) { (void)height; (void)width; }

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
