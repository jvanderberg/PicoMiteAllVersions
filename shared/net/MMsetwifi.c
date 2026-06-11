/*
 * MMsetwifi.c — WEB-only OPTION handlers + MM.* info dispatch +
 * printoptions block. Compiled only on PICOMITEWEB builds (gated in
 * CMakeLists). Lifted out of MM_Misc.c so its OPTION setters and
 * fun_mminfo paths stay preprocessor-clean. Stubs for non-WEB live in
 * host_runtime.c (host) and ports/pico_sdk_common/cmd_files_hooks.c
 * (device, since cmd_files_hooks.c is the device-side companion to
 * the host-runtime stubs). Real impls below set `iret`/`sret`/`targ`
 * the same way the original inline code did.
 */

#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "hal/hal_net.h"
#include "shared/net/mm_net_http.h"
#include "shared/net/mm_net_lifecycle.h"
#include "shared/net/mm_net_options.h"
#include "shared/net/mm_net_web_cmd.h"
#include "shared/net/mm_net_wifi_cmd.h"
#include "lwip/ip4_addr.h"
#define CJSON_NESTING_LIMIT 100

extern char id_out[12];
extern bool optionsuppressstatus;
extern void setwifi(unsigned char * tp);
extern void WebConnect(void);
extern int open_tcp_server(uint16_t port);
extern void close_tcp_server(void);
extern int open_udp_server(uint16_t port);
extern void close_udp_server(void);
extern int pico_tcp_server_request_pending(int pcb);
extern const char * pico_tcp_server_path(int pcb);
extern int cmd_mqtt(void);
extern void cmd_ntp(unsigned char * tp);
extern void cmd_udp(unsigned char * tp);
extern int cmd_tcpclient(void);
extern int cmd_tcpserver(void);
extern int cmd_tftp_server_init(void);
extern int pico_telnet_open(void);
extern void pico_telnet_close(void);
extern void pico_tftp_close(void);

static void pico_validate_static_ip(
    const mm_net_wifi_credentials_t * credentials) {
    ip4_addr_t ipaddr;
    if (!ip4addr_aton(credentials->ipaddress, &ipaddr))
        error("Invalid IP address");
    if (!ip4addr_aton(credentials->mask, &ipaddr))
        error("Invalid mask address");
    if (!ip4addr_aton(credentials->gateway, &ipaddr))
        error("Invalid gateway address");
}

void setwifi(unsigned char * tp) {
    char default_hostname[32];
    strcpy(default_hostname, "PICO");
    strcat(default_hostname, id_out);
    mm_net_lifecycle_store_wifi_credentials(tp, default_hostname,
                                            pico_validate_static_ip);
}

/* WEB-specific printoptions block. Called from MM_Misc.c's printoptions
 * unconditionally; non-WEB builds get a no-op stub elsewhere. */
void port_web_print_options(void) {
    mm_net_print_wifi_option((char *)Option.SSID, (char *)Option.PASSWORD,
                             Option.hostname, Option.ipaddress, Option.mask,
                             Option.gateway);
    mm_net_print_options(Option.TCP_PORT, Option.ServerResponceTime,
                         Option.UDP_PORT, Option.UDPServerResponceTime,
                         optionsuppressstatus);
    mm_net_print_service_options((int)Option.Telnet, Option.disabletftp);
}

static mm_net_lifecycle_result_t pico_lifecycle_apply_wifi(unsigned char * arg) {
    setwifi(arg);
    return MM_NET_LIFECYCLE_OK;
}

static int pico_lifecycle_open_tftp(void) {
    return cmd_tftp_server_init();
}

static const mm_net_lifecycle_hooks_t pico_lifecycle_hooks = {
    .apply_wifi = pico_lifecycle_apply_wifi,
    .open_tcp_server = open_tcp_server,
    .close_tcp_server = close_tcp_server,
    .open_udp_server = open_udp_server,
    .close_udp_server = close_udp_server,
    .open_tftp = pico_lifecycle_open_tftp,
    .close_tftp = pico_tftp_close,
    .open_telnet = pico_telnet_open,
    .close_telnet = pico_telnet_close,
    .reboot_after_option_mask = MM_NET_LIFECYCLE_REBOOT_WIFI,
};

static void pico_lifecycle_reboot_required(void) {
    _excep_code = RESET_COMMAND;
    SoftReset();
}

static const mm_net_lifecycle_result_handler_t pico_lifecycle_result_handler = {
    .reboot_required = pico_lifecycle_reboot_required,
};

/* WEB-only OPTION setters: WEB MESSAGES / WIFI / TCP SERVER PORT /
 * UDP SERVER PORT / TELNET / TFTP. Returns 1 if matched. */
int port_web_option_setter(unsigned char * cmdline) {
    return mm_net_lifecycle_handle_option_result(
        mm_net_lifecycle_option_setter(cmdline, &pico_lifecycle_hooks),
        &pico_lifecycle_result_handler);
}

/* WEB-only MM.<X> info function: TCP REQUEST / TCP PORT / UDP PORT /
 * IP ADDRESS / MAX CONNECTIONS / WIFI STATUS / TCPIP STATUS. */
int port_web_mminfo(unsigned char * ep, int64_t * out_iret,
                    unsigned char * out_sret, int * out_targ) {
    const mm_net_info_hooks_t hooks = {
        .tcp_path = pico_tcp_server_path,
        .tcp_request_pending = pico_tcp_server_request_pending,
        .max_connections = MaxPcb,
        .tcp_port = Option.TCP_PORT,
        .udp_port = Option.UDP_PORT,
        .ip_address = hal_net_ip_address,
        .wifi_status = hal_net_wifi_status,
        .tcpip_status = hal_net_tcpip_status,
    };
    return mm_net_mminfo(ep, out_iret, out_sret, out_targ, &hooks);
}

/* OPTION SSID$ getter — separate so MM_Misc.c's OPTION info chain stays
 * unconditional. */
int port_web_get_ssid(unsigned char * out_sret, int * out_targ) {
    strcpy((char *)out_sret, (char *)Option.SSID);
    CtoM(out_sret);
    *out_targ = T_STR;
    return 1;
}

/* WebConnect: relocated from PicoMite.c so the call site there can
 * be unconditional. Linked only on WiFi ports (MMsetwifi.c is in the
 * WiFi-port source list); MMweb_stubs.c provides the no-op shim on
 * non-WiFi devices. */
volatile int WIFIconnected = 0;
int startupcomplete = 0;

void WebConnect(void) {
    static const mm_net_lifecycle_wifi_connect_t connect = {
        .hooks = &pico_lifecycle_hooks,
        .default_hostname = NULL,
        .no_ssid_timeout_ms = 0,
    };
    (void)mm_net_lifecycle_wifi_connect(&connect);
}

#include "hal/hal_option_setters.h"

/* WiFi ports don't expose OPTION PICO ON/OFF (CYW43 actually owns the
 * shadow pins). */
int port_setter_pico_pins(unsigned char * cmdline) {
    (void)cmdline;
    return 0;
}

/* REPL-startup WiFi init: chip bring-up via hal_net_init + WebConnect.
 * The CYW43 bring-up (gSPI clock divider for the persisted CPU speed,
 * cyw43_arch_init) lives in drivers/net_lwip_raw/hal_net_lwip.c. */
#include "hal/hal_main_init.h"
extern int startupcomplete;

void port_repl_wifi_arch_init_and_connect(void) {
    if (hal_net_init() == HAL_NET_OK) {
        startupcomplete = 1;
        WebConnect();
    }
}

/* WiFi ports leave GPIO 23 to the CYW43 module — no PWM shadow. */
void hal_pwm_mode_shadow_apply(void) {}

/* WiFi PIO pin-reset loop walks the full NBRPINS range — the CYW43
 * radio doesn't expose a shadow-pin range that needs to be skipped. */
void port_pio_pin_reset_inputs(void) {
    for (int i = 1; i < NBRPINS; i++) {
        if (CheckPin(i, CP_NOABORT | CP_IGNORE_INUSE | CP_IGNORE_RESERVED)) {
            gpio_set_input_enabled(PinDef[i].GPno, true);
        }
    }
}

/* WiFi ports limit OPTION HEARTBEAT to ON/OFF — no pin reassignment
 * (the heartbeat LED lives on the CYW43 module). */
int port_setter_heartbeat(unsigned char * cmdline) {
    unsigned char * tp = checkstring(cmdline, (unsigned char *)"HEARTBEAT");
    if (!tp) return 0;
    if (checkstring(tp, (unsigned char *)"OFF") || checkstring(tp, (unsigned char *)"DISABLE")) {
        Option.NoHeartbeat = 1;
    } else if (checkstring(tp, (unsigned char *)"ON") || checkstring(tp, (unsigned char *)"ENABLE")) {
        Option.NoHeartbeat = 0;
    } else
        error("Syntax");
    SaveOptions();
    return 1;
}

/* WiFi-only command + function bodies (cmd_web / fun_json / scan
 * result callback) relocated from Custom.c. Custom.c is universal
 * across device ports; this code references CYW43 APIs that only
 * exist on WiFi builds. Non-WiFi ports get a `cmd_web` stub from
 * MMweb_stubs.c and never reference fun_json (excluded from the
 * non-WiFi token-palette in port_tokens.h). */

extern int startupcomplete;

static void pico_web_connect_cmd(unsigned char * arg) {
    if (*arg) {
        setwifi(arg);
        WebConnect();
    } else if (hal_net_wifi_status() < 0) {
        WebConnect();
    }
}

static void pico_web_scan_cmd(unsigned char * arg) {
    mm_net_wifi_scan_command(arg);
}

static int pico_web_is_connected(void) {
    return WIFIconnected && hal_net_link_up();
}

static int pico_web_mqtt_cmd(unsigned char * line) {
    (void)line;
    return cmd_mqtt();
}

static int pico_web_tcp_client_cmd(unsigned char * line) {
    (void)line;
    return cmd_tcpclient();
}

static int pico_web_tcp_server_cmd(unsigned char * arg) {
    (void)arg;
    return cmd_tcpserver();
}

static void pico_web_ntp_cmd(unsigned char * arg) {
    cmd_ntp(arg);
}

static int pico_web_udp_cmd(unsigned char * arg) {
    cmd_udp(arg);
    return 1;
}

void cmd_web(void) {
    static const mm_net_web_dispatch_t dispatch = {
        .connect = pico_web_connect_cmd,
        .scan = pico_web_scan_cmd,
        .is_connected = pico_web_is_connected,
        .not_connected_error = "WIFI not connected",
        .mqtt = pico_web_mqtt_cmd,
        .tcp_client = pico_web_tcp_client_cmd,
        .transmit = pico_web_tcp_server_cmd,
        .tcp_server = pico_web_tcp_server_cmd,
        .ntp = pico_web_ntp_cmd,
        .udp = pico_web_udp_cmd,
    };
    mm_net_web_dispatch(cmdline, &dispatch);
}
