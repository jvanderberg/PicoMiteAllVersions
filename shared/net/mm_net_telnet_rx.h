/*
 * shared/net/mm_net_telnet_rx.h — RFC 854 inbound telnet stream parser.
 *
 * Implements the canonical 5-state machine (DATA / IAC / OPT / SB /
 * SB_IAC): it strips RFC 854 protocol bytes, dedups CR NUL pairs, and
 * pushes the resulting data stream into ConsoleRxBuf with the usual
 * BreakKey / keyselect / overflow semantics. State is module-private and
 * persists across feed() calls; call mm_net_telnet_rx_reset() when a
 * telnet connection closes so the next client starts in DATA state.
 */
#ifndef MM_NET_TELNET_RX_H
#define MM_NET_TELNET_RX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void mm_net_telnet_rx_reset(void);
void mm_net_telnet_rx_feed(const uint8_t * data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* MM_NET_TELNET_RX_H */
