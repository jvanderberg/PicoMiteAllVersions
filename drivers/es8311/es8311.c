/*
 * drivers/es8311/es8311.c - ES8311 audio codec recipe (see es8311.h).
 *
 * The DAC bring-up register program was written for the ES8311 running as
 * an I2S slave with MCLK supplied, 16-bit stereo - the configuration the
 * shared audio backend drives. A future board that needs a genuinely
 * different program (clocking, routing, gain staging) earns a variant
 * entry point or a parameter here, the way the SPI LCD table grew
 * ILI9488P/ILI9488W - never a board name.
 */

#include <stddef.h>

#include "es8311.h"

static int write_seq(const es8311_bus_t * bus, const uint8_t (*seq)[2],
                     size_t count) {
    for (size_t i = 0; i < count; i++) {
        int err = bus->write_reg(seq[i][0], seq[i][1]);
        if (err) return err;
    }
    return 0;
}

int es8311_start(const es8311_bus_t * bus, uint8_t * chip_id) {
    if (!bus || !bus->write_reg || !bus->read_reg || !bus->delay_ms) return -1;

    uint8_t id = 0;
    int err = bus->read_reg(0xfd, &id);
    if (err) return err;
    if (chip_id) *chip_id = id;

    static const uint8_t seq[][2] = {
        {0x00, 0x1f},
        {0x00, 0x80},
        {0x01, 0x3f},
        {0x02, 0x00},
        {0x03, 0x10},
        {0x04, 0x10},
        {0x05, 0x00},
        {0x06, 0x03},
        {0x07, 0x00},
        {0x08, 0xff},
        {0x09, 0x0c},
        {0x0a, 0x0c},
        {0x0b, 0x00},
        {0x0c, 0x1f},
        {0x0d, 0x01},
        {0x0e, 0x02},
        {0x0f, 0x44},
        {0x10, 0x1f},
        {0x11, 0x7f},
        {0x12, 0x00},
        {0x13, 0x00},
        {0x14, 0x10},
        {0x15, 0x00},
        {0x16, 0x24},
        {0x1b, 0x0a},
        {0x1c, 0x6a},
        {0x31, 0x00},
        {0x32, 0xbf},
        {0x37, 0x08},
        {0x44, 0x00},
    };
    bus->delay_ms(20);
    return write_seq(bus, seq, sizeof(seq) / sizeof(seq[0]));
}

int es8311_mute(const es8311_bus_t * bus) {
    if (!bus || !bus->write_reg) return -1;
    return bus->write_reg(0x09, 0x4c);
}
