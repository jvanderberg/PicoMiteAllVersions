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
/* font1.h defines `font1[]` inline — pulled in exclusively by Draw.c
 * now that FontTable initialisation lives there. */
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
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
int FSerror = 0;
int GPSchannel = 0;
int gui_bcolour = 0;
int gui_fcolour = 0xFFFFFF;
int last_bcolour = 0;
int last_fcolour = 0xFFFFFF;
short gui_font = 0;
short gui_font_height = 8;
short gui_font_width = 6;
uint8_t I2C0locked = 0;
uint8_t I2C1locked = 0;
unsigned char IgnorePIN = 0;
unsigned char *InterruptReturn = NULL;
int InterruptUsed = 0;
int last_adc = 0;
lfs_t lfs;
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
/* WAVcomplete / WAVInterrupt / CurrentlyPlaying defined in Audio.c host body. */
volatile unsigned int WDTimer = 0;
/* Display_Refresh is a function - see function stubs below */

/* struct option_s Option */
struct option_s Option = {0};

/* PinDef array */
const struct s_PinDef PinDef[NBRPINS + 1] = {{0}};

/* CFunctionFlash / CFunctionLibrary are defined by FileIO.c now. We seed
 * CFunctionFlash from host_runtime_begin below so scan loops terminate
 * immediately (host_cfunction_flash_buf is pre-filled with 0xFF to match
 * erased flash). */

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
/* OptionFileErrorAbort / filesource / FatFSFileSystem{,Save} / FlashLoad
 * are now defined by FileIO.c (the host build links the shared file). */
volatile unsigned int GPSTimer = 0;
uint16_t AUDIO_L_PIN = 0, AUDIO_R_PIN = 0, AUDIO_SLICE = 0;
uint16_t AUDIO_WRAP = 0;
int ticks_per_second = 1000;
lfs_dir_t lfs_dir;
struct lfs_info lfs_info;
/* lfs_FileFnbr is now defined by FileIO.c. */
short DisplayHRes = 0, DisplayVRes = 0;
int ScreenSize = 0;
unsigned char *DisplayBuf = NULL;
unsigned char *SecondLayer = NULL;
unsigned char *SecondFrame = NULL;
char LCDAttrib = 0;
s_camera camera[MAXCAM + 1] = {{0}};
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

/* LFS config is defined by FileIO.c. */

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

void host_options_snapshot(void);

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
    /* FontTable[] is initialised in Draw.c now (static initialiser).
     *
     * Tell Draw.c "a display IS configured" so the 16 `DISPLAY_TYPE == 0
     * → error "Display not configured"` checks in cmd_box / cmd_pixel /
     * cmd_cls / … pass. DISP_USER (28) is the generic user-defined
     * display type that relies entirely on the DrawPixel / DrawRectangle
     * function pointers below — which is exactly the host model. It
     * falls outside every specific-panel range in Draw.c (I2C_PANEL+1 …
     * BufferedPanel, SSDPANEL … VIRTUAL, NEXTGEN+), so none of the
     * hardware-specific code paths fire.
     *
     * HRes/VRes are now defined in Draw.c (initialised to 0 there for
     * device boot); re-sync them to the host framebuffer dimensions so
     * MMBasic's geometry math + the VM's HRes/VRes checks see the same
     * values as the framebuffer plane. */
    Option.DISPLAY_TYPE = DISP_USER;
    HRes = (short)host_fb_width;
    VRes = (short)host_fb_height;
    DrawPixel = host_draw_pixel_ptr;
    DrawRectangle = host_fb_draw_rectangle;
    DrawBitmap = host_fb_draw_bitmap;
    ScrollLCD = host_fb_scroll_lcd;
    ReadBuffer = host_fb_read_buffer;
    /* CFunctionFlash is now defined in FileIO.c (initialised to NULL). Seed
     * it here with a pre-erased 0xFF buffer so the CFunction scan loops
     * terminate immediately — matches the device state after cold boot. */
    CFunctionFlash = host_cfunction_flash_buf;
    /* FatFSFileSystem default: 1 = use B: (vm_host_fat RAM disk or POSIX
     * via host_fs_posix). Keeps BasicFileOpen out of the LFS branch. */
    FatFSFileSystem = FatFSFileSystemSave = 1;
    /* Option.Height is the terminal row count that cmd_files / LIST use
     * for "PRESS ANY KEY" pagination. Zero (LoadOptions' default on
     * host) makes `ListCnt >= Option.Height - overlap` fire immediately,
     * hanging the test harness on stdin it never gets. 1000 effectively
     * disables pagination under batch runs; real REPL users can
     * OPTION HEIGHT to set their own. */
    if (Option.Height == 0) Option.Height = 1000;
    /* Snapshot Option back into flash_option_buf so the reset path in
     * error() (MMBasic.c:2835 calls LoadOptions) restores the *current*
     * host configuration, not a zero-filled default. Without this,
     * every error wipes Option.Width / Height / DISPLAY_CONSOLE / etc.
     * — the symptom is: browser console stops echoing, cmd_files wraps
     * at column 0, ListCnt pagination fires immediately. */
    host_options_snapshot();
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

/* cmd_3D, cmd_arc, cmd_blit, cmd_blitmemory, cmd_box now come from
 * Draw.c (gated with MMBASIC_HOST for the display-driver pieces). */
void cmd_adc(void) {}
/* cmd_autosave is provided by FileIO.c. */
void cmd_backlight(void) {}
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
/* cmd_chdir, cmd_close, cmd_copy, cmd_disk are provided by FileIO.c. */
void cmd_Classic(void) {}
void cmd_configure(void) {}
void cmd_cpu(void) {}
void cmd_csubinterrupt(void) {}
void cmd_date(void) {}
void cmd_device(void) {}
void cmd_DHT22(void) {}
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
/* FatFS directory-walker wrappers — the surface `cmd_files`, `cmd_copy`,
 * `cmd_kill`, `fun_dir` need. In REPL / --sim mode (host_sd_root set) we
 * walk the user's real directory via host_fs_walk_* (POSIX) and populate
 * the FatFS FILINFO the caller expects. That way cmd_files' sort,
 * pagination, and formatting — all of which live in FileIO.c — run
 * unchanged on host-hosted file trees.
 *
 * Without host_sd_root the test harness wants the vm_host_fat RAM disk,
 * so we delegate straight to FatFS.
 *
 * FatFS date/time packing (ff.h: fdate = yr-1980<<9 | mon<<5 | day,
 * ftime = hr<<11 | min<<5 | sec/2). Zero-filled when mtime decode fails;
 * cmd_files prints "00:00 00-00-1980" but doesn't crash. */
static host_fs_walker_t *host_find_walker = NULL;

FRESULT host_f_findfirst(DIR *dp, FILINFO *fi, const TCHAR *path, const TCHAR *pattern);
FRESULT host_f_findnext(DIR *dp, FILINFO *fi);
FRESULT host_f_closedir(DIR *dp);
static void host_strip_fatfs_drive(const char *in, char *out, int out_cap);
void host_join_sd_root(const char *relpath, char *out, int out_cap);

static void host_fill_finfo_from_posix(FILINFO *fi, const char *name,
                                       int is_dir, unsigned long long size,
                                       long long mtime_epoch) {
    memset(fi, 0, sizeof(*fi));
    snprintf(fi->fname, sizeof(fi->fname), "%s", name);
    fi->fattrib = is_dir ? AM_DIR : 0;
    fi->fsize = (FSIZE_t)size;
    time_t t = (time_t)mtime_epoch;
    struct tm lt;
    if (localtime_r(&t, &lt) != NULL) {
        int yr = lt.tm_year + 1900;
        if (yr < 1980) yr = 1980;
        fi->fdate = (WORD)(((yr - 1980) << 9) | ((lt.tm_mon + 1) << 5) | lt.tm_mday);
        fi->ftime = (WORD)((lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec / 2));
    }
}

FRESULT host_f_findfirst(DIR *dp, FILINFO *fi, const TCHAR *path,
                         const TCHAR *pattern) {
    if (!host_sd_root) return f_findfirst(dp, fi, path, pattern);
    /* FileIO.c hands us a FatFS-style path like "/build" or just "/"
     * (drive prefix already stripped by fullpath(). Join it onto
     * host_sd_root so FILES / COPY / KILL / DIR$ see the subdirectory
     * the user has CHDIR'd into. */
    char target[FF_MAX_LFN];
    host_join_sd_root(path, target, sizeof(target));
    if (host_find_walker) host_fs_walk_close(host_find_walker);
    host_find_walker = host_fs_walk_open(target, pattern);
    if (!host_find_walker) { memset(fi, 0, sizeof(*fi)); return FR_NO_PATH; }
    return host_f_findnext(dp, fi);
}

FRESULT host_f_findnext(DIR *dp, FILINFO *fi) {
    if (!host_sd_root) return f_findnext(dp, fi);
    (void)dp;
    if (!host_find_walker) { memset(fi, 0, sizeof(*fi)); return FR_NO_FILE; }
    char name[FF_MAX_LFN + 1];
    int is_dir = 0;
    unsigned long long size = 0;
    long long mtime = 0;
    if (!host_fs_walk_next(host_find_walker, name, (int)sizeof(name),
                           &is_dir, &size, &mtime)) {
        memset(fi, 0, sizeof(*fi));
        fi->fname[0] = 0;
        return FR_OK;   /* FatFS signals end-of-dir by empty fname, FR_OK */
    }
    host_fill_finfo_from_posix(fi, name, is_dir, size, mtime);
    return FR_OK;
}

FRESULT host_f_closedir(DIR *dp) {
    if (!host_sd_root) return f_closedir(dp);
    (void)dp;
    if (host_find_walker) {
        host_fs_walk_close(host_find_walker);
        host_find_walker = NULL;
    }
    return FR_OK;
}

/* Whole-path wrappers. cmd_kill, cmd_name, cmd_mkdir, cmd_chdir, cmd_copy,
 * fun_cwd all call these. Without host_sd_root we go straight to FatFS
 * (vm_host_fat RAM disk for the test harness). With host_sd_root we
 * resolve the path against the user's root directory and hit POSIX.
 *
 * The path argument FileIO.c hands us has a FatFS drive prefix (e.g.
 * "0:/foo.bas" from getfullfilename). We strip that so POSIX paths
 * work cleanly under host_sd_root. */
static void host_strip_fatfs_drive(const char *in, char *out, int out_cap) {
    if (out_cap <= 0) return;
    /* Skip "N:" drive prefix if present. */
    if (in[0] && in[1] == ':') in += 2;
    /* Skip leading "/" so host_resolve_sd_path will join under host_sd_root. */
    while (*in == '/') in++;
    snprintf(out, out_cap, "%s", in);
}

void host_join_sd_root(const char *relpath, char *out, int out_cap) {
    char stripped[FF_MAX_LFN];
    host_strip_fatfs_drive(relpath, stripped, sizeof(stripped));
    host_resolve_sd_path(stripped, out, (size_t)out_cap);
}

FRESULT host_f_unlink(const TCHAR *path) {
    if (!host_sd_root) return f_unlink(path);
    char p[FF_MAX_LFN];
    host_join_sd_root(path, p, sizeof(p));
    return host_fs_unlink(p) == 0 ? FR_OK : FR_NO_FILE;
}

FRESULT host_f_rename(const TCHAR *from, const TCHAR *to) {
    if (!host_sd_root) return f_rename(from, to);
    char a[FF_MAX_LFN], b[FF_MAX_LFN];
    host_join_sd_root(from, a, sizeof(a));
    host_join_sd_root(to, b, sizeof(b));
    return host_fs_rename(a, b) == 0 ? FR_OK : FR_NO_FILE;
}

FRESULT host_f_mkdir(const TCHAR *path) {
    if (!host_sd_root) return f_mkdir(path);
    char p[FF_MAX_LFN];
    host_join_sd_root(path, p, sizeof(p));
    return host_fs_mkdir(p) == 0 ? FR_OK : FR_EXIST;
}

FRESULT host_f_chdir(const TCHAR *path) {
    if (!host_sd_root) return f_chdir(path);
    char p[FF_MAX_LFN];
    host_join_sd_root(path, p, sizeof(p));
    return host_fs_chdir(p) == 0 ? FR_OK : FR_NO_PATH;
}

FRESULT host_f_getcwd(TCHAR *buff, UINT len) {
    if (!host_sd_root) return f_getcwd(buff, len);
    char tmp[FF_MAX_LFN];
    if (host_fs_getcwd(tmp, (int)sizeof(tmp)) != 0) return FR_INT_ERR;
    /* Prepend drive prefix so Editor.c / PRINT CWD$ format works. */
    snprintf(buff, len, "0:%s", tmp);
    return FR_OK;
}
/* cmd_flash / cmd_flush are provided by FileIO.c. */
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
/* cmd_kill is provided by FileIO.c. */
void cmd_label(void) {}
void cmd_lcd(void) {}
void cmd_library(void) {}
/* Defined in host_main.c — used by FileLoadProgram when SaveProgramToFlash
 * feeds buffered source through the host tokeniser path. */
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
/* cmd_load is provided by FileIO.c. It opens the file through BasicFileOpen
 * (which routes through host_fs_posix_try_open → fopen when host_sd_root is
 * set), reads chars via FileGetChar, and tokenises via SaveProgramToFlash
 * (stubbed on host to call load_basic_source + PrepareProgram). */
void cmd_longString(void) {}
/* cmd_mkdir / cmd_name / cmd_open are provided by FileIO.c. */
void cmd_mouse(void) {}
void cmd_mov(void) {}
void cmd_nop(void) {}
void cmd_Nunchuck(void) {}
void cmd_onewire(void) {}
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
/* cmd_rmdir, cmd_save, cmd_seek are provided by FileIO.c. cmd_save now
 * writes through BasicFileOpen → host_fs_posix; the source-text path
 * routes through FilePutStr which shunts to fputs on host. */
void cmd_rtc(void) {}
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
void cmd_sync(void) {}

void cmd_time(void) {}
void cmd_timer(void) {
    uint64_t mytime = host_time_us_64();
    while (*cmdline && tokenfunction(*cmdline) != op_equal) cmdline++;
    if (!*cmdline) error("Syntax");
    timeroffset = mytime - (uint64_t)getint(++cmdline, 0, (int)(mytime / 1000ULL)) * 1000ULL;
}
void cmd_update(void) {}
/* cmd_var is provided by FileIO.c. */
void cmd_wait(void) {}
void cmd_watchdog(void) {}
void cmd_wrap(void) {}
void cmd_wraptarget(void) {}
void cmd_WS2812(void) {}
void cmd_xmodem(void) {}

/* =========================================================================
 *  Stub Functions -- fun_* (hardware functions): void fun_xxx(void) {}
 * ========================================================================= */

/* fun_cwd is provided by FileIO.c. */
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
/* fun_dir is provided by FileIO.c. */
void fun_distance(void) {}
void fun_ds18b20(void) {}
/* fun_eof is provided by FileIO.c. */
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
/* fun_inputstr is provided by FileIO.c. */
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
/* fun_loc / fun_lof are provided by FileIO.c. */
void fun_peek(void) {}
void fun_pin(void) {
    int pin;
    pin = host_parse_pin_arg(ep);
    iret = vm_sys_pin_read(pin);
    targ = T_INT;
}
void fun_pio(void) {}
void fun_port(void) {}
void fun_pulsin(void) {}
void fun_spi(void) {}
void fun_spi2(void) {}
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
/* CloseAllFiles is provided by FileIO.c. */
/* CloseAudio is provided by Audio.c host body. */
void closeframebuffer(char layer) { host_framebuffer_close(layer); }
void clear320(void) {}
/* DisplayPutC is now the real one from gfx_console_shared.c. It gates on
 * Option.DISPLAY_CONSOLE and calls through the DrawBitmap / DrawRectangle
 * function pointers set up in host_runtime_begin. */
/* enable_interrupts_pico / disable_interrupts_pico are provided by
 * FileIO.c (body empty-gated under MMBASIC_HOST). */
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
 * PFltComma are in MMBasic_Print.c (shared). MMfputs/MMfeof/MMfputc/MMfgetc
 * are now provided by FileIO.c: PRINT and INPUT on host land in those
 * functions, which then dispatch to the SerialPutchar path for COM#s (no-op
 * on host) or to FilePutStr/FileGetChar — which themselves shunt through
 * host_fs_posix_* for POSIX-backed files. Console (fnbr==0) still reaches
 * putConsole → MMputchar via the same dispatch, untouched. */
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

/* host_fs_posix_* — POSIX side table consumed by FileIO.c shunts.
 * When host_sd_root is set (REPL / --sim) every BasicFileOpen call is
 * redirected through fopen() so the Editor / RUN / OPEN … for OUTPUT
 * etc. can read and write real files under the user's directory. When
 * host_sd_root is NULL (test harness) the side table stays empty and
 * FileIO.c's normal FatFS path runs against vm_host_fat.c's RAM disk.
 *
 * FileIO.c's primitives (BasicFileOpen, FileGetChar, FilePutChar,
 * FileClose, FileEOF, cmd_seek, cmd_flush, fun_loc, fun_lof) each begin
 * with a small #ifdef MMBASIC_HOST preamble that hands off to the
 * host_fs_posix_* routine below if this fnbr has a POSIX entry. */
static FILE *host_posix_files[MAXOPENFILES + 1] = {0};

int host_fs_posix_active(int fnbr) {
    return fnbr >= 1 && fnbr <= MAXOPENFILES && host_posix_files[fnbr] != NULL;
}

int host_fs_posix_try_open(char *fname, int fnbr, int mode) {
    if (!host_sd_root) return 0;
    char path[STRINGSIZE];
    host_resolve_sd_path(fname, path, sizeof(path));
    const char *m = (mode & FA_WRITE) ? ((mode & FA_CREATE_ALWAYS) ? "wb"
                                       : (mode & FA_OPEN_APPEND)  ? "ab" : "rb+")
                                      : "rb";
    FILE *fp = fopen(path, m);
    if (!fp) error("File error");
    struct stat st;
    size_t size = 0;
    if (fstat(fileno(fp), &st) == 0) size = (size_t)st.st_size;
    FileTable[fnbr].fptr = calloc(1, sizeof(FIL));
    if (!FileTable[fnbr].fptr) { fclose(fp); error("Not enough memory"); }
    /* Fill obj.objsize so Editor.c's f_size(fptr) returns the real size. */
    FileTable[fnbr].fptr->obj.objsize = (FSIZE_t)size;
    host_posix_files[fnbr] = fp;
    filesource[fnbr] = FATFSFILE;
    return 1;
}

char host_fs_posix_get_char(int fnbr) {
    int c = fgetc(host_posix_files[fnbr]);
    return c == EOF ? 0 : (char)c;
}

char host_fs_posix_put_char(char c, int fnbr) {
    if (fputc((unsigned char)c, host_posix_files[fnbr]) == EOF) error("File error");
    return c;
}

void host_fs_posix_put_str(int count, char *s, int fnbr) {
    if (count <= 0) return;
    if (fwrite(s, 1, (size_t)count, host_posix_files[fnbr]) != (size_t)count)
        error("File error");
}

int host_fs_posix_eof(int fnbr) {
    FILE *fp = host_posix_files[fnbr];
    /* ANSI feof only returns true after a read that hit EOF — BASIC's EOF
     * wants a lookahead. Peek one byte, push it back. */
    int c = fgetc(fp);
    if (c == EOF) return 1;
    ungetc(c, fp);
    return 0;
}

void host_fs_posix_close(int fnbr) {
    fclose(host_posix_files[fnbr]);
    host_posix_files[fnbr] = NULL;
    free(FileTable[fnbr].fptr);
    FileTable[fnbr].fptr = NULL;
    filesource[fnbr] = NONEFILE;
}

int64_t host_fs_posix_loc(int fnbr) {
    return (int64_t)ftell(host_posix_files[fnbr]);
}

int64_t host_fs_posix_lof(int fnbr) {
    return (int64_t)FileTable[fnbr].fptr->obj.objsize;
}

void host_fs_posix_seek(int fnbr, int64_t offset) {
    /* BASIC SEEK is 1-based. FileIO.c's cmd_seek adjusts before calling. */
    if (fseek(host_posix_files[fnbr], (long)offset, SEEK_SET) != 0)
        error("File error");
}

void host_fs_posix_flush(int fnbr) {
    fflush(host_posix_files[fnbr]);
}

/* POSIX-backed existence check. The Editor's file-load path (EDIT "foo.bas")
 * needs this to return truthful answers — otherwise `edit` leaves its p
 * pointer NULL and dereferences it at Editor.c:511. Also relied on by
 * cmd_run "file", AUTOSAVE recovery, etc. When no --sd-root is configured
 * (host_sd_root == NULL) we fall back to the CWD so the tree-of-.bas files
 * in the repo root just work for direct-run invocations. */
int ExistsFile(char *fname) {
    if (!fname || !*fname) return 0;
    char path[STRINGSIZE];
    if (host_sd_root) {
        size_t rl = strlen(host_sd_root);
        size_t fl = strlen(fname);
        int need_sep = (fname[0] != '/' && rl > 0 && host_sd_root[rl - 1] != '/');
        if (fname[0] == '/') {
            snprintf(path, sizeof(path), "%s", fname);
        } else if (rl + (need_sep ? 1 : 0) + fl + 1 > sizeof(path)) {
            return 0;
        } else {
            snprintf(path, sizeof(path), "%s%s%s",
                     host_sd_root, need_sep ? "/" : "", fname);
        }
    } else {
        snprintf(path, sizeof(path), "%s", fname);
    }
    struct stat st;
    return stat(path, &st) == 0 ? 1 : 0;
}
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

/* FlashWrite*, FlashSetAddress, LoadOptions, SaveOptions, ResetAllFlash,
 * ResetOptions, ResetFlashStorage, CheckSDCard, CrunchData, ClearSavedVars,
 * ForceFileClose, ErrorCheck, positionfile, drivecheck, getfullfilename,
 * GetCWD, InitSDCard are now provided by FileIO.c. Flash writes go through
 * flash_range_* (RAM-backed flash_prog_buf); InitSDCard just needs
 * vm_host_fat to be mounted. */
void CallCFunction(unsigned char *p, unsigned char *args, int *t, unsigned char **s) { (void)p; (void)args; (void)t; (void)s; }
void CallExecuteProgram(char *p) { (void)p; }
/* SaveProgramToFlash lives in PicoMite.c on device; on host that file
 * isn't linked so we provide a tokenise-in-place shim. Called from the
 * Editor (F1 Save) and from FileIO.c's cmd_load / FileLoadProgram with
 * EdBuff or an assembled source buffer. `pm` is NUL-terminated source
 * text, NOT tokens, so route it through the host tokeniser path.
 *
 * Do NOT call ClearRuntime here: EdBuff is allocated via GetTempMemory
 * inside edit(), and ClearRuntime frees temp memory — freeing the very
 * buffer we're about to tokenise. */
void SaveProgramToFlash(unsigned char *pm, int msg) {
    (void)msg;
    if (!pm) return;
    load_basic_source((const char *)pm);
    PrepareProgram(false);
}

/* LFS stubs — FileIO.c references the full littlefs surface. On host
 * nothing actually stores to flash via LFS (BasicFileOpen routes through
 * POSIX / FatFS instead), but every reachable call site has to link. */
int lfs_file_close(lfs_t *l, lfs_file_t *file) { (void)l; (void)file; return 0; }
int lfs_file_open(lfs_t *l, lfs_file_t *file, const char *path, int flags) { (void)l; (void)file; (void)path; (void)flags; return -1; }
lfs_ssize_t lfs_file_read(lfs_t *l, lfs_file_t *file, void *buf, lfs_size_t size) { (void)l; (void)file; (void)buf; (void)size; return 0; }
lfs_soff_t lfs_file_seek(lfs_t *l, lfs_file_t *file, lfs_soff_t off, int whence) { (void)l; (void)file; (void)off; (void)whence; return 0; }
lfs_ssize_t lfs_file_write(lfs_t *l, lfs_file_t *file, const void *buf, lfs_size_t size) { (void)l; (void)file; (void)buf; (void)size; return 0; }
lfs_ssize_t lfs_fs_size(lfs_t *l) { (void)l; return 0; }
int lfs_remove(lfs_t *l, const char *path) { (void)l; (void)path; return 0; }
int lfs_stat(lfs_t *l, const char *path, struct lfs_info *info) { (void)l; (void)path; (void)info; return -1; }
int lfs_dir_open(lfs_t *l, lfs_dir_t *dir, const char *path) { (void)l; (void)dir; (void)path; return -1; }
int lfs_dir_close(lfs_t *l, lfs_dir_t *dir) { (void)l; (void)dir; return 0; }
int lfs_dir_read(lfs_t *l, lfs_dir_t *dir, struct lfs_info *info) { (void)l; (void)dir; (void)info; return 0; }
int lfs_file_rewind(lfs_t *l, lfs_file_t *file) { (void)l; (void)file; return 0; }
int lfs_file_sync(lfs_t *l, lfs_file_t *file) { (void)l; (void)file; return 0; }
lfs_soff_t lfs_file_tell(lfs_t *l, lfs_file_t *file) { (void)l; (void)file; return 0; }
int lfs_format(lfs_t *l, const struct lfs_config *cfg) { (void)l; (void)cfg; return 0; }
int lfs_mount(lfs_t *l, const struct lfs_config *cfg) { (void)l; (void)cfg; return 0; }
int lfs_unmount(lfs_t *l) { (void)l; return 0; }
lfs_ssize_t lfs_getattr(lfs_t *l, const char *path, uint8_t type, void *buf, lfs_size_t size) { (void)l; (void)path; (void)type; (void)buf; (void)size; return -1; }
int lfs_setattr(lfs_t *l, const char *path, uint8_t type, const void *buf, lfs_size_t size) { (void)l; (void)path; (void)type; (void)buf; (void)size; return 0; }
int lfs_removeattr(lfs_t *l, const char *path, uint8_t type) { (void)l; (void)path; (void)type; return 0; }
int lfs_mkdir(lfs_t *l, const char *path) { (void)l; (void)path; return 0; }
int lfs_rename(lfs_t *l, const char *oldpath, const char *newpath) { (void)l; (void)oldpath; (void)newpath; return 0; }

/* Drawing stubs */
void UnloadFont(int f) { (void)f; }
/* ShowCursor is now the real one from gfx_console_shared.c. It reads
 * CursorTimer (ticked below in host_sync_msec_timer_value) and draws the
 * blinking underline via DrawLine. */
void setmode(int mode, bool clear) { (void)mode; (void)clear; }
void copyframetoscreen(uint8_t *s, int xstart, int xend, int ystart, int yend, int odd) { (void)s; (void)xstart; (void)xend; (void)ystart; (void)yend; (void)odd; }
void copybuffertoscreen(unsigned char *s, int lx, int ly, int hx, int hy) { (void)s; (void)lx; (void)ly; (void)hx; (void)hy; }
void merge(uint8_t colour) { (void)colour; }
void blitmerge(int x0, int y0, int w, int h, uint8_t colour) { (void)x0; (void)y0; (void)w; (void)h; (void)colour; }

/* SPI stubs */
void spi_write_command(unsigned char data) { (void)data; }
void spi_write_data(unsigned char data) { (void)data; }
unsigned char SerialPutchar(int comnbr, unsigned char c) { (void)comnbr; (void)c; return c; }
void WriteComand(int cmd) { (void)cmd; }
void WriteData(int data) { (void)data; }
void SPIClose(void) {}
void SPI2Close(void) {}

/* Serial / GPS / Audio stubs — FileIO.c references these transitively.
 * Host has no UART peripheral and no GPS, so every call errors or no-ops. */
void SerialOpen(unsigned char *spec) { (void)spec; error("COM: not supported on host"); }
void SerialClose(int comnbr) { (void)comnbr; }
int SerialGetchar(int comnbr) { (void)comnbr; return -1; }
int SerialRxStatus(int comnbr) { (void)comnbr; return 0; }
int SerialTxStatus(int comnbr) { (void)comnbr; return 0; }
void disable_audio(void) {}
int ExistsDir(char *p, char *q, int *filesystem) { (void)p; (void)q; (void)filesystem; return 0; }

/* GPS globals — FileIO.c externs them for DATE$/TIME$ with GPS=N. */
volatile char gpsbuf1[128] = {0};
volatile char gpsbuf2[128] = {0};
volatile char * volatile gpsbuf = NULL;
volatile char gpscount = 0;
volatile int gpscurrent = 0;
volatile int gpsmonitor = 0;
MMFLOAT GPSlatitude = 0, GPSlongitude = 0, GPSspeed = 0, GPStrack = 0;
MMFLOAT GPSdop = 0, GPSaltitude = 0;
int GPSvalid = 0, GPSfix = 0, GPSadjust = 0, GPSsatellites = 0;
char GPStime[9] = {0};
char GPSdate[11] = {0};

/* Flash layout externs referenced from FileIO.c. On device these live at
 * fixed XIP addresses (linker script); on host there's no flash so we
 * back them with RAM buffers.
 *
 * flash_option_contents is memcpy()'d into Option by LoadOptions — NOT
 * just at startup but every time error() fires (see MMBasic.c:2835).
 * Host-specific terminal geometry (Option.Width / Height /
 * DISPLAY_CONSOLE / …) gets set by host_main.c at boot; if this
 * buffer is blank, every error wipes those fields and the framebuffer
 * console goes silent + cmd_files' wrap-at-Width collapses to zero.
 *
 * The fix: host_options_snapshot() (called by host_runtime_begin
 * after everyone has finished mutating Option) copies the current
 * Option back into this buffer so subsequent LoadOptions calls
 * restore the initialized state, not the zero-filled one.
 *
 * We still start zero-filled (not 0xFF) because Option.PIN is an int
 * and all-0xFF would be 0xFFFFFFFF which trips the PIN-lockdown
 * prompt in MMBasic_REPL.c:196.
 *
 * flash_target_buf uses 0xFF fill — PrepareProgramExt walks it looking
 * for the CFunction terminator, and 0xFF is correct "erased" semantics. */
static uint8_t host_flash_option_buf[sizeof(struct option_s)];
static uint8_t host_flash_target_buf[4096];
const uint8_t *flash_option_contents = host_flash_option_buf;
const uint8_t *flash_target_contents = host_flash_target_buf;
__attribute__((constructor))
static void host_flash_contents_init(void) {
    memset(host_flash_option_buf, 0x00, sizeof(host_flash_option_buf));
    memset(host_flash_target_buf, 0xFF, sizeof(host_flash_target_buf));
}

void host_options_snapshot(void) {
    memcpy(host_flash_option_buf, &Option, sizeof(Option));
}

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

/* Draw.c references these BMP-decoder symbols (BmpDecoder.c is not in
 * the host build) and the display_details[] table (SPI-LCD.c not in
 * host build). On host DISPLAY_TYPE is always 0 so display_details is
 * never indexed in practice; BMP loading falls through to an error. */
#include "SPI-LCD.h"
const struct Displays display_details[1] = {{ .ref = 0, .name = {0}, .speed = 0,
    .horizontal = 0, .vertical = 0, .bits = 0, .buffered = 0,
    .CPOL = 0, .CPHASE = 0 }};

unsigned char BDEC_bReadHeader(void *pBmpDec, int fnbr) {
    (void)pBmpDec; (void)fnbr;
    error("BMP not supported on host");
    return 1;
}

unsigned char BMP_bDecode_memory(int x, int y, int xlen, int ylen, int fnbr, char *p) {
    (void)x; (void)y; (void)xlen; (void)ylen; (void)fnbr; (void)p;
    error("BMP not supported on host");
    return 1;
}
