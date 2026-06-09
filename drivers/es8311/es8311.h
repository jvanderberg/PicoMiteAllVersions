/*
 * drivers/es8311/es8311.h - ES8311 audio codec recipe.
 *
 * MCU-neutral codec knowledge: the register program that brings the DAC up
 * and the mute sequence. The caller supplies the I2C transport and delay —
 * everything board- or MCU-specific (bus bring-up, amplifier enable,
 * wiring) stays with the port. Calls happen at codec init/teardown only,
 * never on the audio sample path.
 */

#ifndef ES8311_H
#define ES8311_H

#include <stdint.h>

#define ES8311_I2C_ADDR 0x18

typedef struct {
    /* Both return 0 on success, nonzero on failure. */
    int (*write_reg)(uint8_t reg, uint8_t value);
    int (*read_reg)(uint8_t reg, uint8_t * value);
    void (*delay_ms)(int ms);
} es8311_bus_t;

/* Probe the codec and run the DAC bring-up program. Returns 0 on success;
 * the chip id read from the part is stored in *chip_id when non-NULL. */
int es8311_start(const es8311_bus_t * bus, uint8_t * chip_id);

/* Mute the DAC serial input (the safe pre-teardown state). */
int es8311_mute(const es8311_bus_t * bus);

#endif /* ES8311_H */
