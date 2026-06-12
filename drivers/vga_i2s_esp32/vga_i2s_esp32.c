/*
 * drivers/vga_i2s_esp32/vga_i2s_esp32.c — classic-ESP32 VGA scanout via the
 * I2S1 parallel "LCD mode". See the header for the wire format.
 *
 * Frame structure (industry-standard 640x480@60, 25.175 MHz pixel clock):
 *   horizontal, per 800-byte line buffer, starting at the sync pulse:
 *     [96 hsync] [48 back porch] [640 visible] [16 front porch]
 *   vertical, per 525-descriptor loop:
 *     lines 0..1    vsync        (shared constant buffer, VS low)
 *     lines 2..34   back porch   (shared constant buffer)
 *     lines 35..514 visible      (ring of VGA_I2S_RING buffers)
 *     lines 515..524 front porch (shared constant buffer)
 *
 * Starting each line at the sync pulse keeps the VSync level transitions
 * aligned with the start of an hsync pulse, which monitors expect.
 *
 * The DMA-completion (out_eof) interrupt fires after each visible line.
 * The buffer that just finished scanning is refilled — through the fill
 * callback — with the content of the line that will reuse it one ring
 * revolution later, so the callback runs VGA_I2S_RING-1 lines ahead of
 * the beam and one ISR hiccup under ~90 us is invisible.
 */

#include "vga_i2s_esp32.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_private/periph_ctrl.h"
#include "esp_rom_gpio.h"
#include "rom/lldesc.h"
#include "soc/gpio_periph.h"
#include "soc/gpio_sig_map.h"
#include "soc/i2s_reg.h"
#include "soc/i2s_struct.h"
#include "soc/io_mux_reg.h"
#include "soc/rtc.h"

/* 640x480@60 timing. */
#define VGA_H_SYNC 96
#define VGA_H_BACK 48
#define VGA_H_VIS 640
#define VGA_H_FRONT 16
#define VGA_LINE_BYTES (VGA_H_SYNC + VGA_H_BACK + VGA_H_VIS + VGA_H_FRONT) /* 800 */
#define VGA_VIS_OFFSET (VGA_H_SYNC + VGA_H_BACK)                           /* 144 */

#define VGA_V_SYNC 2
#define VGA_V_BACK 33
#define VGA_V_VIS 480
#define VGA_V_FRONT 10
#define VGA_V_LINES (VGA_V_SYNC + VGA_V_BACK + VGA_V_VIS + VGA_V_FRONT) /* 525 */
#define VGA_FIRST_VIS (VGA_V_SYNC + VGA_V_BACK)                         /* 35 */

#define VGA_PIXEL_HZ 25175000L

/* Visible-line buffer ring. Four buffers = ~110 us of fill slack before
 * a stalled fill ISR shows stale lines. The fill interrupt lives on core
 * 1 at level 3, where nothing else runs long enough to eat that slack;
 * the no-PSRAM boards need the other four line buffers (3.2 KB) for the
 * Wi-Fi stack to coexist with the scanout. */
#define VGA_I2S_RING 4

/* Byte values for the constant line regions: black pixels, sync bits per
 * region. (Constant regions are all one value, so the FIFO byte swizzle
 * is a no-op there; region widths are multiples of 4 so swizzle groups
 * never straddle a boundary.) */
#define VGA_BYTE_IDLE 0xC0u /* HS idle, VS idle */
#define VGA_BYTE_HS 0x80u   /* HS pulse, VS idle */
#define VGA_BYTE_VS 0x40u   /* HS idle, VS pulse */
#define VGA_BYTE_HSVS 0x00u /* both pulses (hsync region of a vsync line) */

typedef struct {
    intr_handle_t intr;
    lldesc_t * desc;      /* VGA_V_LINES descriptors, looped */
    uint8_t * line_blank; /* shared vertical-porch line */
    uint8_t * line_vsync; /* shared vsync line */
    uint8_t * ring[VGA_I2S_RING];
    vga_i2s_line_fill_fn fill;
    void * fill_ctx;
    int8_t gpio[8];
    volatile bool active;
} vga_i2s_state_t;

static vga_i2s_state_t s_vga;

/* ---- line buffer construction ---- */

static void build_line(uint8_t * buf, uint8_t sync_byte, uint8_t rest_byte) {
    memset(buf, sync_byte, VGA_H_SYNC);
    memset(buf + VGA_H_SYNC, rest_byte, VGA_LINE_BYTES - VGA_H_SYNC);
}

/* ---- DMA completion ISR ---- */

static void IRAM_ATTR vga_i2s_isr(void * arg) {
    vga_i2s_state_t * st = (vga_i2s_state_t *)arg;
    uint32_t status = I2S1.int_st.val;
    I2S1.int_clr.val = status;
    if (!(status & I2S_OUT_EOF_INT_ST_M)) return;

    /* Which descriptor finished? Its index gives the absolute line. */
    lldesc_t * done = (lldesc_t *)I2S1.out_eof_des_addr;
    int line = (int)(done - st->desc);
    int v = line - VGA_FIRST_VIS;
    if (v < 0 || v >= VGA_V_VIS) return; /* blanking buffers carry no EOF */

    /* The buffer just scanned is reused one ring revolution from now. */
    int next = v + VGA_I2S_RING;
    if (next >= VGA_V_VIS) next -= VGA_V_VIS;
    if (st->fill)
        st->fill(next, st->ring[v % VGA_I2S_RING] + VGA_VIS_OFFSET, st->fill_ctx);
}

/* ---- peripheral bring-up (technique per bitluni's ESP32Lib + TRM) ---- */

static void vga_i2s_reset_periph(void) {
    const uint32_t lc_rst =
        I2S_IN_RST_M | I2S_OUT_RST_M | I2S_AHBM_RST_M | I2S_AHBM_FIFO_RST_M;
    I2S1.lc_conf.val |= lc_rst;
    I2S1.lc_conf.val &= ~lc_rst;
    const uint32_t conf_rst =
        I2S_RX_RESET_M | I2S_RX_FIFO_RESET_M | I2S_TX_RESET_M | I2S_TX_FIFO_RESET_M;
    I2S1.conf.val |= conf_rst;
    I2S1.conf.val &= ~conf_rst;
    while (I2S1.state.rx_fifo_reset_back) {
    }
}

static void vga_i2s_apll_pixel_clock(void) {
    /* APLL output = 2 x pixel clock for the 8-bit LCD path (wrx2 doubles
     * each sample write; the I2S module divider below halves the APLL).
     * sdm is a 16.16 fixed-point multiplier: apll = 40MHz*(4+sdm)/(2*(odir+2)).
     * Walk odir up until sdm lands in the silicon's working window. */
    long freq = VGA_PIXEL_HZ * 2;
    int sdm, sdmn, odir = -1;
    do {
        odir++;
        sdm = (int)(((double)freq / (20000000.0 / (odir + 2))) * 65536.0) - 0x40000;
        sdmn = (int)(((double)freq / (20000000.0 / (odir + 2 + 1))) * 65536.0) - 0x40000;
    } while (sdm < 0x8c0ec && odir < 31 && sdmn < 0xA1fff);
    if (sdm > 0xA1fff) sdm = 0xA1fff;
    rtc_clk_apll_enable(true);
    rtc_clk_apll_coeff_set((uint32_t)odir, (uint32_t)(sdm & 255),
                           (uint32_t)((sdm >> 8) & 255), (uint32_t)(sdm >> 16));
}

static void vga_i2s_config_periph(void) {
    periph_module_enable(PERIPH_I2S1_MODULE);

    I2S1.conf.rx_reset = 1;
    I2S1.conf.rx_reset = 0;
    I2S1.conf.tx_reset = 1;
    I2S1.conf.tx_reset = 0;
    vga_i2s_reset_periph();

    /* Parallel (LCD) transmit, 8 data bits, each sample emitted twice as
     * fast as the module clock feeds them (wrx2): pixel rate = APLL / 2. */
    I2S1.conf2.val = 0;
    I2S1.conf2.lcd_en = 1;
    I2S1.conf2.lcd_tx_wrx2_en = 1;
    I2S1.conf2.lcd_tx_sdx2_en = 0;

    I2S1.sample_rate_conf.val = 0;
    I2S1.sample_rate_conf.tx_bits_mod = 8;
    I2S1.sample_rate_conf.tx_bck_div_num = 1;

    vga_i2s_apll_pixel_clock();
    I2S1.clkm_conf.val = 0;
    I2S1.clkm_conf.clkm_div_num = 2;
    I2S1.clkm_conf.clkm_div_a = 1;
    I2S1.clkm_conf.clkm_div_b = 0;
    I2S1.clkm_conf.clka_en = 1;

    I2S1.fifo_conf.val = 0;
    I2S1.fifo_conf.tx_fifo_mod_force_en = 1;
    I2S1.fifo_conf.tx_fifo_mod = 1;
    I2S1.fifo_conf.tx_data_num = 32;
    I2S1.fifo_conf.dscr_en = 1;

    I2S1.conf1.val = 0;
    I2S1.conf1.tx_stop_en = 0;
    I2S1.conf1.tx_pcm_bypass = 1;

    I2S1.conf_chan.val = 0;
    I2S1.conf_chan.tx_chan_mod = 1;

    I2S1.conf.tx_right_first = 1;
    I2S1.conf.tx_msb_right = 0;
    I2S1.conf.tx_msb_shift = 0;

    I2S1.timing.val = 0;
}

static void vga_i2s_route_pins(const int8_t gpio[8]) {
    for (int bit = 0; bit < 8; bit++) {
        if (gpio[bit] < 0) continue;
        PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[gpio[bit]], PIN_FUNC_GPIO);
        gpio_set_direction((gpio_num_t)gpio[bit], GPIO_MODE_OUTPUT);
        /* Maximum pad drive: the sync lines feed the monitor's PLL
         * through ~120R and the colour lines switch at pixel rate into
         * the DAC ladder — soft edges show as horizontal jitter. */
        gpio_set_drive_capability((gpio_num_t)gpio[bit], GPIO_DRIVE_CAP_3);
        esp_rom_gpio_connect_out_signal(gpio[bit], I2S1O_DATA_OUT0_IDX + bit,
                                        false, false);
    }
}

static void vga_i2s_detach_pins(const int8_t gpio[8]) {
    for (int bit = 0; bit < 8; bit++) {
        if (gpio[bit] < 0) continue;
        gpio_reset_pin((gpio_num_t)gpio[bit]);
    }
}

static void vga_i2s_free_buffers(void) {
    heap_caps_free(s_vga.desc);
    s_vga.desc = NULL;
    heap_caps_free(s_vga.line_blank);
    s_vga.line_blank = NULL;
    heap_caps_free(s_vga.line_vsync);
    s_vga.line_vsync = NULL;
    for (int i = 0; i < VGA_I2S_RING; i++) {
        heap_caps_free(s_vga.ring[i]);
        s_vga.ring[i] = NULL;
    }
}

/* The line-fill interrupt must live on core 1: core 0 carries MMBasic,
 * Wi-Fi, and flash-write critical sections whose interrupt-blocked
 * windows exceed the ring slack and scan out as horizontal glitch bands.
 * esp_intr_alloc binds the interrupt to the calling core, so a short-lived
 * pinned task performs the alloc (and the matching free). */
typedef struct {
    bool do_alloc;
    volatile int done;
    esp_err_t err;
} vga_intr_req_t;

static void vga_i2s_intr_task(void * p) {
    vga_intr_req_t * req = (vga_intr_req_t *)p;
    if (req->do_alloc) {
        req->err = esp_intr_alloc(ETS_I2S1_INTR_SOURCE,
                                  ESP_INTR_FLAG_LEVEL3 | ESP_INTR_FLAG_IRAM,
                                  vga_i2s_isr, &s_vga, &s_vga.intr);
    } else {
        esp_intr_free(s_vga.intr);
        s_vga.intr = NULL;
        req->err = ESP_OK;
    }
    req->done = 1;
    vTaskDelete(NULL);
}

static esp_err_t vga_i2s_intr_on_core1(bool do_alloc) {
    vga_intr_req_t req = {.do_alloc = do_alloc, .done = 0, .err = ESP_FAIL};
    if (xTaskCreatePinnedToCore(vga_i2s_intr_task, "vga_intr", 2048, &req,
                                configMAX_PRIORITIES - 2, NULL, 1) != pdPASS)
        return ESP_FAIL;
    while (!req.done) vTaskDelay(1);
    return req.err;
}

bool vga_i2s_start(const int8_t data_gpio[8], vga_i2s_line_fill_fn fill, void * ctx) {
    if (s_vga.active) return false;

    s_vga.fill = fill;
    s_vga.fill_ctx = ctx;
    memcpy(s_vga.gpio, data_gpio, sizeof(s_vga.gpio));

    s_vga.desc = heap_caps_calloc(VGA_V_LINES, sizeof(lldesc_t),
                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_vga.line_blank =
        heap_caps_malloc(VGA_LINE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_vga.line_vsync =
        heap_caps_malloc(VGA_LINE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    bool ok = s_vga.desc && s_vga.line_blank && s_vga.line_vsync;
    for (int i = 0; i < VGA_I2S_RING && ok; i++) {
        s_vga.ring[i] =
            heap_caps_malloc(VGA_LINE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_vga.ring[i]) ok = false;
    }
    if (!ok) {
        vga_i2s_free_buffers();
        return false;
    }

    build_line(s_vga.line_blank, VGA_BYTE_HS, VGA_BYTE_IDLE);
    build_line(s_vga.line_vsync, VGA_BYTE_HSVS, VGA_BYTE_VS);
    for (int i = 0; i < VGA_I2S_RING; i++)
        build_line(s_vga.ring[i], VGA_BYTE_HS, VGA_BYTE_IDLE);

    /* Pre-fill the ring with the first visible lines so the first frame
     * is already correct. */
    if (fill)
        for (int i = 0; i < VGA_I2S_RING; i++)
            fill(i, s_vga.ring[i] + VGA_VIS_OFFSET, ctx);

    /* Descriptor loop: one per scanline. */
    for (int line = 0; line < VGA_V_LINES; line++) {
        lldesc_t * d = &s_vga.desc[line];
        uint8_t * buf;
        int eof = 0;
        if (line < VGA_V_SYNC) {
            buf = s_vga.line_vsync;
        } else if (line < VGA_FIRST_VIS) {
            buf = s_vga.line_blank;
        } else if (line < VGA_FIRST_VIS + VGA_V_VIS) {
            buf = s_vga.ring[(line - VGA_FIRST_VIS) % VGA_I2S_RING];
            eof = 1;
        } else {
            buf = s_vga.line_blank;
        }
        d->size = VGA_LINE_BYTES;
        d->length = VGA_LINE_BYTES;
        d->buf = buf;
        d->eof = eof;
        d->owner = 1;
        d->sosf = 0;
        d->offset = 0;
        d->qe.stqe_next = &s_vga.desc[(line + 1) % VGA_V_LINES];
    }

    vga_i2s_config_periph();
    vga_i2s_route_pins(s_vga.gpio);

    if (vga_i2s_intr_on_core1(true) != ESP_OK) {
        vga_i2s_detach_pins(s_vga.gpio);
        periph_module_disable(PERIPH_I2S1_MODULE);
        vga_i2s_free_buffers();
        return false;
    }

    vga_i2s_reset_periph();
    I2S1.lc_conf.val = I2S_OUT_DATA_BURST_EN | I2S_OUTDSCR_BURST_EN;
    I2S1.out_link.addr = (uint32_t)&s_vga.desc[0];
    I2S1.out_link.start = 1;
    I2S1.int_clr.val = I2S1.int_raw.val;
    I2S1.int_ena.val = 0;
    I2S1.int_ena.out_eof = 1;
    I2S1.conf.tx_start = 1;

    s_vga.active = true;
    return true;
}

void vga_i2s_stop(void) {
    if (!s_vga.active) return;
    I2S1.int_ena.val = 0;
    I2S1.conf.tx_start = 0;
    I2S1.out_link.stop = 1;
    if (s_vga.intr) (void)vga_i2s_intr_on_core1(false);
    vga_i2s_reset_periph();
    periph_module_disable(PERIPH_I2S1_MODULE);
    rtc_clk_apll_enable(false);
    vga_i2s_detach_pins(s_vga.gpio);
    vga_i2s_free_buffers();
    s_vga.fill = NULL;
    s_vga.active = false;
}

void vga_i2s_set_fill(vga_i2s_line_fill_fn fill, void * ctx) {
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&mux);
    s_vga.fill_ctx = ctx;
    s_vga.fill = fill;
    portEXIT_CRITICAL(&mux);
}

bool vga_i2s_is_active(void) {
    return s_vga.active;
}
