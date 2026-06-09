/***********************************************************************************************************************
PicoMite MMBasic

I2C.c

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
/**
 * @file I2C.c
 * @author Geoff Graham, Peter Mather
 * @brief Source for I2C MMBasic commands
 */
/**
 * @cond
 * The following section will be excluded from the documentation.
 */

#include "MMBasic_Includes.h"
#include "hal/hal_i2c.h"
#include "hal/hal_i2c_keypad.h"
#include "hal/hal_time.h"

#include "Hardware_Includes.h"

extern void DrawBufferMEM(int x1, int y1, int x2, int y2, unsigned char * p);
extern void ReadBufferMEM(int x1, int y1, int x2, int y2, unsigned char * buff);
// Declare functions
void i2cEnable(unsigned char * p);
void i2cDisable(unsigned char * p);
void i2cSend(unsigned char * p);
void i2cSendSlave(unsigned char * p, int channel);
void i2cReceive(unsigned char * p);
void i2c_disable(void);
void i2c_enable(int bps);
void i2c_masterCommand(int timer, unsigned char * buff);
void i2cCheck(unsigned char * p);
void i2c2Enable(unsigned char * p);
void i2c2Disable(unsigned char * p);
void i2c2Send(unsigned char * p);
void i2c2Receive(unsigned char * p);
void i2c2_disable(void);
void i2c2_enable(int bps);
void i2c2_masterCommand(int timer, unsigned char * buff);
void i2c2Check(unsigned char * p);
static MMFLOAT * I2C_Rcvbuf_Float;         // pointer to the master receive buffer for a MMFLOAT
static int64_t * I2C_Rcvbuf_Int;           // pointer to the master receive buffer for an integer
static char * I2C_Rcvbuf_String;           // pointer to the master receive buffer for a string
static unsigned int I2C_Addr;              // I2C device address
static volatile unsigned int I2C_Sendlen;  // length of the master send buffer
static volatile unsigned int I2C_Rcvlen;   // length of the master receive buffer
static unsigned char I2C_Send_Buffer[256]; // I2C send buffer
bool I2C_enabled = false;                  // I2C enable marker
unsigned int I2C_Timeout;                  // master timeout value
volatile unsigned int I2C_Status;          // status flags
int mmI2Cvalue;
// value of MM.I2C
static MMFLOAT * I2C2_Rcvbuf_Float;        // pointer to the master receive buffer for a MMFLOAT
static int64_t * I2C2_Rcvbuf_Int;          // pointer to the master receive buffer for an integer
static char * I2C2_Rcvbuf_String;          // pointer to the master receive buffer for a string
static unsigned int I2C2_Addr;             // I2C device address
static volatile unsigned int I2C2_Sendlen; // length of the master send buffer
static volatile unsigned int I2C2_Rcvlen;  // length of the master receive buffer
//static unsigned char I2C_Send_Buffer[256];                                   // I2C send buffer
bool I2C2_enabled = false;         // I2C enable marker
unsigned int I2C2_Timeout;         // master timeout value
volatile unsigned int I2C2_Status; // status flags
//static char I2C_Rcv_Buffer[256];                                // I2C receive buffer
static unsigned int I2C_Slave_Addr; // slave address
char * I2C_Slave_Send_IntLine;      // pointer to the slave send interrupt line number
char * I2C_Slave_Receive_IntLine;   // pointer to the slave receive interrupt line number
//static char I2C2_Rcv_Buffer[256];                                // I2C receive buffer
char * I2C2_Slave_Send_IntLine;      // pointer to the slave send interrupt line number
char * I2C2_Slave_Receive_IntLine;   // pointer to the slave receive interrupt line number
static unsigned int I2C2_Slave_Addr; // slave address
bool noRTC = false, noI2C = false;
extern void SaveToBuffer(void);
extern void CompareToBuffer(void);
extern void DrawPixelMEM(int x1, int y1, int c);
extern void DrawRectangleMEM(int x1, int y1, int x2, int y2, int c);
extern void DrawBitmapMEM(int x1, int y1, int width, int height, int scale, int fc, int bc, unsigned char * bitmap);
void i2cSlave(unsigned char * p);
void i2c2Slave(unsigned char * p);
void i2cReceiveSlave(unsigned char * p, int channel);
const unsigned char nuninit[2] = {
    0xF0,
    0x55};
const unsigned char nuninit2[2] = {
    0xFB,
    0x0};
const unsigned char readcontroller[1] = {
    0};
const unsigned char nunid[1] = {
    0xFC};
const unsigned char nuncalib[1] = {
    0x20};
volatile uint8_t classic1 = false, nunchuck1 = false;
uint8_t nunbuff[10];
uint32_t swap32(uint32_t in) {
    in = __builtin_bswap32(in);
    return in;
}
volatile struct s_nunstruct nunstruct[6];
char * nunInterruptc[6] = {
    NULL};
bool nunfoundc[6] = {
    false};
unsigned char classicread = 0, nunchuckread = 0;
/*******************************************************************************************
							  I2C related commands in MMBasic
                              ===============================
These are the functions responsible for executing the I2C related commands in MMBasic
They are supported by utility functions that are grouped at the end of this file

********************************************************************************************/
/* Translate a hal_i2c_* master return into the BASIC-visible MM.I2C
 * status: 0 = ok, 1 = NACK / generic error, 2 = timeout. The HAL
 * returns the byte count (>= 0) on success and a negative errno on
 * failure (-3 = timeout, anything else negative = generic). */
static void I2C_SetStatusFromHal(int ret) {
    mmI2Cvalue = 0;
    if (ret < 0) mmI2Cvalue = (ret == -3) ? 2 : 1;
}

/* The system bus selects i2c0 or i2c1 by which one is locked to it. */
static int I2C_SystemBus(void) {
    return I2C1locked ? 1 : 0;
}

/* Keypad MCU register write. Used on PicoCalc by
 * drivers/i2c_picocalc_kbd/i2ckbd.c; on non-keypad ports nothing
 * calls it and the linker dead-code-eliminates the body. */
void I2C_Send_RegData(int i2caddr, int reg, char command) {
    I2C_Send_Buffer[0] = reg;
    I2C_Send_Buffer[1] = command;
    I2C_Sendlen = 2;
    I2C_Timeout = 1000;
    int ret = hal_i2c_master_write(I2C_SystemBus(), (uint8_t)i2caddr,
                                   (uint8_t *)I2C_Send_Buffer, I2C_Sendlen,
                                   0, I2C_Timeout * 1000);
    I2C_SetStatusFromHal(ret);
}
void I2C_Send_Command(char command) {
    int i2caddr = SSD1306_I2C_Addr;
    I2C_Send_Buffer[0] = 0;
    I2C_Send_Buffer[1] = command;
    I2C_Sendlen = 2;
    I2C_Timeout = 1000;
    int ret = hal_i2c_master_write(I2C_SystemBus(), (uint8_t)i2caddr,
                                   (uint8_t *)I2C_Send_Buffer, I2C_Sendlen,
                                   0, I2C_Timeout * 1000);
    I2C_SetStatusFromHal(ret);
}
void I2C_Send_Data(unsigned char * data, int n) {
    int i, i2caddr = SSD1306_I2C_Addr;
    I2C_Sendlen = n + 1;
    I2C_Send_Buffer[0] = 0x40;
    I2C_Timeout = 1000;
    for (i = 1; i <= n; i++) {
        I2C_Send_Buffer[i] = data[i - 1];
    }
    int ret = hal_i2c_master_write(I2C_SystemBus(), (uint8_t)i2caddr,
                                   (uint8_t *)I2C_Send_Buffer, I2C_Sendlen,
                                   0, I2C_Timeout * 1000);
    I2C_SetStatusFromHal(ret);
}
void ConfigDisplayI2C(unsigned char * p) {
    unsigned char DISPLAY_TYPE = 0;
    getargs(&p, 5, (unsigned char *)",");
    if (!(argc == 3 || argc == 5)) error("Argument count");
    if (checkstring(argv[0], (unsigned char *)"SSD1306I2C")) {
        DISPLAY_TYPE = SSD1306I2C;
    } else if (checkstring(argv[0], (unsigned char *)"SSD1306I2C32")) {
        DISPLAY_TYPE = SSD1306I2C32;
    } else
        error("Invalid display type");

    if (checkstring(argv[2], (unsigned char *)"L") || checkstring(argv[2], (unsigned char *)"LANDSCAPE"))
        Option.DISPLAY_ORIENTATION = LANDSCAPE;
    else if (checkstring(argv[2], (unsigned char *)"P") || checkstring(argv[2], (unsigned char *)"PORTRAIT"))
        Option.DISPLAY_ORIENTATION = PORTRAIT;
    else if (checkstring(argv[2], (unsigned char *)"RL") || checkstring(argv[2], (unsigned char *)"RLANDSCAPE"))
        Option.DISPLAY_ORIENTATION = RLANDSCAPE;
    else if (checkstring(argv[2], (unsigned char *)"RP") || checkstring(argv[2], (unsigned char *)"RPORTRAIT"))
        Option.DISPLAY_ORIENTATION = RPORTRAIT;
    else
        error("Orientation");
    Option.I2Coffset = 0;
    if (argc == 5) Option.I2Coffset = getint(argv[4], 0, 10);
    if (!(I2C0locked || I2C1locked)) error("SYSTEM I2C not configured");
    Option.Refresh = 1;
    Option.DISPLAY_TYPE = DISPLAY_TYPE;
}

void InitDisplayI2C(int InitOnly) {
    if (Option.DISPLAY_TYPE == 0 || Option.DISPLAY_TYPE > I2C_PANEL) return;
    //	I2Con();
    //	i2c_enable(display_details[Option.DISPLAY_TYPE].speed);
    DrawRectangle = DrawRectangleMEM;
    DrawBitmap = DrawBitmapMEM;
    DrawBuffer = DrawBufferMEM;
    ReadBuffer = ReadBufferMEM;
    DrawPixel = DrawPixelMEM;
    DrawBLITBuffer = DrawBufferMEM;
    ReadBLITBuffer = ReadBufferMEM;
    DisplayHRes = display_details[Option.DISPLAY_TYPE].horizontal;
    DisplayVRes = display_details[Option.DISPLAY_TYPE].vertical;
    I2C_Send_Command(0xAE); //DISPLAYOFF

    I2C_Send_Command(0xD5); //DISPLAYCLOCKDIV
    I2C_Send_Command(0xF0); //the suggested ratio &H80

    I2C_Send_Command(0xA8); //MULTIPLEX
    if (Option.DISPLAY_TYPE == SSD1306I2C)
        I2C_Send_Command(0x3F);
    else if (Option.DISPLAY_TYPE == SSD1306I2C32)
        I2C_Send_Command(0x1F);

    I2C_Send_Command(0xD3); //DISPLAYOFFSET
    I2C_Send_Command(0x0);  //no offset

    I2C_Send_Command(0x40); //STARTLINE

    I2C_Send_Command(0x8D); //CHARGEPUMP
    I2C_Send_Command(0x14);

    I2C_Send_Command(0x20); //MEMORYMODE
    I2C_Send_Command(0x00); //&H0 act like ks0108

    I2C_Send_Command(0xA1); //SEGREMAP OR 1
    I2C_Send_Command(0xC8); //COMSCANDEC

    I2C_Send_Command(0xDA); //COMPINS
    if (Option.DISPLAY_TYPE == SSD1306I2C)
        I2C_Send_Command(0x12);
    else if (Option.DISPLAY_TYPE == SSD1306I2C32)
        I2C_Send_Command(0x02);

    I2C_Send_Command(0x81); //SETCONTRAST
    I2C_Send_Command(0xCF);

    I2C_Send_Command(0xd9); //SETPRECHARGE
    I2C_Send_Command(0x22);

    I2C_Send_Command(0xDB); //VCOMDETECT
    I2C_Send_Command(0x20);

    I2C_Send_Command(0xA4); //DISPLAYALLON_RESUME
    I2C_Send_Command(0xA6); //NORMALDISPLAY
    I2C_Send_Command(0xAF); //DISPLAYON
    if (Option.DISPLAY_ORIENTATION & 1) {
        VRes = DisplayVRes;
        HRes = DisplayHRes;
    } else {
        VRes = DisplayHRes;
        HRes = DisplayVRes;
    }
    if (!InitOnly) {
        ResetDisplay();
        ClearScreen(0);
        Display_Refresh();
    }
}
/*  @endcond */

void cmd_i2c(void) {
    unsigned char * p; //, *pp;
    if (I2C0SDApin == 99 || I2C0SCLpin == 99) error("Pin not set for I2C");

    if ((p = checkstring(cmdline, (unsigned char *)"OPEN")) != NULL)
        i2cEnable(p);
    else if ((p = checkstring(cmdline, (unsigned char *)"CLOSE")) != NULL)
        i2cDisable(p);
    else if ((p = checkstring(cmdline, (unsigned char *)"WRITE")) != NULL) {
        if (I2C0SDApin == Option.SYSTEM_I2C_SDA) I2C_Timeout = 1000;
        i2cSend(p);
        if (I2C0SDApin == Option.SYSTEM_I2C_SDA) I2C_Timeout = SystemI2CTimeout;
    } else if ((p = checkstring(cmdline, (unsigned char *)"READ")) != NULL) {
        if (I2C0SDApin == Option.SYSTEM_I2C_SDA) I2C_Timeout = 1000;
        i2cReceive(p);
        if (I2C0SDApin == Option.SYSTEM_I2C_SDA) I2C_Timeout = SystemI2CTimeout;
    } else if ((p = checkstring(cmdline, (unsigned char *)"CHECK")) != NULL)
        i2cCheck(p);
    else if ((p = checkstring(cmdline, (unsigned char *)"SLAVE OPEN")) != NULL)
        i2cSlave(p);
    else if ((p = checkstring(cmdline, (unsigned char *)"SLAVE READ")) != NULL)
        i2cReceiveSlave(p, 0);
    else if ((p = checkstring(cmdline, (unsigned char *)"SLAVE WRITE")) != NULL)
        i2cSendSlave(p, 0);
    else if ((p = checkstring(cmdline, (unsigned char *)"SLAVE CLOSE")) != NULL)
        i2cDisable(p);
    else
        error("Unknown command");
}
void cmd_i2c2(void) {
    unsigned char * p; //, *pp;
    if (I2C1SDApin == 99 || I2C1SCLpin == 99) error("Pin not set for I2C2");

    if ((p = checkstring(cmdline, (unsigned char *)"OPEN")) != NULL)
        i2c2Enable(p);
    else if ((p = checkstring(cmdline, (unsigned char *)"CLOSE")) != NULL)
        i2c2Disable(p);
    else if ((p = checkstring(cmdline, (unsigned char *)"WRITE")) != NULL) {
        if (I2C1SDApin == Option.SYSTEM_I2C_SDA) I2C2_Timeout = 1000;
        i2c2Send(p);
        if (I2C1SDApin == Option.SYSTEM_I2C_SDA) I2C2_Timeout = SystemI2CTimeout;
    } else if ((p = checkstring(cmdline, (unsigned char *)"READ")) != NULL) {
        if (I2C1SDApin == Option.SYSTEM_I2C_SDA) I2C2_Timeout = 1000;
        i2c2Receive(p);
        if (I2C1SDApin == Option.SYSTEM_I2C_SDA) I2C2_Timeout = SystemI2CTimeout;
    } else if ((p = checkstring(cmdline, (unsigned char *)"CHECK")) != NULL)
        i2c2Check(p);
    else if ((p = checkstring(cmdline, (unsigned char *)"SLAVE OPEN")) != NULL)
        i2c2Slave(p);
    else if ((p = checkstring(cmdline, (unsigned char *)"SLAVE READ")) != NULL)
        i2cReceiveSlave(p, 1);
    else if ((p = checkstring(cmdline, (unsigned char *)"SLAVE WRITE")) != NULL)
        i2cSendSlave(p, 1);
    else if ((p = checkstring(cmdline, (unsigned char *)"SLAVE CLOSE")) != NULL)
        i2c2Disable(p);
    else
        error("Unknown command");
}
/*
 * @cond
 * The following section will be excluded from the documentation.
 */

void i2cSlave(unsigned char * p) {
    int addr;
    getargs(&p, 5, (unsigned char *)",");
    if (argc != 5) error("Argument count");
    if (I2C_Status & I2C_Status_Slave) error("Slave already open");
    addr = getinteger(argv[0]);
    ExtCfg(I2C0SDApin, EXT_COM_RESERVED, 0);
    ExtCfg(I2C0SCLpin, EXT_COM_RESERVED, 0);
    I2C_Slave_Addr = addr;
    I2C_Slave_Send_IntLine = (char *)GetIntAddress(argv[2]);    // get the interrupt routine's location
    I2C_Slave_Receive_IntLine = (char *)GetIntAddress(argv[4]); // get the interrupt routine's location
    InterruptUsed = true;
    I2C_Status = I2C_Status_Slave;
    hal_i2c_slave_enable(0, (uint8_t)I2C_Slave_Addr);
}
void i2c2Slave(unsigned char * p) {
    int addr;
    getargs(&p, 5, (unsigned char *)",");
    if (argc != 5) error("Argument count");
    if (I2C2_Status & I2C_Status_Slave) error("Slave already open");
    addr = getinteger(argv[0]);
    ExtCfg(I2C1SDApin, EXT_COM_RESERVED, 0);
    ExtCfg(I2C1SCLpin, EXT_COM_RESERVED, 0);
    I2C2_Slave_Addr = addr;
    I2C2_Slave_Send_IntLine = (char *)GetIntAddress(argv[2]);    // get the interrupt routine's location
    I2C2_Slave_Receive_IntLine = (char *)GetIntAddress(argv[4]); // get the interrupt routine's location
    InterruptUsed = true;
    I2C2_Status = I2C_Status_Slave;
    hal_i2c_slave_enable(1, (uint8_t)I2C2_Slave_Addr);
}
int DoRtcI2C(int addr, unsigned char * buff) {
    if (I2C0locked) {
        I2C_Addr = addr; // address of the device
        i2c_masterCommand(1, buff);
    } else {
        I2C2_Addr = addr; // address of the device
        i2c2_masterCommand(1, buff);
    }
    return !mmI2Cvalue;
}
void CheckI2CKeyboard(int noerror, int read) {
    uint16_t buff;
    //	int readover=0;
    static int ctrlheld = 0;
    //	while(readover==0){
    if (I2C0locked) {
        if (read == 0) {
            I2C_Sendlen = 1; // send one byte
            I2C_Rcvlen = 0;
            I2C_Status = 0;
            I2C_Send_Buffer[0] = 9; // the first register to read
            if (!(DoRtcI2C(0x1F, NULL))) goto i2c_error_exit;
        } else {
            I2C_Rcvbuf_String = (char *)&buff; // we want a string of bytes
            I2C_Rcvbuf_Float = NULL;
            I2C_Rcvbuf_Int = NULL;
            I2C_Rcvlen = 2; // get 7 bytes
            I2C_Sendlen = 0;
            if (!DoRtcI2C(0x1F, (unsigned char *)&buff)) goto i2c_error_exit;
        }
    } else {
        I2C2_Sendlen = 1; // send one byte
        I2C2_Rcvlen = 0;
        I2C2_Status = 0;
        I2C_Send_Buffer[0] = 9; // the first register to read
        if (!(DoRtcI2C(0x1F, NULL))) goto i2c_error_exit;
        I2C2_Rcvbuf_String = (char *)&buff; // we want a string of bytes
        I2C2_Rcvbuf_Float = NULL;
        I2C2_Rcvbuf_Int = NULL;
        I2C2_Rcvlen = 2; // get 7 bytes
        I2C2_Sendlen = 0;
        if (!DoRtcI2C(0x1F, (unsigned char *)&buff)) goto i2c_error_exit;
    }
    uSec(1000);
    if (buff) {
        /* Per-board scancode translator: real impl is the PicoCalc keymap
     * (drivers/i2c_picocalc_kbd/i2c_keypad_real.c); stub impl is the
     * legacy generic-I²C-keyboard keymap. Returns -1 to skip
     * (modifier / state change / non-press); otherwise the cooked
     * character to enqueue. */
        int c = hal_i2c_keypad_translate(buff, &ctrlheld);
        if (c >= 0) {
            if (c >= 'a' && c <= 'z' && ctrlheld) c = c - 'a' + 1;
            if (c == BreakKey) {                     // if the user wants to stop the progran
                MMAbort = true;                      // set the flag for the interpreter to see
                ConsoleRxBufHead = ConsoleRxBufTail; // empty the buffer
            } else {
                ConsoleRxBuf[ConsoleRxBufHead] = c; // store the byte in the ring buffer
                if (ConsoleRxBuf[ConsoleRxBufHead] == keyselect && KeyInterrupt != NULL) {
                    Keycomplete = true;
                } else {
                    ConsoleRxBufHead = (ConsoleRxBufHead + 1) % CONSOLE_RX_BUF_SIZE;
                    if (ConsoleRxBufHead == ConsoleRxBufTail) {
                        ConsoleRxBufTail = (ConsoleRxBufTail + 1) % CONSOLE_RX_BUF_SIZE;
                    }
                }
            }
        }
    }
    return;
i2c_error_exit:
    if (noerror) {
        noI2C = 1;
        return;
    }
    if (CurrentLinePtr) error("I2C Keyboard not responding");
    if (Option.KeyboardConfig == CONFIG_I2C) {
        MMPrintString("I2C Keyboard not responding");
        MMPrintString("\r\n");
    }
}

void RtcGetTime(int noerror) {
    char * buff = GetTempMemory(STRINGSIZE); // Received data is stored here
    int DS1307;
    clocktimer = (1000 * 60 * 60);
    if (I2C0locked) {
        I2C_Sendlen = 1; // send one byte
        I2C_Rcvlen = 0;
        I2C_Status = 0;
        I2C_Send_Buffer[0] = 0; // the first register to read
        if (!(DS1307 = DoRtcI2C(0x68, NULL))) {
            I2C_Send_Buffer[0] = 2; // the first register is different for the PCF8563
            if (!DoRtcI2C(0x51, NULL)) goto error_exit;
        }
        I2C_Rcvbuf_String = buff; // we want a string of bytes
        I2C_Rcvbuf_Float = NULL;
        I2C_Rcvbuf_Int = NULL;
        I2C_Rcvlen = 7; // get 7 bytes
        I2C_Sendlen = 0;
        if (!DoRtcI2C(DS1307 ? 0x68 : 0x51, (unsigned char *)buff)) goto error_exit;
    } else {
        I2C2_Sendlen = 1; // send one byte
        I2C2_Rcvlen = 0;
        I2C2_Status = 0;
        I2C_Send_Buffer[0] = 0; // the first register to read
        if (!(DS1307 = DoRtcI2C(0x68, NULL))) {
            I2C_Send_Buffer[0] = 2; // the first register is different for the PCF8563
            if (!DoRtcI2C(0x51, NULL)) goto error_exit;
        }
        I2C2_Rcvbuf_String = buff; // we want a string of bytes
        I2C2_Rcvbuf_Float = NULL;
        I2C2_Rcvbuf_Int = NULL;
        I2C2_Rcvlen = 7; // get 7 bytes
        I2C2_Sendlen = 0;
        if (!DoRtcI2C(DS1307 ? 0x68 : 0x51, (unsigned char *)buff)) goto error_exit;
    }
    //    mT4IntEnable(0);
    int year, month, day, hour, minute, second;
    second = ((buff[0] & 0x7f) >> 4) * 10 + (buff[0] & 0x0f);
    minute = ((buff[1] & 0x7f) >> 4) * 10 + (buff[1] & 0x0f);
    hour = ((buff[2] & 0x3f) >> 4) * 10 + (buff[2] & 0x0f);
    day = ((buff[DS1307 ? 4 : 3] & 0x3f) >> 4) * 10 + (buff[DS1307 ? 4 : 3] & 0x0f);
    month = ((buff[5] & 0x1f) >> 4) * 10 + (buff[5] & 0x0f);
    year = (buff[6] >> 4) * 10 + (buff[6] & 0x0f) + 2000;
    //    mT4IntEnable(1);
    TimeOffsetToUptime = get_epoch(year, month, day, hour, minute, second) - hal_time_us_64() / 1000000;
    return;

error_exit:
    if (noerror) {
        noRTC = 1;
        return;
    }
    if (CurrentLinePtr) error("RTC not responding");
    if (Option.RTC) {
        MMPrintString("RTC not responding");
        MMPrintString("\r\n");
    }
}
// universal function to send/receive data to/from the RTC
// addr is the I2C address WITHOUT the read/write bit
char CvtToBCD(unsigned char * p, int min, int max) {
    int64_t t;
    t = getint(p, min, max) % 100;
    return ((t / 10) << 4) | (t % 10);
}

char CvtCharsToBCD(unsigned char * p, int min, int max) {
    int t;
    t = (p[0] - '0') * 10 + (p[1] - '0');
    //    dp("|%c|  |%c|  %d   %d   %d", p[0], p[1], t, min, max);
    if (!isdigit(p[0]) || !isdigit(p[1]) || t < min || t > max) error("Date/time format");
    return ((t / 10) << 4) | (t % 10);
}

/*  @endcond */
void MIPS16 cmd_rtc(void) {
    char buff[7]; // Received data is stored here
    int DS1307;
    unsigned char * p;
    void * ptr = NULL;
    if (!(I2C0locked || I2C1locked)) error("SYSTEM I2C not configured");
    if (checkstring(cmdline, (unsigned char *)"GETTIME")) {
        int repeat = 5;
        noRTC = 0;
        while (1) {
            while (!(classicread == 0 && nunchuckread == 0)) {
                routinechecks();
            }
            RtcGetTime(1);
            if (noRTC == 0) break;
            repeat--;
            if (!repeat) break;
        }
        if (noRTC) {
            if (CurrentLinePtr) error("RTC not responding");
            if (Option.RTC) {
                MMPrintString("RTC not responding");
                MMPrintString("\r\n");
            }
        }

        return;
    }
    if ((p = checkstring(cmdline, (unsigned char *)"SETTIME")) != NULL) {
        int Fulldate = 0;
        getargs(&p, 11, (unsigned char *)",");
        if (I2C0locked) {
            if (argc == 1) {
                // single argument - assume the data is in DATETIME2 format used by GUI FORMATBOX
                p = getCstring(argv[0]);
                if (!(p[2] == '/' || p[2] == '-') || !(p[11] == ':' || p[13] == ':')) error("Date/time format");
                if (p[13] == ':') Fulldate = 2;
                if (p[14 + Fulldate] == ':')
                    I2C_Send_Buffer[1] = CvtCharsToBCD(p + 15 + Fulldate, 0, 59); // seconds
                else
                    I2C_Send_Buffer[1] = 0;                                   // seconds defaults to zero
                I2C_Send_Buffer[2] = CvtCharsToBCD(p + 12 + Fulldate, 0, 59); // minutes
                I2C_Send_Buffer[3] = CvtCharsToBCD(p + 9 + Fulldate, 0, 23);  // hour
                I2C_Send_Buffer[5] = CvtCharsToBCD(p, 1, 31);                 // day
                I2C_Send_Buffer[6] = CvtCharsToBCD(p + 3, 1, 12);             // month
                I2C_Send_Buffer[7] = CvtCharsToBCD(p + 6 + Fulldate, 0, 99);  // year
            } else {
                // multiple arguments - data should be in the original yy, mm, dd, etc format
                if (argc != 11) error("Argument count");
                I2C_Send_Buffer[1] = CvtToBCD(argv[10], 0, 59);  // seconds
                I2C_Send_Buffer[2] = CvtToBCD(argv[8], 0, 59);   // minutes
                I2C_Send_Buffer[3] = CvtToBCD(argv[6], 0, 23);   // hour
                I2C_Send_Buffer[5] = CvtToBCD(argv[4], 1, 31);   // day
                I2C_Send_Buffer[6] = CvtToBCD(argv[2], 1, 12);   // month
                I2C_Send_Buffer[7] = CvtToBCD(argv[0], 0, 2099); // year
            }
            I2C_Send_Buffer[0] = 0; // turn off the square wave
            I2C_Send_Buffer[4] = 1;
            I2C_Rcvlen = 0;
            I2C_Sendlen = 9; // send 7 bytes
            if (!DoRtcI2C(0x68, NULL)) {
                I2C_Send_Buffer[9] = I2C_Send_Buffer[7]; // year
                I2C_Send_Buffer[8] = I2C_Send_Buffer[6]; // month
                I2C_Send_Buffer[7] = 1;
                I2C_Send_Buffer[6] = I2C_Send_Buffer[5];                          // day
                I2C_Send_Buffer[5] = I2C_Send_Buffer[3];                          // hour
                I2C_Send_Buffer[4] = I2C_Send_Buffer[2];                          // minutes
                I2C_Send_Buffer[3] = I2C_Send_Buffer[1];                          // seconds
                I2C_Send_Buffer[0] = I2C_Send_Buffer[1] = I2C_Send_Buffer[2] = 0; // set the register pointer to the first register then zero the first two registers
                I2C_Sendlen = 10;                                                 // send 10 bytes
                if (!DoRtcI2C(0x51, NULL)) error("RTC not responding");
            }
        } else {
            if (argc == 1) {
                // single argument - assume the data is in DATETIME2 format used by GUI FORMATBOX
                p = getCstring(argv[0]);
                if (!(p[2] == '/' || p[2] == '-') || !(p[11] == ':' || p[13] == ':')) error("Date/time format");
                if (p[13] == ':') Fulldate = 2;
                if (p[14 + Fulldate] == ':')
                    I2C_Send_Buffer[1] = CvtCharsToBCD(p + 15 + Fulldate, 0, 59); // seconds
                else
                    I2C_Send_Buffer[1] = 0;                                   // seconds defaults to zero
                I2C_Send_Buffer[2] = CvtCharsToBCD(p + 12 + Fulldate, 0, 59); // minutes
                I2C_Send_Buffer[3] = CvtCharsToBCD(p + 9 + Fulldate, 0, 23);  // hour
                I2C_Send_Buffer[5] = CvtCharsToBCD(p, 1, 31);                 // day
                I2C_Send_Buffer[6] = CvtCharsToBCD(p + 3, 1, 12);             // month
                I2C_Send_Buffer[7] = CvtCharsToBCD(p + 6 + Fulldate, 0, 99);  // year
            } else {
                // multiple arguments - data should be in the original yy, mm, dd, etc format
                if (argc != 11) error("Argument count");
                I2C_Send_Buffer[1] = CvtToBCD(argv[10], 0, 59);  // seconds
                I2C_Send_Buffer[2] = CvtToBCD(argv[8], 0, 59);   // minutes
                I2C_Send_Buffer[3] = CvtToBCD(argv[6], 0, 23);   // hour
                I2C_Send_Buffer[5] = CvtToBCD(argv[4], 1, 31);   // day
                I2C_Send_Buffer[6] = CvtToBCD(argv[2], 1, 12);   // month
                I2C_Send_Buffer[7] = CvtToBCD(argv[0], 0, 2099); // year
            }
            I2C_Send_Buffer[0] = 0; // turn off the square wave
            I2C_Send_Buffer[4] = 1;
            I2C2_Rcvlen = 0;
            I2C2_Sendlen = 9; // send 7 bytes
            if (!DoRtcI2C(0x68, NULL)) {
                I2C_Send_Buffer[9] = I2C_Send_Buffer[7]; // year
                I2C_Send_Buffer[8] = I2C_Send_Buffer[6]; // month
                I2C_Send_Buffer[7] = 1;
                I2C_Send_Buffer[6] = I2C_Send_Buffer[5];                          // day
                I2C_Send_Buffer[5] = I2C_Send_Buffer[3];                          // hour
                I2C_Send_Buffer[4] = I2C_Send_Buffer[2];                          // minutes
                I2C_Send_Buffer[3] = I2C_Send_Buffer[1];                          // seconds
                I2C_Send_Buffer[0] = I2C_Send_Buffer[1] = I2C_Send_Buffer[2] = 0; // set the register pointer to the first register then zero the first two registers
                I2C2_Sendlen = 10;                                                // send 10 bytes
                if (!DoRtcI2C(0x51, NULL)) error("RTC not responding");
            }
        }
        RtcGetTime(0);
    } else if ((p = checkstring(cmdline, (unsigned char *)"GETREG")) != NULL) {
        getargs(&p, 3, (unsigned char *)",");
        if (argc != 3) error("Argument count");
        if (I2C0locked) {
            I2C_Sendlen = 1; // send one byte
            I2C_Rcvlen = 0;
            *I2C_Send_Buffer = getint(argv[0], 0, 255); // the register to read
        } else {
            I2C2_Sendlen = 1; // send one byte
            I2C2_Rcvlen = 0;
            *I2C_Send_Buffer = getint(argv[0], 0, 255); // the register to read
        }
        ptr = findvar(argv[2], V_FIND);
        if (g_vartbl[g_VarIndex].type & T_CONST) error("Cannot change a constant");
        if (g_vartbl[g_VarIndex].type & T_STR) error("Invalid variable");

        if (!(DS1307 = DoRtcI2C(0x68, NULL))) {
            if (!DoRtcI2C(0x51, NULL)) error("RTC not responding");
        }
        if (I2C0locked) {
            I2C_Rcvbuf_String = buff; // we want a string of bytes
            I2C_Rcvbuf_Float = NULL;
            I2C_Rcvbuf_Int = NULL;
            I2C_Rcvlen = 1; // get 1 byte
            I2C_Sendlen = 0;
        } else {
            I2C2_Rcvbuf_String = buff; // we want a string of bytes
            I2C2_Rcvbuf_Float = NULL;
            I2C2_Rcvbuf_Int = NULL;
            I2C2_Rcvlen = 1; // get 1 byte
            I2C2_Sendlen = 0;
        }
        if (!DoRtcI2C(DS1307 ? 0x68 : 0x51, (unsigned char *)buff)) error("RTC not responding1");
        if (g_vartbl[g_VarIndex].type & T_NBR)
            *(MMFLOAT *)ptr = buff[0];
        else
            *(int64_t *)ptr = buff[0];
    } else if ((p = checkstring(cmdline, (unsigned char *)"SETREG")) != NULL) {
        getargs(&p, 3, (unsigned char *)",");
        if (argc != 3) error("Argument count");
        if (I2C0locked) {
            I2C_Rcvlen = 0;
            I2C_Send_Buffer[0] = getint(argv[0], 0, 255); // set the register pointer
            I2C_Send_Buffer[1] = getint(argv[2], 0, 255); // and the data to be written
            I2C_Sendlen = 2;                              // send 2 bytes
        } else {
            I2C2_Rcvlen = 0;
            I2C_Send_Buffer[0] = getint(argv[0], 0, 255); // set the register pointer
            I2C_Send_Buffer[1] = getint(argv[2], 0, 255); // and the data to be written
            I2C2_Sendlen = 2;                             // send 2 bytes
        }
        if (!DoRtcI2C(0x68, NULL)) {
            if (!DoRtcI2C(0x51, NULL)) error("RTC not responding");
        }
    } else
        error("Unknown command");
}
/*
 * @cond
 * The following section will be excluded from the documentation.
 */

// enable the I2C1 module - master mode
void i2cEnable(unsigned char * p) {
    int speed, timeout;
    getargs(&p, 3, (unsigned char *)",");
    if (argc != 3) error("Invalid syntax");
    speed = getinteger(argv[0]);
    if (!(speed == 100 || speed == 400 || speed == 1000)) error("Valid speeds 100, 400, 1000");
    timeout = getinteger(argv[2]);
    if (timeout < 0 || (timeout > 0 && timeout < 100)) error("Number out of bounds");
    if (I2C_enabled || I2C_Status & I2C_Status_Slave) error("I2C already OPEN");
    I2C_Timeout = timeout;
    i2c_enable(speed);
}
// enable the I2C1 module - master mode
void i2c2Enable(unsigned char * p) {
    int speed, timeout;
    getargs(&p, 3, (unsigned char *)",");
    if (argc != 3) error("Invalid syntax");
    speed = getinteger(argv[0]);
    if (!(speed == 100 || speed == 400 || speed == 1000)) error("Valid speeds 100, 400, 1000");
    timeout = getinteger(argv[2]);
    if (timeout < 0 || (timeout > 0 && timeout < 100)) error("Number out of bounds");
    if (I2C2_enabled || I2C2_Status & I2C_Status_Slave) error("I2C already OPEN");
    I2C2_Timeout = timeout;
    i2c2_enable(speed);
}

// disable the I2C1 module - master mode
void i2cDisable(unsigned char * p) {
    if (!I2C0locked)
        i2c_disable();
    else
        error("Allocated to System I2C");
}

// disable the I2C1 module - master mode
void i2c2Disable(unsigned char * p) {
    if (!I2C1locked)
        i2c2_disable();
    else
        error("Allocated to System I2C");
}

// send data to an I2C slave - master mode
void i2cSend(unsigned char * p) {
    int addr, i2c_options, sendlen, i;
    void * ptr = NULL;
    unsigned char * cptr = NULL;

    getargs(&p, 99, (unsigned char *)",");
    if (!(argc & 0x01) || (argc < 7)) error("Invalid syntax");
    if (!I2C_enabled) error("I2C not open");
    addr = getinteger(argv[0]);
    i2c_options = getinteger(argv[2]);
    if (i2c_options < 0 || i2c_options > 3) error("Number out of bounds");
    I2C_Status = 0;
    if (i2c_options & 0x01) I2C_Status = I2C_Status_BusHold;
    I2C_Addr = addr;
    sendlen = getint(argv[4], 1, 256);

    if (sendlen == 1 || argc > 7) { // numeric expressions for data
        if (sendlen != ((argc - 5) >> 1)) error("Incorrect argument count");
        for (i = 0; i < sendlen; i++) {
            I2C_Send_Buffer[i] = getinteger(argv[i + i + 6]);
        }
    } else { // an array of MMFLOAT, integer or a string
        ptr = findvar(argv[6], V_NOFIND_NULL | V_EMPTY_OK);
        if (ptr == NULL) error("Invalid variable");
        if ((g_vartbl[g_VarIndex].type & T_STR) && g_vartbl[g_VarIndex].dims[0] == 0) { // string
            if (sendlen > 255) error("Number out of bounds");
            cptr = (unsigned char *)ptr;
            cptr++; // skip the length byte in a MMBasic string
            for (i = 0; i < sendlen; i++) {
                I2C_Send_Buffer[i] = (int)(*(cptr + i));
            }
        } else if ((g_vartbl[g_VarIndex].type & T_NBR) && g_vartbl[g_VarIndex].dims[0] > 0 && g_vartbl[g_VarIndex].dims[1] == 0) { // numeric array
            if ((((MMFLOAT *)ptr - g_vartbl[g_VarIndex].val.fa) + sendlen) > (g_vartbl[g_VarIndex].dims[0] + 1 - g_OptionBase)) {
                error("Insufficient data");
            } else {
                for (i = 0; i < sendlen; i++) {
                    I2C_Send_Buffer[i] = (int)(*((MMFLOAT *)ptr + i));
                }
            }
        } else if ((g_vartbl[g_VarIndex].type & T_INT) && g_vartbl[g_VarIndex].dims[0] > 0 && g_vartbl[g_VarIndex].dims[1] == 0) { // integer array
            if ((((int64_t *)ptr - g_vartbl[g_VarIndex].val.ia) + sendlen) > (g_vartbl[g_VarIndex].dims[0] + 1 - g_OptionBase)) {
                error("Insufficient data");
            } else {
                for (i = 0; i < sendlen; i++) {
                    I2C_Send_Buffer[i] = (int)(*((int64_t *)ptr + i));
                }
            }
        } else
            error("Invalid variable");
    }
    I2C_Sendlen = sendlen;
    I2C_Rcvlen = 0;

    i2c_masterCommand(1, NULL);
}
// send data to an I2C slave - master mode
void i2cSendSlave(unsigned char * p, int channel) {
    int sendlen, i;
    void * ptr = NULL;
    unsigned char * cptr = NULL;
    getargs(&p, 99, (unsigned char *)",");
    if (!(argc >= 3)) error("Invalid syntax");
    if (!((I2C_Status & I2C_Status_Slave && channel == 0) || (I2C2_Status & I2C_Status_Slave && channel == 1))) error("I2C slave not open");
    unsigned char * bbuff;
    if (channel == 0) {
        bbuff = I2C_Send_Buffer;
    } else {
        bbuff = I2C_Send_Buffer;
    }
    sendlen = getinteger(argv[0]);
    if (sendlen < 1 || sendlen > 255) error("Number out of bounds");

    if (sendlen == 1 || argc > 3) { // numeric expressions for data
        if (sendlen != ((argc - 1) >> 1)) error("Incorrect argument count");
        for (i = 0; i < sendlen; i++) {
            bbuff[i] = getinteger(argv[i + i + 2]);
        }
    } else { // an array of MMFLOAT, integer or a string
        ptr = findvar(argv[2], V_NOFIND_NULL | V_EMPTY_OK);
        if (ptr == NULL) error("Invalid variable");
        if ((g_vartbl[g_VarIndex].type & T_STR) && g_vartbl[g_VarIndex].dims[0] == 0) { // string
            cptr = (unsigned char *)ptr;
            cptr++; // skip the length byte in a MMBasic string
            for (i = 0; i < sendlen; i++) {
                bbuff[i] = (int)(*(cptr + i));
            }
        } else if ((g_vartbl[g_VarIndex].type & T_NBR) && g_vartbl[g_VarIndex].dims[0] > 0 && g_vartbl[g_VarIndex].dims[1] == 0) { // numeric array
            if ((((MMFLOAT *)ptr - g_vartbl[g_VarIndex].val.fa) + sendlen) > (g_vartbl[g_VarIndex].dims[0] + 1 - g_OptionBase)) {
                error("Insufficient data");
            } else {
                for (i = 0; i < sendlen; i++) {
                    bbuff[i] = (int)(*((MMFLOAT *)ptr + i));
                }
            }
        } else if ((g_vartbl[g_VarIndex].type & T_INT) && g_vartbl[g_VarIndex].dims[0] > 0 && g_vartbl[g_VarIndex].dims[1] == 0) { // integer array
            if ((((int64_t *)ptr - g_vartbl[g_VarIndex].val.ia) + sendlen) > (g_vartbl[g_VarIndex].dims[0] + 1 - g_OptionBase)) {
                error("Insufficient data");
            } else {
                for (i = 0; i < sendlen; i++) {
                    bbuff[i] = (int)(*((int64_t *)ptr + i));
                }
            }
        } else
            error("Invalid variable");
    }
    hal_i2c_slave_send(channel, bbuff, sendlen);
}
// send data to an I2C slave - master mode
void i2c2Send(unsigned char * p) {
    int addr, i2c2_options, sendlen, i;
    void * ptr = NULL;
    unsigned char * cptr = NULL;

    getargs(&p, 99, (unsigned char *)",");
    if (!(argc & 0x01) || (argc < 7)) error("Invalid syntax");
    if (!I2C2_enabled) error("I2C not open");
    addr = getinteger(argv[0]);
    i2c2_options = getinteger(argv[2]);
    if (i2c2_options < 0 || i2c2_options > 3) error("Number out of bounds");
    I2C2_Status = 0;
    if (i2c2_options & 0x01) I2C2_Status = I2C_Status_BusHold;
    I2C2_Addr = addr;
    sendlen = getint(argv[4], 1, 256);

    if (sendlen == 1 || argc > 7) { // numeric expressions for data
        if (sendlen != ((argc - 5) >> 1)) error("Incorrect argument count");
        for (i = 0; i < sendlen; i++) {
            I2C_Send_Buffer[i] = getinteger(argv[i + i + 6]);
        }
    } else { // an array of MMFLOAT, integer or a string
        ptr = findvar(argv[6], V_NOFIND_NULL | V_EMPTY_OK);
        if (ptr == NULL) error("Invalid variable");
        if ((g_vartbl[g_VarIndex].type & T_STR) && g_vartbl[g_VarIndex].dims[0] == 0) { // string
            if (sendlen > 255) error("Number out of bounds");
            cptr = (unsigned char *)ptr;
            cptr++; // skip the length byte in a MMBasic string
            for (i = 0; i < sendlen; i++) {
                I2C_Send_Buffer[i] = (int)(*(cptr + i));
            }
        } else if ((g_vartbl[g_VarIndex].type & T_NBR) && g_vartbl[g_VarIndex].dims[0] > 0 && g_vartbl[g_VarIndex].dims[1] == 0) { // numeric array
            if ((((MMFLOAT *)ptr - g_vartbl[g_VarIndex].val.fa) + sendlen) > (g_vartbl[g_VarIndex].dims[0] + 1 - g_OptionBase)) {
                error("Insufficient data");
            } else {
                for (i = 0; i < sendlen; i++) {
                    I2C_Send_Buffer[i] = (int)(*((MMFLOAT *)ptr + i));
                }
            }
        } else if ((g_vartbl[g_VarIndex].type & T_INT) && g_vartbl[g_VarIndex].dims[0] > 0 && g_vartbl[g_VarIndex].dims[1] == 0) { // integer array
            if ((((int64_t *)ptr - g_vartbl[g_VarIndex].val.ia) + sendlen) > (g_vartbl[g_VarIndex].dims[0] + 1 - g_OptionBase)) {
                error("Insufficient data");
            } else {
                for (i = 0; i < sendlen; i++) {
                    I2C_Send_Buffer[i] = (int)(*((int64_t *)ptr + i));
                }
            }
        } else
            error("Invalid variable");
    }
    I2C2_Sendlen = sendlen;
    I2C2_Rcvlen = 0;

    i2c2_masterCommand(1, NULL);
}

void i2cCheck(unsigned char * p) {
    int addr;
    uint8_t rxdata;
    getargs(&p, 1, (unsigned char *)",");
    if (!I2C_enabled) error("I2C not open");
    addr = getinteger(argv[0]);
    if (addr < 0 || addr > 0x7F) error("Invalid I2C address");
    int ret = hal_i2c_master_read(0, (uint8_t)addr, &rxdata, 1, 0, 1000);
    I2C_SetStatusFromHal(ret);
}
void i2c2Check(unsigned char * p) {
    int addr;
    uint8_t rxdata;
    getargs(&p, 1, (unsigned char *)",");
    if (!I2C2_enabled) error("I2C not open");
    addr = getinteger(argv[0]);
    if (addr < 0 || addr > 0x7F) error("Invalid I2C address");
    int ret = hal_i2c_master_read(1, (uint8_t)addr, &rxdata, 1, 0, 1000);
    I2C_SetStatusFromHal(ret);
}
// receive data from an I2C slave - master mode
void i2cReceive(unsigned char * p) {
    int addr, i2c_options, rcvlen;
    void * ptr = NULL;
    getargs(&p, 7, (unsigned char *)",");
    if (argc != 7) error("Invalid syntax");
    if (!I2C_enabled) error("I2C not open");
    addr = getinteger(argv[0]);
    i2c_options = getint(argv[2], 0, 1);
    I2C_Status = 0;
    I2C_Rcvbuf_Float = NULL;
    I2C_Rcvbuf_Int = NULL;
    I2C_Rcvbuf_String = NULL;
    if (i2c_options & 0x01) I2C_Status = I2C_Status_BusHold;
    I2C_Addr = addr;
    rcvlen = getinteger(argv[4]);
    if (rcvlen < 1) error("Number out of bounds");
    ptr = findvar(argv[6], V_FIND | V_EMPTY_OK);
    if (g_vartbl[g_VarIndex].type & T_CONST) error("Cannot change a constant");
    if (ptr == NULL) error("Invalid variable");
    if (g_vartbl[g_VarIndex].type & T_NBR) {
        if (g_vartbl[g_VarIndex].dims[1] != 0) error("Invalid variable");
        if (g_vartbl[g_VarIndex].dims[0] <= 0) { // Not an array
            if (rcvlen != 1) error("Invalid variable");
        } else { // An array
            if ((((MMFLOAT *)ptr - g_vartbl[g_VarIndex].val.fa) + rcvlen) > (g_vartbl[g_VarIndex].dims[0] + 1 - g_OptionBase))
                error("Insufficient space in array");
        }
        I2C_Rcvbuf_Float = (MMFLOAT *)ptr;
    } else if (g_vartbl[g_VarIndex].type & T_INT) {
        if (g_vartbl[g_VarIndex].dims[1] != 0) error("Invalid variable");
        if (g_vartbl[g_VarIndex].dims[0] <= 0) { // Not an array
            if (rcvlen != 1) error("Invalid variable");
        } else { // An array
            if ((((int64_t *)ptr - g_vartbl[g_VarIndex].val.ia) + rcvlen) > (g_vartbl[g_VarIndex].dims[0] + 1 - g_OptionBase))
                error("Insufficient space in array");
        }
        I2C_Rcvbuf_Int = (int64_t *)ptr;
    } else if (g_vartbl[g_VarIndex].type & T_STR) {
        if (rcvlen < 1 || rcvlen > 255) error("Number out of bounds");
        if (g_vartbl[g_VarIndex].dims[0] != 0) error("Invalid variable");
        *(char *)ptr = rcvlen;
        I2C_Rcvbuf_String = (char *)ptr + 1;
    } else
        error("Invalid variable");
    I2C_Rcvlen = rcvlen;

    I2C_Sendlen = 0;
    char * buff = GetTempMemory(rcvlen > 255 ? rcvlen + 2 : STRINGSIZE);
    //	PInt((uint32_t)I2C_Rcvbuf_String);
    i2c_masterCommand(1, (unsigned char *)buff);
    //	PIntComma(rcvlen);
    //	PInt((uint32_t)I2C_Rcvbuf_String);PRet();
    //	if(g_vartbl[g_VarIndex].type & T_STR)*(char *)ptr = rcvlen;
}
void i2cReceiveSlave(unsigned char * p, int channel) {
    int rcvlen;
    void * ptr = NULL;
    MMFLOAT * rcvdlenFloat = NULL;
    int64_t * rcvdlenInt = NULL;
    int count = 1;
    I2C_Rcvbuf_Float = NULL;
    I2C_Rcvbuf_Int = NULL;
    I2C_Rcvbuf_String = NULL;
    I2C2_Rcvbuf_Float = NULL;
    I2C2_Rcvbuf_Int = NULL;
    I2C2_Rcvbuf_String = NULL;
    getargs(&p, 5, (unsigned char *)",");
    if (argc != 5) error("Invalid syntax");
    if (!((I2C_Status & I2C_Status_Slave && channel == 0) || (I2C2_Status & I2C_Status_Slave && channel == 1))) error("I2C slave not open");
    rcvlen = getinteger(argv[0]);
    if (rcvlen < 1 || rcvlen > 255) error("Number out of bounds");
    ptr = findvar(argv[2], V_FIND | V_EMPTY_OK);
    if (g_vartbl[g_VarIndex].type & T_CONST) error("Cannot change a constant");
    if (ptr == NULL) error("Invalid variable");
    if (g_vartbl[g_VarIndex].type & T_NBR) {
        if (g_vartbl[g_VarIndex].dims[1] != 0) error("Invalid variable");
        if (g_vartbl[g_VarIndex].dims[0] <= 0) { // Not an array
            if (rcvlen != 1) error("Invalid variable");
        } else { // An array
            if ((((MMFLOAT *)ptr - g_vartbl[g_VarIndex].val.fa) + rcvlen) > (g_vartbl[g_VarIndex].dims[0] + 1 - g_OptionBase))
                error("Insufficient space in array");
        }
        I2C_Rcvbuf_Float = (MMFLOAT *)ptr;
    } else if (g_vartbl[g_VarIndex].type & T_INT) {
        if (g_vartbl[g_VarIndex].dims[1] != 0) error("Invalid variable");
        if (g_vartbl[g_VarIndex].dims[0] <= 0) { // Not an array
            if (rcvlen != 1) error("Invalid variable");
        } else { // An array
            if ((((int64_t *)ptr - g_vartbl[g_VarIndex].val.ia) + rcvlen) > (g_vartbl[g_VarIndex].dims[0] + 1 - g_OptionBase))
                error("Insufficient space in array");
        }
        I2C_Rcvbuf_Int = (int64_t *)ptr;
    } else if (g_vartbl[g_VarIndex].type & T_STR) {
        if (g_vartbl[g_VarIndex].dims[0] != 0) error("Invalid variable");
        *(char *)ptr = rcvlen;
        I2C_Rcvbuf_String = (char *)ptr + 1;
    } else
        error("Invalid variable");
    ptr = findvar(argv[4], V_FIND);
    if (g_vartbl[g_VarIndex].type & T_CONST) error("Cannot change a constant");
    if (g_vartbl[g_VarIndex].type & T_NBR)
        rcvdlenFloat = (MMFLOAT *)ptr;
    else if (g_vartbl[g_VarIndex].type & T_INT)
        rcvdlenInt = (int64_t *)ptr;
    else
        error("Invalid variable");

    unsigned char * bbuff = I2C_Send_Buffer;
    size_t got = 0;
    hal_i2c_slave_poll(channel, bbuff, (size_t)rcvlen, &got);
    count = (int)got;
    for (int i = 0; i < rcvlen; i++) {
        if (I2C_Rcvbuf_String != NULL) {
            *I2C_Rcvbuf_String = bbuff[i];
            I2C_Rcvbuf_String++;
        }
        if (I2C_Rcvbuf_Float != NULL) {
            *I2C_Rcvbuf_Float = bbuff[i];
            I2C_Rcvbuf_Float++;
        }
        if (I2C_Rcvbuf_Int != NULL) {
            *I2C_Rcvbuf_Int = bbuff[i];
            I2C_Rcvbuf_Int++;
        }
    }
    if (!(rcvdlenFloat == NULL))
        *rcvdlenFloat = (MMFLOAT)count;
    else
        *rcvdlenInt = (int64_t)count;
}
// receive data from an I2C slave - master mode
void i2c2Receive(unsigned char * p) {
    int addr, i2c2_options, rcvlen;
    void * ptr = NULL;
    getargs(&p, 7, (unsigned char *)",");
    if (argc != 7) error("Invalid syntax");
    if (!I2C2_enabled) error("I2C not open");
    addr = getinteger(argv[0]);
    i2c2_options = getint(argv[2], 0, 1);
    I2C2_Status = 0;
    if (i2c2_options & 0x01) I2C2_Status = I2C_Status_BusHold;
    I2C2_Addr = addr;
    I2C2_Rcvbuf_Float = NULL;
    I2C2_Rcvbuf_Int = NULL;
    I2C2_Rcvbuf_String = NULL;
    rcvlen = getinteger(argv[4]);
    if (rcvlen < 1) error("Number out of bounds");
    ptr = findvar(argv[6], V_FIND | V_EMPTY_OK);
    if (g_vartbl[g_VarIndex].type & T_CONST) error("Cannot change a constant");
    if (ptr == NULL) error("Invalid variable");
    if (g_vartbl[g_VarIndex].type & T_NBR) {
        if (g_vartbl[g_VarIndex].dims[1] != 0) error("Invalid variable");
        if (g_vartbl[g_VarIndex].dims[0] <= 0) { // Not an array
            if (rcvlen != 1) error("Invalid variable");
        } else { // An array
            if ((((MMFLOAT *)ptr - g_vartbl[g_VarIndex].val.fa) + rcvlen) > (g_vartbl[g_VarIndex].dims[0] + 1 - g_OptionBase))
                error("Insufficient space in array");
        }
        I2C2_Rcvbuf_Float = (MMFLOAT *)ptr;
    } else if (g_vartbl[g_VarIndex].type & T_INT) {
        if (g_vartbl[g_VarIndex].dims[1] != 0) error("Invalid variable");
        if (g_vartbl[g_VarIndex].dims[0] <= 0) { // Not an array
            if (rcvlen != 1) error("Invalid variable");
        } else { // An array
            if ((((int64_t *)ptr - g_vartbl[g_VarIndex].val.ia) + rcvlen) > (g_vartbl[g_VarIndex].dims[0] + 1 - g_OptionBase))
                error("Insufficient space in array");
        }
        I2C2_Rcvbuf_Int = (int64_t *)ptr;
    } else if (g_vartbl[g_VarIndex].type & T_STR) {
        if (rcvlen < 1 || rcvlen > 255) error("Number out of bounds");
        if (g_vartbl[g_VarIndex].dims[0] != 0) error("Invalid variable");
        *(char *)ptr = rcvlen;
        I2C2_Rcvbuf_String = (char *)ptr + 1;
    } else
        error("Invalid variable");
    I2C2_Rcvlen = rcvlen;

    I2C2_Sendlen = 0;

    char * buff = GetTempMemory(rcvlen > 255 ? rcvlen + 2 : STRINGSIZE);
    i2c2_masterCommand(1, (unsigned char *)buff);
}

/**************************************************************************************************
Enable the I2C1 module - master mode
***************************************************************************************************/
void i2c_enable(int bps) {
    ExtCfg(I2C0SDApin, EXT_COM_RESERVED, 0);
    ExtCfg(I2C0SCLpin, EXT_COM_RESERVED, 0);
    hal_i2c_master_init(0, PinDef[I2C0SDApin].GPno, PinDef[I2C0SCLpin].GPno,
                        (uint32_t)bps * 1000);
    I2C_enabled = 1;
}
void i2c2_enable(int bps) {
    ExtCfg(I2C1SDApin, EXT_COM_RESERVED, 0);
    ExtCfg(I2C1SCLpin, EXT_COM_RESERVED, 0);
    hal_i2c_master_init(1, PinDef[I2C1SDApin].GPno, PinDef[I2C1SCLpin].GPno,
                        (uint32_t)bps * 1000);
    I2C2_enabled = 1;
}

/**************************************************************************************************
Disable the I2C1 module - master mode
***************************************************************************************************/
void i2c_disable() {
    if (I2C_Status & I2C_Status_Slave) {
        hal_i2c_slave_disable(0);
    }
    I2C_Status = I2C_Status_Disable;
    I2C_Rcvbuf_String = NULL; // pointer to the master receive buffer
    I2C_Rcvbuf_Float = NULL;
    I2C_Rcvbuf_Int = NULL;
    I2C_Sendlen = 0; // length of the master send buffer
    I2C_Rcvlen = 0;  // length of the master receive buffer
    I2C_Addr = 0;    // I2C device address
    I2C_Timeout = 0; // master timeout value
    hal_i2c_master_deinit(0);
    I2C_enabled = 0;
    if (I2C0SDApin != 99) ExtCfg(I2C0SDApin, EXT_NOT_CONFIG, 0);
    if (I2C0SCLpin != 99) ExtCfg(I2C0SCLpin, EXT_NOT_CONFIG, 0);
}
void i2c2_disable() {
    if (I2C2_Status & I2C_Status_Slave) {
        hal_i2c_slave_disable(1);
    }
    I2C2_Status = I2C_Status_Disable;
    I2C2_Rcvbuf_String = NULL; // pointer to the master receive buffer
    I2C2_Rcvbuf_Float = NULL;
    I2C2_Rcvbuf_Int = NULL;
    I2C2_Sendlen = 0; // length of the master send buffer
    I2C2_Rcvlen = 0;  // length of the master receive buffer
    I2C2_Addr = 0;    // I2C device address
    I2C2_Timeout = 0; // master timeout value
    hal_i2c_master_deinit(1);
    I2C2_enabled = 0;
    if (I2C1SDApin != 99) ExtCfg(I2C1SDApin, EXT_NOT_CONFIG, 0);
    if (I2C1SCLpin != 99) ExtCfg(I2C1SCLpin, EXT_NOT_CONFIG, 0);
}
/**************************************************************************************************
Send and/or Receive data - master mode
***************************************************************************************************/
void i2c_masterCommand(int timer, unsigned char * I2C_Rcv_Buffer) {
    //	unsigned char start_type,
    unsigned char i2caddr = I2C_Addr;
    if (I2C_Sendlen) {
        int ret = hal_i2c_master_write(0, (uint8_t)i2caddr, (uint8_t *)I2C_Send_Buffer, I2C_Sendlen, (I2C_Status == I2C_Status_BusHold), I2C_Timeout * 1000);
        I2C_SetStatusFromHal(ret);
    }
    if (I2C_Rcvlen) {
        int ret = hal_i2c_master_read(0, (uint8_t)i2caddr, (uint8_t *)I2C_Rcv_Buffer, I2C_Rcvlen, (I2C_Status == I2C_Status_BusHold), I2C_Timeout * 1000);
        I2C_SetStatusFromHal(ret);
        for (int i = 0; i < I2C_Rcvlen; i++) {
            if (I2C_Rcvbuf_String != NULL) {
                *I2C_Rcvbuf_String = I2C_Rcv_Buffer[i];
                I2C_Rcvbuf_String++;
            }
            if (I2C_Rcvbuf_Float != NULL) {
                *I2C_Rcvbuf_Float = I2C_Rcv_Buffer[i];
                I2C_Rcvbuf_Float++;
            }
            if (I2C_Rcvbuf_Int != NULL) {
                *I2C_Rcvbuf_Int = I2C_Rcv_Buffer[i];
                I2C_Rcvbuf_Int++;
            }
        }
    }
}

void i2c2_masterCommand(int timer, unsigned char * I2C2_Rcv_Buffer) {
    //	unsigned char start_type,
    unsigned char i2c2addr = I2C2_Addr;
    if (I2C2_Sendlen) {
        int ret = hal_i2c_master_write(1, (uint8_t)i2c2addr, (uint8_t *)I2C_Send_Buffer, I2C2_Sendlen, (I2C2_Status == I2C_Status_BusHold), I2C2_Timeout * 1000);
        I2C_SetStatusFromHal(ret);
    }
    if (I2C2_Rcvlen) {
        int ret = hal_i2c_master_read(1, (uint8_t)i2c2addr, (uint8_t *)I2C2_Rcv_Buffer, I2C2_Rcvlen, (I2C2_Status == I2C_Status_BusHold), I2C2_Timeout * 1000);
        I2C_SetStatusFromHal(ret);
        for (int i = 0; i < I2C2_Rcvlen; i++) {
            if (I2C2_Rcvbuf_String != NULL) {
                *I2C2_Rcvbuf_String = I2C2_Rcv_Buffer[i];
                I2C2_Rcvbuf_String++;
            }
            if (I2C2_Rcvbuf_Float != NULL) {
                *I2C2_Rcvbuf_Float = I2C2_Rcv_Buffer[i];
                I2C2_Rcvbuf_Float++;
            }
            if (I2C2_Rcvbuf_Int != NULL) {
                *I2C2_Rcvbuf_Int = I2C2_Rcv_Buffer[i];
                I2C2_Rcvbuf_Int++;
            }
        }
    }
}
/*  @endcond */

void fun_mmi2c(void) {
    iret = mmI2Cvalue;
    targ = T_INT;
}
/*
 * @cond
 * The following section will be excluded from the documentation.
 */
void GeneralSend(unsigned int addr, int nbr, char * p) {
    if (I2C0locked) {
        I2C_Sendlen = nbr; // send one byte
        I2C_Rcvlen = 0;
        memcpy(I2C_Send_Buffer, p, nbr);
        I2C_Addr = addr; // address of the device
        i2c_masterCommand(1, NULL);
    } else {
        I2C2_Sendlen = nbr; // send one byte
        I2C2_Rcvlen = 0;
        memcpy(I2C_Send_Buffer, p, nbr);
        I2C2_Addr = addr; // address of the device
        i2c2_masterCommand(1, NULL);
    }
}

void GeneralReceive(unsigned int addr, int nbr, char * p) {
    if (I2C0locked) {
        I2C_Rcvbuf_Float = NULL;
        I2C_Rcvbuf_Int = NULL;
        I2C_Rcvbuf_String = NULL;
        I2C_Sendlen = 0; // send one byte
        I2C_Rcvlen = nbr;
        I2C_Addr = addr; // address of the device
        i2c_masterCommand(1, (unsigned char *)p);
    } else {
        I2C2_Rcvbuf_Float = NULL;
        I2C2_Rcvbuf_Int = NULL;
        I2C2_Rcvbuf_String = NULL;
        I2C2_Sendlen = 0; // send one byte
        I2C2_Rcvlen = nbr;
        I2C2_Addr = addr; // address of the device
        i2c2_masterCommand(1, (unsigned char *)p);
    }
}
void WiiSend(int nbr, char * p) {
    unsigned int addr = nunaddr;
    GeneralSend(addr, nbr, p);
}

void WiiReceive(int nbr, char * p) {
    unsigned int addr = nunaddr;
    GeneralReceive(addr, nbr, p);
}

uint8_t readRegister8(unsigned int addr, uint8_t reg) {
    uint8_t buff;
    GeneralSend(addr, 1, (char *)&reg);
    GeneralReceive(addr, 1, (char *)&buff);
    return buff;
}
uint32_t readRegister32(unsigned int addr, uint8_t reg) {
    uint32_t buff;
    GeneralSend(addr, 1, (char *)&reg);
    GeneralReceive(addr, 4, (char *)&buff);
    return buff;
}
void WriteRegister8(unsigned int addr, uint8_t reg, uint8_t data) {
    uint8_t buff[2];
    buff[0] = reg;
    buff[1] = data;
    GeneralSend(addr, 2, (char *)buff);
}
void Write8Register16(unsigned int addr, uint16_t reg, uint8_t data) {
    uint8_t buff[3];
    buff[0] = reg >> 8;
    buff[1] = reg & 0xFF;
    buff[2] = data;
    GeneralSend(addr, 3, (char *)buff);
}
uint8_t read8Register16(unsigned int addr, uint16_t reg) {
    uint8_t buff;
    uint8_t rbuff[2];
    rbuff[0] = reg >> 8;
    rbuff[1] = reg & 0xFF;
    if (I2C0locked)
        I2C_Status = I2C_Status_BusHold;
    else
        I2C2_Status = I2C_Status_BusHold;
    GeneralSend(addr, 2, (char *)rbuff);
    if (I2C0locked)
        I2C_Status = 0;
    else
        I2C2_Status = 0;
    GeneralReceive(addr, 1, (char *)&buff);
    return buff;
}

void nunproc(void) {
    static int lastc = 0, lastz = 0;
    nunstruct[5].x = nunbuff[0];
    nunstruct[5].y = nunbuff[1];
    nunstruct[5].ax = nunbuff[2] << 2;
    nunstruct[5].ay = nunbuff[3] << 2;
    nunstruct[5].az = nunbuff[4] << 2;
    nunstruct[5].Z = (~(nunbuff[5] & 1)) & 1;
    nunstruct[5].C = (~((nunbuff[5] & 2) >> 1)) & 1;
    nunstruct[5].ax += ((nunbuff[5] >> 2) & 3);
    nunstruct[5].ay += ((nunbuff[5] >> 4) & 3);
    nunstruct[5].az += ((nunbuff[5] >> 6) & 3);
    if (lastc == 0 && nunstruct[5].C) {
        lastc = 1;
        nunfoundc[5] = 1;
    }
    if (lastz == 0 && nunstruct[5].Z) {
        lastz = 1;
        nunfoundc[5] = 1;
    }
    if (nunstruct[5].C == 0) lastc = 0;
    if (nunstruct[5].Z == 0) lastz = 0;
}

/*  @endcond */
void MIPS16 cmd_Nunchuck(void) {
    unsigned char * tp = NULL;
    uint32_t id = 0;
    if ((tp = checkstring(cmdline, (unsigned char *)"OPEN"))) {
        getargs(&tp, 1, (unsigned char *)",");
        if (!(I2C0locked || I2C1locked)) error("SYSTEM I2C not configured");
        if (classic1 || nunchuck1) error("Already open");
        memset((void *)&nunstruct[5].x, 0, sizeof(nunstruct[5]));
        int retry = 5;
        do {
            WiiSend(sizeof(nuninit), (char *)nuninit);
            uSec(5000);
        } while (mmI2Cvalue && retry--);
        if (mmI2Cvalue) error("Nunchuck not connected");
        WiiSend(sizeof(nuninit2), (char *)nuninit2);
        if (mmI2Cvalue) error("Nunchuck not connected");
        uSec(5000);
        retry = 5;
        do {
            WiiSend(sizeof(nunid), (char *)nunid);
            uSec(5000);
            WiiReceive(4, (char *)&id);
            uSec(5000);
        } while (mmI2Cvalue && retry--);
        if (mmI2Cvalue) error("Device ID not returned");
        nunstruct[5].type = swap32(id);
        if (nunstruct[5].type != 0xA4200000) error("Device connected is not a Nunchuck");
        uSec(5000);
        retry = 5;
        nunbuff[5] = 0;
        if (argc == 1) {
            nunInterruptc[5] = (char *)GetIntAddress(argv[0]); // get the interrupt location
            InterruptUsed = true;
        }
        nunchuck1 = 1;
        while (nunchuck1 == 1) routinechecks();
        if (nunbuff[5] == 0 || nunbuff[5] == 255) {
            nunchuck1 = 0;
            error("Nunchuck not responding");
        }
        nunproc();
        return;
    } else if ((tp = checkstring(cmdline, (unsigned char *)"CLOSE"))) {
        if (!nunchuck1) error("Not open");
        nunchuck1 = 0;
        nunchuckread = false;
        WiiReceive(6, (char *)nunbuff);
        nunInterruptc[5] = NULL;
    } else
        error("Syntax");
}

void MIPS16 cmd_Classic(void) {
    unsigned char * tp = NULL;
    uint32_t id = 0;
    if ((tp = checkstring(cmdline, (unsigned char *)"OPEN"))) {
        getargs(&tp, 3, (unsigned char *)",");
        if (!(I2C0locked || I2C1locked)) error("SYSTEM I2C not configured");
        if (classic1 || nunchuck1) error("Already open");
        memset((void *)&nunstruct[0].x, 0, sizeof(nunstruct[0]));
        int retry = 5;
        do {
            WiiSend(sizeof(nuninit), (char *)nuninit);
            uSec(5000);
        } while (mmI2Cvalue && retry--);
        if (mmI2Cvalue) error("Classic not connected");
        WiiSend(sizeof(nuninit2), (char *)nuninit2);
        if (mmI2Cvalue) error("Classic not connected");
        uSec(5000);
        retry = 5;
        do {
            WiiSend(sizeof(nunid), (char *)nunid);
            uSec(5000);
            WiiReceive(4, (char *)&id);
            uSec(5000);
        } while (mmI2Cvalue && retry--);
        if (mmI2Cvalue) error("Device ID not returned");
        nunstruct[0].type = swap32(id);
        if (nunstruct[0].type == 0xA4200000) error("Device connected is a Nunchuck");
        uSec(5000);
        if (argc >= 1) {
            nunInterruptc[0] = (char *)GetIntAddress(argv[0]); // get the interrupt location
            InterruptUsed = true;
            nunstruct[0].x1 = 0b111111111111111;
            if (argc == 3) nunstruct[0].x1 = getint(argv[2], 0, 0b111111111111111);
        }
        classic1 = 1;
        while (classic1 == 1) routinechecks();
        if (nunbuff[0] == 0 || nunbuff[0] == 255) {
            classic1 = 0;
            error("Classic not responding");
        }
        classicproc();
        return;
    } else if ((tp = checkstring(cmdline, (unsigned char *)"CLOSE"))) {
        if (!classic1) error("Not open");
        classic1 = 0;
        classicread = false;
        WiiReceive(6, (char *)nunbuff);
        nunInterruptc[0] = NULL;
    } else
        error("Syntax");
}

/*
 * @cond
 * The following section will be excluded from the documentation.
 */

void classicproc(void) {
    //	int ax; //classic left x
    //	int ay; //classic left y
    //	int az; //classic centre
    //	int Z;  //classic right x
    //	int C;  //classic right y
    //	int L;  //classic left analog
    //	int R;  //classic right analog
    //	unsigned short x0; //classic buttons
    static unsigned short buttonlast = 0;
    unsigned short inttest = (((nunbuff[4] >> 1) | (nunbuff[5] << 7)) ^ 0b111111111111111) & nunstruct[0].x1;
    nunstruct[0].classic[0] = nunbuff[0];
    nunstruct[0].classic[1] = nunbuff[1];
    nunstruct[0].classic[2] = nunbuff[2];
    nunstruct[0].classic[3] = nunbuff[3];
    nunstruct[0].classic[4] = nunbuff[4];
    nunstruct[0].classic[5] = nunbuff[5];
    if (inttest != buttonlast) {
        nunfoundc[0] = 1;
    }
    buttonlast = inttest;
    nunstruct[0].ax = (nunbuff[0] & 0b111111) << 2;
    nunstruct[0].ay = (nunbuff[1] & 0b111111) << 2;
    nunstruct[0].Z = (((nunbuff[2] & 0b10000000) >> 7) |
                      ((nunbuff[1] & 0b11000000) >> 5) |
                      ((nunbuff[0] & 0b11000000) >> 3))
                     << 3;
    nunstruct[0].C = (nunbuff[2] & 0b11111) << 3;
    nunstruct[0].R = ((nunbuff[3] & 0b00011111)) << 3;
    nunstruct[0].L = (((nunbuff[3] & 0b11100000) >> 5) |
                      ((nunbuff[2] & 0b01100000) >> 2))
                     << 3;
    nunstruct[0].x0 = ((nunbuff[4] >> 1) | (nunbuff[5] << 7)) ^ 0b111111111111111;
}

