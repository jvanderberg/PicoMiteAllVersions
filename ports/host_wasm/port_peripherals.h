/*
 * ports/host_wasm/port_peripherals.h — the browser build ships the GUI
 * controls (drivers/gui_controls/GUI.c) with mouse-emulated touch, so it
 * needs the Touch + GUI declarations device ports pull in. There is no
 * SPI-LCD panel here (the framebuffer is the canvas), so SSD1963.h is
 * intentionally omitted.
 *
 * Wins over ports/host_native/port_peripherals.h because the Makefile
 * orders -I$(WASM_DIR) before -I$(NATIVE_DIR).
 */

#ifndef PORT_PERIPHERALS_H
#define PORT_PERIPHERALS_H

#include "Touch.h"
#include "GUI.h"

#endif /* PORT_PERIPHERALS_H */
