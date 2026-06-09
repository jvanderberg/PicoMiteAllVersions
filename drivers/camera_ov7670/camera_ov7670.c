/*
 * camera_ov7670.c — OV7670 camera driver (BASIC CAMERA command).
 *
 * RP-only DCMI-style capture: a PWM XCLK plus GPIO bit-banged PCLK/HREF/
 * VSYNC/data capture. The camera's SCCB (I²C) register access routes
 * through the shared I²C bus driver's GeneralSend / GeneralReceive so the
 * actual transactions go through hal_i2c_*. VGA-family displays carry no
 * camera connector, so those variants link the error stub.
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "I2C.h"

extern void GeneralSend(unsigned int addr, int nbr, char * p);
extern void GeneralReceive(unsigned int addr, int nbr, char * p);
extern void DrawBufferMEM(int x1, int y1, int x2, int y2, unsigned char * p);
extern void PWMoff(int slice);

int CameraSlice = -1;
int CameraChannel = -1;

#if !HAL_PORT_IS_VGA
#define ov7670_address 0x21
#define top 120
#define left 160
uint8_t PCLK = 0;
uint8_t XCLK = 0;
uint8_t HREF = 0;
uint8_t VSYNC = 0;
uint8_t RESET = 0;
uint8_t D0 = 0;
uint8_t XCLKGP = 0;
uint8_t PCLKGP = 0;
uint8_t VSYNCGP = 0;
uint8_t HREFGP = 0;
//extern volatile int ExtCurrentConfig[];
void cameraclose(void) {
    if (PCLK) ExtCfg(PCLK, EXT_NOT_CONFIG, 0);
    if (HREF) ExtCfg(HREF, EXT_NOT_CONFIG, 0);
    if (VSYNC) ExtCfg(VSYNC, EXT_NOT_CONFIG, 0);
    if (RESET) ExtCfg(RESET, EXT_NOT_CONFIG, 0);
    if (D0) {
        int startdata = PinDef[D0].GPno;
        for (int i = startdata; i < startdata + 8; i++) {
            ExtCfg(PINMAP[i], EXT_NOT_CONFIG, 0);
        }
    }
    if (XCLK) {
        PWMoff(CameraSlice);
        ExtCfg(XCLK, EXT_NOT_CONFIG, 0);
    }
    CameraSlice = -1;
    CameraChannel = -1;
    PCLK = HREF = VSYNC = D0 = XCLK = 0;
}
int readregister(int reg) {
    unsigned char buff[2];
    unsigned char regb = (unsigned char)reg;
    GeneralSend(ov7670_address, 1, (char *)&regb);
    if (mmI2Cvalue) {
        cameraclose();
        error("I2C failure");
    }
    uSec(1000);
    GeneralReceive(ov7670_address, 1, (char *)buff);
    uSec(1000);
    return buff[0];
}

void ov7670_set(char a, char b) {
    //send the command
    unsigned char buf[2];
    buf[0] = a;
    buf[1] = b;
    GeneralSend(ov7670_address, 2, (char *)buf);
    if (mmI2Cvalue) {
        cameraclose();
        error("I2C failure");
    }
    if (a == REG_COM7 && b == COM7_RESET) {
        uSec(500000);
        return;
    }
    uSec(1000);
    if (readregister(a) != b) error("Camera Config Failure");
    uSec(1000);
    return;
}

void OV7670_test_pattern(OV7670_pattern pattern) {
    // Read current SCALING_XSC and SCALING_YSC register settings,
    // so image scaling settings aren't corrupted.
    uint8_t xsc = readregister(OV7670_REG_SCALING_XSC);
    uint8_t ysc = readregister(OV7670_REG_SCALING_YSC);
    if (pattern & 1) {
        xsc |= 0x80;
    } else {
        xsc &= ~0x80;
    }
    if (pattern & 2) {
        ysc |= 0x80;
    } else {
        ysc &= ~0x80;
    }
    // Write modified results back to SCALING_XSC and SCALING_YSC registers
    ov7670_set(OV7670_REG_SCALING_XSC, xsc);
    ov7670_set(OV7670_REG_SCALING_YSC, ysc);
}

static const OV7670_command
    OV7670_yuv[] = {
        // Manual output format, YUV, use full output range
        {
            OV7670_REG_COM7,
            OV7670_COM7_YUV},
        {OV7670_REG_COM15,
         OV7670_COM15_R00FF},
        {0xFF,
         0xFF}},
    OV7670_rgb[] = {
        // Manual output format, RGB, use RGB565 and full 0-255 output range
        {OV7670_REG_COM7, OV7670_COM7_RGB},
        {OV7670_REG_RGB444, 0},
        {OV7670_REG_COM15, OV7670_COM15_RGB565 | OV7670_COM15_R00FF},
        {0xFF, 0xFF}};

void OV7670_frame_control(uint8_t size, uint8_t vstart,
                          uint16_t hstart, uint8_t edge_offset,
                          uint8_t pclk_delay) {
    uint8_t value;

    // Enable downsampling if sub-VGA, and zoom if 1:16 scale
    value = (size > OV7670_SIZE_DIV1) ? OV7670_COM3_DCWEN : 0;
    if (size == OV7670_SIZE_DIV16)
        value |= OV7670_COM3_SCALEEN;
    ov7670_set(OV7670_REG_COM3, value);

    // Enable PCLK division if sub-VGA 2,4,8,16 = 0x19,1A,1B,1C
    value = (size > OV7670_SIZE_DIV1) ? (0x18 + size) : 0;
    ov7670_set(OV7670_REG_COM14, value);

    // Horiz/vert downsample ratio, 1:8 max (H,V are always equal for now)
    value = (size <= OV7670_SIZE_DIV8) ? size : OV7670_SIZE_DIV8;
    ov7670_set(OV7670_REG_SCALING_DCWCTR, value * 0x11);

    // Pixel clock divider if sub-VGA
    value = (size > OV7670_SIZE_DIV1) ? (0xF0 + size) : 0x08;
    ov7670_set(OV7670_REG_SCALING_PCLK_DIV, value);

    // Apply 0.5 digital zoom at 1:16 size (others are downsample only)
    value = (size == OV7670_SIZE_DIV16) ? 0x40 : 0x20; // 0.5, 1.0
    // Read current SCALING_XSC and SCALING_YSC register values because
    // test pattern settings are also stored in those registers and we
    // don't want to corrupt anything there.
    uint8_t xsc = readregister(OV7670_REG_SCALING_XSC);
    uint8_t ysc = readregister(OV7670_REG_SCALING_YSC);
    xsc = (xsc & 0x80) | value; // Modify only scaling bits (not test pattern)
    ysc = (ysc & 0x80) | value;
    // Write modified result back to SCALING_XSC and SCALING_YSC
    ov7670_set(OV7670_REG_SCALING_XSC, xsc);
    ov7670_set(OV7670_REG_SCALING_YSC, ysc);

    // Window size is scattered across multiple registers.
    // Horiz/vert stops can be automatically calc'd from starts.
    uint16_t vstop = vstart + 480;
    uint16_t hstop = (hstart + 640) % 784;
    ov7670_set(OV7670_REG_HSTART, hstart >> 3);
    ov7670_set(OV7670_REG_HSTOP, hstop >> 3);
    ov7670_set(OV7670_REG_HREF,
               (edge_offset << 6) | ((hstop & 0b111) << 3) |
                   (hstart & 0b111));
    ov7670_set(OV7670_REG_VSTART, vstart >> 2);
    ov7670_set(OV7670_REG_VSTOP, vstop >> 2);
    ov7670_set(OV7670_REG_VREF,
               ((vstop & 0b11) << 2) | (vstart & 0b11));

    ov7670_set(OV7670_REG_SCALING_PCLK_DELAY, pclk_delay);
}
void OV7670_set_size(OV7670_size size) {
    // Array of five window settings, index of each (0-4) aligns with the five
    // OV7670_size enumeration values. If enum changes, list must change!
    static struct {
        uint8_t vstart;
        uint8_t hstart;
        uint8_t edge_offset;
        uint8_t pclk_delay;
    } window[] = {
        // Window settings were tediously determined empirically.
        // I hope there's a formula for this, if a do-over is needed.
        {
            9,
            162,
            2,
            2}, // SIZE_DIV1  640x480 VGA
        {
            10,
            174,
            4,
            2}, // SIZE_DIV2  320x240 QVGA
        {
            11,
            186,
            2,
            2}, // SIZE_DIV4  160x120 QQVGA
        {
            12,
            210,
            0,
            2}, // SIZE_DIV8  80x60   ...
        {
            15,
            252,
            3,
            2}, // SIZE_DIV16 40x30
    };

    OV7670_frame_control(size, window[size].vstart, window[size].hstart,
                         window[size].edge_offset, window[size].pclk_delay);
}
#define ST_PCLK gpio_get(PCLKGP)
#define ST_HREF gpio_get(HREFGP)
#define ST_VSYNC gpio_get(VSYNCGP)
void __not_in_flash_func(capture)(char * buff) {
    char * k = buff;
    while (ST_VSYNC) {
    } /* wait for the old frame to end */
    while (!ST_VSYNC) {
    } /* wait for a new frame to start */
    //At this point VSync has gone high and the frame is about to start
    while (!ST_HREF) {
    } // wait for the first line to start
    while (!ST_PCLK) {
    } // wait for clock to go high /
    while (ST_PCLK) {
    } // wait for clock to go back low /
    for (int i = 0; i < 160; i++) {
        while (!ST_PCLK) {
        } // wait for clock to go high /
        *k++ = gpio_get_all64() >> PinDef[D0].GPno;
        while (ST_PCLK) {
        } // wait for clock to go back low /

        // second byte/
        while (!ST_PCLK) {
        } // wait for clock to go high /
        *k++ = gpio_get_all64() >> PinDef[D0].GPno;
        while (ST_PCLK) {
        } // wait for clock to go back low /
    }
    while (ST_HREF) {
    } // wait for the first line to end*/
    k = buff;
    for (int j = 0; j < 119; j++) {
        while (!ST_HREF) {
        } // wait for the first line to end
        for (int i = 0; i < 160; i++) {
            while (!ST_PCLK) {
            } // wait for clock to go high /
            *k++ = gpio_get_all64() >> PinDef[D0].GPno;
            while (ST_PCLK) {
            } // wait for clock to go back low /

            // second byte/
            while (!ST_PCLK) {
            } // wait for clock to go high /
            *k++ = gpio_get_all64() >> PinDef[D0].GPno;
            while (ST_PCLK) {
            } // wait for clock to go back low /
        }
        while (ST_HREF) {
        } // wait for the first line to end
    }
}
void saturation(int s) //-2 to 2
{
    //color matrix values
    ov7670_set(0x4f, 0x80 + 0x20 * s);
    ov7670_set(0x50, 0x80 + 0x20 * s);
    ov7670_set(0x51, 0x00);
    ov7670_set(0x52, 0x22 + (0x11 * s) / 2);
    ov7670_set(0x53, 0x5e + (0x2f * s) / 2);
    ov7670_set(0x54, 0x80 + 0x20 * s);
    ov7670_set(0x58, 0x9e); //matrix signs
}

/*  @endcond */

void MIPS16 cmd_camera(void) {
    union colourmap {
        char rgbbytes[4];
        unsigned int rgb;
    } c;
    unsigned char * tp = NULL;
    if ((tp = checkstring(cmdline, (unsigned char *)"OPEN"))) {
        int pin1, pin2, pin3, pin4, pin5, pin6;
        getargs(&tp, 11, (unsigned char *)",");
        if (argc != 11) error("Syntax");
        if (!(Option.DISPLAY_TYPE > I2C_PANEL && Option.DISPLAY_TYPE < BufferedPanel)) error("Invalid display type");
        if (!(I2C0locked || I2C1locked)) error("SYSTEM I2C not configured");
        if (XCLK) error("Camera already open");
        unsigned char code;
        //XCLK pin
        if (!(code = codecheck(argv[0]))) argv[0] += 2;
        pin1 = getinteger(argv[0]);
        if (!code) pin1 = codemap(pin1);
        if (IsInvalidPin(pin1)) error("Invalid pin");
        if (ExtCurrentConfig[pin1] != EXT_NOT_CONFIG) error("Pin %/| is in use", pin1, pin1);
        int slice = getslice(pin1);
        if ((PinDef[Option.DISPLAY_BL].slice & 0x7f) == slice) error("Channel in use for backlight");
        if ((PinDef[pin1].slice & 0x7f) == Option.AUDIO_SLICE) error("Channel in use for Audio");

        //PCLK pin
        if (!(code = codecheck(argv[2]))) argv[2] += 2;
        pin2 = getinteger(argv[2]);
        if (!code) pin2 = codemap(pin2);
        if (IsInvalidPin(pin2)) error("Invalid pin");
        if (ExtCurrentConfig[pin2] != EXT_NOT_CONFIG) error("Pin %/| is in use", pin2, pin2);

        //HREF pin
        if (!(code = codecheck(argv[4]))) argv[4] += 2;
        pin3 = getinteger(argv[4]);
        if (!code) pin3 = codemap(pin3);
        if (IsInvalidPin(pin3)) error("Invalid pin");
        if (ExtCurrentConfig[pin3] != EXT_NOT_CONFIG) error("Pin %/| is in use", pin3, pin3);

        //VSYNC pin
        if (!(code = codecheck(argv[6]))) argv[6] += 2;
        pin4 = getinteger(argv[6]);
        if (!code) pin4 = codemap(pin4);
        if (IsInvalidPin(pin4)) error("Invalid pin");
        if (ExtCurrentConfig[pin4] != EXT_NOT_CONFIG) error("Pin %/| is in use", pin4, pin4);

        //RESET pin
        if (!(code = codecheck(argv[8]))) argv[8] += 2;
        pin5 = getinteger(argv[8]);
        if (!code) pin5 = codemap(pin5);
        if (IsInvalidPin(pin5)) error("Invalid pin");
        if (ExtCurrentConfig[pin5] != EXT_NOT_CONFIG) error("Pin %/| is in use", pin5, pin5);

        //D0-D7 pins
        if (!(code = codecheck(argv[10]))) argv[10] += 2;
        pin6 = getinteger(argv[10]);
        if (!code) pin6 = codemap(pin6);
        if (IsInvalidPin(pin6)) error("Invalid pin");
        int startdata = PinDef[pin6].GPno;
        for (int i = startdata; i < startdata + 8; i++) {
            if (IsInvalidPin(PINMAP[i])) error("Invalid pin");
            if (ExtCurrentConfig[PINMAP[i]] != EXT_NOT_CONFIG) error("Pin %/| is in use", PINMAP[i], PINMAP[i]);
        }
        XCLK = pin1;
        PCLK = pin2;
        HREF = pin3;
        VSYNC = pin4;
        RESET = pin5;
        D0 = pin6;
        setpwm(pin1, &CameraChannel, &CameraSlice, 12000000.0, 50.0);
        ExtCfg(XCLK, EXT_COM_RESERVED, 0);
        ExtCfg(PCLK, EXT_DIG_IN, 0);
        ExtCfg(PCLK, EXT_COM_RESERVED, 0);
        ExtCfg(HREF, EXT_DIG_IN, 0);
        ExtCfg(HREF, EXT_COM_RESERVED, 0);
        ExtCfg(VSYNC, EXT_DIG_IN, 0);
        ExtCfg(VSYNC, EXT_COM_RESERVED, 0);
        ExtCfg(RESET, EXT_DIG_OUT, 1);
        ExtCfg(RESET, EXT_COM_RESERVED, 0);
        for (int i = startdata; i < startdata + 8; i++) {
            ExtCfg(PINMAP[i], EXT_DIG_IN, 0);
            ExtCfg(PINMAP[i], EXT_COM_RESERVED, 0);
        }
        PCLKGP = PinDef[PCLK].GPno;
        VSYNCGP = PinDef[VSYNC].GPno;
        HREFGP = PinDef[HREF].GPno;
        uSec(1000);
        PinSetBit(pin5, LATCLR);
        uSec(1000);
        PinSetBit(pin5, LATSET);
        uSec(1000);
        if (readregister(REG_PID) != 118) error("Camera not found");
        ov7670_set(REG_COM7, COM7_RESET); // RESET CAMERA
        ov7670_set(REG_COM7, COM7_RESET); // RESET CAMERA
        ov7670_set(REG_RGB444, 0);
        ov7670_set(REG_COM10, 0x02); // 0x02   VSYNC negative (http://nasulica.homelinux.org/?p=959)
        ov7670_set(REG_MVFP, 0x37);
        // 		ov7670_set( REG_CLKRC, 0x40);
        ov7670_set(REG_COM11, 0x0A);
        ov7670_set(REG_COM7, COM7_RGB);
        ov7670_set(REG_COM1, 0);
        ov7670_set(REG_COM15, COM15_RGB565);
        ov7670_set(REG_COM9, 0x2A);
        ov7670_set(REG_TSLB, 0x04); // 0D = UYVY  04 = YUYV
        ov7670_set(REG_COM13, 0x88);
        ov7670_set(REG_HSTART, 0x13);
        ov7670_set(REG_HSTOP, 0x01);
        ov7670_set(REG_HREF, 0xb6);
        ov7670_set(REG_VSTART, 0x02);
        ov7670_set(REG_VSTOP, 0x7a);
        ov7670_set(REG_VREF, 0x0a);
        ov7670_set(REG_COM5, 0x61);
        ov7670_set(REG_COM6, 0x4b);
        ov7670_set(0x16, 0x02);
        ov7670_set(0x21, 0x02);
        ov7670_set(0x22, 0x91);
        ov7670_set(0x29, 0x07);
        ov7670_set(0x33, 0x0b);
        ov7670_set(0x35, 0x0b);
        ov7670_set(0x37, 0x1d);
        ov7670_set(0x38, 0x71);
        ov7670_set(0x39, 0x2a);
        ov7670_set(REG_COM12, 0x78);

        ov7670_set(0x4d, 0x40);
        ov7670_set(0x4e, 0x20);
        ov7670_set(REG_GFIX, 0);
        ov7670_set(0x74, 0x10);
        ov7670_set(0x8d, 0x4f);
        ov7670_set(0x8e, 0);
        ov7670_set(0x8f, 0);
        ov7670_set(0x90, 0);
        ov7670_set(0x91, 0);
        ov7670_set(0x96, 0);
        ov7670_set(0x9a, 0);

        ov7670_set(0xb0, 0x84);
        ov7670_set(0xb1, 0x0c);
        ov7670_set(0xb2, 0x0e);
        ov7670_set(0xb3, 0x82); //
        ov7670_set(0xb8, 0x0a);
        ov7670_set(0x7a, 0x20); //gamma correction
        ov7670_set(0x7b, 0x10);
        ov7670_set(0x7c, 0x1e);
        ov7670_set(0x7d, 0x35);
        ov7670_set(0x7e, 0x5a);
        ov7670_set(0x7f, 0x69);
        ov7670_set(0x80, 0x76);
        ov7670_set(0x81, 0x80);
        ov7670_set(0x82, 0x88);
        ov7670_set(0x83, 0x8f);
        ov7670_set(0x84, 0x96);
        ov7670_set(0x85, 0xa3);
        ov7670_set(0x86, 0xaf);
        ov7670_set(0x87, 0xc4);
        ov7670_set(0x88, 0xd7);
        ov7670_set(0x89, 0xe8);
        // AGC and AEC parameters. Note we start by disabling those features,
        //then turn them only after tweaking the values.
        ov7670_set(0x13, COM8_FASTAEC | COM8_AECSTEP | COM8_BFILT);
        ov7670_set(0x00, 0);
        ov7670_set(0x10, 0);
        ov7670_set(0x0d, 0x40);
        ov7670_set(0x14, 0x18);
        ov7670_set(0xa5, 0x05);
        ov7670_set(0xab, 0x07);
        ov7670_set(0x24, 0x95);
        ov7670_set(0x25, 0x33);
        ov7670_set(0x26, 0xe3);
        ov7670_set(0x9f, 0x78);
        ov7670_set(0xa0, 0x68);
        ov7670_set(0xa1, 0x03);
        ov7670_set(0xa6, 0xd8);
        ov7670_set(0xa7, 0xd8);
        ov7670_set(0xa8, 0xf0);
        ov7670_set(0xa9, 0x90);
        ov7670_set(0xaa, 0x94);
        ov7670_set(0x13, COM8_FASTAEC | COM8_AECSTEP | COM8_BFILT | COM8_AGC | COM8_AEC);
        // Almost all of these are magic "reserved" values. */
        ov7670_set(0x0e, 0x61);
        ov7670_set(0x0f, 0x4b);
        ov7670_set(0x16, 0x02);
        // 		ov7670_set(0x1e, 0x27);
        ov7670_set(0x21, 0x02);
        ov7670_set(0x22, 0x91);
        ov7670_set(0x29, 0x07);
        ov7670_set(0x33, 0x0b);
        ov7670_set(0x35, 0x0b);
        ov7670_set(0x37, 0x1d);
        ov7670_set(0x38, 0x71);
        ov7670_set(0x39, 0x2a);
        // 		ov7670_set(0x3c, 0x78);
        ov7670_set(0x4d, 0x40);
        ov7670_set(0x4e, 0x20);
        ov7670_set(0x69, 0);
        // 		ov7670_set(0x6b, 0x0a);
        ov7670_set(0x74, 0x10);
        ov7670_set(0x8d, 0x4f);
        ov7670_set(0x8e, 0);
        ov7670_set(0x8f, 0);
        ov7670_set(0x90, 0);
        ov7670_set(0x91, 0);
        ov7670_set(0x96, 0);
        ov7670_set(0x9a, 0);
        ov7670_set(0xb0, 0x84);
        ov7670_set(0xb1, 0x0c);
        ov7670_set(0xb2, 0x0e);
        ov7670_set(0xb3, 0x82);
        ov7670_set(0xb8, 0x0a);
        // More reserved magic, some of which tweaks white balance */
        ov7670_set(0x43, 0x0a);
        ov7670_set(0x44, 0xf0);
        ov7670_set(0x45, 0x34);
        ov7670_set(0x46, 0x58);
        ov7670_set(0x47, 0x28);
        ov7670_set(0x48, 0x3a);
        ov7670_set(0x59, 0x88);
        ov7670_set(0x5a, 0x88);
        ov7670_set(0x5b, 0x44);
        ov7670_set(0x5c, 0x67);
        ov7670_set(0x5d, 0x49);
        ov7670_set(0x5e, 0x0e);
        ov7670_set(0x6c, 0x0a);
        ov7670_set(0x6d, 0x55);
        ov7670_set(0x6e, 0x11);
        ov7670_set(0x6f, 0x9f);
        ov7670_set(0x6a, 0x40);
        ov7670_set(0x01, 0x40);
        ov7670_set(0x02, 0x60);
        // COLOR SETTING
        ov7670_set(0x4f, 0x80);
        ov7670_set(0x50, 0x80);
        ov7670_set(0x51, 0x00);
        ov7670_set(0x52, 0x22);
        ov7670_set(0x53, 0x5e);
        ov7670_set(0x54, 0x80);
        ov7670_set(0x56, 0x40);
        ov7670_set(0x58, 0x9e);
        ov7670_set(0x59, 0x88);
        ov7670_set(0x5a, 0x88);
        ov7670_set(0x5b, 0x44);
        ov7670_set(0x5c, 0x67);
        ov7670_set(0x5d, 0x49);
        ov7670_set(0x5e, 0x0e);
        ov7670_set(0x69, 0x00);
        ov7670_set(0x6a, 0x40);
        ov7670_set(0x6b, 0x0a);
        ov7670_set(0x6c, 0x0a);
        ov7670_set(0x6d, 0x55);
        ov7670_set(0x6e, 0x11);
        ov7670_set(0x6f, 0x9f);

        ov7670_set(0xb0, 0x84);
        ov7670_set(0x13, COM8_FASTAEC | COM8_AECSTEP | COM8_BFILT | COM8_AGC | COM8_AEC | COM8_AWB);
        // Matrix coefficients */
        ov7670_set(0x4f, 0x80);
        ov7670_set(0x50, 0x80);
        ov7670_set(0x51, 0);
        ov7670_set(0x52, 0x22);
        ov7670_set(0x53, 0x5e);
        ov7670_set(0x54, 0x80);
        ov7670_set(0x58, 0x9e);

        ov7670_set(0x41, 0x08);
        ov7670_set(0x3f, 0);
        ov7670_set(0x75, 0x05);
        ov7670_set(0x76, 0xe1);
        ov7670_set(0x4c, 0);
        ov7670_set(0x77, 0x01);
        ov7670_set(0x3d, 0xc3);
        ov7670_set(0x4b, 0x09);
        ov7670_set(0x41, 0x38);
        ov7670_set(0x56, 0x40);

        ov7670_set(0x34, 0x11);
        ov7670_set(0x3b, COM11_EXP | COM11_HZAUTO);
        ov7670_set(0xa4, 0x88);
        ov7670_set(0x96, 0);
        ov7670_set(0x97, 0x30);
        ov7670_set(0x98, 0x20);
        ov7670_set(0x99, 0x30);
        ov7670_set(0x9a, 0x84);
        ov7670_set(0x9b, 0x29);
        ov7670_set(0x9c, 0x03);
        ov7670_set(0x9d, 0x4c);
        ov7670_set(0x9e, 0x3f);
        ov7670_set(0x78, 0x04);
        // Extra-weird stuff. Some sort of multiplexor register */
        ov7670_set(0x79, 0x01);
        ov7670_set(0xc8, 0xf0);
        ov7670_set(0x79, 0x0f);
        ov7670_set(0xc8, 0x00);
        ov7670_set(0x79, 0x10);
        ov7670_set(0xc8, 0x7e);
        ov7670_set(0x79, 0x0a);
        ov7670_set(0xc8, 0x80);
        ov7670_set(0x79, 0x0b);
        ov7670_set(0xc8, 0x01);
        ov7670_set(0x79, 0x0c);
        ov7670_set(0xc8, 0x0f);
        ov7670_set(0x79, 0x0d);
        ov7670_set(0xc8, 0x20);
        ov7670_set(0x79, 0x09);
        ov7670_set(0xc8, 0x80);
        ov7670_set(0x79, 0x02);
        ov7670_set(0xc8, 0xc0);
        ov7670_set(0x79, 0x03);
        ov7670_set(0xc8, 0x40);
        ov7670_set(0x79, 0x05);
        ov7670_set(0xc8, 0x30);
        ov7670_set(0x79, 0x26);
        //			for (int i = 0; OV7670_init[i].reg <= OV7670_REG_LAST; i++) {
        //				ov7670_set(OV7670_init[i].reg, OV7670_init[i].value);
        //			}
        saturation(1);
        OV7670_set_size(OV7670_SIZE_DIV4);
        ov7670_set(REG_COM10, 0x02); // 0x02   VSYNC negative (http://nasulica.homelinux.org/?p=959)
        //check the input signals
        uint64_t us = time_us_64() + 1000000;
        while (!ST_PCLK && time_us_64() < us) {
        } /* wait for clock to go high */
        while (ST_PCLK && time_us_64() < us) {
        } /* wait for clock to go back low */
        if (time_us_64() > us) error("Timeout on camera PCLK signal");
        while (ST_HREF && time_us_64() < us) {
        } /* wait for a line to end */
        while (!ST_HREF && time_us_64() < us) {
        } /* wait for a line to end */
        if (time_us_64() > us) error("Timeout on camera HREF signal");
        while (ST_VSYNC && time_us_64() < us) {
        } /* wait for the old frame to end */
        while (!ST_VSYNC && time_us_64() < us) {
        } /* wait for a new frame to start */
        if (time_us_64() > us) error("Timeout on camera VSYNC signal");

        //			OV7670_test_pattern(OV7670_TEST_PATTERN_COLOR_BAR);
    } else if ((tp = checkstring(cmdline, (unsigned char *)"TEST"))) {
        getargs(&tp, 1, (unsigned char *)",");
        OV7670_test_pattern(getint(argv[0], 0, 3));
    } else if ((tp = checkstring(cmdline, (unsigned char *)"REGISTER"))) {
        getargs(&tp, 3, (unsigned char *)",");
        if (!XCLK) error("Camera not open");
        int a = getint(argv[0], 0, 255);
        int b = getint(argv[2], 0, 255);
        int c = readregister(a);
        ov7670_set(a, b);
        MMPrintString("Register &H");
        PIntH(a);
        MMPrintString(" was &H");
        PIntH(c);
        MMPrintString(" now &H");
        PIntH(b);
        PRet();

    } else if ((tp = checkstring(cmdline, (unsigned char *)"CHANGE"))) {
        int size = 0;
        unsigned char * cp = NULL;
        int totaldifference = 0, difference;
        if (!XCLK) error("Camera not open");
        getargs(&tp, 9, (unsigned char *)",");
        if (!(argc == 3 || argc == 5 || argc == 9)) error("Syntax");
        int xs = 0, ys = 0;
        if (!XCLK) error("Camera not open");
        int scale = 1;
        int64_t * aint;
        size = parseintegerarray(argv[0], &aint, 1, 1, NULL, true);
        cp = (unsigned char *)aint;
        // get the two variables
        MMFLOAT * outdiff = findvar(argv[2], V_FIND);
        if (!(g_vartbl[g_VarIndex].type & T_NBR)) error("Invalid variable");
        if (size < 160 * 120 / 8) error("Array too small");
        int picout = 0;
        if (argc >= 5) {
            scale = getint(argv[4], 1, HRes / 160);
            picout = 1;
            if (argc == 9) {
                xs = getint(argv[6], 0, HRes - 1);
                ys = getint(argv[8], 0, VRes - 1);
            }
        }
        for (int i = 0; OV7670_yuv[i].reg <= OV7670_REG_LAST; i++) {
            ov7670_set(OV7670_yuv[i].reg, OV7670_yuv[i].value);
        }
        char * buff = GetTempMemory(160 * 120 * 2);
        char * k = buff;
        c.rgb = 0;
        fileio_flash_write_begin();
        capture(buff);
        fileio_flash_write_end();
        char * linebuff = NULL;
        if (scale) linebuff = GetTempMemory(160 * 3);
        for (int y = ys; y < 120 * scale + ys; y += scale) {
            int kk = 0;
            for (int x = 0; x < 160; x++) {
                c.rgbbytes[1] = *k++;
                c.rgbbytes[0] = *k++;
                if (c.rgbbytes[1] > *cp)
                    difference = (c.rgbbytes[1] - *cp);
                else
                    difference = (*cp - c.rgbbytes[1]);
                totaldifference += difference;
                *cp++ = c.rgbbytes[1];
                if (picout) {
                    for (int r = 0; r < scale; r++) {
                        linebuff[kk++] = difference;
                        linebuff[kk++] = difference;
                        linebuff[kk++] = difference;
                    }
                }
            }
            if (picout) {
                for (int r = 0; r < scale; r++) {
                    if (y + r < VRes) {
                        int w = 160 * scale;
                        if (w > HRes - xs) w = HRes - xs;
                        DrawBuffer(xs, y + r, xs + w - 1, y + r, (unsigned char *)linebuff);
                    }
                }
            }
        }
        *outdiff = (MMFLOAT)totaldifference / (160.0 * 120.0 * 255.0) * 100.0;
    } else if ((tp = checkstring(cmdline, (unsigned char *)"CAPTURE"))) {
        getargs(&tp, 5, (unsigned char *)",");
        int xs = 0, ys = 0;
        if (!XCLK) error("Camera not open");
        int scale = 1;
        if (argc >= 1) {
            scale = getint(argv[0], 1, HRes / 160);
            if (argc == 5) {
                xs = getint(argv[2], 0, HRes - 1);
                ys = getint(argv[4], 0, VRes - 1);
            } else if (argc == 3)
                error("Syntax");
        }
        for (int i = 0; OV7670_rgb[i].reg <= OV7670_REG_LAST; i++) {
            ov7670_set(OV7670_rgb[i].reg, OV7670_rgb[i].value);
        }
        char * buff = GetTempMemory(160 * 120 * 2);
        c.rgb = 0;
        fileio_flash_write_begin();
        capture(buff);
        fileio_flash_write_end();
        char * linebuff = GetTempMemory(160 * 3 * scale);
        char * k = buff;
        for (int y = ys; y < 120 * scale + ys; y += scale) {
            int kk = 0;
            for (int x = 0; x < 160; x++) {
                c.rgbbytes[1] = *k++;
                c.rgbbytes[0] = *k++;
                for (int r = 0; r < scale; r++) {
                    linebuff[kk++] = (c.rgbbytes[0] & 0x1F) << 3;
                    linebuff[kk++] = (c.rgb & 0x07E0) >> 3;
                    linebuff[kk++] = (c.rgbbytes[1] & 0xF8);
                }
            }
            for (int r = 0; r < scale; r++) {
                if (y + r < VRes) {
                    int w = 160 * scale;
                    if (w > HRes - xs) w = HRes - xs;
                    DrawBuffer(xs, y + r, xs + w - 1, y + r, (unsigned char *)linebuff);
                }
            }
        }
    } else if (checkstring(cmdline, (unsigned char *)"CLOSE")) {
        cameraclose();
    } else
        error("Syntax");
}
#else
/* VGA variants have no CAMERA hardware. Stubs so core can call
 * cmd_camera / cameraclose unconditionally without a preprocessor
 * gate; the BASIC layer sees "no camera configured" instead of a
 * missing command. */
void cmd_camera(void) {
    error("Camera not supported on this display");
}
void cameraclose(void) {}
#endif
