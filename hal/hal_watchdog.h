#ifndef HAL_WATCHDOG_H
#define HAL_WATCHDOG_H

void hal_watchdog_disable(void);
void hal_watchdog_enable_ms(unsigned int ms, int pause_on_debug);

/* Force an immediate processor reboot via the watchdog (the classic
 * 1 ms watchdog-timeout idiom). Pauses while a debugger is attached,
 * so the reboot only fires on a free-running chip. Ports without
 * reset hardware no-op; callers follow with their own spin loop. */
void hal_watchdog_reboot(void);

#endif
