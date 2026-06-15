/*
 * esp32_cyd_usb_stub.c — USB role/keyboard surface for a chip without USB.
 *
 * Classic ESP32 has no native USB controller, so the S3 port's USB
 * Serial/JTAG role switching and USB host keyboard do not exist here. The
 * console is always the UART, OPTION USB is not part of this port's option
 * surface (the setter reports the line as unhandled so OPTION parsing
 * raises the normal error), and the keyboard never has input.
 */

void esp32_usb_role_resolve_boot(void) {}

int esp32_usb_role_is_serial(void) {
    return 1;
}

int esp32_usb_role_is_keyboard(void) {
    return 0;
}

void esp32_usb_role_prepare_keyboard_host(void) {}

int esp32_usb_role_option_setter(unsigned char * cmdline) {
    (void)cmdline;
    return 0;
}

void esp32_usb_role_print_options(void) {}

void esp32_usb_keyboard_start_host(void) {}

int esp32_usb_keyboard_has_keyboard(void) {
    return 0;
}

void esp32_usb_keyboard_service(void) {}

void esp32_usb_keyboard_clear_repeat_state(void) {}

int esp32_usb_keyboard_input_available(void) {
    return 0;
}

int esp32_usb_keyboard_pop_key(void) {
    return -1;
}
