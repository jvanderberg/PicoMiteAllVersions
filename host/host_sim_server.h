/*
 * host_sim_server.h -- Start/stop API for the --sim HTTP+WS server.
 */

#ifndef HOST_SIM_SERVER_H
#define HOST_SIM_SERVER_H

int host_sim_server_start(const char *listen_addr, int port, const char *web_root);
void host_sim_server_stop(void);

/*
 * 1ms tick thread. On device PicoMite.c:826 `timer_callback` bumps
 * mSecTimer / CursorTimer / PauseTimer / Timer1..5 / TickTimer[] /
 * ScrewUpTimer every ms from an IRQ. The --sim build runs the same
 * updates on a background pthread so PAUSE, TIMER, ON INTERRUPT TICK,
 * and cursor blink advance without hardware timers.
 *
 * Only available in MMBASIC_SIM builds — non-sim test harness calls
 * host_time_us_64 / host_sync_msec_timer to advance the counters
 * lazily instead.
 */
void host_sim_tick_start(void);
void host_sim_tick_stop(void);

/*
 * Single-producer (WS handler) / single-consumer (MMBasic main thread)
 * key queue. The server pushes on WS message receipt; MMInkey() drains
 * via host_sim_pop_key(). Returns -1 when empty.
 */
void host_sim_push_key(int code);
int  host_sim_pop_key(void);

#endif
